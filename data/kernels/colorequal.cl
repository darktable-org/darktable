/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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
typedef enum dt_iop_colorequal_channel_t
{
  HUE = 0,
  SATURATION = 1,
  BRIGHTNESS = 2,
  NUM_CHANNELS = 3,
  GRAD_SWITCH = 4,
  SATURATION_GRAD = SATURATION + GRAD_SWITCH,
  BRIGHTNESS_GRAD = BRIGHTNESS + GRAD_SWITCH
} dt_iop_colorequal_channel_t;

#define SAT_EFFECT 2.0f
#define BRIGHT_EFFECT 8.0f

#include "common.h"
#include "colorspace.h"
#include "color_conversion.h"

#define SATSIZE 4096.0f

static inline float _interpolatef(const float a, const float b, const float c)
{
  return a * (b - c) + c;
}

static inline float _get_satweight(const float sat, global float *weights)
{
  const float isat = SATSIZE * (1.0f + clamp(sat, -1.0f, 1.0f - (1.0f / SATSIZE)));
  const float base = floor(isat);
  const int i = (int)base;
  return weights[i] + (isat - base) * (weights[i+1] - weights[i]);
}

static inline float _scharr_gradient(global float *in,
                                     const size_t k,
                                     const int w)
{
  const float gx = 47.0f / 255.0f * (in[k-w-1] - in[k-w+1] + in[k+w-1] - in[k+w+1])
                + 162.0f / 255.0f * (in[k-1]   - in[k+1]);
  const float gy = 47.0f / 255.0f * (in[k-w-1] - in[k+w-1] + in[k-w+1] - in[k+w+1])
                + 162.0f / 255.0f * (in[k-w]   - in[k+w]);
  return dt_fast_hypot(gx, gy);
}

static inline float gamut_map_HSB(const float4 HSB, global float *gamut_LUT, const float L_white)
{
  const float4 JCH = dt_UCS_HSB_to_JCH(HSB);
  const float max_colorfulness = lookup_gamut(gamut_LUT, JCH.z);
  const float max_chroma = 15.932993652962535f * dtcl_pow(JCH.x * L_white, 0.6523997524738018f) * dtcl_pow(max_colorfulness, 0.6007557017508491f) / L_white;
  const float4 JCH_gamut_boundary = { JCH.x, max_chroma, JCH.z, 0.0f };
  const float4 HSB_gamut_boundary = dt_UCS_JCH_to_HSB(JCH_gamut_boundary);

  // Soft-clip the current pixel saturation at constant brightness
  return soft_clip(HSB.y, 0.8f * HSB_gamut_boundary.y, HSB_gamut_boundary.y);
}

__kernel void init_covariance(global float4 *covariance,
                              global float2 *uv,
                              const int width,
                              const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int k = mad24(row, width, col);

  covariance[k].x = uv[k].x * uv[k].x;
  covariance[k].y = uv[k].x * uv[k].y;
  covariance[k].z = uv[k].x * uv[k].y;
  covariance[k].w = uv[k].y * uv[k].y;
}

__kernel void finish_covariance(global float4 *covariance,
                                global float2 *uv,
                                const int width,
                                const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int k = mad24(row, width, col);

  covariance[k].x -= uv[k].x * uv[k].x;
  covariance[k].y -= uv[k].x * uv[k].y;
  covariance[k].z -= uv[k].x * uv[k].y;
  covariance[k].w -= uv[k].y * uv[k].y;
}

__kernel void prepare_prefilter(global float2 *uv,
                                global float4 *covariance,
                                global float4 *a,
                                global float2 *b,
                                const float eps,
                                const int width,
                                const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int k = mad24(row, width, col);

  const float4 sigma = { covariance[k].x + eps, covariance[k].y, covariance[k].z, covariance[k].w + eps};
  const float det = sigma.x * sigma.w - sigma.y * sigma.z;

  if(fabs(det) > 4.0f * FLT_EPSILON)
  {
    const float4 sigma_inv = { sigma.w / det, -sigma.y / det, -sigma.z / det, sigma.x / det };
    a[k].x = covariance[k].x * sigma_inv.x + covariance[k].y * sigma_inv.y;
    a[k].y = covariance[k].x * sigma_inv.z + covariance[k].y * sigma_inv.w;
    a[k].z = covariance[k].z * sigma_inv.x + covariance[k].w * sigma_inv.y;
    a[k].w = covariance[k].z * sigma_inv.z + covariance[k].w * sigma_inv.w;
  }
  else
    a[k].x = a[k].y = a[k].z = a[k].w = 0.0f;

  b[k].x = uv[k].x - a[k].x * uv[k].x - a[k].y * uv[k].y;
  b[k].y = uv[k].y - a[k].z * uv[k].x - a[k].w * uv[k].y;
}

__kernel void apply_prefilter(global float2 *uv,
                              global float *saturation,
                              global float4 *a,
                              global float2 *b,
                              global float *weights,
                              const float sat_shift,
                              const int width,
                              const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int k = mad24(row, width, col);

  const float2 UV = uv[k];
  const float2 cv = (float2)(a[k].x * UV.x + a[k].y * UV.y + b[k].x,
                             a[k].z * UV.x + a[k].w * UV.y + b[k].y);

  const float satweight = _get_satweight(saturation[k] - sat_shift, weights);
  uv[k].x = _interpolatef(satweight, cv.x, UV.x);
  uv[k].y = _interpolatef(satweight, cv.y, UV.y);
}

__kernel void prepare_correlations(global float2 *corrections,
                                   global float *b_corrections,
                                   global float2 *uv,
                                   global float4 *correlations,
                                   const int width,
                                   const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int k = mad24(row, width, col);

  correlations[k].x = uv[k].x * corrections[k].y;
  correlations[k].y = uv[k].y * corrections[k].y;
  correlations[k].z = uv[k].x * b_corrections[k];
  correlations[k].w = uv[k].y * b_corrections[k];
}

// also write covariance
__kernel void finish_correlations(global float2 *corrections,
                                  global float *b_corrections,
                                  global float2 *uv,
                                  global float4 *correlations,
                                  global float4 *covariance,
                                  const int width,
                                  const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int k = mad24(row, width, col);

  covariance[k].x -= uv[k].x * uv[k].x;
  covariance[k].y -= uv[k].x * uv[k].y;
  covariance[k].z -= uv[k].x * uv[k].y;
  covariance[k].w -= uv[k].y * uv[k].y;

  correlations[k].x -= uv[k].x * corrections[k].y;
  correlations[k].y -= uv[k].y * corrections[k].y;
  correlations[k].z -= uv[k].x * b_corrections[k];
  correlations[k].w -= uv[k].y * b_corrections[k];
}

__kernel void final_guide(global float4 *covariance,
                          global float4 *correlations,
                          global float2 *corrections,
                          global float *b_corrections,
                          global float2 *uv,
                          global float4 *a,
                          global float2 *b,
                          const float eps,
                          const int width,
                          const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int k = mad24(row, width, col);

  const float4 sigma = { covariance[k].x + eps, covariance[k].y, covariance[k].z, covariance[k].w + eps };
  const float det = sigma.x * sigma.w - sigma.y * sigma.z;

  if(fabs(det) > 4.0f * FLT_EPSILON)
  {
    const float4 sigma_inv = { sigma.w / det, -sigma.y / det, -sigma.z / det, sigma.x / det };
    a[k].x = correlations[k].x * sigma_inv.x + correlations[k].y * sigma_inv.y;
    a[k].y = correlations[k].x * sigma_inv.z + correlations[k].y * sigma_inv.w;
    a[k].z = correlations[k].z * sigma_inv.x + correlations[k].w * sigma_inv.y;
    a[k].w = correlations[k].z * sigma_inv.z + correlations[k].w * sigma_inv.w;
  }
  else
    a[k].x = a[k].y = a[k].z = a[k].w = 0.0f;

  b[k].x = corrections[k].y - a[k].x * uv[k].x - a[k].y * uv[k].y;
  b[k].y = b_corrections[k] - a[k].z * uv[k].x - a[k].w * uv[k].y;
}

__kernel void apply_guided(global float2 *uv,
                           global float *saturation,
                           global float *scharr,
                           global float4 *a,
                           global float2 *b,
                           global float2 *corrections,
                           global float *b_corrections,
                           global float *weights,
                           const float sat_shift,
                           const float bright_shift,
                           const int width,
                           const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int k = mad24(row, width, col);

  const float2 CV = { a[k].x * uv[k].x + a[k].y * uv[k].y + b[k].x,
                      a[k].z * uv[k].x + a[k].w * uv[k].y + b[k].y };

  corrections[k].y = _interpolatef(_get_satweight(saturation[k] - sat_shift, weights), CV.x, 1.0f);
  const float gradient_weight = 1.0f - clamp(scharr[k], 0.0f, 1.0f);
  b_corrections[k] = _interpolatef(gradient_weight * _get_satweight(saturation[k] - bright_shift, weights), CV.y, 0.0f);
}

__kernel void sample_input(__read_only image2d_t dev_in,
                           global float *saturation,
                           global float *lum,
                           global float2 *uv,
                           global float4 *pix_out,
                           global float4 *mat,
                           const int width,
                           const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int k = mad24(row, width, col);

  const float4 pix_in = read_imagef(dev_in, samplerA, (int2)(col, row));
  // calc saturation from input data
  const float dmin = fmin(pix_in.x, fmin(pix_in.y, pix_in.z));
  const float dmax = fmax(pix_in.x, fmax(pix_in.y, pix_in.z));
  const float delta = dmax - dmin;
  saturation[k] = (dmax > NORM_MIN && delta > NORM_MIN) ? delta / dmax : 0.0f;

  const float4 M[3] = { mat[0], mat[1], mat[2] };
  const float4 XYZ_D65 = matrix_dot(pix_in, M);
  const float4 xyY = dt_D65_XYZ_to_xyY(XYZ_D65);

  lum[k] = Y_to_dt_UCS_L_star(xyY.z);
  uv[k] = xyY_to_dt_UCS_UV(xyY);
  pix_out[k].w = pix_in.w;
}

__kernel void write_output(__write_only image2d_t dev_out,
                            global float4 *pix_out,
                            global float2 *corrections,
                            global float *b_corrections,
                            global float4 *mat,
                            global float *gamut_LUT,
                            const float white,
                            const int width,
                            const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const float4 M[3] = { mat[0], mat[1], mat[2] };

  const int k = mad24(row, width, col);

  pix_out[k].x += corrections[k].x;
  pix_out[k].y = fmax(0.0f, pix_out[k].y * (1.0f + SAT_EFFECT * (corrections[k].y - 1.0f)));
  pix_out[k].z = fmax(0.0f, pix_out[k].z * (1.0f + BRIGHT_EFFECT * b_corrections[k]));

  pix_out[k].y = gamut_map_HSB(pix_out[k], gamut_LUT, white);
  const float4 XYZ_D65 = dt_UCS_HSB_to_XYZ(pix_out[k], white);
  const float4 pout = matrix_dot(XYZ_D65, M);
  write_imagef(dev_out, (int2)(col, row), pout);
}

__kernel void write_visual (__write_only image2d_t dev_out,
                            global float4 *pixout,
                            global float2 *corrections,
                            global float *b_corrections,
                            global float *saturation,
                            global float *scharr,
                            global float *weights,
                            const float bright_shift,
                            const float sat_shift,
                            const float white,
                            const int mode,
                            const int width,
                            const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int k = mad24(row, width, col);
  const float val = dtcl_sqrt(fmax(0.0f,  pixout[k].z * white));
  float corr = 0.0f;
  switch(mode)
  {
    case BRIGHTNESS:
        corr = BRIGHT_EFFECT * b_corrections[k];
        break;
    case SATURATION:
        corr = SAT_EFFECT * (corrections[k].y - 1.0f);
        break;
    case BRIGHTNESS_GRAD:
        corr = _get_satweight(saturation[k] - bright_shift, weights) - 0.5f;
        break;
    case SATURATION_GRAD:
        corr = _get_satweight(saturation[k] - sat_shift, weights) - 0.5f;
        break;
    default:  // HUE
        corr = 0.2f * corrections[k].x;
  }

  const int neg = corr < 0.0f;
  corr = fabs(corr);
  corr = corr < 2e-3f ? 0.0f : corr;

  float4 pout = (float4)(fmax(0.0f, neg ? val - corr : val),
                         fmax(0.0f, val - corr),
                         fmax(0.0f, neg ? val : val - corr),
                         0.0f);


  if(mode == BRIGHTNESS)
  {
    if(scharr[k] > 0.1f)
    {
      pout.x = pout.z = 0.0f;
      pout.y = scharr[k];
    }
  }
  write_imagef(dev_out, (int2)(col, row), pout);
}

__kernel void draw_weight(__write_only image2d_t dev_out,
                          global float *weights,
                          const float bright_shift,
                          const float sat_shift,
                          const int mode,
                          const int width,
                          const int height)
{
  const int col = get_global_id(0);

  if(col >= width) return;

  const float eps = 0.5f / (float)height;
  const float shift = (mode == SATURATION_GRAD) ? sat_shift : bright_shift;
  const float4 mark = (float4)( 0.0f, 1.0f, 0.0f, 0.0f);
  for(int i = 0; i < 16; i++)
  {
    const float pos = (float)(16*col +i) / (float)(16*width);
    const float weight = _get_satweight(pos - shift, weights);
    if(weight > eps && weight < 1.0f - eps)
    {
      const int row = (int)((1.0f - weight) * (float)(height-1));
      write_imagef(dev_out, (int2)(col, row), mark);
    }
  }
}

__kernel void process_data(global float2 *uv,
                           global float *Lscharr,
                           global float *saturation,
                           global float2 *corrections,
                           global float *b_corrections,
                           global float4 *pixout,
                           global float *LUT_saturation,
                           global float *LUT_hue,
                           global float *LUT_brightness,
                           const float white,
                           const float gradient_amp,
                           const int guiding,
                           const int width,
                           const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int k = mad24(row, width, col);

  const float4 JCH = dt_UCS_LUV_to_JCH(Lscharr[k], white, uv[k]);
  const float4 HSB = dt_UCS_JCH_to_HSB(JCH);

  const float hue = HSB.x;
  const float sat = HSB.y;
  pixout[k].x = hue;
  pixout[k].y = sat;
  pixout[k].z = HSB.z;

  if(guiding)
  {
    const int kk = mad24(clamp(row, 1, height - 2), width, clamp(col, 1, width - 2));

    const float kscharr = fmax(0.0f, _scharr_gradient(saturation, kk, width) - 0.02f);
    Lscharr[k] = gradient_amp * kscharr * kscharr;
  }

  if(JCH.y > NORM_MIN)
  {
    corrections[k].x = lookup_gamut(LUT_hue, hue);
    corrections[k].y = lookup_gamut(LUT_saturation, hue);
    b_corrections[k] = sat * (lookup_gamut(LUT_brightness, hue) - 1.0f);
  }
  else
  {
    corrections[k].x = 0.0f;
    corrections[k].y = 1.0f;
    b_corrections[k] = 0.0f;
  }
}

// bilinear interpolators on global buffers
kernel void bilinear4(global float4 *in,
                      const int width_in,
                      const int height_in,
                      global float4 *out,
                      const int width_out,
                      const int height_out)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width_out || y >= height_out) return;

  // Relative coordinates of the pixel in output space
  const float x_out = (float)x /(float)width_out;
  const float y_out = (float)y /(float)height_out;

  // Corresponding absolute coordinates of the pixel in input space
  const float x_in = x_out * (float)width_in;
  const float y_in = y_out * (float)height_in;

  // Nearest neighbours coordinates in input space
  int x_prev = (int)floor(x_in);
  int x_next = x_prev + 1;
  int y_prev = (int)floor(y_in);
  int y_next = y_prev + 1;

  x_prev = (x_prev < width_in) ? x_prev : width_in - 1;
  x_next = (x_next < width_in) ? x_next : width_in - 1;
  y_prev = (y_prev < height_in) ? y_prev : height_in - 1;
  y_next = (y_next < height_in) ? y_next : height_in - 1;


  // Nearest pixels in input array (nodes in grid)
  const float4 Q_NW = in[mad24(y_prev, width_in, x_prev)]; //read_imagef(in, samplerA, (int2)(x_prev, y_prev));
  const float4 Q_NE = in[mad24(y_prev, width_in, x_next)]; //read_imagef(in, samplerA, (int2)(x_next, y_prev));
  const float4 Q_SE = in[mad24(y_next, width_in, x_next)]; // read_imagef(in, samplerA, (int2)(x_next, y_next));
  const float4 Q_SW = in[mad24(y_next, width_in, x_prev)]; // read_imagef(in, samplerA, (int2)(x_prev, y_next));

  // Spatial differences between nodes
  const float Dy_next = (float)y_next - y_in;
  const float Dy_prev = 1.f - Dy_next; // because next - prev = 1
  const float Dx_next = (float)x_next - x_in;
  const float Dx_prev = 1.f - Dx_next; // because next - prev = 1

  // Interpolate
  const float4 pix_out = Dy_prev * (Q_SW * Dx_next + Q_SE * Dx_prev) +
                         Dy_next * (Q_NW * Dx_next + Q_NE * Dx_prev);

  out[mad24(y, width_out, x)] = pix_out;
}

kernel void bilinear2(global float2 *in,
                      const int width_in,
                      const int height_in,
                      global float2 *out,
                      const int width_out,
                      const int height_out)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width_out || y >= height_out) return;

  // Relative coordinates of the pixel in output space
  const float x_out = (float)x /(float)width_out;
  const float y_out = (float)y /(float)height_out;

  // Corresponding absolute coordinates of the pixel in input space
  const float x_in = x_out * (float)width_in;
  const float y_in = y_out * (float)height_in;

  // Nearest neighbours coordinates in input space
  int x_prev = (int)floor(x_in);
  int x_next = x_prev + 1;
  int y_prev = (int)floor(y_in);
  int y_next = y_prev + 1;

  x_prev = (x_prev < width_in) ? x_prev : width_in - 1;
  x_next = (x_next < width_in) ? x_next : width_in - 1;
  y_prev = (y_prev < height_in) ? y_prev : height_in - 1;
  y_next = (y_next < height_in) ? y_next : height_in - 1;


  // Nearest pixels in input array (nodes in grid)
  const float2 Q_NW = in[mad24(y_prev, width_in, x_prev)]; //read_imagef(in, samplerA, (int2)(x_prev, y_prev));
  const float2 Q_NE = in[mad24(y_prev, width_in, x_next)]; //read_imagef(in, samplerA, (int2)(x_next, y_prev));
  const float2 Q_SE = in[mad24(y_next, width_in, x_next)]; // read_imagef(in, samplerA, (int2)(x_next, y_next));
  const float2 Q_SW = in[mad24(y_next, width_in, x_prev)]; // read_imagef(in, samplerA, (int2)(x_prev, y_next));

  // Spatial differences between nodes
  const float Dy_next = (float)y_next - y_in;
  const float Dy_prev = 1.f - Dy_next; // because next - prev = 1
  const float Dx_next = (float)x_next - x_in;
  const float Dx_prev = 1.f - Dx_next; // because next - prev = 1

  // Interpolate
  const float2 pix_out = Dy_prev * (Q_SW * Dx_next + Q_SE * Dx_prev) +
                         Dy_next * (Q_NW * Dx_next + Q_NE * Dx_prev);

  out[mad24(y, width_out, x)] = pix_out;
}

kernel void bilinear1(global float *in,
                      const int width_in,
                      const int height_in,
                      global float *out,
                      const int width_out,
                      const int height_out)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width_out || y >= height_out) return;

  // Relative coordinates of the pixel in output space
  const float x_out = (float)x /(float)width_out;
  const float y_out = (float)y /(float)height_out;

  // Corresponding absolute coordinates of the pixel in input space
  const float x_in = x_out * (float)width_in;
  const float y_in = y_out * (float)height_in;

  // Nearest neighbours coordinates in input space
  int x_prev = (int)floor(x_in);
  int x_next = x_prev + 1;
  int y_prev = (int)floor(y_in);
  int y_next = y_prev + 1;

  x_prev = (x_prev < width_in) ? x_prev : width_in - 1;
  x_next = (x_next < width_in) ? x_next : width_in - 1;
  y_prev = (y_prev < height_in) ? y_prev : height_in - 1;
  y_next = (y_next < height_in) ? y_next : height_in - 1;


  // Nearest pixels in input array (nodes in grid)
  const float Q_NW = in[mad24(y_prev, width_in, x_prev)]; //read_imagef(in, samplerA, (int2)(x_prev, y_prev));
  const float Q_NE = in[mad24(y_prev, width_in, x_next)]; //read_imagef(in, samplerA, (int2)(x_next, y_prev));
  const float Q_SE = in[mad24(y_next, width_in, x_next)]; // read_imagef(in, samplerA, (int2)(x_next, y_next));
  const float Q_SW = in[mad24(y_next, width_in, x_prev)]; // read_imagef(in, samplerA, (int2)(x_prev, y_next));

  // Spatial differences between nodes
  const float Dy_next = (float)y_next - y_in;
  const float Dy_prev = 1.f - Dy_next; // because next - prev = 1
  const float Dx_next = (float)x_next - x_in;
  const float Dx_prev = 1.f - Dx_next; // because next - prev = 1

  // Interpolate
  const float pix_out = Dy_prev * (Q_SW * Dx_next + Q_SE * Dx_prev) +
                        Dy_next * (Q_NW * Dx_next + Q_NE * Dx_prev);

  out[mad24(y, width_out, x)] = pix_out;
}
