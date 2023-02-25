/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "iop/iop_api.h"
#include "common/nlmeans_core.h"
#include <stdbool.h>
#include <stdlib.h>

// to avoid accumulation of rounding errors, we should do a full recomputation of the patch differences
//   every so many rows of the image.  We'll also use that interval as the target maximum chunk size for
//   parallelization
// in addition, to keep the working set within L1 cache, we need to limit the width of the chunks that
//   are processed.  The working set uses (2*radius+3)*(ceil(width/4)+1) + (2*radius+3)*(ceil(width/16)+1)
//   64-byte cache lines.  The typical x86 CPU has an L1 cache containing 256 lines, and we'll need to
//   reserve a few for variables in the stack frame and the like.  That results in a maximal width of
//   96 pixels for radius=2, 72 pixels for radius=3, and 56 for radius=4 (default patch radius is 2)

// lower values for SLICE_HEIGHT reduce the accumulation of rounding errors at the cost of more computation;
//  to avoid excessive overhead, width*height should be at least 2000.  Keeping width*height below 10000 or so
//  will greatly improve L2/L3 cache hit rates and help with scaling beyond 16 threads.  Note that the values
//  specified here are targets and may be adjusted slightly to avoid having extremely small chunks at the
//  right/bottom edge of the images (width will only be reduced, height could be either reduced or increased)
#define SLICE_WIDTH 72
#define SLICE_HEIGHT 60

// try to speed up processing by caching pixel differences?  If cached, they won't need to be computed a
// second time when sliding the patch window away from the pixel.  Testing shows it to be slower than
// recomputing for both scalar and SSE on a Threadripper due to increased memory writes; this may differ on
// architectures with slower multiplication.
//#define CACHE_PIXDIFFS

// number of intermediate buffers used by OpenCL code path.  If you change this, you must also change
//   the definition in src/iop/nlmeans.c and src/iop/denoiseprofile.c
#define NUM_BUCKETS 4

// a structure to collect together the items which define the location of a patch relative to the pixel
//  being denoised
struct patch_t
{
  short rows;		// number of rows difference
  short cols;		// number of columns difference
  int offset;	  	// array distance between corresponding pixels
};
typedef struct patch_t patch_t;

// avoid cluttering the scalar codepath with #ifdefs by hiding the dependency on SSE2
#ifndef __SSE2__
# define _mm_prefetch(where,hint)
#endif

static inline float gh(const float f)
{
  return dt_fast_mexp2f(f) ;
}

static inline int sign(int a)
{
  return (a > 0) - (a < 0);
}

// map the basic row/column offset into a possible much larger offset based on a user parameter
static int scatter(
        const float scale,
        const float scattering,
        const int index1,
        const int index2)
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
static struct patch_t* define_patches(
        const dt_nlmeans_param_t *const params,
        const int stride,
        int *num_patches,
        int *max_shift)
{
  const int search_radius = params->search_radius;
  const float scale = params->scale;
  const float scattering = params->scattering;
  int decimate = params->decimate;
  // determine how many patches we have
  int n_patches = (2 * search_radius + 1) * (2 * search_radius + 1);
  if(decimate)
    n_patches = (n_patches + 1) / 2;
  *num_patches = n_patches ;
  // allocate a cacheline-aligned buffer
  struct patch_t* patches = dt_alloc_align(64, sizeof(struct patch_t) * n_patches);
  // set up the patch offsets
  int patch_num = 0;
  int shift = 0;
  for(int row_index = -search_radius; row_index <= search_radius; row_index++)
  {
    for(int col_index = -search_radius; col_index <= search_radius; col_index++)
    {
      if(decimate && (++decimate & 1)) continue; // skip every other patch
      int r = scatter(scale,scattering,row_index,col_index);
      int c = scatter(scale,scattering,col_index,row_index);
      patches[patch_num].rows = r;
      patches[patch_num].cols = c;
      if(r > shift) shift = r;
      else if(-r > shift) shift = -r;
      if(c > shift) shift = c;
      else if(-c > shift) shift = -c;
      patches[patch_num].offset = (r * stride + c * 4);
      patch_num++;
    }
  }
  *max_shift = shift;
  return patches;
}

static float compute_center_pixel_norm(const float center_weight, const int radius)
{
  // scale the central pixel's contribution by the size of the patch so that the center-weight
  //   setting can be independent of patch size
  const int width = 2 * radius + 1;
  return center_weight * width * width;
}

// compute the channel-normed squared difference between two pixels
static inline float pixel_difference(
        const float* const pix1,
        const float* pix2,
        const dt_aligned_pixel_t norm)
{
  dt_aligned_pixel_t sum = { 0.f, 0.f, 0.f, 0.f };
  for_each_channel(i, aligned(sum:16))
  {
    const float diff = pix1[i] - pix2[i];
    sum[i] = diff * diff * norm[i];
  }
  return sum[0] + sum[1] + sum[2];
}

// optimized: pixel_difference(pix1, pix2, norm) - pixel_difference(pix3, pix4, norm)
static inline float diff_of_pixels_diff(
        const float* const pix1,
        const float* pix2,
        const float* const pix3,
        const float* pix4,
        const dt_aligned_pixel_t norm)
{
  dt_aligned_pixel_t sum = { 0.f, 0.f, 0.f, 0.f };
  for_each_channel(i, aligned(sum:16))
  {
    const float diff1 = pix1[i] - pix2[i];
    const float diff2 = pix3[i] - pix4[i];
    sum[i] = (diff1 * diff1 - diff2 * diff2) * norm[i];
  }
  return sum[0] + sum[1] + sum[2];
}

#if defined(CACHE_PIXDIFFS)
static inline float get_pixdiff(
        const float *const col_sums,
        const int radius,
        const int row,
        const int col)
{
  const int stride = 2*(radius+1);
  const int modrow = 1 + (row + stride) % stride;
  const float *const pixrow = col_sums + (SLICE_WIDTH + 2*radius)*modrow;
  return pixrow[col];
}

static inline void set_pixdiff(
        float *const col_sums,
        const int radius,
        const int row,
        const int col,
        const float diff)
{
  const int stride = 2*(radius+1);
  const int modrow = 1 + (row + stride) % stride;
  float *const pixrow = col_sums + (SLICE_WIDTH + 2*radius)*modrow;
  pixrow[col] = diff;
}

static inline float pixdiff_column_sum(
        const float *const col_sums,
        const int radius,
        const int col)
{
  const int stride = SLICE_WIDTH + 2*radius;
  float sum = col_sums[stride+col];
  for(int i = 2; i <= (2*radius+1) ; i++)
    sum  += col_sums[i*stride+col];
  return sum;
}
#endif

static void init_column_sums(
        float *const col_sums,
        const patch_t *const patch,
        const float *const in,
        const int row,
        const int chunk_left,
        const int chunk_right,
        const int height,
        const int width,
        const int stride,
        const int radius,
        const float *const norm)
{
  // Compute column sums from scratch.  Needed for the very first row, and at intervals thereafter
  //   to limit accumulation of rounding errors

  // figure out which columns can possibly contribute to patches whose centers lie within the RoI
  // we can go up to 'radius' columns beyond the current chunk provided that the patch does not
  // lie in the same direction from the pixel being denoised and that we're still in the RoI
  const int scol = patch->cols;
  const int col_min = chunk_left - MIN(radius,MIN(chunk_left,chunk_left+scol));
  const int col_max = chunk_right + MIN(radius,MIN(width-chunk_right,width-(chunk_right+scol)));
  // adjust bounds if the patch extends past top/bottom of RoI
  const int srow = patch->rows;
  const int rmin = row - MIN(radius,MIN(row,row+srow));
  const int rmax = row + MIN(radius,MIN(height-1-row,height-1-(row+srow)));
  for(int col = chunk_left-radius-1; col < MIN(col_min,chunk_right+radius); col++)
  {
    col_sums[col] = 0;
#ifdef CACHE_PIXDIFFS
    for(int i = row-radius; i <= row+radius; i++)
      set_pixdiff(col_sums,radius,i,col,0.0f);
#endif
  }
  for(int col = col_min; col < col_max; col++)
  {
    float sum = 0;
    for(int r = rmin; r <= rmax; r++)
    {
      const float *pixel = in + r*stride + 4*col;
      const float diff = pixel_difference(pixel,pixel+patch->offset,norm);
#ifdef CACHE_PIXDIFFS
      set_pixdiff(col_sums,radius,r,col,diff);
#endif
      sum += diff;
    }
    col_sums[col] = sum;
  }
  // clear out any columns where the patch column would be outside the RoI, as well as our overrun area
  for(int col = MAX(col_min,col_max); col < chunk_right + radius; col++)
  {
    col_sums[col] = 0;
#ifdef CACHE_PIXDIFFS
    for(int i = row-radius; i <= row+radius; i++)
      set_pixdiff(col_sums,radius,i,col,0.0f);
#endif
  }
  return;
}


// determine the height of the horizontal slice each thread will process
static int compute_slice_height(const int height)
{
  if(height % SLICE_HEIGHT == 0)
    return SLICE_HEIGHT;
  // try to make the heights of the chunks as even as possible
  int best = height % SLICE_HEIGHT;
  int best_incr = 0;
  for(int incr = 1; incr < 10; incr++)
  {
    int plus_rem = height % (SLICE_HEIGHT + incr);
    if(plus_rem == 0)
      return SLICE_HEIGHT + incr;
    else if(plus_rem > best)
    {
      best_incr = +incr;
      best = plus_rem;
    }
    int minus_rem = height % (SLICE_HEIGHT - incr);
    if(minus_rem == 0)
      return SLICE_HEIGHT - incr;
    else if(minus_rem > best)
    {
      best_incr = -incr;
      best = minus_rem;
    }
  }
  return SLICE_HEIGHT + best_incr;
}

// determine the width of the horizontal slice each thread will process
static int compute_slice_width(const int width)
{
  int sl_width = SLICE_WIDTH;
  // if there's just a sliver left over for the last column, see whether slicing a few pixels off each gives
  // us a more nearly full final chunk
  int rem = width % sl_width;
  if(rem < SLICE_WIDTH/2 && (width % (sl_width-4)) > rem)
  {
    sl_width -= 4;
    // check whether removing an additional sliver improves things even more
    rem = width % sl_width;
    if(rem < SLICE_WIDTH/2 && (width % (sl_width-4)) > rem)
      sl_width -= 4;
  }
  return sl_width;
}

__DT_CLONE_TARGETS__
void nlmeans_denoise(
        const float *const inbuf,
        float *const outbuf,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out,
        const dt_nlmeans_param_t *const params)
{
  // define the factors for applying blending between the original image and the denoised version
  // if running in RGB space, 'luma' should equal 'chroma'
  const dt_aligned_pixel_t weight = { params->luma, params->chroma, params->chroma, 1.0f };
  const dt_aligned_pixel_t invert = { 1.0f - params->luma, 1.0f - params->chroma, 1.0f - params->chroma, 0.0f };
  const gboolean skip_blend = (params->luma == 1.0 && params->chroma == 1.0);

  // define the normalization to convert central pixel differences into central pixel weights
  const float cp_norm = compute_center_pixel_norm(params->center_weight,params->patch_radius);
  const dt_aligned_pixel_t center_norm = { cp_norm, cp_norm, cp_norm, 1.0f };

  // define the patches to be compared when denoising a pixel
  const size_t stride = 4 * roi_in->width;
  int num_patches;
  int max_shift;
  struct patch_t* patches = define_patches(params,stride,&num_patches,&max_shift);
  // allocate scratch space, including an overrun area on each end so we don't need a boundary check on every access
  const int radius = params->patch_radius;
#if defined(CACHE_PIXDIFFS)
  const size_t scratch_size = (2*radius+3)*(SLICE_WIDTH + 2*radius + 1);
#else
  const size_t scratch_size = SLICE_WIDTH + 2*radius + 1 + 48; // getting false sharing without the +48....
#endif /* CACHE_PIXDIFFS */
  size_t padded_scratch_size;
  float *const restrict scratch_buf = dt_alloc_perthread_float(scratch_size, &padded_scratch_size);
  const int chk_height = compute_slice_height(roi_out->height);
  const int chk_width = compute_slice_width(roi_out->width);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(patches, num_patches, scratch_buf, padded_scratch_size, chk_height, chk_width, radius) \
      dt_omp_sharedconst(params, roi_out, outbuf, inbuf, stride, center_norm, skip_blend, weight, invert) \
      schedule(static) \
      collapse(2)
#endif
  for(int chunk_top = 0 ; chunk_top < roi_out->height; chunk_top += chk_height)
  {
    for(int chunk_left = 0; chunk_left < roi_out->width; chunk_left += chk_width)
    {
      // locate our scratch space within the big buffer allocated above
      // we'll offset by chunk_left so that we don't have to subtract on every access
      float *const restrict tmpbuf = dt_get_perthread(scratch_buf, padded_scratch_size);
      float *const col_sums =  tmpbuf + (radius+1) - chunk_left;
      // determine which horizontal slice of the image to process
      const int chunk_bot = MIN(chunk_top + chk_height, roi_out->height);
      // determine which vertical slice of the image to process
      const int chunk_right = MIN(chunk_left + chk_width, roi_out->width);
      // we want to incrementally sum results (especially weights in col[3]), so clear the output buffer to zeros
      for(int i = chunk_top; i < chunk_bot; i++)
      {
        memset(outbuf + 4*(i*roi_out->width+chunk_left), '\0', sizeof(float) * 4 * (chunk_right-chunk_left));
      }
      // cycle through all of the patches over our slice of the image
      for(int p = 0; p < num_patches; p++)
      {
        // retrieve info about the current patch
        const patch_t *patch = &patches[p];
        // skip any rows where the patch center would be above top of RoI or below bottom of RoI
        const int height = roi_out->height;
        const int row_min = MAX(chunk_top,MAX(0,-patch->rows));
        const int row_max = MIN(chunk_bot,height - MAX(0,patch->rows));
        // figure out which rows at top and bottom result in patches extending outside the RoI, even though the
        // center pixel is inside
        const int row_top = MAX(row_min,MAX(radius,radius-patch->rows));
        const int row_bot = MIN(row_max,height-1-MAX(radius,radius+patch->rows));
        // skip any columns where the patch center would be to the left or the right of the RoI
        const int width = roi_out->width;
        const int scol = patch->cols;
        const int col_min = MAX(chunk_left,-scol);
        const int col_max = MIN(chunk_right,roi_out->width - scol);

        init_column_sums(col_sums,patch,inbuf,row_min,chunk_left,chunk_right,height,width,
                         stride,radius,params->norm);
        for(int row = row_min; row < row_max; row++)
        {
          // add up the initial columns of the sliding window of total patch distortion
          float distortion = 0.0;
          for(int i = col_min - radius; i < MIN(col_min+radius, col_max); i++)
          {
            distortion += col_sums[i];
          }
          // now proceed down the current row of the image
          const float *in = inbuf + stride * row;
          float *const out = outbuf + (size_t)4 * width * row;
          const int offset = patch->offset;
          const float sharpness = params->sharpness;
          if(params->center_weight < 0)
          {
            // computation as used by denoise(non-local) iop
            for(int col = col_min; col < col_max; col++)
            {
              distortion += (col_sums[col+radius] - col_sums[col-radius-1]);
              const float wt = gh(distortion * sharpness);
              const float *const inpx = in+4*col;
              const dt_aligned_pixel_t pixel = { inpx[offset],  inpx[offset+1], inpx[offset+2], 1.0f };
              for_four_channels(c,aligned(pixel,out:16))
              {
                out[4*col+c] += pixel[c] * wt;
              }
              _mm_prefetch(in+4*col+offset+stride,_MM_HINT_T0);	// try to ensure next row is ready in time
            }
          }
          else
          {
            // computation as used by denoiseprofiled iop with non-local means
            for(int col = col_min; col < col_max; col++)
            {
              distortion += (col_sums[col+radius] - col_sums[col-radius-1]);
              const float dissimilarity = (distortion + pixel_difference(in+4*col,in+4*col+offset,center_norm))
                                           / (1.0f + params->center_weight);
              const float wt = gh(fmaxf(0.0f, dissimilarity * sharpness - 2.0f));
              const float *const inpx = in + 4*col;
              const dt_aligned_pixel_t pixel = { inpx[offset],  inpx[offset+1], inpx[offset+2], 1.0f };
              for_four_channels(c,aligned(pixel,out:16))
              {
                out[4*col+c] += pixel[c] * wt;
              }
              _mm_prefetch(in+4*col+offset+stride,_MM_HINT_T0);	// try to ensure next row is ready in time
            }
          }
          const int pcol_min = chunk_left - MIN(radius,MIN(chunk_left,chunk_left+scol));
          const int pcol_max = chunk_right + MIN(radius,MIN(width-chunk_right,width-(chunk_right+scol)));
          if(row < MIN(row_top, row_bot))
          {
            // top edge of patch was above top of RoI, so it had a value of zero; just add in the new row
            const float *bot_row = inbuf + (row+1+radius)*stride;
            for(int col = pcol_min; col < pcol_max; col++)
            {
              const float *const bot_px = bot_row + 4*col;
              const float diff = pixel_difference(bot_px,bot_px+offset,params->norm);
              _mm_prefetch(bot_px+stride, _MM_HINT_T0);
#ifdef CACHE_PIXDIFFS
              set_pixdiff(col_sums,radius,row+radius+1,col,diff);
#endif
              col_sums[col] += diff;
              _mm_prefetch(bot_px+offset+stride, _MM_HINT_T0);
            }
          }
          else if(row < row_bot)
          {
#ifndef CACHE_PIXDIFFS
            const float *const top_row = inbuf + (row-radius)*stride   /* +(2*radius+1)*stride*/ ;
#endif /* !CACHE_PIXDIFFS */
            const float *const bot_row = inbuf + (row+1+radius)*stride ;
            // both prior and new positions are entirely within the RoI, so subtract the old row and add the new one
            for(int col = pcol_min; col < pcol_max; col++)
            {
#ifdef CACHE_PIXDIFFS
              const float *const bot_px = bot_row + 4*col;
              const float diff = pixel_difference(bot_px,bot_px+offset,params->norm);
              col_sums[col] += diff - get_pixdiff(col_sums,radius,row-radius,col);
              _mm_prefetch(bot_px+stride, _MM_HINT_T0);
              set_pixdiff(col_sums,radius,row+1+radius,col,diff);
#else
              const float *const top_px = top_row + 4*col;
              const float *const bot_px = bot_row + 4*col;
              const float diff = diff_of_pixels_diff(bot_px,bot_px+offset,top_px,top_px+offset,params->norm);
              _mm_prefetch(bot_px+stride, _MM_HINT_T0);
              col_sums[col] += diff;
#endif /* CACHE_PIXDIFFS */
              _mm_prefetch(bot_px+offset+stride, _MM_HINT_T0);
            }
          }
          else if(row >= row_top && row + 1 < row_max) // don't bother updating if last iteration
          {
            // new row of the patch is below the bottom of RoI, so its value is zero; just subtract the old row
#ifndef CACHE_PIXDIFFS
            const float *top_row = inbuf + (row-radius)*stride;
#endif /* !CACHE_PIXDIFFS */
            for(int col = pcol_min; col < pcol_max; col++)
            {
#ifdef CACHE_PIXDIFFS
              col_sums[col] -= get_pixdiff(col_sums,radius,row-radius,col);
#else
              const float *const top_px = top_row + 4*col;
              col_sums[col] -= pixel_difference(top_px,top_px+offset,params->norm);
#endif /* CACHE_PIXDIFFS */
            }
          }
        }
      }
      if(skip_blend)
      {
        // normalize the pixels
        for(int row = chunk_top; row < chunk_bot; row++)
        {
          float *const out = outbuf + 4 * row * roi_out->width;
          for(int col = chunk_left; col < chunk_right; col++)
          {
            for_each_channel(c,aligned(out:16))
            {
              out[4*col+c] /= out[4*col+3];
            }
          }
        }
      }
      else
      {
        // normalize and apply chroma/luma blending
        for(int row = chunk_top; row < chunk_bot; row++)
        {
          const float *in = inbuf + row * stride;
          float *out = outbuf + row * 4 * roi_out->width;
          for(int col = chunk_left; col < chunk_right; col++)
          {
            for_each_channel(c,aligned(in,out,weight,invert:16))
            {
              out[4*col+c] = (in[4*col+c] * invert[c]) + (out[4*col+c] / out[4*col+3] * weight[c]);
            }
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

/**************************************************************/
/**************************************************************/
/*      Everything from here to end of file is OpenCL         */
/**************************************************************/
/**************************************************************/

#ifdef HAVE_OPENCL
static int bucket_next(unsigned int *state, unsigned int max)
{
  unsigned int current = *state;
  unsigned int next = (current >= max - 1 ? 0 : current + 1);

  *state = next;

  return next;
}

static void get_blocksizes(
        int *h,
        int *v,
        const int radius,
        const int devid,
        const int horiz_kernel,
        const int vert_kernel)
{
  dt_opencl_local_buffer_t hlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 2 * radius, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                  .cellsize = sizeof(float), .overhead = 0,
                                  .sizex = 1 << 16, .sizey = 1 };

  *h = dt_opencl_local_buffer_opt(devid, horiz_kernel, &hlocopt) ? hlocopt.sizex : 1;

  dt_opencl_local_buffer_t vlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 1, .xfactor = 1, .yoffset = 2 * radius, .yfactor = 1,
                                  .cellsize = sizeof(float), .overhead = 0,
                                  .sizex = 1, .sizey = 1 << 16 };

  *v = dt_opencl_local_buffer_opt(devid, vert_kernel, &vlocopt) ? vlocopt.sizey : 1;
  return;
}

// zero output pixels, as we will be accumulating them one patch at a time
static inline cl_int nlmeans_cl_init(
        const int devid,
        const int kernel,
        cl_mem dev_out,
        const int height,
        const int width)
{
  return dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
    CLARG(dev_out), CLARG(width), CLARG(height));
}

// horizontal pass, add together columns of each patch
static inline cl_int nlmeans_cl_horiz(
        const int devid,
        const int kernel,
        cl_mem dev_U4,
        cl_mem dev_U4_t,
        const int P,
        const int q[2],
        const int height,
        const int width,
        const int bwidth,
        const int hblocksize)
{
  const size_t sizesl[3] = { bwidth, ROUNDUPDHT(height, devid), 1 };
  const size_t local[3] = { hblocksize, 1, 1 };
  dt_opencl_set_kernel_args(devid, kernel, 0, CLARG(dev_U4), CLARG(dev_U4_t), CLARG(width), CLARG(height), CLARRAY(2, q),
    CLARG(P), CLLOCAL((hblocksize + 2 * P) * sizeof(float)));
  return dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizesl, local);
}

// add difference-weighted proportion of patch-center pixel to output pixel
static inline cl_int nlmeans_cl_accu(
        const int devid,
        const int kernel,
        cl_mem dev_in,
        cl_mem dev_U4_tt,
        cl_mem dev_out,
        const int q[2],
        const int height,
        const int width,
        const size_t sizes[3])
{
  dt_opencl_set_kernel_args(devid, kernel, 0, CLARG(dev_in), CLARG(dev_out), CLARG(dev_U4_tt), CLARG(width),
    CLARG(height), CLARRAY(2, q));
  return dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
}

int nlmeans_denoise_cl(
        const dt_nlmeans_param_t *const params,
        const int devid,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in)
{
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int P = params->patch_radius;
  const float nL2 = params->norm[0] * params->norm[0];
  const float nC2 = params->norm[1] * params->norm[1];

  // define the patches to be compared when denoising a pixel
  const size_t stride = 4 * roi_in->width;
  int num_patches;
  int max_shift;
  struct patch_t* patches = define_patches(params,stride,&num_patches,&max_shift);

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem buckets[NUM_BUCKETS] = { NULL };
  unsigned int state = 0;
  for(int k = 0; k < NUM_BUCKETS; k++)
  {
    buckets[k] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
    if(buckets[k] == NULL) goto error;
  }

  int hblocksize;
  int vblocksize;
  get_blocksizes(&hblocksize, &vblocksize, P, devid, params->kernel_horiz, params->kernel_vert);

  // zero the output buffer into which we will be accumulating results
  err = nlmeans_cl_init(devid,params->kernel_init,dev_out,height,width);
  if(err != CL_SUCCESS) goto error;

  const size_t bwidth = ROUNDUP(width, hblocksize);
  const size_t bheight = ROUNDUP(height, vblocksize);

  for(int p = 0; p < num_patches; p++)
  {
    const patch_t *patch = &patches[p];
    int q[2] = { patch->rows, patch->cols };
    const size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

    // compute channel-normed squared differences between input pixels and shifted (by q) pixels
    cl_mem dev_U4 = buckets[bucket_next(&state, NUM_BUCKETS)];
    dt_opencl_set_kernel_args(devid, params->kernel_dist, 0, CLARG(dev_in), CLARG(dev_U4), CLARG(width),
      CLARG(height), CLARG(q), CLARG(nL2), CLARG(nC2));
    err = dt_opencl_enqueue_kernel_2d(devid, params->kernel_dist, sizes);
    if(err != CL_SUCCESS) break;

    // add up individual columns
    cl_mem dev_U4_t = buckets[bucket_next(&state, NUM_BUCKETS)];
    err = nlmeans_cl_horiz(devid,params->kernel_horiz,dev_U4,dev_U4_t,P,q,height,width,bwidth,hblocksize);
    if(err != CL_SUCCESS) break;

    // add together the column sums and compute the weighting of the current patch for each pixel
    const size_t sizesl[3] = { ROUNDUPDWD(width, devid), bheight, 1 };
    const size_t local[3] = { 1, vblocksize, 1 };
    const float sharpness = params->sharpness;
    cl_mem dev_U4_tt = buckets[bucket_next(&state, NUM_BUCKETS)];
    dt_opencl_set_kernel_args(devid, params->kernel_vert, 0, CLARG(dev_U4_t), CLARG(dev_U4_tt), CLARG(width),
      CLARG(height), CLARG(q), CLARG(P), CLARG(sharpness), CLLOCAL((vblocksize + 2 * P) * sizeof(float)));
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, params->kernel_vert, sizesl, local);
    if(err != CL_SUCCESS) break;

    // add weighted proportion of patch's center pixel to output pixel
    err = nlmeans_cl_accu(devid,params->kernel_accu,dev_in,dev_U4_tt,dev_out,q,height,width,sizes);
    if(err != CL_SUCCESS) break;

    dt_opencl_finish_sync_pipe(devid, params->pipetype);

    // indirectly give gpu some air to breathe (and to do display related stuff)
    dt_iop_nap(dt_opencl_micro_nap(devid));
  }

error:
  // clean up and return status
  dt_free_align(patches);
  for(int k = 0; k < NUM_BUCKETS; k++)
  {
    dt_opencl_release_mem_object(buckets[k]);
  }
  return err;
}

int nlmeans_denoiseprofile_cl(
        const dt_nlmeans_param_t *const params,
        const int devid,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in)
{
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int P = params->patch_radius;
  const float norm = params->sharpness;

  // define the patches to be compared when denoising a pixel
  const size_t stride = 4 * roi_in->width;
  int num_patches;
  int max_shift;
  struct patch_t* patches = define_patches(params,stride,&num_patches,&max_shift);

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem buckets[NUM_BUCKETS] = { NULL };
  unsigned int state = 0;
  for(int k = 0; k < NUM_BUCKETS; k++)
  {
    buckets[k] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
    if(buckets[k] == NULL) goto error;
  }

  int hblocksize;
  int vblocksize;
  get_blocksizes(&hblocksize, &vblocksize, P, devid, params->kernel_horiz, params->kernel_vert);

  // zero the output buffer into which we will be accumulating results
  err = nlmeans_cl_init(devid,params->kernel_init,dev_out,height,width);
  if(err != CL_SUCCESS) goto error;

  const size_t bwidth = ROUNDUP(width, hblocksize);
  const size_t bheight = ROUNDUP(height, vblocksize);

  for(int p = 0; p < num_patches; p++)
  {
    const patch_t *patch = &patches[p];
    int q[2] = { patch->rows, patch->cols };
    const size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

    // compute squared differences between input pixels and shifted (by q) pixels
    cl_mem dev_U4 = buckets[bucket_next(&state, NUM_BUCKETS)];
    dt_opencl_set_kernel_args(devid, params->kernel_dist, 0, CLARG(dev_in), CLARG(dev_U4), CLARG(width),
      CLARG(height), CLARG(q));
    err = dt_opencl_enqueue_kernel_2d(devid, params->kernel_dist, sizes);
    if(err != CL_SUCCESS) break;

    // add up individual columns
    cl_mem dev_U4_t = buckets[bucket_next(&state, NUM_BUCKETS)];
    err = nlmeans_cl_horiz(devid,params->kernel_horiz,dev_U4,dev_U4_t,P,q,height,width,bwidth,hblocksize);
    if(err != CL_SUCCESS) break;

    // add together the column sums and compute the weighting of the current patch for each pixel
    const size_t sizesl[3] = { ROUNDUPDWD(width, devid), bheight, 1 };
    const size_t local[3] = { 1, vblocksize, 1 };
    const float central_pixel_weight = params->center_weight;
    cl_mem dev_U4_tt = buckets[bucket_next(&state, NUM_BUCKETS)];
    dt_opencl_set_kernel_args(devid, params->kernel_vert, 0, CLARG(dev_U4_t), CLARG(dev_U4_tt), CLARG(width),
      CLARG(height), CLARG(q), CLARG(P), CLARG(norm), CLLOCAL((vblocksize + 2 * P) * sizeof(float)),
      CLARG(central_pixel_weight), CLARG(dev_U4));
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, params->kernel_vert, sizesl, local);
    if(err != CL_SUCCESS) break;

    // add weighted proportion of patch's center pixel to output pixel
    err = nlmeans_cl_accu(devid,params->kernel_accu,dev_in,dev_U4_tt,dev_out,q,height,width,sizes);
    if(err != CL_SUCCESS) break;

    dt_opencl_finish_sync_pipe(devid, params->pipetype);

    // indirectly give gpu some air to breathe (and to do display related stuff)
    dt_iop_nap(dt_opencl_micro_nap(devid));
  }

error:
  // clean up and return status
  dt_free_align(patches);
  for(int k = 0; k < NUM_BUCKETS; k++)
  {
    dt_opencl_release_mem_object(buckets[k]);
  }
  return err;
}
#endif /* HAVE_OPENCL */
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
