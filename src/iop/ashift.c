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
#include "common/bilateral.h"
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

// Inspiration to this module comes from the program ShiftN (http://www.shiftn.de) by
// Marcus Hebel.

// Thanks to Marcus for his support when implementing part of the ShiftN functionality
// to darktable.

#define ROTATION_RANGE 10                   // allowed min/max default range for rotation parameter
#define ROTATION_RANGE_SOFT 20              // allowed min/max range for rotation parameter with manual adjustment
#define LENSSHIFT_RANGE 0.5                 // allowed min/max default range for lensshift paramters
#define LENSSHIFT_RANGE_SOFT 1              // allowed min/max range for lensshift paramters with manual adjustment
#define SHEAR_RANGE 0.2                     // allowed min/max range for shear parameter
#define SHEAR_RANGE_SOFT 0.5                // allowed min/max range for shear parameter with manual adjustment
#define MIN_LINE_LENGTH 5                   // the minimum length of a line in pixels to be regarded as relevant
#define MAX_TANGENTIAL_DEVIATION 30         // by how many degrees a line may deviate from the +/-180 and +/-90 to be regarded as relevant
#define POINTS_NEAR_DELTA 4                 // distance of mouse pointer to line for "near" detection
#define LSD_SCALE 0.99                      // LSD: scaling factor for line detection
#define LSD_SIGMA_SCALE 0.6                 // LSD: sigma for Gaussian filter is computed as sigma = sigma_scale/scale
#define LSD_QUANT 2.0                       // LSD: bound to the quantization error on the gradient norm
#define LSD_ANG_TH 22.5                     // LSD: gradient angle tolerance in degrees
#define LSD_LOG_EPS 0.0                     // LSD: detection threshold: -log10(NFA) > log_eps
#define LSD_DENSITY_TH 0.7                  // LSD: minimal density of region points in rectangle
#define LSD_N_BINS 1024                     // LSD: number of bins in pseudo-ordering of gradient modulus
#define LSD_GAMMA 0.45                      // gamma correction to apply on raw images prior to line detection
#define RANSAC_RUNS 400                     // how many interations to run in ransac
#define RANSAC_EPSILON 2                    // starting value for ransac epsilon (in -log10 units)
#define RANSAC_EPSILON_STEP 1               // step size of epsilon optimization (log10 units)
#define RANSAC_ELIMINATION_RATIO 60         // percentage of lines we try to eliminate as outliers
#define RANSAC_OPTIMIZATION_STEPS 5         // home many steps to optimize epsilon
#define RANSAC_OPTIMIZATION_DRY_RUNS 50     // how man runs per optimization steps
#define RANSAC_HURDLE 5                     // hurdle rate: the number of lines below which we do a complete permutation instead of random sampling
#define MINIMUM_FITLINES 4                  // minimum number of lines needed for automatic parameter fit
#define NMS_EPSILON 1e-3                    // break criterion for Nelder-Mead simplex
#define NMS_SCALE 1.0                       // scaling factor for Nelder-Mead simplex
#define NMS_ITERATIONS 400                  // number of iterations for Nelder-Mead simplex
#define NMS_CROP_EPSILON 100.0              // break criterion for Nelder-Mead simplex on crop fitting
#define NMS_CROP_SCALE 0.5                  // scaling factor for Nelder-Mead simplex on crop fitting
#define NMS_CROP_ITERATIONS 100             // number of iterations for Nelder-Mead simplex on crop fitting
#define NMS_ALPHA 1.0                       // reflection coefficient for Nelder-Mead simplex
#define NMS_BETA 0.5                        // contraction coefficient for Nelder-Mead simplex
#define NMS_GAMMA 2.0                       // expansion coefficient for Nelder-Mead simplex
#define DEFAULT_F_LENGTH 28.0               // focal length we assume if no exif data are available

// define to get debugging output
#undef ASHIFT_DEBUG

#define SQR(a) ((a) * (a))

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


DT_MODULE_INTROSPECTION(4, dt_iop_ashift_params_t)

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
  ASHIFT_FIT_SHEAR         = 1 << 3,  // flag indicates to fit shear parameter
  ASHIFT_FIT_LINES_VERT    = 1 << 4,  // use vertical lines for fitting
  ASHIFT_FIT_LINES_HOR     = 1 << 5,  // use horizontal lines for fitting
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
  ASHIFT_FIT_BOTH_SHEAR = ASHIFT_FIT_ROTATION | ASHIFT_FIT_LENS_VERT | ASHIFT_FIT_LENS_HOR |
                    ASHIFT_FIT_SHEAR | ASHIFT_FIT_LINES_VERT | ASHIFT_FIT_LINES_HOR,
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
  ASHIFT_ENHANCE_NONE       = 0,
  ASHIFT_ENHANCE_EDGES      = 1 << 0,
  ASHIFT_ENHANCE_DETAIL     = 1 << 1,
  ASHIFT_ENHANCE_HORIZONTAL = 0x100,
  ASHIFT_ENHANCE_VERTICAL   = 0x200
} dt_iop_ashift_enhance_t;

typedef enum dt_iop_ashift_mode_t
{
  ASHIFT_MODE_GENERIC = 0,
  ASHIFT_MODE_SPECIFIC = 1
} dt_iop_ashift_mode_t;

typedef enum dt_iop_ashift_crop_t
{
  ASHIFT_CROP_OFF = 0,
  ASHIFT_CROP_LARGEST = 1,
  ASHIFT_CROP_ASPECT = 2
} dt_iop_ashift_crop_t;

typedef enum dt_iop_ashift_bounding_t
{
  ASHIFT_BOUNDING_OFF = 0,
  ASHIFT_BOUNDING_SELECT = 1,
  ASHIFT_BOUNDING_DESELECT = 2
} dt_iop_ashift_bounding_t;

typedef enum dt_iop_ashift_jobcode_t
{
  ASHIFT_JOBCODE_NONE = 0,
  ASHIFT_JOBCODE_GET_STRUCTURE = 1,
  ASHIFT_JOBCODE_FIT = 2
} dt_iop_ashift_jobcode_t;

typedef struct dt_iop_ashift_params1_t
{
  float rotation;
  float lensshift_v;
  float lensshift_h;
  int toggle;
} dt_iop_ashift_params1_t;

typedef struct dt_iop_ashift_params2_t
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
} dt_iop_ashift_params2_t;

typedef struct dt_iop_ashift_params3_t
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
  dt_iop_ashift_crop_t cropmode;
  float cl;
  float cr;
  float ct;
  float cb;
} dt_iop_ashift_params3_t;

typedef struct dt_iop_ashift_params_t
{
  float rotation;
  float lensshift_v;
  float lensshift_h;
  float shear;
  float f_length;
  float crop_factor;
  float orthocorr;
  float aspect;
  dt_iop_ashift_mode_t mode;
  int toggle;
  dt_iop_ashift_crop_t cropmode;
  float cl;
  float cr;
  float ct;
  float cb;
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
  int bounded;
  dt_iop_ashift_linetype_t type;
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
  float shear;
  float rotation_range;
  float lensshift_v_range;
  float lensshift_h_range;
  float shear_range;
} dt_iop_ashift_fit_params_t;

typedef struct dt_iop_ashift_cropfit_params_t
{
  int width;
  int height;
  float x;
  float y;
  float alpha;
  float homograph[3][3];
  float edges[4][3];
} dt_iop_ashift_cropfit_params_t;

typedef struct dt_iop_ashift_gui_data_t
{
  GtkWidget *rotation;
  GtkWidget *lensshift_v;
  GtkWidget *lensshift_h;
  GtkWidget *shear;
  GtkWidget *guide_lines;
  GtkWidget *cropmode;
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
  dt_iop_ashift_bounding_t isbounding;
  int selecting_lines_version;
  float rotation_range;
  float lensshift_v_range;
  float lensshift_h_range;
  float shear_range;
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
  uint64_t lines_hash;
  uint64_t grid_hash;
  uint64_t buf_hash;
  dt_iop_ashift_fitaxis_t lastfit;
  float lastx;
  float lasty;
  dt_iop_ashift_jobcode_t jobcode;
  int jobparams;
  dt_pthread_mutex_t lock;
} dt_iop_ashift_gui_data_t;

typedef struct dt_iop_ashift_data_t
{
  float rotation;
  float lensshift_v;
  float lensshift_h;
  float shear;
  float f_length_kb;
  float orthocorr;
  float aspect;
  float cl;
  float cr;
  float ct;
  float cb;
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
  if(old_version == 1 && new_version == 4)
  {
    const dt_iop_ashift_params1_t *old = old_params;
    dt_iop_ashift_params_t *new = new_params;
    new->rotation = old->rotation;
    new->lensshift_v = old->lensshift_v;
    new->lensshift_h = old->lensshift_h;
    new->shear = 0.0f;
    new->toggle = old->toggle;
    new->f_length = DEFAULT_F_LENGTH;
    new->crop_factor = 1.0f;
    new->orthocorr = 100.0f;
    new->aspect = 1.0f;
    new->mode = ASHIFT_MODE_GENERIC;
    new->cropmode = ASHIFT_CROP_OFF;
    new->cl = 0.0f;
    new->cr = 1.0f;
    new->ct = 0.0f;
    new->cb = 1.0f;
    return 0;
  }
  if(old_version == 2 && new_version == 4)
  {
    const dt_iop_ashift_params2_t *old = old_params;
    dt_iop_ashift_params_t *new = new_params;
    new->rotation = old->rotation;
    new->lensshift_v = old->lensshift_v;
    new->lensshift_h = old->lensshift_h;
    new->shear = 0.0f;
    new->toggle = old->toggle;
    new->f_length = old->f_length;
    new->crop_factor = old->crop_factor;
    new->orthocorr = old->orthocorr;
    new->aspect = old->aspect;
    new->mode = old->mode;
    new->cropmode = ASHIFT_CROP_OFF;
    new->cl = 0.0f;
    new->cr = 1.0f;
    new->ct = 0.0f;
    new->cb = 1.0f;
    return 0;
  }
  if(old_version == 3 && new_version == 4)
  {
    const dt_iop_ashift_params3_t *old = old_params;
    dt_iop_ashift_params_t *new = new_params;
    new->rotation = old->rotation;
    new->lensshift_v = old->lensshift_v;
    new->lensshift_h = old->lensshift_h;
    new->shear = 0.0f;
    new->toggle = old->toggle;
    new->f_length = old->f_length;
    new->crop_factor = old->crop_factor;
    new->orthocorr = old->orthocorr;
    new->aspect = old->aspect;
    new->mode = old->mode;
    new->cropmode = old->cropmode;
    new->cl = old->cl;
    new->cr = old->cr;
    new->ct = old->ct;
    new->cb = old->cb;
    return 0;
  }

  return 1;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "rotation"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "lens shift (v)"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "lens shift (h)"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "shear"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "rotation", GTK_WIDGET(g->rotation));
  dt_accel_connect_slider_iop(self, "lens shift (v)", GTK_WIDGET(g->lensshift_v));
  dt_accel_connect_slider_iop(self, "lens shift (h)", GTK_WIDGET(g->lensshift_h));
  dt_accel_connect_slider_iop(self, "shear", GTK_WIDGET(g->shear));
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

// normalize a 3x1 vector so that x^2 + y^2 = 1; a useful normalization for
// lines in homogeneous coordinates
// dst and v may be the same
static inline void vec3lnorm(float *dst, const float *const v)
{
  const float sq = sqrt(v[0] * v[0] + v[1] * v[1]);

  // special handling for a point vector of the image center
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

#ifdef ASHIFT_DEBUG
static void print_roi(const dt_iop_roi_t *roi, const char *label)
{
  printf("{ %5d  %5d  %5d  %5d  %.6f } %s\n", roi->x, roi->y, roi->width, roi->height, roi->scale, label);
}
#endif

#define MAT3SWAP(a, b) { float (*tmp)[3] = (a); (a) = (b); (b) = tmp; }

static void homography(float *homograph, const float angle, const float shift_v, const float shift_h,
                       const float shear, const float f_length_kb, const float orthocorr, const float aspect,
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


  // Step 3: apply shearing
  memset(mwork, 0, 9 * sizeof(float));
  mwork[0][0] = 1.0f;
  mwork[0][1] = shear;
  mwork[1][1] = 1.0f;
  mwork[1][0] = shear;
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // Step 4: apply vertical lens shift effect
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


  // Step 5: horizontal compression
  memset(mwork, 0, 9 * sizeof(float));
  mwork[0][0] = 1.0f;
  mwork[1][1] = r_v;
  mwork[1][2] = 0.5f * u * (1.0f - r_v);
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // Step 6: flip x and y back again
  memset(mwork, 0, 9 * sizeof(float));
  mwork[0][1] = 1.0f;
  mwork[1][0] = 1.0f;
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // from here output vectors would be in (x : y : 1) format

  // Step 7: now we can apply horizontal lens shift with the same matrix format as above
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


  // Step 8: vertical compression
  memset(mwork, 0, 9 * sizeof(float));
  mwork[0][0] = 1.0f;
  mwork[1][1] = r_h;
  mwork[1][2] = 0.5f * v * (1.0f - r_h);
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // Step 9: apply aspect ratio scaling
  memset(mwork, 0, 9 * sizeof(float));
  mwork[0][0] = 1.0f * ascale;
  mwork[1][1] = 1.0f / ascale;
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // Step 10: find x/y offsets and apply according correction so that
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
      // moutput expects input in (x:y:1) format and gives output as (x:y:1)
      mat3mulv(po, (float *)moutput, pi);
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


// check if module parameters are set to all neutral values in which case the module's
// output is identical to its input
// TODO: we can ignore the clipping parameters here as long as only automatic clipping is
//       offered (clipping will have no effect if warping parameters are all zero).
//       This needs to be adapted once manual clipping options are added to this module.
static inline int isneutral(dt_iop_ashift_data_t *data)
{
  // values lower than this have no visible effect
  const float eps = 1.0e-4f;

  return(fabs(data->rotation) < eps &&
         fabs(data->lensshift_v) < eps &&
         fabs(data->lensshift_h) < eps &&
         fabs(data->shear) < eps);
}


int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;

  // nothing to be done if parameters are set to neutral values
  if(isneutral(data)) return 1;

  float homograph[3][3];
  homography((float *)homograph, data->rotation, data->lensshift_v, data->lensshift_h, data->shear, data->f_length_kb,
             data->orthocorr, data->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_FORWARD);

  // clipping offset
  const float fullwidth = (float)piece->buf_out.width / (data->cr - data->cl);
  const float fullheight = (float)piece->buf_out.height / (data->cb - data->ct);
  const float cx = fullwidth * data->cl;
  const float cy = fullheight * data->ct;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(points, points_count, homograph)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    float pi[3] = { points[i], points[i + 1], 1.0f };
    float po[3];
    mat3mulv(po, (float *)homograph, pi);
    points[i] = po[0] / po[2] - cx;
    points[i + 1] = po[1] / po[2] - cy;
  }

  return 1;
}


int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points,
                          size_t points_count)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;

  // nothing to be done if parameters are set to neutral values
  if(isneutral(data)) return 1;

  float ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h, data->shear, data->f_length_kb,
             data->orthocorr, data->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

  // clipping offset
  const float fullwidth = (float)piece->buf_out.width / (data->cr - data->cl);
  const float fullheight = (float)piece->buf_out.height / (data->cb - data->ct);
  const float cx = fullwidth * data->cl;
  const float cy = fullheight * data->ct;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(points, points_count, ihomograph)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    float pi[3] = { points[i] + cx, points[i + 1] + cy, 1.0f };
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

  // nothing more to be done if parameters are set to neutral values
  if(isneutral(data)) return;

  float homograph[3][3];
  homography((float *)homograph, data->rotation, data->lensshift_v, data->lensshift_h, data->shear, data->f_length_kb,
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

      // apply homograph
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
  float width = xM - xm + 1;
  float height = yM - ym + 1;

  // clipping adjustments
  width *= data->cr - data->cl;
  height *= data->cb - data->ct;

  roi_out->width = floorf(width);
  roi_out->height = floorf(height);

#ifdef ASHIFT_DEBUG
  print_roi(roi_in, "roi_in (going into modify_roi_out)");
  print_roi(roi_out, "roi_out (after modify_roi_out)");
#endif
}

void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;
  *roi_in = *roi_out;

  // nothing more to be done if parameters are set to neutral values
  if(isneutral(data)) return;

  float ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h, data->shear, data->f_length_kb,
             data->orthocorr, data->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;

  // clipping offset
  const float fullwidth = (float)piece->buf_out.width / (data->cr - data->cl);
  const float fullheight = (float)piece->buf_out.height / (data->cb - data->ct);
  const float cx = roi_out->scale * fullwidth * data->cl;
  const float cy = roi_out->scale * fullheight * data->ct;

  float xm = FLT_MAX, xM = -FLT_MAX, ym = FLT_MAX, yM = -FLT_MAX;

  // go through all four vertices of output roi and convert coordinates to input
  for(int y = 0; y < roi_out->height; y += roi_out->height - 1)
  {
    for(int x = 0; x < roi_out->width; x += roi_out->width - 1)
    {
      float pin[3], pout[3];

      // convert from output image coordinates to original image coordinates
      pout[0] = roi_out->x + x + cx;
      pout[1] = roi_out->y + y + cy;
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
#ifdef ASHIFT_DEBUG
  print_roi(roi_out, "roi_out (going into modify_roi_in)");
  print_roi(roi_in, "roi_in (after modify_roi_in)");
#endif
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

// XYZ -> sRGB matrix
static void XYZ_to_sRGB(const float *XYZ, float *sRGB)
{
  sRGB[0] =  3.1338561f * XYZ[0] - 1.6168667f * XYZ[1] - 0.4906146f * XYZ[2];
  sRGB[1] = -0.9787684f * XYZ[0] + 1.9161415f * XYZ[1] + 0.0334540f * XYZ[2];
  sRGB[2] =  0.0719453f * XYZ[0] - 0.2289914f * XYZ[1] + 1.4052427f * XYZ[2];
}

// sRGB -> XYZ matrix
static void sRGB_to_XYZ(const float *sRGB, float *XYZ)
{
  XYZ[0] = 0.4360747f * sRGB[0] + 0.3850649f * sRGB[1] + 0.1430804f * sRGB[2];
  XYZ[1] = 0.2225045f * sRGB[0] + 0.7168786f * sRGB[1] + 0.0606169f * sRGB[2];
  XYZ[2] = 0.0139322f * sRGB[0] + 0.0971045f * sRGB[1] + 0.7141733f * sRGB[2];
}

// detail enhancement via bilateral grid (function arguments in and out may represent identical buffers)
static int detail_enhance(const float *in, float *out, const int width, const int height)
{
  const float sigma_r = 5.0f;
  const float sigma_s = fminf(width, height) * 0.02f;
  const float detail = 10.0f;

  int success = TRUE;

  // we need to convert from RGB to Lab first;
  // as colors don't matter we are safe to assume data to be sRGB

  // convert RGB input to Lab, use output buffer for intermediate storage
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out)
#endif
  for(int j = 0; j < height; j++)
  {
    const float *inp = in + (size_t)4 * j * width;
    float *outp = out + (size_t)4 * j * width;
    for(int i = 0; i < width; i++, inp += 4, outp += 4)
    {
      float XYZ[3];
      sRGB_to_XYZ(inp, XYZ);
      dt_XYZ_to_Lab(XYZ, outp);
    }
  }

  // bilateral grid detail enhancement
  dt_bilateral_t *b = dt_bilateral_init(width, height, sigma_s, sigma_r);

  if(b != NULL)
  {
    dt_bilateral_splat(b, out);
    dt_bilateral_blur(b);
    dt_bilateral_slice_to_output(b, out, out, detail);
    dt_bilateral_free(b);
  }
  else
    success = FALSE;

  // convert resulting Lab to RGB output
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(out)
#endif
  for(int j = 0; j < height; j++)
  {
    float *outp = out + (size_t)4 * j * width;
    for(int i = 0; i < width; i++, outp += 4)
    {
      float XYZ[3];
      dt_Lab_to_XYZ(outp, XYZ);
      XYZ_to_sRGB(XYZ, outp);
    }
  }

  return success;
}

// apply gamma correction to RGB buffer (function arguments in and out may represent identical buffers)
static void gamma_correct(const float *in, float *out, const int width, const int height)
{
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out)
#endif
  for(int j = 0; j < height; j++)
  {
    const float *inp = in + (size_t)4 * j * width;
    float *outp = out + (size_t)4 * j * width;
    for(int i = 0; i < width; i++, inp += 4, outp += 4)
    {
      for(int c = 0; c < 3; c++)
        outp[c] = powf(inp[c], LSD_GAMMA);
    }
  }
}

// do actual line_detection based on LSD algorithm and return results according
// to this module's conventions
static int line_detect(float *in, const int width, const int height, const int x_off, const int y_off,
                       const float scale, dt_iop_ashift_line_t **alines, int *lcount, int *vcount, int *hcount,
                       float *vweight, float *hweight, dt_iop_ashift_enhance_t enhance, const int is_raw)
{
  double *greyscale = NULL;
  double *lsd_lines = NULL;
  dt_iop_ashift_line_t *ashift_lines = NULL;

  int vertical_count = 0;
  int horizontal_count = 0;
  float vertical_weight = 0.0f;
  float horizontal_weight = 0.0f;

  // apply gamma correction if image is raw
  if(is_raw)
  {
    gamma_correct(in, in, width, height);
  }

  // if requested perform an additional detail enhancement step
  if(enhance & ASHIFT_ENHANCE_DETAIL)
  {
    (void)detail_enhance(in, in, width, height);
  }

  // allocate intermediate buffers
  greyscale = malloc((size_t)width * height * sizeof(double));
  if(greyscale == NULL) goto error;

  // convert to greyscale image
  rgb2grey256(in, greyscale, width, height);

  // if requested perform an additional edge enhancement step
  if(enhance & ASHIFT_ENHANCE_EDGES)
  {
    (void)edge_enhance(greyscale, greyscale, width, height);
  }

  // call the line segment detector LSD;
  // LSD stores the number of found lines in lines_count.
  // it returns structural details as vector 'double lines[7 * lines_count]'
  int lines_count;
  lsd_lines = LineSegmentDetection(&lines_count, greyscale, width, height,
                                   LSD_SCALE, LSD_SIGMA_SCALE, LSD_QUANT,
                                   LSD_ANG_TH, LSD_LOG_EPS, LSD_DENSITY_TH,
                                   LSD_N_BINS, NULL, NULL, NULL);

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
      ashift_lines[lct].p2[2] = 1.0f;

      // calculate homogeneous coordinates of connecting line (defined by the two points)
      vec3prodn(ashift_lines[lct].L, ashift_lines[lct].p1, ashift_lines[lct].p2);

      // normalaze line coordinates so that x^2 + y^2 = 1
      // (this will always succeed as L is a real line connecting two real points)
      vec3lnorm(ashift_lines[lct].L, ashift_lines[lct].L);

      // length and width of rectangle (see LSD)
      ashift_lines[lct].length = sqrt((px2 - px1) * (px2 - px1) + (py2 - py1) * (py2 - py1));
      ashift_lines[lct].width = lsd_lines[n * 7 + 4] / scale;

      // ...  and weight (= length * width * angle precision)
      const float weight = ashift_lines[lct].length * ashift_lines[lct].width * lsd_lines[n * 7 + 5];
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
  free(lsd_lines);
  free(greyscale);
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
                  enhance, dt_image_is_raw(&module->dev->image_storage)))
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
// calculate the vantage point. Then we check the "distance" of all other lines to the
// vantage point. The model that gives highest number of lines combined with the highest
// total weight and lowest overall "distance" wins.
// Disadvantage: compared to the original RANSAC we don't get any model parameters that
// we could use for the following NMS fit.
// Self-tuning: we optimize "epsilon", the hurdle rate to reject a line as an outlier,
// by a number of dry runs first. The target average percentage value of lines to eliminate as
// outliers (without judging on the quality of the model) is given by RANSAC_ELIMINATION_RATIO,
// note: the actual percentage of outliers removed in the final run will be lower because we
// will finally look for the best quality model with the optimized epsilon and that quality value also
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

  // hurdle value epsilon for rejecting a line as an outlier will be self-tuning
  // in a number of dry runs
  float epsilon = pow(10.0f, -RANSAC_EPSILON);
  float epsilon_step = RANSAC_EPSILON_STEP;
  // some accounting variables for self-tuning
  int lines_eliminated = 0;
  int valid_runs = 0;

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

    // catch special cases:
    // a) L1 and L2 are identical -> V is NULL -> no valid vantage point
    // b) vantage point lies inside image frame (no chance to correct for this case)
    if(vec3isnull(V) ||
       (fabs(V[2]) > 0.0f &&
        V[0]/V[2] >= xmin &&
        V[1]/V[2] >= ymin &&
        V[0]/V[2] <= xmax &&
        V[1]/V[2] <= ymax))
    {
      // no valid model
      quality = 0.0f;
    }
    else
    {
      // valid model

      // normalize V so that x^2 + y^2 + z^2 = 1
      vec3norm(V, V);

      // the two lines constituting the model are part of the set
      inout[0] = 1;
      inout[1] = 1;

      // go through all remaining lines, check if they are within the model, and
      // mark that fact in inout[].
      // summarize a quality parameter for all lines within the model
      for(int n = 2; n < set_count; n++)
      {
        // L is normalized so that x^2 + y^2 = 1
        const float *L3 = lines[index_set[n]].L;

        // we take the absolute value of the dot product of V and L as a measure
        // of the "distance" between point and line. Note that this is not the real euclidian
        // distance but - with the given normalization - just a pragmatically selected number
        // that goes to zero if V lies on L and increases the more V and L are apart
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
      valid_runs++;
    }

    if(r < optiruns)
    {
      // on last run of each self-tuning step
      if((r % RANSAC_OPTIMIZATION_DRY_RUNS) == (RANSAC_OPTIMIZATION_DRY_RUNS - 1) && (valid_runs > 0))
      {
#ifdef ASHIFT_DEBUG
        printf("ransac self-tuning (run %d): epsilon %f", r, epsilon);
#endif
        // average ratio of lines that we eliminated with the given epsilon
        float ratio = 100.0f * (float)lines_eliminated / ((float)set_count * valid_runs);
        // adjust epsilon accordingly
        if(ratio < RANSAC_ELIMINATION_RATIO)
          epsilon = pow(10.0f, log10(epsilon) - epsilon_step);
        else if(ratio > RANSAC_ELIMINATION_RATIO)
          epsilon = pow(10.0f, log10(epsilon) + epsilon_step);
#ifdef ASHIFT_DEBUG
        printf(" (elimination ratio %f) -> %f\n", ratio, epsilon);
#endif
        // reduce step-size for next optimization round
        epsilon_step /= 2.0f;
        lines_eliminated = 0;
        valid_runs = 0;
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
    printf("ransac run %d: best qual %.6f, eps %.6f, line count %d of %d (this run: qual %.5f, count %d (%2f%%))\n", r,
           best_quality, epsilon, count, set_count, quality, lastcount, 100.0f * lastcount / (float)set_count);
#endif
  }

  // store back best set
  memcpy(index_set, best_set, set_count * sizeof(int));
  memcpy(inout_set, best_inout, set_count * sizeof(int));
}


// try to clean up structural data by eliminating outliers and thereby increasing
// the chance of a convergent fitting
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

  // some accounting variables
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
  // make sure p does not touch the borders of its definition area,
  // not critical for data accuracy as logit() is only used on initial fit parameters
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
//    * calculate scalar product s of line with perpendicular axis
//    * sum over weighted s^2 values
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
  float shear = fit->shear;
  float rotation_range = fit->rotation_range;
  float lensshift_v_range = fit->lensshift_v_range;
  float lensshift_h_range = fit->lensshift_h_range;
  float shear_range = fit->shear_range;

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

  if(isnan(shear))
  {
    shear = ilogit(params[pcount], -shear_range, shear_range);
    pcount++;
  }

  assert(pcount == fit->params_count);

  // the possible reference axes
  const float Av[3] = { 1.0f, 0.0f, 0.0f };
  const float Ah[3] = { 0.0f, 1.0f, 0.0f };

  // generate homograph out of the parameters
  float homograph[3][3];
  homography((float *)homograph, rotation, lensshift_v, lensshift_h, shear, f_length_kb,
             orthocorr, aspect, width, height, ASHIFT_HOMOGRAPH_FORWARD);

  // accounting variables
  double sumsq_v = 0.0;
  double sumsq_h = 0.0;
  double weight_v = 0.0;
  double weight_h = 0.0;
  int count_v = 0;
  int count_h = 0;
  int count = 0;

  // iterate over all lines
  for(int n = 0; n < lines_count; n++)
  {
    // check if this is a line which we must skip
    if((lines[n].type & fit->linemask) != fit->linetype)
      continue;

    // the direction of this line (vertical?)
    const int isvertical = lines[n].type & ASHIFT_LINE_DIRVERT;

    // select the perpendicular reference axis
    const float *A = isvertical ? Ah : Av;

    // apply homographic transformation to the end points
    float P1[3], P2[3];
    mat3mulv(P1, (float *)homograph, lines[n].p1);
    mat3mulv(P2, (float *)homograph, lines[n].p2);

    // get line connecting the two points
    float L[3];
    vec3prodn(L, P1, P2);

    // normalize L so that x^2 + y^2 = 1; makes sure that
    // y^2 = 1 / (1 + m^2) and x^2 = m^2 / (1 + m^2) with m defining the slope of the line
    vec3lnorm(L, L);

    // get scalar product of line L with orthogonal axis A -> gives 0 if line is perpendicular
    float s = vec3scalar(L, A);

    // sum up weighted s^2 for both directions individually
    sumsq_v += isvertical ? s * s * lines[n].weight : 0.0;
    weight_v  += isvertical ? lines[n].weight : 0.0;
    count_v += isvertical ? 1 : 0;
    sumsq_h += !isvertical ? s * s * lines[n].weight : 0.0;
    weight_h  += !isvertical ? lines[n].weight : 0.0;
    count_h += !isvertical ? 1 : 0;
    count++;
  }

  const double v = weight_v > 0.0f && count > 0 ? sumsq_v / weight_v * (float)count_v / count : 0.0;
  const double h = weight_h > 0.0f && count > 0 ? sumsq_h / weight_h * (float)count_h / count : 0.0;

  double sum = sqrt(1.0 - (1.0 - v) * (1.0 - h)) * 1.0e6;
  //double sum = sqrt(v + h) * 1.0e6;

#ifdef ASHIFT_DEBUG
  printf("fitness with rotation %f, lensshift_v %f, lensshift_h %f, shear %f -> lines %d, quality %10f\n",
         rotation, lensshift_v, lensshift_h, shear, count, sum);
#endif

  return sum;
}

// setup all data structures for fitting and call NM simplex
static dt_iop_ashift_nmsresult_t nmsfit(dt_iop_module_t *module, dt_iop_ashift_params_t *p, dt_iop_ashift_fitaxis_t dir)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  if(!g->lines) return NMS_NOT_ENOUGH_LINES;
  if(dir == ASHIFT_FIT_NONE) return NMS_SUCCESS;

  double params[4];
  int pcount = 0;
  int enough_lines = TRUE;

  // initialize fit parameters
  dt_iop_ashift_fit_params_t fit;
  fit.lines = g->lines;
  fit.lines_count = g->lines_count;
  fit.width = g->lines_in_width;
  fit.height = g->lines_in_height;
  fit.f_length_kb = (p->mode == ASHIFT_MODE_GENERIC) ? DEFAULT_F_LENGTH : p->f_length * p->crop_factor;
  fit.orthocorr = (p->mode == ASHIFT_MODE_GENERIC) ? 0.0f : p->orthocorr;
  fit.aspect = (p->mode == ASHIFT_MODE_GENERIC) ? 1.0f : p->aspect;
  fit.rotation = p->rotation;
  fit.lensshift_v = p->lensshift_v;
  fit.lensshift_h = p->lensshift_h;
  fit.shear = p->shear;
  fit.rotation_range = g->rotation_range;
  fit.lensshift_v_range = g->lensshift_v_range;
  fit.lensshift_h_range = g->lensshift_h_range;
  fit.shear_range = g->shear_range;
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

  if(mdir & ASHIFT_FIT_SHEAR)
  {
    // we fit the shear parameter
    fit.params_count++;
    params[pcount] = logit(fit.shear, -fit.shear_range, fit.shear_range);
    pcount++;
    fit.shear = NAN;
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
  p->shear = isnan(fit.shear) ? ilogit(params[pcount++], -fit.shear_range, fit.shear_range) : fit.shear;
#ifdef ASHIFT_DEBUG
  printf("params after optimization (%d interations): rotation %f, lensshift_v %f, lensshift_h %f, shear %f\n",
         iter, p->rotation, p->lensshift_v, p->lensshift_h, p->shear);
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

  double params[4];
  int enough_lines = TRUE;

  // initialize fit parameters
  dt_iop_ashift_fit_params_t fit;
  fit.lines = g->lines;
  fit.lines_count = g->lines_count;
  fit.width = g->lines_in_width;
  fit.height = g->lines_in_height;
  fit.f_length_kb = (p->mode == ASHIFT_MODE_GENERIC) ? DEFAULT_F_LENGTH : p->f_length * p->crop_factor;
  fit.orthocorr = (p->mode == ASHIFT_MODE_GENERIC) ? 0.0f : p->orthocorr;
  fit.aspect = (p->mode == ASHIFT_MODE_GENERIC) ? 1.0f : p->aspect;
  fit.rotation = p->rotation;
  fit.lensshift_v = p->lensshift_v;
  fit.lensshift_h = p->lensshift_h;
  fit.shear = p->shear;
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

  printf("model fitness: %.8f (rotation %f, lensshift_v %f, lensshift_h %f, shear %f)\n",
         quality, p->rotation, p->lensshift_v, p->lensshift_h, p->shear);
}
#endif

// function to keep crop fitting parameters within contraints
static void crop_constraint(double *params, int pcount)
{
  if(pcount > 0) params[0] = fabs(params[0]);
  if(pcount > 1) params[1] = fabs(params[1]);
  if(pcount > 2) params[2] = fabs(params[2]);

  if(pcount > 0 && params[0] > 1.0) params[0] = 1.0 - params[0];
  if(pcount > 1 && params[1] > 1.0) params[1] = 1.0 - params[1];
  if(pcount > 2 && params[2] > 0.5*M_PI) params[2] = 0.5*M_PI - params[2];
}

// helper function for getting the best fitting crop area;
// returns the negative area of the largest rectangle that fits within the
// defined image with a given rectangle's center and its aspect angle;
// the trick: the rectangle center coordinates are given in the input
// image coordinates so we know for sure that it also lies within the image after
// conversion to the output coordinates
static double crop_fitness(double *params, void *data)
{
  dt_iop_ashift_cropfit_params_t *cropfit = (dt_iop_ashift_cropfit_params_t *)data;

  const float wd = cropfit->width;
  const float ht = cropfit->height;

  // get variable and constant parameters, respectively
  const float x = isnan(cropfit->x) ? params[0] : cropfit->x;
  const float y = isnan(cropfit->y) ? params[1] : cropfit->y;
  const float alpha = isnan(cropfit->alpha) ? params[2] : cropfit->alpha;

  // the center of the rectangle in input image coordinates
  const float Pc[3] = { x * wd, y * ht, 1.0f };

  // convert to the output image coordinates and normalize
  float P[3];
  mat3mulv(P, (float *)cropfit->homograph, Pc);
  P[0] /= P[2];
  P[1] /= P[2];
  P[2] = 1.0f;

  // two auxiliary points (some arbitrary distance away from P) to contruct the diagonals
  const float Pa[2][3] = { { P[0] + 10.0f * cos(alpha), P[1] + 10.0f * sin(alpha), 1.0f },
                           { P[0] + 10.0f * cos(alpha), P[1] - 10.0f * sin(alpha), 1.0f } };

  // the two diagonals: D = P x Pa
  float D[2][3];
  vec3prodn(D[0], P, Pa[0]);
  vec3prodn(D[1], P, Pa[1]);

  // find all intersection points of all four edges with both diagonals (I = E x D);
  // the shortest distance d2min of the intersection point I to the crop area center P determines
  // the size of the crop area that still fits into the image (for the given center and aspect angle)
  float d2min = FLT_MAX;
  for(int k = 0; k < 4; k++)
    for(int l = 0; l < 2; l++)
    {
      // the intersection point
      float I[3];
      vec3prodn(I, cropfit->edges[k], D[l]);

      // special case: I is all null -> E and D are identical -> P lies on E -> d2min = 0
      if(vec3isnull(I))
      {
        d2min = 0.0f;
        break;
      }

      // special case: I[2] is 0.0f -> E and D are parallel and intersect at infinity -> no relevant point
      if(I[2] == 0.0f)
        continue;

      // the default case -> normlize I
      I[0] /= I[2];
      I[1] /= I[2];

      // calculate distance from I to P
      const float d2 = SQR(P[0] - I[0]) + SQR(P[1] - I[1]);

      // the minimum distance over all intersection points
      d2min = MIN(d2min, d2);
    }

  // calculate the area of the rectangle
  const float A = 2.0f * d2min * sin(2.0f * alpha);

#ifdef ASHIFT_DEBUG
  printf("crop fitness with x %f, y %f, angle %f -> distance %f, area %f\n",
         x, y, alpha, d2min, A);
#endif
  // and return -A to allow Nelder-Mead simplex to search for the minimum
  return -A;
}

// strategy: for a given center of the crop area and a specific aspect angle
// we calculate the largest crop area that still lies within the output image;
// now we allow a Nelder-Mead simplex to search for the center coordinates
// (and optionally the aspect angle) that delivers the largest overall crop area.
static void do_crop(dt_iop_module_t *module, dt_iop_ashift_params_t *p)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  // skip if fitting is still running
  if(g->fitting) return;

  // reset fit margins if auto-cropping is off
  if(p->cropmode == ASHIFT_CROP_OFF)
  {
    p->cl = 0.0f;
    p->cr = 1.0f;
    p->ct = 0.0f;
    p->cb = 1.0f;
    return;
  }

  g->fitting = 1;

  double params[3];
  int pcount;

  // get parameters for the homograph
  const float f_length_kb = (p->mode == ASHIFT_MODE_GENERIC) ? DEFAULT_F_LENGTH : p->f_length * p->crop_factor;
  const float orthocorr = (p->mode == ASHIFT_MODE_GENERIC) ? 0.0f : p->orthocorr;
  const float aspect = (p->mode == ASHIFT_MODE_GENERIC) ? 1.0f : p->aspect;
  const float rotation = p->rotation;
  const float lensshift_v = p->lensshift_v;
  const float lensshift_h = p->lensshift_h;
  const float shear = p->shear;

  // prepare structure of constant parameters
  dt_iop_ashift_cropfit_params_t cropfit;
  cropfit.width = g->buf_width;
  cropfit.height = g->buf_height;
  homography((float *)cropfit.homograph, rotation, lensshift_v, lensshift_h, shear, f_length_kb,
             orthocorr, aspect, cropfit.width, cropfit.height, ASHIFT_HOMOGRAPH_FORWARD);

  const float wd = cropfit.width;
  const float ht = cropfit.height;

  // the four vertices of the image in input image coordinates
  const float Vc[4][3] = { { 0.0f, 0.0f, 1.0f },
                           { 0.0f,   ht, 1.0f },
                           {   wd,   ht, 1.0f },
                           {   wd, 0.0f, 1.0f } };

  // convert the vertices to output image coordinates
  float V[4][3];
  for(int n = 0; n < 4; n++)
    mat3mulv(V[n], (float *)cropfit.homograph, Vc[n]);

  // get width and height of output image for later use
  float xmin = FLT_MAX, ymin = FLT_MAX, xmax = FLT_MIN, ymax = FLT_MIN;
  for(int n = 0; n < 4; n++)
  {
    // normalize V
    V[n][0] /= V[n][2];
    V[n][1] /= V[n][2];
    V[n][2] = 1.0f;
    xmin = MIN(xmin, V[n][0]);
    xmax = MAX(xmax, V[n][0]);
    ymin = MIN(ymin, V[n][1]);
    ymax = MAX(ymax, V[n][1]);
  }
  const float owd = xmax - xmin;
  const float oht = ymax - ymin;

  // calculate the lines defining the four edges of the image area: E = V[n] x V[n+1]
  for(int n = 0; n < 4; n++)
    vec3prodn(cropfit.edges[n], V[n], V[(n + 1) % 4]);

  // initial fit parameters: crop area is centered and aspect angle is that of the original image
  // number of parameters: fit only crop center coordinates with a fixed aspect ratio, or fit all three variables
  if(p->cropmode == ASHIFT_CROP_LARGEST)
  {
    params[0] = 0.5;
    params[1] = 0.5;
    params[2] = atan2((float)cropfit.height, (float)cropfit.width);
    cropfit.x = NAN;
    cropfit.y = NAN;
    cropfit.alpha = NAN;
    pcount = 3;
  }
  else //(p->cropmode == ASHIFT_CROP_ASPECT)
  {
    params[0] = 0.5;
    params[1] = 0.5;
    cropfit.x = NAN;
    cropfit.y = NAN;
    cropfit.alpha = atan2((float)cropfit.height, (float)cropfit.width);
    pcount = 2;
  }

  // start the simplex fit
  int iter = simplex(crop_fitness, params, pcount, NMS_CROP_EPSILON, NMS_CROP_SCALE, NMS_CROP_ITERATIONS,
                     crop_constraint, (void*)&cropfit);

  // in case the fit did not converge -> failed
  if(iter >= NMS_CROP_ITERATIONS) goto failed;

  // the fit did converge -> get clipping margins out of params:
  cropfit.x = isnan(cropfit.x) ? params[0] : cropfit.x;
  cropfit.y = isnan(cropfit.y) ? params[1] : cropfit.y;
  cropfit.alpha = isnan(cropfit.alpha) ? params[2] : cropfit.alpha;

  // the area of the best fitting rectangle
  const float A = fabs(crop_fitness(params, (void*)&cropfit));

  // unlikely to happen but we need to catch this case
  if(A == 0.0f) goto failed;

  // we need the half diagonal of that rectangle (this is in output image dimensions);
  // no need to check for division by zero here as this case implies A == 0.0f, catched above
  const float d = sqrt(A / (2.0f * sin(2.0f * cropfit.alpha)));

  // the rectangle's center in input image (homogeneous) coordinates
  const float Pc[3] = { cropfit.x * wd, cropfit.y * ht, 1.0f };

  // convert rectangle center to output image coordinates and normalize
  float P[3];
  mat3mulv(P, (float *)cropfit.homograph, Pc);
  P[0] /= P[2];
  P[1] /= P[2];

  // calculate clipping margins relative to output image dimensions
  p->cl = CLAMP((P[0] - d * cos(cropfit.alpha)) / owd, 0.0f, 1.0f);
  p->cr = CLAMP((P[0] + d * cos(cropfit.alpha)) / owd, 0.0f, 1.0f);
  p->ct = CLAMP((P[1] - d * sin(cropfit.alpha)) / oht, 0.0f, 1.0f);
  p->cb = CLAMP((P[1] + d * sin(cropfit.alpha)) / oht, 0.0f, 1.0f);

  // final sanity check
  if(p->cr - p->cl <= 0.0f || p->cb - p->ct <= 0.0f) goto failed;

  g->fitting = 0;

#ifdef ASHIFT_DEBUG
  printf("margins after crop fitting: iter %d, x %f, y %f, angle %f, crop area (%f %f %f %f), width %f, height %f\n",
         iter, cropfit.x, cropfit.y, cropfit.alpha, p->cl, p->cr, p->ct, p->cb, wd, ht);
#endif
  return;

failed:
  // in case of failure: reset clipping margins, set "automatic cropping" parameter
  // to "off" state, and display warning message
  p->cl = 0.0f;
  p->cr = 1.0f;
  p->ct = 0.0f;
  p->cb = 1.0f;
  p->cropmode = ASHIFT_CROP_OFF;
  dt_bauhaus_combobox_set(g->cropmode, p->cropmode);
  g->fitting = 0;
  dt_control_log(_("automatic cropping failed"));
  return;
}

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

  // finally apply cropping
  do_crop(module, p);

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

  // only for preview pipe: collect input buffer data and do some other evaluations
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
      memcpy(g->buf, ivoid, (size_t)width * height * ch * sizeof(float));

      g->buf_width = width;
      g->buf_height = height;
      g->buf_x_off = x_off;
      g->buf_y_off = y_off;
      g->buf_scale = scale;
      g->buf_hash = hash;
    }

    dt_pthread_mutex_unlock(&g->lock);
  }

  // if module is set to neutral parameters we just copy input->output and are done
  if(isneutral(data))
  {
    memcpy(ovoid, ivoid, (size_t)roi_out->width * roi_out->height * ch * sizeof(float));
    return;
  }

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

  float ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h, data->shear, data->f_length_kb,
             data->orthocorr, data->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

  // clipping offset
  const float fullwidth = (float)piece->buf_out.width / (data->cr - data->cl);
  const float fullheight = (float)piece->buf_out.height / (data->cb - data->ct);
  const float cx = roi_out->scale * fullwidth * data->cl;
  const float cy = roi_out->scale * fullheight * data->ct;


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
      pout[0] = roi_out->x + i + cx;
      pout[1] = roi_out->y + j + cy;
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

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_ashift_data_t *d = (dt_iop_ashift_data_t *)piece->data;
  dt_iop_ashift_global_data_t *gd = (dt_iop_ashift_global_data_t *)self->data;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  const int devid = piece->pipe->devid;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int width = roi_out->width;
  const int height = roi_out->height;

  cl_int err = -999;
  cl_mem dev_homo = NULL;

  // only for preview pipe: collect input buffer data and do some other evaluations
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

  // if module is set to neutral parameters we just copy input->output and are done
  if(isneutral(d))
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }

  float ihomograph[3][3];
  homography((float *)ihomograph, d->rotation, d->lensshift_v, d->lensshift_h, d->shear, d->f_length_kb,
             d->orthocorr, d->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

  // clipping offset
  const float fullwidth = (float)piece->buf_out.width / (d->cr - d->cl);
  const float fullheight = (float)piece->buf_out.height / (d->cb - d->ct);
  const float cx = roi_out->scale * fullwidth * d->cl;
  const float cy = roi_out->scale * fullheight * d->ct;

  dev_homo = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 9, ihomograph);
  if(dev_homo == NULL) goto error;

  const int iroi[2] = { roi_in->x, roi_in->y };
  const int oroi[2] = { roi_out->x, roi_out->y };
  const float in_scale = roi_in->scale;
  const float out_scale = roi_out->scale;
  const float clip[2] = { cx, cy };

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
  dt_opencl_set_kernel_arg(devid, ldkernel, 10, 2 * sizeof(float), (void *)clip);
  dt_opencl_set_kernel_arg(devid, ldkernel, 11, sizeof(cl_mem), (void *)&dev_homo);
  err = dt_opencl_enqueue_kernel_2d(devid, ldkernel, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_homo);
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

    // skip irrelevant lines
    if(points_idx[n].type == ASHIFT_LINE_IRRELEVANT)
      continue;

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

// mark lines which are inside a rectangular area in isbounding mode
static void get_bounded_inside(const float *points, dt_iop_ashift_points_idx_t *points_idx,
                               const int points_lines_count, float pzx, float pzy, float pzx2, float pzy2,
                               dt_iop_ashift_bounding_t mode)
{
  // get bounding box coordinates
  float ax = pzx;
  float ay = pzy;
  float bx = pzx2;
  float by = pzy2;
  if(pzx > pzx2)
  {
    ax = pzx2;
    bx = pzx;
  }
  if(pzy > pzy2)
  {
    ay = pzy2;
    by = pzy;
  }

  // we either look for the selected or the deselected lines
  dt_iop_ashift_linetype_t mask = ASHIFT_LINE_SELECTED;
  dt_iop_ashift_linetype_t state = (mode == ASHIFT_BOUNDING_DESELECT) ? ASHIFT_LINE_SELECTED : 0;

  for(int n = 0; n < points_lines_count; n++)
  {
    // mark line as "not near" and "not bounded"
    points_idx[n].near = 0;
    points_idx[n].bounded = 0;

    // skip irrelevant lines
    if(points_idx[n].type == ASHIFT_LINE_IRRELEVANT)
      continue;

    // is the line inside the box ?
    if(points_idx[n].bbx >= ax && points_idx[n].bbx <= bx && points_idx[n].bbX >= ax
       && points_idx[n].bbX <= bx && points_idx[n].bby >= ay && points_idx[n].bby <= by
       && points_idx[n].bbY >= ay && points_idx[n].bbY <= by)
    {
      points_idx[n].bounded = 1;
      // only mark "near"-ness of those lines we are interested in
      points_idx[n].near = ((points_idx[n].type & mask) != state) ? 0 : 1;
    }
  }
}

// generate hash value for lines taking into account only the end point coordinates
static uint64_t get_lines_hash(const dt_iop_ashift_line_t *lines, const int lines_count)
{
  uint64_t hash = 5381;
  for(int n = 0; n < lines_count; n++)
  {
    float v[4] = { lines[n].p1[0], lines[n].p1[1], lines[n].p2[0], lines[n].p2[1] };

    for(int i = 0; i < 4; i++)
      hash = ((hash << 5) + hash) ^ ((uint32_t *)v)[i];
  }
  return hash;
}

// update color information in points_idx if lines have changed in terms of type (but not in terms
// of number or position)
static int update_colors(struct dt_iop_module_t *self, dt_iop_ashift_points_idx_t *points_idx,
                         int points_lines_count)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  // is the display flipped relative to the original image?
  const int isflipped = g->isflipped;

  // go through all lines
  for(int n = 0; n < points_lines_count; n++)
  {
    const dt_iop_ashift_linetype_t type = points_idx[n].type;

    // set line color according to line type/orientation
    // note: if the screen display is flipped versus the original image we need
    // to respect that fact in the color selection
    if((type & ASHIFT_LINE_MASK) == ASHIFT_LINE_VERTICAL_SELECTED)
      points_idx[n].color = isflipped ? ASHIFT_LINECOLOR_BLUE : ASHIFT_LINECOLOR_GREEN;
    else if((type & ASHIFT_LINE_MASK) == ASHIFT_LINE_VERTICAL_NOT_SELECTED)
      points_idx[n].color = isflipped ? ASHIFT_LINECOLOR_YELLOW : ASHIFT_LINECOLOR_RED;
    else if((type & ASHIFT_LINE_MASK) == ASHIFT_LINE_HORIZONTAL_SELECTED)
      points_idx[n].color = isflipped ? ASHIFT_LINECOLOR_GREEN : ASHIFT_LINECOLOR_BLUE;
    else if((type & ASHIFT_LINE_MASK) == ASHIFT_LINE_HORIZONTAL_NOT_SELECTED)
      points_idx[n].color = isflipped ? ASHIFT_LINECOLOR_RED : ASHIFT_LINECOLOR_YELLOW;
    else
      points_idx[n].color = ASHIFT_LINECOLOR_GREY;
  }

  return TRUE;
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
    my_points_idx[n].bounded = 0;

    const dt_iop_ashift_linetype_t type = lines[n].type;
    my_points_idx[n].type = type;

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
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;

  // the usual rescaling stuff
  float wd = dev->preview_pipe->backbuf_width;
  float ht = dev->preview_pipe->backbuf_height;
  if(wd < 1.0 || ht < 1.0) return;
  float zoom_y = dt_control_get_dev_zoom_y();
  float zoom_x = dt_control_get_dev_zoom_x();
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

  // we draw the cropping area; we need x_off/y_off/width/height which is only availabe
  // after g->buf has been processed
  if(g->buf  && (p->cropmode != ASHIFT_CROP_OFF) && self->enabled)
  {
    // roi data of the preview pipe input buffer
    float iwd = g->buf_width;
    float iht = g->buf_height;
    float ixo = g->buf_x_off;
    float iyo = g->buf_y_off;

    // the four corners of the input buffer of this module
    const float V[4][2] = { { ixo,        iyo       },
                          {   ixo,        iyo + iht },
                          {   ixo + iwd,  iyo + iht },
                          {   ixo + iwd,  iyo       } };

    // convert coordinates of corners to coordinates of this module's output
    if(!dt_dev_distort_transform_plus(self->dev, self->dev->preview_pipe, self->priority, self->priority + 1,
      (float *)V, 4))
      return;

    // get x/y-offset as well as width and height of output buffer
    float xmin = FLT_MAX, ymin = FLT_MAX, xmax = FLT_MIN, ymax = FLT_MIN;
    for(int n = 0; n < 4; n++)
    {
      xmin = MIN(xmin, V[n][0]);
      xmax = MAX(xmax, V[n][0]);
      ymin = MIN(ymin, V[n][1]);
      ymax = MAX(ymax, V[n][1]);
    }
    const float owd = xmax - xmin;
    const float oht = ymax - ymin;

    // the four clipping corners
    const float C[4][2] = { { xmin + p->cl * owd, ymin + p->ct * oht },
                            { xmin + p->cl * owd, ymin + p->cb * oht },
                            { xmin + p->cr * owd, ymin + p->cb * oht },
                            { xmin + p->cr * owd, ymin + p->ct * oht } };

    // convert clipping corners to final output image
    if(!dt_dev_distort_transform_plus(self->dev, self->dev->preview_pipe, self->priority + 1, 9999999,
      (float *)C, 4))
      return;

    cairo_save(cr);

    double dashes = DT_PIXEL_APPLY_DPI(5.0) / zoom_scale;
    cairo_set_dash(cr, &dashes, 0, 0);

    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);

    // mask parts of image outside of clipping area in dark grey
    cairo_set_source_rgba(cr, .2, .2, .2, .8);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_translate(cr, width / 2.0, height / 2.0);
    cairo_scale(cr, zoom_scale, zoom_scale);
    cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);
    cairo_move_to(cr, C[0][0], C[0][1]);
    cairo_line_to(cr, C[1][0], C[1][1]);
    cairo_line_to(cr, C[2][0], C[2][1]);
    cairo_line_to(cr, C[3][0], C[3][1]);
    cairo_close_path(cr);
    cairo_fill(cr);

    // draw white outline around clipping area
    cairo_set_source_rgb(cr, .7, .7, .7);
    cairo_move_to(cr, C[0][0], C[0][1]);
    cairo_line_to(cr, C[1][0], C[1][1]);
    cairo_line_to(cr, C[2][0], C[2][1]);
    cairo_line_to(cr, C[3][0], C[3][1]);
    cairo_close_path(cr);
    cairo_stroke(cr);

    cairo_restore(cr);
  }

  // show guide lines on request
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

  // no structural data or visibility switched off? -> stop here
  if(g->lines == NULL || g->lines_suppressed || !gui_has_focus(self)) return;

  // get hash value that changes if distortions from here to the end of the pixelpipe changed
  uint64_t hash = dt_dev_hash_distort(dev);
  // get hash value that changes if coordinates of lines have changed
  uint64_t lines_hash = get_lines_hash(g->lines, g->lines_count);

  // points data are missing or outdated, or distortion has changed?
  if(g->points == NULL || g->points_idx == NULL || hash != g->grid_hash ||
    (g->lines_version > g->points_version && g->lines_hash != lines_hash))
  {
    // we need to reprocess points
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
    g->lines_hash = lines_hash;
  }
  else if(g->lines_hash == lines_hash)
  {
    // update line type information in points_idx
    for(int n = 0; n < g->points_lines_count; n++)
      g->points_idx[n].type = g->lines[n].type;

    // coordinates of lines are unchanged -> we only need to update colors
    if(!update_colors(self, g->points_idx, g->points_lines_count))
      return;

    g->points_version = g->lines_version;
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

  // and we draw the selection box if any
  if(g->isbounding != ASHIFT_BOUNDING_OFF)
  {
    float pzx, pzy;
    dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
    pzx += 0.5f;
    pzy += 0.5f;

    double dashed[] = { 4.0, 4.0 };
    dashed[0] /= zoom_scale;
    dashed[1] /= zoom_scale;
    int len = sizeof(dashed) / sizeof(dashed[0]);

    cairo_rectangle(cr, g->lastx * wd, g->lasty * ht, (pzx - g->lastx) * wd, (pzy - g->lasty) * ht);

    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_set_line_width(cr, 1.0 / zoom_scale);
    cairo_set_dash(cr, dashed, len, 0);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_set_dash(cr, dashed, len, 4);
    cairo_stroke(cr);
  }

  cairo_restore(cr);
}

// update the number of selected vertical and horizontal lines
static void update_lines_count(const dt_iop_ashift_line_t *lines, const int lines_count,
                        int *vertical_count, int *horizontal_count)
{
  int vlines = 0;
  int hlines = 0;

  for(int n = 0; n < lines_count; n++)
  {
    if((lines[n].type & ASHIFT_LINE_MASK) == ASHIFT_LINE_VERTICAL_SELECTED)
      vlines++;
    else if((lines[n].type & ASHIFT_LINE_MASK) == ASHIFT_LINE_HORIZONTAL_SELECTED)
      hlines++;
  }

  *vertical_count = vlines;
  *horizontal_count = hlines;
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

  // if in rectangle selecting mode adjust "near"-ness of lines according to
  // the rectangular selection
  if(g->isbounding != ASHIFT_BOUNDING_OFF)
  {
    float pzx, pzy;
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);

    pzx += 0.5f;
    pzy += 0.5f;

    float wd = self->dev->preview_pipe->backbuf_width;
    float ht = self->dev->preview_pipe->backbuf_height;

    if(wd >= 1.0 && ht >= 1.0)
    {
      // mark lines inside the rectangle
      get_bounded_inside(g->points, g->points_idx, g->points_lines_count, pzx * wd, pzy * ht, g->lastx * wd,
                         g->lasty * ht, g->isbounding);
    }

    dt_control_queue_redraw_center();
    return FALSE;
  }

  // gather information about "near"-ness in g->points_idx
  get_near(g->points, g->points_idx, g->points_lines_count, pzx * wd, pzy * ht, POINTS_NEAR_DELTA);

  // if we are in sweeping mode iterate over lines as we move the pointer and change "selected" state.
  if(g->isdeselecting || g->isselecting)
  {
    for(int n = 0; g->selecting_lines_version == g->lines_version && n < g->points_lines_count; n++)
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
  {
    update_lines_count(g->lines, g->lines_count, &g->vertical_count, &g->horizontal_count);
    g->lines_version++;
    g->selecting_lines_version++;
  }

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

  // remember lines version at this stage so we can continuously monitor if the
  // lines have changed in-between
  g->selecting_lines_version = g->lines_version;

  // if shift button is pressed go into bounding mode (selecting or deselecting
  // in a rectangle area)
  if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
  {
    float pzx, pzy;
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
    pzx += 0.5f;
    pzy += 0.5f;

    g->lastx = pzx;
    g->lasty = pzy;

    g->isbounding = (which == 3) ? ASHIFT_BOUNDING_DESELECT : ASHIFT_BOUNDING_SELECT;
    dt_control_change_cursor(GDK_CROSS);

    return TRUE;
  }

  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  const float min_scale = dt_dev_get_zoom_scale(self->dev, DT_ZOOM_FIT, closeup ? 2.0 : 1.0, 0);
  const float cur_scale = dt_dev_get_zoom_scale(self->dev, zoom, closeup ? 2.0 : 1.0, 0);

  // if we are zoomed out (no panning possible) and we have lines to display we take control
  int take_control = (cur_scale == min_scale) && (g->points_lines_count > 0);

  // iterate over all lines close to the pointer and change "selected" state.
  // left-click selects and right-click deselects the line
  for(int n = 0; g->selecting_lines_version == g->lines_version && n < g->points_lines_count; n++)
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
  {
    update_lines_count(g->lines, g->lines_count, &g->vertical_count, &g->horizontal_count);
    g->lines_version++;
    g->selecting_lines_version++;
  }

  return (take_control || handled);
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  // finalize the isbounding mode
  // if user has released the shift button in-between -> do nothing
  if(g->isbounding != ASHIFT_BOUNDING_OFF && (state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
  {
    int handled = 0;

    // we compute the rectangle selection
    float pzx, pzy;
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);

    pzx += 0.5f;
    pzy += 0.5f;

    float wd = self->dev->preview_pipe->backbuf_width;
    float ht = self->dev->preview_pipe->backbuf_height;

    if(wd >= 1.0 && ht >= 1.0)
    {
      // mark lines inside the rectangle
      get_bounded_inside(g->points, g->points_idx, g->points_lines_count, pzx * wd, pzy * ht, g->lastx * wd,
                         g->lasty * ht, g->isbounding);

      // select or deselect lines within the rectangle according to isbounding state
      for(int n = 0; g->selecting_lines_version == g->lines_version && n < g->points_lines_count; n++)
      {
        if(g->points_idx[n].bounded == 0) continue;

        if(g->isbounding == ASHIFT_BOUNDING_DESELECT)
          g->lines[n].type &= ~ASHIFT_LINE_SELECTED;
        else
          g->lines[n].type |= ASHIFT_LINE_SELECTED;

        handled = 1;
      }

      if(handled)
      {
        update_lines_count(g->lines, g->lines_count, &g->vertical_count, &g->horizontal_count);
        g->lines_version++;
        g->selecting_lines_version++;
      }

    dt_control_queue_redraw_center();
    }
  }

  // end of sweeping/isbounding mode
  dt_control_change_cursor(GDK_LEFT_PTR);
  g->isselecting = g->isdeselecting = 0;
  g->isbounding = ASHIFT_BOUNDING_OFF;
  g->lastx = g->lasty = -1;

  return 0;
}

static void rotation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->rotation = dt_bauhaus_slider_get(slider);
#ifdef ASHIFT_DEBUG
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  model_probe(self, p, g->lastfit);
#endif
  do_crop(self, p);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lensshift_v_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->lensshift_v = dt_bauhaus_slider_get(slider);
#ifdef ASHIFT_DEBUG
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  model_probe(self, p, g->lastfit);
#endif
  do_crop(self, p);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lensshift_h_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->lensshift_h = dt_bauhaus_slider_get(slider);
#ifdef ASHIFT_DEBUG
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  model_probe(self, p, g->lastfit);
#endif
  do_crop(self, p);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void shear_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->shear = dt_bauhaus_slider_get(slider);
#ifdef ASHIFT_DEBUG
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  model_probe(self, p, g->lastfit);
#endif
  do_crop(self, p);
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

static void cropmode_callback(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->cropmode = dt_bauhaus_combobox_get(widget);
  do_crop(self, p);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
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

  do_crop(self, p);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void f_length_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->f_length = dt_bauhaus_slider_get(slider);
  do_crop(self, p);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void crop_factor_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->crop_factor = dt_bauhaus_slider_get(slider);
  do_crop(self, p);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void orthocorr_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->orthocorr = dt_bauhaus_slider_get(slider);
  do_crop(self, p);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void aspect_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  p->aspect = dt_bauhaus_slider_get(slider);
  do_crop(self, p);
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
      g->lastfit = fitaxis = ASHIFT_FIT_ROTATION_VERTICAL_LINES;
    else if(shift)
      g->lastfit = fitaxis = ASHIFT_FIT_VERTICALLY_NO_ROTATION;
    else
      g->lastfit = fitaxis = ASHIFT_FIT_VERTICALLY;

    dt_iop_request_focus(self);
    dt_dev_reprocess_all(self->dev);

    if(self->enabled)
    {
      // module is enable -> we process directly
      if(do_fit(self, p, fitaxis))
      {
        darktable.gui->reset = 1;
        dt_bauhaus_slider_set_soft(g->rotation, p->rotation);
        dt_bauhaus_slider_set_soft(g->lensshift_v, p->lensshift_v);
        dt_bauhaus_slider_set_soft(g->lensshift_h, p->lensshift_h);
        dt_bauhaus_slider_set_soft(g->shear, p->shear);
        darktable.gui->reset = 0;
      }
    }
    else
    {
      // module is not enabled -> invoke it and queue the job to be processed once
      // the preview image is ready
      g->jobcode = ASHIFT_JOBCODE_FIT;
      g->jobparams = g->lastfit = fitaxis;
      p->toggle ^= 1;
    }

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
      g->lastfit = fitaxis = ASHIFT_FIT_ROTATION_HORIZONTAL_LINES;
    else if(shift)
      g->lastfit = fitaxis = ASHIFT_FIT_HORIZONTALLY_NO_ROTATION;
    else
      g->lastfit = fitaxis = ASHIFT_FIT_HORIZONTALLY;

    dt_iop_request_focus(self);
    dt_dev_reprocess_all(self->dev);

    if(self->enabled)
    {
      // module is enable -> we process directly
      if(do_fit(self, p, fitaxis))
      {
        darktable.gui->reset = 1;
        dt_bauhaus_slider_set_soft(g->rotation, p->rotation);
        dt_bauhaus_slider_set_soft(g->lensshift_v, p->lensshift_v);
        dt_bauhaus_slider_set_soft(g->lensshift_h, p->lensshift_h);
        dt_bauhaus_slider_set_soft(g->shear, p->shear);
        darktable.gui->reset = 0;
      }
    }
    else
    {
      // module is not enabled -> invoke it and queue the job to be processed once
      // the preview image is ready
      g->jobcode = ASHIFT_JOBCODE_FIT;
      g->jobparams = g->lastfit = fitaxis;
      p->toggle ^= 1;
    }

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

    if(control && shift)
      fitaxis = ASHIFT_FIT_BOTH;
    else if(control)
      fitaxis = ASHIFT_FIT_ROTATION_BOTH_LINES;
    else if(shift)
      fitaxis = ASHIFT_FIT_BOTH_NO_ROTATION;
    else
      fitaxis = ASHIFT_FIT_BOTH_SHEAR;

    dt_iop_request_focus(self);
    dt_dev_reprocess_all(self->dev);

    if(self->enabled)
    {
      // module is enable -> we process directly
      if(do_fit(self, p, fitaxis))
      {
        darktable.gui->reset = 1;
        dt_bauhaus_slider_set_soft(g->rotation, p->rotation);
        dt_bauhaus_slider_set_soft(g->lensshift_v, p->lensshift_v);
        dt_bauhaus_slider_set_soft(g->lensshift_h, p->lensshift_h);
        dt_bauhaus_slider_set_soft(g->shear, p->shear);
        darktable.gui->reset = 0;
      }
    }
    else
    {
      // module is not enabled -> invoke it and queue the job to be processed once
      // the preview image is ready
      g->jobcode = ASHIFT_JOBCODE_FIT;
      g->jobparams = g->lastfit = fitaxis;
      p->toggle ^= 1;
    }

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
    dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

    const int control = (event->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK;
    const int shift = (event->state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK;

    dt_iop_ashift_enhance_t enhance;

    if(control && shift)
      enhance = ASHIFT_ENHANCE_EDGES | ASHIFT_ENHANCE_DETAIL;
    else if(shift)
      enhance = ASHIFT_ENHANCE_DETAIL;
    else if(control)
      enhance = ASHIFT_ENHANCE_EDGES;
    else
      enhance = ASHIFT_ENHANCE_NONE;

    dt_iop_request_focus(self);
    dt_dev_reprocess_all(self->dev);

    if(self->enabled)
    {
      // module is enabled -> process directly
      (void)do_get_structure(self, p, enhance);
    }
    else
    {
      // module is not enabled -> invoke it and queue the job to be processed once
      // the preview image is ready
      g->jobcode = ASHIFT_JOBCODE_GET_STRUCTURE;
      g->jobparams = enhance;
      p->toggle ^= 1;
    }

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
  dt_control_queue_redraw_center();
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
  dt_control_queue_redraw_center();
}

// routine that is called after preview image has been processed. we use it
// to perform structure collection or fitting in case those have been triggered while
// the module had not yet been enabled
static void process_after_preview_callback(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  dt_iop_ashift_jobcode_t jobcode = g->jobcode;
  int jobparams = g->jobparams;

  // purge
  g->jobcode = ASHIFT_JOBCODE_NONE;
  g->jobparams = 0;

  if(darktable.gui->reset) return;

  switch(jobcode)
  {
    case ASHIFT_JOBCODE_GET_STRUCTURE:
      (void)do_get_structure(self, p, (dt_iop_ashift_enhance_t)jobparams);
      break;

    case ASHIFT_JOBCODE_FIT:
      if(do_fit(self, p, (dt_iop_ashift_fitaxis_t)jobparams))
      {
        darktable.gui->reset = 1;
        dt_bauhaus_slider_set_soft(g->rotation, p->rotation);
        dt_bauhaus_slider_set_soft(g->lensshift_v, p->lensshift_v);
        dt_bauhaus_slider_set_soft(g->lensshift_h, p->lensshift_h);
        dt_bauhaus_slider_set_soft(g->shear, p->shear);
        darktable.gui->reset = 0;
      }
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      break;
      
    case ASHIFT_JOBCODE_NONE:
    default:
      break;
  }

  dt_control_queue_redraw_center();
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)p1;
  dt_iop_ashift_data_t *d = (dt_iop_ashift_data_t *)piece->data;

  d->rotation = p->rotation;
  d->lensshift_v = p->lensshift_v;
  d->lensshift_h = p->lensshift_h;
  d->shear = p->shear;
  d->f_length_kb = (p->mode == ASHIFT_MODE_GENERIC) ? DEFAULT_F_LENGTH : p->f_length * p->crop_factor;
  d->orthocorr = (p->mode == ASHIFT_MODE_GENERIC) ? 0.0f : p->orthocorr;
  d->aspect = (p->mode == ASHIFT_MODE_GENERIC) ? 1.0f : p->aspect;

  if(gui_has_focus(self))
  {
    // if gui has focus we want to see the full uncropped image
    d->cl = 0.0f;
    d->cr = 1.0f;
    d->ct = 0.0f;
    d->cb = 1.0f;
  }
  else
  {
    d->cl = p->cl;
    d->cr = p->cr;
    d->ct = p->ct;
    d->cb = p->cb;
  }
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
  dt_bauhaus_slider_set_soft(g->shear, p->shear);
  dt_bauhaus_slider_set_soft(g->f_length, p->f_length);
  dt_bauhaus_slider_set_soft(g->crop_factor, p->crop_factor);
  dt_bauhaus_slider_set(g->orthocorr, p->orthocorr);
  dt_bauhaus_slider_set(g->aspect, p->aspect);
  dt_bauhaus_combobox_set(g->mode, p->mode);
  dt_bauhaus_combobox_set(g->guide_lines, g->show_guides);
  dt_bauhaus_combobox_set(g->cropmode, p->cropmode);
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
  module->priority = 215; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_ashift_params_t);
  module->gui_data = NULL;
  dt_iop_ashift_params_t tmp = (dt_iop_ashift_params_t){ 0.0f, 0.0f, 0.0f, 0.0f, DEFAULT_F_LENGTH, 1.0f, 100.0f, 1.0f, ASHIFT_MODE_GENERIC, 0,
                                                         ASHIFT_CROP_OFF, 0.0f, 1.0f, 0.0f, 1.0f };
  memcpy(module->params, &tmp, sizeof(dt_iop_ashift_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_ashift_params_t));
}

void reload_defaults(dt_iop_module_t *module)
{
  // our module is disabled by default
  module->default_enabled = 0;

  int isflipped = 0;
  float f_length = DEFAULT_F_LENGTH;
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
  dt_iop_ashift_params_t tmp = (dt_iop_ashift_params_t){ 0.0f, 0.0f, 0.0f, 0.0f, f_length, crop_factor, 100.0f, 1.0f, ASHIFT_MODE_GENERIC, 0,
                                                         ASHIFT_CROP_OFF, 0.0f, 1.0f, 0.0f, 1.0f };
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
    g->lines_hash = 0;
    g->rotation_range = ROTATION_RANGE_SOFT;
    g->lensshift_v_range = LENSSHIFT_RANGE_SOFT;
    g->lensshift_h_range = LENSSHIFT_RANGE_SOFT;
    g->shear_range = SHEAR_RANGE_SOFT;
    g->lines_suppressed = 0;
    g->lines_version = 0;
    g->show_guides = 0;
    g->isselecting = 0;
    g->isdeselecting = 0;
    g->isbounding = ASHIFT_BOUNDING_OFF;
    g->selecting_lines_version = 0;

    free(g->points);
    g->points = NULL;
    free(g->points_idx);
    g->points_idx = NULL;
    g->points_lines_count = 0;
    g->points_version = 0;

    g->jobcode = ASHIFT_JOBCODE_NONE;
    g->jobparams = 0;
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
    dt_dev_reprocess_all(self->dev);
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
  g->buf_x_off = 0;
  g->buf_y_off = 0;
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
  g->lines_hash = 0;
  g->rotation_range = ROTATION_RANGE_SOFT;
  g->lensshift_v_range = LENSSHIFT_RANGE_SOFT;
  g->lensshift_h_range = LENSSHIFT_RANGE_SOFT;
  g->shear_range = SHEAR_RANGE_SOFT;
  g->show_guides = 0;
  g->isselecting = 0;
  g->isdeselecting = 0;
  g->isbounding = ASHIFT_BOUNDING_OFF;
  g->selecting_lines_version = 0;

  g->jobcode = ASHIFT_JOBCODE_NONE;
  g->jobparams = 0;


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

  g->shear = dt_bauhaus_slider_new_with_range(self, -SHEAR_RANGE, SHEAR_RANGE, 0.01*SHEAR_RANGE, p->shear, 3);
  dt_bauhaus_widget_set_label(g->shear, NULL, _("shear"));
  dt_bauhaus_slider_enable_soft_boundaries(g->shear, -SHEAR_RANGE_SOFT, SHEAR_RANGE_SOFT);
  gtk_box_pack_start(GTK_BOX(self->widget), g->shear, TRUE, TRUE, 0);

  g->guide_lines = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->guide_lines, NULL, _("guides"));
  dt_bauhaus_combobox_add(g->guide_lines, _("off"));
  dt_bauhaus_combobox_add(g->guide_lines, _("on"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->guide_lines, TRUE, TRUE, 0);

  g->cropmode = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->cropmode, NULL, _("automatic cropping"));
  dt_bauhaus_combobox_add(g->cropmode, _("off"));
  dt_bauhaus_combobox_add(g->cropmode, _("largest area"));
  dt_bauhaus_combobox_add(g->cropmode, _("original format"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->cropmode, TRUE, TRUE, 0);

  g->mode = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->mode, NULL, _("lens model"));
  dt_bauhaus_combobox_add(g->mode, _("generic"));
  dt_bauhaus_combobox_add(g->mode, _("specific"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->mode, TRUE, TRUE, 0);

  g->f_length = dt_bauhaus_slider_new_with_range(self, 1.0f, 3.0f, 0.01f, 1.0f, 2);
  dt_bauhaus_widget_set_label(g->f_length, NULL, _("focal length"));
  dt_bauhaus_slider_set_callback(g->f_length, log10_callback);
  dt_bauhaus_slider_set_format(g->f_length, "%.0fmm");
  dt_bauhaus_slider_set_default(g->f_length, DEFAULT_F_LENGTH);
  dt_bauhaus_slider_set(g->f_length, DEFAULT_F_LENGTH);
  dt_bauhaus_slider_enable_soft_boundaries(g->f_length, 1.0f, 2000.0f);
  gtk_box_pack_start(GTK_BOX(self->widget), g->f_length, TRUE, TRUE, 0);

  g->crop_factor = dt_bauhaus_slider_new_with_range(self, 1.0f, 2.0f, 0.01f, p->crop_factor, 2);
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

  gtk_widget_set_tooltip_text(g->rotation, _("rotate image"));
  gtk_widget_set_tooltip_text(g->lensshift_v, _("apply lens shift correction in one direction"));
  gtk_widget_set_tooltip_text(g->lensshift_h, _("apply lens shift correction in one direction"));
  gtk_widget_set_tooltip_text(g->shear, _("shear the image along one diagonal"));
  gtk_widget_set_tooltip_text(g->guide_lines, _("display guide lines overlay"));
  gtk_widget_set_tooltip_text(g->cropmode, _("automatically crop to avoid black edges"));
  gtk_widget_set_tooltip_text(g->mode, _("lens model of the perspective correction: "
                                         "generic or according to the focal length"));
  gtk_widget_set_tooltip_text(g->f_length, _("focal length of the lens, "
                                             "default value set from exif data if available"));
  gtk_widget_set_tooltip_text(g->crop_factor, _("crop factor of the camera sensor, "
                                                "default value set from exif data if available, "
                                                "manual setting is often required"));
  gtk_widget_set_tooltip_text(g->orthocorr, _("the level of lens dependent correction, set to maximum for full lens dependency, "
                                              "set to zero for the generic case"));
  gtk_widget_set_tooltip_text(g->aspect, _("adjust aspect ratio of image by horizontal and vertical scaling"));
  gtk_widget_set_tooltip_text(g->fit_v, _("automatically correct for vertical perspective distortion\n"
                                          "ctrl-click to only fit rotation\n"
                                          "shift-click to only fit lens shift"));
  gtk_widget_set_tooltip_text(g->fit_h, _("automatically correct for horizontal perspective distortion\n"
                                          "ctrl-click to only fit rotation\n"
                                          "shift-click to only fit lens shift"));
  gtk_widget_set_tooltip_text(g->fit_both, _("automatically correct for vertical and "
                                             "horizontal perspective distortions; fitting rotation,"
                                             "lens shift in both directions, and shear\n"
                                             "ctrl-click to only fit rotation\n"
                                             "shift-click to only fit lens shift\n"
                                             "ctrl-shift-click to only fit rotation and lens shift"));
  gtk_widget_set_tooltip_text(g->structure, _("analyse line structure in image\n"
                                              "ctrl-click for an additional edge enhancement\n"
                                              "shift-click for an additional detail enhancement\n"
                                              "ctrl-shift-click for a combination of both methods"));
  gtk_widget_set_tooltip_text(g->clean, _("remove line structure information"));
  gtk_widget_set_tooltip_text(g->eye, _("toggle visibility of structure lines"));

  g_signal_connect(G_OBJECT(g->rotation), "value-changed", G_CALLBACK(rotation_callback), self);
  g_signal_connect(G_OBJECT(g->lensshift_v), "value-changed", G_CALLBACK(lensshift_v_callback), self);
  g_signal_connect(G_OBJECT(g->lensshift_h), "value-changed", G_CALLBACK(lensshift_h_callback), self);
  g_signal_connect(G_OBJECT(g->shear), "value-changed", G_CALLBACK(shear_callback), self);
  g_signal_connect(G_OBJECT(g->guide_lines), "value-changed", G_CALLBACK(guide_lines_callback), self);
  g_signal_connect(G_OBJECT(g->cropmode), "value-changed", G_CALLBACK(cropmode_callback), self);
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

  /* add signal handler for preview pipe finish to redraw the overlay */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(process_after_preview_callback), self);

}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(process_after_preview_callback), self);

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
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
