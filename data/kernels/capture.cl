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
#define CAPTURE_BLEND_EPS 0.01f
#define CAPTURE_YMIN 0.001f
#define CAPTURE_THRESHPOWER 0.15f

static inline float sqrf(float a)
{
  return (a * a);
}

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
  if(blend[i] <= CAPTURE_BLEND_EPS)
     return;

  global const float *kern = kernels + CAPTURE_KERNEL_ALIGN * table[i];
  global float *d = in + i;

  float val = 0.0f;
  if(col >= 4 && row >= 4 && col < w1 - 4 && row < height - 4)
  {
    val = kern[10+4] * (d[-w4-2]  + d[-w4+2]  + d[-w2-4]  + d[-w2+4] + d[w2-4] + d[w2+4] + d[w4-2] + d[w4+2]) +
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
  else
  {
    for(int ir = -4; ir <= 4; ir++)
    {
      const int irow = row+ir;
      if(irow >= 0 && irow < height)
      {
        for(int ic = -4; ic <= 4; ic++)
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
                             global float *divbuff,
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
  if(blend[i] <= CAPTURE_BLEND_EPS)
    return;

  global const float *kern = kernels + CAPTURE_KERNEL_ALIGN * table[i];
  global float *d = in + i;

  float val = 0.0f;
  if(col >= 4 && row >= 4 && col < w1 - 4 && row < height - 4)
  {
    val = kern[10+4] * (d[-w4-2]  + d[-w4+2]  + d[-w2-4]  + d[-w2+4] + d[w2-4] + d[w2+4] + d[w4-2] + d[w4+2]) +
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
  else
  {
    for(int ir = -4; ir <= 4; ir++)
    {
      const int irow = row+ir;
      if(irow >= 0 && irow < height)
      {
        for(int ic = -4; ic <= 4; ic++)
        {
          const int icol = col+ic;
          if(icol >=0 && icol < w1)
            val += kern[5 * abs(ir) + abs(ic)] * in[mad24(irow, w1, icol)];
        }
      }
    }
  }
  out[i] = divbuff[i] / fmax(val, 0.00001f);
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

  const float4 rgb = read_imagef(dev_out, samplerA, (int2)(col, row));
  const float Y = fmax(0.0f, 0.2626f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z);
  const int k = mad24(row, w, col);
  Yold[k] = Y;

  if(row > 1 && col > 1 && row < height-2 && col < w -2)
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
                           const float threshold,
                           const int width,
                           const int height)
{
  const int icol = get_global_id(0);
  const int irow = get_global_id(1);
  if(icol >= width || irow >= height) return;

  const int row = clamp(irow, 2, height-3);
  const int col = clamp(icol, 2, width-3);

  float av = 0.0f;
  for(int y = row-1; y < row+2; y++)
  {
    for(int x = col-2; x < col+3; x++)
      av += Yold[mad24(y, width, x)];
  }
  for(int x = col-1; x < col+2; x++)
  {
    av += Yold[mad24(row-2, width, x)];
    av += Yold[mad24(row+2, width, x)];
  }
  av /= 21.0f;

  float sv = 0.0f;
  for(int y = row-1; y < row+2; y++)
  {
    for(int x = col-2; x < col+3; x++)
      sv += sqrf(Yold[mad24(y, width, x)] - av);
  }
  for(int x = col-2; x < col+3; x++)
  {
    sv+= sqrf(Yold[mad24(row-2, width, x)] - av);
    sv+= sqrf(Yold[mad24(row+2, width, x)] - av);
  }
  sv = dtcl_pow(fmax(0.0f, 5.0f * dtcl_sqrt(sv / 21.f) - threshold), CAPTURE_THRESHPOWER);
  const int k = mad24(irow, width, icol);

  blend[k] *= clamp(sv, 0.0f, 1.0f);
  luminance[k] = Yold[k];
}

__kernel void show_blend_mask(__read_only image2d_t in,
                              __write_only image2d_t out,
                              global float *blend_mask,
                              const int width,
                              const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  float4 pix = read_imagef(in, samplerA, (int2)(col, row));
  const float blend = blend_mask[mad24(row, width, col)];
  pix.w = blend < CAPTURE_BLEND_EPS ? 0.0f : blend;
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

  if(blendmask[k] > CAPTURE_BLEND_EPS)
  {
    const float mixer = clamp(blendmask[k], 0.0f, 1.0f);
    const float lumold = fmax(luminance[k], 0.000001f);
    const float lumtmp = fmax(tmp[k], 0.0000001f);
    const float luminance_new = mix(lumold, lumtmp, mixer);
    const float4 factor = luminance_new / lumold;
    pix = pix * factor;
  }
  write_imagef(out, (int2)(col, row), pix);
}

#undef CAPTURE_KERNEL_ALIGN
