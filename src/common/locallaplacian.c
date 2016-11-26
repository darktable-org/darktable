/*
    This file is part of darktable,
    copyright (c) 2016 johannes hanika.

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

#include "common/darktable.h"
#include "common/locallaplacian.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <xmmintrin.h>

// downsample width/height to given level
static inline int dl(int size, const int level)
{
  for(int l=0;l<level;l++)
    size = (size-1)/2+1;
  return size;
}

// needs a boundary of 2px around i,j or else it will crash.
// (translates to a 1px boundary around the corresponding pixel in the coarse buffer)
// the lower bound 0 would be fine with 1px boundary, but the
// upper bound needs one more pixel for even widths/heights.
static inline float ll_expand_gaussian(
    const float *const coarse,
    const int i,
    const int j,
    const int wd,
    const int ht)
{
  assert(i > 0);
  assert(i < wd-1);
  assert(j > 0);
  assert(j < ht-1);
  assert(j/2 + 1 < (ht-1)/2+1);
  assert(i/2 + 1 < (wd-1)/2+1);
  float c = 0.0f;
  const int cw = (wd-1)/2+1;
  // TODO: manually expand to sums:
  const float w[5] = {1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f};
  const int ind = (j/2)*cw+i/2;
  // case 0:     case 1:     case 2:     case 3:
  //  x . x . x   x . x . x   x . x . x   x . x . x
  //  . . . . .   . . . . .   . .[.]. .   .[.]. . .
  //  x .[x]. x   x[.]x . x   x . x . x   x . x . x
  //  . . . . .   . . . . .   . . . . .   . . . . .
  //  x . x . x   x . x . x   x . x . x   x . x . x
  switch((i&1) + 2*(j&1))
  {
    case 0: // both are even, 3x3 stencil
      for(int ii=-1;ii<=1;ii++)
        for(int jj=-1;jj<=1;jj++)
          c += coarse[ind+cw*jj+ii]*w[2*jj+2]*w[2*ii+2];
      break;
    case 1: // i is odd, 2x3 stencil
      for(int ii=0;ii<=1;ii++)
        for(int jj=-1;jj<=1;jj++)
          c += coarse[ind+cw*jj+ii]*w[2*jj+2]*w[2*ii+1];
      break;
    case 2: // j is odd, 3x2 stencil
      for(int ii=-1;ii<=1;ii++)
        for(int jj=0;jj<=1;jj++)
          c += coarse[ind+cw*jj+ii]*w[2*jj+1]*w[2*ii+2];
      break;
    default: // case 3: // both are odd, 2x2 stencil
      for(int ii=0;ii<=1;ii++)
        for(int jj=0;jj<=1;jj++)
          c += coarse[ind+cw*jj+ii]*w[2*jj+1]*w[2*ii+1];
      break;
  }
  assert(c==c);
  return 4.0f * c;
}

// helper to fill in one pixel boundary by copying it
static inline void ll_fill_boundary1(
    float *const input,
    const int wd,
    const int ht)
{
  for(int j=1;j<ht-1;j++) input[j*wd] = input[j*wd+1];
  for(int j=1;j<ht-1;j++) input[j*wd+wd-1] = input[j*wd+wd-2];
  memcpy(input,    input+wd, sizeof(float)*wd);
  memcpy(input+wd*(ht-1), input+wd*(ht-2), sizeof(float)*wd);
}

// XXX i believe this blurs in wrong values! the buffer padding needs to be fixed instead!
// helper to fill in two pixels boundary by copying it
static inline void ll_fill_boundary2(
    float *const input,
    const int wd,
    const int ht)
{
  for(int j=1;j<ht-1;j++) input[j*wd] = input[j*wd+1] = input[j*wd+2];
  for(int j=1;j<ht-1;j++) input[j*wd+wd-1] = input[j*wd+wd-2] = input[j*wd+wd-3];
  memcpy(input,    input+2*wd, sizeof(float)*wd);
  memcpy(input+wd, input+2*wd, sizeof(float)*wd);
  memcpy(input+wd*(ht-1), input+wd*(ht-3), sizeof(float)*wd);
  memcpy(input+wd*(ht-2), input+wd*(ht-3), sizeof(float)*wd);
}

static inline void gauss_expand(
    const float *const input, // coarse input
    float *const fine,        // upsampled, blurry output
    const int wd,             // fine res
    const int ht)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) collapse(2)
#endif
  for(int j=2;j<ht-2;j++)
    for(int i=2;i<wd-2;i++)
      fine[j*wd+i] = ll_expand_gaussian(input, i, j, wd, ht);
  ll_fill_boundary2(fine, wd, ht);
}

static inline void gauss_reduce(
    const float *const input, // fine input buffer
    float *const coarse,      // coarse scale, blurred input buf
    const int wd,             // fine res
    const int ht)
{
  // blur, store only coarse res
  const int cw = (wd-1)/2+1, ch = (ht-1)/2+1;

#if 0
  // TODO: this is the new hotspot after simd-fying the curve processing (14% this vs 7% curve)
  const float a = 0.4f;
  const float w[5] = {1./4.-a/2., 1./4., a, 1./4., 1./4.-a/2.};
  memset(coarse, 0, sizeof(float)*cw*ch);
  // direct 5x5 stencil only on required pixels:
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) collapse(2)
#endif
  for(int j=1;j<ch-1;j++) for(int i=1;i<cw-1;i++)
    for(int jj=-2;jj<=2;jj++) for(int ii=-2;ii<=2;ii++)
      coarse[j*cw+i] += input[(2*j+jj)*wd+2*i+ii] * w[ii+2] * w[jj+2];
#else
  // this version is inspired by opencv's pyrDown_ :
  // - allocate 5 rows of ring buffer (aligned)
  // - for coarse res y
  //   - fill 5 coarse-res row buffers with 1 4 6 4 1 weights (reuse some from last time)
  //   - do vertical convolution via sse and write to coarse output buf

  const int stride = ((cw+8)&~7); // assure sse alignment of rows
  float *ringbuf = dt_alloc_align(16, sizeof(*ringbuf)*stride*5);
  float *rows[5] = {0};
  int rowj = 0; // we initialised this many rows so far

  for(int j=1;j<ch-1;j++)
  {
    // horizontal pass, convolve with 1 4 6 4 1 kernel and decimate
    for(;rowj<=2*j+2;rowj++)
    {
      float *const row = ringbuf + (rowj % 5)*stride;
      const float *const in = input + rowj*wd;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
      for(int i=1;i<cw-1;i++)
        row[i] = 6*in[2*i] + 4*(in[2*i-1]+in[2*i+1]) + in[2*i-2] + in[2*i+2];
    }

    // init row pointers
    for(int k=0;k<5;k++)
      rows[k] = ringbuf + ((2*j-2+k)%5)*stride;

    // vertical pass, convolve and decimate using SIMD:
    // note that we're ignoring the (1..cw-1) buffer limit, we'll pull in
    // garbage and fix it later by border filling.
    float *const out = coarse + j*cw;
    const float *const row0 = rows[0], *const row1 = rows[1],
                *const row2 = rows[2], *const row3 = rows[3], *const row4 = rows[4];
    const __m128 four = _mm_set1_ps(4.f), scale = _mm_set1_ps(1.f/256.f);
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
    for(int i=0;i<=cw-8;i+=8)
    {
      __m128 r0, r1, r2, r3, r4, t0, t1;
      r0 = _mm_load_ps(row0 + i);
      r1 = _mm_load_ps(row1 + i);
      r2 = _mm_load_ps(row2 + i);
      r3 = _mm_load_ps(row3 + i);
      r4 = _mm_load_ps(row4 + i);
      r0 = _mm_add_ps(r0, r4);
      r1 = _mm_add_ps(_mm_add_ps(r1, r3), r2);
      r0 = _mm_add_ps(r0, _mm_add_ps(r2, r2));
      t0 = _mm_add_ps(r0, _mm_mul_ps(r1, four));

      r0 = _mm_load_ps(row0 + i + 4);
      r1 = _mm_load_ps(row1 + i + 4);
      r2 = _mm_load_ps(row2 + i + 4);
      r3 = _mm_load_ps(row3 + i + 4);
      r4 = _mm_load_ps(row4 + i + 4);
      r0 = _mm_add_ps(r0, r4);
      r1 = _mm_add_ps(_mm_add_ps(r1, r3), r2);
      r0 = _mm_add_ps(r0, _mm_add_ps(r2, r2));
      t1 = _mm_add_ps(r0, _mm_mul_ps(r1, four));

      t0 = _mm_mul_ps(t0, scale);
      t1 = _mm_mul_ps(t1, scale);

      _mm_storeu_ps(out + i, t0);
      _mm_storeu_ps(out + i + 4, t1);
    }
    // process the rest
    for(int i=cw&~7;i<cw-1;i++)
      out[i] = (6*row2[i] + 4*(row1[i] + row3[i]) + row0[i] + row4[i])*(1.0f/256.0f);
  }
  free(ringbuf);
#endif
  ll_fill_boundary1(coarse, cw, ch);
}

// allocate output buffer with monochrome brightness channel from input, padded
// up by max_supp on all four sides, dimensions written to wd2 ht2
static inline float *ll_pad_input(
    const float *const input,
    const int wd,
    const int ht,
    const int max_supp,
    int *wd2,
    int *ht2)
{
  const int stride = 4;
  *wd2 = 2*max_supp + wd;
  *ht2 = 2*max_supp + ht;
  float *out = (float *)dt_alloc_align(16, *wd2**ht2*sizeof(float));

  for(int j=0;j<ht;j++)
  {
    for(int i=0;i<max_supp;i++)
      // out[(j+max_supp)**wd2+i] = powf(MAX(0.0f, input[stride*wd*j+0]), 1./2.2);
      // out[(j+max_supp)**wd2+i] = rgb_to_y(input+stride*wd*j+0);
      out[(j+max_supp)**wd2+i] = input[stride*wd*j]* 0.01f; // L -> [0,1]
    for(int i=0;i<wd;i++)
      // out[(j+max_supp)**wd2+i+max_supp] = powf(MAX(0.0f, input[stride*(wd*j+i)+0]), 1./2.2);
      // out[(j+max_supp)**wd2+i+max_supp] = rgb_to_y(input+stride*(wd*j+i)+0);
      out[(j+max_supp)**wd2+i+max_supp] = input[stride*(wd*j+i)] * 0.01f; // L -> [0,1]
    for(int i=wd+max_supp;i<*wd2;i++)
      // out[(j+max_supp)**wd2+i] = powf(MAX(0.0f, input[stride*(j*wd+wd-1)+0]), 1./2.2);
      // out[(j+max_supp)**wd2+i] = rgb_to_y(input+stride*(j*wd+wd-1)+0);
      out[(j+max_supp)**wd2+i] = input[stride*(j*wd+wd-1)] * 0.01f; // L -> [0,1]
  }
  for(int j=0;j<max_supp;j++)
    memcpy(out + *wd2*j, out+max_supp**wd2, sizeof(float)**wd2);
  for(int j=max_supp+ht;j<*ht2;j++)
    memcpy(out + *wd2*j, out + *wd2*(max_supp+ht-1), sizeof(float)**wd2);

  return out;
}

static inline float ll_laplacian(
    const float *const coarse,   // coarse res gaussian
    const float *const fine,     // fine res gaussian
    const int i,                 // fine index
    const int j,
    const int wd,                // fine width
    const int ht)                // fine height
{
  const float c = ll_expand_gaussian(coarse, i, j, wd, ht);
  return fine[j*wd+i] - c;
}

#if 0
// guess what, this does nothing:
// #pragma omp declare simd notinbranch
static inline float ll_curve(
    const float x,
    const float g,
    const float sigma,
    const float shadows,
    const float highlights,
    const float clarity)
{
  // this is in the original matlab code instead:
  // I_remap=fact*(I-ref).*exp(-(I-ref).*(I-ref)./(2*sigma*sigma));
  // also they add up the laplacian to the ones of the base image
  // return g + 0.7 *  (x-g) + clarity * (x - g) * expf(-(x-g)*(x-g)/(2.0*sigma*sigma));

#if 0
  // XXX highlights does weird things, causing halos in dark regions
  // XXX shadows seems to compress highlight contrast (???)
  // XXX need to adjust this to bias shad/hi depending on g!
  // const float compress = .2;// + powf(g, .4);
  if(x > g+sigma)
    return g + shadows * (x-g);// + clarity * (x - g) * expf(-(x-g)*(x-g)/(2.0*sigma*sigma));
  //   return g+sigma + highlights * (x-g-sigma) + clarity * (x - g) * expf(-(x-g)*(x-g)/(2.0*sigma*sigma));
  if(x < g-sigma)
    return g + highlights * (x-g);// + clarity * (x - g) * expf(-(x-g)*(x-g)/(2.0*sigma*sigma));
  //   return g-sigma + shadows * (x-g+sigma) + clarity * (x - g) * expf(-(x-g)*(x-g)/(2.0*sigma*sigma));
  // else
    return g + (x-g) + clarity * (x - g) * dt_fast_expf(-(x-g)*(x-g)/(2.0*sigma*sigma));
#endif
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
  if(c >  sigma) return g + sigma + shadows    * (c-sigma);
  if(c < -sigma) return g - sigma + highlights * (c+sigma);
  // const float beta = 0.1;//(1.0-x)*(1.0-x);
  // if(c >  sigma) return g + sigma + beta * (c-sigma);
  // if(c < -sigma) return g - sigma + beta * (c+sigma);

  // TODO: paper says to blend this in to avoid boosting noise:
  // t = smoothstep(x, 0.01, 0.02)
  const float t0 = MIN(MAX(0.0f, (x-0.01)/(0.02-0.01)), 1.0f);
  const float t = t0*t0*(3.0f-2.0f*t0);
  assert(t == t);
  const float delta = fabsf(c)/sigma;
  assert(delta == delta);
  const float f_d = t * powf(delta, 1.-clarity) + (1.0f-t)*delta;
  assert(f_d == f_d);
  assert(g + copysignf(sigma * f_d, c) == g + copysignf(sigma * f_d, c));
  return g + copysignf(sigma * f_d, c);
#endif
#if 0
  const float c = x-g;
  if(c >  sigma) return g + sigma + shadows    * (c-sigma);
  if(c < -sigma) return g - sigma + highlights * (c+sigma);

  const float norm = 1.0 + clarity * dt_fast_expf(-.5f);
  // paper says to blend this in to avoid boosting noise:
  // t = smoothstep(x, 0.01, 0.02)
  const float t0 = fminf(fmaxf(0.0f, (x-0.01)/(0.02-0.01)), 1.0f);
  const float t = t0*t0*(3.0f-2.0f*t0);
  assert(t == t);
  const float delta = fabsf(c)/sigma;
  assert(delta == delta);
  // const float enh = powf(delta, 1./clarity);
  const float enh = (delta + clarity * delta * dt_fast_expf(-delta*delta/2.0))/norm;
  const float f_d = t * enh + (1.0f-t)*delta;
  assert(f_d == f_d);
  assert(g + copysignf(sigma * f_d, c) == g + copysignf(sigma * f_d, c));
  return g + copysignf(sigma * f_d, c);
#endif
#if 1
  float val = g + (x-g) + clarity * (x - g) * dt_fast_expf(-(x-g)*(x-g)/(2.0*sigma*sigma));

  const float off = sigma; // 2.0f*sigma
  float b0 = CLAMPS((x-g-off)/sigma, 0.0f, 1.0f);
  float lin0 = g + off + shadows * (x-g-off);
  val = val * (1.0f-b0) + b0 * lin0;

  float b1 = CLAMPS(-(x-g+off)/sigma, 0.0f, 1.0f);
  float lin1 = g - off + highlights * (x-g+off);
  val = val * (1.0f-b1) + b1 * lin1;

  const float t0 = CLAMPS((fabsf(x-g)-0.01)/(0.02-0.01), 0.0f, 1.0f);
  const float t = t0*t0*(3.0f-2.0f*t0);
  val = val * t + x * (1.0f - t);
  return val;
#endif
}
#endif

void apply_curve(
    float *const out,
    const float *const in,
    const uint32_t w,
    const uint32_t h,
    const float gamma,
    const float sigma,
    const float shadows,
    const float highlights,
    const float clarity)
{
  // TODO: do all this in avx2 8-wide (should be straight forward):
  const __m128 g4     = _mm_set_ps1(gamma);
  const __m128 s4     = _mm_set_ps1(sigma);
  const __m128 s22    = _mm_set_ps1(2.0f*sigma*sigma);
  const __m128 sh4    = _mm_set_ps1(shadows);
  const __m128 hi4    = _mm_set_ps1(highlights);
  const __m128 cl4    = _mm_set_ps1(clarity);
  const __m128 const0 = _mm_set_ps1(0x3f800000u);
  const __m128 const1 = _mm_set_ps1(0x402DF854u); // for e^x
  const __m128 sign_mask = _mm_set1_ps(-0.f); // -0.f = 1 << 31

  // TODO: this actually processes a lot of padding, too! this could be copied
  // with boundary filling instead.
#pragma omp parallel for default(none) schedule(dynamic)
  for(size_t j=0;j<(size_t)w*h;j+=4)
  {
    __m128 val;
    const __m128 input = _mm_load_ps(in+j);
    const __m128 c = _mm_sub_ps(input, g4); // centered value x - g
    { // clarity
      const __m128 arg = _mm_xor_ps(sign_mask, _mm_div_ps(_mm_mul_ps(c, c), s22));
      const __m128 k0 = _mm_add_ps(const0, _mm_mul_ps(arg, _mm_sub_ps(const1, const0)));
      const __m128 k = _mm_max_ps(k0, _mm_setzero_ps());
      const __m128i ki = _mm_cvtps_epi32(k);
      const __m128 gauss = _mm_load_ps((float*)&ki);
      val = _mm_add_ps(g4, _mm_add_ps(c, _mm_mul_ps(cl4, _mm_mul_ps(c, gauss))));
    }
    // const __m128 twosig = _mm_mul_ps(_mm_set1_ps(2.0f), s4);
    const __m128 twosig = s4; // drag shad/hi earlier, more powerful
    { // shadows
      const __m128 f0 = _mm_sub_ps(c, twosig);
      const __m128 b0 = _mm_min_ps(_mm_set1_ps(1.0f), _mm_max_ps(_mm_setzero_ps(),
            _mm_div_ps(f0, s4)));
      const __m128 lin0 = _mm_add_ps(g4, _mm_add_ps(twosig, _mm_mul_ps(sh4, f0)));
      val = _mm_add_ps(_mm_mul_ps(val, _mm_sub_ps(_mm_set1_ps(1.0f), b0)), _mm_mul_ps(lin0, b0));
    }
    { // highlights
      const __m128 f1 = _mm_add_ps(c, twosig);
      const __m128 b1 =  _mm_min_ps(_mm_set1_ps(1.0f), _mm_max_ps(_mm_setzero_ps(),
            _mm_xor_ps(sign_mask, _mm_div_ps(f1, s4)) // *-1
            ));
      const __m128 lin1 = _mm_add_ps(_mm_sub_ps(g4, twosig), _mm_mul_ps(hi4, f1));
      val = _mm_add_ps(_mm_mul_ps(val, _mm_sub_ps(_mm_set1_ps(1.0f), b1)), _mm_mul_ps(lin1, b1));
    }
#if 1 // not sure this is actually needed/useful:
    { // noise
      const __m128 t0 = _mm_min_ps(_mm_set1_ps(1.0f), _mm_max_ps(_mm_setzero_ps(),
            _mm_div_ps(_mm_sub_ps(
                _mm_andnot_ps(sign_mask, c), // abs(c)
                _mm_set1_ps(0.01f)),
              _mm_sub_ps(_mm_set1_ps(0.02f), _mm_set1_ps(0.01f)))));
      const __m128 t = _mm_mul_ps(_mm_mul_ps(t0, t0),
          _mm_sub_ps(_mm_set1_ps(3.0f), _mm_mul_ps(_mm_set1_ps(2.0f), t0)));
      val = _mm_add_ps(_mm_mul_ps(input, _mm_sub_ps(_mm_set1_ps(1.0f), t)), _mm_mul_ps(val, t));

    }
#endif
    _mm_stream_ps(out + j, val);
  }
  // TODO: process last 0-3 elements if not divisible by simd width=4
}

void local_laplacian(
    const float *const input,   // input buffer in some Labx or yuvx format
    float *const out,           // output buffer with colour
    const int wd,               // width and
    const int ht,               // height of the input buffer
    const float sigma,          // user param: separate shadows/midtones/highlights
    const float shadows,        // user param: lift shadows
    const float highlights,     // user param: compress highlights
    const float clarity)        // user param: increase clarity/local contrast
{
  // XXX TODO: the paper says level 5 is good enough, too? more does look significantly different.
#define num_levels 10
#define num_gamma 8
  const int max_supp = 1<<(num_levels-1);
  int w, h;
  float *padded[num_levels] = {0};
  padded[0] = ll_pad_input(input, wd, ht, max_supp, &w, &h);

  // allocate pyramid pointers for padded input
  for(int l=1;l<num_levels;l++)
    padded[l] = (float *)dt_alloc_align(16, sizeof(float)*dl(w,l)*dl(h,l));

  // allocate pyramid pointers for output
  float *output[num_levels] = {0};
  for(int l=0;l<num_levels;l++)
    output[l] = (float *)dt_alloc_align(16, sizeof(float)*dl(w,l)*dl(h,l));

  // create gauss pyramid of padded input, write coarse directly to output
  for(int l=1;l<num_levels-1;l++)
    gauss_reduce(padded[l-1], padded[l], dl(w,l-1), dl(h,l-1));
  gauss_reduce(padded[num_levels-2], output[num_levels-1], dl(w,num_levels-2), dl(h,num_levels-2));

  // evenly sample brightness [0,1]:
  float gamma[num_gamma] = {0.0f};
  for(int k=0;k<num_gamma;k++) gamma[k] = (k+.5f)/(float)num_gamma;
  // for(int k=0;k<num_gamma;k++) gamma[k] = k/(num_gamma-1.0f);

  // XXX FIXME: don't need to alloc all the memory at once!
  // XXX FIXME: accumulate into output pyramid one by one?
  // allocate memory for intermediate laplacian pyramids
  float *buf[num_gamma][num_levels] = {{0}};
  for(int k=0;k<num_gamma;k++) for(int l=0;l<num_levels;l++)
    buf[k][l] = (float *)dt_alloc_align(16, sizeof(float)*dl(w,l)*dl(h,l));

  // XXX TODO: the paper says remapping only level 3 not 0 does the trick, too:
  for(int k=0;k<num_gamma;k++)
  { // process images
#if 0
#pragma omp parallel for simd default(none) schedule(dynamic) shared(w,h,k,buf,gamma,padded)
    for(size_t j=0;j<(size_t)w*h;j++)
      buf[k][0][j] = ll_curve(padded[0][j], gamma[k], sigma, shadows, highlights, clarity);
#else
    apply_curve(
        buf[k][0], padded[0], w, h, gamma[k], sigma, shadows, highlights, clarity);
#endif

    // create gaussian pyramids
    for(int l=1;l<num_levels;l++)
      gauss_reduce(buf[k][l-1], buf[k][l], dl(w,l-1), dl(h,l-1));
  }

  // assemble output pyramid coarse to fine
  for(int l=num_levels-2;l >= 0; l--)
  {
    const int pw = dl(w,l), ph = dl(h,l);

    gauss_expand(output[l+1], output[l], pw, ph);
    // go through all coefficients in the upsampled gauss buffer:
#pragma omp parallel for default(none) schedule(static) collapse(2) shared(w,h,buf,output,l,gamma,padded)
    for(int j=2;j<ph-2;j++) for(int i=2;i<pw-2;i++)
    {
      const float v = padded[l][j*pw+i];
      int hi = 1;
      for(;hi<num_gamma-1 && gamma[hi] <= v;hi++);
      int lo = hi-1;
      const float a = MIN(MAX((v - gamma[lo])/(gamma[hi]-gamma[lo]), 0.0f), 1.0f);
      const float l0 = ll_laplacian(buf[lo][l+1], buf[lo][l], i, j, pw, ph);
      const float l1 = ll_laplacian(buf[hi][l+1], buf[hi][l], i, j, pw, ph);
      assert(a == a);
      assert(l0 == l0);
      assert(l1 == l1);
      float laplace = l0 * (1.0f-a) + l1 * a;
      output[l][j*pw+i] += laplace;
    }
    ll_fill_boundary2(output[l], pw, ph);
  }
#pragma omp parallel for default(none) schedule(dynamic) collapse(2) shared(w,output,buf)
  for(int j=0;j<ht;j++) for(int i=0;i<wd;i++)
  {
    out[4*(j*wd+i)+0] = 100.0f * output[0][(j+max_supp)*w+max_supp+i]; // [0,1] -> L
    out[4*(j*wd+i)+1] = input[4*(j*wd+i)+1]; // copy original colour channels
    out[4*(j*wd+i)+2] = input[4*(j*wd+i)+2];
  }
  // free all buffers!
  for(int l=0;l<num_levels;l++)
  {
    free(padded[l]);
    free(output[l]);
    for(int k=0;k<num_gamma;k++)
      free(buf[k][l]);
  }
#undef num_levels
#undef num_gamma
}

