/*
    This file is part of darktable,
    copyright (c) 2023 darktable developers.

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

#include "common.h"

typedef struct dt_iop_sigmoid_value_order_t
{
  size_t min;
  size_t mid;
  size_t max;
} dt_iop_sigmoid_value_order_t;

static void _pixel_channel_order(const float *pix_in, dt_iop_sigmoid_value_order_t *pixel_value_order)
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

static inline float4 _desaturate_negative_values(const float4 i)
{
  const float pixel_average = fmax((i.x + i.y + i.z) / 3.0f, 0.0f);
  const float min_value = fmin(fmin(i.x, i.y), i.z);
  const float saturation_factor = min_value < 0.0f ? -pixel_average / (min_value - pixel_average) : 1.0f;

  return pixel_average + saturation_factor * (i - pixel_average);
}

static inline float _generalized_loglogistic_sigmoid_scalar(const float value,
                                                            const float magnitude,
                                                            const float paper_exp,
                                                            const float film_fog,
                                                            const float film_power,
                                                            const float paper_power)
{
  const float clamped_value = fmax(value, 0.0f);
  // The following equation can be derived as a model for film + paper but it has a pole at 0
  // magnitude * powf(1.0 + paper_exp * powf(film_fog + value, -film_power), -paper_power);
  // Rewritten on a stable around zero form:
  const float film_response = pow(film_fog + clamped_value, film_power);
  const float paper_response = magnitude * pow(film_response / (paper_exp + film_response), paper_power);

  // Safety check for very large floats that cause numerical errors
  return isnan(paper_response) ? magnitude : paper_response;
}

static inline float4 _generalized_loglogistic_sigmoid_vector(const float4 i,
                                                             const float magnitude,
                                                             const float paper_exp,
                                                             const float film_fog,
                                                             const float film_power,
                                                             const float paper_power)
{
  //clamped_value
  float4 io = fmax(i, 0.0f);

  // The following equation can be derived as a model for film + paper but it has a pole at 0
  // magnitude * powf(1.0 + paper_exp * powf(film_fog + i, -film_power), -paper_power);
  // Rewritten on a stable around zero form:

  //film_response
  io = pow(film_fog + io, film_power);
  //paper_response
  io = magnitude * pow(io / (paper_exp + io), paper_power);

  // Safety check for very large floats that cause numerical errors
  return isnan(io) ? magnitude : io;
}

// Linear interpolation of hue that also preserve sum of channels
// Assumes hue_preservation strictly in range [0, 1]
static inline void _preserve_hue_and_energy(float *pix_io,
                                            const float *per_channel,
                                            const dt_iop_sigmoid_value_order_t order,
                                            const float hue_preservation)
{
  if (per_channel[order.max] - per_channel[order.min] < 1e-9f ||
    per_channel[order.mid] - per_channel[order.min] < 1e-9f )
  {
    pix_io[0] = per_channel[0];
    pix_io[1] = per_channel[1];
    pix_io[2] = per_channel[2];
    return;  // Nothing to fix
  }

  // Naive Hue correction of the middle channel
  const float full_hue_correction =
    per_channel[order.min] + ((per_channel[order.max] - per_channel[order.min]) *
    (pix_io[order.mid] - pix_io[order.min]) / (pix_io[order.max] - pix_io[order.min]));
  const float naive_hue_mid =
    (1.0f - hue_preservation) * per_channel[order.mid] + hue_preservation * full_hue_correction;

  const float per_channel_energy = per_channel[order.min] + per_channel[order.mid] + per_channel[order.max];
  const float naive_hue_energy = per_channel[order.min] + naive_hue_mid + per_channel[order.max];
  const float blend_factor = 2.0f * pix_io[order.min] / (pix_io[order.min] + pix_io[order.mid]);
  const float midscale = (pix_io[order.mid] - pix_io[order.min]) / (pix_io[order.max] - pix_io[order.min]);

  // Preserve hue constrained to maintain the same energy as the per channel result
  if (naive_hue_mid <= per_channel[order.mid])
  {
    const float energy_target = blend_factor * per_channel_energy + (1.0f - blend_factor) * naive_hue_energy;
    const float corrected_mid =
      ((1.0f - hue_preservation) * per_channel[order.mid] + hue_preservation *
      (midscale * per_channel[order.max] + (1.0f - midscale) * (energy_target - per_channel[order.max]))) /
      (1.0f + hue_preservation * (1.0f - midscale));
    pix_io[order.min] = energy_target - per_channel[order.max] - corrected_mid;
    pix_io[order.mid] = corrected_mid;
    pix_io[order.max] = per_channel[order.max];
  }
  else
  {
    const float energy_target = blend_factor * per_channel_energy + (1.0f - blend_factor) * naive_hue_energy;
    const float corrected_mid =
      ((1.0f - hue_preservation) * per_channel[order.mid] +
      hue_preservation * (per_channel[order.min] * (1.0f - midscale) +
      midscale * (energy_target - per_channel[order.min]))) / (1.0f + hue_preservation * midscale);
    pix_io[order.min] = per_channel[order.min];
    pix_io[order.mid] = corrected_mid;
    pix_io[order.max] = energy_target - per_channel[order.min] - corrected_mid;
  }
}

kernel void
sigmoid_loglogistic_per_channel (read_only image2d_t in,
                                 write_only image2d_t out,
                                 const int width,
                                 const int height,
                                 const float white_target,
                                 const float paper_exp,
                                 const float film_fog,
                                 const float contrast_power,
                                 const float skew_power)
{
  const unsigned int x = get_global_id(0);
  const unsigned int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 i = read_imagef(in, sampleri, (int2)(x, y));
  float alpha = i.w;

  // Force negative values to zero
  i = _desaturate_negative_values(i);

  i = _generalized_loglogistic_sigmoid_vector(i, white_target, paper_exp, film_fog, contrast_power, skew_power);

  // Copy over the alpha channel
  i.w = alpha;

  write_imagef(out, (int2)(x, y), i);
}

kernel void
sigmoid_loglogistic_per_channel_interpolated (read_only image2d_t in,
                                              write_only image2d_t out,
                                              const int width,
                                              const int height,
                                              const float white_target,
                                              const float paper_exp,
                                              const float film_fog,
                                              const float contrast_power,
                                              const float skew_power,
                                              const float hue_preservation)
{
  const unsigned int x = get_global_id(0);
  const unsigned int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 i = read_imagef(in, sampleri, (int2)(x, y));
  float alpha = i.w;

  // Force negative values to zero
  i = _desaturate_negative_values(i);
  float pix_array[3] = {i.x, i.y, i.z};

  i = _generalized_loglogistic_sigmoid_vector(i, white_target, paper_exp, film_fog, contrast_power, skew_power);
  float per_channel[3] = {i.x, i.y, i.z};

  // Hue correction by scaling the middle value relative to the max and min values.
  dt_iop_sigmoid_value_order_t pixel_value_order;
  _pixel_channel_order(pix_array, &pixel_value_order);
  _preserve_hue_and_energy(pix_array, per_channel, pixel_value_order, hue_preservation);

  i.xyz = (float3)(pix_array[0], pix_array[1], pix_array[2]);
  // Copy over the alpha channel
  i.w = alpha;

  write_imagef(out, (int2)(x, y), i);
}

kernel void
sigmoid_loglogistic_rgb_ratio(read_only image2d_t in,
                              write_only image2d_t out,
                              const int width,
                              const int height,
                              const float white_target,
                              const float black_target,
                              const float paper_exp,
                              const float film_fog,
                              const float contrast_power,
                              const float skew_power)
{
  const unsigned int x = get_global_id(0);
  const unsigned int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 i = read_imagef(in, sampleri, (int2)(x, y));
  float alpha = i.w;


  // Force negative values to zero
  i = _desaturate_negative_values(i);

  // Preserve color ratios by applying the tone curve on a luma estimate and then scale the RGB tripplet uniformly
  const float luma = (i.x + i.y + i.z) / 3.0f;
  const float mapped_luma =
    _generalized_loglogistic_sigmoid_scalar(luma, white_target, paper_exp, film_fog, contrast_power, skew_power);

  if (luma > 1e-9f)
  {
    const float scaling_factor = mapped_luma / luma;
    i = scaling_factor * i;
  }
  else
  {
    i = (float4)mapped_luma;
  }

  // get min an max;
  const float pixel_min = fmin(fmin(i.x, i.y), i.z);
  const float pixel_max = fmax(fmax(i.x, i.y), i.z);

  // Chroma relative display gamut and scene "mapping" gamut.

  const float epsilon = 1e-6f;
  // "Distance" to max channel = white_target
  const float display_border_vs_chroma_white = (white_target - mapped_luma) / (pixel_max - mapped_luma + epsilon);
  // "Distance" to min_channel = black_target
  const float display_border_vs_chroma_black = (black_target - mapped_luma) / (pixel_min - mapped_luma - epsilon);

  const float display_border_vs_chroma = fmin(display_border_vs_chroma_white, display_border_vs_chroma_black);
  // "Distance" to min channel = 0.0
  const float chroma_vs_mapping_border = (mapped_luma - pixel_min) / (mapped_luma + epsilon);

  // Hyperbolic gamut compression
  // Small chroma values, i.e. colors close to the acromatic axis are preserved while large chroma values are compressed.

  const float pixel_chroma_adjustment = 1.0f / (chroma_vs_mapping_border * display_border_vs_chroma + epsilon);
  const float hyperbolic_chroma =
    2.0f * chroma_vs_mapping_border /
    (1.0f - chroma_vs_mapping_border * chroma_vs_mapping_border + epsilon) * pixel_chroma_adjustment;

  const float hyperbolic_z = sqrt(hyperbolic_chroma * hyperbolic_chroma + 1.0f);
  const float chroma_factor = hyperbolic_chroma / (1.0f + hyperbolic_z) * display_border_vs_chroma;

  i = mapped_luma + chroma_factor * (i - mapped_luma);

  // Copy over the alpha channel
  i.w = alpha;

  write_imagef(out, (int2)(x, y), i);
}
