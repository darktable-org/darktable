/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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

#pragma once

// Allocate a buffer for storing the internal state of 'numthreads' parallel instances of the Tiny Encryption
// Algorithm.  We need to ensure that each state falls in a separate cache line, or all threads sharing a
// cache line will be running in lock-step as the cache line bounces back and forth between them, effectively
// cutting throughput by a factor equal to the number of threads sharing a cache line (8 with a 64-byte cache
// line and 32-bit ints)
#define TEA_STATE_SIZE (MAX(64, 2*sizeof(unsigned int)))
static inline unsigned int* alloc_tea_states(size_t numthreads)
{
  unsigned int* states = dt_alloc_align(64, numthreads * TEA_STATE_SIZE);
  if (states) memset(states, 0, numthreads * TEA_STATE_SIZE);
  return states;
}

// retrieve the state for the instance in the given thread from the array of states previously allocated with
// alloc_tea_states()
static inline unsigned int* get_tea_state(unsigned int* const states, int threadnum)
{
  return states + threadnum * TEA_STATE_SIZE/sizeof(states[0]);
}

static inline void free_tea_states(unsigned int* states)
{
  dt_free_align(states);
}

// How many rounds of the mixing function to run for one encryption
#define TEA_ROUNDS 8

// Run the encryption mixing function using and updating the given internal state.  For use as a PRNG, you can
// set arg[0] to the random-number seed, then read out the value of arg[0] after each call to this function.
static inline void encrypt_tea(unsigned int *arg)
{
  const unsigned int key[] = { 0xa341316c, 0xc8013ea4, 0xad90777d, 0x7e95761e };
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

static inline float tpdf(unsigned int urandom)
{
  float frandom = (float)urandom / (float)0xFFFFFFFFu;

  return (frandom < 0.5f ? (sqrtf(2.0f * frandom) - 1.0f) : (1.0f - sqrtf(2.0f * (1.0f - frandom))));
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

