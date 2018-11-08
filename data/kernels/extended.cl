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
#include "colorspace.cl"


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


typedef  enum _channelmixer_output_t
{
  CHANNEL_HUE=0,
  CHANNEL_SATURATION,
  CHANNEL_LIGHTNESS,
  CHANNEL_RED,
  CHANNEL_GREEN,
  CHANNEL_BLUE,
  CHANNEL_GRAY,
  CHANNEL_SIZE
} _channelmixer_output_t;


__kernel void
channelmixer (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
              const int gray_mix_mode, global const float *red, global const float *green, global const float *blue)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float hmix = clamp(pixel.x * red[CHANNEL_HUE], 0.0f, 1.0f) + pixel.y * green[CHANNEL_HUE] + pixel.z * blue[CHANNEL_HUE];
  float smix = clamp(pixel.x * red[CHANNEL_SATURATION], 0.0f, 1.0f) + pixel.y * green[CHANNEL_SATURATION] + pixel.z * blue[CHANNEL_SATURATION];
  float lmix = clamp(pixel.x * red[CHANNEL_LIGHTNESS], 0.0f, 1.0f) + pixel.y * green[CHANNEL_LIGHTNESS] + pixel.z * blue[CHANNEL_LIGHTNESS];

  if( hmix != 0.0f || smix != 0.0f || lmix != 0.0f )
  {
    float4 hsl = RGB_2_HSL(pixel);
    hsl.x = (hmix != 0.0f ) ? hmix : hsl.x;
    hsl.y = (smix != 0.0f ) ? smix : hsl.y;
    hsl.z = (lmix != 0.0f ) ? lmix : hsl.z;
    pixel = HSL_2_RGB(hsl);
  }

  float graymix = clamp(pixel.x * red[CHANNEL_GRAY]+ pixel.y * green[CHANNEL_GRAY] + pixel.z * blue[CHANNEL_GRAY], 0.0f, 1.0f);

  float rmix = clamp(pixel.x * red[CHANNEL_RED] + pixel.y * green[CHANNEL_RED] + pixel.z * blue[CHANNEL_RED], 0.0f, 1.0f);
  float gmix = clamp(pixel.x * red[CHANNEL_GREEN] + pixel.y * green[CHANNEL_GREEN] + pixel.z * blue[CHANNEL_GREEN], 0.0f, 1.0f);
  float bmix = clamp(pixel.x * red[CHANNEL_BLUE] + pixel.y * green[CHANNEL_BLUE] + pixel.z * blue[CHANNEL_BLUE], 0.0f, 1.0f);

  pixel = gray_mix_mode ? (float4)(graymix, graymix, graymix, pixel.w) : (float4)(rmix, gmix, bmix, pixel.w);

  write_imagef (out, (int2)(x, y), pixel); 
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
  float pmax = fmax(pixel.x, fmax(pixel.y, pixel.z));			// max value in RGB set
  float pmin = fmin(pixel.x, fmin(pixel.y, pixel.z));			// min value in RGB set
  float plum = (pmax + pmin) / 2.0f;				        // pixel luminocity
  float psat = (plum <= 0.5f) ? (pmax-pmin)/(1e-5f + pmax+pmin) : (pmax-pmin)/(1e-5f + fmax(0.0f, 2.0f-pmax-pmin));

  float pweight = clamp(((1.0f- (1.5f*psat)) + ((1.0f+(fabs(plum-0.5f)*2.0f))*(1.0f-bias))) / (1.0f+(1.0f-bias)), 0.0f, 1.0f); // The weight of pixel
  float saturation = strength*pweight;			// So lets calculate the final affection of filter on pixel

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
  float frandom = (float)urandom / 0xFFFFFFFFu;

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
  sRGB = (sRGB <= (float4)0.0031308f) ? 12.92f * sRGB : (1.0f + 0.055f) * pow(sRGB, (float4)1.0f/2.4f) - (float4)0.055f;
  sRGB = pow(fmax(((sRGB - (float4)1.0f) * lift + (float4)1.0f) * gain, (float4)0.0f), gamma_inv);
  sRGB = (sRGB <= (float4)0.04045f) ? sRGB / 12.92f : pow((sRGB + (float4)0.055f) / (1.0f + 0.055f), (float4)2.4f);
  Lab.xyz = XYZ_to_Lab(sRGB_to_XYZ(sRGB)).xyz;

  write_imagef (out, (int2)(x, y), Lab);
}

kernel void
colorbalance_lgg (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
              const float4 lift, const float4 gain, const float4 gamma_inv, const float saturation, const float contrast, const float grey)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 Lab = read_imagef(in, sampleri, (int2)(x, y));
  const float4 XYZ = Lab_to_XYZ(Lab);
  float4 RGB = XYZ_to_prophotorgb(XYZ);

  // saturation
  if (saturation != 1.0f) 
  {
    const float4 luma = XYZ.y;
    const float4 saturation4 = saturation;
    RGB = luma + saturation4 * (RGB - luma);
  }

  // Lift gamma gain
  RGB = (RGB <= (float4)0.0f) ? (float4)0.0f : pow(RGB, (float4)1.0f/2.2f);
  RGB = ((RGB - (float4)1.0f) * lift + (float4)1.0f) * gain;
  RGB = (RGB <= (float4)0.0f) ? (float4)0.0f : pow(RGB, gamma_inv * (float4)2.2f);
  
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
              const float4 lift, const float4 gain, const float4 gamma_inv, const float saturation, const float contrast, const float grey)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 Lab = read_imagef(in, sampleri, (int2)(x, y));
  const float4 XYZ = Lab_to_XYZ(Lab);
  float4 RGB = XYZ_to_prophotorgb(XYZ);

  // saturation
  if (saturation != 1.0f) 
  {
    const float4 luma = XYZ.y;
    const float4 saturation4 = saturation;
    RGB = luma + saturation4 * (RGB - luma);
  }
 
  // lift power slope
  RGB = RGB * gain + lift;
  RGB = (RGB <= (float4)0.0f) ? (float4)0.0f : pow(RGB, gamma_inv);
  
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
