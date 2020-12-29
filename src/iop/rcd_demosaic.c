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

/*
* RATIO CORRECTED DEMOSAICING
* Luis Sanz Rodríguez (luis.sanz.rodriguez(at)gmail(dot)com)
*
* Release 2.3 @ 171125
*
* Original code from https://github.com/LuisSR/RCD-Demosaicing
* Licensed under the GNU GPL version 3
*/

/*
* The tiling and sse2 optimised coding has been done by
* Ingo Weyrich (heckflosse67@gmx.de) in rawtherapee code and has been slightly
* modified in cooperation.
* Hanno Schwalm (hanno@schwalm-bremen.de)
*/

/* some notes about the algorithm
  - the calculated data at the borders are not stable for tiling if we don't use
    a RCD_BORDER of at least 9. The bordersize is the overlapping region for tiles.
  - Is it safe to use -Ofast? Yes
*/

// this allows to pass RCD_TILESIZE to the code. On some machines larger tilesizes are faster
// If it is undefined it will be set to 140, which is a good guess for modern x86/64 machines
// Tested and tuned on Xeon E-2288G, i5-8250U and AMD FX8350 
#ifndef RCD_TILESIZE
 #define RCD_TILESIZE 140
#endif

// Make sure we use -Ofast only in the rcd code section
#ifdef __GNUC__
  #pragma GCC push_options
  #pragma GCC optimize ("-Ofast")
#endif

#ifdef __GNUC__
#define INLINE __inline
#else
#define INLINE inline
#endif

#ifdef __SSE__
#include <x86intrin.h>
#endif

#define FCRCD(row, col) (cfarray[(((row) & 1)<<1) | ((col) & 1)])

#define RCD_BORDER 9          // must be at least 9 to avoid tile-overlap errors
#define RCD_TILEVALID (RCD_TILESIZE - 2 * RCD_BORDER)
#define w1 RCD_TILESIZE
#define w2 (2 * RCD_TILESIZE)
#define w3 (3 * RCD_TILESIZE)
#define w4 (4 * RCD_TILESIZE)

#define eps 1e-5    // // Tolerance to avoid dividing by zero
#define epssq 1e-10

/* Some macros and inline functions taken from amaze_demosaic_RT */
// Only include the vector macros/functions for SSE
#ifdef __SSE2__
typedef __m128i vmask;
typedef __m128 vfloat;

#define F2V(a) _mm_set1_ps((a))
#define LVFU(x) _mm_loadu_ps(&x)
#define STVFU(x,y) _mm_storeu_ps(&x,y)
#ifdef __SSE4_1__
// SSE4.1 => use _mm_blend_ps instead of _mm_set_epi32 and vself
#define STC2VFU(a,v) {\
                         __m128 TST1V = _mm_loadu_ps(&a);\
                         __m128 TST2V = _mm_unpacklo_ps(v,v);\
                         _mm_storeu_ps(&a, _mm_blend_ps(TST1V,TST2V,5));\
                         TST1V = _mm_loadu_ps((&a)+4);\
                         TST2V = _mm_unpackhi_ps(v,v);\
                         _mm_storeu_ps((&a)+4, _mm_blend_ps(TST1V,TST2V,5));\
                     }
#else
#define STC2VFU(a,v) {\
                         __m128 TST1V = _mm_loadu_ps(&a);\
                         __m128 TST2V = _mm_unpacklo_ps(v,v);\
                         vmask cmask = _mm_set_epi32(0xffffffff,0,0xffffffff,0);\
                         _mm_storeu_ps(&a, vself(cmask,TST1V,TST2V));\
                         TST1V = _mm_loadu_ps((&a)+4);\
                         TST2V = _mm_unpackhi_ps(v,v);\
                         _mm_storeu_ps((&a)+4, vself(cmask,TST1V,TST2V));\
                     }
#endif

static INLINE vfloat LC2VFU(float *a)
{ // Load 8 floats from a and combine a[0],a[2],a[4] and a[6] into a vector of 4 floats
  vfloat a1 = _mm_loadu_ps(a);
  vfloat a2 = _mm_loadu_ps((a) + 4);
  return _mm_shuffle_ps(a1, a2, _MM_SHUFFLE(2, 0, 2, 0));
}
static INLINE vfloat vcast_vf_f(float f)
{
  return _mm_set_ps(f, f, f, f);
}
static INLINE vmask vorm(vmask x, vmask y)
{
  return _mm_or_si128(x, y);
}
static INLINE vmask vandm(vmask x, vmask y)
{
  return _mm_and_si128(x, y);
}
static INLINE vmask vandnotm(vmask x, vmask y)
{
  return _mm_andnot_si128(x, y);
}
static INLINE vfloat vabsf(vfloat f)
{
  return (vfloat)vandnotm((vmask)vcast_vf_f(-0.0f), (vmask)f);
}

#ifdef __SSE4_1__
// only one instruction when using SSE4.1
static INLINE vfloat vself(vmask mask, vfloat x, vfloat y) {
    return _mm_blendv_ps(y,x,(vfloat)mask);
}

#else
// three instructions when using SSE2
static INLINE vfloat vself(vmask mask, vfloat x, vfloat y) {
    return (vfloat)vorm(vandm(mask, (vmask)x), vandnotm(mask, (vmask)y));
}
#endif

static INLINE vmask vmaskf_lt(vfloat x, vfloat y)
{
  return (__m128i)_mm_cmplt_ps(x, y);
}
static INLINE vfloat vintpf(vfloat a, vfloat b, vfloat c)
{
  return a * (b - c) + c;
}
static INLINE vfloat vmaxf(vfloat x, vfloat y)
{
  return _mm_max_ps(x, y);
}
static INLINE vfloat vminf(vfloat x, vfloat y)
{
  return _mm_min_ps(x, y);
}
#endif

static INLINE float intp(float a, float b, float c)
{
    // calculate a * b + (1 - a) * c
    // following is valid:
    // intp(a, b+x, c+x) = intp(a, b, c) + x
    // intp(a, b*x, c*x) = intp(a, b, c) * x
    return a * (b - c) + c;
}

/* End of macros and inline functions taken from amaze_demosaic_RT */

// The border interpolation has been taken from rt, adapted to dt.
// The original dcraw based code had much stronger color artefacts in the border region. 
static INLINE void approxit(float *out, const float *cfa, const float *sum, const int idx, const int c)
{
  float (*rgb)[4] = (void *)out;
  if(c == 1)
  {
    rgb[idx][0] = (sum[0] / sum[3]);
    rgb[idx][1] = CLIP(cfa[idx]);
    rgb[idx][2] = (sum[2] / sum[5]);
  }
  else
  {
    rgb[idx][1] =  (sum[1] / sum[4]);
    if (c == 0)
    {
      rgb[idx][0] = CLIP(cfa[idx]);
      rgb[idx][2] = (sum[2] / sum[5]);
    }
    else
    {
      rgb[idx][0] = (sum[0] / sum[3]);
      rgb[idx][2] = CLIP(cfa[idx]);
    }
  }
}

static void rcd_border_interpolate(float *out, const float *cfa, const int *cfarray, const int width, const int height, int border)
{
  for(int i = 0; i < height; i++)
  {
    float sum[6];
    for(int j = 0; j < border; j++)
    {
      for(int x = 0; x < 6; x++) { sum[x] = 0.0f; }
      for(int i1 = i - 1; i1 < i + 2; i1++)
      {
        for(int j1 = j - 1; j1 < j + 2; j1++)
        {
          if((i1 > -1) && (i1 < height) && (j1 > -1))
          {
            const int c = FCRCD(i1, j1);
            sum[c] += CLIP(cfa[i1 * width + j1]);
            sum[c + 3]++;
          }
        }
      }
      approxit(out, cfa, sum, i * width + j, FCRCD(i, j)); 
    }

    for(int j = width - border; j < width; j++)
    {
      for(int x = 0; x < 6; x++) { sum[x] = 0.0f; }
      for(int i1 = i - 1; i1 < i + 2; i1++)
      {
        for(int j1 = j - 1; j1 < j + 2; j1++)
        {
          if((i1 > -1) && (i1 < height ) && (j1 < width))
          {
            const int c = FCRCD(i1, j1);
            sum[c] += CLIP(cfa[i1 * width + j1]);
            sum[c + 3]++;
          }
        }
      }
      approxit(out, cfa, sum, i * width + j, FCRCD(i, j)); 
    }
  }
  for(int i = 0; i < border; i++)
  {
    float sum[6];
    for(int j = border; j < width - border; j++)
    {
      for(int x = 0; x < 6; x++) { sum[x] = 0.0f; }
      for(int i1 = i - 1; i1 < i + 2; i1++)
      {
        for(int j1 = j - 1; j1 < j + 2; j1++)
        {
          if((i1 > -1) && (i1 < height) && (j1 > -1))
          {
            const int c = FCRCD(i1, j1);
            sum[c] += CLIP(cfa[i1 * width + j1]);
            sum[c + 3]++;
          }
        }
      }
      approxit(out, cfa, sum, i * width + j, FCRCD(i, j)); 
    }
  }
  for(int i = height - border; i < height; i++)
  {
    float sum[6];
    for(int j = border; j < width - border; j++)
    {
      for(int x = 0; x < 6; x++) { sum[x] = 0.0f; }
      for(int i1 = i - 1; i1 < i + 2; i1++)
      {
        for(int j1 = j - 1; j1 < j + 2; j1++)
        {
          if((i1 > -1) && (i1 < height) && (j1 < width))
          {
            const int c = FCRCD(i1, j1);
            sum[c] += CLIP(cfa[i1 * width + j1]);
            sum[c + 3]++;
          }
        }
      }
      approxit(out, cfa, sum, i * width + j, FCRCD(i, j)); 
    }
  }
}

#ifdef _OPENMP
  #pragma omp declare simd aligned(in, out)
#endif
static void rcd_demosaic(dt_dev_pixelpipe_iop_t *piece, float *const restrict out, const float *const restrict in, dt_iop_roi_t *const roi_out,
                                   const dt_iop_roi_t *const roi_in, const uint32_t filters)
{
  const int width = roi_in->width;
  const int height = roi_in->height;

  if((width < 16) || (height < 16))
  {
    dt_control_log(_("[rcd_demosaic] too small area"));
    return;
  }

  const float scaler = fmaxf(piece->pipe->dsc.processed_maximum[0], fmaxf(piece->pipe->dsc.processed_maximum[1], piece->pipe->dsc.processed_maximum[2]));
  const float revscaler = 1.0f / scaler;

  const int cfarray[4] = {FC(roi_in->y, roi_in->x, filters), FC(roi_in->y, roi_in->x + 1, filters), FC(roi_in->y + 1, roi_in->x, filters), FC(roi_in->y + 1, roi_in->x + 1, filters)};

  const int num_vertical = 1 + (height - 2 * RCD_BORDER -1) / RCD_TILEVALID;
  const int num_horizontal = 1 + (width - 2 * RCD_BORDER -1) / RCD_TILEVALID;

#ifdef _OPENMP
  #pragma omp parallel \
  dt_omp_firstprivate(width, height, cfarray, out, in, scaler, revscaler)
#endif
  { 
    float *const VH_Dir = dt_alloc_align_float((size_t) RCD_TILESIZE * RCD_TILESIZE);
    float *const PQ_Dir = dt_alloc_align_float((size_t) RCD_TILESIZE * RCD_TILESIZE / 2);
    float *const cfa =    dt_alloc_align_float((size_t) RCD_TILESIZE * RCD_TILESIZE);
    float (*const rgb)[RCD_TILESIZE * RCD_TILESIZE] = (void *)dt_alloc_align_float((size_t)3 * RCD_TILESIZE * RCD_TILESIZE);

    // No overlapping use so re-use same buffer; also note we use divide-by-2 index for lower mem pressure
    // this divide-by-2 also allows slightly faster sse2 specific code.
    float *const lpf = PQ_Dir;

    // There has been a discussion about the schedule strategy, at least on the tested machines the
    // dynamic scheduling seems to be slightly faster.
#ifdef _OPENMP
  #pragma omp for schedule(simd:dynamic, 6) collapse(2) nowait
#endif
    for(int tile_vertical = 0; tile_vertical < num_vertical; tile_vertical++)
    {
      for(int tile_horizontal = 0; tile_horizontal < num_horizontal; tile_horizontal++)
      {
        const int rowStart = tile_vertical * RCD_TILEVALID;
        const int rowEnd = MIN(rowStart + RCD_TILESIZE, height);

        const int colStart = tile_horizontal * RCD_TILEVALID;
        const int colEnd = MIN(colStart + RCD_TILESIZE, width);

        const int tileRows = MIN(rowEnd - rowStart, RCD_TILESIZE);
        const int tileCols = MIN(colEnd - colStart, RCD_TILESIZE);

        // Step 0: fill data and make sure data are not negative.
        for(int row = rowStart; row < rowEnd; row++)
        {
          
          int indx = (row - rowStart) * RCD_TILESIZE;
          int in_indx = (row * width + colStart);
          const int c0 = FCRCD(row, colStart);
          const int c1 = FCRCD(row, colStart + 1);
          int col = colStart;

          for(; col < colEnd - 1; col+=2, indx+=2, in_indx+=2)
          {
            cfa[indx]     = rgb[c0][indx]     = CLIP(in[in_indx] * revscaler);
            cfa[indx + 1] = rgb[c1][indx + 1] = CLIP(in[in_indx+1] * revscaler);
          }
          if(col < colEnd)
          {
            cfa[indx] = rgb[c0][indx] = CLIP(in[indx] * revscaler);
          }
        }

        // STEP 1: Find vertical and horizontal interpolation directions
        // Step 1.1: Calculate vertical and horizontal local discrimination
        for(int row = 4; row < tileRows - 4; row++)
        {
          for(int col = 4, indx = row * RCD_TILESIZE + col; col < tileCols - 4; col++, indx++)
          {
            const float V_Stat = fmaxf(epssq,
               -2.0f * (cfa[indx]) * (9.f * (cfa[indx - w1] + cfa[indx + w1] - cfa[indx - w3] - cfa[indx + w3] + 2.0f * (cfa[indx - w2] + cfa[indx + w2]) ) + cfa[indx - w4] + cfa[indx + w4] - 19.f * cfa[indx])
              - 70.f * cfa[indx - w1] * cfa[indx + w1]
              - 12.f * (cfa[indx - w1] * cfa[indx - w2] - cfa[indx - w1] * cfa[indx - w4] + cfa[indx + w1] * cfa[indx + w2] - cfa[indx + w1] * cfa[indx + w4] + cfa[indx - w2] * cfa[indx + w3] + cfa[indx + w2] * cfa[indx - w3])
              + 24.f * (cfa[indx - w1] * cfa[indx + w2] + cfa[indx + w1] * cfa[indx - w2])
              + 16.f * (cfa[indx - w1] * cfa[indx + w3] + cfa[indx + w1] * cfa[indx - w3])
              - 6.f * (cfa[indx + w4] * (cfa[indx - w1] + cfa[indx + w3]) + cfa[indx - w4] * (cfa[indx + w1] + cfa[indx - w3]))
              + 46.f * (cfa[indx - w1] * cfa[indx - w1] + cfa[indx + w1] * cfa[indx + w1])
              - 38.f * (cfa[indx + w1] * cfa[indx + w3] + cfa[indx - w1] * cfa[indx - w3])
              + 14.f * cfa[indx - w2] * cfa[indx + w2]
              - 2.0f * ((cfa[indx - w2] - cfa[indx + w2]) * (cfa[indx - w4] - cfa[indx + w4]) - cfa[indx - w3] * cfa[indx + w3])
              + 11.f * (cfa[indx - w2] * cfa[indx - w2] + cfa[indx + w2] * cfa[indx + w2])
              + 10.f * (cfa[indx - w3] * cfa[indx - w3] + cfa[indx + w3] * cfa[indx + w3])
              + cfa[indx - w4] * cfa[indx - w4]
              + cfa[indx + w4] * cfa[indx + w4]
              );

            const float cfai = cfa[indx];
            const float H_Stat = fmaxf(epssq,
                -18.f * cfai * (cfa[indx -  1] + cfa[indx +  1] + 2.0f * (cfa[indx -  2] + cfa[indx +  2]) - cfa[indx -  3] - cfa[indx +  3])
                - 2.0f * cfai * (cfa[indx -  4] + cfa[indx +  4] - 19.f * cfai)
                - cfa[indx -  1] * (70.f * cfa[indx +  1] + 12.f * (cfa[indx -  2] - cfa[indx -  4] - 2.0f * cfa[indx +  2]) + 38.f * cfa[indx -  3] - 16.f * cfa[indx +  3] + 6.f * cfa[indx +  4] - 46.f * cfa[indx -  1])
                + cfa[indx +  1] * (24.f * cfa[indx -  2] + 12.f * (cfa[indx +  4] - cfa[indx +  2]) + 16.f * cfa[indx -  3] - 38.f * cfa[indx +  3] -  6.f * cfa[indx -  4] + 46.f * cfa[indx +  1])
                + cfa[indx -  2] * (14.f * cfa[indx +  2] - 12.f * cfa[indx +  3] - 2.0f * cfa[indx -  4] + 2.0f * cfa[indx +  4] + 11.f * cfa[indx -  2])
                + cfa[indx +  2] * (-12.f * cfa[indx -  3] + 2.0f * (cfa[indx -  4] - cfa[indx +  4]) + 11.f * cfa[indx +  2])
                + cfa[indx -  3] * (2.0f * cfa[indx +  3] - 6.f * cfa[indx -  4] + 10.f * cfa[indx -  3])
                + cfa[indx +  3] * (-6.f * cfa[indx +  4] + 10.f * cfa[indx +  3])
                + cfa[indx -  4] * cfa[indx -  4]
                + cfa[indx +  4] * cfa[indx +  4]);
            VH_Dir[indx] = V_Stat / (V_Stat + H_Stat);
          }
        }

        // STEP 2: Calculate the low pass filter
        // Step 2.1: Low pass filter incorporating green, red and blue local samples from the raw data
        // as an index>>1 access breaks proper vectorizing we use an extra index for the lpf results
        for(int row = 2; row < tileRows - 2; row++)
        {
          for(int col = 2 + (FCRCD(row, 0) & 1), indx = row * RCD_TILESIZE + col, lp_indx = indx / 2; col < tileCols - 2; col += 2, indx +=2, lp_indx++)
          {
            lpf[lp_indx] = 0.25f * cfa[indx]
                        + 0.125f * (cfa[indx - w1] + cfa[indx + w1] + cfa[indx - 1] + cfa[indx + 1])
                       + 0.0625f * (cfa[indx - w1 - 1] + cfa[indx - w1 + 1] + cfa[indx + w1 - 1] + cfa[indx + w1 + 1]);
          }
        }

        // STEP 3: Populate the green channel
        // Step 3.1: Populate the green channel at blue and red CFA positions
        for(int row = 4; row < tileRows - 4; row++)
        {
          int col = 4 + (FCRCD(row, 0) & 1);
          int indx = row * RCD_TILESIZE + col;
          int lp_indx = indx / 2;
          // There has been quite some performance testing for the generic vs. SSE2 optimized loop, as the generated generic code (-O3)
          // is not as fast as Ingos optmized code (~4% loss) we keep the SSE2 specific path.
#ifdef __SSE2__
          const vfloat zd5v = F2V(0.5f);
          const vfloat zd25v = F2V(0.25f);
          const vfloat epsv = F2V(eps);
          for (; col < tileCols - 7; col += 8, indx += 8, lp_indx +=4)
          {
            // Cardinal gradients
            const vfloat cfai = LC2VFU(&cfa[indx]);
            const vfloat N_Grad = epsv + (vabsf(LC2VFU(&cfa[indx - w1]) - LC2VFU(&cfa[indx + w1])) + vabsf(cfai - LC2VFU(&cfa[indx - w2]))) + (vabsf(LC2VFU(&cfa[indx - w1]) - LC2VFU(&cfa[indx - w3])) + vabsf(LC2VFU(&cfa[indx - w2]) - LC2VFU(&cfa[indx - w4])));
            const vfloat S_Grad = epsv + (vabsf(LC2VFU(&cfa[indx - w1]) - LC2VFU(&cfa[indx + w1])) + vabsf(cfai - LC2VFU(&cfa[indx + w2]))) + (vabsf(LC2VFU(&cfa[indx + w1]) - LC2VFU(&cfa[indx + w3])) + vabsf(LC2VFU(&cfa[indx + w2]) - LC2VFU(&cfa[indx + w4])));
            const vfloat W_Grad = epsv + (vabsf(LC2VFU(&cfa[indx -  1]) - LC2VFU(&cfa[indx +  1])) + vabsf(cfai - LC2VFU(&cfa[indx -  2]))) + (vabsf(LC2VFU(&cfa[indx -  1]) - LC2VFU(&cfa[indx -  3])) + vabsf(LC2VFU(&cfa[indx -  2]) - LC2VFU(&cfa[indx -  4])));
            const vfloat E_Grad = epsv + (vabsf(LC2VFU(&cfa[indx +  1]) - LC2VFU(&cfa[indx -  1])) + vabsf(cfai - LC2VFU(&cfa[indx +  2]))) + (vabsf(LC2VFU(&cfa[indx +  1]) - LC2VFU(&cfa[indx +  3])) + vabsf(LC2VFU(&cfa[indx +  2]) - LC2VFU(&cfa[indx +  4])));

            // Cardinal pixel estimations
            const vfloat lpfi = LVFU(lpf[lp_indx]);
            const vfloat N_Est = LC2VFU(&cfa[indx - w1]) + (LC2VFU(&cfa[indx - w1]) * (lpfi - LVFU(lpf[lp_indx - w1])) / (epsv + lpfi + LVFU(lpf[lp_indx - w1])));
            const vfloat S_Est = LC2VFU(&cfa[indx + w1]) + (LC2VFU(&cfa[indx + w1]) * (lpfi - LVFU(lpf[lp_indx + w1])) / (epsv + lpfi + LVFU(lpf[lp_indx + w1])));
            const vfloat W_Est = LC2VFU(&cfa[indx -  1]) + (LC2VFU(&cfa[indx -  1]) * (lpfi - LVFU(lpf[lp_indx -  1])) / (epsv + lpfi + LVFU(lpf[lp_indx -  1])));
            const vfloat E_Est = LC2VFU(&cfa[indx +  1]) + (LC2VFU(&cfa[indx +  1]) * (lpfi - LVFU(lpf[lp_indx +  1])) / (epsv + lpfi + LVFU(lpf[lp_indx +  1])));

            // Vertical and horizontal estimations
            const vfloat V_Est = (S_Grad * N_Est + N_Grad * S_Est) / (N_Grad + S_Grad);
            const vfloat H_Est = (W_Grad * E_Est + E_Grad * W_Est) / (E_Grad + W_Grad);

            // G@B and G@R interpolation
            // Refined vertical and horizontal local discrimination
            const vfloat VH_Central_Value = LC2VFU(&VH_Dir[indx]);
            const vfloat VH_Neighbourhood_Value = zd25v * ((LC2VFU(&VH_Dir[indx - w1 - 1]) + LC2VFU(&VH_Dir[indx - w1 + 1])) + (LC2VFU(&VH_Dir[indx + w1 - 1]) + LC2VFU(&VH_Dir[indx + w1 + 1])));

            const vfloat VH_Disc = vself(vmaskf_lt(vabsf(zd5v - VH_Central_Value), vabsf(zd5v - VH_Neighbourhood_Value)), VH_Neighbourhood_Value, VH_Central_Value);
            const vfloat result = vintpf(VH_Disc, H_Est, V_Est);
            STC2VFU(rgb[1][indx], result);
          }
#endif
          for(; col < tileCols - 4; col += 2, indx +=2, lp_indx++)
          {
            const float cfai = cfa[indx];

            // Cardinal gradients
            const float N_Grad = eps + fabs(cfa[indx - w1] - cfa[indx + w1]) + fabs(cfai - cfa[indx - w2]) + fabs(cfa[indx - w1] - cfa[indx - w3]) + fabs(cfa[indx - w2] - cfa[indx - w4]);
            const float S_Grad = eps + fabs(cfa[indx - w1] - cfa[indx + w1]) + fabs(cfai - cfa[indx + w2]) + fabs(cfa[indx + w1] - cfa[indx + w3]) + fabs(cfa[indx + w2] - cfa[indx + w4]);
            const float W_Grad = eps + fabs(cfa[indx -  1] - cfa[indx +  1]) + fabs(cfai - cfa[indx -  2]) + fabs(cfa[indx -  1] - cfa[indx -  3]) + fabs(cfa[indx -  2] - cfa[indx -  4]);
            const float E_Grad = eps + fabs(cfa[indx +  1] - cfa[indx -  1]) + fabs(cfai - cfa[indx +  2]) + fabs(cfa[indx +  1] - cfa[indx +  3]) + fabs(cfa[indx +  2] - cfa[indx +  4]);

            const float lpfi = lpf[lp_indx];
            // Cardinal pixel estimations
            const float N_Est = cfa[indx - w1] + (cfa[indx - w1] * (lpfi - lpf[lp_indx - w1]) / (eps + lpfi + lpf[lp_indx - w1]));
            const float S_Est = cfa[indx + w1] + (cfa[indx + w1] * (lpfi - lpf[lp_indx + w1]) / (eps + lpfi + lpf[lp_indx + w1]));
            const float W_Est = cfa[indx -  1] + (cfa[indx -  1] * (lpfi - lpf[lp_indx -  1]) / (eps + lpfi + lpf[lp_indx -  1]));
            const float E_Est = cfa[indx +  1] + (cfa[indx +  1] * (lpfi - lpf[lp_indx +  1]) / (eps + lpfi + lpf[lp_indx +  1]));

            // Vertical and horizontal estimations
            const float V_Est = (S_Grad * N_Est + N_Grad * S_Est) / (N_Grad + S_Grad);
            const float H_Est = (W_Grad * E_Est + E_Grad * W_Est) / (E_Grad + W_Grad);

            // G@B and G@R interpolation
            // Refined vertical and horizontal local discrimination
            const float VH_Central_Value = VH_Dir[indx];
            const float VH_Neighbourhood_Value = 0.25f * (VH_Dir[indx - w1 - 1] + VH_Dir[indx - w1 + 1] + VH_Dir[indx + w1 - 1] + VH_Dir[indx + w1 + 1]);
            const float VH_Disc = (fabs(0.5f - VH_Central_Value) < fabs(0.5f - VH_Neighbourhood_Value)) ? VH_Neighbourhood_Value : VH_Central_Value;

            rgb[1][indx] = intp(VH_Disc, H_Est, V_Est);
          }
        }

        // STEP 4: Populate the red and blue channels
        // Step 4.1: Calculate P/Q diagonal local discrimination
        for(int row = 4; row < tileRows - 4; row++)
        {
          for(int col = 4 + (FCRCD(row, 0) & 1), indx = row * RCD_TILESIZE + col, pqindx = indx / 2; col < tileCols - 4; col += 2, indx += 2, pqindx++)
          {
            const float cfai = cfa[indx];

            float P_Stat = fmaxf(epssq, - 18.f * cfai * (cfa[indx - w1 - 1] + cfa[indx + w1 + 1] + 2.f * (cfa[indx - w2 - 2] + cfa[indx + w2 + 2]) - cfa[indx - w3 - 3] - cfa[indx + w3 + 3]) - 2.f * cfai * (cfa[indx - w4 - 4] + cfa[indx + w4 + 4] - 19.f * cfai) - cfa[indx - w1 - 1] * (70.f * cfa[indx + w1 + 1] - 12.f * cfa[indx - w2 - 2] + 24.f * cfa[indx + w2 + 2] - 38.f * cfa[indx - w3 - 3] + 16.f * cfa[indx + w3 + 3] + 12.f * cfa[indx - w4 - 4] - 6.f * cfa[indx + w4 + 4] + 46.f * cfa[indx - w1 - 1]) + cfa[indx + w1 + 1] * (24.f * cfa[indx - w2 - 2] - 12.f * cfa[indx + w2 + 2] + 16.f * cfa[indx - w3 - 3] - 38.f * cfa[indx + w3 + 3] - 6.f * cfa[indx - w4 - 4] + 12.f * cfa[indx + w4 + 4] + 46.f * cfa[indx + w1 + 1]) + cfa[indx - w2 - 2] * (14.f * cfa[indx + w2 + 2] - 12.f * cfa[indx + w3 + 3] - 2.f * (cfa[indx - w4 - 4] - cfa[indx + w4 + 4]) + 11.f * cfa[indx - w2 - 2]) - cfa[indx + w2 + 2] * (12.f * cfa[indx - w3 - 3] + 2.f * (cfa[indx - w4 - 4] - cfa[indx + w4 + 4]) + 11.f * cfa[indx + w2 + 2]) + cfa[indx - w3 - 3] * (2.f * cfa[indx + w3 + 3] - 6.f * cfa[indx - w4 - 4] + 10.f * cfa[indx - w3 - 3]) - cfa[indx + w3 + 3] * (6.f * cfa[indx + w4 + 4] + 10.f * cfa[indx + w3 + 3]) + cfa[indx - w4 - 4] * cfa[indx - w4 - 4] + cfa[indx + w4 + 4] * cfa[indx + w4 + 4]);
            float Q_Stat = fmaxf(epssq, - 18.f * cfai * (cfa[indx + w1 - 1] + cfa[indx - w1 + 1] + 2.f * (cfa[indx + w2 - 2] + cfa[indx - w2 + 2]) - cfa[indx + w3 - 3] - cfa[indx - w3 + 3]) - 2.f * cfai * (cfa[indx + w4 - 4] + cfa[indx - w4 + 4] - 19.f * cfai) - cfa[indx + w1 - 1] * (70.f * cfa[indx - w1 + 1] - 12.f * cfa[indx + w2 - 2] + 24.f * cfa[indx - w2 + 2] - 38.f * cfa[indx + w3 - 3] + 16.f * cfa[indx - w3 + 3] + 12.f * cfa[indx + w4 - 4] - 6.f * cfa[indx - w4 + 4] + 46.f * cfa[indx + w1 - 1]) + cfa[indx - w1 + 1] * (24.f * cfa[indx + w2 - 2] - 12.f * cfa[indx - w2 + 2] + 16.f * cfa[indx + w3 - 3] - 38.f * cfa[indx - w3 + 3] - 6.f * cfa[indx + w4 - 4] + 12.f * cfa[indx - w4 + 4] + 46.f * cfa[indx - w1 + 1]) + cfa[indx + w2 - 2] * (14.f * cfa[indx - w2 + 2] - 12.f * cfa[indx - w3 + 3] - 2.f * (cfa[indx + w4 - 4] - cfa[indx - w4 + 4]) + 11.f * cfa[indx + w2 - 2]) - cfa[indx - w2 + 2] * (12.f * cfa[indx + w3 - 3] + 2.f * (cfa[indx + w4 - 4] - cfa[indx - w4 + 4]) + 11.f * cfa[indx - w2 + 2]) + cfa[indx + w3 - 3] * (2.f * cfa[indx - w3 + 3] - 6.f * cfa[indx + w4 - 4] + 10.f * cfa[indx + w3 - 3]) - cfa[indx - w3 + 3] * (6.f * cfa[indx - w4 + 4] + 10.f * cfa[indx - w3 + 3]) + cfa[indx + w4 - 4] * cfa[indx + w4 - 4] + cfa[indx - w4 + 4] * cfa[indx - w4 + 4]);

            PQ_Dir[pqindx] = P_Stat / (P_Stat + Q_Stat);
          }
        }

        // Step 4.2: Populate the red and blue channels at blue and red CFA positions
        for(int row = 4; row < tileRows - 4; row++)
        {
          for(int col = 4 + (FCRCD(row, 0) & 1), indx = row * RCD_TILESIZE + col, c = 2 - FCRCD(row, col), pqindx = indx / 2, pqindx2 = (indx - w1 - 1) / 2, pqindx3 = (indx + w1 - 1) / 2; col < tileCols - 4; col += 2, indx += 2, pqindx++, pqindx2++, pqindx3++)
          {
            // Refined P/Q diagonal local discrimination
            float PQ_Central_Value   = PQ_Dir[pqindx];
            float PQ_Neighbourhood_Value = 0.25f * (PQ_Dir[pqindx2] + PQ_Dir[pqindx2 + 1] + PQ_Dir[pqindx3] + PQ_Dir[pqindx3 + 1]);

            float PQ_Disc = (fabs(0.5f - PQ_Central_Value) < fabs(0.5f - PQ_Neighbourhood_Value)) ? PQ_Neighbourhood_Value : PQ_Central_Value;

            // Diagonal gradients
            float NW_Grad = eps + fabs(rgb[c][indx - w1 - 1] - rgb[c][indx + w1 + 1]) + fabs(rgb[c][indx - w1 - 1] - rgb[c][indx - w3 - 3]) + fabs(rgb[1][indx] - rgb[1][indx - w2 - 2]);
            float NE_Grad = eps + fabs(rgb[c][indx - w1 + 1] - rgb[c][indx + w1 - 1]) + fabs(rgb[c][indx - w1 + 1] - rgb[c][indx - w3 + 3]) + fabs(rgb[1][indx] - rgb[1][indx - w2 + 2]);
            float SW_Grad = eps + fabs(rgb[c][indx - w1 + 1] - rgb[c][indx + w1 - 1]) + fabs(rgb[c][indx + w1 - 1] - rgb[c][indx + w3 - 3]) + fabs(rgb[1][indx] - rgb[1][indx + w2 - 2]);
            float SE_Grad = eps + fabs(rgb[c][indx - w1 - 1] - rgb[c][indx + w1 + 1]) + fabs(rgb[c][indx + w1 + 1] - rgb[c][indx + w3 + 3]) + fabs(rgb[1][indx] - rgb[1][indx + w2 + 2]);

            // Diagonal colour differences
            float NW_Est = rgb[c][indx - w1 - 1] - rgb[1][indx - w1 - 1];
            float NE_Est = rgb[c][indx - w1 + 1] - rgb[1][indx - w1 + 1];
            float SW_Est = rgb[c][indx + w1 - 1] - rgb[1][indx + w1 - 1];
            float SE_Est = rgb[c][indx + w1 + 1] - rgb[1][indx + w1 + 1];

            // P/Q estimations
            float P_Est = (NW_Grad * SE_Est + SE_Grad * NW_Est) / (NW_Grad + SE_Grad);
            float Q_Est = (NE_Grad * SW_Est + SW_Grad * NE_Est) / (NE_Grad + SW_Grad);

            // R@B and B@R interpolation
            rgb[c][indx] = rgb[1][indx] + intp(PQ_Disc, Q_Est, P_Est);
          }
        }

        // Step 4.3: Populate the red and blue channels at green CFA positions
        for(int row = 4; row < tileRows - 4; row++)
        {
          for(int col = 4 + (FCRCD(row, 1) & 1), indx = row * RCD_TILESIZE + col; col < tileCols - 4; col += 2, indx +=2)
          {
            // Refined vertical and horizontal local discrimination
            const float VH_Central_Value = VH_Dir[indx];
            const float VH_Neighbourhood_Value = 0.25f * (VH_Dir[indx - w1 - 1] + VH_Dir[indx - w1 + 1] + VH_Dir[indx + w1 - 1] + VH_Dir[indx + w1 + 1]);
            const float VH_Disc = (fabs(0.5f - VH_Central_Value) < fabs(0.5f - VH_Neighbourhood_Value) ) ? VH_Neighbourhood_Value : VH_Central_Value;
            const float rgb1 = rgb[1][indx];
            const float N1 = eps + fabs(rgb1 - rgb[1][indx - w2]);
            const float S1 = eps + fabs(rgb1 - rgb[1][indx + w2]);
            const float W1 = eps + fabs(rgb1 - rgb[1][indx -  2]);
            const float E1 = eps + fabs(rgb1 - rgb[1][indx +  2]);

            const float rgb1mw1 = rgb[1][indx - w1];
            const float rgb1pw1 = rgb[1][indx + w1];
            const float rgb1m1 =  rgb[1][indx - 1];
            const float rgb1p1 =  rgb[1][indx + 1];

            for(int c = 0; c <= 2; c += 2)
            {
              const float rgbc_mw1 = rgb[c][indx - w1];
              const float rgbc_pw1 = rgb[c][indx + w1];
              const float rgbc_m1  = rgb[c][indx -  1];
              const float rgbc_p1  = rgb[c][indx +  1];

              // Cardinal gradients
              const float N_Grad = N1 + fabs(rgbc_mw1 - rgbc_pw1) + fabs(rgbc_mw1 - rgb[c][indx - w3]);
              const float S_Grad = S1 + fabs(rgbc_pw1 - rgbc_mw1) + fabs(rgbc_pw1 - rgb[c][indx + w3]);
              const float W_Grad = W1 + fabs(rgbc_m1 - rgbc_p1) + fabs(rgbc_m1 - rgb[c][indx -  3]);
              const float E_Grad = E1 + fabs(rgbc_p1 - rgbc_m1) + fabs(rgbc_p1 - rgb[c][indx +  3]);

              // Cardinal colour differences
              const float N_Est = rgbc_mw1 - rgb1mw1;
              const float S_Est = rgbc_pw1 - rgb1pw1;
              const float W_Est = rgbc_m1 - rgb1m1;
              const float E_Est = rgbc_p1 - rgb1p1;

              // Vertical and horizontal estimations
              const float V_Est = (N_Grad * S_Est + S_Grad * N_Est) / (N_Grad + S_Grad);
              const float H_Est = (E_Grad * W_Est + W_Grad * E_Est) / (E_Grad + W_Grad);

              // R@G and B@G interpolation
              rgb[c][indx] = rgb1 + intp(VH_Disc, V_Est, H_Est);
            }
          }
        }
        for(int row = rowStart + RCD_BORDER; row < rowEnd - RCD_BORDER; row++)
        {
          int col = colStart + RCD_BORDER;
          int o_idx = (row * width + col) * 4;
          int idx = (row - rowStart) * RCD_TILESIZE + col - colStart;
          for(; col < colEnd - RCD_BORDER ; col++, o_idx += 4, idx++)
          {
            out[o_idx]   = scaler * CLIP(rgb[0][idx]);
            out[o_idx+1] = scaler * CLIP(rgb[1][idx]);
            out[o_idx+2] = scaler * CLIP(rgb[2][idx]);
          }
        }
      }
    }

    dt_free_align(cfa);
    dt_free_align(rgb);
    dt_free_align(VH_Dir);
    dt_free_align(PQ_Dir);
  }

  rcd_border_interpolate(out, in, cfarray, width, height, RCD_BORDER);
}

#ifdef __GNUC__
  #pragma GCC pop_options
#endif

#undef FCRCD
#undef RCD_BORDER
#undef RCD_TILEVALID
#undef w1
#undef w2
#undef w3
#undef w4
#undef eps
#undef epssq

