/*
 *    This file is part of darktable,
 *    Copyright (C) 2022 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "colorspaces_inline_conversions.h"
#include "iop_profile.h"
#include "math.h"


// Gamut map generation and gamut compression routines.
// Inspired by Björn Ottosson's blog post about gamut compression:
// https://bottosson.github.io/posts/gamutclipping/

// The gamut boundary is described as a set of hue slices.
// Polynomials are fitted to the upper and lower boundaries
// in the (C, L) coordinates at each hue slice. This makes
// numerically calculating intersections pretty easy.


#define DT_GAMUT_MAP_HUE_STEPS 1080
#define DT_GAMUT_MAP_UPPER_DEGREE 5
#define DT_GAMUT_MAP_UPPER_SAMPLES 10
#define DT_GAMUT_MAP_LOWER_DEGREE 5
#define DT_GAMUT_MAP_LOWER_SAMPLES 10


typedef struct
{
  float upper_boundary_coeffs[DT_GAMUT_MAP_UPPER_DEGREE];
  float upper_boundary_approx_slope;
  float lower_boundary_coeffs[DT_GAMUT_MAP_LOWER_DEGREE];
  float lower_boundary_approx_slope;
  float cusp_lightness;
} dt_gamut_hue_slice_t;


typedef DT_ALIGNED_ARRAY struct
{
  size_t hue_steps;
  float white_lightness;
  float black_lightness;
  dt_gamut_hue_slice_t *slices;
} dt_gamut_boundary_data_t;


dt_gamut_boundary_data_t *const
dt_prepare_gamut_boundary_data(const dt_iop_order_iccprofile_info_t *const target_profile,
                               const float target_white_luminance, const float target_black_luminance,
                               const float blur_sigma_degrees, const char *const debug_filename);

void dt_free_gamut_boundary_data(dt_gamut_boundary_data_t *const data);


void dt_make_gamut_mapping_input_and_output_matrix(const dt_iop_order_iccprofile_info_t *const profile,
                                                   dt_colormatrix_t RGB_to_Oklab_lms,
                                                   dt_colormatrix_t Oklab_lms_to_RGB);


static inline float _find_intersection_with_upper_boundary(const dt_gamut_hue_slice_t *const slice,
                                                           const float white_lightness, const float lightness)
{
  // Initial guess is based on the linear approximation
  const float x_guess = (lightness - white_lightness) / slice->upper_boundary_approx_slope;
  // Apply one Halley iteration to find the lightness intersecion. It seems to suffice here.
  float intersection_polynomial[DT_GAMUT_MAP_UPPER_DEGREE + 1];
  for(size_t d = 1; d <= DT_GAMUT_MAP_UPPER_DEGREE; d++)
    intersection_polynomial[d] = slice->upper_boundary_coeffs[d - 1];
  intersection_polynomial[0] = white_lightness - lightness;
  return dt_polynomial_halley_iteration(intersection_polynomial, DT_GAMUT_MAP_UPPER_DEGREE, x_guess);
}


static inline float _find_intersection_with_lower_boundary(const dt_gamut_hue_slice_t *const slice,
                                                           const float black_lightness, const float lightness)
{
  // Initial guess is based on the linear approximation
  const float x_guess = (lightness - black_lightness) / slice->lower_boundary_approx_slope;
  // Apply one Halley iteration to find the lightness intersecion. It seems to suffice here.
  float intersection_polynomial[DT_GAMUT_MAP_LOWER_DEGREE + 1];
  for(size_t d = 1; d <= DT_GAMUT_MAP_LOWER_DEGREE; d++)
    intersection_polynomial[d] = slice->lower_boundary_coeffs[d - 1];
  intersection_polynomial[0] = black_lightness - lightness;
  return dt_polynomial_halley_iteration(intersection_polynomial, DT_GAMUT_MAP_LOWER_DEGREE, x_guess);
}


static inline float _compress_chroma(const float knee, const float target_chroma, const float source_chroma,
                                     const float hard_limit, const float chroma)
{
  // Knee function from Eq (2) of the following paper:
  // Colour gamut mapping between small and large colour gamuts: Part I. gamut compression
  // Lihao Xu, Baiyue Zhao, and M. R. Luo
  // https://doi.org/10.1364/OE.26.011481
  //
  // This is essentially a linear scaling of the range from [knee * target_chroma; source_chroma]
  // to [knee * target_chroma; target_chroma]
  const float knee_chroma = knee * target_chroma;
  if(chroma < knee_chroma) return chroma;

  const float result_chroma
      = knee_chroma + (chroma - knee_chroma) / (source_chroma - knee_chroma) * (target_chroma - knee_chroma);
  // Clip to target max chroma
  return MIN(result_chroma, hard_limit);
}


static inline float _find_intersection_with_slice(const dt_gamut_boundary_data_t *const data,
                                                  const dt_gamut_hue_slice_t *const slice, const float lightness)
{
  if(lightness > slice->cusp_lightness)
  {
    // line intersects with upper boundary
    return _find_intersection_with_upper_boundary(slice, data->white_lightness, lightness);
  }
  else
  {
    // line intersects with lower boundary
    return _find_intersection_with_lower_boundary(slice, data->black_lightness, lightness);
  }
}


static inline size_t _get_hue_index(const size_t hue_steps, const float hue)
{
  // NOTE: hue should be between 0 and 2 * pi
  return hue_steps * hue / (DT_M_PI_F * 2.f);
}


static inline float _get_hue_at_index(const size_t hue_steps, const size_t index)
{
  return 2.f * DT_M_PI_F * (float)index / hue_steps;
}


static inline float _find_boundary_chroma(const dt_gamut_boundary_data_t *const data, const float hue,
                                          const float lightness)
{
  const size_t hue_index_1 = _get_hue_index(data->hue_steps, hue);
  const float hue_at_index_1 = _get_hue_at_index(data->hue_steps, hue_index_1);
  const size_t hue_index_2 = (hue_index_1 + 1) % data->hue_steps;
  const float hue_at_index_2 = _get_hue_at_index(data->hue_steps, hue_index_2);

  // Linearly interpolate max chroma between two neighboring slices
  const float hue_1_coeff = (hue_at_index_2 - hue) / (hue_at_index_2 - hue_at_index_1);
  const float hue_2_coeff = 1.f - hue_1_coeff;
  const float target_chroma_1 = _find_intersection_with_slice(data, &data->slices[hue_index_1], lightness);
  const float target_chroma_2 = _find_intersection_with_slice(data, &data->slices[hue_index_2], lightness);

  return hue_1_coeff * target_chroma_1 + hue_2_coeff * target_chroma_2;
}


// Compress chroma from source gamut to fit the target gamut.
// Takes Oklab lms vector in and gives also Oklab lms out.
// This is done to allow going from working RGB to Oklab lms (and vice versa)
// in one matrix * vector multiplication.
static inline void dt_gamut_compress(const dt_aligned_pixel_t lms_in, dt_aligned_pixel_t lms_out,
                                     const dt_gamut_boundary_data_t *const target_data,
                                     const dt_gamut_boundary_data_t *const source_data, const float knee)
{
  dt_aligned_pixel_t lms, Lab;

  // Ensure lms is clipped to zero and take the cube root
  for_three_channels(c) lms[c] = cbrtf(MAX(lms_in[c], 0.f));
  dt_apply_transposed_color_matrix(lms, Oklab_M2_T, Lab);

  const float chroma = dt_fast_hypotf(Lab[1], Lab[2]);
  float hue = atan2f(Lab[2], Lab[1]);
  if(hue < 0.f) hue += DT_M_PI_F * 2.f;

  // Clamp the lightness between black point and white point
  const float new_lightness = MAX(MIN(Lab[0], target_data->white_lightness), target_data->black_lightness);

  const float target_chroma = _find_boundary_chroma(target_data, hue, new_lightness);
  const float source_chroma = MAX(_find_boundary_chroma(source_data, hue, new_lightness), target_chroma);
  const float new_chroma = _compress_chroma(knee, target_chroma, source_chroma, target_chroma, chroma);

  const float chroma_coeff = chroma != 0.f ? new_chroma / chroma : 1.f;
  Lab[0] = new_lightness;
  Lab[1] *= chroma_coeff;
  Lab[2] *= chroma_coeff;

  // There shouldn't be a need to clip the lms because chroma is only reduced.
  dt_apply_transposed_color_matrix(Lab, Oklab_M2_inv_T, lms);
  for_each_channel(c) lms_out[c] = lms[c] * lms[c] * lms[c];
}
