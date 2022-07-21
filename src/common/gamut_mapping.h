/*
   This file is part of darktable,
   Copyright (C) 2022 darktable developers.

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

#pragma once

#include "common/dttypes.h"
#include "common/iop_profile.h"


// Pipeline and ICC luminance is CIE Y 1931
// Kirk Ych/Yrg uses CIE Y 2006
// 1 CIE Y 1931 = 1.05785528 CIE Y 2006, so we need to adjust that.
// This also accounts for the CAT16 D50->D65 adaptation that has to be done
// to go from RGB to CIE LMS 2006.
// Warning: only applies to achromatic pixels.
#define CIE_Y_1931_to_CIE_Y_2006(x) (1.05785528f * (x))


static inline float _clip_chroma_white_raw(const float coeffs[3], const float target_white, const float Y,
                                           const float cos_h, const float sin_h)
{
  const float denominator_Y_coeff = coeffs[0] * (0.979381443298969f * cos_h + 0.391752577319588f * sin_h)
                                    + coeffs[1] * (0.0206185567010309f * cos_h + 0.608247422680412f * sin_h)
                                    - coeffs[2] * (cos_h + sin_h);
  const float denominator_target_term = target_white * (0.68285981628866f * cos_h + 0.482137060515464f * sin_h);

  // this channel won't limit the chroma
  if(denominator_Y_coeff == 0.f) return FLT_MAX;

  // The equation for max chroma has an asymptote at this point (zero of denominator).
  // Any Y below that value won't give us sensible results for the upper bound
  // and we should consider the lower bound instead.
  const float Y_asymptote = denominator_target_term / denominator_Y_coeff;
  if(Y <= Y_asymptote) return FLT_MAX;

  // Get chroma that brings one component of target RGB to the given target_rgb value.
  // coeffs are the transformation coeffs to get one components (R, G or B) from input LMS.
  // i.e. it is a row of the LMS -> RGB transformation matrix.
  // See tools/derive_filmic_v6_gamut_mapping.py for derivation of these equations.
  const float denominator = Y * denominator_Y_coeff - denominator_target_term;
  const float numerator = -0.427506877216495f
                          * (Y * (coeffs[0] + 0.856492345150334f * coeffs[1] + 0.554995960637719f * coeffs[2])
                             - 0.988237752433297f * target_white);

  return numerator / denominator;
}


static inline float _clip_chroma_white(const float coeffs[3], const float target_white, const float Y,
                                       const float cos_h, const float sin_h)
{
  // Due to slight numerical inaccuracies in color matrices,
  // the chroma clipping curves for each RGB channel may be
  // slightly at the max luminance. Thus we linearly interpolate
  // each clipping line to zero chroma near max luminance.
  const float eps = 1e-3f;
  const float max_Y = CIE_Y_1931_to_CIE_Y_2006(target_white);
  const float delta_Y = MAX(max_Y - Y, 0.f);
  float max_chroma;
  if(delta_Y < eps)
  {
    max_chroma = delta_Y / (eps * max_Y) * _clip_chroma_white_raw(coeffs, target_white, (1.f - eps) * max_Y, cos_h, sin_h);
  }
  else
  {
    max_chroma = _clip_chroma_white_raw(coeffs, target_white, Y, cos_h, sin_h);
  }
  return max_chroma >= 0.f ? max_chroma : FLT_MAX;
}


static inline float _clip_chroma_black(const float coeffs[3], const float cos_h, const float sin_h)
{
  // N.B. this is the same as clip_chroma_white_raw() but with target value = 0.
  // This allows eliminating some computation.

  // Get chroma that brings one component of target RGB to zero.
  // coeffs are the transformation coeffs to get one components (R, G or B) from input LMS.
  // i.e. it is a row of the LMS -> RGB transformation matrix.
  // See tools/derive_filmic_v6_gamut_mapping.py for derivation of these equations.
  const float denominator = coeffs[0] * (0.979381443298969f * cos_h + 0.391752577319588f * sin_h)
                            + coeffs[1] * (0.0206185567010309f * cos_h + 0.608247422680412f * sin_h)
                            - coeffs[2] * (cos_h + sin_h);

  // this channel won't limit the chroma
  if(denominator == 0.f) return FLT_MAX;

  const float numerator = -0.427506877216495f * (coeffs[0] + 0.856492345150334f * coeffs[1] + 0.554995960637719f * coeffs[2]);
  const float max_chroma = numerator / denominator;
  return max_chroma >= 0.f ? max_chroma : FLT_MAX;
}


static inline float Ych_max_chroma_without_negatives(const dt_colormatrix_t matrix_out,
                                                     const float cos_h, const float sin_h)
{
  const float chroma_R_black = _clip_chroma_black(matrix_out[0], cos_h, sin_h);
  const float chroma_G_black = _clip_chroma_black(matrix_out[1], cos_h, sin_h);
  const float chroma_B_black = _clip_chroma_black(matrix_out[2], cos_h, sin_h);
  return MIN(MIN(chroma_R_black, chroma_G_black), chroma_B_black);
}


#ifdef _OPENMP
#pragma omp declare simd uniform(matrix) aligned(in, out:16) aligned(matrix:64)
#endif
static inline void RGB_to_Ych(const dt_aligned_pixel_t in, const dt_colormatrix_t matrix, dt_aligned_pixel_t out)
{
  dt_aligned_pixel_t LMS = { 0.f };
  dt_aligned_pixel_t Yrg = { 0.f };

  // go from pipeline RGB to CIE 2006 LMS D65
  dot_product(in, matrix, LMS);

  // go from CIE LMS 2006 to Kirk/Filmlight Yrg
  LMS_to_Yrg(LMS, Yrg);

  // rewrite in polar coordinates
  Yrg_to_Ych(Yrg, out);
}


#ifdef _OPENMP
#pragma omp declare simd uniform(matrix) aligned(in, out:16) aligned(matrix:64)
#endif
static inline void Ych_to_RGB(const dt_aligned_pixel_t in, const dt_colormatrix_t matrix, dt_aligned_pixel_t out)
{
  dt_aligned_pixel_t LMS = { 0.f };
  dt_aligned_pixel_t Yrg = { 0.f };

  // rewrite in cartesian coordinates
  Ych_to_Yrg(in, Yrg);

  // go from Kirk/Filmlight Yrg to CIE LMS 2006
  Yrg_to_LMS(Yrg, LMS);

  // go from CIE LMS 2006 to pipeline RGB
  dot_product(LMS, matrix, out);
}


static inline void prepare_RGB_Yrg_matrices(const dt_iop_order_iccprofile_info_t *const profile,
                                            dt_colormatrix_t input_matrix, dt_colormatrix_t output_matrix)
{
  dt_colormatrix_t temp_matrix;

  // Prepare the RGB (D50) -> XYZ D50 -> XYZ D65 -> LMS 2006 matrix
  dt_colormatrix_mul(temp_matrix, XYZ_D50_to_D65_CAT16, profile->matrix_in);
  dt_colormatrix_mul(input_matrix, XYZ_D65_to_LMS_2006_D65, temp_matrix);

  // Prepare the LMS 2006 -> XYZ D65 -> XYZ D50 -> RGB matrix (D50)
  dt_colormatrix_mul(temp_matrix, XYZ_D65_to_D50_CAT16, LMS_2006_D65_to_XYZ_D65);
  dt_colormatrix_mul(output_matrix, profile->matrix_out, temp_matrix);  
}


static inline float Ych_max_chroma(const dt_colormatrix_t matrix_out, const float target_white, const float Y,
                                   const float cos_h, const float sin_h)
{
  // Note: ideally we should figure out in advance which channel is going to clip first
  // (either go negative or over maximum allowed value) and calculate chroma clipping
  // curves only for those channels. That would avoid some ambiguities
  // (what do negative chroma values mean etc.) and reduce computation. However this
  // "brute-force" approach seems to work fine for now.

  const float chroma_R_white = _clip_chroma_white(matrix_out[0], target_white, Y, cos_h, sin_h);
  const float chroma_G_white = _clip_chroma_white(matrix_out[1], target_white, Y, cos_h, sin_h);
  const float chroma_B_white = _clip_chroma_white(matrix_out[2], target_white, Y, cos_h, sin_h);
  const float max_chroma_white = MIN(MIN(chroma_R_white, chroma_G_white), chroma_B_white);

  const float max_chroma_black = Ych_max_chroma_without_negatives(matrix_out, cos_h, sin_h);

  return MIN(max_chroma_black, max_chroma_white);
}
