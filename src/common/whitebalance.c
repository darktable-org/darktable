/*
    This file is part of darktable,
    Copyright (C) 2009-2026 darktable developers.
    SPDX-License-Identifier: GPL-3.0-or-later

    Original temperature.c spectral and camera-multiplier calculations, shared
    by the desktop temperature widgets and the mobile native adapter. Preserve
    the existing desktop tint convention, including its documented limitations.
*/
#include "common/whitebalance.h"
#include <math.h>
#include <stddef.h>
#include <string.h>
#include "external/cie_colorimetric_tables.c"

#define INITIALBLACKBODYTEMPERATURE 4000

/*
 * Spectral power distribution functions
 * https://en.wikipedia.org/wiki/Spectral_power_distribution
 */
typedef double((*spd)(unsigned long int wavelength, double TempK));

/*
 * Bruce Lindbloom, "Spectral Power Distribution of a Blackbody Radiator"
 * http://www.brucelindbloom.com/Eqn_Blackbody.html
 */
static double _spd_blackbody(unsigned long int wavelength, double TempK)
{
  // convert wavelength from nm to m
  const long double lambda = (double)wavelength * 1e-9;

/*
 * these 2 constants were computed using following Sage code:
 *
 * (from http://physics.nist.gov/cgi-bin/cuu/Value?h)
 * h = 6.62606957 * 10^-34 # Planck
 * c= 299792458 # speed of light in vacuum
 * k = 1.3806488 * 10^-23 # Boltzmann
 *
 * c_1 = 2 * pi * h * c^2
 * c_2 = h * c / k
 *
 * print 'c_1 = ', c_1, ' ~= ', RealField(128)(c_1)
 * print 'c_2 = ', c_2, ' ~= ', RealField(128)(c_2)
 */

#define c1 3.7417715246641281639549488324352159753e-16L
#define c2 0.014387769599838156481252937624049081933L

  return (double)(c1 / (powl(lambda, 5) * (expl(c2 / (lambda * TempK)) - 1.0L)));

#undef c2
#undef c1
}

/*
 * Bruce Lindbloom, "Spectral Power Distribution of a CIE D-Illuminant"
 * http://www.brucelindbloom.com/Eqn_DIlluminant.html
 * and https://en.wikipedia.org/wiki/Standard_illuminant#Illuminant_series_D
 */
static double _spd_daylight(unsigned long int wavelength, double TempK)
{
  // Original D65xyY initial value; LCMS computes the daylight white point.
  cmsCIExyY WhitePoint = { 0.31271, 0.32902, 1.0 };

  /*
   * Bruce Lindbloom, "TempK to xy"
   * http://www.brucelindbloom.com/Eqn_T_to_xy.html
   */
  cmsWhitePointFromTemp(&WhitePoint, TempK);

  const double M = (0.0241 + 0.2562 * WhitePoint.x - 0.7341 * WhitePoint.y),
               m1 = (-1.3515 - 1.7703 * WhitePoint.x + 5.9114 * WhitePoint.y) / M,
               m2 = (0.0300 - 31.4424 * WhitePoint.x + 30.0717 * WhitePoint.y) / M;

  const unsigned long int j
      = ((wavelength - cie_daylight_components[0].wavelength)
         / (cie_daylight_components[1].wavelength
            - cie_daylight_components[0].wavelength));

  return (cie_daylight_components[j].S[0] + m1 * cie_daylight_components[j].S[1]
          + m2 * cie_daylight_components[j].S[2]);
}

/*
 * Bruce Lindbloom, "Computing XYZ From Spectral Data (Emissive Case)"
 * http://www.brucelindbloom.com/Eqn_Spect_to_XYZ.html
 */
static cmsCIEXYZ _spectrum_to_XYZ(double TempK, spd I)
{
  cmsCIEXYZ Source = {.X = 0.0, .Y = 0.0, .Z = 0.0 };

  /*
   * Color matching functions
   * https://en.wikipedia.org/wiki/CIE_1931_color_space#Color_matching_functions
   */
  for(size_t i = 0; i < cie_1931_std_colorimetric_observer_count; i++)
  {
    const unsigned long int lambda =
      cie_1931_std_colorimetric_observer[0].wavelength
      + (cie_1931_std_colorimetric_observer[1].wavelength
         - cie_1931_std_colorimetric_observer[0].wavelength) * i;

    const double P = I(lambda, TempK);
    Source.X += P * cie_1931_std_colorimetric_observer[i].xyz.X;
    Source.Y += P * cie_1931_std_colorimetric_observer[i].xyz.Y;
    Source.Z += P * cie_1931_std_colorimetric_observer[i].xyz.Z;
  }

  // normalize so that each component is in [0.0, 1.0] range
  const double _max = fmax(fmax(Source.X, Source.Y), Source.Z);
  Source.X /= _max;
  Source.Y /= _max;
  Source.Z /= _max;

  return Source;
}

// TODO: temperature and tint cannot be disjoined! (here it assumes no tint)
cmsCIEXYZ dt_whitebalance_temperature_to_XYZ(double TempK)
{
  if(TempK < DT_IOP_LOWEST_TEMPERATURE) TempK = DT_IOP_LOWEST_TEMPERATURE;
  if(TempK > DT_IOP_HIGHEST_TEMPERATURE) TempK = DT_IOP_HIGHEST_TEMPERATURE;

  if(TempK < INITIALBLACKBODYTEMPERATURE)
  {
    // if temperature is less than 4000K we use blackbody,
    // because there will be no Daylight reference below 4000K...
    return _spectrum_to_XYZ(TempK, _spd_blackbody);
  }
  else
  {
    return _spectrum_to_XYZ(TempK, _spd_daylight);
  }
}

cmsCIEXYZ dt_whitebalance_temperature_tint_to_XYZ(double TempK, double tint)
{
  cmsCIEXYZ xyz = dt_whitebalance_temperature_to_XYZ(TempK);

  /* Retain the desktop tint convention. It scales Y rather than moving
   * orthogonally to the Planckian locus; this is intentionally the original
   * temperature module algorithm, not a different color-temperature model. */
  xyz.Y /= tint;

  return xyz;
}

// binary search inversion
void dt_whitebalance_XYZ_to_temperature(cmsCIEXYZ XYZ, float *TempK, float *tint)
{
  double maxtemp = DT_IOP_HIGHEST_TEMPERATURE, mintemp = DT_IOP_LOWEST_TEMPERATURE;
  cmsCIEXYZ _xyz;

  for(*TempK = (maxtemp + mintemp) / 2.0;
      (maxtemp - mintemp) > 1.0;
      *TempK = (maxtemp + mintemp) / 2.0)
  {
    _xyz = dt_whitebalance_temperature_to_XYZ(*TempK);
    if(_xyz.Z / _xyz.X > XYZ.Z / XYZ.X)
      maxtemp = *TempK;
    else
      mintemp = *TempK;
  }

  // TODO: Fix this to move orthogonally to planckian locus
  *tint = (_xyz.Y / _xyz.X) / (XYZ.Y / XYZ.X);


  if(*TempK < DT_IOP_LOWEST_TEMPERATURE) *TempK = DT_IOP_LOWEST_TEMPERATURE;
  if(*TempK > DT_IOP_HIGHEST_TEMPERATURE) *TempK = DT_IOP_HIGHEST_TEMPERATURE;
  if(*tint < DT_IOP_LOWEST_TINT) *tint = DT_IOP_LOWEST_TINT;
  if(*tint > DT_IOP_HIGHEST_TINT) *tint = DT_IOP_HIGHEST_TINT;
}

void dt_whitebalance_XYZ_to_multipliers(const double XYZ_to_CAM[4][3],
                                       cmsCIEXYZ xyz, double mul[4])
{
  const double XYZ[3] = { xyz.X, xyz.Y, xyz.Z };
  double CAM[4];
  for(int k = 0; k < 4; k++)
  {
    CAM[k] = 0.0;
    for(int i = 0; i < 3; i++) CAM[k] += XYZ_to_CAM[k][i] * XYZ[i];
  }
  for(int k = 0; k < 4; k++) mul[k] = 1.0 / CAM[k];
}

cmsCIEXYZ dt_whitebalance_multipliers_to_XYZ(const double CAM_to_XYZ[3][4],
                                             const double mul[4])
{
  double CAM[4];
  for(int k = 0; k < 4; k++) CAM[k] = mul[k] > 0.0f ? 1.0 / mul[k] : 0.0f;
  double XYZ[3];
  for(int k = 0; k < 3; k++)
  {
    XYZ[k] = 0.0;
    for(int i = 0; i < 4; i++) XYZ[k] += CAM_to_XYZ[k][i] * CAM[i];
  }
  return (cmsCIEXYZ){ XYZ[0], XYZ[1], XYZ[2] };
}

void dt_whitebalance_srgb_matrices(double XYZ_to_CAM[4][3], double CAM_to_XYZ[3][4])
{
  // sRGB D65, exactly as used by the desktop temperature module.
  const double RGB_to_XYZ[3][4] = { { 0.4124564, 0.3575761, 0.1804375, 0 },
                                    { 0.2126729, 0.7151522, 0.0721750, 0 },
                                    { 0.0193339, 0.1191920, 0.9503041, 0 } };
  const double XYZ_to_RGB[4][3] = { { 3.2404542, -1.5371385, -0.4985314 },
                                    { -0.9692660, 1.8760108, 0.0415560 },
                                    { 0.0556434, -0.2040259, 1.0572252 },
                                    { 0, 0, 0 } };
  memcpy(XYZ_to_CAM, XYZ_to_RGB, sizeof(XYZ_to_RGB));
  memcpy(CAM_to_XYZ, RGB_to_XYZ, sizeof(RGB_to_XYZ));
}
