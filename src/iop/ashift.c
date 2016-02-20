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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/debug.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/guides.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Inspiration for this module comes from the program ShiftN (http://www.shiftn.de) by
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

// For parameter optimization we are using the Nelder-Mead simplex method
// implemented by Michael F. Hutt.
#include "ashift_nmsimplex.c"

#define ROTATION_RANGE 10                   // allowed min/max default range for rotation parameter
#define ROTATION_RANGE_SOFT 20              // allowed min/max range for rotation parameter with manual adjustment
#define LENSSHIFT_RANGE 0.5                 // allowed min/max default range for lensshift paramters
#define LENSSHIFT_RANGE_SOFT 1              // allowed min/max range for lensshift paramters with manual adjustment
#define MIN_LINE_LENGTH 10                  // the minimum length of a line in pixels to be regarded as relevant
#define MAX_TANGENTIAL_DEVIATION 15         // by how many degrees a line may deviate from the +/-180 and +/-90 to be regarded as relevant
#define POINTS_NEAR_DELTA 4                 // distance of mouse pointer to line for "near" detection
#define LSD_SCALE 1.0                       // scaling factor for LSD line detection
#define RANSAC_RUNS 200                     // how many interations to run in ransac
#define RANSAC_EPSILON 4                    // starting value for ransac epsilon (in -log10 units)
#define RANSAC_EPSILON_STEP 1               // step size of epsilon optimization (log10 units)
#define RANSAC_ELIMINATION_RATIO 60         // percentage of lines we try to eliminate as outliers
#define RANSAC_OPTIMIZATION_STEPS 4         // home many steps to optimize epsilon
#define RANSAC_OPTIMIZATION_DRY_RUNS 50     // how man runs per optimization steps
#define RANSAC_HURDLE 5                     // hurdle rate: the number of lines below which we do a complete permutation instead of random sampling
#define MINIMUM_FITLINES 4                  // minimum number of lines needed for automatic parameter fit
#define NMS_EPSILON 1e-10                   // break criterion for Nelder-Mead simplex
#define NMS_SCALE 1.0                       // scaling factor for Nelder-Mead simplex
#define NMS_ITERATIONS 200                  // maximum number of iterations for Nelder-Mead simplex
#define ASHIFT_DEFAULT_F_LENGTH 28.0        // focal length we assume if no exif data are available

#undef ASHIFT_DEBUG

DT_MODULE_INTROSPECTION(2, dt_iop_ashift_params_t)

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

int operation_tags_filter()
{
  // switch off clipping and decoration, we want to see the full image.
  return IOP_TAG_DECORATION | IOP_TAG_CLIPPING;
}

typedef enum dt_iop_ashift_homodir_t
{
  ASHIFT_HOMOGRAPH_FORWARD,
  ASHIFT_HOMOGRAPH_INVERTED
} dt_iop_ashift_homodir_t;

typedef enum dt_iop_ashift_linetype_t
{
  ASHIFT_LINE_IRRELEVANT   = 0,       // the line is found to be not interesting
                                      // eg. too short, or not horizontal or vertical
  ASHIFT_LINE_RELEVANT     = 1 << 0,  // the line is relevant for us
  ASHIFT_LINE_DIRVERT      = 1 << 1,  // the line is (mostly) vertical, else (mostly) horizontal
  ASHIFT_LINE_SELECTED     = 1 << 2,  // the line is selected for fitting
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

typedef enum dt_iop_ashift_fitaxis_t
{
  ASHIFT_FIT_NONE          = 0,       // none
  ASHIFT_FIT_ROTATION      = 1 << 0,  // flag indicates to fit rotation angle
  ASHIFT_FIT_LENS_VERT     = 1 << 1,  // flag indicates to fit vertical lens shift
  ASHIFT_FIT_LENS_HOR      = 1 << 2,  // flag indicates to fit horizontal lens shift
  ASHIFT_FIT_LINES_VERT    = 1 << 3,  // use vertical lines for fitting
  ASHIFT_FIT_LINES_HOR     = 1 << 4,  // use horizontal lines for fitting
  ASHIFT_FIT_LENS_BOTH = ASHIFT_FIT_LENS_VERT | ASHIFT_FIT_LENS_HOR,
  ASHIFT_FIT_LINES_BOTH = ASHIFT_FIT_LINES_VERT | ASHIFT_FIT_LINES_HOR,
  ASHIFT_FIT_VERTICALLY = ASHIFT_FIT_ROTATION | ASHIFT_FIT_LENS_VERT | ASHIFT_FIT_LINES_VERT,
  ASHIFT_FIT_HORIZONTALLY = ASHIFT_FIT_ROTATION | ASHIFT_FIT_LENS_HOR | ASHIFT_FIT_LINES_HOR,
  ASHIFT_FIT_BOTH = ASHIFT_FIT_ROTATION | ASHIFT_FIT_LENS_VERT | ASHIFT_FIT_LENS_HOR |
                    ASHIFT_FIT_LINES_VERT | ASHIFT_FIT_LINES_HOR,
  ASHIFT_FIT_VERTICALLY_NO_ROTATION = ASHIFT_FIT_LENS_VERT | ASHIFT_FIT_LINES_VERT,
  ASHIFT_FIT_HORIZONTALLY_NO_ROTATION = ASHIFT_FIT_LENS_HOR | ASHIFT_FIT_LINES_HOR,
  ASHIFT_FIT_BOTH_NO_ROTATION = ASHIFT_FIT_LENS_VERT | ASHIFT_FIT_LENS_HOR |
                                ASHIFT_FIT_LINES_VERT | ASHIFT_FIT_LINES_HOR,
  ASHIFT_FIT_ROTATION_VERTICAL_LINES = ASHIFT_FIT_ROTATION | ASHIFT_FIT_LINES_VERT,
  ASHIFT_FIT_ROTATION_HORIZONTAL_LINES = ASHIFT_FIT_ROTATION | ASHIFT_FIT_LINES_HOR,
  ASHIFT_FIT_ROTATION_BOTH_LINES = ASHIFT_FIT_ROTATION | ASHIFT_FIT_LINES_VERT | ASHIFT_FIT_LINES_HOR,
  ASHIFT_FIT_FLIP = ASHIFT_FIT_LENS_VERT | ASHIFT_FIT_LENS_HOR | ASHIFT_FIT_LINES_VERT | ASHIFT_FIT_LINES_HOR
} dt_iop_ashift_fitaxis_t;

typedef enum dt_iop_ashift_nmsresult_t
{
  NMS_SUCCESS = 0,
  NMS_NOT_ENOUGH_LINES = 1,
  NMS_DID_NOT_CONVERGE = 2
} dt_iop_ashift_nmsresult_t;

typedef enum dt_iop_ashift_enhance_t
{
  ASHIFT_ENHANCE_NONE = 0,
  ASHIFT_ENHANCE_HORIZONTAL = 1,
  ASHIFT_ENHANCE_VERTICAL = 2,
  ASHIFT_ENHANCE_EDGES = 3
} dt_iop_ashift_enhance_t;

typedef enum dt_iop_ashift_mode_t
{
  ASHIFT_MODE_GENERIC = 0,
  ASHIFT_MODE_SPECIFIC = 1
} dt_iop_ashift_mode_t;

typedef struct dt_iop_ashift_params1_t
{
  float rotation;
  float lensshift_v;
  float lensshift_h;
  int toggle;
} dt_iop_ashift_params1_t;

typedef struct dt_iop_ashift_params_t
{
  float rotation;
  float lensshift_v;
  float lensshift_h;
  float f_length;
  float crop_factor;
  float orthocorr;
  float aspect;
  dt_iop_ashift_mode_t mode;
  int toggle;
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

typedef struct dt_iop_ashift_fit_params_t
{
  int params_count;
  dt_iop_ashift_linetype_t linetype;
  dt_iop_ashift_linetype_t linemask;
  dt_iop_ashift_line_t *lines;
  int lines_count;
  int width;
  int height;
  float weight;
  float f_length_kb;
  float orthocorr;
  float aspect;
  float rotation;
  float lensshift_v;
  float lensshift_h;
  float rotation_range;
  float lensshift_v_range;
  float lensshift_h_range;
} dt_iop_ashift_fit_params_t;

typedef struct dt_iop_ashift_gui_data_t
{
  GtkWidget *rotation;
  GtkWidget *lensshift_v;
  GtkWidget *lensshift_h;
  GtkWidget *guide_lines;
  GtkWidget *mode;
  GtkWidget *f_length;
  GtkWidget *crop_factor;
  GtkWidget *orthocorr;
  GtkWidget *aspect;
  GtkWidget *fit_v;
  GtkWidget *fit_h;
  GtkWidget *fit_both;
  GtkWidget *structure;
  GtkWidget *clean;
  GtkWidget *eye;
  int lines_suppressed;
  int fitting;
  int isflipped;
  int show_guides;
  int isselecting;
  int isdeselecting;
  float rotation_range;
  float lensshift_v_range;
  float lensshift_h_range;
  dt_iop_ashift_line_t *lines;
  int lines_in_width;
  int lines_in_height;
  int lines_x_off;
  int lines_y_off;
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
  uint64_t buf_hash;
  dt_iop_ashift_fitaxis_t lastfit;
  dt_pthread_mutex_t lock;
} dt_iop_ashift_gui_data_t;

typedef struct dt_iop_ashift_data_t
{
  float rotation;
  float lensshift_v;
  float lensshift_h;
  float f_length_kb;
  float orthocorr;
  float aspect;
} dt_iop_ashift_data_t;

typedef struct dt_iop_ashift_global_data_t
{
  int kernel_ashift_bilinear;
  int kernel_ashift_bicubic;
  int kernel_ashift_lanczos2;
  int kernel_ashift_lanczos3;
} dt_iop_ashift_global_data_t;

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
    new->toggle = old->toggle;
    new->f_length = ASHIFT_DEFAULT_F_LENGTH;
    new->crop_factor = 1.0f;
    new->orthocorr = 100.0f;
    new->aspect = 1.0f;
    new->mode = ASHIFT_MODE_GENERIC;
    return 0;
  }
  return 1;
}

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
// dst needs to be different from v
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
// dst needs to be different from m1 and m2
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
// dst needs to be different from v1 and v2
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

// normalize a 3x1 vector so that x^2 + y^2 + z^2 = 1
// dst and v may be the same
static inline void vec3norm(float *dst, const float *const v)
{
  const float sq = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

  // special handling for an all-zero vector
  const float f = sq > 0.0f ? 1.0f / sq : 1.0f;

  dst[0] = v[0] * f;
  dst[1] = v[1] * f;
  dst[2] = v[2] * f;
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

#if 0
static void _print_roi(const dt_iop_roi_t *roi, const char *label)
{
  printf("{ %5d  %5d  %5d  %5d  %.6f } %s\n", roi->x, roi->y, roi->width, roi->height, roi->scale, label);
}
#endif

#define MAT3SWAP(a, b) { float (*tmp)[3] = (a); (a) = (b); (b) = tmp; }

static void homography(float *homograph, const float angle, const float shift_v, const float shift_h,
                       const float f_length_kb, const float orthocorr, const float aspect,
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

  const float phi = M_PI * angle / 180.0f;
  const float cosi = cos(phi);
  const float sini = sin(phi);
  const float ascale = sqrt(aspect);

  // most of this comes from ShiftN
  const float f_global = f_length_kb;
  const float horifac = 1.0f - orthocorr / 100.0f;
  const float exppa_v = exp(shift_v);
  const float fdb_v = f_global / (14.4f + (v / u - 1) * 7.2f);
  const float rad_v = fdb_v * (exppa_v - 1.0f) / (exppa_v + 1.0f);
  const float alpha_v = CLAMP(atan(rad_v), -1.5f, 1.5f);
  const float rt_v = sin(0.5f * alpha_v);
  const float r_v = fmax(0.1f, 2.0f * (horifac - 1.0f) * rt_v * rt_v + 1.0f);

  const float vertifac = 1.0f - orthocorr / 100.0f;
  const float exppa_h = exp(shift_h);
  const float fdb_h = f_global / (14.4f + (u / v - 1) * 7.2f);
  const float rad_h = fdb_h * (exppa_h - 1.0f) / (exppa_h + 1.0f);
  const float alpha_h = CLAMP(atan(rad_h), -1.5f, 1.5f);
  const float rt_h = sin(0.5f * alpha_h);
  const float r_h = fmax(0.1f, 2.0f * (vertifac - 1.0f) * rt_h * rt_h + 1.0f);


  // three intermediate buffers for matrix calculation ...
  float m1[3][3], m2[3][3], m3[3][3];

  // ... and some pointers to handle them more intuitively
  float (*mwork)[3] = m1;
  float (*minput)[3] = m2;
  float (*moutput)[3] = m3;

  // Step 1: flip x and y coordinates (see above)
  memset(minput, 0, 9 * sizeof(float));
  minput[0][1] = 1.0f;
  minput[1][0] = 1.0f;
  minput[2][2] = 1.0f;


  // Step 2: rotation of image around its center
  memset(mwork, 0, 9 * sizeof(float));
  mwork[0][0] = cosi;
  mwork[0][1] = -sini;
  mwork[1][0] = sini;
  mwork[1][1] = cosi;
  mwork[0][2] = -0.5f * v * cosi + 0.5f * u * sini + 0.5f * v;
  mwork[1][2] = -0.5f * v * sini - 0.5f * u * cosi + 0.5f * u;
  mwork[2][2] = 1.0f;

  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // Step 3: apply vertical lens shift effect
  memset(mwork, 0, 9 * sizeof(float));
  mwork[0][0] = exppa_v;
  mwork[1][0] = 0.5f * ((exppa_v - 1.0f) * u) / v;
  mwork[1][1] = 2.0f * exppa_v / (exppa_v + 1.0f);
  mwork[1][2] = -0.5f * ((exppa_v - 1.0f) * u) / (exppa_v + 1.0f);
  mwork[2][0] = (exppa_v - 1.0f) / v;
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // Step 4: horizontal compression
  memset(mwork, 0, 9 * sizeof(float));
  mwork[0][0] = 1.0f;
  mwork[1][1] = r_v;
  mwork[1][2] = 0.5f * u * (1.0f - r_v);
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // Step 5: flip x and y back again
  memset(mwork, 0, 9 * sizeof(float));
  mwork[0][1] = 1.0f;
  mwork[1][0] = 1.0f;
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // from here output vectors would be in (x : y : 1) format

  // Step 6: now we can apply horizontal lens shift with the same matrix format as above
  memset(mwork, 0, 9 * sizeof(float));
  mwork[0][0] = exppa_h;
  mwork[1][0] = 0.5f * ((exppa_h - 1.0f) * v) / u;
  mwork[1][1] = 2.0f * exppa_h / (exppa_h + 1.0f);
  mwork[1][2] = -0.5f * ((exppa_h - 1.0f) * v) / (exppa_h + 1.0f);
  mwork[2][0] = (exppa_h - 1.0f) / u;
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // Step 7: vertical compression
  memset(mwork, 0, 9 * sizeof(float));
  mwork[0][0] = 1.0f;
  mwork[1][1] = r_h;
  mwork[1][2] = 0.5f * v * (1.0f - r_h);
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // Step 8: apply aspect ratio scaling
  memset(mwork, 0, 9 * sizeof(float));
  mwork[0][0] = 1.0f * ascale;
  mwork[1][1] = 1.0f / ascale;
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // Step 9: find x/y offsets and apply according correction so that
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
      mat3mulv(po, (float *)minput, pi);
      umin = fmin(umin, po[0] / po[2]);
      vmin = fmin(vmin, po[1] / po[2]);
    }
  memset(mwork, 0, 9 * sizeof(float));
  mwork[0][0] = 1.0f;
  mwork[1][1] = 1.0f;
  mwork[2][2] = 1.0f;
  mwork[0][2] = -umin;
  mwork[1][2] = -vmin;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // on request we either keep the final matrix for forward conversions
  // or produce an inverted matrix for backward conversions
  if(dir == ASHIFT_HOMOGRAPH_FORWARD)
  {
    // we have what we need -> copy it to the right place
    memcpy(homograph, moutput, 9 * sizeof(float));
  }
  else
  {
    // generate inverted homograph (mat3inv function defined in colorspaces.c)
    if(mat3inv((float *)homograph, (float *)moutput))
    {
      // in case of error we set to unity matrix
      memset(mwork, 0, 9 * sizeof(float));
      mwork[0][0] = 1.0f;
      mwork[1][1] = 1.0f;
      mwork[2][2] = 1.0f;
      memcpy(homograph, mwork, 9 * sizeof(float));
    }
  }
}
#undef MAT3SWAP

int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;

  float homograph[3][3];
  homography((float *)homograph, data->rotation, data->lensshift_v, data->lensshift_h, data->f_length_kb,
             data->orthocorr, data->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_FORWARD);

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
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h, data->f_length_kb,
             data->orthocorr, data->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

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
  homography((float *)homograph, data->rotation, data->lensshift_v, data->lensshift_h, data->f_length_kb,
             data->orthocorr, data->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_FORWARD);

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
  //roi_out->x = CLAMP(roi_out->x, 0, INT_MAX);
  //roi_out->y = CLAMP(roi_out->y, 0, INT_MAX);
  //roi_out->width = CLAMP(roi_out->width, 1, INT_MAX);
  //roi_out->height = CLAMP(roi_out->height, 1, INT_MAX);

  //_print_roi(roi_out, "roi_out");
  //_print_roi(roi_in, "roi_in");
}

void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;
  *roi_in = *roi_out;

  float ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h, data->f_length_kb,
             data->orthocorr, data->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

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
  roi_in->width = CLAMP(roi_in->width, 1, (int)floorf(orig_w) - roi_in->x);
  roi_in->height = CLAMP(roi_in->height, 1, (int)floorf(orig_h) - roi_in->y);

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

// sobel edge enhancement in one direction
static void edge_enhance_1d(const double *in, double *out, const int width, const int height,
                            dt_iop_ashift_enhance_t dir)
{
  // Sobel kernels for both directions
  const double hkernel[3][3] = { { 1.0, 0.0, -1.0 }, { 2.0, 0.0, -2.0 }, { 1.0, 0.0, -1.0 } };
  const double vkernel[3][3] = { { 1.0, 2.0, 1.0 }, { 0.0, 0.0, 0.0 }, { -1.0, -2.0, -1.0 } };
  const int kwidth = 3;
  const int khwidth = kwidth / 2;

  // select kernel
  const double *kernel = (dir == ASHIFT_ENHANCE_HORIZONTAL) ? (const double *)hkernel : (const double *)vkernel;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out, kernel)
#endif
  // loop over image pixels and perform sobel convolution
  for(int j = khwidth; j < height - khwidth; j++)
  {
    const double *inp = in + (size_t)j * width + khwidth;
    double *outp = out + (size_t)j * width + khwidth;
    for(int i = khwidth; i < width - khwidth; i++, inp++, outp++)
    {
      double sum = 0.0f;
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
      double val = out[j * width + i];

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

// edge enhancement in both directions
static int edge_enhance(const double *in, double *out, const int width, const int height)
{
  double *Gx = NULL;
  double *Gy = NULL;

  Gx = malloc((size_t)width * height * sizeof(double));
  if(Gx == NULL) goto error;

  Gy = malloc((size_t)width * height * sizeof(double));
  if(Gy == NULL) goto error;

  // perform edge enhancement in both directions
  edge_enhance_1d(in, Gx, width, height, ASHIFT_ENHANCE_HORIZONTAL);
  edge_enhance_1d(in, Gy, width, height, ASHIFT_ENHANCE_VERTICAL);

// calculate absolute values
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(Gx, Gy, out)
#endif
  for(size_t k = 0; k < (size_t)width * height; k++)
  {
    out[k] = sqrt(Gx[k] * Gx[k] + Gy[k] * Gy[k]);
  }

  free(Gx);
  free(Gy);
  return TRUE;

error:
  if(Gx) free(Gx);
  if(Gy) free(Gy);
  return FALSE;
}

// do actual line_detection based on LSD algorithm and return results according to this module's
// conventions
static int line_detect(const float *in, const int width, const int height, const int x_off, const int y_off,
                       const float scale, dt_iop_ashift_line_t **alines, int *lcount, int *vcount, int *hcount,
                       float *vweight, float *hweight, dt_iop_ashift_enhance_t enhance)
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

  if(enhance == ASHIFT_ENHANCE_EDGES)
  {
    // if requested perform an additional edge enhancement step
    if(!edge_enhance(greyscale, greyscale, width, height))
      goto error;
  }

  // call the line segment detector LSD;
  // LSD stores the number of found lines in lines_count.
  // it returns structural details as vector 'double lines[7 * lines_count]'
  int lines_count;
  lsd_lines = lsd_scale(&lines_count, greyscale, width, height, LSD_SCALE);

  // we count the lines that we really want to use
  int lct = 0;

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

      // check for lines running along image borders and skip them.
      // these would likely be false-positives which could result
      // from any kind of processing artifacts
      if((fabs(x1 - x2) < 1 && fmax(x1, x2) < 2) ||
         (fabs(x1 - x2) < 1 && fmin(x1, x2) > width - 3) ||
         (fabs(y1 - y2) < 1 && fmax(y1, y2) < 2) ||
         (fabs(y1 - y2) < 1 && fmin(y1, y2) > height - 3))
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
      ashift_lines[lct].p1[0] = px1;
      ashift_lines[lct].p1[1] = py1;
      ashift_lines[lct].p1[2] = 1.0f;
      ashift_lines[lct].p2[0] = px2;
      ashift_lines[lct].p2[1] = py2;
      ashift_lines[lct].p2[2] = 1.0f;;

      // calculate homogeneous coordinates of connecting line (defined by the two points)
      vec3prodn(ashift_lines[lct].L, ashift_lines[lct].p1, ashift_lines[lct].p2);

      // length and width of rectangle (see LSD) and weight (= length * width)
      ashift_lines[lct].length = sqrt((px2 - px1) * (px2 - px1) + (py2 - py1) * (py2 - py1));
      ashift_lines[lct].width = lsd_lines[n * 7 + 4] / scale;

      const float weight = ashift_lines[lct].length * ashift_lines[lct].width;
      ashift_lines[lct].weight = weight;


      const float angle = atan2(py2 - py1, px2 - px1) / M_PI * 180.0f;
      const int vertical = fabs(fabs(angle) - 90.0f) < MAX_TANGENTIAL_DEVIATION ? 1 : 0;
      const int horizontal = fabs(fabs(fabs(angle) - 90.0f) - 90.0f) < MAX_TANGENTIAL_DEVIATION ? 1 : 0;

      const int relevant = ashift_lines[lct].length > MIN_LINE_LENGTH ? 1 : 0;

      // register type of line
      dt_iop_ashift_linetype_t type = ASHIFT_LINE_IRRELEVANT;
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
      ashift_lines[lct].type = type;

      // the next valid line
      lct++;
    }
  }
#ifdef ASHIFT_DEBUG
    printf("%d lines (vertical %d, horizontal %d, not relevant %d)\n", lines_count, vertical_count,
           horizontal_count, lct - vertical_count - horizontal_count);
    float xmin = FLT_MAX, xmax = FLT_MIN, ymin = FLT_MAX, ymax = FLT_MIN;
    for(int n = 0; n < lct; n++)
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

  // store results in provided locations
  *lcount = lct;
  *vcount = vertical_count;
  *vweight = vertical_weight;
  *hcount = horizontal_count;
  *hweight = horizontal_weight;
  *alines = ashift_lines;

  // free intermediate buffers
  free(lsd_lines);
  free(greyscale);
  return lct > 0 ? TRUE : FALSE;

error:
  if(lsd_lines) free(lsd_lines);
  if(greyscale) free(greyscale);
  return FALSE;
}

// get image from buffer, analyze for structure and save results
static int get_structure(dt_iop_module_t *module, dt_iop_ashift_enhance_t enhance)
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
                  &vertical_count, &horizontal_count, &vertical_weight, &horizontal_weight,
                  enhance))
    goto error;

  // save new structural data
  g->lines_in_width = width;
  g->lines_in_height = height;
  g->lines_x_off = x_off;
  g->lines_y_off = y_off;
  g->lines_count = lines_count;
  g->vertical_count = vertical_count;
  g->horizontal_count = horizontal_count;
  g->vertical_weight = vertical_weight;
  g->horizontal_weight = horizontal_weight;
  g->lines_version++;
  g->lines_suppressed = 0;
  g->lines = lines;

  free(buffer);
  return TRUE;

error:
  free(buffer);
  return FALSE;
}


// swap two integer values
static inline void swap(int *a, int *b)
{
  int tmp = *a;
  *a = *b;
  *b = tmp;
}

// do complete permutations
static int quickperm(int *a, int *p, const int N, int *i)
{
  if(*i >= N) return FALSE;

  p[*i]--;
  int j = (*i % 2 == 1) ? p[*i] : 0;
  swap(&a[j], &a[*i]);
  *i = 1;
  while(p[*i] == 0)
  {
    p[*i] = *i;
    (*i)++;
  }
  return TRUE;
}

// Fisher-Yates shuffle
static void shuffle(int *a, const int N)
{
  for(int i = 0; i < N; i++)
  {
    int j = i + rand() % (N - i);
    swap(&a[j], &a[i]);
  }
}

// factorial function
static int fact(const int n)
{
  return (n == 1 ? 1 : n * fact(n - 1));
}

// We use a pseudo-RANSAC algorithm to elminiate ouliers from our set of lines. The
// original RANSAC works on linear optimization problems. Our model is nonlinear. We
// take advantage of the fact that lines interesting for our model are vantage lines
// that meet in one vantage point for each subset of lines (vertical/horizontal).
// Stragegy: we construct a model by (random) sampling within the subset of lines and
// calculate the vantage point. Then we check the distance in homogeneous coordinates
// of all other lines to the vantage point. The model that gives highest number of lines
// combined with the highest total weight wins.
// Disadvantage: compared to the original RANSAC we don't get any model parameters that
// we could use for the following NMS fit.
// Self optimization: we optimize "epsilon", the hurdle line to reject a line as an outlier,
// by a number of dry runs first. The target average percentage value of lines to eliminate as
// outliers (without judging on the quality of the model) is given by RANSAC_ELIMINATION_RATIO,
// note: the actual percentage of outliers removed in the final run will be lower because we
// will look for the best quality model with the optimized epsilon and quality also
// encloses the number of good lines
static void ransac(const dt_iop_ashift_line_t *lines, int *index_set, int *inout_set,
                  const int set_count, const float total_weight, const int xmin, const int xmax,
                  const int ymin, const int ymax)
{
  if(set_count < 3) return;

  int best_set[set_count];
  memcpy(best_set, index_set, sizeof(best_set));
  int best_inout[set_count];
  memset(best_inout, 0, sizeof(best_inout));
  float best_quality = 0.0f;

  // hurdle value epsilon for rejecting a line as an outlier will be self-optimized
  // in a number of dry runs
  float epsilon = pow(10.0f, -RANSAC_EPSILON);
  float epsilon_step = RANSAC_EPSILON_STEP;
  int lines_eliminated = 0;

  // number of runs to optimize epsilon
  const int optiruns = RANSAC_OPTIMIZATION_STEPS * RANSAC_OPTIMIZATION_DRY_RUNS;
  // go for complete permutations on small set sizes, else for random sample consensus
  const int riter = (set_count > RANSAC_HURDLE) ? RANSAC_RUNS : fact(set_count);

  // some data needed for quickperm
  int perm[set_count + 1];
  for(int n = 0; n < set_count + 1; n++) perm[n] = n;
  int piter = 1;

  for(int r = 0; r < optiruns + riter; r++)
  {
    // get random or systematic variation of index set
    if(set_count > RANSAC_HURDLE || r < optiruns)
      shuffle(index_set, set_count);
    else
      (void)quickperm(index_set, perm, set_count, &piter);

    // inout holds good/bad qualification for each line
    int inout[set_count];

    // summed quality evaluation of this run
    float quality = 0.0f;

    // we build a model ouf of the first two lines
    const float *L1 = lines[index_set[0]].L;
    const float *L2 = lines[index_set[1]].L;

    // get intersection point (ideally a vantage point)
    float V[3];
    vec3prodn(V, L1, L2);

    // seldom case: L1 and L2 are identical -> no valid vantage point
    if(vec3isnull(V))
      continue;

    // no chance for this module to correct for a vantage point which lies inside the image frame.
    // check that and skip if needed
    if(fabs(V[2]) > 0.0f &&
         V[0]/V[2] >= xmin &&
         V[1]/V[2] >= ymin &&
         V[0]/V[2] <= xmax &&
         V[1]/V[2] <= ymax)
      continue;

    // normalize V
    vec3norm(V, V);

    // the two lines constituting the model are part of the set
    inout[0] = 1;
    inout[1] = 1;

    // go through all remaining lines, check if they are within the model, and
    // mark that fact in inout[].
    // summarize a quality parameter for all lines within the model
    for(int n = 2; n < set_count; n++)
    {
      const float *L3 = lines[index_set[n]].L;
      const float d = fabs(vec3scalar(V, L3));

      // depending on d we either include or exclude the point from the set
      inout[n] = (d < epsilon) ? 1 : 0;

      float q;

      if(inout[n] == 1)
      {
        // a quality parameter that depends 1/3 on the number of lines within the model,
        // 1/3 on their weight, and 1/3 on their weighted distance d to the vantage point
        q = 0.33f / (float)set_count
            + 0.33f * lines[index_set[n]].weight / total_weight
            + 0.33f * (1.0f - d / epsilon) * (float)set_count * lines[index_set[n]].weight / total_weight;
      }
      else
      {
        q = 0.0f;
        lines_eliminated++;
      }

      quality += q;
    }

    if(r < optiruns)
    {
      // on last run of each optimization steps
      if((r % RANSAC_OPTIMIZATION_DRY_RUNS) == (RANSAC_OPTIMIZATION_DRY_RUNS - 1))
      {
        // average ratio of lines that we eliminated with the given epsilon
        float ratio = 100.0f * (float)lines_eliminated / ((float)set_count * RANSAC_OPTIMIZATION_DRY_RUNS);
        // adjust epsilon accordingly
        if(ratio < RANSAC_ELIMINATION_RATIO)
          epsilon = pow(10.0f, log10(epsilon) - epsilon_step);
        else if(ratio > RANSAC_ELIMINATION_RATIO)
          epsilon = pow(10.0f, log10(epsilon) + epsilon_step);

        // reduce step-size for next optimization round
        epsilon_step /= 2.0f;
        lines_eliminated = 0;
      }
    }
    else
    {
      // in the "real" runs check against the best model found so far
      if(quality > best_quality)
      {
        memcpy(best_set, index_set, sizeof(best_set));
        memcpy(best_inout, inout, sizeof(best_inout));
        best_quality = quality;
      }
    }

#ifdef ASHIFT_DEBUG
    // report some statistics
    int count = 0, lastcount = 0;
    for(int n = 0; n < set_count; n++) count += best_inout[n];
    for(int n = 0; n < set_count; n++) lastcount += inout[n];
    printf("run %d: best qual %.6f, eps %.6f, line count %d of %d (this run: qual %.5f, count %d (%2f%%))\n", r,
           best_quality, epsilon, count, set_count, quality, lastcount, 100.0f * lastcount / (float)set_count);
#endif
  }

  // store back best set
  memcpy(index_set, best_set, set_count * sizeof(int));
  memcpy(inout_set, best_inout, set_count * sizeof(int));
}


// try to clean up structural lines to increase chance of a convergent fitting
static int remove_outliers(dt_iop_module_t *module)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  const int width = g->lines_in_width;
  const int height = g->lines_in_height;
  const int xmin = g->lines_x_off;
  const int ymin = g->lines_y_off;
  const int xmax = xmin + width;
  const int ymax = ymin + height;

  // holds the index set of lines we want to work on
  int lines_set[g->lines_count];
  // holds the result of ransac
  int inout_set[g->lines_count];

  // some counter variables
  int vnb = 0, vcount = 0;
  int hnb = 0, hcount = 0;

  // just to be on the safe side
  if(g->lines == NULL) goto error;

  // generate index list for the vertical lines
  for(int n = 0; n < g->lines_count; n++)
  {
    // is this a selected vertical line?
    if((g->lines[n].type & ASHIFT_LINE_MASK) != ASHIFT_LINE_VERTICAL_SELECTED)
      continue;

    lines_set[vnb] = n;
    inout_set[vnb] = 0;
    vnb++;
  }

  // it only makes sense to call ransac if we have more than two lines
  if(vnb > 2)
    ransac(g->lines, lines_set, inout_set, vnb, g->vertical_weight,
           xmin, xmax, ymin, ymax);

  // adjust line selected flag according to the ransac results
  for(int n = 0; n < vnb; n++)
  {
    const int m = lines_set[n];
    if(inout_set[n] == 1)
    {
      g->lines[m].type |= ASHIFT_LINE_SELECTED;
      vcount++;
    }
    else
      g->lines[m].type &= ~ASHIFT_LINE_SELECTED;
  }
  // update number of vertical lines
  g->vertical_count = vcount;
  g->lines_version++;

  // now generate index list for the horizontal lines
  for(int n = 0; n < g->lines_count; n++)
  {
    // is this a selected horizontal line?
    if((g->lines[n].type & ASHIFT_LINE_MASK) != ASHIFT_LINE_HORIZONTAL_SELECTED)
      continue;

    lines_set[hnb] = n;
    inout_set[hnb] = 0;
    hnb++;
  }

  // it only makes sense to call ransac if we have more than two lines
  if(hnb > 2)
    ransac(g->lines, lines_set, inout_set, hnb, g->horizontal_weight,
           xmin, xmax, ymin, ymax);

  // adjust line selected flag according to the ransac results
  for(int n = 0; n < hnb; n++)
  {
    const int m = lines_set[n];
    if(inout_set[n] == 1)
    {
      g->lines[m].type |= ASHIFT_LINE_SELECTED;
      hcount++;
    }
    else
      g->lines[m].type &= ~ASHIFT_LINE_SELECTED;
  }
  // update number of horizontal lines
  g->horizontal_count = hcount;
  g->lines_version++;

  return TRUE;

error:
  return FALSE;
}

// utility function to map a variable in [min; max] to [-INF; + INF]
static inline double logit(double x, double min, double max)
{
  const double eps = 1.0e-6;
  // make sure p does not touch the borders of ist definition area
  // not critical as logit() is only used on initial fit parameters
  double p = CLAMP((x - min) / (max - min), eps, 1.0 - eps);

  return (2.0 * atanh(2.0 * p - 1.0));
}

// inverted function to logit()
static inline double ilogit(double L, double min, double max)
{
  double p = 0.5 * (1.0 + tanh(0.5 * L));

  return (p * (max - min) + min);
}

// helper function for simplex() return quality parameter for the given model
// strategy:
//    * generate homography matrix out of fixed parameters and fitting parameters
//    * apply homography to all end points of affected lines
//    * generate new line out of transformed end points
//    * calculate scalar product v of line with perpendicular axis
//    * sum over weighted v^2 values
// TODO: for fitting in both directions check if we should consolidate
//       individually and combine with a 50:50 weighting
static double model_fitness(double *params, void *data)
{
  dt_iop_ashift_fit_params_t *fit = (dt_iop_ashift_fit_params_t *)data;

  // just for convenience: get shorter names
  dt_iop_ashift_line_t *lines = fit->lines;
  const int lines_count = fit->lines_count;
  const int width = fit->width;
  const int height = fit->height;
  const float f_length_kb = fit->f_length_kb;
  const float orthocorr = fit->orthocorr;
  const float aspect = fit->aspect;

  float rotation = fit->rotation;
  float lensshift_v = fit->lensshift_v;
  float lensshift_h = fit->lensshift_h;
  float rotation_range = fit->rotation_range;
  float lensshift_v_range = fit->lensshift_v_range;
  float lensshift_h_range = fit->lensshift_h_range;

  int pcount = 0;

  // fill in fit parameters from params[]. Attention: order matters!!!
  if(isnan(rotation))
  {
    rotation = ilogit(params[pcount], -rotation_range, rotation_range);
    pcount++;
  }

  if(isnan(lensshift_v))
  {
    lensshift_v = ilogit(params[pcount], -lensshift_v_range, lensshift_v_range);
    pcount++;
  }

  if(isnan(lensshift_h))
  {
    lensshift_h = ilogit(params[pcount], -lensshift_h_range, lensshift_h_range);
    pcount++;
  }

  assert(pcount == fit->params_count);

  // the possible reference axes
  const float Av[3] = { 1.0f, 0.0f, 0.0f };
  const float Ah[3] = { 0.0f, 1.0f, 0.0f };

  // generate homograph out of the parameters
  float homograph[3][3];
  homography((float *)homograph, rotation, lensshift_v, lensshift_h, f_length_kb,
             orthocorr, aspect, width, height, ASHIFT_HOMOGRAPH_FORWARD);

  // accounting variables
  double sumsq_v = 0.0;
  double sumsq_h = 0.0;
  double weight_v = 0.0;
  double weight_h = 0.0;
  int count = 0;

  // iterate over all lines
  for(int n = 0; n < lines_count; n++)
  {
    // check if this is a line which we must skip
    if((lines[n].type & fit->linemask) != fit->linetype)
      continue;

    // the direction of this line (vertical)
    const int vertical = lines[n].type & ASHIFT_LINE_DIRVERT;

    // select the perpendicular reference axis
    const float *A = vertical ? Ah : Av;

    // apply homographic transformation to the end points
    float P1[3], P2[3];
    mat3mulv(P1, (float *)homograph, lines[n].p1);
    mat3mulv(P2, (float *)homograph, lines[n].p2);

    // get line connecting the two points
    float L[3];
    vec3prodn(L, P1, P2);

    // get scalar product of line with orthogonal axis -> gives 0 if line is perpendicular
    float v = vec3scalar(L, A);

    // sum up weighted v^2 for both directions individually
    sumsq_v += vertical ? v * v * lines[n].weight : 0.0;
    weight_v  += vertical ? lines[n].weight : 0.0;
    sumsq_h += !vertical ? v * v * lines[n].weight : 0.0;
    weight_h  += !vertical ? lines[n].weight : 0.0;;
    count++;
  }

  sumsq_v = weight_v > 0.0f ? sumsq_v / weight_v : 0.0;
  sumsq_h = weight_h > 0.0f ? sumsq_h / weight_h : 0.0;

  double sum = sqrt(1.0 - (1.0 - sumsq_v) * (1.0 - sumsq_h)) * 1.0e6;

#ifdef ASHIFT_DEBUG
  printf("fitness with rotation %f, lensshift_v %f, lensshift_h %f -> lines %d, quality %10f\n",
         rotation, lensshift_v, lensshift_h, count, sum);
#endif

  return sum;
}

// setup all data structures for fitting and call NM simplex
static dt_iop_ashift_nmsresult_t nmsfit(dt_iop_module_t *module, dt_iop_ashift_params_t *p, dt_iop_ashift_fitaxis_t dir)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  if(!g->lines) return NMS_NOT_ENOUGH_LINES;
  if(dir == ASHIFT_FIT_NONE) return NMS_SUCCESS;

  double params[3];
  int pcount = 0;
  int enough_lines = TRUE;

  // initialize fit parameters
  dt_iop_ashift_fit_params_t fit;
  fit.lines = g->lines;
  fit.lines_count = g->lines_count;
  fit.width = g->lines_in_width;
  fit.height = g->lines_in_height;
  fit.f_length_kb = (p->mode == ASHIFT_MODE_GENERIC) ? ASHIFT_DEFAULT_F_LENGTH : p->f_length * p->crop_factor;
  fit.orthocorr = (p->mode == ASHIFT_MODE_GENERIC) ? 0.0f : p->orthocorr;
  fit.aspect = (p->mode == ASHIFT_MODE_GENERIC) ? 1.0f : p->aspect;
  fit.rotation = p->rotation;
  fit.lensshift_v = p->lensshift_v;
  fit.lensshift_h = p->lensshift_h;
  fit.rotation_range = g->rotation_range;
  fit.lensshift_v_range = g->lensshift_v_range;
  fit.lensshift_h_range = g->lensshift_h_range;
  fit.linetype = ASHIFT_LINE_RELEVANT | ASHIFT_LINE_SELECTED;
  fit.linemask = ASHIFT_LINE_MASK;
  fit.params_count = 0;
  fit.weight = 0.0f;

  // if the image is flipped and if we do not want to fit both lens shift
  // directions or none at all, then we need to change direction
  dt_iop_ashift_fitaxis_t mdir = dir;
  if((mdir & ASHIFT_FIT_LENS_BOTH) != ASHIFT_FIT_LENS_BOTH &&
     (mdir & ASHIFT_FIT_LENS_BOTH) != 0)
  {
    // flip all directions
    mdir ^= g->isflipped ? ASHIFT_FIT_FLIP : 0;
    // special case that needs to be corrected
    mdir |= (mdir & ASHIFT_FIT_LINES_BOTH) == 0 ? ASHIFT_FIT_LINES_BOTH : 0;
  }


  // prepare fit structure and starting parameters for simplex fit.
  // note: the sequence of parameters in params[] needs to match the
  // respective order in dt_iop_ashift_fit_params_t. Parameters which are
  // to be fittet are marked with NAN in the fit structure. Non-NAN
  // parameters are assumed to be constant.
  if(mdir & ASHIFT_FIT_ROTATION)
  {
    // we fit rotation
    fit.params_count++;
    params[pcount] = logit(fit.rotation, -fit.rotation_range, fit.rotation_range);
    pcount++;
    fit.rotation = NAN;
  }

  if(mdir & ASHIFT_FIT_LENS_VERT)
  {
    // we fit vertical lens shift
    fit.params_count++;
    params[pcount] = logit(fit.lensshift_v, -fit.lensshift_v_range, fit.lensshift_v_range);
    pcount++;
    fit.lensshift_v = NAN;
  }

  if(mdir & ASHIFT_FIT_LENS_HOR)
  {
    // we fit horizontal lens shift
    fit.params_count++;
    params[pcount] = logit(fit.lensshift_h, -fit.lensshift_h_range, fit.lensshift_h_range);
    pcount++;
    fit.lensshift_h = NAN;
  }

  if(mdir & ASHIFT_FIT_LINES_VERT)
  {
    // we use vertical lines for fitting
    fit.linetype |= ASHIFT_LINE_DIRVERT;
    fit.weight += g->vertical_weight;
    enough_lines = enough_lines && (g->vertical_count >= MINIMUM_FITLINES);
  }

  if(mdir & ASHIFT_FIT_LINES_HOR)
  {
    // we use horizontal lines for fitting
    fit.linetype |= 0;
    fit.weight += g->horizontal_weight;
    enough_lines = enough_lines && (g->horizontal_count >= MINIMUM_FITLINES);
  }

  // this needs to come after ASHIFT_FIT_LINES_VERT and ASHIFT_FIT_LINES_HOR
  if((mdir & ASHIFT_FIT_LINES_BOTH) == ASHIFT_FIT_LINES_BOTH)
  {
    // if we use fitting in both directions we need to
    // adjust fit.linetype and fit.linemask to match all selected lines
    fit.linetype = ASHIFT_LINE_RELEVANT | ASHIFT_LINE_SELECTED;
    fit.linemask = ASHIFT_LINE_RELEVANT | ASHIFT_LINE_SELECTED;
  }

  // error case: we do not run simplex if there are not enough lines
  if(!enough_lines)
    return NMS_NOT_ENOUGH_LINES;

  // start the simplex fit
  int iter = simplex(model_fitness, params, fit.params_count, NMS_EPSILON, NMS_SCALE, NMS_ITERATIONS, NULL, (void*)&fit);

  // error case: the fit did not converge
  if(iter >= NMS_ITERATIONS)
    return NMS_DID_NOT_CONVERGE;

  // fit was successful: now write the results into structure p (order matters!!!)
  pcount = 0;
  p->rotation = isnan(fit.rotation) ? ilogit(params[pcount++], -fit.rotation_range, fit.rotation_range) : fit.rotation;
  p->lensshift_v = isnan(fit.lensshift_v) ? ilogit(params[pcount++], -fit.lensshift_v_range, fit.lensshift_v_range) : fit.lensshift_v;
  p->lensshift_h = isnan(fit.lensshift_h) ? ilogit(params[pcount++], -fit.lensshift_h_range, fit.lensshift_h_range) : fit.lensshift_h;

#ifdef ASHIFT_DEBUG
  printf("params after optimization (%d interations): rotation %f, lensshift_v %f, lensshift_h %f\n",
         iter, p->rotation, p->lensshift_v, p->lensshift_h);
#endif

  return NMS_SUCCESS;
}

#ifdef ASHIFT_DEBUG
// only used in development phase. call model_fitness() with current parameters and
// print some useful information
static void model_probe(dt_iop_module_t *module, dt_iop_ashift_params_t *p, dt_iop_ashift_fitaxis_t dir)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  if(!g->lines) return;
  if(dir == ASHIFT_FIT_NONE) return;

  double params[3];
  int enough_lines = TRUE;

  // initialize fit parameters
  dt_iop_ashift_fit_params_t fit;
  fit.lines = g->lines;
  fit.lines_count = g->lines_count;
  fit.width = g->lines_in_width;
  fit.height = g->lines_in_height;
  fit.f_length_kb = (p->mode == ASHIFT_MODE_GENERIC) ? ASHIFT_DEFAULT_F_LENGTH : p->f_length * p->crop_factor;
  fit.orthocorr = (p->mode == ASHIFT_MODE_GENERIC) ? 0.0f : p->orthocorr;
  fit.aspect = (p->mode == ASHIFT_MODE_GENERIC) ? 1.0f : p->aspect;
  fit.rotation = p->rotation;
  fit.lensshift_v = p->lensshift_v;
  fit.lensshift_h = p->lensshift_h;
  fit.linetype = ASHIFT_LINE_RELEVANT | ASHIFT_LINE_SELECTED;
  fit.linemask = ASHIFT_LINE_MASK;
  fit.params_count = 0;
  fit.weight = 0.0f;

  // if the image is flipped and if we do not want to fit both lens shift
  // directions or none at all, then we need to change direction
  dt_iop_ashift_fitaxis_t mdir = dir;
  if((mdir & ASHIFT_FIT_LENS_BOTH) != ASHIFT_FIT_LENS_BOTH &&
     (mdir & ASHIFT_FIT_LENS_BOTH) != 0)
  {
    // flip all directions
    mdir ^= g->isflipped ? ASHIFT_FIT_FLIP : 0;
    // special case that needs to be corrected
    mdir |= (mdir & ASHIFT_FIT_LINES_BOTH) == 0 ? ASHIFT_FIT_LINES_BOTH : 0;
  }

  if(mdir & ASHIFT_FIT_LINES_VERT)
  {
    // we use vertical lines for fitting
    fit.linetype |= ASHIFT_LINE_DIRVERT;
    fit.weight += g->vertical_weight;
    enough_lines = enough_lines && (g->vertical_count >= MINIMUM_FITLINES);
  }

  if(mdir & ASHIFT_FIT_LINES_HOR)
  {
    // we use horizontal lines for fitting
    fit.linetype |= 0;
    fit.weight += g->horizontal_weight;
    enough_lines = enough_lines && (g->horizontal_count >= MINIMUM_FITLINES);
  }

  // this needs to come after ASHIFT_FIT_LINES_VERT and ASHIFT_FIT_LINES_HOR
  if((mdir & ASHIFT_FIT_LINES_BOTH) == ASHIFT_FIT_LINES_BOTH)
  {
    // if we use fitting in both directions we need to
    // adjust fit.linetype and fit.linemask to match all selected lines
    fit.linetype = ASHIFT_LINE_RELEVANT | ASHIFT_LINE_SELECTED;
    fit.linemask = ASHIFT_LINE_RELEVANT | ASHIFT_LINE_SELECTED;
  }

  double quality = model_fitness(params, (void *)&fit);

  printf("model fitness: %.8f (rotation %f, lensshift_v %f, lensshift_h %f)\n",
         quality, p->rotation, p->lensshift_v, p->lensshift_h);
}
#endif

// helper function to start analysis for structural data and report about errors
static int do_get_structure(dt_iop_module_t *module, dt_iop_ashift_params_t *p,
                            dt_iop_ashift_enhance_t enhance)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  if(g->fitting) return FALSE;

  g->fitting = 1;

  dt_pthread_mutex_lock(&g->lock);
  float *b = g->buf;
  dt_pthread_mutex_unlock(&g->lock);

  if(b == NULL)
  {
    dt_control_log(_("data pending - please repeat"));
    goto error;
  }

  if(!get_structure(module, enhance))
  {
    dt_control_log(_("could not detect structural data in image"));
#ifdef ASHIFT_DEBUG
    // find out more
    printf("do_get_structure: buf %p, buf_hash %lu, buf_width %d, buf_height %d, lines %p, lines_count %d\n",
           g->buf, g->buf_hash, g->buf_width, g->buf_height, g->lines, g->lines_count);
#endif
    goto error;
  }

  if(!remove_outliers(module))
  {
    dt_control_log(_("could not run outlier removal"));
#ifdef ASHIFT_DEBUG
    // find out more
    printf("remove_outliers: buf %p, buf_hash %lu, buf_width %d, buf_height %d, lines %p, lines_count %d\n",
           g->buf, g->buf_hash, g->buf_width, g->buf_height, g->lines, g->lines_count);
#endif
    goto error;
  }

  g->fitting = 0;
  return TRUE;

error:
  g->fitting = 0;
  return FALSE;
}

// helper function to clean structural data
static int do_clean_structure(dt_iop_module_t *module, dt_iop_ashift_params_t *p)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  if(g->fitting) return FALSE;

  g->fitting = 1;
  g->lines_count = 0;
  g->vertical_count = 0;
  g->horizontal_count = 0;
  free(g->lines);
  g->lines = NULL;
  g->lines_version++;
  g->lines_suppressed = 0;
  g->fitting = 0;
  return TRUE;
}

// helper function to start parameter fit and report about errors
static int do_fit(dt_iop_module_t *module, dt_iop_ashift_params_t *p, dt_iop_ashift_fitaxis_t dir)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  if(g->fitting) return FALSE;

  // if no structure available get it
  if(g->lines == NULL)
    if(!do_get_structure(module, p, ASHIFT_ENHANCE_NONE)) goto error;

  g->fitting = 1;

  dt_iop_ashift_nmsresult_t res = nmsfit(module, p, dir);

  switch(res)
  {
    case NMS_NOT_ENOUGH_LINES:
      dt_control_log(_("not enough structure for automatic correction"));
      goto error;
      break;
    case NMS_DID_NOT_CONVERGE:
      dt_control_log(_("automatic correction failed, please correct manually"));
      goto error;
      break;
    case NMS_SUCCESS:
    default:
      break;
  }

  g->fitting = 0;
  return TRUE;

error:
  g->fitting = 0;
  return FALSE;
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  const int ch = piece->colors;
  const int ch_width = ch * roi_in->width;

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

  float ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h, data->f_length_kb,
             data->orthocorr, data->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(ihomograph, interpolation)
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

    // do modules coming before this one in pixelpipe have changed? -> check via hash value
    uint64_t hash = dt_dev_hash_plus(self->dev, self->dev->preview_pipe, 0, self->priority - 1);

    dt_pthread_mutex_lock(&g->lock);
    g->isflipped = isflipped;

    // save a copy of preview input buffer for parameter fitting
    if(g->buf == NULL || (size_t)g->buf_width * g->buf_height < (size_t)width * height)
    {
      // if needed allocate buffer
      free(g->buf); // a no-op if g->buf is NULL
      // only get new buffer if no old buffer or old buffer does not fit in terms of size
      g->buf = malloc((size_t)width * height * 4 * sizeof(float));
    }

    if(g->buf /* && hash != g->buf_hash */)
    {
      // copy data
      memcpy(g->buf, ivoid, (size_t)width * height * 4 * sizeof(float));

      g->buf_width = width;
      g->buf_height = height;
      g->buf_x_off = x_off;
      g->buf_y_off = y_off;
      g->buf_scale = scale;
      g->buf_hash = hash;
    }

    dt_pthread_mutex_unlock(&g->lock);
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_ashift_data_t *d = (dt_iop_ashift_data_t *)piece->data;
  dt_iop_ashift_global_data_t *gd = (dt_iop_ashift_global_data_t *)self->data;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  float ihomograph[3][3];
  homography((float *)ihomograph, d->rotation, d->lensshift_v, d->lensshift_h, d->f_length_kb,
             d->orthocorr, d->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

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

    // do modules coming before this one in pixelpipe have changed? -> check via hash value
    uint64_t hash = dt_dev_hash_plus(self->dev, self->dev->preview_pipe, 0, self->priority - 1);

    dt_pthread_mutex_lock(&g->lock);
    g->isflipped = isflipped;

    // save a copy of preview input buffer for parameter fitting
    if(g->buf == NULL || (size_t)g->buf_width * g->buf_height < (size_t)width * height)
    {
      // if needed allocate buffer
      free(g->buf); // a no-op if g->buf is NULL
      // only get new buffer if no old buffer or old buffer does not fit in terms of size
      g->buf = malloc((size_t)width * height * 4 * sizeof(float));
    }

    if(g->buf /* && hash != g->buf_hash */)
    {
      // copy data
      err = dt_opencl_copy_device_to_host(devid, g->buf, dev_in, width, height, 4 * sizeof(float));

      g->buf_width = width;
      g->buf_height = height;
      g->buf_x_off = x_off;
      g->buf_y_off = y_off;
      g->buf_scale = scale;
      g->buf_hash = hash;
    }
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

// gather information about "near"-ness in g->points_idx
static void get_near(const float *points, dt_iop_ashift_points_idx_t *points_idx, const int lines_count,
                     float pzx, float pzy, float delta)
{
  const float delta2 = delta * delta;

  for(int n = 0; n < lines_count; n++)
  {
    points_idx[n].near = 0;

    // first check if the mouse pointer is outside the bounding box of the line -> skip this line
    if(pzx < points_idx[n].bbx - delta &&
       pzx > points_idx[n].bbX + delta &&
       pzy < points_idx[n].bby - delta &&
       pzy > points_idx[n].bbY + delta)
      continue;

    // pointer is inside bounding box
    size_t offset = points_idx[n].offset;
    const int length = points_idx[n].length;

    // sanity check (this should not happen)
    if(length < 2) continue;

    // check line point by point
    for(int l = 0; l < length; l++, offset++)
    {
      float dx = pzx - points[offset * 2];
      float dy = pzy - points[offset * 2 + 1];

      if(dx * dx + dy * dy < delta2)
      {
        points_idx[n].near = 1;
        break;
      }
    }
  }
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

  // account for total number of points
  size_t total_points = 0;

  // first step: basic initialization of my_points_idx and counting of total_points
  for(int n = 0; n < lines_count; n++)
  {
    const int length = lines[n].length;
    
    total_points += length;

    my_points_idx[n].length = length;
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

// does this gui have focus?
static int gui_has_focus(struct dt_iop_module_t *self)
{
  return self->dev->gui_module == self;
}

void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  // the usual rescaling stuff
  float wd = dev->preview_pipe->backbuf_width;
  float ht = dev->preview_pipe->backbuf_height;
  if(wd < 1.0 || ht < 1.0) return;
  float zoom_y = dt_control_get_dev_zoom_y();
  float zoom_x = dt_control_get_dev_zoom_x();
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

  if(g->show_guides)
  {
    dt_guides_t *guide = (dt_guides_t *)g_list_nth_data(darktable.guides, 0);
    double dashes = DT_PIXEL_APPLY_DPI(5.0);
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
    cairo_set_source_rgb(cr, .8, .8, .8);
    cairo_set_dash(cr, &dashes, 1, 0);
    guide->draw(cr, 0, 0, width, height, 1.0, guide->user_data);
    cairo_stroke_preserve(cr);
    cairo_set_dash(cr, &dashes, 0, 0);
    cairo_set_source_rgba(cr, 0.3, .3, .3, .8);
    cairo_stroke(cr);
    cairo_restore(cr);
  }

  // structural data are currently being collected or fit procedure is running? -> skip
  if(g->fitting) return;

  // no structural data or visibility switched off? -> nothing to do
  if(g->lines == NULL || g->lines_suppressed || !gui_has_focus(self)) return;

  // points data are missing or outdated, or distortion has changed? -> generate points
  uint64_t hash = dt_dev_hash_distort(dev);

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


  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_clip(cr);
  cairo_translate(cr, width / 2.0, height / 2.0);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  // this must match the sequence of enum dt_iop_ashift_linecolor_t!
  const float line_colors[5][4] =
  { { 0.3f, 0.3f, 0.3f, 0.8f },                    // grey (misc. lines)
    { 0.0f, 1.0f, 0.0f, 0.8f },                    // green (selected vertical lines)
    { 0.8f, 0.0f, 0.0f, 0.8f },                    // red (de-selected vertical lines)
    { 0.0f, 0.0f, 1.0f, 0.8f },                    // blue (selected horizontal lines)
    { 0.8f, 0.8f, 0.0f, 0.8f } };                  // yellow (de-selected horizontal lines)

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  // now draw all lines
  for(int n = 0; n < g->points_lines_count; n++)
  {
    // is the near flag set? -> draw line a bit thicker
    if(g->points_idx[n].near)
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3.0) / zoom_scale);
    else
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5) / zoom_scale);

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

int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  int handled = 0;

  float wd = self->dev->preview_pipe->backbuf_width;
  float ht = self->dev->preview_pipe->backbuf_height;
  if(wd < 1.0 || ht < 1.0) return 1;

  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  // gather information about "near"-ness in g->points_idx
  get_near(g->points, g->points_idx, g->points_lines_count, pzx * wd, pzy * ht, POINTS_NEAR_DELTA);

  // if we are in sweeping mode iterate over lines as we move the pointer and change "selected" state.
  if(g->isdeselecting || g->isselecting)
  {
    for(int n = 0; n < g->points_lines_count; n++)
    {
      if(g->points_idx[n].near == 0)
        continue;

      if(g->isdeselecting)
        g->lines[n].type &= ~ASHIFT_LINE_SELECTED;
      else if(g->isselecting)
        g->lines[n].type |= ASHIFT_LINE_SELECTED;

      handled = 1;
    }
  }

  if(handled)
    g->lines_version++;

  dt_control_queue_redraw_center();

  // if not in sweeping mode we need to pass the event
  return (g->isdeselecting || g->isselecting);
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  int handled = 0;

  // do nothing if visibility of lines is switched off or no lines available
  if(g->lines_suppressed || g->lines == NULL)
    return FALSE;

  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  const float min_scale = dt_dev_get_zoom_scale(self->dev, DT_ZOOM_FIT, closeup ? 2.0 : 1.0, 0);
  const float cur_scale = dt_dev_get_zoom_scale(self->dev, zoom, closeup ? 2.0 : 1.0, 0);

  // if we are zoomed out (no panning possible) and we have lines to display we take control
  int take_control = (cur_scale == min_scale) && (g->points_lines_count > 0);

  // iterate over all lines close to the pointer and change "selected" state.
  // left-click selects and right-click deselects the line
  for(int n = 0; n < g->points_lines_count; n++)
  {
    if(g->points_idx[n].near == 0)
      continue;

    if(which == 3)
      g->lines[n].type &= ~ASHIFT_LINE_SELECTED;
    else
      g->lines[n].type |= ASHIFT_LINE_SELECTED;

    handled = 1;
  }

  // we switch into sweeping mode either if we anyhow take control
  // or if cursor was close to a line when button was pressed. in other
  // cases we hand over the event (for image panning)
  if((take_control || handled) && which == 3)
  {
    dt_control_change_cursor(GDK_PIRATE);
    g->isdeselecting = 1;
  }
  else if(take_control || handled)
  {
    dt_control_change_cursor(GDK_PLUS);
    g->isselecting = 1;
  }

  if(handled)
    g->lines_version++;

  return (take_control || handled);
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  // end of sweeping mode
  dt_control_change_cursor(GDK_LEFT_PTR);
  g->isselecting = g->isdeselecting = 0;

  return 0;
}

// adjust the range values in gui data if needed from the narrow default boundaries
// to soft boundaries
static void range_adjust(dt_iop_ashift_params_t *p, dt_iop_ashift_gui_data_t *g)
{
  g->rotation_range = fabs(p->rotation) > ROTATION_RANGE ? ROTATION_RANGE_SOFT : g->rotation_range;
  g->lensshift_v_range = fabs(p->lensshift_v) > LENSSHIFT_RANGE ? LENSSHIFT_RANGE_SOFT : g->lensshift_v_range;
  g->lensshift_h_range = fabs(p->lensshift_h) > LENSSHIFT_RANGE ? LENSSHIFT_RANGE_SOFT : g->lensshift_h_range;
}

static void rotation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  p->rotation = dt_bauhaus_slider_get(slider);
#ifdef ASHIFT_DEBUG
  model_probe(self, p, g->lastfit);
#endif
  range_adjust(p, g);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lensshift_v_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  p->lensshift_v = dt_bauhaus_slider_get(slider);
#ifdef ASHIFT_DEBUG
  model_probe(self, p, g->lastfit);
#endif
  range_adjust(p, g);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lensshift_h_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  p->lensshift_h = dt_bauhaus_slider_get(slider);
#ifdef ASHIFT_DEBUG
  model_probe(self, p, g->lastfit);
#endif
  range_adjust(p, g);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void guide_lines_callback(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  g->show_guides = dt_bauhaus_combobox_get(widget);
  dt_iop_request_focus(self);
  dt_dev_reprocess_all(self->dev);
}

static void mode_callback(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  p->mode = dt_bauhaus_combobox_get(widget);

  switch(p->mode)
  {
    case ASHIFT_MODE_GENERIC:
      gtk_widget_hide(g->f_length);
      gtk_widget_hide(g->crop_factor);
      gtk_widget_hide(g->orthocorr);
      gtk_widget_hide(g->aspect);
      break;
    case ASHIFT_MODE_SPECIFIC:
    default:
      gtk_widget_show(g->f_length);
      gtk_widget_show(g->crop_factor);
      gtk_widget_show(g->orthocorr);
      gtk_widget_show(g->aspect);
      break;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void f_length_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->f_length = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void crop_factor_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->crop_factor = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void orthocorr_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->orthocorr = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void aspect_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->aspect = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static int fit_v_button_clicked(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return FALSE;

  if(event->button == 1)
  {
    dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
    dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

    const int control = (event->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK;
    const int shift = (event->state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK;

    dt_iop_ashift_fitaxis_t fitaxis = ASHIFT_FIT_NONE;

    if(control)
      fitaxis = ASHIFT_FIT_ROTATION_VERTICAL_LINES;
    else if(shift)
      fitaxis = ASHIFT_FIT_VERTICALLY_NO_ROTATION;
    else
      fitaxis = ASHIFT_FIT_VERTICALLY;

    dt_iop_request_focus(self);
    dt_dev_reprocess_all(self->dev);
    if(do_fit(self, p, fitaxis))
    {
      darktable.gui->reset = 1;
      dt_bauhaus_slider_set_soft(g->rotation, p->rotation);
      dt_bauhaus_slider_set_soft(g->lensshift_v, p->lensshift_v);
      dt_bauhaus_slider_set_soft(g->lensshift_h, p->lensshift_h);
      darktable.gui->reset = 0;
    }
    g->lastfit = fitaxis;

    // hack to guarantee that module gets enabled on button click
    if(!self->enabled) p->toggle ^= 1;

    dt_dev_add_history_item(darktable.develop, self, TRUE);

    return TRUE;
  }
  return FALSE;
}

static int fit_h_button_clicked(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return FALSE;

  if(event->button == 1)
  {
    dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
    dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

    const int control = (event->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK;
    const int shift = (event->state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK;

    dt_iop_ashift_fitaxis_t fitaxis = ASHIFT_FIT_NONE;

    if(control)
      fitaxis = ASHIFT_FIT_ROTATION_HORIZONTAL_LINES;
    else if(shift)
      fitaxis = ASHIFT_FIT_HORIZONTALLY_NO_ROTATION;
    else
      fitaxis = ASHIFT_FIT_HORIZONTALLY;

    dt_iop_request_focus(self);
    dt_dev_reprocess_all(self->dev);
    if(do_fit(self, p, fitaxis))
    {
      darktable.gui->reset = 1;
      dt_bauhaus_slider_set_soft(g->rotation, p->rotation);
      dt_bauhaus_slider_set_soft(g->lensshift_v, p->lensshift_v);
      dt_bauhaus_slider_set_soft(g->lensshift_h, p->lensshift_h);
      darktable.gui->reset = 0;
    }
    g->lastfit = fitaxis;

    // hack to guarantee that module gets enabled on button click
    if(!self->enabled) p->toggle ^= 1;

    dt_dev_add_history_item(darktable.develop, self, TRUE);

    return TRUE;
  }
  return FALSE;
}

static int fit_both_button_clicked(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return FALSE;

  if(event->button == 1)
  {
    dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
    dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

    const int control = (event->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK;
    const int shift = (event->state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK;

    dt_iop_ashift_fitaxis_t fitaxis = ASHIFT_FIT_NONE;

    if(control)
      fitaxis = ASHIFT_FIT_ROTATION_BOTH_LINES;
    else if(shift)
      fitaxis = ASHIFT_FIT_BOTH_NO_ROTATION;
    else
      fitaxis = ASHIFT_FIT_BOTH;

    dt_iop_request_focus(self);
    dt_dev_reprocess_all(self->dev);
    if(do_fit(self, p, fitaxis))
    {
      darktable.gui->reset = 1;
      dt_bauhaus_slider_set_soft(g->rotation, p->rotation);
      dt_bauhaus_slider_set_soft(g->lensshift_v, p->lensshift_v);
      dt_bauhaus_slider_set_soft(g->lensshift_h, p->lensshift_h);
      darktable.gui->reset = 0;
    }
    g->lastfit = fitaxis;

    // hack to guarantee that module gets enabled on button click
    if(!self->enabled) p->toggle ^= 1;

    dt_dev_add_history_item(darktable.develop, self, TRUE);

    return TRUE;
  }
  return FALSE;
}

static int structure_button_clicked(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return FALSE;

  if(event->button == 1)
  {
    dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;

    const int control = (event->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK;

    dt_iop_request_focus(self);
    dt_dev_reprocess_all(self->dev);
    if(control)
      (void)do_get_structure(self, p, ASHIFT_ENHANCE_EDGES);
    else
      (void)do_get_structure(self, p, ASHIFT_ENHANCE_NONE);
    // hack to guarantee that module gets enabled on button click
    if(!self->enabled) p->toggle ^= 1;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return TRUE;
  }
  return FALSE;
}

static void clean_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  (void)do_clean_structure(self, p);
  dt_iop_request_focus(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void eye_button_toggled(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  if(darktable.gui->reset) return;
  if(g->lines == NULL)
  {
    g->lines_suppressed = 0;
    gtk_toggle_button_set_active(togglebutton, 0);
  }
  else
  {
    g->lines_suppressed = gtk_toggle_button_get_active(togglebutton);
  }
  dt_iop_request_focus(self);
  dt_dev_reprocess_all(self->dev);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)p1;
  dt_iop_ashift_data_t *d = (dt_iop_ashift_data_t *)piece->data;

  d->rotation = p->rotation;
  d->lensshift_v = p->lensshift_v;
  d->lensshift_h = p->lensshift_h;
  d->f_length_kb = (p->mode == ASHIFT_MODE_GENERIC) ? ASHIFT_DEFAULT_F_LENGTH : p->f_length * p->crop_factor;
  d->orthocorr = (p->mode == ASHIFT_MODE_GENERIC) ? 0.0f : p->orthocorr;
  d->aspect = (p->mode == ASHIFT_MODE_GENERIC) ? 1.0f : p->aspect;
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
  dt_bauhaus_slider_set_soft(g->rotation, p->rotation);
  dt_bauhaus_slider_set_soft(g->lensshift_v, p->lensshift_v);
  dt_bauhaus_slider_set_soft(g->lensshift_h, p->lensshift_h);
  dt_bauhaus_slider_set(g->f_length, p->f_length);
  dt_bauhaus_slider_set(g->crop_factor, p->crop_factor);
  dt_bauhaus_slider_set(g->orthocorr, p->orthocorr);
  dt_bauhaus_slider_set(g->aspect, p->aspect);
  dt_bauhaus_combobox_set(g->mode, p->mode);
  dt_bauhaus_combobox_set(g->guide_lines, g->show_guides);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->eye), 0);

  switch(p->mode)
  {
    case ASHIFT_MODE_GENERIC:
      gtk_widget_hide(g->f_length);
      gtk_widget_hide(g->crop_factor);
      gtk_widget_hide(g->orthocorr);
      gtk_widget_hide(g->aspect);
      break;
    case ASHIFT_MODE_SPECIFIC:
    default:
      gtk_widget_show(g->f_length);
      gtk_widget_show(g->crop_factor);
      gtk_widget_show(g->orthocorr);
      gtk_widget_show(g->aspect);
      break;
  }
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_ashift_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_ashift_params_t));
  module->default_enabled = 0;
  module->priority = 252; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_ashift_params_t);
  module->gui_data = NULL;
  dt_iop_ashift_params_t tmp = (dt_iop_ashift_params_t){ 0.0f, 0.0f, 0.0f, ASHIFT_DEFAULT_F_LENGTH, 1.0f, 100.0f, 1.0f, ASHIFT_MODE_GENERIC, 0 };
  memcpy(module->params, &tmp, sizeof(dt_iop_ashift_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_ashift_params_t));
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)module->params;
  // our module is disabled by default
  module->default_enabled = 0;

  int isflipped = 0;
  float f_length = ASHIFT_DEFAULT_F_LENGTH;
  float crop_factor = 1.0f;

  // try to get information on orientation, focal length and crop factor from image data
  if(module->dev)
  {
    const dt_image_t *img = &module->dev->image_storage;
    // orientation only needed as a-priori information to correctly label some sliders
    // before pixelpipe has been set up. later we will get a definite result by
    // assessing the pixelpipe
    isflipped = (img->orientation == ORIENTATION_ROTATE_CCW_90_DEG
                 || img->orientation == ORIENTATION_ROTATE_CW_90_DEG)
                    ? 1
                    : 0;

    // focal length should be available in exif data if lens is electronically coupled to the camera
    f_length = isfinite(img->exif_focal_length) && img->exif_focal_length > 0.0f ? img->exif_focal_length : f_length;
    // crop factor of the camera is often not available and user will need to set it manually in the gui
    crop_factor = isfinite(img->exif_crop) && img->exif_crop > 0.0f ? img->exif_crop : crop_factor;
  }

  // init defaults:
  dt_iop_ashift_params_t tmp = (dt_iop_ashift_params_t){ 0.0f, 0.0f, 0.0f, f_length, crop_factor, 100.0f, 1.0f, ASHIFT_MODE_GENERIC, 0 };
  memcpy(module->params, &tmp, sizeof(dt_iop_ashift_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_ashift_params_t));

  // reset gui elements
  if(module->gui_data)
  {
    dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

    char string_v[256];
    char string_h[256];

    snprintf(string_v, sizeof(string_v), _("lens shift (%s)"), isflipped ? _("horizontal") : _("vertical"));
    snprintf(string_h, sizeof(string_h), _("lens shift (%s)"), isflipped ? _("vertical") : _("horizontal"));

    dt_bauhaus_widget_set_label(g->lensshift_v, NULL, string_v);
    dt_bauhaus_widget_set_label(g->lensshift_h, NULL, string_h);

    dt_bauhaus_slider_set_default(g->f_length, tmp.f_length);
    dt_bauhaus_slider_set_default(g->crop_factor, tmp.crop_factor);

    dt_pthread_mutex_lock(&g->lock);
    free(g->buf);
    g->buf = NULL;
    g->buf_width = 0;
    g->buf_height = 0;
    g->buf_x_off = 0;
    g->buf_y_off = 0;
    g->buf_scale = 1.0f;
    g->buf_hash = 0;
    g->isflipped = -1;
    g->lastfit = ASHIFT_FIT_NONE;
    dt_pthread_mutex_unlock(&g->lock);

    g->fitting = 0;
    free(g->lines);
    g->lines = NULL;
    g->lines_count =0;
    g->horizontal_count = 0;
    g->vertical_count = 0;
    g->grid_hash = 0;
    g->rotation_range = ROTATION_RANGE;
    g->lensshift_v_range = LENSSHIFT_RANGE;
    g->lensshift_h_range = LENSSHIFT_RANGE;
    g->lines_suppressed = 0;
    g->lines_version = 0;
    g->show_guides = 0;
    g->isselecting = 0;
    g->isdeselecting = 0;

    free(g->points);
    g->points = NULL;
    free(g->points_idx);
    g->points_idx = NULL;
    g->points_lines_count = 0;
    g->points_version = 0;

    range_adjust(p, g);
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

  if(isflipped == -1) return FALSE;

  char string_v[256];
  char string_h[256];

  snprintf(string_v, sizeof(string_v), _("lens shift (%s)"), isflipped ? _("horizontal") : _("vertical"));
  snprintf(string_h, sizeof(string_h), _("lens shift (%s)"), isflipped ? _("vertical") : _("horizontal"));

  darktable.gui->reset = 1;
  dt_bauhaus_widget_set_label(g->lensshift_v, NULL, string_v);
  dt_bauhaus_widget_set_label(g->lensshift_h, NULL, string_h);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->eye), g->lines_suppressed);
  darktable.gui->reset = 0;

  return FALSE;
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  if(self->enabled)
  {
    if(in)
    {
      // got focus. make it redraw in full
      dt_dev_reprocess_all(self->dev);
    }
    else
    {
      dt_dev_reprocess_all(self->dev);
      //dt_control_queue_redraw_center();
    }
  }
}

static float log10_callback(GtkWidget *self, float inval, dt_bauhaus_callback_t dir)
{
  float outval;
  switch(dir)
  {
    case DT_BAUHAUS_SET:
      outval = log10(fmax(inval, 1e-15f));
      break;
    case DT_BAUHAUS_GET:
      outval = exp(M_LN10 * inval);
      break;
    default:
      outval = inval;
  }
  return outval;
}

static float log2_callback(GtkWidget *self, float inval, dt_bauhaus_callback_t dir)
{
  float outval;
  switch(dir)
  {
    case DT_BAUHAUS_SET:
      outval = log(fmax(inval, 1e-15f)) / M_LN2;
      break;
    case DT_BAUHAUS_GET:
      outval = exp(M_LN2 * inval);
      break;
    default:
      outval = inval;
  }
  return outval;
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
  g->buf_hash = 0;
  g->isflipped = -1;
  g->lastfit = ASHIFT_FIT_NONE;
  dt_pthread_mutex_unlock(&g->lock);

  g->fitting = 0;
  g->lines = NULL;
  g->lines_count = 0;
  g->vertical_count = 0;
  g->horizontal_count = 0;
  g->lines_version = 0;
  g->lines_suppressed = 0;
  g->points = NULL;
  g->points_idx = NULL;
  g->points_lines_count = 0;
  g->points_version = 0;
  g->grid_hash = 0;
  g->rotation_range = ROTATION_RANGE;
  g->lensshift_v_range = LENSSHIFT_RANGE;
  g->lensshift_h_range = LENSSHIFT_RANGE;
  g->show_guides = 0;
  g->isselecting = 0;
  g->isdeselecting = 0;
  range_adjust(p, g);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->rotation = dt_bauhaus_slider_new_with_range(self, -ROTATION_RANGE, ROTATION_RANGE, 0.01*ROTATION_RANGE, p->rotation, 2);
  dt_bauhaus_widget_set_label(g->rotation, NULL, _("rotation"));
  dt_bauhaus_slider_set_format(g->rotation, "%.2f°");
  dt_bauhaus_slider_enable_soft_boundaries(g->rotation, -ROTATION_RANGE_SOFT, ROTATION_RANGE_SOFT);
  gtk_box_pack_start(GTK_BOX(self->widget), g->rotation, TRUE, TRUE, 0);

  g->lensshift_v = dt_bauhaus_slider_new_with_range(self, -LENSSHIFT_RANGE, LENSSHIFT_RANGE, 0.01*LENSSHIFT_RANGE, p->lensshift_v, 3);
  dt_bauhaus_widget_set_label(g->lensshift_v, NULL, _("lens shift (vertical)"));
  dt_bauhaus_slider_enable_soft_boundaries(g->lensshift_v, -LENSSHIFT_RANGE_SOFT, LENSSHIFT_RANGE_SOFT);
  gtk_box_pack_start(GTK_BOX(self->widget), g->lensshift_v, TRUE, TRUE, 0);

  g->lensshift_h = dt_bauhaus_slider_new_with_range(self, -LENSSHIFT_RANGE, LENSSHIFT_RANGE, 0.01*LENSSHIFT_RANGE, p->lensshift_v, 3);
  dt_bauhaus_widget_set_label(g->lensshift_h, NULL, _("lens shift (horizontal)"));
  dt_bauhaus_slider_enable_soft_boundaries(g->lensshift_h, -LENSSHIFT_RANGE_SOFT, LENSSHIFT_RANGE_SOFT);
  gtk_box_pack_start(GTK_BOX(self->widget), g->lensshift_h, TRUE, TRUE, 0);

  g->guide_lines = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->guide_lines, NULL, _("guides"));
  dt_bauhaus_combobox_add(g->guide_lines, _("off"));
  dt_bauhaus_combobox_add(g->guide_lines, _("on"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->guide_lines, TRUE, TRUE, 0);

  g->mode = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->mode, NULL, _("lens model"));
  dt_bauhaus_combobox_add(g->mode, _("generic"));
  dt_bauhaus_combobox_add(g->mode, _("specific"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->mode, TRUE, TRUE, 0);

  g->f_length = dt_bauhaus_slider_new_with_range(self, 1.0f, 3.0f, 0.01f, 1.0f, 2);
  dt_bauhaus_widget_set_label(g->f_length, NULL, _("focal length"));
  dt_bauhaus_slider_set_callback(g->f_length, log10_callback);
  dt_bauhaus_slider_set_format(g->f_length, "%.0fmm");
  dt_bauhaus_slider_set_default(g->f_length, ASHIFT_DEFAULT_F_LENGTH);
  dt_bauhaus_slider_set(g->f_length, ASHIFT_DEFAULT_F_LENGTH);
  dt_bauhaus_slider_enable_soft_boundaries(g->f_length, 1.0f, 2000.0f);
  gtk_box_pack_start(GTK_BOX(self->widget), g->f_length, TRUE, TRUE, 0);

  g->crop_factor = dt_bauhaus_slider_new_with_range(self, 1.0f, 2.0f, 0.1f, p->crop_factor, 2);
  dt_bauhaus_widget_set_label(g->crop_factor, NULL, _("crop factor"));
  dt_bauhaus_slider_enable_soft_boundaries(g->crop_factor, 0.5f, 10.0f);
  gtk_box_pack_start(GTK_BOX(self->widget), g->crop_factor, TRUE, TRUE, 0);

  g->orthocorr = dt_bauhaus_slider_new_with_range(self, 0.0f, 100.0f, 1.0f, p->orthocorr, 2);
  dt_bauhaus_widget_set_label(g->orthocorr, NULL, _("lens dependence"));
  dt_bauhaus_slider_set_format(g->orthocorr, "%.0f%%");
#if 0
  // this parameter could serve to finetune between generic model (0%) and specific model (100%).
  // however, users can more easily get the same effect with the aspect adjust parameter so we keep
  // this one hidden.
  gtk_box_pack_start(GTK_BOX(self->widget), g->orthocorr, TRUE, TRUE, 0);
#endif

  g->aspect = dt_bauhaus_slider_new_with_range(self, -1.0f, 1.0f, 0.01f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->aspect, NULL, _("aspect adjust"));
  dt_bauhaus_slider_set_callback(g->aspect, log2_callback);
  dt_bauhaus_slider_set_default(g->aspect, 1.0f);
  dt_bauhaus_slider_set(g->aspect, 1.0f);
  gtk_box_pack_start(GTK_BOX(self->widget), g->aspect, TRUE, TRUE, 0);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 2 * DT_BAUHAUS_SPACE);
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(10));

  GtkWidget *label1 = gtk_label_new(_("automatic fit"));
  gtk_widget_set_halign(label1, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), label1, 0, 0, 1, 1);

  g->fit_v = dtgtk_button_new(dtgtk_cairo_paint_perspective, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER | 1);
  gtk_widget_set_hexpand(GTK_WIDGET(g->fit_v), TRUE);
  gtk_widget_set_size_request(g->fit_v, -1, DT_PIXEL_APPLY_DPI(24));
  gtk_grid_attach_next_to(GTK_GRID(grid), g->fit_v, label1, GTK_POS_RIGHT, 1, 1);

  g->fit_h = dtgtk_button_new(dtgtk_cairo_paint_perspective, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER | 2);
  gtk_widget_set_hexpand(GTK_WIDGET(g->fit_h), TRUE);
  gtk_widget_set_size_request(g->fit_h, -1, DT_PIXEL_APPLY_DPI(24));
  gtk_grid_attach_next_to(GTK_GRID(grid), g->fit_h, g->fit_v, GTK_POS_RIGHT, 1, 1);

  g->fit_both = dtgtk_button_new(dtgtk_cairo_paint_perspective, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER | 3);
  gtk_widget_set_hexpand(GTK_WIDGET(g->fit_both), TRUE);
  gtk_widget_set_size_request(g->fit_both, -1, DT_PIXEL_APPLY_DPI(24));
  gtk_grid_attach_next_to(GTK_GRID(grid), g->fit_both, g->fit_h, GTK_POS_RIGHT, 1, 1);

  GtkWidget *label2 = gtk_label_new(_("get structure"));
  gtk_widget_set_halign(label1, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), label2, 0, 1, 1, 1);

  g->structure = dtgtk_button_new(dtgtk_cairo_paint_structure, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER);
  gtk_widget_set_hexpand(GTK_WIDGET(g->structure), TRUE);
  gtk_grid_attach_next_to(GTK_GRID(grid), g->structure, label2, GTK_POS_RIGHT, 1, 1);

  g->clean = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER);
  gtk_widget_set_hexpand(GTK_WIDGET(g->clean), TRUE);
  gtk_grid_attach_next_to(GTK_GRID(grid), g->clean, g->structure, GTK_POS_RIGHT, 1, 1);

  g->eye = dtgtk_togglebutton_new(dtgtk_cairo_paint_eye_toggle, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER);
  gtk_widget_set_hexpand(GTK_WIDGET(g->eye), TRUE);
  gtk_grid_attach_next_to(GTK_GRID(grid), g->eye, g->clean, GTK_POS_RIGHT, 1, 1);

  gtk_box_pack_start(GTK_BOX(self->widget), grid, TRUE, TRUE, 0);

  gtk_widget_show_all(g->f_length);
  gtk_widget_set_no_show_all(g->f_length, TRUE);
  gtk_widget_show_all(g->crop_factor);
  gtk_widget_set_no_show_all(g->crop_factor, TRUE);
  gtk_widget_show_all(g->orthocorr);
  gtk_widget_set_no_show_all(g->orthocorr, TRUE);
  gtk_widget_show_all(g->aspect);
  gtk_widget_set_no_show_all(g->aspect, TRUE);


  switch(p->mode)
  {
    case ASHIFT_MODE_GENERIC:
      gtk_widget_hide(g->f_length);
      gtk_widget_hide(g->crop_factor);
      gtk_widget_hide(g->orthocorr);
      gtk_widget_hide(g->aspect);
      break;
    case ASHIFT_MODE_SPECIFIC:
    default:
      gtk_widget_show(g->f_length);
      gtk_widget_show(g->crop_factor);
      gtk_widget_show(g->orthocorr);
      gtk_widget_show(g->aspect);
      break;
  }

  g_object_set(g->rotation, "tooltip-text", _("rotate image"), (char *)NULL);
  g_object_set(g->lensshift_v, "tooltip-text", _("apply lens shift correction in one direction"),
               (char *)NULL);
  g_object_set(g->lensshift_h, "tooltip-text", _("apply lens shift correction in one direction"),
               (char *)NULL);
  g_object_set(g->guide_lines, "tooltip-text", _("display guide lines overlay"),
               (char *)NULL);
  g_object_set(g->mode, "tooltip-text", _("lens model of the perspective correction: generic or according to the focal length"),
               (char *)NULL);
  g_object_set(g->f_length, "tooltip-text", _("focal length of the lens, default value set from exif data if available"),
               (char *)NULL);
  g_object_set(g->crop_factor, "tooltip-text", _("crop factor of the camera sensor, default value set from exif data if available,"
                                                 "manual setting is often required"),
               (char *)NULL);
  g_object_set(g->orthocorr, "tooltip-text", _("the level of lens dependent correction, 100%% for full dependency, 0%% for the generic case"),
               (char *)NULL);
  g_object_set(g->aspect, "tooltip-text", _("adjust aspect ratio of image by horizontal and vertical scaling"),
               (char *)NULL);
  g_object_set(g->fit_v, "tooltip-text", _("automatically correct for vertical perspective distortion\n"
                                           "ctrl-click to only fit rotation\n"
                                           "shift-click to only fit lens shift"), (char *)NULL);
  g_object_set(g->fit_h, "tooltip-text", _("automatically correct for horizontal perspective distortion\n"
                                           "ctrl-click to only fit rotation\n"
                                           "shift-click to only fit lens shift"), (char *)NULL);
  g_object_set(g->fit_both, "tooltip-text", _("automatically correct for vertical and horizontal perspective distortions\n"
                                              "ctrl-click to only fit rotation\n"
                                              "shift-click to only fit lens shift"), (char *)NULL);
  g_object_set(g->structure, "tooltip-text", _("analyse line structure in image\n"
                                               "ctrl-click for additional edge enhancement"), (char *)NULL);
  g_object_set(g->clean, "tooltip-text", _("remove line structure information"), (char *)NULL);
  g_object_set(g->eye, "tooltip-text", _("toggle visibility of structure lines"), (char *)NULL);

  g_signal_connect(G_OBJECT(g->rotation), "value-changed", G_CALLBACK(rotation_callback), self);
  g_signal_connect(G_OBJECT(g->lensshift_v), "value-changed", G_CALLBACK(lensshift_v_callback), self);
  g_signal_connect(G_OBJECT(g->lensshift_h), "value-changed", G_CALLBACK(lensshift_h_callback), self);
  g_signal_connect(G_OBJECT(g->guide_lines), "value-changed", G_CALLBACK(guide_lines_callback), self);
  g_signal_connect(G_OBJECT(g->mode), "value-changed", G_CALLBACK(mode_callback), self);
  g_signal_connect(G_OBJECT(g->f_length), "value-changed", G_CALLBACK(f_length_callback), self);
  g_signal_connect(G_OBJECT(g->crop_factor), "value-changed", G_CALLBACK(crop_factor_callback), self);
  g_signal_connect(G_OBJECT(g->orthocorr), "value-changed", G_CALLBACK(orthocorr_callback), self);
  g_signal_connect(G_OBJECT(g->aspect), "value-changed", G_CALLBACK(aspect_callback), self);
  g_signal_connect(G_OBJECT(g->fit_v), "button-press-event", G_CALLBACK(fit_v_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(g->fit_h), "button-press-event", G_CALLBACK(fit_h_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(g->fit_both), "button-press-event", G_CALLBACK(fit_both_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(g->structure), "button-press-event", G_CALLBACK(structure_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(g->clean), "clicked", G_CALLBACK(clean_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(g->eye), "toggled", G_CALLBACK(eye_button_toggled), (gpointer)self);
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
