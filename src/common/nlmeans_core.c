/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
<<<<<<< HEAD
<<<<<<< HEAD
=======
//#include "bauhaus/bauhaus.h"
>>>>>>> New more-scaleable implementation of non-local means
=======
>>>>>>> add support for central pixel weighting, as used by denoiseprofile iop
#include "common/opencl.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "iop/iop_api.h"
<<<<<<< HEAD
<<<<<<< HEAD
=======
//#include <gtk/gtk.h>
>>>>>>> New more-scaleable implementation of non-local means
=======
>>>>>>> add support for central pixel weighting, as used by denoiseprofile iop
#include "common/nlmeans_core.h"
#include <stdbool.h>
#include <stdlib.h>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

// to avoid accumulation of rounding errors, we should do a full recomputation of the patch differences
<<<<<<< HEAD
<<<<<<< HEAD
//   every so many rows of the image.  We'll also use that interval as the target maximum chunk size for
//   parallelization
// lower values reduce the accumulation of rounding errors at the cost of slightly more computation, but
//  also improve cache sharing between threads (on the boundaries of slices), resulting in better scaling
//  because memory bandwidth is less of a limitation.  The value of 20 is a good tradeoff between
//  single-threaded performance and highly-threaded performance; 12 was found to be best on a 32-core
//  third-gen Threadripper.
#define SLICE_HEIGHT 20
=======
//   every so many rows of the image.  We'll also use that interval as the maximum chunk size for
//   parallelization
#define SLICE_HEIGHT 100
>>>>>>> New more-scaleable implementation of non-local means
=======
//   every so many rows of the image.  We'll also use that interval as the target maximum chunk size for
//   parallelization
// lower values reduce the accumulation of rounding errors at the cost of slightly more computation, but
//  also improve cache sharing between threads (on the boundaries of slices), resulting in better scaling
//  because memory bandwidth is less of a limitation.  The value of 20 is a good tradeoff between
//  single-threaded performance and highly-threaded performance; 12 was found to be best on a 32-core
//  third-gen Threadripper.
#define SLICE_HEIGHT 20
>>>>>>> fine-tune slice sizes - trading off a bit of single-threaded speed for better scalability

// a structure to collect together the items which define the location of a patch relative to the pixel
//  being denoised
struct patch_t
{
  short rows;		// number of rows difference
  short cols;		// number of columns difference
  int offset;	  	// array distance between corresponding pixels
};
typedef struct patch_t patch_t;

// some shorthand to make code more legible
// if we have OpenMP simd enabled, declare a vectorizable for loop;
// otherwise, just leave it a plain for()
#if defined(_OPENMP) && defined(OPENMP_SIMD_)
#define SIMD_FOR \
  _Pragma("omp simd") \
  for
#else
#define SIMD_FOR for
#endif


<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> replace exp2f() by much faster integer-hack version
typedef union floatint_t
{
  float f;
  uint32_t i;
} floatint_t;

<<<<<<< HEAD
<<<<<<< HEAD
static inline float gh(const float f)
{
#if 0
  return exp2f(-f);
#else
  // fast integer-hack version of the above
  const int i1 = 0x3f800000; // 2^0
  const int i2 = 0x3f000000; // 2^-1
  const int k0 = i1 + (int)(f * (i2 - i1));
  floatint_t k;
  k.i = k0 >= 0x800000 ? k0 : 0;
  return k.f;
#endif
=======
=======
>>>>>>> replace exp2f() by much faster integer-hack version
static inline float gh(const float f, const float sharpness)
{
#if 0
  return exp2f(-f * sharpness);
<<<<<<< HEAD
>>>>>>> New more-scaleable implementation of non-local means
=======
=======
static inline float gh(const float f)
{
#if 0
  return exp2f(-f);
>>>>>>> add support for central pixel weighting, as used by denoiseprofile iop
#else
  // fast integer-hack version of the above
  const int i1 = 0x3f800000; // 2^0
  const int i2 = 0x3f000000; // 2^-1
  const int k0 = i1 + (int)(f * (i2 - i1));
  floatint_t k;
  k.i = k0 >= 0x800000 ? k0 : 0;
  return k.f;
#endif
>>>>>>> replace exp2f() by much faster integer-hack version
}

static inline int sign(int a)
{
  return (a > 0) - (a < 0);
}

// map the basic row/column offset into a possible much larger offset based on a user parameter
static int scatter(float scale, int scattering, int index1, int index2)
{
  // this formula is designed to
  //  - produce an identity mapping when scattering = 0
  //  - avoiding duplicate patches provided that 0 <= scattering <= 1
  //  - avoiding grid artifacts by trying to take patches on various rows and columns
  const int abs_i1 = abs(index1);
  const int abs_i2 = abs(index2);
  return scale * ((abs_i1 * abs_i1 * abs_i1 + 7.0 * abs_i1 * sqrt(abs_i2)) * sign(index1) * scattering / 6.0 + index1);
}

// allocate and fill an array of patch definitions
static struct patch_t*
define_patches(const dt_nlmeans_param_t *const params, const int stride, int *num_patches, int *max_shift)
{
  const int search_radius = params->search_radius;
  const float scale = params->scale;
  const float scattering = params->scattering;
  int decimate = params->decimate;
<<<<<<< HEAD
<<<<<<< HEAD
=======
  decimate=0;//!!!
>>>>>>> New more-scaleable implementation of non-local means
=======
>>>>>>> enable even faster processing on previews and thumbnails by only evaluating half as many patches
  // determine how many patches we have
  int n_patches = (2 * search_radius + 1) * (2 * search_radius + 1);
  if (decimate)
    n_patches = (n_patches + 1) / 2;
  *num_patches = n_patches ;
  // allocate a cacheline-aligned buffer
  struct patch_t* patches = dt_alloc_align(64,n_patches*sizeof(struct patch_t));
  // set up the patch offsets
  int patch_num = 0;
  int shift = 0;
  for (int row_index = -search_radius; row_index <= search_radius; row_index++)
  {
    for (int col_index = -search_radius; col_index <= search_radius; col_index++)
    {
      if (decimate && (++decimate & 1)) continue; // skip every other patch
      int r = scatter(scale,scattering,row_index,col_index);
      int c = scatter(scale,scattering,col_index,row_index);
      patches[patch_num].rows = r;
      patches[patch_num].cols = c;
      if (r > shift) shift = r;
      else if (-r > shift) shift = -r;
      if (c > shift) shift = c;
      else if (-c > shift) shift = -c;
      patches[patch_num].offset = (r * stride + c * 4);
      patch_num++;
    }
  }
  *max_shift = shift;
  return patches;
}

<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> add support for central pixel weighting, as used by denoiseprofile iop
static float compute_center_pixel_norm(const float center_weight, const int radius)
{
  // scale the central pixel's contribution by the size of the patch so that the center-weight
  //   setting can be independent of patch size
  const int width = 2 * radius + 1;
  return center_weight * width * width;
}

<<<<<<< HEAD
=======
>>>>>>> New more-scaleable implementation of non-local means
=======
>>>>>>> add support for central pixel weighting, as used by denoiseprofile iop
// compute the channel-normed squared difference between two pixels
static inline float pixel_difference(const float* const pix1, const float* pix2, const float norm[4])
{
  float dif1 = pix1[0] - pix2[0];
  float dif2 = pix1[1] - pix2[1];
  float dif3 = pix1[2] - pix2[2];
  return (dif1 * dif1 * norm[0]) + (dif2 * dif2 * norm[1]) + (dif3 * dif3 * norm[2]);
}

<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> add SSE implementation of nlmeans_denoise
#if defined(__SSE__)
// compute the channel-normed squared difference between two pixels; don't do horizontal sum until later
static inline __m128 channel_difference_sse2(const float* const pix1, const float* pix2, const float norm[4])
{
  const __m128 px1 = _mm_load_ps(pix1);
  const __m128 px2 = _mm_load_ps(pix2);
  const __m128 n = _mm_load_ps(norm);
  const __m128 dif = px1 - px2;
  return dif * dif * n;
}
#endif /* __SSE__ */

#if defined(__SSE__)
// compute the channel-normed squared difference between two pixels
static inline float pixel_difference_sse2(const float* const pix1, const float* pix2, const float norm[4])
{
  const __m128 px1 = _mm_load_ps(pix1);
  const __m128 px2 = _mm_load_ps(pix2);
  const __m128 n = _mm_load_ps(norm);
  const __m128 dif = px1 - px2;
  const __m128 ssd = dif * dif * n;
  return ssd[0] + ssd[1] + ssd[2];
}
#endif /* __SSE__ */

<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> New more-scaleable implementation of non-local means
=======

>>>>>>> add SSE implementation of nlmeans_denoise
=======
>>>>>>> add support for central pixel weighting, as used by denoiseprofile iop
static void init_column_sums(float *const col_sums, const patch_t *const patch, const float *const in,
                             const int row, const int height, const int width, const int stride,
                             const int radius, const float *const norm)
{
  const int srow = row + patch->rows;
  const int scol = patch->cols;
  const int col_min = MAX(0,-scol);        // columns left of this correspond to a patch column outside the RoI
  const int col_max = width - MAX(0,scol); // columns right of this correspond to a patch column outside the RoI
  const float *pixel = in + 4*col_min;
  const float *shifted = pixel + patch->offset;
  // Compute column sums from scratch.  Needed for the very first row, and at intervals thereafter
  //   to limit accumulation of rounding errors
  // adjust bounds if the patch extends past top/bottom of RoI
  const int rmin = MAX(-radius,MAX(MIN(-srow,0),-row));
  const int rmax = MIN(radius,MIN(height-srow,height-row)-1);
  for (int col = 0; col < col_min; col++)
    col_sums[col] = 0;
  for (int col = col_min; col < col_max; col++, pixel+=4, shifted+=4)
  {
    float sum = 0;
    for (int r = rmin; r <= rmax; r++)
    {
      sum += pixel_difference(pixel+r*stride,shifted+r*stride,norm);
    }
    col_sums[col] = sum;
  }
  // clear out any columns where the patch column would be outside the RoI, as well as our overrun area
  for (int col = col_max; col < width + radius; col++)
    col_sums[col] = 0;
  return;
}

<<<<<<< HEAD
<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> add SSE implementation of nlmeans_denoise
#if defined(__SSE__)
static void init_column_sums_sse2(float *const col_sums, const patch_t *const patch, const float *const in,
                                  const int row, const int height, const int width, const int stride,
                                  const int radius, const float *const norm)
{
  const int srow = row + patch->rows;
  const int scol = patch->cols;
  const int col_min = MAX(0,-scol);        // columns left of this correspond to a patch column outside the RoI
  const int col_max = width - MAX(0,scol); // columns right of this correspond to a patch column outside the RoI
  const float *pixel = in + 4*col_min;
  const float *shifted = pixel + patch->offset;
  // Compute column sums from scratch.  Needed for the very first row, and at intervals thereafter
  //   to limit accumulation of rounding errors
  // adjust bounds if the patch extends past top/bottom of RoI
  const int rmin = MAX(-radius,MAX(MIN(-srow,0),-row));
  const int rmax = MIN(radius,MIN(height-srow,height-row)-1);
  for (int col = 0; col < col_min; col++)
    col_sums[col] = 0;
  for (int col = col_min; col < col_max; col++, pixel+=4, shifted+=4)
  {
    __m128 sum = { 0, 0, 0, 0};
    for (int r = rmin; r <= rmax; r++)
    {
      sum += channel_difference_sse2(pixel+r*stride,shifted+r*stride,norm);
    }
    col_sums[col] = sum[0] + sum[1] + sum[2];
  }
  // clear out any columns where the patch column would be outside the RoI, as well as our overrun area
  for (int col = col_max; col < width + radius; col++)
    col_sums[col] = 0;
  return;
}
#endif /* __SSE__ */

<<<<<<< HEAD
=======
>>>>>>> fine-tune slice sizes - trading off a bit of single-threaded speed for better scalability
=======
>>>>>>> add SSE implementation of nlmeans_denoise
// determine the height of the horizontal slice each thread will process
static int compute_slice_size(const int height)
{
  // dt_get_num_threads() always returns the number of processors, even if the user has requested a different
  //   number of threads, so go directly to the source
  const int numthreads = darktable.num_openmp_threads;
  const int base_chunk_size = MIN(SLICE_HEIGHT, (height + numthreads - 1) / numthreads);
  // tweak the basic size a bit so that the number of chunks ends up an exact multiple of the
  //   number of threads
  const int num_chunks = (height + base_chunk_size - 1) / base_chunk_size;
<<<<<<< HEAD
<<<<<<< HEAD
  const int low = MAX(1,numthreads * (num_chunks / numthreads));
=======
  const int low = numthreads * (num_chunks / numthreads);
>>>>>>> fine-tune slice sizes - trading off a bit of single-threaded speed for better scalability
=======
  const int low = MAX(1,numthreads * (num_chunks / numthreads));
>>>>>>> fix division-by-zero on thumbnail processing
  const int high = numthreads * ((num_chunks + numthreads - 1) / numthreads);
  const int chunk_size_low = (height + low - 1) / low;
  const int chunk_size_high = (height + high - 1) / high;
  const int diff_low = chunk_size_low - base_chunk_size;
  const int diff_high = base_chunk_size - chunk_size_high;
<<<<<<< HEAD
<<<<<<< HEAD
  return diff_high <= diff_low ?  chunk_size_high : chunk_size_low;
}

=======
>>>>>>> New more-scaleable implementation of non-local means
=======
  fprintf(stderr,"ht=%d, base=%d, #chk=%d, lo=%d, hi=%d, szlo=%d, szhi=%d, diflo=%d, difhi=%d\n",height,base_chunk_size,num_chunks,low,high,chunk_size_low,chunk_size_high,diff_low,diff_high);
=======
>>>>>>> remove debugging printf
  return diff_high <= diff_low ?  chunk_size_high : chunk_size_low;
}

>>>>>>> fine-tune slice sizes - trading off a bit of single-threaded speed for better scalability
void nlmeans_denoise(const float *const inbuf, float *const outbuf,
                     const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                     const dt_nlmeans_param_t *const params)
{
  // define the factors for applying blending between the original image and the denoised version
  // if running in RGB space, 'luma' should equal 'chroma'
  const float weight[4] = { params->luma, params->chroma, params->chroma, 1.0f };
  const float invert[4] = { 1.0f - params->luma, 1.0f - params->chroma, 1.0f - params->chroma, 0.0f };
<<<<<<< HEAD
<<<<<<< HEAD
=======
>>>>>>> add support for central pixel weighting, as used by denoiseprofile iop
  const bool skip_blend = (params->luma == 1.0 && params->chroma == 1.0);

  // define the normalization to convert central pixel differences into central pixel weights
  const float cp_norm = compute_center_pixel_norm(params->center_weight,params->patch_radius);
  const float center_norm[4] = { cp_norm, cp_norm, cp_norm, 1.0f };
  
<<<<<<< HEAD
=======

>>>>>>> New more-scaleable implementation of non-local means
=======
>>>>>>> add support for central pixel weighting, as used by denoiseprofile iop
  // define the patches to be compared when denoising a pixel
  const size_t stride = 4 * roi_in->width;
  int num_patches;
  int max_shift;
  struct patch_t* patches = define_patches(params,stride,&num_patches,&max_shift);
  // allocate scratch space, including an overrun area on each end so we don't need a boundary check on every access
  const int scratch_size = (roi_out->width + 2*params->patch_radius + 1) ;
  const int padded_scratch_size = ((scratch_size+15)/16)*16; // round up to a full cache line
  const int numthreads = dt_get_num_threads() ;
  float *scratch_buf = dt_alloc_align(64,numthreads * padded_scratch_size * sizeof(float));
  // zero out the overrun areas
  memset(scratch_buf,'\0',numthreads*padded_scratch_size*sizeof(float));
<<<<<<< HEAD
<<<<<<< HEAD
  const int chunk_size = compute_slice_size(roi_out->height);
  const int num_chunks = (roi_out->height + chunk_size - 1) / chunk_size;
#ifdef _OPENMP
#pragma omp parallel for default(none) num_threads(darktable.num_openmp_threads) \
      dt_omp_firstprivate(patches, num_patches, scratch_buf) \
      schedule(static)
#endif
  for (int chk = 0 ; chk < num_chunks; chk++)
=======
  const int chunk_size = (roi_out->height + numthreads - 1) / numthreads;
=======
  const int chunk_size = compute_slice_size(roi_out->height);
  const int num_chunks = (roi_out->height + chunk_size - 1) / chunk_size;
<<<<<<< HEAD
  fprintf(stderr,"ch_size=%d, n_chks=%d, #thr=%d\n",chunk_size,num_chunks,numthreads);
>>>>>>> fine-tune slice sizes - trading off a bit of single-threaded speed for better scalability
=======
>>>>>>> remove debugging printf
#ifdef _OPENMP
#pragma omp parallel for default(none) num_threads(darktable.num_openmp_threads) \
      dt_omp_firstprivate(patches, num_patches, scratch_buf) \
      schedule(static)
#endif
<<<<<<< HEAD
  for (int thr = 0 ; thr < numthreads; thr++)
>>>>>>> New more-scaleable implementation of non-local means
=======
  for (int chk = 0 ; chk < num_chunks; chk++)
>>>>>>> fine-tune slice sizes - trading off a bit of single-threaded speed for better scalability
  {
    // locate our scratch space within the big buffer allocated above
    size_t tnum = dt_get_thread_num();
    const int radius = params->patch_radius;
    float *const col_sums = scratch_buf + tnum * padded_scratch_size + radius + 1;
    // determine which horizontal slice of the image to process
<<<<<<< HEAD
<<<<<<< HEAD
    const int chunk_start = chk * chunk_size;
    const int chunk_end = MIN((chk+1)*chunk_size,roi_out->height);
=======
    const int chunk_start = thr * chunk_size;
    const int chunk_end = MIN((thr+1)*chunk_size,roi_out->height);
>>>>>>> New more-scaleable implementation of non-local means
=======
    const int chunk_start = chk * chunk_size;
    const int chunk_end = MIN((chk+1)*chunk_size,roi_out->height);
>>>>>>> fine-tune slice sizes - trading off a bit of single-threaded speed for better scalability
    // we want to incrementally sum results (especially weights in col[3]), so clear the output buffer to zeros
    memset(outbuf+chunk_start*roi_out->width*4, '\0', roi_out->width * (chunk_end-chunk_start) * 4 * sizeof(float));
    // cycle through all of the patches over our slice of the image
    for (int p = 0; p < num_patches; p++)
    {
      // retrieve info about the current patch
      const patch_t *patch = &patches[p];
<<<<<<< HEAD
<<<<<<< HEAD
      // skip any rows where the patch center would be above top of RoI or below bottom of RoI
      const int height = roi_out->height;
      const int row_min = MAX(chunk_start,MAX(0,-patch->rows));
      const int row_max = MIN(chunk_end,height - MAX(0,patch->rows));
      // figure out which rows at top and bottom result in patches extending outside the RoI, even though the
      // center pixel is inside
      const int row_top = MAX(row_min,MAX(radius,radius-patch->rows));
      const int row_bot = MIN(row_max,height-MAX(radius+1,radius+patch->rows+1));
=======
      const float sharpness = params->sharpness;
      const int offset = patch->offset;
      // skip any rows where the patch center would be above top of RoI or below bottom of RoI
      const int height = roi_out->height;
      int row_min = MAX(chunk_start,MAX(0,-patch->rows));
      int row_max = MIN(chunk_end,height - MAX(0,patch->rows));
>>>>>>> New more-scaleable implementation of non-local means
=======
      // skip any rows where the patch center would be above top of RoI or below bottom of RoI
      const int height = roi_out->height;
      const int row_min = MAX(chunk_start,MAX(0,-patch->rows));
      const int row_max = MIN(chunk_end,height - MAX(0,patch->rows));
      // figure out which rows at top and bottom result in patches extending outside the RoI, even though the
      // center pixel is inside
      const int row_top = MAX(row_min,MAX(radius,radius-patch->rows));
      const int row_bot = MIN(row_max,height-MAX(radius+1,radius+patch->rows+1));
>>>>>>> fine-tune slice sizes - trading off a bit of single-threaded speed for better scalability
      // skip any columns where the patch center would be to the left or the right of the RoI
      const int scol = patch->cols;
      const int col_min = MAX(0,-scol);
      const int col_max = roi_out->width - MAX(0,scol);
      init_column_sums(col_sums,patch,inbuf+stride*row_min,row_min,height,roi_out->width,stride,radius,params->norm);
      for (int row = row_min; row < row_max; row++)
      {
        // add up the initial columns of the sliding window of total patch distortion
        float distortion = 0.0;
        for (int i = col_min - radius; i < col_min+radius; i++)
          distortion += col_sums[i];
        // now proceed down the current row of the image
        const float *in = inbuf + stride * row + 4 * col_min;
        float *out = outbuf + 4 * ((size_t)roi_out->width * row + col_min);
<<<<<<< HEAD
<<<<<<< HEAD
        const int offset = patch->offset;
        const float sharpness = params->sharpness;
        if (params->center_weight < 0)
        {
          // computation as used by denoise(non-local) iop
          for (int col = col_min; col < col_max; col++, in+=4, out+=4)
          {
            distortion += (col_sums[col+radius] - col_sums[col-radius-1]);
            const float wt = gh(distortion * sharpness);
            const float pixel[4] = { in[offset],  in[offset+1], in[offset+2], 1.0f };
            SIMD_FOR (size_t c = 0; c < 4; c++)
            {
              out[c] += pixel[c] * wt;
            }
          }
        }
        else
        {
          // computation as used by denoiseprofiled iop with non-local means
          for (int col = col_min; col < col_max; col++, in+=4, out+=4)
          {
            distortion += (col_sums[col+radius] - col_sums[col-radius-1]);
//TODO
            const float dissimilarity = (distortion + pixel_difference(in,in+offset,center_norm)
                                         / (1.0f + params->center_weight));
            const float wt = gh(fmaxf(0.0f, dissimilarity * params->norm[0] - 2.0f));
            const float pixel[4] = { in[offset],  in[offset+1], in[offset+2], 1.0f };
            SIMD_FOR (size_t c = 0; c < 4; c++)
            {
              out[c] += pixel[c] * wt;
            }
          }
        }
        if (row < row_top)
        {
          // top edge of patch was above top of RoI, so it had a value of zero; just add in the new row
          const float *pixel_bot = inbuf + (row+1+radius)*stride + 4*col_min;
          for (int col = col_min; col < col_max; col++, pixel_bot+=4)
          {
            col_sums[col] += pixel_difference(pixel_bot,pixel_bot+offset,params->norm);
          }
        }
        else if (row >= row_bot)
        {
          if (row + 1 < row_max) // don't bother updating if last iteration
          {
            // new row of the patch is below the bottom of RoI, so its value is zero; just subtract the old row
            const float *pixel_top = inbuf + (row-radius)*stride + 4*col_min;
            for (int col = col_min; col < col_max; col++, pixel_top+=4)
            {
              col_sums[col] -= pixel_difference(pixel_top,pixel_top+offset,params->norm);
            }
          }
        }
        else
        {
          const float *pixel_top = inbuf + (row-radius)*stride + 4*col_min;
          const float *pixel_bot = inbuf + (row+1+radius)*stride + 4*col_min;
          // both prior and new positions are entirely within the RoI, so subtract the old row and add the new one
          for (int col = col_min; col < col_max; col++, pixel_top+=4, pixel_bot+=4)
          {
            col_sums[col] += (pixel_difference(pixel_bot,pixel_bot+offset,params->norm)
                              - pixel_difference(pixel_top,pixel_top+offset,params->norm));
          }
        }
      }
    }
    if (skip_blend)
    {
      // normalize the pixels
      for (int row = chunk_start; row < chunk_end; row++)
      {
        float *out = outbuf + row * 4 * roi_out->width;
        for (int col = 0; col < roi_out->width; col++, out+=4)
        {
          SIMD_FOR(size_t c = 0; c < 4; c++)
          {
            out[c] /= out[3];
          }
        }
      }
    }
    else
    {
      // normalize and apply chroma/luma blending
      for (int row = chunk_start; row < chunk_end; row++)
      {
        const float *in = inbuf + row * stride;
        float *out = outbuf + row * 4 * roi_out->width;
        for (int col = 0; col < roi_out->width; col++, in+=4, out+=4)
        {
          SIMD_FOR(size_t c = 0; c < 4; c++)
          {
            out[c] = (in[c] * invert[c]) + (out[c] / out[3] * weight[c]);
          }
        }
      }
    }
  }

  // clean up: free the work space
  dt_free_align(patches);
  dt_free_align(scratch_buf);
  return;
}

#if defined(__SSE__)
void nlmeans_denoise_sse2(const float *const inbuf, float *const outbuf,
                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                          const dt_nlmeans_param_t *const params)
{
  // define the factors for applying blending between the original image and the denoised version
  // if running in RGB space, 'luma' should equal 'chroma'
  const __m128 weight = { params->luma, params->chroma, params->chroma, 1.0f };
  const __m128 invert = { 1.0f - params->luma, 1.0f - params->chroma, 1.0f - params->chroma, 0.0f };
  const bool skip_blend = (params->luma == 1.0 && params->chroma == 1.0);

  // define the normalization to convert central pixel differences into central pixel weights
  const float cp_norm = compute_center_pixel_norm(params->center_weight,params->patch_radius);
  const float center_norm[4] = { cp_norm, cp_norm, cp_norm, 1.0f };
  
  // define the patches to be compared when denoising a pixel
  const size_t stride = 4 * roi_in->width;
  int num_patches;
  int max_shift;
  struct patch_t* patches = define_patches(params,stride,&num_patches,&max_shift);
  // allocate scratch space, including an overrun area on each end so we don't need a boundary check on every access
  const int scratch_size = (roi_out->width + 2*params->patch_radius + 1) ;
  const int padded_scratch_size = ((scratch_size+15)/16)*16; // round up to a full cache line
  const int numthreads = dt_get_num_threads() ;
  float *scratch_buf = dt_alloc_align(64,numthreads * padded_scratch_size * sizeof(float));
  // zero out the overrun areas
  memset(scratch_buf,'\0',numthreads*padded_scratch_size*sizeof(float));
  const int chunk_size = compute_slice_size(roi_out->height);
  const int num_chunks = (roi_out->height + chunk_size - 1) / chunk_size;
#ifdef _OPENMP
#pragma omp parallel for default(none) num_threads(darktable.num_openmp_threads) \
      dt_omp_firstprivate(patches, num_patches, scratch_buf) \
      schedule(static)
#endif
  for (int chk = 0 ; chk < num_chunks; chk++)
  {
    // locate our scratch space within the big buffer allocated above
    size_t tnum = dt_get_thread_num();
    const int radius = params->patch_radius;
    float *const col_sums = scratch_buf + tnum * padded_scratch_size + radius + 1;
    // determine which horizontal slice of the image to process
    const int chunk_start = chk * chunk_size;
    const int chunk_end = MIN((chk+1)*chunk_size,roi_out->height);
    // we want to incrementally sum results (especially weights in col[3]), so clear the output buffer to zeros
    memset(outbuf+chunk_start*roi_out->width*4, '\0', roi_out->width * (chunk_end-chunk_start) * 4 * sizeof(float));
    // cycle through all of the patches over our slice of the image
    for (int p = 0; p < num_patches; p++)
    {
      // retrieve info about the current patch
      const patch_t *patch = &patches[p];
      // skip any rows where the patch center would be above top of RoI or below bottom of RoI
      const int height = roi_out->height;
      const int row_min = MAX(chunk_start,MAX(0,-patch->rows));
      const int row_max = MIN(chunk_end,height - MAX(0,patch->rows));
      // figure out which rows at top and bottom result in patches extending outside the RoI, even though the
      // center pixel is inside
      const int row_top = MAX(row_min,MAX(radius,radius-patch->rows));
      const int row_bot = MIN(row_max,height-MAX(radius+1,radius+patch->rows+1));
      // skip any columns where the patch center would be to the left or the right of the RoI
      const int scol = patch->cols;
      const int col_min = MAX(0,-scol);
      const int col_max = roi_out->width - MAX(0,scol);
      init_column_sums_sse2(col_sums,patch,inbuf+stride*row_min,row_min,height,roi_out->width,stride,radius,
                            params->norm);
      for (int row = row_min; row < row_max; row++)
      {
        // add up the initial columns of the sliding window of total patch distortion
        float distortion = 0.0;
        for (int i = col_min - radius; i < col_min+radius; i++)
          distortion += col_sums[i];
        // now proceed down the current row of the image
        const float *in = inbuf + stride * row + 4 * col_min;
        float *out = outbuf + 4 * ((size_t)roi_out->width * row + col_min);
        const int offset = patch->offset;
        const float sharpness = params->sharpness;
        if (params->center_weight < 0)
<<<<<<< HEAD
        {
          // computation as used by denoise(non-local) iop
          for (int col = col_min; col < col_max; col++, in+=4, out+=4)
          {
            distortion += (col_sums[col+radius] - col_sums[col-radius-1]);
            const __m128 wt = _mm_set1_ps(gh(distortion * sharpness));
            __m128 pixel = _mm_load_ps(in+offset);
            pixel[3] = 1.0f;
            const __m128 outpx = _mm_load_ps(out);
            _mm_store_ps(out, outpx + (pixel * wt));
          }
        }
        else
        {
          // computation as used by denoiseprofiled iop with non-local means
          for (int col = col_min; col < col_max; col++, in+=4, out+=4)
          {
            distortion += (col_sums[col+radius] - col_sums[col-radius-1]);
//TODO
            const float dissimilarity = (distortion + pixel_difference_sse2(in,in+offset,center_norm)
                                         / (1.0f + params->center_weight));
            const __m128 wt = _mm_set1_ps(gh(fmaxf(0.0f, dissimilarity * params->norm[0] - 2.0f)));
            __m128 pixel = _mm_load_ps(in+offset);
            pixel[3] = 1.0f;
            const __m128 outpx = _mm_load_ps(out);
            _mm_store_ps(out, outpx + (pixel * wt));
          }
        }
        if (row < row_top)
        {
          // top edge of patch was above top of RoI, so it had a value of zero; just add in the new row
          const float *pixel_bot = inbuf + (row+1+radius)*stride + 4*col_min;
          for (int col = col_min; col < col_max; col++, pixel_bot+=4)
          {
            col_sums[col] += pixel_difference_sse2(pixel_bot,pixel_bot+offset,params->norm);
          }
        }
        else if (row >= row_bot)
        {
          if (row + 1 < row_max) // don't bother updating if last iteration
          {
            // new row of the patch is below the bottom of RoI, so its value is zero; just subtract the old row
            const float *pixel_top = inbuf + (row-radius)*stride + 4*col_min;
            for (int col = col_min; col < col_max; col++, pixel_top+=4)
            {
              col_sums[col] -= pixel_difference_sse2(pixel_top,pixel_top+offset,params->norm);
            }
          }
        }
        else
        {
          const float *pixel_top = inbuf + (row-radius)*stride + 4*col_min;
          const float *pixel_bot = inbuf + (row+1+radius)*stride + 4*col_min;
          // both prior and new positions are entirely within the RoI, so subtract the old row and add the new one
          for (int col = col_min; col < col_max; col++, pixel_top+=4, pixel_bot+=4)
          {
            __m128 dif = (channel_difference_sse2(pixel_bot,pixel_bot+offset,params->norm)
                          - channel_difference_sse2(pixel_top,pixel_top+offset,params->norm));
            col_sums[col] += (dif[0] + dif[1] + dif[2]);
          }
        }
      }
    }
    if (skip_blend)
    {
      // normalize the pixels
      for (int row = chunk_start; row < chunk_end; row++)
      {
        float *out = outbuf + row * 4 * roi_out->width;
        for (int col = 0; col < roi_out->width; col++, out+=4)
        {
          const __m128 outpx = _mm_load_ps(out);
          const __m128 scale = _mm_set1_ps(out[3]);
          _mm_stream_ps(out, outpx / scale) ;
        }
      }
    }
    else
    {
      // normalize and apply chroma/luma blending
      for (int row = chunk_start; row < chunk_end; row++)
      {
        const float *in = inbuf + row * stride;
        float *out = outbuf + row * 4 * roi_out->width;
        for (int col = 0; col < roi_out->width; col++, in+=4, out+=4)
        {
          const __m128 inpx = _mm_load_ps(in);
          const __m128 outpx = _mm_load_ps(out);
          const __m128 scale = _mm_set1_ps(out[3]);
          _mm_stream_ps(out, (inpx * invert) + (outpx / scale * weight)) ;
=======
=======
        const int offset = patch->offset;
        const float sharpness = params->sharpness;
>>>>>>> fine-tune slice sizes - trading off a bit of single-threaded speed for better scalability
        for (int col = col_min; col < col_max; col++, in+=4, out+=4)
=======
>>>>>>> add support for central pixel weighting, as used by denoiseprofile iop
        {
          // computation as used by denoise(non-local) iop
          for (int col = col_min; col < col_max; col++, in+=4, out+=4)
          {
            distortion += (col_sums[col+radius] - col_sums[col-radius-1]);
            const float wt = gh(distortion * sharpness);
            const float pixel[4] = { in[offset],  in[offset+1], in[offset+2], 1.0f };
            SIMD_FOR (size_t c = 0; c < 4; c++)
            {
              out[c] += pixel[c] * wt;
            }
          }
        }
        else
        {
          // computation as used by denoiseprofiled iop with non-local means
          for (int col = col_min; col < col_max; col++, in+=4, out+=4)
          {
            distortion += (col_sums[col+radius] - col_sums[col-radius-1]);
//TODO
            const float dissimilarity = (distortion + pixel_difference(in,in+offset,center_norm)
                                         / (1.0f + params->center_weight));
            const float wt = gh(fmaxf(0.0f, dissimilarity * params->norm[0] - 2.0f));
            const float pixel[4] = { in[offset],  in[offset+1], in[offset+2], 1.0f };
            SIMD_FOR (size_t c = 0; c < 4; c++)
            {
              out[c] += pixel[c] * wt;
            }
          }
        }
        if (row < row_top)
        {
          // top edge of patch was above top of RoI, so it had a value of zero; just add in the new row
          const float *pixel_bot = inbuf + (row+1+radius)*stride + 4*col_min;
          for (int col = col_min; col < col_max; col++, pixel_bot+=4)
          {
            col_sums[col] += pixel_difference(pixel_bot,pixel_bot+offset,params->norm);
          }
        }
        else if (row >= row_bot)
        {
          if (row + 1 < row_max) // don't bother updating if last iteration
          {
            // new row of the patch is below the bottom of RoI, so its value is zero; just subtract the old row
            const float *pixel_top = inbuf + (row-radius)*stride + 4*col_min;
            for (int col = col_min; col < col_max; col++, pixel_top+=4)
            {
              col_sums[col] -= pixel_difference(pixel_top,pixel_top+offset,params->norm);
            }
          }
        }
        else
        {
          const float *pixel_top = inbuf + (row-radius)*stride + 4*col_min;
          const float *pixel_bot = inbuf + (row+1+radius)*stride + 4*col_min;
          // both prior and new positions are entirely within the RoI, so subtract the old row and add the new one
          for (int col = col_min; col < col_max; col++, pixel_top+=4, pixel_bot+=4)
          {
            col_sums[col] += (pixel_difference(pixel_bot,pixel_bot+offset,params->norm)
                              - pixel_difference(pixel_top,pixel_top+offset,params->norm));
          }
        }
      }
    }
    if (skip_blend)
    {
      // normalize the pixels
      for (int row = chunk_start; row < chunk_end; row++)
      {
        float *out = outbuf + row * 4 * roi_out->width;
        for (int col = 0; col < roi_out->width; col++, out+=4)
        {
<<<<<<< HEAD
          out[c] = (in[c] * invert[c]) + (out[c] / out[3] * weight[c]);
>>>>>>> New more-scaleable implementation of non-local means
=======
          SIMD_FOR(size_t c = 0; c < 4; c++)
          {
            out[c] /= out[3];
          }
        }
      }
    }
    else
    {
      // normalize and apply chroma/luma blending
      for (int row = chunk_start; row < chunk_end; row++)
      {
        const float *in = inbuf + row * stride;
        float *out = outbuf + row * 4 * roi_out->width;
        for (int col = 0; col < roi_out->width; col++, in+=4, out+=4)
        {
          SIMD_FOR(size_t c = 0; c < 4; c++)
          {
            out[c] = (in[c] * invert[c]) + (out[c] / out[3] * weight[c]);
          }
>>>>>>> add support for central pixel weighting, as used by denoiseprofile iop
        }
      }
    }
  }

  // clean up: free the work space
  dt_free_align(patches);
  dt_free_align(scratch_buf);
  return;
}
<<<<<<< HEAD
<<<<<<< HEAD
#endif /* __SSE__ */
=======
>>>>>>> New more-scaleable implementation of non-local means
=======

#if defined(__SSE__)
void nlmeans_denoise_sse2(const float *const inbuf, float *const outbuf,
                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                          const dt_nlmeans_param_t *const params)
{
  // define the factors for applying blending between the original image and the denoised version
  // if running in RGB space, 'luma' should equal 'chroma'
  const __m128 weight = { params->luma, params->chroma, params->chroma, 1.0f };
  const __m128 invert = { 1.0f - params->luma, 1.0f - params->chroma, 1.0f - params->chroma, 0.0f };
  const bool skip_blend = (params->luma == 1.0 && params->chroma == 1.0);

  // define the normalization to convert central pixel differences into central pixel weights
  const float cp_norm = compute_center_pixel_norm(params->center_weight,params->patch_radius);
  const float center_norm[4] = { cp_norm, cp_norm, cp_norm, 1.0f };
  
  // define the patches to be compared when denoising a pixel
  const size_t stride = 4 * roi_in->width;
  int num_patches;
  int max_shift;
  struct patch_t* patches = define_patches(params,stride,&num_patches,&max_shift);
  // allocate scratch space, including an overrun area on each end so we don't need a boundary check on every access
  const int scratch_size = (roi_out->width + 2*params->patch_radius + 1) ;
  const int padded_scratch_size = ((scratch_size+15)/16)*16; // round up to a full cache line
  const int numthreads = dt_get_num_threads() ;
  float *scratch_buf = dt_alloc_align(64,numthreads * padded_scratch_size * sizeof(float));
  // zero out the overrun areas
  memset(scratch_buf,'\0',numthreads*padded_scratch_size*sizeof(float));
  const int chunk_size = compute_slice_size(roi_out->height);
  const int num_chunks = (roi_out->height + chunk_size - 1) / chunk_size;
#ifdef _OPENMP
#pragma omp parallel for default(none) num_threads(darktable.num_openmp_threads) \
      dt_omp_firstprivate(patches, num_patches, scratch_buf) \
      schedule(static)
#endif
  for (int chk = 0 ; chk < num_chunks; chk++)
  {
    // locate our scratch space within the big buffer allocated above
    size_t tnum = dt_get_thread_num();
    const int radius = params->patch_radius;
    float *const col_sums = scratch_buf + tnum * padded_scratch_size + radius + 1;
    // determine which horizontal slice of the image to process
    const int chunk_start = chk * chunk_size;
    const int chunk_end = MIN((chk+1)*chunk_size,roi_out->height);
    // we want to incrementally sum results (especially weights in col[3]), so clear the output buffer to zeros
    memset(outbuf+chunk_start*roi_out->width*4, '\0', roi_out->width * (chunk_end-chunk_start) * 4 * sizeof(float));
    // cycle through all of the patches over our slice of the image
    for (int p = 0; p < num_patches; p++)
    {
      // retrieve info about the current patch
      const patch_t *patch = &patches[p];
      // skip any rows where the patch center would be above top of RoI or below bottom of RoI
      const int height = roi_out->height;
      const int row_min = MAX(chunk_start,MAX(0,-patch->rows));
      const int row_max = MIN(chunk_end,height - MAX(0,patch->rows));
      // figure out which rows at top and bottom result in patches extending outside the RoI, even though the
      // center pixel is inside
      const int row_top = MAX(row_min,MAX(radius,radius-patch->rows));
      const int row_bot = MIN(row_max,height-MAX(radius+1,radius+patch->rows+1));
      // skip any columns where the patch center would be to the left or the right of the RoI
      const int scol = patch->cols;
      const int col_min = MAX(0,-scol);
      const int col_max = roi_out->width - MAX(0,scol);
      init_column_sums_sse2(col_sums,patch,inbuf+stride*row_min,row_min,height,roi_out->width,stride,radius,
                            params->norm);
      for (int row = row_min; row < row_max; row++)
      {
        // add up the initial columns of the sliding window of total patch distortion
        float distortion = 0.0;
        for (int i = col_min - radius; i < col_min+radius; i++)
          distortion += col_sums[i];
        // now proceed down the current row of the image
        const float *in = inbuf + stride * row + 4 * col_min;
        float *out = outbuf + 4 * ((size_t)roi_out->width * row + col_min);
        const int offset = patch->offset;
        const float sharpness = params->sharpness;
        if (params->center_weight < 0)
        {
          // computation as used by denoise(non-local) iop
          for (int col = col_min; col < col_max; col++, in+=4, out+=4)
          {
            distortion += (col_sums[col+radius] - col_sums[col-radius-1]);
            const __m128 wt = _mm_set1_ps(gh(distortion * sharpness));
            __m128 pixel = _mm_load_ps(in+offset);
            pixel[3] = 1.0f;
            const __m128 outpx = _mm_load_ps(out);
            _mm_store_ps(out, outpx + (pixel * wt));
          }
        }
        else
        {
          // computation as used by denoiseprofiled iop with non-local means
          for (int col = col_min; col < col_max; col++, in+=4, out+=4)
          {
            distortion += (col_sums[col+radius] - col_sums[col-radius-1]);
//TODO
            const float dissimilarity = (distortion + pixel_difference_sse2(in,in+offset,center_norm)
                                         / (1.0f + params->center_weight));
            const __m128 wt = _mm_set1_ps(gh(fmaxf(0.0f, dissimilarity * params->norm[0] - 2.0f)));
            __m128 pixel = _mm_load_ps(in+offset);
            pixel[3] = 1.0f;
            const __m128 outpx = _mm_load_ps(out);
            _mm_store_ps(out, outpx + (pixel * wt));
          }
        }
        if (row < row_top)
        {
          // top edge of patch was above top of RoI, so it had a value of zero; just add in the new row
          const float *pixel_bot = inbuf + (row+1+radius)*stride + 4*col_min;
          for (int col = col_min; col < col_max; col++, pixel_bot+=4)
          {
            col_sums[col] += pixel_difference_sse2(pixel_bot,pixel_bot+offset,params->norm);
          }
        }
        else if (row >= row_bot)
        {
          if (row + 1 < row_max) // don't bother updating if last iteration
          {
            // new row of the patch is below the bottom of RoI, so its value is zero; just subtract the old row
            const float *pixel_top = inbuf + (row-radius)*stride + 4*col_min;
            for (int col = col_min; col < col_max; col++, pixel_top+=4)
            {
              col_sums[col] -= pixel_difference_sse2(pixel_top,pixel_top+offset,params->norm);
            }
          }
        }
        else
        {
          const float *pixel_top = inbuf + (row-radius)*stride + 4*col_min;
          const float *pixel_bot = inbuf + (row+1+radius)*stride + 4*col_min;
          // both prior and new positions are entirely within the RoI, so subtract the old row and add the new one
          for (int col = col_min; col < col_max; col++, pixel_top+=4, pixel_bot+=4)
          {
            __m128 dif = (channel_difference_sse2(pixel_bot,pixel_bot+offset,params->norm)
                          - channel_difference_sse2(pixel_top,pixel_top+offset,params->norm));
            col_sums[col] += (dif[0] + dif[1] + dif[2]);
          }
        }
      }
    }
    if (skip_blend)
    {
      // normalize the pixels
      for (int row = chunk_start; row < chunk_end; row++)
      {
        float *out = outbuf + row * 4 * roi_out->width;
        for (int col = 0; col < roi_out->width; col++, out+=4)
        {
          const __m128 outpx = _mm_load_ps(out);
          const __m128 scale = _mm_set1_ps(out[3]);
          _mm_stream_ps(out, outpx / scale) ;
        }
      }
    }
    else
    {
      // normalize and apply chroma/luma blending
      for (int row = chunk_start; row < chunk_end; row++)
      {
        const float *in = inbuf + row * stride;
        float *out = outbuf + row * 4 * roi_out->width;
        for (int col = 0; col < roi_out->width; col++, in+=4, out+=4)
        {
          const __m128 inpx = _mm_load_ps(in);
          const __m128 outpx = _mm_load_ps(out);
          const __m128 scale = _mm_set1_ps(out[3]);
          _mm_stream_ps(out, (inpx * invert) + (outpx / scale * weight)) ;
        }
      }
    }
  }

  // clean up: free the work space
  dt_free_align(patches);
  dt_free_align(scratch_buf);
  return;
}
#endif /* __SSE__ */
>>>>>>> add SSE implementation of nlmeans_denoise
