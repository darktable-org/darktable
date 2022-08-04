/*
    This file is part of darktable,
    copyright (c) 2021 darktable developers.

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

#include "colorspace.h"
#include "common.h"

typedef enum dt_iop_channelmixer_rgb_version_t
{
  CHANNELMIXERRGB_V_1 = 0, // $DESCRIPTION: "Version 1 (2020)"
  CHANNELMIXERRGB_V_2 = 1, // $DESCRIPTION: "Version 2 (2021)"
  CHANNELMIXERRGB_V_3 = 2, // $DESCRIPTION: "Version 3 (Apr 2021)"
} dt_iop_channelmixer_rgb_version_t;


typedef enum dt_adaptation_t
{
  DT_ADAPTATION_LINEAR_BRADFORD = 0, // $DESCRIPTION: "Linear Bradford (ICC v4)"
  DT_ADAPTATION_CAT16           = 1, // $DESCRIPTION: "CAT16 (CIECAM16)"
  DT_ADAPTATION_FULL_BRADFORD   = 2, // $DESCRIPTION: "Non-linear Bradford"
  DT_ADAPTATION_XYZ             = 3, // $DESCRIPTION: "XYZ "
  DT_ADAPTATION_RGB             = 4, // $DESCRIPTION: "None (bypass)"
  DT_ADAPTATION_LAST
} dt_adaptation_t;


#define INVERSE_SQRT_3 0.5773502691896258f
#define TRUE 1
#define FALSE 0
#define NORM_MIN 1e-6f

static inline float sqf(const float x)
{
  return x * x;
}

static inline float euclidean_norm(const float4 input)
{
  return fmax(native_sqrt(sqf(input.x) + sqf(input.y) + sqf(input.z)), NORM_MIN);
}

static inline float4 gamut_mapping(const float4 input, const float compression, const int clip)
{
  // Get the sum XYZ
  const float sum = input.x + input.y + input.z;
  const float Y = input.y;

  float4 output;

  if(sum > 0.f && Y > 0.f)
  {
    // Convert to xyY
    float4 xyY = { input.x / sum, input.y / sum , Y, 0.0f };

    // Convert to uvY
    float4 uvY = dt_xyY_to_uvY(xyY);

    // Get the chromaticity difference with white point uv
    const float2 D50 = { 0.20915914598542354f, 0.488075320769787f };
    const float2 delta = D50 - uvY.xy;
    const float Delta = Y * (sqf(delta.x) + sqf(delta.y));

    // Compress chromaticity (move toward white point)
    const float correction = (compression == 0.0f) ? 0.f : native_powr(Delta, compression);

    // Ensure the correction does not bring our uyY vector the other side of D50
    // that would switch to the opposite color, so we clip at D50
    const float2 tmp =  correction * delta + uvY.xy;
    uvY.xy = (uvY.xy > D50) ? fmax(tmp, D50)
                            : fmin(tmp, D50);

    // Convert back to xyY
    xyY = dt_uvY_to_xyY(uvY);

    // Clip upon request
    if(clip) xyY.xy = fmax(xyY.xy, 0.0f);

    // Check sanity of y
    // since we later divide by y, it can't be zero
    xyY.y = fmax(xyY.y, NORM_MIN);

    // Check sanity of x and y :
    // since Z = Y (1 - x - y) / y, if x + y >= 1, Z will be negative
    const float scale = xyY.x + xyY.y;
    const int sanitize = (scale >= 1.f);
    xyY.xy = (sanitize) ? xyY.xy / scale : xyY.xy;

    // Convert back to XYZ
    output = dt_xyY_to_XYZ(xyY);
  }
  else
  {
    // sum of channels == 0, and/or Y == 0 so we have black
    output = (float4)0.f;
  }
  return output;
}

static inline float4 luma_chroma(const float4 input, const float4 saturation, const float4 lightness,
                                 const dt_iop_channelmixer_rgb_version_t version)
{
  float4 output;

  // Compute euclidean norm
  float norm = euclidean_norm(input);
  const float avg = fmax((input.x + input.y + input.z) / 3.0f, NORM_MIN);

  if(norm > 0.f && avg > 0.f)
  {
    // Compute flat lightness adjustment
    const float mix = dot(input, lightness);

    // Compensate the norm to get color ratios (R, G, B) = (1, 1, 1) for grey (colorless) pixels.
    if(version == CHANNELMIXERRGB_V_3) norm *= INVERSE_SQRT_3;

    // Ratios
    // WARNING : dot product below uses all 4 channels, you need to make sure
    // input.w != NaN since saturation.w = 0.f
    output = input / norm;

    // Compute ratios and a flat colorfulness adjustment for the whole pixel
    float coeff_ratio = 0.f;

    if(version == CHANNELMIXERRGB_V_1)
      coeff_ratio = dot((1.f - output), saturation);
    else
      coeff_ratio = dot(output, saturation) / 3.f;

    // Adjust the RGB ratios with the pixel correction

    // if the ratio was already invalid (negative), we accept the result to be invalid too
    // otherwise bright saturated blues end up solid black
    const float4 min_ratio = (output < 0.0f) ? output : 0.0f;
    const float4 output_inverse = 1.0f - output;
    output = fmax(output_inverse * coeff_ratio + output, min_ratio);

    // The above interpolation between original pixel ratios and (1, 1, 1) might change the norm of the
    // ratios. Compensate for that.
    if(version == CHANNELMIXERRGB_V_3) norm /= euclidean_norm(output) * INVERSE_SQRT_3;

    // Apply colorfulness adjustment channel-wise and repack with lightness to get LMS back
    norm *= fmax(1.f + mix / avg, 0.f);
    output *= norm;
  }
  else
  {
    // we have black, 0 stays 0, no luminance = no color
    output = input;
  }

  return output;
}

#define unswitch_convert_any_LMS_to_XYZ(kind) \
  ({ switch(kind) \
    { \
      case DT_ADAPTATION_FULL_BRADFORD: \
      case DT_ADAPTATION_LINEAR_BRADFORD: \
      { \
        XYZ = convert_bradford_LMS_to_XYZ(LMS); \
        break; \
      }\
      case DT_ADAPTATION_CAT16:\
      { \
        XYZ = convert_CAT16_LMS_to_XYZ(LMS); \
        break; \
      } \
      case DT_ADAPTATION_XYZ: \
      { \
        XYZ = LMS; \
        break; \
      } \
      case DT_ADAPTATION_RGB: \
      case DT_ADAPTATION_LAST: \
      default: \
      { \
        XYZ = matrix_product_float4(LMS, RGB_to_XYZ); \
        break; \
      } \
    }})


#define unswitch_convert_XYZ_to_any_LMS(kind) \
  ({ switch(kind) \
  { \
    case DT_ADAPTATION_FULL_BRADFORD: \
    case DT_ADAPTATION_LINEAR_BRADFORD: \
    { \
      LMS = convert_XYZ_to_bradford_LMS(XYZ); \
      break; \
    } \
    case DT_ADAPTATION_CAT16: \
    { \
      LMS = convert_XYZ_to_CAT16_LMS(XYZ); \
      break; \
    } \
    case DT_ADAPTATION_XYZ: \
    { \
      LMS = XYZ; \
      break; \
    } \
    case DT_ADAPTATION_RGB: \
    case DT_ADAPTATION_LAST: \
    default: \
    { \
      LMS = matrix_product_float4(XYZ, XYZ_to_RGB); \
      break; \
    } \
  }})


static inline void downscale_vector(float4 *const vector, const float scaling)
{
  const int valid = (scaling > NORM_MIN) && !isnan(scaling);
  *vector /= (valid) ? (scaling + NORM_MIN) : NORM_MIN;
}

static inline void upscale_vector(float4 *const vector, const float scaling)
{
  const int valid = (scaling > NORM_MIN) && !isnan(scaling);
  *vector *= (valid) ? (scaling + NORM_MIN) : NORM_MIN;
}

static inline float4 chroma_adapt_bradford(const float4 RGB,
                                           constant const float *const RGB_to_XYZ,
                                           constant const float *const MIX,
                                           const float4 illuminant, const float p,
                                           const int full)
{
  // Convert from RGB to XYZ
  const float4 XYZ = matrix_product_float4(RGB, RGB_to_XYZ);
  const float Y = XYZ.y;

  // Convert to LMS
  float4 LMS = convert_XYZ_to_bradford_LMS(XYZ);

  // Do white balance
  downscale_vector(&LMS, Y);
    bradford_adapt_D50(&LMS, illuminant, p, full);
  upscale_vector(&LMS, Y);

  // Compute the 3D mix - this is a rotation + homothety of the vector base
  const float4 LMS_mixed = matrix_product_float4(LMS, MIX);

  return convert_bradford_LMS_to_XYZ(LMS_mixed);
};

static inline float4 chroma_adapt_CAT16(const float4 RGB,
                                        constant const float *const RGB_to_XYZ,
                                        constant const float *const MIX,
                                        const float4 illuminant, const float p,
                                        const int full)
{
  // Convert from RGB to XYZ
  const float4 XYZ = matrix_product_float4(RGB, RGB_to_XYZ);
  const float Y = XYZ.y;

  // Convert to LMS
  float4 LMS = convert_XYZ_to_CAT16_LMS(XYZ);

  // Do white balance
  downscale_vector(&LMS, Y);
    CAT16_adapt_D50(&LMS, illuminant, p, full); // force full-adaptation
  upscale_vector(&LMS, Y);

  // Compute the 3D mix - this is a rotation + homothety of the vector base
  const float4 LMS_mixed = matrix_product_float4(LMS, MIX);

  return convert_CAT16_LMS_to_XYZ(LMS_mixed);
}

static inline float4 chroma_adapt_XYZ(const float4 RGB,
                                      constant const float *const RGB_to_XYZ,
                                      constant const float *const MIX, const float4 illuminant)
{
  // Convert from RGB to XYZ
  float4 XYZ_mixed = matrix_product_float4(RGB, RGB_to_XYZ);
  const float Y = XYZ_mixed.y;

  // Do white balance in XYZ
  downscale_vector(&XYZ_mixed, Y);
    XYZ_adapt_D50(&XYZ_mixed, illuminant);
  upscale_vector(&XYZ_mixed, Y);

  // Compute the 3D mix in XYZ - this is a rotation + homothety of the vector base
  return matrix_product_float4(XYZ_mixed, MIX);
}

static inline float4 chroma_adapt_RGB(const float4 RGB,
                               constant const float *const RGB_to_XYZ,
                               constant const float *const MIX)
{
  // No white balance.

  // Compute the 3D mix in RGB - this is a rotation + homothety of the vector base
  float4 RGB_mixed = matrix_product_float4(RGB, MIX);

  // Convert from RGB to XYZ
  return matrix_product_float4(RGB_mixed, RGB_to_XYZ);
}

#define unswitch_chroma_adapt(kind) \
 ({ switch(kind) \
  { \
    case DT_ADAPTATION_FULL_BRADFORD: \
    { \
      XYZ = chroma_adapt_bradford(RGB, RGB_to_XYZ, MIX, illuminant, p, TRUE); \
      break; \
    } \
    case DT_ADAPTATION_LINEAR_BRADFORD: \
    { \
      XYZ = chroma_adapt_bradford(RGB, RGB_to_XYZ, MIX, illuminant, p, FALSE); \
      break; \
    } \
    case DT_ADAPTATION_CAT16: \
    { \
      XYZ = chroma_adapt_CAT16(RGB, RGB_to_XYZ, MIX, illuminant, 1.f, FALSE); \
      break; \
    } \
    case DT_ADAPTATION_XYZ: \
    { \
      XYZ = chroma_adapt_XYZ(RGB, RGB_to_XYZ, MIX, illuminant); \
      break; \
    } \
    case DT_ADAPTATION_RGB: \
    case DT_ADAPTATION_LAST: \
    default: \
    { \
      XYZ = chroma_adapt_RGB(RGB, RGB_to_XYZ, MIX); \
      break; \
    } \
  }})


/*
* The following kernels are 100% copy-pasted with the exception of
* the first line : const dt_adaptation_t kind = ...
* This ensures to unswitch the color space conversions branches for performance
* while keeping the same overall code structure for maintenance.
*
* The reference C version in src/iop/channelmixerrgb.c does it differently
* since C has an explicit -funswitchloop option, but OpenCL doesn't and
* we have to do it manually using macros and duplicating kernels.
*/

kernel void
channelmixerrgb_CAT16(read_only image2d_t in, write_only image2d_t out,
                      const int width, const int height,
                      constant const float *const RGB_to_XYZ,
                      constant const float *const XYZ_to_RGB,
                      constant const float *const MIX,
                      const float4 illuminant,
                      const float4 saturation,
                      const float4 lightness,
                      const float4 grey,
                      const float p, const float gamut, const int clip, const int apply_grey,
                      const dt_iop_channelmixer_rgb_version_t version)
{
  const dt_adaptation_t kind = DT_ADAPTATION_CAT16;

  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pix_in = read_imagef(in, sampleri, (int2)(x, y));

  float4 XYZ, LMS;
  float4 RGB = pix_in;
  RGB.w = 0.f;

  if(clip) RGB = fmax(RGB, 0.f);

  /* WE START IN PIPELINE RGB */
  unswitch_chroma_adapt(kind);

  /* FROM HERE WE ARE MANDATORILY IN XYZ - DATA IS IN temp_one */

  // Gamut mapping happens in XYZ space no matter what
  XYZ = gamut_mapping(XYZ, gamut, clip);

  // convert to LMS, XYZ or pipeline RGB
  unswitch_convert_XYZ_to_any_LMS(kind);

  /* FROM HERE WE ARE IN LMS, XYZ OR PIPELINE RGB depending on user param */

  // Clip in LMS
  if(clip) LMS = fmax(LMS, 0.0f);

  // Apply lightness / saturation adjustment
  LMS = luma_chroma(LMS, saturation, lightness, version);

  // Clip in LMS
  if(clip) LMS = fmax(LMS, 0.0f);

  // Save
  if(apply_grey)
  {
    // Turn LMS, XYZ or pipeline RGB into monochrome
    const float grey_mix = fmax(dot(LMS, grey), 0.0f);

    RGB.xyz = grey_mix;
    RGB.w = pix_in.w; // alpha mask
  }
  else
  {
    // Convert back to XYZ
    unswitch_convert_any_LMS_to_XYZ(kind);

    /* FROM HERE WE ARE MANDATORILY IN XYZ */

    // Clip in XYZ
    if(clip) XYZ = fmax(XYZ, 0.0f);

    // Convert back to RGB
    RGB = matrix_product_float4(XYZ, XYZ_to_RGB);

    if(clip) RGB = fmax(RGB, 0.f);
    RGB.w = pix_in.w;
  }

  write_imagef(out, (int2)(x, y), RGB);
}


kernel void
channelmixerrgb_bradford_linear(read_only image2d_t in, write_only image2d_t out,
                                const int width, const int height,
                                constant const float *const RGB_to_XYZ,
                                constant const float *const XYZ_to_RGB,
                                constant const float *const MIX,
                                const float4 illuminant,
                                const float4 saturation,
                                const float4 lightness,
                                const float4 grey,
                                const float p, const float gamut, const int clip, const int apply_grey,
                                const dt_iop_channelmixer_rgb_version_t version)
{
  const dt_adaptation_t kind = DT_ADAPTATION_LINEAR_BRADFORD;

  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pix_in = read_imagef(in, sampleri, (int2)(x, y));

  float4 XYZ, LMS;
  float4 RGB = pix_in;
  RGB.w = 0.f;

  if(clip) RGB = fmax(RGB, 0.f);

  /* WE START IN PIPELINE RGB */
  unswitch_chroma_adapt(kind);

  /* FROM HERE WE ARE MANDATORILY IN XYZ - DATA IS IN temp_one */

  // Gamut mapping happens in XYZ space no matter what
  XYZ = gamut_mapping(XYZ, gamut, clip);

  // convert to LMS, XYZ or pipeline RGB
  unswitch_convert_XYZ_to_any_LMS(kind);

  /* FROM HERE WE ARE IN LMS, XYZ OR PIPELINE RGB depending on user param */

  // Clip in LMS
  if(clip) LMS = fmax(LMS, 0.0f);

  // Apply lightness / saturation adjustment
  LMS = luma_chroma(LMS, saturation, lightness, version);

  // Clip in LMS
  if(clip) LMS = fmax(LMS, 0.0f);

  // Save
  if(apply_grey)
  {
    // Turn LMS, XYZ or pipeline RGB into monochrome
    const float grey_mix = fmax(dot(LMS, grey), 0.0f);

    RGB.xyz = grey_mix;
    RGB.w = pix_in.w; // alpha mask
  }
  else
  {
    // Convert back to XYZ
    unswitch_convert_any_LMS_to_XYZ(kind);

    /* FROM HERE WE ARE MANDATORILY IN XYZ */

    // Clip in XYZ
    if(clip) XYZ = fmax(XYZ, 0.0f);

    // Convert back to RGB
    RGB = matrix_product_float4(XYZ, XYZ_to_RGB);

    if(clip) RGB = fmax(RGB, 0.f);
    RGB.w = pix_in.w;
  }

  write_imagef(out, (int2)(x, y), RGB);
}

kernel void
channelmixerrgb_bradford_full(read_only image2d_t in, write_only image2d_t out,
                              const int width, const int height,
                              constant const float *const RGB_to_XYZ,
                              constant const float *const XYZ_to_RGB,
                              constant const float *const MIX,
                              const float4 illuminant,
                              const float4 saturation,
                              const float4 lightness,
                              const float4 grey,
                              const float p, const float gamut, const int clip, const int apply_grey,
                              const dt_iop_channelmixer_rgb_version_t version)
{
  const dt_adaptation_t kind = DT_ADAPTATION_FULL_BRADFORD;

  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pix_in = read_imagef(in, sampleri, (int2)(x, y));

  float4 XYZ, LMS;
  float4 RGB = pix_in;
  RGB.w = 0.f;

  if(clip) RGB = fmax(RGB, 0.f);

  /* WE START IN PIPELINE RGB */
  unswitch_chroma_adapt(kind);

  /* FROM HERE WE ARE MANDATORILY IN XYZ - DATA IS IN temp_one */

  // Gamut mapping happens in XYZ space no matter what
  XYZ = gamut_mapping(XYZ, gamut, clip);

  // convert to LMS, XYZ or pipeline RGB
  unswitch_convert_XYZ_to_any_LMS(kind);

  /* FROM HERE WE ARE IN LMS, XYZ OR PIPELINE RGB depending on user param */

  // Clip in LMS
  if(clip) LMS = fmax(LMS, 0.0f);

  // Apply lightness / saturation adjustment
  LMS = luma_chroma(LMS, saturation, lightness, version);

  // Clip in LMS
  if(clip) LMS = fmax(LMS, 0.0f);

  // Save
  if(apply_grey)
  {
    // Turn LMS, XYZ or pipeline RGB into monochrome
    const float grey_mix = fmax(dot(LMS, grey), 0.0f);

    RGB.xyz = grey_mix;
    RGB.w = pix_in.w; // alpha mask
  }
  else
  {
    // Convert back to XYZ
    unswitch_convert_any_LMS_to_XYZ(kind);

    /* FROM HERE WE ARE MANDATORILY IN XYZ */

    // Clip in XYZ
    if(clip) XYZ = fmax(XYZ, 0.0f);

    // Convert back to RGB
    RGB = matrix_product_float4(XYZ, XYZ_to_RGB);

    if(clip) RGB = fmax(RGB, 0.f);
    RGB.w = pix_in.w;
  }

  write_imagef(out, (int2)(x, y), RGB);
}


kernel void
channelmixerrgb_XYZ(read_only image2d_t in, write_only image2d_t out,
                    const int width, const int height,
                    constant const float *const RGB_to_XYZ,
                    constant const float *const XYZ_to_RGB,
                    constant const float *const MIX,
                    const float4 illuminant,
                    const float4 saturation,
                    const float4 lightness,
                    const float4 grey,
                    const float p, const float gamut, const int clip, const int apply_grey,
                    const dt_iop_channelmixer_rgb_version_t version)
{
  const dt_adaptation_t kind = DT_ADAPTATION_XYZ;

  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pix_in = read_imagef(in, sampleri, (int2)(x, y));

  float4 XYZ, LMS;
  float4 RGB = pix_in;
  RGB.w = 0.f;

  if(clip) RGB = fmax(RGB, 0.f);

  /* WE START IN PIPELINE RGB */
  unswitch_chroma_adapt(kind);

  /* FROM HERE WE ARE MANDATORILY IN XYZ - DATA IS IN temp_one */

  // Gamut mapping happens in XYZ space no matter what
  XYZ = gamut_mapping(XYZ, gamut, clip);

  // convert to LMS, XYZ or pipeline RGB
  unswitch_convert_XYZ_to_any_LMS(kind);

  /* FROM HERE WE ARE IN LMS, XYZ OR PIPELINE RGB depending on user param */

  // Clip in LMS
  if(clip) LMS = fmax(LMS, 0.0f);

  // Apply lightness / saturation adjustment
  LMS = luma_chroma(LMS, saturation, lightness, version);

  // Clip in LMS
  if(clip) LMS = fmax(LMS, 0.0f);

  // Save
  if(apply_grey)
  {
    // Turn LMS, XYZ or pipeline RGB into monochrome
    const float grey_mix = fmax(dot(LMS, grey), 0.0f);

    RGB.xyz = grey_mix;
    RGB.w = pix_in.w; // alpha mask
  }
  else
  {
    // Convert back to XYZ
    unswitch_convert_any_LMS_to_XYZ(kind);

    /* FROM HERE WE ARE MANDATORILY IN XYZ */

    // Clip in XYZ
    if(clip) XYZ = fmax(XYZ, 0.0f);

    // Convert back to RGB
    RGB = matrix_product_float4(XYZ, XYZ_to_RGB);

    if(clip) RGB = fmax(RGB, 0.f);
    RGB.w = pix_in.w;
  }

  write_imagef(out, (int2)(x, y), RGB);
}

kernel void
channelmixerrgb_RGB(read_only image2d_t in, write_only image2d_t out,
                    const int width, const int height,
                    constant const float *const RGB_to_XYZ,
                    constant const float *const XYZ_to_RGB,
                    constant const float *const MIX,
                    const float4 illuminant,
                    const float4 saturation,
                    const float4 lightness,
                    const float4 grey,
                    const float p, const float gamut, const int clip, const int apply_grey,
                    const dt_iop_channelmixer_rgb_version_t version)
{
  const dt_adaptation_t kind = DT_ADAPTATION_RGB;

  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pix_in = read_imagef(in, sampleri, (int2)(x, y));

  float4 XYZ, LMS;
  float4 RGB = pix_in;
  RGB.w = 0.f;

  if(clip) RGB = fmax(RGB, 0.f);

  /* WE START IN PIPELINE RGB */
  unswitch_chroma_adapt(kind);

  /* FROM HERE WE ARE MANDATORILY IN XYZ - DATA IS IN temp_one */

  // Gamut mapping happens in XYZ space no matter what
  XYZ = gamut_mapping(XYZ, gamut, clip);

  // convert to LMS, XYZ or pipeline RGB
  unswitch_convert_XYZ_to_any_LMS(kind);

  /* FROM HERE WE ARE IN LMS, XYZ OR PIPELINE RGB depending on user param */

  // Clip in LMS
  if(clip) LMS = fmax(LMS, 0.0f);

  // Apply lightness / saturation adjustment
  LMS = luma_chroma(LMS, saturation, lightness, version);

  // Clip in LMS
  if(clip) LMS = fmax(LMS, 0.0f);

  // Save
  if(apply_grey)
  {
    // Turn LMS, XYZ or pipeline RGB into monochrome
    const float grey_mix = fmax(dot(LMS, grey), 0.0f);

    RGB.xyz = grey_mix;
    RGB.w = pix_in.w; // alpha mask
  }
  else
  {
    // Convert back to XYZ
    unswitch_convert_any_LMS_to_XYZ(kind);

    /* FROM HERE WE ARE MANDATORILY IN XYZ */

    // Clip in XYZ
    if(clip) XYZ = fmax(XYZ, 0.0f);

    // Convert back to RGB
    RGB = matrix_product_float4(XYZ, XYZ_to_RGB);

    if(clip) RGB = fmax(RGB, 0.f);
    RGB.w = pix_in.w;
  }

  write_imagef(out, (int2)(x, y), RGB);
}
