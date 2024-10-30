/*
    This file is part of darktable,
    Copyright (C) 2016-2024 darktable developers.

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
#include "common/math.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

// the maximum number of levels for the gaussian pyramid
#define max_levels 30
// the number of segments for the piecewise linear interpolation
#define num_gamma 6

// downsample width/height to given level
static inline int dl(int size, const int level)
{
  for(int l=0;l<level;l++)
    size = (size-1)/2+1;
  return size;
}

// needs a boundary of 1 or 2px around i,j or else it will crash.
// (translates to a 1px boundary around the corresponding pixel in the coarse buffer)
// more precisely, 1<=i<wd-1 for even wd and
//                 1<=i<wd-2 for odd wd (j likewise with ht)
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
  const int cw = (wd-1)/2+1;
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
    case 1: // i is odd, 2x3 stencil
      return 4./256. * (
          24.0*(coarse[ind] + coarse[ind+1]) +
          4.0*(coarse[ind-cw] + coarse[ind-cw+1] + coarse[ind+cw] + coarse[ind+cw+1]));
    case 2: // j is odd, 3x2 stencil
      return 4./256. * (
          24.0*(coarse[ind] + coarse[ind+cw]) +
          4.0*(coarse[ind-1] + coarse[ind+1] + coarse[ind+cw-1] + coarse[ind+cw+1]));
    default: // case 3: // both are odd, 2x2 stencil
      return .25f * (coarse[ind] + coarse[ind+1] + coarse[ind+cw] + coarse[ind+cw+1]);
  }
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

// helper to fill in two pixels boundary by copying it
static inline void ll_fill_boundary2(
    float *const input,
    const int wd,
    const int ht)
{
  for(int j=1;j<ht-1;j++) input[j*wd] = input[j*wd+1];
  if(wd & 1) for(int j=1;j<ht-1;j++) input[j*wd+wd-1] = input[j*wd+wd-2];
  else       for(int j=1;j<ht-1;j++) input[j*wd+wd-1] = input[j*wd+wd-2] = input[j*wd+wd-3];
  memcpy(input, input+wd, sizeof(float)*wd);
  if(!(ht & 1)) memcpy(input+wd*(ht-2), input+wd*(ht-3), sizeof(float)*wd);
  memcpy(input+wd*(ht-1), input+wd*(ht-2), sizeof(float)*wd);
}

static void pad_by_replication(
    float *buf,			// the buffer to be padded
    const uint32_t w,		// width of a line
    const uint32_t h,		// total height, including top and bottom padding
    const uint32_t padding)	// number of lines of padding on each side
{
  DT_OMP_FOR()
  for(int j=0;j<padding;j++)
  {
    memcpy(buf + w*j, buf+padding*w, sizeof(float)*w);
    memcpy(buf + w*(h-padding+j), buf+w*(h-padding-1), sizeof(float)*w);
  }
}

static inline void gauss_expand(
    const float *const input, // coarse input
    float *const fine,        // upsampled, blurry output
    const int wd,             // fine res
    const int ht)
{
  DT_OMP_FOR(collapse(2))
  for(int j=1;j<((ht-1)&~1);j++)  // even ht: two px boundary. odd ht: one px.
    for(int i=1;i<((wd-1)&~1);i++)
      fine[j*wd+i] = ll_expand_gaussian(input, i, j, wd, ht);
  ll_fill_boundary2(fine, wd, ht);
}

static inline void _convolve_14641_vert(dt_aligned_pixel_t conv, const float *in, const size_t wd)
{
  static const dt_aligned_pixel_t four = { 4.f, 4.f, 4.f, 4.f };
  dt_aligned_pixel_t r0, r1, r2, r3, r4;
  for_four_channels(c)
  {
    // 'in' is only 4-byte aligned, so we can't use copy_pixel here
    r0[c] = in[c];
    r1[c] = in[wd+c];
    r2[c] = in[2*wd+c];
    r3[c] = in[3*wd+c];
    r4[c] = in[4*wd+c];
  }
  dt_aligned_pixel_t t;
  for_four_channels(c)
  {
    r0[c] = r0[c] + r4[c];		// r0 = r0+r4
    r1[c] = r1[c] + r2[c] + r3[c];	// r1 = r1+r2+r2
    r0[c] = r0[c] + r2[c] + r2[c];	// r0 = r0 + 2*r2 * r4
    t[c] = r1[c] * four[c];		// t = 4*r1 + 4*r2 + r*43
    conv[c] = r0[c] + t[c];		// conv = r0 + 4*r1 + 6*r2 + 4*r3 + r4
  }
}

static inline void gauss_reduce(
    const float *const input, // fine input buffer
    float *const coarse,      // coarse scale, blurred input buf
    const size_t wd,             // fine res
    const size_t ht)
{
  // blur, store only coarse res
  const size_t cw = (wd-1)/2+1, ch = (ht-1)/2+1;
  // DON'T parallelize the very smallest levels of the pyramid, as the threading overhead
  // is greater than the time needed to do it sequentially
  DT_OMP_FOR(if(ch*cw>2000))
  for(size_t j=1;j<ch-1;j++)
  {
    const float *base = input + 2*(j-1)*wd;
    float *const out = coarse + j*cw + 1;
    // prime the vertical axis
    static const dt_aligned_pixel_t kernel = { 1.0f, 4.0f, 6.0f, 4.0f };
    dt_aligned_pixel_t left;
    _convolve_14641_vert(left,base,wd);
    for(size_t col=0; col<cw-3; col += 2)
    {
      // convolve the next four pixel wide vertical slice
      base += 4;
      dt_aligned_pixel_t right;
      _convolve_14641_vert(right,base,wd);
      // horizontal pass, generate two output values from convolving with 1 4 6 4 1
      // the first uses pixels 0-4, the second uses 2-6
      dt_aligned_pixel_t conv;
      for_four_channels(c)
        conv[c] = left[c] * kernel[c];
      out[col] = (conv[0] + conv[1] + conv[2] + conv[3] + right[0]) / 256.0f;
      out[col+1] = (left[2] + 4*(left[3]+right[1]) + 6.0f*right[0] + right[2]) / 256.0f;
      // shift to next pair of output columns (four input columns)
      copy_pixel(left, right);
    }
    // handle the left-over pixel if the output size is odd
    if(cw % 2)
    {
      base += 4;
      // convolve the right-most column
      float right = base[0] + 4.0f*(base[wd]+base[3*wd]) + 6.0f*base[2*wd] + base[4*wd];
      dt_aligned_pixel_t conv;
      for_four_channels(c)
        conv[c] = left[c] * kernel[c];
      out[cw-3] = (conv[0] + conv[1] + conv[2] + conv[3] + right) / 256.0f;
    }
  }
  dt_omploop_sfence();
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
    int *ht2,
    local_laplacian_boundary_t *b)
{
  const int stride = 4;
  *wd2 = 2*max_supp + wd;
  *ht2 = 2*max_supp + ht;
  float *const out = dt_alloc_align_float((size_t) *wd2 * *ht2);

  if(b && b->mode == 2)
  { // pad by preview buffer
    // fill regular pixels:
    DT_OMP_FOR(collapse(2))
    for(int j=0;j<ht;j++) for(int i=0;i<wd;i++)
      out[(j+max_supp)**wd2+i+max_supp] = input[stride*(wd*j+i)] * 0.01f; // L -> [0,1]

    // for all out of roi pixels on the boundary we wish to pad:
    // compute coordinate in full image.
    // if not out of buf:
    //   compute padded preview pixel coordinate (clamp to padded preview buffer size)
    // else
    //   pad as usual (hi-res sample and hold)
#define LL_FILL(fallback) do {\
    float isx = ((i - max_supp) + b->roi->x)/b->roi->scale;\
    float isy = ((j - max_supp) + b->roi->y)/b->roi->scale;\
    if(isx < 0 || isy >= b->buf->width\
    || isy < 0 || isy >= b->buf->height)\
      out[*wd2*j+i] = (fallback);\
    else\
    {\
      int px = CLAMP(isx / (float)b->buf->width  * b->wd + (b->pwd-b->wd)/2, 0, b->pwd-1);\
      int py = CLAMP(isy / (float)b->buf->height * b->ht + (b->pht-b->ht)/2, 0, b->pht-1);\
      /* TODO: linear interpolation?*/\
      out[*wd2*j+i] = b->pad0[b->pwd*py+px];\
    } } while(0)
    // left border
    DT_OMP_FOR(collapse(2))
    for(int j=max_supp;j<*ht2-max_supp;j++)
      for(int i=0;i<max_supp;i++)
        LL_FILL(input[stride*wd*(j-max_supp)]* 0.01f);
    // right border
    DT_OMP_FOR(collapse(2))
    for(int j=max_supp;j<*ht2-max_supp;j++)
      for(int i=wd+max_supp;i<*wd2;i++)
        LL_FILL(input[stride*((j-max_supp)*wd+wd-1)] * 0.01f);
    // top border
    DT_OMP_FOR(collapse(2))
    for(int j=0;j<max_supp;j++)
      for(int i=0;i<*wd2;i++)
        LL_FILL(out[*wd2*max_supp+i]);
    // bottom border
    DT_OMP_FOR(collapse(2))
    for(int j=max_supp+ht;j<*ht2;j++)
      for(int i=0;i<*wd2;i++)
        LL_FILL(out[*wd2*(max_supp+ht-1)+i]);
#undef LL_FILL
  }
  else
  { // pad by replication:
    DT_OMP_FOR()
    for(int j=0;j<ht;j++)
    {
      for(int i=0;i<max_supp;i++)
        out[(j+max_supp)**wd2+i] = input[stride*wd*j]* 0.01f; // L -> [0,1]
      for(int i=0;i<wd;i++)
        out[(j+max_supp)**wd2+i+max_supp] = input[stride*(wd*j+i)] * 0.01f; // L -> [0,1]
      for(int i=wd+max_supp;i<*wd2;i++)
        out[(j+max_supp)**wd2+i] = input[stride*(j*wd+wd-1)] * 0.01f; // L -> [0,1]
    }
    pad_by_replication(out, *wd2, *ht2, max_supp);
  }
  if((b && b->mode == 2) && (darktable.dump_pfm_module))
  {
    dt_dump_pfm("padded", out, *wd2, *ht2, 4 * sizeof(float), "locallaplacian");
  }
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
  const float c = ll_expand_gaussian(coarse,
      CLAMPS(i, 1, ((wd-1)&~1)-1), CLAMPS(j, 1, ((ht-1)&~1)-1), wd, ht);
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
  val += clarity * c * dt_fast_expf(-c*c/(2.0f*sigma*sigma/3.0f));
  return val;
}

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
  DT_OMP_FOR()
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
  pad_by_replication(out, w, h, padding);
}

void local_laplacian_internal(
    const float *const input,   // input buffer in some Labx or yuvx format
    float *const out,           // output buffer with colour
    const int wd,               // width and
    const int ht,               // height of the input buffer
    const float sigma,          // user param: separate shadows/mid-tones/highlights
    const float shadows,        // user param: lift shadows
    const float highlights,     // user param: compress highlights
    const float clarity,        // user param: increase clarity/local contrast
    local_laplacian_boundary_t *b)
{
  if(wd <= 1 || ht <= 1) return;

  // don't divide by 2 more often than we can:
  const int num_levels = MIN(max_levels, 31-__builtin_clz(MIN(wd,ht)));
  int last_level = num_levels-1;
  if(b && b->mode == 2) // higher number here makes it less prone to aliasing and slower.
    last_level = num_levels > 4 ? 4 : num_levels-1;
  const int max_supp = 1<<last_level;
  int w, h;
  float *padded[max_levels] = {0};
  if(b && b->mode == 2)
    padded[0] = ll_pad_input(input, wd, ht, max_supp, &w, &h, b);
  else
    padded[0] = ll_pad_input(input, wd, ht, max_supp, &w, &h, 0);

  // allocate pyramid pointers for padded input
  gboolean success = padded[0] != NULL;
  for(int l=1;l<=last_level;l++)
  {
    padded[l] = dt_alloc_align_float((size_t)dl(w,l) * dl(h,l));
    if (!padded[l])
    {
      success = FALSE;
      break;
    }
  }

  // allocate pyramid pointers for output
  float *output[max_levels] = {0};
  for(int l=0;l<=last_level;l++)
  {
    output[l] = dt_alloc_align_float((size_t)dl(w,l) * dl(h,l));
    if (!output[l])
    {
      success = FALSE;
      break;
    }
  }

  if(!success)
  {
    // we can't jump to cleanup from here because it would reference a
    // variable which hasn't been initialized yet because it is
    // declared below.  So just free whatever we've allocated and return.
    for(int l = 0; l <= last_level; l++)
    {
      dt_free_align(padded[l]);
      dt_free_align(output[l]);
    }
    // copy the input buffer to the output so that we at least get a
    // valid result
    for(size_t k = 0; k < (size_t)4 * wd * ht; k++)
      out[k] = input[k];
    return;
  }

  // create gauss pyramid of padded input, write coarse directly to output
  for(int l=1;l<last_level;l++)
    gauss_reduce(padded[l-1], padded[l], dl(w,l-1), dl(h,l-1));
  gauss_reduce(padded[last_level-1], output[last_level], dl(w,last_level-1), dl(h,last_level-1));

  // evenly sample brightness [0,1]:
  float gamma[num_gamma] = {0.0f};
  for(int k=0;k<num_gamma;k++) gamma[k] = (k+.5f)/(float)num_gamma;
  // for(int k=0;k<num_gamma;k++) gamma[k] = k/(num_gamma-1.0f);

  // allocate memory for intermediate laplacian pyramids
  float *buf[num_gamma][max_levels] = {{0}};
  for(int k=0;k<num_gamma;k++)
    for(int l=0;l<=last_level;l++)
    {
      buf[k][l] = dt_alloc_align_float((size_t)dl(w,l)*dl(h,l));
      if(!buf[k][l])
      {
        // copy the input buffer to the output so that we at least get a
        // valid result
        for(size_t p = 0; p < (size_t)4 * wd * ht; p++)
          out[p] = input[p];
        goto cleanup;
      }
    }

  // the paper says remapping only level 3 not 0 does the trick, too
  // (but i really like the additional octave of sharpness we get,
  // willing to pay the cost).
  for(int k=0;k<num_gamma;k++)
  { // process images
    apply_curve(buf[k][0], padded[0], w, h, max_supp, gamma[k], sigma, shadows, highlights, clarity);

    // create gaussian pyramids
    for(int l=1;l<=last_level;l++)
      gauss_reduce(buf[k][l-1], buf[k][l], dl(w,l-1), dl(h,l-1));
  }

  // resample output[last_level] from preview
  // requires to transform from padded/downsampled to full image and then
  // to padded/downsampled in preview
  if(b && b->mode == 2)
  {
    const float isize = powf(2.0f, last_level) / b->roi->scale; // pixel size of coarsest level in image space
    const float psize = isize / b->buf->width * b->wd; // pixel footprint rescaled to preview buffer
    const float pl = log2f(psize); // mip level in preview buffer
    const int pl0 = CLAMP((int)pl, 0, b->num_levels-1), pl1 = CLAMP((int)(pl+1), 0, b->num_levels-1);
    const float weight = CLAMP(pl-pl0, 0, 1); // weight between mip levels
    const float mul0 = 1.0/powf(2.0f, pl0);
    const float mul1 = 1.0/powf(2.0f, pl1);
    const float mul = powf(2.0f, last_level);
    const int pw = dl(w,last_level), ph = dl(h,last_level);
    const int pw0 = dl(b->pwd, pl0), ph0 = dl(b->pht, pl0);
    const int pw1 = dl(b->pwd, pl1), ph1 = dl(b->pht, pl1);
    if(darktable.dump_pfm_module)
    {
      dt_dump_pfm("coarse", b->output[pl0], pw0, ph0,  4 * sizeof(float), "locallaplacian");
      dt_dump_pfm("oldcoarse", output[last_level], pw, ph,  4 * sizeof(float), "locallaplacian");
    }
    DT_OMP_FOR(collapse(2))
    for(int j=0;j<ph;j++) for(int i=0;i<pw;i++)
    {
      // image coordinates in full buffer
      float ix = ((i*mul - max_supp) + b->roi->x)/b->roi->scale;
      float iy = ((j*mul - max_supp) + b->roi->y)/b->roi->scale;
      // coordinates in padded preview buffer (
      float px = CLAMP(ix / (float)b->buf->width  * b->wd + (b->pwd-b->wd)/2.0f, 0, b->pwd);
      float py = CLAMP(iy / (float)b->buf->height * b->ht + (b->pht-b->ht)/2.0f, 0, b->pht);
      // trilinear lookup:
      int px0 = CLAMP(px*mul0, 0, pw0-1);
      int py0 = CLAMP(py*mul0, 0, ph0-1);
      int px1 = CLAMP(px*mul1, 0, pw1-1);
      int py1 = CLAMP(py*mul1, 0, ph1-1);
#if 1
      float f0x = CLAMP(px*mul0 - px0, 0.0f, 1.0f);
      float f0y = CLAMP(py*mul0 - py0, 0.0f, 1.0f);
      float f1x = CLAMP(px*mul1 - px1, 0.0f, 1.0f);
      float f1y = CLAMP(py*mul1 - py1, 0.0f, 1.0f);
      float c0 =
        (1.0f-f0x)*(1.0f-f0y)*b->output[pl0][CLAMP(py0  , 0, ph0-1)*pw0 + CLAMP(px0  , 0, pw0-1)]+
        (     f0x)*(1.0f-f0y)*b->output[pl0][CLAMP(py0  , 0, ph0-1)*pw0 + CLAMP(px0+1, 0, pw0-1)]+
        (1.0f-f0x)*(     f0y)*b->output[pl0][CLAMP(py0+1, 0, ph0-1)*pw0 + CLAMP(px0  , 0, pw0-1)]+
        (     f0x)*(     f0y)*b->output[pl0][CLAMP(py0+1, 0, ph0-1)*pw0 + CLAMP(px0+1, 0, pw0-1)];
      float c1 =
        (1.0f-f1x)*(1.0f-f1y)*b->output[pl1][CLAMP(py1  , 0, ph1-1)*pw1 + CLAMP(px1  , 0, pw1-1)]+
        (     f1x)*(1.0f-f1y)*b->output[pl1][CLAMP(py1  , 0, ph1-1)*pw1 + CLAMP(px1+1, 0, pw1-1)]+
        (1.0f-f1x)*(     f1y)*b->output[pl1][CLAMP(py1+1, 0, ph1-1)*pw1 + CLAMP(px1  , 0, pw1-1)]+
        (     f1x)*(     f1y)*b->output[pl1][CLAMP(py1+1, 0, ph1-1)*pw1 + CLAMP(px1+1, 0, pw1-1)];
#else
      float c0 = b->output[pl0][py0*pw0 + px0];
      float c1 = b->output[pl1][py1*pw1 + px1];
#endif
      output[last_level][j*pw+i] = weight * c1 + (1.0f-weight) * c0;
    }
    if(darktable.dump_pfm_module)
      dt_dump_pfm("newcoarse", output[last_level], pw, ph,  4 * sizeof(float), "locallaplacian");
  }

  // assemble output pyramid coarse to fine
  for(int l=last_level-1;l >= 0; l--)
  {
    const int pw = dl(w,l), ph = dl(h,l);

    gauss_expand(output[l+1], output[l], pw, ph);
    // go through all coefficients in the upsampled gauss buffer:
    DT_OMP_FOR(collapse(2))
    for(int j=0;j<ph;j++) for(int i=0;i<pw;i++)
    {
      const float v = padded[l][j*pw+i];
      int hi = 1;
      for(;hi<num_gamma-1 && gamma[hi] <= v;hi++);
      int lo = hi-1;
      const float a = CLAMPS((v - gamma[lo])/(gamma[hi]-gamma[lo]), 0.0f, 1.0f);
      const float l0 = ll_laplacian(buf[lo][l+1], buf[lo][l], i, j, pw, ph);
      const float l1 = ll_laplacian(buf[hi][l+1], buf[hi][l], i, j, pw, ph);
      output[l][j*pw+i] += l0 * (1.0f-a) + l1 * a;
      // we could do this to save on memory (no need for finest buf[][]).
      // unfortunately it results in a quite noticeable loss of sharpness, i think
      // the extra level is worth it.
      // else if(l == 0) // use finest scale from input to not amplify noise (and use less memory)
      //   output[l][j*pw+i] += ll_laplacian(padded[l+1], padded[l], i, j, pw, ph);
    }
  }
  DT_OMP_FOR(collapse(2))
  for(int j=0;j<ht;j++) for(int i=0;i<wd;i++)
  {
    out[4*(j*wd+i)+0] = 100.0f * output[0][(j+max_supp)*w+max_supp+i]; // [0,1] -> L
    out[4*(j*wd+i)+1] = input[4*(j*wd+i)+1]; // copy original colour channels
    out[4*(j*wd+i)+2] = input[4*(j*wd+i)+2];
  }
  if(b && b->mode == 1)
  { // output the buffers for later re-use
    b->pad0 = padded[0];
    b->wd = wd;
    b->ht = ht;
    b->pwd = w;
    b->pht = h;
    b->num_levels = num_levels;
    for(int l=0;l<num_levels;l++) b->output[l] = output[l];
  }
  // free all buffers except the ones passed out for preview rendering
cleanup:
  for(int l=0;l<max_levels;l++)
  {
    if(!b || b->mode != 1 || l)   dt_free_align(padded[l]);
    if(!b || b->mode != 1)        dt_free_align(output[l]);
    for(int k=0; k<num_gamma;k++) dt_free_align(buf[k][l]);
  }
}


size_t local_laplacian_memory_use(const int width,     // width of input image
                                  const int height)    // height of input image
{
  const int num_levels = MIN(max_levels, 31-__builtin_clz(MIN(width,height)));
  const int max_supp = 1<<(num_levels-1);
  const int paddwd = width  + 2*max_supp;
  const int paddht = height + 2*max_supp;

  size_t memory_use = 0;

  for(int l=0;l<num_levels;l++)
    memory_use += sizeof(float) * (2 + num_gamma) * dl(paddwd, l) * dl(paddht, l);

  return memory_use;
}

size_t local_laplacian_singlebuffer_size(const int width,     // width of input image
                                         const int height)    // height of input image
{
  const int num_levels = MIN(max_levels, 31-__builtin_clz(MIN(width,height)));
  const int max_supp = 1<<(num_levels-1);
  const int paddwd = width  + 2*max_supp;
  const int paddht = height + 2*max_supp;

  return sizeof(float) * dl(paddwd, 0) * dl(paddht, 0);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
