/*
    This file is part of darktable,
    Copyright (C) 2013-2023 darktable developers.

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

/* How are "detail masks" implemented?

  The details masks are used by the dual demosaicer and as a refinement
  step for shape / parametric masks.
  They contain threshold weighed values of pixel-wise local signal changes
  so they can be understood as "areas with or without local detail".

  The using modules (like dual demosaicing, sharpening ...) all want
  the "original data" from the sensor and not what the have as data input.
  (Calculating the mask from the modules roi might not detect such regions
  at all because of scaling / rotating artifacts, blurring earlier in the
  pipeline, color changes ...)

  So we prepare an intermediate scharr mask in rawprepare using Y0 data.
  (for sraws we have rgb data available, for raws we avarage)

  In all cases the user interface is pretty simple, we require a
  threshold value, which is in the range of -1.0 to 1.0 by an
  additional slider in the masks refinement section.  Positive values
  will select regions with lots of local detail, negatives select for
  flat areas.  (The dual demosaicer only wants positives as we always
  look for high frequency content.)  A threshold value of 0.0 means
  bypassing.

  The details mask is taken from the prepared scharr mask, is slightly
  gaussian-blurred for noise resistance and sized&cropped according to the
  current roi. We have this returning a valid buffer or NULL in case of errors

  float *dt_masks_calc_detail_mask(
    struct dt_dev_pixelpipe_iop_t *piece,
    const float threshold,
    const gboolean detail, // use detail mode as in mask refinements
    const gboolean output) // if true include target module in mask resizing

  Some additional comments:

  1. intentionally this details mask refinement has only been
     implemented for raws. Especially for compressed inmages like
     jpegs or 8bit input the algo didn't work as good because of input
     precision and compression artifacts.

  2. In the gui the slider is above the rest of the refinemt sliders
     to emphasize that blurring & feathering use the mask corrected by
     detail refinemnt.

  3. Of course credit goes to Ingo @heckflosse from rt team for the
     original idea. (in the rt world this is known as details mask)

  4. Thanks to rawfiner for pointing out how to use Y0 and scharr for better maths.

  hanno@schwalm-bremen.de
*/

void dt_masks_extend_border(float *const mask,
                            const int width,
                            const int height,
                            const int border)
{
  if(border <= 0) return;
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(mask, width, height, border) \
  schedule(static)
 #endif
  for(size_t row = border; row < height - border; row++)
  {
    const size_t idx = row * width;
    for(size_t i = 0; i < border; i++)
    {
      mask[idx + i] = mask[idx + border];
      mask[idx + width - i - 1] = mask[idx + width - border -1];
    }
  }
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(mask, width, height, border) \
  schedule(static)
 #endif
  for(size_t col = 0; col < width; col++)
  {
    const float top = mask[border * width + MIN(width - border - 1, MAX(col, border))];
    const float bot = mask[(height - border - 1) * width
                           + MIN(width - border - 1, MAX(col, border))];
    for(size_t i = 0; i < border; i++)
    {
      mask[col + i * width] = top;
      mask[col + (height - i - 1) * width] = bot;
    }
  }
}

void _masks_blur_5x5_coeff(float *c,
                           const float sigma)
{
  float kernel[5][5];
  const float temp = -2.0f * sqrf(sigma);
  const float range = sqrf(3.0f * 0.84f);
  float sum = 0.0f;
  for(int k = -2; k <= 2; k++)
  {
    for(int j = -2; j <= 2; j++)
    {
      if((sqrf(k) + sqrf(j)) <= range)
      {
        kernel[k + 2][j + 2] = expf((sqrf(k) + sqrf(j)) / temp);
        sum += kernel[k + 2][j + 2];
      }
      else
        kernel[k + 2][j + 2] = 0.0f;
    }
  }
  for(int i = 0; i < 5; i++)
  {
#if defined(__GNUC__)
  #pragma GCC ivdep
#endif
    for(int j = 0; j < 5; j++)
      kernel[i][j] /= sum;
  }
  /* c21 */ c[0]  = kernel[0][1];
  /* c20 */ c[1]  = kernel[0][2];
  /* c11 */ c[2]  = kernel[1][1];
  /* c10 */ c[3]  = kernel[1][2];
  /* c00 */ c[4]  = kernel[2][2];
}
#define FAST_BLUR_5 ( \
      blurmat[0] * ((src[i - w2 - 1] + src[i - w2 + 1])                   \
                 + (src[i - w1 - 2] + src[i - w1 + 2])                    \
                 + (src[i + w1 - 2] + src[i + w1 + 2])                    \
                 + (src[i + w2 - 1] + src[i + w2 + 1]))                   \
    + blurmat[1] * (src[i - w2] + src[i - 2] + src[i + 2] + src[i + w2])  \
    + blurmat[2] * (src[i - w1 - 1] + src[i - w1 + 1] + src[i + w1 - 1]   \
                    + src[i + w1 + 1])                                    \
    + blurmat[3] * (src[i - w1] + src[i - 1] + src[i + 1] + src[i + w1])  \
    + blurmat[4] * src[i] )

void dt_masks_blur_9x9_coeff(float *c, const float sigma)
{
  float kernel[9][9];
  const float temp = -2.0f * sqrf(sigma);
  const float range = sqrf(3.0f * 1.5f);
  float sum = 0.0f;
  for(int k = -4; k <= 4; k++)
  {
    for(int j = -4; j <= 4; j++)
    {
      if((sqrf(k) + sqrf(j)) <= range)
      {
        kernel[k + 4][j + 4] = expf((sqrf(k) + sqrf(j)) / temp);
        sum += kernel[k + 4][j + 4];
      }
      else
        kernel[k + 4][j + 4] = 0.0f;
    }
  }
  for(int i = 0; i < 9; i++)
  {
#if defined(__GNUC__)
  #pragma GCC ivdep
#endif
    for(int j = 0; j < 9; j++)
      kernel[i][j] /= sum;
  }
  /* c00 */ c[0]  = kernel[4][4];
  /* c10 */ c[1]  = kernel[3][4];
  /* c11 */ c[2]  = kernel[3][3];
  /* c20 */ c[3]  = kernel[2][4];
  /* c21 */ c[4]  = kernel[2][3];
  /* c22 */ c[5]  = kernel[2][2];
  /* c30 */ c[6]  = kernel[1][4];
  /* c31 */ c[7]  = kernel[1][3];
  /* c32 */ c[8]  = kernel[1][2];
  /* c33 */ c[9]  = kernel[1][1];
  /* c40 */ c[10] = kernel[0][4];
  /* c41 */ c[11] = kernel[0][3];
  /* c42 */ c[12] = kernel[0][2];
}

#define FAST_BLUR_9 ( \
  blurmat[12] * (src[i - w4 - 2] + src[i - w4 + 2] + src[i - w2 - 4] + src[i - w2 + 4] + src[i + w2 - 4] + src[i + w2 + 4] + src[i + w4 - 2] + src[i + w4 + 2]) + \
  blurmat[11] * (src[i - w4 - 1] + src[i - w4 + 1] + src[i - w1 - 4] + src[i - w1 + 4] + src[i + w1 - 4] + src[i + w1 + 4] + src[i + w4 - 1] + src[i + w4 + 1]) + \
  blurmat[10] * (src[i - w4] + src[i - 4] + src[i + 4] + src[i + w4]) + \
  blurmat[9]  * (src[i - w3 - 3] + src[i - w3 + 3] + src[i + w3 - 3] + src[i + w3 + 3]) + \
  blurmat[8]  * (src[i - w3 - 2] + src[i - w3 + 2] + src[i - w2 - 3] + src[i - w2 + 3] + src[i + w2 - 3] + src[i + w2 + 3] + src[i + w3 - 2] + src[i + w3 + 2]) + \
  blurmat[7]  * (src[i - w3 - 1] + src[i - w3 + 1] + src[i - w1 - 3] + src[i - w1 + 3] + src[i + w1 - 3] + src[i + w1 + 3] + src[i + w3 - 1] + src[i + w3 + 1]) + \
  blurmat[6]  * (src[i - w3] + src[i - 3] + src[i + 3] + src[i + w3]) + \
  blurmat[5]  * (src[i - w2 - 2] + src[i - w2 + 2] + src[i + w2 - 2] + src[i + w2 + 2]) + \
  blurmat[4]  * (src[i - w2 - 1] + src[i - w2 + 1] + src[i - w1 - 2] + src[i - w1 + 2] + src[i + w1 - 2] + src[i + w1 + 2] + src[i + w2 - 1] + src[i + w2 + 1]) + \
  blurmat[3]  * (src[i - w2] + src[i - 2] + src[i + 2] + src[i + w2]) + \
  blurmat[2]  * (src[i - w1 - 1] + src[i - w1 + 1] + src[i + w1 - 1] + src[i + w1 + 1]) + \
  blurmat[1]  * (src[i - w1] + src[i - 1] + src[i + 1] + src[i + w1]) + \
  blurmat[0]  * src[i] )

void dt_masks_blur_9x9(float *const restrict src,
                       float *const restrict out,
                       const int width,
                       const int height,
                       const float sigma)
{
  float blurmat[13];
  dt_masks_blur_9x9_coeff(blurmat, sigma);

  const size_t w1 = width;
  const size_t w2 = 2*width;
  const size_t w3 = 3*width;
  const size_t w4 = 4*width;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(blurmat, src, out, height, w1, w2, w3, w4) \
  schedule(simd:static) aligned(src, out : 64)
 #endif
  for(size_t row = 4; row < height - 4; row++)
  {
    for(size_t col = 4; col < w1 - 4; col++)
    {
      const size_t i = row * w1 + col;
      out[i] = fminf(1.0f, fmaxf(0.0f, FAST_BLUR_9));
    }
  }
  dt_masks_extend_border(out, width, height, 4);
}

void _masks_blur_13x13_coeff(float *c, const float sigma)
{
  float kernel[13][13];
  const float temp = -2.0f * sqrf(sigma);
  const float range = sqrf(3.0f * 2.0f);
  float sum = 0.0f;
  for(int k = -6; k <= 6; k++)
  {
    for(int j = -6; j <= 6; j++)
    {
      if((sqrf(k) + sqrf(j)) <= range)
      {
        kernel[k + 6][j + 6] = expf((sqrf(k) + sqrf(j)) / temp);
        sum += kernel[k + 6][j + 6];
      }
      else
        kernel[k + 6][j + 6] = 0.0f;
    }
  }
  for(int i = 0; i < 13; i++)
  {
#if defined(__GNUC__)
  #pragma GCC ivdep
#endif
    for(int j = 0; j < 13; j++)
      kernel[i][j] /= sum;
  }
  /* c60 */ c[0]  = kernel[0][6];
  /* c53 */ c[1]  = kernel[1][3];
  /* c52 */ c[2]  = kernel[1][4];
  /* c51 */ c[3]  = kernel[1][5];
  /* c50 */ c[4]  = kernel[1][6];
  /* c44 */ c[5]  = kernel[2][2];
  /* c42 */ c[6]  = kernel[2][4];
  /* c41 */ c[7]  = kernel[2][5];
  /* c40 */ c[8]  = kernel[2][6];
  /* c33 */ c[9]  = kernel[3][3];
  /* c32 */ c[10] = kernel[3][4];
  /* c31 */ c[11] = kernel[3][5];
  /* c30 */ c[12] = kernel[3][6];
  /* c22 */ c[13] = kernel[4][4];
  /* c21 */ c[14] = kernel[4][5];
  /* c20 */ c[15] = kernel[4][6];
  /* c11 */ c[16] = kernel[5][5];
  /* c10 */ c[17] = kernel[5][6];
  /* c00 */ c[18] = kernel[6][6];
}

#define FAST_BLUR_13 ( \
  blurmat[0] * (src[i - w6] + src[i - 6] + src[i + 6] + src[i + w6]) + \
  blurmat[1] * ((src[i - w5 - 3] + src[i - w5 + 3]) + (src[i - w3 - 5] + src[i - w3 + 5]) + (src[i + w3 - 5] + src[i + w3 + 5]) + (src[i + w5 - 3] + src[i + w5 + 3])) + \
  blurmat[2] * ((src[i - w5 - 2] + src[i - w5 + 2]) + (src[i - w2 - 5] + src[i - w2 + 5]) + (src[i + w2 - 5] + src[i + w2 + 5]) + (src[i + w5 - 2] + src[i + w5 + 2])) + \
  blurmat[3] * ((src[i - w5 - 1] + src[i - w5 + 1]) + (src[i - w1 - 5] + src[i - w1 + 5]) + (src[i + w1 - 5] + src[i + w1 + 5]) + (src[i + w5 - 1] + src[i + w5 + 1])) + \
  blurmat[4] * ((src[i - w5] + src[i - 5] + src[i + 5] + src[i + w5]) + ((src[i - w4 - 3] + src[i - w4 + 3]) + (src[i - w3 - 4] + src[i - w3 + 4]) + (src[i + w3 - 4] + src[i + w3 + 4]) + (src[i + w4 - 3] + src[i + w4 + 3]))) + \
  blurmat[5] * (src[i - w4 - 4] + src[i - w4 + 4] + src[i + w4 - 4] + src[i + w4 + 4]) + \
  blurmat[6] * ((src[i - w4 - 2] + src[i - w4 + 2]) + (src[i - w2 - 4] + src[i - w2 + 4]) + (src[i + w2 - 4] + src[i + w2 + 4]) + (src[i + w4 - 2] + src[i + w4 + 2])) + \
  blurmat[7] * ((src[i - w4 - 1] + src[i - w4 + 1]) + (src[i - w1 - 4] + src[i - w1 + 4]) + (src[i + w1 - 4] + src[i + w1 + 4]) + (src[i + w4 - 1] + src[i + w4 + 1])) + \
  blurmat[8] * (src[i - w4] + src[i - 4] + src[i + 4] + src[i + w4]) + \
  blurmat[9] * (src[i - w3 - 3] + src[i - w3 + 3] + src[i + w3 - 3] + src[i + w3 + 3]) + \
  blurmat[10] * ((src[i - w3 - 2] + src[i - w3 + 2]) + (src[i - w2 - 3] + src[i - w2 + 3]) + (src[i + w2 - 3] + src[i + w2 + 3]) + (src[i + w3 - 2] + src[i + w3 + 2])) + \
  blurmat[11] * ((src[i - w3 - 1] + src[i - w3 + 1]) + (src[i - w1 - 3] + src[i - w1 + 3]) + (src[i + w1 - 3] + src[i + w1 + 3]) + (src[i + w3 - 1] + src[i + w3 + 1])) + \
  blurmat[12] * (src[i - w3] + src[i - 3] + src[i + 3] + src[i + w3]) + \
  blurmat[13] * (src[i - w2 - 2] + src[i - w2 + 2] + src[i + w2 - 2] + src[i + w2 + 2]) + \
  blurmat[14] * ((src[i - w2 - 1] + src[i - w2 + 1]) + (src[i - w1 - 2] + src[i - w1 + 2]) + (src[i + w1 - 2] + src[i + w1 + 2]) + (src[i + w2 - 1] + src[i + w2 + 1])) + \
  blurmat[15] * (src[i - w2] + src[i - 2] + src[i + 2] + src[i + w2]) + \
  blurmat[16] * (src[i - w1 - 1] + src[i - w1 + 1] + src[i + w1 - 1] + src[i + w1 + 1]) + \
  blurmat[17] * (src[i - w1] + src[i - 1] + src[i + 1] + src[i + w1]) + \
  blurmat[18] * src[i] )

int dt_masks_blur_fast(float *const restrict src,
                       float *const restrict out,
                       const int width,
                       const int height,
                       const float sigma,
                       const float gain,
                       const float clip)
{
  float blurmat[19];
  const size_t w1 = width;
  const size_t w2 = 2*width;
  const size_t w3 = 3*width;
  const size_t w4 = 4*width;
  const size_t w5 = 5*width;
  const size_t w6 = 6*width;
  if(sigma <= 0.0f)
  {
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(src, out, gain, width, height, clip) \
  schedule(simd:static) aligned(src, out : 64)
#endif
    for(size_t i = 0; i < width * height; i++)
      out[i] = fmaxf(0.0f, fminf(clip, gain * src[i]));
    return 0;
  }
  else if(sigma <= 0.8f)
  {
    _masks_blur_5x5_coeff(blurmat, sigma);
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(src, out, gain, height, w1, w2, clip) \
  shared(blurmat) \
  schedule(simd:static) aligned(src, out : 64)
#endif
    for(size_t row = 2; row < height - 2; row++)
    {
      for(size_t col = 2; col < w1 - 2; col++)
      {
        const size_t i = row * w1 + col;
        out[i] = fmaxf(0.0f, fminf(clip, gain * FAST_BLUR_5));
      }
    }
    return 2;
  }
  else if(sigma <= 1.5f)
  {
    dt_masks_blur_9x9_coeff(blurmat, sigma);
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(src, out, gain, height, w1, w2, w3, w4, clip) \
  shared(blurmat) \
  schedule(simd:static) aligned(src, out : 64)
 #endif
    for(size_t row = 4; row < height - 4; row++)
    {
      for(size_t col = 4; col < w1 - 4; col++)
      {
        const size_t i = row * w1 + col;
        out[i] = fmaxf(0.0f, fminf(clip, gain * FAST_BLUR_9));
      }
    }
    return 4;
  }
  _masks_blur_13x13_coeff(blurmat, sigma);
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(src, out, gain, height, w1, w2, w3, w4, w5, w6, clip) \
  shared(blurmat) \
  schedule(simd:static) aligned(src, out : 64)
 #endif
  for(size_t row = 6; row < height - 6; row++)
  {
    for(size_t col = 6; col < w1 - 6; col++)
    {
      const size_t i = row * w1 + col;
      out[i] = fmaxf(0.0f, fminf(clip, gain * FAST_BLUR_13));
    }
  }
  return 6;
}

void dt_dev_clear_detail_mask(dt_dev_pixelpipe_t *pipe)
{
  if(pipe->details.data) dt_free_align(pipe->details.data);
  memset(&pipe->details, 0, sizeof(dt_dev_detail_mask_t));
}

float *dt_masks_calc_detail_mask(struct dt_dev_pixelpipe_iop_t *piece,
                               const float threshold,
                               const gboolean detail,
                               const gboolean output)
{
  dt_dev_pixelpipe_t *pipe = piece->pipe;
  dt_dev_detail_mask_t *details = &pipe->details;

  dt_print_pipe(DT_DEBUG_MASKS,
    "calc detail mask",
       pipe, piece->module, &details->roi, &piece->processed_roi_out, "%s\n",
       details->data ? "" : "no mask data available");

  if(!details->data)
    return NULL;

  const size_t dsize = (size_t) details->roi.width * details->roi.height;
  float *tmp = dt_alloc_align_float(dsize);
  float *mask = dt_alloc_align_float(dsize);
  if(!tmp || !mask)
  {
    dt_free_align(tmp);
    dt_free_align(mask);
    return NULL;
  }

  const float thresh = 16.0f / fmaxf(1e-9, threshold);
  float *src = details->data;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(src, tmp, dsize, thresh, detail) \
  schedule(simd:static) aligned(src, tmp : 64)
#endif
  for(size_t idx = 0; idx < dsize; idx++)
  {
    // sigmoid function, result is in [0;1] range, inflexion point at threshold=0.5
    const float blend = CLIP(1.0f / (1.0f + dt_fast_expf(16.0f - thresh * src[idx])));
    tmp[idx] = detail ? blend : 1.0f - blend;
  }
  // for very small images the blurring should be slightly less to have an effect at all
  const float blurring = (MIN(details->roi.width, details->roi.height) < 500) ? 1.5f : 2.0f;
  dt_masks_blur_9x9(tmp, mask, details->roi.width, details->roi.height, blurring);
  dt_free_align(tmp);

  gboolean valid = FALSE;

  GList *source_iter;
  for(source_iter = pipe->nodes; source_iter; source_iter = g_list_next(source_iter))
  {
    const dt_dev_pixelpipe_iop_t *candidate = (dt_dev_pixelpipe_iop_t *)source_iter->data;
    if(dt_iop_module_is(candidate->module->so, "rawprepare")
       && candidate->enabled)
    {
      valid = TRUE;
      break;
    }
  }
  if(!valid)
  {
    dt_free_align(mask);
    return NULL;
  }

  dt_iop_module_t *target_module = piece->module;
  float *resmask = mask;
  float *inmask  = mask;

  if(source_iter)
  {
    for(GList *iter = g_list_next(source_iter); iter; iter = g_list_next(iter))
    {
      dt_dev_pixelpipe_iop_t *module = (dt_dev_pixelpipe_iop_t *)iter->data;

      // we might not want to include the target module in the 'distort_mask' list
      if((module->module == target_module) && !output) break;

      if(module->enabled
         && !(module->module->dev->gui_module
              && module->module->dev->gui_module != module->module
              && module->module->dev->gui_module->operation_tags_filter()
                 & module->module->operation_tags()))
      {
        // hack against pipes not using finalscale
        if(module->module->distort_mask
              && !(dt_iop_module_is(module->module->so, "finalscale")
                    && module->processed_roi_in.width == 0
                    && module->processed_roi_in.height == 0))
        {
          float *tmp = dt_alloc_align_float((size_t)module->processed_roi_out.width
                                            * module->processed_roi_out.height);
          dt_print_pipe(DT_DEBUG_MASKS,
             "distort detail mask", pipe, module->module, &module->processed_roi_in, &module->processed_roi_out, "\n");

          module->module->distort_mask(module->module, module, inmask, tmp,
                                       &module->processed_roi_in,
                                       &module->processed_roi_out);
          resmask = tmp;
          if(inmask != src) dt_free_align(inmask);
          inmask = tmp;
        }
        else if(!module->module->distort_mask
                && (module->processed_roi_in.width != module->processed_roi_out.width
                    || module->processed_roi_in.height != module->processed_roi_out.height
                    || module->processed_roi_in.x != module->processed_roi_out.x
                    || module->processed_roi_in.y != module->processed_roi_out.y))
              dt_print_pipe(DT_DEBUG_ALWAYS,
                      "distort details mask",
                      pipe, module->module,
                      &module->processed_roi_in, &module->processed_roi_out,
                      "misses distort_mask()\n");

        if((module->module == target_module) && output) break;
      }
    }
  }
  return resmask;
}
#undef FAST_BLUR_5
#undef FAST_BLUR_9
#undef FAST_BLUR_13
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
