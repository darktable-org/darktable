/*
    This file is part of darktable,
    Copyright (C) 2020-2024 darktable developers.

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
#include "common/custom_primaries.h"
#include "common/math.h"
#include "common/matrices.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/openmp_maths.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(3, dt_iop_sigmoid_params_t)


#define MIDDLE_GREY 0.1845f


typedef enum dt_iop_sigmoid_methods_type_t
{
  DT_SIGMOID_METHOD_PER_CHANNEL = 0, // $DESCRIPTION: "per channel"
  DT_SIGMOID_METHOD_RGB_RATIO = 1,   // $DESCRIPTION: "RGB ratio"
} dt_iop_sigmoid_methods_type_t;


typedef enum dt_iop_sigmoid_base_primaries_t
{
  DT_SIGMOID_WORK_PROFILE = 0, // $DESCRIPTION: "working profile"
  DT_SIGMOID_REC2020 = 1, // $DESCRIPTION: "Rec2020"
  DT_SIGMOID_DISPLAY_P3 = 2, // $DESCRIPTION: "Display P3"
  DT_SIGMOID_ADOBE_RGB = 3, // $DESCRIPTION: "Adobe RGB (compatible)"
  DT_SIGMOID_SRGB = 4, // $DESCRIPTION: "sRGB"
} dt_iop_sigmoid_base_primaries_t;


typedef struct dt_iop_sigmoid_params_t
{
  float middle_grey_contrast; // $MIN: 0.1  $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "contrast"
  float contrast_skewness;    // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "skew"
  float display_white_target; // $MIN: 20.0  $MAX: 1600.0 $DEFAULT: 100.0 $DESCRIPTION: "target white"
  float display_black_target; // $MIN: 0.0  $MAX: 15.0 $DEFAULT: 0.0152 $DESCRIPTION: "target black"
  dt_iop_sigmoid_methods_type_t color_processing; // $DEFAULT: DT_SIGMOID_METHOD_PER_CHANNEL $DESCRIPTION: "color processing"
  float hue_preservation; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "preserve hue"
  float red_inset;        // $MIN:  0.0  $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "red attenuation"
  float red_rotation;     // $MIN: -0.4  $MAX: 0.4  $DEFAULT: 0.0 $DESCRIPTION: "red rotation"
  float green_inset;      // $MIN:  0.0  $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "green attenuation"
  float green_rotation;   // $MIN: -0.4  $MAX: 0.4  $DEFAULT: 0.0 $DESCRIPTION: "green rotation"
  float blue_inset;       // $MIN:  0.0  $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "blue attenuation"
  float blue_rotation;    // $MIN: -0.4  $MAX: 0.4  $DEFAULT: 0.0 $DESCRIPTION: "blue rotation"
  float purity;           // $MIN:  0.0  $MAX: 1.0  $DEFAULT: 0.0 $DESCRIPTION: "recover purity"
  dt_iop_sigmoid_base_primaries_t base_primaries; // $DEFAULT: DT_SIGMOID_WORK_PROFILE $DESCRIPTION: "base primaries"
} dt_iop_sigmoid_params_t;

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_sigmoid_params_v2_t
  {
    float middle_grey_contrast;
    float contrast_skewness;
    float display_white_target;
    float display_black_target;
    dt_iop_sigmoid_methods_type_t color_processing;
    float hue_preservation;

    /* v2 params */
    float red_inset;
    float red_rotation;
    float green_inset;
    float green_rotation;
    float blue_inset;
    float blue_rotation;
    float purity;

    /* v3 params */
    dt_iop_sigmoid_base_primaries_t base_primaries;
  } dt_iop_sigmoid_params_v3_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_sigmoid_params_v1_t
    {
      float middle_grey_contrast;
      float contrast_skewness;
      float display_white_target;
      float display_black_target;
      dt_iop_sigmoid_methods_type_t color_processing;
      float hue_preservation;
    } dt_iop_sigmoid_params_v1_t;

    // Copy the common part of the params struct
    dt_iop_sigmoid_params_v3_t *n = calloc(1, sizeof(dt_iop_sigmoid_params_v3_t));
    memcpy(n, old_params, sizeof(dt_iop_sigmoid_params_v1_t));

    *new_params = n;
    *new_params_size = sizeof(dt_iop_sigmoid_params_v3_t);
    *new_version = 3;

    return 0;
  }
  else if(old_version == 2)
  {
    typedef struct dt_iop_sigmoid_params_v2_t
    {
      float middle_grey_contrast;
      float contrast_skewness;
      float display_white_target;
      float display_black_target;
      dt_iop_sigmoid_methods_type_t color_processing;
      float hue_preservation;

      /* v2 params */
      float red_inset;
      float red_rotation;
      float green_inset;
      float green_rotation;
      float blue_inset;
      float blue_rotation;
      float purity;
    } dt_iop_sigmoid_params_v2_t;
    // Copy the common part of the params struct
    dt_iop_sigmoid_params_v3_t *n = calloc(1, sizeof(dt_iop_sigmoid_params_v3_t));
    memcpy(n, old_params, sizeof(dt_iop_sigmoid_params_v2_t));

    *new_params = n;
    *new_params_size = sizeof(dt_iop_sigmoid_params_v3_t);
    *new_version = 3;

    return 0;
  }

  return 1;
}

typedef struct dt_iop_sigmoid_data_t
{
  float white_target;
  float black_target;
  float paper_exposure;
  float film_fog;
  float film_power;
  float paper_power;
  dt_iop_sigmoid_methods_type_t color_processing;
  float hue_preservation;
  float inset[3];
  float rotation[3];
  float purity;
  dt_iop_sigmoid_base_primaries_t base_primaries;
} dt_iop_sigmoid_data_t;

typedef struct dt_iop_sigmoid_gui_data_t
{
  GtkWidget *color_processing_list, *hue_preservation_slider;

  dt_gui_collapsible_section_t display_luminance_section, primaries_section;
} dt_iop_sigmoid_gui_data_t;

typedef struct dt_iop_sigmoid_global_data_t
{
  int kernel_sigmoid_loglogistic_per_channel;
  int kernel_sigmoid_loglogistic_rgb_ratio;
} dt_iop_sigmoid_global_data_t;


const char *name()
{
  return _("sigmoid");
}

const char *aliases()
{
  return _("tone mapping|view transform|display transform");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("apply a view transform to make a image displayable\n"
                                  "on a screen or print. uses a robust and smooth\n"
                                  "tone curve with optional color preservation methods."),
                                _("corrective and creative"), _("linear, RGB, scene-referred"),
                                _("non-linear, RGB"), _("linear, RGB, display-referred"));
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

void init_presets(dt_iop_module_so_t *self)
{
  // auto-applied scene-referred default
  self->pref_based_presets = TRUE;

  const char *workflow = dt_conf_get_string_const("plugins/darkroom/workflow");
  const gboolean auto_apply_sigmoid = strcmp(workflow, "scene-referred (sigmoid)") == 0;

  if(auto_apply_sigmoid)
  {
    dt_gui_presets_add_generic(_("scene-referred default"),
                               self->op, self->version(), NULL, 0, 1,
                               DEVELOP_BLEND_CS_RGB_SCENE);

    dt_gui_presets_update_format(_("scene-referred default"),
                                 self->op, self->version(), FOR_RAW | FOR_MATRIX);

    dt_gui_presets_update_autoapply(_("scene-referred default"),
                                    self->op, self->version(), TRUE);
  }

  // others

  dt_iop_sigmoid_params_t p = { 0 };
  p.display_white_target = 100.0f;
  p.display_black_target = 0.0152f;
  p.color_processing = DT_SIGMOID_METHOD_PER_CHANNEL;

  p.middle_grey_contrast = 1.22f;
  p.contrast_skewness = 0.65f;
  p.hue_preservation = 100.0f;
  dt_gui_presets_add_generic(_("neutral gray"), self->op,
                             self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.middle_grey_contrast = 1.6f;
  p.contrast_skewness = -0.2f;
  p.hue_preservation = 0.0f;
  dt_gui_presets_add_generic(_("ACES 100-nit like"), self->op,
                             self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.middle_grey_contrast = 1.0f;
  p.contrast_skewness = 0.0f;
  p.color_processing = DT_SIGMOID_METHOD_RGB_RATIO;
  dt_gui_presets_add_generic(_("Reinhard"), self->op,
                             self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  const float DEG_TO_RAD = M_PI_F / 180.f;

  // smooth - a preset that utilizes the primaries feature
  p.middle_grey_contrast = 1.5f;
  // Allow a little bit more room for the highlights
  p.contrast_skewness = -0.2f;
  p.color_processing = DT_SIGMOID_METHOD_PER_CHANNEL;
  // Allow shifts of the chromaticity. This will work well for sunsets etc.
  p.hue_preservation = 0.0f;
  p.red_inset = 0.1f;
  p.green_inset = 0.1f;
  p.blue_inset = 0.15f;
  p.red_rotation = 2.f * DEG_TO_RAD;
  p.green_rotation = -1.f * DEG_TO_RAD;
  p.blue_rotation = -3.f * DEG_TO_RAD;
  // Don't restore purity - try to avoid posterization.
  p.purity = 0.f;
  // Constant base primaries (not dependent on work profile) to
  // maintain a consistent behavior
  p.base_primaries = DT_SIGMOID_REC2020;
  dt_gui_presets_add_generic(_("smooth"), self->op, self->version(),
                             &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
}

// Declared here as it is used in the commit params function
DT_OMP_DECLARE_SIMD(uniform(magnitude, paper_exp, film_fog, film_power, paper_power))
static inline float _generalized_loglogistic_sigmoid(const float value,
                                                     const float magnitude,
                                                     const float paper_exp,
                                                     const float film_fog,
                                                     const float film_power,
                                                     const float paper_power)
{
  const float clamped_value = fmaxf(value, 0.0f);
  // The following equation can be derived as a model for film + paper but it has a pole at 0
  // magnitude * powf(1.0f + paper_exp * powf(film_fog + value, -film_power), -paper_power);
  // Rewritten on a stable around zero form:
  const float film_response = powf(film_fog + clamped_value, film_power);
  const float paper_response = magnitude * powf(film_response / (paper_exp + film_response), paper_power);

  // Safety check for very large floats that cause numerical errors
  return dt_isnan(paper_response) ? magnitude : paper_response;
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_sigmoid_params_t *params = (dt_iop_sigmoid_params_t *)p1;
  dt_iop_sigmoid_data_t *module_data = piece->data;
  /* Calculate actual skew log logistic parameters to fulfill the following:
   * f(scene_zero) = display_black_target
   * f(scene_grey) = MIDDLE_GREY
   * f(scene_inf)  = display_white_target
   * Slope at scene_grey independent of skewness i.e. only changed by the contrast parameter.
   */

  // Calculate a reference slope for no skew and a normalized display
  const float ref_film_power = params->middle_grey_contrast;
  const float ref_paper_power = 1.0f;
  const float ref_magnitude = 1.0f;
  const float ref_film_fog = 0.0f;
  const float ref_paper_exposure
      = powf(ref_film_fog + MIDDLE_GREY, ref_film_power) * ((ref_magnitude / MIDDLE_GREY) - 1.0f);
  const float delta = 1e-6f;
  const float ref_slope
      = (_generalized_loglogistic_sigmoid(MIDDLE_GREY + delta, ref_magnitude, ref_paper_exposure, ref_film_fog,
                                          ref_film_power, ref_paper_power)
         - _generalized_loglogistic_sigmoid(MIDDLE_GREY - delta, ref_magnitude, ref_paper_exposure, ref_film_fog,
                                            ref_film_power, ref_paper_power))
        / 2.0f / delta;

  // Add skew
  module_data->paper_power = powf(5.0f, -params->contrast_skewness);

  // Slope at low film power
  const float temp_film_power = 1.0f;
  const float temp_white_target = 0.01f * params->display_white_target;
  const float temp_white_grey_relation
      = powf(temp_white_target / MIDDLE_GREY, 1.0f / module_data->paper_power) - 1.0f;
  const float temp_paper_exposure = powf(MIDDLE_GREY, temp_film_power) * temp_white_grey_relation;
  const float temp_slope
      = (_generalized_loglogistic_sigmoid(MIDDLE_GREY + delta, temp_white_target, temp_paper_exposure,
                                          ref_film_fog, temp_film_power, module_data->paper_power)
         - _generalized_loglogistic_sigmoid(MIDDLE_GREY - delta, temp_white_target, temp_paper_exposure,
                                            ref_film_fog, temp_film_power, module_data->paper_power))
        / 2.0f / delta;

  // Figure out what film power fulfills the target slope
  // (linear when assuming display_black = 0.0)
  module_data->film_power = ref_slope / temp_slope;

  // Calculate the other parameters now that both film and paper power is known
  module_data->white_target = 0.01f * params->display_white_target;
  module_data->black_target = 0.01f * params->display_black_target;
  const float white_grey_relation
      = powf(module_data->white_target / MIDDLE_GREY, 1.0f / module_data->paper_power) - 1.0f;
  const float white_black_relation
      = powf(module_data->black_target / module_data->white_target, -1.0f / module_data->paper_power) - 1.0f;

  module_data->film_fog = MIDDLE_GREY * powf(white_grey_relation, 1.0f / module_data->film_power)
                          / (powf(white_black_relation, 1.0f / module_data->film_power)
                             - powf(white_grey_relation, 1.0f / module_data->film_power));
  module_data->paper_exposure
      = powf(module_data->film_fog + MIDDLE_GREY, module_data->film_power) * white_grey_relation;

  module_data->color_processing = params->color_processing;
  module_data->hue_preservation = fminf(fmaxf(0.01f * params->hue_preservation, 0.0f), 1.0f);

  module_data->purity = params->purity;
  module_data->inset[0] = params->red_inset;
  module_data->inset[1] = params->green_inset;
  module_data->inset[2] = params->blue_inset;
  module_data->rotation[0] = params->red_rotation;
  module_data->rotation[1] = params->green_rotation;
  module_data->rotation[2] = params->blue_rotation;
  module_data->base_primaries = params->base_primaries;
}

static void _calculate_adjusted_primaries(const dt_iop_sigmoid_data_t *const module_data,
                                          const dt_iop_order_iccprofile_info_t *const pipe_work_profile,
                                          const dt_iop_order_iccprofile_info_t *const base_profile,
                                          dt_colormatrix_t pipe_to_base,
                                          dt_colormatrix_t base_to_rendering,
                                          dt_colormatrix_t rendering_to_pipe)
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

  // First, calculate matrix to get from pipe work profile to "base primaries".
  dt_colormatrix_t base_to_pipe;
  if(pipe_work_profile != base_profile)
  {
    dt_colormatrix_mul(pipe_to_base, pipe_work_profile->matrix_in_transposed,
                       base_profile->matrix_out_transposed);
    mat3SSEinv(base_to_pipe, pipe_to_base);
  }
  else
  {
    // Special case: if pipe and base profile are the same, pipe_to_base is an identity matrix.
    for(size_t i = 0; i < 4; i++)
    {
      for(size_t j = 0; j < 4; j++)
      {
        if(i == j && i < 3)
        {
          pipe_to_base[i][j] = 1.f;
          base_to_pipe[i][j] = 1.f;
        }
        else
        {
          pipe_to_base[i][j] = 0.f;
          base_to_pipe[i][j] = 0.f;
        }
      }
    }
  }

  // Rotated, scaled primaries are calculated based on the "base profile"
  float custom_primaries[3][2];
  for(size_t i = 0; i < 3; i++)
    dt_rotate_and_scale_primary(base_profile, 1.f - module_data->inset[i], module_data->rotation[i], i,
                                custom_primaries[i]);

  dt_colormatrix_t custom_to_XYZ;
  dt_make_transposed_matrices_from_primaries_and_whitepoint(custom_primaries, base_profile->whitepoint,
                                                            custom_to_XYZ);
  dt_colormatrix_mul(base_to_rendering, custom_to_XYZ, base_profile->matrix_out_transposed);

  for(size_t i = 0; i < 3; i++)
  {
    const float scaling = 1.f - module_data->purity * module_data->inset[i];
    dt_rotate_and_scale_primary(base_profile, scaling, module_data->rotation[i], i, custom_primaries[i]);
  }

  dt_make_transposed_matrices_from_primaries_and_whitepoint(custom_primaries, base_profile->whitepoint,
                                                            custom_to_XYZ);
  dt_colormatrix_t tmp;
  dt_colormatrix_mul(tmp, custom_to_XYZ, base_profile->matrix_out_transposed);
  dt_colormatrix_t rendering_to_base;
  mat3SSEinv(rendering_to_base, tmp);
  dt_colormatrix_mul(rendering_to_pipe, rendering_to_base, base_to_pipe);
}

static dt_colorspaces_color_profile_type_t _get_base_profile_type(const dt_iop_sigmoid_base_primaries_t base_primaries)
{
  if(base_primaries == DT_SIGMOID_SRGB)
    return DT_COLORSPACE_SRGB;

  if(base_primaries == DT_SIGMOID_DISPLAY_P3)
    return DT_COLORSPACE_DISPLAY_P3;

  if(base_primaries == DT_SIGMOID_ADOBE_RGB)
    return DT_COLORSPACE_ADOBERGB;

  return DT_COLORSPACE_LIN_REC2020;
}

static const dt_iop_order_iccprofile_info_t * _get_base_profile(dt_develop_t *dev,
                                                                const dt_iop_order_iccprofile_info_t *pipe_work_profile,
                                                                const dt_iop_sigmoid_base_primaries_t base_primaries)
{
  if(base_primaries == DT_SIGMOID_WORK_PROFILE)
    return pipe_work_profile;

  return dt_ioppr_add_profile_info_to_list(dev, _get_base_profile_type(base_primaries), "", DT_INTENT_RELATIVE_COLORIMETRIC);
}

DT_OMP_DECLARE_SIMD()
static inline void _desaturate_negative_values(const dt_aligned_pixel_t pix_in, dt_aligned_pixel_t pix_out)
{
  const float pixel_average = fmaxf((pix_in[0] + pix_in[1] + pix_in[2]) / 3.0f, 0.0f);
  const float min_value = fminf(fminf(pix_in[0], pix_in[1]), pix_in[2]);
  const float saturation_factor = min_value < 0.0f ? -pixel_average / (min_value - pixel_average) : 1.0f;
  for_each_channel(c, aligned(pix_in, pix_out))
  {
    pix_out[c] = pixel_average + saturation_factor * (pix_in[c] - pixel_average);
  }
}

typedef struct dt_iop_sigmoid_value_order_t
{
  size_t min;
  size_t mid;
  size_t max;
} dt_iop_sigmoid_value_order_t;

static void _pixel_channel_order(const dt_aligned_pixel_t pix_in, dt_iop_sigmoid_value_order_t *pixel_value_order)
{
  if(pix_in[0] >= pix_in[1])
  {
    if(pix_in[1] > pix_in[2])
    { // Case 1: r >= g >  b
      pixel_value_order->max = 0;
      pixel_value_order->mid = 1;
      pixel_value_order->min = 2;
    }
    else if(pix_in[2] > pix_in[0])
    { // Case 2: b >  r >= g
      pixel_value_order->max = 2;
      pixel_value_order->mid = 0;
      pixel_value_order->min = 1;
    }
    else if(pix_in[2] > pix_in[1])
    { // Case 3: r >= b >  g
      pixel_value_order->max = 0;
      pixel_value_order->mid = 2;
      pixel_value_order->min = 1;
    }
    else
    { // Case 4: r == g == b
      // No change of the middle value, just assign something.
      pixel_value_order->max = 0;
      pixel_value_order->mid = 1;
      pixel_value_order->min = 2;
    }
  }
  else
  {
    if(pix_in[0] >= pix_in[2])
    { // Case 5: g >  r >= b
      pixel_value_order->max = 1;
      pixel_value_order->mid = 0;
      pixel_value_order->min = 2;
    }
    else if(pix_in[2] > pix_in[1])
    { // Case 6: b >  g >  r
      pixel_value_order->max = 2;
      pixel_value_order->mid = 1;
      pixel_value_order->min = 0;
    }
    else
    { // Case 7: g >= b >  r
      pixel_value_order->max = 1;
      pixel_value_order->mid = 2;
      pixel_value_order->min = 0;
    }
  }
}

void process_loglogistic_rgb_ratio(dt_dev_pixelpipe_iop_t *piece,
                                   const void *const ivoid,
                                   void *const ovoid,
                                   const dt_iop_roi_t *const roi_in,
                                   const dt_iop_roi_t *const roi_out)
{
  const dt_iop_sigmoid_data_t *module_data = piece->data;
  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;
  const size_t npixels = (size_t)roi_in->width * roi_in->height;

  const float white_target = module_data->white_target;
  const float black_target = module_data->black_target;
  const float paper_exp = module_data->paper_exposure;
  const float film_fog = module_data->film_fog;
  const float contrast_power = module_data->film_power;
  const float skew_power = module_data->paper_power;

  DT_OMP_FOR()
  for(size_t k = 0; k < 4 * npixels; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    dt_aligned_pixel_t pre_out;
    dt_aligned_pixel_t pix_in_strict_positive;

    // Force negative values to zero
    _desaturate_negative_values(pix_in, pix_in_strict_positive);

    // Preserve color ratios by applying the tone curve on a luma estimate and then scale the RGB tripplet uniformly
    const float luma = (pix_in_strict_positive[0] + pix_in_strict_positive[1] + pix_in_strict_positive[2]) / 3.0f;
    const float mapped_luma
        = _generalized_loglogistic_sigmoid(luma, white_target, paper_exp, film_fog, contrast_power, skew_power);

    if(luma > 1e-9)
    {
      const float scaling_factor = mapped_luma / luma;
      for_each_channel(c, aligned(pix_in_strict_positive, pix_out))
      {
        pre_out[c] = scaling_factor * pix_in_strict_positive[c];
      }
    }
    else
    {
      for_each_channel(c, aligned(pix_in_strict_positive, pix_out))
      {
        pre_out[c] = mapped_luma;
      }
    }

    // RGB index order sorted by value;
    dt_iop_sigmoid_value_order_t pixel_value_order;
    _pixel_channel_order(pre_out, &pixel_value_order);
    const float pixel_min = pre_out[pixel_value_order.min];
    const float pixel_max = pre_out[pixel_value_order.max];

    // Chroma relative display gamut and scene "mapping" gamut.
    const float epsilon = 1e-6;
    const float display_border_vs_chroma_white
        = (white_target - mapped_luma)
          / (pixel_max - mapped_luma + epsilon); // "Distance" to max channel = white_target
    const float display_border_vs_chroma_black
        = (black_target - mapped_luma)
          / (pixel_min - mapped_luma - epsilon); // "Distance" to min_channel = black_target
    const float display_border_vs_chroma = fminf(display_border_vs_chroma_white, display_border_vs_chroma_black);
    const float chroma_vs_mapping_border
        = (mapped_luma - pixel_min) / (mapped_luma + epsilon); // "Distance" to min channel = 0.0

    // Hyperbolic gamut compression
    // Small chroma values, i.e., colors close to the acromatic axis are preserved while large chroma values are
    // compressed.

    const float pixel_chroma_adjustment = 1.0f / (chroma_vs_mapping_border * display_border_vs_chroma + epsilon);
    const float hyperbolic_chroma = 2.0f * chroma_vs_mapping_border
                                    / (1.0f - chroma_vs_mapping_border * chroma_vs_mapping_border + epsilon)
                                    * pixel_chroma_adjustment;

    const float hyperbolic_z = sqrtf(hyperbolic_chroma * hyperbolic_chroma + 1.0f);
    const float chroma_factor = hyperbolic_chroma / (1.0f + hyperbolic_z) * display_border_vs_chroma;

    for_each_channel(c, aligned(pre_out, pix_out))
    {
      pix_out[c] = mapped_luma + chroma_factor * (pre_out[c] - mapped_luma);
    }

    // Copy over the alpha channel
    pix_out[3] = pix_in[3];
  }
}

// Linear interpolation of hue that also preserve sum of channels
// Assumes hue_preservation strictly in range [0, 1]
static inline void _preserve_hue_and_energy(const dt_aligned_pixel_t pix_in,
                                            const dt_aligned_pixel_t per_channel,
                                            dt_aligned_pixel_t pix_out,
                                            const dt_iop_sigmoid_value_order_t order,
                                            const float hue_preservation)
{
  // Naive Hue correction of the middle channel
  const float chroma = pix_in[order.max] - pix_in[order.min];
  const float midscale = chroma != 0.f ? (pix_in[order.mid] - pix_in[order.min]) / chroma : 0.f;
  const float full_hue_correction
      = per_channel[order.min] + (per_channel[order.max] - per_channel[order.min]) * midscale;
  const float naive_hue_mid
      = (1.0f - hue_preservation) * per_channel[order.mid] + hue_preservation * full_hue_correction;

  const float per_channel_energy = per_channel[0] + per_channel[1] + per_channel[2];
  const float naive_hue_energy = per_channel[order.min] + naive_hue_mid + per_channel[order.max];
  const float pix_in_min_plus_mid = pix_in[order.min] + pix_in[order.mid];
  const float blend_factor = pix_in_min_plus_mid != 0.f ? 2.0f * pix_in[order.min] / pix_in_min_plus_mid : 0.f;
  const float energy_target = blend_factor * per_channel_energy + (1.0f - blend_factor) * naive_hue_energy;

  // Preserve hue constrained to maintain the same energy as the per channel result
  if(naive_hue_mid <= per_channel[order.mid])
  {
    const float corrected_mid = ((1.0f - hue_preservation) * per_channel[order.mid]
                                 + hue_preservation
                                       * (midscale * per_channel[order.max]
                                          + (1.0f - midscale) * (energy_target - per_channel[order.max])))
                                / (1.0f + hue_preservation * (1.0f - midscale));
    pix_out[order.min] = energy_target - per_channel[order.max] - corrected_mid;
    pix_out[order.mid] = corrected_mid;
    pix_out[order.max] = per_channel[order.max];
  }
  else
  {
    const float corrected_mid = ((1.0f - hue_preservation) * per_channel[order.mid]
                                 + hue_preservation
                                       * (per_channel[order.min] * (1.0f - midscale)
                                          + midscale * (energy_target - per_channel[order.min])))
                                / (1.0f + hue_preservation * midscale);
    pix_out[order.min] = per_channel[order.min];
    pix_out[order.mid] = corrected_mid;
    pix_out[order.max] = energy_target - per_channel[order.min] - corrected_mid;
  }
}

void process_loglogistic_per_channel(dt_develop_t *dev,
                                     dt_dev_pixelpipe_iop_t *piece,
                                     const void *const ivoid, void *const ovoid,
                                     const dt_iop_roi_t *const roi_in,
                                     const dt_iop_roi_t *const roi_out)
{
  const dt_iop_sigmoid_data_t *module_data = piece->data;

  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;
  const size_t npixels = (size_t)roi_in->width * roi_in->height;

  const float white_target = module_data->white_target;
  const float paper_exp = module_data->paper_exposure;
  const float film_fog = module_data->film_fog;
  const float contrast_power = module_data->film_power;
  const float skew_power = module_data->paper_power;
  const float hue_preservation = module_data->hue_preservation;

  const dt_iop_order_iccprofile_info_t *pipe_work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  const dt_iop_order_iccprofile_info_t *base_profile = _get_base_profile(dev, pipe_work_profile, module_data->base_primaries);
  dt_colormatrix_t pipe_to_base, base_to_rendering, rendering_to_pipe;
  _calculate_adjusted_primaries(module_data, pipe_work_profile, base_profile, pipe_to_base, base_to_rendering, rendering_to_pipe);

  DT_OMP_FOR()
  for(size_t k = 0; k < 4 * npixels; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    dt_aligned_pixel_t pix_in_base, pix_in_strict_positive;
    dt_aligned_pixel_t per_channel;

    // Convert to "base primaries"
    dt_apply_transposed_color_matrix(pix_in, pipe_to_base, pix_in_base);

    // Force negative values to zero
    _desaturate_negative_values(pix_in_base, pix_in_strict_positive);

    dt_aligned_pixel_t rendering_RGB;
    dt_apply_transposed_color_matrix(pix_in_strict_positive, base_to_rendering, rendering_RGB);

    for_each_channel(c, aligned(rendering_RGB, per_channel))
    {
      per_channel[c] = _generalized_loglogistic_sigmoid(rendering_RGB[c], white_target, paper_exp, film_fog,
                                                        contrast_power, skew_power);
    }

    // Hue correction by scaling the middle value relative to the max and min values.
    dt_iop_sigmoid_value_order_t pixel_value_order;
    dt_aligned_pixel_t per_channel_hue_corrected;
    _pixel_channel_order(rendering_RGB, &pixel_value_order);
    _preserve_hue_and_energy(rendering_RGB, per_channel, per_channel_hue_corrected, pixel_value_order,
                             hue_preservation);
    dt_apply_transposed_color_matrix(per_channel_hue_corrected, rendering_to_pipe, pix_out);

    // Copy over the alpha channel
    pix_out[3] = pix_in[3];
  }
}

/** process, all real work is done here. */
void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  dt_iop_sigmoid_data_t *module_data = piece->data;

  if(module_data->color_processing == DT_SIGMOID_METHOD_PER_CHANNEL)
  {
    process_loglogistic_per_channel(self->dev, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else // DT_SIGMOID_METHOD_RGB_RATIO
  {
    process_loglogistic_rgb_ratio(piece, ivoid, ovoid, roi_in, roi_out);
  }
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_sigmoid_data_t *const d = piece->data;
  dt_iop_sigmoid_global_data_t *const gd = self->global_data;

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float white_target = d->white_target;
  const float paper_exp = d->paper_exposure;
  const float film_fog = d->film_fog;
  const float contrast_power = d->film_power;
  const float skew_power = d->paper_power;

  const dt_iop_order_iccprofile_info_t *pipe_work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  const dt_iop_order_iccprofile_info_t *base_profile = _get_base_profile(self->dev, pipe_work_profile, d->base_primaries);
  dt_colormatrix_t pipe_to_base_transposed, base_to_rendering_transposed,
      rendering_to_pipe_transposed, pipe_to_base, base_to_rendering, rendering_to_pipe;
  _calculate_adjusted_primaries(d, pipe_work_profile, base_profile, pipe_to_base_transposed, base_to_rendering_transposed, rendering_to_pipe_transposed);
  transpose_3xSSE(pipe_to_base_transposed, pipe_to_base);
  transpose_3xSSE(base_to_rendering_transposed, base_to_rendering);
  transpose_3xSSE(rendering_to_pipe_transposed, rendering_to_pipe);
  cl_mem dev_pipe_to_base
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(pipe_to_base), pipe_to_base);
  cl_mem dev_base_to_rendering
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(base_to_rendering), base_to_rendering);
  cl_mem dev_rendering_to_pipe
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(rendering_to_pipe), rendering_to_pipe);
  if(dev_pipe_to_base == NULL || dev_base_to_rendering == NULL || dev_rendering_to_pipe == NULL)
    goto cleanup;

  if(d->color_processing == DT_SIGMOID_METHOD_PER_CHANNEL)
  {
    const float hue_preservation = d->hue_preservation;
    err = dt_opencl_enqueue_kernel_2d_args(
        devid, gd->kernel_sigmoid_loglogistic_per_channel, width, height, CLARG(dev_in), CLARG(dev_out),
        CLARG(width), CLARG(height), CLARG(white_target), CLARG(paper_exp), CLARG(film_fog), CLARG(contrast_power),
        CLARG(skew_power), CLARG(hue_preservation), CLARG(dev_pipe_to_base), CLARG(dev_base_to_rendering), CLARG(dev_rendering_to_pipe));
  }
  else
  {
    const float black_target = d->black_target;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_sigmoid_loglogistic_rgb_ratio, width, height,
                                           CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height),
                                           CLARG(white_target), CLARG(black_target), CLARG(paper_exp),
                                           CLARG(film_fog), CLARG(contrast_power), CLARG(skew_power));
  }

cleanup:
  dt_opencl_release_mem_object(dev_pipe_to_base);
  dt_opencl_release_mem_object(dev_base_to_rendering);
  dt_opencl_release_mem_object(dev_rendering_to_pipe);
  return err;
}
#endif // HAVE_OPENCL

void init_global(dt_iop_module_so_t *self)
{
  const int program = 36; // sigmoid.cl, from programs.conf
  dt_iop_sigmoid_global_data_t *gd = malloc(sizeof(dt_iop_sigmoid_global_data_t));

  self->data = gd;
  gd->kernel_sigmoid_loglogistic_per_channel = dt_opencl_create_kernel(program, "sigmoid_loglogistic_per_channel");
  gd->kernel_sigmoid_loglogistic_rgb_ratio = dt_opencl_create_kernel(program, "sigmoid_loglogistic_rgb_ratio");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_sigmoid_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_sigmoid_loglogistic_per_channel);
  dt_opencl_free_kernel(gd->kernel_sigmoid_loglogistic_rgb_ratio);
  free(self->data);
  self->data = NULL;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_sigmoid_data_t));
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_sigmoid_gui_data_t *g = self->gui_data;
  dt_iop_sigmoid_params_t *p = self->params;

  if(!w || w == g->color_processing_list)
  {
    const gboolean is_per_channel = p->color_processing == DT_SIGMOID_METHOD_PER_CHANNEL;
    gtk_widget_set_visible(g->hue_preservation_slider, is_per_channel);
    gtk_widget_set_visible(g->primaries_section.expander, is_per_channel);
  }
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_sigmoid_gui_data_t *g = self->gui_data;

  dt_gui_update_collapsible_section(&g->display_luminance_section);
  dt_gui_update_collapsible_section(&g->primaries_section);

  gui_changed(self, NULL, NULL);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_sigmoid_gui_data_t *g = IOP_GUI_ALLOC(sigmoid);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // Look controls
  GtkWidget *slider = dt_bauhaus_slider_from_params(self, "middle_grey_contrast");
  dt_bauhaus_slider_set_soft_range(slider, 0.7f, 3.0f);
  dt_bauhaus_slider_set_digits(slider, 3);
  gtk_widget_set_tooltip_text(slider, _("compression of the applied curve\n"
                                        "implicitly defines the supported input dynamic range"));
  slider = dt_bauhaus_slider_from_params(self, "contrast_skewness");
  gtk_widget_set_tooltip_text(slider, _("shift the compression towards shadows or highlights.\n"
                                        "negative values increase contrast in shadows.\n"
                                        "positive values increase contrast in highlights.\n"
                                        "the opposite end will see a reduction in contrast."));

  // Color handling
  g->color_processing_list = dt_bauhaus_combobox_from_params(self, "color_processing");
  g->hue_preservation_slider = dt_bauhaus_slider_from_params(self, "hue_preservation");
  dt_bauhaus_slider_set_format(g->hue_preservation_slider, "%");
  gtk_widget_set_tooltip_text(g->hue_preservation_slider, _("optional correction of the hue twist introduced by\n"
                                                            "the per-channel processing method."));

  GtkWidget *main_box = self->widget;

  // primaries collapsible section
  dt_gui_new_collapsible_section(&g->primaries_section, "plugins/darkroom/sigmoid/expand_primaries",
                                 _("primaries"), GTK_BOX(main_box), DT_ACTION(self));
  gtk_widget_set_tooltip_text(g->primaries_section.expander, _("set custom primaries"));

  self->widget = GTK_WIDGET(g->primaries_section.container);
  dt_iop_module_t *sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("primaries"));

  GtkWidget *base_primaries = dt_bauhaus_combobox_from_params(self, "base_primaries");
  gtk_widget_set_tooltip_text(base_primaries, _("primaries to use as the base for below adjustments\n"
                                                "'working profile' uses the profile set in 'input color profile'"));

#define SETUP_COLOR_COMBO(color, r, g, b, inset_tooltip, rotation_tooltip)                                        \
  slider = dt_bauhaus_slider_from_params(sect, #color "_inset");                                                  \
  dt_bauhaus_slider_set_format(slider, "%");                                                                      \
  dt_bauhaus_slider_set_digits(slider, 1);                                                                        \
  dt_bauhaus_slider_set_factor(slider, 100.f);                                                                    \
  dt_bauhaus_slider_set_soft_range(slider, 0.f, 0.5f);                                                            \
  dt_bauhaus_slider_set_stop(slider, 0.f, r, g, b);                                                               \
  gtk_widget_set_tooltip_text(slider, inset_tooltip);                                                             \
                                                                                                                  \
  slider = dt_bauhaus_slider_from_params(sect, #color "_rotation");                                               \
  dt_bauhaus_slider_set_format(slider, "°");                                                                      \
  dt_bauhaus_slider_set_digits(slider, 1);                                                                        \
  dt_bauhaus_slider_set_factor(slider, 180.f / M_PI_F);                                                        \
  dt_bauhaus_slider_set_stop(slider, 0.f, r, g, b);                                                               \
  gtk_widget_set_tooltip_text(slider, rotation_tooltip);

  const float desaturation = 0.2f;
  SETUP_COLOR_COMBO(red, 1.f - desaturation, desaturation, desaturation, _("attenuate the purity of the red primary"),
                    _("rotate the red primary"));
  SETUP_COLOR_COMBO(green, desaturation, 1.f - desaturation, desaturation, _("attenuate the purity of the green primary"),
                    _("rotate the green primary"));
  SETUP_COLOR_COMBO(blue, desaturation, desaturation, 1.f - desaturation, _("attenuate the purity of the blue primary"),
                    _("rotate the blue primary"));
#undef SETUP_COLOR_COMBO

  slider = dt_bauhaus_slider_from_params(sect, "purity");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 0);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider, _("recover some of the original purity after the inset"));

  // display luminance section
  dt_gui_new_collapsible_section(&g->display_luminance_section, "plugins/darkroom/sigmoid/expand_values",
                                 _("display luminance"), GTK_BOX(main_box), DT_ACTION(self));
  gtk_widget_set_tooltip_text(g->display_luminance_section.expander, _("set display black/white targets"));

  self->widget = GTK_WIDGET(g->display_luminance_section.container);

  slider = dt_bauhaus_slider_from_params(self, "display_black_target");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
  dt_bauhaus_slider_set_digits(slider, 4);
  dt_bauhaus_slider_set_format(slider, "%");
  gtk_widget_set_tooltip_text(slider, _("the black luminance of the target display or print.\n"
                                        "can be used creatively for a faded look."));
  slider = dt_bauhaus_slider_from_params(self, "display_white_target");
  dt_bauhaus_slider_set_soft_range(slider, 50.0f, 100.0f);
  dt_bauhaus_slider_set_format(slider, "%");
  gtk_widget_set_tooltip_text(slider, _("the white luminance of the target display or print.\n"
                                        "can be used creatively for a faded look or blowing out whites earlier."));

  self->widget = main_box;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
