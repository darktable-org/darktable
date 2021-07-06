/*
    This file is part of darktable,
    copyright (c) 2021 darktable developers.

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
#include "noise_generator.h"

// use our own coordinate sampler
const sampler_t samplerA = CLK_NORMALIZED_COORDS_FALSE |
                           CLK_ADDRESS_NONE            |
                           CLK_FILTER_NEAREST;

typedef enum dt_isotropy_t
{
  DT_ISOTROPY_ISOTROPE = 0, // diffuse in all directions with same intensity
  DT_ISOTROPY_ISOPHOTE = 1, // diffuse more in the isophote direction (orthogonal to gradient)
  DT_ISOTROPY_GRADIENT = 2  // diffuse more in the gradient direction
} dt_isotropy_t;


kernel void
diffuse_init(read_only image2d_t in, write_only image2d_t out,
             const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  write_imagef(out, (int2)(x, y), (float4)0.f);
}

#define FSIZE 5

kernel void
diffuse_blur_bspline(read_only image2d_t in,
                     write_only image2d_t HF, write_only image2d_t LF,
                     const int mult, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 acc = 0.f;

  for(int ii = 0; ii < FSIZE; ++ii)
    for(int jj = 0; jj < FSIZE; ++jj)
    {
      const int row = clamp(y + mult * (int)(ii - (FSIZE - 1) / 2), 0, height - 1);
      const int col = clamp(x + mult * (int)(jj - (FSIZE - 1) / 2), 0, width - 1);
      const int k_index = (row * width + col);

      const float filter[FSIZE]
          = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
      const float filters = filter[ii] * filter[jj];

      acc += filters * read_imagef(in, samplerA, (int2)(col, row));
    }

  write_imagef(LF, (int2)(x, y), acc);
  write_imagef(HF, (int2)(x, y), read_imagef(in, samplerA, (int2)(x, y)) - acc);
}

// Discretization parameters for the Partial Derivative Equation solver
#define H 1         // spatial step
#define KAPPA 0.25f // 0.25 if h = 1, 1 if h = 2


inline void find_gradient(const float4 pixels[9], float4 xy[2])
{
  // Compute the gradient with centered finite differences in a 3×3 stencil
  // warning : x is vertical, y is horizontal
  xy[0] = (pixels[7] - pixels[1]) / 2.f;
  xy[1] = (pixels[5] - pixels[3]) / 2.f;
}

inline void find_laplacian(const float4 pixels[9], float4 xy[2])
{
  // Compute the laplacian with centered finite differences in a 3×3 stencil
  // warning : x is vertical, y is horizontal
  xy[0] = (pixels[7] + pixels[1]) - 2.f * pixels[4];
  xy[1] = (pixels[5] + pixels[3]) - 2.f * pixels[4];
}

inline float4 sqf(const float4 in)
{
  return in * in;
}

inline void rotation_matrix_isophote(const float4 c2,
                                     const float4 cos_theta, const float4 sin_theta,
                                     const float4 cos_theta2, const float4 sin_theta2,
                                     float4 a[2][2])
{
  // Write the coefficients of a square symmetrical matrice of rotation of the gradient :
  // [[ a11, a12 ],
  //  [ a12, a22 ]]
  // taken from https://www.researchgate.net/publication/220663968
  // c dampens the gradient direction
  a[0][0] = clamp(cos_theta2 + c2 * sin_theta2, 0.f, 1.f);
  a[1][1] = clamp(c2 * cos_theta2 + sin_theta2, 0.f, 1.f);
  a[0][1] = a[1][0] = clamp((c2 - 1.0f) * cos_theta * sin_theta, 0.f, 1.f);
}

inline void rotation_matrix_gradient(const float4 c2,
                                     const float4 cos_theta, const float4 sin_theta,
                                     const float4 cos_theta2, const float4 sin_theta2,
                                     float4 a[2][2])
{
  // Write the coefficients of a square symmetrical matrice of rotation of the gradient :
  // [[ a11, a12 ],
  //  [ a12, a22 ]]
  // based on https://www.researchgate.net/publication/220663968 and inverted
  // c dampens the isophote direction
  a[0][0] = clamp(c2 * cos_theta2 + sin_theta2, 0.f, 1.f);
  a[1][1] = clamp(cos_theta2 + c2 * sin_theta2, 0.f, 1.f);
  a[0][1] = a[1][0] = clamp((1.0f - c2) * cos_theta * sin_theta, 0.f, 1.f);
}


inline void build_matrix(const float4 a[2][2], float4 kern[9])
{
  const float4 b13 = a[0][1] / 2.0f;
  const float4 b11 = -b13;
  const float4 b22 = -2.0f * (a[0][0] + a[1][1]);

  // build the kernel of rotated anisotropic laplacian
  // from https://www.researchgate.net/publication/220663968 :
  // [ [ -a12 / 2,  a22,           a12 / 2  ],
  //   [ a11,      -2 (a11 + a22), a11      ],
  //   [ a12 / 2,   a22,          -a12 / 2  ] ]
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


inline void isotrope_laplacian(float4 kern[9])
{
  // see in https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Second-order-isotropic-finite-differences
  // for references (Oono & Puri)
  kern[0] = 0.25f;
  kern[1] = 0.5f;
  kern[2] = 0.25f;
  kern[3] = 0.5f;
  kern[4] = -3.f;
  kern[5] = 0.5f;
  kern[6] = 0.25f;
  kern[7] = 0.5f;
  kern[8] = 0.25f;
}


inline void compute_kern(const float4 c2,
                           const float4 cos_theta, const float4 sin_theta,
                           const float4 cos_theta2, const float4 sin_theta2,
                           const dt_isotropy_t isotropy_type,
                           float4 kern[9])
{
  // Build the matrix of rotation with anisotropy

  switch(isotropy_type)
  {
    case(DT_ISOTROPY_ISOTROPE):
    default:
    {
      isotrope_laplacian(kern);
      break;
    }
    case(DT_ISOTROPY_ISOPHOTE):
    {
      float4 a[2][2] = { { (float4)0.f } };
      rotation_matrix_isophote(c2, cos_theta, sin_theta, cos_theta2, sin_theta2, a);
      build_matrix(a, kern);
      break;
    }
    case(DT_ISOTROPY_GRADIENT):
    {
      float4 a[2][2] = { { (float4)0.f } };
      rotation_matrix_gradient(c2, cos_theta, sin_theta, cos_theta2, sin_theta2, a);
      build_matrix(a, kern);
      break;
    }
  }
}


kernel void
diffuse_pde(read_only image2d_t HF, read_only image2d_t LF,
            read_only image2d_t mask, const int has_mask,
            write_only image2d_t output,
            const int width, const int height,
            const float4 anisotropy, const int4 isotropy_type,
            const float regularization, const float variance_threshold,
            const float current_radius_square, const int mult,
            const float4 ABCD, const float strength)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const char opacity = (has_mask) ? read_imageui(mask, sampleri, (int2)(x, y)).x : 1;

  float4 out;

  if(opacity)
  {
    // non-local neighbours coordinates
    const int j_neighbours[3] = {
      clamp((x - mult * H), 0, width - 1),
      x,
      clamp((x + mult * H), 0, width - 1) };
    const int i_neighbours[3] = {
      clamp((y - mult * H), 0, height - 1),
      y,
      clamp((y + mult * H), 0, height - 1) };

    // fetch non-local pixels and store them locally and contiguously
    float4 neighbour_pixel_HF[9];
    float4 neighbour_pixel_LF[9];

    for(int ii = 0; ii < 3; ii++)
      for(int jj = 0; jj < 3; jj++)
      {
        neighbour_pixel_HF[3 * ii + jj] = read_imagef(HF, samplerA, (int2)(j_neighbours[ii], i_neighbours[jj]));
        neighbour_pixel_LF[3 * ii + jj] = read_imagef(LF, samplerA, (int2)(j_neighbours[ii], i_neighbours[jj]));
      }

    // build the local anisotropic convolution filters for gradients and laplacians
    // we use the low freq layer all the type as it is less likely to be nosy
    float4 gradient[2], laplacian[2];
    find_gradient(neighbour_pixel_LF, gradient);
    find_gradient(neighbour_pixel_HF, laplacian);

    const float4 magnitude_grad = hypot(gradient[0], gradient[1]);
    const float4 magnitude_lapl = hypot(laplacian[0], laplacian[1]);

    const float4 theta_grad = atan2(gradient[1], gradient[0]);
    const float4 theta_lapl = atan2(laplacian[1], laplacian[0]);

    const float4 cos_theta_grad = native_cos(theta_grad);
    const float4 cos_theta_lapl = native_cos(theta_lapl);
    const float4 sin_theta_grad = native_sin(theta_grad);
    const float4 sin_theta_lapl = native_sin(theta_lapl);

    const float4 cos_theta_grad_sq = sqf(cos_theta_grad);
    const float4 sin_theta_grad_sq = sqf(sin_theta_grad);
    const float4 cos_theta_lapl_sq = sqf(cos_theta_lapl);
    const float4 sin_theta_lapl_sq = sqf(sin_theta_lapl);

    // c² in https://www.researchgate.net/publication/220663968
    // warning : in c2[s], s is the order of the derivative
    const float4 c2[4] = { native_exp(-magnitude_grad / anisotropy.x),
                           native_exp(-magnitude_lapl / anisotropy.y),
                           native_exp(-magnitude_grad / anisotropy.z),
                           native_exp(-magnitude_lapl / anisotropy.w) };

    float4 kern_first[9], kern_second[9], kern_third[9], kern_fourth[9];
    compute_kern(c2[0], cos_theta_grad, sin_theta_grad, cos_theta_grad_sq, sin_theta_grad_sq, isotropy_type.x, kern_first);
    compute_kern(c2[1], cos_theta_lapl, sin_theta_lapl, cos_theta_lapl_sq, sin_theta_lapl_sq, isotropy_type.y, kern_second);
    compute_kern(c2[2], cos_theta_grad, sin_theta_grad, cos_theta_grad_sq, sin_theta_grad_sq, isotropy_type.z, kern_third);
    compute_kern(c2[3], cos_theta_lapl, sin_theta_lapl, cos_theta_lapl_sq, sin_theta_lapl_sq, isotropy_type.w, kern_fourth);

    // convolve filters and compute the variance and the regularization term
    float4 derivatives[4] = { (float4)0.f };
    float4 variance = (float4)0.f;

    #pragma unroll
    for(int k = 0; k < 9; k++)
    {
      derivatives[0] += kern_first[k] * neighbour_pixel_LF[k];
      derivatives[1] += kern_second[k] * neighbour_pixel_LF[k];
      derivatives[2] += kern_third[k] * neighbour_pixel_HF[k];
      derivatives[3] += kern_fourth[k] * neighbour_pixel_HF[k];
      variance += sqf(neighbour_pixel_HF[k]);
    }

    // Regularize the variance taking into account the blurring scale.
    // This allows to keep the scene-referred variance roughly constant
    // regardless of the wavelet scale where we compute it.
    // Prevents large scale halos when deblurring.
    variance /= 9.f / current_radius_square;
    variance = variance_threshold + native_sqrt(variance * regularization);

    // compute the update
    float4 acc = (float4)0.f;
    for(int k = 0; k < 4; k++) acc += derivatives[k] * ((float *)&ABCD)[k];
    float4 hf = read_imagef(HF, samplerA, (int2)(x, y));
    acc = (hf * strength + acc / variance);

    // update the solution
    float4 lf = read_imagef(LF, samplerA, (int2)(x, y));
    out = fmax(acc + lf, 0.f);
  }
  else
  {
    float4 hf = read_imagef(HF, samplerA, (int2)(x, y));
    float4 lf = read_imagef(LF, samplerA, (int2)(x, y));
    out = hf + lf;
  }

  write_imagef(output, (int2)(x, y), out);
}

kernel void
build_mask(read_only image2d_t in, write_only image2d_t mask,
           const float threshold, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;
  float4 pix_in = read_imagef(in, samplerA, (int2)(x, y));
  float m = (pix_in.x > threshold || pix_in.y > threshold || pix_in.z > threshold);
  write_imageui(mask, (int2)(x, y), m);
}

kernel void
inpaint_mask(write_only image2d_t inpainted, read_only image2d_t original,
              read_only image2d_t mask, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;
  float4 pix_in = read_imagef(original, samplerA, (int2)(x, y));
  char m = read_imageui(mask, samplerA, (int2)(x, y)).x;
  float4 pix_out = pix_in;

  if(m)
  {
    unsigned int state[4] = { splitmix32(x + 1), splitmix32((x + 1) * (y + 3)), splitmix32(1337), splitmix32(666) };
    xoshiro128plus(state);
    xoshiro128plus(state);
    xoshiro128plus(state);
    xoshiro128plus(state);
    pix_out = fabs(gaussian_noise_simd(pix_in, pix_in, state));
  }

  write_imagef(inpainted, (int2)(x, y), pix_out);
}
