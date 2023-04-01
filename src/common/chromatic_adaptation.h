/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

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

#include "common/colorspaces_inline_conversions.h"
#include "common/math.h"

typedef enum dt_adaptation_t
{
  DT_ADAPTATION_LINEAR_BRADFORD = 0, // $DESCRIPTION: "linear Bradford (ICC v4)"
  DT_ADAPTATION_CAT16           = 1, // $DESCRIPTION: "CAT16 (CIECAM16)"
  DT_ADAPTATION_FULL_BRADFORD   = 2, // $DESCRIPTION: "non-linear Bradford"
  DT_ADAPTATION_XYZ             = 3, // $DESCRIPTION: "XYZ"
  DT_ADAPTATION_RGB             = 4, // $DESCRIPTION: "none (bypass)"
  DT_ADAPTATION_LAST
} dt_adaptation_t;


// modified LMS cone response space for Bradford transform
// explanation here : https://onlinelibrary.wiley.com/doi/pdf/10.1002/9781119021780.app3
// but coeffs are wrong in the above, so they come from :
// http://www2.cmp.uea.ac.uk/Research/compvis/Papers/FinSuss_COL00.pdf
// At any time, ensure XYZ_to_LMS is the exact matrice inverse of LMS_to_XYZ
static const dt_colormatrix_t XYZ_to_Bradford_LMS = { {  0.8951f,  0.2664f, -0.1614f, 0.f },
                                                      { -0.7502f,  1.7135f,  0.0367f, 0.f },
                                                      {  0.0389f, -0.0685f,  1.0296f, 0.f } };

static const dt_colormatrix_t Bradford_LMS_to_XYZ = { {  0.9870f, -0.1471f,  0.1600f, 0.f },
                                                      {  0.4323f,  0.5184f,  0.0493f, 0.f },
                                                      { -0.0085f,  0.0400f,  0.9685f, 0.f } };

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, LMS:16)
#endif
static inline void convert_XYZ_to_bradford_LMS(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t LMS)
{
  // Warning : needs XYZ normalized with Y - you need to downscale before
  dot_product(XYZ, XYZ_to_Bradford_LMS, LMS);
}

static inline void make_RGB_to_Bradford_LMS(const dt_colormatrix_t rgb, dt_colormatrix_t lms)
{
  dt_colormatrix_mul(lms, XYZ_to_Bradford_LMS, rgb);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, LMS:16)
#endif
static inline void convert_bradford_LMS_to_XYZ(const dt_aligned_pixel_t LMS, dt_aligned_pixel_t XYZ)
{
  // Warning : output XYZ normalized with Y - you need to upscale later
  dot_product(LMS, Bradford_LMS_to_XYZ, XYZ);
}

static inline void make_Bradford_LMS_to_RGB(const dt_colormatrix_t lms_to_rgb, dt_colormatrix_t rgb)
{
  dt_colormatrix_mul(rgb, lms_to_rgb, Bradford_LMS_to_XYZ);
}

static inline void make_Bradford_LMS_to_XYZ(const dt_colormatrix_t lms, dt_colormatrix_t xyz)
{
  dt_colormatrix_mul(xyz, Bradford_LMS_to_XYZ, lms);
}


// modified LMS cone response for CAT16, from CIECAM16
// reference : https://ntnuopen.ntnu.no/ntnu-xmlui/bitstream/handle/11250/2626317/CCIW-23.pdf?sequence=1
// At any time, ensure XYZ_to_LMS is the exact matrice inverse of LMS_to_XYZ
static const dt_colormatrix_t XYZ_to_CAT16_LMS = { {  0.401288f, 0.650173f, -0.051461f, 0.f },
                                                   { -0.250268f, 1.204414f,  0.045854f, 0.f },
                                                   { -0.002079f, 0.048952f,  0.953127f, 0.f } };

static const dt_colormatrix_t CAT16_LMS_to_XYZ = { {  1.862068f, -1.011255f,  0.149187f, 0.f },
                                                   {  0.38752f ,  0.621447f, -0.008974f, 0.f },
                                                   { -0.015841f, -0.034123f,  1.049964f, 0.f } };

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, LMS:16)
#endif
static inline void convert_XYZ_to_CAT16_LMS(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t LMS)
{
  // Warning : needs XYZ normalized with Y - you need to downscale before
  dot_product(XYZ, XYZ_to_CAT16_LMS, LMS);
}

static inline void make_RGB_to_CAT16_LMS(const dt_colormatrix_t rgb, dt_colormatrix_t lms)
{
  dt_colormatrix_mul(lms, XYZ_to_CAT16_LMS, rgb);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, LMS:16)
#endif
static inline void convert_CAT16_LMS_to_XYZ(const dt_aligned_pixel_t LMS, dt_aligned_pixel_t XYZ)
{
  // Warning : output XYZ normalized with Y - you need to upscale later
  dot_product(LMS, CAT16_LMS_to_XYZ, XYZ);
}

static inline void make_CAT16_LMS_to_RGB(const dt_colormatrix_t lms_to_rgb, dt_colormatrix_t rgb)
{
  dt_colormatrix_mul(rgb, lms_to_rgb, CAT16_LMS_to_XYZ);
}

static inline void make_CAT16_LMS_to_XYZ(const dt_colormatrix_t lms, dt_colormatrix_t xyz)
{
  dt_colormatrix_mul(xyz, CAT16_LMS_to_XYZ, lms);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, LMS:16) uniform(kind)
#endif
static inline void convert_any_LMS_to_XYZ(const dt_aligned_pixel_t LMS, dt_aligned_pixel_t XYZ,
                                          const dt_adaptation_t kind)
{
  // helper function switching internally to the proper conversion

  switch(kind)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    case DT_ADAPTATION_LINEAR_BRADFORD:
    {
      convert_bradford_LMS_to_XYZ(LMS, XYZ);
      break;
    }
    case DT_ADAPTATION_CAT16:
    {
      convert_CAT16_LMS_to_XYZ(LMS, XYZ);
      break;
    }
    case DT_ADAPTATION_XYZ:
    case DT_ADAPTATION_RGB:
    case DT_ADAPTATION_LAST:
    default:
    {
      // special case : just pass through.
      XYZ[0] = LMS[0];
      XYZ[1] = LMS[1];
      XYZ[2] = LMS[2];
      break;
    }
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, LMS:16) uniform(kind)
#endif
static inline void convert_any_XYZ_to_LMS(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t LMS, dt_adaptation_t kind)
{
  // helper function switching internally to the proper conversion

  switch(kind)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    case DT_ADAPTATION_LINEAR_BRADFORD:
    {
      convert_XYZ_to_bradford_LMS(XYZ, LMS);
      break;
    }
    case DT_ADAPTATION_CAT16:
    {
      convert_XYZ_to_CAT16_LMS(XYZ, LMS);
      break;
    }
    case DT_ADAPTATION_XYZ:
    case DT_ADAPTATION_RGB:
    case DT_ADAPTATION_LAST:
    default:
    {
      // special case : just pass through.
      LMS[0] = XYZ[0];
      LMS[1] = XYZ[1];
      LMS[2] = XYZ[2];
      break;
    }
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(RGB, LMS:16) uniform(kind)
#endif
static inline void convert_any_LMS_to_RGB(const dt_aligned_pixel_t LMS, dt_aligned_pixel_t RGB, dt_adaptation_t kind)
{
  // helper function switching internally to the proper conversion
  dt_aligned_pixel_t XYZ = { 0.f };
  convert_any_LMS_to_XYZ(LMS, XYZ, kind);

  // Fixme : convert to RGB display space instead of sRGB but first the display profile should be global in dt,
  // not confined to colorout where it gets created/destroyed all the time.
  dt_XYZ_to_Rec709_D65(XYZ, RGB);

  // Handle gamut clipping
  float max_RGB = fmaxf(fmaxf(RGB[0], RGB[1]), RGB[2]);
  for(int c = 0; c < 3; c++) RGB[c] = fmaxf(RGB[c] / max_RGB, 0.f);

}


/* Bradford adaptations pre-computed for D50 and D65 outputs */

#ifdef _OPENMP
#pragma omp declare simd uniform(origin_illuminant) \
  aligned(lms_in, lms_out, origin_illuminant:16)
#endif
static inline void bradford_adapt_D65(const dt_aligned_pixel_t lms_in,
                                      const dt_aligned_pixel_t origin_illuminant,
                                      const float p, const int full,
                                      dt_aligned_pixel_t lms_out)
{
  // Bradford chromatic adaptation from origin to target D65 illuminant in LMS space
  // p = powf(origin_illuminant[2] / D65[2], 0.0834f) needs to be precomputed for performance,
  // since it is independent from current pixel values
  // origin illuminant need also to be precomputed to LMS

  // Precomputed D65 primaries in Bradford LMS for camera WB adjustment
  const dt_aligned_pixel_t D65 = { 0.941238f, 1.040633f, 1.088932f, 0.f };

  dt_aligned_pixel_t temp = { lms_in[0] / origin_illuminant[0],
                              lms_in[1] / origin_illuminant[1],
                              lms_in[2] / origin_illuminant[2],
                              0.f };

  // use linear Bradford if B is negative
  if(full) temp[2] = (temp[2] > 0.f) ? powf(temp[2], p) : temp[2];

  lms_out[0] = D65[0] * temp[0];
  lms_out[1] = D65[1] * temp[1];
  lms_out[2] = D65[2] * temp[2];
}


#ifdef _OPENMP
#pragma omp declare simd uniform(origin_illuminant) \
  aligned(lms_in, lms_out, origin_illuminant:16)
#endif
static inline void bradford_adapt_D50(const dt_aligned_pixel_t lms_in,
                                      const dt_aligned_pixel_t origin_illuminant,
                                      const float p, const int full,
                                      dt_aligned_pixel_t lms_out)
{
  // Bradford chromatic adaptation from origin to target D50 illuminant in LMS space
  // p = powf(origin_illuminant[2] / D50[2], 0.0834f) needs to be precomputed for performance,
  // since it is independent from current pixel values
  // origin illuminant need also to be precomputed to LMS

  // Precomputed D50 primaries in Bradford LMS for ICC transforms
  const dt_aligned_pixel_t D50 = { 0.996078f, 1.020646f, 0.818155f, 0.f };

  dt_aligned_pixel_t temp = { lms_in[0] / origin_illuminant[0],
                              lms_in[1] / origin_illuminant[1],
                              lms_in[2] / origin_illuminant[2],
                              0.f };

  // use linear Bradford if B is negative
  if(full) temp[2] = (temp[2] > 0.f) ? powf(temp[2], p) : temp[2];

  lms_out[0] = D50[0] * temp[0];
  lms_out[1] = D50[1] * temp[1];
  lms_out[2] = D50[2] * temp[2];
}


/* CAT16 adaptations pre-computed for D50 and D65 outputs */

#ifdef _OPENMP
#pragma omp declare simd uniform(origin_illuminant) \
  aligned(lms_in, lms_out, origin_illuminant:16)
#endif
static inline void CAT16_adapt_D65(const dt_aligned_pixel_t lms_in,
                                   const dt_aligned_pixel_t origin_illuminant,
                                   const float D, const int full, dt_aligned_pixel_t lms_out)
{
  // CAT16 chromatic adaptation from origin to target D65 illuminant in LMS space
  // D is the coefficient of adaptation, depending of the surround lighting
  // origin illuminant need also to be precomputed to LMS

  // Precomputed D65 primaries in CAT16 LMS for camera WB adjustment
  const dt_aligned_pixel_t D65 = { 0.97553267f, 1.01647859f, 1.0848344f, 0.f };

  if(full)
  {
    lms_out[0] = lms_in[0] * D65[0] / origin_illuminant[0];
    lms_out[1] = lms_in[1] * D65[1] / origin_illuminant[1];
    lms_out[2] = lms_in[2] * D65[2] / origin_illuminant[2];
  }
  else
  {
    lms_out[0] = lms_in[0] * (D * D65[0] / origin_illuminant[0] + 1.f - D);
    lms_out[1] = lms_in[1] * (D * D65[1] / origin_illuminant[1] + 1.f - D);
    lms_out[2] = lms_in[2] * (D * D65[2] / origin_illuminant[2] + 1.f - D);
  }
}


#ifdef _OPENMP
#pragma omp declare simd uniform(origin_illuminant) \
  aligned(lms_in, lms_out, origin_illuminant:16)
#endif
static inline void CAT16_adapt_D50(const dt_aligned_pixel_t lms_in,
                                      const dt_aligned_pixel_t origin_illuminant,
                                      const float D, const int full,
                                      dt_aligned_pixel_t lms_out)
{
  // CAT16 chromatic adaptation from origin to target D50 illuminant in LMS space
  // D is the coefficient of adaptation, depending of the surround lighting
  // origin illuminant need also to be precomputed to LMS

  // Precomputed D50 primaries in CAT16 LMS for ICC transforms
  const dt_aligned_pixel_t D50 = { 0.994535f, 1.000997f, 0.833036f, 0.f };

  if(full)
  {
    lms_out[0] = lms_in[0] * D50[0] / origin_illuminant[0];
    lms_out[1] = lms_in[1] * D50[1] / origin_illuminant[1];
    lms_out[2] = lms_in[2] * D50[2] / origin_illuminant[2];
  }
  else
  {
    lms_out[0] = lms_in[0] * (D * D50[0] / origin_illuminant[0] + 1.f - D);
    lms_out[1] = lms_in[1] * (D * D50[1] / origin_illuminant[1] + 1.f - D);
    lms_out[2] = lms_in[2] * (D * D50[2] / origin_illuminant[2] + 1.f - D);
  }
}

/* XYZ adaptations pre-computed for D50 and D65 outputs */

#ifdef _OPENMP
#pragma omp declare simd uniform(origin_illuminant) \
  aligned(lms_in, lms_out, origin_illuminant:16)
#endif
static inline void XYZ_adapt_D65(const dt_aligned_pixel_t lms_in,
                                 const dt_aligned_pixel_t origin_illuminant,
                                 dt_aligned_pixel_t lms_out)
{
  // XYZ chromatic adaptation from origin to target D65 illuminant in XYZ space
  // origin illuminant need also to be precomputed to XYZ

  // Precomputed D65 primaries in XYZ for camera WB adjustment
  const dt_aligned_pixel_t D65 = { 0.9504285453771807f, 1.0f, 1.0889003707981277f, 0.f };

  lms_out[0] = lms_in[0] * D65[0] / origin_illuminant[0];
  lms_out[1] = lms_in[1] * D65[1] / origin_illuminant[1];
  lms_out[2] = lms_in[2] * D65[2] / origin_illuminant[2];
}

#ifdef _OPENMP
#pragma omp declare simd uniform(origin_illuminant) \
  aligned(lms_in, lms_out, origin_illuminant:16)
#endif
static inline void XYZ_adapt_D50(const dt_aligned_pixel_t lms_in,
                                 const dt_aligned_pixel_t origin_illuminant,
                                 dt_aligned_pixel_t lms_out)
{
  // XYZ chromatic adaptation from origin to target D65 illuminant in XYZ space
  // origin illuminant need also to be precomputed to XYZ

  // Precomputed D50 primaries in XYZ for camera WB adjustment
  const dt_aligned_pixel_t D50 = { 0.9642119944211994f, 1.0f, 0.8251882845188288f, 0.f };

  lms_out[0] = lms_in[0] * D50[0] / origin_illuminant[0];
  lms_out[1] = lms_in[1] * D50[1] / origin_illuminant[1];
  lms_out[2] = lms_in[2] * D50[2] / origin_illuminant[2];
}

/* Pre-solved matrices to adjust white point for triplets in CIE XYZ 1931 2° observer */

static const dt_colormatrix_t XYZ_D50_to_D65_CAT16
    = { { 9.89466254e-01f, -4.00304626e-02f, 4.40530317e-02f, 0.f },
        { -5.40518733e-03f, 1.00666069e+00f, -1.75551955e-03f, 0.f },
        { -4.03920992e-04f, 1.50768030e-02f, 1.30210211e+00f, 0.f } };

static const dt_colormatrix_t XYZ_D50_to_D65_Bradford
    = { { 0.95547342f, -0.02309845f, 0.06325924f, 0.f },
        { -0.02836971f, 1.00999540f, 0.02104144f, 0.f },
        { 0.01231401f, -0.02050765f, 1.33036593f, 0.f } };

static const dt_colormatrix_t XYZ_D65_to_D50_CAT16
    = { { 1.01085433e+00f, 4.07086103e-02f, -3.41445825e-02f, 0.f },
        { 5.42814201e-03f, 9.93581926e-01f, 1.15592039e-03f, 0.f },
        { 2.50722468e-04f, -1.14918759e-02f, 7.67964947e-01f, 0.f } };

static const dt_colormatrix_t XYZ_D65_to_D50_Bradford
    = { { 1.04792979f, 0.02294687f, -0.05019227f, 0.f },
        { 0.02962781f, 0.99043443f, -0.0170738f, 0.f },
        { -0.00924304f, 0.01505519f, 0.75187428f, 0.f } };

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ_in, XYZ_out:16)
#endif
static inline void XYZ_D50_to_D65(const dt_aligned_pixel_t XYZ_in, dt_aligned_pixel_t XYZ_out)
{
  dot_product(XYZ_in, XYZ_D50_to_D65_CAT16, XYZ_out);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ_in, XYZ_out:16)
#endif
static inline void XYZ_D65_to_D50(const dt_aligned_pixel_t XYZ_in, dt_aligned_pixel_t XYZ_out)
{
  dot_product(XYZ_in, XYZ_D65_to_D50_CAT16, XYZ_out);
}

/* Helper function to directly chroma-adapt a pixel in CIE XYZ 1931 2° */

static inline void chroma_adapt_pixel(const dt_aligned_pixel_t in, dt_aligned_pixel_t out,
                                      const dt_aligned_pixel_t illuminant, const dt_adaptation_t adaptation, const float p)
{

  // intermediate temp buffers
  dt_aligned_pixel_t temp_one;
  dt_aligned_pixel_t temp_two;

  /* WE START IN XYZ */
  const float Y = in[1];

  switch(adaptation)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    {
      convert_XYZ_to_bradford_LMS(in, temp_two);
      downscale_vector(temp_two, Y);
      bradford_adapt_D50(temp_two, illuminant, p, TRUE, temp_one);
      upscale_vector(temp_one, Y);
      convert_bradford_LMS_to_XYZ(temp_one, out);
      break;
    }
    case DT_ADAPTATION_LINEAR_BRADFORD:
    {
      convert_XYZ_to_bradford_LMS(in, temp_two);
      downscale_vector(temp_two, Y);
      bradford_adapt_D50(temp_two, illuminant, p, FALSE, temp_one);
      upscale_vector(temp_one, Y);
      convert_bradford_LMS_to_XYZ(temp_one, out);
      break;
    }
    case DT_ADAPTATION_CAT16:
    {
      convert_XYZ_to_CAT16_LMS(in, temp_two);
      downscale_vector(temp_two, Y);
      CAT16_adapt_D50(temp_two, illuminant, 1.0f, TRUE, temp_one); // force full-adaptation
      upscale_vector(temp_one, Y);
      convert_CAT16_LMS_to_XYZ(temp_one, out);
      break;
    }
    case DT_ADAPTATION_XYZ:
    {
      for(size_t c = 0; c < DT_PIXEL_SIMD_CHANNELS; ++c) temp_one[c] = in[c];
      downscale_vector(temp_one, Y);
      XYZ_adapt_D50(temp_one, illuminant, temp_two);
      upscale_vector(temp_two, Y);
      for(size_t c = 0; c < DT_PIXEL_SIMD_CHANNELS; ++c) out[c] = temp_two[c];
      break;
    }
    case DT_ADAPTATION_RGB:
    case DT_ADAPTATION_LAST:
    default:
    {
      // No white balance.
      for(size_t c = 0; c < DT_PIXEL_SIMD_CHANNELS; ++c) out[c] = in[c];
      break;
    }
  }
}

/* Helper to get the D50 white point coordinates in LMS spaces */
static inline void convert_D50_to_LMS(const dt_adaptation_t adaptation, dt_aligned_pixel_t D50)
{
  switch(adaptation)
  {
    case DT_ADAPTATION_FULL_BRADFORD:
    case DT_ADAPTATION_LINEAR_BRADFORD:
    {
      D50[0] = 0.996078f;
      D50[1] = 1.020646f;
      D50[2] = 0.818155f;
      D50[3] = 0.f;
      break;
    }
    case DT_ADAPTATION_CAT16:
    {
      D50[0] = 0.994535f;
      D50[1] = 1.000997f;
      D50[2] = 0.833036f;
      D50[3] = 0.f;
      break;
    }
    case DT_ADAPTATION_XYZ:
    {
      D50[0] = 0.9642119944211994f;
      D50[1] = 1.0f;
      D50[2] = 0.8251882845188288f;
      D50[3] = 0.f;
      break;
    }
    case DT_ADAPTATION_RGB:
    case DT_ADAPTATION_LAST:
    default:
    {
      D50[0] = 1.f;
      D50[1] = 1.f;
      D50[2] = 1.f;
      D50[3] = 0.f;
      break;
    }
  }
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

