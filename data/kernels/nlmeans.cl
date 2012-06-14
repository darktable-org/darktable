/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.
    copyright (c) 2012 Ulrich Pegelow.

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
const sampler_t samplerc =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP         | CLK_FILTER_NEAREST;

#define ICLAMP(a, mn, mx) ((a) < (mn) ? (mn) : ((a) > (mx) ? (mx) : (a)))



/* 
    To speed up processing we use an algorithm proposed from B. Goossens, H.Q. Luong, J. Aelterman, A. Pizurica,  and W. Philips, 
    "A GPU-Accelerated Real-Time NLMeans Algorithm for Denoising Color Video Sequences", in Proc. ACIVS (2), 2010, pp.46-57. 

    Benchmarking figures (export of a 20MPx image on a i7-2600 with an NVIDIA GTS450):

    This GPU-code: 18s
    Brute force GPU-code: 136s
    Optimized CPU-code: 27s

*/


float gh(const float f, const float sharpness)
{
  // make sharpness bigger: less smoothing
  return native_exp(-f*f*sharpness);
}


float ddirac(const int2 q)
{
  return ((q.x || q.y) ? 1.0f : 0.0f);
}


kernel void
nlmeans_init(write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  write_imagef (out, (int2)(x, y), (float4)0.0f);
}
              

kernel void
nlmeans_dist(read_only image2d_t in, write_only image2d_t U4, const int width, const int height, 
             const int2 q, const float nL2, const float nC2)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const float4 norm2 = (float4)(nL2, nC2, nC2, 1.0f);

  if(x >= width || y >= height) return;

  float4 p1 = read_imagef(in, sampleri, (int2)(x, y));
  float4 p2 = read_imagef(in, sampleri, (int2)(x, y) + q);
  float4 tmp = (p1 - p2)*(p1 - p2)*norm2;
  float dist = tmp.x + tmp.y + tmp.z;
  
  write_imagef (U4, (int2)(x, y), dist);
}

kernel void
nlmeans_horiz(read_only image2d_t U4_in, write_only image2d_t U4_out, const int width, const int height, 
              const int2 q, const int P, local float *buffer)
{
  const int lid = get_local_id(0);
  const int lsz = get_local_size(0);
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(y >= height) return;

  /* fill center part of buffer */
  buffer[P + lid] = read_imagef(U4_in, samplerc, (int2)(x, y)).x;

  /* left wing of buffer */
  for(int n=0; n <= P/lsz; n++)
  {
    const int l = mad24(n, lsz, lid + 1);
    if(l > P) continue;
    const int xx = mad24((int)get_group_id(0), lsz, -l);
    buffer[P - l] = read_imagef(U4_in, samplerc, (int2)(xx, y)).x;
  }
    
  /* right wing of buffer */
  for(int n=0; n <= P/lsz; n++)
  {
    const int r = mad24(n, lsz, lsz - lid);
    if(r > P) continue;
    const int xx = mad24((int)get_group_id(0), lsz, lsz - 1 + r);
    buffer[P + lsz - 1 + r] = read_imagef(U4_in, samplerc, (int2)(xx, y)).x;
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width) return;

  buffer += lid + P;

  float distacc = 0.0f;
  for(int pi = -P; pi <= P; pi++)
  {
    distacc += buffer[pi];
  }

  write_imagef (U4_out, (int2)(x, y), distacc);
}


kernel void
nlmeans_vert(read_only image2d_t U4_in, write_only image2d_t U4_out, const int width, const int height, 
              const int2 q, const int P, const float sharpness, local float *buffer)
{
  const int lid = get_local_id(1);
  const int lsz = get_local_size(1);
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width) return;

  /* fill center part of buffer */
  buffer[P + lid] = read_imagef(U4_in, samplerc, (int2)(x, y)).x;

  /* left wing of buffer */
  for(int n=0; n <= P/lsz; n++)
  {
    const int l = mad24(n, lsz, lid + 1);
    if(l > P) continue;
    const int yy = mad24((int)get_group_id(1), lsz, -l);
    buffer[P - l] = read_imagef(U4_in, samplerc, (int2)(x, yy)).x;
  }

  /* right wing of buffer */
  for(int n=0; n <= P/lsz; n++)
  {
    const int r = mad24(n, lsz, lsz - lid);
    if(r > P) continue;
    const int yy = mad24((int)get_group_id(1), lsz, lsz - 1 + r);
    buffer[P + lsz - 1 + r] = read_imagef(U4_in, samplerc, (int2)(x, yy)).x;
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  if(y >= height) return;

  buffer += lid + P;

  float distacc = 0.0f;
  for(int pj = -P; pj <= P; pj++)
  {
    distacc += buffer[pj];
  }

  distacc = gh(distacc, sharpness);

  write_imagef (U4_out, (int2)(x, y), distacc);
}



kernel void
nlmeans_accu(read_only image2d_t in, read_only image2d_t U2_in, read_only image2d_t U4_in,
             write_only image2d_t U2_out, const int width, const int height, 
             const int2 q)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 u1_pq = read_imagef(in, sampleri, (int2)(x, y) + q);
  float4 u1_mq = read_imagef(in, sampleri, (int2)(x, y) - q);

  float4 u2    = read_imagef(U2_in, sampleri, (int2)(x, y));

  float  u4    = read_imagef(U4_in, sampleri, (int2)(x, y)).x;
  float  u4_mq = read_imagef(U4_in, sampleri, (int2)(x, y) - q).x;

  float u3 = u2.w;
  float u4_mq_dd = u4_mq * ddirac(q);

  u2 += (u4 * u1_pq) + (u4_mq_dd * u1_mq);
  u3 += (u4 + u4_mq_dd);

  u2.w = u3;
  
  write_imagef(U2_out, (int2)(x, y), u2);
}


kernel void
nlmeans_finish(read_only image2d_t in, read_only image2d_t U2, write_only image2d_t out, 
               const int width, const int height, const float4 weight)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 i  = read_imagef(in, sampleri, (int2)(x, y));
  float4 u2 = read_imagef(U2, sampleri, (int2)(x, y));
  float  u3 = u2.w;

  float4 o = i * ((float4)1.0f - weight) + u2/u3 * weight;
  o.w = i.w;

  write_imagef(out, (int2)(x, y), o);
}



