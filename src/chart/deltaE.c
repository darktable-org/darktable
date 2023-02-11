/*
 *    This file is part of darktable,
 *    Copyright (C) 2016-2020 darktable developers.
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

#include "deltaE.h"

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif

#include <math.h>


#define DEG2RAD(deg) (deg * M_PI / 180.0)
#define RAD2DEG(rad) (rad * 180.0 / M_PI)

// http://www.brucelindbloom.com/index.html?Eqn_DeltaE_CIE76.html
float dt_colorspaces_deltaE_1976(dt_aligned_pixel_t Lab0, dt_aligned_pixel_t Lab1)
{
  float dE = 0.0;
  for(int i = 0; i < 3; i++)
  {
    float difference = Lab0[i] - Lab1[i];
    dE += difference * difference;
  }
  return sqrtf(dE);
}

// http://www.brucelindbloom.com/index.html?Eqn_DeltaE_CIE2000.html
float dt_colorspaces_deltaE_2000(dt_aligned_pixel_t Lab0, dt_aligned_pixel_t Lab1)
{
  float L_ip = (Lab0[0] + Lab1[0]) * 0.5;
  float C1 = sqrtf(Lab0[1] * Lab0[1] + Lab0[2] * Lab0[2]);
  float C2 = sqrtf(Lab1[1] * Lab1[1] + Lab1[2] * Lab1[2]);
  float C_i = (C1 + C2) * 0.5;
  float G = (1.0 - sqrtf(powf(C_i, 7) / (powf(C_i, 7) + powf(25, 7)))) * 0.5;
  float a1_p = Lab0[1] * (1 + G);
  float a2_p = Lab1[1] * (1 + G);
  float C1_p = sqrtf(a1_p * a1_p + Lab0[2] * Lab0[2]);
  float C2_p = sqrtf(a2_p * a2_p + Lab1[2] * Lab1[2]);
  float C_ip = (C1_p + C2_p) * 0.5;
  float h1_p = RAD2DEG(atan2f(Lab0[2], a1_p));
  if(h1_p < 0) h1_p += 360.0;
  float h2_p = RAD2DEG(atan2f(Lab1[2], a2_p));
  if(h2_p < 0) h2_p += 360.0;
  float H_ip;
  if(fabsf(h1_p - h2_p) > 180.0)
    H_ip = (h1_p + h2_p + 360.0) * 0.5;
  else
    H_ip = (h1_p + h2_p) * 0.5;
  float T = 1.0 - 0.17 * cosf(DEG2RAD(H_ip - 30.0)) + 0.24 * cosf(DEG2RAD(2.0 * H_ip))
            + 0.32 * cosf(DEG2RAD(3.0 * H_ip + 6.0)) - 0.20 * cosf(DEG2RAD(4.0 * H_ip - 63.0));
  float dh_p = h2_p - h1_p;
  if(fabsf(dh_p) > 180.0)
  {
    if(h2_p <= h1_p)
      dh_p += 360.0;
    else
      dh_p -= 360.0;
  }
  float dL_p = Lab1[0] - Lab0[0];
  float dC_p = C2_p - C1_p;
  float dH_p = 2.0 * sqrtf(C1_p * C2_p) * sinf(DEG2RAD(dh_p * 0.5));
  float SL = 1.0 + ((0.015 * (L_ip - 50.0) * (L_ip - 50.0)) / sqrtf(20.0 + (L_ip - 50.0) * (L_ip - 50.0)));
  float SC = 1.0 + 0.045 * C_ip;
  float SH = 1.0 + 0.015 * C_ip * T;
  float dtheta = 30.0 * expf(-1.0 * ((H_ip - 275.0) / 25.0) * ((H_ip - 275.0) / 25.0));
  float RC = 2.0 * sqrtf(powf(C_ip, 7) / (powf(C_ip, 7) + powf(25, 7)));
  float RT = -1.0 * RC * sinf(DEG2RAD(2.0 * dtheta));
  float KL = 1.0;
  float KC = 1.0;
  float KH = 1.0;

  float dE = sqrtf((dL_p / (KL * SL)) * (dL_p / (KL * SL)) + (dC_p / (KC * SC)) * (dC_p / (KC * SC))
                   + (dH_p / (KH * SH)) * (dH_p / (KH * SH)) + RT * (dC_p / (KC * SC)) * (dH_p / (KH * SH)));
  return dE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
