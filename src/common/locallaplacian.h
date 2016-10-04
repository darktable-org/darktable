/*
 *  This file is part of darktable,
 *  copyright (c) 2016 johannes hanika
 *  copyright (c) 2016 Maixmilian Trescher
 *
 *  darktable is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  darktable is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <math.h>

// downsample width/height to given level
static inline int dl(int size, const int level)
{
  for(int l = 0; l < level; l++) size = (size - 1) / 2 + 1;
  return size;
}

// needs a boundary of 1px around i,j or else it will crash.
static inline float ll_expand_gaussian(const float *const coarse, const int i, const int j, const int wd,
                                       const int ht)
{
  assert(i > 0);
  assert(i < wd - 1);
  assert(j > 0);
  assert(j < ht - 1);
  float c = 0.0f;
  const int cw = (wd - 1) / 2 + 1;
  const float a = 0.4f;
  const float w[5] = { 1. / 4. - a / 2., 1. / 4., a, 1. / 4., 1. / 4. - a / 2. };
  const int ind = (j / 2) * cw + i / 2;
  switch((i & 1) + 2 * (j & 1))
  {
    case 0: // both are even, 3x3 stencil
      for(int ii = -1; ii <= 1; ii++)
        for(int jj = -1; jj <= 1; jj++) c += coarse[ind + cw * jj + ii] * w[2 * jj + 2] * w[2 * ii + 2];
      break;
    case 1: // i is odd, 2x3 stencil
      for(int ii = 0; ii <= 1; ii++)
        for(int jj = -1; jj <= 1; jj++) c += coarse[ind + cw * jj + ii] * w[2 * jj + 2] * w[2 * ii + 1];
      break;
    case 2: // j is odd, 3x2 stencil
      for(int ii = -1; ii <= 1; ii++)
        for(int jj = 0; jj <= 1; jj++) c += coarse[ind + cw * jj + ii] * w[2 * jj + 1] * w[2 * ii + 2];
      break;
    default: // case 3: // both are odd, 2x2 stencil
      for(int ii = 0; ii <= 1; ii++)
        for(int jj = 0; jj <= 1; jj++) c += coarse[ind + cw * jj + ii] * w[2 * jj + 1] * w[2 * ii + 1];
      break;
  }
  assert(c == c);
  return 4.0f * c;
}

// helper to fill in one pixel boundary by copying it
static inline void ll_fill_boundary(float *const input, const int wd, const int ht)
{
  for(int j = 1; j < ht - 1; j++) input[j * wd] = input[j * wd + 1];
  for(int j = 1; j < ht - 1; j++) input[j * wd + wd - 1] = input[j * wd + wd - 2];
  memcpy(input, input + wd, sizeof(float) * wd);
  memcpy(input + wd * (ht - 1), input + wd * (ht - 2), sizeof(float) * wd);
}

// static inline float ll_get_laplacian(
static inline void gauss_expand(const float *const input, // coarse input
                                float *const fine,        // upsampled, blurry output
                                const int wd,             // fine res
                                const int ht)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) collapse(2)
#endif
  for(int j = 1; j < ht - 1; j++)
    for(int i = 1; i < wd - 1; i++) fine[j * wd + i] = ll_expand_gaussian(input, i, j, wd, ht);
  ll_fill_boundary(fine, wd, ht);
}

static inline void gauss_reduce(const float *const input, // fine input buffer
                                float *const coarse,      // coarse scale, blurred input buf
                                const int wd,             // fine res
                                const int ht)
{
  // blur, store only coarse res
  const int cw = (wd - 1) / 2 + 1, ch = (ht - 1) / 2 + 1;

  memset(coarse, 0, sizeof(float) * cw * ch);
  const float a = 0.4f;
  const float w[5] = { 1. / 4. - a / 2., 1. / 4., a, 1. / 4., 1. / 4. - a / 2. };
  // direct 5x5 stencil only on required pixels:
  for(int j = 1; j < ch - 1; j++)
    for(int i = 1; i < cw - 1; i++)
      for(int jj = -2; jj <= 2; jj++)
        for(int ii = -2; ii <= 2; ii++)
        {
          coarse[j * cw + i] += input[(2 * j + jj) * wd + 2 * i + ii] * w[ii + 2] * w[jj + 2];
          assert(coarse[j * cw + i] == coarse[j * cw + i]);
        }
  ll_fill_boundary(coarse, cw, ch);
}

// allocate output buffer with monochrome brightness channel from input, padded
// up by max_supp on all four sides, dimensions written to wd2 ht2
static inline float *ll_pad_input(const float *const input, const int wd, const int ht, const int max_supp,
                                  const int stride, int *wd2, int *ht2, int channel)
{
  *wd2 = 2 * max_supp + wd;
  *ht2 = 2 * max_supp + ht;
  // float *out = (float *)dt_alloc_align(64, *wd2**ht2*sizeof(float));
  float *out = (float *)malloc(*wd2 * *ht2 * sizeof(float));

  for(int j = 0; j < ht; j++)
  {
    for(int i = 0; i < max_supp; i++)
      // out[(j+max_supp)**wd2+i] = powf(MAX(0.0f, input[stride*wd*j+0]), 1./2.2);
      out[(j + max_supp) * *wd2 + i] = (input + stride * wd * j + 0)[channel];
    for(int i = 0; i < wd; i++)
      // out[(j+max_supp)**wd2+i+max_supp] = powf(MAX(0.0f, input[stride*(wd*j+i)+0]), 1./2.2);
      out[(j + max_supp) * *wd2 + i + max_supp] = (input + stride * (wd * j + i))[channel];
    for(int i = wd + max_supp; i < *wd2; i++)
      // out[(j+max_supp)**wd2+i] = powf(MAX(0.0f, input[stride*(j*wd+wd-1)+0]), 1./2.2);
      out[(j + max_supp) * *wd2 + i] = (input + stride * (j * wd + wd - 1) + 0)[channel];
  }
  for(int j = 0; j < max_supp; j++) memcpy(out + *wd2 * j, out + max_supp * *wd2, sizeof(float) * *wd2);
  for(int j = max_supp + ht; j < *ht2; j++)
    memcpy(out + *wd2 * j, out + *wd2 * (max_supp + ht - 1), sizeof(float) * *wd2);

  return out;
}

static inline float ll_laplacian(const float *const coarse, // coarse res gaussian
                                 const float *const fine,   // fine res gaussian
                                 const int i,               // fine index
                                 const int j,
                                 const int wd, // fine width
                                 const int ht) // fine height
{
  const float c = ll_expand_gaussian(coarse, i, j, wd, ht);
  return fine[j * wd + i] - c;
}

static inline void local_laplacian(const float *const input, // input buffer in some Labx or yuvx format
                                   float *const out,         // output buffer with colour
                                   const int wd,             // width and
                                   const int ht,             // height of the input buffer
                                   const int stride,         // stride for input buffer
                                   const float contrast,     // user param: control strength of effect
                                   int num_levels)      // user param: number of levels in the pyramid
{
  const int max_supp = 1 << (num_levels - 1);
  fprintf(stderr, "Do padding for %i level", num_levels);
  fprintf(stderr, "Do padding by %i pixesl", max_supp);
  int w, h;
  // we should do this dynamically, but we don't allow for more then 10 levels anyways, so some more pointers don't hurt
  num_levels = (num_levels <= 10) ? num_levels : 10;
  float *padded[10][3] = { 0 };
  for(int channel = 0; channel < 3; channel++)
  {
    padded[0][channel] = ll_pad_input(input, wd, ht, max_supp, stride, &w, &h, channel);
  }

  // allocate pyramid pointers for padded input
  for(int l = 1; l < num_levels; l++)
  {
    for(int channel = 0; channel < 3; channel++)
    {
      padded[l][channel] = (float *)malloc(sizeof(float) * dl(w, l) * dl(h, l));
    }
  }
  // create gauss pyramid of padded input
  for(int l = 1; l < num_levels - 1; l++)
  {
    for(int channel = 0; channel < 3; channel++)
    {
      gauss_reduce(padded[l - 1][channel], padded[l][channel], dl(w, l - 1), dl(h, l - 1));
    }
  }

  // allocate pyramid pointers for output
  float *output[10] = { 0 };
  // only use L channel (channel 0) from precomputed B&W image
  output[0] = ll_pad_input(out, wd, ht, max_supp, stride, &w, &h, 0);

  // allocate pyramid pointers for padded output
  for(int l = 1; l < num_levels; l++)
  {
    output[l] = (float *)malloc(sizeof(float) * dl(w, l) * dl(h, l));
  }
  // create gauss pyramid of output image
  for(int l = 1; l < num_levels - 1; l++)
  {
    gauss_reduce(output[l - 1], output[l], dl(w, l - 1), dl(h, l - 1));
  }

  #pragma omp parallel for default(none) schedule(dynamic) collapse(2) shared(w, h, padded, output, num_levels)
  for(int j = 0; j < ht; j++)
    for(int i = 0; i < wd; i++)
    {
      // copy basic image
      (out + stride * (j * wd + i))[0] = output[0][(j + max_supp) * w + max_supp + i];
      // or start from black for debugging locla contrast additions etc
      //(out + stride * (j * wd + i))[0] = 0;
      // and increase color contrast
      const float k[8] = { contrast, contrast, contrast, contrast, contrast, contrast, contrast, contrast};
      for(int l = 0; l < num_levels-1; l++)
      {
        // TODO: verify that these coordinates make sense
        // we want the coordinates at level l in the pyramid corresponding to image coordinates i,j
        int ii = (i + max_supp) / (1 << l);
        int jj = (j + max_supp) / (1 << l);
        const int pw = dl(w, l), ph = dl(h, l);
        float lL = ll_laplacian(padded[l + 1][0], padded[l][0], ii, jj, pw, ph);
        float la = ll_laplacian(padded[l + 1][1], padded[l][1], ii, jj, pw, ph);
        float lb = ll_laplacian(padded[l + 1][2], padded[l][2], ii, jj, pw, ph);
        float DeltaE = sqrtf(lL * lL + la * la + lb * lb);
        float lG = (ll_laplacian(output[l + 1], output[l], ii, jj, pw, ph));

        const float p = 0.25;
        float lambda = powf(DeltaE / fabs(lG), p);
        // formula from K. Smith et al. / Apparent Lighntess Grayscale
        // not convincing, maybe they 'mean' their formula differently?
        (out + stride * (j * wd + i))[0] += k[l] * lambda * lG;
      }
      // other components are 0 -> monochrome!
      (out + stride * (j * wd + i))[1] = 0; //(input + stride * (j * wd + i))[1];
      (out + stride * (j * wd + i))[2] = 0; //(input + stride * (j * wd + i))[2];
    }
  // free all buffers!
  for(int l = 0; l < num_levels; l++)
  {
    for(int channel = 0; channel < 3; channel++)
    {
      free(padded[l][channel]);
    }
    free(output[l]);
  }
#undef num_levels
}

// vim: shiftwidth=2:expandtab:tabstop=2:cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
