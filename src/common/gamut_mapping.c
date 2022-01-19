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


#include "gamut_mapping.h"
#include "chromatic_adaptation.h"
#include "develop/openmp_maths.h"
#include "iop/gaussian_elimination.h"


#define TOLERANCE 1e-6f
#define MAX_DEGREE (MAX(DT_GAMUT_MAP_UPPER_DEGREE, DT_GAMUT_MAP_LOWER_DEGREE))


static gboolean gamut_white_check(const dt_aligned_pixel_t Lab, const float white_luminance,
                                  const dt_colormatrix_t output_matrix)
{
  dt_aligned_pixel_t lms, RGB;

  // Same as Oklab_to_lms but without clipping of lms
  dt_apply_transposed_color_matrix(Lab, Oklab_M2_inv_T, lms);
  for_four_channels(c) lms[c] = lms[c] * lms[c] * lms[c];
  dt_apply_transposed_color_matrix(lms, output_matrix, RGB);

  // Check that RGB is not above the white boundary
  return RGB[0] <= white_luminance && RGB[1] <= white_luminance && RGB[2] <= white_luminance;
}


static float find_max_L(const float a, const float b, const float max_L, const float white_luminance,
                        const dt_colormatrix_t output_matrix)
{
  // Find the upper boundary maximum lightness at given chroma by bisection
  float upper = max_L;
  float lower = 0.f;
  float midpoint;
  do
  {
    midpoint = (upper + lower) / 2.f;
    const dt_aligned_pixel_t Lab = { midpoint, a, b, 0.f };
    if(gamut_white_check(Lab, white_luminance, output_matrix))
      lower = midpoint;
    else
      upper = midpoint;
  } while(upper - lower > TOLERANCE);
  return lower;
}


static void sample_upper_boundary(const float a, const float b, const float white_lightness,
                                  const float white_luminance, const dt_colormatrix_t output_matrix,
                                  float samples[][2])
{
  // Take a number of samples from the upper boundary
  const float C_step = 1.f / DT_GAMUT_MAP_UPPER_SAMPLES;
  float max_L = white_lightness;
  for(size_t i = 0; i < DT_GAMUT_MAP_UPPER_SAMPLES; i++)
  {
    const float C = (i + 1) * C_step;
    max_L = find_max_L(C * a, C * b, max_L, white_luminance, output_matrix);
    samples[i][0] = max_L;
    samples[i][1] = C;
  }
}


static gboolean gamut_black_check(const dt_aligned_pixel_t Lab, const float black_luminance,
                                  const dt_colormatrix_t output_matrix)
{
  // Roughly the same process as Oklab_to_lms and conversion to target RGB, but without any clipping between
  // and gamut checks instead.

  dt_aligned_pixel_t lms, RGB;
  // Check that lms is non-negative
  dt_apply_transposed_color_matrix(Lab, Oklab_M2_inv_T, lms);
  if(lms[0] < 0.f || lms[1] < 0.f || lms[2] < 0.f) return FALSE;

  for_four_channels(c) lms[c] = lms[c] * lms[c] * lms[c];
  dt_apply_transposed_color_matrix(lms, output_matrix, RGB);

  // Finally check that RGB is not below the black boundary
  return RGB[0] >= black_luminance && RGB[1] >= black_luminance && RGB[2] >= black_luminance;
}


static float find_max_C(const float a, const float b, const float L, const float C_start,
                        const float black_luminance, const dt_colormatrix_t output_matrix)
{
  // Roughly find the first edge when increasing chroma at constant lightness.
  // This is required to guarantee that we actually find the first boundary
  // where one RGB component goes out of gamut. There may be many of these
  // points and bisection alone can't guarantee finding the first one.
  const float C_step = 1e-2f;
  float C = C_start;
  int in_gamut = 1;
  do
  {
    C += C_step;
    const dt_aligned_pixel_t Lab = { L, C * a, C * b, 0.f };
    in_gamut = gamut_black_check(Lab, black_luminance, output_matrix);
  } while(in_gamut);

  // Refine the estimate by bisection
  float upper = C;
  float lower = C - C_step;
  float midpoint;
  do
  {
    midpoint = (upper + lower) / 2.f;
    const dt_aligned_pixel_t Lab = { L, midpoint * a, midpoint * b, 0.f };
    if(gamut_black_check(Lab, black_luminance, output_matrix))
      lower = midpoint;
    else
      upper = midpoint;
  } while(upper - lower > TOLERANCE);

  return lower;
}


static void sample_lower_boundary(const float a, const float b, const float black_lightness,
                                  const float white_lightness, const float black_luminance,
                                  const dt_colormatrix_t output_matrix, float samples[][2])
{
  // Take a number of samples of the lower boundary at various lightnesses
  const float L_step = (white_lightness - black_lightness) / DT_GAMUT_MAP_LOWER_SAMPLES;
  float max_C = 0.f;
  for(size_t i = 0; i < DT_GAMUT_MAP_LOWER_SAMPLES; i++)
  {
    const float L = black_lightness + (i + 1) * L_step;
    max_C = find_max_C(a, b, L, max_C, black_luminance, output_matrix);
    samples[i][0] = L;
    samples[i][1] = max_C;
  }
}


static gboolean fit_smoothed_polynomial(const size_t hue_steps, const float lightness_intercept,
                                        const size_t num_samples, const size_t degree, const ssize_t i,
                                        const float samples[][2], const float kernel[], const size_t kernel_size,
                                        double *const A, double *const y, double *const A_square,
                                        double *const y_square, float coeffs[])
{
  // Fit a polynomial to the gamut boundary by weighted linear least squares.
  // Samples from neighboring hue slices are included based on the given
  // smoothing kernel.

  for(ssize_t j = 0; j < kernel_size; j++)
  {
    ssize_t hue_index = i + j - kernel_size / 2;
    while(hue_index < 0) hue_index += hue_steps;
    while(hue_index >= hue_steps) hue_index -= hue_steps;

    for(size_t k = 0; k < num_samples; k++)
    {
      const size_t sample_index = j * num_samples + k;
      y[sample_index] = kernel[j] * (samples[hue_index * num_samples + k][0] - lightness_intercept);
      const float C = samples[hue_index * num_samples + k][1];
      const size_t matrix_row_base = sample_index * degree;
      float C_power = 1.f;
      for(size_t d = 0; d < degree; d++)
      {
        C_power *= C;
        A[matrix_row_base + d] = kernel[j] * C_power;
      }
    }
  }

  if(!pseudo_solve_gaussian_with_preallocated_buffers(A, y, A_square, y_square, kernel_size * num_samples, degree,
                                                      TRUE))
    return FALSE;

  for(size_t d = 0; d < degree; d++) coeffs[d] = y[d];

  return TRUE;
}


static gboolean fit_boundary(const dt_colormatrix_t target_input_matrix,
                             const dt_colormatrix_t target_output_matrix, const float white_luminance,
                             const float black_luminance, const size_t hue_steps, const float blur_sigma_degrees,
                             dt_gamut_boundary_data_t *const data)
{
  float(*upper_samples)[2] = dt_alloc_align(64, hue_steps * DT_GAMUT_MAP_UPPER_SAMPLES * 2 * sizeof(float));
  float(*lower_samples)[2] = dt_alloc_align(64, hue_steps * DT_GAMUT_MAP_LOWER_SAMPLES * 2 * sizeof(float));

  const float blur_sigma = MAX(blur_sigma_degrees / 360.f * hue_steps, 1e-6f);
  // Use a finite width of 6 sigma for the kernel
  const size_t kernel_width = blur_sigma * 6 + 1;
  float *const smoothing_kernel = dt_alloc_align(64, kernel_width * sizeof(float));

  if(!upper_samples || !lower_samples || !smoothing_kernel)
  {
    dt_free_align(upper_samples);
    dt_free_align(lower_samples);
    dt_free_align(smoothing_kernel);
    return FALSE;
  }

  // Blur the samples with Gaussian kernel to remove some of the sharp ridges yielding unpleasant transitions
  for(ssize_t i = 0; i < kernel_width; i++)
  {
    const ssize_t x = i - kernel_width / 2;
    // We take the square root because the kernel value will be squared in the
    // linear least squares calculations
    smoothing_kernel[i] = sqrtf(expf(-sqf(x) / (2.f * sqf(blur_sigma))));
  }

  dt_aligned_pixel_t lms, Lab;

  const dt_aligned_pixel_t RGB_white = { white_luminance, white_luminance, white_luminance, 0.f };
  dt_apply_transposed_color_matrix(RGB_white, target_input_matrix, lms);
  lms_to_Oklab(lms, Lab);
  data->white_lightness = Lab[0];

  const dt_aligned_pixel_t RGB_black = { black_luminance, black_luminance, black_luminance, 0.f };
  dt_apply_transposed_color_matrix(RGB_black, target_input_matrix, lms);
  lms_to_Oklab(lms, Lab);
  data->black_lightness = Lab[0];

  const size_t max_num_samples = kernel_width * MAX(DT_GAMUT_MAP_UPPER_SAMPLES, DT_GAMUT_MAP_LOWER_SAMPLES);
  int valid = TRUE;

#ifdef _OPENMP
#pragma omp parallel default(none)                                                                                \
    shared(valid, smoothing_kernel, kernel_width, max_num_samples, hue_steps, white_luminance, black_luminance,   \
           data, upper_samples, lower_samples, target_output_matrix)
#endif
  {
    double *const A = dt_alloc_align(64, max_num_samples * MAX_DEGREE * sizeof(double));
    double *const y = dt_alloc_align(64, max_num_samples * sizeof(double));
    double *const A_square = dt_alloc_align(64, MAX_DEGREE * MAX_DEGREE * sizeof(double));
    double *const y_square = dt_alloc_align(64, MAX_DEGREE * sizeof(double));
    float intersection_coeffs[MAX_DEGREE + 1];
    if(!A || !y || !A_square || !y_square) valid = FALSE;

#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
    // Initialize raw boundary sample values
    for(size_t i = 0; i < hue_steps; i++)
    {
      if(!valid) continue;
      const float h = _get_hue_at_index(hue_steps, i);
      const float a = cosf(h);
      const float b = sinf(h);
      sample_upper_boundary(a, b, data->white_lightness, white_luminance, target_output_matrix,
                            &upper_samples[i * DT_GAMUT_MAP_UPPER_SAMPLES]);
      sample_lower_boundary(a, b, data->black_lightness, data->white_lightness, black_luminance,
                            target_output_matrix, &lower_samples[i * DT_GAMUT_MAP_LOWER_SAMPLES]);
    }

#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
    // Fit polynomials to smoothed sampled points
    for(ssize_t i = 0; i < hue_steps; i++)
    {
      if(!valid) continue;

      dt_gamut_hue_slice_t *const slice = &data->slices[i];

      // Fit the upper boundary
      valid = fit_smoothed_polynomial(hue_steps, data->white_lightness, DT_GAMUT_MAP_UPPER_SAMPLES,
                                      DT_GAMUT_MAP_UPPER_DEGREE, i, upper_samples, smoothing_kernel, kernel_width,
                                      A, y, A_square, y_square, slice->upper_boundary_coeffs);
      if(!valid) continue;

      // Fit the lower boundary
      valid = fit_smoothed_polynomial(hue_steps, data->black_lightness, DT_GAMUT_MAP_LOWER_SAMPLES,
                                      DT_GAMUT_MAP_LOWER_DEGREE, i, lower_samples, smoothing_kernel, kernel_width,
                                      A, y, A_square, y_square, slice->lower_boundary_coeffs);
      if(!valid) continue;

      // Find the cusp as the intersection of the upper and lower boundary curves
      intersection_coeffs[0] = data->white_lightness - data->black_lightness;
      for(size_t d = 1; d <= MAX_DEGREE; d++)
      {
        const float upper_coeff = d <= DT_GAMUT_MAP_UPPER_DEGREE ? slice->upper_boundary_coeffs[d - 1] : 0.f;
        const float lower_coeff = d <= DT_GAMUT_MAP_LOWER_DEGREE ? slice->lower_boundary_coeffs[d - 1] : 0.f;
        intersection_coeffs[d] = upper_coeff - lower_coeff;
      }
      const float cusp_chroma = dt_find_polynomial_root(intersection_coeffs, MAX_DEGREE, 0.f, TOLERANCE, 1000);
      slice->cusp_lightness = data->black_lightness
                              + cusp_chroma
                                    * dt_evaluate_polynomial(slice->lower_boundary_coeffs,
                                                             DT_GAMUT_MAP_LOWER_DEGREE - 1, cusp_chroma);
      // Approximate the upper and lower boundary with a line from white point to the cusp
      slice->upper_boundary_approx_slope = (slice->cusp_lightness - data->white_lightness) / cusp_chroma;
      slice->lower_boundary_approx_slope = (slice->cusp_lightness - data->black_lightness) / cusp_chroma;
    }

    dt_free_align(A);
    dt_free_align(y);
    dt_free_align(A_square);
    dt_free_align(y_square);
  }

  dt_free_align(upper_samples);
  dt_free_align(lower_samples);
  dt_free_align(smoothing_kernel);
  return valid;
}


void dt_make_gamut_mapping_input_and_output_matrix(const dt_iop_order_iccprofile_info_t *const profile,
                                                   dt_colormatrix_t RGB_to_Oklab_lms,
                                                   dt_colormatrix_t Oklab_lms_to_RGB)
{
  dt_colormatrix_t tmp;
  // input matrix is a product of these matrices:
  //   XYZ_D65_to_Oklab_lms * XYZ_D50_to_XYZ_D65 * RGB_to_XYZ_D50
  // For the transposed matrix, the multiplication order is reverse.
  dt_colormatrix_mul(tmp, XYZ_D50_to_D65_CAT16_transposed, Oklab_M1_T);
  dt_colormatrix_mul(RGB_to_Oklab_lms, profile->matrix_in_transposed, tmp);
  // output matrix is the following product:
  //   XYZ_D50_to_RGB * XYZ_D65_to_XYZ_D50 * Oklab_lms_to_XYZ_D65
  // For the transposed matrix, the multiplication order is reverse.
  dt_colormatrix_mul(tmp, XYZ_D65_to_D50_CAT16_transposed, profile->matrix_out_transposed);
  dt_colormatrix_mul(Oklab_lms_to_RGB, Oklab_M1_inv_T, tmp);
}


void dt_free_gamut_boundary_data(dt_gamut_boundary_data_t *const data)
{
  if(!data) return;
  dt_free_align(data->slices);
  dt_free_align(data);
}


dt_gamut_boundary_data_t *const
dt_prepare_gamut_boundary_data(const dt_iop_order_iccprofile_info_t *const target_profile,
                               const float target_white_luminance, const float target_black_luminance,
                               const float blur_sigma_degrees, const char *const debug_filename)
{
  const size_t hue_steps = DT_GAMUT_MAP_HUE_STEPS;

  dt_times_t start_time;
  dt_get_times(&start_time);

  dt_gamut_boundary_data_t *const data = dt_alloc_align(64, sizeof(dt_gamut_boundary_data_t));
  if(data == NULL) return NULL;

  data->slices = dt_alloc_align(64, hue_steps * sizeof(dt_gamut_hue_slice_t));
  if(data->slices == NULL) goto error_with_data;

  data->hue_steps = hue_steps;

  dt_colormatrix_t target_output_matrix, target_input_matrix;
  dt_make_gamut_mapping_input_and_output_matrix(target_profile, target_input_matrix, target_output_matrix);

  if(!fit_boundary(target_input_matrix, target_output_matrix, target_white_luminance, target_black_luminance,
                   hue_steps, blur_sigma_degrees, data))
    goto error_with_data;

  FILE *const fcsv = debug_filename ? g_fopen(debug_filename, "w") : NULL;
  if(fcsv)
  {
    // for easily plotting gamut diagrams with scripts
    for(size_t i = 0; i < hue_steps; i++)
    {
      const dt_gamut_hue_slice_t *const slice = &data->slices[i];
      const float hue_deg = 360.f * (float)i / hue_steps;
      fprintf(fcsv, "%.4f;%.4f;%.4f;%.4f;%.4f;", hue_deg, slice->cusp_lightness, data->white_lightness,
              slice->upper_boundary_approx_slope, slice->lower_boundary_approx_slope);
      for(size_t d = 0; d < DT_GAMUT_MAP_UPPER_DEGREE; d++)
        fprintf(fcsv, "%.4f;", slice->upper_boundary_coeffs[d]);
      fprintf(fcsv, "%.4f;", data->black_lightness);
      for(size_t d = 0; d < DT_GAMUT_MAP_LOWER_DEGREE; d++)
        fprintf(fcsv, "%.4f;", slice->lower_boundary_coeffs[d]);
      fprintf(fcsv, "0\n");
    }
    fclose(fcsv);
  }

  dt_show_times_f(&start_time, "[gamut_mapping]", "gamut map creation");

  return data;

error_with_data:
  dt_free_gamut_boundary_data(data);
  return NULL;
}
