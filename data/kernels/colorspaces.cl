/*
    This file is part of darktable,
    copyright (c) 2019 edgardo hoszowski.

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

#include "color_conversion.h"
#include "colorspace.h"

kernel void
colorspaces_transform_lab_to_rgb_matrix(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
    constant const dt_colorspaces_iccprofile_info_cl_t *const profile_info, read_only image2d_t lut)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  pixel.xyz = Lab_to_XYZ(pixel).xyz;
  pixel = matrix_product(pixel, profile_info->matrix_out);

  if(profile_info->nonlinearlut)
    pixel = apply_trc_out(pixel, profile_info, lut);

  write_imagef(out, (int2)(x, y), pixel);
}

kernel void
colorspaces_transform_rgb_matrix_to_lab(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
    constant const dt_colorspaces_iccprofile_info_cl_t *const profile_info, read_only image2d_t lut)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  if(profile_info->nonlinearlut)
    pixel = apply_trc_in(pixel, profile_info, lut);

  pixel = matrix_product(pixel, profile_info->matrix_in);
  pixel.xyz = XYZ_to_Lab(pixel).xyz;

  write_imagef(out, (int2)(x, y), pixel);
}

kernel void
colorspaces_transform_rgb_matrix_to_rgb(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
    constant const dt_colorspaces_iccprofile_info_cl_t *const profile_info_from, read_only image2d_t lut_from,
    constant const dt_colorspaces_iccprofile_info_cl_t *const profile_info_to, read_only image2d_t lut_to,
    constant const float *const matrix)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  if(profile_info_from->nonlinearlut)
    pixel = apply_trc_in(pixel, profile_info_from, lut_from);

  pixel = matrix_product(pixel, matrix);

  if(profile_info_to->nonlinearlut)
    pixel = apply_trc_out(pixel, profile_info_to, lut_to);

  write_imagef(out, (int2)(x, y), pixel);
}
