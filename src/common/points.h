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

#ifndef __SSE2__

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

#else

#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <inttypes.h>

#define MEXP 19937

#ifndef SFMT_PARAMS_H
#define SFMT_PARAMS_H

#if !defined(MEXP)
#ifdef __GNUC__
#warning "MEXP is not defined. I assume MEXP is 19937."
#endif
#define MEXP 19937
#endif
/*-----------------
  BASIC DEFINITIONS
  -----------------*/
/** Mersenne Exponent. The period of the sequence
 *  is a multiple of 2^MEXP-1.
 * #define MEXP 19937 */
/** SFMT generator has an internal state array of 128-bit integers,
 * and N is its size. */
#define N (MEXP / 128 + 1)
/** N32 is the size of internal state array when regarded as an array
 * of 32-bit integers.*/
#define N32 (N * 4)
/** N64 is the size of internal state array when regarded as an array
 * of 64-bit integers.*/
#define N64 (N * 2)

/*----------------------
  the parameters of SFMT
  following definitions are in paramsXXXX.h file.
  ----------------------*/
/** the pick up position of the array.
#define POS1 122
*/

/** the parameter of shift left as four 32-bit registers.
#define SL1 18
 */

/** the parameter of shift left as one 128-bit register.
 * The 128-bit integer is shifted by (SL2 * 8) bits.
#define SL2 1
*/

/** the parameter of shift right as four 32-bit registers.
#define SR1 11
*/

/** the parameter of shift right as one 128-bit register.
 * The 128-bit integer is shifted by (SL2 * 8) bits.
#define SR2 1
*/

/** A bitmask, used in the recursion.  These parameters are introduced
 * to break symmetry of SIMD.
#define MSK1 0xdfffffefU
#define MSK2 0xddfecb7fU
#define MSK3 0xbffaffffU
#define MSK4 0xbffffff6U
*/

/** These definitions are part of a 128-bit period certification vector.
#define PARITY1 0x00000001U
#define PARITY2 0x00000000U
#define PARITY3 0x00000000U
#define PARITY4 0xc98e126aU
*/

#if 0
#if MEXP == 607
#include "SFMT-params607.h"
#elif MEXP == 1279
#include "SFMT-params1279.h"
#elif MEXP == 2281
#include "SFMT-params2281.h"
#elif MEXP == 4253
#include "SFMT-params4253.h"
#elif MEXP == 11213
#include "SFMT-params11213.h"
#elif MEXP == 19937
#include "SFMT-params19937.h"
#elif MEXP == 44497
#include "SFMT-params44497.h"
#elif MEXP == 86243
#include "SFMT-params86243.h"
#elif MEXP == 132049
#include "SFMT-params132049.h"
#elif MEXP == 216091
#include "SFMT-params216091.h"
#else
#ifdef __GNUC__
#error "MEXP is not valid."
#undef MEXP
#else
#undef MEXP
#endif
#endif

#endif

#endif /* SFMT_PARAMS_H */

#ifndef SFMT_PARAMS19937_H
#define SFMT_PARAMS19937_H

#define POS1 122
#define SL1 18
#define SL2 1
#define SR1 11
#define SR2 1
#define MSK1 0xdfffffefU
#define MSK2 0xddfecb7fU
#define MSK3 0xbffaffffU
#define MSK4 0xbffffff6U
#define PARITY1 0x00000001U
#define PARITY2 0x00000000U
#define PARITY3 0x00000000U
#define PARITY4 0x13c9e684U


#define ALTI_SL1                                                                                             \
  {                                                                                                          \
    SL1, SL1, SL1, SL1                                                                                       \
  }
#define ALTI_SR1                                                                                             \
  {                                                                                                          \
    SR1, SR1, SR1, SR1                                                                                       \
  }
#define ALTI_MSK                                                                                             \
  {                                                                                                          \
    MSK1, MSK2, MSK3, MSK4                                                                                   \
  }
#define ALTI_MSK64                                                                                           \
  {                                                                                                          \
    MSK2, MSK1, MSK4, MSK3                                                                                   \
  }
#define ALTI_SL2_PERM                                                                                        \
  {                                                                                                          \
    1, 2, 3, 23, 5, 6, 7, 0, 9, 10, 11, 4, 13, 14, 15, 8                                                     \
  }
#define ALTI_SL2_PERM64                                                                                      \
  {                                                                                                          \
    1, 2, 3, 4, 5, 6, 7, 31, 9, 10, 11, 12, 13, 14, 15, 0                                                    \
  }
#define ALTI_SR2_PERM                                                                                        \
  {                                                                                                          \
    7, 0, 1, 2, 11, 4, 5, 6, 15, 8, 9, 10, 17, 12, 13, 14                                                    \
  }
#define ALTI_SR2_PERM64                                                                                      \
  {                                                                                                          \
    15, 0, 1, 2, 3, 4, 5, 6, 17, 8, 9, 10, 11, 12, 13, 14                                                    \
  }
#define IDSTR "SFMT-19937:122-18-1-11-1:dfffffef-ddfecb7f-bffaffff-bffffff6"

#endif /* SFMT_PARAMS19937_H */

/** 128-bit data structure */
typedef union w128_t
{
  __m128i si;
  uint32_t u[4];
} w128_t;

typedef struct sfmt_state_t
{
  /** the 128-bit internal state array */
  w128_t sfmt[N];
  /** the 32bit integer pointer to the 128-bit internal state array */
  uint32_t *psfmt32;
#if !defined(BIG_ENDIAN64) || defined(ONLY64)
  /** the 64bit integer pointer to the 128-bit internal state array */
  uint64_t *psfmt64;
#endif
  /** index counter to the 32-bit internal state array */
  int idx;
  /** a flag: it is 0 if and only if the internal state is not yet
   * initialized. */
  int initialized;
  /** a parity check vector which certificate the period of 2^{MEXP} */
  uint32_t parity[4];
} sfmt_state_t;

/**
 * @file SFMT.h
 *
 * @brief SIMD oriented Fast Mersenne Twister(SFMT) pseudorandom
 * number generator
 *
 * @author Mutsuo Saito (Hiroshima University)
 * @author Makoto Matsumoto (Hiroshima University)
 *
 * Copyright (C) 2006, 2007 Mutsuo Saito, Makoto Matsumoto and Hiroshima
 * University. All rights reserved.
 *
 * The new BSD License is applied to this software.
 * see LICENSE.txt
 *
 * @note We assume that your system has inttypes.h.  If your system
 * doesn't have inttypes.h, you have to typedef uint32_t and uint64_t,
 * and you have to define PRIu64 and PRIx64 in this file as follows:
 * @verbatim
 typedef unsigned int uint32_t
 typedef unsigned long long uint64_t
#define PRIu64 "llu"
#define PRIx64 "llx"
@endverbatim
 * uint32_t must be exactly 32-bit unsigned integer type (no more, no
 * less), and uint64_t must be exactly 64-bit unsigned integer type.
 * PRIu64 and PRIx64 are used for printf function to print 64-bit
 * unsigned int and 64-bit unsigned int in hexadecimal format.
 */

#ifndef SFMT_H
#define SFMT_H

#include <stdio.h>

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
#include <inttypes.h>
#elif defined(_MSC_VER) || defined(__BORLANDC__)
typedef unsigned int uint32_t;
typedef unsigned __int64 uint64_t;
#define inline __inline
#else
#include <inttypes.h>
#if defined(__GNUC__)
#define inline __inline__
#endif
#endif

#ifndef PRIu64
#if defined(_MSC_VER) || defined(__BORLANDC__)
#define PRIu64 "I64u"
#define PRIx64 "I64x"
#else
#define PRIu64 "llu"
#define PRIx64 "llx"
#endif
#endif

#if defined(__GNUC__)
#define ALWAYSINLINE __attribute__((always_inline))
#else
#define ALWAYSINLINE
#endif

#if defined(_MSC_VER)
#if _MSC_VER >= 1200
#define PRE_ALWAYS __forceinline
#else
#define PRE_ALWAYS inline
#endif
#else
#define PRE_ALWAYS inline
#endif

static inline uint32_t gen_rand32(struct sfmt_state_t *s);
static inline uint64_t gen_rand64(struct sfmt_state_t *s);
static inline void fill_array32(struct sfmt_state_t *s, uint32_t *array, int size) __attribute__((unused));
static inline void fill_array64(struct sfmt_state_t *s, uint64_t *array, int size) __attribute__((unused));
static inline void init_gen_rand(struct sfmt_state_t *s, uint32_t seed) __attribute__((unused));
static inline void init_by_array(struct sfmt_state_t *s, uint32_t *init_key, int key_length)
    __attribute__((unused));
static inline const char *get_idstring(void) __attribute__((unused));
static inline int get_min_array_size32(void) __attribute__((unused));
static inline int get_min_array_size64(void) __attribute__((unused));

/* These real versions are due to Isaku Wada */
/** generates a random number on [0,1]-real-interval */
inline static double to_real1(uint32_t v)
{
  return v * (1.0 / 4294967295.0);
  /* divided by 2^32-1 */
}

/** generates a random number on [0,1]-real-interval */
inline static double genrand_real1(struct sfmt_state_t *s)
{
  return to_real1(gen_rand32(s));
}

/** generates a random number on [0,1)-real-interval */
inline static double to_real2(uint32_t v)
{
  return v * (1.0 / 4294967296.0);
  /* divided by 2^32 */
}

/** generates a random number on [0,1)-real-interval (float) */
inline static float to_real2f(uint32_t v)
{
  union {
      float f;
      uint32_t u;
  } x;
  x.u = 0x3f800000 | (v >> 9); // faster than double version.
  return x.f - 1.0f;
  /* divided by 2^32 */
}

/** generates a random number on [0,1)-real-interval */
inline static double genrand_real2(struct sfmt_state_t *s)
{
  return to_real2(gen_rand32(s));
}

inline static float genrand_real2f(struct sfmt_state_t *s)
{
  return to_real2f(gen_rand32(s));
}

/** generates a random number on (0,1)-real-interval */
inline static double to_real3(uint32_t v)
{
  return (((double)v) + 0.5) * (1.0 / 4294967296.0);
  /* divided by 2^32 */
}

/** generates a random number on (0,1)-real-interval */
inline static double genrand_real3(struct sfmt_state_t *s)
{
  return to_real3(gen_rand32(s));
}
/** These real versions are due to Isaku Wada */

/** generates a random number on [0,1) with 53-bit resolution*/
inline static double to_res53(uint64_t v)
{
  return v * (1.0 / 18446744073709551616.0L);
}

/** generates a random number on [0,1) with 53-bit resolution from two
 * 32 bit integers */
inline static double to_res53_mix(uint32_t x, uint32_t y)
{
  return to_res53(x | ((uint64_t)y << 32));
}

/** generates a random number on [0,1) with 53-bit resolution
*/
inline static double genrand_res53(struct sfmt_state_t *s)
{
  return to_res53(gen_rand64(s));
}

/** generates a random number on [0,1) with 53-bit resolution
  using 32bit integer.
  */
inline static double genrand_res53_mix(struct sfmt_state_t *s)
{
  uint32_t x, y;

  x = gen_rand32(s);
  y = gen_rand32(s);
  return to_res53_mix(x, y);
}
#endif
/**
 * @file  SFMT-sse2.h
 * @brief SIMD oriented Fast Mersenne Twister(SFMT) for Intel SSE2
 *
 * @author Mutsuo Saito (Hiroshima University)
 * @author Makoto Matsumoto (Hiroshima University)
 *
 * @note We assume LITTLE ENDIAN in this file
 *
 * Copyright (C) 2006, 2007 Mutsuo Saito, Makoto Matsumoto and Hiroshima
 * University. All rights reserved.
 *
 * The new BSD License is applied to this software, see LICENSE.txt
 */

#ifndef SFMT_SSE2_H
#define SFMT_SSE2_H

PRE_ALWAYS static __m128i mm_recursion(__m128i *a, __m128i *b, __m128i c, __m128i d,
                                       __m128i mask) ALWAYSINLINE;

/**
 * This function represents the recursion formula.
 * @param a a 128-bit part of the internal state array
 * @param b a 128-bit part of the internal state array
 * @param c a 128-bit part of the internal state array
 * @param d a 128-bit part of the internal state array
 * @param mask 128-bit mask
 * @return output
 */
PRE_ALWAYS static __m128i mm_recursion(__m128i *a, __m128i *b, __m128i c, __m128i d, __m128i mask)
{
  __m128i v, x, y, z;

  x = _mm_load_si128(a);
  y = _mm_srli_epi32(*b, SR1);
  z = _mm_srli_si128(c, SR2);
  v = _mm_slli_epi32(d, SL1);
  z = _mm_xor_si128(z, x);
  z = _mm_xor_si128(z, v);
  x = _mm_slli_si128(x, SL2);
  y = _mm_and_si128(y, mask);
  z = _mm_xor_si128(z, x);
  z = _mm_xor_si128(z, y);
  return z;
}

/**
 * This function fills the internal state array with pseudorandom
 * integers.
 */
inline static void gen_rand_all(struct sfmt_state_t *s)
{
  int i;
  __m128i r, r1, r2, mask;
  mask = _mm_set_epi32(MSK4, MSK3, MSK2, MSK1);

  r1 = _mm_load_si128(&(s->sfmt[N - 2].si));
  r2 = _mm_load_si128(&(s->sfmt[N - 1].si));
  for(i = 0; i < N - POS1; i++)
  {
    r = mm_recursion(&(s->sfmt[i].si), &(s->sfmt[i + POS1].si), r1, r2, mask);
    _mm_store_si128(&(s->sfmt[i].si), r);
    r1 = r2;
    r2 = r;
  }
  for(; i < N; i++)
  {
    r = mm_recursion(&(s->sfmt[i].si), &(s->sfmt[i + POS1 - N].si), r1, r2, mask);
    _mm_store_si128(&(s->sfmt[i].si), r);
    r1 = r2;
    r2 = r;
  }
}

/**
 * This function fills the user-specified array with pseudorandom
 * integers.
 *
 * @param array an 128-bit array to be filled by pseudorandom numbers.
 * @param size number of 128-bit pesudorandom numbers to be generated.
 */
inline static void gen_rand_array(struct sfmt_state_t *s, w128_t *array, int size)
{
  int i, j;
  __m128i r, r1, r2, mask;
  mask = _mm_set_epi32(MSK4, MSK3, MSK2, MSK1);

  r1 = _mm_load_si128(&(s->sfmt[N - 2].si));
  r2 = _mm_load_si128(&(s->sfmt[N - 1].si));
  for(i = 0; i < N - POS1; i++)
  {
    r = mm_recursion(&(s->sfmt[i].si), &(s->sfmt[i + POS1].si), r1, r2, mask);
    _mm_store_si128(&array[i].si, r);
    r1 = r2;
    r2 = r;
  }
  for(; i < N; i++)
  {
    r = mm_recursion(&(s->sfmt[i].si), &array[i + POS1 - N].si, r1, r2, mask);
    _mm_store_si128(&array[i].si, r);
    r1 = r2;
    r2 = r;
  }
  /* main loop */
  for(; i < size - N; i++)
  {
    r = mm_recursion(&array[i - N].si, &array[i + POS1 - N].si, r1, r2, mask);
    _mm_store_si128(&array[i].si, r);
    r1 = r2;
    r2 = r;
  }
  for(j = 0; j < 2 * N - size; j++)
  {
    r = _mm_load_si128(&array[j + size - N].si);
    _mm_store_si128(&(s->sfmt[j].si), r);
  }
  for(; i < size; i++)
  {
    r = mm_recursion(&array[i - N].si, &array[i + POS1 - N].si, r1, r2, mask);
    _mm_store_si128(&array[i].si, r);
    _mm_store_si128(&(s->sfmt[j++].si), r);
    r1 = r2;
    r2 = r;
  }
}

#endif
/**
 * @file  SFMT.c
 * @brief SIMD oriented Fast Mersenne Twister(SFMT)
 *
 * @author Mutsuo Saito (Hiroshima University)
 * @author Makoto Matsumoto (Hiroshima University)
 *
 * Copyright (C) 2006,2007 Mutsuo Saito, Makoto Matsumoto and Hiroshima
 * University. All rights reserved.
 *
 * The new BSD License is applied to this software, see LICENSE.txt
 */
#include <assert.h>
#include <string.h>
//#include "SFMT.h"
//#include "SFMT-params.h"

#if defined(__BIG_ENDIAN__) && !defined(__amd64) && !defined(BIG_ENDIAN64)
#define BIG_ENDIAN64 1
#endif
#if defined(HAVE_ALTIVEC) && !defined(BIG_ENDIAN64)
#define BIG_ENDIAN64 1
#endif
#if defined(ONLY64) && !defined(BIG_ENDIAN64)
#if defined(__GNUC__)
#error "-DONLY64 must be specified with -DBIG_ENDIAN64"
#endif
#undef ONLY64
#endif


typedef struct dt_points_t
{
  sfmt_state_t **s;
  unsigned int num;
} dt_points_t;

#if 0
/*--------------------------------------
  FILE GLOBAL VARIABLES
  internal state, index counter and flag
  --------------------------------------*/
/** the 128-bit internal state array */
static w128_t sfmt[N];
/** the 32bit integer pointer to the 128-bit internal state array */
static uint32_t *psfmt32 = &sfmt[0].u[0];
#if !defined(BIG_ENDIAN64) || defined(ONLY64)
/** the 64bit integer pointer to the 128-bit internal state array */
static uint64_t *psfmt64 = (uint64_t *)&sfmt[0].u[0];
#endif
/** index counter to the 32-bit internal state array */
static int idx;
/** a flag: it is 0 if and only if the internal state is not yet
 * initialized. */
static int initialized = 0;
/** a parity check vector which certificate the period of 2^{MEXP} */
static uint32_t parity[4] = {PARITY1, PARITY2, PARITY3, PARITY4};
#endif

/*----------------
  STATIC FUNCTIONS
  ----------------*/
inline static int idxof(int i);
inline static void rshift128(w128_t *out, w128_t const *in, int shift);
inline static void lshift128(w128_t *out, w128_t const *in, int shift);
inline static void gen_rand_all(sfmt_state_t *s);
inline static void gen_rand_array(sfmt_state_t *s, w128_t *array, int size);
inline static uint32_t func1(uint32_t x);
inline static uint32_t func2(uint32_t x);
static void period_certification(sfmt_state_t *s);
#if defined(BIG_ENDIAN64) && !defined(ONLY64)
inline static void swap(w128_t *array, int size);
#endif

/*#if defined(HAVE_ALTIVEC)
#include "SFMT-alti.h"
#elif defined(HAVE_SSE2)
#include "SFMT-sse2.h"
#endif*/

/**
 * This function simulate a 64-bit index of LITTLE ENDIAN
 * in BIG ENDIAN machine.
 */
#ifdef ONLY64
inline static int idxof(int i)
{
  return i ^ 1;
}
#else
inline static int idxof(int i)
{
  return i;
}
#endif
/**
 * This function simulates SIMD 128-bit right shift by the standard C.
 * The 128-bit integer given in in is shifted by (shift * 8) bits.
 * This function simulates the LITTLE ENDIAN SIMD.
 * @param out the output of this function
 * @param in the 128-bit data to be shifted
 * @param shift the shift value
 */
#ifdef ONLY64
inline static void rshift128(w128_t *out, w128_t const *in, int shift)
{
  uint64_t th, tl, oh, ol;

  th = ((uint64_t)in->u[2] << 32) | ((uint64_t)in->u[3]);
  tl = ((uint64_t)in->u[0] << 32) | ((uint64_t)in->u[1]);

  oh = th >> (shift * 8);
  ol = tl >> (shift * 8);
  ol |= th << (64 - shift * 8);
  out->u[0] = (uint32_t)(ol >> 32);
  out->u[1] = (uint32_t)ol;
  out->u[2] = (uint32_t)(oh >> 32);
  out->u[3] = (uint32_t)oh;
}
#else
inline static void rshift128(w128_t *out, w128_t const *in, int shift)
{
  uint64_t th, tl, oh, ol;

  th = ((uint64_t)in->u[3] << 32) | ((uint64_t)in->u[2]);
  tl = ((uint64_t)in->u[1] << 32) | ((uint64_t)in->u[0]);

  oh = th >> (shift * 8);
  ol = tl >> (shift * 8);
  ol |= th << (64 - shift * 8);
  out->u[1] = (uint32_t)(ol >> 32);
  out->u[0] = (uint32_t)ol;
  out->u[3] = (uint32_t)(oh >> 32);
  out->u[2] = (uint32_t)oh;
}
#endif
/**
 * This function simulates SIMD 128-bit left shift by the standard C.
 * The 128-bit integer given in in is shifted by (shift * 8) bits.
 * This function simulates the LITTLE ENDIAN SIMD.
 * @param out the output of this function
 * @param in the 128-bit data to be shifted
 * @param shift the shift value
 */
#ifdef ONLY64
inline static void lshift128(w128_t *out, w128_t const *in, int shift)
{
  uint64_t th, tl, oh, ol;

  th = ((uint64_t)in->u[2] << 32) | ((uint64_t)in->u[3]);
  tl = ((uint64_t)in->u[0] << 32) | ((uint64_t)in->u[1]);

  oh = th << (shift * 8);
  ol = tl << (shift * 8);
  oh |= tl >> (64 - shift * 8);
  out->u[0] = (uint32_t)(ol >> 32);
  out->u[1] = (uint32_t)ol;
  out->u[2] = (uint32_t)(oh >> 32);
  out->u[3] = (uint32_t)oh;
}
#else
inline static void lshift128(w128_t *out, w128_t const *in, int shift)
{
  uint64_t th, tl, oh, ol;

  th = ((uint64_t)in->u[3] << 32) | ((uint64_t)in->u[2]);
  tl = ((uint64_t)in->u[1] << 32) | ((uint64_t)in->u[0]);

  oh = th << (shift * 8);
  ol = tl << (shift * 8);
  oh |= tl >> (64 - shift * 8);
  out->u[1] = (uint32_t)(ol >> 32);
  out->u[0] = (uint32_t)ol;
  out->u[3] = (uint32_t)(oh >> 32);
  out->u[2] = (uint32_t)oh;
}
#endif

/**
 * This function represents the recursion formula.
 * @param r output
 * @param a a 128-bit part of the internal state array
 * @param b a 128-bit part of the internal state array
 * @param c a 128-bit part of the internal state array
 * @param d a 128-bit part of the internal state array
 */
#if(!defined(HAVE_ALTIVEC)) && (!defined(HAVE_SSE2))
#ifdef ONLY64
inline static void do_recursion(w128_t *r, w128_t *a, w128_t *b, w128_t *c, w128_t *d)
{
  w128_t x;
  w128_t y;

  lshift128(&x, a, SL2);
  rshift128(&y, c, SR2);
  r->u[0] = a->u[0] ^ x.u[0] ^ ((b->u[0] >> SR1) & MSK2) ^ y.u[0] ^ (d->u[0] << SL1);
  r->u[1] = a->u[1] ^ x.u[1] ^ ((b->u[1] >> SR1) & MSK1) ^ y.u[1] ^ (d->u[1] << SL1);
  r->u[2] = a->u[2] ^ x.u[2] ^ ((b->u[2] >> SR1) & MSK4) ^ y.u[2] ^ (d->u[2] << SL1);
  r->u[3] = a->u[3] ^ x.u[3] ^ ((b->u[3] >> SR1) & MSK3) ^ y.u[3] ^ (d->u[3] << SL1);
}
#else
inline static void do_recursion(w128_t *r, w128_t *a, w128_t *b, w128_t *c, w128_t *d)
{
  w128_t x;
  w128_t y;

  lshift128(&x, a, SL2);
  rshift128(&y, c, SR2);
  r->u[0] = a->u[0] ^ x.u[0] ^ ((b->u[0] >> SR1) & MSK1) ^ y.u[0] ^ (d->u[0] << SL1);
  r->u[1] = a->u[1] ^ x.u[1] ^ ((b->u[1] >> SR1) & MSK2) ^ y.u[1] ^ (d->u[1] << SL1);
  r->u[2] = a->u[2] ^ x.u[2] ^ ((b->u[2] >> SR1) & MSK3) ^ y.u[2] ^ (d->u[2] << SL1);
  r->u[3] = a->u[3] ^ x.u[3] ^ ((b->u[3] >> SR1) & MSK4) ^ y.u[3] ^ (d->u[3] << SL1);
}
#endif
#endif

#if defined(BIG_ENDIAN64) && !defined(ONLY64) && !defined(HAVE_ALTIVEC)
inline static void swap(w128_t *array, int size)
{
  for(int i = 0; i < size; i++)
  {
    uint32_t x = array[i].u[0];
    uint32_t y = array[i].u[2];
    array[i].u[0] = array[i].u[1];
    array[i].u[2] = array[i].u[3];
    array[i].u[1] = x;
    array[i].u[3] = y;
  }
}
#endif
/**
 * This function represents a function used in the initialization
 * by init_by_array
 * @param x 32-bit integer
 * @return 32-bit integer
 */
static uint32_t func1(uint32_t x)
{
  return (x ^ (x >> 27)) * (uint32_t)1664525UL;
}

/**
 * This function represents a function used in the initialization
 * by init_by_array
 * @param x 32-bit integer
 * @return 32-bit integer
 */
static uint32_t func2(uint32_t x)
{
  return (x ^ (x >> 27)) * (uint32_t)1566083941UL;
}

/**
 * This function certificate the period of 2^{MEXP}
 */
static void period_certification(sfmt_state_t *s)
{
  int inner = 0;

  for(int i = 0; i < 4; i++) inner ^= s->psfmt32[idxof(i)] & s->parity[i];
  for(int i = 16; i > 0; i >>= 1) inner ^= inner >> i;
  inner &= 1;
  /* check OK */
  if(inner == 1)
  {
    return;
  }
  /* check NG, and modification */
  for(int i = 0; i < 4; i++)
  {
    uint32_t work = 1;
    for(int j = 0; j < 32; j++)
    {
      if((work & s->parity[i]) != 0)
      {
        s->psfmt32[idxof(i)] ^= work;
        return;
      }
      work = work << 1;
    }
  }
}

/*----------------
  PUBLIC FUNCTIONS
  ----------------*/
/**
 * This function returns the identification string.
 * The string shows the word size, the Mersenne exponent,
 * and all parameters of this generator.
 */
const char *get_idstring(void)
{
  return IDSTR;
}

/**
 * This function returns the minimum size of array used for \b
 * fill_array32() function.
 * @return minimum size of array used for fill_array32() function.
 */
int get_min_array_size32(void)
{
  return N32;
}

/**
 * This function returns the minimum size of array used for \b
 * fill_array64() function.
 * @return minimum size of array used for fill_array64() function.
 */
int get_min_array_size64(void)
{
  return N64;
}

#ifndef ONLY64
/**
 * This function generates and returns 32-bit pseudorandom number.
 * init_gen_rand or init_by_array must be called before this function.
 * @return 32-bit pseudorandom number
 */
uint32_t gen_rand32(sfmt_state_t *s)
{
  uint32_t r;

  // assert(s->initialized);
  if(s->idx >= N32)
  {
    gen_rand_all(s);
    s->idx = 0;
  }
  r = s->psfmt32[s->idx++];
  return r;
}
#endif
/**
 * This function generates and returns 64-bit pseudorandom number.
 * init_gen_rand or init_by_array must be called before this function.
 * The function gen_rand64 should not be called after gen_rand32,
 * unless an initialization is again executed.
 * @return 64-bit pseudorandom number
 */
uint64_t gen_rand64(sfmt_state_t *s)
{
#if defined(BIG_ENDIAN64) && !defined(ONLY64)
  uint32_t r1, r2;
#else
  uint64_t r;
#endif

  // assert(s->initialized);
  // assert(s->idx % 2 == 0);

  if(s->idx >= N32)
  {
    gen_rand_all(s);
    s->idx = 0;
  }
#if defined(BIG_ENDIAN64) && !defined(ONLY64)
  r1 = s->psfmt32[s->idx];
  r2 = s->psfmt32[s->idx + 1];
  s->idx += 2;
  return ((uint64_t)r2 << 32) | r1;
#else
  r = s->psfmt64[s->idx / 2];
  s->idx += 2;
  return r;
#endif
}

#ifndef ONLY64
/**
 * This function generates pseudorandom 32-bit integers in the
 * specified array[] by one call. The number of pseudorandom integers
 * is specified by the argument size, which must be at least 624 and a
 * multiple of four.  The generation by this function is much faster
 * than the following gen_rand function.
 *
 * For initialization, init_gen_rand or init_by_array must be called
 * before the first call of this function. This function can not be
 * used after calling gen_rand function, without initialization.
 *
 * @param array an array where pseudorandom 32-bit integers are filled
 * by this function.  The pointer to the array must be \b "aligned"
 * (namely, must be a multiple of 16) in the SIMD version, since it
 * refers to the address of a 128-bit integer.  In the standard C
 * version, the pointer is arbitrary.
 *
 * @param size the number of 32-bit pseudorandom integers to be
 * generated.  size must be a multiple of 4, and greater than or equal
 * to (MEXP / 128 + 1) * 4.
 *
 * @note \b memalign or \b posix_memalign is available to get aligned
 * memory. Mac OSX doesn't have these functions, but \b malloc of OSX
 * returns the pointer to the aligned memory block.
 */
void fill_array32(sfmt_state_t *s, uint32_t *array, int size)
{
  // assert(s->initialized);
  // assert(s->idx == N32);
  // assert(size % 4 == 0);
  // assert(size >= N32);

  gen_rand_array(s, (w128_t *)array, size / 4);
  s->idx = N32;
}
#endif

/**
 * This function generates pseudorandom 64-bit integers in the
 * specified array[] by one call. The number of pseudorandom integers
 * is specified by the argument size, which must be at least 312 and a
 * multiple of two.  The generation by this function is much faster
 * than the following gen_rand function.
 *
 * For initialization, init_gen_rand or init_by_array must be called
 * before the first call of this function. This function can not be
 * used after calling gen_rand function, without initialization.
 *
 * @param array an array where pseudorandom 64-bit integers are filled
 * by this function.  The pointer to the array must be "aligned"
 * (namely, must be a multiple of 16) in the SIMD version, since it
 * refers to the address of a 128-bit integer.  In the standard C
 * version, the pointer is arbitrary.
 *
 * @param size the number of 64-bit pseudorandom integers to be
 * generated.  size must be a multiple of 2, and greater than or equal
 * to (MEXP / 128 + 1) * 2
 *
 * @note \b memalign or \b posix_memalign is available to get aligned
 * memory. Mac OSX doesn't have these functions, but \b malloc of OSX
 * returns the pointer to the aligned memory block.
 */
void fill_array64(sfmt_state_t *s, uint64_t *array, int size)
{
  // assert(s->initialized);
  // assert(s->idx == N32);
  // assert(size % 2 == 0);
  // assert(size >= N64);

  gen_rand_array(s, (w128_t *)array, size / 2);
  s->idx = N32;

#if defined(BIG_ENDIAN64) && !defined(ONLY64)
  swap((w128_t *)array, size / 2);
#endif
}

/**
 * This function initializes the internal state array with a 32-bit
 * integer seed.
 *
 * @param seed a 32-bit integer used as the seed.
 */
void init_gen_rand(sfmt_state_t *s, uint32_t seed)
{
  int i;

  s->psfmt32[idxof(0)] = seed;
  for(i = 1; i < N32; i++)
  {
    s->psfmt32[idxof(i)] = 1812433253UL * (s->psfmt32[idxof(i - 1)] ^ (s->psfmt32[idxof(i - 1)] >> 30)) + i;
  }
  s->idx = N32;
  period_certification(s);
  s->initialized = 1;
}

/**
 * This function initializes the internal state array,
 * with an array of 32-bit integers used as the seeds
 * @param init_key the array of 32-bit integers, used as a seed.
 * @param key_length the length of init_key.
 */
void init_by_array(sfmt_state_t *s, uint32_t *init_key, int key_length)
{
  int i, j, count;
  uint32_t r;
  int lag;
  int mid;
  int size = N * 4;

  if(size >= 623)
  {
    lag = 11;
  }
  else if(size >= 68)
  {
    lag = 7;
  }
  else if(size >= 39)
  {
    lag = 5;
  }
  else
  {
    lag = 3;
  }
  mid = (size - lag) / 2;

  memset(s->sfmt, 0x8b, sizeof(s->sfmt));
  if(key_length + 1 > N32)
  {
    count = key_length + 1;
  }
  else
  {
    count = N32;
  }
  r = func1(s->psfmt32[idxof(0)] ^ s->psfmt32[idxof(mid)] ^ s->psfmt32[idxof(N32 - 1)]);
  s->psfmt32[idxof(mid)] += r;
  r += key_length;
  s->psfmt32[idxof(mid + lag)] += r;
  s->psfmt32[idxof(0)] = r;

  count--;
  for(i = 1, j = 0; (j < count) && (j < key_length); j++)
  {
    r = func1(s->psfmt32[idxof(i)] ^ s->psfmt32[idxof((i + mid) % N32)]
              ^ s->psfmt32[idxof((i + N32 - 1) % N32)]);
    s->psfmt32[idxof((i + mid) % N32)] += r;
    r += init_key[j] + i;
    s->psfmt32[idxof((i + mid + lag) % N32)] += r;
    s->psfmt32[idxof(i)] = r;
    i = (i + 1) % N32;
  }
  for(; j < count; j++)
  {
    r = func1(s->psfmt32[idxof(i)] ^ s->psfmt32[idxof((i + mid) % N32)]
              ^ s->psfmt32[idxof((i + N32 - 1) % N32)]);
    s->psfmt32[idxof((i + mid) % N32)] += r;
    r += i;
    s->psfmt32[idxof((i + mid + lag) % N32)] += r;
    s->psfmt32[idxof(i)] = r;
    i = (i + 1) % N32;
  }
  for(j = 0; j < N32; j++)
  {
    r = func2(s->psfmt32[idxof(i)] + s->psfmt32[idxof((i + mid) % N32)]
              + s->psfmt32[idxof((i + N32 - 1) % N32)]);
    s->psfmt32[idxof((i + mid) % N32)] ^= r;
    r -= i;
    s->psfmt32[idxof((i + mid + lag) % N32)] ^= r;
    s->psfmt32[idxof(i)] = r;
    i = (i + 1) % N32;
  }

  s->idx = N32;
  period_certification(s);
  s->initialized = 1;
}


static inline void dt_points_init(dt_points_t *p, const unsigned int num_threads)
{
  sfmt_state_t *states = (sfmt_state_t *)dt_alloc_align(64, sizeof(sfmt_state_t) * num_threads);
  p->s = (sfmt_state_t **)calloc(num_threads, sizeof(sfmt_state_t *));
  p->num = num_threads;

  int seed = 0xD71337;
  for(int i = 0; i < (int)num_threads; i++)
  {
    p->s[i] = states + i;
#if !defined(BIG_ENDIAN64) || defined(ONLY64)
    p->s[i]->psfmt64 = (uint64_t *)&(p->s[i]->sfmt[0].u[0]);
#endif
    p->s[i]->psfmt32 = &(p->s[i]->sfmt[0].u[0]);
    p->s[i]->initialized = 0;
    p->s[i]->parity[0] = PARITY1;
    p->s[i]->parity[1] = PARITY2;
    p->s[i]->parity[2] = PARITY3;
    p->s[i]->parity[3] = PARITY4;
    init_gen_rand(p->s[i], seed);
    seed ^= seed << 1;
  }
}

static inline void dt_points_cleanup(dt_points_t *p)
{
  dt_free_align(p->s[0]);
  free(p->s);
}

static inline float dt_points_get_for(dt_points_t *p, const unsigned int thread_num)
{
  return genrand_real2f(p->s[thread_num]);
}

static inline float dt_points_get()
{
  return dt_points_get_for(darktable.points, dt_get_thread_num());
}

#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
