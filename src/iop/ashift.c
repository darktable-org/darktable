/*
  This file is part of darktable,
  Copyright (C) 2016-2022 darktable developers.

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
#include "common/bilateral.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/debug.h"
#include "common/imagebuf.h"
#include "common/interpolation.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/expander.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "libs/modulegroups.h"
#include "gui/guides.h"

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
#define ROTATION_RANGE_SOFT 180             // allowed min/max range for rotation parameter with manual adjustment
#define LENSSHIFT_RANGE 1.0                 // allowed min/max default range for lensshift parameters
#define LENSSHIFT_RANGE_SOFT 2.0            // allowed min/max range for lensshift parameters with manual adjustment
#define SHEAR_RANGE 0.2                     // allowed min/max range for shear parameter
#define SHEAR_RANGE_SOFT 0.5                // allowed min/max range for shear parameter with manual adjustment
#define MIN_LINE_LENGTH 5                   // the minimum length of a line in pixels to be regarded as relevant
#define MAX_TANGENTIAL_DEVIATION 30         // by how many degrees a line may deviate from the +/-180 and +/-90 to be regarded as relevant
#define LSD_SCALE 0.99                      // LSD: scaling factor for line detection
#define LSD_SIGMA_SCALE 0.6                 // LSD: sigma for Gaussian filter is computed as sigma = sigma_scale/scale
#define LSD_QUANT 2.0                       // LSD: bound to the quantization error on the gradient norm
#define LSD_ANG_TH 22.5                     // LSD: gradient angle tolerance in degrees
#define LSD_LOG_EPS 0.0                     // LSD: detection threshold: -log10(NFA) > log_eps
#define LSD_DENSITY_TH 0.7                  // LSD: minimal density of region points in rectangle
#define LSD_N_BINS 1024                     // LSD: number of bins in pseudo-ordering of gradient modulus
#define LSD_GAMMA 0.45                      // gamma correction to apply on raw images prior to line detection
#define RANSAC_RUNS 400                     // how many iterations to run in ransac
#define RANSAC_EPSILON 2                    // starting value for ransac epsilon (in -log10 units)
#define RANSAC_EPSILON_STEP 1               // step size of epsilon optimization (log10 units)
#define RANSAC_ELIMINATION_RATIO 60         // percentage of lines we try to eliminate as outliers
#define RANSAC_OPTIMIZATION_STEPS 5         // home many steps to optimize epsilon
#define RANSAC_OPTIMIZATION_DRY_RUNS 50     // how man runs per optimization steps
#define RANSAC_HURDLE 5                     // hurdle rate: the number of lines below which we do a complete permutation instead of random sampling
#define MINIMUM_FITLINES 2                  // minimum number of lines needed for automatic parameter fit
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

// maximum number of drawn lines that can be saved in parameters
// any change in this value needs to upgrade parameters version !
#define MAX_SAVED_LINES 50

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


DT_MODULE_INTROSPECTION(5, dt_iop_ashift_params_t)


const char *name()
{
  return _("rotate and perspective");
}

const char *aliases()
{
  return _("rotation|keystone|distortion|crop|reframe");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("rotate or distort perspective"),
                                      _("corrective or creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("geometric, RGB"),
                                      _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_ALLOW_FAST_PIPE
         | IOP_FLAGS_GUIDES_SPECIAL_DRAW | IOP_FLAGS_GUIDES_WIDGET;
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
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

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

typedef enum dt_iop_ashift_method_t
{
  ASHIFT_METHOD_NONE = 0,
  ASHIFT_METHOD_AUTO,
  ASHIFT_METHOD_QUAD,
  ASHIFT_METHOD_LINES
} dt_iop_ashift_method_t;

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
  NMS_DID_NOT_CONVERGE = 2,
  NMS_INSANE = 3
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
  ASHIFT_MODE_GENERIC = 0, // $DESCRIPTION: "generic"
  ASHIFT_MODE_SPECIFIC = 1 // $DESCRIPTION: "specific"
} dt_iop_ashift_mode_t;

typedef enum dt_iop_ashift_crop_t
{
  ASHIFT_CROP_OFF = 0,    // $DESCRIPTION: "off"
  ASHIFT_CROP_LARGEST = 1,// $DESCRIPTION: "largest area"
  ASHIFT_CROP_ASPECT = 2  // $DESCRIPTION: "original format"
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
  ASHIFT_JOBCODE_FIT = 2,
  ASHIFT_JOBCODE_GET_STRUCTURE_LINES = 3,
  ASHIFT_JOBCODE_GET_STRUCTURE_QUAD = 4,
  ASHIFT_JOBCODE_DO_CROP = 5
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

typedef struct dt_iop_ashift_params4_t
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
} dt_iop_ashift_params4_t;

typedef struct dt_iop_ashift_params_t
{
  float rotation;    // $MIN: -ROTATION_RANGE_SOFT $MAX: ROTATION_RANGE_SOFT $DEFAULT: 0.0
  float lensshift_v; // $MIN: -LENSSHIFT_RANGE_SOFT $MAX: LENSSHIFT_RANGE_SOFT $DEFAULT: 0.0 $DESCRIPTION: "lens shift (vertical)"
  float lensshift_h; // $MIN: -LENSSHIFT_RANGE_SOFT $MAX: LENSSHIFT_RANGE_SOFT $DEFAULT: 0.0 $DESCRIPTION: "lens shift (horizontal)"
  float shear;       // $MIN: -SHEAR_RANGE_SOFT $MAX: SHEAR_RANGE_SOFT $DEFAULT: 0.0 $DESCRIPTION: "shear"
  float f_length;    // $MIN: 1.0 $MAX: 2000.0 $DEFAULT: DEFAULT_F_LENGTH $DESCRIPTION: "focal length"
  float crop_factor; // $MIN: 0.5 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "crop factor"
  float orthocorr;   // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "lens dependence"
  float aspect;      // $MIN: 0.5 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "aspect adjust"
  dt_iop_ashift_mode_t mode;     // $DEFAULT: ASHIFT_MODE_GENERIC $DESCRIPTION: "lens model"
  dt_iop_ashift_crop_t cropmode; // $DEFAULT: ASHIFT_CROP_LARGEST $DESCRIPTION: "automatic cropping"
  float cl;          // $DEFAULT: 0.0
  float cr;          // $DEFAULT: 1.0
  float ct;          // $DEFAULT: 0.0
  float cb;          // $DEFAULT: 1.0
  float last_drawn_lines[MAX_SAVED_LINES * 4];
  int last_drawn_lines_count;
  float last_quad_lines[8];
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
  GtkWidget *cropmode;
  GtkWidget *mode;
  GtkWidget *specifics;
  GtkWidget *f_length;
  GtkWidget *crop_factor;
  GtkWidget *orthocorr;
  GtkWidget *aspect;
  GtkWidget *fit_v;
  GtkWidget *fit_h;
  GtkWidget *fit_both;
  GtkWidget *structure_auto;
  GtkWidget *structure_quad;
  GtkWidget *structure_lines;
  gboolean straightening;
  float straighten_x;
  float straighten_y;
  int fitting;
  int isflipped;
  int isselecting;
  int isdeselecting;
  dt_iop_ashift_bounding_t isbounding;
  float near_delta;
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
  float crop_cx;
  float crop_cy;
  dt_iop_ashift_jobcode_t jobcode;
  int jobparams;
  gboolean adjust_crop;
  float cl;	// shadow copy of dt_iop_ashift_data_t.cl
  float cr;	// shadow copy of dt_iop_ashift_data_t.cr
  float ct;	// shadow copy of dt_iop_ashift_data_t.ct
  float cb;	// shadow copy of dt_iop_ashift_data_t.cb

  dt_iop_ashift_method_t current_structure_method;
  int draw_near_point;
  gboolean draw_point_move;
  int draw_line_move;
  float draw_pointmove_x;
  float draw_pointmove_y;
  float *draw_points;
  dt_gui_collapsible_section_t cs;
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
  if(old_version == 1 && new_version == 5)
  {
    const dt_iop_ashift_params1_t *old = old_params;
    dt_iop_ashift_params_t *new = new_params;
    new->rotation = old->rotation;
    new->lensshift_v = old->lensshift_v;
    new->lensshift_h = old->lensshift_h;
    new->shear = 0.0f;
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
    for(int i = 0; i < MAX_SAVED_LINES * 4; i++) new->last_drawn_lines[i] = 0.0f;
    for(int i = 0; i < 8; i++) new->last_quad_lines[i] = 0.0f;
    new->last_drawn_lines_count = 0;
    return 0;
  }
  if(old_version == 2 && new_version == 5)
  {
    const dt_iop_ashift_params2_t *old = old_params;
    dt_iop_ashift_params_t *new = new_params;
    new->rotation = old->rotation;
    new->lensshift_v = old->lensshift_v;
    new->lensshift_h = old->lensshift_h;
    new->shear = 0.0f;
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
    for(int i = 0; i < MAX_SAVED_LINES * 4; i++) new->last_drawn_lines[i] = 0.0f;
    for(int i = 0; i < 8; i++) new->last_quad_lines[i] = 0.0f;
    new->last_drawn_lines_count = 0;
    return 0;
  }
  if(old_version == 3 && new_version == 5)
  {
    const dt_iop_ashift_params3_t *old = old_params;
    dt_iop_ashift_params_t *new = new_params;
    new->rotation = old->rotation;
    new->lensshift_v = old->lensshift_v;
    new->lensshift_h = old->lensshift_h;
    new->shear = 0.0f;
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
    for(int i = 0; i < MAX_SAVED_LINES * 4; i++) new->last_drawn_lines[i] = 0.0f;
    for(int i = 0; i < 8; i++) new->last_quad_lines[i] = 0.0f;
    new->last_drawn_lines_count = 0;
    return 0;
  }
  if(old_version == 4 && new_version == 5)
  {
    const dt_iop_ashift_params4_t *old = old_params;
    dt_iop_ashift_params_t *new = new_params;
    new->rotation = old->rotation;
    new->lensshift_v = old->lensshift_v;
    new->lensshift_h = old->lensshift_h;
    new->shear = old->shear;
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
    for(int i = 0; i < MAX_SAVED_LINES * 4; i++) new->last_drawn_lines[i] = 0.0f;
    for(int i = 0; i < 8; i++) new->last_quad_lines[i] = 0.0f;
    new->last_drawn_lines_count = 0;
    return 0;
  }

  return 1;
}

// normalized product of two 3x1 vectors
// dst needs to be different from v1 and v2
static inline void vec3prodn(float *dst, const float *const v1, const float *const v2)
{
  const float l1 = v1[1] * v2[2] - v1[2] * v2[1];
  const float l2 = v1[2] * v2[0] - v1[0] * v2[2];
  const float l3 = v1[0] * v2[1] - v1[1] * v2[0];

  // normalize so that l1^2 + l2^2 + l3^3 = 1
  const float sq = sqrtf(l1 * l1 + l2 * l2 + l3 * l3);

  const float f = sq > 0.0f ? 1.0f / sq : 1.0f;

  dst[0] = l1 * f;
  dst[1] = l2 * f;
  dst[2] = l3 * f;
}

// normalize a 3x1 vector so that x^2 + y^2 + z^2 = 1
// dst and v may be the same
static inline void vec3norm(float *dst, const float *const v)
{
  const float sq = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

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
  const float sq = sqrtf(v[0] * v[0] + v[1] * v[1]);

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
  return (fabsf(v[0]) < eps && fabsf(v[1]) < eps && fabsf(v[2]) < eps);
}

#ifdef ASHIFT_DEBUG
static void print_roi(const dt_iop_roi_t *roi, const char *label)
{
  printf("{ %5d  %5d  %5d  %5d  %.6f } %s\n", roi->x, roi->y, roi->width, roi->height, roi->scale, label);
}
#endif

static inline void _shadow_crop_box(dt_iop_ashift_params_t *p, dt_iop_ashift_gui_data_t *g)
{
  // copy actual crop box values into shadow variables
  g->cl = p->cl;
  g->cr = p->cr;
  g->ct = p->ct;
  g->cb = p->cb;
}

static void _clear_shadow_crop_box(dt_iop_ashift_gui_data_t *g)
{
  // reset the crop to the full image
  g->cl = 0.0f;
  g->cr = 1.0f;
  g->ct = 0.0f;
  g->cb = 1.0f;
}

static inline void _commit_crop_box(dt_iop_ashift_params_t *p, dt_iop_ashift_gui_data_t *g)
{
  // copy shadow values for crop box into actual parameters
  p->cl = g->cl;
  p->cr = g->cr;
  p->ct = g->ct;
  p->cb = g->cb;
}

static inline void _swap_shadow_crop_box(dt_iop_ashift_params_t *p, dt_iop_ashift_gui_data_t *g)
{
  // exchange shadow values and actual crop values
  // this is needed for a temporary commit to be able to properly update the undo history
  float tmp;
  tmp = p->cl; p->cl = g->cl; g->cl = tmp;
  tmp = p->cr; p->cr = g->cr; g->cr = tmp;
  tmp = p->ct; p->ct = g->ct; g->ct = tmp;
  tmp = p->cb; p->cb = g->cb; g->cb = tmp;
}

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
  const float cosi = cosf(phi);
  const float sini = sinf(phi);
  const float ascale = sqrtf(aspect);

  // most of this comes from ShiftN
  const float f_global = f_length_kb;
  const float horifac = 1.0f - orthocorr / 100.0f;
  const float exppa_v = expf(shift_v);
  const float fdb_v = f_global / (14.4f + (v / u - 1) * 7.2f);
  const float rad_v = fdb_v * (exppa_v - 1.0f) / (exppa_v + 1.0f);
  const float alpha_v = CLAMP(atanf(rad_v), -1.5f, 1.5f);
  const float rt_v = sinf(0.5f * alpha_v);
  const float r_v = fmaxf(0.1f, 2.0f * (horifac - 1.0f) * rt_v * rt_v + 1.0f);

  const float vertifac = 1.0f - orthocorr / 100.0f;
  const float exppa_h = expf(shift_h);
  const float fdb_h = f_global / (14.4f + (u / v - 1) * 7.2f);
  const float rad_h = fdb_h * (exppa_h - 1.0f) / (exppa_h + 1.0f);
  const float alpha_h = CLAMP(atanf(rad_h), -1.5f, 1.5f);
  const float rt_h = sinf(0.5f * alpha_h);
  const float r_h = fmaxf(0.1f, 2.0f * (vertifac - 1.0f) * rt_h * rt_h + 1.0f);


  // three intermediate buffers for matrix calculation ...
  float m1[3][3], m2[3][3], m3[3][3];

  // ... and some pointers to handle them more intuitively
  float (*mwork)[3] = m1;
  float (*minput)[3] = m2;
  float (*moutput)[3] = m3;

  // Step 1: flip x and y coordinates (see above)
  memset(minput, 0, sizeof(float) * 9);
  minput[0][1] = 1.0f;
  minput[1][0] = 1.0f;
  minput[2][2] = 1.0f;


  // Step 2: rotation of image around its center
  memset(mwork, 0, sizeof(float) * 9);
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
  memset(mwork, 0, sizeof(float) * 9);
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
  memset(mwork, 0, sizeof(float) * 9);
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
  memset(mwork, 0, sizeof(float) * 9);
  mwork[0][0] = 1.0f;
  mwork[1][1] = r_v;
  mwork[1][2] = 0.5f * u * (1.0f - r_v);
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // Step 6: flip x and y back again
  memset(mwork, 0, sizeof(float) * 9);
  mwork[0][1] = 1.0f;
  mwork[1][0] = 1.0f;
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // from here output vectors would be in (x : y : 1) format

  // Step 7: now we can apply horizontal lens shift with the same matrix format as above
  memset(mwork, 0, sizeof(float) * 9);
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
  memset(mwork, 0, sizeof(float) * 9);
  mwork[0][0] = 1.0f;
  mwork[1][1] = r_h;
  mwork[1][2] = 0.5f * v * (1.0f - r_h);
  mwork[2][2] = 1.0f;

  // moutput (of last calculation) -> minput
  MAT3SWAP(minput, moutput);
  // multiply mwork * minput -> moutput
  mat3mul((float *)moutput, (float *)mwork, (float *)minput);


  // Step 9: apply aspect ratio scaling
  memset(mwork, 0, sizeof(float) * 9);
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

  memset(mwork, 0, sizeof(float) * 9);
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
    memcpy(homograph, moutput, sizeof(float) * 9);
  }
  else
  {
    // generate inverted homograph (mat3inv function defined in colorspaces.c)
    if(mat3inv((float *)homograph, (float *)moutput))
    {
      // in case of error we set to unity matrix
      memset(mwork, 0, sizeof(float) * 9);
      mwork[0][0] = 1.0f;
      mwork[1][1] = 1.0f;
      mwork[2][2] = 1.0f;
      memcpy(homograph, mwork, sizeof(float) * 9);
    }
  }
}
#undef MAT3SWAP


// check if module parameters are set to all neutral values in which case the module's
// output is identical to its input
static inline int isneutral(const dt_iop_ashift_data_t *data)
{
  // values lower than this have no visible effect
  const float eps = 1.0e-4f;

  return(fabs(data->rotation) < eps &&
         fabs(data->lensshift_v) < eps &&
         fabs(data->lensshift_h) < eps &&
         fabs(data->shear) < eps &&
         fabs(data->aspect - 1.0f) < eps &&
         data->cl < eps &&
         1.0f - data->cr < eps &&
         data->ct < eps &&
         1.0f - data->cb < eps);
}


int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points, size_t points_count)
{
  const dt_iop_ashift_data_t *const data = (dt_iop_ashift_data_t *)piece->data;

  // nothing to be done if parameters are set to neutral values
  if(isneutral(data)) return 1;

  float DT_ALIGNED_ARRAY homograph[3][3];
  homography((float *)homograph, data->rotation, data->lensshift_v, data->lensshift_h, data->shear, data->f_length_kb,
             data->orthocorr, data->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_FORWARD);

  // clipping offset
  const float fullwidth = (float)piece->buf_out.width / (data->cr - data->cl);
  const float fullheight = (float)piece->buf_out.height / (data->cb - data->ct);
  const float cx = fullwidth * data->cl;
  const float cy = fullheight * data->ct;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(cx, cy, points_count, points, homograph) \
  schedule(static) if(points_count > 100) aligned(points, homograph:64)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    float DT_ALIGNED_PIXEL pi[3] = { points[i], points[i + 1], 1.0f };
    float DT_ALIGNED_PIXEL po[3];
    mat3mulv(po, (float *)homograph, pi);
    points[i] = po[0] / po[2] - cx;
    points[i + 1] = po[1] / po[2] - cy;
  }

  return 1;
}


int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points,
                          size_t points_count)
{
  const dt_iop_ashift_data_t *const data = (dt_iop_ashift_data_t *)piece->data;

  // nothing to be done if parameters are set to neutral values
  if(isneutral(data)) return 1;

  float DT_ALIGNED_ARRAY ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h, data->shear, data->f_length_kb,
             data->orthocorr, data->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

  // clipping offset
  const float fullwidth = (float)piece->buf_out.width / (data->cr - data->cl);
  const float fullheight = (float)piece->buf_out.height / (data->cb - data->ct);
  const float cx = fullwidth * data->cl;
  const float cy = fullheight * data->ct;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(cx, cy, points, points_count, ihomograph) \
  schedule(static) if(points_count > 100) aligned(ihomograph, points:64)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    float DT_ALIGNED_PIXEL pi[3] = { points[i] + cx, points[i + 1] + cy, 1.0f };
    float DT_ALIGNED_PIXEL po[3];
    mat3mulv(po, (float *)ihomograph, pi);
    points[i] = po[0] / po[2];
    points[i + 1] = po[1] / po[2];
  }

  return 1;
}

void distort_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                  float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_ashift_data_t *const data = (dt_iop_ashift_data_t *)piece->data;

  // if module is set to neutral parameters we just copy input->output and are done
  if(isneutral(data))
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);
    return;
  }

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  float ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h, data->shear, data->f_length_kb,
             data->orthocorr, data->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

  // clipping offset
  const float fullwidth = (float)piece->buf_out.width / (data->cr - data->cl);
  const float fullheight = (float)piece->buf_out.height / (data->cb - data->ct);
  const float cx = roi_out->scale * fullwidth * data->cl;
  const float cy = roi_out->scale * fullheight * data->ct;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(cx, cy, in, out, roi_in, roi_out) \
  shared(ihomograph, interpolation) \
  schedule(static)
#endif
  // go over all pixels of output image
  for(int j = 0; j < roi_out->height; j++)
  {
    float *const restrict _out = out + (size_t)j * roi_out->width;
    for(int i = 0; i < roi_out->width; i++)
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
      dt_interpolation_compute_pixel1c(interpolation, in, _out + i, pin[0], pin[1], roi_in->width,
                                       roi_in->height, roi_in->width);
    }
  }
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

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
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
static void rgb2grey256(const float *const in, double *const out, const int width, const int height)
{
  const size_t npixels = (size_t)width * height;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(int index = 0; index < npixels; index++)
  {
    out[index] = (0.3f * in[4*index+0] + 0.59f * in[4*index+1] + 0.11f * in[4*index+2]) * 256.0;
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
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, width, khwidth, kwidth) \
  shared(in, out, kernel) \
  schedule(static)
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
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, width, khwidth) \
  shared(out) \
  schedule(static)
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

  Gx = malloc(sizeof(double) * width * height);
  if(Gx == NULL) goto error;

  Gy = malloc(sizeof(double) * width * height);
  if(Gy == NULL) goto error;

  // perform edge enhancement in both directions
  edge_enhance_1d(in, Gx, width, height, ASHIFT_ENHANCE_HORIZONTAL);
  edge_enhance_1d(in, Gy, width, height, ASHIFT_ENHANCE_VERTICAL);

// calculate absolute values
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, width) \
  shared(Gx, Gy, out) \
  schedule(static)
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
static void XYZ_to_sRGB(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t sRGB)
{
  sRGB[0] =  3.1338561f * XYZ[0] - 1.6168667f * XYZ[1] - 0.4906146f * XYZ[2];
  sRGB[1] = -0.9787684f * XYZ[0] + 1.9161415f * XYZ[1] + 0.0334540f * XYZ[2];
  sRGB[2] =  0.0719453f * XYZ[0] - 0.2289914f * XYZ[1] + 1.4052427f * XYZ[2];
}

// sRGB -> XYZ matrix
static void sRGB_to_XYZ(const dt_aligned_pixel_t sRGB, dt_aligned_pixel_t XYZ)
{
  XYZ[0] = 0.4360747f * sRGB[0] + 0.3850649f * sRGB[1] + 0.1430804f * sRGB[2];
  XYZ[1] = 0.2225045f * sRGB[0] + 0.7168786f * sRGB[1] + 0.0606169f * sRGB[2];
  XYZ[2] = 0.0139322f * sRGB[0] + 0.0971045f * sRGB[1] + 0.7141733f * sRGB[2];
}

// detail enhancement via bilateral grid (function arguments in and out may represent identical buffers)
static int detail_enhance(const float *const in, float *const out, const int width, const int height)
{
  const float sigma_r = 5.0f;
  const float sigma_s = fminf(width, height) * 0.02f;
  const float detail = 10.0f;
  const size_t npixels = (size_t)width * height;
  int success = TRUE;

  // we need to convert from RGB to Lab first;
  // as colors don't matter we are safe to assume data to be sRGB

  // convert RGB input to Lab, use output buffer for intermediate storage
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t index = 0; index < 4*npixels; index += 4)
  {
    dt_aligned_pixel_t XYZ;
    sRGB_to_XYZ(in + index, XYZ);
    dt_XYZ_to_Lab(XYZ, out + index);
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
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels) \
  dt_omp_sharedconst(out) \
  schedule(static)
#endif
  for(size_t index = 0; index < 4*npixels; index += 4)
  {
    dt_aligned_pixel_t XYZ;
    dt_Lab_to_XYZ(out + index, XYZ);
    XYZ_to_sRGB(XYZ, out + index);
  }

  return success;
}

// apply gamma correction to RGB buffer (function arguments in and out may represent identical buffers)
static void gamma_correct(const float *const in, float *const out, const int width, const int height)
{
  const size_t npixels = (size_t)width * height;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(int index = 0; index < 4*npixels; index += 4)
  {
    for(int c = 0; c < 3; c++)
      out[index+c] = powf(in[index+c], LSD_GAMMA);
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
  greyscale = malloc(sizeof(double) * width * height);
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
    ashift_lines = (dt_iop_ashift_line_t *)malloc(sizeof(dt_iop_ashift_line_t) * lines_count);
    if(ashift_lines == NULL) goto error;

    for(int n = 0; n < lines_count; n++)
    {
      const float x1 = lsd_lines[n * 7 + 0];
      const float y1 = lsd_lines[n * 7 + 1];
      const float x2 = lsd_lines[n * 7 + 2];
      const float y2 = lsd_lines[n * 7 + 3];

      // check for lines running along image borders and skip them.
      // these would likely be false-positives which could result
      // from any kind of processing artifacts
      if((fabsf(x1 - x2) < 1 && fmaxf(x1, x2) < 2)
         || (fabsf(x1 - x2) < 1 && fminf(x1, x2) > width - 3)
         || (fabsf(y1 - y2) < 1 && fmaxf(y1, y2) < 2)
         || (fabsf(y1 - y2) < 1 && fminf(y1, y2) > height - 3))
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


      const float angle = atan2f(py2 - py1, px2 - px1) / M_PI * 180.0f;
      const int vertical = fabsf(fabsf(angle) - 90.0f) < MAX_TANGENTIAL_DEVIATION ? 1 : 0;
      const int horizontal = fabsf(fabsf(fabsf(angle) - 90.0f) - 90.0f) < MAX_TANGENTIAL_DEVIATION ? 1 : 0;

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
static int _get_structure(dt_iop_module_t *module, dt_iop_ashift_enhance_t enhance)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  float *buffer = NULL;
  int width = 0;
  int height = 0;
  int x_off = 0;
  int y_off = 0;
  float scale = 0.0f;

  dt_iop_gui_enter_critical_section(module);
  // read buffer data if they are available
  if(g->buf != NULL)
  {
    width = g->buf_width;
    height = g->buf_height;
    x_off = g->buf_x_off;
    y_off = g->buf_y_off;
    scale = g->buf_scale;

    // create a temporary buffer to hold image data
    buffer = malloc(sizeof(float) * 4 * (size_t)width * height);
    if(buffer != NULL)
      dt_iop_image_copy_by_size(buffer, g->buf, width, height, 4);
  }
  dt_iop_gui_leave_critical_section(module);

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
// Strategy: we construct a model by (random) sampling within the subset of lines and
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

  const size_t set_size = set_count * sizeof(int);
  int *best_set = malloc(set_size);
  memcpy(best_set, index_set, set_size);
  int *best_inout = calloc(1, set_size);

  float best_quality = 0.0f;

  // hurdle value epsilon for rejecting a line as an outlier will be self-tuning
  // in a number of dry runs
  float epsilon = powf(10.0f, -RANSAC_EPSILON);
  float epsilon_step = RANSAC_EPSILON_STEP;
  // some accounting variables for self-tuning
  int lines_eliminated = 0;
  int valid_runs = 0;

  // number of runs to optimize epsilon
  const int optiruns = RANSAC_OPTIMIZATION_STEPS * RANSAC_OPTIMIZATION_DRY_RUNS;
  // go for complete permutations on small set sizes, else for random sample consensus
  const int riter = (set_count > RANSAC_HURDLE) ? RANSAC_RUNS : fact(set_count);

  // some data needed for quickperm
  int *perm = malloc(sizeof(int) * (set_count + 1));
  for(int n = 0; n < set_count + 1; n++) perm[n] = n;
  int piter = 1;

  // inout holds good/bad qualification for each line
  int *inout = malloc(set_size);

  for(int r = 0; r < optiruns + riter; r++)
  {
    // get random or systematic variation of index set
    if(set_count > RANSAC_HURDLE || r < optiruns)
      shuffle(index_set, set_count);
    else
      (void)quickperm(index_set, perm, set_count, &piter);

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
       (fabsf(V[2]) > 0.0f &&
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
        // of the "distance" between point and line. Note that this is not the real euclidean
        // distance but - with the given normalization - just a pragmatically selected number
        // that goes to zero if V lies on L and increases the more V and L are apart
        const float d = fabsf(vec3scalar(V, L3));

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
          epsilon = powf(10.0f, log10(epsilon) - epsilon_step);
        else if(ratio > RANSAC_ELIMINATION_RATIO)
          epsilon = powf(10.0f, log10(epsilon) + epsilon_step);
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
        memcpy(best_set, index_set, set_size);
        memcpy(best_inout, inout, set_size);
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
  memcpy(index_set, best_set, set_size);
  memcpy(inout_set, best_inout, set_size);

  free(inout);
  free(perm);
  free(best_inout);
  free(best_set);
}


// try to clean up structural data by eliminating outliers and thereby increasing
// the chance of a convergent fitting
static int _remove_outliers(dt_iop_module_t *module)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  const int width = g->lines_in_width;
  const int height = g->lines_in_height;
  const int xmin = g->lines_x_off;
  const int ymin = g->lines_y_off;
  const int xmax = xmin + width;
  const int ymax = ymin + height;

  // holds the index set of lines we want to work on
  int *lines_set = malloc(sizeof(int) * g->lines_count);
  // holds the result of ransac
  int *inout_set = malloc(sizeof(int) * g->lines_count);

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

  free(inout_set);
  free(lines_set);

  return TRUE;

error:
  free(inout_set);
  free(lines_set);
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
  {
#ifdef ASHIFT_DEBUG
    printf("optimization not possible: insufficient number of lines\n");
#endif
    return NMS_NOT_ENOUGH_LINES;
  }

  // start the simplex fit
  int iter = simplex(model_fitness, params, fit.params_count, NMS_EPSILON, NMS_SCALE, NMS_ITERATIONS, NULL, (void*)&fit);

  // error case: the fit did not converge
  if(iter >= NMS_ITERATIONS)
  {
#ifdef ASHIFT_DEBUG
    printf("optimization not successful: maximum number of iterations reached (%d)\n", iter);
#endif
    return NMS_DID_NOT_CONVERGE;
  }

  // fit was successful: now consolidate the results (order matters!!!)
  pcount = 0;
  fit.rotation = isnan(fit.rotation) ? ilogit(params[pcount++], -fit.rotation_range, fit.rotation_range) : fit.rotation;
  fit.lensshift_v = isnan(fit.lensshift_v) ? ilogit(params[pcount++], -fit.lensshift_v_range, fit.lensshift_v_range) : fit.lensshift_v;
  fit.lensshift_h = isnan(fit.lensshift_h) ? ilogit(params[pcount++], -fit.lensshift_h_range, fit.lensshift_h_range) : fit.lensshift_h;
  fit.shear = isnan(fit.shear) ? ilogit(params[pcount++], -fit.shear_range, fit.shear_range) : fit.shear;
#ifdef ASHIFT_DEBUG
  printf("params after optimization (%d iterations): rotation %f, lensshift_v %f, lensshift_h %f, shear %f\n",
         iter, fit.rotation, fit.lensshift_v, fit.lensshift_h, fit.shear);
#endif

  // sanity check: in case of extreme values the image gets distorted so strongly that it spans an insanely huge area. we check that
  // case and assume values that increase the image area by more than a factor of 4 as being insane.
  float homograph[3][3];
  homography((float *)homograph, fit.rotation, fit.lensshift_v, fit.lensshift_h, fit.shear, fit.f_length_kb,
             fit.orthocorr, fit.aspect, fit.width, fit.height, ASHIFT_HOMOGRAPH_FORWARD);

  // visit all four corners and find maximum span
  float xm = FLT_MAX, xM = -FLT_MAX, ym = FLT_MAX, yM = -FLT_MAX;
  for(int y = 0; y < fit.height; y += fit.height - 1)
    for(int x = 0; x < fit.width; x += fit.width - 1)
    {
      float pi[3], po[3];
      pi[0] = x;
      pi[1] = y;
      pi[2] = 1.0f;
      mat3mulv(po, (float *)homograph, pi);
      po[0] /= po[2];
      po[1] /= po[2];
      xm = fmin(xm, po[0]);
      ym = fmin(ym, po[1]);
      xM = fmax(xM, po[0]);
      yM = fmax(yM, po[1]);
    }

  if((xM - xm) * (yM - ym) > 4.0f * fit.width * fit.height)
  {
#ifdef ASHIFT_DEBUG
    printf("optimization not successful: degenerate case with area growth factor (%f) exceeding limits\n",
           (xM - xm) * (yM - ym) / (fit.width * fit.height));
#endif
    return NMS_INSANE;
  }

  // now write the results into structure p
  p->rotation = fit.rotation;
  p->lensshift_v = fit.lensshift_v;
  p->lensshift_h = fit.lensshift_h;
  p->shear = fit.shear;
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

// function to keep crop fitting parameters within constraints
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

  // two auxiliary points (some arbitrary distance away from P) to construct the diagonals
  const float Pa[2][3] = { { P[0] + 10.0f * cosf(alpha), P[1] + 10.0f * sinf(alpha), 1.0f },
                           { P[0] + 10.0f * cosf(alpha), P[1] - 10.0f * sinf(alpha), 1.0f } };

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

      // the default case -> normalize I
      I[0] /= I[2];
      I[1] /= I[2];

      // calculate distance from I to P
      const float d2 = SQR(P[0] - I[0]) + SQR(P[1] - I[1]);

      // the minimum distance over all intersection points
      d2min = MIN(d2min, d2);
    }

  // calculate the area of the rectangle
  const float A = 2.0f * d2min * sinf(2.0f * alpha);

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

  // if sizes are not ready (module disabled), just ignore this
  if(g->buf_width == 0 || g->buf_height == 0) return;

  // skip if fitting is still running
  if(g->fitting) return;

  // reset fit margins if auto-cropping is off
  if(p->cropmode == ASHIFT_CROP_OFF)
  {
    _clear_shadow_crop_box(g);
    _commit_crop_box(p, g);
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
    params[2] = atan2f((float)cropfit.height, (float)cropfit.width);
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
    cropfit.alpha = atan2f((float)cropfit.height, (float)cropfit.width);
    pcount = 2;
  }

  // start the simplex fit
  const int iter = simplex(crop_fitness, params, pcount, NMS_CROP_EPSILON, NMS_CROP_SCALE, NMS_CROP_ITERATIONS,
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
  // no need to check for division by zero here as this case implies A == 0.0f, caught above
  const float d = sqrtf(A / (2.0f * sinf(2.0f * cropfit.alpha)));

  // the rectangle's center in input image (homogeneous) coordinates
  const float Pc[3] = { cropfit.x * wd, cropfit.y * ht, 1.0f };

  // convert rectangle center to output image coordinates and normalize
  float P[3];
  mat3mulv(P, (float *)cropfit.homograph, Pc);
  P[0] /= P[2];
  P[1] /= P[2];

  // calculate clipping margins relative to output image dimensions
  g->cl = CLAMP((P[0] - d * cosf(cropfit.alpha)) / owd, 0.0f, 1.0f);
  g->cr = CLAMP((P[0] + d * cosf(cropfit.alpha)) / owd, 0.0f, 1.0f);
  g->ct = CLAMP((P[1] - d * sinf(cropfit.alpha)) / oht, 0.0f, 1.0f);
  g->cb = CLAMP((P[1] + d * sinf(cropfit.alpha)) / oht, 0.0f, 1.0f);

  // final sanity check
  if(g->cr - g->cl <= 0.0f || g->cb - g->ct <= 0.0f) goto failed;

  g->fitting = 0;

#ifdef ASHIFT_DEBUG
  printf("margins after crop fitting: iter %d, x %f, y %f, angle %f, crop area (%f %f %f %f), width %f, height %f\n",
         iter, cropfit.x, cropfit.y, cropfit.alpha, g->cl, g->cr, g->ct, g->cb, wd, ht);
#endif
  dt_control_queue_redraw_center();
  return;

failed:
  // in case of failure: reset clipping margins, set "automatic cropping" parameter
  // to "off" state, and display warning message
  _clear_shadow_crop_box(g);
  _commit_crop_box(p, g);
  p->cropmode = ASHIFT_CROP_OFF;
  dt_bauhaus_combobox_set(g->cropmode, p->cropmode);
  g->fitting = 0;
  dt_control_log(_("automatic cropping failed"));
  return;
}

// manually adjust crop area by shifting its center
static void crop_adjust(dt_iop_module_t *module, const dt_iop_ashift_params_t *const p,
                        const float newx, const float newy)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  // skip if fitting is still running
  if(g->fitting) return;

  // get parameters for the homograph
  const float f_length_kb = (p->mode == ASHIFT_MODE_GENERIC) ? DEFAULT_F_LENGTH : p->f_length * p->crop_factor;
  const float orthocorr = (p->mode == ASHIFT_MODE_GENERIC) ? 0.0f : p->orthocorr;
  const float aspect = (p->mode == ASHIFT_MODE_GENERIC) ? 1.0f : p->aspect;
  const float rotation = p->rotation;
  const float lensshift_v = p->lensshift_v;
  const float lensshift_h = p->lensshift_h;
  const float shear = p->shear;
  const float wd = g->buf_width;
  const float ht = g->buf_height;

  const float alpha = atan2f(ht, wd);

  float homograph[3][3];
  homography((float *)homograph, rotation, lensshift_v, lensshift_h, shear, f_length_kb,
             orthocorr, aspect, wd, ht, ASHIFT_HOMOGRAPH_FORWARD);

  // the four vertices of the image in input image coordinates
  const float Vc[4][3] = { { 0.0f, 0.0f, 1.0f },
                           { 0.0f,   ht, 1.0f },
                           {   wd,   ht, 1.0f },
                           {   wd, 0.0f, 1.0f } };

  // convert the vertices to output image coordinates
  float V[4][3];
  for(int n = 0; n < 4; n++)
    mat3mulv(V[n], (float *)homograph, Vc[n]);

  // get width and height of output image
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
  float E[4][3];
  for(int n = 0; n < 4; n++)
    vec3prodn(E[n], V[n], V[(n + 1) % 4]);

  // the center of the rectangle in output image coordinates
  const float P[3] = { newx * owd, newy * oht, 1.0f };

  // two auxiliary points (some arbitrary distance away from P) to construct the diagonals
  const float Pa[2][3] = { { P[0] + 10.0f * cosf(alpha), P[1] + 10.0f * sinf(alpha), 1.0f },
                           { P[0] + 10.0f * cosf(alpha), P[1] - 10.0f * sinf(alpha), 1.0f } };

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
      vec3prodn(I, E[k], D[l]);

      // special case: I is all null -> E and D are identical -> P lies on E -> d2min = 0
      if(vec3isnull(I))
      {
        d2min = 0.0f;
        break;
      }

      // special case: I[2] is 0.0f -> E and D are parallel and intersect at infinity -> no relevant point
      if(I[2] == 0.0f)
        continue;

      // the default case -> normalize I
      I[0] /= I[2];
      I[1] /= I[2];

      // calculate distance from I to P
      const float d2 = SQR(P[0] - I[0]) + SQR(P[1] - I[1]);

      // the minimum distance over all intersection points
      d2min = MIN(d2min, d2);
    }

  const float d = sqrtf(d2min);

  // do not allow crop area to drop below 1% of input image area
  const float A = 2.0f * d * d * sinf(2.0f * alpha);
  if(A < 0.01f * wd * ht) return;

  // calculate clipping margins relative to output image dimensions
  g->cl = CLAMP((P[0] - d * cosf(alpha)) / owd, 0.0f, 1.0f);
  g->cr = CLAMP((P[0] + d * cosf(alpha)) / owd, 0.0f, 1.0f);
  g->ct = CLAMP((P[1] - d * sinf(alpha)) / oht, 0.0f, 1.0f);
  g->cb = CLAMP((P[1] + d * sinf(alpha)) / oht, 0.0f, 1.0f);

#ifdef ASHIFT_DEBUG
  printf("margins after crop adjustment: x %f, y %f, angle %f, crop area (%f %f %f %f), width %f, height %f\n",
         0.5f * (g->cl + g->cr), 0.5f * (g->ct + g->cb), alpha, g->cl, g->cr, g->ct, g->cb, wd, ht);
#endif
  return;
}

// determine if the line is vertical or horizontal
static void _draw_retrieve_line_type(dt_iop_ashift_line_t *line)
{
  dt_iop_ashift_linetype_t linetype = ASHIFT_LINE_VERTICAL_SELECTED;
  if(fabsf(line->p1[0] - line->p2[0]) > fabsf(line->p1[1] - line->p2[1]))
    linetype = ASHIFT_LINE_HORIZONTAL_SELECTED;
  line->type = linetype;
}

// add a basic line. used for drawing perspective method
static void _draw_basic_line(dt_iop_ashift_line_t *line, float x1, float y1, float x2, float y2,
                             dt_iop_ashift_linetype_t type)
{
  // store as homogeneous coordinates
  line->p1[0] = x1;
  line->p1[1] = y1;
  line->p1[2] = 1.0f;
  line->p2[0] = x2;
  line->p2[1] = y2;
  line->p2[2] = 1.0f;

  // calculate homogeneous coordinates of connecting line (defined by the two points)
  vec3prodn(line->L, line->p1, line->p2);

  // normalaze line coordinates so that x^2 + y^2 = 1
  // (this will always succeed as L is a real line connecting two real points)
  vec3lnorm(line->L, line->L);

  // length and width of rectangle (see LSD)
  line->length = sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
  line->width = 1.0f;
  line->weight = 1.0f;

  // register type of line
  line->type = type;
}

static void _gui_update_structure_states(dt_iop_module_t *self, GtkWidget *widget)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  if(widget && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), FALSE);
  else
  {
    if(widget != g->structure_lines) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->structure_lines), FALSE);
    if(widget != g->structure_quad) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->structure_quad), FALSE);
    if(widget != g->structure_auto) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->structure_auto), FALSE);
    if(widget) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), TRUE);
  }

  // update fit buttons state
  const gboolean enable = (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->structure_auto))
                           || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->structure_quad))
                           || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->structure_lines)));
  gtk_widget_set_sensitive(g->fit_v, enable);
  gtk_widget_set_sensitive(g->fit_h, enable);
  gtk_widget_set_sensitive(g->fit_both, enable);
}

static void _draw_save_lines_to_params(dt_iop_module_t *self)
{
  // to save drawn lines in parameters, we only need extremas positions
  // this positions needs to be saved in "original image" reference

  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  if(!g || !p) return;

  const float pr_d = self->dev->preview_downsampling;
  // save quad lines (we only handle the 2 vertical lines)
  if(g->current_structure_method == ASHIFT_METHOD_QUAD && g->lines && g->lines_count >= 4)
  {
    float pts[8] = { g->lines[0].p1[0] / pr_d, g->lines[0].p1[1] / pr_d, g->lines[0].p2[0] / pr_d,
                     g->lines[0].p2[1] / pr_d, g->lines[1].p1[0] / pr_d, g->lines[1].p1[1] / pr_d,
                     g->lines[1].p2[0] / pr_d, g->lines[1].p2[1] / pr_d };
    if(dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                         DT_DEV_TRANSFORM_DIR_BACK_EXCL, pts, 4))
    {
      for(int i = 0; i < 8; i++) p->last_quad_lines[i] = pts[i];

      dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
    }
  }
  // save drawn lines (we drop the unselected ones)
  if(g->current_structure_method == ASHIFT_METHOD_LINES && g->lines)
  {
    p->last_drawn_lines_count = 0;

    for(int i = 0; i < g->lines_count; i++)
    {
      // we only save selected lines, not removed ones
      if(g->lines[i].type == ASHIFT_LINE_HORIZONTAL_SELECTED
         || g->lines[i].type == ASHIFT_LINE_VERTICAL_SELECTED)
      {
        p->last_drawn_lines[p->last_drawn_lines_count * 4    ] = g->lines[i].p1[0];
        p->last_drawn_lines[p->last_drawn_lines_count * 4 + 1] = g->lines[i].p1[1];
        p->last_drawn_lines[p->last_drawn_lines_count * 4 + 2] = g->lines[i].p2[0];
        p->last_drawn_lines[p->last_drawn_lines_count * 4 + 3] = g->lines[i].p2[1];
        p->last_drawn_lines_count++;
        if(p->last_drawn_lines_count >= MAX_SAVED_LINES) break;
      }
    }
    if(dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                         DT_DEV_TRANSFORM_DIR_BACK_EXCL, p->last_drawn_lines,
                                         p->last_drawn_lines_count * 2))
    {
      dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
    }
  }
}
static gboolean _draw_retrieve_lines_from_params(dt_iop_module_t *self, dt_iop_ashift_method_t method)
{
  // parameters contains lines extremas positions in "original image" reference
  // so we need to translate them in module input reference
  // and to compute length and ... values

  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  if(!g || !p) return FALSE;

  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
  const float pr_d = self->dev->preview_downsampling;

  if(method == ASHIFT_METHOD_QUAD
     && p->last_quad_lines[0] > 0.0f && p->last_quad_lines[1] > 0.0f
     && p->last_quad_lines[2] > 0.0f && p->last_quad_lines[3] > 0.0f)
  {
    float pts[8] = { p->last_quad_lines[0], p->last_quad_lines[1],
                     p->last_quad_lines[2], p->last_quad_lines[3],
                     p->last_quad_lines[4], p->last_quad_lines[5],
                     p->last_quad_lines[6], p->last_quad_lines[7] };
    if(dt_dev_distort_transform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                     DT_DEV_TRANSFORM_DIR_BACK_EXCL, pts, 4))
    {
      if(g->lines) free(g->lines);
      g->lines = (dt_iop_ashift_line_t *)g_malloc0(sizeof(dt_iop_ashift_line_t) * 4);
      // vertical lines
      _draw_basic_line(&g->lines[0], pts[0] * pr_d, pts[1] * pr_d, pts[2] * pr_d, pts[3] * pr_d,
                       ASHIFT_LINE_VERTICAL_SELECTED);
      _draw_basic_line(&g->lines[1], pts[4] * pr_d, pts[5] * pr_d, pts[6] * pr_d, pts[7] * pr_d,
                       ASHIFT_LINE_VERTICAL_SELECTED);

      // horizontal lines
      _draw_basic_line(&g->lines[2], pts[0] * pr_d, pts[1] * pr_d, pts[4] * pr_d, pts[5] * pr_d,
                       ASHIFT_LINE_HORIZONTAL_SELECTED);
      _draw_basic_line(&g->lines[3], pts[2] * pr_d, pts[3] * pr_d, pts[6] * pr_d, pts[7] * pr_d,
                       ASHIFT_LINE_HORIZONTAL_SELECTED);

      g->lines_count = 4;
      g->vertical_count = 2;
      g->horizontal_count = 2;
      g->vertical_weight = 2.0;
      g->horizontal_weight = 2.0;
      g->lines_in_width = piece->iwidth * pr_d;
      g->lines_in_height = piece->iheight * pr_d;
      g->current_structure_method = method;
      return TRUE;
    }
  }

  if(method == ASHIFT_METHOD_LINES && p->last_drawn_lines_count > 0)
  {
    float pts[MAX_SAVED_LINES * 4] = { 0.0f };

    for(int i = 0; i < p->last_drawn_lines_count * 4; i++)
      pts[i] = p->last_drawn_lines[i];

    if(dt_dev_distort_transform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                     DT_DEV_TRANSFORM_DIR_BACK_EXCL, pts, p->last_drawn_lines_count * 2))
    {
      if(g->lines) free(g->lines);
      g->lines = (dt_iop_ashift_line_t *)g_malloc0(sizeof(dt_iop_ashift_line_t) * p->last_drawn_lines_count);

      int vnb = 0; // number of vertical lines
      int hnb = 0; // number of horizontal lines
      for(int i = 0; i < p->last_drawn_lines_count; i++)
      {
        // determine if the line is vertical or horizontal
        dt_iop_ashift_linetype_t linetype = ASHIFT_LINE_VERTICAL_SELECTED;
        if(fabsf(pts[i * 4] - pts[i * 4 + 2]) > fabsf(pts[i * 4 + 1] - pts[i * 4 + 3]))
          linetype = ASHIFT_LINE_HORIZONTAL_SELECTED;

        _draw_basic_line(&g->lines[i], pts[i * 4], pts[i * 4 + 1], pts[i * 4 + 2], pts[i * 4 + 3], linetype);
        if(linetype == ASHIFT_LINE_VERTICAL_SELECTED)
          vnb++;
        else
          hnb++;
      }

      g->lines_count = p->last_drawn_lines_count;
      g->vertical_count = vnb;
      g->horizontal_count = hnb;
      g->vertical_weight = (float)vnb;
      g->horizontal_weight = (float)hnb;
      g->lines_in_width = piece->iwidth * pr_d;
      g->lines_in_height = piece->iheight * pr_d;
      g->current_structure_method = method;
      return TRUE;
    }
  }
  return FALSE;
}

// helper function to clean structural data
static int _do_clean_structure(dt_iop_module_t *module, dt_iop_ashift_params_t *p, gboolean save_drawn)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  if(g->fitting) return FALSE;

  // if needed, we save the actual drawn line
  if(save_drawn) _draw_save_lines_to_params(module);

  g->fitting = 1;
  g->lines_count = 0;
  g->vertical_count = 0;
  g->horizontal_count = 0;
  if(g->lines) free(g->lines);
  g->lines = NULL;
  g->lines_version++;
  g->current_structure_method = ASHIFT_METHOD_NONE;
  g->fitting = 0;
  return TRUE;
}

// helper function to start analysis for structural data and report about errors
static int _do_get_structure_auto(dt_iop_module_t *module, dt_iop_ashift_params_t *p,
                                  dt_iop_ashift_enhance_t enhance)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  if(g->fitting) return FALSE;

  g->fitting = 1;

  dt_iop_gui_enter_critical_section(module);
  float *b = g->buf;
  dt_iop_gui_leave_critical_section(module);

  if(b == NULL)
  {
    dt_control_log(_("data pending - please repeat"));
    // force to reprocess the preview, otherwise the buffer is ko
    dt_dev_pixelpipe_flush_caches(module->dev->preview_pipe);
    dt_dev_reprocess_preview(module->dev);
    goto error;
  }

  if(!_get_structure(module, enhance))
  {
    dt_control_log(_("could not detect structural data in image"));
#ifdef ASHIFT_DEBUG
    // find out more
    printf("do_get_structure: buf %p, buf_hash %lu, buf_width %d, buf_height %d, lines %p, lines_count %d\n",
           g->buf, g->buf_hash, g->buf_width, g->buf_height, g->lines, g->lines_count);
#endif
    goto error;
  }

  if(!_remove_outliers(module))
  {
    dt_control_log(_("could not run outlier removal"));
#ifdef ASHIFT_DEBUG
    // find out more
    printf("_remove_outliers: buf %p, buf_hash %lu, buf_width %d, buf_height %d, lines %p, lines_count %d\n",
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

// initialise the lines structure method
static void _do_get_structure_lines(dt_iop_module_t *self)
{
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  // we verify that we have a valid buffer
  dt_iop_gui_enter_critical_section(self);
  float *b = g->buf;
  dt_iop_gui_leave_critical_section(self);

  if(b == NULL)
  {
    dt_control_log(_("data pending - please repeat"));
    // force to reprocess the preview, otherwise the buffer is ko
    dt_dev_pixelpipe_flush_caches(self->dev->preview_pipe);
    dt_dev_reprocess_preview(self->dev);
    return;
  }

  _gui_update_structure_states(self, g->structure_lines);

  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);

  _do_clean_structure(self, p, TRUE);

  // if the button is unselected, we don't go further
  if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->structure_lines)))
  {
    dt_control_queue_redraw_center();
    return;
  }

  g->current_structure_method = ASHIFT_METHOD_LINES;

  const float pr_d = self->dev->preview_downsampling;
  g->lines_in_width = piece->iwidth * pr_d;
  g->lines_in_height = piece->iheight * pr_d;
  g->lines_x_off = 0;
  g->lines_y_off = 0;

  // we try to recover eventual saved lines
  _draw_retrieve_lines_from_params(self, ASHIFT_METHOD_LINES);

  dt_control_queue_redraw_center();
}

// initialise the quad structure method
static void _do_get_structure_quad(dt_iop_module_t *self)
{
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  // we verify that we have a valid buffer
  dt_iop_gui_enter_critical_section(self);
  float *b = g->buf;
  dt_iop_gui_leave_critical_section(self);

  if(b == NULL)
  {
    dt_control_log(_("data pending - please repeat"));
    // force to reprocess the preview, otherwise the buffer is ko
    dt_dev_pixelpipe_flush_caches(self->dev->preview_pipe);
    dt_dev_reprocess_preview(self->dev);
    return;
  }

  _gui_update_structure_states(self, g->structure_quad);

  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);

  _do_clean_structure(self, p, TRUE);

  // if the button is unselected, we don't go further
  if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->structure_quad)))
  {
    dt_control_queue_redraw_center();
    return;
  }
  // we try to recover eventual saved lines
  if(_draw_retrieve_lines_from_params(self, ASHIFT_METHOD_QUAD))
  {
    dt_control_queue_redraw_center();
  }
  else
  {
    const float pr_d = self->dev->preview_downsampling;
    const float wd = self->dev->preview_pipe->backbuf_width;
    const float ht = self->dev->preview_pipe->backbuf_height;
    float pts[8] = { wd * 0.2, ht * 0.2, wd * 0.2, ht * 0.8, wd * 0.8, ht * 0.2, wd * 0.8, ht * 0.8 };
    if(dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                         DT_DEV_TRANSFORM_DIR_FORW_INCL, pts, 4))
    {
      g->current_structure_method = ASHIFT_METHOD_QUAD;
      g->lines = (dt_iop_ashift_line_t *)malloc(sizeof(dt_iop_ashift_line_t) * 4);
      g->lines_count = 4;

      _draw_basic_line(&g->lines[0], pts[0] * pr_d, pts[1] * pr_d, pts[2] * pr_d, pts[3] * pr_d,
                       ASHIFT_LINE_VERTICAL_SELECTED);
      _draw_basic_line(&g->lines[1], pts[4] * pr_d, pts[5] * pr_d, pts[6] * pr_d, pts[7] * pr_d,
                       ASHIFT_LINE_VERTICAL_SELECTED);
      _draw_basic_line(&g->lines[2], pts[0] * pr_d, pts[1] * pr_d, pts[4] * pr_d, pts[5] * pr_d,
                       ASHIFT_LINE_HORIZONTAL_SELECTED);
      _draw_basic_line(&g->lines[3], pts[2] * pr_d, pts[3] * pr_d, pts[6] * pr_d, pts[7] * pr_d,
                       ASHIFT_LINE_HORIZONTAL_SELECTED);

      // get real line type (they may be wrong due to image rotation)
      for(int i = 0; i < 4; i++) _draw_retrieve_line_type(&g->lines[i]);

      g->lines_in_width = piece->iwidth * pr_d;
      g->lines_in_height = piece->iheight * pr_d;
      g->lines_x_off = 0;
      g->lines_y_off = 0;
      g->vertical_count = 2;
      g->horizontal_count = 2;
      g->vertical_weight = 2.0;
      g->horizontal_weight = 2.0;
      g->lines_version++;

      dt_control_queue_redraw_center();
    }
  }
}

// helper function to start parameter fit and report about errors
static void do_fit(dt_iop_module_t *module, dt_iop_ashift_params_t *p, dt_iop_ashift_fitaxis_t dir)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;

  if(g->fitting) return;

  // if no structure available get it
  if(g->lines == NULL)
    if(!_do_get_structure_auto(module, p, ASHIFT_ENHANCE_NONE)) return;

  g->fitting = 1;

  dt_iop_ashift_nmsresult_t res = nmsfit(module, p, dir);

  g->fitting = 0;

  switch(res)
  {
    case NMS_NOT_ENOUGH_LINES:
      dt_control_log(
          _("not enough structure for automatic correction\nminimum %d lines in each relevant direction"),
          MINIMUM_FITLINES);
      return;
    case NMS_DID_NOT_CONVERGE:
    case NMS_INSANE:
      dt_control_log(_("automatic correction failed, please correct manually"));
      return;
    case NMS_SUCCESS:
    default:
      break;
  }

  // finally apply cropping
  do_crop(module, p);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->rotation, p->rotation);
  dt_bauhaus_slider_set(g->lensshift_v, p->lensshift_v);
  dt_bauhaus_slider_set(g->lensshift_h, p->lensshift_h);
  dt_bauhaus_slider_set(g->shear, p->shear);
  --darktable.gui->reset;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_ashift_data_t *data = (dt_iop_ashift_data_t *)piece->data;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  const int ch = piece->colors;
  const int ch_width = ch * roi_in->width;

  // only for preview pipe: collect input buffer data and do some other evaluations
  if(g && self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW))
  {
    // we want to find out if the final output image is flipped in relation to this iop
    // so we can adjust the gui labels accordingly
    const float pr_d = self->dev->preview_downsampling;
    const int width = roi_in->width;
    const int height = roi_in->height;
    const int x_off = roi_in->x;
    const int y_off = roi_in->y;
    const float scale = roi_in->scale / pr_d;

    // origin of image and opposite corner as reference points
    dt_boundingbox_t points = { 0.0f, 0.0f, (float)piece->buf_in.width, (float)piece->buf_in.height };
    float ivec[2] = { points[2] - points[0], points[3] - points[1] };
    float ivecl = sqrtf(ivec[0] * ivec[0] + ivec[1] * ivec[1]);

    // where do they go?
    dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                      DT_DEV_TRANSFORM_DIR_FORW_EXCL, points, 2);

    float ovec[2] = { points[2] - points[0], points[3] - points[1] };
    float ovecl = sqrtf(ovec[0] * ovec[0] + ovec[1] * ovec[1]);

    // angle between input vector and output vector
    float alpha = acos(CLAMP((ivec[0] * ovec[0] + ivec[1] * ovec[1]) / (ivecl * ovecl), -1.0f, 1.0f));

    // we are interested if |alpha| is in the range of 90° +/- 45° -> we assume the image is flipped
    const int isflipped = fabs(fmod(alpha + M_PI, M_PI) - M_PI / 2.0f) < M_PI / 4.0f ? 1 : 0;

    // did modules prior to this one in pixelpipe have changed? -> check via hash value
    uint64_t hash = dt_dev_hash_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL);

    dt_iop_gui_enter_critical_section(self);
    g->isflipped = isflipped;

    // save a copy of preview input buffer for parameter fitting
    if(g->buf == NULL || (size_t)g->buf_width * g->buf_height < (size_t)width * height)
    {
      // if needed allocate buffer
      free(g->buf); // a no-op if g->buf is NULL
      // only get new buffer if no old buffer available or old buffer does not fit in terms of size
      g->buf = malloc(sizeof(float) * 4 * width * height);
    }

    if(g->buf /* && hash != g->buf_hash */)
    {
      // copy data
      dt_iop_image_copy_by_size(g->buf, ivoid, width, height, ch);

      g->buf_width = width;
      g->buf_height = height;
      g->buf_x_off = x_off;
      g->buf_y_off = y_off;
      g->buf_scale = scale;
      g->buf_hash = hash;
    }

    dt_iop_gui_leave_critical_section(self);
  }

  // if module is set to neutral parameters we just copy input->output and are done
  if(isneutral(data))
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
    return;
  }

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  float ihomograph[3][3];
  homography((float *)ihomograph, data->rotation, data->lensshift_v, data->lensshift_h, data->shear, data->f_length_kb,
             data->orthocorr, data->aspect, piece->buf_in.width, piece->buf_in.height, ASHIFT_HOMOGRAPH_INVERTED);

  // clipping offset
  const float fullwidth = (float)piece->buf_out.width / (data->cr - data->cl);
  const float fullheight = (float)piece->buf_out.height / (data->cb - data->ct);
  const float cx = roi_out->scale * fullwidth * data->cl;
  const float cy = roi_out->scale * fullheight * data->ct;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, ch_width, cx, cy, ivoid, ovoid, roi_in, roi_out) \
  shared(ihomograph, interpolation) \
  schedule(static)
#endif
  // go over all pixels of output image
  for(int j = 0; j < roi_out->height; j++)
  {
    float *const restrict out = ((float *)ovoid) + (size_t)ch * j * roi_out->width;
    for(int i = 0; i < roi_out->width; i++)
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
      dt_interpolation_compute_pixel4c(interpolation, (float *)ivoid, out + ch*i, pin[0], pin[1], roi_in->width,
                                       roi_in->height, ch_width);
    }
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_ashift_data_t *d = (dt_iop_ashift_data_t *)piece->data;
  dt_iop_ashift_global_data_t *gd = (dt_iop_ashift_global_data_t *)self->global_data;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  const int devid = piece->pipe->devid;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int width = roi_out->width;
  const int height = roi_out->height;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem dev_homo = NULL;

  // only for preview pipe: collect input buffer data and do some other evaluations
  if(self->dev->gui_attached && g && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW))
  {
    // we want to find out if the final output image is flipped in relation to this iop
    // so we can adjust the gui labels accordingly
    const float pr_d = self->dev->preview_downsampling;
    const int x_off = roi_in->x;
    const int y_off = roi_in->y;
    const float scale = roi_in->scale / pr_d;

    // origin of image and opposite corner as reference points
    dt_boundingbox_t points = { 0.0f, 0.0f, (float)piece->buf_in.width, (float)piece->buf_in.height };
    const float ivec[2] = { points[2] - points[0], points[3] - points[1] };
    const float ivecl = sqrtf(ivec[0] * ivec[0] + ivec[1] * ivec[1]);

    // where do they go?
    dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                      DT_DEV_TRANSFORM_DIR_FORW_EXCL, points, 2);

    const float ovec[2] = { points[2] - points[0], points[3] - points[1] };
    const float ovecl = sqrtf(ovec[0] * ovec[0] + ovec[1] * ovec[1]);

    // angle between input vector and output vector
    const float alpha = acos(CLAMP((ivec[0] * ovec[0] + ivec[1] * ovec[1]) / (ivecl * ovecl), -1.0f, 1.0f));

    // we are interested if |alpha| is in the range of 90° +/- 45° -> we assume the image is flipped
    const int isflipped = fabs(fmod(alpha + M_PI, M_PI) - M_PI / 2.0f) < M_PI / 4.0f ? 1 : 0;

    // do modules coming before this one in pixelpipe have changed? -> check via hash value
    uint64_t hash = dt_dev_hash_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL);

    dt_iop_gui_enter_critical_section(self);
    g->isflipped = isflipped;

    // save a copy of preview input buffer for parameter fitting
    if(g->buf == NULL || (size_t)g->buf_width * g->buf_height < (size_t)iwidth * iheight)
    {
      // if needed allocate buffer
      free(g->buf); // a no-op if g->buf is NULL
      // only get new buffer if no old buffer or old buffer does not fit in terms of size
      g->buf = malloc(sizeof(float) * 4 * iwidth * iheight);
    }

    if(g->buf /* && hash != g->buf_hash */)
    {
      // copy data
      err = dt_opencl_copy_device_to_host(devid, g->buf, dev_in, iwidth, iheight, sizeof(float) * 4);

      g->buf_width = iwidth;
      g->buf_height = iheight;
      g->buf_x_off = x_off;
      g->buf_y_off = y_off;
      g->buf_scale = scale;
      g->buf_hash = hash;
    }
    dt_iop_gui_leave_critical_section(self);
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


  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

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

  err = dt_opencl_enqueue_kernel_2d_args(devid, ldkernel, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(iwidth), CLARG(iheight), CLARRAY(2, iroi),
    CLARRAY(2, oroi),
    CLARG(in_scale), CLARG(out_scale), CLARRAY(2, clip), CLARG(dev_homo));
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_homo);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_homo);
  dt_print(DT_DEBUG_OPENCL, "[opencl_ashift] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif

// gather information about "near"-ness in g->points_idx
static void _get_near(const float *points, dt_iop_ashift_points_idx_t *points_idx, const int lines_count, float pzx,
                      float pzy, float delta, gboolean multiple)
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
    // if we don't want multiple selection, stop here
    if(!multiple && points_idx[n].near) break;
  }
}

// mark lines which are inside a rectangular area in isbounding mode
static void _get_bounded_inside(const float *points, dt_iop_ashift_points_idx_t *points_idx,
                                const int points_lines_count, float pzx, float pzy,
                                float pzx2, float pzy2, dt_iop_ashift_bounding_t mode)
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
static uint64_t _get_lines_hash(const dt_iop_ashift_line_t *lines, const int lines_count)
{
  uint64_t hash = 5381;
  for(int n = 0; n < lines_count; n++)
  {
    const dt_boundingbox_t v = { lines[n].p1[0], lines[n].p1[1], lines[n].p2[0], lines[n].p2[1] };
    union {
        float f;
        uint32_t u;
    } x;

    for(size_t i = 0; i < 4; i++) {
      x.f = v[i];
      hash = ((hash << 5) + hash) ^ x.u;
    }
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
                      const int lines_version, float **points, float **extremas,
                      dt_iop_ashift_points_idx_t **points_idx, int *points_lines_count, float scale)
{
  dt_develop_t *dev = self->dev;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  dt_iop_ashift_points_idx_t *my_points_idx = NULL;
  float *my_points = NULL;
  float *my_extremas = NULL;

  // is the display flipped relative to the original image?
  const int isflipped = g->isflipped;

  // allocate new index array
  my_points_idx = (dt_iop_ashift_points_idx_t *)malloc(sizeof(dt_iop_ashift_points_idx_t) * lines_count);
  if(my_points_idx == NULL) goto error;

  // account for total number of points
  size_t total_points = 0;

  // first step: basic initialization of my_points_idx and counting of total_points
  for(int n = 0; n < lines_count; n++)
  {
    const int length = MAX(lines[n].length, 2);

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
  my_points = (float *)malloc(sizeof(float) * 2 * total_points);
  my_extremas = (float *)malloc(sizeof(float) * 2 * 2 * lines_count);
  if(my_points == NULL) goto error;

  // second step: generate points for each line
  for(int n = 0, offset = 0; n < lines_count; n++)
  {
    my_extremas[4 * n] = lines[n].p1[0] / scale;
    my_extremas[4 * n + 1] = lines[n].p1[1] / scale;
    my_extremas[4 * n + 2] = lines[n].p2[0] / scale;
    my_extremas[4 * n + 3] = lines[n].p2[1] / scale;

    my_points_idx[n].offset = offset;

    float x = lines[n].p1[0] / scale;
    float y = lines[n].p1[1] / scale;
    const int length = lines[n].length;

    const float dx = (lines[n].p2[0] / scale - x) / (float)(length - 1);
    const float dy = (lines[n].p2[1] / scale - y) / (float)(length - 1);

    // for very small length, we set the second extrema at last point
    if(length < 2)
    {
      my_points[2 * offset] = x;
      my_points[2 * offset + 1] = y;
      offset++;
      my_points[2 * offset] = lines[n].p2[0] / scale;
      my_points[2 * offset + 1] = lines[n].p2[1] / scale;
      offset++;
    }
    else
    {
      for(int l = 0; l < length && offset < total_points; l++, offset++)
      {
        my_points[2 * offset] = x;
        my_points[2 * offset + 1] = y;

        x += dx;
        y += dy;
      }
    }
  }

  // third step: transform all points
  if(!dt_dev_distort_transform_plus(dev, dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_INCL, my_points, total_points))
    goto error;
  if(!dt_dev_distort_transform_plus(dev, dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_INCL,
                                    my_extremas, 2 * lines_count))
    goto error;

  // fourth step: get bounding box in final coordinates (used later for checking "near"-ness to mouse pointer)
  for(int n = 0; n < lines_count; n++)
  {
    float xmin = FLT_MAX, xmax = FLT_MIN, ymin = FLT_MAX, ymax = FLT_MIN;

    const size_t offset = my_points_idx[n].offset;
    const int length = my_points_idx[n].length;

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
  *extremas = my_extremas;

  return TRUE;

error:
  if(my_points_idx != NULL) free(my_points_idx);
  if(my_points != NULL) free(my_points);
  if(my_extremas) free(my_extremas);
  return FALSE;
}

// does this gui have focus?
static int gui_has_focus(struct dt_iop_module_t *self)
{
  return (self->dev->gui_module == self
          && dt_dev_modulegroups_get_activated(darktable.develop) != DT_MODULEGROUP_BASICS);
}

/* this function replaces this sentence, it calls distort_transform() for this module on the pipe
if(!dt_dev_distort_transform_plus(self->dev, self->dev->preview_pipe, self->priority, self->priority + 1,
      (float *)V, 4))
*/
static int call_distort_transform(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, struct dt_iop_module_t *self,
                                  float *points, size_t points_count)
{
  int ret = 0;
  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
  if(!piece) return ret;
  if(piece->module == self && /*piece->enabled && */  //see note below
     !(dev->gui_module && dev->gui_module->operation_tags_filter() & piece->module->operation_tags()))
  {
    ret = piece->module->distort_transform(piece->module, piece, points, points_count);
  }
  return ret;
  //NOTE: piece->enabled is FALSE for exactly the first mouse_moved event following a button_pressed event
  //  when ASHIFT_CROP_ASPECT is active, which causes the first gui_post_expose call on starting to resize
  //  the crop box to draw the center image without the crop overlay, resulting in an annoying visual glitch.
  //  Removing the check appears to have no adverse effects and eliminates the glitch.
}

void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;

  // the usual rescaling stuff
  const float wd = dev->preview_pipe->backbuf_width;
  const float ht = dev->preview_pipe->backbuf_height;
  if(wd < 1.0 || ht < 1.0) return;
  const float pr_d = dev->preview_downsampling;
  const float zoom_y = dt_control_get_dev_zoom_y();
  const float zoom_x = dt_control_get_dev_zoom_x();
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);

  // we draw the cropping area; we need x_off/y_off/width/height which is only available
  // after g->buf has been processed
  if(g->buf && self->enabled)
  {
    // roi data of the preview pipe input buffer

    const float iwd = g->buf_width / pr_d;
    const float iht = g->buf_height / pr_d;
    const float ixo = g->buf_x_off / pr_d;
    const float iyo = g->buf_y_off / pr_d;

    // the four corners of the input buffer of this module
    float V[4][2] = { { ixo,        iyo       },
                      { ixo,        iyo + iht },
                      { ixo + iwd,  iyo + iht },
                      { ixo + iwd,  iyo       } };

    // convert coordinates of corners to coordinates of this module's output
    if(!call_distort_transform(self->dev, self->dev->preview_pipe, self, (float *)V, 4))
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
    float C[4][2] = { { xmin + g->cl * owd, ymin + g->ct * oht },
                      { xmin + g->cl * owd, ymin + g->cb * oht },
                      { xmin + g->cr * owd, ymin + g->cb * oht },
                      { xmin + g->cr * owd, ymin + g->ct * oht } };

    // convert clipping corners to final output image
    if(!dt_dev_distort_transform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                     DT_DEV_TRANSFORM_DIR_FORW_EXCL, (float *)C, 4))
      return;

    cairo_save(cr);

    double dashes = DT_PIXEL_APPLY_DPI(5.0) / zoom_scale;
    cairo_set_dash(cr, &dashes, 0, 0);

    float cl_x = 0.0f, cl_y = 0.0f, cl_width = 0.0f, cl_height = 0.0f;

    if(wd / (float)width > ht / (float)height)
    {
      // more spaces top/bottom
      cl_x      = self->dev->border_size;
      cl_y      = ((float)height - (ht * zoom_scale)) / 2.0f;
      cl_width  = width - 2.0f * self->dev->border_size;
      cl_height = ht * zoom_scale;
    }
    else
    {
      // more spaces left/right
      cl_y      = self->dev->border_size;
      cl_x      = ((float)width - (wd * zoom_scale)) / 2.0f;
      cl_height = height - (2.0f * self->dev->border_size);
      cl_width  = wd * zoom_scale;
    }

    cairo_rectangle(cr, cl_x, cl_y, cl_width, cl_height);
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
    dt_draw_set_color_overlay(cr, TRUE, 1.0);
    cairo_set_line_width(cr, 2.0 / zoom_scale);
    cairo_move_to(cr, C[0][0], C[0][1]);
    cairo_line_to(cr, C[1][0], C[1][1]);
    cairo_line_to(cr, C[2][0], C[2][1]);
    cairo_line_to(cr, C[3][0], C[3][1]);
    cairo_close_path(cr);
    cairo_stroke(cr);

    // we draw the guides correctly scaled here instead of using the darkroom expose callback

    const float cx = fminf(C[0][0], fminf(C[1][0], fminf(C[2][0], C[3][0])));
    const float cy = fminf(C[0][1], fminf(C[1][1], fminf(C[2][1], C[3][1])));
    const float cw = fmaxf(C[0][0], fmaxf(C[1][0], fmaxf(C[2][0], C[3][0]))) - cx;
    const float ch = fmaxf(C[0][1], fmaxf(C[1][1], fmaxf(C[2][1], C[3][1]))) - cy;
    dt_guides_draw(cr, cx, cy, cw, ch, zoom_scale);

    // if adjusting crop, draw indicator
    if(g->adjust_crop && p->cropmode == ASHIFT_CROP_ASPECT)
    {
      const double x1 = C[0][0];
      const double x2 = fabs(x1 - C[1][0]) < 0.001f ? C[2][0] : C[1][0];
      const double y1 = C[0][1];
      const double y2 = fabs(y1 - C[1][1]) < 0.001f ? C[2][1] : C[1][1];

      const double xpos = (x1 + x2) / 2.0f;
      const double ypos = (y1 + y2) / 2.0f;
      const double base_size = fabs(x1 - x2);
      const double size_circle = base_size / 30.0f;
      const double size_line = base_size / 5.0f;
      const double size_arrow = base_size / 25.0f;

      cairo_set_line_width(cr, 2.0 / zoom_scale);
      dt_draw_set_color_overlay(cr, TRUE, 1.0);
      cairo_arc (cr, xpos, ypos, size_circle, 0, 2.0 * M_PI);
      cairo_stroke(cr);
      cairo_fill(cr);

      cairo_set_line_width(cr, 2.0 / zoom_scale);
      dt_draw_set_color_overlay(cr, TRUE, 1.0);

      // horizontal line
      cairo_move_to(cr, xpos - size_line, ypos);
      cairo_line_to(cr, xpos + size_line, ypos);

      cairo_move_to(cr, xpos - size_line, ypos);
      cairo_rel_line_to(cr, size_arrow, size_arrow);
      cairo_move_to(cr, xpos - size_line, ypos);
      cairo_rel_line_to(cr, size_arrow, -size_arrow);

      cairo_move_to(cr, xpos + size_line, ypos);
      cairo_rel_line_to(cr, -size_arrow, size_arrow);
      cairo_move_to(cr, xpos + size_line, ypos);
      cairo_rel_line_to(cr, -size_arrow, -size_arrow);

      // vertical line
      cairo_move_to(cr, xpos, ypos - size_line);
      cairo_line_to(cr, xpos, ypos + size_line);

      cairo_move_to(cr, xpos, ypos - size_line);
      cairo_rel_line_to(cr, -size_arrow, size_arrow);
      cairo_move_to(cr, xpos, ypos - size_line);
      cairo_rel_line_to(cr, size_arrow, size_arrow);

      cairo_move_to(cr, xpos, ypos + size_line);
      cairo_rel_line_to(cr, -size_arrow, -size_arrow);
      cairo_move_to(cr, xpos, ypos + size_line);
      cairo_rel_line_to(cr, size_arrow, -size_arrow);

      cairo_stroke(cr);
    }

    cairo_restore(cr);
  }

  // we draw the straightening line
  if(g->straightening)
  {
    cairo_save(cr);
    cairo_translate(cr, width / 2.0, height / 2.0);
    cairo_scale(cr, zoom_scale, zoom_scale);
    cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0) / zoom_scale);
    dt_draw_set_color_overlay(cr, FALSE, 1.0);

    float pzx, pzy;
    dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
    pzx += 0.5f;
    pzy += 0.5f;

    PangoRectangle ink;
    PangoLayout *layout;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(16) * PANGO_SCALE / zoom_scale);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);
    const float bzx = g->straighten_x + .5f, bzy = g->straighten_y + .5f;
    cairo_arc(cr, bzx * wd, bzy * ht, DT_PIXEL_APPLY_DPI(3) * pr_d, 0, 2.0 * M_PI);
    cairo_stroke(cr);
    cairo_arc(cr, pzx * wd, pzy * ht, DT_PIXEL_APPLY_DPI(3) * pr_d, 0, 2.0 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, bzx * wd, bzy * ht);
    cairo_line_to(cr, pzx * wd, pzy * ht);
    cairo_stroke(cr);

    // show rotation angle
    float dx = pzx * wd - bzx * wd, dy = pzy * ht - bzy * ht;
    if(dx < 0)
    {
      dx = -dx;
      dy = -dy;
    }
    float angle = atan2f(dy, dx);
    angle = angle * 180 / M_PI;
    if(angle > 45.0) angle -= 90;
    if(angle < -45.0) angle += 90;

    char view_angle[16];
    view_angle[0] = '\0';
    snprintf(view_angle, sizeof(view_angle), "%.2f°", angle);
    pango_layout_set_text(layout, view_angle, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    const float text_w = ink.width;
    const float text_h = DT_PIXEL_APPLY_DPI(16 + 2) / zoom_scale;
    const float margin = DT_PIXEL_APPLY_DPI(6) / zoom_scale;
    cairo_set_source_rgba(cr, .5, .5, .5, .9);
    const float xp = pzx * wd + DT_PIXEL_APPLY_DPI(20) / zoom_scale;
    const float yp = pzy * ht - ink.height;
    dt_gui_draw_rounded_rectangle(cr, text_w + 2 * margin, text_h + 2 * margin, xp - margin, yp - margin);
    cairo_set_source_rgba(cr, .7, .7, .7, .7);
    cairo_move_to(cr, xp, yp);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(desc);
    g_object_unref(layout);
    cairo_restore(cr);
  }

  // structural data are currently being collected or fit procedure is running? -> skip
  if(g->fitting) return;

  // no structural data or visibility switched off? -> stop here
  if(g->lines == NULL || !gui_has_focus(self)) return;

  // get hash value that changes if distortions from here to the end of the pixelpipe changed
  const uint64_t hash = dt_dev_hash_distort(dev);
  // get hash value that changes if coordinates of lines have changed
  const uint64_t lines_hash = _get_lines_hash(g->lines, g->lines_count);

  // points data are missing or outdated, or distortion has changed?
  if(g->points == NULL || g->points_idx == NULL || hash != g->grid_hash
     || (g->lines_version > g->points_version
         && g->lines_hash != lines_hash))
  {
    // we need to reprocess points
    free(g->points);
    g->points = NULL;
    free(g->points_idx);
    g->points_idx = NULL;
    free(g->draw_points);
    g->draw_points = NULL;
    g->points_lines_count = 0;

    if(!get_points(self, g->lines, g->lines_count, g->lines_version, &g->points, &g->draw_points, &g->points_idx,
                   &g->points_lines_count, pr_d))
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
    // hide removed lines in drawn mode
    if((g->current_structure_method == ASHIFT_METHOD_QUAD || g->current_structure_method == ASHIFT_METHOD_LINES)
       && g->points_idx[n].type != ASHIFT_LINE_HORIZONTAL_SELECTED
       && g->points_idx[n].type != ASHIFT_LINE_VERTICAL_SELECTED)
      continue;
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

  // we also draw the corner in case of drawn perspective
  if((g->current_structure_method == ASHIFT_METHOD_QUAD || g->current_structure_method == ASHIFT_METHOD_LINES)
     && g->draw_points)
  {
    dt_draw_set_color_overlay(cr, FALSE, 1.0);
    const int nb = (g->current_structure_method == ASHIFT_METHOD_LINES) ? g->lines_count * 2 : 4;
    for(int i = 0; i < nb; i++)
    {
      // hide removed lines
      if(g->lines[i / 2].type != ASHIFT_LINE_HORIZONTAL_SELECTED
         && g->lines[i / 2].type != ASHIFT_LINE_VERTICAL_SELECTED)
        continue;
      if(g->draw_near_point == i)
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(4.0) / zoom_scale);
      else
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0) / zoom_scale);
      cairo_arc(cr, g->draw_points[i * 2], g->draw_points[i * 2 + 1], DT_PIXEL_APPLY_DPI(5.0) / zoom_scale, 0,
                2.0 * M_PI);
      cairo_stroke(cr);
    }
  }

  // and we draw the selection box if any
  if(g->isbounding != ASHIFT_BOUNDING_OFF)
  {
    float pzx = 0.0f, pzy = 0.0f;
    dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
    pzx += 0.5f;
    pzy += 0.5f;

    double dashed[] = { 4.0, 4.0 };
    dashed[0] /= zoom_scale;
    dashed[1] /= zoom_scale;
    const int len = sizeof(dashed) / sizeof(dashed[0]);

    cairo_rectangle(cr, g->lastx * wd, g->lasty * ht, (pzx - g->lastx) * wd,
                   (pzy - g->lasty) * ht);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_set_line_width(cr, 1.0 / zoom_scale);
    cairo_set_dash(cr, dashed, len, 0);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_set_dash(cr, dashed, len, 4);
    cairo_stroke(cr);
  }

  // indicate which area is used for "near"-ness detection when selecting/deselecting lines
  if(g->near_delta > 0)
  {
    float pzx = 0.0f, pzy = 0.0f;
    dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
    pzx += 0.5f;
    pzy += 0.5f;

    double dashed[] = { 4.0, 4.0 };
    dashed[0] /= zoom_scale;
    dashed[1] /= zoom_scale;
    const int len = sizeof(dashed) / sizeof(dashed[0]);

    cairo_arc(cr, pzx * wd, pzy * ht, g->near_delta, 0, 2.0 * M_PI);

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
static void _update_lines_count(const dt_iop_ashift_line_t *lines, const int lines_count,
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

// determine if we are near a drawn line extrema
static int _draw_near_point(const float x, const float y, const float *points, const int limit)
{
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, 1 << closeup, 1);
  const float delta = DT_PIXEL_APPLY_DPI(6) / zoom_scale;

  for(int i = 0; i < limit; i++)
  {
    if(x - points[i * 2] < delta && x - points[i * 2] > -delta && y - points[i * 2 + 1] < delta
       && y - points[i * 2 + 1] > -delta)
      return i;
  }
  return -1;
}

static void _draw_recompute_line_length(dt_iop_ashift_line_t *line)
{
  line->length = sqrt((line->p2[0] - line->p1[0]) * (line->p2[0] - line->p1[0])
                      + (line->p2[1] - line->p1[1]) * (line->p2[1] - line->p1[1]));
}

int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  if(g->straightening)
  {
    dt_control_queue_redraw_center();
    return TRUE;
  }

  gboolean handled = FALSE;

  const float wd = self->dev->preview_pipe->backbuf_width;
  const float ht = self->dev->preview_pipe->backbuf_height;
  const float pr_d = self->dev->preview_downsampling;
  if(wd < 1.0 || ht < 1.0) return 1;

  float pzx = 0.0f, pzy = 0.0f;
  dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  if(g->adjust_crop)
  {
    dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;

    dt_boundingbox_t pts = { pzx, pzy, 1.0f, 1.0f };
    dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                      DT_DEV_TRANSFORM_DIR_FORW_INCL, pts, 2);

    pts[0] *= pr_d;
    pts[1] *= pr_d;
    pts[2] *= pr_d;
    pts[3] *= pr_d;

    const float newx = g->crop_cx + (pts[0] - pts[2]) - g->lastx;
    const float newy = g->crop_cy + (pts[1] - pts[3]) - g->lasty;

    crop_adjust(self, p, newx, newy);
    dt_control_queue_redraw_center();
    return TRUE;
  }

  // if visibility of lines is switched off or no lines available, we would normally adjust the crop box
  // but since g->adjust_crop was FALSE, we have nothing to do
  if(!g->lines) return FALSE;

  // if we are moving a drawn line extrema, we do the change here
  if(g->draw_point_move)
  {
    float pts[2] = { pzx * wd, pzy * ht };
    if(dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                         DT_DEV_TRANSFORM_DIR_FORW_INCL, pts, 1))
    {
      pts[0] *= pr_d;
      pts[1] *= pr_d;
      // first we move the point
      if(g->draw_near_point >= 0)
      {
        const int l = g->draw_near_point / 2;
        if(g->draw_near_point % 2 == 0)
        {
          g->lines[l].p1[0] = pts[0];
          g->lines[l].p1[1] = pts[1];
        }
        else
        {
          g->lines[l].p2[0] = pts[0];
          g->lines[l].p2[1] = pts[1];
        }
        _draw_recompute_line_length(&g->lines[l]);
      }

      // for the rectangle method, we need to move the horizontal line too
      if(g->current_structure_method == ASHIFT_METHOD_QUAD)
      {
        if(g->draw_near_point == 0)
        {
          g->lines[2].p1[0] = pts[0];
          g->lines[2].p1[1] = pts[1];
          _draw_recompute_line_length(&g->lines[2]);
        }
        else if(g->draw_near_point == 1)
        {
          g->lines[3].p1[0] = pts[0];
          g->lines[3].p1[1] = pts[1];
          _draw_recompute_line_length(&g->lines[3]);
        }
        else if(g->draw_near_point == 2)
        {
          g->lines[2].p2[0] = pts[0];
          g->lines[2].p2[1] = pts[1];
          _draw_recompute_line_length(&g->lines[2]);
        }
        else if(g->draw_near_point == 3)
        {
          g->lines[3].p2[0] = pts[0];
          g->lines[3].p2[1] = pts[1];
          _draw_recompute_line_length(&g->lines[3]);
        }
      }
      g->lines_hash++;
      g->lines_version++;
      dt_control_queue_redraw_center();
    }
    return TRUE;
  }

  // case where we move a drawn line
  if(g->draw_line_move >= 0)
  {
    float pts[2] = { pzx * wd, pzy * ht };
    if(dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                         DT_DEV_TRANSFORM_DIR_FORW_INCL, pts, 1))
    {
      const float dx = (pts[0] - g->draw_pointmove_x) * pr_d;
      const float dy = (pts[1] - g->draw_pointmove_y) * pr_d;
      const int n = g->draw_line_move;
      g->draw_pointmove_x = pts[0];
      g->draw_pointmove_y = pts[1];

      // we move the line extremas
      g->lines[n].p1[0] += dx;
      g->lines[n].p1[1] += dy;
      g->lines[n].p2[0] += dx;
      g->lines[n].p2[1] += dy;
      // sanity check to be sure the extremas don't go outside the image area
      g->lines[n].p1[0] = CLAMPF(g->lines[n].p1[0], 0.0f, g->lines_in_width);
      g->lines[n].p1[1] = CLAMPF(g->lines[n].p1[1], 0.0f, g->lines_in_height);
      g->lines[n].p2[0] = CLAMPF(g->lines[n].p2[0], 0.0f, g->lines_in_width);
      g->lines[n].p2[1] = CLAMPF(g->lines[n].p2[1], 0.0f, g->lines_in_height);

      _draw_recompute_line_length(&g->lines[n]);

      // for the rectangle method, we need to move the adjacent lines too
      if(g->current_structure_method == ASHIFT_METHOD_QUAD)
      {
        if(n == 0)
        {
          g->lines[2].p1[0] = g->lines[n].p1[0];
          g->lines[2].p1[1] = g->lines[n].p1[1];
          g->lines[3].p1[0] = g->lines[n].p2[0];
          g->lines[3].p1[1] = g->lines[n].p2[1];
          _draw_recompute_line_length(&g->lines[2]);
          _draw_recompute_line_length(&g->lines[3]);
        }
        else if(n == 1)
        {
          g->lines[2].p2[0] = g->lines[n].p1[0];
          g->lines[2].p2[1] = g->lines[n].p1[1];
          g->lines[3].p2[0] = g->lines[n].p2[0];
          g->lines[3].p2[1] = g->lines[n].p2[1];
          _draw_recompute_line_length(&g->lines[2]);
          _draw_recompute_line_length(&g->lines[3]);
        }
        else if(n == 2)
        {
          g->lines[0].p1[0] = g->lines[n].p1[0];
          g->lines[0].p1[1] = g->lines[n].p1[1];
          g->lines[1].p1[0] = g->lines[n].p2[0];
          g->lines[1].p1[1] = g->lines[n].p2[1];
          _draw_recompute_line_length(&g->lines[0]);
          _draw_recompute_line_length(&g->lines[1]);
        }
        else if(n == 3)
        {
          g->lines[0].p2[0] = g->lines[n].p1[0];
          g->lines[0].p2[1] = g->lines[n].p1[1];
          g->lines[1].p2[0] = g->lines[n].p2[0];
          g->lines[1].p2[1] = g->lines[n].p2[1];
          _draw_recompute_line_length(&g->lines[0]);
          _draw_recompute_line_length(&g->lines[1]);
        }
      }

      g->lines_hash++;
      g->lines_version++;
      dt_control_queue_redraw_center();
    }
    return TRUE;
  }
  // if we are in draw mode, we check if we are near a corner
  if(g->draw_points
     && ((g->current_structure_method == ASHIFT_METHOD_QUAD && g->lines_count >= 4)
         || g->current_structure_method == ASHIFT_METHOD_LINES))
  {
    const int limit = (g->current_structure_method == ASHIFT_METHOD_LINES) ? g->lines_count * 2 : 4;
    g->draw_near_point = _draw_near_point(pzx * wd, pzy * ht, g->draw_points, limit);
  }

  // if in rectangle selecting mode adjust "near"-ness of lines according to
  // the rectangular selection
  if(g->isbounding != ASHIFT_BOUNDING_OFF)
  {
    if(wd >= 1.0 && ht >= 1.0)
    {
      // mark lines inside the rectangle
      _get_bounded_inside(g->points, g->points_idx, g->points_lines_count, pzx * wd, pzy * ht, g->lastx * wd,
                          g->lasty * ht, g->isbounding);
    }

    dt_control_queue_redraw_center();
    return FALSE;
  }

  // gather information about "near"-ness in g->points_idx
  _get_near(
      g->points, g->points_idx, g->points_lines_count, pzx * wd, pzy * ht, g->near_delta,
      !(g->current_structure_method == ASHIFT_METHOD_LINES || g->current_structure_method == ASHIFT_METHOD_QUAD));

  // if we are in sweeping mode iterate over lines as we move the pointer and change "selected" state.
  if(g->isdeselecting || g->isselecting)
  {
    for(int n = 0; g->selecting_lines_version == g->lines_version && n < g->points_lines_count; n++)
    {
      if(g->points_idx[n].near == 0)
        continue;

      if(g->isdeselecting)
      {
        g->lines[n].type &= ~ASHIFT_LINE_SELECTED;
        handled = TRUE;
      }
      else if(g->isselecting && g->current_structure_method != ASHIFT_METHOD_LINES)
      {
        g->lines[n].type |= ASHIFT_LINE_SELECTED;
        handled = TRUE;
      }
    }
  }

  if(handled)
  {
    _update_lines_count(g->lines, g->lines_count, &g->vertical_count, &g->horizontal_count);
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
  gboolean handled = FALSE;

  // avoid unexpected back to lt mode:
  if(type == GDK_2BUTTON_PRESS && which == 1)
    return TRUE;

  float pzx = 0.0f, pzy = 0.0f;
  dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  const float wd = self->dev->preview_pipe->backbuf_width;
  const float ht = self->dev->preview_pipe->backbuf_height;
  if(wd < 1.0 || ht < 1.0) return 1;

  // if we start to draw a straightening line
  if(!g->lines && which == 3)
  {
    dt_control_change_cursor(GDK_CROSSHAIR);
    g->straightening = TRUE;
    g->lastx = x;
    g->lasty = y;
    g->straighten_x = pzx - 0.5f;
    g->straighten_y = pzy - 0.5f;
    return TRUE;
  }

  // if no lines available -> potentially adjust crop area
  if(g->current_structure_method != ASHIFT_METHOD_LINES && !g->lines)
  {
    dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
    if(p->cropmode == ASHIFT_CROP_ASPECT)
    {
      const float pr_d = self->dev->preview_downsampling;
      dt_control_change_cursor(GDK_HAND1);
      g->adjust_crop = TRUE;

      dt_boundingbox_t pts = { pzx, pzy, 1.0f, 1.0f };
      dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                        DT_DEV_TRANSFORM_DIR_FORW_INCL, pts, 2);

      pts[0] *= pr_d;
      pts[1] *= pr_d;
      pts[2] *= pr_d;
      pts[3] *= pr_d;

      g->lastx = pts[0] - pts[2];
      g->lasty = pts[1] - pts[3];
      g->crop_cx = 0.5f * (g->cl + g->cr);
      g->crop_cy = 0.5f * (g->ct + g->cb);
      return TRUE;
    }
    else
      return FALSE;
  }

  // grab a draw corner
  if((g->current_structure_method == ASHIFT_METHOD_QUAD
      || g->current_structure_method == ASHIFT_METHOD_LINES)
     && g->draw_near_point >= 0)
  {
    g->draw_point_move = TRUE;
    g->lastx = x;
    g->lasty = y;
    return TRUE;
  }

  // remember lines version at this stage so we can continuously monitor if the
  // lines have changed in-between
  g->selecting_lines_version = g->lines_version;

  // if shift button is pressed go into bounding mode (selecting or deselecting
  // in a rectangle area)
  if(dt_modifier_is(state, GDK_SHIFT_MASK))
  {
    g->lastx = pzx;
    g->lasty = pzy;

    g->isbounding = (which == 3) ? ASHIFT_BOUNDING_DESELECT : ASHIFT_BOUNDING_SELECT;
    dt_control_change_cursor(GDK_CROSS);

    return TRUE;
  }

  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float min_scale = dt_dev_get_zoom_scale(self->dev, DT_ZOOM_FIT, 1<<closeup, 0);
  const float cur_scale = dt_dev_get_zoom_scale(self->dev, zoom, 1<<closeup, 0);

  // if we are zoomed out (no panning possible) and we have lines to display we take control
  const int take_control = (cur_scale == min_scale) && (g->points_lines_count > 0);

  if(g->current_structure_method == ASHIFT_METHOD_QUAD || g->current_structure_method == ASHIFT_METHOD_LINES)
    g->near_delta = dt_conf_get_float("plugins/darkroom/ashift/near_delta_draw");
  else
    g->near_delta = dt_conf_get_float("plugins/darkroom/ashift/near_delta");

  // gather information about "near"-ness in g->points_idx
  _get_near(g->points, g->points_idx, g->points_lines_count,
            pzx * wd, pzy * ht, g->near_delta,
            !(g->current_structure_method == ASHIFT_METHOD_QUAD
              || g->current_structure_method == ASHIFT_METHOD_LINES));

  if((g->current_structure_method == ASHIFT_METHOD_LINES && which == 1)
     || g->current_structure_method == ASHIFT_METHOD_QUAD)
  {
    // we search the selected line and mark it as the moved line
    for(int n = 0; n < g->points_lines_count; n++)
    {
      if(g->points_idx[n].near)
      {
        float pts[2] = { pzx * wd, pzy * ht };
        dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                          DT_DEV_TRANSFORM_DIR_FORW_INCL, pts, 1);
        g->draw_line_move = n;
        g->draw_pointmove_x = pts[0];
        g->draw_pointmove_y = pts[1];
        return TRUE;
      }
    }
    // for the rectangle draw fitting, we don't go further
    if(g->current_structure_method == ASHIFT_METHOD_QUAD) return FALSE;
  }
  else
  {
    // iterate over all lines close to the pointer and change "selected" state.
    // left-click selects and right-click deselects the line
    for(int n = 0; g->selecting_lines_version == g->lines_version && n < g->points_lines_count; n++)
    {
      if(g->points_idx[n].near == 0) continue;

      if(which == 3)
      {
        if(g->current_structure_method != ASHIFT_METHOD_LINES)
          g->lines[n].type &= ~ASHIFT_LINE_SELECTED;
        else
        {
          // we completely remove the line from the list
          if(g->lines[n].type == ASHIFT_LINE_HORIZONTAL_SELECTED)
          {
            g->horizontal_count--;
            g->horizontal_weight -= 1.0f;
          }
          else
          {
            g->vertical_count--;
            g->vertical_weight -= 1.0f;
          }

          const int count = g->lines_count - 1;
          dt_iop_ashift_line_t *lines = (dt_iop_ashift_line_t *)malloc(sizeof(dt_iop_ashift_line_t) * count);
          int pos = 0;
          for(int i = 0; i < g->lines_count; i++)
          {
            if(i != n)
            {
              lines[pos] = g->lines[i];
              pos++;
            }
          }
          if(g->lines) free(g->lines);
          g->lines = lines;
          g->lines_count = count;
        }

        handled = TRUE;
      }
      else if(g->current_structure_method != ASHIFT_METHOD_LINES)
      {
        g->lines[n].type |= ASHIFT_LINE_SELECTED;
        handled = TRUE;
      }
    }
  }

  if(!handled && g->current_structure_method == ASHIFT_METHOD_LINES && which == 1)
  {
    // start to draw a manual line
    g->draw_point_move = TRUE;
    g->lastx = x;
    g->lasty = y;

    // we instantiate a new line with both extrema at the current position
    // and enable the "move point" mode with the second extrema
    const float pr_d = self->dev->preview_downsampling;
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                      DT_DEV_TRANSFORM_DIR_FORW_INCL, pts, 1);

    pts[0] *= pr_d;
    pts[1] *= pr_d;
    const int count = g->lines_count + 1;
    // if count > MAX_SAVED_LINES we alert that the next lines won't be saved in params
    // but they still may be used for the current section (that's why we still allow them)
    if(count > MAX_SAVED_LINES) dt_control_log(_("only %d lines can be saved in parameters"), MAX_SAVED_LINES);

    dt_iop_ashift_line_t *lines = (dt_iop_ashift_line_t *)malloc(sizeof(dt_iop_ashift_line_t) * count);
    for(int i = 0; i < g->lines_count; i++)
    {
      lines[i] = g->lines[i];
    }
    if(g->lines) free(g->lines);
    g->lines = lines;
    g->lines_count = count;
    _draw_basic_line(&g->lines[count - 1], pts[0], pts[1], pts[0], pts[1], ASHIFT_LINE_VERTICAL_SELECTED);

    g->vertical_count++;
    g->vertical_weight += 1.0f;
    g->draw_near_point = g->lines_count * 2 - 1;
    return TRUE;
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
    _update_lines_count(g->lines, g->lines_count, &g->vertical_count, &g->horizontal_count);
    g->lines_version++;
    g->selecting_lines_version++;
  }

  return (take_control || handled);
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  const float wd = self->dev->preview_pipe->backbuf_width;
  const float ht = self->dev->preview_pipe->backbuf_height;

  dt_control_change_cursor(GDK_LEFT_PTR);

  // ends eventual line move
  if(g->draw_line_move >= 0)
  {
    g->draw_line_move = -1;
    // we save the lines in params
    _draw_save_lines_to_params(self);
    return TRUE;
  }

  if(g->straightening)
  {
    g->straightening = FALSE;
    // adjust the line with possible current angle and flip on this module
    float pts[4] = { x, y, g->lastx, g->lasty };
    dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe,
                                      self->iop_order,
                                      DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 2);

    float dx = pts[0] - pts[2];
    float dy = pts[1] - pts[3];
    if(dx < 0)
    {
      dx = -dx;
      dy = -dy;
    }

    float angle = atan2f(dy, dx);
    if(!(angle >= -M_PI / 2.0 && angle <= M_PI / 2.0)) angle = 0.0f;
    float close = angle;
    if(close > M_PI / 4.0)
      close = M_PI / 2.0 - close;
    else if(close < -M_PI / 4.0)
      close = -M_PI / 2.0 - close;
    else
      close = -close;

    float a = 180.0 / M_PI * close;
    if(a < -180.0) a += 360.0;
    if(a > 180.0) a -= 360.0;

    a -= dt_bauhaus_slider_get(g->rotation);
    dt_bauhaus_slider_set(g->rotation, -a);
    return TRUE;
  }

  // release a drawn corner
  if(g->draw_point_move)
  {
    // we determine the vertical/horizontal line type (that may have changed)
    // we also save the lines in params
    // points move are done directly in mouse_move routine
    for(int l = 0; l < g->lines_count; l++)
    {
      const dt_iop_ashift_linetype_t old_linetype = g->lines[l].type;
      _draw_retrieve_line_type(&g->lines[l]);

      if(g->lines[l].type != old_linetype && g->lines[l].type == ASHIFT_LINE_VERTICAL_SELECTED)
      {
        g->vertical_count++;
        g->vertical_weight += 1.0f;
        g->horizontal_count--;
        g->horizontal_weight -= 1.0f;
      }
      else if(g->lines[l].type != old_linetype)
      {
        g->horizontal_count++;
        g->horizontal_weight += 1.0f;
        g->vertical_count--;
        g->vertical_weight -= 1.0f;
      }

      g->lines_version++;
    }
    g->draw_point_move = FALSE;
    g->draw_near_point = -1;

    // we save the lines in params
    _draw_save_lines_to_params(self);

    dt_control_queue_redraw_center();
    return TRUE;
  }

  if(g->adjust_crop)
  {
    // stop adjust crop
    g->adjust_crop = FALSE;
    dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
    _swap_shadow_crop_box(p,g);  // temporarily update the crop box in p
    dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
    _swap_shadow_crop_box(p,g);  // restore p
  }

  // finalize the isbounding mode
  // if user has released the shift button in-between -> do nothing
  if(g->isbounding != ASHIFT_BOUNDING_OFF && dt_modifier_is(state, GDK_SHIFT_MASK))
  {
    gboolean handled = FALSE;

    // we compute the rectangle selection
    float pzx = 0.0f, pzy = 0.0f;
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);

    pzx += 0.5f;
    pzy += 0.5f;

    if(wd >= 1.0 && ht >= 1.0)
    {
      // mark lines inside the rectangle
      _get_bounded_inside(g->points, g->points_idx, g->points_lines_count, pzx * wd, pzy * ht, g->lastx * wd,
                          g->lasty * ht, g->isbounding);

      // select or deselect lines within the rectangle according to isbounding state
      for(int n = 0; g->selecting_lines_version == g->lines_version && n < g->points_lines_count; n++)
      {
        if(g->points_idx[n].bounded == 0) continue;

        if(g->isbounding == ASHIFT_BOUNDING_DESELECT)
        {
          g->lines[n].type &= ~ASHIFT_LINE_SELECTED;
          handled = TRUE;
        }
        else if(g->current_structure_method != ASHIFT_METHOD_LINES)
        {
          g->lines[n].type |= ASHIFT_LINE_SELECTED;
          handled = TRUE;
        }
      }

      if(handled)
      {
        _update_lines_count(g->lines, g->lines_count, &g->vertical_count, &g->horizontal_count);
        g->lines_version++;
        g->selecting_lines_version++;
      }

    dt_control_queue_redraw_center();
    }
  }

  // end of sweeping/isbounding mode
  g->isselecting = g->isdeselecting = 0;
  g->isbounding = ASHIFT_BOUNDING_OFF;
  g->near_delta = 0;
  g->lastx = g->lasty = -1.0f;
  g->crop_cx = g->crop_cy = -1.0f;

  // if we have deselected drawn lines, we need to update params
  if(g->current_structure_method == ASHIFT_METHOD_LINES && which == 3)
  {
    // we save the lines in params
    _draw_save_lines_to_params(self);
  }

  return 0;
}

int scrolled(struct dt_iop_module_t *self, double x, double y, int up, uint32_t state)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  // do nothing if visibility of lines is switched off or no lines available
  if(!g->lines) return FALSE;

  if(g->near_delta > 0 && (g->isdeselecting || g->isselecting))
  {
    gboolean handled = FALSE;

    float pzx = 0.0f, pzy = 0.0f;
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
    pzx += 0.5f;
    pzy += 0.5f;

    const float wd = self->dev->preview_pipe->backbuf_width;
    const float ht = self->dev->preview_pipe->backbuf_height;

    float near_delta = 5.0f;
    if(g->current_structure_method == ASHIFT_METHOD_QUAD || g->current_structure_method == ASHIFT_METHOD_LINES)
      near_delta = dt_conf_get_float("plugins/darkroom/ashift/near_delta_draw");
    else
      near_delta = dt_conf_get_float("plugins/darkroom/ashift/near_delta");
    const float amount = up ? 0.8f : 1.25f;
    near_delta = MAX(4.0f, MIN(near_delta * amount, 100.0f));
    if(g->current_structure_method == ASHIFT_METHOD_QUAD || g->current_structure_method == ASHIFT_METHOD_LINES)
      dt_conf_set_float("plugins/darkroom/ashift/near_delta_draw", near_delta);
    else
      dt_conf_set_float("plugins/darkroom/ashift/near_delta", near_delta);
    g->near_delta = near_delta;

    // for drawn structure, we stop here
    if(g->current_structure_method == ASHIFT_METHOD_QUAD || g->current_structure_method == ASHIFT_METHOD_LINES)
      return TRUE;

    // gather information about "near"-ness in g->points_idx
    _get_near(g->points, g->points_idx, g->points_lines_count, pzx * wd, pzy * ht, g->near_delta, TRUE);

    // iterate over all lines close to the pointer and change "selected" state.
    for(int n = 0; g->selecting_lines_version == g->lines_version && n < g->points_lines_count; n++)
    {
      if(g->points_idx[n].near == 0)
        continue;

      if(g->isdeselecting)
      {
        g->lines[n].type &= ~ASHIFT_LINE_SELECTED;
        handled = TRUE;
      }
      else if(g->isselecting && g->current_structure_method != ASHIFT_METHOD_LINES)
      {
        g->lines[n].type |= ASHIFT_LINE_SELECTED;
        handled = TRUE;
      }

      handled = TRUE;
    }

    if(handled)
    {
      _update_lines_count(g->lines, g->lines_count, &g->vertical_count, &g->horizontal_count);
      g->lines_version++;
      g->selecting_lines_version++;
    }

    dt_control_queue_redraw_center();
    return TRUE;
  }

  return FALSE;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

#ifdef ASHIFT_DEBUG
  model_probe(self, p, g->lastfit);
#endif
  if(g->buf_height > 0 && g->buf_width > 0)
  {
    do_crop(self, p);
    _commit_crop_box(p, g);
  }
  else
  {
    g->jobcode = ASHIFT_JOBCODE_DO_CROP;
  }

  if(w == g->mode)
  {
    gtk_widget_set_visible(g->specifics, p->mode == ASHIFT_MODE_SPECIFIC);
  }
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  /* reset eventual remaining structures */
  _do_clean_structure(self, p, FALSE);
  _gui_update_structure_states(self, NULL);
  // force to reprocess the preview, otherwise the buffer is ko
  dt_dev_pixelpipe_flush_caches(self->dev->preview_pipe);
}

static void cropmode_callback(GtkWidget *widget, gpointer user_data)
{
  if(darktable.gui->reset) return;

  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

  dt_conf_set_int("plugins/darkroom/ashift/autocrop_value", dt_bauhaus_combobox_get(g->cropmode));
  _swap_shadow_crop_box(p,g);	//temporarily update real crop box
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
  _swap_shadow_crop_box(p,g);
}

static int _event_fit_v_button_clicked(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return FALSE;

  if(event->button == 1)
  {
    dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
    dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

    const int control = dt_modifiers_include(event->state, GDK_CONTROL_MASK);
    const int shift = dt_modifiers_include(event->state, GDK_SHIFT_MASK);

    dt_iop_ashift_fitaxis_t fitaxis = ASHIFT_FIT_NONE;

    if(control)
      g->lastfit = fitaxis = ASHIFT_FIT_ROTATION_VERTICAL_LINES;
    else if(shift)
      g->lastfit = fitaxis = ASHIFT_FIT_VERTICALLY_NO_ROTATION;
    else
      g->lastfit = fitaxis = ASHIFT_FIT_VERTICALLY;

    dt_iop_request_focus(self);

    if(self->enabled)
    {
      // module is enable -> we process directly
      do_fit(self, p, fitaxis);
    }
    else
    {
      // module is not enabled -> invoke it and queue the job to be processed once
      // the preview image is ready
      g->jobcode = ASHIFT_JOBCODE_FIT;
      g->jobparams = g->lastfit = fitaxis;
    }

    _swap_shadow_crop_box(p, g);                             // temporarily update real crop box
    dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE); //also calls dt_control_queue_redraw_center
    _swap_shadow_crop_box(p, g);
    return TRUE;
  }
  return FALSE;
}

static int _event_fit_h_button_clicked(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return FALSE;

  if(event->button == 1)
  {
    dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
    dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

    const int control = dt_modifiers_include(event->state, GDK_CONTROL_MASK);
    const int shift = dt_modifiers_include(event->state, GDK_SHIFT_MASK);

    dt_iop_ashift_fitaxis_t fitaxis = ASHIFT_FIT_NONE;

    if(control)
      g->lastfit = fitaxis = ASHIFT_FIT_ROTATION_HORIZONTAL_LINES;
    else if(shift)
      g->lastfit = fitaxis = ASHIFT_FIT_HORIZONTALLY_NO_ROTATION;
    else
      g->lastfit = fitaxis = ASHIFT_FIT_HORIZONTALLY;

    dt_iop_request_focus(self);

    if(self->enabled)
    {
      // module is enable -> we process directly
      do_fit(self, p, fitaxis);
    }
    else
    {
      // module is not enabled -> invoke it and queue the job to be processed once
      // the preview image is ready
      g->jobcode = ASHIFT_JOBCODE_FIT;
      g->jobparams = g->lastfit = fitaxis;
    }

    _swap_shadow_crop_box(p, g);                             // temporarily update real crop box
    dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE); //also calls dt_control_queue_redraw_center
    _swap_shadow_crop_box(p, g);
    return TRUE;
  }
  return FALSE;
}

static int _event_fit_both_button_clicked(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return FALSE;

  if(event->button == 1)
  {
    dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
    dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

    const int control = dt_modifiers_include(event->state, GDK_CONTROL_MASK);
    const int shift = dt_modifiers_include(event->state, GDK_SHIFT_MASK);

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

    if(self->enabled)
    {
      // module is enable -> we process directly
      do_fit(self, p, fitaxis);
    }
    else
    {
      // module is not enabled -> invoke it and queue the job to be processed once
      // the preview image is ready
      g->jobcode = ASHIFT_JOBCODE_FIT;
      g->jobparams = g->lastfit = fitaxis;
    }

    _swap_shadow_crop_box(p, g);                             // temporarily update real crop box
    dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE); //also calls dt_control_queue_redraw_center
    _swap_shadow_crop_box(p, g);
    return TRUE;
  }
  return FALSE;
}

static int _event_structure_auto_clicked(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return FALSE;

  if(event->button == 1)
  {
    dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
    dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;

    _do_clean_structure(self, p, TRUE);

    const int control = dt_modifiers_include(event->state, GDK_CONTROL_MASK);
    const int shift = dt_modifiers_include(event->state, GDK_SHIFT_MASK);

    dt_iop_ashift_enhance_t enhance;

    if(control && shift)
      enhance = ASHIFT_ENHANCE_EDGES | ASHIFT_ENHANCE_DETAIL;
    else if(shift)
      enhance = ASHIFT_ENHANCE_DETAIL;
    else if(control)
      enhance = ASHIFT_ENHANCE_EDGES;
    else
      enhance = ASHIFT_ENHANCE_NONE;

    // if the button is unselected, we don't go further
    if(enhance == ASHIFT_ENHANCE_NONE && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)))
    {
      _gui_update_structure_states(self, widget);
      dt_control_queue_redraw_center();
      return TRUE;
    }
    else
    {
      // force the button to be untoggled, so the update routine can enable it
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), FALSE);
      _gui_update_structure_states(self, widget);
    }

    g->current_structure_method = ASHIFT_METHOD_AUTO;

    dt_iop_request_focus(self);

    if(self->enabled)
    {
      // module is enabled -> process directly
      (void)_do_get_structure_auto(self, p, enhance);
    }
    else
    {
      // module is not enabled -> invoke it and queue the job to be processed once
      // the preview image is ready
      g->jobcode = ASHIFT_JOBCODE_GET_STRUCTURE;
      g->jobparams = enhance;
    }

    dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE); // also calls dt_control_queue_redraw_center
    return TRUE;
  }
  return FALSE;
}

// routine that is called after preview image has been processed. we use it
// to perform structure collection or fitting in case those have been triggered while
// the module had not yet been enabled
static void _event_process_after_preview_callback(gpointer instance, gpointer user_data)
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
    case ASHIFT_JOBCODE_DO_CROP:
      do_crop(self, p);
      _commit_crop_box(p, g);
      // save all that
      _swap_shadow_crop_box(p, g); // temporarily update real crop box
      dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
      _swap_shadow_crop_box(p, g);
      break;

    case ASHIFT_JOBCODE_GET_STRUCTURE_QUAD:
      _do_get_structure_quad(self);
      break;

    case ASHIFT_JOBCODE_GET_STRUCTURE_LINES:
      _do_get_structure_lines(self);
      break;

    case ASHIFT_JOBCODE_GET_STRUCTURE:
      (void)_do_get_structure_auto(self, p, (dt_iop_ashift_enhance_t)jobparams);
      break;

    case ASHIFT_JOBCODE_FIT:
      do_fit(self, p, (dt_iop_ashift_fitaxis_t)jobparams);
      dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
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
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;

  gtk_widget_set_visible(g->specifics, p->mode == ASHIFT_MODE_SPECIFIC);

  // copy crop box into shadow variables
  _shadow_crop_box(p,g);

  dt_gui_update_collapsible_section(&g->cs);
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
                 || img->orientation == ORIENTATION_ROTATE_CW_90_DEG) ? 1 : 0;

    // focal length should be available in exif data if lens is electronically coupled to the camera
    f_length = isfinite(img->exif_focal_length) && img->exif_focal_length > 0.0f ? img->exif_focal_length : f_length;
    // crop factor of the camera is often not available and user will need to set it manually in the gui
    crop_factor = isfinite(img->exif_crop) && img->exif_crop > 0.0f ? img->exif_crop : crop_factor;
  }

  // init defaults:
  ((dt_iop_ashift_params_t *)module->default_params)->f_length = f_length;
  ((dt_iop_ashift_params_t *)module->default_params)->crop_factor = crop_factor;
  ((dt_iop_ashift_params_t *)module->default_params)->cropmode
      = dt_conf_get_int("plugins/darkroom/ashift/autocrop_value");

  // reset gui elements
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)module->gui_data;
  if(g)
  {

    char string_v[256];
    char string_h[256];

    snprintf(string_v, sizeof(string_v), _("lens shift (%s)"), isflipped ? _("horizontal") : _("vertical"));
    snprintf(string_h, sizeof(string_h), _("lens shift (%s)"), isflipped ? _("vertical") : _("horizontal"));

    dt_bauhaus_widget_set_label(g->lensshift_v, NULL, string_v);
    dt_bauhaus_widget_set_label(g->lensshift_h, NULL, string_h);

    dt_bauhaus_slider_set_default(g->f_length, f_length);
    dt_bauhaus_slider_set_default(g->crop_factor, crop_factor);

    dt_iop_gui_enter_critical_section(module);
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
    dt_iop_gui_leave_critical_section(module);

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
    g->lines_version = 0;
    g->isselecting = 0;
    g->isdeselecting = 0;
    g->isbounding = ASHIFT_BOUNDING_OFF;
    g->near_delta = 0;
    g->selecting_lines_version = 0;

    free(g->points);
    g->points = NULL;
    free(g->points_idx);
    g->points_idx = NULL;
    g->points_lines_count = 0;
    g->points_version = 0;

    g->jobcode = ASHIFT_JOBCODE_NONE;
    g->jobparams = 0;
    g->adjust_crop = FALSE;
    g->lastx = g->lasty = -1.0f;
    g->crop_cx = g->crop_cy = 1.0f;

    g->current_structure_method = ASHIFT_METHOD_NONE;
    g->draw_line_move = -1;
    g->draw_near_point = -1;
    g->draw_point_move = FALSE;

    _gui_update_structure_states(module, NULL);
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
static gboolean _event_draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  if(darktable.gui->reset) return FALSE;

  dt_iop_gui_enter_critical_section(self);
  const int isflipped = g->isflipped;
  dt_iop_gui_leave_critical_section(self);

  if(isflipped == -1) return FALSE;

  char string_v[256];
  char string_h[256];

  snprintf(string_v, sizeof(string_v), _("lens shift (%s)"), isflipped ? _("horizontal") : _("vertical"));
  snprintf(string_h, sizeof(string_h), _("lens shift (%s)"), isflipped ? _("vertical") : _("horizontal"));

  ++darktable.gui->reset;
  dt_bauhaus_widget_set_label(g->lensshift_v, NULL, string_v);
  dt_bauhaus_widget_set_label(g->lensshift_h, NULL, string_h);
  --darktable.gui->reset;

  return FALSE;
}

static void _event_preview_updated_callback(gpointer instance, dt_iop_module_t *self)
{
  if(self->dev->gui_module != self)
  {
    dt_image_update_final_size(self->dev->preview_pipe->output_imgid);
  }
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_event_preview_updated_callback), self);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  if(self->enabled)
  {
    dt_iop_ashift_params_t *p = (dt_iop_ashift_params_t *)self->params;
    dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
    if(in)
    {
      _shadow_crop_box(p,g);
      dt_control_queue_redraw_center();
    }
    else
    {
      // once the pipe is recomputed, we want to update final sizes
      DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                                      G_CALLBACK(_event_preview_updated_callback), self);
      _commit_crop_box(p, g);
    }
  }
}

static float log10_curve(float inval, dt_bauhaus_curve_t dir)
{
  float outval;
  if(dir == DT_BAUHAUS_SET)
  {
    outval = log10f(inval * 999.0f + 1.0f) / 3.0f;
  }
  else
  {
    outval = (expf(M_LN10 * inval * 3.0f) - 1.0f) / 999.0f;
  }
  return outval;
}

static float log2_curve(float inval, dt_bauhaus_curve_t dir)
{
  float outval;
  if(dir == DT_BAUHAUS_SET)
  {
      outval = log2f(inval * 1.5f + 0.5f) / 2.0f + 0.5f;
  }
  else
  {
    outval = (exp2f(inval * 2.0 - 1.0) - 0.5f) / 1.5f;
  }
  return outval;
}

static int _event_structure_quad_clicked(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  if(darktable.gui->reset) return FALSE;

  dt_iop_request_focus(self);

  if(self->enabled)
  {
    // module is enabled -> process directly
    _do_get_structure_quad(self);
  }
  else
  {
    // module is not enabled -> invoke it and queue the job to be processed once
    // the preview image is ready
    g->jobcode = ASHIFT_JOBCODE_GET_STRUCTURE_QUAD;
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE); // also calls dt_control_queue_redraw_center

  return TRUE;
}

static int _event_structure_lines_clicked(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  if(darktable.gui->reset) return FALSE;

  dt_iop_request_focus(self);

  if(self->enabled)
  {
    // module is enabled -> process directly
    _do_get_structure_lines(self);
  }
  else
  {
    // module is not enabled -> invoke it and queue the job to be processed once
    // the preview image is ready
    g->jobcode = ASHIFT_JOBCODE_GET_STRUCTURE_LINES;
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE); // also calls dt_control_queue_redraw_center

  return TRUE;
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_ashift_gui_data_t *g = IOP_GUI_ALLOC(ashift);

  dt_iop_gui_enter_critical_section(self); //not actually needed, we're the only one with a pointer to this instance
  g->buf = NULL;
  g->buf_width = 0;
  g->buf_height = 0;
  g->buf_x_off = 0;
  g->buf_y_off = 0;
  g->buf_scale = 1.0f;
  g->buf_hash = 0;
  g->isflipped = -1;
  g->lastfit = ASHIFT_FIT_NONE;
  dt_iop_gui_leave_critical_section(self);

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
  g->lines_hash = 0;
  g->rotation_range = ROTATION_RANGE_SOFT;
  g->lensshift_v_range = LENSSHIFT_RANGE_SOFT;
  g->lensshift_h_range = LENSSHIFT_RANGE_SOFT;
  g->shear_range = SHEAR_RANGE_SOFT;
  g->isselecting = 0;
  g->isdeselecting = 0;
  g->isbounding = ASHIFT_BOUNDING_OFF;
  g->near_delta = 0;
  g->selecting_lines_version = 0;

  g->jobcode = ASHIFT_JOBCODE_NONE;
  g->jobparams = 0;
  g->adjust_crop = FALSE;
  g->lastx = g->lasty = -1.0f;
  g->crop_cx = g->crop_cy = 1.0f;

  g->draw_near_point = -1;
  g->draw_line_move = -1;

  g->rotation = dt_bauhaus_slider_from_params(self, N_("rotation"));
  dt_bauhaus_slider_set_format(g->rotation, "°");
  dt_bauhaus_slider_set_soft_range(g->rotation, -ROTATION_RANGE, ROTATION_RANGE);

  g->cropmode = dt_bauhaus_combobox_from_params(self, "cropmode");
  g_signal_connect(G_OBJECT(g->cropmode), "value-changed", G_CALLBACK(cropmode_callback), self);

  GtkWidget *main_box = self->widget;

  dt_gui_new_collapsible_section
    (&g->cs,
     "plugins/darkroom/ashift/expand_values",
     _("manual perspective"),
     GTK_BOX(main_box));

  self->widget = GTK_WIDGET(g->cs.container);

  g->lensshift_v = dt_bauhaus_slider_from_params(self, "lensshift_v");
  dt_bauhaus_slider_set_soft_range(g->lensshift_v, -LENSSHIFT_RANGE, LENSSHIFT_RANGE);
  dt_bauhaus_slider_set_digits(g->lensshift_v, 3);

  g->lensshift_h = dt_bauhaus_slider_from_params(self, "lensshift_h");
  dt_bauhaus_slider_set_soft_range(g->lensshift_h, -LENSSHIFT_RANGE, LENSSHIFT_RANGE);
  dt_bauhaus_slider_set_digits(g->lensshift_h, 3);

  g->shear = dt_bauhaus_slider_from_params(self, "shear");
  dt_bauhaus_slider_set_soft_range(g->shear, -SHEAR_RANGE, SHEAR_RANGE);

  g->mode = dt_bauhaus_combobox_from_params(self, "mode");
  self->widget = g->specifics = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->f_length = dt_bauhaus_slider_from_params(self, "f_length");
  dt_bauhaus_slider_set_soft_range(g->f_length, 10.0f, 1000.0f);
  dt_bauhaus_slider_set_curve(g->f_length, log10_curve);
  dt_bauhaus_slider_set_digits(g->f_length, 0);
  dt_bauhaus_slider_set_format(g->f_length, " mm");

  g->crop_factor = dt_bauhaus_slider_from_params(self, "crop_factor");
  dt_bauhaus_slider_set_soft_range(g->crop_factor, 1.0f, 2.0f);

  g->orthocorr = dt_bauhaus_slider_from_params(self, "orthocorr");
  dt_bauhaus_slider_set_format(g->orthocorr, "%");
  // this parameter could serve to finetune between generic model (0%) and specific model (100%).
  // however, users can more easily get the same effect with the aspect adjust parameter so we keep
  // this one hidden.
  gtk_widget_set_no_show_all(g->orthocorr, TRUE);
  gtk_widget_set_visible(g->orthocorr, FALSE);

  g->aspect = dt_bauhaus_slider_from_params(self, "aspect");
  dt_bauhaus_slider_set_curve(g->aspect, log2_curve);

  gtk_box_pack_start(GTK_BOX(g->cs.container), g->specifics, TRUE, TRUE, 0);

  self->widget = main_box;

  GtkWidget *helpers = dt_ui_section_label_new(_("perspective"));
  gtk_box_pack_start(GTK_BOX(self->widget), helpers, TRUE, TRUE, 0);

  GtkGrid *auto_grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_row_spacing(auto_grid, 2 * DT_BAUHAUS_SPACE);
  gtk_grid_set_column_spacing(auto_grid, DT_PIXEL_APPLY_DPI(10));

  gtk_grid_attach(auto_grid, dt_ui_label_new(_("structure")), 0, 0, 1, 1);

  g->structure_lines = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_drawn, 0, NULL);
  gtk_widget_set_hexpand(GTK_WIDGET(g->structure_lines), TRUE);
  gtk_grid_attach(auto_grid, g->structure_lines, 1, 0, 1, 1);

  g->structure_quad = dtgtk_togglebutton_new(dtgtk_cairo_paint_draw_structure, 0, NULL);
  gtk_widget_set_hexpand(GTK_WIDGET(g->structure_quad), TRUE);
  gtk_grid_attach(auto_grid, g->structure_quad, 2, 0, 1, 1);

  g->structure_auto = dtgtk_togglebutton_new(dtgtk_cairo_paint_structure, 0, NULL);
  gtk_widget_set_hexpand(GTK_WIDGET(g->structure_auto), TRUE);
  gtk_grid_attach(auto_grid, g->structure_auto, 3, 0, 1, 1);

  gtk_grid_attach(auto_grid, dt_ui_label_new(_("fit")), 0, 1, 1, 1);

  g->fit_v = dtgtk_button_new(dtgtk_cairo_paint_perspective, 1, NULL);
  gtk_widget_set_hexpand(GTK_WIDGET(g->fit_v), TRUE);
  gtk_grid_attach(auto_grid, g->fit_v, 1, 1, 1, 1);

  g->fit_h = dtgtk_button_new(dtgtk_cairo_paint_perspective, 2, NULL);
  gtk_widget_set_hexpand(GTK_WIDGET(g->fit_h), TRUE);
  gtk_grid_attach(auto_grid, g->fit_h, 2, 1, 1, 1);

  g->fit_both = dtgtk_button_new(dtgtk_cairo_paint_perspective, 3, NULL);
  gtk_widget_set_hexpand(GTK_WIDGET(g->fit_both), TRUE);
  gtk_grid_attach(auto_grid, g->fit_both, 3, 1, 1, 1);

  gtk_widget_show_all(GTK_WIDGET(auto_grid));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(auto_grid), TRUE, TRUE, 0);

  self->widget = main_box;

  gtk_widget_set_tooltip_text(g->rotation, _("rotate image\nright-click and drag to define a horizontal or vertical line by drawing on the image"));
  gtk_widget_set_tooltip_text(g->lensshift_v, _("apply lens shift correction in one direction"));
  gtk_widget_set_tooltip_text(g->lensshift_h, _("apply lens shift correction in one direction"));
  gtk_widget_set_tooltip_text(g->shear, _("shear the image along one diagonal"));
  gtk_widget_set_tooltip_text(g->cropmode, _("automatically crop to avoid black edges"));
  gtk_widget_set_tooltip_text(g->mode, _("lens model of the perspective correction: "
                                         "generic or according to the focal length"));
  gtk_widget_set_tooltip_text(g->f_length, _("focal length of the lens, "
                                             "default value set from EXIF data if available"));
  gtk_widget_set_tooltip_text(g->crop_factor, _("crop factor of the camera sensor, "
                                                "default value set from EXIF data if available, "
                                                "manual setting is often required"));
  gtk_widget_set_tooltip_text(g->orthocorr, _("the level of lens dependent correction, set to maximum for full lens dependency, "
                                              "set to zero for the generic case"));
  gtk_widget_set_tooltip_text(g->aspect, _("adjust aspect ratio of image by horizontal and vertical scaling"));
  gtk_widget_set_tooltip_text(g->fit_v, _("automatically correct for vertical perspective distortion\n"
                                          "ctrl+click to only fit rotation\n"
                                          "shift+click to only fit lens shift"));
  gtk_widget_set_tooltip_text(g->fit_h, _("automatically correct for horizontal perspective distortion\n"
                                          "ctrl+click to only fit rotation\n"
                                          "shift+click to only fit lens shift"));
  gtk_widget_set_tooltip_text(g->fit_both, _("automatically correct for vertical and "
                                             "horizontal perspective distortions, fitting rotation, "
                                             "lens shift in both directions, and shear\n"
                                             "ctrl+click to only fit rotation\n"
                                             "shift+click to only fit lens shift\n"
                                             "ctrl+shift+click to only fit rotation and lens shift"));
  gtk_widget_set_tooltip_text(g->structure_auto, _("automatically analyse line structure in image\n"
                                                   "ctrl+click for an additional edge enhancement\n"
                                                   "shift+click for an additional detail enhancement\n"
                                                   "ctrl+shift+click for a combination of both methods"));
  gtk_widget_set_tooltip_text(g->structure_quad, _("manually define perspective rectangle"));
  gtk_widget_set_tooltip_text(g->structure_lines, _("manually draw structure lines"));

  g_signal_connect(G_OBJECT(g->fit_v), "button-press-event", G_CALLBACK(_event_fit_v_button_clicked),
                   (gpointer)self);
  g_signal_connect(G_OBJECT(g->fit_h), "button-press-event", G_CALLBACK(_event_fit_h_button_clicked),
                   (gpointer)self);
  g_signal_connect(G_OBJECT(g->fit_both), "button-press-event", G_CALLBACK(_event_fit_both_button_clicked),
                   (gpointer)self);
  g_signal_connect(G_OBJECT(g->structure_quad), "button-press-event", G_CALLBACK(_event_structure_quad_clicked),
                   (gpointer)self);
  g_signal_connect(G_OBJECT(g->structure_lines), "button-press-event", G_CALLBACK(_event_structure_lines_clicked),
                   (gpointer)self);
  g_signal_connect(G_OBJECT(g->structure_auto), "button-press-event", G_CALLBACK(_event_structure_auto_clicked),
                   (gpointer)self);
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(_event_draw), self);

  dt_action_define_iop(self, N_("fit"), N_("vertical"), g->fit_v, &dt_action_def_button);
  dt_action_define_iop(self, N_("fit"), N_("horizontal"), g->fit_h, &dt_action_def_button);
  dt_action_define_iop(self, N_("fit"), N_("both"), g->fit_both, &dt_action_def_button);
  dt_action_define_iop(self, N_("structure"), N_("rectangle"), g->structure_quad, &dt_action_def_toggle);
  dt_action_define_iop(self, N_("structure"), N_("lines"), g->structure_lines, &dt_action_def_toggle);
  dt_action_define_iop(self, N_("structure"), N_("auto"), g->structure_auto, &dt_action_def_toggle);

  /* add signal handler for preview pipe finish to redraw the overlay */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                                  G_CALLBACK(_event_process_after_preview_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_event_process_after_preview_callback), self);

  dt_iop_ashift_gui_data_t *g = (dt_iop_ashift_gui_data_t *)self->gui_data;
  if(g->lines) free(g->lines);
  if(g->buf) free(g->buf);
  if(g->points) free(g->points);
  if(g->points_idx) free(g->points_idx);

  IOP_GUI_FREE;
}

GSList *mouse_actions(struct dt_iop_module_t *self)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_RIGHT_DRAG, 0, _("[%s] define/rotate horizon"), self->name());
  lm  = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_LEFT,  0, _("[%s on segment] select segment"), self->name());
  lm  = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_RIGHT, 0,
                                      _("[%s on segment] unselect segment"), self->name());
  lm  = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_LEFT_DRAG,  GDK_SHIFT_MASK,
                                      _("[%s] select all segments from zone"), self->name());
  lm  = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_RIGHT_DRAG,  GDK_SHIFT_MASK,
                                      _("[%s] unselect all segments from zone"), self->name());
  return lm;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
