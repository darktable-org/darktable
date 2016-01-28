/*
  This file is part of darktable,
  copyright (c) 2016 Ulrich Pegelow.

  darktable is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  darktable is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma GCC diagnostic ignored "-Wunused-variable"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include "bauhaus/bauhaus.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "control/control.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "common/interpolation.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(1, dt_iop_ashift_params_t)

typedef struct dt_iop_ashift_params_t
{
  float rotation;
  float keystone;
} dt_iop_ashift_params_t;


typedef struct dt_iop_ashift_gui_data_t
{
  GtkWidget *rotation;
  GtkWidget *keystone;
  dt_pthread_mutex_t lock;
} dt_iop_ashift_gui_data_t;

typedef struct dt_iop_ashift_data_t
{
  float rotation;
  float keystone;
} dt_iop_ashift_data_t;

typedef struct dt_iop_ashift_global_data_t
{
  int kernel_ashift_void;
} dt_iop_ashift_global_data_t;

typedef enum dt_iop_ashift_homodir_t
{
  ASHIFT_HOMOGRAPH_FORWARD,
  ASHIFT_HOMOGRAPH_INVERTED
} dt_iop_ashift_homodir_t;

const char *name()
{
  return _("perspective correction");
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE;
}

int groups()
{
  return IOP_GROUP_CORRECT;
}

int operation_tags()
{
  return IOP_TAG_DISTORT;
}

#if 0
int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    const dt_iop_ashift_params1_t *old = old_params;
    dt_iop_ashift_params_t *new = new_params;
    new->rotation = old->rotation;
    new->keystone = old->keystone;
    return 0;
  }
  return 1;
}
#endif

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "rotation"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "keystone"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "rotation", GTK_WIDGET(g->rotation));
  dt_accel_connect_slider_iop(self, "keystone", GTK_WIDGET(g->keystone));
}

#define generate_mat3inv_body(c_type, A, B)                                                                  \
  int mat3inv_##c_type(c_type *const dst, const c_type *const src)                                           \
  {                                                                                                          \
                                                                                                             \
    const c_type det = A(1, 1) * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3))                                     \
                       - A(2, 1) * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3))                                   \
                       + A(3, 1) * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));                                  \
                                                                                                             \
    const c_type epsilon = 1e-7f;                                                                            \
    if(fabs(det) < epsilon) return 1;                                                                        \
                                                                                                             \
    const c_type invDet = 1.0 / det;                                                                         \
                                                                                                             \
    B(1, 1) = invDet * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3));                                              \
    B(1, 2) = -invDet * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3));                                             \
    B(1, 3) = invDet * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));                                              \
                                                                                                             \
    B(2, 1) = -invDet * (A(3, 3) * A(2, 1) - A(3, 1) * A(2, 3));                                             \
    B(2, 2) = invDet * (A(3, 3) * A(1, 1) - A(3, 1) * A(1, 3));                                              \
    B(2, 3) = -invDet * (A(2, 3) * A(1, 1) - A(2, 1) * A(1, 3));                                             \
                                                                                                             \
    B(3, 1) = invDet * (A(3, 2) * A(2, 1) - A(3, 1) * A(2, 2));                                              \
    B(3, 2) = -invDet * (A(3, 2) * A(1, 1) - A(3, 1) * A(1, 2));                                             \
    B(3, 3) = invDet * (A(2, 2) * A(1, 1) - A(2, 1) * A(1, 2));                                              \
    return 0;                                                                                                \
  }

#define A(y, x) src[(y - 1) * 3 + (x - 1)]
#define B(y, x) dst[(y - 1) * 3 + (x - 1)]
/** inverts the given 3x3 matrix */
generate_mat3inv_body(float, A, B)

int mat3inv(float *const dst, const float *const src)
{
  return mat3inv_float(dst, src);
}

generate_mat3inv_body(double, A, B)
#undef B
#undef A
#undef generate_mat3inv_body

void mat3mulv(float *dst, const float *const mat, const float *const v)
{
  for(int k = 0; k < 3; k++)
  {
    float x = 0.0f;
    for(int i = 0; i < 3; i++) x += mat[3 * k + i] * v[i];
    dst[k] = x;
  }
}

void mat3mul(float *dst, const float *const m1, const float *const m2)
{
  for(int k = 0; k < 3; k++)
  {
    for(int i = 0; i < 3; i++)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++) x += m1[3 * k + j] * m2[3 * j + i];
      dst[3 * k + i] = x;
    }
  }
}

void _print_roi(const dt_iop_roi_t *roi, const char *label)
{
  printf("{ %5d  %5d  %5d  %5d  %.6f } %s\n", roi->x, roi->y, roi->width, roi->height, roi->scale, label);
}


static void homography(float *homograph, const float angle, const float shift,
                       const int width, const int height, dt_iop_ashift_homodir_t dir)
{
  const float u = width;
  const float v = height;

  const float phi = (angle / 180.0f) * M_PI;
  const float cosi = cos(phi);
  const float sini = sin(phi);

  //const float f_global = 100.0f;
  //const float horifac = 0.0f;

  const float kst = shift;
  const float kst2 = kst * v + 1.0f;

#if 0
  const float exppa = exp(shift);
  const float fdb = f_global / (14.4f + (v / u - 1) * 7.2f);
  const float rad = fdb * (exppa - 1.0f) / (exppa + 1.0f);
  const float alpha = CLAMP(atan(rad), -1.5f, 1.5f);
  const float rt = sin(0.5f * alpha);
  const float r = fmax(0.1f, 2.0f * (horifac - 1.0f) * rt * rt + 1.0f);
#endif

  const float beta = atan2(v, u);
  const float diag = sqrt(u * u + v * v);

  // three intermediate buffers for matrix calculation
  float m1[3][3], m2[3][3], m3[3][3];


  // step 1: translation of image origin to image center
  memset(m1, 0, sizeof(m1));
  m1[0][0] = 1.0f;
  m1[1][1] = 1.0f;
  m1[2][2] = 1.0f;
  m1[0][2] = -0.5f * u;
  m1[1][2] = -0.5f * v;

  // step 2: rotation of image around center
  memset(m2, 0, sizeof(m2));
  m2[0][0] = cosi;
  m2[0][1] = -sini;
  m2[1][0] = sini;
  m2[1][1] = cosi;
  m2[2][2] = 1.0f;

  // multiply m2 * m1 -> m3
  mat3mul((float *)m3, (float *)m2, (float *)m1);

#if 0
  // step 3: shift correction
  memset(m1, 0, sizeof(m1));
  m1[0][0] = exppa;
  m1[1][0] = 0.5f* ((exppa - 1.0f) * u) / v;
  m1[1][1] = 2.0f * exppa / (exppa + 1.0f);
  m1[1][2] = -0.5f * ((exppa - 1.0f) * u ) / (exppa + 1.0f);
  m1[2][0] = (exppa - 1.0f) / v;
  m1[2][2] = 1.0f;
#else
  // step 3: shift correction
  memset(m1, 0, sizeof(m1));
  m1[0][0] = 1.0f; //kst2;
  m1[1][0] = 0.0f; //0.5f * (kst2 - 1.0f) * (u / v);
  m1[1][1] = 1.0f; //2.0f * kst2 / (1.0f + kst2);
  m1[1][2] = 0.0f; //0.5f * u * (1.0f - kst2) / (1.0f + kst2);
  m1[2][0] = 0.0f; //kst;
  m1[2][2] = 1.0f;
#endif

  // multiply m1 * m3 -> m2
  mat3mul((float *)m2, (float *)m1, (float *)m3);


  // step 4: horizontal compression
  memset(m1, 0, sizeof(m1));
  m1[0][0] = 1.0f;
  m1[1][1] = 1.0f; //r;
  m1[1][2] = 0.0f; //0.5f* u * (1.0f - r);
  m1[2][2] = 1.0f;

  // multiply m1 * m2 -> m3
  mat3mul((float *)m3, (float *)m1, (float *)m2);


  // step 5: adjust for change of dimension size versus original viewport.
  // we want to make sure that we don't get pixels with negative
  // x/y coordinates after adjustments, so we need to translate accordingly.
  // TODO: there must be a more elegant mathematical expression
  memset(m1, 0, sizeof(m1));
  m1[0][0] = 1.0f;
  m1[1][1] = 1.0f;
  m1[2][2] = 1.0f;
  m1[0][2] = 0.5f * diag * fmax(fabs(cos(beta + phi)), fabs(cos(beta - phi))) - 0.5 * u;
  m1[1][2] = 0.5f * diag * fmax(fabs(sin(beta + phi)), fabs(sin(beta - phi))) - 0.5 * v;

  // multiply m1 * m3 -> m2
  mat3mul((float *)m2, (float *)m1, (float *)m3);


  // step 5: translate image back to origin
  memset(m1, 0, sizeof(m1));
  m1[0][0] = 1.0f;
  m1[1][1] = 1.0f;
  m1[2][2] = 1.0f;
  m1[0][2] = 0.5f * u;
  m1[1][2] = 0.5f * v;

  // multiply m1 * m2 -> m3
  mat3mul((float *)m3, (float *)m1, (float *)m2);

  if(dir == ASHIFT_HOMOGRAPH_INVERTED)
  {
    // generate inverted homograph
    if(mat3inv((float *)homograph, (float *)m3))
    {
      // in case of error we set to unity matrix
      memset(m3, 0, sizeof(m3));
      m3[0][0] = 1.0f;
      m3[1][1] = 1.0f;
      m3[2][2] = 1.0f;
      memcpy(homograph, m3, sizeof(m3));
    }
  }
  else
  {
    memcpy(homograph, m3, sizeof(m3));
  }
}


int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;

  float homograph[3][3];
  homography((float *)homograph, data->rotation, data->keystone,
             piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_FORWARD);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)                                                      \
  shared(points, points_count, homograph)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    // as points are already in orignal image coordinates we can homograph them directly
    float pi[3] = { points[i], points[i + 1], 1.0f };
    float po[3];
    mat3mulv(po, (float *)homograph, pi);
    points[i] = po[0] / po[2];
    points[i + 1] = po[1] / po[2];
  }

  return 1;
}


int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points,
                          size_t points_count)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;

  float ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->keystone,
             piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)                                                      \
  shared(points, points_count, ihomograph)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    // as points are already in original image coordinates we can homograph them directly
    float pi[3] = { points[i], points[i + 1], 1.0f };
    float po[3];
    mat3mulv(po, (float *)ihomograph, pi);
    points[i] = po[0] / po[2];
    points[i + 1] = po[1] / po[2];
  }

  return 1;
}

void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;
  *roi_out = *roi_in;

  float homograph[3][3];
  homography((float *)homograph, data->rotation, data->keystone,
             piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_FORWARD);

  float xm = FLT_MAX, xM = -FLT_MAX, ym = FLT_MAX, yM = -FLT_MAX;

  // go through all four vertices of input roi and convert coordinates to output
  for(int y = 0; y < roi_in->height; y += roi_in->height - 1)
  {
    for(int x = 0; x < roi_in->width; x += roi_in->width - 1)
    {
      float pin[3], pout[3];

      // convert from input coordinates to original image coordinates
      pin[0] = roi_in->x + x;
      pin[1] = roi_in->y + y;
      pin[0] /= roi_in->scale;
      pin[1] /= roi_in->scale;
      pin[2] = 1.0f;

      // apply hompgraph
      mat3mulv(pout, (float *)homograph, pin);

      // convert to output image coordinates
      pout[0] /= pout[2];
      pout[1] /= pout[2];
      pout[0] *= roi_out->scale;
      pout[1] *= roi_out->scale;
      xm = MIN(xm, pout[0]);
      xM = MAX(xM, pout[0]);
      ym = MIN(ym, pout[1]);
      yM = MAX(yM, pout[1]);
    }
  }

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  roi_out->x = fmaxf(0.0f, xm - interpolation->width);
  roi_out->y = fmaxf(0.0f, ym - interpolation->width);
  roi_out->width = ceilf(xM - roi_out->x + 1 + interpolation->width);
  roi_out->height = ceilf(yM - roi_out->y + 1 + interpolation->width);

  // sanity check.
  roi_out->x = CLAMP(roi_out->x, 0, MAX(roi_in->width, roi_in->height));
  roi_out->y = CLAMP(roi_out->y, 0, MAX(roi_in->width, roi_in->height));
  roi_out->width = CLAMP(roi_out->width, 1, sqrt(2.0f) * MAX(roi_in->width, roi_in->height));
  roi_out->height = CLAMP(roi_out->height, 1, sqrt(2.0f) * MAX(roi_in->width, roi_in->height));

  //_print_roi(roi_out, "roi_out");
  //_print_roi(roi_in, "roi_in");

}

void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;
  *roi_in = *roi_out;

  float ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->keystone,
             piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;

  float xm = FLT_MAX, xM = -FLT_MAX, ym = FLT_MAX, yM = -FLT_MAX;

  // go through all four vertices of output roi and convert coordinates to input
  for(int y = 0; y < roi_out->height; y += roi_out->height - 1)
  {
    for(int x = 0; x < roi_out->width; x += roi_out->width - 1)
    {
      float pin[3], pout[3];

      // convert from output image coordinates to original image coordinates
      pout[0] = roi_out->x + x;
      pout[1] = roi_out->y + y;
      pout[0] /= roi_out->scale;
      pout[1] /= roi_out->scale;
      pout[2] = 1.0f;

      // apply homograph
      mat3mulv(pin, (float *)ihomograph, pout);

      // convert to input image coordinates
      pin[0] /= pin[2];
      pin[1] /= pin[2];
      pin[0] *= roi_in->scale;
      pin[1] *= roi_in->scale;
      xm = MIN(xm, pin[0]);
      xM = MAX(xM, pin[0]);
      ym = MIN(ym, pin[1]);
      yM = MAX(yM, pin[1]);
    }
  }

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  roi_in->x = fmaxf(0.0f, xm - interpolation->width);
  roi_in->y = fmaxf(0.0f, ym - interpolation->width);
  roi_in->width = fminf(ceilf(orig_w) - roi_in->x, xM - roi_in->x + 1 + interpolation->width);
  roi_in->height = fminf(ceilf(orig_h) - roi_in->y, yM - roi_in->y + 1 + interpolation->width);

  // sanity check.
  roi_in->x = CLAMP(roi_in->x, 0, (int)floorf(orig_w));
  roi_in->y = CLAMP(roi_in->y, 0, (int)floorf(orig_h));
  roi_in->width = CLAMP(roi_in->width, 1, (int)ceilf(orig_w) - roi_in->x);
  roi_in->height = CLAMP(roi_in->height, 1, (int)ceilf(orig_h) - roi_in->y);

  //_print_roi(roi_out, "roi_out");
  //_print_roi(roi_in, "roi_in");
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;
  //dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  const int ch = piece->colors;
  const int ch_width = ch * roi_in->width;

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

  float ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->keystone,
             piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)                                                      \
  shared(ivoid, ovoid, roi_in, roi_out, ihomograph, interpolation)
#endif
  // go over all pixels of output image
  for(int j = 0; j < roi_out->height; j++)
  {
    float *out = ((float *)ovoid) + (size_t)ch * j * roi_out->width;
    for(int i = 0; i < roi_out->width; i++, out += ch)
    {
      float pin[3], pout[3];

      // convert output pixel coordinates to original image coordinates
      pout[0] = roi_out->x + i;
      pout[1] = roi_out->y + j;
      pout[0] /= roi_out->scale;
      pout[1] /= roi_out->scale;
      pout[2] = 1.0f;

      // apply homograph
      mat3mulv(pin, (float *)ihomograph, pout);

      // convert to input pixel coordinates
      pin[0] /= pin[2];
      pin[1] /= pin[2];
      pin[0] *= roi_in->scale;
      pin[1] *= roi_in->scale;
      pin[0] -= roi_in->x;
      pin[1] -= roi_in->y;

      // get output values by interpolation from input image
      dt_interpolation_compute_pixel4c(interpolation, (float *)ivoid, out, pin[0], pin[1], roi_in->width,
                                       roi_in->height, ch_width);
    }
  }
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 2.0f;
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

static void rotation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->rotation = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void keystone_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->keystone = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)p1;
  dt_iop_ashift_data_t *d = (dt_iop_ashift_data_t *)piece->data;

  d->rotation = p->rotation;
  d->keystone = p->keystone;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_ashift_data_t *d = (dt_iop_ashift_data_t *)calloc(1, sizeof(dt_iop_ashift_data_t));
  piece->data = (void *)d;
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)module->params;
  dt_bauhaus_slider_set(g->rotation, p->rotation);
  dt_bauhaus_slider_set(g->keystone, p->keystone);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_ashift_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_ashift_params_t));
  module->default_enabled = 0;
  module->priority = 270; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_ashift_params_t);
  module->gui_data = NULL;
  dt_iop_ashift_params_t tmp = (dt_iop_ashift_params_t){ 0.0f, 0.0f };
  memcpy(module->params, &tmp, sizeof(dt_iop_ashift_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_ashift_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_ashift_global_data_t *gd
      = (dt_iop_ashift_global_data_t *)malloc(sizeof(dt_iop_ashift_global_data_t));
  module->data = gd;
  //const int program = 13; // colorcorrection.cl, from programs.conf
  //gd->kernel_ashift_void = dt_opencl_create_kernel(program, "ashift_void");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  //dt_iop_ashift_global_data_t *gd = (dt_iop_ashift_global_data_t *)module->data;
  //dt_opencl_free_kernel(gd->kernel_ashift_void);
  free(module->data);
  module->data = NULL;
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_ashift_gui_data_t));
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;

  dt_pthread_mutex_init(&g->lock, NULL);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->rotation = dt_bauhaus_slider_new_with_range(self, -10.0f, 10.0f, 0.1f, p->rotation, 2);
  g->keystone = dt_bauhaus_slider_new_with_range(self, -2.0f, 2.0f, 0.01f, p->keystone, 2);

  dt_bauhaus_widget_set_label(g->rotation, NULL, _("rotation"));
  dt_bauhaus_widget_set_label(g->keystone, NULL, _("keystone"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->rotation, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->keystone, TRUE, TRUE, 0);

  g_object_set(g->rotation, "tooltip-text", _("rotation angle"), (char *)NULL);
  g_object_set(g->keystone, "tooltip-text", _("keystone correction"), (char *)NULL);

  g_signal_connect(G_OBJECT(g->rotation), "value-changed", G_CALLBACK(rotation_callback), self);
  g_signal_connect(G_OBJECT(g->keystone), "value-changed", G_CALLBACK(keystone_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  dt_pthread_mutex_destroy(&g->lock);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
