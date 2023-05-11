/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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

#if !defined _XOPEN_SOURCE && !defined(__DragonFly__) && !defined(__FreeBSD__) && !defined(__NetBSD__)       \
    && !defined(__OpenBSD__) && !defined(_WIN32)
#define _XOPEN_SOURCE
#endif

#include <stdlib.h>

// xorshift128+, period 2^128-1, apparently passes all TestU01 suite tests.
typedef struct dt_points_state_t
{
  uint64_t state0;
  uint64_t state1;
  char pad[64];  // ensure that each instance is in a different cache line
} dt_points_state_t;

typedef struct dt_points_t
{
  dt_points_state_t *s;
} dt_points_t;

static inline void dt_points_init(dt_points_t *p, const unsigned int num_threads)
{
  p->s = (dt_points_state_t *)malloc(sizeof(dt_points_state_t) * num_threads);
  for(int k = 0; k < num_threads; k++)
  {
    p->s[k].state0 = 1 + k;
    p->s[k].state1 = 2 + k;
  }
}

static inline void dt_points_cleanup(dt_points_t *p)
{
  free(p->s);
}

static inline float dt_points_get_for(dt_points_t *p, const unsigned int thread_num)
{
  uint64_t s1 = p->s[thread_num].state0;
  uint64_t s0 = p->s[thread_num].state1;
  p->s[thread_num].state0 = s0;
  s1 ^= s1 << 23;
  s1 ^= s1 >> 17;
  s1 ^= s0;
  s1 ^= s0 >> 26;
  p->s[thread_num].state1 = s1;
  // return (state0 + state1) / ((double)((uint64_t)-1) + 1.0);
  union {
      float f;
      uint32_t u;
  } v;
  v.u = 0x3f800000 |
      ((p->s[thread_num].state0 + p->s[thread_num].state1) >> 41); // faster than double version.
  return v.f - 1.0f;
}

static inline float dt_points_get()
{
  return dt_points_get_for(darktable.points, dt_get_thread_num());
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

