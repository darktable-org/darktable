/*
    This file is part of darktable,
    Copyright (C) 2012-2026 darktable developers.

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
#define NORM_MIN 1.52587890625e-05f // norm can't be < to 2^(-16)


constant sampler_t sampleri =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

constant sampler_t samplerf =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_LINEAR;

constant sampler_t samplerc =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP         | CLK_FILTER_NEAREST;

// sampler for when the bound checks are already done manually
constant sampler_t samplerA = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_NONE            | CLK_FILTER_NEAREST;


#ifndef M_PI_F
#define M_PI_F 3.14159265358979323846f
#endif

#ifndef M_LN2f
#define M_LN2f 0.69314718055994530942f
#endif

#ifndef M_PI_2f
#define M_PI_2f 1.57079632679489661923f
#endif

#ifndef M_PI_4f
#define M_PI_4f 0.78539816339744830962f
#endif

#ifndef M_SQRT2_F
#define M_SQRT2_F 1.41421356237309504880f
#endif

#define DT_2PI_F 6.28318530717958647693f

#define LUT_ELEM 512 // gamut LUT number of elements:

#define RED 0
#define GREEN 1
#define BLUE 2
#define ALPHA 3

#if(defined(__FAST_RELAXED_MATH__) && __FAST_RELAXED_MATH__ == 1)
  #define dtcl_sin(A) native_sin(A)
  #define dtcl_cos(A) native_cos(A)
  #define dtcl_sqrt(A) native_sqrt(A)
  #define dtcl_pow(A,B) native_powr(A,B)
  #define dtcl_exp(A) native_exp(A)
  #define dtcl_log(A) native_log(A)
  #define dtcl_log2(A) native_log2(A)
  #define dtcl_exp2(A) native_exp2(A)
  #define dtcl_sin(A) native_sin(A)
  #define dtcl_cos(A) native_cos(A)

  static inline float dt_fast_hypot(const float x, const float y)
  {
    return native_sqrt(x * x + y * y);
  }

  // Allow the compiler to convert a * b + c to fused multiply-add to use hardware acceleration
  // on compatible platforms
  #pragma OPENCL FP_CONTRACT ON
#else
  #define dtcl_sin(A) sin(A)
  #define dtcl_cos(A) cos(A)
  #define dtcl_sqrt(A) sqrt(A)
  #define dtcl_pow(A,B) pow(A,B)
  #define dtcl_exp(A) exp(A)
  #define dtcl_log(A) log(A)
  #define dtcl_log2(A) log2(A)
  #define dtcl_exp2(A) exp2(A)
  #define dtcl_sin(A) sin(A)
  #define dtcl_cos(A) cos(A)

  static inline float dt_fast_hypot(const float x, const float y)
  {
    return hypot(x, y);
  }

  #pragma OPENCL FP_CONTRACT OFF
#endif

// Kahan summation algorithm
#define Kahan_sum(m, c, add)        \
  {                                 \
    const float t1 = (add) - (c);   \
    const float t2 = (m) + t1;      \
    c = (t2 - m) - t1;              \
    m = t2;                         \
  }

static inline float scharr_gradient(global float *in, const int k, const int w)
{
  const float gx = 47.0f / 255.0f * (in[k-w-1] - in[k-w+1] + in[k+w-1] - in[k+w+1])
                + 162.0f / 255.0f * (in[k-1]   - in[k+1]);
  const float gy = 47.0f / 255.0f * (in[k-w-1] - in[k+w-1] + in[k-w+1] - in[k+w+1])
                + 162.0f / 255.0f * (in[k-w]   - in[k+w]);
  return dt_fast_hypot(gx, gy);
}

static inline int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3;
}


static inline int
FCxtrans(const int row, const int col, global const unsigned char (*const xtrans)[6])
{
  // There used to be a few cases in xtrans demosaicers in which row or col was negative.
  // The +600 ensures a non-negative array index as in CPU code
  return xtrans[(row + 600) % 6][(col + 600) % 6];
}

static inline int
fcol(const int row, const int col, const unsigned int filters, global const unsigned char (*const xtrans)[6])
{
  return (filters == 9) ? xtrans[(row + 600) % 6][(col + 600) % 6]
                        : filters >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3;
}

void
atomic_add_f(
    global float *val,
    const  float  delta)
{
#ifdef NVIDIA_SM_20
  // buys me another 3x--10x over the `algorithmic' improvements in the splat kernel below,
  // depending on configuration (sigma_s and sigma_r)
  float res = 0;
  asm volatile ("atom.global.add.f32 %0, [%1], %2;" : "=f"(res) : "l"(val), "f"(delta));

#else
  union
  {
    float f;
    unsigned int i;
  }
  old_val;
  union
  {
    float f;
    unsigned int i;
  }
  new_val;

  global volatile unsigned int *ival = (global volatile unsigned int *)val;

  do
  {
    // the following is equivalent to old_val.f = *val. however, as according to the opencl standard
    // we can not rely on global buffer val to be consistently cached (relaxed memory consistency) we 
    // access it via a slower but consistent atomic operation.
    old_val.i = atomic_add(ival, 0);
    new_val.f = old_val.f + delta;
  }
  while (atomic_cmpxchg (ival, old_val.i, new_val.i) != old_val.i);
#endif
}

float fast_mexp2f(const float x)
{
  const float i1 = (float)0x3f800000u; // 2^0
  const float i2 = (float)0x3f000000u; // 2^-1
  const float k0 = i1 + x * (i2 - i1);
  union { float f; unsigned int i; } k;
  k.i = (k0 >= (float)0x800000u) ? k0 : 0;
  return k.f;
}

/* we use this exp approximation to maintain full identity with cpu path */
static inline float
dt_fast_expf(const float x)
{
  // meant for the range [-100.0f, 0.0f]. largest error ~ -0.06 at 0.0f.
  // will get _a_lot_ worse for x > 0.0f (9000 at 10.0f)..
  const int i1 = 0x3f800000u;
  // e^x, the comment would be 2^x
  const int i2 = 0x402DF854u;//0x40000000u;
  // const int k = CLAMPS(i1 + x * (i2 - i1), 0x0u, 0x7fffffffu);
  // without max clamping (doesn't work for large x, but is faster):
  const int k0 = i1 + x * (i2 - i1);
  union {
      float f;
      int k;
  } u;
  u.k = k0 > 0 ? k0 : 0;
  return u.f;
}

static inline float fsquare(const float a)
{
  return (a * a);
}

static inline float fcube(const float a)
{
  return (a * a * a);
}

static inline float clipf(const float a)
{
  return clamp(a, 0.0f, 1.0f);
}

static inline float4 clip4(const float4 a)
{
  return clamp(a, (float4)0.0f, (float4)1.0f);
}

static inline float fmax3(const float4 o)
{
  return fmax(fmax(o.x, o.y), o.z);
}

static inline int clip_mirror(int i, const int max)
{
  if(i < 0)
    i = -i;
  else if(i > max)
    i = max - (i - max);
  return i;
}

/* Some inline functions making life easier when reading photosites
   or pixels from cl_mem images.
  The variants with a leading A use the faster samplerA interpolater, only
  to be used with safe positions as otherwise the read value will be undefined,
  (on AMD possibly NaN).
*/
static inline float readsingle(read_only image2d_t in, int col, int row)
{
  return read_imagef(in, sampleri, (int2)(col, row)).x;
}

static inline float Areadsingle(read_only image2d_t in, int col, int row)
{
  return read_imagef(in, samplerA, (int2)(col, row)).x;
}

static inline float4 readpixel(read_only image2d_t in, int col, int row)
{
  return read_imagef(in, sampleri, (int2)(col, row));
}

static inline float4 Areadpixel(read_only image2d_t in, int col, int row)
{
  return read_imagef(in, samplerA, (int2)(col, row));
}

static inline float4 Creadpixel(read_only image2d_t in, int col, int row)
{
  return read_imagef(in, samplerc, (int2)(col, row));
}

static inline float readalpha(read_only image2d_t in, int col, int row)
{
  return clipf(read_imagef(in, sampleri, (int2)(col, row)).w);
}

static inline void write_ipixel(write_only image2d_t out, const int2 pos, const float4 pixel)
{
  write_imagef(out, pos, pixel);
}