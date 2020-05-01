/*
 *    This file is part of darktable,
 *    Copyright (C) 2019-2020 darktable developers.
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

 typedef enum dt_iop_rgb_norms_t
 {
   DT_RGB_NORM_NONE = 0,
   DT_RGB_NORM_LUMINANCE = 1,
   DT_RGB_NORM_MAX = 2,
   DT_RGB_NORM_AVERAGE = 3,
   DT_RGB_NORM_SUM = 4,
   DT_RGB_NORM_NORM = 5,
   DT_RGB_NORM_POWER = 6
 } dt_iop_rgb_norms_t;

inline float
dt_rgb_norm(const float4 in, const int norm, const int work_profile,
  constant dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t lut)
{
  if (norm == DT_RGB_NORM_LUMINANCE)
  {
    return (work_profile == 0) ? dt_camera_rgb_luminance(in): get_rgb_matrix_luminance(in, profile_info, profile_info->matrix_in, lut);
  }
  else if (norm == DT_RGB_NORM_MAX)
  {
    return max(in.x, max(in.y, in.z));
  }
  else if (norm == DT_RGB_NORM_AVERAGE)
  {
    return (in.x + in.y + in.z) / 3.0f;
  }
  else if (norm == DT_RGB_NORM_SUM)
  {
    return in.x + in.y + in.z;
  }
  else if (norm == DT_RGB_NORM_NORM)
  {
    return native_powr(in.x * in.x + in.y * in.y + in.z * in.z, 0.5f);
  }
  else if (norm == DT_RGB_NORM_POWER)
  {
    float R, G, B;
    R = in.x * in.x;
    G = in.y * in.y;
    B = in.z * in.z;
    return (in.x * R + in.y * G + in.z * B) / (R + G + B);
  }
  else return (in.x + in.y + in.z) / 3.0f;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
