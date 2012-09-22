/*
    This file is part of darktable,
    copyright (c) 2012 johannes hanika.

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
#include "common/denoise.h"
#include <string.h>
#include <xmmintrin.h>
#include <stdint.h>

typedef union floatint_t
{
  float f;
  uint32_t i;
}
floatint_t;

static inline float
fast_mexp2f(const float x, const float sharpness)
{
  const float x2 = x*x*sharpness;
  const float i1 = (float)0x3f800000u; // 2^0
  const float i2 = (float)0x3f000000u; // 2^-1
  const float k0 = i1 + x2 * (i2 - i1);
  floatint_t k;
  k.i = k0 >= (float)0x800000u ? k0 : 0;
  return k.f;
}

// do an iteration of non-local means accumulation
// with a downscaled prior.
void dt_nlm_accum_scaled(
    const float *edges,        // input features
    const float *input2,       // prior payload
    const float *edges2,       // prior features
    float       *output,       // accum buffer
    const int    width,
    const int    height,
    const int    prior_width,  // = s*width, s<1
    const int    prior_height,
    const int    P,            // patch size
    const int    K,            // search window size
    const float  sharpness,
    float       *tmp)
{
  if(P < 1) return;

  // scale factor of prior/input:
  const float scalex = prior_width/(float)width;
  const float scaley = prior_height/(float)height;

  // for each shift vector, moving the smaller prior all over the input buffer:
  for(int kj=-K;kj<height - prior_height + K;kj++)
  {
    for(int ki=-K;ki<width - prior_width + K;ki++)
    {
      int inited_slide = 0;
      // determine invariant pivot pixel:
      const int pvx = ki/(1.0f-scalex);
      const int pvy = kj/(1.0f-scaley);
      // determine relevant pixels, clip to possible bounds of prior
      // we'll access the prior at (i,j) - k(i,j) in this iteration of the loop, so
      // we want that to be inside [0, prior_{width,height}).
      const int rel_i = CLAMP(pvx - K, MAX(0, ki), MIN(width,  prior_width +ki)),
                rel_I = CLAMP(pvx + K, MAX(0, ki), MIN(width,  prior_width +ki));
      const int rel_j = CLAMP(pvy - K, MAX(0, kj), MIN(height, prior_height+kj)),
                rel_J = CLAMP(pvy + K, MAX(0, kj), MIN(height, prior_height+kj));
#ifdef _OPENMP
#  pragma omp parallel for schedule(static) default(none) firstprivate(inited_slide) shared(kj, ki, edges, edges2, input2, output, tmp)
#endif
      for(int j=rel_j; j<rel_J; j++)
      {
        float *S = tmp + dt_get_thread_num() * width;
        const float *ins = input2 + 4*(width *(j-kj) - ki);
        float *out = output + 4*width*j;

        // first line of every thread
        if(!inited_slide)
        {
          // TODO: check these!
          const int Pm = MIN(MIN(P, j-kj), j);
          const int PM = MIN(MIN(P, rel_J-1-j-kj), rel_J-1-j);

          // sum up a line 
          memset(S, 0x0, sizeof(float)*width);
          for(int jj=-Pm;jj<=PM;jj++)
          {
            float *s = S + rel_i;
            const float *inp  = edges  + 4*rel_i + 4* width *(j+jj);
            const float *inps = edges2 + 4*rel_i + 4*(width *(j+jj-kj) - ki);
            for(int i=rel_i; i<rel_I; i++)
            {
              for(int k=0;k<3;k++)
                s[0] += (inp[k] - inps[k])*(inp[k] - inps[k]);
              s++; inp+=4; inps+=4;
            }
          }
          // only reuse this if we had a full stripe
          if(Pm == P && PM == P) inited_slide = 1;
        }

        // sliding window for this line:
        float *s = S;
        float slide = 0.0f;
        // sum up the first -P..P
        for(int i=rel_i;i<rel_i+2*P+1;i++) slide += s[i];
        for(int i=rel_i; i<rel_I; i++)
        {
          if(i-P > rel_i && i+P<rel_I)
            slide += s[P] - s[-P-1];
          const __m128 iv = { ins[0], ins[1], ins[2], 1.0f };
          _mm_store_ps(out, _mm_load_ps(out) + iv * _mm_set1_ps(
                fast_mexp2f(slide, sharpness)));

          s++; ins+=4; out+=4;
        }

        if(inited_slide && j+P+1 < rel_J && j+P+1-kj < prior_height)
        {
          // sliding window in j direction:
          float *s = S + rel_i;
          const float *inp  = edges  + 4*rel_i + 4* width *(j+P+1);
          const float *inps = edges2 + 4*rel_i + 4*(width *(j+P+1-kj) - ki);
          const float *inm  = edges  + 4*rel_i + 4* width *(j-P);
          const float *inms = edges2 + 4*rel_i + 4*(width *(j-P-kj) - ki);
          for(int i=rel_i;i<rel_I;i++)
          {
            float stmp = s[0];
            for(int k=0;k<3;k++)
              stmp += ((inp[k] - inps[k])*(inp[k] - inps[k])
                    -  (inm[k] - inms[k])*(inm[k] - inms[k]));
            s[0] = stmp;
            inp+=4; inps+=4; inm+=4; inms+=4; s++;
          }
        }
        else inited_slide = 0;
      }
    }
  }
}

// version for same scale 
void dt_nlm_accum(
    const float *edges,
    const float *input2,
    const float *edges2,
    float       *output,
    const int    width,
    const int    height,
    const int    P,
    const int    K,
    const float  sharpness,
    float       *tmp)
{
  if(P < 1) return;

  // for each shift vector
  for(int kj=-K;kj<=K;kj++)
  {
    for(int ki=-K;ki<=K;ki++)
    {
      int inited_slide = 0;
      // don't construct summed area tables but use sliding window! (applies to cpu version res < 1k only, or else we will add up errors)
      // do this in parallel with a little threading overhead. could parallelize the outer loops with a bit more memory
#ifdef _OPENMP
#  pragma omp parallel for schedule(static) default(none) firstprivate(inited_slide) shared(kj, ki, edges, edges2, input2, output, tmp)
#endif
      for(int j=0; j<height; j++)
      {
        if(j+kj < 0 || j+kj >= height) continue;
        float *S = tmp + dt_get_thread_num() * width;
        const float *ins = input2 + 4*(width *(j+kj) + ki);
        float *out = output + 4*width*j;

        const int Pm = MIN(MIN(P, j+kj), j);
        const int PM = MIN(MIN(P, height-1-j-kj), height-1-j);
        // first line of every thread
        // TODO: also every once in a while to assert numerical precision!
        if(!inited_slide)
        {
          // sum up a line 
          memset(S, 0x0, sizeof(float)*width);
          for(int jj=-Pm;jj<=PM;jj++)
          {
            int i = MAX(0, -ki);
            float *s = S + i;
            const float *inp  = edges  + 4*i + 4* width *(j+jj);
            const float *inps = edges2 + 4*i + 4*(width *(j+jj+kj) + ki);
            const int last = width + MIN(0, -ki);
            for(; i<last; i++, inp+=4, inps+=4, s++)
            {
              for(int k=0;k<3;k++)
                s[0] += (inp[k] - inps[k])*(inp[k] - inps[k]);
            }
          }
          // only reuse this if we had a full stripe
          if(Pm == P && PM == P) inited_slide = 1;
        }

        // sliding window for this line:
        float *s = S;
        float slide = 0.0f;
        // sum up the first -P..P
        for(int i=0;i<2*P+1;i++) slide += s[i];
        for(int i=0; i<width; i++)
        {
          if(i-P > 0 && i+P<width)
            slide += s[P] - s[-P-1];
          if(i+ki >= 0 && i+ki < width)
          {
            const __m128 iv = { ins[0], ins[1], ins[2], 1.0f };
            _mm_store_ps(out, _mm_load_ps(out) + iv * _mm_set1_ps(
                  fast_mexp2f(slide, sharpness)));
          }
          s   ++;
          ins += 4;
          out += 4;
        }
        if(inited_slide && j+P+1+MAX(0,kj) < height)
        {
          // sliding window in j direction:
          int i = MAX(0, -ki);
          float *s = S + i;
          const float *inp  = edges  + 4*i + 4* width *(j+P+1);
          const float *inps = edges2 + 4*i + 4*(width *(j+P+1+kj) + ki);
          const float *inm  = edges  + 4*i + 4* width *(j-P);
          const float *inms = edges2 + 4*i + 4*(width *(j-P+kj) + ki);
          const int last = width + MIN(0, -ki);
          for(; ((unsigned long)s & 0xf) != 0 && i<last; i++, inp+=4, inps+=4, inm+=4, inms+=4, s++)
          {
            float stmp = s[0];
            for(int k=0;k<3;k++)
              stmp += ((inp[k] - inps[k])*(inp[k] - inps[k])
                    -  (inm[k] - inms[k])*(inm[k] - inms[k]));
            s[0] = stmp;
          }
          /* Process most of the line 4 pixels at a time */
          for(; i<last-4; i+=4, inp+=16, inps+=16, inm+=16, inms+=16, s+=4)
          {
            __m128 sv = _mm_load_ps(s);
            const __m128 inp1 = _mm_load_ps(inp)    - _mm_load_ps(inps);
            const __m128 inp2 = _mm_load_ps(inp+4)  - _mm_load_ps(inps+4);
            const __m128 inp3 = _mm_load_ps(inp+8)  - _mm_load_ps(inps+8);
            const __m128 inp4 = _mm_load_ps(inp+12) - _mm_load_ps(inps+12);

            const __m128 inp12lo = _mm_unpacklo_ps(inp1,inp2);
            const __m128 inp34lo = _mm_unpacklo_ps(inp3,inp4);
            const __m128 inp12hi = _mm_unpackhi_ps(inp1,inp2);
            const __m128 inp34hi = _mm_unpackhi_ps(inp3,inp4);

            const __m128 inpv0 = _mm_movelh_ps(inp12lo,inp34lo);
            sv += inpv0*inpv0;

            const __m128 inpv1 = _mm_movehl_ps(inp34lo,inp12lo);
            sv += inpv1*inpv1;

            const __m128 inpv2 = _mm_movelh_ps(inp12hi,inp34hi);
            sv += inpv2*inpv2;

            const __m128 inm1 = _mm_load_ps(inm)    - _mm_load_ps(inms);
            const __m128 inm2 = _mm_load_ps(inm+4)  - _mm_load_ps(inms+4);
            const __m128 inm3 = _mm_load_ps(inm+8)  - _mm_load_ps(inms+8);
            const __m128 inm4 = _mm_load_ps(inm+12) - _mm_load_ps(inms+12);

            const __m128 inm12lo = _mm_unpacklo_ps(inm1,inm2);
            const __m128 inm34lo = _mm_unpacklo_ps(inm3,inm4);
            const __m128 inm12hi = _mm_unpackhi_ps(inm1,inm2);
            const __m128 inm34hi = _mm_unpackhi_ps(inm3,inm4);

            const __m128 inmv0 = _mm_movelh_ps(inm12lo,inm34lo);
            sv -= inmv0*inmv0;

            const __m128 inmv1 = _mm_movehl_ps(inm34lo,inm12lo);
            sv -= inmv1*inmv1;

            const __m128 inmv2 = _mm_movelh_ps(inm12hi,inm34hi);
            sv -= inmv2*inmv2;

            _mm_store_ps(s, sv);
          }
          for(; i<last; i++, inp+=4, inps+=4, inm+=4, inms+=4, s++)
          {
            float stmp = s[0];
            for(int k=0;k<3;k++)
              stmp += ((inp[k] - inps[k])*(inp[k] - inps[k])
                    -  (inm[k] - inms[k])*(inm[k] - inms[k]));
            s[0] = stmp;
          }
        }
        else inited_slide = 0;
      }
    }
  }
}

void dt_nlm_normalize(
    const float *const input,
    float       *const output,
    const int          width,
    const int          height,
    const float        luma,
    const float        chroma)
{
  // normalize and apply chroma/luma blending
  const __m128 weight = _mm_set_ps(1.0f, chroma, chroma, luma);
  const __m128 invert = _mm_sub_ps(_mm_set1_ps(1.0f), weight);
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=0; j<height; j++)
  {
    float *out = output + 4*width*j;
    const float *in  = input  + 4*width*j;
    for(int i=0; i<width; i++)
    {
      _mm_store_ps(out, _mm_add_ps(
          _mm_mul_ps(_mm_load_ps(in),  invert),
          _mm_mul_ps(_mm_load_ps(out), _mm_div_ps(weight, _mm_set1_ps(out[3])))));
      out += 4;
      in  += 4;
    }
  }
}
