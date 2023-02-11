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

// This is the OpenCL translation of common/noise_generator.h


typedef enum dt_noise_distribution_t
{
  DT_NOISE_UNIFORM = 0,   // $DESCRIPTION: "Uniform"
  DT_NOISE_GAUSSIAN = 1,  // $DESCRIPTION: "Gaussian"
  DT_NOISE_POISSONIAN = 2 // $DESCRIPTION: "Poissonian"
} dt_noise_distribution_t;


static inline unsigned int splitmix32(const unsigned long seed)
{
  // fast random number generator
  // reference : https://gist.github.com/imneme/6179748664e88ef3c34860f44309fc71
  unsigned long result = (seed ^ (seed >> 33)) * 0x62a9d9ed799705f5ul;
  result = (result ^ (result >> 28)) * 0xcb24d0a5c88c35b3ul;
  return (unsigned int)(result >> 32);
}



static inline unsigned rol32(const unsigned int x, const int k)
{
  return (x << k) | (x >> (32 - k));
}


static inline float xoshiro128plus(uint state[4])
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


static inline float4 uniform_noise_simd(const float4 mu, const float4 sigma, uint state[4])
{
  const float4 noise = { xoshiro128plus(state), xoshiro128plus(state), xoshiro128plus(state), 0.f };
  return mu + 2.0f * (noise - 0.5f) * sigma;
}


static inline float4 gaussian_noise_simd(const float4 mu, const float4 sigma, uint state[4])
{
  // Create gaussian noise centered in mu of standard deviation sigma
  // state should be initialized with xoshiro256_init() before calling and private in thread
  // flip needs to be flipped every next iteration
  // reference : https://en.wikipedia.org/wiki/Box%E2%80%93Muller_transform

  float4 u1, u2;

  u1.x = xoshiro128plus(state);
  u1.y = xoshiro128plus(state);
  u1.z = xoshiro128plus(state);

  u2.x = xoshiro128plus(state);
  u2.y = xoshiro128plus(state);
  u2.z = xoshiro128plus(state);

  u1 = fmax(u1, FLT_MIN);

  const float4 flip = { 1.0f, 0.0f, 1.0f, 0.0f };
  const float4 flip_comp = { 0.0f, 1.0f, 0.0f, 0.0f };

  // flip is a 4×1 boolean mask
  const float4 noise = flip * native_sqrt(-2.0f * native_log(u1)) * native_cos(2.f * M_PI_F * u2) +
                       flip_comp * native_sqrt(-2.0f * native_log(u1)) * native_sin(2.f * M_PI_F * u2);
  return noise * sigma + mu;
}


static inline float4 poisson_noise_simd(const float4 mu, const float4 sigma, uint state[4])
{
  // create poissonian noise - It's just gaussian noise with Anscombe transform applied
  float4 u1, u2;

  // we need to generate the random numbers in this order to match CPU path.
  u1.x = xoshiro128plus(state);
  u2.x = xoshiro128plus(state);
  u1.y = xoshiro128plus(state);
  u2.y = xoshiro128plus(state);
  u1.z = xoshiro128plus(state);
  u2.z = xoshiro128plus(state);

  u1 = fmax(u1, (float4)FLT_MIN);

  const float4 flip = { 1.0f, 0.0f, 1.0f, 0.0f };
  const float4 flip_comp = { 0.0f, 1.0f, 0.0f, 0.0f };

  // flip is a 4×1 boolean mask
  const float4 noise = flip * native_sqrt(-2.0f * native_log(u1)) * native_cos(2.f * M_PI_F * u2) +
                       flip_comp * native_sqrt(-2.0f * native_log(u1)) * native_sin(2.f * M_PI_F * u2);

  // now we have gaussian noise, then apply Anscombe transform to get poissonian one
  const float4 r = noise * sigma + 2.0f * native_sqrt(fmax(mu + (3.f / 8.f), 0.0f));
  return ((r * r - sigma * sigma) / (4.f)) - (3.f / 8.f);
}


static inline float4 dt_noise_generator_simd(const dt_noise_distribution_t distribution,
                                             const float4 mu, const float4 param,
                                             uint state[4])
{
  // vector version

  switch(distribution)
  {
    case(DT_NOISE_UNIFORM):
    default:
    {
      return uniform_noise_simd(mu, param, state);
    }

    case(DT_NOISE_GAUSSIAN):
    {
      return gaussian_noise_simd(mu, param, state);
    }

    case(DT_NOISE_POISSONIAN):
    {
      return poisson_noise_simd(mu, param, state);
    }
  }
}
