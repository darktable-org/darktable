/*
    This file is part of darktable,
    copyright (c) 2025 darktable developer.

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

#include "common.h"

#define CAPTURE_KERNEL_ALIGN 32
#define CAPTURE_YMIN 0.001f
#define CAPTURE_SMALL_ULIM 66

__kernel void kernel_9x9_mul(global float *in,
                             global float *out,
                             global float *blend,
                             global float *kernels,
                             global unsigned char *table,
                             const int w1,
                             const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= w1 || row >= height) return;

  const int i = mad24(row, w1, col);
  const int w2 = 2 * w1;
  const int w3 = 3 * w1;
  const int w4 = 4 * w1;
  if(blend[i] <= 0.0f) return;

  global const float *kern = kernels + CAPTURE_KERNEL_ALIGN * table[i];
  global float *d = in + i;
  const bool small = table[i] < CAPTURE_SMALL_ULIM;
  const int bd = small ? 2 : 4;

  float val = 0.0f;
  if(col >= bd && row >= bd && (col < w1 - bd) && (row < height - bd))
  {
    if(small)
    {
      val =
          kern[ 5+2] * (d[-w2-1]  + d[-w2+1]  + d[-w1-2]  + d[-w1+2] + d[w1-2] + d[w1+2] + d[w2-1] + d[w2+1]) +
          kern[   2] * (d[-w2  ]  + d[   -2]  + d[    2]  + d[ w2  ]) +
          kern[ 5+1] * (d[-w1-1]  + d[-w1+1]  + d[ w1-1]  + d[ w1+1]) +
          kern[   1] * (d[-w1  ]  + d[   -1]  + d[    1]  + d[ w1  ]) +
          kern[   0] * (d[0]);
    }
    else
    {
      val =
          kern[10+4] * (d[-w4-2]  + d[-w4+2]  + d[-w2-4]  + d[-w2+4] + d[w2-4] + d[w2+4] + d[w4-2] + d[w4+2]) +
          kern[5 +4] * (d[-w4-1]  + d[-w4+1]  + d[-w1-4]  + d[-w1+4] + d[w1-4] + d[w1+4] + d[w4-1] + d[w4+1]) +
          kern[4]    * (d[-w4  ]  + d[   -4]  + d[    4]  + d[ w4  ]) +
          kern[15+3] * (d[-w3-3]  + d[-w3+3]  + d[ w3-3]  + d[ w3+3]) +
          kern[10+3] * (d[-w3-2]  + d[-w3+2]  + d[-w2-3]  + d[-w2+3] + d[w2-3] + d[w2+3] + d[w3-2] + d[w3+2]) +
          kern[ 5+3] * (d[-w3-1]  + d[-w3+1]  + d[-w1-3]  + d[-w1+3] + d[w1-3] + d[w1+3] + d[w3-1] + d[w3+1]) +
          kern[   3] * (d[-w3  ]  + d[   -3]  + d[    3]  + d[ w3  ]) +
          kern[10+2] * (d[-w2-2]  + d[-w2+2]  + d[ w2-2]  + d[ w2+2]) +
          kern[ 5+2] * (d[-w2-1]  + d[-w2+1]  + d[-w1-2]  + d[-w1+2] + d[w1-2] + d[w1+2] + d[w2-1] + d[w2+1]) +
          kern[   2] * (d[-w2  ]  + d[   -2]  + d[    2]  + d[ w2  ]) +
          kern[ 5+1] * (d[-w1-1]  + d[-w1+1]  + d[ w1-1]  + d[ w1+1]) +
          kern[   1] * (d[-w1  ]  + d[   -1]  + d[    1]  + d[ w1  ]) +
          kern[   0] * (d[0]);
    }
  }
  else
  {
    for(int ir = -bd; ir <= bd; ir++)
    {
      const int irow = row+ir;
      if(irow >= 0 && irow < height)
      {
        for(int ic = -bd; ic <= bd; ic++)
        {
          const int icol = col+ic;
          if(icol >=0 && icol < w1)
            val += kern[5 * abs(ir) + abs(ic)] * in[mad24(irow, w1, icol)];
        }
      }
    }
  }
  out[i] *= val;
}

__kernel void kernel_9x9_div(global float *in,
                             global float *out,
                             global float *luminance,
                             global float *blend,
                             global float *kernels,
                             global unsigned char *table,
                             const int w1,
                             const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= w1 || row >= height) return;

  const int i = mad24(row, w1, col);
  const int w2 = 2 * w1;
  const int w3 = 3 * w1;
  const int w4 = 4 * w1;
  if(blend[i] <= 0.0f) return;

  global const float *kern = kernels + CAPTURE_KERNEL_ALIGN * table[i];
  global float *d = in + i;
  const bool small = table[i] < CAPTURE_SMALL_ULIM;
  const int bd = small ? 2 : 4;

  float val = 0.0f;
  if(col >= bd && row >= bd && (col < w1 - bd) && (row < height - bd))
  {
    if(small)
    {
      val =
          kern[ 5+2] * (d[-w2-1]  + d[-w2+1]  + d[-w1-2]  + d[-w1+2] + d[w1-2] + d[w1+2] + d[w2-1] + d[w2+1]) +
          kern[   2] * (d[-w2  ]  + d[   -2]  + d[    2]  + d[ w2  ]) +
          kern[ 5+1] * (d[-w1-1]  + d[-w1+1]  + d[ w1-1]  + d[ w1+1]) +
          kern[   1] * (d[-w1  ]  + d[   -1]  + d[    1]  + d[ w1  ]) +
          kern[   0] * (d[0]);
    }
    else
    {
      val =
          kern[10+4] * (d[-w4-2]  + d[-w4+2]  + d[-w2-4]  + d[-w2+4] + d[w2-4] + d[w2+4] + d[w4-2] + d[w4+2]) +
          kern[5 +4] * (d[-w4-1]  + d[-w4+1]  + d[-w1-4]  + d[-w1+4] + d[w1-4] + d[w1+4] + d[w4-1] + d[w4+1]) +
          kern[4]    * (d[-w4  ]  + d[   -4]  + d[    4]  + d[ w4  ]) +
          kern[15+3] * (d[-w3-3]  + d[-w3+3]  + d[ w3-3]  + d[ w3+3]) +
          kern[10+3] * (d[-w3-2]  + d[-w3+2]  + d[-w2-3]  + d[-w2+3] + d[w2-3] + d[w2+3] + d[w3-2] + d[w3+2]) +
          kern[ 5+3] * (d[-w3-1]  + d[-w3+1]  + d[-w1-3]  + d[-w1+3] + d[w1-3] + d[w1+3] + d[w3-1] + d[w3+1]) +
          kern[   3] * (d[-w3  ]  + d[   -3]  + d[    3]  + d[ w3  ]) +
          kern[10+2] * (d[-w2-2]  + d[-w2+2]  + d[ w2-2]  + d[ w2+2]) +
          kern[ 5+2] * (d[-w2-1]  + d[-w2+1]  + d[-w1-2]  + d[-w1+2] + d[w1-2] + d[w1+2] + d[w2-1] + d[w2+1]) +
          kern[   2] * (d[-w2  ]  + d[   -2]  + d[    2]  + d[ w2  ]) +
          kern[ 5+1] * (d[-w1-1]  + d[-w1+1]  + d[ w1-1]  + d[ w1+1]) +
          kern[   1] * (d[-w1  ]  + d[   -1]  + d[    1]  + d[ w1  ]) +
          kern[   0] * (d[0]);
    }
  }
  else
  {
    for(int ir = -bd; ir <= bd; ir++)
    {
      const int irow = row+ir;
      if(irow >= 0 && irow < height)
      {
        for(int ic = -bd; ic <= bd; ic++)
        {
          const int icol = col+ic;
          if(icol >=0 && icol < w1)
            val += kern[5 * abs(ir) + abs(ic)] * in[mad24(irow, w1, icol)];
        }
      }
    }
  }
  out[i] = luminance[i] / fmax(val, CAPTURE_YMIN);
}

__kernel void prefill_clip_mask(global float *mask,
                                const int width,
                                const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int i = mad24(row, width, col);
  mask[i] = 1.0f;
}

__kernel void prepare_blend(__read_only image2d_t cfa,
                            __read_only image2d_t dev_out,
                            const int filters,
                            global const unsigned char (*const xtrans)[6],
                            global float *mask,
                            global float *Yold,
                            global float *whites,
                            const int w,
                            const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= w || row >= height) return;

  float4 rgb = read_imagef(dev_out, samplerA, (int2)(col, row));
  // Photometric/digital ITU BT.709
  const float4 flum = (float4)( 0.212671f, 0.715160f, 0.072169f, 0.0f );
  rgb *= flum;
  const float Y = fmax(0.0f, rgb.x + rgb.y + rgb.z);
  const int k = mad24(row, w, col);
  Yold[k] = Y;

  if(row > 1 && col > 1 && (row < height-2) && (col < w -2))
  {
    const int w2 = 2 * w;
    const int color = (filters == 9u) ? FCxtrans(row, col, xtrans) : FC(row, col, filters);
    const float val = read_imagef(cfa, samplerA, (int2)(col, row)).x;
    if(val > whites[color] || Y < CAPTURE_YMIN)
    {
      mask[k-w2-1] = mask[k-w2]  = mask[k-w2+1] =
      mask[k-w-2]  = mask[k-w-1] = mask[k-w ]   = mask[k-w+1] = mask[k-w+2] =
      mask[k-2]    = mask[k-1]   = mask[k]      = mask[k+1]   = mask[k+2] =
      mask[k+w-2]  = mask[k+w-1] = mask[k+w]    = mask[k+w+1] = mask[k+w+2] =
      mask[k+w2-1] = mask[k+w2]  = mask[k+w2+1] = 0.0f;
    }
  }
  else
    mask[k] = 0.0f;
}

__kernel void modify_blend(global float *blend,
                           global float *Yold,
                           global float *luminance,
                           const float dthresh,
                           const int width,
                           const int height)
{
  const int icol = get_global_id(0);
  const int irow = get_global_id(1);
  if(icol >= width || irow >= height) return;

  const int row = clamp(irow, 2, height-3);
  const int col = clamp(icol, 2, width-3);

  const float threshold = 0.6f * fsquare(dthresh);
  const float tscale = 200.0f;
  const float offset = -2.5f + tscale * threshold / 2.0f;

  float sum = 0.0f;
  float sum_sq = 0.0f;
  for(int y = row-1; y < row+2; y++)
  {
    for(int x = col-2; x < col+3; x++)
    {
      sum += Yold[mad24(y, width, x)];
      sum_sq += fsquare(Yold[mad24(y, width, x)]);
    }
  }
  for(int x = col-1; x < col+2; x++)
  {
    sum += Yold[mad24(row-2, width, x)];
    sum_sq += fsquare(Yold[mad24(row-2, width, x)]);
    sum += Yold[mad24(row+2, width, x)];
    sum_sq += fsquare(Yold[mad24(row+2, width, x)]);
  }

  const int k = mad24(irow, width, icol);
  const float sum_of_squares = fmax(0.0f, sum_sq - fsquare(sum) / 21.0f);
  const float std_deviation = dtcl_sqrt(sum_of_squares / 21.0f);
  const float modified_coef_variation = std_deviation / dtcl_sqrt(fmax(NORM_MIN, sum / 21.0f));
  const float t = dtcl_log(1.0f + modified_coef_variation);
  const float weight = 1.0f / (1.0f + dtcl_exp(offset - tscale * t));
  blend[k] = clipf(blend[k] * 1.01011f * (weight - 0.01f));
  luminance[k] = Yold[k];
}

__kernel void final_blend(global float *blendmask,
                          global float *unblurred,
                          int pixels)
{
  const int k = get_global_id(0);
  if(k >= pixels) return;

  const float diff = unblurred[k] - blendmask[k];
  const float w_tmp2 = 1.0f / (1.0f + dtcl_exp(5.0f - 10.0f * diff));
  blendmask[k] = clipf(w_tmp2 * unblurred[k] + (1.0f - w_tmp2) * blendmask[k]);
}

__kernel void show_blend_mask(__read_only image2d_t in,
                              __write_only image2d_t out,
                              global float *blend_mask,
                              global unsigned char *sigma_mask,
                              const int width,
                              const int height,
                              const int blender)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  float4 pix = read_imagef(in, samplerA, (int2)(col, row));
  const float blend = blender ? blend_mask[mad24(row, width, col)]
                              : (float)sigma_mask[mad24(row, width, col)] / 255.0f;
  pix.w = blend;
  write_imagef(out, (int2)(col, row), pix);
}

__kernel void capture_result( __read_only image2d_t in,
                              __write_only image2d_t out,
                              global float *blendmask,
                              global float *luminance,
                              global float *tmp,
                              const int width,
                              const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  float4 pix = read_imagef(in, samplerA, (int2)(col, row));
  const int k = mad24(row, width, col);

  if(blendmask[k] > 0.0f)
  {
    const float mixer = clipf(blendmask[k]);
    const float luminance_new = mix(luminance[k], tmp[k], mixer);
    const float4 factor = luminance_new / fmax(luminance[k], CAPTURE_YMIN);
    pix *= factor;
  }
  write_imagef(out, (int2)(col, row), pix);
}
