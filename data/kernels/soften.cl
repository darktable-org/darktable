/*
    This file is part of darktable,
    copyright (c) 2012 ulrich pegelow.

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


float4 RGB_2_HSL(const float4 RGB)
{
  float H, S, L;

  // assumes that each channel is scaled to [0; 1]
  float R = RGB.x;
  float G = RGB.y;
  float B = RGB.z;

  float var_Min = fmin(R, fmin(G, B));
  float var_Max = fmax(R, fmax(G, B));
  float del_Max = var_Max - var_Min;

  L = (var_Max + var_Min) / 2.0f;

  if (del_Max == 0.0f)
  {
    H = 0.0f;
    S = 0.0f;
  }
  else
  {
    if (L < 0.5f) S = del_Max / (var_Max + var_Min);
    else          S = del_Max / (2.0f - var_Max - var_Min);

    float del_R = (((var_Max - R) / 6.0f) + (del_Max / 2.0f)) / del_Max;
    float del_G = (((var_Max - G) / 6.0f) + (del_Max / 2.0f)) / del_Max;
    float del_B = (((var_Max - B) / 6.0f) + (del_Max / 2.0f)) / del_Max;

    if      (R == var_Max) H = del_B - del_G;
    else if (G == var_Max) H = (1.0f / 3.0f) + del_R - del_B;
    else if (B == var_Max) H = (2.0f / 3.0f) + del_G - del_R;

    if (H < 0.0f) H += 1.0f;
    if (H > 1.0f) H -= 1.0f;
  }

  return (float4)(H, S, L, RGB.w);
}


float Hue_2_RGB(float v1, float v2, float vH)
{
  if (vH < 0.0f) vH += 1.0f;
  if (vH > 1.0f) vH -= 1.0f;
  if ((6.0f * vH) < 1.0f) return (v1 + (v2 - v1) * 6.0f * vH);
  if ((2.0f * vH) < 1.0f) return (v2);
  if ((3.0f * vH) < 2.0f) return (v1 + (v2 - v1) * ((2.0f / 3.0f) - vH) * 6.0f);
  return (v1);
}


float4 HSL_2_RGB(const float4 HSL)
{
  float R, G, B;

  float H = HSL.x;
  float S = HSL.y;
  float L = HSL.z;

  float var_1, var_2;

  if (S == 0.0f)
  {
    R = B = G = L;
  }
  else
  {
    if (L < 0.5f) var_2 = L * (1.0f + S);
    else          var_2 = (L + S) - (S * L);

    var_1 = 2.0f * L - var_2;

    R = Hue_2_RGB(var_1, var_2, H + (1.0f / 3.0f)); 
    G = Hue_2_RGB(var_1, var_2, H);
    B = Hue_2_RGB(var_1, var_2, H - (1.0f / 3.0f));
  } 

  // returns RGB scaled to [0; 1] for each channel
  return (float4)(R, G, B, HSL.w);
}


/* first step for soften module: generate overexposed image */
kernel void
soften_overexposed(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                  const float saturation, const float brightness)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float4 hsl = RGB_2_HSL(pixel);

  hsl.y = clamp(hsl.y * saturation, 0.0f, 1.0f);
  hsl.z = clamp(hsl.z * brightness, 0.0f, 1.0f);

  pixel = HSL_2_RGB(hsl);

  write_imagef (out, (int2)(x, y), pixel);
}

/* horizontal gaussian blur */
kernel void 
soften_hblur(read_only image2d_t in, write_only image2d_t out, global const float *m, const int rad,
      const int width, const int height, const int blocksize, local float4 *buffer)
{
  const int lid = get_local_id(0);
  const int lsz = get_local_size(0);
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  float4 pixel = (float4)0.0f;

  /* read pixel and fill center part of buffer */
  pixel = read_imagef(in, sampleri, (int2)(x, y));
  buffer[rad + lid] = pixel;

  /* left wing of buffer */
  for(int n=0; n <= rad/lsz; n++)
  {
    const int l = mad24(n, lsz, lid + 1);
    if(l > rad) continue;
    const int xx = mad24((int)get_group_id(0), lsz, -l);
    buffer[rad - l] = read_imagef(in, sampleri, (int2)(xx, y));
  }
    
  /* right wing of buffer */
  for(int n=0; n <= rad/lsz; n++)
  {
    const int r = mad24(n, lsz, lsz - lid);
    if(r > rad) continue;
    const int xx = mad24((int)get_group_id(0), lsz, lsz - 1 + r);
    buffer[rad + lsz - 1 + r] = read_imagef(in, sampleri, (int2)(xx, y));
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  buffer += lid + rad;
  m += rad;

  float4 sum = (float4)0.0f;

  for (int i=-rad; i<=rad; i++)
  {
    sum += buffer[i] * m[i];
  }

  pixel = sum;
  write_imagef (out, (int2)(x, y), pixel);
}


/* vertical gaussian blur */
kernel void 
soften_vblur(read_only image2d_t in, write_only image2d_t out, global const float *m, const int rad,
      const int width, const int height, const int blocksize, local float4 *buffer)
{
  const int lid = get_local_id(1);
  const int lsz = get_local_size(1);
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  float4 pixel = (float4)0.0f;

  /* read pixel and fill center part of buffer */
  pixel = read_imagef(in, sampleri, (int2)(x, y));
  buffer[rad + lid] = pixel;

  /* left wing of buffer */
  for(int n=0; n <= rad/lsz; n++)
  {
    const int l = mad24(n, lsz, lid + 1);
    if(l > rad) continue;
    const int yy = mad24((int)get_group_id(1), lsz, -l);
    buffer[rad - l] = read_imagef(in, sampleri, (int2)(x, yy));
  }
    
  /* right wing of buffer */
  for(int n=0; n <= rad/lsz; n++)
  {
    const int r = mad24(n, lsz, lsz - lid);
    if(r > rad) continue;
    const int yy = mad24((int)get_group_id(1), lsz, lsz - 1 + r);
    buffer[rad + lsz - 1 + r] = read_imagef(in, sampleri, (int2)(x, yy));
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  buffer += lid + rad;
  m += rad;

  float4 sum = (float4)0.0f;

  for (int i=-rad; i<=rad; i++)
  {
    sum += buffer[i] * m[i];
  }

  pixel = sum;
  write_imagef (out, (int2)(x, y), pixel);
}




/* final step for soften module */
kernel void
soften_mix(read_only image2d_t in_a, read_only image2d_t in_b, write_only image2d_t out, const int width, const int height,
                  const float amount)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 original  = read_imagef(in_a, sampleri, (int2)(x, y));
  float4 processed = read_imagef(in_b, sampleri, (int2)(x, y));

  float4 pixel = original * (1.0f - amount) + clamp(processed, (float4)0.0f, (float4)1.0f) * amount;
  pixel.w = original.w;

  write_imagef (out, (int2)(x, y), pixel);
}


