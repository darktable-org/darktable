/*
    This file is part of darktable,
    copyright (c) 2026 darktable developer.

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

__kernel void prepare_blend(__read_only image2d_t cfa,
                            __read_only image2d_t dev_out,
                            const int filters,
                            global const unsigned char (*const xtrans)[6],
                            global float *mask,
                            global float *Yold,
                            const float4 wb,
                            const int w,
                            const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= w || row >= height) return;

  float whites[4] = { wb.x, wb.y, wb.z, wb.w };

  float4 rgb = Areadpixel(dev_out, col, row);
  // Photometric/digital ITU BT.709
  rgb *= (float4)( 0.212671f, 0.715160f, 0.072169f, 0.0f );
  const float Y = fmax(0.0f, rgb.x + rgb.y + rgb.z);
  const int k = mad24(row, w, col);
  Yold[k] = Y;

  if(row > 1 && col > 1 && (row < height-2) && (col < w -2))
  {
    const int w2 = 2 * w;
    const int color = fcol(row, col, filters, xtrans);
    const float val = Areadsingle(cfa, col, row);
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

  float4 pix = fmax(0.0f, Areadpixel(in, col, row));
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

  float4 pix = Areadpixel(in, col, row);
  const int k = mad24(row, width, col);

  if(blendmask[k] > 0.0f)
  {
    const float mixer = clipf(blendmask[k]);
    const float luminance_new = mix(luminance[k], tmp[k], mixer);
    pix *= (float4)(luminance_new / fmax(luminance[k], CAPTURE_YMIN));
  }
  write_imagef(out, (int2)(col, row), pix);
}

// auto radius: sensor value at (row, col) of the tested area, white balance undone
static inline float _radius_val(read_only image2d_t in,
                                global const unsigned char (*const xtrans)[6],
                                const unsigned int filters,
                                const float4 coeff,
                                const int dx,
                                const int dy,
                                const int row,
                                const int col)
{
  const int c = filters == 9u ? FCxtrans(row, col, xtrans) : FC(row, col, filters);
  const float f = c == 0 ? coeff.x : (c == 1 ? coeff.y : (c == 2 ? coeff.z : coeff.w));
  return f * read_imagef(in, sampleri, (int2)(col + dx, row + dy)).x;
}

#define RAWEPS 0.005f
#define lowerLimit 0.01f
#define upperLimit 0.9f
#define V(r, c) _radius_val(in, xtrans, filters, coeff, dx, dy, row + (r), col + (c))

// largest ratio of neighbouring greens at one site, same tests as _calcRadiusBayer()
static inline float _radius_bayer(read_only image2d_t in,
                                  global const unsigned char (*const xtrans)[6],
                                  const unsigned int filters,
                                  const float4 coeff,
                                  const int dx,
                                  const int dy,
                                  const int row,
                                  const int col)
{
  float maxRatio = 1.0f;
  const float val00 = V(0, 0);
  if(val00 <= RAWEPS) return maxRatio;

  const float val1m1 = V(1, -1);
  const float val1p1 = V(1, 1);
  const float maxVal0 = fmax(val00, val1m1);
  if(val1m1 > RAWEPS && maxVal0 > lowerLimit)
  {
    const float minVal = fmin(val00, val1m1);
    if(maxVal0 > maxRatio * minVal)
    {
      const float clip = maxVal0 == val00
                       ? fmax(fmax(V(-1, -1), V(-1, 1)), val1p1)
                       : fmax(fmax(fmax(V(0, -2), val00), V(2, -2)), V(2, 0));
      if(clip < upperLimit)
        maxRatio = maxVal0 / minVal;
    }
  }

  const float maxVal1 = fmax(val00, val1p1);
  if(val1p1 > RAWEPS && maxVal1 > lowerLimit)
  {
    const float minVal = fmin(val00, val1p1);
    if(maxVal1 > maxRatio * minVal)
    {
      const float clip = maxVal1 == val00
                       ? fmax(fmax(V(-1, -1), V(-1, 1)), val1p1)
                       : fmax(fmax(fmax(val00, V(0, 2)), V(2, 0)), V(2, 2));
      if(clip < upperLimit)
        maxRatio = maxVal1 / minVal;
    }
  }
  return maxRatio;
}

static inline float _radius_pair(const float a,
                                 const float b,
                                 const float maxRatio)
{
  const float maxVal = fmax(a, b);
  const float minVal = fmin(a, b);
  return maxVal > lowerLimit && maxVal > maxRatio * minVal ? maxVal / minVal : maxRatio;
}

// same tests as _calcRadiusXtrans()
static inline float _radius_xtrans(read_only image2d_t in,
                                   global const unsigned char (*const xtrans)[6],
                                   const unsigned int filters,
                                   const float4 coeff,
                                   const int dx,
                                   const int dy,
                                   const int row,
                                   const int col)
{
  float maxRatio = 1.0f;
  const float valp1p1 = V(1, 1);
  const bool squareClipped = fmax(fmax(fmax(valp1p1, V(1, 2)), V(2, 1)), V(2, 2)) >= upperLimit;
  const float greenSolitary = V(0, 0);
  if(greenSolitary > RAWEPS && fmax(V(-1, -1), V(-1, 1)) < upperLimit)
  {
    if(greenSolitary < upperLimit)
    {
      const float valp1m1 = V(1, -1);
      if(valp1m1 > RAWEPS && fmax(fmax(fmax(V(1, -2), valp1m1), V(2, -2)), V(1, -1)) < upperLimit)
        maxRatio = _radius_pair(greenSolitary, valp1m1, maxRatio);
      if(valp1p1 > RAWEPS && !squareClipped)
        maxRatio = _radius_pair(greenSolitary, valp1p1, maxRatio);
    }
  }

  if(!squareClipped)
  {
    const float valp2p2 = V(2, 2);
    if(valp2p2 > RAWEPS)
    {
      if(valp1p1 > RAWEPS)
        maxRatio = _radius_pair(valp1p1, valp2p2, maxRatio);
      const float greenSolitaryRight = V(3, 3);
      if(fmax(fmax(greenSolitaryRight, V(4, 2)), V(4, 4)) < upperLimit && greenSolitaryRight > RAWEPS)
        maxRatio = _radius_pair(greenSolitaryRight, valp2p2, maxRatio);
    }
    const float valp1p2 = V(1, 2);
    const float valp2p1 = V(2, 1);
    if(valp2p1 > RAWEPS)
    {
      if(valp1p2 > RAWEPS)
        maxRatio = _radius_pair(valp1p2, valp2p1, maxRatio);
      const float greenSolitaryLeft = V(3, 0);
      if(fmax(fmax(greenSolitaryLeft, V(4, -1)), V(4, 1)) < upperLimit && greenSolitaryLeft > RAWEPS)
        maxRatio = _radius_pair(greenSolitaryLeft, valp2p1, maxRatio);
    }
  }
  return maxRatio;
}

#undef V

// one work item per tested site, each work group writes the largest ratio it found
__kernel void capture_radius(read_only image2d_t in,
                             global const unsigned char (*const xtrans)[6],
                             const unsigned int filters,
                             const float4 coeff,
                             const int dx,
                             const int dy,
                             const int width,
                             const int height,
                             const int startx,
                             const int starty,
                             global float *partial,
                             local float *buffer)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int xlsz = get_local_size(0);
  const int l = mad24(get_local_id(1), xlsz, get_local_id(0));

  float maxRatio = 1.0f;
  if(filters == 9u)
  {
    const int row = starty + 2 + 3 * y;
    const int col = startx + 2 + 3 * x;
    if(row < height - 4 && col < width - 4)
      maxRatio = _radius_xtrans(in, xtrans, filters, coeff, dx, dy, row, col);
  }
  else
  {
    const int row = 4 + y;
    const int col = (filters ? 5 + (FC(row & 1, 0, filters) & 1) : 5) + 2 * x;
    if(row < height - 4 && col < width - 4)
      maxRatio = _radius_bayer(in, xtrans, filters, coeff, dx, dy, row, col);
  }

  buffer[l] = maxRatio;
  barrier(CLK_LOCAL_MEM_FENCE);

  for(int offset = mul24(xlsz, get_local_size(1)) / 2; offset > 0; offset = offset / 2)
  {
    if(l < offset)
      buffer[l] = fmax(buffer[l], buffer[l + offset]);
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  if(l == 0)
  {
    const int xgid = get_group_id(0);
    const int ygid = get_group_id(1);
    const int xgsz = get_num_groups(0);
    partial[mad24(ygid, xgsz, xgid)] = buffer[0];
  }
}

#undef RAWEPS
#undef lowerLimit
#undef upperLimit
