/*
   This file is part of darktable,
   Copyright (C) 2020 - Aurélien Pierre.

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
  DT_NOISE_UNIFORM = 0,   // $DESCRIPTION: "uniform"
  DT_NOISE_GAUSSIAN = 1,  // $DESCRIPTION: "gaussian"
  DT_NOISE_POISSONIAN = 2 // $DESCRIPTION: "poissonian"
} dt_noise_distribution_t;


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline uint64_t splitmix64(const uint64_t seed)
{
  // fast random number generator
  // reference : http://prng.di.unimi.it/splitmix64.c
  uint64_t result = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9;
  result = (result ^ (result >> 27)) * 0x94D049BB133111EB;
  return result ^ (result >> 31);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(state:64)
#endif
static inline void xoshiro256_init(uint64_t seed, uint64_t state[4])
{
  // Init the xoshiro256 random generator
  uint64_t tmp = splitmix64(seed);
  state[0] = (uint32_t)tmp;
  state[1] = (uint32_t)(tmp >> 32);

  tmp = splitmix64(seed + 0x9E3779B97f4A7C15);
  state[2] = (uint32_t)tmp;
  state[3] = (uint32_t)(tmp >> 32);
}


#ifdef _OPENMP
#pragma omp declare simd uniform(k)
#endif
static inline uint64_t rol64(const uint64_t x, const uint64_t k)
{
  return (x << k) | (x >> (64 - k));
}


#ifdef _OPENMP
#pragma omp declare simd aligned(state:64)
#endif
static inline float xoshiro256ss(uint64_t state[4])
{
  // fast random number generator
  // reference : http://prng.di.unimi.it/
  const uint64_t result = rol64(state[1] * 5, 7) * 9;
  const uint64_t t = state[1] << 17;

  state[2] ^= state[0];
  state[3] ^= state[1];
  state[1] ^= state[2];
  state[0] ^= state[3];

  state[2] ^= t;
  state[3] = rol64(state[3], 45);

  return (float)result / (float)UINT64_MAX;
}


#ifdef _OPENMP
#pragma omp declare simd uniform(sigma) aligned(state:64)
#endif
static inline float uniform_noise(const float mu, const float sigma, uint64_t state[4])
{
  return mu + 2.0f * (xoshiro256ss(state) - 0.5f) * sigma;
}


#ifdef _OPENMP
#pragma omp declare simd uniform(sigma) aligned(state:64)
#endif
static inline float gaussian_noise(const float mu, const float sigma, const int flip, uint64_t state[4])
{
  // Create gaussian noise centered in mu of standard deviation sigma
  // state should be initialized with xoshiro256_init() before calling and private in thread
  // flip needs to be flipped every next iteration
  // reference : https://en.wikipedia.org/wiki/Box%E2%80%93Muller_transform

  const float u1 = fmaxf(xoshiro256ss(state), FLT_MIN);
  const float u2 = xoshiro256ss(state);
  const float noise = (flip) ? sqrtf(-2.0f * logf(u1)) * cosf(2.f * M_PI * u2) :
                               sqrtf(-2.0f * logf(u1)) * sinf(2.f * M_PI * u2);
  return noise * sigma + mu;
}


#ifdef _OPENMP
#pragma omp declare simd uniform(sigma) aligned(state:64)
#endif
static inline float poisson_noise(const float mu, const float sigma, const int flip, uint64_t state[4])
{
  // create poisson noise - It's just gaussian noise with Anscombe transform applied
  const float u1 = fmaxf(xoshiro256ss(state), FLT_MIN);
  const float u2 = xoshiro256ss(state);
  const float noise = (flip) ? sqrtf(-2.0f * logf(u1)) * cosf(2.f * M_PI * u2) :
                               sqrtf(-2.0f * logf(u1)) * sinf(2.f * M_PI * u2);
  const float r = noise * sigma + 2.0f * sqrtf(fmaxf(mu + 3.f / 8.f, 0.0f));
  return (r * r - sigma * sigma) / 4.f - 3.f / 8.f;
}


#ifdef _OPENMP
#pragma omp declare simd uniform(distribution, param) aligned(state:64)
#endif
static inline float dt_noise_generator(const dt_noise_distribution_t distribution,
                                       const float mu, const float param, const int flip, uint64_t state[4])
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
static inline void uniform_noise_simd(const float mu[3], const float sigma[3], uint64_t state[4], float out[3])
{
  const float DT_ALIGNED_ARRAY noise[3] = { xoshiro256ss(state), xoshiro256ss(state), xoshiro256ss(state) };

  #pragma unroll
  for(size_t c = 0; c < 3; c++)
    out[c] = mu[c] + 2.0f * (noise[c] - 0.5f) * sigma[c];
}


#ifdef _OPENMP
#pragma omp declare simd uniform(sigma) aligned(state:64) aligned(mu, sigma, flip, out:16)
#endif
static inline void gaussian_noise_simd(const float mu[3], const float sigma[3], const int flip[3], uint64_t state[4], float out[3])
{
  // Create gaussian noise centered in mu of standard deviation sigma
  // state should be initialized with xoshiro256_init() before calling and private in thread
  // flip needs to be flipped every next iteration
  // reference : https://en.wikipedia.org/wiki/Box%E2%80%93Muller_transform

  float DT_ALIGNED_ARRAY u1[3] = { 0.f };
  float DT_ALIGNED_ARRAY u2[3] = { 0.f };

  #pragma unroll
  for(size_t c = 0; c < 3; c++)
  {
    u1[c] = fmaxf(xoshiro256ss(state), FLT_MIN);
    u2[c] = xoshiro256ss(state);
  }

  float DT_ALIGNED_ARRAY noise[3] = { 0.f };

  #pragma unroll
  for(size_t c = 0; c < 3; c++)
  {
    noise[c] = (flip[c]) ? sqrtf(-2.0f * logf(u1[c])) * cosf(2.f * M_PI * u2[c]) :
                           sqrtf(-2.0f * logf(u1[c])) * sinf(2.f * M_PI * u2[c]);
  }

  #pragma unroll
  for(size_t c = 0; c < 3; c++) out[c] = noise[c] * sigma[c] + mu[c];
}


#ifdef _OPENMP
#pragma omp declare simd uniform(sigma) aligned(state:64) aligned(mu, sigma, flip, out:16)
#endif
static inline void poisson_noise_simd(const float mu[3], const float sigma[3], const int flip[3], uint64_t state[4], float out[3])
{
  // create poisson noise - It's just gaussian noise with Anscombe transform applied
  float DT_ALIGNED_ARRAY u1[3] = { 0.f };
  float DT_ALIGNED_ARRAY u2[3] = { 0.f };

  #pragma unroll
  for(size_t c = 0; c < 3; c++)
  {
    u1[c] = fmaxf(xoshiro256ss(state), FLT_MIN);
    u2[c] = xoshiro256ss(state);
  }

  float DT_ALIGNED_ARRAY noise[3] = { 0.f };

  #pragma unroll
  for(size_t c = 0; c < 3; c++)
  {
    noise[c] = (flip[c]) ? sqrtf(-2.0f * logf(u1[c])) * cosf(2.f * M_PI * u2[c]) :
                           sqrtf(-2.0f * logf(u1[c])) * sinf(2.f * M_PI * u2[c]);
  }

  float DT_ALIGNED_ARRAY r[3] = { 0.f };

  #pragma unroll
  for(size_t c = 0; c < 3; c++)
  {
    r[c] = noise[c] * sigma[c] + 2.0f * sqrtf(fmaxf(mu[c] + 3.f / 8.f, 0.0f));
    out[c] = (r[c] * r[c] - sigma[c] * sigma[c]) / 4.f - 3.f / 8.f;
  }
}


#ifdef _OPENMP
#pragma omp declare simd uniform(distribution, param) aligned(state:64) aligned(mu, param, flip, out:16)
#endif
static inline void dt_noise_generator_simd(const dt_noise_distribution_t distribution,
                                           const float mu[3], const float param[3], const int flip[3],
                                           uint64_t state[4], float out[3])
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
