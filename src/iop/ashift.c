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

// during development only:
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

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
#include "common/colorspaces.h"
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

// For line detection we use the LSD algorithm as published by Rafael Grompone:
//
//  "LSD: a Line Segment Detector" by Rafael Grompone von Gioi,
//  Jeremie Jakubowicz, Jean-Michel Morel, and Gregory Randall,
//  Image Processing On Line, 2012. DOI:10.5201/ipol.2012.gjmr-lsd
//  http://dx.doi.org/10.5201/ipol.2012.gjmr-lsd
#include "ashift_lsd.c"

#define MIN_LINE_LENGTH 10                  // the minimum length of a line in pixels to be regarded as relevant
#define MAX_TANGENTIAL_DEVIATION 30         // by how many degrees a line may deviate from the +/-180 and +/-90 to be regarded as relevant


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

typedef enum dt_iop_ashift_edge_t
{
  ASHIFT_EDGE_HORIZONTAL,
  ASHIFT_EDGE_VERTICAL,
} dt_iop_ashift_edge_t;

typedef enum dt_iop_ashift_linetype_t
{
  ASHIFT_LINE_IRRELEVANT = 0,       // the line is found to be not interesting
                                    // eg. too short, or not horizontal or vertical
  ASHIFT_LINE_VERTICAL   = 1 << 0,  // the line is (mostly) vertical
  ASHIFT_LINE_VSELECTED  = 1 << 1,  // vertical line is selected
  ASHIFT_LINE_HORIZONTAL = 1 << 2,  // the line is (mostly) horizontal
  ASHIFT_LINE_HSELECTED  = 1 << 3   // horizontal line is selected
} dt_iop_ashift_linetype_t;

typedef struct dt_iop_ashift_params_t
{
  float rotation;
  float lensshift_v;
  float lensshift_h;
} dt_iop_ashift_params_t;

typedef struct dt_iop_ashift_line_t
{
  float p1[3];
  float p2[3];
  float length;
  float width;
  float weight;
  dt_iop_ashift_linetype_t type;
  // homogeneous coordinates:
  float L[3];
} dt_iop_ashift_line_t;

typedef struct dt_iop_ashift_gui_data_t
{
  GtkWidget *rotation;
  GtkWidget *lensshift_v;
  GtkWidget *lensshift_h;
  GtkWidget *fit;
  int isflipped;
  int lines_in_width;
  int lines_in_height;
  int lines_count;
  int vertical_count;
  int horizontal_count;
  float *buf;
  int buf_width;
  int buf_height;
  int buf_x_off;
  int buf_y_off;
  float buf_scale;
  uint64_t buf_hash;
  dt_iop_ashift_line_t *lines;
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

// multiply 3x3 matrix with 3x1 vector
static inline void mat3mulv(float *dst, const float *const mat, const float *const v)
{
  for(int k = 0; k < 3; k++)
  {
    float x = 0.0f;
    for(int i = 0; i < 3; i++) x += mat[3 * k + i] * v[i];
    dst[k] = x;
  }
}

// multiply two 3x3 matrices
static inline void mat3mul(float *dst, const float *const m1, const float *const m2)
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

// normalized product of two 3x1 vectors
static inline void vec3prodn(float *dst, const float *const v1, const float *const v2)
{
  const float l1 = v1[1] * v2[2] - v1[2] * v2[1];
  const float l2 = v1[2] * v2[0] - v1[0] * v2[2];
  const float l3 = v1[0] * v2[1] - v1[1] * v2[0];

  // normalize so that l1^2 + l2^2 + l3^3 = 1
  const float sq = sqrt(l1 * l1 + l2 * l2 + l3 * l3);

  const float f = sq > 0.0f ? 1.0f / sq : 1.0f;

  dst[0] = l1 * f;
  dst[1] = l2 * f;
  dst[2] = l3 * f;
}

// scalar product of two 3x1 vectors
static inline float vec3scalar(const float *const v1, const float *const v2)
{
  return (v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2]);
}

// check if 3x1 vector is (very close to) null
static inline int vec3isnull(const float *const v)
{
  const float eps = 1e-10f;
  return (fabs(v[0]) < eps && fabs(v[1]) < eps && fabs(v[2]) < eps);
}



static void _print_roi(const dt_iop_roi_t *roi, const char *label)
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
  const float f_global
      = 28.0; // TODO: this is a parameter in ShiftN -> check use in darktable (in comb. with horifac)
  const float horifac = 1.0f; // TODO: see f_global
  const float exppa_v = exp(shift_v);
  const float fdb_v = f_global / (14.4f + (v / u - 1) * 7.2f);
  const float rad_v = fdb_v * (exppa_v - 1.0f) / (exppa_v + 1.0f);
  const float alpha_v = CLAMP(atan(rad_v), -1.5f, 1.5f);
  const float rt_v = sin(0.5f * alpha_v);
  const float r_v = fmax(0.1f, 2.0f * (horifac - 1.0f) * rt_v * rt_v + 1.0f);

  const float vertifac = 1.0f; // TODO: see f_global
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
  m1[1][0] = 0.5f * ((exppa_v - 1.0f) * u) / v;
  m1[1][1] = 2.0f * exppa_v / (exppa_v + 1.0f);
  m1[1][2] = -0.5f * ((exppa_v - 1.0f) * u) / (exppa_v + 1.0f);
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
  m1[1][0] = 0.5f * ((exppa_h - 1.0f) * v) / u;
  m1[1][1] = 2.0f * exppa_h / (exppa_h + 1.0f);
  m1[1][2] = -0.5f * ((exppa_h - 1.0f) * v) / (exppa_h + 1.0f);
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
    // generate inverted homograph (mat3inv function defined in colorspaces.c)
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
  homography((float *)homograph, data->rotation, data->lensshift_v, data->lensshift_h, piece->buf_in.width,
             piece->buf_in.height, ASHIFT_HOMOGRAPH_FORWARD);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(points, points_count, homograph)
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
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h, piece->buf_in.width,
             piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(points, points_count, ihomograph)
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
  homography((float *)homograph, data->rotation, data->lensshift_v, data->lensshift_h, piece->buf_in.width,
             piece->buf_in.height, ASHIFT_HOMOGRAPH_FORWARD);

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
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h, piece->buf_in.width,
             piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

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

// simple conversion of rgb image into greyscale variant suitable for line segment detection
// the lsd routines expect input as *double in the range [0; 256]
static void rgb2grey256(const float *in, double *out, const int width, const int height)
{
  const int ch = 4;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out)
#endif
  for(int j = 0; j < height; j++)
  {
    const float *inp = in + (size_t)ch * j * width;
    double *outp = out + (size_t)j * width;
    for(int i = 0; i < width; i++, inp += ch, outp++)
    {
      *outp = (0.3f * inp[0] + 0.59f * inp[1] + 0.11f * inp[2]) * 256.0;
    }
  }
}

// simple conversion of rgb image into greyscale variant
static void rgb2grey(const float *in, float *out, const int width, const int height)
{
  const int ch = 4;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out)
#endif
  for(int j = 0; j < height; j++)
  {
    const float *inp = in + (size_t)ch * j * width;
    float *outp = out + (size_t)j * width;
    for(int i = 0; i < width; i++, inp += ch, outp++)
    {
      *outp = 0.3f * inp[0] + 0.59f * inp[1] + 0.11f * inp[2];
    }
  }
}


// support function only needed during development
static void grey2rgb(const float *in, float *out, const int out_width, const int out_height,
                     const int in_width, const int in_height)
{
  const int ch = 4;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out)
#endif
  for(int j = 0; j < out_height; j++)
  {
    if(j >= in_height) continue;
    const float *inp = in + (size_t)j * in_width;
    float *outp = out + (size_t)ch * j * out_width;
    for(int i = 0; i < out_width; i++, inp++, outp += ch)
    {
      if(i >= in_width) continue;
      outp[0] = outp[1] = outp[2] = *inp;
    }
  }
}

// sobel edge detection in one direction
static void edge_detect_1d(const float *in, float *out, const int width, const int height,
                           dt_iop_ashift_edge_t dir)
{
  // Sobel kernels for both directions
  const float hkernel[3][3] = { { 1.0f, 0.0f, -1.0f }, { 2.0f, 0.0f, -2.0f }, { 1.0f, 0.0f, -1.0f } };
  const float vkernel[3][3] = { { 1.0f, 2.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { -1.0f, -2.0f, -1.0f } };
  const int kwidth = 3;
  const int khwidth = kwidth / 2;

  // select kernel
  const float *kernel = (dir == ASHIFT_EDGE_HORIZONTAL) ? (const float *)hkernel : (const float *)vkernel;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out, kernel)
#endif
  // loop over image pixels and perform sobel convolution
  for(int j = khwidth; j < height - khwidth; j++)
  {
    const float *inp = in + (size_t)j * width + khwidth;
    float *outp = out + (size_t)j * width + khwidth;
    for(int i = khwidth; i < width - khwidth; i++, inp++, outp++)
    {
      float sum = 0.0f;
      for(int jj = 0; jj < kwidth; jj++)
      {
        const int k = jj * kwidth;
        const int l = (jj - khwidth) * width;
        for(int ii = 0; ii < kwidth; ii++)
        {
          sum += inp[l + ii - khwidth] * kernel[k + ii];
        }
      }
      *outp = sum;
    }
  }

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(out)
#endif
  // border fill in output buffer, so we don't get pseudo lines at image frame
  for(int j = 0; j < height; j++)
    for(int i = 0; i < width; i++)
    {
      float val = out[j * width + i];

      if(j < khwidth)
        val = out[(khwidth - j) * width + i];
      else if(j >= height - khwidth)
        val = out[(j - khwidth) * width + i];
      else if(i < khwidth)
        val = out[j * width + (khwidth - i)];
      else if(i >= width - khwidth)
        val = out[j * width + (i - khwidth)];

      out[j * width + i] = val;

      // jump over center of image
      if(i == khwidth && j >= khwidth && j < height - khwidth) i = width - khwidth;
    }
}

// edge detection in both directions after conversion into greyscale
// outputs absolute values and gradients of edges
static int edge_detect(const float *in, float *value, float *gradient, const int width, const int height)
{
  float *greyscale = NULL;
  float *Gx = NULL;
  float *Gy = NULL;

  // allocate intermediate buffers
  greyscale = malloc((size_t)width * height * sizeof(float));
  if(greyscale == NULL) goto error;

  Gx = malloc((size_t)width * height * sizeof(float));
  if(Gx == NULL) goto error;

  Gy = malloc((size_t)width * height * sizeof(float));
  if(Gy == NULL) goto error;

  // generate greyscale image
  rgb2grey(in, greyscale, width, height);

  // perform edge detection in both directions
  edge_detect_1d(greyscale, Gx, width, height, ASHIFT_EDGE_HORIZONTAL);
  edge_detect_1d(greyscale, Gy, width, height, ASHIFT_EDGE_VERTICAL);

// calculate absolute values and gradients
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(Gx, Gy, value, gradient)
#endif
  for(size_t k = 0; k < (size_t)width * height; k++)
  {
    value[k] = sqrt(Gx[k] * Gx[k] + Gy[k] * Gy[k]);
    gradient[k] = atan2(Gy[k], Gx[k]);
  }

  free(greyscale);
  free(Gx);
  free(Gy);
  return TRUE;

error:
  if(greyscale) free(greyscale);
  if(Gx) free(Gx);
  if(Gy) free(Gy);
  return FALSE;
}


// Do actual line_detection based on LSD algorithm and return results according to this module's
// conventions
static int line_detect(const float *in, const int width, const int height, const int x_off, const int y_off,
                       const float scale, dt_iop_ashift_line_t **alines, int *lcount, int *vcount, int *hcount)
{
  double *greyscale = NULL;
  double *lines = NULL;
  dt_iop_ashift_line_t *ashift_lines = NULL;

  int vertical_count = 0;
  int horizontal_count = 0;

  // allocate intermediate buffers
  greyscale = malloc((size_t)width * height * sizeof(double));
  if(greyscale == NULL) goto error;

  // generate greyscale image
  rgb2grey256(in, greyscale, width, height);

  // call the line segment detector LSD;
  // LSD stores the number of found lines in lines_count.
  // it returns structural details as vector double lines[7 * lines_count]
  int lines_count;
  lines = lsd(&lines_count, greyscale, width, height);

  if(lines_count > 0)
  {
    // aggregate lines data into our own structures
    ashift_lines = (dt_iop_ashift_line_t *)malloc((size_t)lines_count * sizeof(dt_iop_ashift_line_t));
    if(ashift_lines == NULL) goto error;

    for(int n = 0; n < lines_count; n++)
    {
      // line position in absolute coordinates
      float px1 = x_off + lines[n * 7 + 0];
      float py1 = y_off + lines[n * 7 + 1];
      float px2 = x_off + lines[n * 7 + 2];
      float py2 = y_off + lines[n * 7 + 3];

      // scale back to input buffer
      px1 /= scale;
      py1 /= scale;
      px2 /= scale;
      py2 /= scale;

      ashift_lines[n].p1[0] = px1;
      ashift_lines[n].p1[1] = py1;
      ashift_lines[n].p1[2] = 1.0f;
      ashift_lines[n].p2[0] = px2;
      ashift_lines[n].p2[1] = py2;
      ashift_lines[n].p2[2] = 1.0f;;

      // calculate homogeneous coordinates of line (defined by both points)
      vec3prodn(ashift_lines[n].L, ashift_lines[n].p1, ashift_lines[n].p2);

      // length and width of rectangle (see LSD) and weight (= length * width)
      ashift_lines[n].length = sqrt((px2 - px1) * (px2 - px1) + (py2 - py1) * (py2 - py1));
      ashift_lines[n].width = lines[n * 7 + 4] / scale;
      ashift_lines[n].weight = ashift_lines[n].length * ashift_lines[n].width;

      const float angle = atan2(py2 - py1, px2 - px1) / M_PI * 180.0f;
      const int relevant = ashift_lines[n].length > MIN_LINE_LENGTH ? 1 : 0;
      const int vertical = fabs(fabs(angle) - 90.0f) < MAX_TANGENTIAL_DEVIATION ? 1 : 0;
      const int horizontal = fabs(fabs(fabs(angle) - 90.0f) - 90.0f) < MAX_TANGENTIAL_DEVIATION ? 1 : 0;

      // register type of line
      dt_iop_ashift_linetype_t type = 0;
      if(vertical && relevant)
      {
        type |= (ASHIFT_LINE_VERTICAL | ASHIFT_LINE_VSELECTED);
        vertical_count++;
      }
      else if(horizontal && relevant)
      {
        type |= (ASHIFT_LINE_HORIZONTAL | ASHIFT_LINE_HSELECTED);
        horizontal_count++;
      }

      ashift_lines[n].type = type;
    }

#if 0
    printf("%d lines (vertical %d, horizontal %d, not relevant %d)\n", lines_count, vertical_count,
           horizontal_count, lines_count - vertical_count - horizontal_count);
    for(int n = 0; n < lines_count; n++)
    {
      printf("x1 %.0f, y1 %.0f, x2 %.0f, y2 %.0f, length %.0f, width %f, X %f, Y %f, Z %f, type %d, scalars %f %f\n",
             ashift_lines[n].p1[0], ashift_lines[n].p1[1], ashift_lines[n].p2[0], ashift_lines[n].p2[1],
             ashift_lines[n].length, ashift_lines[n].width,
             ashift_lines[n].L[0], ashift_lines[n].L[1], ashift_lines[n].L[2], ashift_lines[n].type,
             vec3scalar(ashift_lines[n].p1, ashift_lines[n].L),
             vec3scalar(ashift_lines[n].p2, ashift_lines[n].L));
    }
    printf("\n");
#endif
  }

  // store results in provides locations
  *lcount = lines_count;
  *vcount = vertical_count;
  *hcount = horizontal_count;
  *alines = ashift_lines;

  // free intermediate buffers
  free(lines);
  free(greyscale);
  return TRUE;

error:
  if(lines) free(lines);
  if(greyscale) free(greyscale);
  return FALSE;
}

static int get_structure(dt_iop_module_t *module)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  float *buffer = NULL;

  dt_pthread_mutex_lock(&g->lock);
  // read buffer data
  const int width = g->buf_width;
  const int height = g->buf_height;
  const int x_off = g->buf_x_off;
  const int y_off = g->buf_y_off;
  const float scale = g->buf_scale;

  // create a temporary buffer to hold image data
  buffer = malloc((size_t)width * height * 4 * sizeof(float));
  if(buffer != NULL)
    memcpy(buffer, g->buf, (size_t)width * height * 4 * sizeof(float));

  // get rid of old structural data
  g->lines_count = 0;
  free(g->lines);
  g->lines = NULL;
  dt_pthread_mutex_unlock(&g->lock);

  if(buffer == NULL) goto error;

  dt_iop_ashift_line_t *lines;
  int lines_count;
  int vertical_count;
  int horizontal_count;

  // get structural data
  if(!line_detect(buffer, width, height, x_off, y_off, scale, &lines, &lines_count,
                  &vertical_count, &horizontal_count))
    goto error;

  dt_pthread_mutex_lock(&g->lock);
  // save new structural data
  g->lines_in_width = width;
  g->lines_in_height = height;
  g->lines_count = lines_count;
  g->vertical_count = vertical_count;
  g->horizontal_count = horizontal_count;
  g->lines = lines;
  dt_pthread_mutex_unlock(&g->lock);

  free(buffer);
  return TRUE;

error:
  free(buffer);
  return FALSE;
}


static int do_fit(dt_iop_module_t *module, dt_iop_ashift_params_t *p)
{
  if(!get_structure(module))
  {
    dt_control_log(_("could not detect structural data in image"));
    return FALSE;
  }

  return TRUE;
}

#if 1
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  const int ch = piece->colors;
  const int ch_width = ch * roi_in->width;

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

  float ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h, piece->buf_in.width,
             piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(ivoid, ovoid, roi_in, roi_out, ihomograph,    \
                                                               interpolation)
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

    const int width = roi_in->width;
    const int height = roi_in->height;
    const int x_off = roi_in->x;
    const int y_off = roi_in->y;
    const float scale = roi_in->scale;

    // origin of image and opposite corner as reference points
    float points[4] = { 0.0f, 0.0f, (float)piece->buf_in.width, (float)piece->buf_in.height };
    float ivec[2] = { points[2] - points[0], points[3] - points[1] };
    float ivecl = sqrt(ivec[0] * ivec[0] + ivec[1] * ivec[1]);

    // where do they go?
    dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->priority + 1, 9999999, points,
                                      2);

    float ovec[2] = { points[2] - points[0], points[3] - points[1] };
    float ovecl = sqrt(ovec[0] * ovec[0] + ovec[1] * ovec[1]);

    // angle between input vector and output vector
    float alpha = acos(CLAMP((ivec[0] * ovec[0] + ivec[1] * ovec[1]) / (ivecl * ovecl), -1.0f, 1.0f));

    // we are interested if |alpha| is in the range of 90° +/- 45° -> we assume the image is flipped
    int isflipped = fabs(fmod(alpha + M_PI, M_PI) - M_PI / 2.0f) < M_PI / 4.0f ? 1 : 0;

    dt_pthread_mutex_lock(&g->lock);
    g->isflipped = isflipped;

    // save a copy of preview input buffer for parameter fitting
    // TODO: this should ideally be dependent on a hash value that represents image changes up to
    // this module. piece->pipe->backbuf_hash is not suited as it does not depend on
    // pixelpipe parameters
    if(g->buf == NULL || (size_t)g->buf_width * g->buf_height < (size_t)width * height)
    {
      free(g->buf); // a no-op if g->buf is NULL
      // only get new buffer if no old buffer or old buffer does not fit in terms of size
      g->buf = malloc((size_t)width * height * 4 * sizeof(float));
    }

    // copy data
    if(g->buf) memcpy(g->buf, ivoid, (size_t)width * height * 4 * sizeof(float));

    g->buf_width = width;
    g->buf_height = height;
    g->buf_x_off = x_off;
    g->buf_y_off = y_off;
    g->buf_scale = scale;
    g->buf_hash = 0;

    dt_pthread_mutex_unlock(&g->lock);
  }
}
#else
// dummy process() for testing purposes during development
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  const int ch = 4;
  const int in_width = roi_in->width;
  const int in_height = roi_in->height;
  const int out_width = roi_out->width;
  const int out_height = roi_out->height;

  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out)
#endif
  for(int j = 0; j < out_height; j++)
  {
    if(j >= in_height) continue;
    const float *inp = in + (size_t)ch * j * in_width;
    float *outp = out + (size_t)ch * j * out_width;
    for(int i = 0; i < out_width; i++, inp += ch, outp += ch)
    {
      if(i >= in_width) continue;
      outp[0] = inp[0];
      outp[1] = inp[1];
      outp[2] = inp[2];
    }
  }
}
#endif

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_ashift_data_t *d = (dt_iop_ashift_data_t *)piece->data;
  dt_iop_ashift_global_data_t *gd = (dt_iop_ashift_global_data_t *)self->data;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  float ihomograph[3][3];
  homography((float *)ihomograph, d->rotation, d->lensshift_v, d->lensshift_h, piece->buf_in.width,
             piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

  cl_int err = -999;
  cl_mem dev_homo = NULL;

  const int devid = piece->pipe->devid;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int width = roi_out->width;
  const int height = roi_out->height;

  dev_homo = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 9, ihomograph);
  if(dev_homo == NULL) goto error;

  const int iroi[2] = { roi_in->x, roi_in->y };
  const int oroi[2] = { roi_out->x, roi_out->y };
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
      goto error;
  }

  dt_opencl_set_kernel_arg(devid, ldkernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, ldkernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, ldkernel, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, ldkernel, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, ldkernel, 4, sizeof(int), (void *)&iwidth);
  dt_opencl_set_kernel_arg(devid, ldkernel, 5, sizeof(int), (void *)&iheight);
  dt_opencl_set_kernel_arg(devid, ldkernel, 6, 2 * sizeof(int), (void *)iroi);
  dt_opencl_set_kernel_arg(devid, ldkernel, 7, 2 * sizeof(int), (void *)oroi);
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

    const int width = roi_in->width;
    const int height = roi_in->height;
    const int x_off = roi_in->x;
    const int y_off = roi_in->y;
    const float scale = roi_in->scale;

    // origin of image and opposite corner as reference points
    float points[4] = { 0.0f, 0.0f, (float)piece->buf_in.width, (float)piece->buf_in.height };
    float ivec[2] = { points[2] - points[0], points[3] - points[1] };
    float ivecl = sqrt(ivec[0] * ivec[0] + ivec[1] * ivec[1]);

    // where do they go?
    dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->priority + 1, 9999999, points,
                                      2);

    float ovec[2] = { points[2] - points[0], points[3] - points[1] };
    float ovecl = sqrt(ovec[0] * ovec[0] + ovec[1] * ovec[1]);

    // angle between input vector and output vector
    float alpha = acos(CLAMP((ivec[0] * ovec[0] + ivec[1] * ovec[1]) / (ivecl * ovecl), -1.0f, 1.0f));

    // we are interested if |alpha| is in the range of 90° +/- 45° -> we assume the image is flipped
    int isflipped = fabs(fmod(alpha + M_PI, M_PI) - M_PI / 2.0f) < M_PI / 4.0f ? 1 : 0;

    dt_pthread_mutex_lock(&g->lock);
    g->isflipped = isflipped;

    // save a copy of preview input buffer for parameter fitting
    // TODO: this should ideally be dependent on a hash value that represents image changes up to
    // this module. piece->pipe->backbuf_hash is not suited as it does not depend on
    // pixelpipe parameters
    if(g->buf == NULL || (size_t)g->buf_width * g->buf_height < (size_t)width * height)
    {
      free(g->buf); // a no-op if g->buf is NULL
      // only get new buffer if no old buffer or old buffer does not fit in terms of size
      g->buf = malloc((size_t)width * height * 4 * sizeof(float));
    }

    // copy data
    if(g->buf)
      err = dt_opencl_copy_device_to_host(devid, g->buf, dev_in, width, height, 4 * sizeof(float));

    g->buf_width = width;
    g->buf_height = height;
    g->buf_x_off = x_off;
    g->buf_y_off = y_off;
    g->buf_scale = scale;
    g->buf_hash = 0;
    dt_pthread_mutex_unlock(&g->lock);

    if(err != CL_SUCCESS) goto error;
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
  tiling->overlap = 3; // accounts for interpolation width
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

static void fit_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  if(!do_fit(self, p)) return;
  dt_iop_request_focus(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)p1;
  dt_iop_ashift_data_t *d = (dt_iop_ashift_data_t *)piece->data;

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
  free(g->lines);
  free(g->buf);
  g->lines = NULL;
  g->buf = NULL;
  g->lines_count = 0;
  g->buf_hash = 0;
  g->buf_width = 0;
  g->buf_height = 0;
  g->buf_scale = 1.0f;
  dt_pthread_mutex_unlock(&g->lock);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_ashift_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_ashift_params_t));
  module->default_enabled = 0;
  module->priority = 252; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_ashift_params_t);
  module->gui_data = NULL;
  dt_iop_ashift_params_t tmp = (dt_iop_ashift_params_t){ 0.0f, 0.0f, 0.0f };
  memcpy(module->params, &tmp, sizeof(dt_iop_ashift_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_ashift_params_t));
}

void reload_defaults(dt_iop_module_t *module)
{
  // our module is disabled by default
  module->default_enabled = 0;
  // init defaults:
  dt_iop_ashift_params_t tmp = (dt_iop_ashift_params_t){ 0.0f, 0.0f, 0.0f };
  memcpy(module->params, &tmp, sizeof(dt_iop_ashift_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_ashift_params_t));

  int isflipped = 0;

  if(module && module->dev)
  {
    const dt_image_t *img = &module->dev->image_storage;
    isflipped = (img->orientation == ORIENTATION_ROTATE_CCW_90_DEG
                 || img->orientation == ORIENTATION_ROTATE_CW_90_DEG)
                    ? 1
                    : 0;
  }

  if(module && module->gui_data)
  {
    dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

    char string_v[256];
    char string_h[256];

    snprintf(string_v, sizeof(string_v), _("lens shift (%s)"), isflipped ? _("horizontal") : _("vertical"));
    snprintf(string_h, sizeof(string_h), _("lens shift (%s)"), isflipped ? _("vertical") : _("horizontal"));

    dt_bauhaus_widget_set_label(g->lensshift_v, NULL, string_v);
    dt_bauhaus_widget_set_label(g->lensshift_h, NULL, string_h);
  }
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
  g->lines_count = 0;
  g->lines = NULL;
  g->buf = NULL;
  g->buf_hash = 0;
  g->buf_width = 0;
  g->buf_height = 0;
  g->buf_scale = 1.0f;
  dt_pthread_mutex_unlock(&g->lock);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->rotation = dt_bauhaus_slider_new_with_range(self, -10.0f, 10.0f, 0.1f, p->rotation, 2);
  g->lensshift_v = dt_bauhaus_slider_new_with_range(self, -1.0f, 1.0f, 0.01f, p->lensshift_v, 2);
  g->lensshift_h = dt_bauhaus_slider_new_with_range(self, -1.0f, 1.0f, 0.01f, p->lensshift_h, 2);
  g->fit = gtk_button_new_with_label(_("fit"));

  dt_bauhaus_widget_set_label(g->rotation, NULL, _("rotation"));
  dt_bauhaus_widget_set_label(g->lensshift_v, NULL, _("lens shift (vertical)"));
  dt_bauhaus_widget_set_label(g->lensshift_h, NULL, _("lens shift (horizontal)"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->rotation, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->lensshift_v, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->lensshift_h, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->fit, TRUE, TRUE, 0);

  g_object_set(g->rotation, "tooltip-text", _("rotate image"), (char *)NULL);
  g_object_set(g->lensshift_v, "tooltip-text", _("apply lens shift correction in one direction"),
               (char *)NULL);
  g_object_set(g->lensshift_h, "tooltip-text", _("apply lens shift correction in one direction"),
               (char *)NULL);
  g_object_set(g->fit, "tooltip-text", _("start fitting routine"), (char *)NULL);

  g_signal_connect(G_OBJECT(g->rotation), "value-changed", G_CALLBACK(rotation_callback), self);
  g_signal_connect(G_OBJECT(g->lensshift_v), "value-changed", G_CALLBACK(lensshift_v_callback), self);
  g_signal_connect(G_OBJECT(g->lensshift_h), "value-changed", G_CALLBACK(lensshift_h_callback), self);
  g_signal_connect(G_OBJECT(g->fit), "clicked", G_CALLBACK(fit_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(draw), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  dt_pthread_mutex_destroy(&g->lock);
  free(g->lines);
  free(g->buf);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
