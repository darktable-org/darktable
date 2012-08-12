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
    clamp(p->x/sigma->x, 0, size->x-1),
    clamp(p->y/sigma->y, 0, size->y-1),
    clamp(p->z/sigma->z, 0, size->z-1), 0);
}

void
atomic_add(
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
  while (atom_cmpxchg((global unsigned int *)val, old_val.i, new_val.i) != old_val.i);
}

kernel void
bilateral_splat(
    read_only image2d_t  in,
    global float        *grid,
    const int            width,
    const int            height,
    const int            sizex,
    const int            sizey,
    const int            sizez,
    const float          sigma_s,
    const float          sigma_r)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  // splat into downsampled grid

  float4 size  = (float4)(sizex, sizey, sizez, 0);
  float4 sigma = (float4)(sigma_s, sigma_s, sigma_r, 0);

  const float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  float L = pixel.x;
  float4 p = (float4)(x, y, L, 0);
  float4 gridp = image_to_grid(&p, &size, &sigma);
  int4 gridi = min((int4)gridp, size);
  float4 gridf = gridp - gridi;

  int ox = 1;
  int oy = size.x;
  int oz = size.y*size.x;

  int gi = xi.x + oy*xi.y + oz*xi.z;
  float contrib = 120.0f/(sigma_s*sigma_s);
  atomic_add(grid + gi,          contrib * (1.0f-xf.x) * (1.0f-xf.y) * (1.0f-xf.z));
  atomic_add(grid + gi+ox,       contrib * (     xf.x) * (1.0f-xf.y) * (1.0f-xf.z));
  atomic_add(grid + gi+oy,       contrib * (1.0f-xf.x) * (     xf.y) * (1.0f-xf.z));
  atomic_add(grid + gi+oy+ox,    contrib * (     xf.x) * (     xf.y) * (1.0f-xf.z));
  atomic_add(grid + gi+oz,       contrib * (1.0f-xf.x) * (1.0f-xf.y) * (     xf.z));
  atomic_add(grid + gi+oz+ox,    contrib * (     xf.x) * (1.0f-xf.y) * (     xf.z));
  atomic_add(grid + gi+oz+oy,    contrib * (1.0f-xf.x) * (     xf.y) * (     xf.z));
  atomic_add(grid + gi+oz+oy+ox, contrib * (     xf.x) * (     xf.y) * (     xf.z));
}

kernel void
blur_line(
    global float        *grid,
    const int            width,
    const int            height,
    const int            sizex,
    const int            sizey,
    const int            sizez,
    const float          sigma_s,
    const float          sigma_r)
    float    *buf,
    float    *scratch,
    const int offset1,
    const int offset2,
    const int offset3,
    const int size1,
    const int size2,
    const int size3,
    const float wm2,
    const float wm1,
    const float w0,
    const float w1,
    const float w2)
{
  for(int k=0;k<size1;k++)
  {
    int index = k*offset1;
    for(int j=0;j<size2;j++)
    {
      // need to cache our neighbours because we want to do
      // the convolution in place:
      float *cache = scratch + dt_get_thread_num() * size3;
      for(int i=0;i<size3;i++)
      {
        cache[i] = buf[index];
        index += offset3;
      }
      index -= offset3*size3;
      buf[index]  = w0 * cache[0];
      buf[index] += w1 * cache[1];
      buf[index] += w2 * cache[2];
      index += offset3;
      buf[index]  = wm1* cache[0];
      buf[index] += w0 * cache[1];
      buf[index] += w1 * cache[2];
      buf[index] += w2 * cache[3];
      index += offset3;
      for(int i=2;i<size3-2;i++)
      {
        float sum;
        sum  = wm2* cache[i-2];
        sum += wm1* cache[i-1];
        sum += w0 * cache[i];
        sum += w1 * cache[i+1];
        sum += w2 * cache[i+2];
        buf[index] = sum;
        index += offset3;
      }
      buf[index]  = wm2* cache[size3-4];
      buf[index] += wm1* cache[size3-3];
      buf[index] += w0 * cache[size3-2];
      buf[index] += w1 * cache[size3-1];
      index += offset3;
      buf[index]  = wm2* cache[size3-3];
      buf[index] += wm1* cache[size3-2];
      buf[index] += w0 * cache[size3-1];
      index += offset3;
      index += offset2 - offset3*size3;
    }
  }
}


kernel void
dt_bilateral_blur(
    dt_bilateral_t *b)
{
  // gaussian up to 3 sigma
  blur_line(b->buf, b->scratch, b->size_x*b->size_y, b->size_x, 1,
      b->size_z, b->size_y, b->size_x,
      1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f);
  // gaussian up to 3 sigma
  blur_line(b->buf, b->scratch, b->size_x*b->size_y, 1, b->size_x,
      b->size_z, b->size_x, b->size_y,
      1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f);
  // -2 derivative of the gaussian up to 3 sigma: x*exp(-x*x)
  blur_line(b->buf, b->scratch, 1, b->size_x, b->size_x*b->size_y,
      b->size_x, b->size_y, b->size_z,
      -2.0f*1.0f/16.0f, -1.0f*4.0f/16.0f, 0.0f, 1.0f*4.0f/16.0f, 2.0f*1.0f/16.0f);
}


kernel void
dt_bilateral_slice(
    const dt_bilateral_t *const b,
    const float          *const in,
    float                *out,
    const float           detail)
{
  // detail: 0 is leave as is, -1 is bilateral filtered, +1 is contrast boost
  const float norm = -detail;
  const int ox = 1;
  const int oy = b->size_x;
  const int oz = b->size_y*b->size_x;
  for(int j=0;j<b->height;j++)
  {
    int index = 4*j*b->width;
    for(int i=0;i<b->width;i++)
    {
      float x, y, z;
      const float L = in[index];
      image_to_grid(b, i, j, L, &x, &y, &z);
      // trilinear lookup:
      const int xi = MIN((int)x, b->size_x-2);
      const int yi = MIN((int)y, b->size_y-2);
      const int zi = MIN((int)z, b->size_z-2);
      const float xf = x - xi;
      const float yf = y - yi;
      const float zf = z - zi;
      const int gi = xi + b->size_x*(yi + b->size_y*zi);
      const float Ldiff =
        b->buf[gi]          * (1.0f - xf) * (1.0f - yf) * (1.0f - zf) +
        b->buf[gi+ox]       * (       xf) * (1.0f - yf) * (1.0f - zf) +
        b->buf[gi+oy]       * (1.0f - xf) * (       yf) * (1.0f - zf) +
        b->buf[gi+ox+oy]    * (       xf) * (       yf) * (1.0f - zf) +
        b->buf[gi+oz]       * (1.0f - xf) * (1.0f - yf) * (       zf) +
        b->buf[gi+ox+oz]    * (       xf) * (1.0f - yf) * (       zf) +
        b->buf[gi+oy+oz]    * (1.0f - xf) * (       yf) * (       zf) +
        b->buf[gi+ox+oy+oz] * (       xf) * (       yf) * (       zf);
      out[index] = L + norm * Ldiff;
      // and copy color and mask
      out[index+1] = in[index+1];
      out[index+2] = in[index+2];
      out[index+3] = in[index+3];
      index += 4;
    }
  }
}
