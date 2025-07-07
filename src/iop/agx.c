/*
    This file is part of darktable,
    Copyright (C) 2020-2025 darktable developers.

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
#include "common/iop_profile.h"
#include "common/math.h"
#include "common/matrices.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
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

DT_MODULE_INTROSPECTION(3, dt_iop_agx_user_params_t)

const char *name()
{
  return _("agx");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("applies a tone mapping curve.\n"
                                  "Inspired by Blender's AgX tone mapper"),
                                _("corrective and creative"), _("linear, RGB, scene-referred"),
                                _("non-linear, RGB"), _("linear, RGB, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_TECHNICAL;
}

static const float _epsilon = 1E-6f;

typedef enum dt_iop_agx_base_primaries_t
{
  DT_AGX_EXPORT_PROFILE = 0,         // $DESCRIPTION: "export profile"
  DT_AGX_WORK_PROFILE = 1,           // $DESCRIPTION: "working profile"
  DT_AGX_REC2020 = 2,                // $DESCRIPTION: "Rec2020"
  DT_AGX_DISPLAY_P3 = 3,             // $DESCRIPTION: "Display P3"
  DT_AGX_ADOBE_RGB = 4,              // $DESCRIPTION: "Adobe RGB (compatible)"
  DT_AGX_SRGB = 5,                   // $DESCRIPTION: "sRGB"
} dt_iop_agx_base_primaries_t;

// Params exposed on the UI
typedef struct dt_iop_agx_user_params_t
{
  float look_offset_percent;           // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "offset"
  float look_slope;                    // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "slope"
  float look_power;                    // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "power"
  float look_saturation_percent;       // $MIN: 0.0 $MAX: 1000.0 $DEFAULT: 100.0 $DESCRIPTION: "saturation"
  float look_original_hue_mix_percent; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "preserve hue"

  // log mapping
  float range_black_relative_exposure;  // $MIN: -20.0 $MAX: -0.1 $DEFAULT: -10 $DESCRIPTION: "black relative exposure"
  float range_white_relative_exposure;  // $MIN: 0.1 $MAX: 20 $DEFAULT: 6.5 $DESCRIPTION: "white relative exposure"
  float security_factor;                // $MIN: -50 $MAX: 200 $DEFAULT: 10.0 $DESCRIPTION: "dynamic range scaling"

  // curve params - comments indicate the original variables from https://www.desmos.com/calculator/yrysofmx8h
  // Corresponds to p_x, but not directly -- allows shifting the default 0.18 towards black or white relative exposure
  float curve_pivot_x_shift_percent;      // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0 $DESCRIPTION: "pivot input shift"
  // Corresponds to p_y, but not directly -- needs application of gamma
  float curve_pivot_y_percent;            // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 18 $DESCRIPTION: "pivot target output"
  // P_slope
  float curve_contrast_around_pivot;      // $MIN: 0.1 $MAX: 10.0 $DEFAULT: 2.4 $DESCRIPTION: "contrast around the pivot"
  // related to P_tlength; the number expresses the portion of the y range below the pivot
  float curve_linear_percent_below_pivot;  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "toe start"
  // related to P_slength; the number expresses the portion of the y range below the pivot
  float curve_linear_percent_above_pivot;  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "shoulder start"
  // t_p
  float curve_toe_power;                  // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "toe power"
  // s_p
  float curve_shoulder_power;             // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "shoulder power"
  float curve_gamma;                      // $MIN: 0.01 $MAX: 100.0 $DEFAULT: 2.2 $DESCRIPTION: "curve y gamma"
  gboolean auto_gamma;                    // $MIN: 0 $MAX: 1 $DEFAULT: 0 $DESCRIPTION: "keep the pivot on the identity line"
  // t_ly
  float curve_target_display_black_percent;     // $MIN: 0.0 $MAX: 15.0 $DEFAULT: 0.0 $DESCRIPTION: "target black"
  // s_ly
  float curve_target_display_white_percent;     // $MIN: 20.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "target white"

  // custom primaries; 30 degrees = 0.5236 radian for rotation
  dt_iop_agx_base_primaries_t base_primaries; // $DEFAULT: DT_AGX_REC2020 $DESCRIPTION: "base primaries"
  gboolean disable_primaries_adjustments; // $MIN: 0 $MAX: 1 $DEFAULT: 0 $DESCRIPTION: "disable adjustments"
  float red_inset;        // $MIN:  0.0  $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "red attenuation"
  float red_rotation;     // $MIN: -0.5236  $MAX: 0.5236  $DEFAULT: 0.0 $DESCRIPTION: "red rotation"
  float green_inset;      // $MIN:  0.0  $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "green attenuation"
  float green_rotation;   // $MIN: -0.5236  $MAX: 0.5236  $DEFAULT: 0.0 $DESCRIPTION: "green rotation"
  float blue_inset;       // $MIN:  0.0  $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "blue attenuation"
  float blue_rotation;    // $MIN: -0.5236  $MAX: 0.5236  $DEFAULT: 0.0 $DESCRIPTION: "blue rotation"

  float master_outset_ratio;     // $MIN:  0.0  $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "master purity boost"
  float master_unrotation_ratio; // $MIN:  0.0  $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "master rotation reversal"
  float red_outset;              // $MIN:  0.0  $MAX: 2.0 $DEFAULT: 0.0 $DESCRIPTION: "red purity boost"
  float red_unrotation;          // $MIN: -0.5236  $MAX: 0.5236  $DEFAULT: 0.0 $DESCRIPTION: "red reverse rotation"
  float green_outset;            // $MIN:  0.0  $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "green purity boost"
  float green_unrotation;        // $MIN: -0.5236  $MAX: 0.5236  $DEFAULT: 0.0 $DESCRIPTION: "green reverse rotation"
  float blue_outset;             // $MIN:  0.0  $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "blue purity boost"
  float blue_unrotation;         // $MIN: -0.5236  $MAX: 0.5236  $DEFAULT: 0.0 $DESCRIPTION: "blue reverse rotation"
} dt_iop_agx_user_params_t;

typedef struct dt_iop_basic_curve_controls_t
{
  GtkWidget *curve_pivot_x_shift;
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

  // Exposure pickers and their sliders
  GtkWidget *range_exposure_picker;
  GtkWidget *black_exposure_picker;
  GtkWidget *white_exposure_picker;
  GtkWidget *security_factor;

  // the duplicated curve controls that appear on both the 'settings' and on the 'curve' page
  dt_iop_basic_curve_controls_t basic_curve_controls_settings_page;
  dt_iop_basic_curve_controls_t basic_curve_controls_curve_page;

  // curve graph/plot
  GtkAllocation allocation;
  PangoRectangle ink;
  GtkStyleContext *context;

  GtkWidget *disable_primaries_adjustments;
  GtkWidget *primaries_controls_vbox;
  gboolean curve_tab_enabled;
  GtkComboBoxText *primaries_preset_combo;
  GtkWidget *primaries_preset_apply_button;
} dt_iop_agx_gui_data_t;

typedef struct curve_and_look_params_t
{
  float min_ev;
  float max_ev;
  float range_in_ev;
  float curve_gamma;

  // the toe runs from (t_lx = 0, target black) to (toe_transition_x, toe_transition_y)
  float pivot_x;
  float pivot_y;
  float target_black; // t_ly
  float toe_power; // t_p
  float toe_transition_x; // t_tx
  float toe_transition_y; // t_ty
  float toe_scale; // t_s
  gboolean need_convex_toe;
  float toe_fallback_coefficient;
  float toe_fallback_power;

  // the linear section lies on y = mx + b, running from (toe_transition_x, toe_transition_y) to (shoulder_transition_x, shoulder_transition_y)
  // it can have length 0, in which case it only contains the pivot (pivot_x, pivot_y)
  // the pivot may coincide with toe_transition or shoulder_start or both
  float slope;     // m - for the linear section
  float intercept; // b parameter of the straight segment (y = mx + b, intersection with the y-axis at (0, b))

  // the shoulder runs from (shoulder_transition_x, shoulder_transition_y) to (s_lx = 1, target_white)
  float target_white; // s_ly
  float shoulder_power; // s_p
  float shoulder_transition_x; // s_tx
  float shoulder_transition_y; // s_ty
  float shoulder_scale; // s_s
  gboolean need_concave_shoulder;
  float shoulder_fallback_coefficient;
  float shoulder_fallback_power;

  // look
  float look_offset;
  float look_slope;
  float look_power;
  float look_saturation;
  float look_original_hue_mix_ratio;
} tone_mapping_params_t;

typedef struct primaries_params_t
{
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
  dt_colormatrix_t pipe_to_base_transposed;
  dt_colormatrix_t base_to_rendering_transposed;
  dt_colormatrix_t rendering_to_pipe_transposed;
  dt_iop_order_iccprofile_info_t rendering_profile;
} dt_iop_agx_data_t;

// Primaries preset deduplication: hashtable key type, hash and equality functions
typedef struct
{
  dt_iop_agx_base_primaries_t base_primaries;
  gboolean disable_primaries_adjustments;
  float red_inset;
  float red_rotation;
  float green_inset;
  float green_rotation;
  float blue_inset;
  float blue_rotation;
  float master_outset_ratio;
  float master_unrotation_ratio;
  float red_outset;
  float red_unrotation;
  float green_outset;
  float green_unrotation;
  float blue_outset;
  float blue_unrotation;
} _agx_primaries_key;

// djb2 hash
static inline guint _agx_primaries_hash(const gconstpointer p)
{
  guint hash = 5381;
  const unsigned char *data = p;
  size_t len = sizeof(_agx_primaries_key);

  while(len-- > 0)
  {
    hash = (hash << 5) + hash + *data++;
  }
  return hash;
}

static inline gboolean _agx_primaries_equal(const gconstpointer a, const gconstpointer b)
{
  return memcmp(a, b, sizeof(_agx_primaries_key)) == 0;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void **new_params,
                  int32_t *new_params_size, int *new_version)
{
  typedef dt_iop_agx_user_params_t dt_iop_agx_params_v2_t;

  if (old_version == 1)
  {
    typedef struct dt_iop_agx_params_v1_t
    {
      float look_offset;
      float look_slope;
      float look_power;
      float look_saturation;
      float look_original_hue_mix_ratio;

      // log mapping
      float range_black_relative_exposure;
      float range_white_relative_exposure;
      float security_factor;

      // curve params
      float curve_pivot_x_shift;
      float curve_pivot_y_linear;
      float curve_contrast_around_pivot;
      float curve_linear_percent_below_pivot;
      float curve_linear_percent_above_pivot;
      float curve_toe_power;
      float curve_shoulder_power;
      float curve_gamma;
      gboolean auto_gamma;
      float curve_target_display_black_percent;
      float curve_target_display_white_percent;

      // custom primaries
      dt_iop_agx_base_primaries_t base_primaries;
      // 'disable_primaries_adjustments' is missing here in v1
      float red_inset;
      float red_rotation;
      float green_inset;
      float green_rotation;
      float blue_inset;
      float blue_rotation;

      float master_outset_ratio;
      float master_unrotation_ratio;
      float red_outset;
      float red_unrotation;
      float green_outset;
      float green_unrotation;
      float blue_outset;
      float blue_unrotation;
    } dt_iop_agx_params_v1_t;

    dt_iop_agx_params_v2_t *np = calloc(1, sizeof(dt_iop_agx_params_v2_t));
    const dt_iop_agx_params_v1_t *op = old_params;

    // Because the new 'disable_primaries_adjustments' field was added in the middle of the struct,
    // we must copy the data in two parts, around the new field.

    // Part 1: All fields before 'disable_primaries_adjustments'.
    const size_t part1_size = offsetof(dt_iop_agx_params_v2_t, disable_primaries_adjustments);
    memcpy(np, op, part1_size);

    // Initialize the new parameter to its default value.
    np->disable_primaries_adjustments = FALSE;

    // Part 2: All fields after 'disable_primaries_adjustments'.
    const void *old_part2_start = &op->red_inset;
    void *new_part2_start = &np->red_inset;
    const size_t part2_size = sizeof(dt_iop_agx_params_v1_t) - offsetof(dt_iop_agx_params_v1_t, red_inset);
    memcpy(new_part2_start, old_part2_start, part2_size);

    // Set the output parameters for the framework.
    *new_params = np;
    *new_params_size = sizeof(dt_iop_agx_params_v2_t);
    *new_version = 2;

    return 0; // success
  }
  if (old_version == 2)
  {
    const dt_iop_agx_params_v2_t *op = old_params;
    dt_iop_agx_params_v2_t *np = calloc(1, sizeof(dt_iop_agx_params_v2_t));

    // v2 and v3 have the same layout, but some parameters are now percentages
    memcpy(np, op, sizeof(dt_iop_agx_params_v2_t));
    np->look_offset_percent *= 100;
    np->look_saturation_percent *= 100;
    np->look_original_hue_mix_percent *= 100;
    np->curve_pivot_x_shift_percent *= 100;
    np->curve_pivot_y_percent *= 100;

    // Set the output parameters for the framework.
    *new_params = np;
    *new_params_size = sizeof(dt_iop_agx_params_v2_t);
    *new_version = 3;

    return 0; // success
  }

  return 1; // no other conversion possible
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

// Get the profile info struct based on the user selection
static const dt_iop_order_iccprofile_info_t * _agx_get_base_profile(dt_develop_t *dev,
                                                                    const dt_iop_order_iccprofile_info_t *pipe_work_profile,
                                                                    const dt_iop_agx_base_primaries_t base_primaries_selection)
{
  dt_iop_order_iccprofile_info_t *selected_profile_info = NULL;

  switch(base_primaries_selection)
  {
    case DT_AGX_EXPORT_PROFILE:
    {
      dt_colorspaces_color_profile_type_t profile_type;
      const char *profile_filename;

      // Get the configured export profile settings
      dt_ioppr_get_export_profile_type(
        dev, &profile_type, &profile_filename);

      if (profile_type != DT_COLORSPACE_NONE && profile_filename != NULL)
      {
        // intent does not matter, we just need the primaries
        selected_profile_info
            = dt_ioppr_add_profile_info_to_list(dev, profile_type, profile_filename, INTENT_PERCEPTUAL);
        if (!selected_profile_info || !dt_is_valid_colormatrix(selected_profile_info->matrix_in_transposed[0][0]))
        {
          dt_print(DT_DEBUG_PIPE, "[agx] Export profile '%s' unusable or missing matrix, falling back to Rec2020.",
                   dt_colorspaces_get_name(profile_type, profile_filename));
          selected_profile_info = NULL; // Force fallback
        }
      }
      else
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[agx] Failed to get configured export profile settings, falling back to Rec2020.");
        // Fallback handled below
      }
    }
    break;

    case DT_AGX_WORK_PROFILE:
      // Cast away const, as dt_ioppr_add_profile_info_to_list returns non-const
      return (dt_iop_order_iccprofile_info_t *)pipe_work_profile;

    case DT_AGX_REC2020:
    case DT_AGX_DISPLAY_P3:
    case DT_AGX_ADOBE_RGB:
    case DT_AGX_SRGB:
    {
      const dt_colorspaces_color_profile_type_t profile_type
          = _get_base_profile_type_from_enum(base_primaries_selection);
      // Use relative intent for standard profiles when used as base
      selected_profile_info
          = dt_ioppr_add_profile_info_to_list(dev, profile_type, "", DT_INTENT_RELATIVE_COLORIMETRIC);
      if (!selected_profile_info || !dt_is_valid_colormatrix(selected_profile_info->matrix_in_transposed[0][0]))
      {
        dt_print(DT_DEBUG_PIPE,
                 "[agx] Standard base profile '%s' unusable or missing matrix, falling back to Rec2020.",
                 dt_colorspaces_get_name(profile_type, ""));
        selected_profile_info = NULL; // Force fallback
      }
    }
    break;
  }

  // Fallback if selected profile wasn't found or was invalid
  if (!selected_profile_info)
  {
    selected_profile_info =
      dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_RELATIVE_COLORIMETRIC);
    // If even Rec2020 fails, something is very wrong, but let the caller handle NULL if necessary.
    if (!selected_profile_info)
      dt_print(DT_DEBUG_ALWAYS, "[agx] CRITICAL: Failed to get even Rec2020 base profile info.");
  }

  return selected_profile_info;
}

DT_OMP_DECLARE_SIMD(aligned(pixel : 16))
static inline float _max(const dt_aligned_pixel_t pixel)
{
  return fmaxf(fmaxf(pixel[0], pixel[1]), pixel[2]);
}

DT_OMP_DECLARE_SIMD(aligned(pixel : 16))
static inline float _min(const dt_aligned_pixel_t pixel)
{
  return fminf(fminf(pixel[0], pixel[1]), pixel[2]);
}

static inline float _luminance_from_matrix(const dt_aligned_pixel_t pixel,
                                           const dt_colormatrix_t rgb_to_xyz_transposed)
{
  dt_aligned_pixel_t xyz;
  dt_apply_transposed_color_matrix(pixel, rgb_to_xyz_transposed, xyz);
  return xyz[1];
}

static inline float _luminance_from_profile(dt_aligned_pixel_t pixel,
                                            const dt_iop_order_iccprofile_info_t *const profile)
{
  return dt_ioppr_get_rgb_matrix_luminance(pixel, profile->matrix_in, profile->lut_in,
                                           profile->unbounded_coeffs_in, profile->lutsize, profile->nonlinearlut);
}


static inline float _line(const float x, const float slope, const float intercept)
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
static float _scale(const float limit_x,
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

  const float scale_value = powf(base, -1.0f / power);

  // avoid 'explosions'
  return fminf(1e9, scale_value);
}

// f_t(x), f_s(x) at https://www.desmos.com/calculator/yrysofmx8h
static inline float _sigmoid(const float x, const float power)
{
  return x / powf(1.0f + powf(x, power), 1.0f / power);
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
static inline float _fallback_toe(const float x, const tone_mapping_params_t *params)
{
  return x <= 0 ? params->target_black
                : params->target_black
                      + fmaxf(0, params->toe_fallback_coefficient * powf(x, params->toe_fallback_power));
}

static inline float _fallback_shoulder(const float x, const tone_mapping_params_t *params)
{
  return x >= 1 ? params->target_white
                : params->target_white
                      - fmaxf(0, params->shoulder_fallback_coefficient
                                     * powf(1 - x, params->shoulder_fallback_power));
}

static float _apply_curve(const float x, const tone_mapping_params_t *params)
{
  float result;

  if (x < params->toe_transition_x)
  {
    result = params->need_convex_toe ? _fallback_toe(x, params)
                                     : _scaled_sigmoid(x, params->toe_scale, params->slope, params->toe_power,
                                                       params->toe_transition_x, params->toe_transition_y);
  }
  else if (x <= params->shoulder_transition_x)
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

static inline float _sanitize_hue(float hue)
{
  if (hue < 0.0f) hue += 1.0f;
  if (hue >= 1.0f) hue -= 1.0f;
  return hue;
}

// 'lerp', but take care of the boundary: hue wraps around 1->0
static inline float _lerp_hue(float original_hue, float processed_hue, const float mix)
{
  original_hue = _sanitize_hue(original_hue);
  processed_hue = _sanitize_hue(processed_hue);

  const float hue_diff = processed_hue - original_hue;

  if (hue_diff > 0.5)
  {
    processed_hue -= 1;
  }
  else if (hue_diff < -0.5)
  {
    processed_hue += 1;
  }

  const float restored_hue = processed_hue + (original_hue - processed_hue) * mix;
  return _sanitize_hue(restored_hue);
}

static inline float _apply_slope_offset(const float x, const float slope, const float offset)
{
  // negative offset should darken the image; positive brighten it
  // without the scale: m = 1 / (1 + offset)
  // offset = 1, slope = 1, x = 0 -> m = 1 / (1+1) = 1/2, b = 1 * 1/2 = 1/2, y = 1/2*0 + 1/2 = 1/2
  const float m = slope / (1 + offset);
  const float b = offset * m;
  return m * x + b;
  // ASC CDL:
  // return x * slope + offset;
  // alternative:
  // y = mx + b, b is the offset, m = (1 - offset), so the line runs from (0, offset) to (1, 1)
  // return (1 - offset) * x + offset;
}

// https://docs.acescentral.com/specifications/acescct/#appendix-a-application-of-asc-cdl-parameters-to-acescct-image-data
DT_OMP_DECLARE_SIMD(aligned(pixel_in_out : 16))
static void _agx_look(dt_aligned_pixel_t pixel_in_out,
                      const tone_mapping_params_t *params,
                      const dt_colormatrix_t rendering_to_xyz_transposed)
{
  const float slope = params->look_slope;
  const float offset = params->look_offset;
  const float power = params->look_power;
  const float sat = params->look_saturation;

  // ASC CDL (Slope, Offset, Power) per channel
  for_three_channels(k, aligned(pixel_in_out : 16))
  {
    const float slope_and_offset_val = _apply_slope_offset(pixel_in_out[k], slope, offset);
    pixel_in_out[k] = slope_and_offset_val > 0.0f ? powf(slope_and_offset_val, power) : slope_and_offset_val;
  }

  const float luma = _luminance_from_matrix(pixel_in_out, rendering_to_xyz_transposed);

  // saturation
  for_three_channels(k, aligned(pixel_in_out : 16))
  {
    pixel_in_out[k] = luma + sat * (pixel_in_out[k] - luma);
  }
}

static inline float _apply_log_encoding(float x, const float range_in_ev, const float min_ev)
{
  // Assume input is linear RGB relative to 0.18 mid-gray
  // Ensure value > 0 before log
  x = fmaxf(_epsilon, x / 0.18f);
  // normalise to [0, 1] based on min_ev and range_in_ev
  const float mapped = (log2f(fmaxf(x, 0)) - min_ev) / range_in_ev;
  // Clamp result to [0, 1] - this is the input domain for the curve
  return CLAMPF(mapped, 0.0f, 1.0f);
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
  const float luminance_coeffs[] = { 0.2658180370250449, 0.59846986045365, 0.1357121025213052 };

  const float input_y = pixel_in_out[0] * luminance_coeffs[0] + pixel_in_out[1] * luminance_coeffs[1]
                        + pixel_in_out[2] * luminance_coeffs[2];

  // Calculate luminance of the opponent color, and use it to compensate for negative luminance values
  const float max_rgb = _max(pixel_in_out);
  dt_aligned_pixel_t inverse_rgb;
  for_each_channel(c, aligned(inverse_rgb, pixel_in_out))
  {
    inverse_rgb[c] = max_rgb - pixel_in_out[c];
  }
  const float max_inverse = _max(inverse_rgb);

  const float inverse_y = inverse_rgb[0] * luminance_coeffs[0] + inverse_rgb[1] * luminance_coeffs[1]
                          + inverse_rgb[2] * luminance_coeffs[2];

  const float y_compensate_negative = max_inverse - inverse_y + input_y;

  // Offset the input tristimulus such that there are no negatives
  const float min_rgb = _min(pixel_in_out);
  const float offset = fmaxf(-min_rgb, 0);
  dt_aligned_pixel_t rgb_offset;
  for_each_channel(c, aligned(rgb_offset, pixel_in_out))
  {
    rgb_offset[c] = pixel_in_out[c] + offset;
  }

  const float max_of_rgb_offset = _max(rgb_offset);

  // Calculate luminance of the opponent color, and use it to compensate for negative luminance values
  dt_aligned_pixel_t inverse_rgb_offset;
  for_each_channel(c, aligned(inverse_rgb_offset, rgb_offset))
  {
    inverse_rgb_offset[c] = max_of_rgb_offset - rgb_offset[c];
  }

  const float max_inverse_rgb_offset = _max(inverse_rgb_offset);
  const float y_inverse_rgb_offset = inverse_rgb_offset[0] * luminance_coeffs[0]
                                     + inverse_rgb_offset[1] * luminance_coeffs[1]
                                     + inverse_rgb_offset[2] * luminance_coeffs[2];
  float y_new = rgb_offset[0] * luminance_coeffs[0] + rgb_offset[1] * luminance_coeffs[1]
                + rgb_offset[2] * luminance_coeffs[2];
  y_new = max_inverse_rgb_offset - y_inverse_rgb_offset + y_new;

  // Compensate the intensity to match the original luminance
  const float luminance_ratio = y_new > y_compensate_negative ? y_compensate_negative / y_new : 1.0;
  for_each_channel(c, aligned(pixel_in_out, rgb_offset))
  {
    pixel_in_out[c] = luminance_ratio * rgb_offset[c];
  }
}

static void _adjust_pivot(const dt_iop_agx_user_params_t *user_params, tone_mapping_params_t *params)
{
  const float mid_gray_in_log_range = fabsf(params->min_ev / params->range_in_ev);
  if (user_params->curve_pivot_x_shift_percent < 0)
  {
    const float black_ratio = -user_params->curve_pivot_x_shift_percent / 100;
    const float gray_ratio = 1 - black_ratio;
    params->pivot_x = gray_ratio * mid_gray_in_log_range;
  }
  else if (user_params->curve_pivot_x_shift_percent > 0)
  {
    const float white_ratio = user_params->curve_pivot_x_shift_percent / 100;
    const float gray_ratio = 1 - white_ratio;
    params->pivot_x = mid_gray_in_log_range * gray_ratio + white_ratio;
  }
  else
  {
    params->pivot_x = mid_gray_in_log_range;
  }

  // don't allow pivot_x to touch the endpoints
  params->pivot_x = CLAMPF(params->pivot_x, _epsilon, 1 - _epsilon);

  if (user_params->auto_gamma)
  {
    params->curve_gamma = params->pivot_x > 0 && user_params->curve_pivot_y_percent > 0
                              ? log2f(user_params->curve_pivot_y_percent / 100) / log2f(params->pivot_x)
                              : user_params->curve_gamma;
  }
  else
  {
    params->curve_gamma = user_params->curve_gamma;
  }

  params->pivot_y
      = powf(CLAMPF(user_params->curve_pivot_y_percent / 100, user_params->curve_target_display_black_percent / 100.0,
                    user_params->curve_target_display_white_percent / 100.0),
             1.0f / params->curve_gamma);
}

static void _calculate_log_mapping_params(const dt_iop_agx_user_params_t *user_params,
                                          tone_mapping_params_t *curve_and_look_params)
{
  curve_and_look_params->max_ev = user_params->range_white_relative_exposure;
  curve_and_look_params->min_ev = user_params->range_black_relative_exposure;
  curve_and_look_params->range_in_ev = curve_and_look_params->max_ev - curve_and_look_params->min_ev;
}

static tone_mapping_params_t _calculate_tone_mapping_params(const dt_iop_agx_user_params_t *user_params)
{
  tone_mapping_params_t tone_mapping_params;

  // look
  tone_mapping_params.look_offset = user_params->look_offset_percent / 100;
  tone_mapping_params.look_slope = user_params->look_slope;
  tone_mapping_params.look_saturation = user_params->look_saturation_percent / 100;
  tone_mapping_params.look_power = user_params->look_power;
  tone_mapping_params.look_original_hue_mix_ratio = user_params->look_original_hue_mix_percent / 100;

  // log mapping
  _calculate_log_mapping_params(user_params, &tone_mapping_params);

  _adjust_pivot(user_params, &tone_mapping_params);

  // avoid range altering slope - 16.5 EV is the default AgX range; keep the meaning of slope
  tone_mapping_params.slope = user_params->curve_contrast_around_pivot * (tone_mapping_params.range_in_ev / 16.5f);

  // toe
  tone_mapping_params.target_black
      = powf(user_params->curve_target_display_black_percent / 100.0f, 1.0f / tone_mapping_params.curve_gamma);
  tone_mapping_params.toe_power = user_params->curve_toe_power;

  const float remaining_y_below_pivot = tone_mapping_params.pivot_y - tone_mapping_params.target_black;
  const float toe_length_y = remaining_y_below_pivot * user_params->curve_linear_percent_below_pivot / 100.0f;
  float dx_linear_below_pivot = toe_length_y / tone_mapping_params.slope;
  // ...and subtract it from pivot_x to get the x coordinate where the linear section joins the toe
  // ... but keep the transition point above x = 0
  tone_mapping_params.toe_transition_x = fmaxf(_epsilon, tone_mapping_params.pivot_x - dx_linear_below_pivot);
  // fix up in case the limitation kicked in
  dx_linear_below_pivot = tone_mapping_params.pivot_x - tone_mapping_params.toe_transition_x;

  // from the 'run' pivot_x->toe_transition_x, we calculate the 'rise'
  const float toe_y_below_pivot_y = tone_mapping_params.slope * dx_linear_below_pivot;
  tone_mapping_params.toe_transition_y = tone_mapping_params.pivot_y - toe_y_below_pivot_y;

  // we use the same calculation as for the shoulder, so we flip the toe left <-> right, up <-> down
  const float inverse_toe_limit_x = 1.0f; // 1 - toe_limix_x (toe_limix_x = 0, so inverse = 1)
  const float inverse_toe_limit_y = 1.0f - tone_mapping_params.target_black; // Inverse limit y

  const float inverse_toe_transition_x = 1.0f - tone_mapping_params.toe_transition_x;
  const float inverse_toe_transition_y = 1.0f - tone_mapping_params.toe_transition_y;

  // and then flip the scale
  tone_mapping_params.toe_scale = -_scale(inverse_toe_limit_x, inverse_toe_limit_y,
                                          inverse_toe_transition_x, inverse_toe_transition_y,
                                          tone_mapping_params.slope, tone_mapping_params.toe_power);

  // limit_x is 0 -> dx to limit is just toe_transition_x
  // use epsilon to avoid division by 0 later
  const float toe_dx_transition_to_limit = fmaxf(_epsilon, tone_mapping_params.toe_transition_x);
  const float toe_dy_transition_to_limit
      = fmaxf(_epsilon, tone_mapping_params.toe_transition_y - tone_mapping_params.target_black);
  const float toe_slope_transition_to_limit = toe_dy_transition_to_limit / toe_dx_transition_to_limit;
  tone_mapping_params.need_convex_toe = toe_slope_transition_to_limit > tone_mapping_params.slope;

  // toe fallback curve params
  tone_mapping_params.toe_fallback_power = _calculate_slope_matching_power(
      tone_mapping_params.slope, toe_dx_transition_to_limit, toe_dy_transition_to_limit);
  tone_mapping_params.toe_fallback_coefficient = _calculate_fallback_curve_coefficient(
      toe_dx_transition_to_limit, toe_dy_transition_to_limit, tone_mapping_params.toe_fallback_power);

  // if x went from toe_transition_x to 0, at the given slope, starting from toe_transition_y, where would we
  // intersect the y-axis?
  tone_mapping_params.intercept
      = tone_mapping_params.toe_transition_y - tone_mapping_params.slope * tone_mapping_params.toe_transition_x;

  // shoulder
  tone_mapping_params.target_white
      = powf(user_params->curve_target_display_white_percent / 100.0, 1.0f / tone_mapping_params.curve_gamma);
  const float remaining_y_above_pivot = tone_mapping_params.target_white - tone_mapping_params.pivot_y;
  const float shoulder_length_y = remaining_y_above_pivot * user_params->curve_linear_percent_above_pivot / 100.0f;
  float dx_linear_above_pivot = shoulder_length_y / tone_mapping_params.slope;

  // don't allow shoulder_transition_x to reach 1
  tone_mapping_params.shoulder_transition_x
      = fminf(1 - _epsilon, tone_mapping_params.pivot_x + dx_linear_above_pivot);
  dx_linear_above_pivot = tone_mapping_params.pivot_x - tone_mapping_params.shoulder_transition_x;
  tone_mapping_params.shoulder_transition_y = tone_mapping_params.pivot_y + shoulder_length_y;
  tone_mapping_params.shoulder_power = user_params->curve_shoulder_power;

  const float shoulder_limit_x = 1;
  tone_mapping_params.shoulder_scale = _scale(
      shoulder_limit_x, tone_mapping_params.target_white, tone_mapping_params.shoulder_transition_x,
      tone_mapping_params.shoulder_transition_y, tone_mapping_params.slope, tone_mapping_params.shoulder_power);

  const float shoulder_dx_transition_to_limit
      = fmaxf(_epsilon, 1 - tone_mapping_params.shoulder_transition_x); // dx to 0, avoid division by 0 later
  const float shoulder_dy_transition_to_limit
      = fmaxf(_epsilon, tone_mapping_params.target_white - tone_mapping_params.shoulder_transition_y);
  const float shoulder_slope_transition_to_limit
      = shoulder_dy_transition_to_limit / shoulder_dx_transition_to_limit;
  tone_mapping_params.need_concave_shoulder = shoulder_slope_transition_to_limit > tone_mapping_params.slope;

  // shoulder fallback curve params
  tone_mapping_params.shoulder_fallback_power = _calculate_slope_matching_power(
      tone_mapping_params.slope, shoulder_dx_transition_to_limit, shoulder_dy_transition_to_limit);
  tone_mapping_params.shoulder_fallback_coefficient
      = _calculate_fallback_curve_coefficient(shoulder_dx_transition_to_limit, shoulder_dy_transition_to_limit,
                                              tone_mapping_params.shoulder_fallback_power);

  return tone_mapping_params;
}

static primaries_params_t _get_primaries_params(const dt_iop_agx_user_params_t *user_params)
{
  primaries_params_t primaries_params;

  primaries_params.inset[0] = user_params->red_inset;
  primaries_params.inset[1] = user_params->green_inset;
  primaries_params.inset[2] = user_params->blue_inset;
  primaries_params.rotation[0] = user_params->red_rotation;
  primaries_params.rotation[1] = user_params->green_rotation;
  primaries_params.rotation[2] = user_params->blue_rotation;
  primaries_params.master_outset_ratio = user_params->master_outset_ratio;
  primaries_params.master_unrotation_ratio = user_params->master_unrotation_ratio;
  primaries_params.outset[0] = user_params->red_outset;
  primaries_params.outset[1] = user_params->green_outset;
  primaries_params.outset[2] = user_params->blue_outset;
  primaries_params.unrotation[0] = user_params->red_unrotation;
  primaries_params.unrotation[1] = user_params->green_unrotation;
  primaries_params.unrotation[2] = user_params->blue_unrotation;

  if (user_params->disable_primaries_adjustments)
  {
    for(int i = 0; i < 3; i++)
      primaries_params.inset[i] = primaries_params.rotation[i] = primaries_params.outset[i]
          = primaries_params.unrotation[i] = 0.0f;
  }

  return primaries_params;
}

static void _agx_tone_mapping(dt_aligned_pixel_t rgb_in_out, const tone_mapping_params_t *params,
                              const dt_colormatrix_t rendering_to_xyz_transposed)
{
  // record current chromaticity angle
  dt_aligned_pixel_t hsv_pixel = { 0.0f };
  dt_RGB_2_HSV(rgb_in_out, hsv_pixel);
  const float h_before = hsv_pixel[0];

  dt_aligned_pixel_t transformed_pixel = { 0.0f };

  for_three_channels(k, aligned(rgb_in_out, transformed_pixel : 16))
  {
    const float log_value = _apply_log_encoding(rgb_in_out[k], params->range_in_ev, params->min_ev);
    transformed_pixel[k] = _apply_curve(log_value, params);
  }

  _agx_look(transformed_pixel, params, rendering_to_xyz_transposed);

  // Linearize
  for_three_channels(k, aligned(transformed_pixel : 16))
  {
    transformed_pixel[k] = powf(fmaxf(0.0f, transformed_pixel[k]), params->curve_gamma);
  }

  // get post-curve chroma angle
  dt_RGB_2_HSV(transformed_pixel, hsv_pixel);

  float h_after = hsv_pixel[0];

  // Mix hue back if requested
  h_after = _lerp_hue(h_before, h_after, params->look_original_hue_mix_ratio);

  hsv_pixel[0] = h_after;
  dt_HSV_2_RGB(hsv_pixel, rgb_in_out);
}

// Apply logic for black point picker
static void apply_auto_black_exposure(dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_agx_user_params_t *user_params = self->params;
  const dt_iop_agx_gui_data_t *gui_data = self->gui_data;

  const float black_norm = _min(self->picked_color_min);
  user_params->range_black_relative_exposure = CLAMPF(log2f(fmaxf(_epsilon, black_norm) / 0.18f), -20.0f, -0.1f)
                                               * (1.0f + user_params->security_factor / 100.0f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(gui_data->black_exposure_picker, user_params->range_black_relative_exposure);
  --darktable.gui->reset;

  gtk_widget_queue_draw(GTK_WIDGET(gui_data->graph_drawing_area));
}

// Apply logic for white point picker
static void apply_auto_white_exposure(dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_agx_user_params_t *user_params = self->params;
  const dt_iop_agx_gui_data_t *gui_data = self->gui_data;

  const float white_norm = _max(self->picked_color_max);
  user_params->range_white_relative_exposure = CLAMPF(log2f(fmaxf(_epsilon, white_norm) / 0.18f), 0.1f, 20.0f)
                                               * (1.0f + user_params->security_factor / 100.0f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(gui_data->white_exposure_picker, user_params->range_white_relative_exposure);
  --darktable.gui->reset;
}

// Apply logic for auto-tuning both black and white points
static void apply_auto_tune_exposure(dt_iop_module_t *self)
{
  if (darktable.gui->reset) return;
  dt_iop_agx_user_params_t *user_params = self->params;
  const dt_iop_agx_gui_data_t *gui_data = self->gui_data;

  // Black point
  const float black_norm = _min(self->picked_color_min);
  user_params->range_black_relative_exposure = CLAMPF(log2f(fmaxf(_epsilon, black_norm) / 0.18f), -20.0f, -0.1f)
                                               * (1.0f + user_params->security_factor / 100.0f);

  // White point
  const float white_norm = _max(self->picked_color_max);
  user_params->range_white_relative_exposure = CLAMPF(log2f(fmaxf(_epsilon, white_norm) / 0.18f), 0.1f, 20.0f)
                                               * (1.0f + user_params->security_factor / 100.0f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(gui_data->black_exposure_picker, user_params->range_black_relative_exposure);
  dt_bauhaus_slider_set(gui_data->white_exposure_picker, user_params->range_white_relative_exposure);
  --darktable.gui->reset;
}

// Apply logic for pivot x picker
static void apply_auto_pivot_x(dt_iop_module_t *self, const dt_iop_order_iccprofile_info_t *profile)
{
  if (darktable.gui->reset) return;
  dt_iop_agx_user_params_t *user_params = self->params;
  const dt_iop_agx_gui_data_t *gui_data = self->gui_data;

  // Calculate norm and EV of the picked color
  const float norm = _luminance_from_profile(self->picked_color, profile);
  const float picked_ev = log2f(fmaxf(_epsilon, norm) / 0.18f);

  // Calculate the target pivot_x based on the picked EV and the current EV range
  const float min_ev = user_params->range_black_relative_exposure;
  const float max_ev = user_params->range_white_relative_exposure;
  const float range_in_ev = fmaxf(_epsilon, max_ev - min_ev);
  const float target_pivot_x = CLAMPF((picked_ev - min_ev) / range_in_ev, 0.0f, 1.0f);

  // Calculate the required pivot_x_shift to achieve the target_pivot_x
  const float base_pivot_x = fabsf(min_ev / range_in_ev); // Pivot representing 0 EV (mid-gray)

  dt_iop_agx_user_params_t params_with_mid_gray = *user_params;
  params_with_mid_gray.curve_pivot_y_percent = 18;
  params_with_mid_gray.curve_pivot_x_shift_percent = 0;

  const tone_mapping_params_t tone_mapping_params = _calculate_tone_mapping_params(&params_with_mid_gray);

  // see where the target_pivot would be mapped with defaults of mid-gray to mid-gray mapped
  const float target_y = _apply_curve(target_pivot_x, &tone_mapping_params);
  // try to avoid changing the brightness of the pivot
  const float target_y_linearised = powf(target_y, tone_mapping_params.curve_gamma);
  user_params->curve_pivot_y_percent = target_y_linearised * 100;

  float shift;
  if (fabsf(target_pivot_x - base_pivot_x) < _epsilon)
  {
    shift = 0.0f;
  }
  else if (base_pivot_x > target_pivot_x)
  {
    // Solve target_pivot_x = (1 + s) * base_pivot_x for s
    shift = (base_pivot_x > _epsilon) ? (target_pivot_x / base_pivot_x) - 1.0f : -1.0f;
  }
  else // target_pivot_x > base_pivot_x
  {
    // Solve target_pivot_x = base_pivot_x * (1 - s) + s for s
    const float denominator = 1.0f - base_pivot_x;
    shift = (denominator > _epsilon) ? (target_pivot_x - base_pivot_x) / denominator : 1.0f;
  }

  user_params->curve_pivot_x_shift_percent = CLAMPF(shift, -1.0f, 1.0f) * 100;

  // Update the slider visually
  ++darktable.gui->reset;
  dt_bauhaus_slider_set(gui_data->basic_curve_controls_settings_page.curve_pivot_x_shift,
                        user_params->curve_pivot_x_shift_percent);
  dt_bauhaus_slider_set(gui_data->basic_curve_controls_settings_page.curve_pivot_y_linear,
                        user_params->curve_pivot_y_percent);
  if (gui_data->curve_tab_enabled)
  {
    dt_bauhaus_slider_set(gui_data->basic_curve_controls_curve_page.curve_pivot_x_shift,
                          user_params->curve_pivot_x_shift_percent);
    dt_bauhaus_slider_set(gui_data->basic_curve_controls_curve_page.curve_pivot_y_linear,
                          user_params->curve_pivot_y_percent);
  }
  --darktable.gui->reset;
}

static void _create_matrices(const dt_iop_agx_user_params_t *user_params,
                             const dt_iop_order_iccprofile_info_t *pipe_work_profile,
                             const dt_iop_order_iccprofile_info_t *base_profile,
                             // outputs
                             dt_colormatrix_t rendering_to_xyz_transposed,
                             dt_colormatrix_t pipe_to_base_transposed,
                             dt_colormatrix_t base_to_rendering_transposed,
                             dt_colormatrix_t rendering_to_pipe_transposed)
{
  const primaries_params_t params = _get_primaries_params(user_params);

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
    dt_rotate_and_scale_primary(base_profile, 1.f - params.inset[i], params.rotation[i], i,
                                inset_and_rotated_primaries[i]);

  // The matrix to convert from the inset/rotated to XYZ. When applying to the RGB values that are actually
  // in the 'base' space, it will convert them to XYZ coordinates that represent colors that are partly
  // desaturated (due to the inset) and skewed (do to the rotation).
  dt_make_transposed_matrices_from_primaries_and_whitepoint(inset_and_rotated_primaries, base_profile->whitepoint,
                                                            rendering_to_xyz_transposed);

  // The matrix to convert colors from the original 'base' space to their partially desaturated and skewed
  // versions, using the inset RGB->XYZ and the original base XYZ->RGB matrices.
  dt_colormatrix_mul(base_to_rendering_transposed, rendering_to_xyz_transposed,
                     base_profile->matrix_out_transposed);

  // outbound path, inset and rotated working space for the curve->base RGB

  // Rotated, primaries, with optional restoration of purity. This is to be applied after the sigmoid curve;
  // it can undo the skew and recover purity (saturation).
  float outset_and_unrotated_primaries[3][2];
  for(size_t i = 0; i < 3; i++)
  {
    const float scaling = 1.f - params.master_outset_ratio * params.outset[i];
    dt_rotate_and_scale_primary(base_profile, scaling, params.master_unrotation_ratio * params.unrotation[i], i,
                                outset_and_unrotated_primaries[i]);
  }

  // The matrix to convert the curve's output to XYZ; the primaries reflect the fact that the curve's output
  // was inset and skewed at the start of the process.
  // Its inverse (see the next steps), when applied to RGB values in the curve's working space (which actually uses
  // the base primaries), will undo the rotation and, depending on purity, push colours further from achromatic,
  // resaturating them.
  dt_colormatrix_t outset_and_unrotated_to_xyz_transposed;
  dt_make_transposed_matrices_from_primaries_and_whitepoint(
      outset_and_unrotated_primaries, base_profile->whitepoint, outset_and_unrotated_to_xyz_transposed);

  dt_colormatrix_t tmp;
  dt_colormatrix_mul(tmp,
                     outset_and_unrotated_to_xyz_transposed, // custom (outset, unrotation)->XYZ
                     base_profile->matrix_out_transposed     // XYZ->base
  );

  // 'tmp' is constructed the same way as inbound_inset_and_rotated_to_xyz_transposed,
  // but this matrix will be used to remap colours to the 'base' profile, so we need to invert it.
  dt_colormatrix_t rendering_to_base_transposed;
  mat3SSEinv(rendering_to_base_transposed, tmp);

  dt_colormatrix_mul(rendering_to_pipe_transposed, rendering_to_base_transposed, base_to_pipe_transposed);
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if (!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out))
  {
    return;
  }

  const dt_iop_agx_data_t *processing_params = piece->data;
  const float *const in = ivoid;
  float *const out = ovoid;
  const size_t n_pixels = (size_t)roi_in->width * roi_in->height;

  DT_OMP_FOR()
  for(size_t k = 0; k < 4 * n_pixels; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;

    // Convert from pipe working space to base space
    dt_aligned_pixel_t base_rgb;
    dt_apply_transposed_color_matrix(pix_in, processing_params->pipe_to_base_transposed, base_rgb);

    _compress_into_gamut(base_rgb);

    dt_aligned_pixel_t rendering_rgb;
    dt_apply_transposed_color_matrix(base_rgb, processing_params->base_to_rendering_transposed, rendering_rgb);

    // Apply the tone mapping curve and look adjustments
    _agx_tone_mapping(rendering_rgb, &processing_params->tone_mapping_params,
                      processing_params->rendering_profile.matrix_in_transposed);

    // Convert from internal rendering space back to pipe working space
    dt_apply_transposed_color_matrix(rendering_rgb, processing_params->rendering_to_pipe_transposed, pix_out);

    // Copy over the alpha channel
    pix_out[3] = pix_in[3];
  }
}

// Plot the curve
static gboolean _agx_draw_curve(GtkWidget *widget, cairo_t *crf, const dt_iop_module_t *self)
{
  const dt_iop_agx_user_params_t *user_params = self->params;
  dt_iop_agx_gui_data_t *gui_data = self->gui_data;

  const tone_mapping_params_t tone_mapping_params = _calculate_tone_mapping_params(user_params);

  // --- Boilerplate cairo/pango setup ---
  gtk_widget_get_allocation(widget, &gui_data->allocation);
  gui_data->allocation.height -= DT_RESIZE_HANDLE_SIZE;

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gui_data->allocation.width,
                                                       gui_data->allocation.height);
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  cairo_t *cr = cairo_create(cst);
  PangoLayout *layout = pango_cairo_create_layout(cr);

  pango_layout_set_font_description(layout, desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
  gui_data->context = gtk_widget_get_style_context(widget);

  char text[256];

  // Get text metrics
  const gint font_size = pango_font_description_get_size(desc);
  pango_font_description_set_size(desc, 0.95 * font_size); // Slightly smaller font for graph
  pango_layout_set_font_description(layout, desc);

  g_strlcpy(text, "X", sizeof(text));
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &gui_data->ink, NULL);
  const float line_height = gui_data->ink.height;

  // Set graph dimensions and margins (simplified from filmic)
  const int inner_padding = DT_PIXEL_APPLY_DPI(4);
  const int inset = inner_padding;
  const float margin_left = 3. * line_height + 2. * inset;   // Room for Y labels
  const float margin_bottom = 2. * line_height + 2. * inset; // Room for X labels
  const float margin_top = inset + 0.5 * line_height;
  const float margin_right = inset;

  const float graph_width = gui_data->allocation.width - margin_right - margin_left;
  const float graph_height = gui_data->allocation.height - margin_bottom - margin_top;

  // --- Drawing starts ---
  gtk_render_background(gui_data->context, cr, 0, 0, gui_data->allocation.width, gui_data->allocation.height);

  // Translate origin to bottom-left of graph area for easier plotting
  cairo_translate(cr, margin_left, margin_top + graph_height);
  cairo_scale(cr, 1., -1.); // Flip Y axis

  // Draw graph background and border
  cairo_rectangle(cr, 0, 0, graph_width, graph_height);
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_fill_preserve(cr);
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));
  cairo_stroke(cr);

  // Draw identity line (y=x)
  cairo_save(cr);
  cairo_set_source_rgba(cr, darktable.bauhaus->graph_border.red, darktable.bauhaus->graph_border.green,
                        darktable.bauhaus->graph_border.blue, 0.5);
  cairo_move_to(cr, 0, 0);
  cairo_line_to(cr, graph_width, graph_height);
  cairo_stroke(cr);
  cairo_restore(cr);

  // --- Draw Gamma Guide Lines ---
  cairo_save(cr);
  // Use a distinct style for guides, e.g., dashed and semi-transparent
  set_color(cr, darktable.bauhaus->graph_fg); // Use foreground color for now
  cairo_set_source_rgba(cr, darktable.bauhaus->graph_fg.red, darktable.bauhaus->graph_fg.green,
                        darktable.bauhaus->graph_fg.blue, 0.4);             // Make it semi-transparent
  const double dashes[] = { 4.0 / darktable.gui->ppd, 4.0 / darktable.gui->ppd }; // 4px dash, 4px gap
  cairo_set_dash(cr, dashes, 2, 0);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));

  const float linear_y_guides[]
      = { 0.18f / 16, 0.18f / 8, 0.18f / 4, 0.18f / 2, 0.18f, 0.18f * 2, 0.18f * 4, 1.0f };
  const int num_guides = sizeof(linear_y_guides) / sizeof(linear_y_guides[0]);

  for(int i = 0; i < num_guides; ++i)
  {
    const float y_linear = linear_y_guides[i];
    const float y_pre_gamma = powf(y_linear, 1.0f / tone_mapping_params.curve_gamma);

    const float y_graph = y_pre_gamma * graph_height;

    cairo_move_to(cr, 0, y_graph);
    cairo_line_to(cr, graph_width, y_graph);
    cairo_stroke(cr);

    // Draw label for the guide line
    cairo_save(cr);
    cairo_identity_matrix(cr);                  // Reset transformations for text
    set_color(cr, darktable.bauhaus->graph_fg); // Use standard text color

    snprintf(text, sizeof(text), "%.2f", y_linear); // Format the linear value
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &gui_data->ink, NULL);

    // Position label slightly to the left of the graph
    const float label_x = margin_left - gui_data->ink.width - inset / 2.0f;
    // Vertically center label on the guide line (remember Y is flipped)
    float label_y = margin_top + graph_height - y_graph - gui_data->ink.height / 2.0f - gui_data->ink.y;

    // Ensure label stays within vertical bounds of the graph area
    label_y = CLAMPF(label_y, margin_top - gui_data->ink.height / 2.0f - gui_data->ink.y,
                     margin_top + graph_height - gui_data->ink.height / 2.0f - gui_data->ink.y);

    cairo_move_to(cr, label_x, label_y);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
  }

  // Restore original drawing state (solid line, etc.)
  cairo_restore(cr); // Matches cairo_save(cr) at the beginning of this block
  // --- End Draw Gamma Guide Lines ---

  // --- Draw Vertical EV Guide Lines ---
  cairo_save(cr);
  // Use the same style as horizontal guides
  set_color(cr, darktable.bauhaus->graph_fg);
  cairo_set_source_rgba(cr, darktable.bauhaus->graph_fg.red, darktable.bauhaus->graph_fg.green,
                        darktable.bauhaus->graph_fg.blue, 0.4);
  cairo_set_dash(cr, dashes, 2, 0); // Use the same dash pattern
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));

  const float min_ev = tone_mapping_params.min_ev;
  const float max_ev = tone_mapping_params.max_ev;
  const float range_in_ev = tone_mapping_params.range_in_ev;

  if (range_in_ev > _epsilon) // Avoid division by zero or tiny ranges
  {
    for(int ev = ceilf(min_ev); ev <= floorf(max_ev); ++ev)
    {
      float x_norm = (ev - min_ev) / range_in_ev;
      // Clamp to ensure it stays within the graph bounds if min/max_ev aren't exactly integers
      x_norm = CLAMPF(x_norm, 0.0f, 1.0f);
      const float x_graph = x_norm * graph_width;

      cairo_move_to(cr, x_graph, 0);
      cairo_line_to(cr, x_graph, graph_height);
      cairo_stroke(cr);

      // Draw label for the EV guide line
      if (ev % 5 == 0 || ev == ceilf(min_ev) || ev == floorf(max_ev))
      {
        cairo_save(cr);
        cairo_identity_matrix(cr); // Reset transformations for text
        set_color(cr, darktable.bauhaus->graph_fg);
        snprintf(text, sizeof(text), "%d", ev); // Format the EV value
        pango_layout_set_text(layout, text, -1);
        pango_layout_get_pixel_extents(layout, &gui_data->ink, NULL);
        // Position label slightly below the x-axis, centered horizontally
        float label_x = margin_left + x_graph - gui_data->ink.width / 2.0f - gui_data->ink.x;
        const float label_y = margin_top + graph_height + inset / 2.0f;
        // Ensure label stays within horizontal bounds
        label_x = CLAMPF(label_x, margin_left - gui_data->ink.width / 2.0f - gui_data->ink.x,
                         margin_left + graph_width - gui_data->ink.width / 2.0f - gui_data->ink.x);
        cairo_move_to(cr, label_x, label_y);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
      }
    }
  }
  cairo_restore(cr); // Matches cairo_save(cr) at the beginning of this block
  // --- End Draw Vertical EV Guide Lines ---

  // Draw the curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  set_color(cr, darktable.bauhaus->graph_fg);

  const int steps = 200;
  for(int k = 0; k <= steps; k++)
  {
    const float x_norm = (float)k / steps; // Input to the curve [0, 1]
    const float y_norm = _apply_curve(x_norm, &tone_mapping_params);

    // Map normalized coords [0,1] to graph pixel coords
    const float x_graph = x_norm * graph_width;
    const float y_graph = y_norm * graph_height;

    if (k == 0)
      cairo_move_to(cr, x_graph, y_graph);
    else
      cairo_line_to(cr, x_graph, y_graph);
  }
  cairo_stroke(cr);

  // Draw the pivot point
  cairo_save(cr);
  cairo_rectangle(cr, -DT_PIXEL_APPLY_DPI(4.), -DT_PIXEL_APPLY_DPI(4.), graph_width + 2. * DT_PIXEL_APPLY_DPI(4.),
                  graph_height + 2. * DT_PIXEL_APPLY_DPI(4.));
  cairo_clip(cr);

  const float x_pivot_graph = tone_mapping_params.pivot_x * graph_width;
  const float y_pivot_graph = tone_mapping_params.pivot_y * graph_height;
  set_color(cr, darktable.bauhaus->graph_fg_active); // Use a distinct color, e.g., active foreground
  cairo_arc(cr, x_pivot_graph, y_pivot_graph, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI); // Adjust radius as needed
  cairo_fill(cr);
  cairo_stroke(cr);
  cairo_restore(cr);

  // --- Cleanup ---
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

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc1_align_type(dt_iop_agx_data_t);
}

void cleanup(dt_iop_module_t *self)
{
  dt_iop_default_cleanup(self);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

static void _update_primaries_checkbox_and_sliders(dt_iop_module_t *self)
{
  const dt_iop_agx_gui_data_t *gui_data = self->gui_data;
  const dt_iop_agx_user_params_t *user_params = self->params;

  if (gui_data && gui_data->primaries_controls_vbox)
  {
    gtk_widget_set_visible(gui_data->primaries_controls_vbox, !user_params->disable_primaries_adjustments);
  }
}

void gui_changed(dt_iop_module_t *self, GtkWidget *widget, void *previous)
{
  const dt_iop_agx_gui_data_t *gui_data = self->gui_data;
  dt_iop_agx_user_params_t *user_params = self->params;

  if (widget == gui_data->security_factor)
  {
    darktable.gui->reset++;
    const float prev = *(float *)previous;
    const float ratio = (user_params->security_factor - prev) / (prev + 100.0f);

    user_params->range_black_relative_exposure *= (1.0f + ratio);
    user_params->range_white_relative_exposure *= (1.0f + ratio);

    dt_bauhaus_slider_set(gui_data->black_exposure_picker, user_params->range_black_relative_exposure);
    dt_bauhaus_slider_set(gui_data->white_exposure_picker, user_params->range_white_relative_exposure);
    darktable.gui->reset--;
  }

  if (gui_data->curve_tab_enabled)
  {
    // --- START MANUAL SYNC ---
    if (!darktable.gui->reset) // Check the global reset guard
    {
      darktable.gui->reset++; // Prevent recursion

#define SYNC_SLIDER(param_name, slider1, slider2)                                                                 \
  if (widget == slider1)                                                                                           \
  {                                                                                                               \
    dt_bauhaus_slider_set(slider2, dt_bauhaus_slider_get(slider1));                                               \
  }                                                                                                               \
  else if (widget == slider2)                                                                                      \
  {                                                                                                               \
    dt_bauhaus_slider_set(slider1, dt_bauhaus_slider_get(slider2));                                               \
  }

      SYNC_SLIDER("curve_pivot_x_shift_percent", gui_data->basic_curve_controls_settings_page.curve_pivot_x_shift,
                  gui_data->basic_curve_controls_curve_page.curve_pivot_x_shift);
      SYNC_SLIDER("curve_pivot_y_percent", gui_data->basic_curve_controls_settings_page.curve_pivot_y_linear,
                  gui_data->basic_curve_controls_curve_page.curve_pivot_y_linear);
      SYNC_SLIDER("curve_contrast_around_pivot",
                  gui_data->basic_curve_controls_settings_page.curve_contrast_around_pivot,
                  gui_data->basic_curve_controls_curve_page.curve_contrast_around_pivot);
      SYNC_SLIDER("curve_toe_power", gui_data->basic_curve_controls_settings_page.curve_toe_power,
                  gui_data->basic_curve_controls_curve_page.curve_toe_power);
      SYNC_SLIDER("curve_shoulder_power", gui_data->basic_curve_controls_settings_page.curve_shoulder_power,
                  gui_data->basic_curve_controls_curve_page.curve_shoulder_power);

#undef SYNC_SLIDER

      darktable.gui->reset--; // Release the guard
    }
    // --- END MANUAL SYNC ---
  }

  _update_primaries_checkbox_and_sliders(self);

  // Test which widget was changed.
  // If allowing w == NULL, this can be called from gui_update, so that
  // gui configuration adjustments only need to be dealt with once, here.

  // Trigger redraw when any parameter changes
  if (gui_data && gui_data->graph_drawing_area)
  {
    gtk_widget_queue_draw(GTK_WIDGET(gui_data->graph_drawing_area));
  }

  if (gui_data && user_params->auto_gamma)
  {
    tone_mapping_params_t tone_mapping_params;
    _calculate_log_mapping_params(self->params, &tone_mapping_params);
    _adjust_pivot(self->params, &tone_mapping_params);
    dt_bauhaus_slider_set(gui_data->curve_gamma, tone_mapping_params.curve_gamma);
  }
}

static void _add_basic_curve_controls(dt_iop_module_t *self, dt_iop_basic_curve_controls_t *controls)
{
  GtkWidget *slider;

  // curve_pivot_x_shift with picker
  slider = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE,
                               dt_bauhaus_slider_from_params(self, "curve_pivot_x_shift_percent"));
  controls->curve_pivot_x_shift = slider;
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_soft_range(slider, -50, 50);
  gtk_widget_set_tooltip_text(slider, _("shift the pivot input towards black(-) or white(+)"));

  // curve_pivot_y_linear
  slider = dt_bauhaus_slider_from_params(self, "curve_pivot_y_percent");
  controls->curve_pivot_y_linear = slider;
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 100.0f);
  gtk_widget_set_tooltip_text(slider, _("darken or brighten the pivot (output)"));

  // curve_contrast_around_pivot
  slider = dt_bauhaus_slider_from_params(self, "curve_contrast_around_pivot");
  controls->curve_contrast_around_pivot = slider;
  dt_bauhaus_slider_set_soft_range(slider, 0.1f, 5.0f);
  gtk_widget_set_tooltip_text(slider, _("slope of the linear section"));

  // curve_toe_power
  slider = dt_bauhaus_slider_from_params(self, "curve_toe_power");
  controls->curve_toe_power = slider;
  dt_bauhaus_slider_set_soft_range(slider, 1.0f, 5.0f);
  gtk_widget_set_tooltip_text(slider, _("contrast in shadows"));

  // curve_shoulder_power
  slider = dt_bauhaus_slider_from_params(self, "curve_shoulder_power");
  controls->curve_shoulder_power = slider;
  dt_bauhaus_slider_set_soft_range(slider, 1.0f, 5.0f);
  gtk_widget_set_tooltip_text(slider, _("contrast in highlights"));
}

static void _add_basic_curve_controls_settings_page(dt_iop_module_t *self, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *parent = self->widget;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(parent), self->widget, FALSE, FALSE, 0);

  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "basic curve parameters")));

  _add_basic_curve_controls(self, &gui_data->basic_curve_controls_settings_page);

  self->widget = parent;
}

static void _add_basic_curve_controls_curve_page(dt_iop_module_t *self, dt_iop_agx_gui_data_t *gui_data)
{
  _add_basic_curve_controls(self, &gui_data->basic_curve_controls_curve_page);
}

static void _add_look_sliders(dt_iop_module_t *self, GtkWidget *parent_widget)
{
  GtkWidget *original_self_widget = self->widget;
  self->widget = parent_widget;

  // Reuse the slider variable for all sliders instead of creating new ones in each scope
  GtkWidget *slider;

  // look_offset
  slider = dt_bauhaus_slider_from_params(self, "look_offset_percent");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_soft_range(slider, -50, 50);
  gtk_widget_set_tooltip_text(slider, _("deepen or lift shadows"));

  // look_slope
  slider = dt_bauhaus_slider_from_params(self, "look_slope");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
  gtk_widget_set_tooltip_text(slider, _("decrease or increase contrast and brightness"));

  // look_power
  slider = dt_bauhaus_slider_from_params(self, "look_power");
  dt_bauhaus_slider_set_soft_range(slider, 0.5f, 2.0f);
  gtk_widget_set_tooltip_text(slider, _("increase or decrease brightness"));

  // look_saturation
  slider = dt_bauhaus_slider_from_params(self, "look_saturation_percent");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 200.0f);
  gtk_widget_set_tooltip_text(slider, _("decrease or increase saturation"));

  // look_original_hue_mix_ratio
  slider = dt_bauhaus_slider_from_params(self, "look_original_hue_mix_percent");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 100.0f);
  gtk_widget_set_tooltip_text(slider, _("increase to bring hues closer to original"));

  self->widget = original_self_widget;
}

static void _add_look_box(dt_iop_module_t *self, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *parent = self->widget;

  GtkWidget *look_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  self->widget = look_box;
  dt_gui_new_collapsible_section(&gui_data->look_section, "plugins/darkroom/agx/expand_look_params", _("look"),
                                 GTK_BOX(look_box), DT_ACTION(self));
  _add_look_sliders(self, GTK_WIDGET(gui_data->look_section.container));

  self->widget = parent;

  gtk_box_pack_start(GTK_BOX(parent), look_box, FALSE, FALSE, 0);
}

static void _add_curve_graph(dt_iop_module_t *self, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *parent = self->widget;

  GtkWidget *graph_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(parent), graph_box, FALSE, FALSE, 0);
  self->widget = graph_box;

  dt_gui_new_collapsible_section(&gui_data->graph_section, "plugins/darkroom/agx/expand_curve_graph",
                                 _("show curve"), GTK_BOX(graph_box), DT_ACTION(self));
  GtkWidget *graph_container = GTK_WIDGET(gui_data->graph_section.container);
  gui_data->graph_drawing_area
      = GTK_DRAWING_AREA(dt_ui_resize_wrap(NULL, 0, "plugins/darkroom/agx/curve_graph_height"));
  g_object_set_data(G_OBJECT(gui_data->graph_drawing_area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("graph"), GTK_WIDGET(gui_data->graph_drawing_area), NULL);
  gtk_widget_set_can_focus(GTK_WIDGET(gui_data->graph_drawing_area), TRUE);
  g_signal_connect(G_OBJECT(gui_data->graph_drawing_area), "draw", G_CALLBACK(_agx_draw_curve), self);
  gtk_widget_set_tooltip_text(GTK_WIDGET(gui_data->graph_drawing_area), _("tone mapping curve"));

  // Pack drawing area at the top
  gtk_box_pack_start(GTK_BOX(graph_container), GTK_WIDGET(gui_data->graph_drawing_area), TRUE, TRUE, 0);

  self->widget = parent;
}

static void _add_advanced_box(dt_iop_module_t *self, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *parent = self->widget;

  GtkWidget *advanced_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(parent), advanced_box, FALSE, FALSE, 0);
  self->widget = advanced_box;

  dt_gui_new_collapsible_section(&gui_data->advanced_section, "plugins/darkroom/agx/expand_curve_advanced",
                                 _("advanced"), GTK_BOX(advanced_box), DT_ACTION(self));
  self->widget = GTK_WIDGET(gui_data->advanced_section.container);

  // Reuse the slider variable for all sliders
  GtkWidget *slider;

  // Toe length
  slider = dt_bauhaus_slider_from_params(self, "curve_linear_percent_below_pivot");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 100.0f);
  dt_bauhaus_slider_set_format(slider, "%");
  gtk_widget_set_tooltip_text(slider,
                              _("length to keep curve linear below pivot.\n"
                                "may crush shadows"));

  // Toe intersection point
  slider = dt_bauhaus_slider_from_params(self, "curve_target_display_black_percent");
  dt_bauhaus_slider_set_digits(slider, 3);
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
  gtk_widget_set_tooltip_text(slider, _("raise for a faded look"));

  // Shoulder length
  slider = dt_bauhaus_slider_from_params(self, "curve_linear_percent_above_pivot");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 100.0f);
  dt_bauhaus_slider_set_format(slider, "%");
  gtk_widget_set_tooltip_text(slider,
                              _("length to keep curve linear above pivot.\n"
                                "may clip highlights"));

  // Shoulder intersection point
  slider = dt_bauhaus_slider_from_params(self, "curve_target_display_white_percent");
  dt_bauhaus_slider_set_soft_range(slider, 50.0f, 100.0f);
  dt_bauhaus_slider_set_digits(slider, 0);
  dt_bauhaus_slider_set_format(slider, "%");
  gtk_widget_set_tooltip_text(slider, _("max output brightness"));

  // curve_gamma
  gui_data->auto_gamma = dt_bauhaus_toggle_from_params(self, "auto_gamma");
  gtk_widget_set_tooltip_text(gui_data->auto_gamma,
                              _("tries to make sure the curve always remains S-shaped,\n"
                                "given that contrast is high enough, so toe and shoulder\n"
                                "controls remain effective.\n"
                                "affects overall contrast, you may have to counteract it with the contrast slider."));

  slider = dt_bauhaus_slider_from_params(self, "curve_gamma");
  gui_data->curve_gamma = slider;
  dt_bauhaus_slider_set_soft_range(slider, 1.0f, 5.0f);
  gtk_widget_set_tooltip_text(slider,
                              _("shifts representation (but not output brightness) of pivot\n"
                                "along the y axis of the curve.\n"
                                "affects overall contrast, you may have to counteract it with the contrast slider."));

  self->widget = parent;
}

static void _add_curve_section(dt_iop_module_t *self, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *parent = self->widget;

  GtkWidget *curve_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), curve_box, TRUE, TRUE, 0);
  self->widget = curve_box;

  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "curve parameters")));

  _add_basic_curve_controls_curve_page(self, gui_data);

  _add_advanced_box(self, gui_data);

  self->widget = parent;
}

static void _add_exposure_box(dt_iop_module_t *self, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *parent = self->widget;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // Create section label
  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "input exposure range")));

  // white point slider and picker
  gui_data->white_exposure_picker
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE,
                            dt_bauhaus_slider_from_params(self, "range_white_relative_exposure"));
  dt_bauhaus_slider_set_soft_range(gui_data->white_exposure_picker, 1.0f, 20.0f);
  dt_bauhaus_slider_set_format(gui_data->white_exposure_picker, _(" EV"));
  gtk_widget_set_tooltip_text(gui_data->white_exposure_picker,
                              _("relative exposure above mid-grey (white point)"));

  // black point slider and picker
  gui_data->black_exposure_picker
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE,
                            dt_bauhaus_slider_from_params(self, "range_black_relative_exposure"));
  dt_bauhaus_slider_set_soft_range(gui_data->black_exposure_picker, -20.0f, -1.0f);
  dt_bauhaus_slider_set_format(gui_data->black_exposure_picker, _(" EV"));
  gtk_widget_set_tooltip_text(gui_data->black_exposure_picker,
                              _("relative exposure below mid-grey (black point)"));

  // Dynamic range scaling
  gui_data->security_factor = dt_bauhaus_slider_from_params(self, "security_factor");
  dt_bauhaus_slider_set_soft_max(gui_data->security_factor, 50);
  dt_bauhaus_slider_set_format(gui_data->security_factor, "%");
  gtk_widget_set_tooltip_text(gui_data->security_factor,
                              _("symmetrically increase or decrease the computed dynamic range.\n"
                                "useful to give a safety margin to extreme luminances."));

  // auto-tune picker
  gui_data->range_exposure_picker
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE, dt_bauhaus_combobox_new(self));
  dt_bauhaus_widget_set_label(gui_data->range_exposure_picker, NULL, N_("auto tune levels"));
  gtk_widget_set_tooltip_text(gui_data->range_exposure_picker,
                              _("pick image area to automatically set black and white exposure"));
  gtk_box_pack_start(GTK_BOX(self->widget), gui_data->range_exposure_picker, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(parent), self->widget, FALSE, FALSE, 0);

  self->widget = parent;
}

static void _populate_primaries_presets_combobox(dt_iop_module_t *self)
{
  const dt_iop_agx_gui_data_t *gui_data = self->gui_data;
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(gui_data->primaries_preset_combo));

  // Use a hash table to track unique primaries configurations.
  GHashTable *seen_presets = g_hash_table_new_full(_agx_primaries_hash, _agx_primaries_equal, g_free, NULL);

  sqlite3_stmt *stmt;
  // Fetch name and parameters, filtering by current module version to ensure struct compatibility.
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, op_params"
                              " FROM data.presets"
                              " WHERE operation = ?1 AND op_version = ?2 "
                              " ORDER BY writeprotect DESC, LOWER(name), rowid",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());

  gtk_combo_box_text_append(gui_data->primaries_preset_combo, NULL, _("select a preset..."));

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *preset_name = (const char *)sqlite3_column_text(stmt, 0);
    const int op_params_size = sqlite3_column_bytes(stmt, 1);
    if (op_params_size != sizeof(dt_iop_agx_user_params_t))
    {
      dt_print(DT_DEBUG_ALWAYS, "invalid params size %u for preset %s", op_params_size, preset_name);
      continue;
    }

    const dt_iop_agx_user_params_t *preset_params = sqlite3_column_blob(stmt, 1);

    _agx_primaries_key *key = g_new0(_agx_primaries_key, 1);
    key->base_primaries = preset_params->base_primaries;
    key->disable_primaries_adjustments = preset_params->disable_primaries_adjustments;
    key->red_inset = preset_params->red_inset;
    key->red_rotation = preset_params->red_rotation;
    key->green_inset = preset_params->green_inset;
    key->green_rotation = preset_params->green_rotation;
    key->blue_inset = preset_params->blue_inset;
    key->blue_rotation = preset_params->blue_rotation;
    key->master_outset_ratio = preset_params->master_outset_ratio;
    key->master_unrotation_ratio = preset_params->master_unrotation_ratio;
    key->red_outset = preset_params->red_outset;
    key->red_unrotation = preset_params->red_unrotation;
    key->green_outset = preset_params->green_outset;
    key->green_unrotation = preset_params->green_unrotation;
    key->blue_outset = preset_params->blue_outset;
    key->blue_unrotation = preset_params->blue_unrotation;

    if (!g_hash_table_contains(seen_presets, key))
    {
      g_hash_table_insert(seen_presets, key, (gpointer)1);

      gchar *local_name = dt_util_localize_segmented_name(preset_name, TRUE);
      gtk_combo_box_text_append(gui_data->primaries_preset_combo, preset_name, local_name);
      g_free(local_name); // 'name', OTOH, is managed by sqlite
    }
    else
    {
      // duplicate, discard
      g_free(key);
    }
  }

  sqlite3_finalize(stmt);
  g_hash_table_destroy(seen_presets);

  gtk_combo_box_set_active(GTK_COMBO_BOX(gui_data->primaries_preset_combo), 0);
}

static void _apply_primaries_from_preset_callback(GtkButton *button, dt_iop_module_t *self)
{
  const dt_iop_agx_gui_data_t *gui_data = self->gui_data;
  dt_iop_agx_user_params_t *current_params = self->params;
  const gchar *preset_name = gtk_combo_box_get_active_id(GTK_COMBO_BOX(gui_data->primaries_preset_combo));

  if (preset_name && gtk_combo_box_get_active(GTK_COMBO_BOX(gui_data->primaries_preset_combo)))
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT op_params FROM data.presets"
                                " WHERE operation = ?1 AND name = ?2 AND op_version = ?3",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->op, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, preset_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, self->version());

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int op_params_size = sqlite3_column_bytes(stmt, 0);
      if (op_params_size == sizeof(dt_iop_agx_user_params_t))
      {
        const dt_iop_agx_user_params_t *preset_params = sqlite3_column_blob(stmt, 0);

        // Copy only the primaries settings
        current_params->base_primaries = preset_params->base_primaries;
        current_params->disable_primaries_adjustments = preset_params->disable_primaries_adjustments;
        current_params->red_inset = preset_params->red_inset;
        current_params->red_rotation = preset_params->red_rotation;
        current_params->green_inset = preset_params->green_inset;
        current_params->green_rotation = preset_params->green_rotation;
        current_params->blue_inset = preset_params->blue_inset;
        current_params->blue_rotation = preset_params->blue_rotation;
        current_params->master_outset_ratio = preset_params->master_outset_ratio;
        current_params->master_unrotation_ratio = preset_params->master_unrotation_ratio;
        current_params->red_outset = preset_params->red_outset;
        current_params->red_unrotation = preset_params->red_unrotation;
        current_params->green_outset = preset_params->green_outset;
        current_params->green_unrotation = preset_params->green_unrotation;
        current_params->blue_outset = preset_params->blue_outset;
        current_params->blue_unrotation = preset_params->blue_unrotation;

        // Update UI and commit changes
        dt_iop_gui_update(self);
        dt_dev_add_history_item(darktable.develop, self, TRUE);
      }
    }

    sqlite3_finalize(stmt);
  }

  // refresh the list and set selection prompt
  _populate_primaries_presets_combobox(self);
}

// GUI update (called when module UI is shown/refreshed)
void gui_update(dt_iop_module_t *self)
{
  const dt_iop_agx_gui_data_t *gui_data = self->gui_data;
  const dt_iop_agx_user_params_t *user_params = self->params;

  if (gui_data)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gui_data->auto_gamma), user_params->auto_gamma);
    if (user_params->auto_gamma)
    {
      tone_mapping_params_t tone_mapping_params;
      _calculate_log_mapping_params(self->params, &tone_mapping_params);
      _adjust_pivot(self->params, &tone_mapping_params);
      dt_bauhaus_slider_set(gui_data->curve_gamma, tone_mapping_params.curve_gamma);
    }
  }

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gui_data->disable_primaries_adjustments),
                               user_params->disable_primaries_adjustments);

  _update_primaries_checkbox_and_sliders(self);

  // Ensure the graph is drawn initially
  if (gui_data && gui_data->graph_drawing_area)
  {
    gtk_widget_queue_draw(GTK_WIDGET(gui_data->graph_drawing_area));
  }
}

static GtkWidget *_add_primaries_box(dt_iop_module_t *self)
{
  dt_iop_agx_gui_data_t *gui_data = self->gui_data;
  GtkWidget *main_box = self->widget;

  GtkWidget *primaries_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  self->widget = primaries_box;

  gui_data->disable_primaries_adjustments = dt_bauhaus_toggle_from_params(self, "disable_primaries_adjustments");
  gtk_widget_set_tooltip_text(gui_data->disable_primaries_adjustments,
                              _("disable purity adjustments and rotations, only applying the curve.\n"
                                "note that those adjustments are at the heart of AgX,\n"
                                "without them the results are almost always going to be worse,\n"
                                "especially with bright, saturated lights (e.g. LEDs).\n"
                                "mainly intended to be used for experimenting."));

  gui_data->primaries_controls_vbox = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(primaries_box), self->widget, FALSE, FALSE, 0);

  GtkWidget *preset_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(gui_data->primaries_controls_vbox), preset_hbox, FALSE, FALSE, 0);

  gui_data->primaries_preset_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
  gtk_widget_set_tooltip_text(GTK_WIDGET(gui_data->primaries_preset_combo),
                              _("load primaries settings from a preset"));
  gtk_box_pack_start(GTK_BOX(preset_hbox), GTK_WIDGET(gui_data->primaries_preset_combo), TRUE, TRUE, 0);

  _populate_primaries_presets_combobox(self);
  gui_data->primaries_preset_apply_button = gtk_button_new_with_label(_("apply"));
  gtk_widget_set_tooltip_text(gui_data->primaries_preset_apply_button,
                              _("apply primaries settings from the selected preset"));
  g_signal_connect(gui_data->primaries_preset_apply_button, "clicked",
                   G_CALLBACK(_apply_primaries_from_preset_callback), self);
  gtk_box_pack_start(GTK_BOX(preset_hbox), gui_data->primaries_preset_apply_button, FALSE, FALSE, 0);

  GtkWidget *base_primaries_combo = dt_bauhaus_combobox_from_params(self, "base_primaries");
  gtk_widget_set_tooltip_text(base_primaries_combo,
                              _("color space primaries to use as the base for below adjustments.\n"
                                "'export profile' uses the profile set in 'output color profile'."));

  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "before tone mapping")));

  GtkWidget *slider;
  const float desaturation = 0.2f;
#define SETUP_COLOR_COMBO(color, r, g, b, attenuation_suffix, inset_tooltip, rotation_suffix, rotation_tooltip)   \
  slider = dt_bauhaus_slider_from_params(self, #color attenuation_suffix);                                        \
  dt_bauhaus_slider_set_format(slider, "%");                                                                      \
  dt_bauhaus_slider_set_digits(slider, 1);                                                                        \
  dt_bauhaus_slider_set_factor(slider, 100.f);                                                                    \
  dt_bauhaus_slider_set_soft_range(slider, 0.f, 0.5f);                                                            \
  dt_bauhaus_slider_set_stop(slider, 0.f, r, g, b);                                                               \
  gtk_widget_set_tooltip_text(slider, inset_tooltip);                                                             \
                                                                                                                  \
  slider = dt_bauhaus_slider_from_params(self, #color rotation_suffix);                                           \
  dt_bauhaus_slider_set_format(slider, "°");                                                                      \
  dt_bauhaus_slider_set_digits(slider, 1);                                                                        \
  dt_bauhaus_slider_set_factor(slider, 180.f / M_PI_F);                                                           \
  dt_bauhaus_slider_set_stop(slider, 0.f, r, g, b);                                                               \
  gtk_widget_set_tooltip_text(slider, rotation_tooltip);

  SETUP_COLOR_COMBO(red, 1.f - desaturation, desaturation, desaturation, "_inset",
                    _("attenuate the purity of the red primary"), "_rotation", _("rotate the red primary"));
  SETUP_COLOR_COMBO(green, desaturation, 1.f - desaturation, desaturation, "_inset",
                    _("attenuate the purity of the green primary"), "_rotation", _("rotate the green primary"));
  SETUP_COLOR_COMBO(blue, desaturation, desaturation, 1.f - desaturation, "_inset",
                    _("attenuate the purity of the blue primary"), "_rotation", _("rotate the blue primary"));

  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "after tone mapping")));

  slider = dt_bauhaus_slider_from_params(self, "master_outset_ratio");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 0);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider, _("overall purity boost"));

  slider = dt_bauhaus_slider_from_params(self, "master_unrotation_ratio");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 0);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider, _("overall unrotation ratio"));

  SETUP_COLOR_COMBO(red, 1.f - desaturation, desaturation, desaturation, "_outset",
                    _("boost the purity of the red primary"), "_unrotation", _("unrotate the red primary"));
  SETUP_COLOR_COMBO(green, desaturation, 1.f - desaturation, desaturation, "_outset",
                    _("boost the purity of the green primary"), "_unrotation", _("unrotate the green primary"));
  SETUP_COLOR_COMBO(blue, desaturation, desaturation, 1.f - desaturation, "_outset",
                    _("boost the purity of the blue primary"), "_unrotation", _("unrotate the blue primary"));
#undef SETUP_COLOR_COMBO

  self->widget = main_box;
  return primaries_box;
}

static void _create_settings_page(dt_iop_module_t *self, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *parent = self->widget;

  GtkWidget *settings_page =
    dt_ui_notebook_page(gui_data->notebook, N_("settings"), _("main look and curve settings"));
  self->widget = settings_page;

  _add_look_box(self, gui_data);

  _add_exposure_box(self, gui_data);

  if (!gui_data->curve_tab_enabled)
  {
    _add_curve_graph(self, gui_data);
  }

  _add_basic_curve_controls_settings_page(self, gui_data);

  if (!gui_data->curve_tab_enabled)
  {
    _add_advanced_box(self, gui_data);
  }

  self->widget = parent;
}

static void _create_curve_page(dt_iop_module_t *self, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *parent = self->widget;

  GtkWidget *curve_page = dt_ui_notebook_page(gui_data->notebook, N_("curve"), _("detailed curve settings"));
  self->widget = curve_page;

  _add_curve_graph(self, gui_data);
  _add_curve_section(self, gui_data);

  self->widget = parent;
}

static void _create_primaries_page(dt_iop_module_t *self, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *parent = self->widget;

  GtkWidget *page_primaries
      = dt_ui_notebook_page(gui_data->notebook, N_("primaries"), _("color primaries adjustments"));
  GtkWidget *primaries_box = _add_primaries_box(self);
  gtk_box_pack_start(GTK_BOX(page_primaries), primaries_box, FALSE, FALSE, 0);

  self->widget = parent;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_agx_gui_data_t *gui_data = IOP_GUI_ALLOC(agx);
  GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gui_data->curve_tab_enabled = dt_conf_get_bool("plugins/darkroom/agx/enable_curve_tab");

  // the notebook
  static dt_action_def_t notebook_def = {};
  gui_data->notebook = dt_ui_notebook_new(&notebook_def);
  GtkWidget *notebook_widget = GTK_WIDGET(gui_data->notebook);
  dt_action_define_iop(self, NULL, N_("page"), notebook_widget, &notebook_def);
  gtk_box_pack_start(GTK_BOX(main_vbox), notebook_widget, TRUE, TRUE, 0);

  _create_settings_page(self, gui_data);

  if (gui_data->curve_tab_enabled)
  {
    _create_curve_page(self, gui_data);
  }

  _create_primaries_page(self, gui_data);

  self->widget = main_vbox;
  gui_update(self);
}

static void _set_neutral_params(dt_iop_agx_user_params_t *user_params)
{
  user_params->look_slope = 1.0f;
  user_params->look_power = 1.0f;
  user_params->look_offset_percent = 0.0f;
  user_params->look_saturation_percent = 100.0f;
  user_params->look_original_hue_mix_percent = 0.0f;

  user_params->range_black_relative_exposure = -10;
  user_params->range_white_relative_exposure = 6.5;
  user_params->security_factor = 10.0f;

  user_params->curve_contrast_around_pivot = 2.4;
  user_params->curve_linear_percent_below_pivot = 0.0;
  user_params->curve_linear_percent_below_pivot = 0.0;
  user_params->curve_toe_power = 1.5;
  user_params->curve_shoulder_power = 1.5;
  user_params->curve_target_display_black_percent = 0.0;
  user_params->curve_target_display_white_percent = 100.0;
  user_params->auto_gamma = FALSE;
  user_params->curve_gamma = 2.2f;
  user_params->curve_pivot_x_shift_percent = 0.0f;
  user_params->curve_pivot_y_percent = 18.0f;

  user_params->disable_primaries_adjustments = FALSE;
  user_params->red_inset = 0.0f;
  user_params->red_rotation = 0.0;
  user_params->green_inset = 0.0;
  user_params->green_rotation = 0.0f;
  user_params->blue_inset = 0.0f;
  user_params->blue_rotation = 0.0f;

  user_params->master_outset_ratio = 1.0f;
  user_params->master_unrotation_ratio = 1.0f;
  user_params->red_outset = 0.0f;
  user_params->red_unrotation = 0.0f;
  user_params->green_outset = 0.0f;
  user_params->green_unrotation = 0.0f;
  user_params->blue_outset = 0.0f;
  user_params->blue_unrotation = 0.0f;

  user_params->base_primaries = DT_AGX_REC2020;
}

void init_presets(dt_iop_module_so_t *self)
{
  // auto-applied scene-referred default
  self->pref_based_presets = TRUE;

  dt_iop_agx_user_params_t user_params = { 0 };

  _set_neutral_params(&user_params);

  dt_gui_presets_add_generic(_("unmodified base primaries"), self->op, self->version(), &user_params,
                             sizeof(user_params), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // AgX primaries settings from Eary_Chow
  // https://discuss.pixls.us/t/blender-agx-in-darktable-proof-of-concept/48697/1018
  user_params.auto_gamma = FALSE; // uses a pre-configured gamma

  // AgX primaries settings that produce the same matrices under D50 as those used in the Blender OCIO config
  // https://github.com/EaryChow/AgX_LUT_Gen/blob/main/AgXBaseRec2020.py
  user_params.red_inset = 0.29462451;
  user_params.green_inset = 0.25861925;
  user_params.blue_inset = 0.14641371;
  user_params.red_rotation = 0.03540329;
  user_params.green_rotation = -0.02108586;
  user_params.blue_rotation = -0.06305724;

  user_params.master_outset_ratio = 1.0f;
  user_params.master_unrotation_ratio = 1.0f;

  user_params.red_outset = 0.290776401758;
  user_params.green_outset = 0.263155400753;
  user_params.blue_outset = 0.045810721815;
  user_params.red_unrotation = 0;
  user_params.green_unrotation = 0;
  user_params.blue_unrotation = 0;

  // In Blender, a related param is set to 40%, but is actually used as 1 - param,
  // so 60% would give almost identical results; however, Eary_Chow suggested
  // that we leave this as 0, based on feedback he had received
  user_params.look_original_hue_mix_percent = 0;
  user_params.base_primaries = DT_AGX_REC2020;

  const char *workflow = dt_conf_get_string_const("plugins/darkroom/workflow");
  const gboolean auto_apply_agx = strcmp(workflow, "scene-referred (agx)") == 0;

  dt_gui_presets_add_generic(_("blender-like|base"), self->op, self->version(), &user_params, sizeof(user_params),
                             1, DEVELOP_BLEND_CS_RGB_SCENE);
  if (auto_apply_agx)
  {
    dt_gui_presets_update_format(BUILTIN_PRESET("blender-like|base"), self->op, self->version(),
                                 FOR_RAW | FOR_MATRIX);
    dt_gui_presets_update_autoapply(BUILTIN_PRESET("blender-like|base"), self->op, self->version(), TRUE);
  }

  // Punchy preset
  user_params.look_power = 1.35f;
  user_params.look_offset_percent = 0.0f;
  user_params.look_saturation_percent = 140.0f;
  dt_gui_presets_add_generic(_("blender-like|punchy"), self->op, self->version(), &user_params,
                             sizeof(user_params), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  _set_neutral_params(&user_params);
  // Sigmoid 'smooth' primaries settings
  user_params.red_inset = 0.1f;
  user_params.green_inset = 0.1f;
  user_params.blue_inset = 0.15f;
  user_params.red_rotation = deg2rad(2.f);
  user_params.green_rotation = deg2rad(-1.f);
  user_params.blue_rotation = deg2rad(-3.f);
  user_params.red_outset = 0.1f;
  user_params.green_outset = 0.1f;
  user_params.blue_outset = 0.15f;
  user_params.red_unrotation = deg2rad(2.f);
  user_params.green_unrotation = deg2rad(-1.f);
  user_params.blue_unrotation = deg2rad(-3.f);
  // Don't restore purity - try to avoid posterization.
  user_params.master_outset_ratio = 0.0f;
  user_params.master_unrotation_ratio = 1.0f;
  user_params.base_primaries = DT_AGX_WORK_PROFILE;

  dt_gui_presets_add_generic(_("smooth|base"), self->op, self->version(), &user_params, sizeof(user_params), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  // 'Punchy' look
  user_params.look_power = 1.35f;
  user_params.look_offset_percent = 0.0f;
  user_params.look_saturation_percent = 140.0f;
  dt_gui_presets_add_generic(_("smooth|punchy"), self->op, self->version(), &user_params, sizeof(user_params), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);
}

// Callback for color pickers
void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_t *pipe)
{
  const dt_iop_agx_gui_data_t *gui_data = self->gui_data;

  if (picker == gui_data->black_exposure_picker)
    apply_auto_black_exposure(self);
  else if (picker == gui_data->white_exposure_picker)
    apply_auto_white_exposure(self);
  else if (picker == gui_data->range_exposure_picker)
    apply_auto_tune_exposure(self);
  else if (picker == gui_data->basic_curve_controls_settings_page.curve_pivot_x_shift
              || (gui_data->curve_tab_enabled
                  && picker == gui_data->basic_curve_controls_curve_page.curve_pivot_x_shift))
  {
    apply_auto_pivot_x(self, dt_ioppr_get_pipe_work_profile_info(pipe));
  }

  const dt_iop_agx_user_params_t *user_params = self->params;
  if (user_params->auto_gamma)
  {
    ++darktable.gui->reset;
    tone_mapping_params_t tone_mapping_params;
    _calculate_log_mapping_params(self->params, &tone_mapping_params);
    _adjust_pivot(self->params, &tone_mapping_params);
    dt_bauhaus_slider_set(gui_data->curve_gamma, tone_mapping_params.curve_gamma);
    --darktable.gui->reset;
  }
  gtk_widget_queue_draw(GTK_WIDGET(gui_data->graph_drawing_area));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *gui_params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_agx_data_t *processing_params = piece->data;
  const dt_iop_agx_user_params_t *user_params = gui_params;

  // Calculate curve parameters once
  processing_params->tone_mapping_params = _calculate_tone_mapping_params(user_params);

  // Get profiles and create matrices
  const dt_iop_order_iccprofile_info_t *const pipe_work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  const dt_iop_order_iccprofile_info_t *const base_profile
      = _agx_get_base_profile(self->dev, pipe_work_profile, user_params->base_primaries);

  if (!base_profile)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[agx commit_params] Failed to obtain a valid base profile. Module will not run correctly.");
    return;
  }

  _create_matrices(user_params, pipe_work_profile, base_profile,
                   processing_params->rendering_profile.matrix_in_transposed,
                   processing_params->pipe_to_base_transposed, processing_params->base_to_rendering_transposed,
                   processing_params->rendering_to_pipe_transposed);

  dt_colormatrix_transpose(processing_params->rendering_profile.matrix_in,
                           processing_params->rendering_profile.matrix_in_transposed);
  processing_params->rendering_profile.nonlinearlut = FALSE; // no LUT for this linear transform
}
