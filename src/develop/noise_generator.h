/*
   This file is part of darktable,
   Copyright (C) 2020 - darktable developers.

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

#include "develop/openmp_maths.h"


typedef enum dt_noise_distribution_t
{
  DT_NOISE_UNIFORM = 0,   // $DESCRIPTION: "Uniform"
  DT_NOISE_GAUSSIAN = 1,  // $DESCRIPTION: "Gaussian"
  DT_NOISE_POISSONIAN = 2 // $DESCRIPTION: "Poissonian"
} dt_noise_distribution_t;


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline uint32_t splitmix32(const uint64_t seed)
{
  // fast random number generator
  // reference : http://prng.di.unimi.it/splitmix64.c
  uint64_t result = (seed ^ (seed >> 33)) * 0x62a9d9ed799705f5ul;
  result = (result ^ (result >> 28)) * 0xcb24d0a5c88c35b3ul;
  return (uint32_t)(result >> 32);
}


#ifdef _OPENMP
#pragma omp declare simd uniform(k)
#endif
static inline uint32_t rol32(const uint32_t x, const int k)
{
  return (x << k) | (x >> (32 - k));
}


#ifdef _OPENMP
#pragma omp declare simd aligned(state:64)
#endif
static inline float xoshiro128plus(uint32_t state[4])
{
  // fast random number generator
  // reference : http://prng.di.unimi.it/
  const unsigned int result = state[0] + state[3];
  const unsigned int t = state[1] << 9;

  state[2] ^= state[0];
  state[3] ^= state[1];
  state[1] ^= state[2];
  state[0] ^= state[3];

  state[2] ^= t;
  state[3] = rol32(state[3], 11);

  return (float)(result >> 8) * 0x1.0p-24f; // take the first 24 bits and put them in mantissa
}


#ifdef _OPENMP
#pragma omp declare simd uniform(sigma) aligned(state:64)
#endif
static inline float uniform_noise(const float mu, const float sigma, uint32_t state[4])
{
  return mu + 2.0f * (xoshiro128plus(state) - 0.5f) * sigma;
}


#ifdef _OPENMP
#pragma omp declare simd uniform(sigma) aligned(state:64)
#endif
static inline float gaussian_noise(const float mu, const float sigma, const int flip, uint32_t state[4])
{
  // Create gaussian noise centered in mu of standard deviation sigma
  // state should be initialized with xoshiro256_init() before calling and private in thread
  // flip needs to be flipped every next iteration
  // reference : https://en.wikipedia.org/wiki/Box%E2%80%93Muller_transform

  const float u1 = fmaxf(xoshiro128plus(state), FLT_MIN);
  const float u2 = xoshiro128plus(state);
  const float noise = (flip) ? sqrtf(-2.0f * logf(u1)) * cosf(2.f * M_PI * u2) :
                               sqrtf(-2.0f * logf(u1)) * sinf(2.f * M_PI * u2);
  return noise * sigma + mu;
}


#ifdef _OPENMP
#pragma omp declare simd uniform(sigma) aligned(state:64)
#endif
static inline float poisson_noise(const float mu, const float sigma, const int flip, uint32_t state[4])
{
  // create poisson noise - It's just gaussian noise with Anscombe transform applied
  const float u1 = fmaxf(xoshiro128plus(state), FLT_MIN);
  const float u2 = xoshiro128plus(state);
  const float noise = (flip) ? sqrtf(-2.0f * logf(u1)) * cosf(2.f * M_PI * u2) :
                               sqrtf(-2.0f * logf(u1)) * sinf(2.f * M_PI * u2);
  const float r = noise * sigma + 2.0f * sqrtf(fmaxf(mu + 3.f / 8.f, 0.0f));
  return (r * r - sigma * sigma) / 4.f - 3.f / 8.f;
}


#ifdef _OPENMP
#pragma omp declare simd uniform(distribution, param) aligned(state:64)
#endif
static inline float dt_noise_generator(const dt_noise_distribution_t distribution,
                                       const float mu, const float param, const int flip, uint32_t state[4])
{
  // scalar version

  switch(distribution)
  {
    case(DT_NOISE_UNIFORM):
    default:
      return uniform_noise(mu, param, state);

    case(DT_NOISE_GAUSSIAN):
      return gaussian_noise(mu, param, flip, state);

    case(DT_NOISE_POISSONIAN):
      return poisson_noise(mu, param, flip, state);
  }
}

#ifdef _OPENMP
#pragma omp declare simd uniform(sigma) aligned(state:64) aligned(mu, sigma, out:16)
#endif
static inline void uniform_noise_simd(const dt_aligned_pixel_t mu, const dt_aligned_pixel_t sigma,
                                      uint32_t state[4], dt_aligned_pixel_t out)
{
  const dt_aligned_pixel_t noise = { xoshiro128plus(state), xoshiro128plus(state), xoshiro128plus(state) };

  for_each_channel(c)
    out[c] = mu[c] + 2.0f * (noise[c] - 0.5f) * sigma[c];
}


#ifdef _OPENMP
#pragma omp declare simd uniform(sigma) aligned(state:64) aligned(mu, sigma, flip, out:16)
#endif
static inline void gaussian_noise_simd(const dt_aligned_pixel_t mu, const dt_aligned_pixel_t sigma,
                                       const int flip[4], uint32_t state[4], dt_aligned_pixel_t out)
{
  // Create gaussian noise centered in mu of standard deviation sigma
  // state should be initialized with xoshiro256_init() before calling and private in thread
  // flip needs to be flipped every next iteration
  // reference : https://en.wikipedia.org/wiki/Box%E2%80%93Muller_transform

  dt_aligned_pixel_t u1 = { 0.f };
  dt_aligned_pixel_t u2 = { 0.f };

  #pragma unroll
  for(size_t c = 0; c < 3; c++)
    u1[c] = fmaxf(xoshiro128plus(state), FLT_MIN);

  #pragma unroll
  for(size_t c = 0; c < 3; c++)
    u2[c] = xoshiro128plus(state);

  dt_aligned_pixel_t noise = { 0.f };

  for_each_channel(c)
  {
    noise[c] = (flip[c]) ? sqrtf(-2.0f * logf(u1[c])) * cosf(2.f * M_PI * u2[c]) :
                           sqrtf(-2.0f * logf(u1[c])) * sinf(2.f * M_PI * u2[c]);
  }

  for_each_channel(c)
    out[c] = noise[c] * sigma[c] + mu[c];
}


#ifdef _OPENMP
#pragma omp declare simd uniform(sigma) aligned(state:64) aligned(mu, sigma, flip, out:16)
#endif
static inline void poisson_noise_simd(const dt_aligned_pixel_t mu, const dt_aligned_pixel_t sigma, const int flip[4],
                                      uint32_t state[4], dt_aligned_pixel_t out)
{
  // create poissonian noise - It's just gaussian noise with Anscombe transform applied
  dt_aligned_pixel_t u1 = { 0.f };
  dt_aligned_pixel_t u2 = { 0.f };

  #pragma unroll
  for(size_t c = 0; c < 3; c++)
  {
    u1[c] = fmaxf(xoshiro128plus(state), FLT_MIN);
    u2[c] = xoshiro128plus(state);
  }

  dt_aligned_pixel_t noise = { 0.f };

  for_each_channel(c)
  {
    noise[c] = (flip[c]) ? sqrtf(-2.0f * logf(u1[c])) * cosf(2.f * M_PI * u2[c]) :
                           sqrtf(-2.0f * logf(u1[c])) * sinf(2.f * M_PI * u2[c]);
  }

  // now we have gaussian noise, then apply Anscombe transform to get poissonian one
  dt_aligned_pixel_t r = { 0.f };

  #pragma unroll
  for_each_channel(c)
  {
    r[c] = noise[c] * sigma[c] + 2.0f * sqrtf(fmaxf(mu[c] + 3.f / 8.f, 0.0f));
    out[c] = (r[c] * r[c] - sigma[c] * sigma[c]) / 4.f - 3.f / 8.f;
  }
}


#ifdef _OPENMP
#pragma omp declare simd uniform(distribution, param) aligned(state:64) aligned(mu, param, flip, out:16)
#endif
static inline void dt_noise_generator_simd(const dt_noise_distribution_t distribution,
                                           const dt_aligned_pixel_t mu, const dt_aligned_pixel_t param,
                                           const int flip[4], uint32_t state[4], dt_aligned_pixel_t out)
{
  // vector version

  switch(distribution)
  {
    case(DT_NOISE_UNIFORM):
    default:
    {
      uniform_noise_simd(mu, param, state, out);
      break;
    }

    case(DT_NOISE_GAUSSIAN):
    {
      gaussian_noise_simd(mu, param, flip, state, out);
      break;
    }

    case(DT_NOISE_POISSONIAN):
    {
      poisson_noise_simd(mu, param, flip, state, out);
      break;
    }
  }
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
