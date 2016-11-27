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
  // float c = 0.0f;
  const int cw = (wd-1)/2+1;
  // TODO: manually expand to sums:
  // const float w[5] = {1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f};
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
      return 4./256. * (
          6.0f*(coarse[ind-cw] + coarse[ind-1] + 6.0f*coarse[ind] + coarse[ind+1] + coarse[ind+cw])
          + coarse[ind-cw-1] + coarse[ind-cw+1] + coarse[ind+cw-1] + coarse[ind+cw+1]);
      //for(int ii=-1;ii<=1;ii++)
      //  for(int jj=-1;jj<=1;jj++)
      //    c += coarse[ind+cw*jj+ii]*w[2*jj+2]*w[2*ii+2];
      //break;
    case 1: // i is odd, 2x3 stencil
      return 4./256. * (
          24.0*(coarse[ind] + coarse[ind+1]) +
          4.0*(coarse[ind-cw] + coarse[ind-cw+1] + coarse[ind+cw] + coarse[ind+cw+1]));
      // for(int ii=0;ii<=1;ii++)
      //   for(int jj=-1;jj<=1;jj++)
      //     c += coarse[ind+cw*jj+ii]*w[2*jj+2]*w[2*ii+1];
      // break;
    case 2: // j is odd, 3x2 stencil
      return 4./256. * (
          24.0*(coarse[ind] + coarse[ind+cw]) +
          4.0*(coarse[ind-1] + coarse[ind+1] + coarse[ind+cw-1] + coarse[ind+cw+1]));
      // for(int ii=-1;ii<=1;ii++)
      //   for(int jj=0;jj<=1;jj++)
      //     c += coarse[ind+cw*jj+ii]*w[2*jj+1]*w[2*ii+2];
      // break;
    default: // case 3: // both are odd, 2x2 stencil
      return .25f * (coarse[ind] + coarse[ind+1] + coarse[ind+cw] + coarse[ind+cw+1]);
      // for(int ii=0;ii<=1;ii++)
      //   for(int jj=0;jj<=1;jj++)
      //     c += coarse[ind+cw*jj+ii]*w[2*jj+1]*w[2*ii+1];
      // break;
  }
  // return 4.0f * c;
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
  float *const out = dt_alloc_align(16, *wd2**ht2*sizeof(*out));

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) default(none) shared(wd2, ht2)
#endif
  for(int j=0;j<ht;j++)
  {
    for(int i=0;i<max_supp;i++)
      out[(j+max_supp)**wd2+i] = input[stride*wd*j]* 0.01f; // L -> [0,1]
    for(int i=0;i<wd;i++)
      out[(j+max_supp)**wd2+i+max_supp] = input[stride*(wd*j+i)] * 0.01f; // L -> [0,1]
    for(int i=wd+max_supp;i<*wd2;i++)
      out[(j+max_supp)**wd2+i] = input[stride*(j*wd+wd-1)] * 0.01f; // L -> [0,1]
  }
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) default(none) shared(wd2, ht2)
#endif
  for(int j=0;j<max_supp;j++)
    memcpy(out + *wd2*j, out+max_supp**wd2, sizeof(float)**wd2);
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) default(none) shared(wd2, ht2)
#endif
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

static inline float curve_scalar(
    const float x,
    const float g,
    const float sigma,
    const float shadows,
    const float highlights,
    const float clarity)
{
  const float c = x-g;
  float val;
  // blend in via quadratic bezier
  if     (c >  2*sigma) val = g + sigma + shadows    * (c-sigma);
  else if(c < -2*sigma) val = g - sigma + highlights * (c+sigma);
  else if(c > 0.0f)
  { // shadow contrast
    const float t = CLAMPS(c / (2.0f*sigma), 0.0f, 1.0f);
    const float t2 = t * t;
    const float mt = 1.0f-t;
    val = g + sigma * 2.0f*mt*t + t2*(sigma + sigma*shadows);
  }
  else
  { // highlight contrast
    const float t = CLAMPS(-c / (2.0f*sigma), 0.0f, 1.0f);
    const float t2 = t * t;
    const float mt = 1.0f-t;
    val = g - sigma * 2.0f*mt*t + t2*(- sigma - sigma*highlights);
  }
  // midtone local contrast
  val += clarity * c * dt_fast_expf(-c*c/(2.0*sigma*sigma/3.0f));
  return val;
}

static inline __m128 curve_vec4(
    const __m128 x,
    const __m128 g,
    const __m128 sigma,
    const __m128 shadows,
    const __m128 highlights,
    const __m128 clarity)
{
  // TODO: pull these non-data depedent constants out of the loop to see
  // whether the compiler fail to do so
  const __m128 const0 = _mm_set_ps1(0x3f800000u);
  const __m128 const1 = _mm_set_ps1(0x402DF854u); // for e^x
  const __m128 sign_mask = _mm_set1_ps(-0.f); // -0.f = 1 << 31
  const __m128 one = _mm_set1_ps(1.0f);
  const __m128 two = _mm_set1_ps(2.0f);
  const __m128 twothirds = _mm_set1_ps(2.0f/3.0f);
  const __m128 twosig = _mm_mul_ps(two, sigma);
  const __m128 sigma2 = _mm_mul_ps(sigma, sigma);
  const __m128 s22 = _mm_mul_ps(twothirds, sigma2);

  const __m128 c = _mm_sub_ps(x, g);
  const __m128 select = _mm_cmplt_ps(c, _mm_setzero_ps());
  // select shadows or highlights as multiplier for linear part, based on c < 0
  const __m128 shadhi = _mm_or_ps(_mm_andnot_ps(select, shadows), _mm_and_ps(select, highlights));
  // flip sign bit of sigma based on c < 0 (c < 0 ? - sigma : sigma)
  const __m128 ssigma = _mm_xor_ps(sigma, _mm_and_ps(select, sign_mask));
  // this contains the linear parts valid for c > 2*sigma or c < - 2*sigma
  const __m128 vlin = _mm_add_ps(g, _mm_add_ps(ssigma, _mm_mul_ps(shadhi, _mm_sub_ps(c, ssigma))));

  const __m128 t = _mm_min_ps(one, _mm_max_ps(_mm_setzero_ps(),
        _mm_div_ps(c, _mm_mul_ps(two, ssigma))));
  const __m128 t2 = _mm_mul_ps(t, t);
  const __m128 mt = _mm_sub_ps(one, t);

  // midtone value fading over to linear part, without local contrast:
  const __m128 vmid = _mm_add_ps(g,
      _mm_add_ps(_mm_mul_ps(_mm_mul_ps(ssigma, two), _mm_mul_ps(mt, t)),
        _mm_mul_ps(t2, _mm_add_ps(ssigma, _mm_mul_ps(ssigma, shadhi)))));

  // c > 2*sigma?
  const __m128 linselect = _mm_cmpgt_ps(_mm_andnot_ps(sign_mask, c), twosig);
  const __m128 val = _mm_or_ps(_mm_and_ps(linselect, vlin), _mm_andnot_ps(linselect, vmid));

  // midtone local contrast
  // dt_fast_expf in sse:
  const __m128 arg = _mm_xor_ps(sign_mask, _mm_div_ps(_mm_mul_ps(c, c), s22));
  const __m128 k0 = _mm_add_ps(const0, _mm_mul_ps(arg, _mm_sub_ps(const1, const0)));
  const __m128 k = _mm_max_ps(k0, _mm_setzero_ps());
  const __m128i ki = _mm_cvtps_epi32(k);
  const __m128 gauss = _mm_load_ps((float*)&ki);
  const __m128 vcon = _mm_mul_ps(clarity, _mm_mul_ps(c, gauss));
  return _mm_add_ps(val, vcon);
}

#if 0
// scalar version
void apply_curve(
    float *const out,
    const float *const in,
    const uint32_t w,
    const uint32_t h,
    const uint32_t padding,
    const float g,
    const float sigma,
    const float shadows,
    const float highlights,
    const float clarity)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(dynamic)
#endif
  for(uint32_t j=padding;j<h-padding;j++)
  {
    const float *in2  = in  + j*w + padding;
    float *out2 = out + j*w + padding;
    for(uint32_t i=padding;i<w-padding;i++)
      (*out2++) = curve_scalar(*(in2++), g, sigma, shadows, highlights, clarity);
    out2 = out + j*w;
    for(int i=0;i<padding;i++)   out2[i] = out2[padding];
    for(int i=w-padding;i<w;i++) out2[i] = out2[w-padding-1];
  }
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(dynamic)
#endif
  for(int j=0;j<padding;j++) memcpy(out + w*j, out+padding*w, sizeof(float)*w);
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(dynamic)
#endif
  for(int j=h-padding;j<h;j++) memcpy(out + w*j, out+w*(h-padding-1), sizeof(float)*w);
}
#else
// sse (4-wide)
void apply_curve(
    float *const out,
    const float *const in,
    const uint32_t w,
    const uint32_t h,
    const uint32_t padding,
    const float g,
    const float sigma,
    const float shadows,
    const float highlights,
    const float clarity)
{
  // TODO: do all this in avx2 8-wide (should be straight forward):
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(dynamic)
#endif
  for(uint32_t j=padding;j<h-padding;j++)
  {
    const float *in2  = in  + j*w + padding;
    float *out2 = out + j*w + padding;
    // find 4-byte aligned block in the middle:
    const float *const beg = (float *)((size_t)(out2+3)&(size_t)0x10ul);
    const float *const end = (float *)((size_t)(out2+w-padding)&(size_t)0x10ul);
    const float *const fin = out2+w-padding;
    const __m128 g4 = _mm_set1_ps(g);
    const __m128 sig4 = _mm_set1_ps(sigma);
    const __m128 shd4 = _mm_set1_ps(shadows);
    const __m128 hil4 = _mm_set1_ps(highlights);
    const __m128 clr4 = _mm_set1_ps(clarity);
    for(;out2<beg;out2++,in2++)
      *out2 = curve_scalar(*in2, g, sigma, shadows, highlights, clarity);
    for(;out2<end;out2+=4,in2+=4)
      _mm_stream_ps(out2, curve_vec4(_mm_load_ps(in2), g4, sig4, shd4, hil4, clr4));
    for(;out2<fin;out2++,in2++)
      *out2 = curve_scalar(*in2, g, sigma, shadows, highlights, clarity);
    out2 = out + j*w;
    for(int i=0;i<padding;i++)   out2[i] = out2[padding];
    for(int i=w-padding;i<w;i++) out2[i] = out2[w-padding-1];
  }
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(dynamic)
#endif
  for(int j=0;j<padding;j++) memcpy(out + w*j, out+padding*w, sizeof(float)*w);
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(dynamic)
#endif
  for(int j=h-padding;j<h;j++) memcpy(out + w*j, out+w*(h-padding-1), sizeof(float)*w);
}
#endif

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
    padded[l] = dt_alloc_align(16, sizeof(float)*dl(w,l)*dl(h,l));

  // allocate pyramid pointers for output
  float *output[num_levels] = {0};
  for(int l=0;l<num_levels;l++)
    output[l] = dt_alloc_align(16, sizeof(float)*dl(w,l)*dl(h,l));

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
    buf[k][l] = dt_alloc_align(16, sizeof(float)*dl(w,l)*dl(h,l));

  // XXX TODO: the paper says remapping only level 3 not 0 does the trick, too:
  for(int k=0;k<num_gamma;k++)
  { // process images
#if 0
#pragma omp parallel for simd default(none) schedule(dynamic) shared(w,h,k,buf,gamma,padded)
    for(size_t j=0;j<(size_t)w*h;j++)
      buf[k][0][j] = ll_curve(padded[0][j], gamma[k], sigma, shadows, highlights, clarity);
#else
    apply_curve(
        buf[k][0], padded[0], w, h, max_supp, gamma[k], sigma, shadows, highlights, clarity);
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
      const float a = CLAMPS((v - gamma[lo])/(gamma[hi]-gamma[lo]), 0.0f, 1.0f);
      const float l0 = ll_laplacian(buf[lo][l+1], buf[lo][l], i, j, pw, ph);
      const float l1 = ll_laplacian(buf[hi][l+1], buf[hi][l], i, j, pw, ph);
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

