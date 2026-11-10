/*
    This file is part of darktable,
    copyright (c) 2017 edgardo hoszowski.

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

kernel void
dwt_add_img_to_layer(global float4 *img, global float4 *layer, int width, int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int idx = mad24(y, width, x);

  layer[idx] += img[idx];
}

kernel void
dwt_subtract_layer(global float4 *bl, global float4 *bh, int width, int height, const float lpass_mult)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int idx = mad24(y, width, x);

  bh[idx] -= bl[idx];
}

kernel void
dwt_hat_transform_row(global float4 *lpass, global float4 *hpass, int width, int height, const int sc)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(y >= height) return;

  const float hat_mult = 2.f;
  const int size = width;

  if(x < sc)
  {
    const int h_idx = mad24(y, width, x);
    // the right neighbour also needs reflection once 2*sc > size
    const int rightpos = (x + sc < size) ? (x + sc) : (2 * size - 2 - (x + sc));

    lpass[h_idx] = hat_mult * hpass[mad24(y, width, x)] + hpass[mad24(y, width, (sc - x))]
                   + hpass[mad24(y, width, rightpos)];
  }
  else if(x + sc < size)
  {
    const int h_idx = mad24(y, width, x);

    lpass[h_idx] = hat_mult * hpass[mad24(y, width, x)] + hpass[mad24(y, width, (x - sc))]
                   + hpass[mad24(y, width, (x + sc))];
  }
  else if(x < size)
  {
    const int h_idx = mad24(y, width, x);

    lpass[h_idx] = hat_mult * hpass[mad24(y, width, x)] + hpass[mad24(y, width, (x - sc))]
                   + hpass[mad24(y, width, (2 * size - 2 - (x + sc)))];
  }
}

kernel void
dwt_hat_transform_col(global float4 *lpass, int width, int height, const int sc,
                                  global float4 *temp_buffer, const float lpass_mult)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width) return;

  const float hat_mult = 2.f;
  const int size = height;

  if(y < sc)
  {
    // the lower neighbour also needs reflection once 2*sc > size
    const int belowpos = (y + sc < size) ? (y + sc) : (2 * size - 2 - (y + sc));

    temp_buffer[mad24(y, width, x)] = (hat_mult * lpass[mad24(y, width, x)] + lpass[mad24((sc - y), width, x)]
                                       + lpass[mad24(belowpos, width, x)])
                                      * lpass_mult;
  }
  else if(y + sc < size)
  {
    temp_buffer[mad24(y, width, x)] = (hat_mult * lpass[mad24(y, width, x)] + lpass[mad24((y - sc), width, x)]
                                       + lpass[mad24((y + sc), width, x)])
                                      * lpass_mult;
  }
  else if(y < size)
  {
    temp_buffer[mad24(y, width, x)] = (hat_mult * lpass[mad24(y, width, x)] + lpass[mad24((y - sc), width, x)]
                                       + lpass[mad24((2 * size - 2 - (y + sc)), width, x)])
                                      * lpass_mult;
  }
}

// the Bayer path of src/iop/rawdenoise.c with dwt_denoise() of
// src/common/dwt.c. Each of the four CFA channels is denoised as a
// half-size plane: extract, then a vertical and a horizontal pass per
// wavelet band, then scatter back.

// the sensels of channel c, at rows c & 1 and columns (c & 2) >> 1 modulo 2,
// with sqrt() as a variance-stabilizing transform
kernel void
rawdenoise_extract(read_only image2d_t in,
                   global float *fimg,
                   const int c,
                   const int halfwidth,
                   const int halfheight)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= halfwidth || y >= halfheight) return;

  const int row = 2 * y + (c & 1);
  const int col = 2 * x + ((c & 2) >> 1);
  fimg[mad24(y, halfwidth, x)] = sqrt(fmax(0.0f, Areadsingle(in, col, row)));
}

kernel void
rawdenoise_scatter(global const float *fimg,
                   write_only image2d_t out,
                   const int c,
                   const int halfwidth,
                   const int halfheight)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= halfwidth || y >= halfheight) return;

  const int row = 2 * y + (c & 1);
  const int col = 2 * x + ((c & 2) >> 1);
  const float d = fimg[mad24(y, halfwidth, x)];
  write_imagef(out, (int2)(col, row), d * d);
}

kernel void
rawdenoise_clear(global float *buf,
                 const int width,
                 const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  buf[mad24(y, width, x)] = 0.0f;
}

// dwt_denoise_vert_1ch(): rows vscale apart, reflected at the edges
kernel void
rawdenoise_dwt_vert(global const float *in,
                    global float *out,
                    const int width,
                    const int height,
                    const int vscale)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int below = row + vscale < height ? row + vscale : 2 * (height - 1) - (row + vscale);
  const int above = abs(row - vscale);
  out[mad24(row, width, col)] = 2.0f * in[mad24(row, width, col)] + in[mad24(above, width, col)]
                                + in[mad24(below, width, col)];
}

// dwt_denoise_horiz_1ch(): leaves the coarse scale in img and adds the
// thresholded details to accum, which goes back into img after the last band
kernel void
rawdenoise_dwt_horiz(global const float *coarse,
                     global float *img,
                     global float *accum,
                     const int width,
                     const int height,
                     const int hscale,
                     const float thold,
                     const int last)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int k = mul24(row, width);
  const int left = abs(col - hscale);
  const int right = col + hscale < width ? col + hscale : 2 * (width - 1) - (col + hscale);
  const float hat = (2.0f * coarse[k + col] + coarse[k + left] + coarse[k + right]) / 16.0f;
  const float diff = img[k + col] - hat;
  const float sum = accum[k + col] + (fmax(diff - thold, 0.0f) + fmin(diff + thold, 0.0f));
  accum[k + col] = sum;
  img[k + col] = last ? hat + sum : hat;
}
