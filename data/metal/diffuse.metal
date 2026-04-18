/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include <metal_stdlib>
using namespace metal;

/* ── B-spline wavelet kernels ────────────────────────────────────── */

// B-spline filter: 5-tap {1/16, 4/16, 6/16, 4/16, 1/16}
#define FSIZE 5
#define FSTART 2

constant float4 bspline_filter[FSIZE] = {
  float4(1.0f / 16.0f),
  float4(4.0f / 16.0f),
  float4(6.0f / 16.0f),
  float4(4.0f / 16.0f),
  float4(1.0f / 16.0f)
};

kernel void
blur_2D_Bspline_vertical(device const float4 *input  [[buffer(0)]],
                          device float4       *output [[buffer(1)]],
                          constant int        &width  [[buffer(2)]],
                          constant int        &height [[buffer(3)]],
                          constant int        &mult   [[buffer(4)]],
                          uint2 gid [[thread_position_in_grid]])
{
  const int x = gid.x;
  const int y = gid.y;
  if(x >= width || y >= height) return;

  float4 acc = float4(0.0f);
  for(int jj = 0; jj < FSIZE; ++jj)
  {
    const int yy = clamp(mult * (jj - FSTART) + y, 0, height - 1);
    acc += bspline_filter[jj] * input[yy * width + x];
  }

  output[y * width + x] = max(acc, float4(0.0f));
}

kernel void
blur_2D_Bspline_horizontal(device const float4 *input  [[buffer(0)]],
                            device float4       *output [[buffer(1)]],
                            constant int        &width  [[buffer(2)]],
                            constant int        &height [[buffer(3)]],
                            constant int        &mult   [[buffer(4)]],
                            uint2 gid [[thread_position_in_grid]])
{
  const int x = gid.x;
  const int y = gid.y;
  if(x >= width || y >= height) return;

  float4 acc = float4(0.0f);
  for(int ii = 0; ii < FSIZE; ++ii)
  {
    const int xx = clamp(mult * (ii - FSTART) + x, 0, width - 1);
    acc += bspline_filter[ii] * input[y * width + xx];
  }

  output[y * width + x] = max(acc, float4(0.0f));
}

kernel void
wavelets_detail_level(device const float4 *detail [[buffer(0)]],
                      device const float4 *LF     [[buffer(1)]],
                      device float4       *HF     [[buffer(2)]],
                      constant int        &width  [[buffer(3)]],
                      constant int        &height [[buffer(4)]],
                      uint2 gid [[thread_position_in_grid]])
{
  const int x = gid.x;
  const int y = gid.y;
  if(x >= width || y >= height) return;

  const int idx = y * width + x;
  HF[idx] = detail[idx] - LF[idx];
}


/* ── Diffuse/sharpen kernels ─────────────────────────────────────── */

// Discretization parameters for the PDE solver
#define H_STEP 1
#define KAPPA 0.25f

// Normalization scaling of the wavelet to approximate a laplacian
#define B_SPLINE_TO_LAPLACIAN 3.182727439285017f
#define B_SPLINE_TO_LAPLACIAN_2 10.129753952777762f

// Isotropy types matching dt_isotropy_t
#define DT_ISOTROPY_ISOTROPE 0
#define DT_ISOTROPY_ISOPHOTE 1
#define DT_ISOTROPY_GRADIENT 2

static inline float4 sqf(float4 v) { return v * v; }

static inline void find_gradient(thread const float4 pixels[9], thread float4 xy[2])
{
  xy[0] = (pixels[7] - pixels[1]) / 2.0f;
  xy[1] = (pixels[5] - pixels[3]) / 2.0f;
}

static inline void find_laplacian(thread const float4 pixels[9], thread float4 xy[2])
{
  xy[0] = (pixels[7] + pixels[1]) - 2.0f * pixels[4];
  xy[1] = (pixels[5] + pixels[3]) - 2.0f * pixels[4];
}

static inline void rotation_matrix_isophote(float4 c2,
                                             float4 cos_theta_sin_theta,
                                             float4 cos_theta2, float4 sin_theta2,
                                             thread float4 a[2][2])
{
  a[0][0] = cos_theta2 + c2 * sin_theta2;
  a[1][1] = c2 * cos_theta2 + sin_theta2;
  a[0][1] = a[1][0] = (c2 - 1.0f) * cos_theta_sin_theta;
}

static inline void rotation_matrix_gradient(float4 c2,
                                             float4 cos_theta_sin_theta,
                                             float4 cos_theta2, float4 sin_theta2,
                                             thread float4 a[2][2])
{
  a[0][0] = c2 * cos_theta2 + sin_theta2;
  a[1][1] = cos_theta2 + c2 * sin_theta2;
  a[0][1] = a[1][0] = (1.0f - c2) * cos_theta_sin_theta;
}

static inline void build_matrix(thread const float4 a[2][2], thread float4 kern[9])
{
  const float4 b11 = a[0][1] / 2.0f;
  const float4 b13 = -b11;
  const float4 b22 = -2.0f * (a[0][0] + a[1][1]);

  kern[0] = b11;
  kern[1] = a[1][1];
  kern[2] = b13;
  kern[3] = a[0][0];
  kern[4] = b22;
  kern[5] = a[0][0];
  kern[6] = b13;
  kern[7] = a[1][1];
  kern[8] = b11;
}

static inline void isotrope_laplacian(thread float4 kern[9])
{
  kern[0] = 0.25f;
  kern[1] = 0.5f;
  kern[2] = 0.25f;
  kern[3] = 0.5f;
  kern[4] = -3.0f;
  kern[5] = 0.5f;
  kern[6] = 0.25f;
  kern[7] = 0.5f;
  kern[8] = 0.25f;
}

static inline void compute_kern(float4 c2,
                                  float4 cos_theta_sin_theta,
                                  float4 cos_theta2, float4 sin_theta2,
                                  int isotropy_type,
                                  thread float4 kern[9])
{
  switch(isotropy_type)
  {
    case DT_ISOTROPY_ISOPHOTE:
    {
      float4 a[2][2] = { { float4(0.0f) } };
      rotation_matrix_isophote(c2, cos_theta_sin_theta, cos_theta2, sin_theta2, a);
      build_matrix(a, kern);
      break;
    }
    case DT_ISOTROPY_GRADIENT:
    {
      float4 a[2][2] = { { float4(0.0f) } };
      rotation_matrix_gradient(c2, cos_theta_sin_theta, cos_theta2, sin_theta2, a);
      build_matrix(a, kern);
      break;
    }
    case DT_ISOTROPY_ISOTROPE:
    default:
    {
      isotrope_laplacian(kern);
      break;
    }
  }
}


kernel void
diffuse_pde(device const float4  *HF_buf       [[buffer(0)]],
            device const float4  *LF_buf       [[buffer(1)]],
            device const uchar   *mask_buf     [[buffer(2)]],
            device float4        *output       [[buffer(3)]],
            constant int         &has_mask     [[buffer(4)]],
            constant int         &width        [[buffer(5)]],
            constant int         &height       [[buffer(6)]],
            constant float4      &anisotropy   [[buffer(7)]],
            constant int4        &isotropy_type [[buffer(8)]],
            constant float       &regularization [[buffer(9)]],
            constant float       &variance_threshold [[buffer(10)]],
            constant float       &current_radius_square [[buffer(11)]],
            constant int         &mult         [[buffer(12)]],
            constant float4      &ABCD         [[buffer(13)]],
            constant float       &strength     [[buffer(14)]],
            uint2 gid [[thread_position_in_grid]])
{
  const int x = gid.x;
  const int y = gid.y;
  if(x >= width || y >= height) return;

  const int idx = y * width + x;
  const uchar opacity = has_mask ? mask_buf[idx] : 1;

  const float4 regularization_factor = regularization * current_radius_square / 9.0f;

  float4 out;

  if(opacity)
  {
    // non-local neighbour coordinates
    const int j_neighbours[3] = {
      clamp(x - mult * H_STEP, 0, width - 1),
      x,
      clamp(x + mult * H_STEP, 0, width - 1)
    };
    const int i_neighbours[3] = {
      clamp(y - mult * H_STEP, 0, height - 1),
      y,
      clamp(y + mult * H_STEP, 0, height - 1)
    };

    // fetch non-local pixels
    float4 neighbour_pixel_HF[9];
    float4 neighbour_pixel_LF[9];

    for(int ii = 0; ii < 3; ii++)
      for(int jj = 0; jj < 3; jj++)
      {
        const int nidx = i_neighbours[jj] * width + j_neighbours[ii];
        neighbour_pixel_HF[3 * ii + jj] = HF_buf[nidx];
        neighbour_pixel_LF[3 * ii + jj] = LF_buf[nidx];
      }

    // build local anisotropic convolution filters
    float4 gradient[2], laplacian[2];
    find_gradient(neighbour_pixel_LF, gradient);
    find_gradient(neighbour_pixel_HF, laplacian);

    const float4 magnitude_grad = sqrt(sqf(gradient[0]) + sqf(gradient[1]));
    gradient[0] = select(gradient[0] / magnitude_grad, float4(1.0f), magnitude_grad == 0.0f);
    gradient[1] = select(gradient[1] / magnitude_grad, float4(0.0f), magnitude_grad == 0.0f);

    const float4 magnitude_lapl = sqrt(sqf(laplacian[0]) + sqf(laplacian[1]));
    laplacian[0] = select(laplacian[0] / magnitude_lapl, float4(1.0f), magnitude_lapl == 0.0f);
    laplacian[1] = select(laplacian[1] / magnitude_lapl, float4(0.0f), magnitude_lapl == 0.0f);

    const float4 cos_theta_grad_sq = sqf(gradient[0]);
    const float4 sin_theta_grad_sq = sqf(gradient[1]);
    const float4 cos_theta_sin_theta_grad = gradient[0] * gradient[1];
    const float4 cos_theta_lapl_sq = sqf(laplacian[0]);
    const float4 sin_theta_lapl_sq = sqf(laplacian[1]);
    const float4 cos_theta_sin_theta_lapl = laplacian[0] * laplacian[1];

    // c² anisotropy coefficients
    const float4 c2[4] = {
      exp(-magnitude_grad * anisotropy.x),
      exp(-magnitude_lapl * anisotropy.y),
      exp(-magnitude_grad * anisotropy.z),
      exp(-magnitude_lapl * anisotropy.w)
    };

    float4 kern_first[9], kern_second[9], kern_third[9], kern_fourth[9];
    compute_kern(c2[0], cos_theta_sin_theta_grad, cos_theta_grad_sq, sin_theta_grad_sq, isotropy_type.x, kern_first);
    compute_kern(c2[1], cos_theta_sin_theta_lapl, cos_theta_lapl_sq, sin_theta_lapl_sq, isotropy_type.y, kern_second);
    compute_kern(c2[2], cos_theta_sin_theta_grad, cos_theta_grad_sq, sin_theta_grad_sq, isotropy_type.z, kern_third);
    compute_kern(c2[3], cos_theta_sin_theta_lapl, cos_theta_lapl_sq, sin_theta_lapl_sq, isotropy_type.w, kern_fourth);

    // convolve filters and compute variance + regularization
    float4 derivatives[4] = { float4(0.0f) };
    float4 variance = float4(0.0f);

    for(int k = 0; k < 9; k++)
    {
      derivatives[0] += kern_first[k] * neighbour_pixel_LF[k];
      derivatives[1] += kern_second[k] * neighbour_pixel_LF[k];
      derivatives[2] += kern_third[k] * neighbour_pixel_HF[k];
      derivatives[3] += kern_fourth[k] * neighbour_pixel_HF[k];
      variance += sqf(neighbour_pixel_HF[k]);
    }

    variance = variance_threshold + variance * regularization_factor;

    // compute update
    float4 acc = float4(0.0f);
    acc += derivatives[0] * ABCD.x;
    acc += derivatives[1] * ABCD.y;
    acc += derivatives[2] * ABCD.z;
    acc += derivatives[3] * ABCD.w;

    float4 hf = HF_buf[idx];
    acc = hf * strength + acc / variance;

    float4 lf = LF_buf[idx];
    out = max(acc + lf, float4(0.0f));
  }
  else
  {
    out = HF_buf[idx] + LF_buf[idx];
  }

  output[idx] = out;
}


/* ── Mask kernels ────────────────────────────────────────────────── */

kernel void
build_mask(device const float4 *input   [[buffer(0)]],
           device uchar        *mask    [[buffer(1)]],
           constant float      &threshold [[buffer(2)]],
           constant int        &width   [[buffer(3)]],
           constant int        &height  [[buffer(4)]],
           uint2 gid [[thread_position_in_grid]])
{
  const int x = gid.x;
  const int y = gid.y;
  if(x >= width || y >= height) return;

  const int idx = y * width + x;
  const float4 pix = input[idx];
  mask[idx] = (pix.x > threshold || pix.y > threshold || pix.z > threshold) ? 1 : 0;
}


/* ── Noise generator helpers ─────────────────────────────────────── */

static inline uint splitmix32(uint seed)
{
  ulong result = ((ulong)seed ^ ((ulong)seed >> 33)) * 0x62a9d9ed799705f5UL;
  result = (result ^ (result >> 28)) * 0xcb24d0a5c88c35b3UL;
  return (uint)(result >> 32);
}

static inline uint rol32(uint x, int k)
{
  return (x << k) | (x >> (32 - k));
}

static inline float xoshiro128plus(thread uint state[4])
{
  const uint result = state[0] + state[3];
  const uint t = state[1] << 9;

  state[2] ^= state[0];
  state[3] ^= state[1];
  state[1] ^= state[2];
  state[0] ^= state[3];

  state[2] ^= t;
  state[3] = rol32(state[3], 11);

  return (float)(result >> 8) * 0x1.0p-24f;
}

static inline float4 gaussian_noise_simd(float4 mu, float4 sigma, thread uint state[4])
{
  float4 u1, u2;

  u1.x = xoshiro128plus(state);
  u1.y = xoshiro128plus(state);
  u1.z = xoshiro128plus(state);

  u2.x = xoshiro128plus(state);
  u2.y = xoshiro128plus(state);
  u2.z = xoshiro128plus(state);

  u1 = max(u1, float4(FLT_MIN));

  const float4 flip      = float4(1.0f, 0.0f, 1.0f, 0.0f);
  const float4 flip_comp = float4(0.0f, 1.0f, 0.0f, 0.0f);

  const float4 noise = flip * sqrt(-2.0f * log(u1)) * cos(2.0f * M_PI_F * u2) +
                       flip_comp * sqrt(-2.0f * log(u1)) * sin(2.0f * M_PI_F * u2);
  return noise * sigma + mu;
}


kernel void
inpaint_mask(device float4       *inpainted [[buffer(0)]],
             device const float4 *original  [[buffer(1)]],
             device const uchar  *mask      [[buffer(2)]],
             constant int        &width     [[buffer(3)]],
             constant int        &height    [[buffer(4)]],
             uint2 gid [[thread_position_in_grid]])
{
  const int x = gid.x;
  const int y = gid.y;
  if(x >= width || y >= height) return;

  const int idx = y * width + x;
  const float4 pix_in = original[idx];
  const uchar m = mask[idx];
  float4 pix_out = pix_in;

  if(m)
  {
    uint state[4] = { splitmix32(x + 1), splitmix32((x + 1) * (y + 3)), splitmix32(1337), splitmix32(666) };
    xoshiro128plus(state);
    xoshiro128plus(state);
    xoshiro128plus(state);
    xoshiro128plus(state);
    pix_out = abs(gaussian_noise_simd(pix_in, pix_in, state));
  }

  inpainted[idx] = pix_out;
}
