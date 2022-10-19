/*
    This file is part of darktable,
    Copyright (C) 2017-2020 darktable developers.

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
#include "common/imagebuf.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "dwt.h"
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

/* Based on the original source code of GIMP's Wavelet Decompose plugin, by Marco Rossini
 *
 * http://registry.gimp.org/node/11742
 *
*/

dwt_params_t *dt_dwt_init(float *image, const int width, const int height, const int ch, const int scales,
                          const int return_layer, const int merge_from_scale, void *user_data,
                          const float preview_scale, const int use_sse)
{
  dwt_params_t *p = (dwt_params_t *)malloc(sizeof(dwt_params_t));
  if(!p) return NULL;

  p->image = image;
  p->ch = ch;
  p->width = width;
  p->height = height;
  p->scales = scales;
  p->return_layer = return_layer;
  p->merge_from_scale = merge_from_scale;
  p->user_data = user_data;
  p->preview_scale = preview_scale;
  p->use_sse = use_sse;

  return p;
}

void dt_dwt_free(dwt_params_t *p)
{
  if(!p) return;

  free(p);
}

static int _get_max_scale(const int width, const int height, const float preview_scale)
{
  int maxscale = 0;

  // smallest edge must be higher than or equal to 2^scales
  unsigned int size = MIN(width, height);
  float size_tmp = ((size >>= 1) * preview_scale);
  while(size_tmp > 0.f)
  {
    size_tmp = ((size >>= 1) * preview_scale);
    maxscale++;
  }

  // avoid rounding issues...
  size = MIN(width, height);
  while((maxscale > 0) && ((1 << maxscale) * preview_scale >= size)) maxscale--;

  return maxscale;
}

int dwt_get_max_scale(dwt_params_t *p)
{
  return _get_max_scale(p->width / p->preview_scale, p->height / p->preview_scale, p->preview_scale);
}

static int _first_scale_visible(const int num_scales, const float preview_scale)
{
  int first_scale = 0;

  for(unsigned int lev = 0; lev < num_scales; lev++)
  {
    int sc = 1 << lev;
    sc *= preview_scale;
    if(sc > 0)
    {
      first_scale = lev + 1;
      break;
    }
  }

  return first_scale;
}

int dt_dwt_first_scale_visible(dwt_params_t *p)
{
  return _first_scale_visible(p->scales, p->preview_scale);
}

static void dwt_get_image_layer(float *const layer, dwt_params_t *const p)
{
  if(p->image != layer) memcpy(p->image, layer, sizeof(float) * p->width * p->height * p->ch);
}

// first, "vertical" pass of wavelet decomposition
static void dwt_decompose_vert(float *const restrict out, const float *const restrict in,
                               const size_t height, const size_t width, const size_t lev)
{
  const size_t vscale = MIN(1 << lev, height-1);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, width, vscale) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(int rowid = 0; rowid < height ; rowid++)
  {
    const size_t row = dwt_interleave_rows(rowid,height,vscale);
    // perform a weighted sum of the current pixel row with the rows 'scale' pixels above and below
    // if either of those is beyond the edge of the image, we use reflection to get a value for averaging,
    // i.e. we move as many rows in from the edge as we would have been beyond the edge
    // for the top edge, this means we can simply use the absolute value of row-vscale; for the bottom edge,
    //   we need to reflect around height
    const size_t rowstart = (size_t)4 * row * width;
    const size_t above_row = (row > vscale) ? row - vscale : vscale - row;
    const size_t below_row = (row + vscale < height) ? (row + vscale) : 2*(height-1) - (row + vscale);
    const float* const restrict center = in + rowstart;
    const float* const restrict above = in + 4 * above_row * width;
    const float* const restrict below = in + 4 * below_row * width;
    float* const restrict temprow = out + rowstart;
    for(size_t col = 0; col < 4*width; col += 4)
    {
      for_each_channel(c,aligned(center, above, below, temprow : 16))
      {
        temprow[col + c] = 2.f * center[col+c] + above[col+c] + below[col+c];
      }
    }
  }
}

// second, horizontal pass of wavelet decomposition; generates 'coarse' into the output buffer and overwrites
//   the input buffer with 'details'
static void dwt_decompose_horiz(float *const restrict out, float *const restrict in, float *const temp,
                                const size_t height, const size_t width, const size_t lev)
{
  const int hscale = MIN(1 << lev, width);  //(int because we need a signed difference below)
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, width, hscale) \
  dt_omp_sharedconst(in, out, temp) \
  schedule(static)
#endif
  for(int row = 0; row < height ; row++)
  {
    // perform a weighted sum of the current pixel with the ones 'scale' pixels to the left and right, using
    // reflection to get a value if either of those positions is out of bounds, i.e. we move as many columns
    // in from the edge as we would have been beyond the edge to avoid an additional pass, we also rescale the
    // final sum and split the original input into 'coarse' and 'details' by subtracting the scaled sum from
    // the original input.
    const size_t rowindex = (size_t)4 * (row * width);
    float* const restrict temprow = temp + width * dt_get_thread_num() * 4;
    float* const restrict details = in + rowindex;
    float* const restrict coarse = out + rowindex;

    for(int col = 0; col < width - hscale; col++)
    {
      const size_t leftpos = (size_t)4*abs(col-hscale);	// the abs() handles reflection at the left edge
      const size_t rightpos = (size_t)4*(col+hscale);
      for_each_channel(c,aligned(temprow, details, coarse : 16))
      {
        const float left = coarse[leftpos+c];
        const float right = coarse[rightpos+c];
        // add up left/center/right, and renormalize by dividing by the total weight of all numbers added together
        const float hat = (2.f * coarse[4*col+c] + left + right) / 16.f;
        // the normalized value is our 'coarse' result; 'details' is the difference between original input and 'coarse'
        temprow[4*col+c] = hat;
        details[4*col+c] -= hat;
      }
    }
    // handle reflection at right edge
    for(int col = width - hscale; col < width; col++)
    {
      const size_t leftpos = (size_t)4 * abs(col-hscale); // still need to handle reflection, if hscale>=width/2
      const size_t rightpos = (size_t)4 * (2*width - 2 - (col+hscale));
      for_each_channel(c,aligned(temprow, details, coarse : 16))
      {
        const float left = coarse[leftpos+c];
        const float right = coarse[rightpos+c];
        // add up left/center/right, and renormalize by dividing by the total weight of all numbers added together
        const float hat = (2.f * coarse[4*col+c] + left + right) / 16.f;
        // the normalized value is our 'coarse' result; 'details' is the difference between original input and 'coarse'
        temprow[4*col+c] = hat;
        details[4*col+c] -= hat;
      }
    }
    // now that we're done with the row of pixels, we can overwrite the intermediate result from the
    // first pass with the final decomposition
    memcpy(coarse, temprow, sizeof(float) * 4 * width);
  }
}

// split input into 'coarse' and 'details'; put 'details' back into the input buffer
static void dwt_decompose_layer(float *const restrict out, float *const restrict in, float *const temp, const int lev,
                                const dwt_params_t *const p)
{
  dwt_decompose_vert(out, in, p->height, p->width, lev);
  dwt_decompose_horiz(out, in, temp, p->height, p->width, lev);
  return;
}

/* actual decomposing algorithm */
static void dwt_wavelet_decompose(float *img, dwt_params_t *const p, _dwt_layer_func layer_func)
{
  float *temp = NULL;
  float *layers = NULL;
  float *merged_layers = NULL;
  float *buffer[2] = { 0, 0 };
  int bcontinue = 1;
  const size_t size = (size_t)p->width * p->height * p->ch;

  assert(p->ch == 4);

  if(layer_func) layer_func(img, p, 0);

  if(p->scales <= 0) goto cleanup;

  /* image buffers */
  buffer[0] = img;
  /* temporary storage */
  buffer[1] = dt_alloc_align_float(size);
  // buffer to reconstruct the image
  layers = dt_alloc_align_float((size_t)4 * p->width * p->height);
  // scratch buffer for decomposition
  temp = dt_alloc_align_float(dt_get_num_threads() * 4 * p->width);

  if(buffer[1] == NULL || layers == NULL || temp == NULL)
  {
    printf("not enough memory for wavelet decomposition");
    goto cleanup;
  }
  dt_iop_image_fill(layers,0.0f,p->width,p->height,p->ch);

  if(p->merge_from_scale > 0)
  {
    merged_layers = dt_alloc_align_float((size_t)p->width * p->height * p->ch);
    if(merged_layers == NULL)
    {
      printf("not enough memory for wavelet decomposition");
      goto cleanup;
    }
    dt_iop_image_fill(merged_layers,0.0f,p->width,p->height,p->ch);
  }

  // iterate over wavelet scales
  unsigned int hpass = 0;
  for(unsigned int lev = 0; lev < p->scales && bcontinue; lev++)
  {
    unsigned int lpass = (1 - (lev & 1));

    dwt_decompose_layer(buffer[lpass], buffer[hpass], temp, lev, p);

    // no merge scales or we didn't reach the merge scale from yet
    if(p->merge_from_scale == 0 || p->merge_from_scale > lev + 1)
    {
      // allow to process this detail scale
      if(layer_func) layer_func(buffer[hpass], p, lev + 1);

      // user wants to preview this detail scale
      if(p->return_layer == lev + 1)
      {
        // return this detail scale
        dwt_get_image_layer(buffer[hpass], p);

        bcontinue = 0;
      }
      // user wants the entire reconstructed image
      else if(p->return_layer == 0)
      {
        // add this detail scale to the final image
        dt_iop_image_add_image(layers, buffer[hpass], p->width, p->height, p->ch);
      }
    }
    // we are on the merge scales range
    else
    {
      // add this detail scale to the merged ones
      dt_iop_image_add_image(merged_layers, buffer[hpass], p->width, p->height, p->ch);

      // allow to process this merged scale
      if(layer_func) layer_func(merged_layers, p, lev + 1);

      // user wants to preview this merged scale
      if(p->return_layer == lev + 1)
      {
        // return this merged scale
        dwt_get_image_layer(merged_layers, p);

        bcontinue = 0;
      }
    }

    hpass = lpass;
  }

  // all scales have been processed
  if(bcontinue)
  {
    // allow to process residual image
    if(layer_func) layer_func(buffer[hpass], p, p->scales + 1);

    // user wants to preview residual image
    if(p->return_layer == p->scales + 1)
    {
      // return residual image
      dwt_get_image_layer(buffer[hpass], p);
    }
    // return reconstructed image
    else if(p->return_layer == 0)
    {
      // some of the detail scales are on the merged layers
      if(p->merge_from_scale > 0)
      {
        // add merged layers to final image
        dt_iop_image_add_image(layers, merged_layers, p->width, p->height, p->ch);
      }

      // add residual image to final image
      dt_iop_image_add_image(layers, buffer[hpass], p->width, p->height, p->ch);

      // allow to process reconstructed image
      if(layer_func) layer_func(layers, p, p->scales + 2);

      // return reconstructed image
      dwt_get_image_layer(layers, p);
    }
  }

cleanup:
  if(temp) dt_free_align(temp);
  if(layers) dt_free_align(layers);
  if(merged_layers) dt_free_align(merged_layers);
  if(buffer[1]) dt_free_align(buffer[1]);
}

/* this function prepares for decomposing, which is done in the function dwt_wavelet_decompose() */
void dwt_decompose(dwt_params_t *p, _dwt_layer_func layer_func)
{
  // this is a zoom scale, not a wavelet scale
  if(p->preview_scale <= 0.f) p->preview_scale = 1.f;

  // if a single scale is requested it cannot be grather than the residual
  if(p->return_layer > p->scales + 1)
  {
    p->return_layer = p->scales + 1;
  }

  const int max_scale = dwt_get_max_scale(p);

  // if requested scales is grather than max scales adjust it
  if(p->scales > max_scale)
  {
    // residual should be returned
    if(p->return_layer > p->scales) p->return_layer = max_scale + 1;
    // a scale should be returned, it cannot be grather than max scales
    else if(p->return_layer > max_scale)
      p->return_layer = max_scale;

    p->scales = max_scale;
  }

  // call the actual decompose
  dwt_wavelet_decompose(p->image, p, layer_func);
}

// first, "vertical" pass of wavelet decomposition
static void dwt_denoise_vert_1ch(float *const restrict out, const float *const restrict in,
                                 const size_t height, const size_t width, const size_t lev)
{
  const int vscale = MIN(1 << lev, height);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, width, vscale) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(int rowid = 0; rowid < height ; rowid++)
  {
    const int row = dwt_interleave_rows(rowid,height,vscale);
    // perform a weighted sum of the current pixel row with the rows 'scale' pixels above and below
    // if either of those is beyond the edge of the image, we use reflection to get a value for averaging,
    // i.e. we move as many rows in from the edge as we would have been beyond the edge
    // for the top edge, this means we can simply use the absolute value of row-vscale; for the bottom edge,
    //   we need to reflect around height
    const size_t rowstart = (size_t)row * width;
    const size_t below_row = (row + vscale < height) ? (row + vscale) : 2*(height-1) - (row + vscale);
    const float *const restrict center = in + rowstart;
    const float *const restrict above =  in + abs(row - vscale) * width;
    const float *const restrict below = in + below_row * width;
    float* const restrict outrow = out + rowstart;
#ifdef _OPENMP
#pragma omp simd
#endif
    for(int col= 0; col < width; col++)
    {
      outrow[col] = 2.f * center[col] + above[col] + below[col];
    }
  }
}

// second, horizontal pass of wavelet decomposition; generates 'coarse' into the output buffer and overwrites
//   the input buffer with 'details'
static void dwt_denoise_horiz_1ch(float *const restrict out, float *const restrict in,
                                  float *const restrict accum, const size_t height, const size_t width,
                                  const size_t lev, const float thold, const int last)
{
  const int hscale = MIN(1 << lev, width);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, width, hscale, thold, last) \
  dt_omp_sharedconst(in, out, accum) \
  schedule(static)
#endif
  for(int row = 0; row < height ; row++)
  {
    // perform a weighted sum of the current pixel with the ones 'scale' pixels to the left and right, using
    // reflection to get a value if either of those positions is out of bounds, i.e. we move as many columns
    // in from the edge as we would have been beyond the edge to avoid an additional pass, we also rescale the
    // final sum and split the original input into 'coarse' and 'details' by subtracting the scaled sum from
    // the original input.
    const size_t rowindex = (size_t)row * width;
    float *const restrict details = in + rowindex;
    float *const restrict coarse = out + rowindex;
    float *const restrict accum_row = accum + rowindex;
    // handle reflection at left edge
#ifdef _OPENMP
#pragma omp simd
#endif
    for(int col = 0; col < hscale; col++)
    {
      // add up left/center/right, and renormalize by dividing by the total weight of all numbers added together
      const float hat = (2.f * coarse[col] + coarse[hscale-col] + coarse[col+hscale]) / 16.f;
      // the normalized value is our 'coarse' result; 'diff' is the difference between original input and 'coarse'
      // (which would ordinarily be stored as the details scale, but we don't need it any further)
      const float diff = details[col] - hat;
      details[col] = hat;		// done with original input, so we can overwrite it with 'coarse'
      // GCC8 won't vectorize if we use the following line, but it turns out that just adding the two conditional
      // alternatives produces exactly the same result, and *that* does get vectorized
      //const float excess = diff < 0.0 ? MIN(diff + thold, 0.0f) : MAX(diff - thold, 0.0f);
      accum_row[col] += MAX(diff - thold,0.0f) + MIN(diff + thold, 0.0f);
    }
#ifdef _OPENMP
#pragma omp simd
#endif
    for(int col = hscale; col < width - hscale; col++)
    {
      // add up left/center/right, and renormalize by dividing by the total weight of all numbers added together
      const float hat = (2.f * coarse[col] + coarse[col-hscale] + coarse[col+hscale]) / 16.f;
      // the normalized value is our 'coarse' result; 'diff' is the difference between original input and 'coarse'
      // (which would ordinarily be stored as the details scale, but we don't need it any further)
      const float diff = details[col] - hat;
      details[col] = hat;		// done with original input, so we can overwrite it with 'coarse'
      // GCC8 won't vectorize if we use the following line, but it turns out that just adding the two conditional
      // alternatives produces exactly the same result, and *that* does get vectorized
      //const float excess = diff < 0.0 ? MIN(diff + thold, 0.0f) : MAX(diff - thold, 0.0f);
      accum_row[col] += MAX(diff - thold,0.0f) + MIN(diff + thold, 0.0f);
    }
    // handle reflection at right edge
#ifdef _OPENMP
#pragma omp simd
#endif
    for(int col = width - hscale; col < width; col++)
    {
      const float right = coarse[2*width - 2 - (col+hscale)];
      // add up left/center/right, and renormalize by dividing by the total weight of all numbers added together
      const float hat = (2.f * coarse[col] + coarse[col-hscale] + right) / 16.f;
      // the normalized value is our 'coarse' result; 'diff' is the difference between original input and 'coarse'
      // (which would ordinarily be stored as the details scale, but we don't need it any further)
      const float diff = details[col] - hat;
      details[col] = hat;		// done with original input, so we can overwrite it with 'coarse'
      accum_row[col] += MAX(diff - thold,0.0f) + MIN(diff + thold, 0.0f);
    }
    if(last)
    {
      // add the details to the residue to create the final denoised result
      for(int col = 0; col < width; col++)
      {
        details[col] += accum_row[col];
      }
    }
  }
}

/* this function denoises an image by decomposing it into the specified number of wavelet scales and
 * recomposing the result from just the portion of each scale which exceeds the magnitude of the given
 * threshold for that scale.
 */
void dwt_denoise(float *const img, const int width, const int height, const int bands, const float *const noise)
{
  float *const details = dt_alloc_align_float((size_t)2 * width * height);
  float *const interm = details + width * height;	// temporary storage for use during each pass

  // zero the accumulator
  dt_iop_image_fill(details, 0.0f, width, height, 1);

  for(int lev = 0; lev < bands; lev++)
  {
    const int last = (lev+1) == bands;

    // "vertical" pass, averages pixels with those 'scale' rows above and below and puts result in 'interm'
    dwt_denoise_vert_1ch(interm, img, height, width, lev);
    // horizontal filtering pass, averages pixels in 'interm' with those 'scale' rows to the left and right
    // accumulates the portion of the detail scale that is above the noise threshold into 'details'; this
    // will be added to the residue left in 'img' on the last iteration
    dwt_denoise_horiz_1ch(interm, img, details, height, width, lev, noise[lev], last);
  }
  dt_free_align(details);
}

#ifdef HAVE_OPENCL
dt_dwt_cl_global_t *dt_dwt_init_cl_global()
{
  dt_dwt_cl_global_t *g = (dt_dwt_cl_global_t *)malloc(sizeof(dt_dwt_cl_global_t));

  const int program = 20; // dwt.cl, from programs.conf
  g->kernel_dwt_add_img_to_layer = dt_opencl_create_kernel(program, "dwt_add_img_to_layer");
  g->kernel_dwt_subtract_layer = dt_opencl_create_kernel(program, "dwt_subtract_layer");
  g->kernel_dwt_hat_transform_col = dt_opencl_create_kernel(program, "dwt_hat_transform_col");
  g->kernel_dwt_hat_transform_row = dt_opencl_create_kernel(program, "dwt_hat_transform_row");
  g->kernel_dwt_init_buffer = dt_opencl_create_kernel(program, "dwt_init_buffer");
  return g;
}

void dt_dwt_free_cl_global(dt_dwt_cl_global_t *g)
{
  if(!g) return;

  // destroy kernels
  dt_opencl_free_kernel(g->kernel_dwt_add_img_to_layer);
  dt_opencl_free_kernel(g->kernel_dwt_subtract_layer);
  dt_opencl_free_kernel(g->kernel_dwt_hat_transform_col);
  dt_opencl_free_kernel(g->kernel_dwt_hat_transform_row);
  dt_opencl_free_kernel(g->kernel_dwt_init_buffer);

  free(g);
}

dwt_params_cl_t *dt_dwt_init_cl(const int devid, cl_mem image, const int width, const int height, const int scales,
                                const int return_layer, const int merge_from_scale, void *user_data,
                                const float preview_scale)
{
  dwt_params_cl_t *p = (dwt_params_cl_t *)malloc(sizeof(dwt_params_cl_t));
  if(!p) return NULL;

  p->global = darktable.opencl->dwt;
  p->devid = devid;
  p->image = image;
  p->ch = 4;
  p->width = width;
  p->height = height;
  p->scales = scales;
  p->return_layer = return_layer;
  p->merge_from_scale = merge_from_scale;
  p->user_data = user_data;
  p->preview_scale = preview_scale;

  return p;
}

void dt_dwt_free_cl(dwt_params_cl_t *p)
{
  if(!p) return;

  // be sure we're done with the memory:
  dt_opencl_finish(p->devid);

  free(p);
}

int dwt_get_max_scale_cl(dwt_params_cl_t *p)
{
  return _get_max_scale(p->width / p->preview_scale, p->height / p->preview_scale, p->preview_scale);
}

int dt_dwt_first_scale_visible_cl(dwt_params_cl_t *p)
{
  return _first_scale_visible(p->scales, p->preview_scale);
}

static cl_int dwt_subtract_layer_cl(cl_mem bl, cl_mem bh, dwt_params_cl_t *const p)
{
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  const int devid = p->devid;
  const int kernel = p->global->kernel_dwt_subtract_layer;


  const float lpass_mult = (1.f / 16.f);
  const int width = p->width;
  const int height = p->height;

  size_t sizes[] = { ROUNDUPDWD(p->width, devid), ROUNDUPDHT(p->height, devid), 1 };
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&bl);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&bh);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(width));
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&(height));
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), (void *)&lpass_mult);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);

  return err;
}

static cl_int dwt_add_layer_cl(cl_mem img, cl_mem layers, dwt_params_cl_t *const p, const int n_scale)
{
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  const int devid = p->devid;
  const int kernel = p->global->kernel_dwt_add_img_to_layer;


  const int width = p->width;
  const int height = p->height;

  size_t sizes[] = { ROUNDUPDWD(p->width, devid), ROUNDUPDHT(p->height, devid), 1 };
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&img);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&layers);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(width));
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&(height));
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);

  return err;
}

static cl_int dwt_get_image_layer_cl(cl_mem layer, dwt_params_cl_t *const p)
{
  cl_int err = CL_SUCCESS;

  if(p->image != layer)
    err = dt_opencl_enqueue_copy_buffer_to_buffer(p->devid, layer, p->image, 0, 0,
                                                  (size_t)p->width * p->height * p->ch * sizeof(float));

  return err;
}

static cl_int dwt_wavelet_decompose_cl(cl_mem img, dwt_params_cl_t *const p, _dwt_layer_func_cl layer_func)
{
  cl_int err = CL_SUCCESS;

  const int devid = p->devid;

  cl_mem temp = NULL;
  cl_mem layers = NULL;
  cl_mem merged_layers = NULL;
  unsigned int lpass, hpass;
  cl_mem buffer[2] = { 0, 0 };
  int bcontinue = 1;

  if(layer_func)
  {
    err = layer_func(img, p, 0);
    if(err != CL_SUCCESS) goto cleanup;
  }

  if(p->scales <= 0) goto cleanup;

  /* image buffers */
  buffer[0] = img;
  /* temporary storage */
  buffer[1] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * p->ch * p->width * p->height);
  if(buffer[1] == NULL)
  {
    printf("not enough memory for wavelet decomposition");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  // buffer to reconstruct the image
  layers = dt_opencl_alloc_device_buffer(devid, sizeof(float) * p->ch * p->width * p->height);
  if(layers == NULL)
  {
    printf("not enough memory for wavelet decomposition");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }
  // init layer buffer
  {
    const int kernel = p->global->kernel_dwt_init_buffer;

    const int width = p->width;
    const int height = p->height;

    size_t sizes[] = { ROUNDUPDWD(p->width, devid), ROUNDUPDHT(p->height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&layers);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(int), (void *)&(width));
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(height));
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto cleanup;
  }

  if(p->merge_from_scale > 0)
  {
    merged_layers = dt_opencl_alloc_device_buffer(devid, sizeof(float) * p->ch * p->width * p->height);
    if(merged_layers == NULL)
    {
      printf("not enough memory for wavelet decomposition");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    // init reconstruct buffer
    {
      const int kernel = p->global->kernel_dwt_init_buffer;

      const int width = p->width;
      const int height = p->height;

      size_t sizes[] = { ROUNDUPDWD(p->width, devid), ROUNDUPDHT(p->height, devid), 1 };
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&merged_layers);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(int), (void *)&(width));
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(height));
      err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
      if(err != CL_SUCCESS) goto cleanup;
    }
  }

  // iterate over wavelet scales
  lpass = 1;
  hpass = 0;
  for(unsigned int lev = 0; lev < p->scales && bcontinue; lev++)
  {
    lpass = (1 - (lev & 1));

    // when (*layer_func) uses too much memory I get a -4 error, so alloc and free for each scale
    // setup a temp buffer
    temp = dt_opencl_alloc_device_buffer(devid, sizeof(float) * p->ch * p->width * p->height);
    if(temp == NULL)
    {
      printf("not enough memory for wavelet decomposition");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }

    // hat transform by row
    {
      const int kernel = p->global->kernel_dwt_hat_transform_row;

      int sc = 1 << lev;
      sc = (int)(sc * p->preview_scale);
      if(sc > p->width) sc = p->width;

      size_t sizes[] = { ROUNDUPDWD(p->width, devid), ROUNDUPDHT(p->height, devid), 1 };

      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&temp);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&(buffer[hpass]));
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(p->width));
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&(p->height));
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), (void *)&sc);
      err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
      if(err != CL_SUCCESS) goto cleanup;
    }

    // hat transform by col
    {
      const int kernel = p->global->kernel_dwt_hat_transform_col;

      int sc = 1 << lev;
      sc = (int)(sc * p->preview_scale);
      if(sc > p->height) sc = p->height;
      const float lpass_mult = (1.f / 16.f);

      size_t sizes[] = { ROUNDUPDWD(p->width, devid), ROUNDUPDHT(p->height, devid), 1 };

      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&temp);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(int), (void *)&(p->width));
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(p->height));
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&sc);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), (void *)&(buffer[lpass]));
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), (void *)&lpass_mult);
      err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
      if(err != CL_SUCCESS) goto cleanup;
    }

    if(temp)
    {
      dt_opencl_release_mem_object(temp);
      temp = NULL;
    }

    err = dwt_subtract_layer_cl(buffer[lpass], buffer[hpass], p);
    if(err != CL_SUCCESS) goto cleanup;

    // no merge scales or we didn't reach the merge scale from yet
    if(p->merge_from_scale == 0 || p->merge_from_scale > lev + 1)
    {
      // allow to process this detail scale
      if(layer_func)
      {
        err = layer_func(buffer[hpass], p, lev + 1);
        if(err != CL_SUCCESS) goto cleanup;
      }

      // user wants to preview this detail scale
      if(p->return_layer == lev + 1)
      {
        // return this detail scale
        err = dwt_get_image_layer_cl(buffer[hpass], p);
        if(err != CL_SUCCESS) goto cleanup;

        bcontinue = 0;
      }
      // user wants the entire reconstructed image
      else if(p->return_layer == 0)
      {
        // add this detail scale to the final image
        err = dwt_add_layer_cl(buffer[hpass], layers, p, lev + 1);
        if(err != CL_SUCCESS) goto cleanup;
      }
    }
    // we are on the merge scales range
    else
    {
      // add this detail scale to the merged ones
      err = dwt_add_layer_cl(buffer[hpass], merged_layers, p, lev + 1);
      if(err != CL_SUCCESS) goto cleanup;

      // allow to process this merged scale
      if(layer_func)
      {
        err = layer_func(merged_layers, p, lev + 1);
        if(err != CL_SUCCESS) goto cleanup;
      }

      // user wants to preview this merged scale
      if(p->return_layer == lev + 1)
      {
        // return this merged scale
        err = dwt_get_image_layer_cl(merged_layers, p);
        if(err != CL_SUCCESS) goto cleanup;

        bcontinue = 0;
      }
    }

    hpass = lpass;
  }

  // all scales have been processed
  if(bcontinue)
  {
    // allow to process residual image
    if(layer_func)
    {
      err = layer_func(buffer[hpass], p, p->scales + 1);
      if(err != CL_SUCCESS) goto cleanup;
    }

    // user wants to preview residual image
    if(p->return_layer == p->scales + 1)
    {
      // return residual image
      err = dwt_get_image_layer_cl(buffer[hpass], p);
      if(err != CL_SUCCESS) goto cleanup;
    }
    // return reconstructed image
    else if(p->return_layer == 0)
    {
      // some of the detail scales are on the merged layers
      if(p->merge_from_scale > 0)
      {
        // add merged layers to final image
        err = dwt_add_layer_cl(merged_layers, layers, p, p->scales + 1);
        if(err != CL_SUCCESS) goto cleanup;
      }

      // add residual image to final image
      err = dwt_add_layer_cl(buffer[hpass], layers, p, p->scales + 1);
      if(err != CL_SUCCESS) goto cleanup;

      // allow to process reconstructed image
      if(layer_func)
      {
        err = layer_func(layers, p, p->scales + 2);
        if(err != CL_SUCCESS) goto cleanup;
      }

      // return reconstructed image
      err = dwt_get_image_layer_cl(layers, p);
      if(err != CL_SUCCESS) goto cleanup;
    }
  }

cleanup:
  if(layers) dt_opencl_release_mem_object(layers);
  if(merged_layers) dt_opencl_release_mem_object(merged_layers);
  if(temp) dt_opencl_release_mem_object(temp);
  if(buffer[1]) dt_opencl_release_mem_object(buffer[1]);

  return err;
}

cl_int dwt_decompose_cl(dwt_params_cl_t *p, _dwt_layer_func_cl layer_func)
{
  cl_int err = CL_SUCCESS;

  // this is a zoom scale, not a wavelet scale
  if(p->preview_scale <= 0.f) p->preview_scale = 1.f;

  // if a single scale is requested it cannot be grather than the residual
  if(p->return_layer > p->scales + 1)
  {
    p->return_layer = p->scales + 1;
  }

  const int max_scale = dwt_get_max_scale_cl(p);

  // if requested scales is grather than max scales adjust it
  if(p->scales > max_scale)
  {
    // residual should be returned
    if(p->return_layer > p->scales) p->return_layer = max_scale + 1;
    // a scale should be returned, it cannot be grather than max scales
    else if(p->return_layer > max_scale)
      p->return_layer = max_scale;

    p->scales = max_scale;
  }

  // call the actual decompose
  err = dwt_wavelet_decompose_cl(p->image, p, layer_func);

  return err;
}

#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

