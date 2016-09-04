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

static inline void rgb_to_yuv(const float *const rgb, float *const yuv)
{
  const float M[] = { 0.299f, 0.587f, 0.114f, -0.14713f, -0.28886f, 0.436f, 0.615f, -0.51499f, -0.10001f };
  yuv[0] = yuv[1] = yuv[2] = 0.0f;
  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 3; j++) yuv[i] += M[3 * i + j] * rgb[j];
}

static inline float rgb_to_y(const float *const rgb)
{
  const float M[] = { 0.299f, 0.587f, 0.114f, -0.14713f, -0.28886f, 0.436f, 0.615f, -0.51499f, -0.10001f };
  float y = 0.0f;
  for(int j = 0; j < 3; j++) y += M[j] * rgb[j];
  // XXX try shadow lifting: if(y < .5f) return .5f * powf(2.0f*y, 1./1.8);
  return y;
}

static inline void yuv_to_rgb(const float *const yuv, float *const rgb)
{
  const float Mi[] = { 1.0f, 0.0f, 1.13983f, 1.0f, -0.39465f, -0.58060f, 1.0f, 2.03211f, 0.0f };
  rgb[0] = rgb[1] = rgb[2] = 0.0f;
  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 3; j++) rgb[i] += Mi[3 * i + j] * yuv[j];
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

static inline float ll_curve(const float x, const float g, const float sigma, const float shadows,
                             const float highlights, const float clarity)
{
  // this is in the original matlab code instead:
  // I_remap=fact*(I-ref).*exp(-(I-ref).*(I-ref)./(2*sigma*sigma));
  // also they add up the laplacian to the ones of the base image
  // return g + 0.7 *  (x-g) + clarity * (x - g) * expf(-(x-g)*(x-g)/(2.0*sigma*sigma));

  // XXX highlights does weird things, causing halos in dark regions
  // XXX shadows seems to compress highlight contrast (???)
  // XXX need to adjust this to bias shad/hi depending on g!
  // const float compress = .2;// + powf(g, .4);
  // if(x > g+sigma)
  //   // return g + highlights * (x-g) + clarity * (x - g) * expf(-(x-g)*(x-g)/(2.0*sigma*sigma));
  //   return g+sigma + highlights * (x-g-sigma) + clarity * (x - g) * expf(-(x-g)*(x-g)/(2.0*sigma*sigma));
  // else if(x < g-sigma)
  //   // return g + shadows    * (x-g) + clarity * (x - g) * expf(-(x-g)*(x-g)/(2.0*sigma*sigma));
  //   return g-sigma + shadows * (x-g+sigma) + clarity * (x - g) * expf(-(x-g)*(x-g)/(2.0*sigma*sigma));
  // else
  return g + (x - g) + clarity * (x - g) * expf(-(x - g) * (x - g) / (2.0 * sigma * sigma));
#if 0
  // XXX DEBUG: shad/hi curve needs to be something smart along these lines:
  // if(x < .5f)
  //{
    // const float g2 = powf(g, 1./shadows);
    // return powf(x, g);
    // return x + g * shadows;//- g + g2;
  //}
  // centered value
  const float c = x-g;
  if(c >  sigma) return g + sigma + highlights * (c-sigma);
  if(c < -sigma) return g - sigma + shadows    * (c+sigma);
  // const float beta = 0.1;//(1.0-x)*(1.0-x);
  // if(c >  sigma) return g + sigma + beta * (c-sigma);
  // if(c < -sigma) return g - sigma + beta * (c+sigma);

  // TODO: paper says to blend this in to avoid boosting noise:
  // t = smoothstep(x, 1%, 2%)
  const float t = MIN(MAX(0.0f, (x-0.01)/(0.02-0.01)), 1.0f);
  assert(t == t);
  const float delta = fabsf(c)/sigma;
  assert(delta == delta);
  const float f_d = t * powf(delta, 1.-clarity) + (1.0f-t)*delta;
  assert(f_d == f_d);
  assert(g + copysignf(sigma * f_d, c) == g + copysignf(sigma * f_d, c));
  return g + copysignf(sigma * f_d, c);
#endif
}

static inline void local_laplacian(const float *const input, // input buffer in some Labx or yuvx format
                                   float *const out,         // output buffer with colour
                                   const int wd,             // width and
                                   const int ht,             // height of the input buffer
                                   const int stride,         // stride for input buffer
                                   const float sigma,        // user param: separate shadows/midtones/highlights
                                   const float shadows,      // user param: lift shadows
                                   const float highlights,   // user param: compress highlights
                                   const float clarity)      // user param: increase clarity/local contrast
{
#define num_levels 5
  const int max_supp = 1 << (num_levels - 1);
  fprintf(stderr, "Do padding by %i pixesl", max_supp);
  int w, h;
  float *padded[num_levels][3] = { 0 };
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
  float *output[num_levels] = { 0 };
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

  //#pragma omp parallel for default(none) schedule(dynamic) collapse(2) shared(w, h, padded, output)
  for(int j = 0; j < ht; j++)
    for(int i = 0; i < wd; i++)
    {
      // copy basic image
      (out + stride * (j * wd + i))[0] = output[0][(j + max_supp) * w + max_supp + i];
      // or start from black for debugging locla contrast additions etc
      //(out + stride * (j * wd + i))[0] = 0;
      // and increase color contrast
      const float k[8] = { 0.7, 0.5, 0.4, 0.2, 0.1, 0.1, 0.1, 0.1 };
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
