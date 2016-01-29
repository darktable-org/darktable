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
#include "gui/draw.h"
#include <gtk/gtk.h>
#include <inttypes.h>

// Motivation to this module comes from the program ShiftN (http://www.shiftn.de) by
// Marcus Hebel.

// Thanks to Marcus for his support when implementing part of the ShiftN functionality
// to darktable.


DT_MODULE_INTROSPECTION(1, dt_iop_ashift_params_t)

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

typedef enum dt_iop_ashift_homodir_t
{
  ASHIFT_HOMOGRAPH_FORWARD,
  ASHIFT_HOMOGRAPH_INVERTED
} dt_iop_ashift_homodir_t;

typedef struct dt_iop_ashift_params_t
{
  float rotation;
  float lensshift_v;
  float lensshift_h;
} dt_iop_ashift_params_t;


typedef struct dt_iop_ashift_gui_data_t
{
  GtkWidget *rotation;
  GtkWidget *lensshift_v;
  GtkWidget *lensshift_h;
  int isflipped;
  dt_pthread_mutex_t lock;
} dt_iop_ashift_gui_data_t;

typedef struct dt_iop_ashift_data_t
{
  float rotation;
  float lensshift_v;
  float lensshift_h;
} dt_iop_ashift_data_t;

typedef struct dt_iop_ashift_global_data_t
{
  int kernel_ashift_bilinear;
  int kernel_ashift_bicubic;
  int kernel_ashift_lanczos2;
  int kernel_ashift_lanczos3;
} dt_iop_ashift_global_data_t;


#if 0
int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    const dt_iop_ashift_params1_t *old = old_params;
    dt_iop_ashift_params_t *new = new_params;
    new->rotation = old->rotation;
    new->lensshift_v = old->lensshift_v;
    new->lensshift_h = old->lensshift_h;
    return 0;
  }
  return 1;
}
#endif

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "rotation"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "lens shift (v)"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "lens shift (h)"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "rotation", GTK_WIDGET(g->rotation));
  dt_accel_connect_slider_iop(self, "lens shift (v)", GTK_WIDGET(g->lensshift_v));
  dt_accel_connect_slider_iop(self, "lens shift (h)", GTK_WIDGET(g->lensshift_h));
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


static void homography(float *homograph, const float angle, const float shift_v, const float shift_h,
                       const int width, const int height, dt_iop_ashift_homodir_t dir)
{
  // calculate homograph that combines all translations, rotations
  // and warping into one single matrix operation.
  // this is heavily leaning on ShiftN where the homographic matrix expects
  // input in (y : x : 1) format. in the darktable world we want to keep the
  // (x : y : 1) convention. therefore we need to flip coordinates first and
  // make sure that output is in correct format after corrections are applied.

  const float u = width;
  const float v = height;

  // inverted convention of rotation angle. only used for user convenience
  // so that right shifting the slider intuitively produces a clockwise image rotation
  const float phi = (-angle / 180.0f) * M_PI;
  const float cosi = cos(phi);
  const float sini = sin(phi);

  // all this comes from ShiftN
  const float f_global = 28.0; // TODO: this is a parameter in ShiftN -> check use in darktable (in comb. with horifac)
  const float horifac = 1.0f;  // TODO: see f_global
  const float exppa_v = exp(shift_v);
  const float fdb_v = f_global / (14.4f + (v / u - 1) * 7.2f);
  const float rad_v = fdb_v * (exppa_v - 1.0f) / (exppa_v + 1.0f);
  const float alpha_v = CLAMP(atan(rad_v), -1.5f, 1.5f);
  const float rt_v = sin(0.5f * alpha_v);
  const float r_v = fmax(0.1f, 2.0f * (horifac - 1.0f) * rt_v * rt_v + 1.0f);

  const float vertifac = 1.0f;  // TODO: see f_global
  const float exppa_h = exp(shift_h);
  const float fdb_h = f_global / (14.4f + (u / v - 1) * 7.2f);
  const float rad_h = fdb_h * (exppa_h - 1.0f) / (exppa_h + 1.0f);
  const float alpha_h = CLAMP(atan(rad_h), -1.5f, 1.5f);
  const float rt_h = sin(0.5f * alpha_h);
  const float r_h = fmax(0.1f, 2.0f * (vertifac - 1.0f) * rt_h * rt_h + 1.0f);



  // three intermediate buffers for matrix calculation
  float m1[3][3], m2[3][3], m3[3][3];


  // Step 1: flip x and y coordinates (see above)
  memset(m1, 0, sizeof(m1));
  m1[0][1] = 1.0f;
  m1[1][0] = 1.0f;
  m1[2][2] = 1.0f;


  // Step 2: rotation of image around its center
  memset(m2, 0, sizeof(m2));
  m2[0][0] = cosi;
  m2[0][1] = -sini;
  m2[1][0] = sini;
  m2[1][1] = cosi;
  m2[0][2] = -0.5f * v * cosi + 0.5f * u * sini + 0.5f * v;
  m2[1][2] = -0.5f * v * sini - 0.5f * u * cosi + 0.5f * u;
  m2[2][2] = 1.0f;

  // multiply m2 * m1 -> m3
  mat3mul((float *)m3, (float *)m2, (float *)m1);


  // Step 3: apply vertical lens shift effect
  memset(m1, 0, sizeof(m1));
  m1[0][0] = exppa_v;
  m1[1][0] = 0.5f* ((exppa_v - 1.0f) * u) / v;
  m1[1][1] = 2.0f * exppa_v / (exppa_v + 1.0f);
  m1[1][2] = -0.5f * ((exppa_v - 1.0f) * u ) / (exppa_v + 1.0f);
  m1[2][0] = (exppa_v - 1.0f) / v;
  m1[2][2] = 1.0f;

  // multiply m1 * m3 -> m2
  mat3mul((float *)m2, (float *)m1, (float *)m3);


  // Step 4: horizontal compression
  memset(m1, 0, sizeof(m1));
  m1[0][0] = 1.0f;
  m1[1][1] = r_v;
  m1[1][2] = 0.5f * u * (1.0f - r_v);
  m1[2][2] = 1.0f;

  // multiply m1 * m2 -> m3
  mat3mul((float *)m3, (float *)m1, (float *)m2);


  // Step 5: flip x and y back again
  memset(m1, 0, sizeof(m1));
  m1[0][1] = 1.0f;
  m1[1][0] = 1.0f;
  m1[2][2] = 1.0f;

  // multiply m1 * m3 -> m2
  mat3mul((float *)m2, (float *)m1, (float *)m3);

  // from here output vectors would be in (x : y : 1) format

  // Step 6: now we can apply horizontal lens shift with the same matrix format as above
  memset(m1, 0, sizeof(m1));
  m1[0][0] = exppa_h;
  m1[1][0] = 0.5f* ((exppa_h - 1.0f) * v) / u;
  m1[1][1] = 2.0f * exppa_h / (exppa_h + 1.0f);
  m1[1][2] = -0.5f * ((exppa_h - 1.0f) * v ) / (exppa_h + 1.0f);
  m1[2][0] = (exppa_h - 1.0f) / u;
  m1[2][2] = 1.0f;

  // multiply m1 * m2 -> m3
  mat3mul((float *)m3, (float *)m1, (float *)m2);


  // Step 7: vertical compression
  memset(m1, 0, sizeof(m1));
  m1[0][0] = 1.0f;
  m1[1][1] = r_h;
  m1[1][2] = 0.5f * v * (1.0f - r_h);
  m1[2][2] = 1.0f;

  // multiply m1 * m3 -> m2
  mat3mul((float *)m2, (float *)m1, (float *)m3);


  // Step 8: find x/y offsets and apply according correction so that
  // no negative coordinates occur in output vector
  float umin = FLT_MAX, vmin = FLT_MAX;
  // visit all four corners
  for(int y = 0; y < height; y += height - 1)
    for(int x = 0; x < width; x += width - 1)
    {
      float pi[3], po[3];
      pi[0] = x;
      pi[1] = y;
      pi[2] = 1.0f;
      // m2 expects input in (x:y:1) format and gives output as (x:y:1)
      mat3mulv(po, (float *)m2, pi);
      umin = fmin(umin, po[0] / po[2]);
      vmin = fmin(vmin, po[1] / po[2]);
    }
  memset(m1, 0, sizeof(m1));
  m1[0][0] = 1.0f;
  m1[1][1] = 1.0f;
  m1[2][2] = 1.0f;
  m1[0][2] = -umin;
  m1[1][2] = -vmin;

  // multiply m1 * m2 -> m3
  mat3mul((float *)m3, (float *)m1, (float *)m2);


  // on request we either keep the final matrix for forward conversions
  // or produce an inverted matrix for backward conversions
  if(dir == ASHIFT_HOMOGRAPH_FORWARD)
  {
    memcpy(homograph, m3, sizeof(m3));
  }
  else
  {
    // generate inverted homograph
    if(mat3inv((float *)homograph, (float *)m3))
    {
      // in case of error we set to unity matrix
      memset(m1, 0, sizeof(m1));
      m1[0][0] = 1.0f;
      m1[1][1] = 1.0f;
      m1[2][2] = 1.0f;
      memcpy(homograph, m1, sizeof(m1));
    }
  }
}


int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;

  float homograph[3][3];
  homography((float *)homograph, data->rotation, data->lensshift_v, data->lensshift_h,
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
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h,
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
  homography((float *)homograph, data->rotation, data->lensshift_v, data->lensshift_h,
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
  roi_out->width = floorf(xM - roi_out->x + 1 + interpolation->width);
  roi_out->height = floorf(yM - roi_out->y + 1 + interpolation->width);

  // sanity check.
  roi_out->x = CLAMP(roi_out->x, 0, INT_MAX);
  roi_out->y = CLAMP(roi_out->y, 0, INT_MAX);
  roi_out->width = CLAMP(roi_out->width, 1, INT_MAX);
  roi_out->height = CLAMP(roi_out->height, 1, INT_MAX);

  //_print_roi(roi_out, "roi_out");
  //_print_roi(roi_in, "roi_in");

}

void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;
  *roi_in = *roi_out;

  float ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h,
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
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  const int ch = piece->colors;
  const int ch_width = ch * roi_in->width;

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

  float ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h,
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

  if(self->dev->gui_attached && g && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    // we want to find out if the final output image is flipped in relation to this iop
    // so we can adjust the gui labels accordingly

    // origin of image and opposite corner as reference points
    float points[4] = { 0.0f, 0.0f, (float)piece->buf_in.width, (float)piece->buf_in.height };
    float ivec[2] = { points[2] - points[0], points[3] - points[1] };
    float ivecl = sqrt(ivec[0] * ivec[0] + ivec[1] * ivec[1]);

    // where do they go?
    dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->priority + 1, 9999999, points, 2);

    float ovec[2] = { points[2] - points[0], points[3] - points[1] };
    float ovecl = sqrt(ovec[0] * ovec[0] + ovec[1] * ovec[1]);

    // angle between input vector and output vector
    float alpha = acos(CLAMP((ivec[0] * ovec[0] + ivec[1] * ovec[1]) / (ivecl * ovecl), -1.0f, 1.0f));

    // we are interested if |alpha| is in the range of 90° +/- 45° -> we assume the image is flipped
    int isflipped = fabs(fmod(alpha + M_PI, M_PI) - M_PI/2.0f) < M_PI/4.0f ? 1 : 0;

    dt_pthread_mutex_lock(&g->lock);
    g->isflipped = isflipped;
    dt_pthread_mutex_unlock(&g->lock);
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_ashift_data_t *d = (dt_iop_ashift_data_t *)piece->data;
  dt_iop_ashift_global_data_t *gd = (dt_iop_ashift_global_data_t *)self->data;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  float ihomograph[3][3];
  homography((float *)ihomograph, d->rotation, d->lensshift_v, d->lensshift_h,
             piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

  cl_int err = -999;
  cl_mem dev_homo = NULL;

  const int devid = piece->pipe->devid;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int width = roi_out->width;
  const int height = roi_out->height;

  dev_homo = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 9, ihomograph);
  if(dev_homo == NULL) goto error;

  const int iroi[2] = { roi_in->x, roi_in-> y };
  const int oroi[2] = { roi_out->x, roi_out-> y };
  const float in_scale = roi_in->scale;
  const float out_scale = roi_out->scale;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

  int ldkernel = -1;

  switch(interpolation->id)
  {
    case DT_INTERPOLATION_BILINEAR:
      ldkernel = gd->kernel_ashift_bilinear;
      break;
    case DT_INTERPOLATION_BICUBIC:
      ldkernel = gd->kernel_ashift_bicubic;
      break;
    case DT_INTERPOLATION_LANCZOS2:
      ldkernel = gd->kernel_ashift_lanczos2;
      break;
    case DT_INTERPOLATION_LANCZOS3:
      ldkernel = gd->kernel_ashift_lanczos3;
      break;
    default:
      return FALSE;
  }

  dt_opencl_set_kernel_arg(devid, ldkernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, ldkernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, ldkernel, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, ldkernel, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, ldkernel, 4, sizeof(int), (void *)&iwidth);
  dt_opencl_set_kernel_arg(devid, ldkernel, 5, sizeof(int), (void *)&iheight);
  dt_opencl_set_kernel_arg(devid, ldkernel, 6, 2*sizeof(int), (void *)iroi);
  dt_opencl_set_kernel_arg(devid, ldkernel, 7, 2*sizeof(int), (void *)oroi);
  dt_opencl_set_kernel_arg(devid, ldkernel, 8, sizeof(float), (void *)&in_scale);
  dt_opencl_set_kernel_arg(devid, ldkernel, 9, sizeof(float), (void *)&out_scale);
  dt_opencl_set_kernel_arg(devid, ldkernel, 10, sizeof(cl_mem), (void *)&dev_homo);
  err = dt_opencl_enqueue_kernel_2d(devid, ldkernel, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_homo);

  if(self->dev->gui_attached && g && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    // we want to find out if the final output image is flipped in relation to this iop
    // so we can adjust the gui labels accordingly

    // origin of image and opposite corner as reference points
    float points[4] = { 0.0f, 0.0f, (float)piece->buf_in.width, (float)piece->buf_in.height };
    float ivec[2] = { points[2] - points[0], points[3] - points[1] };
    float ivecl = sqrt(ivec[0] * ivec[0] + ivec[1] * ivec[1]);

    // where do they go?
    dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->priority + 1, 9999999, points, 2);

    float ovec[2] = { points[2] - points[0], points[3] - points[1] };
    float ovecl = sqrt(ovec[0] * ovec[0] + ovec[1] * ovec[1]);

    // angle between input vector and output vector
    float alpha = acos(CLAMP((ivec[0] * ovec[0] + ivec[1] * ovec[1]) / (ivecl * ovecl), -1.0f, 1.0f));

    // we are interested if |alpha| is in the range of 90° +/- 45° -> we assume the image is flipped
    int isflipped = fabs(fmod(alpha + M_PI, M_PI) - M_PI/2.0f) < M_PI/4.0f ? 1 : 0;

    dt_pthread_mutex_lock(&g->lock);
    g->isflipped = isflipped;
    dt_pthread_mutex_unlock(&g->lock);
  }

  return TRUE;

error:
  if(dev_homo != NULL) dt_opencl_release_mem_object(dev_homo);
  dt_print(DT_DEBUG_OPENCL, "[opencl_ashift] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 2.0f;
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 3;  // account for interpolation width
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

static void lensshift_v_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->lensshift_v = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lensshift_h_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->lensshift_h = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)p1;
  dt_iop_ashift_data_t *d = (dt_iop_ashift_data_t *)piece->data;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  d->rotation = p->rotation;
  d->lensshift_v = p->lensshift_v;
  d->lensshift_h = p->lensshift_h;
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
  dt_bauhaus_slider_set(g->lensshift_v, p->lensshift_v);
  dt_bauhaus_slider_set(g->lensshift_h, p->lensshift_h);

  dt_pthread_mutex_lock(&g->lock);
  g->isflipped = -1;
  dt_pthread_mutex_unlock(&g->lock);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_ashift_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_ashift_params_t));
  module->default_enabled = 0;
  module->priority = 260; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_ashift_params_t);
  module->gui_data = NULL;
  dt_iop_ashift_params_t tmp = (dt_iop_ashift_params_t){ 0.0f, 0.0f, 0.0f };
  memcpy(module->params, &tmp, sizeof(dt_iop_ashift_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_ashift_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_ashift_global_data_t *gd
      = (dt_iop_ashift_global_data_t *)malloc(sizeof(dt_iop_ashift_global_data_t));
  module->data = gd;

  const int program = 2; // basic.cl, from programs.conf
  gd->kernel_ashift_bilinear = dt_opencl_create_kernel(program, "ashift_bilinear");
  gd->kernel_ashift_bicubic = dt_opencl_create_kernel(program, "ashift_bicubic");
  gd->kernel_ashift_lanczos2 = dt_opencl_create_kernel(program, "ashift_lanczos2");
  gd->kernel_ashift_lanczos3 = dt_opencl_create_kernel(program, "ashift_lanczos3");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_ashift_global_data_t *gd = (dt_iop_ashift_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_ashift_bilinear);
  dt_opencl_free_kernel(gd->kernel_ashift_bicubic);
  dt_opencl_free_kernel(gd->kernel_ashift_lanczos2);
  dt_opencl_free_kernel(gd->kernel_ashift_lanczos3);
  free(module->data);
  module->data = NULL;
}

// adjust labels of lens shift parameters according to flip status of image
static gboolean draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  if(darktable.gui->reset) return FALSE;

  dt_pthread_mutex_lock(&g->lock);
  const int isflipped = g->isflipped;
  g->isflipped = -1;
  dt_pthread_mutex_unlock(&g->lock);

  // no data after last visit
  if(isflipped == -1) return FALSE;

  char string_v[256];
  char string_h[256];

  snprintf(string_v, sizeof(string_v), _("lens shift (%s)"), isflipped ? _("horizontal") : _("vertical"));
  snprintf(string_h, sizeof(string_h), _("lens shift (%s)"), isflipped ? _("vertical") : _("horizontal"));

  darktable.gui->reset = 1;
  dt_bauhaus_widget_set_label(g->lensshift_v, NULL, string_v);
  dt_bauhaus_widget_set_label(g->lensshift_h, NULL, string_h);
  darktable.gui->reset = 0;

  return FALSE;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_ashift_gui_data_t));
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;

  dt_pthread_mutex_init(&g->lock, NULL);
  dt_pthread_mutex_lock(&g->lock);
  g->isflipped = -1;
  dt_pthread_mutex_unlock(&g->lock);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->rotation = dt_bauhaus_slider_new_with_range(self, -10.0f, 10.0f, 0.1f, p->rotation, 2);
  g->lensshift_v = dt_bauhaus_slider_new_with_range(self, -1.0f, 1.0f, 0.01f, p->lensshift_v, 2);
  g->lensshift_h = dt_bauhaus_slider_new_with_range(self, -1.0f, 1.0f, 0.01f, p->lensshift_h, 2);

  dt_bauhaus_widget_set_label(g->rotation, NULL, _("rotation"));
  dt_bauhaus_widget_set_label(g->lensshift_v, NULL, _("lens shift (vertical)"));
  dt_bauhaus_widget_set_label(g->lensshift_h, NULL, _("lens shift (horizontal)"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->rotation, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->lensshift_v, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->lensshift_h, TRUE, TRUE, 0);

  g_object_set(g->rotation, "tooltip-text", _("rotate image"), (char *)NULL);
  g_object_set(g->lensshift_v, "tooltip-text", _("apply lens lens shift correction in one direction"), (char *)NULL);
  g_object_set(g->lensshift_h, "tooltip-text", _("apply lens lens shift correction in one direction"), (char *)NULL);

  g_signal_connect(G_OBJECT(g->rotation), "value-changed", G_CALLBACK(rotation_callback), self);
  g_signal_connect(G_OBJECT(g->lensshift_v), "value-changed", G_CALLBACK(lensshift_v_callback), self);
  g_signal_connect(G_OBJECT(g->lensshift_h), "value-changed", G_CALLBACK(lensshift_h_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(draw), self);
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
