/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.

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

const sampler_t sampleri =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;
const sampler_t samplerf =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_LINEAR;


float gh(const float f)
{
  // make spread bigger: less smoothing
  const float spread = 100.f;
  return 1.0f/(1.0f + fabs(f)*spread);
}

kernel void
nlmeans (read_only image2d_t in, write_only image2d_t out, const int width, const int height, const int P, const int K, const float nL, const float nC)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int maxx = width - 1;
  const int maxy = height - 1;
  const float4 norm2 = (float4)(nL, nC, nC, 1.0f);

#if 0
  // this is 20s (compared to 29s brute force below) but still unusable:
  // load a block of shared memory, initialize to zero
  local float4 block[32*32];//get_local_size(0)*get_local_size(1)];
  block[get_local_id(0)  + get_local_id(1) * get_local_size(0)] = (float4)0.0f;
  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  // coalesced mem accesses:
  const float4 p1  = read_imagef(in, sampleri, (int2)(x, y));

  // for each shift vector
  for(int kj=-K;kj<=K;kj++)
  {
    for(int ki=-K;ki<=K;ki++)
    {
      const float4 p2  = read_imagef(in, sampleri, (int2)(clamp(x+ki, 0, maxx), clamp(y+kj, 0, maxy)));
      const float4 tmp = (p1 - p2)*norm2;
      const float dist = tmp.x + tmp.y + tmp.z;
      for(int pj=-P;pj<=P;pj++)
      {
        for(int pi=-P;pi<=P;pi++)
        {
          float4 p2 = read_imagef(in, sampleri, (int2)(clamp(x+pi+ki, 0, maxx), clamp(y+pj+kj, 0, maxy)));
          p2.w = dist;
          const int i = get_local_id(0) + pi, j = get_local_id(1) + pj;
          if(i >= 0 && i < get_local_size(0) && j >= 0 && j < get_local_size(1))
          {
            // TODO: for non-linear gh(), this produces results different than the CPU
            block[i + get_local_size(0) * j].x += gh(p2.x);
            block[i + get_local_size(0) * j].y += gh(p2.y);
            block[i + get_local_size(0) * j].z += gh(p2.z);
            block[i + get_local_size(0) * j].w += gh(p2.w);
          }
        }
      }
    }
  }
  // write back normalized shm
  barrier(CLK_LOCAL_MEM_FENCE);
  const float4 tmp = block[get_local_id(0)  + get_local_id(1) * get_local_size(0)];
  tmp.x *= 1.0f/tmp.w;
  tmp.y *= 1.0f/tmp.w;
  tmp.z *= 1.0f/tmp.w;
  write_imagef (out, (int2)(x, y), tmp);
#endif


#if 1
  if(x >= width || y >= height) return;

  const float4 acc   = (float4)(0.0f);
  // brute force (superslow baseline)!
  // for each shift vector
  for(int kj=-K;kj<=K;kj++)
  {
    for(int ki=-K;ki<=K;ki++)
    {
      float dist = 0.0f;
      for(int pj=-P;pj<=P;pj++)
      {
        for(int pi=-P;pi<=P;pi++)
        {
          float4 p1  = read_imagef(in, sampleri, (int2)(clamp(x+pi, 0, maxx), clamp(y+pj, 0, maxy)));
          float4 p2  = read_imagef(in, sampleri, (int2)(clamp(x+pi+ki, 0, maxx), clamp(y+pj+kj, 0, maxy)));
          float4 tmp = (p1 - p2)*norm2;
          dist += tmp.x + tmp.y + tmp.z;
        }
      }
      float4 pin = read_imagef(in, sampleri, (int2)(clamp(x+ki, 0, maxx), clamp(y+kj, 0, maxy)));
      dist = gh(dist);
      acc.x += dist * pin.x;
      acc.y += dist * pin.y;
      acc.z += dist * pin.z;
      acc.w += dist;
    }
  }
  acc.x *= 1.0f/acc.w;
  acc.y *= 1.0f/acc.w;
  acc.z *= 1.0f/acc.w;
  write_imagef (out, (int2)(x, y), acc);
#endif
}

