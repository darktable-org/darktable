/*
    This file is part of darktable,
    copyright (c) 2016 johannes hanika.

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
pad_input(
    read_only  image2d_t input,
    write_only image2d_t padded,
    const int wd,                  // dimensions of input
    const int ht,
    const int max_supp,            // size of border
    const int wd2,                 // padded dimensions
    const int ht2)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  int cx = x - max_supp, cy = y - max_supp;

  if(x >= wd2 || y >= ht2) return;
  // fill boundary with max_supp px:
  if(cx >= wd) cx = wd-1;
  if(cy >= ht) cy = ht-1;
  if(cx < 0) cx = 0;
  if(cy < 0) cy = 0;

  float4 pixel = read_imagef(input, sampleri, (int2)(cx, cy));
  write_imagef (padded, (int2)(x, y), pixel.x*0.01f);
}

float expand_gaussian(
    read_only image2d_t coarse,
    const int i,
    const int j,
    const int wd,
    const int ht)
{
  float c = 0.0f;
  const float w[5] = {1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f};
  const int cx = i/2;
  const int cy = j/2;
  switch((i&1) + 2*(j&1))
  {
    case 0: // both are even, 3x3 stencil
      for(int ii=-1;ii<=1;ii++) for(int jj=-1;jj<=1;jj++)
      {
        float4 pixel = read_imagef(coarse, sampleri, (int2)(cx+ii, cy+jj));
        c += pixel.x*w[2*jj+2]*w[2*ii+2];
      }
      break;
    case 1: // i is odd, 2x3 stencil
      for(int ii=0;ii<=1;ii++) for(int jj=-1;jj<=1;jj++)
      {
        float4 pixel = read_imagef(coarse, sampleri, (int2)(cx+ii, cy+jj));
        c += pixel.x*w[2*jj+2]*w[2*ii+1];
      }
      break;
    case 2: // j is odd, 3x2 stencil
      for(int ii=-1;ii<=1;ii++) for(int jj=0;jj<=1;jj++)
      {
        float4 pixel = read_imagef(coarse, sampleri, (int2)(cx+ii, cy+jj));
        c += pixel.x*w[2*jj+1]*w[2*ii+2];
      }
      break;
    default: // case 3: // both are odd, 2x2 stencil
      for(int ii=0;ii<=1;ii++) for(int jj=0;jj<=1;jj++)
      {
        float4 pixel = read_imagef(coarse, sampleri, (int2)(cx+ii, cy+jj));
        c += pixel.x*w[2*jj+1]*w[2*ii+1];
      }
      break;
  }
  return 4.0f * c;
}

kernel void
gauss_expand(
    read_only  image2d_t coarse, // coarse input
    write_only image2d_t fine,   // upsampled blurry output
    const int wd,                // resolution of fine, also run kernel on fine res
    const int ht)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  int cx = x, cy = y;

  float4 pixel;
  if(x >= wd || y >= ht) return;
  // fill boundary with 1 or 2 px:
  if(wd & 1) { if(x > wd-2) cx = wd-2; }
  else       { if(x > wd-3) cx = wd-3; }
  if(ht & 1) { if(y > ht-2) cy = ht-2; }
  else       { if(y > ht-3) cy = ht-3; }
  if(cx <= 0) cx = 1;
  if(cy <= 0) cy = 1;

  pixel.x = expand_gaussian(coarse, cx, cy, wd, ht);
  write_imagef (fine, (int2)(x, y), pixel);
}

kernel void
gauss_reduce(
    read_only  image2d_t input,  // fine input buffer
    write_only image2d_t coarse, // coarse scale, blurred input buf
    const int wd,                // coarse res (also run this kernel on coarse res only)
    const int ht)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  int cx = x, cy = y;

  float4 pixel;
  if(x >= wd || y >= ht) return;
  // fill boundary with 1 px:
  if(x >= wd-1) cx = wd-2;
  if(y >= ht-1) cy = ht-2;
  if(cx <= 0) cx = 1;
  if(cy <= 0) cy = 1;

  // blur, store only coarse res
  pixel.x = 0.0f;
  const float w[5] = {1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f};
  // direct 5x5 stencil only on required pixels:
  for(int jj=-2;jj<=2;jj++) for(int ii=-2;ii<=2;ii++)
    pixel.x += read_imagef(input, sampleri, (int2)(2*cx+ii, 2*cy+jj)).x * w[ii+2] * w[jj+2];
  write_imagef (coarse, (int2)(x, y), pixel);
}

float laplacian(
    read_only image2d_t coarse, // coarse res gaussian
    read_only image2d_t fine,   // fine res gaussian
    const int i,                // fine index
    const int j,
    const int ci,               // clamped fine index
    const int cj,
    const int wd,               // fine width
    const int ht)               // fine height
{
  const float c = expand_gaussian(coarse, ci, cj, wd, ht);
  return read_imagef(fine, sampleri, (int2)(i, j)).x - c;
}

kernel void
laplacian_assemble(
    read_only  image2d_t input,      // original input buffer, gauss at current fine pyramid level
    read_only  image2d_t output1,    // state of reconstruction, coarse output buffer
    write_only image2d_t output0,    // reconstruction, one level finer, run kernel on this dimension
    read_only  image2d_t buf_g0_l0,  // image2d_array_t only supported in ocl 2.0 :(
    read_only  image2d_t buf_g0_l1,
    read_only  image2d_t buf_g1_l0,
    read_only  image2d_t buf_g1_l1,
    read_only  image2d_t buf_g2_l0,
    read_only  image2d_t buf_g2_l1,
    read_only  image2d_t buf_g3_l0,
    read_only  image2d_t buf_g3_l1,
    read_only  image2d_t buf_g4_l0,
    read_only  image2d_t buf_g4_l1,
    read_only  image2d_t buf_g5_l0,
    read_only  image2d_t buf_g5_l1,
    // read_only  image2d_t buf_g6_l0,
    // read_only  image2d_t buf_g6_l1,
    // read_only  image2d_t buf_g7_l0,
    // read_only  image2d_t buf_g7_l1,
    const int  pw,                   // width and height of the fine buffers (l0)
    const int  ph)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  int i = x, j = y;

  if(x >= pw || y >= ph) return;
  // fill boundary with 1 or 2 px:
  if(pw & 1) { if(x > pw-2) i = pw-2; }
  else       { if(x > pw-3) i = pw-3; }
  if(ph & 1) { if(y > ph-2) j = ph-2; }
  else       { if(y > ph-3) j = ph-3; }
  if(x <= 0) i = 1;
  if(y <= 0) j = 1;

  float4 pixel;
  pixel.x = expand_gaussian(output1, i, j, pw, ph);

  const int num_gamma = 6; // this sucks, have to hardcode for the lack of arrays
  const float v = read_imagef(input, sampleri, (int2)(x, y)).x;
  int hi = 1;
  // what we mean is this:
  // for(;hi<num_gamma-1 && gamma[hi] <= v;hi++);
  for(;hi<num_gamma-1 && ((float)hi+.5f)/(float)num_gamma <= v;hi++);
  int lo = hi-1;
  // const float a = fmin(fmax((v - gamma[lo])/(gamma[hi]-gamma[lo]), 0.0f), 1.0f);
  const float a = fmin(fmax(v*num_gamma - ((float)lo+.5f), 0.0f), 1.0f);
#ifdef AMD
  // See #3756 for further information why this is necessary on AMD using both
  // AMDGPU-Pro and ROCm drivers.
  float r;
  r = select(r, laplacian(buf_g0_l1, buf_g0_l0, x, y, i, j, pw, ph) * (1.0f-a) + laplacian(buf_g1_l1, buf_g1_l0, x, y, i, j, pw, ph) * a, lo == 0);
  r = select(r, laplacian(buf_g1_l1, buf_g1_l0, x, y, i, j, pw, ph) * (1.0f-a) + laplacian(buf_g2_l1, buf_g2_l0, x, y, i, j, pw, ph) * a, lo == 1);
  r = select(r, laplacian(buf_g2_l1, buf_g2_l0, x, y, i, j, pw, ph) * (1.0f-a) + laplacian(buf_g3_l1, buf_g3_l0, x, y, i, j, pw, ph) * a, lo == 2);
  r = select(r, laplacian(buf_g3_l1, buf_g3_l0, x, y, i, j, pw, ph) * (1.0f-a) + laplacian(buf_g4_l1, buf_g4_l0, x, y, i, j, pw, ph) * a, lo == 3);
  r = select(r, laplacian(buf_g4_l1, buf_g4_l0, x, y, i, j, pw, ph) * (1.0f-a) + laplacian(buf_g5_l1, buf_g5_l0, x, y, i, j, pw, ph) * a, lo >= 4);
  pixel.x += r;
#else
  float l0, l1;
  switch(lo)
  { // oh man, this sucks:
    case 0:
      l0 = laplacian(buf_g0_l1, buf_g0_l0, x, y, i, j, pw, ph);
      l1 = laplacian(buf_g1_l1, buf_g1_l0, x, y, i, j, pw, ph);
      break;
    case 1:
      l0 = laplacian(buf_g1_l1, buf_g1_l0, x, y, i, j, pw, ph);
      l1 = laplacian(buf_g2_l1, buf_g2_l0, x, y, i, j, pw, ph);
      break;
    case 2:
      l0 = laplacian(buf_g2_l1, buf_g2_l0, x, y, i, j, pw, ph);
      l1 = laplacian(buf_g3_l1, buf_g3_l0, x, y, i, j, pw, ph);
      break;
    case 3:
      l0 = laplacian(buf_g3_l1, buf_g3_l0, x, y, i, j, pw, ph);
      l1 = laplacian(buf_g4_l1, buf_g4_l0, x, y, i, j, pw, ph);
      break;
    default: //case 4:
      l0 = laplacian(buf_g4_l1, buf_g4_l0, x, y, i, j, pw, ph);
      l1 = laplacian(buf_g5_l1, buf_g5_l0, x, y, i, j, pw, ph);
      break;
    // case 5:
    //   l0 = laplacian(buf_g5_l1, buf_g5_l0, x, y, i, j, pw, ph);
    //   l1 = laplacian(buf_g6_l1, buf_g6_l0, x, y, i, j, pw, ph);
    //   break;
    // default: // case 6:
    //   l0 = laplacian(buf_g6_l1, buf_g6_l0, x, y, i, j, pw, ph);
    //   l1 = laplacian(buf_g7_l1, buf_g7_l0, x, y, i, j, pw, ph);
    //   break;
  }
  pixel.x += l0 * (1.0f-a) + l1 * a;
#endif
  write_imagef (output0, (int2)(x, y), pixel);
}

float curve(
    const float x,
    const float g,
    const float sigma,
    const float shadows,
    const float highlights,
    const float clarity)
{
  const float c = x-g;
  float val;
  const float ssigma = c > 0.0f ? sigma : - sigma;
  const float shadhi = c > 0.0f ? shadows : highlights;
  if (fabs(c) > 2*sigma) val = g + ssigma + shadhi * (c-ssigma); // linear part
  else
  { // blend in via quadratic bezier
    const float t = clamp(c / (2.0f*ssigma), 0.0f, 1.0f);
    const float t2 = t * t;
    const float mt = 1.0f-t;
    val = g + ssigma * 2.0f*mt*t + t2*(ssigma + ssigma*shadhi);
  }
  // midtone local contrast
  val += clarity * c * dt_fast_expf(-c*c/(2.0f*sigma*sigma/3.0f));
  return val;
}

kernel void
process_curve(
    read_only  image2d_t input,
    write_only image2d_t output,
    const float g,
    const float sigma,
    const float shadows,
    const float highlights,
    const float clarity,
    const int wd,
    const int ht)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= wd || y >= ht) return;

  float4 pixel = read_imagef(input, sampleri, (int2)(x, y));
  pixel.x = curve(pixel.x, g, sigma, shadows, highlights, clarity);
  write_imagef (output, (int2)(x, y), pixel);
}

kernel void
write_back(
    read_only  image2d_t input,
    read_only  image2d_t processed,
    write_only image2d_t output,
    const int max_supp,
    const int wd,
    const int ht)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= wd || y >= ht) return;

  float4 pixel = read_imagef(input, sampleri, (int2)(x, y));
  pixel.x = 100.0f*read_imagef(processed, sampleri, (int2)(x+max_supp, y+max_supp)).x;
  write_imagef (output, (int2)(x, y), pixel);
}
