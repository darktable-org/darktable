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
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

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

typedef enum dt_iop_ashift_linetype_t
{
  ASHIFT_LINE_IRRELEVANT = 0,       // the line is found to be not interesting
                                    // eg. too short, or not horizontal or vertical
  ASHIFT_LINE_RELEVANT   = 1 << 0,  // the line is relevant for us
  ASHIFT_LINE_DIRVERT    = 1 << 1,  // the line is (mostly) vertical, else (mostly) horizontal
  ASHIFT_LINE_SELECTED   = 1 << 2,  // the line is selected for fitting
  ASHIFT_LINE_VERTICAL_NOT_SELECTED   = ASHIFT_LINE_RELEVANT | ASHIFT_LINE_DIRVERT,
  ASHIFT_LINE_HORIZONTAL_NOT_SELECTED = ASHIFT_LINE_RELEVANT,
  ASHIFT_LINE_VERTICAL_SELECTED = ASHIFT_LINE_RELEVANT | ASHIFT_LINE_DIRVERT | ASHIFT_LINE_SELECTED,
  ASHIFT_LINE_HORIZONTAL_SELECTED = ASHIFT_LINE_RELEVANT | ASHIFT_LINE_SELECTED,
  ASHIFT_LINE_MASK = ASHIFT_LINE_RELEVANT | ASHIFT_LINE_DIRVERT | ASHIFT_LINE_SELECTED
} dt_iop_ashift_linetype_t;

typedef enum dt_iop_ashift_linecolor_t
{
  ASHIFT_LINECOLOR_GREY    = 0,
  ASHIFT_LINECOLOR_GREEN   = 1,
  ASHIFT_LINECOLOR_RED     = 2,
  ASHIFT_LINECOLOR_BLUE    = 3,
  ASHIFT_LINECOLOR_YELLOW  = 4
} dt_iop_ashift_linecolor_t;

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

typedef struct dt_iop_ashift_points_idx_t
{
  size_t offset;
  int length;
  int near;
  dt_iop_ashift_linecolor_t color;
  // bounding box:
  float bbx, bby, bbX, bbY;
} dt_iop_ashift_points_idx_t;

typedef struct dt_iop_ashift_gui_data_t
{
  GtkWidget *rotation;
  GtkWidget *lensshift_v;
  GtkWidget *lensshift_h;
  GtkWidget *fit;
  int fitting;
  int isflipped;
  dt_iop_ashift_line_t *lines;
  int lines_in_width;
  int lines_in_height;
  int lines_count;
  int vertical_count;
  int horizontal_count;
  int lines_version;
  float vertical_weight;
  float horizontal_weight;
  float *points;
  dt_iop_ashift_points_idx_t *points_idx;
  int points_lines_count;
  int points_version;
  float *buf;
  int buf_width;
  int buf_height;
  int buf_x_off;
  int buf_y_off;
  float buf_scale;
  uint64_t grid_hash;
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

  roi_out->x = fmaxf(0.0f, xm);
  roi_out->y = fmaxf(0.0f, ym);
  roi_out->width = floorf(xM - roi_out->x + 1);
  roi_out->height = floorf(yM - roi_out->y + 1);

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
// the lsd routines expect input as *double, roughly in the range [0.0; 256.0]
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

// do actual line_detection based on LSD algorithm and return results according to this module's
// conventions
static int line_detect(const float *in, const int width, const int height, const int x_off, const int y_off,
                       const float scale, dt_iop_ashift_line_t **alines, int *lcount, int *vcount, int *hcount,
                       float *vweight, float *hweight)
{
  double *greyscale = NULL;
  double *lsd_lines = NULL;
  dt_iop_ashift_line_t *ashift_lines = NULL;

  int vertical_count = 0;
  int horizontal_count = 0;
  float vertical_weight = 0.0f;
  float horizontal_weight = 0.0f;

  // allocate intermediate buffers
  greyscale = malloc((size_t)width * height * sizeof(double));
  if(greyscale == NULL) goto error;

  // generate greyscale image
  rgb2grey256(in, greyscale, width, height);

  // call the line segment detector LSD;
  // LSD stores the number of found lines in lines_count.
  // it returns structural details as vector 'double lines[7 * lines_count]'
  int lines_count;
  lsd_lines = lsd(&lines_count, greyscale, width, height);

  // we count the lines that we really want to use
  int m = 0;

  if(lines_count > 0)
  {
    // aggregate lines data into our own structures
    ashift_lines = (dt_iop_ashift_line_t *)malloc((size_t)lines_count * sizeof(dt_iop_ashift_line_t));
    if(ashift_lines == NULL) goto error;

    for(int n = 0; n < lines_count; n++)
    {
      float x1 = lsd_lines[n * 7 + 0];
      float y1 = lsd_lines[n * 7 + 1];
      float x2 = lsd_lines[n * 7 + 2];
      float y2 = lsd_lines[n * 7 + 3];

      // check for lines along image borders and skip them.
      // these would likely be false-positives which could result
      // from any kind of processing artifacts
      if((fabs(x1 - x2) < 1 && fmax(x1, x2) < 1) ||
         (fabs(x1 - x2) < 1 && fmin(x1, x2) > width - 2) ||
         (fabs(y1 - y2) < 1 && fmax(y1, y2) < 1) ||
         (fabs(y1 - y2) < 1 && fmin(y1, y2) > height - 2))
        continue;

      // line position in absolute coordinates
      float px1 = x_off + x1;
      float py1 = y_off + y1;
      float px2 = x_off + x2;
      float py2 = y_off + y2;

      // scale back to input buffer
      px1 /= scale;
      py1 /= scale;
      px2 /= scale;
      py2 /= scale;

      // store as homogeneous coordinates
      ashift_lines[m].p1[0] = px1;
      ashift_lines[m].p1[1] = py1;
      ashift_lines[m].p1[2] = 1.0f;
      ashift_lines[m].p2[0] = px2;
      ashift_lines[m].p2[1] = py2;
      ashift_lines[m].p2[2] = 1.0f;;

      // calculate homogeneous coordinates of connecting line (defined by both points)
      vec3prodn(ashift_lines[m].L, ashift_lines[m].p1, ashift_lines[m].p2);

      // length and width of rectangle (see LSD) and weight (= length * width)
      ashift_lines[m].length = sqrt((px2 - px1) * (px2 - px1) + (py2 - py1) * (py2 - py1));
      ashift_lines[m].width = lsd_lines[n * 7 + 4] / scale;
      float weight = ashift_lines[m].length * ashift_lines[m].width;

      const float angle = atan2(py2 - py1, px2 - px1) / M_PI * 180.0f;
      const int vertical = fabs(fabs(angle) - 90.0f) < MAX_TANGENTIAL_DEVIATION ? 1 : 0;
      const int horizontal = fabs(fabs(fabs(angle) - 90.0f) - 90.0f) < MAX_TANGENTIAL_DEVIATION ? 1 : 0;

      int relevant = ashift_lines[m].length > MIN_LINE_LENGTH ? 1 : 0;

      // register type of line
      dt_iop_ashift_linetype_t type = 0;
      if(vertical && relevant)
      {
        type = ASHIFT_LINE_VERTICAL_SELECTED;
        vertical_count++;
        vertical_weight += weight;
      }
      else if(horizontal && relevant)
      {
        type = ASHIFT_LINE_HORIZONTAL_SELECTED;
        horizontal_count++;
        horizontal_weight += weight;
      }
      ashift_lines[m].type = type;

      // the next valid line
      m++;
    }

#if 0
    printf("%d lines (vertical %d, horizontal %d, not relevant %d)\n", lines_count, vertical_count,
           horizontal_count, m - vertical_count - horizontal_count);
    float xmin = FLT_MAX, xmax = FLT_MIN, ymin = FLT_MAX, ymax = FLT_MIN;
    for(int n = 0; n < m; n++)
    {
      xmin = fmin(xmin, fmin(ashift_lines[n].p1[0], ashift_lines[n].p2[0]));
      xmax = fmax(xmax, fmax(ashift_lines[n].p1[0], ashift_lines[n].p2[0]));
      ymin = fmin(ymin, fmin(ashift_lines[n].p1[1], ashift_lines[n].p2[1]));
      ymax = fmax(ymax, fmax(ashift_lines[n].p1[1], ashift_lines[n].p2[1]));
      printf("x1 %.0f, y1 %.0f, x2 %.0f, y2 %.0f, length %.0f, width %f, X %f, Y %f, Z %f, type %d, scalars %f %f\n",
             ashift_lines[n].p1[0], ashift_lines[n].p1[1], ashift_lines[n].p2[0], ashift_lines[n].p2[1],
             ashift_lines[n].length, ashift_lines[n].width,
             ashift_lines[n].L[0], ashift_lines[n].L[1], ashift_lines[n].L[2], ashift_lines[n].type,
             vec3scalar(ashift_lines[n].p1, ashift_lines[n].L),
             vec3scalar(ashift_lines[n].p2, ashift_lines[n].L));
    }
    printf("xmin %.0f, xmax %.0f, ymin %.0f, ymax %.0f\n", xmin, xmax, ymin, ymax);
#endif
  }

  // store results in provided locations
  *lcount = m;
  *vcount = vertical_count;
  *vweight = vertical_weight;
  *hcount = horizontal_count;
  *hweight = horizontal_weight;
  *alines = ashift_lines;

  // free intermediate buffers
  free(lsd_lines);
  free(greyscale);
  return TRUE;

error:
  if(lsd_lines) free(lsd_lines);
  if(greyscale) free(greyscale);
  return FALSE;
}

// get image from buffer, analyze for structure and save results
static int get_structure(dt_iop_module_t *module)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  float *buffer = NULL;
  int width = 0;
  int height = 0;
  int x_off = 0;
  int y_off = 0;
  float scale = 0.0f;

  dt_pthread_mutex_lock(&g->lock);
  // read buffer data if they are available
  if(g->buf != NULL)
  {
    width = g->buf_width;
    height = g->buf_height;
    x_off = g->buf_x_off;
    y_off = g->buf_y_off;
    scale = g->buf_scale;

    // create a temporary buffer to hold image data
    buffer = malloc((size_t)width * height * 4 * sizeof(float));
    if(buffer != NULL)
      memcpy(buffer, g->buf, (size_t)width * height * 4 * sizeof(float));
  }
  dt_pthread_mutex_unlock(&g->lock);

  if(buffer == NULL) goto error;

  // get rid of old structural data
  g->lines_count = 0;
  g->vertical_count = 0;
  g->horizontal_count = 0;
  free(g->lines);
  g->lines = NULL;

  dt_iop_ashift_line_t *lines;
  int lines_count;
  int vertical_count;
  int horizontal_count;
  float vertical_weight;
  float horizontal_weight;

  // get new structural data
  if(!line_detect(buffer, width, height, x_off, y_off, scale, &lines, &lines_count,
                  &vertical_count, &horizontal_count, &vertical_weight, &horizontal_weight))
    goto error;

  // save new structural data
  g->lines_in_width = width;
  g->lines_in_height = height;
  g->lines_count = lines_count;
  g->vertical_count = vertical_count;
  g->horizontal_count = horizontal_count;
  g->vertical_weight = vertical_weight;
  g->horizontal_weight = horizontal_weight;
  g->lines_version++;
  g->lines = lines;

  free(buffer);
  return TRUE;

error:
  free(buffer);
  return FALSE;
}


static int do_fit(dt_iop_module_t *module, dt_iop_ashift_params_t *p)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  if(g->fitting) return FALSE;

  g->fitting = 1;

  dt_pthread_mutex_lock(&g->lock);
  float *b = g->buf;
  dt_pthread_mutex_unlock(&g->lock);

  if(b == NULL)
  {
    dt_control_log(_("please first activate this module"));
    goto error;
  }

  if(!get_structure(module))
  {
    dt_control_log(_("could not detect structural data in image"));
    goto error;
  }

  g->fitting = 0;
  return TRUE;

error:
  g->fitting =0;
  return FALSE;
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

// generate a hash that indicates distorting pixelpipe changes, i.e. changes
// effecting the roi or significant effects of warping modules
static uint64_t grid_hash(dt_develop_t *dev, const int width, const int height, const unsigned char steps)
{
  const int stride = steps + 1;
  const int points_count = stride * stride;

  // generate a grid of equally spaced point coordinates and a second copy
  float points[2 * points_count];
  float tpoints[2 * points_count];

  const float xdelta = (float)(width - 1) / steps;
  const float ydelta = (float)(height - 1) / steps;

  float x = 0.0f;
  float y = 0.0f;

  // generate the grid
  for(int j = 0; j <= steps; j++, y += ydelta)
    for(int i = 0; i <= steps; i++, x += xdelta)
    {
      points[2 * (j * stride + i)] = x;
      points[2 * (j * stride + i) + 1] = y;
    }

  // make the copy
  memcpy(tpoints, points, 2 * points_count);

  // transform the copy
  dt_dev_distort_transform(dev, tpoints, points_count);

  // generate a hash out of the deltas of original and transformed points
  uint64_t hash = 5381;
  for(int k = 0; k <= 2 * points_count; k++)
    hash = ((hash << 5) + hash) ^ (int)(points[k] - tpoints[k]);

  return hash;
}

// get all the points to display lines in the gui
static int get_points(struct dt_iop_module_t *self, const dt_iop_ashift_line_t *lines, const int lines_count,
                      const int lines_version, float **points, dt_iop_ashift_points_idx_t **points_idx,
                      int *points_lines_count)
{
  dt_develop_t *dev = self->dev;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  dt_iop_ashift_points_idx_t *my_points_idx = NULL;
  float *my_points = NULL;

  // is the display flipped relative to the original image?
  const int isflipped = g->isflipped;

  // allocate new index array
  my_points_idx = (dt_iop_ashift_points_idx_t *)malloc(lines_count * sizeof(dt_iop_ashift_points_idx_t));
  if(my_points_idx == NULL) goto error;

  size_t total_points = 0;

  // first step: basic initialization of my_points_idx and counting of total_points
  for(int n = 0; n < lines_count; n++)
  {
    const int length = lines[n].length;
    my_points_idx[n].length = length;
    total_points += length;

    my_points_idx[n].near = 0;

    const dt_iop_ashift_linetype_t type = lines[n].type;

    // set line color according to line type/orientation
    // note: if the screen display is flipped versus the original image we need
    // to respect that fact in the color selection
    if((type & ASHIFT_LINE_MASK) == ASHIFT_LINE_VERTICAL_SELECTED)
      my_points_idx[n].color = isflipped ? ASHIFT_LINECOLOR_BLUE : ASHIFT_LINECOLOR_GREEN;
    else if((type & ASHIFT_LINE_MASK) == ASHIFT_LINE_VERTICAL_NOT_SELECTED)
      my_points_idx[n].color = isflipped ? ASHIFT_LINECOLOR_YELLOW : ASHIFT_LINECOLOR_RED;
    else if((type & ASHIFT_LINE_MASK) == ASHIFT_LINE_HORIZONTAL_SELECTED)
      my_points_idx[n].color = isflipped ? ASHIFT_LINECOLOR_GREEN : ASHIFT_LINECOLOR_BLUE;
    else if((type & ASHIFT_LINE_MASK) == ASHIFT_LINE_HORIZONTAL_NOT_SELECTED)
      my_points_idx[n].color = isflipped ? ASHIFT_LINECOLOR_RED : ASHIFT_LINECOLOR_YELLOW;
    else
      my_points_idx[n].color = ASHIFT_LINECOLOR_GREY;
  }

  // now allocate new points buffer
  my_points = (float *)malloc((size_t)2 * total_points * sizeof(float));
  if(my_points == NULL) goto error;

  // second step: generate points for each line
  size_t offset = 0;
  for(int n = 0; n < lines_count; n++)
  {
    my_points_idx[n].offset = offset;

    float x = lines[n].p1[0];
    float y = lines[n].p1[1];
    const int length = lines[n].length;

    const float dx = (lines[n].p2[0] - x) / (float)(length - 1);
    const float dy = (lines[n].p2[1] - y) / (float)(length - 1);

    for(int l = 0; l < length && offset < total_points; l++, offset++)
    {
      my_points[2 * offset] = x;
      my_points[2 * offset + 1] = y;

      x += dx;
      y += dy;
    }
  }

  // third step: transform all points
  if(!dt_dev_distort_transform_plus(dev, dev->preview_pipe, self->priority, 9999999, my_points, total_points))
    goto error;

  // fourth step: get bounding box in final coordinates (used later for checking "near"-ness to mouse pointer)
  for(int n = 0; n < lines_count; n++)
  {
    float xmin = FLT_MAX, xmax = FLT_MIN, ymin = FLT_MAX, ymax = FLT_MIN;

    size_t offset = my_points_idx[n].offset;
    int length = my_points_idx[n].length;

    for(int l = 0; l < length; l++)
    {
      xmin = fmin(xmin, my_points[2 * offset]);
      xmax = fmax(xmax, my_points[2 * offset]);
      ymin = fmin(ymin, my_points[2 * offset + 1]);
      ymax = fmax(ymax, my_points[2 * offset + 1]);
    }

    my_points_idx[n].bbx = xmin;
    my_points_idx[n].bbX = xmax;
    my_points_idx[n].bby = ymin;
    my_points_idx[n].bbY = ymax;
  }

  // check if lines_version has changed in-between -> too bad: we can forget about all we did :(
  if(g->lines_version > lines_version)
    goto error;

  *points = my_points;
  *points_idx = my_points_idx;
  *points_lines_count = lines_count;

  return TRUE;

error:
  if(my_points_idx != NULL) free(my_points_idx);
  if(my_points != NULL) free(my_points);
  return FALSE;
}

void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;

  // structural data are currently being collected or fit procedure is running? -> skip
  if(g->fitting) return;

  // no structural data? -> nothing to do
  if(g->lines == NULL) return;

  // points data are missing or outdated, or distortion has changed? -> generate points
  uint64_t hash = grid_hash(dev, g->buf_width, g->buf_height, 10);
  if(g->points == NULL || g->points_idx == NULL || hash != g->grid_hash || g->lines_version > g->points_version)
  {
    // we need to reprocess points;
    free(g->points);
    g->points = NULL;
    free(g->points_idx);
    g->points_idx = NULL;
    g->points_lines_count = 0;

    if(!get_points(self, g->lines, g->lines_count, g->lines_version, &g->points, &g->points_idx,
                   &g->points_lines_count))
      return;

    g->points_version = g->lines_version;
    g->grid_hash = hash;
  }

  // a final check
  if(g->points == NULL || g->points_idx == NULL) return;

  // the usual rescaling stuff
  float wd = dev->preview_pipe->backbuf_width;
  float ht = dev->preview_pipe->backbuf_height;
  if(wd < 1.0 || ht < 1.0) return;
  float zoom_y = dt_control_get_dev_zoom_y();
  float zoom_x = dt_control_get_dev_zoom_x();
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  cairo_save(cr);
  cairo_translate(cr, width / 2.0, height / 2.0);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  // this must match the sequence of enum dt_iop_ashift_linecolor_t!
  const float line_colors[5][4] =
  { { 0.3f, 0.3f, 0.3f, 0.8f },                    // grey (misc. lines)
    { 0.0f, 1.0f, 0.0f, 0.8f },                    // green (selected vertical lines)
    { 1.0f, 0.0f, 0.0f, 0.8f },                    // red (de-selected vertical lines)
    { 0.0f, 0.0f, 1.0f, 0.8f },                    // blue (selected horizontal lines)
    { 1.0f, 1.0f, 0.0f, 0.8f } };                  // yellow (de-selected horizontal lines)

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  // now draw all lines
  for(int n = 0; n < g->points_lines_count; n++)
  {
    // is the near flag set? -> draw line a bit thicker
    if(g->points_idx[n].near)
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5) / zoom_scale);
    else
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0) / zoom_scale);

    // the color of this line
    const float *color = line_colors[g->points_idx[n].color];
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);

    size_t offset = g->points_idx[n].offset;
    const int length = g->points_idx[n].length;

    // sanity check (this should not happen)
    if(length < 2) continue;

    // set starting point of multi-segment line
    cairo_move_to(cr, g->points[offset * 2], g->points[offset * 2 + 1]);

    offset++;
    // draw individual line segments
    for(int l = 1; l < length; l++, offset++)
    {
      cairo_line_to(cr, g->points[offset * 2], g->points[offset * 2 + 1]);
    }

    // finally stroke the line
    cairo_stroke(cr);
  }

  cairo_restore(cr);
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
  free(g->buf);
  g->buf = NULL;
  g->lines_count = 0;
  g->buf_width = 0;
  g->buf_height = 0;
  g->buf_scale = 1.0f;
  g->isflipped = -1;
  dt_pthread_mutex_unlock(&g->lock);

  g->fitting = 0;
  free(g->lines);
  g->lines = NULL;
  g->lines_count =0;
  g->horizontal_count = 0;
  g->vertical_count = 0;
  g->grid_hash = 0;
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
  g->buf = NULL;
  g->buf_width = 0;
  g->buf_height = 0;
  g->buf_scale = 1.0f;
  g->isflipped = -1;
  dt_pthread_mutex_unlock(&g->lock);

  g->fitting = 0;
  g->lines = NULL;
  g->lines_count = 0;
  g->vertical_count = 0;
  g->horizontal_count = 0;
  g->lines_version = 0;
  g->points = NULL;
  g->points_idx = NULL;
  g->points_lines_count = 0;
  g->points_version = 0;
  g->grid_hash = 0;

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
  free(g->points);
  free(g->points_idx);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
