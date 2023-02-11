/*
    This file is part of darktable,
    Copyright (C) 2020-2022 darktable developers.

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
#include "common/math.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/openmp_maths.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_sigmoid_params_t)


#define MIDDLE_GREY 0.1845f


typedef enum dt_iop_sigmoid_methods_type_t
{
  DT_SIGMOID_METHOD_PER_CHANNEL = 0,     // $DESCRIPTION: "Per channel"
  DT_SIGMOID_METHOD_RGB_RATIO = 1,     // $DESCRIPTION: "Rgb ratio"
} dt_iop_sigmoid_methods_type_t;


typedef struct dt_iop_sigmoid_params_t
{
  float middle_grey_contrast;  // $MIN: 0.1  $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "Contrast"
  float contrast_skewness;     // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Skew"
  float display_white_target;  // $MIN: 20.0  $MAX: 1600.0 $DEFAULT: 100.0 $DESCRIPTION: "Target white"
  float display_black_target;  // $MIN: 0.0  $MAX: 15.0 $DEFAULT: 0.0152 $DESCRIPTION: "Target black"
  dt_iop_sigmoid_methods_type_t color_processing;  // $DEFAULT: DT_SIGMOID_METHOD_PER_CHANNEL $DESCRIPTION: "Color processing"
  float hue_preservation;                          // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "Preserve hue"
} dt_iop_sigmoid_params_t;

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
} dt_iop_sigmoid_data_t;

typedef struct dt_iop_sigmoid_gui_data_t
{
  GtkWidget *contrast_slider, *skewness_slider, *color_processing_list, *hue_preservation_slider,
      *display_black_slider, *display_white_slider;

  dt_gui_collapsible_section_t cs;

} dt_iop_sigmoid_gui_data_t;


const char *name()
{
  return _("Sigmoid");
}

const char *aliases()
{
  return _("Tone mapping|view transform|display transform");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("Apply a view transform to make a image displayable\n"
                                        "on a screen or print. Uses a robust and smooth\n"
                                        "tone curve with optional color preservation methods."),
                                      _("Corrective and creative"),
                                      _("Linear, RGB, scene-referred"),
                                      _("Non-linear, RGB"),
                                      _("Linear, RGB, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_TECHNICAL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_sigmoid_params_t p;
  p.display_white_target = 100.0f;
  p.display_black_target = 0.0152f;
  p.color_processing = DT_SIGMOID_METHOD_PER_CHANNEL;

  p.middle_grey_contrast = 1.22f;
  p.contrast_skewness = 0.65f;
  p.hue_preservation = 100.0f;
  dt_gui_presets_add_generic(_("Neutral gray"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.middle_grey_contrast = 1.6f;
  p.contrast_skewness = -0.2f;
  p.hue_preservation = 0.0f;
  dt_gui_presets_add_generic(_("ACES 100-nit like"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.middle_grey_contrast = 1.0f;
  p.contrast_skewness = 0.0f;
  p.color_processing = DT_SIGMOID_METHOD_RGB_RATIO;
  dt_gui_presets_add_generic(_("Reinhard"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
}

// Declared here as it is used in the commit params function
#ifdef _OPENMP
#pragma omp declare simd uniform(magnitude, paper_exp, film_fog, film_power, paper_power)
#endif
static inline float generalized_loglogistic_sigmoid(const float value, const float magnitude, const float paper_exp,
                                                    const float film_fog, const float film_power, const float paper_power)
{
  const float clamped_value = fmaxf(value, 0.0f);
  // The following equation can be derived as a model for film + paper but it has a pole at 0
  // magnitude * powf(1.0 + paper_exp * powf(film_fog + value, -film_power), -paper_power);
  // Rewritten on a stable around zero form:
  const float film_response = powf(film_fog + clamped_value, film_power);
  const float paper_response = magnitude * powf(film_response / (paper_exp + film_response), paper_power);

  // Safety check for very large floats that cause numerical errors
  return isnan(paper_response) ? magnitude : paper_response;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_sigmoid_params_t *params = (dt_iop_sigmoid_params_t *)p1;
  dt_iop_sigmoid_data_t *module_data = (dt_iop_sigmoid_data_t *)piece->data;
  /* Calculate actual skew log logistic parameters to fulfill the following:
   * f(scene_zero) = display_black_target
   * f(scene_grey) = MIDDLE_GREY
   * f(scene_inf)  = display_white_target
   * Slope at scene_grey independet of skewness i.e. only changed by the contrast parameter.
   */

  // Calculate a reference slope for no skew and a normalized display
  const float ref_film_power = params->middle_grey_contrast;
  const float ref_paper_power = 1.0f;
  const float ref_magnitude = 1.0;
  const float ref_film_fog = 0.0f;
  const float ref_paper_exposure = powf(ref_film_fog + MIDDLE_GREY, ref_film_power) * ((ref_magnitude / MIDDLE_GREY) - 1.0f);
  const float delta = 1e-6f;
  const float ref_slope = (generalized_loglogistic_sigmoid(MIDDLE_GREY + delta, ref_magnitude, ref_paper_exposure, ref_film_fog, ref_film_power, ref_paper_power) -
                           generalized_loglogistic_sigmoid(MIDDLE_GREY - delta, ref_magnitude, ref_paper_exposure, ref_film_fog, ref_film_power, ref_paper_power)) / 2.0f / delta;

  // Add skew
  module_data->paper_power = powf(5.0f, -params->contrast_skewness);

  // Slope at low film power
  const float temp_film_power = 1.0f;
  const float temp_white_target = 0.01f * params->display_white_target;
  const float temp_white_grey_relation = powf(temp_white_target / MIDDLE_GREY, 1.0f / module_data->paper_power) - 1.0f;
  const float temp_paper_exposure = powf(MIDDLE_GREY, temp_film_power) * temp_white_grey_relation;
  const float temp_slope = (generalized_loglogistic_sigmoid(MIDDLE_GREY + delta, temp_white_target, temp_paper_exposure, ref_film_fog, temp_film_power, module_data->paper_power) -
                            generalized_loglogistic_sigmoid(MIDDLE_GREY - delta, temp_white_target, temp_paper_exposure, ref_film_fog, temp_film_power, module_data->paper_power)) / 2.0f / delta;

  // Figure out what film power fulfills the target slope
  // (linear when assuming display_black = 0.0)
  module_data->film_power = ref_slope / temp_slope;

  // Calculate the other parameters now that both film and paper power is known
  module_data->white_target = 0.01f * params->display_white_target;
  module_data->black_target = 0.01f * params->display_black_target;
  const float white_grey_relation = powf(module_data->white_target / MIDDLE_GREY, 1.0f / module_data->paper_power) - 1.0f;
  const float white_black_relation = powf(module_data->black_target / module_data->white_target, -1.0f / module_data->paper_power) - 1.0f;

  module_data->film_fog = MIDDLE_GREY * powf(white_grey_relation, 1.0f / module_data->film_power) / (powf(white_black_relation, 1.0f / module_data->film_power) - powf(white_grey_relation, 1.0f / module_data->film_power));
  module_data->paper_exposure = powf(module_data->film_fog + MIDDLE_GREY, module_data->film_power) * white_grey_relation;

  module_data->color_processing = params->color_processing;
  module_data->hue_preservation = fminf(fmaxf(0.01f * params->hue_preservation, 0.0f), 1.0f);
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void desaturate_negative_values(const dt_aligned_pixel_t pix_in, dt_aligned_pixel_t pix_out)
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

static void pixel_channel_order(const dt_aligned_pixel_t pix_in, dt_iop_sigmoid_value_order_t *pixel_value_order)
{
  if (pix_in[0] >= pix_in[1])
  {
    if (pix_in[1] > pix_in[2])
    {  // Case 1: r >= g >  b
      pixel_value_order->max = 0;
      pixel_value_order->mid = 1;
      pixel_value_order->min = 2;
    }
    else if (pix_in[2] > pix_in[0])
    {  // Case 2: b >  r >= g
      pixel_value_order->max = 2;
      pixel_value_order->mid = 0;
      pixel_value_order->min = 1;
    }
    else if (pix_in[2] > pix_in[1])
    {  // Case 3: r >= b >  g
      pixel_value_order->max = 0;
      pixel_value_order->mid = 2;
      pixel_value_order->min = 1;
    }
    else
    {  // Case 4: r == g == b
      // No change of the middle value, just assign something.
      pixel_value_order->max = 0;
      pixel_value_order->mid = 1;
      pixel_value_order->min = 2;
    }
  }
  else
  {
    if (pix_in[0] >= pix_in[2])
    {  // Case 5: g >  r >= b
      pixel_value_order->max = 1;
      pixel_value_order->mid = 0;
      pixel_value_order->min = 2;
    }
    else if (pix_in[2] > pix_in[1])
    {  // Case 6: b >  g >  r
      pixel_value_order->max = 2;
      pixel_value_order->mid = 1;
      pixel_value_order->min = 0;
    }
    else
    {  // Case 7: g >= b >  r
      pixel_value_order->max = 1;
      pixel_value_order->mid = 2;
      pixel_value_order->min = 0;
    }
  }
}


void process_loglogistic_per_channel(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                                   const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_sigmoid_data_t *module_data = (dt_iop_sigmoid_data_t *)piece->data;

  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;
  const size_t npixels = (size_t)roi_in->width * roi_in->height;

  const float white_target = module_data->white_target;
  const float paper_exp = module_data->paper_exposure;
  const float film_fog = module_data->film_fog;
  const float contrast_power = module_data->film_power;
  const float skew_power = module_data->paper_power;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, white_target, paper_exp, film_fog, contrast_power, skew_power) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4 * npixels; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    dt_aligned_pixel_t pix_in_strict_positive;

    // Force negative values to zero
    desaturate_negative_values(pix_in, pix_in_strict_positive);

    for_each_channel(c, aligned(pix_in_strict_positive, pix_out))
    {
      pix_out[c] = generalized_loglogistic_sigmoid(pix_in_strict_positive[c], white_target, paper_exp, film_fog, contrast_power, skew_power);
    }

    // Copy over the alpha channel
    pix_out[3] = pix_in[3];
  }
}

void process_loglogistic_rgb_ratio(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                                   const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_sigmoid_data_t *module_data = (dt_iop_sigmoid_data_t *)piece->data;
  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;
  const size_t npixels = (size_t)roi_in->width * roi_in->height;

  const float white_target = module_data->white_target;
  const float black_target = module_data->black_target;
  const float paper_exp = module_data->paper_exposure;
  const float film_fog = module_data->film_fog;
  const float contrast_power = module_data->film_power;
  const float skew_power = module_data->paper_power;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, white_target, black_target, paper_exp, film_fog, contrast_power, skew_power) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4 * npixels; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    dt_aligned_pixel_t pre_out;
    dt_aligned_pixel_t pix_in_strict_positive;

    // Force negative values to zero
    desaturate_negative_values(pix_in, pix_in_strict_positive);

    // Preserve color ratios by applying the tone curve on a luma estimate and then scale the RGB tripplet uniformly
    const float luma = (pix_in_strict_positive[0] + pix_in_strict_positive[1] + pix_in_strict_positive[2]) / 3.0f;
    const float mapped_luma = generalized_loglogistic_sigmoid(luma, white_target, paper_exp, film_fog, contrast_power, skew_power);

    if (luma > 1e-9)
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
    pixel_channel_order(pre_out, &pixel_value_order);
    const float pixel_min = pre_out[pixel_value_order.min];
    const float pixel_max = pre_out[pixel_value_order.max];

    // Chroma relative display gamut and scene "mapping" gamut.
    const float epsilon = 1e-6;
    const float display_border_vs_chroma_white = (white_target - mapped_luma) / (pixel_max - mapped_luma + epsilon); // "Distance" to max channel = white_target
    const float display_border_vs_chroma_black = (black_target - mapped_luma) / (pixel_min - mapped_luma - epsilon); // "Distance" to min_channel = black_target
    const float display_border_vs_chroma = fminf(display_border_vs_chroma_white, display_border_vs_chroma_black);
    const float chroma_vs_mapping_border = (mapped_luma - pixel_min) / (mapped_luma + epsilon); // "Distance" to min channel = 0.0

    // Hyperbolic gamut compression
    // Small chroma values, i.e., colors close to the acromatic axis are preserved while large chroma values are compressed.

    const float pixel_chroma_adjustment = 1.0f / (chroma_vs_mapping_border * display_border_vs_chroma + epsilon);
    const float hyperbolic_chroma = 2.0f * chroma_vs_mapping_border / (1.0f - chroma_vs_mapping_border * chroma_vs_mapping_border + epsilon) * pixel_chroma_adjustment;

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
static inline void preserve_hue_and_energy(const dt_aligned_pixel_t pix_in, const dt_aligned_pixel_t per_channel, dt_aligned_pixel_t pix_out,
    const dt_iop_sigmoid_value_order_t order, const float hue_preservation)
{
  if (per_channel[order.max] - per_channel[order.min] < 1e-9 ||
      per_channel[order.mid] - per_channel[order.min] < 1e-9 )
  {
    pix_out[order.min] = per_channel[order.min];
    pix_out[order.mid] = per_channel[order.mid];
    pix_out[order.max] = per_channel[order.max];
    return;  // Nothing to fix
  }

  // Naive Hue correction of the middle channel
  const float full_hue_correction = per_channel[order.min] + ((per_channel[order.max] - per_channel[order.min]) * (pix_in[order.mid] - pix_in[order.min]) / (pix_in[order.max] - pix_in[order.min]));
  const float naive_hue_mid = (1.0 - hue_preservation) * per_channel[order.mid] + hue_preservation * full_hue_correction;

  const float per_channel_energy = per_channel[order.min] + per_channel[order.mid] + per_channel[order.max];
  const float naive_hue_energy = per_channel[order.min] + naive_hue_mid + per_channel[order.max];
  const float blend_factor = 2.0 * pix_in[order.min] / (pix_in[order.min] + pix_in[order.mid]);
  const float midscale = (pix_in[order.mid] - pix_in[order.min]) / (pix_in[order.max] - pix_in[order.min]);

  // Preserve hue constrained to maintain the same energy as the per channel result
  if (naive_hue_mid <= per_channel[order.mid])
  {
    const float energy_target = blend_factor * per_channel_energy + (1.0 - blend_factor) * naive_hue_energy;
    const float corrected_mid = ((1.0 - hue_preservation) * per_channel[order.mid] + hue_preservation * (midscale * per_channel[order.max] + (1.0 - midscale) * (energy_target - per_channel[order.max])))
                                / (1.0 + hue_preservation * (1.0 - midscale));
    pix_out[order.min] = energy_target - per_channel[order.max] - corrected_mid;
    pix_out[order.mid] = corrected_mid;
    pix_out[order.max] = per_channel[order.max];
  }
  else
  {
    const float energy_target = blend_factor * per_channel_energy + (1.0 - blend_factor) * naive_hue_energy;
    const float corrected_mid = ((1.0 - hue_preservation) * per_channel[order.mid] + hue_preservation * (per_channel[order.min] * (1.0f - midscale) + midscale * (energy_target - per_channel[order.min])))
                                / (1.0 + hue_preservation * midscale);
    pix_out[order.min] = per_channel[order.min];
    pix_out[order.mid] = corrected_mid;
    pix_out[order.max] = energy_target - per_channel[order.min] - corrected_mid;
  }
}

void process_loglogistic_per_channel_interpolated(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_sigmoid_data_t *module_data = (dt_iop_sigmoid_data_t *)piece->data;

  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;
  const size_t npixels = (size_t)roi_in->width * roi_in->height;

  const float white_target = module_data->white_target;
  const float paper_exp = module_data->paper_exposure;
  const float film_fog = module_data->film_fog;
  const float contrast_power = module_data->film_power;
  const float skew_power = module_data->paper_power;
  const float hue_preservation = module_data->hue_preservation;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, white_target, paper_exp, film_fog, contrast_power, skew_power, hue_preservation) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4 * npixels; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    dt_aligned_pixel_t pix_in_strict_positive;
    dt_aligned_pixel_t per_channel;

    // Force negative values to zero
    desaturate_negative_values(pix_in, pix_in_strict_positive);

    for_each_channel(c, aligned(pix_in_strict_positive, per_channel))
    {
      per_channel[c] = generalized_loglogistic_sigmoid(pix_in_strict_positive[c], white_target, paper_exp, film_fog, contrast_power, skew_power);
    }

    // Hue correction by scaling the middle value relative to the max and min values.
    dt_iop_sigmoid_value_order_t pixel_value_order;
    pixel_channel_order(pix_in_strict_positive, &pixel_value_order);
    preserve_hue_and_energy(pix_in_strict_positive, per_channel, pix_out, pixel_value_order, hue_preservation);

    // Copy over the alpha channel
    pix_out[3] = pix_in[3];
  }
}



/** process, all real work is done here. */
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  dt_iop_sigmoid_data_t *module_data = (dt_iop_sigmoid_data_t *)piece->data;

  if (module_data->color_processing == DT_SIGMOID_METHOD_PER_CHANNEL)
  {
    if (module_data->hue_preservation >= 0.001f)
    {
      process_loglogistic_per_channel_interpolated(piece, ivoid, ovoid, roi_in, roi_out);
    }
    else
    {
      process_loglogistic_per_channel(piece, ivoid, ovoid, roi_in, roi_out);
    }
  }
  else
  {
    process_loglogistic_rgb_ratio(piece, ivoid, ovoid, roi_in, roi_out);
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_sigmoid_data_t));
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_sigmoid_gui_data_t *g = (dt_iop_sigmoid_gui_data_t *)self->gui_data;
  dt_iop_sigmoid_params_t *p = (dt_iop_sigmoid_params_t *)self->params;

  gtk_widget_set_visible(g->hue_preservation_slider, p->color_processing == DT_SIGMOID_METHOD_PER_CHANNEL);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_sigmoid_gui_data_t *g = (dt_iop_sigmoid_gui_data_t *)self->gui_data;
  dt_iop_sigmoid_params_t *p = (dt_iop_sigmoid_params_t *)self->params;

  dt_bauhaus_slider_set(g->contrast_slider, p->middle_grey_contrast);
  dt_bauhaus_slider_set(g->skewness_slider, p->contrast_skewness);
  dt_bauhaus_slider_set(g->hue_preservation_slider, p->hue_preservation);
  dt_bauhaus_slider_set(g->display_black_slider, p->display_black_target);
  dt_bauhaus_slider_set(g->display_white_slider, p->display_white_target);

  dt_bauhaus_combobox_set_from_value(g->color_processing_list, p->color_processing);
  dt_gui_update_collapsible_section(&g->cs);

  gui_changed(self, NULL, NULL);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_sigmoid_gui_data_t *g = IOP_GUI_ALLOC(sigmoid);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // Look controls
  g->contrast_slider = dt_bauhaus_slider_from_params(self, "middle_grey_contrast");
  dt_bauhaus_slider_set_soft_range(g->contrast_slider, 0.7f, 3.0f);
  dt_bauhaus_slider_set_digits(g->contrast_slider, 3);
  gtk_widget_set_tooltip_text(g->contrast_slider, _("Compression of the applied curve\n"
                                                    "implicitly defines the supported input dynamic range"));
  g->skewness_slider = dt_bauhaus_slider_from_params(self, "contrast_skewness");
  gtk_widget_set_tooltip_text(g->skewness_slider, _("Shift the compression towards shadows or highlights.\n"
                                                    "Negative values increase contrast in shadows.\n"
                                                    "Positive values increase contrast in highlights.\n"
                                                    "The opposite end will see a reduction in contrast."));

  // Color handling
  g->color_processing_list = dt_bauhaus_combobox_from_params(self, "color_processing");
  g->hue_preservation_slider = dt_bauhaus_slider_from_params(self, "hue_preservation");
  dt_bauhaus_slider_set_format(g->hue_preservation_slider, "%");
  gtk_widget_set_tooltip_text(g->hue_preservation_slider, _("Optional correction of the hue twist introduced by\n"
                                                            "the per-channel processing method."));

  // collapsible section
  dt_gui_new_collapsible_section
    (&g->cs,
     "plugins/darkroom/sigmoid/expand_values",
     _("Display luminance"),
     GTK_BOX(self->widget));
  gtk_widget_set_tooltip_text(g->cs.expander,
                                _("Set display black/white targets"));
  GtkWidget *main_box = self->widget;
  self->widget = GTK_WIDGET(g->cs.container);

  g->display_black_slider = dt_bauhaus_slider_from_params(self, "display_black_target");
  dt_bauhaus_slider_set_soft_range(g->display_black_slider, 0.0f, 1.0f);
  dt_bauhaus_slider_set_digits(g->display_black_slider, 4);
  dt_bauhaus_slider_set_format(g->display_black_slider, "%");
  gtk_widget_set_tooltip_text(g->display_black_slider, _("The black luminance of the target display or print.\n"
                                                         "Can be used creatively for a faded look."));
  g->display_white_slider = dt_bauhaus_slider_from_params(self, "display_white_target");
  dt_bauhaus_slider_set_soft_range(g->display_white_slider, 50.0f, 100.0f);
  dt_bauhaus_slider_set_format(g->display_white_slider, "%");
  gtk_widget_set_tooltip_text(g->display_white_slider, _("The white luminance of the target display or print.\n"
                                                         "Can be used creatively for a faded look or blowing out whites earlier."));
  self->widget = main_box;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
