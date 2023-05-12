/*
 *    This file is part of darktable,
 *    Copyright (C) 2019-2023 darktable developers.
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

#include "common/iop_profile.h"

 typedef enum dt_iop_rgb_norms_t
 {
   DT_RGB_NORM_NONE = 0,      // $DESCRIPTION: "none"
   DT_RGB_NORM_LUMINANCE = 1, // $DESCRIPTION: "luminance"
   DT_RGB_NORM_MAX = 2,       // $DESCRIPTION: "max RGB"
   DT_RGB_NORM_AVERAGE = 3,   // $DESCRIPTION: "average RGB"
   DT_RGB_NORM_SUM = 4,       // $DESCRIPTION: "sum RGB"
   DT_RGB_NORM_NORM = 5,      // $DESCRIPTION: "norm RGB"
   DT_RGB_NORM_POWER = 6,     // $DESCRIPTION: "basic power"
 } dt_iop_rgb_norms_t;

static inline float dt_rgb_norm(const float *in, const int norm, const dt_iop_order_iccprofile_info_t *const work_profile)
{
  if (norm == DT_RGB_NORM_LUMINANCE)
  {
    // TODO: unpack work_profile members higher, at loop level, to enable more optimizations
    return (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(in,
                                                              work_profile->matrix_in,
                                                              work_profile->lut_in,
                                                              work_profile->unbounded_coeffs_in,
                                                              work_profile->lutsize,
                                                              work_profile->nonlinearlut)
                          : dt_camera_rgb_luminance(in);
  }
  else if (norm == DT_RGB_NORM_MAX)
  {
    return fmaxf(in[0], fmaxf(in[1], in[2]));
  }
  else if (norm == DT_RGB_NORM_AVERAGE)
  {
    return (in[0] + in[1] + in[2]) / 3.0f;
  }
  else if (norm == DT_RGB_NORM_SUM)
  {
    return in[0] + in[1] + in[2];
  }
  else if (norm == DT_RGB_NORM_NORM)
  {
    return sqrtf(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
  }
  else if (norm == DT_RGB_NORM_POWER)
  {
    float R, G, B;
    R = in[0] * in[0];
    G = in[1] * in[1];
    B = in[2] * in[2];
    return (in[0] * R + in[1] * G + in[2] * B) / (R + G + B);
  }
  else return (in[0] + in[1] + in[2]) / 3.0f;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

