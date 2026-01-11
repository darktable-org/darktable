/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/custom_primaries.h"
#include "common/image.h"
#include "common/iop_profile.h"
#include "common/math.h"
#include "common/matrices.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(7, dt_iop_agx_params_t)

const char *name()
{
  return _("AgX");
}

const char *aliases()
{
  return _("tone mapping|view transform|display transform");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("applies a tone mapping curve.\n"
                                    "inspired by Blender's AgX tone mapper"),
                                _("corrective and creative"),
                                _("linear, RGB, scene-referred"),
                                _("non-linear, RGB"),
                                _("linear, RGB, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_TECHNICAL;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static const float _epsilon = 1E-6f;
static const float _default_gamma = 2.2f;
static const int _red_index = 0;
static const int _green_index = 1;
static const int _blue_index = 2;

typedef enum dt_iop_agx_base_primaries_t
{
  DT_AGX_EXPORT_PROFILE = 0, // $DESCRIPTION: "export profile"
  DT_AGX_WORK_PROFILE = 1,   // $DESCRIPTION: "working profile"
  DT_AGX_REC2020 = 2,        // $DESCRIPTION: "Rec2020"
  DT_AGX_DISPLAY_P3 = 3,     // $DESCRIPTION: "Display P3"
  DT_AGX_ADOBE_RGB = 4,      // $DESCRIPTION: "Adobe RGB (compatible)"
  DT_AGX_SRGB = 5,           // $DESCRIPTION: "sRGB"
} dt_iop_agx_base_primaries_t;

// Params exposed on the UI
typedef struct dt_iop_agx_params_t
{
  float look_lift;                   // $MIN: -1.f $MAX: 1.f $DEFAULT: 0.f $DESCRIPTION: "lift"
  float look_slope;                  // $MIN: 0.f $MAX: 10.f $DEFAULT: 1.f $DESCRIPTION: "slope"
  float look_brightness;             // $MIN: 0.f $MAX: 100.f $DEFAULT: 1.f $DESCRIPTION: "brightness"
  float look_saturation;             // $MIN: 0.f $MAX: 10.f $DEFAULT: 1.f $DESCRIPTION: "saturation"
  float look_original_hue_mix_ratio; // $MIN: 0.f $MAX: 1.f $DEFAULT: 0.f $DESCRIPTION: "preserve hue"

  // log mapping
  float range_black_relative_ev;  // $MIN: -20.f $MAX: -0.1f  $DEFAULT: -10.f $DESCRIPTION: "black relative exposure"
  float range_white_relative_ev;  // $MIN: 0.1f $MAX: 20.f $DEFAULT: 6.5f $DESCRIPTION: "white relative exposure"
  float dynamic_range_scaling;    // $MIN: -0.5f $MAX: 2.f $DEFAULT: 0.1f $DESCRIPTION: "dynamic range scaling"

  // curve params - comments indicate the original variables from https://www.desmos.com/calculator/yrysofmx8h
  // Corresponds to p_x; is displayed as EV using slider offset and scale.
  // 0.606060606061f = 10/16.5, mid-gray's position if black relative exposure is -10 EV, white is +6.5 EV
  float curve_pivot_x;                    // $MIN: 0.f $MAX: 1.f $DEFAULT: 0.606060606061f $DESCRIPTION: "pivot relative exposure"
  // Corresponds to p_y, but not directly -- needs application of gamma
  float curve_pivot_y_linear_output;      // $MIN: 0.f $MAX: 1.f $DEFAULT: 0.18f $DESCRIPTION: "pivot target output"
  // P_slope
  float curve_contrast_around_pivot;      // $MIN: 0.1f $MAX: 10.f $DEFAULT: 2.8f $DESCRIPTION: "contrast"
  // related to P_tlength; the number expresses the portion of the y range below the pivot
  float curve_linear_ratio_below_pivot;   // $MIN: 0.f $MAX: 1.f $DEFAULT: 0.f $DESCRIPTION: "toe start"
  // related to P_slength; the number expresses the portion of the y range below the pivot
  float curve_linear_ratio_above_pivot;   // $MIN: 0.f $MAX: 1.f $DEFAULT: 0.f $DESCRIPTION: "shoulder start"
  // t_p
  float curve_toe_power;                  // $MIN: 0.f $MAX: 10.f $DEFAULT: 1.55f $DESCRIPTION: "toe power"
  // s_p
  float curve_shoulder_power;             // $MIN: 0.f $MAX: 10.f $DEFAULT: 1.55f $DESCRIPTION: "shoulder power"
  float curve_gamma;                      // $MIN: 0.01f $MAX: 100.f $DEFAULT: 2.2f $DESCRIPTION: "curve y gamma"
  gboolean auto_gamma;                    // $DEFAULT: FALSE $DESCRIPTION: "keep the pivot on the diagonal"
  // t_ly
  float curve_target_display_black_ratio; // $MIN: 0.f $MAX: 0.15f $DEFAULT: 0.f $DESCRIPTION: "target black"
  // s_ly
  float curve_target_display_white_ratio; // $MIN: 0.2f $MAX: 1.f $DEFAULT: 1.f $DESCRIPTION: "target white"

  // custom primaries; rotation limits below: +/- 0.5236 radian => +/- 30 degrees
  dt_iop_agx_base_primaries_t base_primaries; // $DEFAULT: DT_AGX_REC2020 $DESCRIPTION: "base primaries"
  gboolean disable_primaries_adjustments; // $DEFAULT: FALSE $DESCRIPTION: "disable adjustments"
  float red_inset;        // $MIN:  0.f  $MAX: 0.99f $DEFAULT: 0.f $DESCRIPTION: "red attenuation"
  float red_rotation;     // $MIN: -0.5236f $MAX: 0.5236f  $DEFAULT: 0.f $DESCRIPTION: "red rotation"
  float green_inset;      // $MIN:  0.f  $MAX: 0.99f $DEFAULT: 0.f $DESCRIPTION: "green attenuation"
  float green_rotation;   // $MIN: -0.5236f $MAX: 0.5236f  $DEFAULT: 0.f $DESCRIPTION: "green rotation"
  float blue_inset;       // $MIN:  0.f  $MAX: 0.99f $DEFAULT: 0.f $DESCRIPTION: "blue attenuation"
  float blue_rotation;    // $MIN: -0.5236f $MAX: 0.5236f  $DEFAULT: 0.f $DESCRIPTION: "blue rotation"

  float master_outset_ratio;     // $MIN:  0.f  $MAX: 2.f $DEFAULT: 1.f $DESCRIPTION: "master purity boost"
  float master_unrotation_ratio; // $MIN:  0.f  $MAX: 2.f $DEFAULT: 1.f $DESCRIPTION: "master rotation reversal"
  float red_outset;              // $MIN:  0.f  $MAX: 0.99f $DEFAULT: 0.f $DESCRIPTION: "red purity boost"
  float red_unrotation;          // $MIN: -0.5236f $MAX: 0.5236f  $DEFAULT: 0.f $DESCRIPTION: "red reverse rotation"
  float green_outset;            // $MIN:  0.f  $MAX: 0.99f $DEFAULT: 0.f $DESCRIPTION: "green purity boost"
  float green_unrotation;        // $MIN: -0.5236f $MAX: 0.5236f  $DEFAULT: 0.f $DESCRIPTION: "green reverse rotation"
  float blue_outset;             // $MIN:  0.f  $MAX: 0.99f $DEFAULT: 0.f $DESCRIPTION: "blue purity boost"
  float blue_unrotation;         // $MIN: -0.5236f $MAX: 0.5236f  $DEFAULT: 0.f $DESCRIPTION: "blue reverse rotation"

  // v5
  gboolean completely_reverse_primaries; // $DEFAULT: FALSE $DESCRIPTION: "reverse all"
} dt_iop_agx_params_t;

typedef struct dt_iop_basic_curve_controls_t
{
  GtkWidget *curve_pivot_x;
  GtkWidget *curve_pivot_y_linear;
  GtkWidget *curve_contrast_around_pivot;
  GtkWidget *curve_toe_power;
  GtkWidget *curve_shoulder_power;
} dt_iop_basic_curve_controls_t;

typedef struct dt_iop_agx_gui_data_t
{
  GtkNotebook *notebook;
  GtkWidget *auto_gamma;
  GtkWidget *curve_gamma;
  GtkDrawingArea *graph_drawing_area;

  dt_gui_collapsible_section_t look_section;
  dt_gui_collapsible_section_t graph_section;
  dt_gui_collapsible_section_t advanced_section;

  GtkWidget *curve_basic_controls_box;
  GtkWidget *curve_graph_box;
  GtkWidget *curve_advanced_controls_box;

  // Exposure pickers and their sliders
  GtkWidget *range_exposure_picker;
  GtkWidget *black_exposure_picker;
  GtkWidget *white_exposure_picker;
  GtkWidget *security_factor;
  GtkWidget *range_exposure_picker_group;
  GtkWidget *btn_read_exposure;

  // basic curve controls for 'settings' and 'curve' page (if enabled)
  dt_iop_basic_curve_controls_t basic_curve_controls;

  // curve graph/plot
  GtkAllocation allocation;
  PangoRectangle ink;
  GtkStyleContext *context;

  GtkWidget *disable_primaries_adjustments;
  GtkWidget *primaries_controls_vbox;
  GtkWidget *completely_reverse_primaries;
  GtkWidget *post_curve_primaries_controls_vbox;
  GtkWidget *set_post_curve_primaries_from_pre_button;
} dt_iop_agx_gui_data_t;

typedef struct tone_mapping_params_t
{
  float black_relative_ev;
  float white_relative_ev;
  float range_in_ev;
  float curve_gamma;

  // the toe runs from (t_lx = 0, target black) to (toe_transition_x,
  // toe_transition_y)
  float pivot_x;
  float pivot_y;
  float target_black;     // t_ly
  float toe_power;        // t_p
  float toe_transition_x; // t_tx
  float toe_transition_y; // t_ty
  float toe_scale;        // t_s
  gboolean need_convex_toe;
  float toe_fallback_coefficient;
  float toe_fallback_power;

  // the linear section lies on y = mx + b, running from
  // (toe_transition_x, toe_transition_y) to (shoulder_transition_x,
  // shoulder_transition_y) it can have length 0, in which case it
  // only contains the pivot (pivot_x, pivot_y) the pivot may coincide
  // with toe_transition or shoulder_start or both
  float slope;     // m - for the linear section
  float intercept; // b parameter of the straight segment (y = mx + b,
                   // intersection with the y-axis at (0, b))

  // the shoulder runs from (shoulder_transition_x,
  // shoulder_transition_y) to (s_lx = 1, target_white)
  float target_white;          // s_ly
  float shoulder_power;        // s_p
  float shoulder_transition_x; // s_tx
  float shoulder_transition_y; // s_ty
  float shoulder_scale;        // s_s
  gboolean need_concave_shoulder;
  float shoulder_fallback_coefficient;
  float shoulder_fallback_power;

  // look
  float look_lift;
  float look_slope;
  float look_power;
  float look_saturation;
  float look_original_hue_mix_ratio;
  gboolean look_tuned;
  gboolean restore_hue;
} tone_mapping_params_t;

typedef struct primaries_params_t
{
  dt_iop_agx_base_primaries_t base_primaries;

  float inset[3];
  float rotation[3];

  float master_outset_ratio;
  float master_unrotation_ratio;

  float outset[3];
  float unrotation[3];
} primaries_params_t;

typedef struct dt_iop_agx_data_t
{
  tone_mapping_params_t tone_mapping_params;
  primaries_params_t primaries_params;
} dt_iop_agx_data_t;

static void _set_scene_referred_default_params(dt_iop_agx_params_t *p);

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  if(old_version < 7)
  {
    // SPECIAL CASE: all versions before 7 were unreleased test versions,
    // and migration is not relevant anymore; they will always be migrated
    // to the CURRENT LATEST version, without further gradual migration steps
    dt_iop_agx_params_t *np = calloc(1, sizeof(dt_iop_agx_params_t));
    _set_scene_referred_default_params(np);
    *new_params = np;
    *new_params_size = sizeof(dt_iop_agx_params_t);
    *new_version = self->so->version(); // SPECIAL CASE: jump directly to latest version

    return 0;
  }

  return 1;
}

static inline dt_colorspaces_color_profile_type_t
_get_base_profile_type_from_enum(const dt_iop_agx_base_primaries_t base_primaries_enum)
{
  switch(base_primaries_enum)
  {
    case DT_AGX_SRGB:
      return DT_COLORSPACE_SRGB;
    case DT_AGX_DISPLAY_P3:
      return DT_COLORSPACE_DISPLAY_P3;
    case DT_AGX_ADOBE_RGB:
      return DT_COLORSPACE_ADOBERGB;
    case DT_AGX_REC2020: // Fall through
    default:
      return DT_COLORSPACE_LIN_REC2020; // Default/fallback
  }
}

static void _set_blenderlike_primaries(dt_iop_agx_params_t *p)
{
  p->disable_primaries_adjustments = FALSE;
  p->completely_reverse_primaries = FALSE;
  p->base_primaries = DT_AGX_REC2020;

  // AgX primaries settings that produce the same matrices under D50
  // as those used in the Blender OCIO config.
  // https://discuss.pixls.us/t/blender-agx-in-darktable-proof-of-concept/48697/1018
  // https://github.com/EaryChow/AgX_LUT_Gen/blob/main/AgXBaseRec2020.py
  p->red_inset = 0.29462451f;
  p->green_inset = 0.25861925f;
  p->blue_inset = 0.14641371f;
  p->red_rotation = 0.03540329f;
  p->green_rotation = -0.02108586f;
  p->blue_rotation = -0.06305724f;

  p->master_outset_ratio = 1.f;
  // Blender doesn't reverse rotations; we set up an exact unrotation below,
  // but let the user turn it on gradually
  p->master_unrotation_ratio = 0.f;

  p->red_outset = 0.290776401758f;
  p->green_outset = 0.263155400753f;
  p->blue_outset = 0.045810721815f;
  p->red_unrotation = p->red_rotation;
  p->green_unrotation = p->green_rotation;
  p->blue_unrotation = p->blue_rotation;
}

static void _set_unmodified_primaries(dt_iop_agx_params_t *p)
{
  p->disable_primaries_adjustments = FALSE;
  p->completely_reverse_primaries = FALSE;
  p->base_primaries = DT_AGX_REC2020;

  p->red_inset = 0.f;
  p->red_rotation = 0.f;
  p->green_inset = 0.f;
  p->green_rotation = 0.f;
  p->blue_inset = 0.f;
  p->blue_rotation = 0.f;

  p->master_outset_ratio = 1.f;
  p->master_unrotation_ratio = 1.f;

  p->red_outset = 0.f;
  p->red_unrotation = 0.f;
  p->green_outset = 0.f;
  p->green_unrotation = 0.f;
  p->blue_outset = 0.f;
  p->blue_unrotation = 0.f;
}

static void _set_smooth_primaries(dt_iop_agx_params_t *p)
{
  p->disable_primaries_adjustments = FALSE;
  p->completely_reverse_primaries = FALSE;

  // Sigmoid 'smooth' primaries settings
  p->base_primaries = DT_AGX_WORK_PROFILE;

  p->red_inset = 0.1f;
  p->green_inset = 0.1f;
  p->blue_inset = 0.15f;
  p->red_rotation = deg2radf(2.f);
  p->green_rotation = deg2radf(-1.f);
  p->blue_rotation = deg2radf(-3.f);

  // sigmoid: "Don't restore purity - try to avoid posterization."
  p->master_outset_ratio = 0.f;
  // but allow the user to do so simply by dragging the master control
  p->red_outset = p->red_inset;
  p->green_outset = p->green_inset;
  p->blue_outset = p->blue_inset;

  // sigmoid always reverses rotations
  p->master_unrotation_ratio = 1.f;
  p->red_unrotation = p->red_rotation;
  p->green_unrotation = p->green_rotation;
  p->blue_unrotation = p->blue_rotation;
}

// user-selected base profile
static const dt_iop_order_iccprofile_info_t *
_agx_get_base_profile(dt_develop_t *dev,
                      const dt_iop_order_iccprofile_info_t *
                      pipe_work_profile,
                      const dt_iop_agx_base_primaries_t
                      base_primaries_selection)
{
  dt_iop_order_iccprofile_info_t *selected_profile_info = NULL;

  switch(base_primaries_selection)
  {
    case DT_AGX_EXPORT_PROFILE:
    {
      dt_colorspaces_color_profile_type_t profile_type;
      const char *profile_filename;

      dt_ioppr_get_export_profile_type(dev, &profile_type, &profile_filename);

      if(profile_type != DT_COLORSPACE_NONE && profile_filename != NULL)
      {
        // intent does not matter, we just need the primaries
        selected_profile_info =
            dt_ioppr_add_profile_info_to_list(dev, profile_type, profile_filename, INTENT_PERCEPTUAL);
        if(!selected_profile_info
           || !dt_is_valid_colormatrix(selected_profile_info->matrix_in_transposed[0][0]))
        {
          dt_print(DT_DEBUG_PIPE,
                   "[agx] Export profile '%s' unusable or missing matrix, falling back to Rec2020.",
                   dt_colorspaces_get_name(profile_type, profile_filename));
          selected_profile_info = NULL; // Force fallback
        }
      }
      else
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[agx] Failed to get configured export profile settings, falling back to Rec2020.");
        // fallback handled below
      }
    }
    break;

    case DT_AGX_WORK_PROFILE:
      return pipe_work_profile;

    case DT_AGX_REC2020:
    case DT_AGX_DISPLAY_P3:
    case DT_AGX_ADOBE_RGB:
    case DT_AGX_SRGB:
    {
      const dt_colorspaces_color_profile_type_t profile_type =
          _get_base_profile_type_from_enum(base_primaries_selection);
      // Use relative intent for standard profiles when used as base
      selected_profile_info =
          dt_ioppr_add_profile_info_to_list(dev, profile_type, "", DT_INTENT_RELATIVE_COLORIMETRIC);
      if(!selected_profile_info
         || !dt_is_valid_colormatrix(selected_profile_info->matrix_in_transposed[0][0]))
      {
        dt_print(DT_DEBUG_PIPE,
                 "[agx] Standard base profile '%s' unusable or missing matrix, falling back to Rec2020.",
                 dt_colorspaces_get_name(profile_type, ""));
        selected_profile_info = NULL; // Force fallback
      }
    }
    break;
  }

  // fallback: selected profile not found or invalid
  if(!selected_profile_info)
  {
    selected_profile_info = dt_ioppr_add_profile_info_to_list(dev,
                                                              DT_COLORSPACE_LIN_REC2020,
                                                              "",
                                                              DT_INTENT_RELATIVE_COLORIMETRIC);

    // if even Rec2020 fails, something is very wrong, but let the caller handle NULL if necessary.
    if(!selected_profile_info)
      dt_print(DT_DEBUG_ALWAYS, "[agx] CRITICAL: Failed to get even Rec2020 base profile info.");
  }

  return selected_profile_info;
}

static inline float _luminance_from_matrix(const dt_aligned_pixel_t pixel,
                                           const dt_colormatrix_t rgb_to_xyz_transposed)
{
  dt_aligned_pixel_t xyz = { 0.f };
  dt_apply_transposed_color_matrix(pixel, rgb_to_xyz_transposed, xyz);
  return xyz[1];
}

static inline float _luminance_from_profile(dt_aligned_pixel_t pixel,
                                            const dt_iop_order_iccprofile_info_t *const profile)
{
  return dt_ioppr_get_rgb_matrix_luminance(pixel, profile->matrix_in,
                                           profile->lut_in,
                                           profile->unbounded_coeffs_in,
                                           profile->lutsize,
                                           profile->nonlinearlut);
}


static inline float _line(const float x,
                          const float slope,
                          const float intercept)
{
  return slope * x + intercept;
}

/*
 * s_t, s_t at https://www.desmos.com/calculator/yrysofmx8h
 * The maths has been rewritten for symmetry, but is equivalent.
 * Original:
 * projected_rise = slope * (limit_x - transition_x)
 * projected_rise_to_power = powf(projected_rise, -power)
 * actual_rise = limit_y - transition_y
 * linear_overshoot_ratio = projected_rise / actual_rise
 * scale_adjustment_factor = powf(linear_overshoot_ratio, power) - 1
 * base = projected_rise_to_power * scale_adjustment_factor
 * scale_value = powf(base, -1 / power)
 *
 * We can substitute scale_adjustment_factor into base:
 * base = projected_rise_to_power * (powf(linear_overshoot_ratio, power) - 1)
 * Then substitute projected_rise_to_power and linear_overshoot_ratio:
 * base = powf(projected_rise, -power) * (powf(projected_rise / actual_rise, power) - 1)
 * Expand the brackets:
 * base = powf(projected_rise, -power) * powf(projected_rise / actual_rise, power) - powf(projected_rise, -power)
 * base = powf(projected_rise, -power) * powf(projected_rise, power) / powf(actual_rise, power) -
 * powf(projected_rise, -power) base = 1 / powf(actual_rise, power) - powf(projected_rise, -power) base =
 * powf(actual_rise, -power) - powf(projected_rise, -power)
 */
 static inline float _scale(const float limit_x,
                            const float limit_y,
                            const float transition_x,
                            const float transition_y,
                            const float slope,
                            const float power)
{
  // the hypothetical 'rise' if the linear section were extended to the limit.
  const float projected_rise = slope * fmaxf(_epsilon, limit_x - transition_x);

  // the actual 'rise' the curve needs to cover.
  const float actual_rise = fmaxf(_epsilon, limit_y - transition_y);

  const float transformed_projected_rise = powf(projected_rise, -power);
  const float transformed_actual_rise = powf(actual_rise, -power);

  const float base = fmaxf(_epsilon, transformed_actual_rise - transformed_projected_rise);

  const float scale_value = powf(base, -1.f / power);

  // avoid 'explosions'
  return fminf(1e9f, scale_value);
}

// f_t(x), f_s(x) at https://www.desmos.com/calculator/yrysofmx8h
static inline float _sigmoid(const float x,
                             const float power)
{
  return x / powf(1.f + powf(x, power), 1.f / power);
}

// f_ss, f_ts at https://www.desmos.com/calculator/yrysofmx8h
static inline float _scaled_sigmoid(const float x,
                                    const float scale,
                                    const float slope,
                                    const float power,
                                    const float transition_x,
                                    const float transition_y)
{
  return scale * _sigmoid(slope * (x - transition_x) / scale, power) + transition_y;
}

// Fallback toe/shoulder, so we can always reach black and white.
// See https://www.desmos.com/calculator/gijzff3wlv
static inline float _fallback_toe(const float x,
                                  const tone_mapping_params_t *params)
{
  return x < 0.f
           ? params->target_black
           : params->target_black
             + fmaxf(0.f, params->toe_fallback_coefficient * powf(x, params->toe_fallback_power));
}

static inline float _fallback_shoulder(const float x,
                                       const tone_mapping_params_t *params)
{
  return x >= 1.f
           ? params->target_white
           : params->target_white
             - fmaxf(0.f, params->shoulder_fallback_coefficient
                          * powf(1.f - x, params->shoulder_fallback_power));
}

static inline float _apply_curve(const float x,
                                 const tone_mapping_params_t *params)
{
  float result = 0.f;

  if(x < params->toe_transition_x)
  {
    result = params->need_convex_toe
               ? _fallback_toe(x, params)
               : _scaled_sigmoid(x, params->toe_scale, params->slope, params->toe_power,
                                 params->toe_transition_x, params->toe_transition_y);
  }
  else if(x <= params->shoulder_transition_x)
  {
    result = _line(x, params->slope, params->intercept);
  }
  else
  {
    result = params->need_concave_shoulder
               ? _fallback_shoulder(x, params)
               : _scaled_sigmoid(x, params->shoulder_scale, params->slope, params->shoulder_power,
                                 params->shoulder_transition_x, params->shoulder_transition_y);
  }
  return CLAMPF(result, params->target_black, params->target_white);
}

// 'lerp', but take care of the boundary: hue wraps around 1->0
static inline float _lerp_hue(const float original_hue,
                              const float processed_hue,
                              const float mix)
{
  // shortest signed difference in [-0.5, 0.5] there is some ambiguity
  // (shortest distance on a circle is undefined if the points are
  // exactly on the opposite side), but the original and processed hue
  // are quite similar, we don't expect 180-degree shifts, and
  // couldn't do anything about it, anyway
  const float shortest_distance_on_hue_circle = remainderf(processed_hue - original_hue, 1.0f);

  // interpolate: mix = 0 -> processed_hue; mix = 1 -> original_hue
  // multiply-add: (1 - mix) * shortest_distance_on_hue_circle + original_hue
  const float mixed_hue = DT_FMA(1.0f - mix, shortest_distance_on_hue_circle, original_hue);

  // wrap to [0, 1)
  return mixed_hue - floorf(mixed_hue);
}

static inline float _apply_slope_lift(const float x,
                                        const float slope,
                                        const float lift)
{
  // https://www.desmos.com/calculator/8a26bc7eb8
  const float m = slope / (1.f + lift);
  const float b = lift * m;
  // m * x + b
  return DT_FMA(m, x, b);
}

DT_OMP_DECLARE_SIMD(aligned(pixel_in_out : 16))
static inline void _agx_look(dt_aligned_pixel_t pixel_in_out,
                             const tone_mapping_params_t *params,
                             const dt_colormatrix_t rendering_to_xyz_transposed)
{
  const float slope = params->look_slope;
  const float lift = params->look_lift;
  const float power = params->look_power;
  const float sat = params->look_saturation;

  for_three_channels(k, aligned(pixel_in_out : 16))
  {
    const float value_with_slope_and_lift = _apply_slope_lift(pixel_in_out[k], slope, lift);
    pixel_in_out[k] =
      value_with_slope_and_lift > 0.f
      ? powf(value_with_slope_and_lift, power)
      : value_with_slope_and_lift;
  }

  const float luma = _luminance_from_matrix(pixel_in_out, rendering_to_xyz_transposed);

  // saturation
  for_three_channels(k, aligned(pixel_in_out : 16))
  {
    pixel_in_out[k] = luma + sat * (pixel_in_out[k] - luma);
  }
}

static inline float _apply_log_encoding(const float x,
                                        const float range_in_ev,
                                        const float black_relative_ev)
{
  // Assume input is linear RGB relative to 0.18 mid-gray
  // Ensure value > 0 before log
  const float x_relative = fmaxf(_epsilon, x / 0.18f);
  // normalise to [0, 1] based on black_relative_ev and range_in_ev
  const float mapped = (log2f(fmaxf(x_relative, 0.f)) - black_relative_ev) / range_in_ev;
  // Clamp result to [0, 1] - this is the input domain for the curve
  return CLIP(mapped);
}

// see https://www.desmos.com/calculator/gijzff3wlv
static inline float _calculate_slope_matching_power(const float slope,
                                                    const float dx_transition_to_limit,
                                                    const float dy_transition_to_limit)
{
  return slope * dx_transition_to_limit / dy_transition_to_limit;
}

static inline float _calculate_fallback_curve_coefficient(const float dx_transition_to_limit,
                                                          const float dy_transition_to_limit,
                                                          const float exponent)
{
  return dy_transition_to_limit / powf(dx_transition_to_limit, exponent);
}

static inline void _compress_into_gamut(dt_aligned_pixel_t pixel_in_out)
{
  // Blender: https://github.com/EaryChow/AgX_LUT_Gen/blob/main/luminance_compenstation_bt2020.py
  // Calculate original luminance
  const float luminance_coeffs[] = { 0.2658180370250449f, 0.59846986045365f, 0.1357121025213052f };

  const float input_y = pixel_in_out[0] * luminance_coeffs[0]
                      + pixel_in_out[1] * luminance_coeffs[1]
                      + pixel_in_out[2] * luminance_coeffs[2];
  const float max_rgb = max3f(pixel_in_out);

  // Calculate luminance of the opponent color, and use it to
  // compensate for negative luminance values
  dt_aligned_pixel_t opponent_rgb = { 0.f };
  for_each_channel(c, aligned(opponent_rgb, pixel_in_out))
  {
    opponent_rgb[c] = max_rgb - pixel_in_out[c];
  }

  const float opponent_y = opponent_rgb[0] * luminance_coeffs[0]
                         + opponent_rgb[1] * luminance_coeffs[1]
                         + opponent_rgb[2] * luminance_coeffs[2];
  const float max_opponent = max3f(opponent_rgb);

  const float y_compensate_negative = max_opponent - opponent_y + input_y;

  // Offset the input tristimulus such that there are no negatives
  const float min_rgb = min3f(pixel_in_out);
  const float offset = fmaxf(-min_rgb, 0.f);
  dt_aligned_pixel_t rgb_offset = { 0.f };
  for_each_channel(c, aligned(rgb_offset, pixel_in_out))
  {
    rgb_offset[c] = pixel_in_out[c] + offset;
  }

  const float max_of_rgb_offset = max3f(rgb_offset);

  // Calculate luminance of the opponent color, and use it to
  // compensate for negative luminance values
  dt_aligned_pixel_t opponent_rgb_offset = { 0.f };
  for_each_channel(c, aligned(opponent_rgb_offset, rgb_offset))
  {
    opponent_rgb_offset[c] = max_of_rgb_offset - rgb_offset[c];
  }

  const float max_inverse_rgb_offset = max3f(opponent_rgb_offset);
  const float y_inverse_rgb_offset = opponent_rgb_offset[0] * luminance_coeffs[0]
                                   + opponent_rgb_offset[1] * luminance_coeffs[1]
                                   + opponent_rgb_offset[2] * luminance_coeffs[2];
  float y_new = rgb_offset[0] * luminance_coeffs[0]
              + rgb_offset[1] * luminance_coeffs[1]
              + rgb_offset[2] * luminance_coeffs[2];
  y_new = max_inverse_rgb_offset - y_inverse_rgb_offset + y_new;

  // Compensate the intensity to match the original luminance; avoid div by 0 or tiny number
  const float luminance_ratio =
    (y_new > y_compensate_negative && y_new > _epsilon)
    ? y_compensate_negative / y_new
    : 1.f;

  for_each_channel(c, aligned(pixel_in_out, rgb_offset))
  {
    pixel_in_out[c] = luminance_ratio * rgb_offset[c];
  }
}

static inline float _calculate_pivot_y_at_gamma(const dt_iop_agx_params_t * p,
                                                const float gamma)
{
  return powf(CLAMPF(p->curve_pivot_y_linear_output,
                     p->curve_target_display_black_ratio,
                     p->curve_target_display_white_ratio),
              1.f / gamma);
}

static void _adjust_pivot(const dt_iop_agx_params_t *p,
                          tone_mapping_params_t *tone_mapping_params)
{
  // don't allow pivot_x to touch the endpoints
  tone_mapping_params->pivot_x = CLAMPF(p->curve_pivot_x, _epsilon, 1.f - _epsilon);

  if(p->auto_gamma)
  {
    tone_mapping_params->curve_gamma =
      tone_mapping_params->pivot_x > 0.f && p->curve_pivot_y_linear_output > 0.f
      ? log2f(p->curve_pivot_y_linear_output) / log2f(tone_mapping_params->pivot_x)
      : p->curve_gamma;
  }
  else
  {
    tone_mapping_params->curve_gamma = p->curve_gamma;
  }

  tone_mapping_params->pivot_y = _calculate_pivot_y_at_gamma(p, tone_mapping_params->curve_gamma);
}

static void _set_log_mapping_params(const dt_iop_agx_params_t *p,
                                    tone_mapping_params_t *curve_and_look_params)
{
  curve_and_look_params->white_relative_ev = p->range_white_relative_ev;
  curve_and_look_params->black_relative_ev = p->range_black_relative_ev;
  curve_and_look_params->range_in_ev = curve_and_look_params->white_relative_ev - curve_and_look_params->black_relative_ev;
}

static inline float _calculate_slope_gamma_compensation(const float gamma,
                                                        const float pivot_y,
                                                        const dt_iop_agx_params_t *p)
{
  // compensate contrast relative to gamma 2.2 to keep contrast around the pivot constant

  const float pivot_y_at_default_gamma = _calculate_pivot_y_at_gamma(p, _default_gamma);

  // We want to maintain the contrast after linearisation, so we need to apply
  // the chain rule (f(g(x)' = f'(g(x)) * g'(x))
  // to find the derivative of linearisation(curve(x)) = curve(x)^gamma.
  // By definition, the derivative of the curve g'(pivot_x)) = the slope;
  // also, curve(pivot_x) = pivot_y, so we need the derivative of the
  // power function at that point: f'(pivot_y).
  // We want to find gamma_compensated_slope to keep the overall derivative constant:
  // gamma_compensated_slope * [gamma * pivot_y_at_current_gamma^(current_gamma-1)] =
  // range_adjusted_slope * [_default_gamma * pivot_y_at_default_gamma^(_default_gamma-1)],
  // and thus gamma_compensated_slope = range_adjusted_slope *
  //              [_default_gamma * pivot_y^(_default_gamma-1)] / [gamma * pivot_y^(current_gamma-1)]

  const float derivative_at_current_gamma = gamma * powf(fmaxf(_epsilon, pivot_y), gamma - 1.0f);
  const float derivative_at_default_gamma =
    _default_gamma * powf(fmaxf(_epsilon, pivot_y_at_default_gamma), _default_gamma - 1.0f);
  return derivative_at_current_gamma / derivative_at_default_gamma;
}

static tone_mapping_params_t _calculate_tone_mapping_params(const dt_iop_agx_params_t *p)
{
  tone_mapping_params_t tone_mapping_params;

  // look
  tone_mapping_params.look_lift = p->look_lift;
  tone_mapping_params.look_slope = p->look_slope;
  tone_mapping_params.look_saturation = p->look_saturation;
  const float brightness = p->look_brightness;
  tone_mapping_params.look_power = brightness < 1 ? 1.f / sqrtf(fmaxf(brightness, _epsilon)) : 1.f / brightness;
  tone_mapping_params.look_original_hue_mix_ratio = p->look_original_hue_mix_ratio;
  tone_mapping_params.look_tuned = p -> look_slope != 1.f
                                   || p->look_brightness != 1.f
                                   || p -> look_lift != 0.f
                                   || p -> look_saturation != 1.f;
  tone_mapping_params.restore_hue = p->look_original_hue_mix_ratio != 0.f;

  // log mapping
  _set_log_mapping_params(p, &tone_mapping_params);

  _adjust_pivot(p, &tone_mapping_params);

  // avoid range altering slope - 16.5 EV is the default AgX range; keep the meaning of slope
  const float range_adjusted_slope =
    p->curve_contrast_around_pivot * (tone_mapping_params.range_in_ev / 16.5f);

  const float compensation_factor = _calculate_slope_gamma_compensation(tone_mapping_params.curve_gamma, tone_mapping_params.pivot_y, p);

  tone_mapping_params.slope = range_adjusted_slope / compensation_factor;

  // toe
  tone_mapping_params.target_black =
    powf(p->curve_target_display_black_ratio, 1.f / tone_mapping_params.curve_gamma);
  tone_mapping_params.toe_power = fmaxf(0.01f, p->curve_toe_power);

  const float remaining_y_below_pivot = tone_mapping_params.pivot_y - tone_mapping_params.target_black;
  const float toe_length_y = remaining_y_below_pivot * p->curve_linear_ratio_below_pivot;
  float dx_linear_below_pivot = toe_length_y / tone_mapping_params.slope;
  // ...and subtract it from pivot_x to get the x coordinate where the linear section joins the toe
  // ... but keep the transition point above x = 0
  tone_mapping_params.toe_transition_x = fmaxf(_epsilon, tone_mapping_params.pivot_x - dx_linear_below_pivot);
  // fix up in case the limitation kicked in
  dx_linear_below_pivot = tone_mapping_params.pivot_x - tone_mapping_params.toe_transition_x;

  // from the 'run' pivot_x->toe_transition_x, we calculate the 'rise'
  const float toe_dy_below_pivot = tone_mapping_params.slope * dx_linear_below_pivot;
  tone_mapping_params.toe_transition_y = tone_mapping_params.pivot_y - toe_dy_below_pivot;

  // we use the same calculation as for the shoulder, so we flip the toe left <-> right, up <-> down
  const float inverse_toe_limit_x = 1.f; // 1 - toe_limix_x (toe_limix_x = 0, so inverse = 1)
  const float inverse_toe_limit_y = 1.f - tone_mapping_params.target_black; // Inverse limit y

  const float inverse_toe_transition_x = 1.f - tone_mapping_params.toe_transition_x;
  const float inverse_toe_transition_y = 1.f - tone_mapping_params.toe_transition_y;

  // and then flip the scale
  tone_mapping_params.toe_scale = -_scale(inverse_toe_limit_x, inverse_toe_limit_y,
                                          inverse_toe_transition_x, inverse_toe_transition_y,
                                          tone_mapping_params.slope, tone_mapping_params.toe_power);

  // limit_x is 0, so toe length =  -> toe_transition_x - limit_x is just toe_transition_x
  // the value is already limited to be >= epsilon, so safe to use in division
  const float toe_length_x = tone_mapping_params.toe_transition_x;
  const float toe_dy_transition_to_limit =
      fmaxf(_epsilon, tone_mapping_params.toe_transition_y - tone_mapping_params.target_black);
  const float toe_slope_transition_to_limit = toe_dy_transition_to_limit / toe_length_x;
  tone_mapping_params.need_convex_toe = toe_slope_transition_to_limit > tone_mapping_params.slope;

  // toe fallback curve params
  tone_mapping_params.toe_fallback_power = _calculate_slope_matching_power
    (tone_mapping_params.slope, toe_length_x, toe_dy_transition_to_limit);
  tone_mapping_params.toe_fallback_coefficient = _calculate_fallback_curve_coefficient
    (toe_length_x, toe_dy_transition_to_limit, tone_mapping_params.toe_fallback_power);

  // if x went from toe_transition_x to 0, at the given slope,
  // starting from toe_transition_y, where would we intersect the
  // y-axis?
  tone_mapping_params.intercept =
    tone_mapping_params.toe_transition_y
    - (tone_mapping_params.slope * tone_mapping_params.toe_transition_x);

  // shoulder
  tone_mapping_params.target_white =
    powf(p->curve_target_display_white_ratio, 1.f / tone_mapping_params.curve_gamma);
  const float remaining_y_above_pivot = tone_mapping_params.target_white - tone_mapping_params.pivot_y;
  const float shoulder_length_y = remaining_y_above_pivot * p->curve_linear_ratio_above_pivot;
  float dx_linear_above_pivot = shoulder_length_y / tone_mapping_params.slope;

  // don't allow shoulder_transition_x to reach 1
  tone_mapping_params.shoulder_transition_x =
    fminf(1.f - _epsilon, tone_mapping_params.pivot_x + dx_linear_above_pivot);
  dx_linear_above_pivot = tone_mapping_params.shoulder_transition_x - tone_mapping_params.pivot_x;

  const float shoulder_dy_above_pivot = tone_mapping_params.slope * dx_linear_above_pivot;
  tone_mapping_params.shoulder_transition_y = tone_mapping_params.pivot_y + shoulder_dy_above_pivot;
  tone_mapping_params.shoulder_power = fmaxf(0.01f, p->curve_shoulder_power);

  const float shoulder_limit_x = 1.f;
  tone_mapping_params.shoulder_scale = _scale(shoulder_limit_x,
                                              tone_mapping_params.target_white,
                                              tone_mapping_params.shoulder_transition_x,
                                              tone_mapping_params.shoulder_transition_y,
                                              tone_mapping_params.slope,
                                              tone_mapping_params.shoulder_power);

  // shoulder_transition_x < 1, guaranteed above
  const float shoulder_length_x = 1.f - tone_mapping_params.shoulder_transition_x;
  const float shoulder_dy_transition_to_limit =
    fmaxf(_epsilon, tone_mapping_params.target_white - tone_mapping_params.shoulder_transition_y);
  const float shoulder_slope_transition_to_limit =
    shoulder_dy_transition_to_limit / shoulder_length_x;
  tone_mapping_params.need_concave_shoulder =
    shoulder_slope_transition_to_limit > tone_mapping_params.slope;

  // shoulder fallback curve params
  tone_mapping_params.shoulder_fallback_power = _calculate_slope_matching_power
    (tone_mapping_params.slope, shoulder_length_x, shoulder_dy_transition_to_limit);
  tone_mapping_params.shoulder_fallback_coefficient = _calculate_fallback_curve_coefficient
    (shoulder_length_x, shoulder_dy_transition_to_limit, tone_mapping_params.shoulder_fallback_power);

  return tone_mapping_params;
}

static primaries_params_t _get_primaries_params(const dt_iop_agx_params_t *p)
{
  primaries_params_t primaries_params;

  primaries_params.base_primaries = p->base_primaries;

  primaries_params.inset[0] = p->red_inset;
  primaries_params.inset[1] = p->green_inset;
  primaries_params.inset[2] = p->blue_inset;
  primaries_params.rotation[0] = p->red_rotation;
  primaries_params.rotation[1] = p->green_rotation;
  primaries_params.rotation[2] = p->blue_rotation;
  primaries_params.master_outset_ratio = p->master_outset_ratio;
  primaries_params.master_unrotation_ratio = p->master_unrotation_ratio;

  if(p->disable_primaries_adjustments)
  {
    for(int i = 0; i < 3; i++)
      primaries_params.inset[i] =
        primaries_params.rotation[i] = primaries_params.outset[i] = primaries_params.unrotation[i] = 0.f;
  }
  else if(p->completely_reverse_primaries)
  {
    for(int i = 0; i < 3; i++)
    {
      primaries_params.outset[i] = primaries_params.inset[i];
      primaries_params.unrotation[i] = primaries_params.rotation[i];
      primaries_params.master_outset_ratio = 1.f;
      primaries_params.master_unrotation_ratio = 1.f;
    }
  }
  else
  {
    primaries_params.outset[0] = p->red_outset;
    primaries_params.outset[1] = p->green_outset;
    primaries_params.outset[2] = p->blue_outset;
    primaries_params.unrotation[0] = p->red_unrotation;
    primaries_params.unrotation[1] = p->green_unrotation;
    primaries_params.unrotation[2] = p->blue_unrotation;
  }

  return primaries_params;
}

static void _update_pivot_slider_settings(GtkWidget* const slider,
                                         const dt_iop_agx_params_t* const p)
{
  darktable.gui->reset++;

  const float range = p->range_white_relative_ev - p->range_black_relative_ev;

  dt_bauhaus_slider_set_factor(slider, range);
  dt_bauhaus_slider_set_offset(slider, p->range_black_relative_ev);
  // 0 EV default with the new exposure params
  dt_bauhaus_slider_set_default(slider, -p->range_black_relative_ev / range);

  dt_bauhaus_slider_set(slider, p->curve_pivot_x);

  darktable.gui->reset--;
}

static void _update_pivot_x(const float old_black_ev, const float old_white_ev, dt_iop_module_t *self, dt_iop_agx_params_t *const p)
{
  const dt_iop_agx_gui_data_t *g = self->gui_data;

  const float new_black_ev = p->range_black_relative_ev;
  const float new_white_ev = p->range_white_relative_ev;
  const float new_range = new_white_ev - new_black_ev;

  const float old_pivot_x = p->curve_pivot_x;
  const float old_range = old_white_ev - old_black_ev;

  // this is what we want to preserve
  const float pivot_ev = old_black_ev + (old_pivot_x * old_range);
  const float clamped_pivot_ev = CLAMPF(pivot_ev, new_black_ev, new_white_ev);

  // new_range is ensured to be > 0 due to hard limits on sliders
  p->curve_pivot_x = (clamped_pivot_ev - new_black_ev) / new_range;

  _update_pivot_slider_settings(g->basic_curve_controls.curve_pivot_x, p);
}

static void _adjust_relative_exposure_from_exposure_params(dt_iop_module_t *self)
{
  dt_iop_agx_params_t *p = self->params;

  const float old_black_ev = p->range_black_relative_ev;
  const float old_white_ev = p->range_white_relative_ev;

  const float exposure = dt_dev_exposure_get_effective_exposure(self->dev);

  p->range_black_relative_ev = CLAMPF((-8.f + 0.5f * exposure) * (1.f + p->dynamic_range_scaling), -20.f, -0.1f);
  p->range_white_relative_ev = CLAMPF((4.f + 0.8 * exposure) * (1.f + p->dynamic_range_scaling), 0.1f, 20.f);

  _update_pivot_x(old_black_ev, old_white_ev, self, p);
}

static void _agx_tone_mapping(dt_aligned_pixel_t rgb_in_out,
                              const tone_mapping_params_t *params,
                              const dt_colormatrix_t rendering_to_xyz_transposed)
{
  // record current chromaticity angle
  dt_aligned_pixel_t hsv_pixel = { 0.f };
  if(params->restore_hue)
    dt_RGB_2_HSV(rgb_in_out, hsv_pixel);
  const float h_before = hsv_pixel[0];

  dt_aligned_pixel_t transformed_pixel = { 0.f };

  for_three_channels(k, aligned(rgb_in_out, transformed_pixel : 16))
  {
    const float log_value = _apply_log_encoding(rgb_in_out[k], params->range_in_ev, params->black_relative_ev);
    transformed_pixel[k] = _apply_curve(log_value, params);
  }

  if(params->look_tuned)
    _agx_look(transformed_pixel, params, rendering_to_xyz_transposed);

  // Linearize
  for_three_channels(k, aligned(transformed_pixel : 16))
  {
    transformed_pixel[k] = powf(fmaxf(0.f, transformed_pixel[k]), params->curve_gamma);
  }

  // get post-curve chroma angle
  if(params->restore_hue)
  {
    dt_RGB_2_HSV(transformed_pixel, hsv_pixel);

    float h_after = hsv_pixel[0];

    // Mix hue back if requested
    h_after = _lerp_hue(h_before, h_after, params->look_original_hue_mix_ratio);

    hsv_pixel[0] = h_after;
    dt_HSV_2_RGB(hsv_pixel, rgb_in_out);
  }
  else
  {
    copy_pixel(rgb_in_out, transformed_pixel);
  }
}

static void _apply_auto_black_exposure(const dt_iop_module_t *self)
{
  dt_iop_agx_params_t *p = self->params;
  const dt_iop_agx_gui_data_t *g = self->gui_data;

  const float black_norm = min3f(self->picked_color_min);
  p->range_black_relative_ev =
    CLAMPF(log2f(fmaxf(_epsilon, black_norm) / 0.18f) * (1.f + p->dynamic_range_scaling),
           -20.f,
           -0.1f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->black_exposure_picker, p->range_black_relative_ev);
  --darktable.gui->reset;
}

static void _apply_auto_white_exposure(const dt_iop_module_t *self)
{
  dt_iop_agx_params_t *p = self->params;
  const dt_iop_agx_gui_data_t *g = self->gui_data;

  const float white_norm = max3f(self->picked_color_max);
  p->range_white_relative_ev =
    CLAMPF(log2f(fmaxf(_epsilon, white_norm) / 0.18f) * (1.f + p->dynamic_range_scaling),
           0.1f,
           20.f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->white_exposure_picker, p->range_white_relative_ev);
  --darktable.gui->reset;
}

static void _apply_auto_tune_exposure(const dt_iop_module_t *self)
{
  dt_iop_agx_params_t *p = self->params;
  const dt_iop_agx_gui_data_t *g = self->gui_data;

  const float black_norm = min3f(self->picked_color_min);
  p->range_black_relative_ev =
    CLAMPF(log2f(fmaxf(_epsilon, black_norm) / 0.18f) * (1.f + p->dynamic_range_scaling),
           -20.f,
           -0.1f);

  const float white_norm = max3f(self->picked_color_max);
  p->range_white_relative_ev =
    CLAMPF(log2f(fmaxf(_epsilon, white_norm) / 0.18f) * (1.f + p->dynamic_range_scaling),
           0.1f,
           20.f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->black_exposure_picker, p->range_black_relative_ev);
  dt_bauhaus_slider_set(g->white_exposure_picker, p->range_white_relative_ev);
  --darktable.gui->reset;
}

static void _read_exposure_params_callback(GtkWidget *widget,
                                     dt_iop_module_t *self)
{
  dt_iop_agx_gui_data_t *g = self->gui_data;
  if(g)
  {
    _adjust_relative_exposure_from_exposure_params(self);
    dt_iop_gui_update(self);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

// move only the pivot's relative (input) exposure, and recalculate its output based on mid-gray
static void _apply_auto_pivot_xy(dt_iop_module_t *self, const dt_iop_order_iccprofile_info_t *profile)
{
  dt_iop_agx_params_t *p = self->params;
  const dt_iop_agx_gui_data_t *g = self->gui_data;

  // Calculate norm and EV of the picked color
  const float picked_input_luminance = _luminance_from_profile(self->picked_color, profile);
  const float picked_ev = CLAMPF(log2f(fmaxf(_epsilon, picked_input_luminance) / 0.18f),
                                  p->range_black_relative_ev,
                                  p->range_white_relative_ev);
  const float range = p->range_white_relative_ev - p->range_black_relative_ev;
  const float picked_pivot_x = (picked_ev - p->range_black_relative_ev) / range;

  const tone_mapping_params_t tone_mapping_params =
     _calculate_tone_mapping_params(p);

  // see where the target_pivot is currently mapped
  const float target_y = _apply_curve(picked_pivot_x, &tone_mapping_params);
  // try to avoid changing the brightness of the pivot
  const float target_y_linearised = powf(target_y, p->curve_gamma);
  p->curve_pivot_y_linear_output = target_y_linearised;
  p->curve_pivot_x = picked_pivot_x;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->basic_curve_controls.curve_pivot_x,
                        p->curve_pivot_x);
  dt_bauhaus_slider_set(g->basic_curve_controls.curve_pivot_y_linear,
                        p->curve_pivot_y_linear_output);
  --darktable.gui->reset;
}

// move only the pivot's relative (input) exposure, but don't change its output
static void _apply_auto_pivot_x(dt_iop_module_t *self, const dt_iop_order_iccprofile_info_t *profile)
{
  dt_iop_agx_params_t *p = self->params;
  const dt_iop_agx_gui_data_t *g = self->gui_data;

  const float picked_input_luminance = _luminance_from_profile(self->picked_color, profile);
  const float picked_ev = CLAMPF(log2f(fmaxf(_epsilon, picked_input_luminance) / 0.18f),
                                  p->range_black_relative_ev,
                                  p->range_white_relative_ev);
  const float range = p->range_white_relative_ev - p->range_black_relative_ev;

  p->curve_pivot_x = (picked_ev - p->range_black_relative_ev) / range;

  // Update the slider visually
  darktable.gui->reset++;
  dt_bauhaus_slider_set(g->basic_curve_controls.curve_pivot_x, p->curve_pivot_x);
  darktable.gui->reset--;
}

static void _create_matrices(const primaries_params_t *params,
                             const dt_iop_order_iccprofile_info_t *pipe_work_profile,
                             const dt_iop_order_iccprofile_info_t *base_profile,
                             // outputs
                             dt_colormatrix_t rendering_to_xyz_transposed,
                             dt_colormatrix_t pipe_to_base_transposed,
                             dt_colormatrix_t base_to_rendering_transposed,
                             dt_colormatrix_t rendering_to_pipe_transposed)
{
  // Make adjusted primaries for generating the inset matrix
  //
  // References:
  // AgX by Troy Sobotka - https://github.com/sobotka/AgX-S2O3
  // Related discussions on Blender Artists forums -
  // https://blenderartists.org/t/feedback-development-filmic-baby-step-to-a-v2/1361663
  //
  // The idea is to "inset" the work RGB data toward achromatic
  // along spectral lines before per-channel curves. This makes
  // handling of bright, saturated colors much better as the
  // per-channel process desaturates them.
  // The primaries are also rotated to compensate for Abney etc.
  // and achieve a favourable shift towards yellow.

  // First, calculate the matrix from pipe the work profile to the base profile whose primaries
  // will be rotated/inset.
  dt_colormatrix_mul(pipe_to_base_transposed,
                     pipe_work_profile->matrix_in_transposed, // pipe->XYZ
                     base_profile->matrix_out_transposed);    // XYZ->base

  dt_colormatrix_t base_to_pipe_transposed;
  mat3SSEinv(base_to_pipe_transposed, pipe_to_base_transposed);

  // inbound path, base RGB->inset and rotated rendering space for the curve

  // Rotated, scaled primaries are calculated based on the base profile.
  float inset_and_rotated_primaries[3][2];
  for(size_t i = 0; i < 3; i++)
    dt_rotate_and_scale_primary(base_profile,
                                1.f - params->inset[i],
                                params->rotation[i],
                                i,
                                inset_and_rotated_primaries[i]);

  // The matrix to convert from the inset/rotated to XYZ. When
  // applying to the RGB values that are actually in the 'base' space,
  // it will convert them to XYZ coordinates that represent colors
  // that are partly desaturated (due to the inset) and skewed (do to
  // the rotation).
  dt_make_transposed_matrices_from_primaries_and_whitepoint(inset_and_rotated_primaries,
                                                            base_profile->whitepoint,
                                                            rendering_to_xyz_transposed);

  // The matrix to convert colors from the original 'base' space to
  // their partially desaturated and skewed versions, using the inset
  // RGB->XYZ and the original base XYZ->RGB matrices.
  dt_colormatrix_mul(base_to_rendering_transposed,
                     rendering_to_xyz_transposed,
                     base_profile->matrix_out_transposed);

  // outbound path, inset and rotated working space for the curve->base RGB

  // Rotated, primaries, with optional restoration of purity. This is
  // to be applied after the sigmoid curve; it can undo the skew and
  // recover purity (saturation).
  float outset_and_unrotated_primaries[3][2];
  for(size_t i = 0; i < 3; i++)
  {
    const float scaling = 1.f - params->master_outset_ratio * params->outset[i];
    dt_rotate_and_scale_primary(base_profile, scaling,
                                params->master_unrotation_ratio * params->unrotation[i],
                                i,
                                outset_and_unrotated_primaries[i]);
  }

  // The matrix to convert the curve's output to XYZ; the primaries
  // reflect the fact that the curve's output was inset and skewed at
  // the start of the process.  Its inverse (see the next steps), when
  // applied to RGB values in the curve's working space (which
  // actually uses the base primaries), will undo the rotation and,
  // depending on purity, push colors further from achromatic,
  // resaturating them.
  dt_colormatrix_t outset_and_unrotated_to_xyz_transposed;
  dt_make_transposed_matrices_from_primaries_and_whitepoint
      (outset_and_unrotated_primaries,
       base_profile->whitepoint,
       outset_and_unrotated_to_xyz_transposed);

  dt_colormatrix_t tmp;
  dt_colormatrix_mul(tmp,
                     outset_and_unrotated_to_xyz_transposed, // custom (outset, unrotation)->XYZ
                     base_profile->matrix_out_transposed);   // XYZ->base

  // 'tmp' is constructed the same way as
  // inbound_inset_and_rotated_to_xyz_transposed, but this matrix will
  // be used to remap colors to the 'base' profile, so we need to
  // invert it.
  dt_colormatrix_t rendering_to_base_transposed;
  mat3SSEinv(rendering_to_base_transposed, tmp);

  dt_colormatrix_mul(rendering_to_pipe_transposed,
                     rendering_to_base_transposed,
                     base_to_pipe_transposed);
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out))
  {
    return;
  }

  const dt_iop_order_iccprofile_info_t *const pipe_work_profile =
    dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  const dt_iop_agx_data_t *d = piece->data;
  const float *const in = ivoid;
  float *const out = ovoid;
  const size_t n_pixels = (size_t)roi_in->width * roi_in->height;

  // Get profiles and create matrices
  const dt_iop_order_iccprofile_info_t *const base_profile =
    _agx_get_base_profile(self->dev, pipe_work_profile, d->primaries_params.base_primaries);

  if(!base_profile)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[agx process] Failed to obtain a valid base profile. Module will not run correctly.");
    return;
  }

  dt_colormatrix_t pipe_to_base_transposed;
  dt_colormatrix_t base_to_rendering_transposed;
  dt_colormatrix_t rendering_to_pipe_transposed;
  dt_iop_order_iccprofile_info_t rendering_profile;

  _create_matrices(&d->primaries_params,
                   pipe_work_profile,
                   base_profile,
                   rendering_profile.matrix_in_transposed,
                   pipe_to_base_transposed,
                   base_to_rendering_transposed,
                   rendering_to_pipe_transposed);

  dt_colormatrix_transpose(rendering_profile.matrix_in,
                           rendering_profile.matrix_in_transposed);
  rendering_profile.nonlinearlut = FALSE; // no LUT for this linear transform

  const gboolean base_working_same_profile = pipe_work_profile == base_profile;

  DT_OMP_FOR()
  for(size_t k = 0; k < 4 * n_pixels; k += 4)
  {
    const float *const restrict pix_in = in + k;
    dt_aligned_pixel_t sanitised_in = { 0.f };
    for_each_channel(c)
    {
      const float component = pix_in[c];
      // allow about 22.5 EV above mid-gray, including out-of-gamut pixels, getting rid of NaNs
      sanitised_in[c] = isnan(component) ? 0.f : CLAMPF(component, -1e6f, 1e6f);
    }
    float *const restrict pix_out = out + k;

    // Convert from pipe working space to base space
    dt_aligned_pixel_t base_rgb = { 0.f };
    if(base_working_same_profile)
    {
      copy_pixel(base_rgb, sanitised_in);
    }
    else
    {
      dt_apply_transposed_color_matrix(sanitised_in, pipe_to_base_transposed, base_rgb);
    }

    _compress_into_gamut(base_rgb);

    dt_aligned_pixel_t rendering_rgb = { 0.f };
    dt_apply_transposed_color_matrix(base_rgb, base_to_rendering_transposed, rendering_rgb);

    // Apply the tone mapping curve and look adjustments
    _agx_tone_mapping(rendering_rgb, &d->tone_mapping_params,
                      rendering_profile.matrix_in_transposed);

    // Convert from internal rendering space back to pipe working space
    dt_apply_transposed_color_matrix(rendering_rgb, rendering_to_pipe_transposed, pix_out);

    // Copy over the alpha channel
    pix_out[3] = sanitised_in[3];
  }
}

static gboolean _agx_draw_curve(GtkWidget *widget,
                                cairo_t *crf,
                                const dt_iop_module_t *self)
{
  const dt_iop_agx_params_t *p = self->params;
  dt_iop_agx_gui_data_t *g = self->gui_data;

  const tone_mapping_params_t tone_mapping_params = _calculate_tone_mapping_params(p);

  gtk_widget_get_allocation(widget, &g->allocation);
  g->allocation.height -= DT_RESIZE_HANDLE_SIZE;

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, g->allocation.width,
                                                       g->allocation.height);
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  cairo_t *cr = cairo_create(cst);
  PangoLayout *layout = pango_cairo_create_layout(cr);

  pango_layout_set_font_description(layout, desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
  g->context = gtk_widget_get_style_context(widget);

  char text[32];

  // text metrics
  const gint font_size = pango_font_description_get_size(desc);
  pango_font_description_set_size(desc, 0.95 * font_size); // Slightly smaller font for graph
  pango_layout_set_font_description(layout, desc);

  g_strlcpy(text, "X", sizeof(text));
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &g->ink, NULL);
  const float line_height = g->ink.height;

  // set graph dimensions and margins
  const int inner_padding = DT_PIXEL_APPLY_DPI(4);
  const int inset = inner_padding;
  const float margin_left = 3.f * line_height + 2.f * inset;   // room for Y labels
  const float margin_bottom = 2.f * line_height + 2.f * inset; // room for X labels
  const float margin_top = inset + 0.5f * line_height;
  const float margin_right = inset;

  const float graph_width = g->allocation.width - margin_right - margin_left;
  const float graph_height = g->allocation.height - margin_bottom - margin_top;

  gtk_render_background(g->context, cr, 0, 0, g->allocation.width, g->allocation.height);

  // translate origin to bottom-left of graph area for easier plotting
  cairo_translate(cr, margin_left, margin_top + graph_height);
  cairo_scale(cr, 1., -1.); // Flip Y axis

  // graph background and border
  cairo_rectangle(cr, 0.0, 0.0, graph_width, graph_height);
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_fill_preserve(cr);
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));
  cairo_stroke(cr);

  // diagonal (y=x)
  cairo_save(cr);
  cairo_set_source_rgba(cr,
                        darktable.bauhaus->graph_border.red,
                        darktable.bauhaus->graph_border.green,
                        darktable.bauhaus->graph_border.blue,
                        0.5);
  cairo_move_to(cr, 0, 0);
  cairo_line_to(cr, graph_width, graph_height);
  cairo_stroke(cr);
  cairo_restore(cr);

  // linear output guide lines
  cairo_save(cr);

  set_color(cr, darktable.bauhaus->graph_fg);
  cairo_set_source_rgba(cr,
                        darktable.bauhaus->graph_fg.red,
                        darktable.bauhaus->graph_fg.green,
                        darktable.bauhaus->graph_fg.blue,
                        0.4);                   // semi-transparent
  const double dashes[] = { 4.0 / darktable.gui->ppd, 4.0 / darktable.gui->ppd }; // 4px dash, 4px gap
  cairo_set_dash(cr, dashes, 2, 0);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));

  const float linear_y_guides[] = { 0.18f / 16.f,
                                    0.18f / 8.f,
                                    0.18f / 4.f,
                                    0.18f / 2.f,
                                    0.18f,
                                    0.18f * 2.f,
                                    0.18f * 4.f,
                                    1.f };
  const int num_guides = sizeof(linear_y_guides) / sizeof(linear_y_guides[0]);

  for(int i = 0; i < num_guides; ++i)
  {
    const float y_linear = linear_y_guides[i];
    const float y_pre_gamma = powf(y_linear, 1.f / tone_mapping_params.curve_gamma);

    const float y_graph = y_pre_gamma * graph_height;

    cairo_move_to(cr, 0, y_graph);
    cairo_line_to(cr, graph_width, y_graph);
    cairo_stroke(cr);

    // label
    cairo_save(cr);
    cairo_identity_matrix(cr);                  // Reset transformations for text
    set_color(cr, darktable.bauhaus->graph_fg); // Use standard text color

    snprintf(text, sizeof(text), "%.0f%%", 100.0f * y_linear); // Format the linear value
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &g->ink, NULL);

    // position label slightly to the left of the graph
    const float label_x = margin_left - g->ink.width - inset / 2.f;
    // vertically center label on the guide line
    float label_y = margin_top + graph_height - y_graph - g->ink.height / 2.f - g->ink.y;

    // ensure label stays within vertical bounds of the graph area
    label_y = CLAMPF(label_y,
                     margin_top - g->ink.height / 2.f - g->ink.y,
                     margin_top + graph_height - g->ink.height / 2.f - g->ink.y);

    cairo_move_to(cr, label_x, label_y);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
  }

  cairo_restore(cr);
  // end linear output guide lines

  // vertical EV guide lines
  cairo_save(cr);
  // Use the same style as horizontal guides
  set_color(cr, darktable.bauhaus->graph_fg);
  cairo_set_source_rgba(cr,
                        darktable.bauhaus->graph_fg.red,
                        darktable.bauhaus->graph_fg.green,
                        darktable.bauhaus->graph_fg.blue,
                        0.4);
  cairo_set_dash(cr, dashes, 2, 0); // Use the same dash pattern
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));

  const float black_relative_ev = tone_mapping_params.black_relative_ev;
  const float white_relative_ev = tone_mapping_params.white_relative_ev;
  const float range_in_ev = tone_mapping_params.range_in_ev;

  if(range_in_ev > _epsilon) // avoid division by zero or tiny ranges
  {
    for(int ev = ceilf(black_relative_ev); ev <= floorf(white_relative_ev); ++ev)
    {
      float x_norm = (ev - black_relative_ev) / range_in_ev;
      // stays within the graph bounds
      x_norm = CLIP(x_norm);
      const float x_graph = x_norm * graph_width;

      cairo_move_to(cr, x_graph, 0);
      cairo_line_to(cr, x_graph, graph_height);
      cairo_stroke(cr);

      // label
      if(ev % 5 == 0 || ev == ceilf(black_relative_ev) || ev == floorf(white_relative_ev))
      {
        cairo_save(cr);
        cairo_identity_matrix(cr); // reset transformations for text
        set_color(cr, darktable.bauhaus->graph_fg);
        snprintf(text, sizeof(text), "%d", ev);
        pango_layout_set_text(layout, text, -1);
        pango_layout_get_pixel_extents(layout, &g->ink, NULL);
        // label slightly below the x-axis, centered horizontally
        float label_x = margin_left + x_graph - g->ink.width / 2.f - g->ink.x;
        const float label_y = margin_top + graph_height + inset / 2.f;
        // stay within horizontal bounds
        label_x = CLAMPF(label_x,
                         margin_left - g->ink.width / 2.f - g->ink.x,
                         margin_left + graph_width - g->ink.width / 2.f - g->ink.x);
        cairo_move_to(cr, label_x, label_y);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
      }
    }
  }
  cairo_restore(cr);
  // end vertical EV guide lines

  // the curve
  const float line_width = DT_PIXEL_APPLY_DPI(2.);
  cairo_set_line_width(cr, line_width);
  set_color(cr, darktable.bauhaus->graph_fg);

  const int steps = 200;

  // draw the main curve
  cairo_move_to(cr, 0, _apply_curve(0, &tone_mapping_params) * graph_height);
  for(int k = 1; k <= steps; k++)
  {
    const float x_norm = (float)k / steps;
    const float y_norm = _apply_curve(x_norm, &tone_mapping_params);
    cairo_line_to(cr, x_norm * graph_width, y_norm * graph_height);
  }
  cairo_stroke(cr);

  // overdraw warning sections in yellow if needed
  if(tone_mapping_params.need_convex_toe)
  {
    cairo_set_source_rgb(cr, 0.75, .5, 0.);
    const int toe_end_step = ceilf(tone_mapping_params.toe_transition_x * steps);
    cairo_move_to(cr, 0, _apply_curve(0, &tone_mapping_params) * graph_height);
    for(int k = 1; k <= toe_end_step; k++)
    {
      const float x_norm = (float)k / steps;
      const float y_norm = _apply_curve(x_norm, &tone_mapping_params);
      cairo_line_to(cr, x_norm * graph_width, y_norm * graph_height);
    }
    cairo_stroke(cr);
  }

  if(tone_mapping_params.need_concave_shoulder)
  {
    cairo_set_source_rgb(cr, 0.75, .5, 0.);
    const int shoulder_start_step = floorf(tone_mapping_params.shoulder_transition_x * steps);
    float x_norm = (float)shoulder_start_step / steps;
    float y_norm = _apply_curve(x_norm, &tone_mapping_params);
    cairo_move_to(cr, x_norm * graph_width, y_norm * graph_height);
    for(int k = shoulder_start_step + 1; k <= steps; k++)
    {
      x_norm = (float)k / steps;
      y_norm = _apply_curve(x_norm, &tone_mapping_params);
      cairo_line_to(cr, x_norm * graph_width, y_norm * graph_height);
    }
    cairo_stroke(cr);
  }

  // draw the toe start, shoulder start, pivot
  cairo_save(cr);
  // restore line width and color for points
  cairo_set_line_width(cr, line_width);
  set_color(cr, darktable.bauhaus->graph_fg);

  cairo_rectangle(cr, -DT_PIXEL_APPLY_DPI(4.), -DT_PIXEL_APPLY_DPI(4.),
                  graph_width + 2. * DT_PIXEL_APPLY_DPI(4.),
                  graph_height + 2. * DT_PIXEL_APPLY_DPI(4.));
  cairo_clip(cr);

  const float x_toe_graph = tone_mapping_params.toe_transition_x * graph_width;
  const float y_toe_graph = tone_mapping_params.toe_transition_y * graph_height;
  set_color(cr, darktable.bauhaus->graph_fg_active);
  cairo_arc(cr, x_toe_graph, y_toe_graph, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
  cairo_fill(cr);
  cairo_stroke(cr);

  const float x_shoulder_graph = tone_mapping_params.shoulder_transition_x * graph_width;
  const float y_shoulder_graph = tone_mapping_params.shoulder_transition_y * graph_height;
  set_color(cr, darktable.bauhaus->graph_fg_active);
  cairo_arc(cr, x_shoulder_graph, y_shoulder_graph, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
  cairo_fill(cr);
  cairo_stroke(cr);

  const float x_pivot_graph = tone_mapping_params.pivot_x * graph_width;
  const float y_pivot_graph = tone_mapping_params.pivot_y * graph_height;
  set_color(cr, darktable.bauhaus->graph_fg_active);
  cairo_arc(cr, x_pivot_graph, y_pivot_graph, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
  cairo_fill(cr);
  cairo_stroke(cr);

  cairo_restore(cr);

  // cleanup
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  g_object_unref(layout);
  pango_font_description_free(desc);

  return FALSE;
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc1_align_type(dt_iop_agx_data_t);
}

void cleanup(dt_iop_module_t *self)
{
  dt_iop_default_cleanup(self);
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

static void _update_curve_warnings(dt_iop_module_t *self)
{
  const dt_iop_agx_gui_data_t *g = self->gui_data;
  const dt_iop_agx_params_t *p = self->params;

  if(!g) return;

  const gboolean warnings_enabled = dt_conf_get_bool("plugins/darkroom/agx/enable_curve_warnings");
  const tone_mapping_params_t params = _calculate_tone_mapping_params(p);

  dt_bauhaus_widget_set_quad_paint(g->basic_curve_controls.curve_toe_power,
                                    params.need_convex_toe && warnings_enabled
                                    ? dtgtk_cairo_paint_warning : NULL, CPF_ACTIVE, NULL);
  dt_bauhaus_widget_set_quad_paint(g->basic_curve_controls.curve_shoulder_power,
                                    params.need_concave_shoulder && warnings_enabled
                                    ? dtgtk_cairo_paint_warning : NULL, CPF_ACTIVE, NULL);
}

static void _update_redraw_dynamic_gui(dt_iop_module_t* const self,
                                       const dt_iop_agx_gui_data_t* const g,
                                       const dt_iop_agx_params_t* const p)
{
  gtk_widget_set_visible(g->curve_gamma, !p->auto_gamma);
  gtk_widget_set_visible(g->primaries_controls_vbox, !p->disable_primaries_adjustments);
  const gboolean post_curve_primaries_available = !p->completely_reverse_primaries && !p->disable_primaries_adjustments;
  gtk_widget_set_visible(g->post_curve_primaries_controls_vbox, post_curve_primaries_available);
  gtk_widget_set_sensitive(g->set_post_curve_primaries_from_pre_button, post_curve_primaries_available);

  _update_curve_warnings(self);

  // Trigger redraw when any parameter changes
  gtk_widget_queue_draw(GTK_WIDGET(g->graph_drawing_area));
}

void gui_changed(dt_iop_module_t *self,
                 GtkWidget *widget,
                 void *previous)
{
  dt_iop_agx_gui_data_t *g = self->gui_data;

  if (darktable.gui->reset) return;

  dt_iop_agx_params_t *p = self->params;

  if(widget == g->black_exposure_picker)
  {
    const float old_black_ev = *(float*)previous;
    const float old_white_ev = p->range_white_relative_ev;

    _update_pivot_x(old_black_ev, old_white_ev, self, p);
  }

  if(widget == g->white_exposure_picker)
  {
    const float old_black_ev = p->range_black_relative_ev;
    const float old_white_ev = *(float*)previous;

    _update_pivot_x(old_black_ev, old_white_ev, self, p);
  }

  if(widget == g->security_factor)
  {
    const float prev = *(float *)previous;
    const float ratio = (p->dynamic_range_scaling - prev) / (prev + 1.f);

    const float old_black_ev = p->range_black_relative_ev;
    const float old_white_ev = p->range_white_relative_ev;

    p->range_black_relative_ev = old_black_ev * (1.f + ratio);
    p->range_white_relative_ev = old_white_ev * (1.f + ratio);
    _update_pivot_x(old_black_ev, old_white_ev, self, p);

    darktable.gui->reset++;
    dt_bauhaus_slider_set(g->black_exposure_picker, p->range_black_relative_ev);
    dt_bauhaus_slider_set(g->white_exposure_picker, p->range_white_relative_ev);
    darktable.gui->reset--;
  }

  if(g && p->auto_gamma)
  {
    tone_mapping_params_t tone_mapping_params;
    _set_log_mapping_params(self->params, &tone_mapping_params);
    _adjust_pivot(self->params, &tone_mapping_params);
    dt_bauhaus_slider_set(g->curve_gamma, tone_mapping_params.curve_gamma);
  }

  _update_redraw_dynamic_gui(self, g, p);
}

static GtkWidget* _create_basic_curve_controls_box(dt_iop_module_t *self,
                                                   dt_iop_agx_gui_data_t *g)
{
  GtkWidget *box = dt_gui_vbox(dt_ui_section_label_new(C_("section", "basic curve parameters")));
  GtkWidget *slider = NULL;
  dt_iop_module_t *section = DT_IOP_SECTION_FOR_PARAMS(self, NC_("section", "curve"), box);
  dt_iop_basic_curve_controls_t *controls = &g->basic_curve_controls;

  // curve_pivot_x_relative_ev with picker
  slider = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE, dt_bauhaus_slider_from_params(section, "curve_pivot_x"));
  controls->curve_pivot_x = slider;
  dt_bauhaus_slider_set_format(slider, _(" EV"));
  dt_bauhaus_slider_set_digits(slider, 2);
  gtk_widget_set_tooltip_text(slider, _("set the pivot's input exposure in EV relative to mid-gray"));
  dt_bauhaus_widget_set_quad_tooltip(slider, _("the average luminance of the selected region will be\n"
                                               "used to set the pivot relative to mid-gray"));

  // curve_pivot_y_linear
  slider = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE, dt_bauhaus_slider_from_params(section, "curve_pivot_y_linear_output"));
  controls->curve_pivot_y_linear = slider;
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 2);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider, _("darken or brighten the pivot (linear output power)"));
  dt_bauhaus_widget_set_quad_tooltip(slider, _("the average luminance of the selected region will be\n"
                                               "used to set the pivot relative to mid-gray,\n"
                                               "and the output will be adjusted based on the default\n"
                                               "mid-gray to mid-gray mapping"));

  // curve_contrast_around_pivot
  slider = dt_bauhaus_slider_from_params(section, "curve_contrast_around_pivot");
  controls->curve_contrast_around_pivot = slider;
  dt_bauhaus_slider_set_soft_range(slider, 0.1f, 5.f);
  gtk_widget_set_tooltip_text(slider, _("slope of the linear section around the pivot"));

  // curve_shoulder_power
  slider = dt_bauhaus_slider_from_params(section, "curve_shoulder_power");
  controls->curve_shoulder_power = slider;
  dt_bauhaus_slider_set_soft_range(slider, 1.f, 5.f);
  gtk_widget_set_tooltip_text(slider, _("contrast in highlights\n"
                                        "higher values keep the slope nearly constant for longer,\n"
                                        "at the cost of a more sudden drop near white"));
  dt_bauhaus_widget_set_quad_tooltip(slider,
                              _("shoulder power cannot be applied because the curve has lost its 'S' shape\n"
                                "due to the current settings for white relative exposure, contrast, and pivot.\n"
                                "to re-enable, do one of the following:\n"
                                " - increase contrast\n"
                                " - increase pivot target output\n"
                                " - increase white relative exposure\n"
                                " - increase curve y gamma (in the advanced curve parmeters section)\n"
                                "\n"
                                "open the 'show curve' section to see the effects of the above settings."));

  // curve_toe_power
  slider = dt_bauhaus_slider_from_params(section, "curve_toe_power");
  controls->curve_toe_power = slider;
  dt_bauhaus_slider_set_soft_range(slider, 1.f, 5.f);
  gtk_widget_set_tooltip_text(slider, _("contrast in shadows\n"
                                        "higher values keep the slope nearly constant for longer,\n"
                                        "at the cost of a more sudden drop near black"));
  dt_bauhaus_widget_set_quad_tooltip(slider,
                              _("toe power cannot be applied because the curve has lost its 'S' shape due\n"
                                "to the current settings for white relative exposure, contrast, and pivot.\n"
                                "to re-enable, do one of the following:\n"
                                " - increase contrast\n"
                                " - decrease pivot target output\n"
                                " - decrease black relative exposure (make more negative)\n"
                                " - decrease curve y gamma (in the advanced curve parmeters section)\n"
                                "\n"
                                "open the 'show curve' section to see the effects of the above settings."));

  return box;
}

static void _add_look_sliders(dt_iop_module_t *section)
{
  // Reuse the slider variable for all sliders instead of creating new ones in each scope
  GtkWidget *slider = NULL;

  slider = dt_bauhaus_slider_from_params(section, "look_slope");
  dt_bauhaus_slider_set_soft_range(slider, 0.f, 2.f);
  gtk_widget_set_tooltip_text(slider, _("decrease or increase contrast and brightness"));

  slider = dt_bauhaus_slider_from_params(section, "look_lift");
  dt_bauhaus_slider_set_digits(slider, 2);
  dt_bauhaus_slider_set_soft_range(slider, -0.5f, 0.5f);
  gtk_widget_set_tooltip_text(slider, _("deepen or lift shadows"));

  slider = dt_bauhaus_slider_from_params(section, "look_brightness");
  dt_bauhaus_slider_set_soft_range(slider, 0.f, 2.f);
  gtk_widget_set_tooltip_text(slider, _("increase or decrease brightness"));

  slider = dt_bauhaus_slider_from_params(section, "look_saturation");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 2);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  dt_bauhaus_slider_set_soft_range(slider, 0.f, 2.f);
  gtk_widget_set_tooltip_text(slider, _("decrease or increase saturation"));

  slider = dt_bauhaus_slider_from_params(section, "look_original_hue_mix_ratio");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 2);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider, _("increase to bring hues closer to the original"));
}

static void _add_look_box(dt_iop_module_t *self,
                          dt_iop_agx_gui_data_t *g)
{
  const gboolean look_always_visible = dt_conf_get_bool("plugins/darkroom/agx/look_always_visible");

  GtkWidget *look_box = dt_gui_vbox();

  gchar *section_name = NC_("section", "look");
  if(look_always_visible)
  {
    dt_gui_box_add(look_box, dt_ui_section_label_new(Q_(section_name)));
    _add_look_sliders(DT_IOP_SECTION_FOR_PARAMS(self, section_name, look_box));
  }
  else
  {
    dt_gui_new_collapsible_section(&g->look_section,
                                   "plugins/darkroom/agx/expand_look_params", Q_(section_name),
                                   GTK_BOX(look_box), DT_ACTION(self));
    _add_look_sliders(DT_IOP_SECTION_FOR_PARAMS(self, section_name, g->look_section.container));
  }

  dt_gui_box_add(self->widget, look_box);
}

static GtkWidget* _create_curve_graph_box(dt_iop_module_t *self,
                                          dt_iop_agx_gui_data_t *g)
{
  GtkWidget *graph_box = dt_gui_vbox();

  dt_gui_new_collapsible_section(&g->graph_section, "plugins/darkroom/agx/expand_curve_graph",
                                 _("show curve"), GTK_BOX(graph_box), DT_ACTION(self));
  g->graph_drawing_area =
      GTK_DRAWING_AREA(dt_ui_resize_wrap(NULL, 0, "plugins/darkroom/agx/curve_graph_height"));
  g_object_set_data(G_OBJECT(g->graph_drawing_area), "iop-instance", self);
  dt_action_define_iop(self, N_("curve"), N_("graph"), GTK_WIDGET(g->graph_drawing_area), NULL);
  gtk_widget_set_can_focus(GTK_WIDGET(g->graph_drawing_area), TRUE);
  g_signal_connect(G_OBJECT(g->graph_drawing_area), "draw", G_CALLBACK(_agx_draw_curve), self);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->graph_drawing_area), _("tone mapping curve"));

  // Pack drawing area at the top
  dt_gui_box_add(g->graph_section.container, g->graph_drawing_area);

  return graph_box;
}

static GtkWidget* _create_advanced_box(dt_iop_module_t *self,
                                       dt_iop_agx_gui_data_t *g)
{
  GtkWidget *advanced_box = dt_gui_vbox();

  const gchar *section_name = NC_("section", "advanced curve parameters");
  dt_gui_new_collapsible_section(&g->advanced_section,
                                 "plugins/darkroom/agx/expand_curve_advanced",
                                 Q_(section_name),
                                 GTK_BOX(advanced_box), DT_ACTION(self));
  dt_iop_module_t *section = DT_IOP_SECTION_FOR_PARAMS(self, NC_("section", "curve"),
                                                             g->advanced_section.container);

  // Reuse the slider variable for all sliders
  GtkWidget *slider = NULL;

  // Shoulder length
  slider = dt_bauhaus_slider_from_params(section, "curve_linear_ratio_above_pivot");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 2);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider,
                              _("length to keep curve linear above the pivot.\n"
                                  "may clip highlights"));

  // Shoulder intersection point
  slider = dt_bauhaus_slider_from_params(section, "curve_target_display_white_ratio");
  dt_bauhaus_slider_set_soft_range(slider, 0.5f, 1.f);
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 2);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider, _("max linear output power"));

  // Toe length
  slider = dt_bauhaus_slider_from_params(section, "curve_linear_ratio_below_pivot");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 2);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider,
                              _("length to keep curve linear below the pivot.\n"
                                  "may crush shadows"));

  // Toe intersection point
  slider = dt_bauhaus_slider_from_params(section, "curve_target_display_black_ratio");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 2);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  dt_bauhaus_slider_set_soft_range(slider, 0.f, 0.025f);
  gtk_widget_set_tooltip_text(slider, _("raise for a faded look"));

  // curve_gamma
  g->auto_gamma = dt_bauhaus_toggle_from_params(section, "auto_gamma");
  gtk_widget_set_tooltip_text
    (g->auto_gamma,
     _("adjusts the gamma automatically, trying to make sure\n"
       "the curve always remains S-shaped (given that contrast\n"
       "is high enough), so toe and shoulder controls remain effective."));

  slider = dt_bauhaus_slider_from_params(section, "curve_gamma");
  g->curve_gamma = slider;
  dt_bauhaus_slider_set_soft_range(slider, 1.f, 5.f);
  gtk_widget_set_tooltip_text
    (slider,
     _("shifts the representation (but not the output power) of the pivot\n"
       "along the y axis of the curve.\n"
       "immediate contrast around the pivot is not affected,\n"
       "but shadows and highlights are; you may have to counteract it\n"
       "with the contrast slider or with toe / shoulder controls."));

  return advanced_box;
}

static void _add_exposure_box(dt_iop_module_t *self, dt_iop_agx_gui_data_t *g, dt_iop_module_t *real_self)
{
  gchar *section_name = NC_("section", "input exposure range");
  dt_gui_box_add(self->widget, dt_ui_section_label_new(Q_(section_name)));

  GtkWidget *white_slider = dt_bauhaus_slider_from_params(self, "range_white_relative_ev");
  g->white_exposure_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE, white_slider);
  dt_bauhaus_slider_set_soft_range(g->white_exposure_picker, 1.f, 10.f);
  dt_bauhaus_slider_set_format(g->white_exposure_picker, _(" EV"));
  gtk_widget_set_tooltip_text(g->white_exposure_picker,
                              _("relative exposure above mid-gray (white point)"));
  dt_bauhaus_widget_set_quad_tooltip(g->white_exposure_picker, _("pick the white point"));


  GtkWidget *black_slider = dt_bauhaus_slider_from_params(self, "range_black_relative_ev");
  g->black_exposure_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE, black_slider);
  dt_bauhaus_slider_set_soft_range(g->black_exposure_picker, -20.f, -1.f);
  dt_bauhaus_slider_set_format(g->black_exposure_picker, _(" EV"));
  gtk_widget_set_tooltip_text(g->black_exposure_picker,
                              _("relative exposure below mid-gray (black point)"));
  dt_bauhaus_widget_set_quad_tooltip(g->black_exposure_picker, _("pick the black point"));

  g->security_factor = dt_bauhaus_slider_from_params(self, "dynamic_range_scaling");
  dt_bauhaus_slider_set_soft_max(g->security_factor, 0.5f);
  dt_bauhaus_slider_set_format(g->security_factor, "%");
  dt_bauhaus_slider_set_digits(g->security_factor, 2);
  dt_bauhaus_slider_set_factor(g->security_factor, 100.f);
  gtk_widget_set_tooltip_text(g->security_factor,
                              _("symmetrically increase or decrease the computed dynamic range.\n"
                                "useful to give a safety margin to extreme luminances."));

  g->range_exposure_picker_group = dt_gui_hbox();

  GtkWidget *auto_tune_box = dt_gui_hbox();
  GtkWidget *auto_tune_label = dt_ui_label_new(_("auto tune levels"));
  g->range_exposure_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE, NULL);
  gtk_widget_set_tooltip_text(g->range_exposure_picker, _("set black and white relative exposure using the selected area"));
  dt_action_define_iop(real_self, N_("exposure range"), N_("auto tune levels"), g->range_exposure_picker, &dt_action_def_toggle);
  dt_gui_box_add(auto_tune_box, dt_gui_expand(auto_tune_label), g->range_exposure_picker);
  dt_gui_box_add(g->range_exposure_picker_group, auto_tune_box);

  g->btn_read_exposure = dtgtk_button_new(dtgtk_cairo_paint_camera, 0, NULL);
  gtk_widget_set_tooltip_text(g->btn_read_exposure, _("read exposure from metadata and exposure module"));
  g_signal_connect(G_OBJECT(g->btn_read_exposure), "clicked", G_CALLBACK(_read_exposure_params_callback), real_self);
  dt_action_define_iop(real_self, N_("exposure range"), N_("read exposure"), g->btn_read_exposure, &dt_action_def_button);
  dt_gui_box_add(g->range_exposure_picker_group, g->btn_read_exposure);

  dt_gui_box_add(self->widget, g->range_exposure_picker_group);
}

static void _apply_primaries_from_menu_callback(GtkMenuItem *menuitem, dt_iop_module_t *self)
{
  const char *preset_id = gtk_widget_get_name(GTK_WIDGET(menuitem));
  dt_iop_agx_params_t *p = self->params;

  if(strcmp(preset_id, "blender") == 0) _set_blenderlike_primaries(p);
  else if(strcmp(preset_id, "smooth") == 0) _set_smooth_primaries(p);
  else if(strcmp(preset_id, "unmodified") == 0) _set_unmodified_primaries(p);

  dt_iop_gui_update(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _primaries_popupmenu_callback(GtkWidget *button, dt_iop_module_t *self)
{
  GtkWidget *menu = gtk_menu_new();

  GtkWidget *blender_item = gtk_menu_item_new_with_mnemonic(_("blender-like"));
  gtk_widget_set_name(blender_item, "blender");
  g_signal_connect(blender_item, "activate", G_CALLBACK(_apply_primaries_from_menu_callback), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), blender_item);

  GtkWidget *smooth_item = gtk_menu_item_new_with_mnemonic(_("smooth"));
  gtk_widget_set_name(smooth_item, "smooth");
  g_signal_connect(smooth_item, "activate", G_CALLBACK(_apply_primaries_from_menu_callback), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), smooth_item);

  GtkWidget *unmodified_item = gtk_menu_item_new_with_mnemonic(_("unmodified"));
  gtk_widget_set_name(unmodified_item, "unmodified");
  g_signal_connect(unmodified_item, "activate", G_CALLBACK(_apply_primaries_from_menu_callback), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), unmodified_item);

  gtk_widget_show_all(menu);
  dt_gui_menu_popup(GTK_MENU(menu), button, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST);
}

static void _set_post_curve_primaries_from_pre_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_agx_params_t *p = self->params;

  p->master_outset_ratio = 1.0f;
  p->master_unrotation_ratio = 1.0f;

  p->red_outset = p->red_inset;
  p->green_outset = p->green_inset;
  p->blue_outset = p->blue_inset;

  p->red_unrotation = p->red_rotation;
  p->green_unrotation = p->green_rotation;
  p->blue_unrotation = p->blue_rotation;

  dt_iop_gui_update(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

typedef void (*hsv_updater_t)(dt_aligned_pixel_t hsv_out, float position_on_slider, float hue_deg, gboolean reverse_or_attenuate);

static void _update_hsv_for_hue(dt_aligned_pixel_t hsv_out, const float position_on_slider, const float hue_deg, const gboolean reverse)
{
  const float hue_range_deg = 60.0f;
  float hue_offset_deg = -hue_range_deg + position_on_slider * (2.0f * hue_range_deg);
  if(reverse) hue_offset_deg = -hue_offset_deg;

  hsv_out[0] = fmodf(hue_deg + hue_offset_deg + 360.0f, 360.0f) / 360.0f;
  hsv_out[1] = 0.7f;
  hsv_out[2] = 1.0f;
}

static void _update_hsv_for_purity(dt_aligned_pixel_t hsv_out, const float position_on_slider, const float hue_deg, const gboolean attenuate)
{
  hsv_out[0] = hue_deg / 360.0f;
  hsv_out[1] = attenuate ? 1.0f - position_on_slider : position_on_slider;
  hsv_out[2] = 1.0f;
}

static void _paint_slider_gradient(GtkWidget *slider, const float hue_deg, const hsv_updater_t update_hsv, const gboolean attenuate_or_reverse)
{
  const float soft_min = dt_bauhaus_slider_get_soft_min(slider);
  const float soft_max = dt_bauhaus_slider_get_soft_max(slider);
  const float hard_min = dt_bauhaus_slider_get_hard_min(slider);
  const float hard_max = dt_bauhaus_slider_get_hard_max(slider);

  dt_aligned_pixel_t hsv;
  dt_aligned_pixel_t rgb;

  for(int stop = 0; stop < DT_BAUHAUS_SLIDER_MAX_STOPS; stop++)
  {
    const float position_on_slider = (float)stop / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1);

    // In order to have the desired, fixed visual clues, we must do some scaling, because bauhaus would
    // paint according to the soft limits, so we rescale according to the hard ones.
    const float value_in_soft_range = soft_min + position_on_slider * (soft_max - soft_min);
    const float value_in_hard_range = (value_in_soft_range - hard_min) / (hard_max - hard_min);

    update_hsv(hsv, position_on_slider, hue_deg, attenuate_or_reverse);

    dt_HSV_2_RGB(hsv, rgb);

    dt_bauhaus_slider_set_stop(slider, value_in_hard_range, rgb[0], rgb[1], rgb[2]);
  }
  gtk_widget_queue_draw(GTK_WIDGET(slider));
}

static GtkWidget *_setup_purity_slider(dt_iop_module_t *self,
                                       const char *param_name,
                                       const char *tooltip,
                                       const int primary_index,
                                       const float hue_deg,
                                       const gboolean attenuate)
{
  const float target_primary_value = 0.8f;
  const float other_primaries_value = 0.2;
  GtkWidget *slider = dt_bauhaus_slider_from_params(self, param_name);
  dt_bauhaus_slider_set_feedback(slider, 0);
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 2);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  dt_bauhaus_slider_set_default(slider, 0.f);

  const float r = primary_index == _red_index ? target_primary_value : other_primaries_value;
  const float g = primary_index == _green_index ? target_primary_value : other_primaries_value;
  const float b = primary_index == _blue_index ? target_primary_value : other_primaries_value;

  dt_bauhaus_slider_set_stop(slider, 0.f, r, g, b);
  gtk_widget_set_tooltip_text(slider, tooltip);

  _paint_slider_gradient(slider, hue_deg, &_update_hsv_for_purity, attenuate);

  return slider;
}

static GtkWidget *_setup_hue_slider(dt_iop_module_t *self,
                                    const char *param_name,
                                    const char *tooltip,
                                    const float hue_deg,
                                    const gboolean reverse)
{
  GtkWidget *slider = dt_bauhaus_slider_from_params(self, param_name);
  dt_bauhaus_slider_set_feedback(slider, 0);
  dt_bauhaus_slider_set_format(slider, "°");
  dt_bauhaus_slider_set_digits(slider, 1);
  dt_bauhaus_slider_set_factor(slider, RAD_2_DEG);
  gtk_widget_set_tooltip_text(slider, tooltip);
  dt_bauhaus_slider_set_default(slider, 0.f);

  _paint_slider_gradient(slider, hue_deg, &_update_hsv_for_hue, reverse);

  return slider;
}

void gui_update(dt_iop_module_t *self)
{
  const dt_iop_agx_gui_data_t* const g = self->gui_data;
  const dt_iop_agx_params_t* const p = self->params;

  _update_pivot_slider_settings(g->basic_curve_controls.curve_pivot_x, p);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->auto_gamma),
                               p->auto_gamma);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->disable_primaries_adjustments),
                               p->disable_primaries_adjustments);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->completely_reverse_primaries),
                               p->completely_reverse_primaries);

  gui_changed(self, NULL, NULL);
}

static void _create_primaries_page(dt_iop_module_t *main,
                                   dt_iop_agx_gui_data_t *g)
{
  GtkWidget *page_primaries =
    dt_ui_notebook_page(g->notebook, N_("primaries"), _("color primaries adjustments"));

  dt_iop_module_t *page = DT_IOP_SECTION_FOR_PARAMS(main, NULL, page_primaries);

  GtkWidget *base_primaries_combo = dt_bauhaus_combobox_from_params(page, "base_primaries");
  gtk_widget_set_tooltip_text(base_primaries_combo,
                              _("color space primaries to use as the base for below adjustments.\n"
                                "'export profile' uses the profile set in 'output color profile'."));

  g->disable_primaries_adjustments =
    dt_bauhaus_toggle_from_params(page, "disable_primaries_adjustments");

  gtk_widget_set_tooltip_text
    (g->disable_primaries_adjustments,
     _("disable purity adjustments and rotations, only applying the curve.\n"
       "note that those adjustments are at the heart of AgX,\n"
       "without them the results are almost always going to be worse,\n"
       "especially with bright, saturated lights (e.g. LEDs).\n"
       "mainly intended to be used for experimenting."));

  GtkWidget *primaries_button = dtgtk_button_new(dtgtk_cairo_paint_styles, 0, NULL);
  gtk_widget_set_tooltip_text(primaries_button, _("reset primaries to a predefined configuration"));
  g_signal_connect(primaries_button, "clicked", G_CALLBACK(_primaries_popupmenu_callback), main);
  dt_action_define_iop(main, NULL, N_("reset primaries"),
                       primaries_button, &dt_action_def_button);

  g->primaries_controls_vbox = dt_gui_vbox(dt_gui_hbox(dt_ui_label_new(_("reset primaries")),
                                                       dt_gui_align_right(primaries_button)));
  dt_gui_box_add(page_primaries, g->primaries_controls_vbox);

  dt_iop_module_t *self = DT_IOP_SECTION_FOR_PARAMS(main, NULL, g->primaries_controls_vbox);

  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "before tone mapping")));

  GtkWidget *slider = NULL;

  const float red_hue = 0.0f;
  const float green_hue = 120.0f;
  const float blue_hue = 240.0f;

  slider = _setup_purity_slider(self,
                                "red_inset",
                                _("increase to desaturate reds in highlights faster"),
                                _red_index,
                                red_hue,
                                TRUE);

  slider = _setup_hue_slider(self,
                             "red_rotation",
                             _("shift the red primary towards yellow (+) or magenta (-)"),
                             red_hue,
                             FALSE);

  slider = _setup_purity_slider(self,
                                "green_inset",
                                _("increase to desaturate greens in highlights faster"),
                                _green_index,
                                green_hue,
                                TRUE);

  slider = _setup_hue_slider(self,
                             "green_rotation",
                             _("shift the green primary towards cyan (+) or yellow (-)"),
                             green_hue,
                             FALSE);

  slider = _setup_purity_slider(self,
                                "blue_inset",
                                _("increase to desaturate blues in highlights faster"),
                                _blue_index,
                                blue_hue,
                                TRUE);

  slider = _setup_hue_slider(self,
                             "blue_rotation",
                             _("shift the blue primary towards magenta (+) or cyan (-)"),
                             blue_hue,
                             FALSE);

  GtkWidget *reversal_hbox = dt_gui_hbox();
  g->post_curve_primaries_controls_vbox = dt_gui_vbox();
  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "after tone mapping")),
                               reversal_hbox, g->post_curve_primaries_controls_vbox);

  self->widget = reversal_hbox;
  g->completely_reverse_primaries = dt_bauhaus_toggle_from_params(self, "completely_reverse_primaries");
  gtk_widget_set_tooltip_text(g->completely_reverse_primaries, _("completely restore purity, undo all rotations, and hide\n"
                                                                 "the controls below. uncheck to restore the previous state."));

  g->set_post_curve_primaries_from_pre_button = gtk_button_new_with_label(_("set from above"));
  gtk_widget_set_tooltip_text(g->set_post_curve_primaries_from_pre_button,
                              _("set parameters to completely reverse primaries modifications,\n"
                                  "but allow subsequent editing"));
  g_signal_connect(g->set_post_curve_primaries_from_pre_button, "clicked", G_CALLBACK(_set_post_curve_primaries_from_pre_callback), main);
  dt_action_define_iop(main, NULL, N_("reverse pre-mapping primaries"),
                       g->set_post_curve_primaries_from_pre_button, &dt_action_def_button);
  dt_gui_box_add(reversal_hbox, dt_gui_align_right(g->set_post_curve_primaries_from_pre_button));

  self->widget = g->post_curve_primaries_controls_vbox;

  slider = dt_bauhaus_slider_from_params(self, "master_outset_ratio");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 2);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  // make sure a double-click sets it to 100%, overriding preset defaults
  dt_bauhaus_slider_set_default(slider, 1.f);
  gtk_widget_set_tooltip_text(slider, _("overall purity boost"));

  slider = dt_bauhaus_slider_from_params(self, "master_unrotation_ratio");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 2);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  // make sure a double-click sets it to 100%, overriding preset defaults
  dt_bauhaus_slider_set_default(slider, 1.f);
  gtk_widget_set_tooltip_text(slider, _("overall unrotation ratio"));

  slider = _setup_purity_slider(self,
                                "red_outset",
                                _("restore the purity of red, mostly in midtones and shadows"),
                                _red_index,
                                red_hue,
                                FALSE);

  slider = _setup_hue_slider(self,
                             "red_unrotation",
                             _("reverse the color shift in reds"),
                             red_hue,
                             TRUE);

  slider = _setup_purity_slider(self,
                                "green_outset",
                                _("restore the purity of green, mostly in midtones and shadows"),
                                _green_index,
                                green_hue,
                                FALSE);

  slider = _setup_hue_slider(self,
                             "green_unrotation",
                             _("reverse the color shift in greens"),
                             green_hue,
                             TRUE);

  slider = _setup_purity_slider(self,
                                "blue_outset",
                                _("restore the purity of blue, mostly in midtones and shadows"),
                                _blue_index,
                                blue_hue,
                                FALSE);

  slider = _setup_hue_slider(self,
                             "blue_unrotation",
                             _("reverse the color shift in blues"),
                             blue_hue,
                             TRUE);
}

static void _notebook_page_changed(GtkNotebook *notebook,
                                   GtkWidget *page,
                                   const guint page_num,
                                   dt_iop_module_t *self)
{
  dt_iop_agx_gui_data_t *g = self->gui_data;
  GtkWidget *basics = g->curve_basic_controls_box;
  GtkWidget *current_parent = gtk_widget_get_parent(basics);

  // 'settings' or 'curve' page only
  if(page_num <= 1 && current_parent)
  {
    GtkWidget *target_container = (page_num == 0) ? gtk_widget_get_parent(g->range_exposure_picker_group) : page;

    if(current_parent != target_container)
    {
      g_object_ref(basics);
      gtk_container_remove(GTK_CONTAINER(current_parent), basics);
      dt_gui_box_add(target_container, basics);
      g_object_unref(basics);
    }

    int position = -1;
    if(page_num == 0)
    {
      // on settings page, place after "auto tune levels" picker group
      gtk_container_child_get(GTK_CONTAINER(target_container), g->range_exposure_picker_group,
                              "position", &position, NULL);
    }
    gtk_box_reorder_child(GTK_BOX(target_container), basics, ++position);
  }
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_agx_gui_data_t *g = IOP_GUI_ALLOC(agx);

  static dt_action_def_t notebook_def = {};
  g->notebook = dt_ui_notebook_new(&notebook_def);
  self->widget = GTK_WIDGET(g->notebook);
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);

  g->curve_basic_controls_box = _create_basic_curve_controls_box(self, g);
  g->curve_graph_box = _create_curve_graph_box(self, g);
  g->curve_advanced_controls_box = _create_advanced_box(self, g);

  GtkWidget *settings_page = dt_ui_notebook_page(g->notebook,
                                                N_("settings"),
                                                _("main look and curve settings"));
  dt_iop_module_t *settings_section = DT_IOP_SECTION_FOR_PARAMS(self, NULL, settings_page);
  _add_exposure_box(settings_section, g, self);
  dt_gui_box_add(settings_section->widget, g->curve_basic_controls_box);
  GtkWidget *curve_page_parent = settings_page;
  if(dt_conf_get_bool("plugins/darkroom/agx/enable_curve_tab"))
  {
    curve_page_parent = dt_ui_notebook_page(g->notebook,
                                            N_("curve"),
                                            _("detailed curve settings"));
    // reparent on tab switch
    g_signal_connect(g->notebook, "switch-page", G_CALLBACK(_notebook_page_changed), self);
  }
  dt_gui_box_add(curve_page_parent, g->curve_graph_box,
                                    g->curve_advanced_controls_box);

  // Finally, add the remaining sections to the settings page
  _add_look_box(settings_section, g);
  _create_primaries_page(self, g);
}

static void _set_shared_params(dt_iop_agx_params_t *p)
{
  p->look_slope = 1.f;
  p->look_brightness = 1.f;
  p->look_lift = 0.f;
  p->look_saturation = 1.f;
  // In Blender, a related param is set to 40%, but is actually used as 1 - param,
  // so 60% would give almost identical results; however, Eary_Chow suggested
  // that we leave this as 0, based on feedback he had received
  p->look_original_hue_mix_ratio = 0.f;

  p->range_black_relative_ev = -10.f;
  p->range_white_relative_ev = 6.5f;
  p->dynamic_range_scaling = 0.1f;

  p->curve_contrast_around_pivot = 2.8f;
  p->curve_linear_ratio_below_pivot = 0.f;
  p->curve_linear_ratio_above_pivot = 0.f;
  p->curve_toe_power = 1.55f;
  p->curve_shoulder_power = 1.55f;
  p->curve_target_display_black_ratio = 0.f;
  p->curve_target_display_white_ratio = 1.f;
  p->auto_gamma = FALSE;
  p->curve_gamma = _default_gamma;
  p->curve_pivot_x = -p->range_black_relative_ev / (p->range_white_relative_ev - p->range_black_relative_ev);
  p->curve_pivot_y_linear_output = 0.18f;
}

static void _set_neutral_params(dt_iop_agx_params_t *p)
{
  _set_shared_params(p);
  _set_unmodified_primaries(p);
}

void _set_smooth_params(dt_iop_agx_params_t *p)
{
  _set_shared_params(p);
  _set_smooth_primaries(p);
}

static void _set_blenderlike_params(dt_iop_agx_params_t *p)
{
  _set_shared_params(p);
  _set_blenderlike_primaries(p);

  // restore the original Blender settings
  p->curve_shoulder_power = 1.5f;
  p->curve_toe_power = 1.5f;
  p->curve_gamma = 2.4f;
  // our default gamma is 2.2, and the gamma compensation logic will be applied
  // later to scale the contrast calculated here, to finally arrive at
  // blender's default contrast, which is 2.4. If we simply set 2.4 here, the compensation
  // would yield another number.

  const float compensation_factor = _calculate_slope_gamma_compensation(p->curve_gamma, powf(0.18f, 1.f/p->curve_gamma), p);

  // we multiply by the factor instead of dividing, which will be reversed when compensating relative to gamma 2.2
  p->curve_contrast_around_pivot = 2.4f * compensation_factor;
}

static void _set_scene_referred_default_params(dt_iop_agx_params_t *p)
{
  _set_shared_params(p);
  _set_blenderlike_primaries(p);
}

static void _make_punchy(dt_iop_agx_params_t * p)
{
  // from Blender; 'power' is 1.35; darkening brightness adjustments (value < 1)
  // are dampened using sqrt in UI 'brightness' param -> algorithmic 'power' param conversion
  p->look_brightness = 1.f / (1.35f * 1.35f);
  p->look_lift = 0.f;
  p->look_saturation = 1.4f;
}

void init_presets(dt_iop_module_so_t *self)
{
  // auto-applied scene-referred default
  self->pref_based_presets = TRUE;

  dt_iop_agx_params_t p = { 0 };

  _set_neutral_params(&p);

  dt_gui_presets_add_generic(_("unmodified base primaries"),
                             self->op, self->version(), &p,
                             sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  ///////////////////////
  // Blender-like presets

  _set_blenderlike_params(&p);

  dt_gui_presets_add_generic(_("blender-like|base"),
                             self->op, self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  _make_punchy(&p);
  dt_gui_presets_add_generic(_("blender-like|punchy"),
                             self->op, self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  /////////////////////////
  // Scene-referred preset

  const char *workflow = dt_conf_get_string_const("plugins/darkroom/workflow");
  const gboolean auto_apply_agx = strcmp(workflow, "scene-referred (AgX)") == 0;

  if(auto_apply_agx)
  {
    // The scene-referred default preset
    _set_scene_referred_default_params(&p);

    dt_gui_presets_add_generic(_("scene-referred default"),
                               self->op, self->version(), &p, sizeof(p),
                               TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

    dt_gui_presets_update_format(BUILTIN_PRESET("scene-referred default"),
                                 self->op, self->version(),
                                 FOR_RAW | FOR_MATRIX);
    dt_gui_presets_update_autoapply(BUILTIN_PRESET("scene-referred default"),
                                    self->op, self->version(), TRUE);
  }

  /////////////////
  // Smooth presets

  _set_smooth_params(&p);

  dt_gui_presets_add_generic(_("smooth|base"), self->op, self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  _make_punchy(&p);
  dt_gui_presets_add_generic(_("smooth|punchy"), self->op, self->version(), &p, sizeof(p),
                             TRUE, DEVELOP_BLEND_CS_RGB_SCENE);
}

// Callback for color pickers
void color_picker_apply(dt_iop_module_t *self,
                        GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  if(darktable.gui->reset) return;

  dt_iop_agx_params_t *p = self->params;
  const dt_iop_agx_gui_data_t *g = self->gui_data;

  const float old_black_ev = p->range_black_relative_ev;
  const float old_white_ev = p->range_white_relative_ev;

  if(picker == g->black_exposure_picker) _apply_auto_black_exposure(self);
  else if(picker == g->white_exposure_picker) _apply_auto_white_exposure(self);
  else if(picker == g->range_exposure_picker) _apply_auto_tune_exposure(self);
  else if(picker == g->basic_curve_controls.curve_pivot_x) _apply_auto_pivot_x(self, dt_ioppr_get_pipe_work_profile_info(pipe));
  else if(picker == g->basic_curve_controls.curve_pivot_y_linear) _apply_auto_pivot_xy(self, dt_ioppr_get_pipe_work_profile_info(pipe));

  _update_pivot_x(old_black_ev, old_white_ev, self, p);

  if(p->auto_gamma)
  {
    ++darktable.gui->reset;
    tone_mapping_params_t tone_mapping_params;
    _set_log_mapping_params(self->params, &tone_mapping_params);
    _adjust_pivot(self->params, &tone_mapping_params);
    dt_bauhaus_slider_set(g->curve_gamma, tone_mapping_params.curve_gamma);
    --darktable.gui->reset;
  }

  _update_curve_warnings(self);
  gtk_widget_queue_draw(GTK_WIDGET(g->graph_drawing_area));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *gui_params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_agx_data_t *processing_params = piece->data;
  const dt_iop_agx_params_t *p = gui_params;

  // Calculate curve parameters once
  processing_params->tone_mapping_params = _calculate_tone_mapping_params(p);
  processing_params->primaries_params = _get_primaries_params(p);
}

void reload_defaults(dt_iop_module_t *self)
{
  if(dt_is_scene_referred())
  {
    dt_iop_agx_params_t *const d = self->default_params;
    _set_scene_referred_default_params(d);
  }
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

#ifdef HAVE_OPENCL

typedef struct dt_iop_agx_global_data_t
{
  int kernel_agx;
} dt_iop_agx_global_data_t;

void init_global(dt_iop_module_so_t *self)
{
  const int program = 39; // agx.cl, from programs.conf
  dt_iop_agx_global_data_t *gd = malloc(sizeof(dt_iop_agx_global_data_t));
  self->data = gd;
  gd->kernel_agx = dt_opencl_create_kernel(program, "kernel_agx");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_agx_global_data_t *gd = self->data;
  if(gd)
  {
    dt_opencl_free_kernel(gd->kernel_agx);
    free(self->data);
    self->data = NULL;
  }
}

int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4, self, piece->colors, (void*)dev_in, (void*)dev_out, roi_in, roi_out))
  {
    return DT_OPENCL_PROCESS_CL;
  }

  const dt_iop_agx_global_data_t *gd = self->global_data;
  const dt_iop_agx_data_t *d = piece->data;
  cl_int err = CL_SUCCESS;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  // Get profiles and create matrices
  const dt_iop_order_iccprofile_info_t *const pipe_work_profile =
    dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  const dt_iop_order_iccprofile_info_t *const base_profile =
    _agx_get_base_profile(self->dev, pipe_work_profile, d->primaries_params.base_primaries);

  if(!base_profile)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[agx process_cl] Failed to obtain a valid base profile. Module will not run correctly.");
    return DT_OPENCL_PROCESS_CL;
  }

  dt_colormatrix_t pipe_to_base;
  dt_colormatrix_t base_to_rendering;
  dt_colormatrix_t rendering_to_pipe;
  dt_colormatrix_t rendering_to_xyz;

  dt_colormatrix_t pipe_to_base_transposed;
  dt_colormatrix_t base_to_rendering_transposed;
  dt_colormatrix_t rendering_to_pipe_transposed;
  dt_colormatrix_t rendering_to_xyz_transposed;

  _create_matrices(&d->primaries_params,
                   pipe_work_profile,
                   base_profile,
                   rendering_to_xyz_transposed,
                   pipe_to_base_transposed,
                   base_to_rendering_transposed,
                   rendering_to_pipe_transposed);

  dt_colormatrix_transpose(pipe_to_base, pipe_to_base_transposed);
  dt_colormatrix_transpose(base_to_rendering, base_to_rendering_transposed);
  dt_colormatrix_transpose(rendering_to_pipe, rendering_to_pipe_transposed);
  dt_colormatrix_transpose(rendering_to_xyz, rendering_to_xyz_transposed);

  const cl_mem dev_pipe_to_base =
    dt_opencl_copy_host_to_device_constant(devid, sizeof(pipe_to_base), pipe_to_base);
  const cl_mem dev_base_to_rendering =
    dt_opencl_copy_host_to_device_constant(devid, sizeof(base_to_rendering), base_to_rendering);
  const cl_mem dev_rendering_to_pipe =
    dt_opencl_copy_host_to_device_constant(devid, sizeof(rendering_to_pipe), rendering_to_pipe);
  const cl_mem dev_rendering_to_xyz =
    dt_opencl_copy_host_to_device_constant(devid, sizeof(rendering_to_xyz), rendering_to_xyz);

  if(!dev_pipe_to_base || !dev_base_to_rendering || !dev_rendering_to_pipe || !dev_rendering_to_xyz)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  const int base_working_same_profile = pipe_work_profile == base_profile;

  err = dt_opencl_enqueue_kernel_2d_args(
    devid, gd->kernel_agx, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height),
    CLARG(d->tone_mapping_params),
    CLARG(dev_pipe_to_base),
    CLARG(dev_base_to_rendering),
    CLARG(dev_rendering_to_pipe),
    CLARG(dev_rendering_to_xyz),
    CLARG(base_working_same_profile)
  );

cleanup:
  dt_opencl_release_mem_object(dev_pipe_to_base);
  dt_opencl_release_mem_object(dev_base_to_rendering);
  dt_opencl_release_mem_object(dev_rendering_to_pipe);
  dt_opencl_release_mem_object(dev_rendering_to_xyz);

  return err;
}
#endif // HAVE_OPENCL


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
