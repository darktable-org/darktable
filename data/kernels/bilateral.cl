/*
    This file is part of darktable,
    copyright (c) 2012 johannes hanika.

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

#pragma OPENCL EXTENSION cl_khr_global_int32_base_atomics : enable

#include "common.h"

float4
image_to_grid(
    const float4 p,
    const int4   size,
    const float4 sigma)
{
  return (float4)(
    clamp(p.x/sigma.x, 0.0f, size.x-1.0f),
    clamp(p.y/sigma.y, 0.0f, size.y-1.0f),
    clamp(p.z/sigma.z, 0.0f, size.z-1.0f), 0.0f);
}

void
atomic_add_f(
    global float *val,
    const  float  delta)
{
#ifdef NVIDIA_SM_20
  // buys me another 3x--10x over the `algorithmic' improvements in the splat kernel below,
  // depending on configuration (sigma_s and sigma_r)
  float res = 0;
  asm volatile ("atom.global.add.f32 %0, [%1], %2;" : "=f"(res) : "l"(val), "f"(delta));

#else
  union
  {
    float f;
    unsigned int i;
  }
  old_val;
  union
  {
    float f;
    unsigned int i;
  }
  new_val;

  global volatile unsigned int *ival = (global volatile unsigned int *)val;

  do
  {
    // the following is equivalent to old_val.f = *val. however, as according to the opencl standard
    // we can not rely on global buffer val to be consistently cached (relaxed memory consistency) we 
    // access it via a slower but consistent atomic operation.
    old_val.i = atom_add(ival, 0);
    new_val.f = old_val.f + delta;
  }
  while (atom_cmpxchg (ival, old_val.i, new_val.i) != old_val.i);
#endif
}

kernel void
zero(
    global float *grid,
    const  int    width,
    const  int    height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  grid[x + width*y] = 0.0f;
}

kernel void
splat(
    read_only image2d_t  in,
    global float        *grid,
    const int            width,
    const int            height,
    const int            sizex,
    const int            sizey,
    const int            sizez,
    const float          sigma_s,
    const float          sigma_r,
    local int            *gi,
    local float          *accum)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int lszx = get_local_size(0);
  const int i = get_local_id(0);
  const int j = get_local_id(1);
  int li = lszx*j + i;

  int4   size  = (int4)(sizex, sizey, sizez, 0);
  float4 sigma = (float4)(sigma_s, sigma_s, sigma_r, 0);

  int ox = 1;
  int oy = size.x;
  int oz = size.y*size.x;

  if(x < width && y < height)
  {
    // splat into downsampled grid

    const float4 pixel = read_imagef (in, samplerc, (int2)(x, y));
    float L = pixel.x;
    float4 p = (float4)(x, y, L, 0);
    float4 gridp = image_to_grid(p, size, sigma);
    int4 xi = min(size - 2, (int4)(gridp.x, gridp.y, gridp.z, 0));
    float fx = gridp.x - xi.x;
    float fy = gridp.y - xi.y;
    float fz = gridp.z - xi.z;

    // first accumulate into local memory
    gi[li] = xi.x + oy*xi.y + oz*xi.z;
    float contrib = 100.0f/(sigma_s*sigma_s);
    li *= 8;
    accum[li++] = contrib * (1.0f-fx) * (1.0f-fy) * (1.0f-fz);
    accum[li++] = contrib * (     fx) * (1.0f-fy) * (1.0f-fz);
    accum[li++] = contrib * (1.0f-fx) * (     fy) * (1.0f-fz);
    accum[li++] = contrib * (     fx) * (     fy) * (1.0f-fz);
    accum[li++] = contrib * (1.0f-fx) * (1.0f-fy) * (     fz);
    accum[li++] = contrib * (     fx) * (1.0f-fy) * (     fz);
    accum[li++] = contrib * (1.0f-fx) * (     fy) * (     fz);
    accum[li++] = contrib * (     fx) * (     fy) * (     fz);
  }
  else
  {
    gi[li] = -1;
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  if(i != 0) return;

  // non-logarithmic reduction..
  // but we also need to take care of where to accumulate (only merge where gi[.] == gi[.])

  li = lszx*j;
  int lii = 8*li;
  int oldgi = gi[li];
  float tmp[8];

  for(int k=0;k<8;k++)
    tmp[k] = accum[lii+k];
   
  for(int ii=1; ii < lszx && oldgi != -1; ii++)
  {
    li = lszx*j + ii;
    lii = 8*li;
    if(gi[li] != oldgi)
    {
      atomic_add_f(grid + oldgi,          tmp[0]);
      atomic_add_f(grid + oldgi+ox,       tmp[1]);
      atomic_add_f(grid + oldgi+oy,       tmp[2]);
      atomic_add_f(grid + oldgi+oy+ox,    tmp[3]);
      atomic_add_f(grid + oldgi+oz,       tmp[4]);
      atomic_add_f(grid + oldgi+oz+ox,    tmp[5]);
      atomic_add_f(grid + oldgi+oz+oy,    tmp[6]);
      atomic_add_f(grid + oldgi+oz+oy+ox, tmp[7]);

      oldgi = gi[li];
      for(int k=0;k<8;k++)
        tmp[k] = accum[lii+k];
    }
    else
    {
      for(int k=0;k<8;k++)
        tmp[k] += accum[lii+k];
    }
  }

  if(oldgi == -1) return;
  atomic_add_f(grid + oldgi,          tmp[0]);
  atomic_add_f(grid + oldgi+ox,       tmp[1]);
  atomic_add_f(grid + oldgi+oy,       tmp[2]);
  atomic_add_f(grid + oldgi+oy+ox,    tmp[3]);
  atomic_add_f(grid + oldgi+oz,       tmp[4]);
  atomic_add_f(grid + oldgi+oz+ox,    tmp[5]);
  atomic_add_f(grid + oldgi+oz+oy,    tmp[6]);
  atomic_add_f(grid + oldgi+oz+oy+ox, tmp[7]);
}

kernel void
blur_line_z(
    global const float *ibuf,
    global float *obuf,
    const int offset1,
    const int offset2,
    const int offset3,
    const int size1,
    const int size2,
    const int size3)
{
  const int k = get_global_id(0);
  const int j = get_global_id(1);
  if(k >= size1 || j >= size2) return;

  const float w1 = 4.0f/16.0f;
  const float w2 = 2.0f/16.0f;

  int index = k*offset1 + j*offset2;

  float tmp1 = ibuf[index];
  obuf[index] = w1*ibuf[index + offset3] + w2*ibuf[index + 2*offset3];
  index += offset3;
  float tmp2 = ibuf[index];
  obuf[index] = w1*(ibuf[index + offset3] - tmp1) + w2*ibuf[index + 2*offset3];
  index += offset3;
  for(int i=2;i<size3-2;i++)
  {
    const float tmp3 = ibuf[index];
    obuf[index] =
      + w1*(ibuf[index + offset3]   - tmp2)
      + w2*(ibuf[index + 2*offset3] - tmp1);
    index += offset3;
    tmp1 = tmp2;
    tmp2 = tmp3;
  }
  const float tmp3 = ibuf[index];
  obuf[index] = w1*(ibuf[index + offset3] - tmp2) - w2*tmp1;
  index += offset3;
  obuf[index] = - w1*tmp3 - w2*tmp2;
}

kernel void
blur_line(
    global const float *ibuf,
    global float *obuf,
    const int offset1,
    const int offset2,
    const int offset3,
    const int size1,
    const int size2,
    const int size3)
{
  const int k = get_global_id(0);
  const int j = get_global_id(1);
  if(k >= size1 || j >= size2) return;

  const float w0 = 6.0f/16.0f;
  const float w1 = 4.0f/16.0f;
  const float w2 = 1.0f/16.0f;
  int index = k*offset1 + j*offset2;

  float tmp1 = ibuf[index];
  obuf[index] = ibuf[index]*w0 + w1*ibuf[index + offset3] + w2*ibuf[index + 2*offset3];
  index += offset3;
  float tmp2 = ibuf[index];
  obuf[index] = ibuf[index]*w0 + w1*(ibuf[index + offset3] + tmp1) + w2*ibuf[index + 2*offset3];
  index += offset3;
  for(int i=2;i<size3-2;i++)
  {
    const float tmp3 = ibuf[index];
    obuf[index] = ibuf[index]*w0
      + w1*(ibuf[index + offset3]   + tmp2)
      + w2*(ibuf[index + 2*offset3] + tmp1);
    index += offset3;
    tmp1 = tmp2;
    tmp2 = tmp3;
  }
  const float tmp3 = ibuf[index];
  obuf[index] = ibuf[index]*w0 + w1*(ibuf[index + offset3] + tmp2) + w2*tmp1;
  index += offset3;
  obuf[index] = ibuf[index]*w0 + w1*tmp3 + w2*tmp2;
}

kernel void
slice_to_output(
    read_only  image2d_t in,
    read_only  image2d_t target,
    write_only image2d_t out,
    global float        *grid,
    const int            width,
    const int            height,
    const int            sizex,
    const int            sizey,
    const int            sizez,
    const float          sigma_s,
    const float          sigma_r,
    const float          detail)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  // detail: 0 is leave as is, -1 is bilateral filtered, +1 is contrast boost
  const float norm = -detail * sigma_r * 0.04f;
  const int ox = 1;
  const int oy = sizex;
  const int oz = sizey*sizex;

  int4   size  = (int4)(sizex, sizey, sizez, 0);
  float4 sigma = (float4)(sigma_s, sigma_s, sigma_r, 0);

  float4 pixel  = read_imagef (in,   samplerc, (int2)(x, y));
  float4 pixel2 = read_imagef (target, samplerc, (int2)(x, y));
  float L = pixel.x;
  float4 p = (float4)(x, y, L, 0);
  float4 gridp = image_to_grid(p, size, sigma);
  int4 gridi = min(size - 2, (int4)(gridp.x, gridp.y, gridp.z, 0));
  float fx = gridp.x - gridi.x;
  float fy = gridp.y - gridi.y;
  float fz = gridp.z - gridi.z;

  // trilinear lookup (wouldn't read/write access to 3d textures be cool)
  // could actually use an array of 2d textures, these only require opencl 1.2
  const int gi = gridi.x + sizex*(gridi.y + sizey*gridi.z);
  const float Ldiff =
        grid[gi]          * (1.0f - fx) * (1.0f - fy) * (1.0f - fz) +
        grid[gi+ox]       * (       fx) * (1.0f - fy) * (1.0f - fz) +
        grid[gi+oy]       * (1.0f - fx) * (       fy) * (1.0f - fz) +
        grid[gi+ox+oy]    * (       fx) * (       fy) * (1.0f - fz) +
        grid[gi+oz]       * (1.0f - fx) * (1.0f - fy) * (       fz) +
        grid[gi+ox+oz]    * (       fx) * (1.0f - fy) * (       fz) +
        grid[gi+oy+oz]    * (1.0f - fx) * (       fy) * (       fz) +
        grid[gi+ox+oy+oz] * (       fx) * (       fy) * (       fz);
  pixel2.x = max(0.0f, pixel2.x + norm * Ldiff);
  write_imagef (out, (int2)(x, y), pixel2);
}

kernel void
slice(
    read_only  image2d_t in,
    write_only image2d_t out,
    global float        *grid,
    const int            width,
    const int            height,
    const int            sizex,
    const int            sizey,
    const int            sizez,
    const float          sigma_s,
    const float          sigma_r,
    const float          detail)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  // detail: 0 is leave as is, -1 is bilateral filtered, +1 is contrast boost
  const float norm = -detail * sigma_r * 0.04f;
  const int ox = 1;
  const int oy = sizex;
  const int oz = sizey*sizex;

  int4   size  = (int4)(sizex, sizey, sizez, 0);
  float4 sigma = (float4)(sigma_s, sigma_s, sigma_r, 0);

  float4 pixel = read_imagef (in, samplerc, (int2)(x, y));
  float L = pixel.x;
  float4 p = (float4)(x, y, L, 0);
  float4 gridp = image_to_grid(p, size, sigma);
  int4 gridi = min(size - 2, (int4)(gridp.x, gridp.y, gridp.z, 0));
  float fx = gridp.x - gridi.x;
  float fy = gridp.y - gridi.y;
  float fz = gridp.z - gridi.z;

  // trilinear lookup (wouldn't read/write access to 3d textures be cool)
  // could actually use an array of 2d textures, these only require opencl 1.2
  const int gi = gridi.x + sizex*(gridi.y + sizey*gridi.z);
  const float Ldiff =
        grid[gi]          * (1.0f - fx) * (1.0f - fy) * (1.0f - fz) +
        grid[gi+ox]       * (       fx) * (1.0f - fy) * (1.0f - fz) +
        grid[gi+oy]       * (1.0f - fx) * (       fy) * (1.0f - fz) +
        grid[gi+ox+oy]    * (       fx) * (       fy) * (1.0f - fz) +
        grid[gi+oz]       * (1.0f - fx) * (1.0f - fy) * (       fz) +
        grid[gi+ox+oz]    * (       fx) * (1.0f - fy) * (       fz) +
        grid[gi+oy+oz]    * (1.0f - fx) * (       fy) * (       fz) +
        grid[gi+ox+oy+oz] * (       fx) * (       fy) * (       fz);
  pixel.x = L + norm * Ldiff;
  write_imagef (out, (int2)(x, y), pixel);
}

