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

const sampler_t sampleri = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP | CLK_FILTER_NEAREST;
const sampler_t samplerf = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP | CLK_FILTER_LINEAR;


float4
image_to_grid(
    const float4 *p,
    const int4   *size,
    const float4 *sigma)
{
  return (float4)(
    clamp(p->x/sigma->x, 0.0f, size->x-1.0f),
    clamp(p->y/sigma->y, 0.0f, size->y-1.0f),
    clamp(p->z/sigma->z, 0.0f, size->z-1.0f), 0.0f);
}

void
atomic_add_f(
    global float *val,
    const  float  delta)
{
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

  do
  {
    old_val.f = *val;
    new_val.f = old_val.f + delta;
  }
  while (atom_cmpxchg ((global unsigned int *)val, old_val.i, new_val.i) != old_val.i);
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
    const float          sigma_r)
#if 0
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  local accum[32*32]; // should be more than local sizes
  const int i = get_local_id(0);
  const int j = get_local_id(1);
  accum[32*j + i] = 0;
  const int startx = get_group_id(0)*get_local_size(0);
  const int starty = get_group_id(1)*get_local_size(1);
}
#endif
#if 1
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  // splat into downsampled grid

  int4   size  = (int4)(sizex, sizey, sizez, 0);
  float4 sigma = (float4)(sigma_s, sigma_s, sigma_r, 0);

  const float4 pixel = read_imagef (in, sampleri, (int2)(x, y));
  float L = pixel.x;
  float4 p = (float4)(x, y, L, 0);
  float4 gridp = image_to_grid(&p, &size, &sigma);
  int4 xi = (int4)(gridp.x, gridp.y, gridp.z, 0);
  float fx = gridp.x - xi.x;
  float fy = gridp.y - xi.y;
  float fz = gridp.z - xi.z;

  int ox = 1;
  int oy = size.x;
  int oz = size.y*size.x;

  int gi = xi.x + oy*xi.y + oz*xi.z;
  float contrib = 100.0f/(sigma_s*sigma_s);
  // FIXME: this is _terribly_ inefficient.
  // this kernel alone takes half a second for just under a megapixel on a GT 540M
  // (as compared to 0.028 seconds for slicing below, which should be about the same)
  // the problem is probably that atomic_add_f always writes to the same address
  // within the same warp, so we have a very bad race condition, resulting in long loops..
  atomic_add_f(grid + gi,          contrib);
#if 0
  atomic_add_f(grid + gi,          contrib * (1.0f-fx) * (1.0f-fy) * (1.0f-fz));
  atomic_add_f(grid + gi+ox,       contrib * (     fx) * (1.0f-fy) * (1.0f-fz));
  atomic_add_f(grid + gi+oy,       contrib * (1.0f-fx) * (     fy) * (1.0f-fz));
  atomic_add_f(grid + gi+oy+ox,    contrib * (     fx) * (     fy) * (1.0f-fz));
  atomic_add_f(grid + gi+oz,       contrib * (1.0f-fx) * (1.0f-fy) * (     fz));
  atomic_add_f(grid + gi+oz+ox,    contrib * (     fx) * (1.0f-fy) * (     fz));
  atomic_add_f(grid + gi+oz+oy,    contrib * (1.0f-fx) * (     fy) * (     fz));
  atomic_add_f(grid + gi+oz+oy+ox, contrib * (     fx) * (     fy) * (     fz));
#endif
}
#endif

kernel void
blur_line_z(
    global float *buf,
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

  const float w1 = 4.f/16.f;
  const float w2 = 2.f/16.f;

  int index = k*offset1 + j*offset2;

  float tmp1 = buf[index];
  buf[index] = w1*buf[index + offset3] + w2*buf[index + 2*offset3];
  index += offset3;
  float tmp2 = buf[index];
  buf[index] = w1*(buf[index + offset3] - tmp1) + w2*buf[index + 2*offset3];
  index += offset3;
  for(int i=2;i<size3-2;i++)
  {
    const float tmp3 = buf[index];
    buf[index] =
      + w1*(buf[index + offset3]   - tmp2)
      + w2*(buf[index + 2*offset3] - tmp1);
    index += offset3;
    tmp1 = tmp2;
    tmp2 = tmp3;
  }
  const float tmp3 = buf[index];
  buf[index] = w1*(buf[index + offset3] - tmp2) - w2*tmp1;
  index += offset3;
  buf[index] = - w1*tmp3 - w2*tmp2;
}

kernel void
blur_line(
    global float *buf,
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

  const float w0 = 6.f/16.f;
  const float w1 = 4.f/16.f;
  const float w2 = 1.f/16.f;
  int index = k*offset1 + j*offset2;

  float tmp1 = buf[index];
  buf[index] = buf[index]*w0 + w1*buf[index + offset3] + w2*buf[index + 2*offset3];
  index += offset3;
  float tmp2 = buf[index];
  buf[index] = buf[index]*w0 + w1*(buf[index + offset3] + tmp1) + w2*buf[index + 2*offset3];
  index += offset3;
  for(int i=2;i<size3-2;i++)
  {
    const float tmp3 = buf[index];
    buf[index] = buf[index]*w0
      + w1*(buf[index + offset3]   + tmp2)
      + w2*(buf[index + 2*offset3] + tmp1);
    index += offset3;
    tmp1 = tmp2;
    tmp2 = tmp3;
  }
  const float tmp3 = buf[index];
  buf[index] = buf[index]*w0 + w1*(buf[index + offset3] + tmp2) + w2*tmp1;
  index += offset3;
  buf[index] = buf[index]*w0 + w1*tmp3 + w2*tmp2;
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
  const float norm = -detail * sigma_r * 0.04;
  const int ox = 1;
  const int oy = sizex;
  const int oz = sizey*sizex;

  int4   size  = (int4)(sizex, sizey, sizez, 0);
  float4 sigma = (float4)(sigma_s, sigma_s, sigma_r, 0);

  float4 pixel = read_imagef (in, sampleri, (int2)(x, y));
  float L = pixel.x;
  float4 p = (float4)(x, y, L, 0);
  float4 gridp = image_to_grid(&p, &size, &sigma);
  int4 gridi = (int4)(gridp.x, gridp.y, gridp.z, 0);
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

