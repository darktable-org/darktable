/*
 *    This file is part of darktable,
 *    Copyright (C) 2021 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

// When included by a C++ file, restrict qualifiers are not allowed
#ifdef __cplusplus
#define DT_RESTRICT
#else
#define DT_RESTRICT restrict
#endif

// Helper to force heap vectors to be aligned on 64 byte blocks to enable AVX2
// If this is applied to a struct member and the struct is allocated on the heap, then it must be allocated
// on a 64 byte boundary to avoid crashes or undefined behavior because of unaligned memory access.
#define DT_ALIGNED_ARRAY __attribute__((aligned(64)))
#define DT_ALIGNED_PIXEL __attribute__((aligned(16)))

// utility type to ease declaration of aligned small arrays to hold a pixel (and document their purpose)
typedef DT_ALIGNED_PIXEL float dt_aligned_pixel_t[4];

// a 3x3 matrix, padded to permit SSE instructions to be used for multiplication and addition
typedef float DT_ALIGNED_ARRAY dt_colormatrix_t[4][4];

// To be able to vectorize per-pixel loops, we need to operate on all four channels, but if the compiler does
// not auto-vectorize, doing so increases computation by 1/3 for a channel which typically is ignored anyway.
// Select the appropriate number of channels over which to loop to produce the fastest code.
#ifdef DT_NO_VECTORIZATION
#define DT_PIXEL_SIMD_CHANNELS 3
#else
#define DT_PIXEL_SIMD_CHANNELS 4
#endif

// A macro which gives us a configurable shorthand to produce the optimal performance when processing all of the
// channels in a pixel.  Its first argument is the name of the variable to be used inside the 'for' loop it creates,
// while the optional second argument is a set of OpenMP directives, typically specifying variable alignment.
// If indexing off of the beginning of any buffer allocated with dt's image or aligned allocation functions, the
// alignment to specify is 64; otherwise, use 16, as there may have been an odd number of pixels from the start.
// Sample usage:
//         for_each_channel(k,aligned(src,dest:16))
//         {
//           src[k] = dest[k] / 3.0f;
//         }
#if defined(_OPENMP) && defined(OPENMP_SIMD_) && !defined(DT_NO_SIMD_HINTS)
//https://stackoverflow.com/questions/45762357/how-to-concatenate-strings-in-the-arguments-of-pragma
#define _DT_Pragma_(x) _Pragma(#x)
#define _DT_Pragma(x) _DT_Pragma_(x)
#define for_each_channel(_var, ...) \
  _DT_Pragma(omp simd __VA_ARGS__) \
  for (size_t _var = 0; _var < DT_PIXEL_SIMD_CHANNELS; _var++)
#define for_four_channels(_var, ...) \
  _DT_Pragma(omp simd __VA_ARGS__) \
  for (size_t _var = 0; _var < 4; _var++)
#define for_three_channels(_var, ...) \
  _DT_Pragma(omp simd __VA_ARGS__) \
  for (size_t _var = 0; _var < 3; _var++)
#else
#define for_each_channel(_var, ...) \
  for (size_t _var = 0; _var < DT_PIXEL_SIMD_CHANNELS; _var++)
#define for_four_channels(_var, ...) \
  for (size_t _var = 0; _var < 4; _var++)
#define for_three_channels(_var, ...) \
  for (size_t _var = 0; _var < 3; _var++)
#endif


// transpose a padded 3x3 matrix
static inline void transpose_3xSSE(const dt_colormatrix_t input, dt_colormatrix_t output)
{
  output[0][0] = input[0][0];
  output[0][1] = input[1][0];
  output[0][2] = input[2][0];
  output[0][3] = 0.0f;

  output[1][0] = input[0][1];
  output[1][1] = input[1][1];
  output[1][2] = input[2][1];
  output[1][3] = 0.0f;

  output[2][0] = input[0][2];
  output[2][1] = input[1][2];
  output[2][2] = input[2][2];
  output[2][3] = 0.0f;

  for_four_channels(c, aligned(output))
    output[3][c] = 0.0f;
}

// transpose and pad a 3x3 matrix into the padded format optimized for vectorization
static inline void transpose_3x3_to_3xSSE(const float input[9], dt_colormatrix_t output)
{
  output[0][0] = input[0];
  output[0][1] = input[3];
  output[0][2] = input[6];
  output[0][3] = 0.0f;

  output[1][0] = input[1];
  output[1][1] = input[4];
  output[1][2] = input[7];
  output[1][3] = 0.0f;

  output[2][0] = input[2];
  output[2][1] = input[5];
  output[2][2] = input[8];
  output[2][3] = 0.0f;

  for_four_channels(c, aligned(output))
    output[3][c] = 0.0f;
}

// convert a 3x3 matrix into the padded format optimized for vectorization
static inline void repack_double3x3_to_3xSSE(const double input[9], dt_colormatrix_t output)
{
  output[0][0] = input[0];
  output[0][1] = input[1];
  output[0][2] = input[2];
  output[0][3] = 0.0f;

  output[1][0] = input[3];
  output[1][1] = input[4];
  output[1][2] = input[5];
  output[1][3] = 0.0f;

  output[2][0] = input[6];
  output[2][1] = input[7];
  output[2][2] = input[8];
  output[2][3] = 0.0f;

  for(size_t c = 0; c < 4; c++)
    output[3][c] = 0.0f;
}

// convert a 3x3 matrix into the padded format optimized for vectorization
static inline void pack_3xSSE_to_3x3(const dt_colormatrix_t input, float output[9])
{
  output[0] = input[0][0];
  output[1] = input[0][1];
  output[2] = input[0][2];
  output[3] = input[1][0];
  output[4] = input[1][1];
  output[5] = input[1][2];
  output[6] = input[2][0];
  output[7] = input[2][1];
  output[8] = input[2][2];
}

// vectorized multiplication of padded 3x3 matrices
static inline void dt_colormatrix_mul(dt_colormatrix_t dst, const dt_colormatrix_t m1, const dt_colormatrix_t m2)
{
  for(int k = 0; k < 3; ++k)
  {
    dt_aligned_pixel_t sum = { 0.0f };
    for_each_channel(i)
    {
      for(int j = 0; j < 3; j++)
        sum[i] += m1[k][j] * m2[j][i];
      dst[k][i] = sum[i];
    }
  }
}

static inline void dt_colormatrix_transpose(dt_colormatrix_t dst,
                                            const dt_colormatrix_t src)
{
  for_four_channels(c)
  {
    dst[0][c] = src[c][0];
    dst[1][c] = src[c][1];
    dst[2][c] = src[c][2];
    dst[3][c] = src[c][3];
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

