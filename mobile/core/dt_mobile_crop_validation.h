/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_CROP_VALIDATION_H
#define DT_MOBILE_CROP_VALIDATION_H
#include <math.h>

#define DT_MOBILE_CROP_MIN_SIZE 0.01f

/* The desktop crop widget stores right=left+width in float space. Compare
 * minimum edges in that same precision: for example 0.5f..0.51f is valid,
 * although subtracting their exact representations in double is below 0.01f. */
static inline int dt_mobile_crop_valid(float left, float top, float right, float bottom)
{
  return isfinite(left) && isfinite(top) && isfinite(right) && isfinite(bottom)
    && left >= 0.0f && left <= 1.0f && top >= 0.0f && top <= 1.0f
    && right >= 0.0f && right <= 1.0f && bottom >= 0.0f && bottom <= 1.0f
    && right >= (float)(left + DT_MOBILE_CROP_MIN_SIZE)
    && bottom >= (float)(top + DT_MOBILE_CROP_MIN_SIZE);
}
#endif
