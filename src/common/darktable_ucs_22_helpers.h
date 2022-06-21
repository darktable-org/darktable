
#pragma once

#include "common/math.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/chromatic_adaptation.h"

#define LUT_ELEM 360     // gamut LUT number of elements: resolution of 1°

static inline float Delta_H(const float h_1, const float h_2)
{
  // Compute the difference between 2 angles
  // and force the result in [-pi; pi] radians
  float diff = h_1 - h_2;
  diff += (diff < -M_PI_F) ? 2.f * M_PI_F : 0.f;
  diff -= (diff > M_PI_F) ? 2.f * M_PI_F : 0.f;
  return diff;
}

static inline void dt_UCS_22_build_gamut_LUT(dt_colormatrix_t input_matrix, float gamut_LUT[LUT_ELEM])
{
  /**
   * @brief Build a LUT of the gamut boundary of the RGB space defined by `input_matrix` in the form
   * boundary_colorfulness = f(hue). The LUT is sampled for every degree of hue angle between [-180°; 180°[.
   * input_matrix is the RGB -> XYZ D65 conversion matrix. Since all ICC profiles use D50 XYZ, it needs
   * to be premultiplied by the chromatic adaptation transform ahead.
   *
   * See https://eng.aurelienpierre.com/2022/02/color-saturation-control-for-the-21th-century/#Gamut-mapping
   * for the details of the computations.
   */

  // init the LUT between -180° and 180° by increments of 1°
  for(size_t k = 0; k < LUT_ELEM; k++) gamut_LUT[k] = 0.f;

  dt_aligned_pixel_t D65_xyY = { 0.31269999999999992f,  0.32899999999999996f ,  1.f, 0.f };

  // Compute the RGB space primaries in xyY
  dt_aligned_pixel_t RGB_red   = { 1.f, 0.f, 0.f, 0.f };
  dt_aligned_pixel_t RGB_green = { 0.f, 1.f, 0.f, 0.f };
  dt_aligned_pixel_t RGB_blue =  { 0.f, 0.f, 1.f, 0.f };

  dt_aligned_pixel_t XYZ_red, XYZ_green, XYZ_blue;
  dot_product(RGB_red, input_matrix, XYZ_red);
  dot_product(RGB_green, input_matrix, XYZ_green);
  dot_product(RGB_blue, input_matrix, XYZ_blue);

  dt_aligned_pixel_t xyY_red, xyY_green, xyY_blue;
  dt_XYZ_to_xyY(XYZ_red, xyY_red);
  dt_XYZ_to_xyY(XYZ_green, xyY_green);
  dt_XYZ_to_xyY(XYZ_blue, xyY_blue);

  // Get the "hue" angles of the primaries in xy compared to D65
  const float h_red   = atan2f(xyY_red[1] - D65_xyY[1], xyY_red[0] - D65_xyY[0]);
  const float h_green = atan2f(xyY_green[1] - D65_xyY[1], xyY_green[0] - D65_xyY[0]);
  const float h_blue  = atan2f(xyY_blue[1] - D65_xyY[1], xyY_blue[0] - D65_xyY[0]);

  // March the gamut boundary in CIE xyY 1931 by angular steps of 0.02°
  #ifdef _OPENMP
    #pragma omp parallel for default(none) \
          dt_omp_firstprivate(input_matrix, xyY_red, xyY_green, xyY_blue, h_red, h_green, h_blue, D65_xyY) \
          schedule(static) dt_omp_sharedconst(gamut_LUT)
  #endif
  for(int i = 0; i < 50 * 360; i++)
  {
    const float angle = -M_PI_F + ((float)i) / (50.f * 360.f) * 2.f * M_PI_F;
    const float tan_angle = tanf(angle);

    const float t_1 = Delta_H(angle, h_blue)  / Delta_H(h_red, h_blue);
    const float t_2 = Delta_H(angle, h_red)   / Delta_H(h_green, h_red);
    const float t_3 = Delta_H(angle, h_green) / Delta_H(h_blue, h_green);

    float x_t = 0;
    float y_t = 0;

    if(t_1 == CLAMP(t_1, 0, 1))
    {
      const float t = (D65_xyY[1] - xyY_blue[1] + tan_angle * (xyY_blue[0] - D65_xyY[0]))
                / (xyY_red[1] - xyY_blue[1] + tan_angle * (xyY_blue[0] - xyY_red[0]));
      x_t = xyY_blue[0] + t * (xyY_red[0] - xyY_blue[0]);
      y_t = xyY_blue[1] + t * (xyY_red[1] - xyY_blue[1]);
    }
    else if(t_2 == CLAMP(t_2, 0, 1))
    {
      const float t = (D65_xyY[1] - xyY_red[1] + tan_angle * (xyY_red[0] - D65_xyY[0]))
                / (xyY_green[1] - xyY_red[1] + tan_angle * (xyY_red[0] - xyY_green[0]));
      x_t = xyY_red[0] + t * (xyY_green[0] - xyY_red[0]);
      y_t = xyY_red[1] + t * (xyY_green[1] - xyY_red[1]);
    }
    else if(t_3 == CLAMP(t_3, 0, 1))
    {
      const float t = (D65_xyY[1] - xyY_green[1] + tan_angle * (xyY_green[0] - D65_xyY[0]))
                    / (xyY_blue[1] - xyY_green[1] + tan_angle * (xyY_green[0] - xyY_blue[0]));
      x_t = xyY_green[0] + t * (xyY_blue[0] - xyY_green[0]);
      y_t = xyY_green[1] + t * (xyY_blue[1] - xyY_green[1]);
    }

    // Convert to darktable UCS
    dt_aligned_pixel_t xyY = { x_t, y_t, 1.f, 0.f };
    float UV_star_prime[2];
    xyY_to_dt_UCS_UV(xyY, UV_star_prime);

    // Get the hue angle in darktable UCS
    const float H = atan2f(UV_star_prime[1], UV_star_prime[0]) * 180.f / M_PI_F;
    const float H_round = roundf(H);
    if(fabsf(H - H_round) < 0.02f)
    {
      int index = (int)(H_round + 180);
      index += (index < 0) ? 360 : 0;
      index -= (index > 359) ? 360 : 0;
      // Warning: we store M², the square of the colorfulness
      gamut_LUT[index] = UV_star_prime[0] * UV_star_prime[0] + UV_star_prime[1] * UV_star_prime[1];
    }
  }
}


static inline float get_minimum_saturation(float gamut_LUT[LUT_ELEM], const float lightness, const float L_white)
{
  // Find the minimum of saturation of the gamut boundary
  // Use this to guess the saturation value that will contain all hues within the gamut boundary
  // at the specified lightness
  float colorfulness_min = FLT_MAX;
  for(size_t k = 0; k < LUT_ELEM; k++) colorfulness_min = fminf(gamut_LUT[k], colorfulness_min);

  // Note : for greys (achromatic colors), brightness = lightness.
  // Since we target a desired brightness but we need
  // a lightness in the computations, we just let that be true all the time.
  const float max_chroma = 15.932993652962535f * powf(lightness * L_white, 0.6523997524738018f) * powf(colorfulness_min, 0.6007557017508491f) / L_white;

  // Convert the boundary chroma to saturation
  dt_aligned_pixel_t HSB_gamut_boundary;
  dt_UCS_JCH_to_HSB((dt_aligned_pixel_t){ lightness, max_chroma, 0.f, 0.f }, HSB_gamut_boundary);
  return HSB_gamut_boundary[1];
}


static inline float lookup_gamut(const float gamut_lut[LUT_ELEM], const float hue)
{
  /**
   * @brief Linearly interpolate the value of the gamut LUT at the hue angle in radians. The LUT needs to be sampled every
   * degree of angle. WARNING : x should be between [-pi ; pi[, which is the default output of atan2 anyway.
   */

  // convert hue in LUT index coordinate
  const float x_test = (float)LUT_ELEM * (hue + M_PI_F) / (2.f * M_PI_F);

  // find the 2 closest integer coordinates (next/previous)
  float x_prev = floorf(x_test);
  float x_next = ceilf(x_test);

  // get the 2 closest LUT elements at integer coordinates
  // cycle on the hue ring if out of bounds
  int xi = (int)x_prev;
  if(xi < 0) xi = LUT_ELEM - 1;
  else if(xi > LUT_ELEM - 1) xi = 0;

  int xii = (int)x_next;
  if(xii < 0) xii = LUT_ELEM - 1;
  else if(xii > LUT_ELEM - 1) xii = 0;

  // fetch the corresponding y values
  const float y_prev = gamut_lut[xi];
  const float y_next = gamut_lut[xii];

  // assume that we are exactly on an integer LUT element
  float out = y_prev;

  if(x_next != x_prev)
    // we are between 2 LUT elements : do linear interpolation
    // actually, we only add the slope term on the previous one
    out += (x_test - x_prev) * (y_next - y_prev) / (x_next - x_prev);

  return out;
}


static inline float soft_clip(const float x, const float soft_threshold, const float hard_threshold)
{
  // use an exponential soft clipping above soft_threshold
  // hard threshold must be > soft threshold
  const float norm = hard_threshold - soft_threshold;
  return (x > soft_threshold) ? soft_threshold + (1.f - expf(-(x - soft_threshold) / norm)) * norm : x;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(HSB: 16) uniform(gamut_LUT, L_white)
#endif
static inline void gamut_map_HSB(dt_aligned_pixel_t HSB, const float gamut_LUT[LUT_ELEM], const float L_white)
{
  /**
   * @brief Soft-clip saturation at constant brightness, taking Helmholtz-Kohlrausch effect into account, such that
   * the HSB color coordinates fit within the destination RGB color space characterized be `gamut_LUT`.
   *
   */

  // Note: HSB[0] = JCH[2], aka the hue is constant no matter the space.

  // We need J to get the max chroma from the max colorfulness, so we need to convert to JCH.
  dt_aligned_pixel_t JCH;
  dt_UCS_HSB_to_JCH(HSB, JCH);

  // Compute the chroma of the boundary from the colorfulness of the boundary (defined in the LUT)
  // and from the lightness J of the current pixel
  const float max_colorfulness = lookup_gamut(gamut_LUT, JCH[2]); // WARNING : this is M²
  const float max_chroma = 15.932993652962535f * powf(JCH[0] * L_white, 0.6523997524738018f) * powf(max_colorfulness, 0.6007557017508491f) / L_white;

  // Convert the boundary chroma to saturation
  const dt_aligned_pixel_t JCH_gamut_boundary = { JCH[0], max_chroma, JCH[2], 0.f };
  dt_aligned_pixel_t HSB_gamut_boundary;
  dt_UCS_JCH_to_HSB(JCH_gamut_boundary, HSB_gamut_boundary);

  // Soft-clip the current pixel saturation at constant brightness
  HSB[1] = soft_clip(HSB[1], 0.8f * HSB_gamut_boundary[1], HSB_gamut_boundary[1]);
}


static inline struct dt_iop_order_iccprofile_info_t * D65_adapt_iccprofile(struct dt_iop_order_iccprofile_info_t *work_profile)
{
  // Premultiply the input and output matrices of a typical D50 ICC profile by a chromatic adaptation matrix to adapt them for D65
  // such that we perform XYZ D65 -> XYZ D50 -> display RGB and display RGB -> XYZ D50 -> XYZ D65 in one matrix multiplication
  // WARNING: white_adapted_profile needs to be freed outside of this function.

  if(work_profile) // && !isnan(work_profile->matrix_in[0][0]))
  {
    // Alloc
    struct dt_iop_order_iccprofile_info_t *white_adapted_profile = (dt_iop_order_iccprofile_info_t *)malloc(sizeof(dt_iop_order_iccprofile_info_t));

    // Init a new temp profile by copying the base profile
    memcpy(white_adapted_profile, work_profile, sizeof(dt_iop_order_iccprofile_info_t));

    // Multiply the in/out matrices by the chromatic adaptation matrix
    dt_colormatrix_t input_matrix;
    dt_colormatrix_t output_matrix;
    dt_colormatrix_mul(input_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
    dt_colormatrix_mul(output_matrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);

    // Replace the D50 matrices in the adapted profile by the D65 ones
    memcpy(white_adapted_profile->matrix_out, output_matrix, sizeof(output_matrix));
    memcpy(white_adapted_profile->matrix_in, input_matrix, sizeof(input_matrix));

    // Update the transposed output matrix since that's what is used in actual color conversion
    transpose_3xSSE(white_adapted_profile->matrix_out, white_adapted_profile->matrix_out_transposed);
    transpose_3xSSE(white_adapted_profile->matrix_in, white_adapted_profile->matrix_in_transposed);

    return white_adapted_profile;
  }
  else
  {
    // We either don't have a work_profile to begin with, or it is not a matrix-based profile.
    // In the latter case, we can't premultiply the white point.
    // This will happen when the display profile is a custom-made 3D LUT profile, which is not recommended.
    // WARNING: the NULL case should be handled later in the actual color conversion, for example
    // by falling back to sRGB.
    return NULL;
  }
}
