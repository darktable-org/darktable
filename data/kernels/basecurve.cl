/*
    This file is part of darktable,
    copyright (c) 2016 johannes hanika.
    copyright (c) 2016 Ulrich Pegelow.

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
#include "rgb_norms.h"

/*
  Primary LUT lookup.  Measures the luminance of a given pixel using a selectable function, looks up that
  luminance in the configured basecurve, and then scales each channel by the result.

  Doing it this way avoids the color shifts documented as being possible in the legacy basecurve approach.

  Also applies a multiplier prior to lookup in order to support fusion.  The idea of doing this here is to
  emulate the original use case of enfuse, which was to fuse multiple JPEGs from a camera that was set up
  for exposure bracketing, and which may have had a camera-specific base curve applied.
*/
kernel void
basecurve_lut(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
              const float mul, read_only image2d_t table, constant float *a, const int preserve_colors,
              constant dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t lut,
              const int use_work_profile)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float ratio = 1.f;
  const float lum = mul * dt_rgb_norm(pixel, preserve_colors, use_work_profile, profile_info, lut);
  if(lum > 0.f)
  {
    const float curve_lum = lookup_unbounded(table, lum, a);
    ratio = mul * curve_lum / lum;
  }
  pixel.xyz *= ratio;
  pixel = fmax(pixel, 0.f);

  write_imagef (out, (int2)(x, y), pixel);
}


kernel void
basecurve_zero(write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  write_imagef (out, (int2)(x, y), (float4)0.0f);
}

/*
  Original basecurve implementation.  Applies a LUT on a per-channel basis which can cause color shifts.

  These can be undesirable (skin tone shifts), or sometimes may be desired (fiery sunset).  Continue to allow
  the "old" method but don't make it the default, both for backwards compatibility and for those who are willing
  to take the risks of "artistic" impacts on their image.
*/
kernel void
basecurve_legacy_lut(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                   const float mul, read_only image2d_t table, constant float *a)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  // apply ev multiplier and use lut or extrapolation:
  pixel.x = lookup_unbounded(table, mul * pixel.x, a);
  pixel.y = lookup_unbounded(table, mul * pixel.y, a);
  pixel.z = lookup_unbounded(table, mul * pixel.z, a);
  pixel = fmax(pixel, 0.f);
  write_imagef (out, (int2)(x, y), pixel);
}

kernel void
basecurve_compute_features(read_only image2d_t in, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 value = read_imagef(in, sampleri, (int2)(x, y));

  const float ma = max(value.x, max(value.y, value.z));
  const float mi = min(value.x, min(value.y, value.z));

  const float sat = 0.1f + 0.1f * (ma - mi) / max(1.0e-4f, ma);
  value.w = sat;

  const float c = 0.54f;

  float v = fabs(value.x - c);
  v = max(fabs(value.y - c), v);
  v = max(fabs(value.z - c), v);

  const float var = 0.5f;
  const float e = 0.2f + dt_fast_expf(-v * v / (var * var));

  value.w *= e;

  write_imagef (out, (int2)(x, y), value);
}

constant float gw[5] = { 1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f };

kernel void
basecurve_blur_h(read_only image2d_t in, write_only image2d_t out,
                 const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int rad = 2;
  constant float *w = gw + rad;

  float4 sum = (float4)0.0f;

  for (int i = -rad; i <= rad; i++)
  {
    const int xx = min(max(-x - i, x + i), width - (x + i - width + 1));
    sum += read_imagef(in, sampleri, (int2)(xx, y)) * w[i];
  }

  write_imagef (out, (int2)(x, y), sum);
}


kernel void
basecurve_blur_v(read_only image2d_t in, write_only image2d_t out,
                 const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);


  if(x >= width || y >= height) return;

  const int rad = 2;
  constant float *w = gw + rad;

  float4 sum = (float4)0.0f;

  for (int i = -rad; i <= rad; i++)
  {
    const int yy = min(max(-y - i, y + i), height - (y + i - height + 1));
    sum += read_imagef(in, sampleri, (int2)(x, yy)) * w[i];
  }

  write_imagef (out, (int2)(x, y), sum);
}

kernel void
basecurve_expand(read_only image2d_t in, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  // fill numbers in even pixels, zero odd ones
  float4 pixel = (x % 2 == 0 && y % 2 == 0) ? 4.0f * read_imagef(in, sampleri, (int2)(x / 2, y / 2)) : (float4)0.0f;

  write_imagef (out, (int2)(x, y), pixel);
}

kernel void
basecurve_reduce(read_only image2d_t in, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(2 * x, 2 * y));

  write_imagef (out, (int2)(x, y), pixel);
}

kernel void
basecurve_detail(read_only image2d_t in, read_only image2d_t det, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 input = read_imagef(in, sampleri, (int2)(x, y));
  float4 detail = read_imagef(det, sampleri, (int2)(x, y));

  write_imagef (out, (int2)(x, y), input - detail);
}

kernel void
basecurve_adjust_features(read_only image2d_t in, read_only image2d_t det, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 input = read_imagef(in, sampleri, (int2)(x, y));
  float4 detail = read_imagef(det, sampleri, (int2)(x, y));

  input.w *= 0.1f + sqrt(detail.x * detail.x + detail.y * detail.y + detail.z * detail.z);

  write_imagef (out, (int2)(x, y), input);
}

kernel void
basecurve_blend_gaussian(read_only image2d_t in, read_only image2d_t col, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 comb = read_imagef(in, sampleri, (int2)(x, y));
  float4 collect = read_imagef(col, sampleri, (int2)(x, y));

  comb.xyz += collect.xyz * collect.w;
  comb.w += collect.w;

  write_imagef (out, (int2)(x, y), comb);
}

kernel void
basecurve_blend_laplacian(read_only image2d_t in, read_only image2d_t col, read_only image2d_t tmp, write_only image2d_t out,
                          const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 comb = read_imagef(in, sampleri, (int2)(x, y));
  float4 collect = read_imagef(col, sampleri, (int2)(x, y));
  float4 temp = read_imagef(tmp, sampleri, (int2)(x, y));

  comb.xyz += (collect.xyz - temp.xyz) * collect.w;
  comb.w += collect.w;

  write_imagef (out, (int2)(x, y), comb);
}

kernel void
basecurve_normalize(read_only image2d_t in, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 comb = read_imagef(in, sampleri, (int2)(x, y));

  comb.xyz /= (comb.w > 1.0e-8f) ? comb.w : 1.0f;

  write_imagef (out, (int2)(x, y), comb);
}

kernel void
basecurve_reconstruct(read_only image2d_t in, read_only image2d_t tmp, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 comb = read_imagef(in, sampleri, (int2)(x, y));
  float4 temp = read_imagef(tmp, sampleri, (int2)(x, y));

  comb += temp;

  write_imagef (out, (int2)(x, y), comb);
}

kernel void
basecurve_finalize(read_only image2d_t in, read_only image2d_t comb, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = fmax(read_imagef(comb, sampleri, (int2)(x, y)), 0.f);
  pixel.w = read_imagef(in, sampleri, (int2)(x, y)).w;

  write_imagef (out, (int2)(x, y), pixel);
}
