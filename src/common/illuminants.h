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

#include "common/colorspaces_inline_conversions.h"

/* Standard CIE illuminants */
typedef enum dt_illuminant_t
{
  DT_ILLUMINANT_PIPE   = 0,    // darktable pipeline D50
  DT_ILLUMINANT_A      = 1,    // incandescent bulb
  DT_ILLUMINANT_D      = 2,    // daylight
  DT_ILLUMINANT_E      = 3,    // equi-energy (x = y)
  DT_ILLUMINANT_F      = 4,    // fluorescent
  DT_ILLUMINANT_LED    = 5,    // LED
  DT_ILLUMINANT_BB     = 6,    // general black body radiator - not CIE standard
  DT_ILLUMINANT_CUSTOM = 7,    // input x and y directly - bypass search
  DT_ILLUMINANT_LAST
} dt_illuminant_t;

typedef enum dt_illuminant_fluo_t
{
  DT_ILLUMINANT_FLUO_F1  = 0,  // Daylight Fluorescent 6430K
  DT_ILLUMINANT_FLUO_F2  = 1,  // Cool White Fluorescent 4230K
  DT_ILLUMINANT_FLUO_F3  = 2,  // White Fluorescent 3450K
  DT_ILLUMINANT_FLUO_F4  = 3,  // Warm White Fluorescent 2940K
  DT_ILLUMINANT_FLUO_F5  = 4,  // Daylight Fluorescent 6350K
  DT_ILLUMINANT_FLUO_F6  = 5,  // Lite White Fluorescent 4150K
  DT_ILLUMINANT_FLUO_F7  = 6,  // D65 simulator, Daylight simulator 6500K
  DT_ILLUMINANT_FLUO_F8  = 7,  // D50 simulator, Sylvania F40 5000K
  DT_ILLUMINANT_FLUO_F9  = 8,  // Cool White Deluxe Fluorescent 4150K
  DT_ILLUMINANT_FLUO_F10 = 9,  // Philips TL85, Ultralume 50 5000K
  DT_ILLUMINANT_FLUO_F11 = 10, // Philips TL84, Ultralume 40 4000K
  DT_ILLUMINANT_FLUO_F12 = 11, // Philips TL83, Ultralume 30 3000K
  DT_ILLUMINANT_FLUO_LAST
} dt_illuminant_fluo_t;

typedef enum dt_illuminant_led_t
{
  DT_ILLUMINANT_LED_B1  = 0,   // phosphor-converted blue 2733K
  DT_ILLUMINANT_LED_B2  = 1,   // phosphor-converted blue 2998K
  DT_ILLUMINANT_LED_B3  = 2,   // phosphor-converted blue 4103K
  DT_ILLUMINANT_LED_B4  = 3,   // phosphor-converted blue 5109K
  DT_ILLUMINANT_LED_B5  = 4,   // phosphor-converted blue 6598K
  DT_ILLUMINANT_LED_BH1 = 5,   // mix of phosphor-converted blue red (blue-hybrid) 2851K
  DT_ILLUMINANT_LED_RGB1= 6,   // mixing of red, green, and blue LEDs 2840K
  DT_ILLUMINANT_LED_V1  = 7,   // phosphor-converted violet 2724K
  DT_ILLUMINANT_LED_V2  = 8,   // phosphor-converted violet 4070K
  DT_ILLUMINANT_LED_LAST
} dt_illuminant_led_t;


/**
 * References:
 *  https://en.wikipedia.org/wiki/Planckian_locus
 *  https://en.wikipedia.org/wiki/Standard_illuminant
 *
 * Note : 
 *  All values are x and y chromaticities for CIE 1931 2° observer
 **/

                                                            //   x_2  //   y_2
 static float fluorescent[DT_ILLUMINANT_FLUO_LAST][2] = { { 0.31310f, 0.33727f },   // DT_ILLUMINANT_FLUO_F1
                                                          { 0.37208f, 0.37529f },   // DT_ILLUMINANT_FLUO_F2
                                                          { 0.40910f, 0.39430f },   // DT_ILLUMINANT_FLUO_F3
                                                          { 0.44018f, 0.40329f },   // DT_ILLUMINANT_FLUO_F4
                                                          { 0.31379f, 0.34531f },   // DT_ILLUMINANT_FLUO_F5
                                                          { 0.37790f, 0.38835f },   // DT_ILLUMINANT_FLUO_F6
                                                          { 0.31292f, 0.32933f },   // DT_ILLUMINANT_FLUO_F7
                                                          { 0.34588f, 0.35875f },   // DT_ILLUMINANT_FLUO_F8
                                                          { 0.37417f, 0.37281f },   // DT_ILLUMINANT_FLUO_F9
                                                          { 0.34609f, 0.35986f },   // DT_ILLUMINANT_FLUO_F10
                                                          { 0.38052f, 0.37713f },   // DT_ILLUMINANT_FLUO_F11
                                                          { 0.43695f, 0.40441f } }; // DT_ILLUMINANT_FLUO_F12
/**
 * FLUORESCENT
 *
 * F1-F6   : standard fluo lamps (two semi-broadband emissions).
 * F4      : used for calibrating the CIE color rendering index (CRI = 51).
 * F7-F9   : broadband / full-spectrum light (multiple phosphors, higher CRI).
 * F10-F12 : three narrowband emissions in the R,G,B regions of the visible spectrum tuned to desired CCT.
 *
 **/
                                                //   x_2  //   y_2
static float led[DT_ILLUMINANT_LED_LAST][2] = { { 0.4560f, 0.4078f },  // DT_ILLUMINANT_LED_B1
                                                { 0.4357f, 0.4012f },  // DT_ILLUMINANT_LED_B2
                                                { 0.3756f, 0.3723f },  // DT_ILLUMINANT_LED_B3
                                                { 0.3422f, 0.3502f },  // DT_ILLUMINANT_LED_B4
                                                { 0.3118f, 0.3236f },  // DT_ILLUMINANT_LED_B5
                                                { 0.4474f, 0.4066f },  // DT_ILLUMINANT_LED_BH1
                                                { 0.4557f, 0.4211f },  // DT_ILLUMINANT_LED_RGB1
                                                { 0.4560f, 0.4548f },  // DT_ILLUMINANT_LED_V1
                                                { 0.3781f, 0.3775f }}; // DT_ILLUMINANT_LED_V2

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float xy_to_CCT(const float x, const float y)
{
  // Try to find correlated color temperature from chromaticity
  // Valid for 3000 K to 50000 K
  // Reference : https://www.usna.edu/Users/oceano/raylee/papers/RLee_AO_CCTpaper.pdf
  // Warning : we throw a number ever if it's grossly off. You need to check the error later.
  const float n = (x - 0.3366f)/(y - 0.1735f);
  return -949.86315f + 6253.80338f * expf(-n / 0.92159f) + 28.70599f * expf(-n / 0.20039f) + 0.00004f * expf(-n / 0.07125f);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void CCT_to_xy_daylight(const float t, float *x, float *y)
{
  // Take correlated color temperature in K and find the closest daylight illuminant in 4000 K - 250000 K
  float x_temp = 0.f;
  float y_temp = 0.f;

  if(t >= 4000.f && t <= 7000.0f)
    x_temp = ((-4.6070e9f / t + 2.9678e6f) / t + 0.09911e3f) / t + 0.244063f;
  else if(t > 7000.f && t <= 25000.f)
    x_temp = ((-2.0064e9f / t + 1.9018e6f) / t + 0.24748e3f) / t + 0.237040f;

  y_temp = (-3.f * x_temp + 2.87f) * x_temp - 0.275f;

  *x = x_temp;
  *y = y_temp;
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void CCT_to_xy_blackbody(const float t, float *x, float *y)
{
  // Take correlated color temperature in K and find the closest blackbody illuminant in 1667 K - 250000 K
  float x_temp = 0.f;
  float y_temp = 0.f;

  if(t >= 1667.f && t <= 4000.f)
    x_temp = ((-0.2661239e9f / t - 0.2343589e6f) / t  + 0.8776956e3f) / t + 0.179910f;
  else if(t > 4000.f && t <= 25000.f)
    x_temp = ((-3.0258469e9f / t + 2.1070379e6f) / t  + 0.2226347e3f) / t + 0.240390f;

  if(t >= 1667.f && t <= 2222.f)
    y_temp = ((-1.1063814f * x_temp - 1.34811020f) * x_temp + 2.18555832f) * x_temp - 0.20219683f;
  else if(t > 2222.f && t <= 4000.f)
    y_temp = ((-0.9549476f * x_temp - 1.37418593f) * x_temp + 2.09137015f) * x_temp - 0.16748867f;
  else if(t > 4000.f && t <= 25000.f)
    y_temp = (( 3.0817580f * x_temp - 5.87338670f) * x_temp + 3.75112997f) * x_temp - 0.37001483f;

  *x = x_temp;
  *y = y_temp;
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void illuminant_xy_to_XYZ(const float x, const float y, float XYZ[3])
{
  XYZ[0] = x / y;             // X
  XYZ[1] = 1.f;               // Y is always 1 by definition, for an illuminant
  XYZ[2] = (1.f - x - y) / y; // Z
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void illuminant_xy_to_RGB(const float x, const float y, float RGB[4])
{
  // Get an sRGB preview of current illuminant
  float XYZ[4];
  illuminant_xy_to_XYZ(x, y, XYZ);

  // Fixme : convert to RGB display space instead of sRGB but first the display profile should be global in dt,
  // not confined to colorout where it gets created/destroyed all the time.
  dt_XYZ_to_Rec709_D65(XYZ, RGB);

  // Handle gamut clipping
  const float max_RGB = fmaxf(fmaxf(RGB[0], RGB[1]), RGB[2]);
  for(int c = 0; c < 3; c++) RGB[c] = fmaxf(RGB[c] / max_RGB, 0.f);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void illuminant_CCT_to_RGB(const float t, float RGB[4])
{
  float x, y;
  if(t > 4000.f)
    CCT_to_xy_daylight(t, &x, &y);
  else
    CCT_to_xy_blackbody(t, &x, &y);

  illuminant_xy_to_RGB(x, y, RGB);
}


static int illuminant_to_xy(const dt_illuminant_t illuminant, // primary type of illuminant
                            float *x_out, float *y_out,       // chromaticity output
                            const float t,                    // temperature in K, if needed
                            const dt_illuminant_fluo_t fluo,  // sub-type of fluorescent illuminant, if needed
                            const dt_illuminant_led_t iled)   // sub-type of led illuminant, if needed
{
  /**
   * Compute the x and y chromaticity coordinates in Yxy spaces for standard illuminants
   *
   * Returns :
   *    TRUE if chromaticity of the requested standard illuminant has been found
   *    FALSE if no valid chromaticity has been found or custom illuminant has been requested
   */

  float x = 0.f;
  float y = 0.f;

  switch(illuminant)
  {
    case DT_ILLUMINANT_PIPE:
    {
      // darktable default pipeline D65
      x = 0.31271f;
      y = 0.32902f;
      break;
    }
    case DT_ILLUMINANT_E:
    {
      // Equi-energy - easy-peasy
      x = y = 1.f / 3.f;
      break;
    }
    case DT_ILLUMINANT_A:
    {
      // Special case of Planckian locus for incandescent tungsten bulbs
      x = 0.44757f;
      y = 0.40745f;
      break;
    }
    case DT_ILLUMINANT_F:
    {
      // Fluorescent lighting - look up
      if(fluo >= DT_ILLUMINANT_FLUO_LAST) break;

      x = fluorescent[fluo][0];
      y = fluorescent[fluo][1];
      break;
    }
    case DT_ILLUMINANT_LED:
    {
      // LED lighting - look up
      if(iled >= DT_ILLUMINANT_LED_LAST) break;

      x = led[iled][0];
      y = led[iled][1];
      break;
    }
    case DT_ILLUMINANT_D:
    {
      // Adjusted Planckian locus for daylight interpolated by cubic splines
      // Model valid for T in [4000 ; 25000] K
      CCT_to_xy_daylight(t, &x, &y);
      if(y != 0.f && x != 0.f) break;
      // else t is out of bounds -> use black body model (next case)
    }
    case DT_ILLUMINANT_BB:
    {
      // General Planckian locus for black body radiator interpolated by cubic splines
      // Model valid for T in [1667 ; 25000] K
      CCT_to_xy_blackbody(t, &x, &y);
      if(y != 0.f && x != 0.f) break;
      // else t is out of bounds -> use custom/original values (next case)
    }
    case DT_ILLUMINANT_CUSTOM: // leave x and y as-is
    case DT_ILLUMINANT_LAST:
    {
      return FALSE;
    }
  }

  if(x != 0.f && y != 0.f)
  {
    *x_out = x;
    *y_out = y;
    return TRUE;
  }
  else
    return FALSE;
}


static inline void WB_coeffs_to_illuminant_xy(const float CAM_to_XYZ[4][3], const float WB[4], float *x, float *y)
{
  // Find the illuminant chromaticity x y from RAW WB coeffs and camera input matrice
  float XYZ[4];
  // Simulate white point, aka convert (1, 1, 1) in camera space to XYZ
  // warning : we multiply the transpose of CAM_to_XYZ  since the pseudoinverse transposes it
  XYZ[0] = CAM_to_XYZ[0][0] / WB[0] + CAM_to_XYZ[1][0] / WB[1] + CAM_to_XYZ[2][0] / WB[2];
  XYZ[1] = CAM_to_XYZ[0][1] / WB[0] + CAM_to_XYZ[1][1] / WB[1] + CAM_to_XYZ[2][1] / WB[2];
  XYZ[2] = CAM_to_XYZ[0][2] / WB[0] + CAM_to_XYZ[1][2] / WB[1] + CAM_to_XYZ[2][2] / WB[2];

  // Matrices white point is D65. We need to convert it for darktable's pipe (D50)
  /*
  static const float DT_ALIGNED_PIXEL D65[4] = { 0.941238f, 1.040633f, 1.088932f, 0.f };
  static const float p = powf(1.088932f / 0.818155f, 0.0834f);

  convert_XYZ_to_bradford_LMS(XYZ, XYZ);
  bradford_adapt_D50(XYZ, D65, p, XYZ);
  convert_bradford_LMS_to_XYZ(XYZ, XYZ);
  */

  // Get white point chromaticity
  XYZ[0] /= XYZ[1];
  XYZ[2] /= XYZ[1];
  XYZ[1] /= XYZ[1];

  *x = XYZ[0] / (XYZ[0] + XYZ[1] + XYZ[2]);
  *y = XYZ[1] / (XYZ[0] + XYZ[1] + XYZ[2]);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float planckian_normal(const float x, const float t)
{
  float n = 0.f;

  // Get the direction of the normal vector to the planckian locus at current temperature
  // This is derivated from CCT_to_xy_blackbody equations
  if(t >= 1667.f && t <= 2222.f)
      n = (-3.3191442f * x - 2.69622040f) * x + 2.18555832f;
  else if(t > 2222.f && t <= 4000.f)
      n = (-2.8648428f * x - 2.74837186f) * x + 2.09137015f;
  else if(t > 4000.f && t < 25000.f)
      n = (9.2452740f * x  - 11.7467734f) * x + 3.75112997f;
  return n;
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void blackbody_xy_to_tinted_xy(const float x, const float y, const float t, const float tint,
                                             float *x_out, float *y_out)
{
  // Move further away from planckian locus in the orthogonal direction, by an amount of "tint"
  const float n = planckian_normal(x, t);
  const float norm = sqrtf(1.f + n * n);
  *x_out = x + tint * n / norm;
  *y_out = y - tint / norm;
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float get_tint_from_tinted_xy(const float x, const float y, const float t)
{
  // Find the distance between planckian locus and arbitrary x y chromaticity in the orthogonal direction
  const float n = planckian_normal(x, t);
  const float norm = sqrtf(1.f + n * n);
  float x_bb, y_bb;
  CCT_to_xy_blackbody(t, &x_bb, &y_bb);
  const float tint = -(y - y_bb) * norm;
  return tint;
}


/*
 * The following defines a custom OpenMP reduction so the reverse-lookup can be fully parallelized without sharing.
 *
 * Reference : https://stackoverflow.com/questions/61821090/parallelization-of-a-reverse-lookup-with-openmp
 *
 * The principle is that each thread has its own private radius and temperature, and find its own local minium radius.
 * Then, at the end of the parallel loops, we fetch all the local minima from each thread, compare them and return
 * the global minimum.
 *
 * This avoids sharing temperature and radius between threads and waiting for thread locks.
 */
typedef struct pair
{
  float radius;
  float temperature;
} pair;

struct pair pair_min(struct pair r, struct pair n)
{
  // r is the current min value, n in the value to compare against it
  if(n.radius < r.radius) return n;
  else return r;
}

// Define a new reduction operation
#ifdef _OPENMP
#pragma omp declare reduction(pairmin:struct pair:omp_out=pair_min(omp_out,omp_in))    \
  initializer(omp_priv = { FLT_MAX, 0.0f })
#endif

static inline float CCT_reverse_lookup(const float x, const float y)
{
  // Find out the closest correlated color temperature (closest point over the planckian locus)
  // for any arbitrary x, y chromaticity, by brute-force reverse-lookup.
  // Note that the LUT computation could be defered somewhere else, and computed once

  static const float T_min = 1667.f;
  static const float T_max = 25000.f;
  static const float T_range = T_max - T_min;
  static const size_t LUT_samples = 1<<16;

  struct pair min_radius = { FLT_MAX, 0.0f };

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(x, y, T_min, T_range, LUT_samples) reduction(pairmin:min_radius)\
  schedule(simd:static)
#endif
  for(size_t i = 0; i < LUT_samples; i++)
  {
    // we need more values for the low temperatures, so we scale the step with a power
    const float step = powf((float)i / (float)(LUT_samples - 1), 4.0f);

    // Current temperature in the lookup range
    const float T = T_min +  step * T_range;

    // Current x, y chromaticity
    float x_bb, y_bb;
    CCT_to_xy_blackbody(T, &x_bb, &y_bb);

    // Compute distance between current planckian chromaticity and input
    const pair radius_tmp = { hypotf((x_bb - x), (y_bb - y)), T };

    // If we found a smaller radius, save it
    min_radius = pair_min(min_radius, radius_tmp);
  }

  return min_radius.temperature;
}
