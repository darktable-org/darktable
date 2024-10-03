/*
  This file is part of darktable,
  Copyright (C) 2010-2024 darktable developers.

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

/** Note :
 * we use finite-math-only and fast-math because divisions by zero are manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 * the rest is loop reorganization and vectorization optimization
 **/
#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "tree-loop-if-convert", \
                      "tree-loop-distribution", "no-strict-aliasing", \
                      "loop-interchange", "loop-nest-optimize", "tree-loop-im", \
                      "unswitch-loops", "tree-loop-ivcanon", "ira-loop-pressure", \
                      "split-ivs-in-unroller", "variable-expansion-in-unroller", \
                      "split-loops", "ivopts", "predictive-commoning",\
                      "tree-loop-linear", "loop-block", "loop-strip-mine", \
                      "finite-math-only", "fp-contract=fast", "fast-math", \
                      "tree-vectorize", "no-math-errno")
#endif

// #define AI_ACTIVATED
/* AI feature not good enough so disabled for now
   If enabled there must be $DESCRIPTION: entries in illuminants.h for bauhaus
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "chart/common.h"
#include "develop/imageop_gui.h"
#include "dtgtk/drawingarea.h"
#include "common/chromatic_adaptation.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/colorchecker.h"
#include "common/opencl.h"
#include "common/illuminants.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "develop/imageop_math.h"
#include "develop/openmp_maths.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "gaussian_elimination.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

DT_MODULE_INTROSPECTION(3, dt_iop_channelmixer_rgb_params_t)

#define CHANNEL_SIZE 4
#define INVERSE_SQRT_3 0.5773502691896258f
#define COLOR_MIN -2.0
#define COLOR_MAX 2.0
#define ILLUM_X_MAX 360.0
#define ILLUM_Y_MAX 300.0
#define LIGHTNESS_MAX 100.0
#define HUE_MAX 360.0
#define CHROMA_MAX 128.0
#define TEMP_MIN 1667.
#define TEMP_MAX 25000.

typedef enum dt_iop_channelmixer_rgb_version_t
{
  CHANNELMIXERRGB_V_1 = 0, // $DESCRIPTION: "version 1 (2020)"
  CHANNELMIXERRGB_V_2 = 1, // $DESCRIPTION: "version 2 (2021)"
  CHANNELMIXERRGB_V_3 = 2, // $DESCRIPTION: "version 3 (Apr 2021)"
} dt_iop_channelmixer_rgb_version_t;

typedef struct dt_iop_channelmixer_rgb_params_t
{
  /* params of v1 and v2 */
  float red[CHANNEL_SIZE];         // $MIN: COLOR_MIN $MAX: COLOR_MAX
  float green[CHANNEL_SIZE];       // $MIN: COLOR_MIN $MAX: COLOR_MAX
  float blue[CHANNEL_SIZE];        // $MIN: COLOR_MIN $MAX: COLOR_MAX
  float saturation[CHANNEL_SIZE];  // $MIN: -2.0 $MAX: 2.0
  float lightness[CHANNEL_SIZE];   // $MIN: -2.0 $MAX: 2.0
  float grey[CHANNEL_SIZE];        // $MIN: -2.0 $MAX: 2.0
  gboolean normalize_R, normalize_G, normalize_B, normalize_sat, normalize_light, normalize_grey; // $DESCRIPTION: "normalize channels"
  dt_illuminant_t illuminant;      // $DEFAULT: DT_ILLUMINANT_D
  dt_illuminant_fluo_t illum_fluo; // $DEFAULT: DT_ILLUMINANT_FLUO_F3 $DESCRIPTION: "F source"
  dt_illuminant_led_t illum_led;   // $DEFAULT: DT_ILLUMINANT_LED_B5 $DESCRIPTION: "LED source"
  dt_adaptation_t adaptation;      // $DEFAULT: DT_ADAPTATION_CAT16
  float x, y;                      // $DEFAULT: 0.333
  float temperature;               // $MIN: TEMP_MIN $MAX: TEMP_MAX $DEFAULT: 5003.
  float gamut;                     // $MIN: 0.0 $MAX: 12.0 $DEFAULT: 1.0 $DESCRIPTION: "gamut compression"
  gboolean clip;                   // $DEFAULT: TRUE $DESCRIPTION: "clip negative RGB from gamut"

  /* params of v3 */
  dt_iop_channelmixer_rgb_version_t version; // $DEFAULT: CHANNELMIXERRGB_V_3 $DESCRIPTION: "saturation algorithm"

  /* always add new params after this so we can import legacy params with memcpy on the common part of the struct */

} dt_iop_channelmixer_rgb_params_t;


typedef enum dt_solving_strategy_t
{
  DT_SOLVE_OPTIMIZE_NONE = 0,
  DT_SOLVE_OPTIMIZE_LOW_SAT = 1,
  DT_SOLVE_OPTIMIZE_HIGH_SAT = 2,
  DT_SOLVE_OPTIMIZE_SKIN = 3,
  DT_SOLVE_OPTIMIZE_FOLIAGE = 4,
  DT_SOLVE_OPTIMIZE_SKY = 5,
  DT_SOLVE_OPTIMIZE_AVG_DELTA_E = 6,
  DT_SOLVE_OPTIMIZE_MAX_DELTA_E = 7,
} dt_solving_strategy_t;


typedef enum dt_spot_mode_t
{
  DT_SPOT_MODE_CORRECT = 0,
  DT_SPOT_MODE_MEASURE = 1,
  DT_SPOT_MODE_LAST
} dt_spot_mode_t;

typedef struct dt_iop_channelmixer_rgb_gui_data_t
{
  GtkNotebook *notebook;
  GtkWidget *illuminant, *temperature, *adaptation, *gamut, *clip;
  GtkWidget *illum_fluo, *illum_led, *illum_x, *illum_y, *approx_cct, *illum_color;
  GtkWidget *scale_red_R, *scale_red_G, *scale_red_B;
  GtkWidget *scale_green_R, *scale_green_G, *scale_green_B;
  GtkWidget *scale_blue_R, *scale_blue_G, *scale_blue_B;
  GtkWidget *scale_saturation_R, *scale_saturation_G, *scale_saturation_B, *saturation_version;
  GtkWidget *scale_lightness_R, *scale_lightness_G, *scale_lightness_B;
  GtkWidget *scale_grey_R, *scale_grey_G, *scale_grey_B;
  GtkWidget *normalize_R, *normalize_G, *normalize_B, *normalize_sat, *normalize_light, *normalize_grey;
  GtkWidget *color_picker;
  float xy[2];
  float XYZ[4];

  point_t box[4];           // the current coordinates, possibly non rectangle, of the bounding box for the color checker
  point_t ideal_box[4];     // the desired coordinates of the perfect rectangle bounding box for the color checker
  point_t center_box;       // the barycenter of both boxes
  gboolean active_node[4];  // true if the cursor is close to a node (node = corner of the bounding box)
  gboolean is_cursor_close; // do we have the cursor close to a node ?
  gboolean drag_drop;       // are we currently dragging and dropping a node ?
  point_t click_start;      // the coordinates where the drag and drop started
  point_t click_end;        // the coordinates where the drag and drop started
  dt_color_checker_t *checker;
  dt_solving_strategy_t optimization;
  float safety_margin;

  float homography[9];          // the perspective correction matrix
  float inverse_homography[9];  // The inverse perspective correction matrix
  gboolean run_profile;         // order a profiling at next pipeline recompute
  gboolean run_validation;      // order a profile validation at next pipeline recompute
  gboolean profile_ready;       // notify that a profile is ready to be applied
  gboolean checker_ready;       // notify that a checker bounding box is ready to be used
  gboolean is_blending;         // it this instance blending?
  dt_colormatrix_t mix;

  gboolean is_profiling_started;
  GtkWidget *checkers_list, *optimize, *safety, *label_delta_E, *button_profile, *button_validate, *button_commit;

  float *delta_E_in;

  gchar *delta_E_label_text;

  dt_gui_collapsible_section_t cs;
  dt_gui_collapsible_section_t csspot;

  GtkWidget *spot_settings, *spot_mode;
  GtkWidget *hue_spot, *chroma_spot, *lightness_spot;
  GtkWidget *origin_spot, *target_spot;
  GtkWidget *Lch_origin, *Lch_target;
  GtkWidget *use_mixing;
  dt_aligned_pixel_t spot_RGB;
  float last_daylight_temperature;
  float last_bb_temperature;
} dt_iop_channelmixer_rgb_gui_data_t;

typedef struct dt_iop_channelmixer_rbg_data_t
{
  dt_colormatrix_t MIX;
  float DT_ALIGNED_PIXEL saturation[CHANNEL_SIZE];
  float DT_ALIGNED_PIXEL lightness[CHANNEL_SIZE];
  float DT_ALIGNED_PIXEL grey[CHANNEL_SIZE];
  dt_aligned_pixel_t illuminant; // XYZ coordinates of illuminant
  float p, gamut;
  gboolean apply_grey;
  gboolean clip;
  dt_adaptation_t adaptation;
  dt_illuminant_t illuminant_type;
  dt_iop_channelmixer_rgb_version_t version;
} dt_iop_channelmixer_rbg_data_t;

typedef struct dt_iop_channelmixer_rgb_global_data_t
{
  int kernel_channelmixer_rgb_xyz;
  int kernel_channelmixer_rgb_cat16;
  int kernel_channelmixer_rgb_bradford_full;
  int kernel_channelmixer_rgb_bradford_linear;
  int kernel_channelmixer_rgb_rgb;
} dt_iop_channelmixer_rgb_global_data_t;


static void _auto_set_illuminant(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe);


const char *name()
{
  return _("color calibration");
}

const char *aliases()
{
  return _("channel mixer|white balance|monochrome");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("perform color space corrections\n"
                                        "such as white balance, channels mixing\n"
                                        "and conversions to monochrome emulating film"),
                                      _("corrective or creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("linear, RGB or XYZ"),
                                      _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_channelmixer_rgb_params_v3_t
  {
    /* params of v1 and v2 */
    float red[CHANNEL_SIZE];
    float green[CHANNEL_SIZE];
    float blue[CHANNEL_SIZE];
    float saturation[CHANNEL_SIZE];
    float lightness[CHANNEL_SIZE];
    float grey[CHANNEL_SIZE];
    gboolean normalize_R, normalize_G, normalize_B, normalize_sat, normalize_light, normalize_grey;
    dt_illuminant_t illuminant;
    dt_illuminant_fluo_t illum_fluo;
    dt_illuminant_led_t illum_led;
    dt_adaptation_t adaptation;
    float x, y;
    float temperature;
    float gamut;
    gboolean clip;

    /* params of v3 */
    dt_iop_channelmixer_rgb_version_t version;

    /* always add new params after this so we can import legacy params with memcpy on the common part of the struct */

  } dt_iop_channelmixer_rgb_params_v3_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_channelmixer_rgb_params_v1_t
    {
      float red[CHANNEL_SIZE];
      float green[CHANNEL_SIZE];
      float blue[CHANNEL_SIZE];
      float saturation[CHANNEL_SIZE];
      float lightness[CHANNEL_SIZE];
      float grey[CHANNEL_SIZE];
      gboolean normalize_R, normalize_G, normalize_B, normalize_sat, normalize_light, normalize_grey;
      dt_illuminant_t illuminant;
      dt_illuminant_fluo_t illum_fluo;
      dt_illuminant_led_t illum_led;
      dt_adaptation_t adaptation;
      float x, y;
      float temperature;
      float gamut;
      gboolean clip;
    } dt_iop_channelmixer_rgb_params_v1_t;

    const dt_iop_channelmixer_rgb_params_v1_t *o = old_params;
    dt_iop_channelmixer_rgb_params_v3_t *n =
      malloc(sizeof(dt_iop_channelmixer_rgb_params_v3_t));

    // V1 and V2 use the same param structure but the normalize_grey
    // param had no effect since commit_params forced normalization no
    // matter what. So we re-import the params and force the param to
    // TRUE to keep edits.
    memcpy(n, o, sizeof(dt_iop_channelmixer_rgb_params_v1_t));

    n->normalize_grey = TRUE;

    // V2 and V3 use the same param structure but these :

    // swap the saturation parameters for R and B to put them in natural order
    const float R = n->saturation[0];
    const float B = n->saturation[2];
    n->saturation[0] = B;
    n->saturation[2] = R;

    // say that these params were created with legacy code
    n->version = CHANNELMIXERRGB_V_1;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_channelmixer_rgb_params_v3_t);
    *new_version = 3;
    return 0;
  }
  if(old_version == 2)
  {
    typedef struct dt_iop_channelmixer_rgb_params_v2_t
    {
      float red[CHANNEL_SIZE];
      float green[CHANNEL_SIZE];
      float blue[CHANNEL_SIZE];
      float saturation[CHANNEL_SIZE];
      float lightness[CHANNEL_SIZE];
      float grey[CHANNEL_SIZE];
      gboolean normalize_R, normalize_G, normalize_B, normalize_sat, normalize_light, normalize_grey;
      dt_illuminant_t illuminant;
      dt_illuminant_fluo_t illum_fluo;
      dt_illuminant_led_t illum_led;
      dt_adaptation_t adaptation;
      float x, y;
      float temperature;
      float gamut;
      gboolean clip;
    } dt_iop_channelmixer_rgb_params_v2_t;

    const dt_iop_channelmixer_rgb_params_v2_t *o = old_params;
    dt_iop_channelmixer_rgb_params_v3_t *n =
      malloc(sizeof(dt_iop_channelmixer_rgb_params_v3_t));

    memcpy(n, o, sizeof(dt_iop_channelmixer_rgb_params_v2_t));

    // swap the saturation parameters for R and B to put them in natural order
    const float R = n->saturation[0];
    const float B = n->saturation[2];
    n->saturation[0] = B;
    n->saturation[2] = R;

    // say that these params were created with legacy code
    n->version = CHANNELMIXERRGB_V_1;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_channelmixer_rgb_params_v3_t);
    *new_version = 3;
    return 0;
  }
  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  // auto-applied scene-referred default
  self->pref_based_presets = TRUE;

  const gboolean is_scene_referred = dt_is_scene_referred();

  if(is_scene_referred)
  {
    dt_gui_presets_add_generic
      (_("scene-referred default"), self->op, self->version(),
       NULL, 0,
       1, DEVELOP_BLEND_CS_RGB_SCENE);

    dt_gui_presets_update_format(_("scene-referred default"), self->op,
                                 self->version(), FOR_MATRIX);

    dt_gui_presets_update_autoapply(_("scene-referred default"),
                                    self->op, self->version(), TRUE);
  }

  // others

  dt_iop_channelmixer_rgb_params_t p;
  memset(&p, 0, sizeof(p));

  p.version = CHANNELMIXERRGB_V_3;

  // bypass adaptation
  p.illuminant = DT_ILLUMINANT_PIPE;
  p.adaptation = DT_ADAPTATION_XYZ;

  // set everything to no-op
  p.gamut = 0.f;
  p.clip = FALSE;
  p.illum_fluo = DT_ILLUMINANT_FLUO_F3;
  p.illum_led = DT_ILLUMINANT_LED_B5;
  p.temperature = 5003.f;
  illuminant_to_xy(DT_ILLUMINANT_PIPE, NULL, NULL, &p.x, &p.y, p.temperature,
                   DT_ILLUMINANT_FLUO_LAST, DT_ILLUMINANT_LED_LAST);

  p.red[0] = 1.f;
  p.red[1] = 0.f;
  p.red[2] = 0.f;
  p.green[0] = 0.f;
  p.green[1] = 1.f;
  p.green[2] = 0.f;
  p.blue[0] = 0.f;
  p.blue[1] = 0.f;
  p.blue[2] = 1.f;

  p.saturation[0] = 0.f;
  p.saturation[1] = 0.f;
  p.saturation[2] = 0.f;
  p.lightness[0] = 0.f;
  p.lightness[1] = 0.f;
  p.lightness[2] = 0.f;
  p.grey[0] = 0.f;
  p.grey[1] = 0.f;
  p.grey[2] = 0.f;

  p.normalize_R = FALSE;
  p.normalize_G = FALSE;
  p.normalize_B = FALSE;
  p.normalize_sat = FALSE;
  p.normalize_light = FALSE;
  p.normalize_grey = TRUE;

  // Create B&W presets
  p.clip = TRUE;
  p.grey[0] = 0.f;
  p.grey[1] = 1.f;
  p.grey[2] = 0.f;

  dt_gui_presets_add_generic(_("B&W: luminance-based"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // film emulations

  /* These emulations are built using spectral sensitivities provided
  * by film manufacturers for tungsten light, corrected in spectral
  * domain for D50 illuminant, and integrated in spectral space
  * against CIE 2° 1931 XYZ color matching functions in the Python lib
  * Colour, with the following code :
  *
    import colour
    import numpy as np

    XYZ = np.zeros((3))

    for l in range(360, 830):
        XYZ += film_CMF[l]
          * colour.colorimetry.STANDARD_OBSERVERS_CMFS
              ['CIE 1931 2 Degree Standard Observer'][l]
                / colour.ILLUMINANTS_SDS['A'][l] * colour.ILLUMINANTS_SDS['D50'][l]

    XYZ / np.sum(XYZ)
  *
  * The film CMF is visually approximated from the graph. It is still
  * more accurate than bullshit factors in legacy channel mixer that
  * don't even say in which RGB space they are supposed to be applied.
  */

  // ILFORD HP5 +
  // https://www.ilfordphoto.com/amfile/file/download/file/1903/product/695/
  p.grey[0] = 0.25304098f;
  p.grey[1] = 0.25958747f;
  p.grey[2] = 0.48737156f;

  dt_gui_presets_add_generic(_("B&W: ILFORD HP5+"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // ILFORD Delta 100
  // https://www.ilfordphoto.com/amfile/file/download/file/3/product/681/
  p.grey[0] = 0.24552374f;
  p.grey[1] = 0.25366007f;
  p.grey[2] = 0.50081619f;

  dt_gui_presets_add_generic(_("B&W: ILFORD DELTA 100"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // ILFORD Delta 400 and 3200 - they have the same curve
  // https://www.ilfordphoto.com/amfile/file/download/file/1915/product/685/
  // https://www.ilfordphoto.com/amfile/file/download/file/1913/product/683/
  p.grey[0] = 0.24376712f;
  p.grey[1] = 0.23613559f;
  p.grey[2] = 0.52009729f;

  dt_gui_presets_add_generic(_("B&W: ILFORD DELTA 400 - 3200"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // ILFORD FP4+
  // https://www.ilfordphoto.com/amfile/file/download/file/1919/product/690/
  p.grey[0] = 0.24149085f;
  p.grey[1] = 0.22149272f;
  p.grey[2] = 0.53701643f;

  dt_gui_presets_add_generic(_("B&W: ILFORD FP4+"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Fuji Acros 100
  // https://dacnard.wordpress.com/2013/02/15/the-real-shades-of-gray-bw-film-is-a-matter-of-heart-pt-1/
  p.grey[0] = 0.333f;
  p.grey[1] = 0.313f;
  p.grey[2] = 0.353f;

  dt_gui_presets_add_generic(_("B&W: Fuji Acros 100"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Kodak ?
  // can't find spectral sensitivity curves and the illuminant under which they are produced,
  // so ¯\_(ツ)_/¯

  // basic channel-mixer
  p.adaptation = DT_ADAPTATION_RGB; // bypass adaptation
  p.grey[0] = 0.f;
  p.grey[1] = 0.f;
  p.grey[2] = 0.f;
  p.normalize_R = TRUE;
  p.normalize_G = TRUE;
  p.normalize_B = TRUE;
  p.normalize_grey = FALSE;
  p.clip = FALSE;
  dt_gui_presets_add_generic(_("basic channel mixer"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // swap G-B
  p.red[0] = 1.f;
  p.red[1] = 0.f;
  p.red[2] = 0.f;
  p.green[0] = 0.f;
  p.green[1] = 0.f;
  p.green[2] = 1.f;
  p.blue[0] = 0.f;
  p.blue[1] = 1.f;
  p.blue[2] = 0.f;
  dt_gui_presets_add_generic(_("swap G and B"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // swap G-R
  p.red[0] = 0.f;
  p.red[1] = 1.f;
  p.red[2] = 0.f;
  p.green[0] = 1.f;
  p.green[1] = 0.f;
  p.green[2] = 0.f;
  p.blue[0] = 0.f;
  p.blue[1] = 0.f;
  p.blue[2] = 1.f;
  dt_gui_presets_add_generic(_("swap G and R"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // swap R-B
  p.red[0] = 0.f;
  p.red[1] = 0.f;
  p.red[2] = 1.f;
  p.green[0] = 0.f;
  p.green[1] = 1.f;
  p.green[2] = 0.f;
  p.blue[0] = 1.f;
  p.blue[1] = 0.f;
  p.blue[2] = 0.f;
  dt_gui_presets_add_generic(_("swap R and B"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
}

static gboolean _area_mapping_active(const dt_iop_channelmixer_rgb_gui_data_t *g)
{
  return g  && g->spot_mode
            && dt_bauhaus_combobox_get(g->spot_mode) != DT_SPOT_MODE_MEASURE
            && ((g->lightness_spot && dt_bauhaus_slider_get_val(g->lightness_spot) != 50.0f) ||
                (g->chroma_spot && dt_bauhaus_slider_get_val(g->chroma_spot) != 0.0f));
}

static const char *_area_mapping_section_text(const dt_iop_channelmixer_rgb_gui_data_t *g)
{
  return _area_mapping_active(g) ? _("area color mapping (active)") : _("area color mapping");
}

static gboolean _get_white_balance_coeff(dt_iop_module_t *self,
                                         dt_aligned_pixel_t custom_wb)
{
  const dt_dev_chroma_t *chr = &self->dev->chroma;

  // Init output with a no-op
  for_four_channels(k)
    custom_wb[k] = 1.0f;

  if(!dt_image_is_matrix_correction_supported(&self->dev->image_storage))
    return TRUE;

  // If we use D65 there are unchanged corrections
  if(dt_dev_is_D65_chroma(self->dev))
    return FALSE;

  const gboolean valid_chroma =
    chr->D65coeffs[0] > 0.0 && chr->D65coeffs[1] > 0.0 && chr->D65coeffs[2] > 0.0;

  const gboolean changed_chroma =
    chr->wb_coeffs[0] > 1.0 || chr->wb_coeffs[1] > 1.0 || chr->wb_coeffs[2] > 1.0;

  // Otherwise - for example because the user made a correct preset, find the
  // WB adaptation ratio
  if(valid_chroma && changed_chroma)
  {
    for_four_channels(k)
      custom_wb[k] = chr->D65coeffs[k] / chr->wb_coeffs[k];
  }
  return FALSE;
}


DT_OMP_DECLARE_SIMD(aligned(input, output:16) uniform(compression, clip))
static inline void _gamut_mapping(const dt_aligned_pixel_t input,
                                  const float compression,
                                  const gboolean clip,
                                  dt_aligned_pixel_t output)
{
  // Get the sum XYZ
  const float sum = input[0] + input[1] + input[2];
  const float Y = input[1];

  // use chromaticity coordinates of reference white for sum == 0
  dt_aligned_pixel_t xyY = {  sum > 0.0f ? input[0] / sum : D50xyY.x,
                              sum > 0.0f ? input[1] / sum : D50xyY.y,
                              Y,
                              0.0f };

  // Convert to uvY
  dt_aligned_pixel_t uvY;
  dt_xyY_to_uvY(xyY, uvY);

  // Get the chromaticity difference with white point uv
  const float D50[2] DT_ALIGNED_PIXEL = { 0.20915914598542354f, 0.488075320769787f };
  const float delta[2] DT_ALIGNED_PIXEL = { D50[0] - uvY[0], D50[1] - uvY[1] };
  const float Delta = Y * (sqf(delta[0]) + sqf(delta[1]));

  // Compress chromaticity (move toward white point)
  const float correction = (compression == 0.0f) ? 0.f : powf(Delta, compression);
  for(size_t c = 0; c < 2; c++)
  {
    // Ensure the correction does not bring our uyY vector the other side of D50
    // that would switch to the opposite color, so we clip at D50
    // correction * delta[c] + uvY[c]
    const float tmp = DT_FMA(correction, delta[c], uvY[c]);
    uvY[c] = (uvY[c] > D50[c])  ? fmaxf(tmp, D50[c])
                                : fminf(tmp, D50[c]);
  }

  // Convert back to xyY
  dt_uvY_to_xyY(uvY, xyY);

  // Clip upon request
  if(clip) for(size_t c = 0; c < 2; c++) xyY[c] = fmaxf(xyY[c], 0.0f);

  // Check sanity of y
  // since we later divide by y, it can't be zero
  xyY[1] = fmaxf(xyY[1], NORM_MIN);

  // Check sanity of x and y :
  // since Z = Y (1 - x - y) / y, if x + y >= 1, Z will be negative
  const float scale = xyY[0] + xyY[1];
  const int sanitize = (scale >= 1.f);
  for(size_t c = 0; c < 2; c++) xyY[c] = (sanitize) ? xyY[c] / scale : xyY[c];

  // Convert back to XYZ
  dt_xyY_to_XYZ(xyY, output);
}


DT_OMP_DECLARE_SIMD(aligned(input, output, saturation, lightness:16) uniform(saturation, lightness))
static inline void _luma_chroma(const dt_aligned_pixel_t input,
                                const dt_aligned_pixel_t saturation,
                                const dt_aligned_pixel_t lightness,
                                dt_aligned_pixel_t output,
                                const dt_iop_channelmixer_rgb_version_t version)
{
  // Compute euclidean norm
  float norm = euclidean_norm(input);
  const float avg = fmaxf((input[0] + input[1] + input[2]) / 3.0f, NORM_MIN);

  if(norm > 0.f && avg > 0.f)
  {
    // Compute flat lightness adjustment
    const float mix = scalar_product(input, lightness);

    // Compensate the norm to get color ratios (R, G, B) = (1, 1, 1)
    // for grey (colorless) pixels.
    if(version == CHANNELMIXERRGB_V_3) norm *= INVERSE_SQRT_3;

    // Ratios
    for_three_channels(c)
      output[c] = input[c] / norm;

    // Compute ratios and a flat colorfulness adjustment for the whole pixel
    float coeff_ratio = 0.f;

    if(version == CHANNELMIXERRGB_V_1)
    {
      for_three_channels(c)
        coeff_ratio += sqf(1.0f - output[c]) * saturation[c];
    }
    else
      coeff_ratio = scalar_product(output, saturation) / 3.f;

    // Adjust the RGB ratios with the pixel correction
    for_three_channels(c)
    {
      // if the ratio was already invalid (negative), we accept the
      // result to be invalid too otherwise bright saturated blues end
      // up solid black
      const float min_ratio = (output[c] < 0.0f) ? output[c] : 0.0f;
      const float output_inverse = 1.0f - output[c];
      output[c] = fmaxf(DT_FMA(output_inverse, coeff_ratio, output[c]),
                        min_ratio); // output_inverse  * coeff_ratio + output
    }

    // The above interpolation between original pixel ratios and
    // (1, 1, 1) might change the norm of the ratios. Compensate for that.
    if(version == CHANNELMIXERRGB_V_3) norm /= euclidean_norm(output) * INVERSE_SQRT_3;

    // Apply colorfulness adjustment channel-wise and repack with
    // lightness to get LMS back
    norm *= fmaxf(1.f + mix / avg, 0.f);
    for_three_channels(c)
      output[c] *= norm;
  }
  else
  {
    // we have black, 0 stays 0, no luminance = no color
    for_three_channels(c)
      output[c] = input[c];
  }
}

DT_OMP_DECLARE_SIMD(aligned(in, out, XYZ_to_RGB, RGB_to_XYZ, MIX : 64) aligned(illuminant, saturation, lightness, grey:16))
static inline void _loop_switch(const float *const restrict in,
                                float *const restrict out,
                                const size_t width,
                                const size_t height,
                                const size_t ch,
                                const dt_colormatrix_t XYZ_to_RGB,
                                const dt_colormatrix_t RGB_to_XYZ,
                                const dt_colormatrix_t MIX,
                                const dt_aligned_pixel_t illuminant,
                                const dt_aligned_pixel_t saturation,
                                const dt_aligned_pixel_t lightness,
                                const dt_aligned_pixel_t grey,
                                const float p,
                                const float gamut,
                                const gboolean clip,
                                const gboolean apply_grey,
                                const dt_adaptation_t kind,
                                const dt_iop_channelmixer_rgb_version_t version)
{
  dt_colormatrix_t RGB_to_LMS = { { 0.0f, 0.0f, 0.0f, 0.0f } };
  dt_colormatrix_t MIX_to_XYZ = { { 0.0f, 0.0f, 0.0f, 0.0f } };
  switch (kind)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    case DT_ADAPTATION_LINEAR_BRADFORD:
      make_RGB_to_Bradford_LMS(RGB_to_XYZ, RGB_to_LMS);
      make_Bradford_LMS_to_XYZ(MIX, MIX_to_XYZ);
      break;
    case DT_ADAPTATION_CAT16:
      make_RGB_to_CAT16_LMS(RGB_to_XYZ, RGB_to_LMS);
      make_CAT16_LMS_to_XYZ(MIX, MIX_to_XYZ);
      break;
    case DT_ADAPTATION_XYZ:
      dt_colormatrix_copy(RGB_to_LMS, RGB_to_XYZ);
      dt_colormatrix_copy(MIX_to_XYZ, MIX);
      break;
    case DT_ADAPTATION_RGB:
    case DT_ADAPTATION_LAST:
    default:
      // RGB_to_LMS not applied, since we are not adapting WB
      dt_colormatrix_mul(MIX_to_XYZ, RGB_to_XYZ, MIX);
      break;
  }
  const float minval = clip ? 0.0f : -FLT_MAX;
  const dt_aligned_pixel_t min_value = { minval, minval, minval, minval };

  dt_colormatrix_t RGB_to_XYZ_trans;
  dt_colormatrix_transpose(RGB_to_XYZ_trans, RGB_to_XYZ);
  dt_colormatrix_t RGB_to_LMS_trans;
  dt_colormatrix_transpose(RGB_to_LMS_trans, RGB_to_LMS);
  dt_colormatrix_t MIX_to_XYZ_trans;
  dt_colormatrix_transpose(MIX_to_XYZ_trans, MIX_to_XYZ);
  dt_colormatrix_t XYZ_to_RGB_trans;
  dt_colormatrix_transpose(XYZ_to_RGB_trans, XYZ_to_RGB);

  DT_OMP_FOR()
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    // intermediate temp buffers
    dt_aligned_pixel_t temp_one;
    dt_aligned_pixel_t temp_two;

    dt_vector_max_nan(temp_two, &in[k], min_value);

    /* WE START IN PIPELINE RGB */

    switch(kind)
    {
      case DT_ADAPTATION_FULL_BRADFORD:
      {
        // Convert from RGB to XYZ
        dt_apply_transposed_color_matrix(temp_two, RGB_to_XYZ_trans, temp_one);
        const float Y = temp_one[1];

        // Convert to LMS
        convert_XYZ_to_bradford_LMS(temp_one, temp_two);
        // Do white balance
        downscale_vector(temp_two, Y);
        bradford_adapt_D50(temp_two, illuminant, p, TRUE, temp_one);
        upscale_vector(temp_one, Y);
        copy_pixel(temp_two, temp_one);
        break;
      }
      case DT_ADAPTATION_LINEAR_BRADFORD:
      {
        // Convert from RGB to XYZ to LMS
        dt_apply_transposed_color_matrix(temp_two, RGB_to_LMS_trans, temp_one);

        // Do white balance
        bradford_adapt_D50(temp_one, illuminant, p, FALSE, temp_two);
        break;
      }
      case DT_ADAPTATION_CAT16:
      {
        // Convert from RGB to XYZ
        dt_apply_transposed_color_matrix(temp_two, RGB_to_LMS_trans, temp_one);

        // Do white balance
        // force full-adaptation
        CAT16_adapt_D50(temp_one, illuminant, 1.0f, TRUE, temp_two);
        break;
      }
      case DT_ADAPTATION_XYZ:
      {
        // Convert from RGB to XYZ
        dt_apply_transposed_color_matrix(temp_two, RGB_to_XYZ_trans, temp_one);

        // Do white balance in XYZ
        XYZ_adapt_D50(temp_one, illuminant, temp_two);
        break;
      }
      case DT_ADAPTATION_RGB:
      case DT_ADAPTATION_LAST:
      default:
      {
        // No white balance.
        for_four_channels(c)
          temp_one[c] = 0.0f; //keep compiler happy by ensuring that always initialized
      }
    }

    // Compute the 3D mix - this is a rotation + homothety of the vector base
    dt_apply_transposed_color_matrix(temp_two, MIX_to_XYZ_trans, temp_one);

    /* FROM HERE WE ARE MANDATORILY IN XYZ - DATA IS IN temp_one */

    // Gamut mapping happens in XYZ space no matter what, only 0->1 values are defined
    // for this
    if(clip)
      dt_vector_clipneg_nan(temp_one);
    _gamut_mapping(temp_one, gamut, clip, temp_two);

    // convert to LMS, XYZ or pipeline RGB
    switch(kind)
    {
      case DT_ADAPTATION_FULL_BRADFORD:
      case DT_ADAPTATION_LINEAR_BRADFORD:
      case DT_ADAPTATION_CAT16:
      case DT_ADAPTATION_XYZ:
      {
        convert_any_XYZ_to_LMS(temp_two, temp_one, kind);
        break;
      }
      case DT_ADAPTATION_RGB:
      case DT_ADAPTATION_LAST:
      default:
      {
        // Convert from XYZ to RGB
        dt_apply_transposed_color_matrix(temp_two, XYZ_to_RGB_trans, temp_one);
        break;
      }
    }

    /* FROM HERE WE ARE IN LMS, XYZ OR PIPELINE RGB depending on user
       param - DATA IS IN temp_one */

    // Clip in LMS
    if(clip)
      dt_vector_clipneg_nan(temp_one);

    // Apply lightness / saturation adjustment
    _luma_chroma(temp_one, saturation, lightness, temp_two, version);

    // Clip in LMS
    if(clip)
      dt_vector_clipneg_nan(temp_two);

    // Save
    if(apply_grey)
    {
      // Turn LMS, XYZ or pipeline RGB into monochrome
      const float grey_mix = fmaxf(scalar_product(temp_two, grey), 0.0f);
      temp_two[0] = temp_two[1] = temp_two[2] = grey_mix;
    }
    else
    {
      // Convert back to XYZ
      switch(kind)
      {
        case DT_ADAPTATION_FULL_BRADFORD:
        case DT_ADAPTATION_LINEAR_BRADFORD:
        case DT_ADAPTATION_CAT16:
        case DT_ADAPTATION_XYZ:
        {
          convert_any_LMS_to_XYZ(temp_two, temp_one, kind);
          break;
        }
        case DT_ADAPTATION_RGB:
        case DT_ADAPTATION_LAST:
        default:
        {
          // Convert from RBG to XYZ
          dt_apply_transposed_color_matrix(temp_two, RGB_to_XYZ_trans, temp_one);
          break;
        }
      }

      /* FROM HERE WE ARE MANDATORILY IN XYZ - DATA IS IN temp_one */

      // Clip in XYZ
      if(clip)
        dt_vector_clipneg_nan(temp_one);

      // Convert back to RGB
      dt_apply_transposed_color_matrix(temp_one, XYZ_to_RGB_trans, temp_two);

      if(clip)
        dt_vector_clipneg_nan(temp_two);
    }

    temp_two[3] = in[k + 3]; // alpha mask
    copy_pixel_nontemporal(&out[k], temp_two);
  }
}

// util to shift pixel index without headache
#define SHF(ii, jj, c) ((i + ii) * width + j + jj) * ch + c
#define OFF 4


#ifdef AI_ACTIVATED

#if defined(__GNUC__) && defined(_WIN32)
  // On Windows there is a rounding issue making the image full
  // black. For a discussion about the issue and tested solutions see
  // PR #12382).
  #pragma GCC push_options
  #pragma GCC optimize ("-fno-finite-math-only")
#endif

static inline void _auto_detect_WB(const float *const restrict in,
                                   float *const restrict temp,
                                   dt_illuminant_t illuminant,
                                   const size_t width,
                                   const size_t height,
                                   const size_t ch,
                                   const dt_colormatrix_t RGB_to_XYZ,
                                   dt_aligned_pixel_t xyz)
{
   /* Detect the chromaticity of the illuminant based on the grey edges hypothesis.
      So we compute a laplacian filter and get the weighted average of its chromaticities

      Inspired by :
      A Fast White Balance Algorithm Based on Pixel Greyness, Ba Thai·Guang Deng·Robert Ross
      https://www.researchgate.net/profile/Ba_Son_Thai/publication/308692177_A_Fast_White_Balance_Algorithm_Based_on_Pixel_Greyness/

      Edge-Based Color Constancy, Joost van de Weijer, Theo Gevers, Arjan Gijsenij
      https://hal.inria.fr/inria-00548686/document
    */
    const float D50[2] = { D50xyY.x, D50xyY.y };
// Convert RGB to xy
  DT_OMP_FOR(collapse(2))
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t index = (i * width + j) * ch;
      dt_aligned_pixel_t RGB;
      dt_aligned_pixel_t XYZ;

      // Clip negatives
      for_each_channel(c,aligned(in))
        RGB[c] = fmaxf(in[index + c], 0.0f);

      // Convert to XYZ
      dot_product(RGB, RGB_to_XYZ, XYZ);

      // Convert to xyY
      const float sum = fmaxf(XYZ[0] + XYZ[1] + XYZ[2], NORM_MIN);
      XYZ[0] /= sum;   // x
      XYZ[2] = XYZ[1]; // Y
      XYZ[1] /= sum;   // y

      // Shift the chromaticity plane so the D50 point (target) becomes the origin
      const float norm = dt_fast_hypotf(D50[0], D50[1]);

      temp[index    ] = (XYZ[0] - D50[0]) / norm;
      temp[index + 1] = (XYZ[1] - D50[1]) / norm;
      temp[index + 2] =  XYZ[2];
    }

  float elements = 0.f;
  dt_aligned_pixel_t xyY = { 0.f };

  if(illuminant == DT_ILLUMINANT_DETECT_SURFACES)
  {
    DT_OMP_FOR(reduction(+:xyY, elements))
    for(size_t i = 2 * OFF; i < height - 4 * OFF; i += OFF)
      for(size_t j = 2 * OFF; j < width - 4 * OFF; j += OFF)
      {
        float DT_ALIGNED_PIXEL central_average[2];

        #pragma unroll
        for(size_t c = 0; c < 2; c++)
        {
          // B-spline local average / blur
          central_average[c] = (temp[SHF(-OFF, -OFF, c)]
                                + 2.f * temp[SHF(-OFF, 0, c)]
                                + temp[SHF(-OFF, +OFF, c)]
                                + 2.f * temp[SHF(   0, -OFF, c)]
                                + 4.f * temp[SHF(   0, 0, c)]
                                + 2.f * temp[SHF(   0, +OFF, c)]
                                + temp[SHF(+OFF, -OFF, c)]
                                + 2.f * temp[SHF(+OFF, 0, c)]
                                + temp[SHF(+OFF, +OFF, c)]) / 16.0f;
          central_average[c] = fmaxf(central_average[c], 0.0f);
        }

        dt_aligned_pixel_t var = { 0.f };

        // compute patch-wise variance
        // If variance = 0, we are on a flat surface and want to discard that patch.
        #pragma unroll
        for(size_t c = 0; c < 2; c++)
        {
          var[c] = (  sqf(temp[SHF(-OFF, -OFF, c)] - central_average[c])
                    + sqf(temp[SHF(-OFF,    0, c)] - central_average[c])
                    + sqf(temp[SHF(-OFF, +OFF, c)] - central_average[c])
                    + sqf(temp[SHF(0,    -OFF, c)] - central_average[c])
                    + sqf(temp[SHF(0,       0, c)] - central_average[c])
                    + sqf(temp[SHF(0,    +OFF, c)] - central_average[c])
                    + sqf(temp[SHF(+OFF, -OFF, c)] - central_average[c])
                    + sqf(temp[SHF(+OFF,    0, c)] - central_average[c])
                    + sqf(temp[SHF(+OFF, +OFF, c)] - central_average[c])
                    ) / 9.0f;
        }

        // Compute the patch-wise chroma covariance.
        // If covariance = 0, chroma channels are not correlated and we either have noise or chromatic aberrations.
        // Both ways, we want to discard that patch from the chroma average.
        var[2] = ((temp[SHF(-OFF, -OFF, 0)] - central_average[0]) * (temp[SHF(-OFF, -OFF, 1)] - central_average[1]) +
                  (temp[SHF(-OFF,    0, 0)] - central_average[0]) * (temp[SHF(-OFF,    0, 1)] - central_average[1]) +
                  (temp[SHF(-OFF, +OFF, 0)] - central_average[0]) * (temp[SHF(-OFF, +OFF, 1)] - central_average[1]) +
                  (temp[SHF(   0, -OFF, 0)] - central_average[0]) * (temp[SHF(   0, -OFF, 1)] - central_average[1]) +
                  (temp[SHF(   0,    0, 0)] - central_average[0]) * (temp[SHF(   0,    0, 1)] - central_average[1]) +
                  (temp[SHF(   0, +OFF, 0)] - central_average[0]) * (temp[SHF(   0, +OFF, 1)] - central_average[1]) +
                  (temp[SHF(+OFF, -OFF, 0)] - central_average[0]) * (temp[SHF(+OFF, -OFF, 1)] - central_average[1]) +
                  (temp[SHF(+OFF,    0, 0)] - central_average[0]) * (temp[SHF(+OFF,    0, 1)] - central_average[1]) +
                  (temp[SHF(+OFF, +OFF, 0)] - central_average[0]) * (temp[SHF(+OFF, +OFF, 1)] - central_average[1])
          ) / 9.0f;

        // Compute the Minkowski p-norm for regularization
        const float p = 8.f;
        const float p_norm
            = powf(powf(fabsf(central_average[0]), p)
                   + powf(fabsf(central_average[1]), p), 1.f / p) + NORM_MIN;
        const float weight = var[0] * var[1] * var[2];

        #pragma unroll
        for(size_t c = 0; c < 2; c++) xyY[c] += central_average[c] * weight / p_norm;
        elements += weight / p_norm;
      }
  }
  else if(illuminant == DT_ILLUMINANT_DETECT_EDGES)
  {
    DT_OMP_FOR(reduction(+:xyY, elements))
    for(size_t i = 2 * OFF; i < height - 4 * OFF; i += OFF)
      for(size_t j = 2 * OFF; j < width - 4 * OFF; j += OFF)
      {
        float DT_ALIGNED_PIXEL dd[2];
        float DT_ALIGNED_PIXEL central_average[2];

        #pragma unroll
        for(size_t c = 0; c < 2; c++)
        {
          // B-spline local average / blur
          central_average[c] = (temp[SHF(-OFF, -OFF, c)]
                                + 2.f * temp[SHF(-OFF, 0, c)]
                                + temp[SHF(-OFF, +OFF, c)]
                                + 2.f * temp[SHF(   0, -OFF, c)]
                                + 4.f * temp[SHF(   0, 0, c)]
                                + 2.f * temp[SHF(   0, +OFF, c)]
                                + temp[SHF(+OFF, -OFF, c)]
                                + 2.f * temp[SHF(+OFF, 0, c)]
                                + temp[SHF(+OFF, +OFF, c)]) / 16.0f;

          // image - blur = laplacian = edges
          dd[c] = temp[SHF(0, 0, c)] - central_average[c];
        }

        // Compute the Minkowski p-norm for regularization
        const float p = 8.f;
        const float p_norm = powf(powf(fabsf(dd[0]), p)
                                  + powf(fabsf(dd[1]), p), 1.f / p) + NORM_MIN;

#pragma unroll
        for(size_t c = 0; c < 2; c++) xyY[c] -= dd[c] / p_norm;
        elements += 1.f;
      }
  }

  const float norm_D50 = dt_fast_hypotf(D50[0], D50[1]);

  for(size_t c = 0; c < 2; c++)
    xyz[c] = norm_D50 * (xyY[c] / elements) + D50[c];
}

#if defined(__GNUC__) && defined(_WIN32)
  #pragma GCC pop_options
#endif

#endif // AI_ACTIVATED

static void _declare_cat_on_pipe(dt_iop_module_t *self, const gboolean preset)
{
  // Avertise in dev->chroma that we are doing chromatic adaptation here
  // preset = TRUE allows to capture the CAT a priori at init time
  const dt_iop_channelmixer_rgb_params_t *p = self->params;
  const dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  if(!g) return;

  dt_dev_chroma_t *chr = &self->dev->chroma;
  const dt_iop_module_t *origcat = chr->adaptation;

  if(preset
    || (self->enabled
        && !g->is_blending
        && !(p->adaptation == DT_ADAPTATION_RGB || p->illuminant == DT_ILLUMINANT_PIPE)))
  {
    // We do CAT here so we need to register this instance as CAT-handler.
    if(chr->adaptation == NULL)
    {
      // We are the first to try to register, let's go !
      chr->adaptation = self;
    }
    else if(chr->adaptation == self)
    {
    }
    else
    {
      // Another instance already registered.
      // If we are lower in the pipe than it, register in its place.
      if(dt_iop_is_first_instance(self->dev->iop, self))
        chr->adaptation = self;
    }
  }

  if(origcat != chr->adaptation)
    dt_print(DT_DEBUG_PIPE, "changed CAT for %s%s from %p to %p",
      self->op, dt_iop_get_instance_id(self), origcat, chr->adaptation);
}

static void _update_illuminants(dt_iop_module_t *self);
static void _update_approx_cct(dt_iop_module_t *self);
static void _update_illuminant_color(dt_iop_module_t *self);

static void _check_if_close_to_daylight(const float x,
                                        const float y,
                                        float *temperature,
                                        dt_illuminant_t *illuminant,
                                        dt_adaptation_t *adaptation)
{
  /* Check if a chromaticity x, y is close to daylight within 2.5 % error margin.
   * If so, we enable the daylight GUI for better ergonomics
   * Otherwise, we default to direct x, y control for better accuracy
   *
   * Note : The use of CCT is discouraged if dE > 5 % in CIE 1960 Yuv space
   *        reference : https://onlinelibrary.wiley.com/doi/abs/10.1002/9780470175637.ch3
   */

  // Get the correlated color temperature (CCT)
  float t = xy_to_CCT(x, y);

  // xy_to_CCT is valid only in 3000 - 25000 K. We need another model below
  if(t < 3000.f && t > 1667.f)
    t = CCT_reverse_lookup(x, y);

  if(temperature)
    *temperature = t;

  // Convert to CIE 1960 Yuv space
  float xy_ref[2] = { x, y };
  float uv_ref[2];
  xy_to_uv(xy_ref, uv_ref);

  float xy_test[2] = { 0.f };
  float uv_test[2];

  // Compute the test chromaticity from the daylight model
  illuminant_to_xy(DT_ILLUMINANT_D, NULL, NULL, &xy_test[0], &xy_test[1], t,
                   DT_ILLUMINANT_FLUO_LAST, DT_ILLUMINANT_LED_LAST);
  xy_to_uv(xy_test, uv_test);

  // Compute the error between the reference illuminant and the test
  // illuminant derivated from the CCT with daylight model
  const float delta_daylight = dt_fast_hypotf(uv_test[0] - uv_ref[0], uv_test[1] - uv_ref[1]);

  // Compute the test chromaticity from the blackbody model
  illuminant_to_xy(DT_ILLUMINANT_BB, NULL, NULL, &xy_test[0], &xy_test[1], t,
                   DT_ILLUMINANT_FLUO_LAST, DT_ILLUMINANT_LED_LAST);
  xy_to_uv(xy_test, uv_test);

  // Compute the error between the reference illuminant and the test
  // illuminant derivated from the CCT with black body model
  const float delta_bb = dt_fast_hypotf(uv_test[0] - uv_ref[0], uv_test[1] - uv_ref[1]);

  // Check the error between original and test chromaticity
  if(delta_bb < 0.005f || delta_daylight < 0.005f)
  {
    if(illuminant)
    {
      if(delta_bb < delta_daylight)
        *illuminant = DT_ILLUMINANT_BB;
      else
        *illuminant = DT_ILLUMINANT_D;
    }
  }
  else
  {
    // error is too big to use a CCT-based model, we fall back to a
    // custom/freestyle chroma selection for the illuminant
    if(illuminant) *illuminant = DT_ILLUMINANT_CUSTOM;
  }

  // CAT16 is more accurate no matter the illuminant
  if(adaptation) *adaptation = DT_ADAPTATION_CAT16;
}

#define DEG_TO_RAD(x) (x * M_PI / 180.f)
#define RAD_TO_DEG(x) (x * 180.f / M_PI)

static inline void _compute_patches_delta_E(const float *const restrict patches,
                                            const dt_color_checker_t *const checker,
                                            float *const restrict delta_E,
                                            float *const restrict avg_delta_E,
                                            float *const restrict max_delta_E)
{
  // Compute the delta E

  float dE = 0.f;
  float max_dE = 0.f;

  for(size_t k = 0; k < checker->patches; k++)
  {
    // Convert to Lab
    dt_aligned_pixel_t Lab_test;
    dt_aligned_pixel_t XYZ_test;

    // If exposure was normalized, denormalized it before
    for(size_t c = 0; c < 4; c++) XYZ_test[c] = patches[k * 4 + c];
    dt_XYZ_to_Lab(XYZ_test, Lab_test);

    const float *const restrict Lab_ref = checker->values[k].Lab;

    // Compute delta E 2000 to make your computer heat
    // ref: https://en.wikipedia.org/wiki/Color_difference#CIEDE2000
    // note : it will only be luck if I didn't mess-up the computation somewhere
    const float DL = Lab_ref[0] - Lab_test[0];
    const float L_avg = (Lab_ref[0] + Lab_test[0]) / 2.f;
    const float C_ref = dt_fast_hypotf(Lab_ref[1], Lab_ref[2]);
    const float C_test = dt_fast_hypotf(Lab_test[1], Lab_test[2]);
    const float C_avg = (C_ref + C_test) / 2.f;
    float C_avg_7 = C_avg * C_avg; // C_avg²
    C_avg_7 *= C_avg_7;            // C_avg⁴
    C_avg_7 *= C_avg_7;            // C_avg⁸
    C_avg_7 /= C_avg;              // C_avg⁷
    // 25⁷ = 6103515625
    const float C_avg_7_ratio_sqrt = sqrtf(C_avg_7 / (C_avg_7 + 6103515625.f));
    const float a_ref_prime = Lab_ref[1] * (1.f + 0.5f * (1.f - C_avg_7_ratio_sqrt));
    const float a_test_prime = Lab_test[1] * (1.f + 0.5f * (1.f - C_avg_7_ratio_sqrt));
    const float C_ref_prime = dt_fast_hypotf(a_ref_prime, Lab_ref[2]);
    const float C_test_prime = dt_fast_hypotf(a_test_prime, Lab_test[2]);
    const float DC_prime = C_ref_prime - C_test_prime;
    const float C_avg_prime = (C_ref_prime + C_test_prime) / 2.f;
    float h_ref_prime = atan2f(Lab_ref[2], a_ref_prime);
    float h_test_prime = atan2f(Lab_test[2], a_test_prime);

    // Comply with recommendations, h = 0° where C = 0 by convention
    if(C_ref_prime == 0.f) h_ref_prime = 0.f;
    if(C_test_prime == 0.f) h_test_prime = 0.f;

    // Get the hue angles from [-pi ; pi] back to [0 ; 2 pi],
    // again, to comply with specifications
    if(h_ref_prime < 0.f) h_ref_prime = 2.f * M_PI - h_ref_prime;
    if(h_test_prime < 0.f) h_test_prime = 2.f * M_PI - h_test_prime;

    // Convert to degrees, again to comply with specs
    h_ref_prime = RAD_TO_DEG(h_ref_prime);
    h_test_prime = RAD_TO_DEG(h_test_prime);

    float Dh_prime = h_test_prime - h_ref_prime;
    float Dh_prime_abs = fabsf(Dh_prime);
    if(C_test_prime == 0.f || C_ref_prime == 0.f)
      Dh_prime = 0.f;
    else if(Dh_prime_abs <= 180.f)
      ;
    else if(Dh_prime_abs > 180.f && (h_test_prime <= h_ref_prime))
      Dh_prime += 360.f;
    else if(Dh_prime_abs > 180.f && (h_test_prime > h_ref_prime))
      Dh_prime -= 360.f;

    // update abs(Dh_prime) for later
    Dh_prime_abs = fabsf(Dh_prime);

    const float DH_prime =
      2.f * sqrtf(C_test_prime * C_ref_prime) * sinf(DEG_TO_RAD(Dh_prime) / 2.f);

    float H_avg_prime = h_ref_prime + h_test_prime;
    if(C_test_prime == 0.f || C_ref_prime == 0.f)
      ;
    else if(Dh_prime_abs <= 180.f)
      H_avg_prime /= 2.f;
    else if(Dh_prime_abs > 180.f && (H_avg_prime < 360.f))
      H_avg_prime = (H_avg_prime + 360.f) / 2.f;
    else if(Dh_prime_abs > 180.f && (H_avg_prime >= 360.f))
      H_avg_prime = (H_avg_prime - 360.f) / 2.f;

    const float T = 1.f
                    - 0.17f * cosf(DEG_TO_RAD(H_avg_prime) - DEG_TO_RAD(30.f))
                    + 0.24f * cosf(2.f * DEG_TO_RAD(H_avg_prime))
                    + 0.32f * cosf(3.f * DEG_TO_RAD(H_avg_prime) + DEG_TO_RAD(6.f))
                    - 0.20f * cosf(4.f * DEG_TO_RAD(H_avg_prime) - DEG_TO_RAD(63.f));

    const float S_L = 1.f + (0.015f * sqf(L_avg - 50.f)) / sqrtf(20.f + sqf(L_avg - 50.f));
    const float S_C = 1.f + 0.045f * C_avg_prime;
    const float S_H = 1.f + 0.015f * C_avg_prime * T;
    const float R_T = -2.f * C_avg_7_ratio_sqrt
                      * sinf(DEG_TO_RAD(60.f) * expf(-sqf((H_avg_prime - 275.f) / 25.f)));

    // roll the drum, here goes the Delta E, finally…
    const float DE = sqrtf(sqf(DL / S_L) + sqf(DC_prime / S_C) + sqf(DH_prime / S_H)
                           + R_T * (DC_prime / S_C) * (DH_prime / S_H));

    // Delta E 1976 for reference :
    //float DE = sqrtf(sqf(Lab_test[0] - Lab_ref[0]) + sqf(Lab_test[1] - Lab_ref[1]) + sqf(Lab_test[2] - Lab_ref[2]));

    //fprintf(stdout, "patch %s : Lab ref \t= \t%.3f \t%.3f \t%.3f \n", checker->values[k].name, Lab_ref[0], Lab_ref[1], Lab_ref[2]);
    //fprintf(stdout, "patch %s : Lab mes \t= \t%.3f \t%.3f \t%.3f \n", checker->values[k].name, Lab_test[0], Lab_test[1], Lab_test[2]);
    //fprintf(stdout, "patch %s : dE mes \t= \t%.3f \n", checker->values[k].name, DE);

    delta_E[k] = DE;
    dE += DE / (float)checker->patches;
    if(DE > max_dE) max_dE = DE;
  }

  *avg_delta_E = dE;
  *max_delta_E = max_dE;
}

#define GET_WEIGHT                                                \
      float hue = atan2f(reference[2], reference[1]);             \
      const float chroma = hypotf(reference[2], reference[1]);    \
      float delta_hue = hue - ref_hue;                            \
      if(chroma == 0.f)                                           \
        delta_hue = 0.f;                                          \
      else if(fabsf(delta_hue) <= M_PI)                           \
        ;                                                         \
      else if(fabsf(delta_hue) > M_PI && (hue <= ref_hue))        \
        delta_hue += 2.f * M_PI;                                  \
      else if(fabsf(delta_hue) > M_PI && (hue > ref_hue))         \
        delta_hue -= 2.f * M_PI;                                  \
      w = sqrtf(expf(-sqf(delta_hue) / 2.f));


typedef struct {
  float black;
  float exposure;
} extraction_result_t;

static const extraction_result_t _extract_patches(const float *const restrict in,
                                                  const dt_iop_roi_t *const roi_in,
                                                  dt_iop_channelmixer_rgb_gui_data_t *g,
                                                  const dt_colormatrix_t RGB_to_XYZ,
                                                  const dt_colormatrix_t XYZ_to_CAM,
                                                  float *const restrict patches,
                                                  const gboolean normalize_exposure)
{
  const size_t width = roi_in->width;
  const size_t height = roi_in->height;
  const float radius_x =
    g->checker->radius * hypotf(1.f, g->checker->ratio) * g->safety_margin;
  const float radius_y = radius_x / g->checker->ratio;

  if(g->delta_E_in == NULL)
    g->delta_E_in = dt_alloc_align_float(g->checker->patches);

  /* Get the average color over each patch */
  for(size_t k = 0; k < g->checker->patches; k++)
  {
    // center of the patch in the ideal reference
    const point_t center = { g->checker->values[k].x, g->checker->values[k].y };

    // corners of the patch in the ideal reference
    const point_t corners[4] = { {center.x - radius_x, center.y - radius_y},
                                 {center.x + radius_x, center.y - radius_y},
                                 {center.x + radius_x, center.y + radius_y},
                                 {center.x - radius_x, center.y + radius_y} };

    // apply patch coordinates transform depending on perspective
    point_t new_corners[4];
    // find the bounding box of the patch at the same time
    size_t x_min = width - 1;
    size_t x_max = 0;
    size_t y_min = height - 1;
    size_t y_max = 0;
    for(size_t c = 0; c < 4; c++)
    {
      new_corners[c] = apply_homography(corners[c], g->homography);
      x_min = fminf(new_corners[c].x, x_min);
      x_max = fmaxf(new_corners[c].x, x_max);
      y_min = fminf(new_corners[c].y, y_min);
      y_max = fmaxf(new_corners[c].y, y_max);
    }

    x_min = CLAMP((size_t)floorf(x_min), 0, width - 1);
    x_max = CLAMP((size_t)ceilf(x_max), 0, width - 1);
    y_min = CLAMP((size_t)floorf(y_min), 0, height - 1);
    y_max = CLAMP((size_t)ceilf(y_max), 0, height - 1);

    // Get the average color on the patch
    patches[k * 4] = patches[k * 4 + 1] = patches[k * 4 + 2] = patches[k * 4 + 3] = 0.f;
    size_t num_elem = 0;

    // Loop through the rectangular bounding box
    for(size_t j = y_min; j < y_max; j++)
      for(size_t i = x_min; i < x_max; i++)
      {
        // Check if this pixel lies inside the sampling area and sample if it does
        point_t current_point = { i + 0.5f, j + 0.5f };
        current_point = apply_homography(current_point, g->inverse_homography);
        current_point.x -= center.x;
        current_point.y -= center.y;

        if(current_point.x < radius_x && current_point.x > -radius_x &&
           current_point.y < radius_y && current_point.y > -radius_y)
        {
          for_three_channels(c)
          {
            patches[k * 4 + c] += in[(j * width + i) * 4 + c];

            // Debug : inpaint a black square in the preview to ensure the coordanites of
            // overlay drawings and actual pixel processing match
            // out[(j * width + i) * 4 + c] = 0.f;
          }
          num_elem++;
        }
      }

    for_three_channels(c)
      patches[k * 4 + c] /= (float)num_elem;

    // Convert to XYZ
    dt_aligned_pixel_t XYZ = { 0 };
    dot_product(patches + k * 4, RGB_to_XYZ, XYZ);
    for(size_t c = 0; c < 3; c++) patches[k * 4 + c] = XYZ[c];
  }

  // find reference white patch
  dt_aligned_pixel_t XYZ_white_ref;
  dt_Lab_to_XYZ(g->checker->values[g->checker->white].Lab, XYZ_white_ref);
  const float white_ref_norm = euclidean_norm(XYZ_white_ref);

  // find test white patch
  dt_aligned_pixel_t XYZ_white_test;
  for_three_channels(c)
    XYZ_white_test[c] = patches[g->checker->white * 4 + c];
  const float white_test_norm = euclidean_norm(XYZ_white_test);

  /* match global exposure */
  // white exposure depends on camera settings and raw white point,
  // we want our profile to be independent from that
  float exposure = white_ref_norm / white_test_norm;

  /* Exposure compensation */
  // Ensure the relative luminance of the test patch (compared to white patch)
  // is the same as the relative luminance of the reference patch.
  // This compensate for lighting fall-off and unevenness
  if(normalize_exposure)
  {
    for(size_t k = 0; k < g->checker->patches; k++)
    {
      float *const sample = patches + k * 4;

      dt_aligned_pixel_t XYZ_ref;
      dt_Lab_to_XYZ(g->checker->values[k].Lab, XYZ_ref);

      const float sample_norm = euclidean_norm(sample);
      const float ref_norm = euclidean_norm(XYZ_ref);

      const float relative_luminance_test = sample_norm / white_test_norm;
      const float relative_luminance_ref = ref_norm / white_ref_norm;

      const float luma_correction = relative_luminance_ref / relative_luminance_test;
      for_three_channels(c)
        sample[c] *= luma_correction * exposure;
    }
  }

  // black point is evaluated by rawspeed on each picture using the dark pixels
  // we want our profile to be also independent from its discrepancies
  // so we convert back the patches to camera RGB space and search the best fit of
  // RGB_ref = exposure * (RGB_test - offset) for offset.
  float black = 0.f;
  const float user_exposure = exp2f(dt_dev_exposure_get_exposure(darktable.develop));
  const float user_black = dt_dev_exposure_get_black(darktable.develop);

  if(XYZ_to_CAM)
  {
    float mean_ref = 0.f;
    float mean_test = 0.f;

    for(size_t k = 0; k < g->checker->patches; k++)
    {
      dt_aligned_pixel_t XYZ_ref, RGB_ref;
      dt_aligned_pixel_t XYZ_test, RGB_test;

      for_three_channels(c)
        XYZ_test[c] = patches[k * 4 + c];
      dt_Lab_to_XYZ(g->checker->values[k].Lab, XYZ_ref);

      dot_product(XYZ_test, XYZ_to_CAM, RGB_test);
      dot_product(XYZ_ref, XYZ_to_CAM, RGB_ref);

      // Undo exposure module settings
      for_three_channels(c)
        RGB_test[c] = RGB_test[c] / user_exposure / exposure + user_black;

      // From now on, we have all the reference and test data in camera RGB space
      // where exposure and black level are applied

      for_three_channels(c)
      {
        mean_test += RGB_test[c];
        mean_ref += RGB_ref[c];
      }
    }
    mean_test /= 3.f * g->checker->patches;
    mean_ref /= 3.f * g->checker->patches;

    float variance = 0.f;
    float covariance = 0.f;

    for(size_t k = 0; k < g->checker->patches; k++)
    {
      dt_aligned_pixel_t XYZ_ref, RGB_ref;
      dt_aligned_pixel_t XYZ_test, RGB_test;

      for_three_channels(c) XYZ_test[c] = patches[k * 4 + c];
      dt_Lab_to_XYZ(g->checker->values[k].Lab, XYZ_ref);

      dot_product(XYZ_test, XYZ_to_CAM, RGB_test);
      dot_product(XYZ_ref, XYZ_to_CAM, RGB_ref);

      // Undo exposure module settings
      for_three_channels(c)
      {
        RGB_test[c] = RGB_test[c] / user_exposure / exposure + user_black;
      }

      for_three_channels(c)
      {
        variance += sqf(RGB_test[c] - mean_test);
        covariance += (RGB_ref[c] - mean_ref) * (RGB_test[c] - mean_ref);
      }
    }
    variance /= 3.f * g->checker->patches;
    covariance /= 3.f * g->checker->patches;

    // Here, we solve the least-squares problem RGB_ref = exposure * RGB_test + offset
    // using :
    //   exposure = covariance(RGB_test, RGB_ref) / variance(RGB_test)
    //   offset = mean(RGB_ref) - exposure * mean(RGB_test)
    exposure = covariance / variance;
    black = mean_ref - exposure * mean_test;
  }

  // the exposure module applies output  = (input - offset) * exposure
  // but we compute output = input * exposure + offset
  // so, rescale offset to adapt our offset to exposure module GUI
  black /= -exposure;

  const extraction_result_t result = { black, exposure };
  return result;
}

static void _extract_color_checker(const float *const restrict in,
                                   float *const restrict out,
                                   const dt_iop_roi_t *const roi_in,
                                   dt_iop_channelmixer_rgb_gui_data_t *g,
                                   const dt_colormatrix_t RGB_to_XYZ,
                                   const dt_colormatrix_t XYZ_to_RGB,
                                   const dt_colormatrix_t XYZ_to_CAM,
                                   const dt_adaptation_t kind)
{
  float *const restrict patches = dt_alloc_align_float(g->checker->patches * 4);

  dt_simd_memcpy(in, out, (size_t)roi_in->width * roi_in->height * 4);

  extraction_result_t extraction_result =
    _extract_patches(out, roi_in, g, RGB_to_XYZ, XYZ_to_CAM,
                     patches, TRUE);

  // Compute the delta E
  float pre_wb_delta_E = 0.f;
  float pre_wb_max_delta_E = 0.f;
  _compute_patches_delta_E(patches, g->checker, g->delta_E_in,
                          &pre_wb_delta_E, &pre_wb_max_delta_E);

  /* find the scene illuminant */

  // find reference grey patch
  dt_aligned_pixel_t XYZ_grey_ref = { 0.0f };
  dt_Lab_to_XYZ(g->checker->values[g->checker->middle_grey].Lab, XYZ_grey_ref);

  // find test grey patch
  dt_aligned_pixel_t XYZ_grey_test = { 0.0f };
  for(size_t c = 0; c < 3; c++)
    XYZ_grey_test[c] = patches[g->checker->middle_grey * 4 + c];

  // compute reference illuminant
  dt_aligned_pixel_t D50_XYZ;
  illuminant_xy_to_XYZ(D50xyY.x, D50xyY.y, D50_XYZ);

  // normalize luminances - note : illuminant is normalized by definition
  const float Y_test = MAX(NORM_MIN, XYZ_grey_test[1]);
  const float Y_ref = MAX(NORM_MIN, XYZ_grey_ref[1]);

  dt_vector_div1(XYZ_grey_ref, XYZ_grey_ref, Y_ref);
  dt_vector_div1(XYZ_grey_test, XYZ_grey_test, Y_test);

  // convert XYZ to LMS
  dt_aligned_pixel_t LMS_grey_ref, LMS_grey_test, D50_LMS;
  convert_any_XYZ_to_LMS(XYZ_grey_ref, LMS_grey_ref, kind);
  convert_any_XYZ_to_LMS(XYZ_grey_test, LMS_grey_test, kind);
  convert_any_XYZ_to_LMS(D50_XYZ, D50_LMS, kind);

  // solve the equation to find the scene illuminant
  dt_aligned_pixel_t illuminant = { 0.0f };
  for_three_channels(c)
    illuminant[c] = D50_LMS[c] * LMS_grey_test[c] / LMS_grey_ref[c];

  // convert back the illuminant to XYZ then xyY
  dt_aligned_pixel_t illuminant_XYZ, illuminant_xyY = { .0f };
  convert_any_LMS_to_XYZ(illuminant, illuminant_XYZ, kind);
  dt_vector_clipneg(illuminant_XYZ);
  const float Y_illu = MAX(NORM_MIN, illuminant_XYZ[1]);
  dt_vector_div1(illuminant_XYZ, illuminant_XYZ, Y_illu);
  dt_D50_XYZ_to_xyY(illuminant_XYZ, illuminant_xyY);

  // save the illuminant in GUI struct for commit
  g->xy[0] = illuminant_xyY[0];
  g->xy[1] = illuminant_xyY[1];

  // and recompute back the LMS to be sure we use the parameters that
  // will be computed later
  illuminant_xy_to_XYZ(illuminant_xyY[0], illuminant_xyY[1], illuminant_XYZ);
  convert_any_XYZ_to_LMS(illuminant_XYZ, illuminant, kind);
  const float p = powf(0.818155f / illuminant[2], 0.0834f);

  /* White-balance the patches */
  for(size_t k = 0; k < g->checker->patches; k++)
  {
    // keep in synch with _loop_switch() from process()
    float *const sample = patches + k * 4;
    const float Y = sample[1];
    downscale_vector(sample, Y);

    dt_aligned_pixel_t LMS;
    convert_any_XYZ_to_LMS(sample, LMS, kind);

    dt_aligned_pixel_t temp;

    switch(kind)
    {
      case DT_ADAPTATION_FULL_BRADFORD:
      {
        bradford_adapt_D50(LMS, illuminant, p, TRUE, temp);
        break;
      }
      case DT_ADAPTATION_LINEAR_BRADFORD:
      {
        bradford_adapt_D50(LMS, illuminant, 1.f, FALSE, temp);
        break;
      }
      case DT_ADAPTATION_CAT16:
      {
        CAT16_adapt_D50(LMS, illuminant, 1.f, TRUE, temp); // force full-adaptation
        break;
      }
      case DT_ADAPTATION_XYZ:
      {
        XYZ_adapt_D50(LMS, illuminant, temp);
        break;
      }
      case DT_ADAPTATION_RGB:
      case DT_ADAPTATION_LAST:
      default:
      {
        // No white balance.
        for(size_t c = 0; c < 3; ++c) temp[c] = LMS[c];
        break;
      }
    }

    convert_any_LMS_to_XYZ(temp, sample, kind);
    upscale_vector(sample, Y);
  }

  // Compute the delta E
  float post_wb_delta_E = 0.f;
  float post_wb_max_delta_E = 0.f;
  _compute_patches_delta_E(patches, g->checker, g->delta_E_in,
                          &post_wb_delta_E, &post_wb_max_delta_E);

  /* Compute the matrix of mix */
  double *const restrict Y = dt_alloc_align_double(g->checker->patches * 3);
  double *const restrict A = dt_alloc_align_double(g->checker->patches * 3 * 9);

  for(size_t k = 0; k < g->checker->patches; k++)
  {
    float *const sample = patches + k * 4;
    dt_aligned_pixel_t LMS_test;
    convert_any_XYZ_to_LMS(sample, LMS_test, kind);

    float *const reference = g->checker->values[k].Lab;
    dt_aligned_pixel_t XYZ_ref, LMS_ref;
    dt_Lab_to_XYZ(reference, XYZ_ref);
    convert_any_XYZ_to_LMS(XYZ_ref, LMS_ref, kind);

    // get the optimization weights
    float w = 1.f;
    if(g->optimization == DT_SOLVE_OPTIMIZE_NONE)
      w = sqrtf(1.f / (float)g->checker->patches);
    else if(g->optimization == DT_SOLVE_OPTIMIZE_HIGH_SAT)
      w = sqrtf(hypotf(reference[1] / 128.f, reference[2] / 128.f));
    else if(g->optimization == DT_SOLVE_OPTIMIZE_LOW_SAT)
      w = sqrtf(1.f - hypotf(reference[1] / 128.f, reference[2] / 128.f));
    else if(g->optimization == DT_SOLVE_OPTIMIZE_SKIN)
    {
      // average skin hue angle is 1.0 rad, hue range is [0.75 ; 1.25]
      const float ref_hue = 1.f;
      GET_WEIGHT;
    }
    else if(g->optimization == DT_SOLVE_OPTIMIZE_FOLIAGE)
    {
      // average foliage hue angle is 2.23 rad, hue range is [1.94 ; 2.44]
      const float ref_hue = 2.23f;
      GET_WEIGHT;
    }
    else if(g->optimization == DT_SOLVE_OPTIMIZE_SKY)
    {
      // average sky/water hue angle is -1.93 rad, hue range is [-1.64 ; -2.41]
      const float ref_hue = -1.93f;
      GET_WEIGHT;
    }
    else if(g->optimization == DT_SOLVE_OPTIMIZE_AVG_DELTA_E)
      w = sqrtf(sqrtf(1.f / g->delta_E_in[k]));
    else if(g->optimization == DT_SOLVE_OPTIMIZE_MAX_DELTA_E)
      w = sqrtf(sqrtf(g->delta_E_in[k]));

    // fill 3 rows of the y column vector
    for(size_t c = 0; c < 3; c++) Y[k * 3 + c] = w * LMS_ref[c];

    // fill line one of the A matrix
    A[k * 3 * 9 + 0] = w * LMS_test[0];
    A[k * 3 * 9 + 1] = w * LMS_test[1];
    A[k * 3 * 9 + 2] = w * LMS_test[2];
    A[k * 3 * 9 + 3] = 0.f;
    A[k * 3 * 9 + 4] = 0.f;
    A[k * 3 * 9 + 5] = 0.f;
    A[k * 3 * 9 + 6] = 0.f;
    A[k * 3 * 9 + 7] = 0.f;
    A[k * 3 * 9 + 8] = 0.f;

    // fill line two of the A matrix
    A[k * 3 * 9 + 9 + 0] = 0.f;
    A[k * 3 * 9 + 9 + 1] = 0.f;
    A[k * 3 * 9 + 9 + 2] = 0.f;
    A[k * 3 * 9 + 9 + 3] = w * LMS_test[0];
    A[k * 3 * 9 + 9 + 4] = w * LMS_test[1];
    A[k * 3 * 9 + 9 + 5] = w * LMS_test[2];
    A[k * 3 * 9 + 9 + 6] = 0.f;
    A[k * 3 * 9 + 9 + 7] = 0.f;
    A[k * 3 * 9 + 9 + 8] = 0.f;

    // fill line three of the A matrix
    A[k * 3 * 9 + 18 + 0] = 0.f;
    A[k * 3 * 9 + 18 + 1] = 0.f;
    A[k * 3 * 9 + 18 + 2] = 0.f;
    A[k * 3 * 9 + 18 + 3] = 0.f;
    A[k * 3 * 9 + 18 + 4] = 0.f;
    A[k * 3 * 9 + 18 + 5] = 0.f;
    A[k * 3 * 9 + 18 + 6] = w * LMS_test[0];
    A[k * 3 * 9 + 18 + 7] = w * LMS_test[1];
    A[k * 3 * 9 + 18 + 8] = w * LMS_test[2];
  }

  pseudo_solve_gaussian(A, Y, g->checker->patches * 3, 9, TRUE);

  // repack the matrix
  repack_double3x3_to_3xSSE(Y, g->mix);

  dt_free_align(Y);
  dt_free_align(A);

  // apply the matrix mix
  for(size_t k = 0; k < g->checker->patches; k++)
  {
    float *const sample = patches + k * 4;
    dt_aligned_pixel_t LMS_test;
    dt_aligned_pixel_t temp = { 0.f };

    // Restore the original exposure of the patch
    for(size_t c = 0; c < 3; c++) temp[c] = sample[c];

    convert_any_XYZ_to_LMS(temp, LMS_test, kind);
      dot_product(LMS_test, g->mix, temp);
    convert_any_LMS_to_XYZ(temp, sample, kind);
  }

  // Compute the delta E
  float post_mix_delta_E = 0.f;
  float post_mix_max_delta_E = 0.f;
  _compute_patches_delta_E(patches, g->checker, g->delta_E_in,
                          &post_mix_delta_E, &post_mix_max_delta_E);

  // get the temperature
  float temperature;
  dt_illuminant_t test_illuminant;
  float x = illuminant_xyY[0];
  float y = illuminant_xyY[1];
  _check_if_close_to_daylight(x, y, &temperature, &test_illuminant, NULL);
  gchar *string;

  if(test_illuminant == DT_ILLUMINANT_D)
    string = _("(daylight)");
  else if(test_illuminant == DT_ILLUMINANT_BB)
    string = _("(black body)");
  else
    string = _("(invalid)");

  gchar *diagnostic;
  if(post_mix_delta_E <= 1.2f)
    diagnostic = _("very good");
  else if(post_mix_delta_E <= 2.3f)
    diagnostic = _("good");
  else if(post_mix_delta_E <= 3.4f)
    diagnostic = _("passable");
  else
    diagnostic = _("bad");

  g->profile_ready = TRUE;

  // Update GUI label
  g_free(g->delta_E_label_text);
  g->delta_E_label_text
      = g_strdup_printf(_("\n<b>Profile quality report: %s</b>\n"
                          "input ΔE: \tavg. %.2f ; \tmax. %.2f\n"
                          "WB ΔE: \tavg. %.2f; \tmax. %.2f\n"
                          "output ΔE: \tavg. %.2f; \tmax. %.2f\n\n"
                          "<b>Profile data</b>\n"
                          "illuminant:  \t%.0f K \t%s\n"
                          "matrix in adaptation space:\n"
                          "<tt>%+.4f \t%+.4f \t%+.4f\n"
                          "%+.4f \t%+.4f \t%+.4f\n"
                          "%+.4f \t%+.4f \t%+.4f</tt>\n\n"
                          "<b>Normalization values</b>\n"
                          "exposure compensation: \t%+.2f EV\n"
                          "black offset: \t%+.4f"
                          ),
                        diagnostic, pre_wb_delta_E, pre_wb_max_delta_E,
                        post_wb_delta_E, post_wb_max_delta_E,
                        post_mix_delta_E, post_mix_max_delta_E,
                        temperature, string,
                        g->mix[0][0], g->mix[0][1], g->mix[0][2],
                        g->mix[1][0], g->mix[1][1], g->mix[1][2],
                        g->mix[2][0], g->mix[2][1], g->mix[2][2],
                        log2f(extraction_result.exposure), extraction_result.black);

  dt_free_align(patches);
}

static void _validate_color_checker(const float *const restrict in,
                                    const dt_iop_roi_t *const roi_in,
                                    dt_iop_channelmixer_rgb_gui_data_t *g,
                                    const dt_colormatrix_t RGB_to_XYZ,
                                    const dt_colormatrix_t XYZ_to_RGB,
                                    const dt_colormatrix_t XYZ_to_CAM)
{
  float *const restrict patches = dt_alloc_align_float(4 * g->checker->patches);
  extraction_result_t extraction_result =
    _extract_patches(in, roi_in, g, RGB_to_XYZ, XYZ_to_CAM, patches, FALSE);

  // Compute the delta E
  float pre_wb_delta_E = 0.f;
  float pre_wb_max_delta_E = 0.f;
  _compute_patches_delta_E(patches, g->checker, g->delta_E_in,
                          &pre_wb_delta_E, &pre_wb_max_delta_E);

  gchar *diagnostic;
  if(pre_wb_delta_E <= 1.2f)
    diagnostic = _("very good");
  else if(pre_wb_delta_E <= 2.3f)
    diagnostic = _("good");
  else if(pre_wb_delta_E <= 3.4f)
    diagnostic = _("passable");
  else
    diagnostic = _("bad");

  // Update GUI label
  g_free(g->delta_E_label_text);
  g->delta_E_label_text =
    g_strdup_printf(_("\n<b>Profile quality report: %s</b>\n"
                      "output ΔE: \tavg. %.2f; \tmax. %.2f\n\n"
                      "<b>Normalization values</b>\n"
                      "exposure compensation: \t%+.2f EV\n"
                      "black offset: \t%+.4f"),
                    diagnostic, pre_wb_delta_E, pre_wb_max_delta_E,
                    log2f(extraction_result.exposure),
                    extraction_result.black);

  dt_free_align(patches);
}

static void _set_trouble_messages(dt_iop_module_t *self)
{
  const dt_iop_channelmixer_rgb_params_t *p = self->params;
  const dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  const dt_develop_t *dev = self->dev;
  const dt_dev_chroma_t *chr = &dev->chroma;

  dt_print(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE, "trouble message for %s%s : temp=%p adapt=%p",
    self->op, dt_iop_get_instance_id(self), chr->temperature, chr->adaptation);

  // in temperature module we make sure this is only presented if temperature is enabled
  if(!chr->temperature)
  {
    if(chr->adaptation)
      dt_iop_set_module_trouble_message(chr->adaptation, NULL, NULL, NULL);
    return;
  }

  if(!chr->adaptation)
  {
    dt_iop_set_module_trouble_message(chr->temperature, NULL, NULL, NULL);
    dt_iop_set_module_trouble_message(self, NULL, NULL, NULL);
    return;
  }

  const gboolean valid =
    self->enabled
    && !(p->illuminant == DT_ILLUMINANT_PIPE || p->adaptation == DT_ADAPTATION_RGB)
    && !dt_image_is_monochrome(&dev->image_storage);

  const gboolean temperature_enabled = chr->temperature && chr->temperature->enabled;
  const gboolean adaptation_enabled = chr->adaptation && chr->adaptation->enabled;

  // our first and biggest problem : white balance module is being
  // clever with WB coeffs
  const gboolean problem1 = valid
                            && chr->adaptation == self
                            && temperature_enabled
                            && !dt_dev_is_D65_chroma(dev);

  // our second biggest problem : another channelmixerrgb instance is doing CAT
  // earlier in the pipe and we don't use masking here.
  const gboolean problem2 = valid
                            && adaptation_enabled
                            && temperature_enabled
                            && chr->adaptation != self
                            && !g->is_blending;

  // our third and minor problem: white balance module is not active but default_enabled
  // and we do chromatic adaptation in color calibration
  const gboolean problem3 = valid
                            && adaptation_enabled
                            && !temperature_enabled
                            && chr->temperature->default_enabled;

  const gboolean anyproblem = problem1 || problem2 || problem3;

  const dt_image_t *img = &dev->image_storage;
  dt_print_pipe(DT_DEBUG_PIPE, anyproblem ? "chroma trouble" : "chroma data",
      NULL, self, DT_DEVICE_NONE, NULL, NULL,
      "%s%s%sD65=%s.  NOW %.3f %.3f %.3f, D65 %.3f %.3f %.3f, AS-SHOT %.3f %.3f %.3f File `%s' ID=%i",
      problem1 ? "white balance applied twice, " : "",
      problem2 ? "double CAT applied, " : "",
      problem3 ? "white balance missing, " : "",
      dt_dev_is_D65_chroma(dev) ? "YES" : "NO",
      chr->wb_coeffs[0], chr->wb_coeffs[1], chr->wb_coeffs[2],
      chr->D65coeffs[0], chr->D65coeffs[1], chr->D65coeffs[2],
      chr->as_shot[0], chr->as_shot[1], chr->as_shot[2],
      img->filename,
      img->id);

  if(problem2)
  {
    dt_iop_set_module_trouble_message
      (self,
        _("double CAT applied"),
        _("you have 2 instances or more of color calibration,\n"
          "all providing chromatic adaptation.\n"
          "this can lead to inconsistencies unless you\n"
          "use them with masks or know what you are doing."),
        NULL);
    return;
  }

  if(problem1)
  {
    dt_iop_set_module_trouble_message
      (chr->temperature,
        _("white balance applied twice"),
        _("the color calibration module is enabled and already provides\n"
          "chromatic adaptation.\n"
          "set the white balance here to camera reference (D65)\n"
          "or disable chromatic adaptation in color calibration."),
        NULL);

    dt_iop_set_module_trouble_message
      (self,
        _("white balance module error"),
        _("the white balance module is not using the camera\n"
          "reference illuminant, which will cause issues here\n"
          "with chromatic adaptation. either set it to reference\n"
          "or disable chromatic adaptation here."),
        NULL);
    return;
  }

  if(problem3)
  {
    dt_iop_set_module_trouble_message
      (chr->temperature,
        _("white balance missing"),
        _("this module is not providing a valid reference illuminant\n"
          "causing chromatic adaptation issues in color calibration.\n"
          "enable this module and either set it to reference\n"
          "or disable chromatic adaptation in color calibration."),
        NULL);

    dt_iop_set_module_trouble_message
      (self,
        _("white balance missing"),
        _("the white balance module is not providing a valid reference\n"
          "illuminant causing issues with chromatic adaptation here.\n"
          "enable white balance and either set it to reference\n"
          "or disable chromatic adaptation here."),
        NULL);
    return;
  }

  if(chr->adaptation && chr->adaptation == self)
  {
    dt_iop_set_module_trouble_message(chr->temperature, NULL, NULL, NULL);
    dt_iop_set_module_trouble_message(self, NULL, NULL, NULL);
  }
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const restrict ivoid,
             void *const restrict ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_iop_channelmixer_rbg_data_t *data = piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  const dt_iop_order_iccprofile_info_t *const input_profile =
    dt_ioppr_get_pipe_input_profile_info(piece->pipe);
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/,
                                        self, piece->colors,
                                        ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's
            // trouble flag has been updated

  if(piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
    _declare_cat_on_pipe(self, FALSE);

  dt_colormatrix_t RGB_to_XYZ;
  dt_colormatrix_t XYZ_to_RGB;
  dt_colormatrix_t XYZ_to_CAM;

  // repack the matrices as flat AVX2-compliant matrice
  if(work_profile)
  {
    // work profile can't be fetched in commit_params since it is not yet initialised
    memcpy(RGB_to_XYZ, work_profile->matrix_in, sizeof(RGB_to_XYZ));
    memcpy(XYZ_to_RGB, work_profile->matrix_out, sizeof(XYZ_to_RGB));
    memcpy(XYZ_to_CAM, input_profile->matrix_out, sizeof(XYZ_to_CAM));
  }

  assert(piece->colors == 4);
  const size_t ch = 4;

  const float *const restrict in = (const float *const restrict)ivoid;
  float *const restrict out = (float *const restrict)ovoid;

  // auto-detect WB upon request
  if(self->dev->gui_attached && g)
  {
#ifdef AI_ACTIVATED
    gboolean exit = FALSE;
#endif
    if(g->run_profile && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
    {
      dt_iop_gui_enter_critical_section(self);
      _extract_color_checker(in, out, roi_in, g, RGB_to_XYZ,
                             XYZ_to_RGB, XYZ_to_CAM, data->adaptation);
      g->run_profile = FALSE;
      dt_iop_gui_leave_critical_section(self);
    }

#ifdef AI_ACTIVATED
    if(data->illuminant_type == DT_ILLUMINANT_DETECT_EDGES
       || data->illuminant_type == DT_ILLUMINANT_DETECT_SURFACES)
    {
      if(piece->pipe->type & DT_DEV_PIXELPIPE_FULL)
      {
        // detection on full image only
        dt_iop_gui_enter_critical_section(self);
        // compute "AI" white balance.  We can use our output buffer
        // as scratch space since we will be overwriting it afterwards
        // anyway
        _auto_detect_WB(in, out, data->illuminant_type, roi_in->width, roi_in->height,
                        ch, RGB_to_XYZ, g->XYZ);
        dt_dev_pixelpipe_cache_invalidate_later(piece->pipe, self->iop_order);
        dt_iop_gui_leave_critical_section(self);
      }

      // passthrough pixels
      dt_iop_image_copy_by_size(out, in, roi_in->width, roi_in->height, ch);

      dt_control_log(_("auto-detection of white balance completed"));

      exit = TRUE;
    }

    if(exit) return;
#endif
  }

  if(data->illuminant_type == DT_ILLUMINANT_CAMERA)
  {
    // The camera illuminant is a behaviour rather than a preset of
    // values: it uses whatever is in the RAW EXIF. But it depends on
    // what temperature.c is doing and needs to be updated
    // accordingly, to give a consistent result.  We initialise the
    // CAT defaults using the temperature coeffs at startup, but if
    // temperature is changed later, we get no notification of the
    // change here, so we can't update the defaults.  So we need to
    // re-run the detection at runtime…
    float x, y;
    dt_aligned_pixel_t custom_wb;
    _get_white_balance_coeff(self, custom_wb);

    if(find_temperature_from_raw_coeffs(&(self->dev->image_storage), custom_wb, &(x), &(y)))
    {
      // Convert illuminant from xyY to XYZ
      dt_aligned_pixel_t XYZ;
      illuminant_xy_to_XYZ(x, y, XYZ);

      // Convert illuminant from XYZ to Bradford modified LMS
      convert_any_XYZ_to_LMS(XYZ, data->illuminant, data->adaptation);
      data->illuminant[3] = 0.f;
    }
    else
    {
      // just use whatever was defined in commit_params hoping the defaults work…
    }
  }

  // force loop unswitching in a controlled way
  switch(data->adaptation)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    {
      _loop_switch(in, out, roi_out->width, roi_out->height, ch,
                   XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                   data->illuminant, data->saturation, data->lightness, data->grey,
                   data->p, data->gamut, data->clip, data->apply_grey,
                   DT_ADAPTATION_FULL_BRADFORD, data->version);
      break;
    }
    case DT_ADAPTATION_LINEAR_BRADFORD:
    {
      _loop_switch(in, out, roi_out->width, roi_out->height, ch,
                   XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                   data->illuminant, data->saturation, data->lightness, data->grey,
                   data->p, data->gamut, data->clip, data->apply_grey,
                   DT_ADAPTATION_LINEAR_BRADFORD, data->version);
      break;
    }
    case DT_ADAPTATION_CAT16:
    {
      _loop_switch(in, out, roi_out->width, roi_out->height, ch,
                   XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                   data->illuminant, data->saturation, data->lightness, data->grey,
                   data->p, data->gamut, data->clip, data->apply_grey,
                   DT_ADAPTATION_CAT16, data->version);
      break;
    }
    case DT_ADAPTATION_XYZ:
    {
      _loop_switch(in, out, roi_out->width, roi_out->height, ch,
                   XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                   data->illuminant, data->saturation, data->lightness, data->grey,
                   data->p, data->gamut, data->clip, data->apply_grey,
                   DT_ADAPTATION_XYZ, data->version);
      break;
    }
    case DT_ADAPTATION_RGB:
    {
      _loop_switch(in, out, roi_out->width, roi_out->height, ch,
                   XYZ_to_RGB, RGB_to_XYZ, data->MIX,
                   data->illuminant, data->saturation, data->lightness, data->grey,
                   data->p, data->gamut, data->clip, data->apply_grey,
                   DT_ADAPTATION_RGB, data->version);
      break;
    }
    case DT_ADAPTATION_LAST:
    default:
    {
      break;
    }
  }

  // run dE validation at output
  if(self->dev->gui_attached && g)
    if(g->run_validation && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
    {
      _validate_color_checker(out, roi_out, g, RGB_to_XYZ, XYZ_to_RGB, XYZ_to_CAM);
      g->run_validation = FALSE;
    }
}

#if HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               const cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_channelmixer_rbg_data_t *const d = piece->data;
  dt_iop_channelmixer_rgb_global_data_t *const gd = self->global_data;

  const dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);

  if(piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
    _declare_cat_on_pipe(self, FALSE);

  if(d->illuminant_type == DT_ILLUMINANT_CAMERA)
  {
    // The camera illuminant is a behaviour rather than a preset of
    // values: it uses whatever is in the RAW EXIF. But it depends on
    // what temperature.c is doing and needs to be updated
    // accordingly, to give a consistent result.  We initialise the
    // CAT defaults using the temperature coeffs at startup, but if
    // temperature is changed later, we get no notification of the
    // change here, so we can't update the defaults.  So we need to
    // re-run the detection at runtime…
    float x, y;
    dt_aligned_pixel_t custom_wb;
    _get_white_balance_coeff(self, custom_wb);

    if(find_temperature_from_raw_coeffs(&(self->dev->image_storage), custom_wb, &(x), &(y)))
    {
      // Convert illuminant from xyY to XYZ
      dt_aligned_pixel_t XYZ;
      illuminant_xy_to_XYZ(x, y, XYZ);

      // Convert illuminant from XYZ to Bradford modified LMS
      convert_any_XYZ_to_LMS(XYZ, d->illuminant, d->adaptation);
      d->illuminant[3] = 0.f;
    }
  }

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  if(piece->colors != 4)
  {
    dt_control_log(_("channelmixerrgb works only on RGB input"));
    return err;
  }

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_mem input_matrix_cl = dt_opencl_copy_host_to_device_constant
    (devid, 12 * sizeof(float), (float*)work_profile->matrix_in);
  cl_mem output_matrix_cl = dt_opencl_copy_host_to_device_constant
    (devid, 12 * sizeof(float), (float*)work_profile->matrix_out);
  cl_mem MIX_cl = dt_opencl_copy_host_to_device_constant
    (devid, 12 * sizeof(float), d->MIX);

  if(input_matrix_cl == NULL || output_matrix_cl == NULL || MIX_cl == NULL)
    goto error;
  // select the right kernel for the current LMS space
  int kernel = gd->kernel_channelmixer_rgb_rgb;

  switch(d->adaptation)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    {
      kernel = gd->kernel_channelmixer_rgb_bradford_full;
      break;
    }
    case DT_ADAPTATION_LINEAR_BRADFORD:
    {
      kernel = gd->kernel_channelmixer_rgb_bradford_linear;
      break;
    }
    case DT_ADAPTATION_CAT16:
    {
      kernel = gd->kernel_channelmixer_rgb_cat16;
      break;
    }
    case DT_ADAPTATION_XYZ:
    {
      kernel = gd->kernel_channelmixer_rgb_xyz;
      break;
     }
    case DT_ADAPTATION_RGB:
    case DT_ADAPTATION_LAST:
    default:
    {
      kernel = gd->kernel_channelmixer_rgb_rgb;
      break;
    }
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
          CLARG(dev_in), CLARG(dev_out),
          CLARG(width), CLARG(height),
          CLARG(input_matrix_cl), CLARG(output_matrix_cl),
          CLARG(MIX_cl), CLARG(d->illuminant), CLARG(d->saturation),
          CLARG(d->lightness), CLARG(d->grey),
          CLARG(d->p), CLARG(d->gamut), CLARG(d->clip), CLARG(d->apply_grey),
          CLARG(d->version));

error:
  dt_opencl_release_mem_object(input_matrix_cl);
  dt_opencl_release_mem_object(output_matrix_cl);
  dt_opencl_release_mem_object(MIX_cl);
  return err;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 32; // extended.cl in programs.conf
  dt_iop_channelmixer_rgb_global_data_t *gd = malloc(sizeof(dt_iop_channelmixer_rgb_global_data_t));

  self->data = gd;
  gd->kernel_channelmixer_rgb_cat16 =
    dt_opencl_create_kernel(program, "channelmixerrgb_CAT16");
  gd->kernel_channelmixer_rgb_bradford_full =
    dt_opencl_create_kernel(program, "channelmixerrgb_bradford_full");
  gd->kernel_channelmixer_rgb_bradford_linear =
    dt_opencl_create_kernel(program, "channelmixerrgb_bradford_linear");
  gd->kernel_channelmixer_rgb_xyz =
    dt_opencl_create_kernel(program, "channelmixerrgb_XYZ");
  gd->kernel_channelmixer_rgb_rgb =
    dt_opencl_create_kernel(program, "channelmixerrgb_RGB");
}


void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_channelmixer_rgb_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_channelmixer_rgb_cat16);
  dt_opencl_free_kernel(gd->kernel_channelmixer_rgb_bradford_full);
  dt_opencl_free_kernel(gd->kernel_channelmixer_rgb_bradford_linear);
  dt_opencl_free_kernel(gd->kernel_channelmixer_rgb_xyz);
  dt_opencl_free_kernel(gd->kernel_channelmixer_rgb_rgb);
  free(self->data);
  self->data = NULL;
}
#endif


static inline void _update_bounding_box(dt_iop_channelmixer_rgb_gui_data_t *g,
                                        const float x_increment,
                                        const float y_increment)
{
  // update box nodes
  for(size_t k = 0; k < 4; k++)
  {
    if(g->active_node[k])
    {
      g->box[k].x += x_increment;
      g->box[k].y += y_increment;
    }
  }

  // update the homography
  get_homography(g->ideal_box, g->box, g->homography);
  get_homography(g->box, g->ideal_box, g->inverse_homography);
}

static inline void _init_bounding_box(dt_iop_channelmixer_rgb_gui_data_t *g,
                                     const float width,
                                     const float height)
{
  if(!g->checker_ready)
  {
    // top left
    g->box[0].x = g->box[0].y = 10.;

    // top right
    g->box[1].x = (width - 10.);
    g->box[1].y = g->box[0].y;

    // bottom right
    g->box[2].x = g->box[1].x;
    g->box[2].y = (width - 10.) * g->checker->ratio;

    // bottom left
    g->box[3].x = g->box[0].x;
    g->box[3].y = g->box[2].y;

    g->checker_ready = TRUE;
  }

  g->center_box.x = 0.5f;
  g->center_box.y = 0.5f;

  g->ideal_box[0].x = 0.f;
  g->ideal_box[0].y = 0.f;
  g->ideal_box[1].x = 1.f;
  g->ideal_box[1].y = 0.f;
  g->ideal_box[2].x = 1.f;
  g->ideal_box[2].y = 1.f;
  g->ideal_box[3].x = 0.f;
  g->ideal_box[3].y = 1.f;

  _update_bounding_box(g, 0.f, 0.f);
}

int mouse_moved(dt_iop_module_t *self,
                const float pzx,
                const float pzy,
                const double pressure,
                const int which,
                const float zoom_scale)
{
  if(!self->enabled) return 0;

  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  if(g == NULL || !g->is_profiling_started) return 0;
  if(g->box[0].x == -1.0f || g->box[1].y == -1.0f) return 0;

  float wd, ht;
  if(!dt_dev_get_preview_size(self->dev, &wd, &ht)) return 0;

  // if dragging and dropping, don't update active nodes,
  // just update cursor coordinates then redraw
  // this ensure smooth updates
  if(g->drag_drop)
  {
    dt_iop_gui_enter_critical_section(self);
    g->click_end.x = pzx * wd;
    g->click_end.y = pzy * ht;

    _update_bounding_box(g, g->click_end.x - g->click_start.x,
                         g->click_end.y - g->click_start.y);

    g->click_start.x = pzx * wd;
    g->click_start.y = pzy * ht;
    dt_iop_gui_leave_critical_section(self);

    dt_control_queue_redraw_center();
    return 1;
  }

  // Find out if we are close to a node
  dt_iop_gui_enter_critical_section(self);
  g->is_cursor_close = FALSE;

  for(size_t k = 0; k < 4; k++)
  {
    if(hypotf(pzx * wd - g->box[k].x, pzy * ht - g->box[k].y) < 15.f)
    {
      g->active_node[k] = TRUE;
      g->is_cursor_close = TRUE;
    }
    else
      g->active_node[k] = FALSE;
  }
  dt_iop_gui_leave_critical_section(self);

  // if cursor is close from a node, remove the system pointer arrow
  // to prevent hiding the spot behind it
  if(g->is_cursor_close)
  {
    dt_control_change_cursor(GDK_BLANK_CURSOR);
  }
  else
  {
    // fall back to default cursor
    GdkCursor *const cursor =
      gdk_cursor_new_from_name(gdk_display_get_default(), "default");
    gdk_window_set_cursor(gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui)),
                          cursor);
    g_object_unref(cursor);
  }

  dt_control_queue_redraw_center();

  return 1;
}

int button_pressed(dt_iop_module_t *self,
                   const float pzx,
                   const float pzy,
                   const double pressure,
                   const int which,
                   const int type,
                   const uint32_t state,
                   const float zoom_scale)
{
  if(!self->enabled) return 0;

  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  if(g == NULL || !g->is_profiling_started) return 0;

  float wd, ht;
  if(!dt_dev_get_preview_size(self->dev, &wd, &ht)) return 0;

  // double click : reset the perspective correction
  if(type == GDK_DOUBLE_BUTTON_PRESS)
  {
    dt_iop_gui_enter_critical_section(self);
    g->checker_ready = FALSE;
    g->profile_ready = FALSE;
    _init_bounding_box(g, wd, ht);
    dt_iop_gui_leave_critical_section(self);

    dt_control_queue_redraw_center();
    return 1;
  }

  // bounded box not inited, abort
  if(g->box[0].x == -1.0f || g->box[1].y == -1.0f) return 0;

  // cursor is not on a node, abort
  if(!g->is_cursor_close) return 0;

  dt_iop_gui_enter_critical_section(self);
  g->drag_drop = TRUE;
  g->click_start.x = pzx * wd;
  g->click_start.y = pzy * ht;
  dt_iop_gui_leave_critical_section(self);

  dt_control_queue_redraw_center();

  return 1;
}

int button_released(dt_iop_module_t *self,
                    const float pzx,
                    const float pzy,
                    const int which,
                    const uint32_t state,
                    const float zoom_scale)
{
  if(!self->enabled) return 0;

  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  if(g == NULL || !g->is_profiling_started) return 0;
  if(g->box[0].x == -1.0f || g->box[1].y == -1.0f) return 0;
  if(!g->is_cursor_close || !g->drag_drop) return 0;

  float wd, ht;
  if(!dt_dev_get_preview_size(self->dev, &wd, &ht)) return 0;

  dt_iop_gui_enter_critical_section(self);
  g->drag_drop = FALSE;
  g->click_end.x = pzx * wd;
  g->click_end.y = pzy * ht;
  _update_bounding_box(g, g->click_end.x - g->click_start.x,
                       g->click_end.y - g->click_start.y);
  dt_iop_gui_leave_critical_section(self);

  dt_control_queue_redraw_center();

  return 1;
}

void gui_post_expose(dt_iop_module_t *self,
                     cairo_t *cr,
                     const float width,
                     const float height,
                     const float pointerx,
                     const float pointery,
                     const float zoom_scale)
{
  const dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_output_profile_info(self->dev->full.pipe);
  if(work_profile == NULL) return;

  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  if(!g->is_profiling_started) return;

  const gboolean showhandle = dt_iop_canvas_not_sensitive(darktable.develop) == FALSE;
  const double lwidth = (showhandle ? 1.0 : 0.5) / zoom_scale;

  cairo_set_line_width(cr, 2.0 * lwidth);
  const double origin = 9. / zoom_scale;
  const double destination = 18. / zoom_scale;

  for(size_t k = 0; k < 4; k++)
  {
    if(g->active_node[k])
    {
      // draw cross hair
      cairo_set_source_rgba(cr, 1., 1., 1., 1.);

      cairo_move_to(cr, g->box[k].x - origin, g->box[k].y);
      cairo_line_to(cr, g->box[k].x - destination, g->box[k].y);

      cairo_move_to(cr, g->box[k].x + origin, g->box[k].y);
      cairo_line_to(cr, g->box[k].x + destination, g->box[k].y);

      cairo_move_to(cr, g->box[k].x, g->box[k].y - origin);
      cairo_line_to(cr, g->box[k].x, g->box[k].y - destination);

      cairo_move_to(cr, g->box[k].x, g->box[k].y + origin);
      cairo_line_to(cr, g->box[k].x, g->box[k].y + destination);

      cairo_stroke(cr);
    }

    if(showhandle)
    {
      // draw outline circle
      cairo_set_source_rgba(cr, 1., 1., 1., 1.);
      cairo_arc(cr, g->box[k].x, g->box[k].y, 8. / zoom_scale, 0, 2. * M_PI);
      cairo_stroke(cr);

      // draw black dot
      cairo_set_source_rgba(cr, 0., 0., 0., 1.);
      cairo_arc(cr, g->box[k].x, g->box[k].y, 1.5 / zoom_scale, 0, 2. * M_PI);
      cairo_fill(cr);
    }
  }

  // draw symmetry axes
  cairo_set_line_width(cr, 1.5 * lwidth);
  cairo_set_source_rgba(cr, 1., 1., 1., 1.);
  const point_t top_ideal = { 0.5f, 1.f };
  const point_t top = apply_homography(top_ideal, g->homography);
  const point_t bottom_ideal = { 0.5f, 0.f };
  const point_t bottom = apply_homography(bottom_ideal, g->homography);
  cairo_move_to(cr, top.x, top.y);
  cairo_line_to(cr, bottom.x, bottom.y);
  cairo_stroke(cr);

  const point_t left_ideal = { 0.f, 0.5f };
  const point_t left = apply_homography(left_ideal, g->homography);
  const point_t right_ideal = { 1.f, 0.5f };
  const point_t right = apply_homography(right_ideal, g->homography);
  cairo_move_to(cr, left.x, left.y);
  cairo_line_to(cr, right.x, right.y);
  cairo_stroke(cr);

  /* For debug : display center of the image and center of the ideal target
  point_t new_target_center = apply_homography(target_center, g->homography);
  cairo_set_source_rgba(cr, 1., 1., 1., 1.);
  cairo_arc(cr, new_target_center.x, new_target_center.y, 7., 0, 2. * M_PI);
  cairo_stroke(cr);

  cairo_set_source_rgba(cr, 0., 1., 1., 1.);
  cairo_arc(cr, 0.5 * wd, 0.5 * ht, 7., 0, 2. * M_PI);
  cairo_stroke(cr);
  */

  const float radius_x =
    g->checker->radius * hypotf(1.f, g->checker->ratio) * g->safety_margin;
  const float radius_y = radius_x / g->checker->ratio;

  for(size_t k = 0; k < g->checker->patches; k++)
  {
    // center of the patch in the ideal reference
    const point_t center = { g->checker->values[k].x, g->checker->values[k].y };

    // corners of the patch in the ideal reference
    const point_t corners[4] = { {center.x - radius_x, center.y - radius_y},
                                 {center.x + radius_x, center.y - radius_y},
                                 {center.x + radius_x, center.y + radius_y},
                                 {center.x - radius_x, center.y + radius_y} };

    // apply patch coordinates transform depending on perspective
    const point_t new_center = apply_homography(center, g->homography);
    // apply_homography_scaling gives a scaling of areas. we need to scale the
    // radius of the center circle so take a square root.
    const float scaling = sqrtf(apply_homography_scaling(center, g->homography));
    point_t new_corners[4];
    for(size_t c = 0; c < 4; c++)
      new_corners[c] = apply_homography(corners[c], g->homography);

    cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
    cairo_set_source_rgba(cr, 0., 0., 0., 1.);
    cairo_move_to(cr, new_corners[0].x, new_corners[0].y);
    cairo_line_to(cr, new_corners[1].x, new_corners[1].y);
    cairo_line_to(cr, new_corners[2].x, new_corners[2].y);
    cairo_line_to(cr, new_corners[3].x, new_corners[3].y);
    cairo_line_to(cr, new_corners[0].x, new_corners[0].y);

    if(g->delta_E_in)
    {
      // draw delta E feedback
      if(g->delta_E_in[k] > 2.3f)
      {
        // one diagonal if delta E > 3
        cairo_move_to(cr, new_corners[0].x, new_corners[0].y);
        cairo_line_to(cr, new_corners[2].x, new_corners[2].y);
      }
      if(g->delta_E_in[k] > 4.6f)
      {
        // the other diagonal if delta E > 6
        cairo_move_to(cr, new_corners[1].x, new_corners[1].y);
        cairo_line_to(cr, new_corners[3].x, new_corners[3].y);
      }
    }

    cairo_set_line_width(cr, 5.0 * lwidth);
    cairo_stroke_preserve(cr);
    cairo_set_line_width(cr, 2.0 * lwidth);
    cairo_set_source_rgba(cr, 1., 1., 1., 1.);
    cairo_stroke(cr);

    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);

    dt_aligned_pixel_t RGB;
    dt_ioppr_lab_to_rgb_matrix(g->checker->values[k].Lab, RGB,
                               work_profile->matrix_out_transposed, work_profile->lut_out,
                               work_profile->unbounded_coeffs_out, work_profile->lutsize,
                               work_profile->nonlinearlut);

    cairo_set_source_rgba(cr, RGB[0], RGB[1], RGB[2], 1.);
    cairo_arc(cr, new_center.x, new_center.y,
              0.25 * (radius_x + radius_y) * scaling, 0, 2. * M_PI);
    cairo_fill(cr);
  }
}

static void _optimize_changed_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  const int i = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("darkroom/modules/channelmixerrgb/optimization", i);

  dt_iop_gui_enter_critical_section(self);
  g->optimization = i;
  dt_iop_gui_leave_critical_section(self);
}

static void _checker_changed_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  const int i = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("darkroom/modules/channelmixerrgb/colorchecker", i);
  g->checker = dt_get_color_checker(i);

  float wd, ht;
  if(!dt_dev_get_preview_size(self->dev, &wd, &ht)) return;

  dt_iop_gui_enter_critical_section(self);
  g->profile_ready = FALSE;
  _init_bounding_box(g, wd, ht);
  dt_iop_gui_leave_critical_section(self);

  dt_control_queue_redraw_center();
}

static void _safety_changed_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  dt_iop_gui_enter_critical_section(self);
  g->safety_margin = dt_bauhaus_slider_get(widget);
  dt_iop_gui_leave_critical_section(self);

  dt_conf_set_float("darkroom/modules/channelmixerrgb/safety", g->safety_margin);
  dt_control_queue_redraw_center();
}


static void _start_profiling_callback(GtkWidget *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_request_focus(self);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), TRUE);

  float wd, ht;
  if(!dt_dev_get_preview_size(self->dev, &wd, &ht)) return;

  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  g->is_profiling_started = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->cs.toggle));

  // init bounding box
  dt_iop_gui_enter_critical_section(self);
  _init_bounding_box(g, wd, ht);
  dt_iop_gui_leave_critical_section(self);

  dt_control_queue_redraw_center();
}

static void _run_profile_callback(GtkWidget *widget,
                                 GdkEventButton *event,
                                 dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  dt_iop_gui_enter_critical_section(self);
  g->run_profile = TRUE;
  dt_iop_gui_leave_critical_section(self);

  dt_dev_reprocess_preview(self->dev);
}

static void _run_validation_callback(GtkWidget *widget,
                                    GdkEventButton *event,
                                    dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  dt_iop_gui_enter_critical_section(self);
  g->run_validation = TRUE;
  dt_iop_gui_leave_critical_section(self);

  dt_dev_reprocess_preview(self->dev);
}

static void _commit_profile_callback(GtkWidget *widget,
                                     GdkEventButton *event,
                                     dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = self->params;

  if(!g->profile_ready) return;

  dt_iop_gui_enter_critical_section(self);

  p->x = g->xy[0];
  p->y = g->xy[1];
  p->illuminant = DT_ILLUMINANT_CUSTOM;
  _check_if_close_to_daylight(p->x, p->y, &p->temperature, NULL, NULL);

  p->red[0] = g->mix[0][0];
  p->red[1] = g->mix[0][1];
  p->red[2] = g->mix[0][2];

  p->green[0] = g->mix[1][0];
  p->green[1] = g->mix[1][1];
  p->green[2] = g->mix[1][2];

  p->blue[0] = g->mix[2][0];
  p->blue[1] = g->mix[2][1];
  p->blue[2] = g->mix[2][2];

  dt_iop_gui_leave_critical_section(self);

  ++darktable.gui->reset;
  dt_bauhaus_combobox_set(g->illuminant, p->illuminant);
  dt_bauhaus_slider_set(g->temperature, p->temperature);

  dt_aligned_pixel_t xyY = { p->x, p->y, 1.f };
  dt_aligned_pixel_t Lch = { 0 };
  dt_xyY_to_Lch(xyY, Lch);
  dt_bauhaus_slider_set(g->illum_x, Lch[2] / M_PI * 180.f);
  dt_bauhaus_slider_set(g->illum_y, Lch[1]);

  dt_bauhaus_slider_set(g->scale_red_R, p->red[0]);
  dt_bauhaus_slider_set(g->scale_red_G, p->red[1]);
  dt_bauhaus_slider_set(g->scale_red_B, p->red[2]);

  dt_bauhaus_slider_set(g->scale_green_R, p->green[0]);
  dt_bauhaus_slider_set(g->scale_green_G, p->green[1]);
  dt_bauhaus_slider_set(g->scale_green_B, p->green[2]);

  dt_bauhaus_slider_set(g->scale_blue_R, p->blue[0]);
  dt_bauhaus_slider_set(g->scale_blue_G, p->blue[1]);
  dt_bauhaus_slider_set(g->scale_blue_B, p->blue[2]);

  --darktable.gui->reset;

  gui_changed(self, NULL, NULL);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

#ifdef AI_ACTIVATED
static void _develop_ui_pipe_finished_callback(gpointer instance, dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  if(!g) return;

  dt_iop_channelmixer_rgb_params_t *p = self->params;

  if(p->illuminant != DT_ILLUMINANT_DETECT_EDGES
     && p->illuminant != DT_ILLUMINANT_DETECT_SURFACES)
    return;

  dt_iop_gui_enter_critical_section(self);
  p->x = g->XYZ[0];
  p->y = g->XYZ[1];
  dt_iop_gui_leave_critical_section(self);

  _check_if_close_to_daylight(p->x, p->y,
                              &p->temperature, &p->illuminant, &p->adaptation);

  ++darktable.gui->reset;

  dt_bauhaus_slider_set(g->temperature, p->temperature);
  dt_bauhaus_combobox_set(g->illuminant, p->illuminant);
  dt_bauhaus_combobox_set(g->adaptation, p->adaptation);

  const dt_aligned_pixel_t xyY = { p->x, p->y, 1.f };
  dt_aligned_pixel_t Lch;
  dt_xyY_to_Lch(xyY, Lch);
  dt_bauhaus_slider_set(g->illum_x, Lch[2] / M_PI * 180.f);
  dt_bauhaus_slider_set(g->illum_y, Lch[1]);

  _update_illuminants(self);
  _update_approx_cct(self);
  _update_illuminant_color(self);

  --darktable.gui->reset;

  gui_changed(self, NULL, NULL);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

static void _preview_pipe_finished_callback(gpointer instance, dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  if(!g) return;

  dt_iop_gui_enter_critical_section(self);
  gtk_label_set_markup(GTK_LABEL(g->label_delta_E), g->delta_E_label_text);
  dt_iop_gui_leave_critical_section(self);
  _set_trouble_messages(self);
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_channelmixer_rgb_params_t *p = (dt_iop_channelmixer_rgb_params_t *)p1;
  dt_iop_channelmixer_rbg_data_t *d = piece->data;
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  d->version = p->version;

  float norm_R = 1.0f;
  if(p->normalize_R)
    norm_R = p->red[0] + p->red[1] + p->red[2];

  float norm_G = 1.0f;
  if(p->normalize_G)
    norm_G = p->green[0] + p->green[1] + p->green[2];

  float norm_B = 1.0f;
  if(p->normalize_B)
    norm_B = p->blue[0] + p->blue[1] + p->blue[2];

  float norm_sat = 0.0f;
  if(p->normalize_sat)
    norm_sat = (p->saturation[0] + p->saturation[1] + p->saturation[2]) / 3.f;

  float norm_light = 0.0f;
  if(p->normalize_light)
    norm_light = (p->lightness[0] + p->lightness[1] + p->lightness[2]) / 3.f;

  float norm_grey = p->grey[0] + p->grey[1] + p->grey[2];
  d->apply_grey = (p->grey[0] != 0.f) || (p->grey[1] != 0.f) || (p->grey[2] != 0.f);
  if(!p->normalize_grey || norm_grey == 0.f)
    norm_grey = 1.f;

  for(int i = 0; i < 3; i++)
  {
    d->MIX[0][i] = p->red[i] / norm_R;
    d->MIX[1][i] = p->green[i] / norm_G;
    d->MIX[2][i] = p->blue[i] / norm_B;
    d->saturation[i] = -p->saturation[i] + norm_sat;
    d->lightness[i] = p->lightness[i] - norm_light;
    // = NaN if(norm_grey == 0.f) but we don't care since (d->apply_grey == FALSE)
    d->grey[i] = p->grey[i] / norm_grey;
  }

  if(p->version == CHANNELMIXERRGB_V_1)
  {
    // for the v1 saturation algo, the effect of R and B coeffs is reversed
    d->saturation[0] = -p->saturation[2] + norm_sat;
    d->saturation[2] = -p->saturation[0] + norm_sat;
  }

  // just in case compiler feels clever and uses SSE 4×1 dot product
  d->saturation[CHANNEL_SIZE - 1] = 0.0f;
  d->lightness[CHANNEL_SIZE - 1] = 0.0f;
  d->grey[CHANNEL_SIZE - 1] = 0.0f;

  d->adaptation = p->adaptation;
  d->clip = p->clip;
  d->gamut = (p->gamut == 0.f) ? p->gamut : 1.f / p->gamut;

  // find x y coordinates of illuminant for CIE 1931 2° observer
  float x = p->x;
  float y = p->y;
  dt_aligned_pixel_t custom_wb;
  _get_white_balance_coeff(self, custom_wb);
  illuminant_to_xy(p->illuminant, &(self->dev->image_storage),
                   custom_wb, &x, &y, p->temperature, p->illum_fluo, p->illum_led);

  // if illuminant is set as camera, x and y are set on-the-fly at
  // commit time, so we need to set adaptation too
  if(p->illuminant == DT_ILLUMINANT_CAMERA)
    _check_if_close_to_daylight(x, y, NULL, NULL, &(d->adaptation));

  d->illuminant_type = p->illuminant;

  // Convert illuminant from xyY to XYZ
  dt_aligned_pixel_t XYZ;
  illuminant_xy_to_XYZ(x, y, XYZ);

  // Convert illuminant from XYZ to Bradford modified LMS
  convert_any_XYZ_to_LMS(XYZ, d->illuminant, d->adaptation);
  d->illuminant[3] = 0.f;

  const gboolean preview = piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW;
  const gboolean run_profile = preview && g && g->run_profile;
  const gboolean run_validation = preview && g && g->run_validation;

  dt_print(DT_DEBUG_PARAMS,
    "[commit color calibration]%s%s  temp=%i  xy=%.4f %.4f - XYZ=%.4f %.4f %.4f - LMS=%.4f %.4f %.4f  %s",
     run_profile ? " [profile]" : "",
     run_validation ? " [validation]" : "",
     (int)p->temperature, x, y, XYZ[0], XYZ[1], XYZ[2],
     d->illuminant[0], d->illuminant[1], d->illuminant[2],
     dt_introspection_get_enum_name(get_f("illuminant"), d->illuminant_type) ?: "DT_ILLUMINANT_UNDEFINED");

  // blue compensation for Bradford transform = (test illuminant blue
  // / reference illuminant blue)^0.0834 reference illuminant is
  // hard-set D50 for darktable's pipeline test illuminant is user
  // params
  d->p = powf(0.818155f / d->illuminant[2], 0.0834f);

  // Disable OpenCL path if we are in any kind of diagnose mode (only CPU has this)
  if(self->dev->gui_attached && g)
  {
    if(run_profile || run_validation
#ifdef AI_ACTIVATED
        || // delta E validation
        ( (d->illuminant_type == DT_ILLUMINANT_DETECT_EDGES ||
           d->illuminant_type == DT_ILLUMINANT_DETECT_SURFACES ) && // WB extraction mode
           (piece->pipe->type & DT_DEV_PIXELPIPE_FULL) )
#endif
      )
    {
      piece->process_cl_ready = FALSE;
    }
  }

  // if this module has some mask applied we assume it's safe so give no warning
  const dt_develop_blend_params_t *b = (const dt_develop_blend_params_t *)piece->blendop_data;
  const dt_develop_mask_mode_t mask_mode = b ? b->mask_mode : DEVELOP_MASK_DISABLED;
  const gboolean is_blending = (mask_mode & DEVELOP_MASK_ENABLED) && (mask_mode >= DEVELOP_MASK_MASK);
  if(g) g->is_blending = is_blending;
}

static void _update_illuminants(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_params_t *p = self->params;
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  if(p->adaptation == DT_ADAPTATION_RGB
     || p->adaptation == DT_ADAPTATION_LAST)
  {
    // user disabled CAT at all, hide everything and exit
    gtk_widget_set_visible(g->illuminant, FALSE);
    gtk_widget_set_visible(g->illum_color, FALSE);
    gtk_widget_set_visible(g->approx_cct, FALSE);
    gtk_widget_set_visible(g->color_picker, FALSE);
    gtk_widget_set_visible(g->temperature, FALSE);
    gtk_widget_set_visible(g->illum_fluo, FALSE);
    gtk_widget_set_visible(g->illum_led, FALSE);
    gtk_widget_set_visible(g->illum_x, FALSE);
    gtk_widget_set_visible(g->illum_y, FALSE);
    return;
  }
  else
  {
    // set everything visible again and carry on
    gtk_widget_set_visible(g->illuminant, TRUE);
    gtk_widget_set_visible(g->illum_color, TRUE);
    gtk_widget_set_visible(g->approx_cct, TRUE);
    gtk_widget_set_visible(g->color_picker, TRUE);
    gtk_widget_set_visible(g->temperature, TRUE);
    gtk_widget_set_visible(g->illum_fluo, TRUE);
    gtk_widget_set_visible(g->illum_led, TRUE);
    gtk_widget_set_visible(g->illum_x, TRUE);
  }

  // Display only the relevant sliders
  switch(p->illuminant)
  {
    case DT_ILLUMINANT_PIPE:
    case DT_ILLUMINANT_A:
    case DT_ILLUMINANT_E:
    {
      gtk_widget_set_visible(g->adaptation, TRUE);
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_D:
    case DT_ILLUMINANT_BB:
    {
      gtk_widget_set_visible(g->adaptation, TRUE);
      gtk_widget_set_visible(g->temperature, TRUE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_F:
    {
      gtk_widget_set_visible(g->adaptation, TRUE);
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, TRUE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_LED:
    {
      gtk_widget_set_visible(g->adaptation, TRUE);
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, TRUE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_CUSTOM:
    {
      gtk_widget_set_visible(g->adaptation, TRUE);
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, TRUE);
      gtk_widget_set_visible(g->illum_y, TRUE);
      break;
    }
    case DT_ILLUMINANT_CAMERA:
    {
      gtk_widget_set_visible(g->adaptation, TRUE);
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_DETECT_EDGES:
    case DT_ILLUMINANT_DETECT_SURFACES:
    {
      gtk_widget_set_visible(g->adaptation, FALSE);
      gtk_widget_set_visible(g->temperature, FALSE);
      gtk_widget_set_visible(g->illum_fluo, FALSE);
      gtk_widget_set_visible(g->illum_led, FALSE);
      gtk_widget_set_visible(g->illum_x, FALSE);
      gtk_widget_set_visible(g->illum_y, FALSE);
      break;
    }
    case DT_ILLUMINANT_LAST:
    default:
    {
      break;
    }
  }
}

/**
 * DOCUMENTATION
 *
 * The illuminant is stored in params as a set of x and y coordinates,
 * describing its chrominance in xyY color space.  xyY is a normalized
 * XYZ space, derivated from the retina cone sensors. By definition,
 * for an illuminant, Y = 1, so we only really care about (x, y).
 *
 * Using (x, y) is a robust and interoperable way to describe an
 * illuminant, since it is all the actual pixel code needs to perform
 * the chromatic adaptation. This (x, y) can be computed in many
 * different ways or taken from databases, and possibly from other
 * software, so storing only the result let us room to improve the
 * computation in the future, without losing compatibility with older
 * versions.
 *
 * However, it's not a great GUI since x and y are not perceptually
 * scaled. So the `g->illum_x` and `g->illum_y` actually display
 * respectively hue and chroma, in LCh color space, which is designed
 * for illuminants and preceptually spaced. This gives UI controls
 * which effect feels more even to the user.
 *
 * But that makes things a bit tricky, API-wise, since a set of (x, y)
 * depends on a set of (hue, chroma), so they always need to be
 * handled together, but also because the back-and-forth computations
 * Lch <-> xyY need to be done anytime we read or write from/to params
 * from/to GUI.
 *
 * Also, the R, G, B sliders have a background color gradient that
 * shows the actual R, G, B sensors used by the selected chromatic
 * adaptation. Each chromatic adaptation method uses a different RGB
 * space, called LMS in the literature (but it's only a
 * special-purpose RGB space for all we care here), which primaries
 * are projected to sRGB colors, to be displayed in the GUI, so users
 * may get a feeling of what colors they will get.
 **/

static void _update_xy_color(dt_iop_module_t *self)
{
  // update the fill background color of x, y sliders
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = self->params;

  // Varies x in range around current y param
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float x = stop * ILLUM_X_MAX;
    dt_aligned_pixel_t RGB = { 0 };

    dt_aligned_pixel_t Lch = { 100.f, 50.f, x / 180.f * M_PI };
    dt_aligned_pixel_t xyY = { 0 };
    dt_Lch_to_xyY(Lch, xyY);
    illuminant_xy_to_RGB(xyY[0], xyY[1], RGB);
    dt_bauhaus_slider_set_stop(g->illum_x, stop, RGB[0], RGB[1], RGB[2]);

    const float y = stop * ILLUM_Y_MAX / 2.0f;

    // Find current hue
    const dt_aligned_pixel_t xyY2 = { p->x, p->y, 1.f };
    dt_xyY_to_Lch(xyY2, Lch);

    // Replace chroma by current step
    Lch[0] = 75.f;
    Lch[1] = y;

    // Go back to xyY
    dt_Lch_to_xyY(Lch, xyY);
    illuminant_xy_to_RGB(xyY[0], xyY[1], RGB);
    dt_bauhaus_slider_set_stop(g->illum_y, stop, RGB[0], RGB[1], RGB[2]);
  }

  gtk_widget_queue_draw(g->illum_x);
  gtk_widget_queue_draw(g->illum_y);
}

static void _paint_hue(dt_iop_module_t *self)
{
  // update the fill background color of LCh sliders
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  const float hue = dt_bauhaus_slider_get(g->hue_spot);

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    dt_aligned_pixel_t RGB = { 0 }, Lab = { 0 }, XYZ = { 0 };

    const dt_aligned_pixel_t Lch_hue = { 67.f, 96.f, (stop * HUE_MAX) / 360.f};

    dt_LCH_2_Lab(Lch_hue, Lab);
    dt_Lab_to_XYZ(Lab, XYZ);
    dt_XYZ_to_sRGB(XYZ, RGB);

    dt_bauhaus_slider_set_stop(g->hue_spot, stop, RGB[0], RGB[1], RGB[2]);

    const dt_aligned_pixel_t Lch_lightness = { stop * LIGHTNESS_MAX, 0.f, 0. };

    dt_LCH_2_Lab(Lch_lightness, Lab);
    dt_Lab_to_XYZ(Lab, XYZ);
    dt_XYZ_to_sRGB(XYZ, RGB);

    dt_bauhaus_slider_set_stop(g->lightness_spot, stop, RGB[0], RGB[1], RGB[2]);

    const dt_aligned_pixel_t Lch_chroma = { 50., stop * CHROMA_MAX, hue / 360.f };

    dt_LCH_2_Lab(Lch_chroma, Lab);
    dt_Lab_to_XYZ(Lab, XYZ);
    dt_XYZ_to_sRGB(XYZ, RGB);

    dt_bauhaus_slider_set_stop(g->chroma_spot, stop, RGB[0], RGB[1], RGB[2]);
  }

  gtk_widget_queue_draw(g->hue_spot);
  gtk_widget_queue_draw(g->lightness_spot);
  gtk_widget_queue_draw(g->chroma_spot);
  gtk_widget_queue_draw(g->target_spot);
}

static void _convert_GUI_colors(dt_iop_channelmixer_rgb_params_t *p,
                                const dt_iop_order_iccprofile_info_t *const work_profile,
                                const dt_aligned_pixel_t LMS,
                                dt_aligned_pixel_t RGB)
{
  if(p->adaptation != DT_ADAPTATION_RGB)
  {
    convert_any_LMS_to_RGB(LMS, RGB, p->adaptation);
    // RGB vector is normalized with max(RGB)
  }
  else
  {
    dt_aligned_pixel_t XYZ;
    if(work_profile)
    {
      dt_ioppr_rgb_matrix_to_xyz(LMS, XYZ, work_profile->matrix_in_transposed,
                                 work_profile->lut_in,
                                 work_profile->unbounded_coeffs_in,
                                 work_profile->lutsize,
                                 work_profile->nonlinearlut);
      dt_XYZ_to_Rec709_D65(XYZ, RGB);

      // normalize with hue-preserving method (sort-of) to prevent
      // gamut-clipping in sRGB
      const float max_RGB = fmaxf(fmaxf(RGB[0], RGB[1]), RGB[2]);
      for_three_channels(c)
        RGB[c] = fmaxf(RGB[c] / max_RGB, 0.f);
    }
    else
    {
      // work profile not available yet - default to grey
      for_three_channels(c) RGB[c] = 0.5f;
    }
  }
}

static void _update_RGB_slider_stop(dt_iop_channelmixer_rgb_params_t *p,
                                    const dt_iop_order_iccprofile_info_t *const work_profile,
                                    GtkWidget *w,
                                    const float stop,
                                    const float c,
                                    const float r,
                                    const float g,
                                    const float b)
{
  const dt_aligned_pixel_t LMS = { 0.5f * (c * r + 1 - r),
                                   0.5f * (c * g + 1 - g),
                                   0.5f * (c * b + 1 - b)};
  dt_aligned_pixel_t RGB_t = { 0.5f };
  _convert_GUI_colors(p, work_profile, LMS, RGB_t);
  dt_bauhaus_slider_set_stop(w, stop, RGB_t[0], RGB_t[1], RGB_t[2]);
}

static void _update_RGB_colors(dt_iop_module_t *self,
                               const float r,
                               const float g,
                               const float b,
                               const gboolean normalize,
                               float *a,
                               GtkWidget *w_r,
                               GtkWidget *w_g,
                               GtkWidget *w_b)
{
  // update the fill background color of x, y sliders
  dt_iop_channelmixer_rgb_params_t *p = self->params;
  const dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_current_profile_info(self, self->dev->full.pipe);

  // scale params if needed
  dt_aligned_pixel_t RGB = { a[0], a[1], a[2] };

  if(normalize)
  {
    const float sum = RGB[0] + RGB[1] + RGB[2];
    if(sum != 0.0f)
      for_three_channels(c) RGB[c] /= sum;
  }

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    // Use the hard bounds of the sliders; drawing will take into
    // account the possible soft rescaling
    const float range_x = COLOR_MAX - COLOR_MIN;

    const float x = COLOR_MIN + stop * range_x;

    _update_RGB_slider_stop(p, work_profile, w_r, stop, x      + RGB[1] + RGB[2], r, g, b);
    _update_RGB_slider_stop(p, work_profile, w_g, stop, RGB[0] + x      + RGB[2], r, g, b);
    _update_RGB_slider_stop(p, work_profile, w_b, stop, RGB[0] + RGB[1] + x     , r, g, b);
  }

  gtk_widget_queue_draw(w_r);
  gtk_widget_queue_draw(w_b);
  gtk_widget_queue_draw(w_g);
}

static void _paint_temperature_background(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  const float temp_range = TEMP_MAX - TEMP_MIN;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float t = TEMP_MIN + stop * temp_range;
    dt_aligned_pixel_t RGB = { 0 };
    illuminant_CCT_to_RGB(t, RGB);
    dt_bauhaus_slider_set_stop(g->temperature, stop, RGB[0], RGB[1], RGB[2]);
  }
}


static void _update_illuminant_color(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  gtk_widget_queue_draw(g->illum_color);
  _update_xy_color(self);
}

static gboolean _illuminant_color_draw(GtkWidget *widget,
                                       cairo_t *crf,
                                       dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_params_t *p = self->params;

  // Init
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width;
  int height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // Margins
  const double INNER_PADDING = 4.0;
  const float margin = 2. * DT_PIXEL_APPLY_DPI(1.5);
  width -= 2* INNER_PADDING;
  height -= 2 * margin;

  // Paint illuminant color - we need to recompute it in full in case
  // camera RAW is chosen
  float x = p->x;
  float y = p->y;
  dt_aligned_pixel_t RGB = { 0 };
  dt_aligned_pixel_t custom_wb;
  _get_white_balance_coeff(self, custom_wb);
  illuminant_to_xy(p->illuminant, &(self->dev->image_storage), custom_wb,
                   &x, &y, p->temperature, p->illum_fluo, p->illum_led);
  illuminant_xy_to_RGB(x, y, RGB);
  cairo_set_source_rgb(cr, RGB[0], RGB[1], RGB[2]);
  cairo_rectangle(cr, INNER_PADDING, margin, width, height);
  cairo_fill(cr);

  // Clean
  cairo_stroke(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean _target_color_draw(GtkWidget *widget,
                                  cairo_t *crf,
                                  dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  // Init
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width;
  int height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // Margins
  const double INNER_PADDING = 4.0;
  const float margin = 2. * DT_PIXEL_APPLY_DPI(1.5);
  width -= 2* INNER_PADDING;
  height -= 2 * margin;

  // Paint target color
  dt_aligned_pixel_t RGB = { 0 };
  dt_aligned_pixel_t Lch = { 0 };
  dt_aligned_pixel_t Lab = { 0 };
  dt_aligned_pixel_t XYZ = { 0 };
  Lch[0] = dt_bauhaus_slider_get(g->lightness_spot);
  Lch[1] = dt_bauhaus_slider_get(g->chroma_spot);
  Lch[2] = dt_bauhaus_slider_get(g->hue_spot) / 360.f;
  dt_LCH_2_Lab(Lch, Lab);
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_XYZ_to_sRGB(XYZ, RGB);

  cairo_set_source_rgb(cr, RGB[0], RGB[1], RGB[2]);
  cairo_rectangle(cr, INNER_PADDING, margin, width, height);
  cairo_fill(cr);

  // Clean
  cairo_stroke(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean _origin_color_draw(GtkWidget *widget,
                                  cairo_t *crf,
                                  dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  // Init
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width;
  int height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // Margins
  const double INNER_PADDING = 4.0;
  const float margin = 2. * DT_PIXEL_APPLY_DPI(1.5);
  width -= 2* INNER_PADDING;
  height -= 2 * margin;

  cairo_set_source_rgb(cr, g->spot_RGB[0], g->spot_RGB[1], g->spot_RGB[2]);
  cairo_rectangle(cr, INNER_PADDING, margin, width, height);
  cairo_fill(cr);

  // Clean
  cairo_stroke(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static void _update_approx_cct(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = self->params;

  float x = p->x;
  float y = p->y;
  dt_aligned_pixel_t custom_wb;
  _get_white_balance_coeff(self, custom_wb);
  illuminant_to_xy(p->illuminant, &(self->dev->image_storage),
                   custom_wb, &x, &y, p->temperature, p->illum_fluo, p->illum_led);

  dt_illuminant_t test_illuminant;
  float t = 5000.f;
  _check_if_close_to_daylight(x, y, &t, &test_illuminant, NULL);

  gchar *str;
  if(t > 1667.f && t < 25000.f)
  {
    if(test_illuminant == DT_ILLUMINANT_D)
    {
      str = g_strdup_printf(_("CCT: %.0f K (daylight)"), t);
      gtk_widget_set_tooltip_text
        (GTK_WIDGET(g->approx_cct),
         _("approximated correlated color temperature.\n"
           "this illuminant can be accurately modeled by a daylight spectrum,\n"
           "so its temperature is relevant and meaningful with a D illuminant."));
    }
    else if(test_illuminant == DT_ILLUMINANT_BB)
    {
      str = g_strdup_printf(_("CCT: %.0f K (black body)"), t);
      gtk_widget_set_tooltip_text
        (GTK_WIDGET(g->approx_cct),
         _("approximated correlated color temperature.\n"
           "this illuminant can be accurately modeled by a black body spectrum,\n"
           "so its temperature is relevant and meaningful with a Planckian illuminant."));
    }
    else
    {
      str = g_strdup_printf(_("CCT: %.0f K (invalid)"), t);
      gtk_widget_set_tooltip_text
        (GTK_WIDGET(g->approx_cct),
         _("approximated correlated color temperature.\n"
           "this illuminant cannot be accurately modeled by a daylight"
           " or black body spectrum,\n"
           "so its temperature is not relevant and meaningful and you need to use a"
           " custom illuminant."));
    }
  }
  else
  {
    str = g_strdup_printf(_("CCT: undefined"));
    gtk_widget_set_tooltip_text
      (GTK_WIDGET(g->approx_cct),
       _("the approximated correlated color temperature\n"
         "cannot be computed at all so you need to use a custom illuminant."));
  }
  gtk_label_set_text(GTK_LABEL(g->approx_cct), str);
  g_free(str);
}


static void _illum_xy_callback(GtkWidget *slider,
                              dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_channelmixer_rgb_params_t *p = self->params;
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  dt_aligned_pixel_t Lch = { 0 };
  Lch[0] = 100.f;
  Lch[2] = dt_bauhaus_slider_get(g->illum_x) / 180. * M_PI;
  Lch[1] = dt_bauhaus_slider_get(g->illum_y);

  dt_aligned_pixel_t xyY = { 0 };
  dt_Lch_to_xyY(Lch, xyY);
  p->x = xyY[0];
  p->y = xyY[1];

  float t = xy_to_CCT(p->x, p->y);
  // xy_to_CCT is valid only above 3000 K
  if(t < 3000.f) t = CCT_reverse_lookup(p->x, p->y);
  p->temperature = t;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->temperature, p->temperature);
  _update_approx_cct(self);
  _update_illuminant_color(self);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc1_align_type(dt_iop_channelmixer_rbg_data_t);
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_dev_reset_chroma(self->dev);
  dt_free_align(piece->data);
  piece->data = NULL;
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  g->is_profiling_started = FALSE;
  dt_iop_color_picker_reset(self, TRUE);
  gui_changed(self, NULL, NULL);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = self->params;

  dt_iop_color_picker_reset(self, TRUE);

  // always reset the mode the correct
  dt_bauhaus_combobox_set(g->spot_mode, DT_SPOT_MODE_CORRECT);

  // get the saved params
  dt_iop_gui_enter_critical_section(self);

  gboolean use_mixing = TRUE;
  if(dt_conf_key_exists("darkroom/modules/channelmixerrgb/use_mixing"))
    use_mixing = dt_conf_get_bool("darkroom/modules/channelmixerrgb/use_mixing");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->use_mixing), use_mixing);

  float lightness = 50.f;
  if(dt_conf_key_exists("darkroom/modules/channelmixerrgb/lightness"))
    lightness = dt_conf_get_float("darkroom/modules/channelmixerrgb/lightness");
  dt_bauhaus_slider_set(g->lightness_spot, lightness);

  float hue = 0.f;
  if(dt_conf_key_exists("darkroom/modules/channelmixerrgb/hue"))
    hue = dt_conf_get_float("darkroom/modules/channelmixerrgb/hue");
  dt_bauhaus_slider_set(g->hue_spot, hue);

  float chroma = 0.f;
  if(dt_conf_key_exists("darkroom/modules/channelmixerrgb/chroma"))
    chroma = dt_conf_get_float("darkroom/modules/channelmixerrgb/chroma");
  dt_bauhaus_slider_set(g->chroma_spot, chroma);

  dt_iop_gui_leave_critical_section(self);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->clip), p->clip);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_R), p->normalize_R);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_G), p->normalize_G);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_B), p->normalize_B);

  if(p->version != CHANNELMIXERRGB_V_3)
    dt_bauhaus_combobox_set(g->saturation_version, p->version);
  else
    gtk_widget_hide(GTK_WIDGET(g->saturation_version));

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_sat), p->normalize_sat);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_light), p->normalize_light);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->normalize_grey), p->normalize_grey);

  dt_iop_gui_enter_critical_section(self);

  const int i = dt_conf_get_int("darkroom/modules/channelmixerrgb/colorchecker");
  dt_bauhaus_combobox_set(g->checkers_list, i);
  g->checker = dt_get_color_checker(i);

  const int j = dt_conf_get_int("darkroom/modules/channelmixerrgb/optimization");
  dt_bauhaus_combobox_set(g->optimize, j);
  g->optimization = j;

  g->safety_margin = dt_conf_get_float("darkroom/modules/channelmixerrgb/safety");
  dt_bauhaus_slider_set(g->safety, g->safety_margin);

  dt_iop_gui_leave_critical_section(self);

  // always disable profiling mode by default
  g->is_profiling_started = FALSE;

  dt_iop_channelmixer_rgb_params_t *d = self->default_params;
  g->last_daylight_temperature = d->temperature;
  g->last_bb_temperature = d->temperature;

  dt_gui_hide_collapsible_section(&g->cs);
  dt_gui_collapsible_section_set_label(&g->csspot, _area_mapping_section_text(g));
  dt_gui_update_collapsible_section(&g->csspot);

  g->spot_RGB[0] = 0.f;
  g->spot_RGB[1] = 0.f;
  g->spot_RGB[2] = 0.f;
  g->spot_RGB[3] = 0.f;

  gui_changed(self, NULL, NULL);
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  dt_iop_channelmixer_rgb_params_t *d = self->default_params;
  d->red[0] = d->green[1] = d->blue[2] = 1.0;
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_params_t *d = self->default_params;

  d->x = self->get_f("x")->Float.Default;
  d->y = self->get_f("y")->Float.Default;
  d->temperature = self->get_f("temperature")->Float.Default;
  d->illuminant = self->get_f("illuminant")->Enum.Default;
  d->adaptation = self->get_f("adaptation")->Enum.Default;

  const gboolean is_workflow_none = dt_conf_is_equal("plugins/darkroom/workflow", "none");
  const gboolean is_modern = dt_is_scene_referred() || is_workflow_none;

  // note that if there is already an instance of this module with an
  // adaptation set we default to RGB (none) in this instance.
  // try to register the CAT here
  _declare_cat_on_pipe(self, is_modern);
  const dt_image_t *img = &self->dev->image_storage;

  // check if we could register
  const gboolean CAT_already_applied =
    (self->dev->chroma.adaptation != NULL)       // CAT exists
    && (self->dev->chroma.adaptation != self); // and it is not us

  self->default_enabled = FALSE;

  if(CAT_already_applied || dt_image_is_monochrome(img))
  {
    // simple channel mixer
    d->illuminant = DT_ILLUMINANT_PIPE;
    d->adaptation = DT_ADAPTATION_RGB;
  }
  else
  {
    d->adaptation = DT_ADAPTATION_CAT16;

    dt_aligned_pixel_t custom_wb;
    if(!_get_white_balance_coeff(self, custom_wb))
    {
      if(find_temperature_from_raw_coeffs(img, custom_wb, &(d->x), &(d->y)))
        d->illuminant = DT_ILLUMINANT_CAMERA;
      _check_if_close_to_daylight(d->x, d->y,
                                  &(d->temperature), &(d->illuminant), &(d->adaptation));
    }
  }

  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  if(g)
  {
    const dt_aligned_pixel_t xyY = { d->x, d->y, 1.f };
    dt_aligned_pixel_t Lch = { 0 };
    dt_xyY_to_Lch(xyY, Lch);

    dt_bauhaus_slider_set_default(g->illum_x, Lch[2] / M_PI * 180.f);
    dt_bauhaus_slider_set_default(g->illum_y, Lch[1]);
    dt_bauhaus_slider_set_default(g->temperature, d->temperature);
    dt_bauhaus_combobox_set_default(g->illuminant, d->illuminant);
    dt_bauhaus_combobox_set_default(g->adaptation, d->adaptation);
    g->last_daylight_temperature = d->temperature;
    g->last_bb_temperature = d->temperature;

    if(g->delta_E_label_text)
    {
      g_free(g->delta_E_label_text);
      g->delta_E_label_text = NULL;
    }

    const int pos = dt_bauhaus_combobox_get_from_value(g->illuminant, DT_ILLUMINANT_CAMERA);
    if(dt_image_is_matrix_correction_supported(img) && !dt_image_is_monochrome(img))
    {
      if(pos == -1)
        dt_bauhaus_combobox_add_introspection(g->illuminant, NULL,
                                              self->so->get_f("illuminant")->Enum.values,
                                              DT_ILLUMINANT_CAMERA, DT_ILLUMINANT_CAMERA);
    }
    else
      dt_bauhaus_combobox_remove_at(g->illuminant, pos);

    gui_changed(self, NULL, NULL);
  }
}


static void _spot_settings_changed_callback(GtkWidget *slider,
                                            dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  dt_aligned_pixel_t Lch_target = { 0.f };

  dt_iop_gui_enter_critical_section(self);
  Lch_target[0] = dt_bauhaus_slider_get(g->lightness_spot);
  Lch_target[1] = dt_bauhaus_slider_get(g->chroma_spot);
  Lch_target[2] = dt_bauhaus_slider_get(g->hue_spot) / 360.f;
  const gboolean use_mixing =
    gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->use_mixing));
  dt_iop_gui_leave_critical_section(self);

  // Save the color on change
  dt_conf_set_float("darkroom/modules/channelmixerrgb/lightness", Lch_target[0]);
  dt_conf_set_float("darkroom/modules/channelmixerrgb/chroma", Lch_target[1]);
  dt_conf_set_float("darkroom/modules/channelmixerrgb/hue", Lch_target[2] * 360.f);
  dt_conf_set_bool("darkroom/modules/channelmixerrgb/use_mixing", use_mixing);

  // update the "active" flag in the GUI
  dt_gui_collapsible_section_set_label(&g->csspot, _area_mapping_section_text(g));

  _paint_hue(self);

  // Re-run auto illuminant if color picker is active and mode is correct
  const dt_spot_mode_t mode = dt_bauhaus_combobox_get(g->spot_mode);
  if(mode == DT_SPOT_MODE_CORRECT)
    _auto_set_illuminant(self, darktable.develop->full.pipe);
  // else : just record new values and do nothing
}


void gui_changed(dt_iop_module_t *self,
                 GtkWidget *w,
                 void *previous)
{
  dt_iop_channelmixer_rgb_params_t *p = self->params;
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;

  if(w == g->illuminant)
  {
    if(previous)
    {
      dt_illuminant_t *prev_illuminant = (dt_illuminant_t *)previous;
      if(*prev_illuminant == DT_ILLUMINANT_CAMERA)
      {
        // If illuminant was previously set with "as set in camera",
        // when changing it, we need to ensure the temperature and
        // chromaticity are inited with the correct values taken from
        // camera EXIF.  Otherwise, if using a preset defining
        // illuminant = "as set in camera", temperature and
        // chromaticity are inited with the preset content when
        // illuminant is changed.
        dt_aligned_pixel_t custom_wb;
        _get_white_balance_coeff(self, custom_wb);
        find_temperature_from_raw_coeffs(&(self->dev->image_storage), custom_wb,
                                         &(p->x), &(p->y));
        _check_if_close_to_daylight(p->x, p->y, &(p->temperature), NULL, &(p->adaptation));
      }
    }

    if(p->illuminant == DT_ILLUMINANT_D)
      p->temperature = g->last_daylight_temperature;

    if(p->illuminant == DT_ILLUMINANT_BB)
      p->temperature = g->last_bb_temperature;

    if(p->illuminant == DT_ILLUMINANT_CAMERA)
    {
      // Get camera WB and update illuminant
      dt_aligned_pixel_t custom_wb;
      _get_white_balance_coeff(self, custom_wb);
      const gboolean found = find_temperature_from_raw_coeffs(&(self->dev->image_storage),
                                                         custom_wb, &(p->x), &(p->y));
      _check_if_close_to_daylight(p->x, p->y, &(p->temperature), NULL, &(p->adaptation));

      if(found)
        dt_control_log(_("white balance successfully extracted from raw image"));
    }
#ifdef AI_ACTIVATED
    else if(p->illuminant == DT_ILLUMINANT_DETECT_EDGES
            || p->illuminant == DT_ILLUMINANT_DETECT_SURFACES)
    {
      // We need to recompute only the full preview
      dt_control_log(_("auto-detection of white balance started…"));
    }
#endif
  }

  if(w == g->temperature)
  {
    if(p->illuminant == DT_ILLUMINANT_D)
      g->last_daylight_temperature = p->temperature;
    if(p->illuminant == DT_ILLUMINANT_BB)
      g->last_bb_temperature = p->temperature;
  }

  if(w == g->illuminant
     || w == g->illum_fluo
     || w == g->illum_led
     || w == g->temperature)
  {
    // Convert and synchronize all the possible ways to define an
    // illuminant to allow swapping modes

    if(p->illuminant != DT_ILLUMINANT_CUSTOM
       && p->illuminant != DT_ILLUMINANT_CAMERA)
    {
      // We are in any mode defining (x, y) indirectly from an
      // interface, so commit (x, y) explicitly
      illuminant_to_xy(p->illuminant, NULL, NULL, &(p->x), &(p->y),
                       p->temperature, p->illum_fluo, p->illum_led);
    }

    if(p->illuminant != DT_ILLUMINANT_D
       && p->illuminant != DT_ILLUMINANT_BB
       && p->illuminant != DT_ILLUMINANT_CAMERA)
    {
      // We are in any mode not defining explicitly a temperature, so
      // find the the closest CCT and commit it
      _check_if_close_to_daylight(p->x, p->y, &(p->temperature), NULL, NULL);
    }
  }

  ++darktable.gui->reset;

  if(!w
     || w == g->hue_spot
     || w == g->chroma_spot
     || w == g->lightness_spot
     || w == g->spot_settings)
  {
    _paint_hue(self);
  }

  if(!w
     || w == g->illuminant
     || w == g->illum_fluo
     || w == g->illum_led
     || w == g->temperature)
  {
    _update_illuminants(self);
    _update_approx_cct(self);
    _update_illuminant_color(self);

    // force-update all the illuminant sliders in case something above
    // changed them notice the hue/chroma of the illuminant has to be
    // computed on-the-fly anyway
    dt_aligned_pixel_t xyY = { p->x, p->y, 1.f };
    dt_aligned_pixel_t Lch;
    dt_xyY_to_Lch(xyY, Lch);

    // If the chroma is zero then there is not a meaningful hue
    // angle. In this case leave the hue slider where it was, so that
    // if chroma is set to zero and then set to a nonzero value, the
    // hue setting will remain unchanged.
    if(Lch[1] > 0)
      dt_bauhaus_slider_set(g->illum_x, Lch[2] / M_PI * 180.f);
    dt_bauhaus_slider_set(g->illum_y, Lch[1]);

    dt_bauhaus_slider_set(g->temperature, p->temperature);
  }

  if(w == g->adaptation)
    _update_illuminants(self);

  if(!w || w == g->adaptation
     || w == g->scale_red_R
     || w == g->scale_red_G
     || w == g->scale_red_B
     || w == g->normalize_R)
    _update_RGB_colors(self, 1, 0, 0, p->normalize_R,
                       p->red, g->scale_red_R, g->scale_red_G, g->scale_red_B);

  if(!w || w == g->adaptation
     || w == g->scale_green_R
     || w == g->scale_green_G
     || w == g->scale_green_B
     || w == g->normalize_G)
    _update_RGB_colors(self, 0, 1, 0, p->normalize_G,
                       p->green, g->scale_green_R, g->scale_green_G, g->scale_green_B);

  if(!w || w == g->adaptation
     || w == g->scale_blue_R
     || w == g->scale_blue_G
     || w == g->scale_blue_B
     || w == g->normalize_B)
    _update_RGB_colors(self, 0, 0, 1, p->normalize_B,
                       p->blue, g->scale_blue_R, g->scale_blue_G, g->scale_blue_B);

  // if grey channel is used and norm = 0 and normalization = ON, we
  // are going to have a division by zero in commit_param, we avoid
  // dividing by zero automatically, but user needs a notification
  if((p->grey[0] != 0.f) || (p->grey[1] != 0.f) || (p->grey[2] != 0.f))
    if((p->grey[0] + p->grey[1] + p->grey[2] == 0.f) && p->normalize_grey)
      dt_control_log(_("color calibration: the sum of the gray channel parameters"
                       " is zero, normalization will be disabled."));

  // If "as shot in camera" illuminant is used, CAT space is forced automatically
  // therefore, make the control insensitive
  gtk_widget_set_sensitive(g->adaptation, p->illuminant != DT_ILLUMINANT_CAMERA);

  _declare_cat_on_pipe(self, FALSE);

  --darktable.gui->reset;
}

void gui_focus(dt_iop_module_t *self, gboolean in)
{
  gui_changed(self, NULL, NULL);
}

static void _auto_set_illuminant(dt_iop_module_t *self,
                                 dt_dev_pixelpipe_t *pipe)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  dt_iop_channelmixer_rgb_params_t *p = self->params;

  // capture gui color picked event.
  if(self->picked_color_max[0] < self->picked_color_min[0]) return;
  const float *RGB = self->picked_color;

  // Get work profile
  const dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_work_profile_info(pipe);
  if(work_profile == NULL) return;

  // Convert to XYZ
  dt_aligned_pixel_t XYZ;
  dot_product(RGB, work_profile->matrix_in, XYZ);
  dt_XYZ_to_sRGB(XYZ, g->spot_RGB);

  // Convert to Lch for GUI feedback (input)
  dt_aligned_pixel_t Lab;
  dt_aligned_pixel_t Lch;
  dt_XYZ_to_Lab(XYZ, Lab);
  dt_Lab_2_LCH(Lab, Lch);

  // Write report in GUI
  gchar *str = g_strdup_printf(_("L: \t%.1f %%\nh: \t%.1f °\nc: \t%.1f"),
                               Lch[0], Lch[2] * 360.f, Lch[1]);
  gtk_label_set_text(GTK_LABEL(g->Lch_origin), str);
  g_free(str);

  const dt_spot_mode_t mode = dt_bauhaus_combobox_get(g->spot_mode);
  const gboolean use_mixing =
    gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->use_mixing));

  // build the channel mixing matrix - keep in synch with commit_params()
  dt_colormatrix_t MIX = { { 0.f } };

  float norm_R = 1.0f;
  if(p->normalize_R)
    norm_R = MAX(NORM_MIN, p->red[0] + p->red[1] + p->red[2]);

  float norm_G = 1.0f;
  if(p->normalize_G)
    norm_G = MAX(NORM_MIN, p->green[0] + p->green[1] + p->green[2]);

  float norm_B = 1.0f;
  if(p->normalize_B)
    norm_B = MAX(NORM_MIN, p->blue[0] + p->blue[1] + p->blue[2]);

  for_three_channels(i)
  {
    MIX[0][i] = p->red[i] / norm_R;
    MIX[1][i] = p->green[i] / norm_G;
    MIX[2][i] = p->blue[i] / norm_B;
  }

  if(mode == DT_SPOT_MODE_MEASURE)
  {
    // Keep the following in sync with commit_params()

    // find x y coordinates of illuminant for CIE 1931 2° observer
    float x = p->x;
    float y = p->y;
    dt_adaptation_t adaptation = p->adaptation;
    dt_aligned_pixel_t custom_wb;
    _get_white_balance_coeff(self, custom_wb);
    illuminant_to_xy(p->illuminant, &(self->dev->image_storage),
                     custom_wb, &x, &y, p->temperature, p->illum_fluo, p->illum_led);

    // if illuminant is set as camera, x and y are set on-the-fly at
    // commit time, so we need to set adaptation too
    if(p->illuminant == DT_ILLUMINANT_CAMERA)
      _check_if_close_to_daylight(x, y, NULL, NULL, &adaptation);

    // Convert illuminant from xyY to XYZ
    dt_aligned_pixel_t XYZ_illuminant = { 0.f };
    illuminant_xy_to_XYZ(x, y, XYZ_illuminant);

    // Convert illuminant from XYZ to Bradford modified LMS
    dt_aligned_pixel_t LMS_illuminant = { 0.f };
    convert_any_XYZ_to_LMS(XYZ_illuminant, LMS_illuminant, adaptation);

    // For the non-linear Bradford
    const float pp = powf(0.818155f / MAX(NORM_MIN, LMS_illuminant[2]), 0.0834f);

    //fprintf(stdout, "illuminant: %i\n", p->illuminant);
    //fprintf(stdout, "x: %f, y: %f\n", x, y);
    //fprintf(stdout, "X: %f - Y: %f - Z: %f\n", XYZ_illuminant[0], XYZ_illuminant[1], XYZ_illuminant[2]);
    //fprintf(stdout, "L: %f - M: %f - S: %f\n", LMS_illuminant[0], LMS_illuminant[1], LMS_illuminant[2]);

    // Finally, chroma-adapt the pixel
    dt_aligned_pixel_t XYZ_output = { 0.f };
    chroma_adapt_pixel(XYZ, XYZ_output, LMS_illuminant, adaptation, pp);

    // Optionally, apply the channel mixing
    if(use_mixing)
    {
      dt_aligned_pixel_t LMS_output = { 0.f };
      convert_any_XYZ_to_LMS(XYZ_output, LMS_output, adaptation);
      dt_aligned_pixel_t temp = { 0.f };
      dot_product(LMS_output, MIX, temp);
      convert_any_LMS_to_XYZ(temp, XYZ_output, adaptation);
    }

    // Convert to Lab and Lch for GUI feedback
    dt_aligned_pixel_t Lab_output = { 0.f };
    dt_aligned_pixel_t Lch_output = { 0.f };
    dt_XYZ_to_Lab(XYZ_output, Lab_output);
    dt_Lab_2_LCH(Lab_output, Lch_output);

    // Return the values in sliders
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->lightness_spot, Lch_output[0]);
    dt_bauhaus_slider_set(g->chroma_spot, Lch_output[1]);
    dt_bauhaus_slider_set(g->hue_spot, Lch_output[2] * 360.f);
    _paint_hue(self);
    --darktable.gui->reset;

    dt_conf_set_float("darkroom/modules/channelmixerrgb/lightness", Lch_output[0]);
    dt_conf_set_float("darkroom/modules/channelmixerrgb/chroma", Lch_output[1]);
    dt_conf_set_float("darkroom/modules/channelmixerrgb/hue", Lch_output[2] * 360.f);
    dt_conf_set_bool("darkroom/modules/channelmixerrgb/use_mixing", use_mixing);
  }
  else if(mode == DT_SPOT_MODE_CORRECT)
  {
    // Get the target color in LMS space
    dt_aligned_pixel_t Lch_target = { 0.f };
    dt_aligned_pixel_t Lab_target = { 0.f };
    dt_aligned_pixel_t XYZ_target = { 0.f };
    dt_aligned_pixel_t LMS_target = { 0.f };

    dt_iop_gui_enter_critical_section(self);
    Lch_target[0] = dt_bauhaus_slider_get(g->lightness_spot);
    Lch_target[1] = dt_bauhaus_slider_get(g->chroma_spot);
    Lch_target[2] = dt_bauhaus_slider_get(g->hue_spot) / 360.f;
    dt_iop_gui_leave_critical_section(self);

    dt_LCH_2_Lab(Lch_target, Lab_target);
    dt_Lab_to_XYZ(Lab_target, XYZ_target);
    const float Y_target = MAX(NORM_MIN, XYZ_target[1]);
    dt_vector_div1(XYZ_target, XYZ_target, Y_target);
    convert_any_XYZ_to_LMS(XYZ_target, LMS_target, p->adaptation);

    // optionally, apply the inverse mixing on the target
    if(use_mixing)
    {
      // Repack the MIX matrix to 3×3 to support the pseudoinverse function
      // I'm just too lazy to rewrite the pseudo-inverse for 3×4 padded input
      float MIX_3x3[9];
      pack_3xSSE_to_3x3(MIX, MIX_3x3);

      /* DEBUG
      fprintf(stdout, "Repacked channel mixer matrix :\n");
      fprintf(stdout, "%f \t%f \t%f\n", MIX_3x3[0][0], MIX_3x3[0][1], MIX_3x3[0][2]);
      fprintf(stdout, "%f \t%f \t%f\n", MIX_3x3[1][0], MIX_3x3[1][1], MIX_3x3[1][2]);
      fprintf(stdout, "%f \t%f \t%f\n", MIX_3x3[2][0], MIX_3x3[2][1], MIX_3x3[2][2]);
      */

      // Invert the matrix
      float MIX_INV_3x3[9];
      matrice_pseudoinverse((float (*)[3])MIX_3x3, (float (*)[3])MIX_INV_3x3, 3);

      // Transpose and repack the inverse to SSE matrix because the inversion transposes too
      dt_colormatrix_t MIX_INV;
      transpose_3x3_to_3xSSE(MIX_INV_3x3, MIX_INV);

      /* DEBUG
      fprintf(stdout, "Repacked inverted channel mixer matrix :\n");
      fprintf(stdout, "%f \t%f \t%f\n", MIX_INV[0][0], MIX_INV[0][1], MIX_INV[0][2]);
      fprintf(stdout, "%f \t%f \t%f\n", MIX_INV[1][0], MIX_INV[1][1], MIX_INV[1][2]);
      fprintf(stdout, "%f \t%f \t%f\n", MIX_INV[2][0], MIX_INV[2][1], MIX_INV[2][2]);
      */

      // Undo the channel mixing on the reference color
      // So we get the expected target color after the CAT
      dt_aligned_pixel_t temp = { 0.0f };
      dot_product(LMS_target, MIX_INV, temp);

      //fprintf(stdout, "LMS before channel mixer inversion : \t%f \t%f \t%f\n", LMS_target[0], LMS_target[1], LMS_target[2]);
      //fprintf(stdout, "LMS after channel mixer inversion : \t%f \t%f \t%f\n", temp[0], temp[1], temp[2]);

      // convert back to XYZ to normalize luminance again
      // in case the matrix is not normalized
      convert_any_LMS_to_XYZ(temp, XYZ_target, p->adaptation);
      const float Y_mix = MAX(NORM_MIN, XYZ_target[1]);
      dt_vector_div1(XYZ_target, XYZ_target, Y_mix);
      convert_any_XYZ_to_LMS(XYZ_target, LMS_target, p->adaptation);

      //fprintf(stdout, "LMS target after everything : %f \t%f \t%f\n", LMS_target[0], LMS_target[1], LMS_target[2]);

      // So now we got the target color after CAT and before mixing
      // in LMS space
    }

    // Get the input color in LMS space
    dt_aligned_pixel_t LMS = { 0.f };
    const float Y = MAX(NORM_MIN, XYZ[1]);
    dt_vector_div1(XYZ, XYZ, Y);
    convert_any_XYZ_to_LMS(XYZ, LMS, p->adaptation);

    // Find the illuminant
    dt_aligned_pixel_t D50;
    convert_D50_to_LMS(p->adaptation, D50);

    dt_aligned_pixel_t illuminant_LMS = { 0.f };
    dt_aligned_pixel_t illuminant_XYZ = { 0.f };

    // We solve the equation : target color / input color = D50 / illuminant, for illuminant
    for_three_channels(c) illuminant_LMS[c] = D50[c] * LMS[c] / LMS_target[c];
    convert_any_LMS_to_XYZ(illuminant_LMS, illuminant_XYZ, p->adaptation);

    // Convert to xyY
    const float sum = fmaxf(illuminant_XYZ[0] + illuminant_XYZ[1] + illuminant_XYZ[2],
                            NORM_MIN);
    illuminant_XYZ[0] /= sum;              // x
    illuminant_XYZ[2] = illuminant_XYZ[1]; // Y
    illuminant_XYZ[1] /= sum;              // y

    p->x = illuminant_XYZ[0];
    p->y = illuminant_XYZ[1];

    // Force illuminant to custom, the daylight/black body approximations are
    // not accurate enough for color matching
    p->illuminant = DT_ILLUMINANT_CUSTOM;

    ++darktable.gui->reset;

    _check_if_close_to_daylight(p->x, p->y, &p->temperature, NULL, NULL);

    dt_bauhaus_slider_set(g->temperature, p->temperature);
    dt_bauhaus_combobox_set(g->illuminant, p->illuminant);
    dt_bauhaus_combobox_set(g->adaptation, p->adaptation);

    const dt_aligned_pixel_t xyY = { p->x, p->y, 1.f };
    dt_aligned_pixel_t Lch_illuminant = { 0 };
    dt_xyY_to_Lch(xyY, Lch_illuminant);
    dt_bauhaus_slider_set(g->illum_x, Lch_illuminant[2] / M_PI * 180.f);
    dt_bauhaus_slider_set(g->illum_y, Lch_illuminant[1]);

    _update_illuminants(self);
    _update_approx_cct(self);
    _update_illuminant_color(self);
    _paint_hue(self);
    gtk_widget_queue_draw(g->origin_spot);

    --darktable.gui->reset;

    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}


void color_picker_apply(dt_iop_module_t *self,
                        GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  if(darktable.gui->reset) return;
  _auto_set_illuminant(self, pipe);
}


void gui_init(dt_iop_module_t *self)
{
  dt_iop_channelmixer_rgb_gui_data_t *g = IOP_GUI_ALLOC(channelmixer_rgb);

  // Init the color checker UI
  for(size_t k = 0; k < 4; k++)
  {
    g->box[k].x = g->box[k].y = -1.;
    g->active_node[k] = FALSE;
  }
  g->is_cursor_close = FALSE;
  g->drag_drop = FALSE;
  g->is_profiling_started = FALSE;
  g->run_profile = FALSE;
  g->run_validation = FALSE;
  g->profile_ready = FALSE;
  g->checker_ready = FALSE;
  g->delta_E_in = NULL;
  g->delta_E_label_text = NULL;

  g->XYZ[0] = NAN;

#ifdef AI_ACTIVATED
   DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED, _develop_ui_pipe_finished_callback);
#endif
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, _preview_pipe_finished_callback);

  // Init GTK notebook
  static dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);

  // Page CAT
  self->widget = dt_ui_notebook_page(g->notebook, N_("CAT"),
                                     _("chromatic adaptation transform"));

  g->adaptation = dt_bauhaus_combobox_from_params(self, N_("adaptation"));
  gtk_widget_set_tooltip_text
    (GTK_WIDGET(g->adaptation),
     _("choose the method to adapt the illuminant\n"
       "and the colorspace in which the module works: \n"
       "• Linear Bradford (1985) is consistent with ICC v4 toolchain.\n"
       "• CAT16 (2016) is more robust and accurate.\n"
       "• Non-linear Bradford (1985) is the original Bradford,\n"
       "it can produce better results than the linear version, but is unreliable.\n"
       "• XYZ is a simple scaling in XYZ space. It is not recommended in general.\n"
       "• none disables any adaptation and uses pipeline working RGB."));

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  g->approx_cct = dt_ui_label_new("CCT:");
  gtk_box_pack_start(GTK_BOX(hbox), g->approx_cct, FALSE, FALSE, 0);

  g->illum_color = GTK_WIDGET(gtk_drawing_area_new());
  gtk_widget_set_size_request
    (g->illum_color, 2 * DT_PIXEL_APPLY_DPI(darktable.bauhaus->quad_width),
     DT_PIXEL_APPLY_DPI(darktable.bauhaus->quad_width));
  gtk_widget_set_tooltip_text
    (GTK_WIDGET(g->illum_color),
     _("this is the color of the scene illuminant before chromatic adaptation\n"
       "this color will be turned into pure white by the adaptation."));

  g_signal_connect(G_OBJECT(g->illum_color), "draw",
                   G_CALLBACK(_illuminant_color_draw), self);
  gtk_box_pack_start(GTK_BOX(hbox), g->illum_color, TRUE, TRUE, 0);

  g->color_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, hbox);
  dt_action_define_iop(self, NULL, N_("picker"), g->color_picker, &dt_action_def_toggle);
  gtk_widget_set_tooltip_text(g->color_picker,
                              _("set white balance to detected from area"));

  dt_gui_box_add(self->widget, hbox);

  g->illuminant = dt_bauhaus_combobox_from_params(self, N_("illuminant"));

  g->illum_fluo = dt_bauhaus_combobox_from_params(self, "illum_fluo");

  g->illum_led = dt_bauhaus_combobox_from_params(self, "illum_led");

  g->temperature = dt_bauhaus_slider_from_params(self, N_("temperature"));
  dt_bauhaus_slider_set_soft_range(g->temperature, 3000., 7000.);
  dt_bauhaus_slider_set_digits(g->temperature, 0);
  dt_bauhaus_slider_set_format(g->temperature, " K");
  _paint_temperature_background(self);

  g->illum_x = dt_bauhaus_slider_new_with_range_and_feedback
    (self, 0., ILLUM_X_MAX, 0, 0, 1, 0);
  dt_bauhaus_widget_set_label(g->illum_x, NULL, N_("hue"));
  dt_bauhaus_slider_set_format(g->illum_x, "°");
  g_signal_connect(G_OBJECT(g->illum_x), "value-changed",
                   G_CALLBACK(_illum_xy_callback), self);

  g->illum_y = dt_bauhaus_slider_new_with_range(self, 0., 100., 0, 0, 1);
  dt_bauhaus_widget_set_label(g->illum_y, NULL, N_("chroma"));
  dt_bauhaus_slider_set_format(g->illum_y, "%");
  dt_bauhaus_slider_set_hard_max(g->illum_y, ILLUM_Y_MAX);
  g_signal_connect(G_OBJECT(g->illum_y), "value-changed",
                   G_CALLBACK(_illum_xy_callback), self);

  dt_gui_box_add(self->widget, g->illum_x, g->illum_y);

  g->gamut = dt_bauhaus_slider_from_params(self, "gamut");
  dt_bauhaus_slider_set_soft_max(g->gamut, 4.f);

  g->clip = dt_bauhaus_toggle_from_params(self, "clip");

  // Add the color mapping collapsible panel

  dt_gui_new_collapsible_section
    (&g->csspot,
     "plugins/darkroom/channelmixerrgb/expand_picker_mapping",
     _area_mapping_section_text(g),
     GTK_BOX(self->widget),
     DT_ACTION(self));

  gtk_widget_set_tooltip_text
    (g->csspot.expander,
     _("define a target chromaticity (hue and chroma) for a particular region"
       " of the image (the control sample), which you then match against the"
       " same target chromaticity in other images. the control sample can either"
       " be a critical part of your subject or a non-moving and consistently-lit"
       " surface over your series of images."));

  DT_BAUHAUS_COMBOBOX_NEW_FULL
    (g->spot_mode, self, N_("mapping"), N_("area mode"),
     _("\"correction\" automatically adjust the illuminant\n"
       "such that the input color is mapped to the target.\n"
       "\"measure\" simply shows how an input color is mapped by the CAT\n"
       "and can be used to sample a target."),
     0, NULL, self,
     N_("correction"),
     N_("measure"));
  g_signal_connect(G_OBJECT(g->spot_mode), "value-changed",
                   G_CALLBACK(_spot_settings_changed_callback), self);

  gchar *label = N_("take channel mixing into account");
  g->use_mixing = gtk_check_button_new_with_label(_(label));
  dt_action_define_iop(self, N_("mapping"), label, g->use_mixing, &dt_action_def_toggle);
  gtk_label_set_ellipsize
    (GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->use_mixing))), PANGO_ELLIPSIZE_END);
  gtk_widget_set_tooltip_text
    (g->use_mixing,
     _("compute the target by taking the channel mixing into account.\n"
       "if disabled, only the CAT is considered."));
  g_signal_connect(G_OBJECT(g->use_mixing), "toggled",
                   G_CALLBACK(_spot_settings_changed_callback), self);

  g->origin_spot = gtk_drawing_area_new();
  gtk_widget_set_vexpand(g->origin_spot, TRUE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->origin_spot),
                              _("the input color that should be mapped to the target"));
  g_signal_connect(G_OBJECT(g->origin_spot), "draw",
                   G_CALLBACK(_origin_color_draw), self);

  g->Lch_origin = gtk_label_new(_("L: \tN/A\nh: \tN/A\nc: \tN/A"));
  gtk_widget_set_tooltip_text
    (GTK_WIDGET(g->Lch_origin),
     _("these LCh coordinates are computed from CIE Lab 1976 coordinates"));

  g->target_spot = GTK_WIDGET(gtk_drawing_area_new());
  gtk_widget_set_size_request(g->target_spot, -1, DT_PIXEL_APPLY_DPI(darktable.bauhaus->quad_width));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->target_spot),
                              _("the desired target color after mapping"));
  g_signal_connect(G_OBJECT(g->target_spot), "draw", G_CALLBACK(_target_color_draw), self);

  g->lightness_spot = dt_bauhaus_slider_new_with_range(self, 0., LIGHTNESS_MAX, 0, 0, 1);
  dt_bauhaus_widget_set_label(g->lightness_spot, N_("mapping"), N_("lightness"));
  dt_bauhaus_slider_set_format(g->lightness_spot, "%");
  dt_bauhaus_slider_set_default(g->lightness_spot, 50.f);
  g_signal_connect(G_OBJECT(g->lightness_spot), "value-changed",
                   G_CALLBACK(_spot_settings_changed_callback), self);

  g->hue_spot = dt_bauhaus_slider_new_with_range_and_feedback
    (self, 0., HUE_MAX, 0, 0, 1, 0);
  dt_bauhaus_widget_set_label(g->hue_spot, N_("mapping"), N_("hue"));
  dt_bauhaus_slider_set_format(g->hue_spot, "°");
  dt_bauhaus_slider_set_default(g->hue_spot, 0.f);
  g_signal_connect(G_OBJECT(g->hue_spot), "value-changed",
                   G_CALLBACK(_spot_settings_changed_callback), self);

  g->chroma_spot = dt_bauhaus_slider_new_with_range(self, 0., CHROMA_MAX, 0, 0, 1);
  dt_bauhaus_widget_set_label(g->chroma_spot, N_("mapping"), N_("chroma"));
  dt_bauhaus_slider_set_default(g->chroma_spot, 0.f);
  g_signal_connect(G_OBJECT(g->chroma_spot), "value-changed",
                   G_CALLBACK(_spot_settings_changed_callback), self);

  dt_gui_box_add(g->csspot.container, 
                 g->spot_mode, g->use_mixing,
                 dt_gui_hbox(
                 dt_gui_vbox(dt_ui_section_label_new(C_("section", "input")),
                             g->origin_spot, g->Lch_origin),
                 dt_gui_expand(
                 dt_gui_vbox(dt_ui_section_label_new(C_("section", "target")),
                             g->target_spot, g->lightness_spot, g->hue_spot, g->chroma_spot)
                 )));

  gtk_widget_set_margin_end(gtk_widget_get_parent(g->origin_spot), DT_PIXEL_APPLY_DPI(darktable.bauhaus->quad_width));
  dt_gui_collapsible_section_set_label(&g->csspot, _area_mapping_section_text(g));

  GtkWidget *first, *second, *third;
#define NOTEBOOK_PAGE(var, short, label, tooltip, section, swap, soft_range, sr_min, sr_max) \
  self->widget = dt_ui_notebook_page(g->notebook, label, _(tooltip));            \
                                                                                 \
  first = dt_bauhaus_slider_from_params(self, swap ? #var "[2]" : #var "[0]");   \
  dt_bauhaus_slider_set_digits(first, 3);                                        \
  dt_bauhaus_widget_set_label(first, section, N_("input R"));                    \
  if(soft_range) dt_bauhaus_slider_set_soft_range(first, sr_min, sr_max);        \
                                                                                 \
  second = dt_bauhaus_slider_from_params(self, #var "[1]");                      \
  dt_bauhaus_slider_set_digits(second, 3);                                       \
  dt_bauhaus_widget_set_label(second, section, N_("input G"));                   \
  if(soft_range) dt_bauhaus_slider_set_soft_range(second, sr_min, sr_max);       \
                                                                                 \
  third = dt_bauhaus_slider_from_params(self, swap ? #var "[0]" : #var "[2]");   \
  dt_bauhaus_slider_set_digits(third, 3);                                        \
  dt_bauhaus_widget_set_label(third, section, N_("input B"));                    \
  if(soft_range) dt_bauhaus_slider_set_soft_range(third, sr_min, sr_max);        \
                                                                                 \
  g->scale_##var##_R = swap ? third : first;                                     \
  g->scale_##var##_G = second;                                                   \
  g->scale_##var##_B = swap ? first : third;                                     \
                                                                                 \
  g->normalize_##short = dt_bauhaus_toggle_from_params                           \
               (DT_IOP_SECTION_FOR_PARAMS(self, section), "normalize_" #short);

  NOTEBOOK_PAGE(red, R, N_("R"), N_("output R"), N_("red"), FALSE, FALSE, 0.0, 0.0)
  NOTEBOOK_PAGE(green, G, N_("G"), N_("output G"), N_("green"), FALSE, FALSE, 0.0, 0.0)
  NOTEBOOK_PAGE(blue, B, N_("B"), N_("output B"), N_("blue"), FALSE, FALSE, 0.0, 0.0)
  NOTEBOOK_PAGE(saturation, sat,
                N_("colorfulness"), N_("output colorfulness"), N_("colorfulness"),
                FALSE, TRUE, -1.0, 1.0)
  g->saturation_version = dt_bauhaus_combobox_from_params(self, "version");
  NOTEBOOK_PAGE(lightness, light,
                N_("brightness"), N_("output brightness"), N_("brightness"),
                FALSE, TRUE, -1.0, 1.0)
  NOTEBOOK_PAGE(grey, grey,
                N_("gray"), N_("output gray"), N_("gray"), FALSE, TRUE, 0.0, 1.0)

  // start building top level widget
  self->widget = dt_gui_vbox(g->notebook);
  const int active_page = dt_conf_get_int("plugins/darkroom/channelmixerrgb/gui_page");
  gtk_widget_show(gtk_notebook_get_nth_page(g->notebook, active_page));
  gtk_notebook_set_current_page(g->notebook, active_page);

  // Add the color checker collapsible panel

  dt_gui_new_collapsible_section
    (&g->cs,
     "plugins/darkroom/channelmixerrgb/expand_values",
     _("calibrate with a color checker"),
     GTK_BOX(self->widget),
     DT_ACTION(self));

  gtk_widget_set_tooltip_text(g->cs.expander,
                              _("use a color checker target to autoset CAT and channels"));
  g_signal_connect(G_OBJECT(g->cs.toggle), "toggled",
                   G_CALLBACK(_start_profiling_callback), self);

  DT_BAUHAUS_COMBOBOX_NEW_FULL
    (g->checkers_list, self, N_("calibrate"), N_("chart"),
     _("choose the vendor and the type of your chart"),
     0, _checker_changed_callback, self,
     N_("Xrite ColorChecker 24 pre-2014"),
     N_("Xrite/Calibrite ColorChecker 24 post-2014"),
     N_("Datacolor SpyderCheckr 24 pre-2018"),
     N_("Datacolor SpyderCheckr 24 post-2018"),
     N_("Datacolor SpyderCheckr 48 pre-2018"),
     N_("Datacolor SpyderCheckr 48 post-2018"),
     N_("Datacolor SpyderCheckr Photo"));

  DT_BAUHAUS_COMBOBOX_NEW_FULL
    (g->optimize, self, N_("calibrate"), N_("optimize for"),
     _("choose the colors that will be optimized with higher priority.\n"
       "neutral colors gives the lowest average delta E but a high maximum delta E\n"
       "saturated colors gives the lowest maximum delta E but a high average delta E\n"
       "none is a trade-off between both\n"
       "the others are special behaviors to protect some hues"),
     0, _optimize_changed_callback, self,
     N_("none"),
     N_("neutral colors"),
     N_("saturated colors"),
     N_("skin and soil colors"),
     N_("foliage colors"),
     N_("sky and water colors"),
     N_("average delta E"),
     N_("maximum delta E"));

  g->safety = dt_bauhaus_slider_new_with_range_and_feedback(self, 0., 1., 0, 0.5, 3, TRUE);
  dt_bauhaus_widget_set_label(g->safety, N_("calibrate"), N_("patch scale"));
  gtk_widget_set_tooltip_text
    (g->safety,
     _("reduce the radius of the patches to select the more or less central part.\n"
       "useful when the perspective correction is sloppy or\n"
       "the patches frame cast a shadows on the edges of the patch." ));
  g_signal_connect(G_OBJECT(g->safety), "value-changed",
                   G_CALLBACK(_safety_changed_callback), self);

  g->label_delta_E = dt_ui_label_new("");
  gtk_widget_set_tooltip_text(g->label_delta_E,
                              _("the delta E is using the CIE 2000 formula"));

  g->button_commit = dtgtk_button_new(dtgtk_cairo_paint_check_mark, 0, NULL);
  dt_action_define_iop(self, N_("calibrate"), N_("accept"),
                       g->button_commit, &dt_action_def_button);
  g_signal_connect(G_OBJECT(g->button_commit), "button-press-event",
                   G_CALLBACK(_commit_profile_callback), (gpointer)self);
  gtk_widget_set_tooltip_text(g->button_commit,
                              _("accept the computed profile and set it in the module"));

  g->button_profile = dtgtk_button_new(dtgtk_cairo_paint_refresh, 0, NULL);
  dt_action_define_iop(self, N_("calibrate"), N_("recompute"),
                       g->button_profile, &dt_action_def_button);
  g_signal_connect(G_OBJECT(g->button_profile), "button-press-event",
                   G_CALLBACK(_run_profile_callback), (gpointer)self);
  gtk_widget_set_tooltip_text(g->button_profile, _("recompute the profile"));

  g->button_validate = dtgtk_button_new(dtgtk_cairo_paint_softproof, 0, NULL);
  dt_action_define_iop(self, N_("calibrate"), N_("validate"),
                       g->button_validate, &dt_action_def_button);
  g_signal_connect(G_OBJECT(g->button_validate), "button-press-event",
                   G_CALLBACK(_run_validation_callback), (gpointer)self);
  gtk_widget_set_tooltip_text(g->button_validate, _("check the output delta E"));

  dt_gui_box_add(g->cs.container, g->checkers_list, g->optimize, g->safety,
                 g->label_delta_E, dt_gui_hbox(dt_gui_align_right(g->button_validate),
                 g->button_profile, g->button_commit));
}

void gui_cleanup(dt_iop_module_t *self)
{
  if(self && self->dev)
  {
    dt_dev_chroma_t *chr = &self->dev->chroma;
    if(chr->adaptation == self)
      chr->adaptation = NULL;
  }

  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  dt_iop_channelmixer_rgb_gui_data_t *g = self->gui_data;
  dt_conf_set_int("plugins/darkroom/channelmixerrgb/gui_page",
                  gtk_notebook_get_current_page (g->notebook));

  if(g->delta_E_in)
  {
    dt_free_align(g->delta_E_in);
    g->delta_E_in = NULL;
  }

  g_free(g->delta_E_label_text);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
