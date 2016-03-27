/*
    This file is part of darktable,
    copyright (c) 2012 johannes hanika.
    copyright (c) 2015 Ulrich Pegelow.

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

typedef enum dt_iop_colorreconstruct_precedence_t
{
  COLORRECONSTRUCT_PRECEDENCE_NONE,
  COLORRECONSTRUCT_PRECEDENCE_CHROMA,
  COLORRECONSTRUCT_PRECEDENCE_HUE
} dt_iop_colorreconstruct_precedence_t;

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

float2
grid_rescale(
    const int2 pxy,
    const int2 roixy,
    const int2 bxy,
    const float scale)
{
  return convert_float2(roixy + pxy) * scale - convert_float2(bxy);
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
colorreconstruction_zero(
    global float  *grid,
    const  int     width,
    const  int     height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  grid[y * width + x] = 0.0f;
}


kernel void
colorreconstruction_splat(
    read_only image2d_t  in,
    global float        *grid,
    const int            width,
    const int            height,
    const int            sizex,
    const int            sizey,
    const int            sizez,
    const float          sigma_s,
    const float          sigma_r,
    const float          threshold,
    const int            precedence,
    const float4         params,
    local int            *gi,
    local float4         *accum)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int lszx = get_local_size(0);
  const int i = get_local_id(0);
  const int j = get_local_id(1);
  int li = lszx*j + i;

  int4   size  = (int4)(sizex, sizey, sizez, 0);
  float4 sigma = (float4)(sigma_s, sigma_s, sigma_r, 0);

  const float4 pixel = read_imagef (in, samplerc, (int2)(x, y));
  float weight, m;

  switch(precedence)
  {
    case COLORRECONSTRUCT_PRECEDENCE_CHROMA:
      weight = sqrt(pixel.y * pixel.y + pixel.z * pixel.z);
      break;

    case COLORRECONSTRUCT_PRECEDENCE_HUE:
      m = atan2(pixel.z, pixel.y) - params.x;
      // readjust m into [-pi, +pi] interval
      m = m > M_PI_F ? m - 2*M_PI_F : (m < -M_PI_F ? m + 2*M_PI_F : m);
      weight = exp(-m*m/params.y);
      break;
      
    case COLORRECONSTRUCT_PRECEDENCE_NONE:
    default:
      weight = 1.0f;
      break;
  }

  if(x < width && y < height)
  {
    // splat into downsampled grid
    float4 p = (float4)(x, y, pixel.x, 0);
    float4 gridp = image_to_grid(p, size, sigma);
    
    // closest integer splatting:    
    int4 xi = clamp(convert_int4(round(gridp)), 0, size - 1);
   
    // first accumulate into local memory
    gi[li] = xi.x + size.x*xi.y + size.x*size.y*xi.z;
    accum[li] = pixel.x < threshold ? weight * (float4)(pixel.x, pixel.y, pixel.z, 1.0f) : (float4)0.0f;
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
  int oldgi = gi[li];
  float4 tmp = accum[li];
 
  for(int ii=1; ii < lszx && oldgi != -1; ii++)
  {
    li = lszx*j + ii;
    if(gi[li] != oldgi)
    {
      atomic_add_f(grid + 4 * oldgi,              tmp.x);
      atomic_add_f(grid + 4 * oldgi + 1,          tmp.y);      
      atomic_add_f(grid + 4 * oldgi + 2,          tmp.z);
      atomic_add_f(grid + 4 * oldgi + 3,          tmp.w);      
      
      oldgi = gi[li];
      tmp = accum[li];
    }
    else
    {
      tmp = accum[li];
    }
  }

  if(oldgi == -1) return;

  atomic_add_f(grid + 4 * oldgi,              tmp.x);
  atomic_add_f(grid + 4 * oldgi + 1,          tmp.y);      
  atomic_add_f(grid + 4 * oldgi + 2,          tmp.z);
  atomic_add_f(grid + 4 * oldgi + 3,          tmp.w);      
}


kernel void
colorreconstruction_blur_line(
    global const float  *ibuf,
    global float        *obuf,
    const int           offset1,
    const int           offset2,
    const int           offset3,
    const int           size1,
    const int           size2,
    const int           size3)
{
  const int k = get_global_id(0);
  const int j = get_global_id(1);
  if(k >= size1 || j >= size2) return;

  const float w0 = 6.0f/16.0f;
  const float w1 = 4.0f/16.0f;
  const float w2 = 1.0f/16.0f;
  int index = k*offset1 + j*offset2;

  float4 tmp1 = vload4(index, ibuf);
  float4 out = vload4(index, ibuf)*w0 + vload4(index + offset3, ibuf)*w1 + vload4(index + 2*offset3, ibuf)*w2;
  vstore4(out, index, obuf);
  index += offset3;
  float4 tmp2 = vload4(index, ibuf);
  out = vload4(index, ibuf)*w0 + (vload4(index + offset3, ibuf) + tmp1)*w1 + vload4(index + 2*offset3, ibuf)*w2;
  vstore4(out, index, obuf);  
  index += offset3;
  for(int i=2;i<size3-2;i++)
  {
    const float4 tmp3 = vload4(index, ibuf);
    out = vload4(index, ibuf)*w0
        + (vload4(index + offset3, ibuf)   + tmp2)*w1
        + (vload4(index + 2*offset3, ibuf) + tmp1)*w2;
    vstore4(out, index, obuf);        
    index += offset3;
    tmp1 = tmp2;
    tmp2 = tmp3;
  }
  const float4 tmp3 = vload4(index, ibuf);
  out = vload4(index, ibuf)*w0 + (vload4(index + offset3, ibuf) + tmp2)*w1 + tmp1*w2;
  vstore4(out, index, obuf);    
  index += offset3;
  out = vload4(index, ibuf)*w0 + tmp3*w1 + tmp2*w2;
  vstore4(out, index, obuf);
}


kernel void
colorreconstruction_slice(
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
    const float          threshold,
    const int2           bxy,
    const int2           roixy,
    const float          scale)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int ox = 1;
  const int oy = sizex;
  const int oz = sizey*sizex;

  int4   size  = (int4)(sizex, sizey, sizez, 0);
  float4 sigma = (float4)(sigma_s, sigma_s, sigma_r, 0);

  float4 pixel = read_imagef (in, samplerc, (int2)(x, y));
  float blend = clamp(20.0f / threshold * pixel.x - 19.0f, 0.0f, 1.0f);
  float2 pxy = grid_rescale((int2)(x, y), roixy, bxy, scale);
  float4 p = (float4)(pxy.x, pxy.y, pixel.x, 0);
  float4 gridp = image_to_grid(p, size, sigma);
  int4 gridi = min(size - 2, (int4)(gridp.x, gridp.y, gridp.z, 0));
  float fx = gridp.x - gridi.x;
  float fy = gridp.y - gridi.y;
  float fz = gridp.z - gridi.z;

  // trilinear lookup (wouldn't read/write access to 3d textures be cool)
  // could actually use an array of 2d textures, these only require opencl 1.2
  const int gi = gridi.x + sizex*(gridi.y + sizey*gridi.z);
  const float4 opixel =
        vload4(gi, grid)          * (1.0f - fx) * (1.0f - fy) * (1.0f - fz) +
        vload4(gi+ox, grid)       * (       fx) * (1.0f - fy) * (1.0f - fz) +
        vload4(gi+oy, grid)       * (1.0f - fx) * (       fy) * (1.0f - fz) +
        vload4(gi+ox+oy, grid)    * (       fx) * (       fy) * (1.0f - fz) +
        vload4(gi+oz, grid)       * (1.0f - fx) * (1.0f - fy) * (       fz) +
        vload4(gi+ox+oz, grid)    * (       fx) * (1.0f - fy) * (       fz) +
        vload4(gi+oy+oz, grid)    * (1.0f - fx) * (       fy) * (       fz) +
        vload4(gi+ox+oy+oz, grid) * (       fx) * (       fy) * (       fz);
        
  const float opixelx = fmax(opixel.x, 0.01f);
  pixel.y = (opixel.w > 0.0f) ? pixel.y * (1.0f - blend) + opixel.y * pixel.x/opixelx * blend : pixel.y;
  pixel.z = (opixel.w > 0.0f) ? pixel.z * (1.0f - blend) + opixel.z * pixel.x/opixelx * blend : pixel.z;  

  write_imagef (out, (int2)(x, y), pixel);
}

