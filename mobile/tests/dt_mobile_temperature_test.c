/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "common/whitebalance.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "temperature:%d: check failed: %s\n", __LINE__, #condition); return 1; } } while(0)

int main(void)
{
  /* Golden values from the unmodified desktop temperature.c at 3873450, with
   * tint 1.2. Include both sides of its blackbody/daylight boundary: the original
   * inverse has a discontinuity there and must not be silently approximated. */
  const double reference[][6] = {
    {1901, 1, 0.63737716535180911, 0.09505682882213401, 1901.3525390625, 1.2002291679382324},
    {3200, 1, 0.78530036422414784, 0.41967782628493477, 3199.8251953125, 1.2000184059143066},
    {3999, 1, 0.8252293514485729, 0.63790301736680977, 4101.4248046875, 1.2218893766403198},
    {4000, 0.99654306804815163, 0.83333333333333337, 0.60978161969585798, 3900.521484375, 1.1790174245834351},
    {6500, 0.87327150596258252, 0.76567518736226314, 1, 6500.2861328125, 1.2000021934509277},
    {25000, 0.50422131829049932, 0.42846451547653069, 1, 24999.6484375, 1.2000007629394531}
  };
  for(size_t row = 0; row < sizeof(reference) / sizeof(reference[0]); row++)
  {
    const cmsCIEXYZ xyz = dt_whitebalance_temperature_tint_to_XYZ(reference[row][0], 1.2);
    CHECK(fabs(xyz.X - reference[row][1]) < 1e-12);
    CHECK(fabs(xyz.Y - reference[row][2]) < 1e-12);
    CHECK(fabs(xyz.Z - reference[row][3]) < 1e-12);
    float kelvin, tint;
    dt_whitebalance_XYZ_to_temperature(xyz, &kelvin, &tint);
    CHECK(fabs(kelvin - reference[row][4]) < 0.01);
    CHECK(fabs(tint - reference[row][5]) < 1e-6);
  }
  const cmsCIEXYZ low = dt_whitebalance_temperature_to_XYZ(1901);
  const cmsCIEXYZ clamped_low = dt_whitebalance_temperature_to_XYZ(1000);
  const cmsCIEXYZ high = dt_whitebalance_temperature_to_XYZ(25000);
  const cmsCIEXYZ clamped_high = dt_whitebalance_temperature_to_XYZ(50000);
  CHECK(low.X == clamped_low.X && low.Y == clamped_low.Y && low.Z == clamped_low.Z);
  CHECK(high.X == clamped_high.X && high.Y == clamped_high.Y && high.Z == clamped_high.Z);

  double XYZ_to_CAM[4][3], CAM_to_XYZ[3][4];
  dt_whitebalance_srgb_matrices(XYZ_to_CAM, CAM_to_XYZ);
  const cmsCIEXYZ white = dt_whitebalance_temperature_tint_to_XYZ(6500, 1.2);
  double gains[4];
  dt_whitebalance_XYZ_to_multipliers(XYZ_to_CAM, white, gains);
  CHECK(isfinite(gains[0]) && isfinite(gains[1]) && isfinite(gains[2]));
  CHECK(isinf(gains[3])); /* Inactive RGB coefficient, as on desktop. */
  const cmsCIEXYZ restored = dt_whitebalance_multipliers_to_XYZ(CAM_to_XYZ, gains);
  CHECK(fabs(restored.X - white.X) < 3e-7);
  CHECK(fabs(restored.Y - white.Y) < 3e-7);
  CHECK(fabs(restored.Z - white.Z) < 3e-7);
  gains[3] = NAN; /* Original inactive fourth-channel sentinel. */
  const cmsCIEXYZ with_sentinel = dt_whitebalance_multipliers_to_XYZ(CAM_to_XYZ, gains);
  CHECK(with_sentinel.X == restored.X && with_sentinel.Y == restored.Y && with_sentinel.Z == restored.Z);

  /* Four-channel camera multiplication must include channel 3 rather than
   * accidentally treating every calibration as RGB. */
  const double four_to_xyz[3][4] = { {1, 0, 0, 1}, {0, 1, 0, 2}, {0, 0, 1, 3} };
  const double four_gains[4] = { 1, 2, 4, 8 };
  const cmsCIEXYZ four = dt_whitebalance_multipliers_to_XYZ(four_to_xyz, four_gains);
  CHECK(four.X == 1.125 && four.Y == 0.75 && four.Z == 0.625);
  return 0;
}
