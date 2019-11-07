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

#include "color_conversion.cl"
#include "colorspace.cl"

kernel void
colorspaces_transform_lab_to_rgb_matrix(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
    constant dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t lut)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float4 xyz, RGB;

  xyz = Lab_to_XYZ(pixel);
  RGB = matrix_product(xyz, profile_info->matrix_out);
  pixel.xyz = apply_trc_out(RGB, profile_info, lut).xyz;

  write_imagef(out, (int2)(x, y), pixel);
}

kernel void
colorspaces_transform_rgb_matrix_to_lab(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
    constant dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t lut)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float4 xyz, RGB;

  RGB = apply_trc_in(pixel, profile_info, lut);
  xyz = matrix_product(RGB, profile_info->matrix_in);
  pixel.xyz = XYZ_to_Lab(xyz).xyz;

  write_imagef(out, (int2)(x, y), pixel);
}

kernel void
colorspaces_transform_rgb_matrix_to_rgb(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
    constant dt_colorspaces_iccprofile_info_cl_t *profile_info_from, read_only image2d_t lut_from,
    constant dt_colorspaces_iccprofile_info_cl_t *profile_info_to, read_only image2d_t lut_to)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float4 xyz, linear_rgb;

  linear_rgb = apply_trc_in(pixel, profile_info_from, lut_from);
  xyz = matrix_product(linear_rgb, profile_info_from->matrix_in);
  linear_rgb = matrix_product(xyz, profile_info_to->matrix_out);
  pixel.xyz = apply_trc_out(linear_rgb, profile_info_to, lut_to).xyz;

  write_imagef(out, (int2)(x, y), pixel);
}
