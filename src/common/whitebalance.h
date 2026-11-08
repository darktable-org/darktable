/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <lcms2.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_IOP_LOWEST_TEMPERATURE 1901
#define DT_IOP_HIGHEST_TEMPERATURE 25000
#define DT_IOP_LOWEST_TINT 0.135
#define DT_IOP_HIGHEST_TINT 2.326

/* The temperature module's original CIE spectral integration and binary-search
 * inversion. Tint uses the existing desktop Y scaling convention. */
cmsCIEXYZ dt_whitebalance_temperature_to_XYZ(double kelvin);
cmsCIEXYZ dt_whitebalance_temperature_tint_to_XYZ(double kelvin, double tint);
void dt_whitebalance_XYZ_to_temperature(cmsCIEXYZ xyz, float *kelvin, float *tint);
void dt_whitebalance_XYZ_to_multipliers(const double XYZ_to_CAM[4][3],
                                       cmsCIEXYZ xyz, double mul[4]);
cmsCIEXYZ dt_whitebalance_multipliers_to_XYZ(const double CAM_to_XYZ[3][4],
                                             const double mul[4]);
void dt_whitebalance_srgb_matrices(double XYZ_to_CAM[4][3], double CAM_to_XYZ[3][4]);

#ifdef __cplusplus
}
#endif
