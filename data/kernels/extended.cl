/*
    This file is part of darktable,
    copyright (c) 2009--2014 Ulrich Pegelow

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
#include "colorspace.h"
#include "color_conversion.h"


__kernel void
graduatedndp (read_only image2d_t in, write_only image2d_t out, const int width, const int height, const float4 color,
              const float density, const float length_base, const float length_inc_x, const float length_inc_y)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  const float len = length_base + y*length_inc_y + x*length_inc_x;

  const float t = 0.693147181f * (density * clamp(0.5f+len, 0.0f, 1.0f)/8.0f);
  const float d1 = t * t * 0.5f;
  const float d2 = d1 * t * 0.333333333f;
  const float d3 = d2 * t * 0.25f;
  float dens = 1.0f + t + d1 + d2 + d3;
  dens *= dens;
  dens *= dens;
  dens *= dens;

  pixel.xyz = fmax((float4)0.0f, pixel / (color + ((float4)1.0f - color) * (float4)dens)).xyz;

  write_imagef (out, (int2)(x, y), pixel);
}


__kernel void
graduatedndm (read_only image2d_t in, write_only image2d_t out, const int width, const int height, const float4 color,
              const float density, const float length_base, const float length_inc_x, const float length_inc_y)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  const float len = length_base + y*length_inc_y + x*length_inc_x;

  const float t = 0.693147181f * (-density * clamp(0.5f-len, 0.0f, 1.0f)/8.0f);
  const float d1 = t * t * 0.5f;
  const float d2 = d1 * t * 0.333333333f;
  const float d3 = d2 * t * 0.25f;
  float dens = 1.0f + t + d1 + d2 + d3;
  dens *= dens;
  dens *= dens;
  dens *= dens;

  pixel.xyz = fmax((float4)0.0f, pixel * (color + ((float4)1.0f - color) * (float4)dens)).xyz;

  write_imagef (out, (int2)(x, y), pixel);
}

__kernel void
colorize (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
          const float mix, const float L, const float a, const float b)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  pixel.x = pixel.x * mix + L - 50.0f * mix;
  pixel.y = a;
  pixel.z = b;

  write_imagef (out, (int2)(x, y), pixel);
}


float
GAUSS(float center, float wings, float x)
{
  const float b = -1.0f + center * 2.0f;
  const float c = (wings / 10.0f) / 2.0f;
  return exp(-(x-b)*(x-b)/(c*c));
}


__kernel void
relight (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
         const float center, const float wings, const float ev)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  const float lightness = pixel.x/100.0f;
  const float value = -1.0f+(lightness*2.0f);
  float gauss = GAUSS(center, wings, value);

  if(isnan(gauss) || isinf(gauss))
    gauss = 0.0f;

  float relight = 1.0f / exp2(-ev * clamp(gauss, 0.0f, 1.0f));

  if(isnan(relight) || isinf(relight))
    relight = 1.0f;

  pixel.x = 100.0f * clamp(lightness*relight, 0.0f, 1.0f);

  write_imagef (out, (int2)(x, y), pixel);
}


typedef enum _channelmixer_operation_mode_t
{
  OPERATION_MODE_RGB = 0,
  OPERATION_MODE_GRAY = 1,
  OPERATION_MODE_HSL_V1 = 2,
  OPERATION_MODE_HSL_V2 = 3,
} _channelmixer_operation_mode_t;


__kernel void
channelmixer (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
              const int operation_mode, global const float *hsl_matrix,
              global const float *rgb_matrix)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  float4 opixel = (float4)(0.0f, 0.0f, 0.0f, pixel.w);
  float gray, hmix, smix, lmix;

  switch(operation_mode)
  {
    case OPERATION_MODE_RGB:
      opixel.x = fmax(pixel.x * rgb_matrix[0] + pixel.y * rgb_matrix[1] + pixel.z * rgb_matrix[2], 0.0f);
      opixel.y = fmax(pixel.x * rgb_matrix[3] + pixel.y * rgb_matrix[4] + pixel.z * rgb_matrix[5], 0.0f);
      opixel.z = fmax(pixel.x * rgb_matrix[6] + pixel.y * rgb_matrix[7] + pixel.z * rgb_matrix[8], 0.0f);
      break;

    case OPERATION_MODE_GRAY:
      gray = fmax(pixel.x * rgb_matrix[0] + pixel.y * rgb_matrix[1] + pixel.z * rgb_matrix[2], 0.0f);
      opixel = (float4)(gray, gray, gray, pixel.w);
      break;

    case OPERATION_MODE_HSL_V1:
      hmix = clamp(pixel.x * hsl_matrix[0], 0.0f, 1.0f) + pixel.y * hsl_matrix[1] + pixel.z * hsl_matrix[2];
      smix = clamp(pixel.x * hsl_matrix[3], 0.0f, 1.0f) + pixel.y * hsl_matrix[4] + pixel.z * hsl_matrix[5];
      lmix = clamp(pixel.x * hsl_matrix[6], 0.0f, 1.0f) + pixel.y * hsl_matrix[7] + pixel.z * hsl_matrix[8];

      if( hmix != 0.0f || smix != 0.0f || lmix != 0.0f )
      {
        float4 hsl = RGB_2_HSL(pixel);
        hsl.x = (hmix != 0.0f ) ? hmix : hsl.x;
        hsl.y = (smix != 0.0f ) ? smix : hsl.y;
        hsl.z = (lmix != 0.0f ) ? lmix : hsl.z;
        pixel = HSL_2_RGB(hsl);
      }

      opixel.x = clamp(pixel.x * rgb_matrix[0] + pixel.y * rgb_matrix[1] + pixel.z * rgb_matrix[2], 0.0f, 1.0f);
      opixel.y = clamp(pixel.x * rgb_matrix[3] + pixel.y * rgb_matrix[4] + pixel.z * rgb_matrix[5], 0.0f, 1.0f);
      opixel.z = clamp(pixel.x * rgb_matrix[6] + pixel.y * rgb_matrix[7] + pixel.z * rgb_matrix[8], 0.0f, 1.0f);
      break;

    case OPERATION_MODE_HSL_V2:
      hmix = clamp(pixel.x * hsl_matrix[0] + pixel.y * hsl_matrix[1] + pixel.z * hsl_matrix[2], 0.0f, 1.0f);
      smix = clamp(pixel.x * hsl_matrix[3] + pixel.y * hsl_matrix[4] + pixel.z * hsl_matrix[5], 0.0f, 1.0f);
      lmix = clamp(pixel.x * hsl_matrix[6] + pixel.y * hsl_matrix[7] + pixel.z * hsl_matrix[8], 0.0f, 1.0f);
      if( hmix != 0.0f || smix != 0.0f || lmix != 0.0f )
      {
        pixel = (float4)(clamp(pixel.x, 0.0f, 1.0f), clamp(pixel.y, 0.0f, 1.0f), clamp(pixel.z, 0.0f, 1.0f), pixel.w);
        float4 hsl = RGB_2_HSL(pixel);
        hsl.x = (hmix != 0.0f ) ? hmix : hsl.x;
        hsl.y = (smix != 0.0f ) ? smix : hsl.y;
        hsl.z = (lmix != 0.0f ) ? lmix : hsl.z;
        pixel = HSL_2_RGB(hsl);
      }
      opixel.x = fmax(pixel.x * rgb_matrix[0] + pixel.y * rgb_matrix[1] + pixel.z * rgb_matrix[2], 0.0f);
      opixel.y = fmax(pixel.x * rgb_matrix[3] + pixel.y * rgb_matrix[4] + pixel.z * rgb_matrix[5], 0.0f);
      opixel.z = fmax(pixel.x * rgb_matrix[6] + pixel.y * rgb_matrix[7] + pixel.z * rgb_matrix[8], 0.0f);
      break;
  }

  write_imagef (out, (int2)(x, y), opixel);
}


__kernel void
velvia (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
        const float strength, const float bias)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  // calculate vibrance, and apply boost velvia saturation at least saturated pixels
  float pmax = fmax(pixel.x, fmax(pixel.y, pixel.z));     // max value in RGB set
  float pmin = fmin(pixel.x, fmin(pixel.y, pixel.z));     // min value in RGB set
  float plum = (pmax + pmin) / 2.0f;                // pixel luminocity
  float psat = (plum <= 0.5f) ? (pmax-pmin)/(1e-5f + pmax+pmin) : (pmax-pmin)/(1e-5f + fmax(0.0f, 2.0f-pmax-pmin));

  float pweight = clamp(((1.0f- (1.5f*psat)) + ((1.0f+(fabs(plum-0.5f)*2.0f))*(1.0f-bias))) / (1.0f+(1.0f-bias)), 0.0f, 1.0f); // The weight of pixel
  float saturation = strength*pweight;      // So lets calculate the final affection of filter on pixel

  float4 opixel;

  opixel.x = clamp(pixel.x + saturation*(pixel.x-0.5f*(pixel.y+pixel.z)), 0.0f, 1.0f);
  opixel.y = clamp(pixel.y + saturation*(pixel.y-0.5f*(pixel.z+pixel.x)), 0.0f, 1.0f);
  opixel.z = clamp(pixel.z + saturation*(pixel.z-0.5f*(pixel.x+pixel.y)), 0.0f, 1.0f);
  opixel.w = pixel.w;

  write_imagef (out, (int2)(x, y), opixel);
}


__kernel void
colorcontrast (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
               const float4 scale, const float4 offset, const int unbound)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  pixel.xyz = (pixel * scale + offset).xyz;
  pixel.y = unbound ? pixel.y : clamp(pixel.y, -128.0f, 128.0f);
  pixel.z = unbound ? pixel.z : clamp(pixel.z, -128.0f, 128.0f);

  write_imagef (out, (int2)(x, y), pixel);
}


__kernel void
vibrance (read_only image2d_t in, write_only image2d_t out, const int width, const int height, const float amount)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  const float sw = sqrt(pixel.y*pixel.y + pixel.z*pixel.z)/256.0f;
  const float ls = 1.0f - amount * sw * 0.25f;
  const float ss = 1.0f + amount * sw;

  pixel.x *= ls;
  pixel.y *= ss;
  pixel.z *= ss;

  write_imagef (out, (int2)(x, y), pixel);
}

#define TEA_ROUNDS 8

void
encrypt_tea(unsigned int *arg)
{
  const unsigned int key[] = {0xa341316c, 0xc8013ea4, 0xad90777d, 0x7e95761e};
  unsigned int v0 = arg[0], v1 = arg[1];
  unsigned int sum = 0;
  unsigned int delta = 0x9e3779b9;
  for(int i = 0; i < TEA_ROUNDS; i++)
  {
    sum += delta;
    v0 += ((v1 << 4) + key[0]) ^ (v1 + sum) ^ ((v1 >> 5) + key[1]);
    v1 += ((v0 << 4) + key[2]) ^ (v0 + sum) ^ ((v0 >> 5) + key[3]);
  }
  arg[0] = v0;
  arg[1] = v1;
}

float
tpdf(unsigned int urandom)
{
  float frandom = (float)urandom / (float)0xFFFFFFFFu;

  return (frandom < 0.5f ? (sqrt(2.0f*frandom) - 1.0f) : (1.0f - sqrt(2.0f*(1.0f - frandom))));
}


__kernel void
vignette (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
          const float2 scale, const float2 roi_center_scaled, const float2 expt,
          const float dscale, const float fscale, const float brightness, const float saturation,
          const float dither, const int unbound)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  unsigned int tea_state[2] = { mad24(y, width, x), 0 };
  encrypt_tea(tea_state);

  const float2 pv = fabs((float2)(x,y) * scale - roi_center_scaled);

  const float cplen = pow(pow(pv.x, expt.x) + pow(pv.y, expt.x), expt.y);

  float weight = 0.0f;
  float dith = 0.0f;

  if(cplen >= dscale)
  {
    weight = ((cplen - dscale) / fscale);

    dith = (weight <= 1.0f && weight >= 0.0f) ? dither * tpdf(tea_state[0]) : 0.0f;

    weight = weight >= 1.0f ? 1.0f : (weight <= 0.0f ? 0.0f : 0.5f - cos(M_PI_F * weight) / 2.0f);
  }

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  if(weight > 0.0f)
  {
    float falloff = brightness < 0.0f ? 1.0f + (weight * brightness) : weight * brightness;

    pixel.xyz = (brightness < 0.0f ? pixel * falloff + dith : pixel + falloff + dith).xyz;

    pixel.xyz = unbound ? pixel.xyz : clamp(pixel, (float4)0.0f, (float4)1.0f).xyz;

    float mv = (pixel.x + pixel.y + pixel.z) / 3.0f;
    float wss = weight * saturation;

    pixel.xyz = (pixel - (mv - pixel)* wss).xyz,

    pixel.xyz = unbound ? pixel.xyz : clamp(pixel, (float4)0.0f, (float4)1.0f).xyz;
  }

  write_imagef (out, (int2)(x, y), pixel);
}


/* kernel for the splittoning plugin. */
kernel void
splittoning (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
            const float compress, const float balance, const float shadow_hue, const float shadow_saturation,
            const float highlight_hue, const float highlight_saturation)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float4 hsl = RGB_2_HSL(pixel);

  if(hsl.z < balance - compress || hsl.z > balance + compress)
  {
    hsl.x = hsl.z < balance ? shadow_hue : highlight_hue;
    hsl.y = hsl.z < balance ? shadow_saturation : highlight_saturation;
    float ra = hsl.z < balance ? clamp(2.0f*fabs(-balance + compress + hsl.z), 0.0f, 1.0f) :
               clamp(2.0f*fabs(-balance - compress + hsl.z), 0.0f, 1.0f);

    float4 mixrgb = HSL_2_RGB(hsl);

    pixel.xyz = clamp(pixel * (1.0f - ra) + mixrgb * ra, (float4)0.0f, (float4)1.0f).xyz;
  }

  write_imagef (out, (int2)(x, y), pixel);
}

/* kernels to get the maximum value of an image */
kernel void
pixelmax_first (read_only image2d_t in, const int width, const int height, global float *accu, local float *buffer)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int xlsz = get_local_size(0);
  const int ylsz = get_local_size(1);
  const int xlid = get_local_id(0);
  const int ylid = get_local_id(1);

  const int l = ylid * xlsz + xlid;

  buffer[l] = (x < width && y < height) ? read_imagef(in, sampleri, (int2)(x, y)).x : -INFINITY;

  barrier(CLK_LOCAL_MEM_FENCE);

  const int lsz = mul24(xlsz, ylsz);

  for(int offset = lsz / 2; offset > 0; offset = offset / 2)
  {
    if (l < offset)
    {
      float other = buffer[l + offset];
      float mine =  buffer[l];
      buffer[l] = (mine > other) ? mine : other;
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  const int xgid = get_group_id(0);
  const int ygid = get_group_id(1);
  const int xgsz = get_num_groups(0);

  const int m = mad24(ygid, xgsz, xgid);
  accu[m] = buffer[0];
}



__kernel void
pixelmax_second(global float* input, global float *result, const int length, local float *buffer)
{
  int x = get_global_id(0);
  float accu = -INFINITY;

  while (x < length)
  {
    float element = input[x];
    accu = (accu > element) ? accu : element;
    x += get_global_size(0);
  }

  int lid = get_local_id(0);
  buffer[lid] = accu;

  barrier(CLK_LOCAL_MEM_FENCE);

  for(int offset = get_local_size(0) / 2; offset > 0; offset = offset / 2)
  {
    if (lid < offset)
    {
      float other = buffer[lid + offset];
      float mine = buffer[lid];
      buffer[lid] = (mine > other) ? mine : other;
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  if (lid == 0)
  {
    result[get_group_id(0)] = buffer[0];
  }
}


/* kernel for the global tonemap plugin: reinhard */
kernel void
global_tonemap_reinhard (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
            const float4 parameters)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float l = pixel.x * 0.01f;

  pixel.x = 100.0f * (l/(1.0f + l));

  write_imagef (out, (int2)(x, y), pixel);
}



/* kernel for the global tonemap plugin: drago */
kernel void
global_tonemap_drago (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
            const float4 parameters)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const float eps = parameters.x;
  const float ldc = parameters.y;
  const float bl = parameters.z;
  const float lwmax = parameters.w;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float lw = pixel.x * 0.01f;

  pixel.x = 100.0f * (ldc * log(fmax(eps, lw + 1.0f)) / log(fmax(eps, 2.0f + (pow(lw/lwmax,bl)) * 8.0f)));

  write_imagef (out, (int2)(x, y), pixel);
}


/* kernel for the global tonemap plugin: filmic */
kernel void
global_tonemap_filmic (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
            const float4 parameters)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float l = pixel.x * 0.01f;
  float m = fmax(0.0f, l - 0.004f);

  pixel.x = 100.0f * ((m*(6.2f*m+0.5f))/(m*(6.2f*m+1.7f)+0.06f));

  write_imagef (out, (int2)(x, y), pixel);
}


/* kernels for the colormapping module */
#define HISTN (1<<11)
#define MAXN 5

// inverse distant weighting according to D. Shepard's method; with power parameter 2.0
void
get_clusters(const float4 col, const int n, global float2 *mean, float *weight)
{
  float mdist = FLT_MAX;
  for(int k=0; k<n; k++)
  {
    const float dist2 = (col.y-mean[k].x)*(col.y-mean[k].x) + (col.z-mean[k].y)*(col.z-mean[k].y);  // dist^2
    weight[k] = dist2 > 1.0e-6f ? 1.0f/dist2 : -1.0f;                                                // direct hits marked as -1
    if(dist2 < mdist) mdist = dist2;
  }
  if(mdist < 1.0e-6f) for(int k=0; k<n; k++) weight[k] = weight[k] < 0.0f ? 1.0f : 0.0f;             // correction in case of direct hits
  float sum = 0.0f;
  for(int k=0; k<n; k++) sum += weight[k];
  if(sum > 0.0f) for(int k=0; k<n; k++) weight[k] /= sum;
}

kernel void
colormapping_histogram (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
            const float equalization, global int *target_hist, global float *source_ihist)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float L = read_imagef(in, sampleri, (int2)(x, y)).x;

  float dL = 0.5f*((L * (1.0f - equalization) + source_ihist[target_hist[(int)clamp(HISTN*L/100.0f, 0.0f, (float)HISTN-1.0f)]] * equalization) - L) + 50.0f;
  dL = clamp(dL, 0.0f, 100.0f);

  write_imagef (out, (int2)(x, y), (float4)(dL, 0.0f, 0.0f, 0.0f));
}

kernel void
colormapping_mapping (read_only image2d_t in, read_only image2d_t tmp, write_only image2d_t out, const int width, const int height,
            const int clusters, global float2 *target_mean, global float2 *source_mean, global float2 *var_ratio, global int *mapio)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 ipixel = read_imagef(in, sampleri, (int2)(x, y));
  float dL = read_imagef(tmp, sampleri, (int2)(x, y)).x;
  float weight[MAXN];
  float4 opixel = (float4)0.0f;

  opixel.x = 2.0f*(dL - 50.0f) + ipixel.x;
  opixel.x = clamp(opixel.x, 0.0f, 100.0f);

  get_clusters(ipixel, clusters, target_mean, weight);

  for(int c=0; c < clusters; c++)
  {
    opixel.y += weight[c] * ((ipixel.y - target_mean[c].x)*var_ratio[c].x + source_mean[mapio[c]].x);
    opixel.z += weight[c] * ((ipixel.z - target_mean[c].y)*var_ratio[c].y + source_mean[mapio[c]].y);
  }
  opixel.w = ipixel.w;

  write_imagef (out, (int2)(x, y), opixel);
}

#undef HISTN
#undef MAXN


/* kernel for the colorbalance module */
kernel void
colorbalance (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
              const float4 lift, const float4 gain, const float4 gamma_inv, const float saturation, const float contrast, const float grey)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 Lab = read_imagef(in, sampleri, (int2)(x, y));
  float4 sRGB = XYZ_to_sRGB(Lab_to_XYZ(Lab));

  // Lift gamma gain
  sRGB = (sRGB <= (float4)0.0031308f) ? 12.92f * sRGB : (1.0f + 0.055f) * native_powr(sRGB, (float4)1.0f/2.4f) - (float4)0.055f;
  sRGB = native_powr(fmax(((sRGB - (float4)1.0f) * lift + (float4)1.0f) * gain, (float4)0.0f), gamma_inv);
  sRGB = (sRGB <= (float4)0.04045f) ? sRGB / 12.92f : native_powr((sRGB + (float4)0.055f) / (1.0f + 0.055f), (float4)2.4f);
  Lab.xyz = XYZ_to_Lab(sRGB_to_XYZ(sRGB)).xyz;

  write_imagef (out, (int2)(x, y), Lab);
}

kernel void
colorbalance_lgg (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
              const float4 lift, const float4 gain, const float4 gamma_inv, const float saturation, const float contrast, const float grey, const float saturation_out)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 Lab = read_imagef(in, sampleri, (int2)(x, y));
  const float4 XYZ = Lab_to_XYZ(Lab);
  float4 RGB = XYZ_to_prophotorgb(XYZ);

  // saturation input
  if (saturation != 1.0f)
  {
    const float4 luma = XYZ.y;
    const float4 saturation4 = saturation;
    RGB = luma + saturation4 * (RGB - luma);
  }

  // Lift gamma gain
  RGB = (RGB <= (float4)0.0f) ? (float4)0.0f : native_powr(RGB, (float4)1.0f/2.2f);
  RGB = ((RGB - (float4)1.0f) * lift + (float4)1.0f) * gain;
  RGB = (RGB <= (float4)0.0f) ? (float4)0.0f : native_powr(RGB, gamma_inv * (float4)2.2f);

  // saturation output
  if (saturation_out != 1.0f)
  {
    const float4 luma = prophotorgb_to_XYZ(RGB).y;
    const float4 saturation_out4 = saturation_out;
    RGB = luma + saturation_out4 * (RGB - luma);
  }

  // fulcrum contrast
  if (contrast != 1.0f)
  {
    const float4 contrast4 = contrast;
    const float4 grey4 = grey;
    RGB = (RGB <= (float4)0.0f) ? (float4)0.0f : pow(RGB / grey4, contrast4) * grey4;
  }

  Lab.xyz = prophotorgb_to_Lab(RGB).xyz;

  write_imagef (out, (int2)(x, y), Lab);
}

kernel void
colorbalance_cdl (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
              const float4 lift, const float4 gain, const float4 gamma_inv, const float saturation, const float contrast, const float grey, const float saturation_out)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 Lab = read_imagef(in, sampleri, (int2)(x, y));
  const float4 XYZ = Lab_to_XYZ(Lab);
  float4 RGB = XYZ_to_prophotorgb(XYZ);

  // saturation input
  if (saturation != 1.0f)
  {
    const float4 luma = XYZ.y;
    const float4 saturation4 = saturation;
    RGB = luma + saturation4 * (RGB - luma);
  }

  // lift power slope
  RGB = RGB * gain + lift;
  RGB = (RGB <= (float4)0.0f) ? (float4)0.0f : native_powr(RGB, gamma_inv);

  // saturation output
  if (saturation_out != 1.0f)
  {
    const float4 luma = prophotorgb_to_XYZ(RGB).y;
    const float4 saturation_out4 = saturation_out;
    RGB = luma + saturation_out4 * (RGB - luma);
  }

  // fulcrum contrast
  if (contrast != 1.0f)
  {
    const float4 contrast4 = contrast;
    const float4 grey4 = grey;
    RGB = (RGB <= (float4)0.0f) ? (float4)0.0f : native_powr(RGB / grey4, contrast4) * grey4;
  }

  Lab.xyz = prophotorgb_to_Lab(RGB).xyz;

  write_imagef (out, (int2)(x, y), Lab);
}


static inline float sqf(const float x)
{
  return x * x;
}


static inline float4 opacity_masks(const float x,
                                   const float shadows_weight, const float highlights_weight,
                                   const float midtones_weight, const float mask_grey_fulcrum)
{
  float4 output;
  const float x_offset = (x - mask_grey_fulcrum);
  const float x_offset_norm = x_offset / mask_grey_fulcrum;
  const float alpha = 1.f / (1.f + native_exp(x_offset_norm * shadows_weight));    // opacity of shadows
  const float beta = 1.f / (1.f + native_exp(-x_offset_norm * highlights_weight)); // opacity of highlights
  const float gamma = native_exp(-sqf(x_offset) * midtones_weight / 4.f) * sqf(1.f - alpha) * sqf(1.f - beta) * 8.f; // opacity of midtones

  output.x = alpha;
  output.y = gamma;
  output.z = beta;
  output.w = 0.f;

  return output;
}


#define LUT_ELEM 360 // gamut LUT number of elements: resolution of 1°

static inline float lookup_gamut(read_only image2d_t gamut_lut, const float x)
{
  // WARNING : x should be between [-pi ; pi ], which is the default output of atan2 anyway

  // convert in LUT coordinate
  const float x_test = (LUT_ELEM - 1) * (x + M_PI_F) / (2.f * M_PI_F);

  // find the 2 closest integer coordinates (next/previous)
  float x_prev = floor(x_test);
  float x_next = ceil(x_test);

  // get the 2 closest LUT elements at integer coordinates
  // cycle on the hue ring if out of bounds
  int xi = (int)x_prev;
  if(xi < 0) xi = LUT_ELEM - 1;
  else if(xi > LUT_ELEM - 1) xi = 0;

  int xii = (int)x_next;
  if(xii < 0) xii = LUT_ELEM - 1;
  else if(xii > LUT_ELEM - 1) xii = 0;

  // fetch the corresponding y values
  const float y_prev = read_imagef(gamut_lut, sampleri, (int2)(xi, 0)).x;
  const float y_next = read_imagef(gamut_lut, sampleri, (int2)(xii, 0)).x;

  // assume that we are exactly on an integer LUT element
  float out = y_prev;

  if(x_next != x_prev)
    // we are between 2 LUT elements : do linear interpolation
    // actually, we only add the slope term on the previous one
    out += (x_test - x_prev) * (y_next - y_prev) / (x_next - x_prev);

  return out;
}


typedef enum dt_iop_colorbalancrgb_saturation_t
{
  DT_COLORBALANCE_SATURATION_JZAZBZ = 0, // $DESCRIPTION: "JzAzBz (2021)"
  DT_COLORBALANCE_SATURATION_DTUCS = 1   // $DESCRIPTION: "darktable UCS (2022)"
} dt_iop_colorbalancrgb_saturation_t;


static inline float soft_clip(const float x, const float soft_threshold, const float hard_threshold)
{
  // use an exponential soft clipping above soft_threshold
  // hard threshold must be > soft threshold
  const float norm = hard_threshold - soft_threshold;
  return (x > soft_threshold) ? soft_threshold + (1.f - native_exp(-(x - soft_threshold) / norm)) * norm : x;
}


kernel void
colorbalancergb (read_only image2d_t in, write_only image2d_t out,
                 const int width, const int height,
                 constant const dt_colorspaces_iccprofile_info_cl_t *const profile_info,
                 constant const float *const matrix_in, constant const float *const matrix_out,
                 read_only image2d_t gamut_lut,
                 const float shadows_weight, const float highlights_weight, const float midtones_weight, const float mask_grey_fulcrum,
                 const float hue_angle, const float chroma_global, const float4 chroma, const float vibrance,
                 const float4 global_offset, const float4 shadows, const float4 highlights, const float4 midtones,
                 const float white_fulcrum, const float midtones_Y,
                 const float grey_fulcrum, const float contrast,
                 const float brilliance_global, const float4 brilliance,
                 const float saturation_global, const float4 saturation,
                 const int mask_display, const int mask_type, const int checker_1, const int checker_2,
                 const float4 checker_color_1, const float4 checker_color_2, const float L_white,
                 const dt_iop_colorbalancrgb_saturation_t saturation_formula,
                 constant const float *const hue_rotation_matrix)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const float4 pix_in = read_imagef(in, sampleri, (int2)(x, y));

  float4 XYZ_D65 = 0.f;
  float4 LMS = 0.f;
  float4 RGB = 0.f;
  float4 Yrg = 0.f;
  float4 Ych = 0.f;

  // clip pipeline RGB
  RGB = fmax(pix_in, 0.f);

  // go to CIE 2006 LMS D65
  LMS = matrix_product_float4(RGB, matrix_in);

  // go to Filmlight Yrg
  Yrg = LMS_to_Yrg(LMS);

  // go to Ych
  Ych = Yrg_to_Ych(Yrg);

  // Sanitize input : no negative luminance
  Ych.x = fmax(Ych.x, 0.f);
  const float4 opacities = opacity_masks(native_powr(Ych.x, 0.4101205819200422f), // center middle grey in 50 %
                                         shadows_weight, highlights_weight, midtones_weight, mask_grey_fulcrum);
  const float4 opacities_comp = (float4)1.f - opacities;

  // Hue shift - do it now because we need the gamut limit at output hue right after
  // The hue rotation is implemented as a matrix multiplication.
  const float cos_h = Ych.z;
  const float sin_h = Ych.w;
  Ych.z = hue_rotation_matrix[0] * cos_h + hue_rotation_matrix[1] * sin_h;
  Ych.w = hue_rotation_matrix[2] * cos_h + hue_rotation_matrix[3] * sin_h;

  // Linear chroma : distance to achromatic at constant luminance in scene-referred
  const float chroma_boost = chroma_global + dot(opacities, chroma);
  const float vib = vibrance * (1.0f - native_powr(Ych.y, fabs(vibrance)));
  const float chroma_factor = fmax(1.f + chroma_boost + vib, 0.f);
  Ych.y *= chroma_factor;

  // clip chroma at constant Y and hue
  Ych = gamut_check_Yrg(Ych);

  // go to Yrg for real
  Yrg = Ych_to_Yrg(Ych);

  // Go to LMS
  LMS = Yrg_to_LMS(Yrg);

  // Go to Filmlight RGB
  RGB = LMS_to_gradingRGB(LMS);

  // Color balance

  // global : offset
  RGB += global_offset;

  // highlights, shadows : 2 slopes with masking
  RGB *= opacities_comp.z * (opacities_comp.x + opacities.x * shadows) + opacities.z * highlights;
  // factorization of : (RGB[c] * (1.f - alpha) + RGB[c] * d->shadows[c] * alpha) * (1.f - beta)  + RGB[c] * d->highlights[c] * beta;

  // midtones : power with sign preservation
  RGB = sign(RGB) * native_powr(fabs(RGB) / white_fulcrum, midtones) * white_fulcrum;

  // for the non-linear ops we need to go in Yrg again because RGB doesn't preserve color
  LMS = gradingRGB_to_LMS(RGB);
  Yrg = LMS_to_Yrg(LMS);

  // Y midtones power (gamma)
  Yrg.x = native_powr(fmax(Yrg.x / white_fulcrum, 0.f), midtones_Y) * white_fulcrum;

  // Y fulcrumed contrast
  Yrg.x = grey_fulcrum * native_powr(Yrg.x / grey_fulcrum, contrast);

  LMS = Yrg_to_LMS(Yrg);
  XYZ_D65 = LMS_to_XYZ(LMS);

  // Perceptual color adjustments
  if(saturation_formula == DT_COLORBALANCE_SATURATION_JZAZBZ)
  {

    // Go to JzAzBz for perceptual saturation
    float4 Jab = XYZ_to_JzAzBz(XYZ_D65);

    // Convert to JCh
    float JC[2] = { Jab.x, hypot(Jab.y, Jab.z) };               // brightness/chroma vector
    const float h = atan2(Jab.z, Jab.y);  // hue : (a, b) angle

    // Project JC onto S, the saturation eigenvector, with orthogonal vector O.
    // Note : O should be = (C * cosf(T) - J * sinf(T)) = 0 since S is the eigenvector,
    // so we add the chroma projected along the orthogonal axis to get some control value
    const float T = atan2(JC[1], JC[0]); // angle of the eigenvector over the hue plane
    const float sin_T = native_sin(T);
    const float cos_T = native_cos(T);
    const float M_rot_dir[2][2] = { {  cos_T,  sin_T },
                                    { -sin_T,  cos_T } };
    const float M_rot_inv[2][2] = { {  cos_T, -sin_T },
                                    {  sin_T,  cos_T } };
    float SO[2];

    // brilliance & Saturation : mix of chroma and luminance
    const float boosts[2] = { 1.f + brilliance_global + dot(opacities, brilliance),     // move in S direction
                              saturation_global + dot(opacities, saturation) }; // move in O direction

    SO[0] = JC[0] * M_rot_dir[0][0] + JC[1] * M_rot_dir[0][1];
    SO[1] = SO[0] * clamp(T * boosts[1], -T, M_PI_F / 2.f - T);
    SO[0] = fmax(SO[0] * boosts[0], 0.f);

    // Project back to JCh, that is rotate back of -T angle
    JC[0] = fmax(SO[0] * M_rot_inv[0][0] + SO[1] * M_rot_inv[0][1], 0.f);
    JC[1] = fmax(SO[0] * M_rot_inv[1][0] + SO[1] * M_rot_inv[1][1], 0.f);

    // Gamut mapping
    const float out_max_sat_h = lookup_gamut(gamut_lut, h);
    // if JC[0] == 0.f, the saturation / luminance ratio is infinite - assign the largest practical value we have
    const float sat = (JC[0] > 0.f) ? soft_clip(JC[1] / JC[0], 0.8f * out_max_sat_h, out_max_sat_h)
                                    : out_max_sat_h;
    const float max_C_at_sat = JC[0] * sat;
    // if sat == 0.f, the chroma is zero - assign the original luminance because there's no need to gamut map
    const float max_J_at_sat = (sat > 0.f) ? JC[1] / sat : JC[0];
    JC[0] = (JC[0] + max_J_at_sat) / 2.f;
    JC[1] = (JC[1] + max_C_at_sat) / 2.f;

    // Gamut-clip in Jch at constant hue and lightness,
    // e.g. find the max chroma available at current hue that doesn't
    // yield negative L'M'S' values, which will need to be clipped during conversion
    const float cos_H = native_cos(h);
    const float sin_H = native_sin(h);

    const float d0 = 1.6295499532821566e-11f;
    const float d = -0.56f;
    float Iz = JC[0] + d0;
    Iz /= (1.f + d - d * Iz);
    Iz = fmax(Iz, 0.f);

    const float4 AI[3] = { {  1.0f,  0.1386050432715393f,  0.0580473161561189f, 0.0f },
                          {  1.0f, -0.1386050432715393f, -0.0580473161561189f, 0.0f },
                          {  1.0f, -0.0960192420263190f, -0.8118918960560390f, 0.0f } };

    // Do a test conversion to L'M'S'
    const float4 IzAzBz = { Iz, JC[1] * cos_H, JC[1] * sin_H, 0.f };
    LMS.x = dot(AI[0], IzAzBz);
    LMS.y = dot(AI[1], IzAzBz);
    LMS.z = dot(AI[2], IzAzBz);

    // Clip chroma
    float max_C = JC[1];
    if(LMS.x < 0.f)
      max_C = fmin(-Iz / (AI[0].y * cos_H + AI[0].z * sin_H), max_C);

    if(LMS.y < 0.f)
      max_C = fmin(-Iz / (AI[1].y * cos_H + AI[1].z * sin_H), max_C);

    if(LMS.z < 0.f)
      max_C = fmin(-Iz / (AI[2].y * cos_H + AI[2].z * sin_H), max_C);

    // Project back to JzAzBz for real
    Jab.x = JC[0];
    Jab.y = max_C * cos_H;
    Jab.z = max_C * sin_H;

    XYZ_D65 = JzAzBz_2_XYZ(Jab);
  }
  else
  {
    float4 xyY = dt_XYZ_to_xyY(XYZ_D65);
    float4 JCH = xyY_to_dt_UCS_JCH(xyY, L_white);
    float4 HCB = dt_UCS_JCH_to_HCB(JCH);

    const float radius = hypot(HCB.y, HCB.z);
    const float sin_T = (radius > 0.f) ? HCB.y / radius : 0.f;
    const float cos_T = (radius > 0.f) ? HCB.z / radius : 0.f;
    const float M_rot_inv[2][2] = { { cos_T,  sin_T }, { -sin_T, cos_T } };
    // This would be the full matrice of direct rotation if we didn't need only its last row
    //const float M_rot_dir[2][2] = { { cos_T, -sin_T }, {  sin_T, cos_T } };

    float P = HCB.y;
    float W = sin_T * HCB.y + cos_T * HCB.z;

    float a = fmax(1.f + saturation_global + dot(opacities, saturation), 0.f);
    const float b = fmax(1.f + brilliance_global + dot(opacities, brilliance), 0.f);

    const float max_a = hypot(P, W) / P;
    a = soft_clip(a, 0.5f * max_a, max_a);

    const float P_prime = (a - 1.f) * P;
    const float W_prime = native_sqrt(sqf(P) * (1.f - sqf(a)) + sqf(W)) * b;

    HCB.y = fmax(M_rot_inv[0][0] * P_prime + M_rot_inv[0][1] * W_prime, 0.f);
    HCB.z = fmax(M_rot_inv[1][0] * P_prime + M_rot_inv[1][1] * W_prime, 0.f);

    JCH = dt_UCS_HCB_to_JCH(HCB);

    // Gamut mapping
    const float max_colorfulness = lookup_gamut(gamut_lut, JCH.z); // WARNING : this is M²
    const float max_chroma = 15.932993652962535f * native_powr(JCH.x * L_white, 0.6523997524738018f) * native_powr(max_colorfulness, 0.6007557017508491f) / L_white;
    const float4 JCH_gamut_boundary = { JCH.x, max_chroma, JCH.z, 0.f };
    const float4 HSB_gamut_boundary = dt_UCS_JCH_to_HSB(JCH_gamut_boundary);

    // Clip saturation at constant brightness
    float4 HSB = { HCB.x, (HCB.z > 0.f) ? HCB.y / HCB.z : 0.f, HCB.z, 0.f };
    HSB.y = soft_clip(HSB.y, 0.8f * HSB_gamut_boundary.y, HSB_gamut_boundary.y);

    JCH = dt_UCS_HSB_to_JCH(HSB);
    xyY = dt_UCS_JCH_to_xyY(JCH, L_white);
    XYZ_D65 = dt_xyY_to_XYZ(xyY);
  }

  // Project back to D50 pipeline RGB
  RGB = matrix_product_float4(XYZ_D65, matrix_out);

  if(mask_display)
  {
    // draw checkerboard
    float4 color;
    if(x % checker_1 < x % checker_2)
    {
      if(y % checker_1 < y % checker_2) color = checker_color_2;
      else color = checker_color_1;
    }
    else
    {
      if(y % checker_1 < y % checker_2) color = checker_color_1;
      else color = checker_color_2;
    }
    const float *op = (const float *)&opacities;
    float opacity = op[mask_type];
    const float opacity_comp = 1.0f - opacity;

    RGB = opacity_comp * color + opacity * fmax(RGB, 0.f);
    RGB.w = 1.0f; // alpha is opaque, we need to preview it
  }
  else
  {
    RGB = fmax(RGB, 0.f);
    RGB.w = pix_in.w; // alpha copy
  }

  write_imagef (out, (int2)(x, y), RGB);
}


/* helpers and kernel for the colorchecker module */
float fastlog2(float x)
{
  union { float f; unsigned int i; } vx = { x };
  union { unsigned int i; float f; } mx = { (vx.i & 0x007FFFFF) | 0x3f000000 };

  float y = vx.i;

  y *= 1.1920928955078125e-7f;

  return y - 124.22551499f
    - 1.498030302f * mx.f
    - 1.72587999f / (0.3520887068f + mx.f);
}

float fastlog(float x)
{
  return 0.69314718f * fastlog2(x);
}

float thinplate(const float4 x, const float4 y)
{
  const float r2 =
      (x.x - y.x) * (x.x - y.x) +
      (x.y - y.y) * (x.y - y.y) +
      (x.z - y.z) * (x.z - y.z);

  return r2 * fastlog(max(1e-8f, r2));
}

kernel void
colorchecker (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
              const int num_patches, global float4 *params)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  global float4 *source_Lab = params;
  global float4 *coeff_Lab = params + num_patches;
  global float4 *poly_Lab = params + 2 * num_patches;

  float4 ipixel = read_imagef(in, sampleri, (int2)(x, y));

  const float w = ipixel.w;

  float4 opixel = poly_Lab[0] + poly_Lab[1] * ipixel.x + poly_Lab[2] * ipixel.y + poly_Lab[3] * ipixel.z;

  for(int k = 0; k < num_patches; k++)
  {
    const float phi = thinplate(ipixel, source_Lab[k]);
    opixel += coeff_Lab[k] * phi;
  }

  opixel.w = w;

  write_imagef (out, (int2)(x, y), opixel);
}
