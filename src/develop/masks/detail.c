/*
    This file is part of darktable,
    Copyright (C) 2013-2024 darktable developers.

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

  The detail masks (DM) are used by the dual demosaicer and as a
  further refinement step for shape / parametric masks.  They contain
  threshold weighed values of pixel-wise local signal changes so they
  can be understood as "areas with or without local detail".

  As the DM using algorithms (like dual demosaicing, sharpening ...)
  are all pixel peeping we want the "original data" from the sensor to
  calculate it.  (Calculating the mask from the modules roi might not
  detect such regions at all because of scaling / rotating artifacts,
  some blurring earlier in the pipeline, color changes ...)

  In all cases the user interface is pretty simple, we just pass a
  threshold value, which is in the range of -1.0 to 1.0 by an
  additional slider in the masks refinement section.  Positive values
  will select regions with lots of local detail, negatives select for
  flat areas.  (The dual demosaicer only wants positives as we always
  look for high frequency content.)  A threshold value of 0.0 means
  bypassing.

  So the first important point is:

  We make sure taking the input data for the DM right from the
  demosaicer for normal raws or from rawprepare in case of
  monochromes. This means some additional housekeeping for the
  pixelpipe.

  If any mask in any module selects a threshold of != 0.0 we leave a
  flag in the pipe struct telling a) we want a DM and b) we want it
  from either demosaic or from rawprepare.  If such a flag has not
  been previously set we will force a pipeline reprocessing.

  gboolean dt_dev_write_scharr_mask(dt_dev_pixelpipe_iop_t *piece,
                                     float *const rgb,
                                     const dt_iop_roi_t *const roi_in,
                                     const gboolean rawmode)
  or it's _cl equivalent write a preliminary mask holding signal-change values
  for every pixel. These mask values are calculated as
  a) get Y0 for every pixel
  b) apply a scharr operator on it

  This scharr mask (SM) is not scaled but only cropped to the roi
  of the writing module (demosaic or rawprepare).  The pipe gets roi
  copy of the writing module so we can later scale/distort the LM.

  Calculating the SM is done for performance and lower mem pressure
  reasons, so we don't have to pass full data to the module.

  If a mask uses the details refinement step it takes the scharr
  mask and calculates an intermediate mask (IM) which is still not
  scaled but has the roi of the writing module.

  For every pixel we calculate the IM value via a sigmoid function
  with the threshold and scharr as parameters.

  At last the IM is slightly blurred to avoid hard transitions, as
  there still is no scaling we can use a constant sigma. As the
  blur_9x9 is pretty fast both in openmp/cl code paths - much faster
  than dt gaussians - it is used here.  Now we have an unscaled detail
  mask which requires to be transformed through the pipeline using

  float *dt_dev_distort_detail_mask(const dt_dev_pixelpipe_t *pipe, float *src, const dt_iop_module_t *target_module)

  returning a pointer to a distorted mask (DT) with same size as used
  in the module wanting the refinement.  This DM is finally used to
  refine the original mask.

  All other refinements and parametric parameters are untouched.

  Some additional comments:

  1. intentionally this details mask refinement has only been
     implemented for raws. Especially for compressed inmages like
     jpegs or 8bit input the algo didn't work as good because of input
     precision and compression artifacts.

  2. In the gui the slider is above the rest of the refinemt sliders
     to emphasize that blurring & feathering use the mask corrected by
     detail refinement.

  3. Of course credit goes to Ingo @heckflosse from rt team for the
     original idea. (in the rt world this is knowb as details mask)

  4. Thanks to rawfiner for pointing out how to use Y0 and scharr for better maths.

  hanno@schwalm-bremen.de 21/04/29
*/

void dt_masks_extend_border(float *const mask,
                            const int width,
                            const int height,
                            const int border)
{
  if(border <= 0) return;
  DT_OMP_FOR()
  for(size_t row = border; row < height - border; row++)
  {
    const size_t idx = row * width;
    for(size_t i = 0; i < border; i++)
    {
      mask[idx + i] = mask[idx + border];
      mask[idx + width - i - 1] = mask[idx + width - border -1];
    }
  }
  DT_OMP_FOR()
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

void dt_masks_blur_coeff(float *c, const float sigma)
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

void dt_masks_blur(float *const restrict src,
                   float *const restrict out,
                   const int width,
                   const int height,
                   const float sigma,
                   const float gain,
                   const float clip)
{
  float blurmat[13];
  dt_masks_blur_coeff(blurmat, sigma);

  const size_t w1 = width;
  const size_t w2 = 2*width;
  const size_t w3 = 3*width;
  const size_t w4 = 4*width;
  DT_OMP_FOR()
  for(size_t row = 4; row < height - 4; row++)
  {
    for(size_t col = 4; col < width - 4; col++)
    {
      const size_t i = row * width + col;
      out[i] = fmaxf(0.0f, fminf(clip, gain * FAST_BLUR_9));
    }
  }
  dt_masks_extend_border(out, width, height, 4);
}

gboolean dt_masks_calc_scharr_mask(dt_dev_detail_mask_t *details,
                                      float *const restrict src,
                                      const dt_aligned_pixel_t wb)
{
  const int width = details->roi.width;
  const int height = details->roi.height;
  float *mask = details->data;

  const size_t msize = (size_t)width * height;
  float *tmp = dt_alloc_align_float(msize);
  if(!tmp) return TRUE;

  DT_OMP_FOR_SIMD(aligned(tmp, src : 64))
  for(size_t idx =0; idx < msize; idx++)
  {
    const float val = fmaxf(0.0f, src[4 * idx] / wb[0])
                    + fmaxf(0.0f, src[4 * idx + 1] / wb[1])
                    + fmaxf(0.0f, src[4 * idx + 2] / wb[2]);
    // add a gamma. sqrtf should make noise variance the same for all image
    tmp[idx] = sqrtf(val / 3.0f);
  }

  DT_OMP_FOR()
  for(size_t row = 1; row < height - 1; row++)
  {
    for(size_t col = 1; col < width - 1; col++)
    {
      const size_t idx = row * width + col;
      const float gradient_magnitude = scharr_gradient(&tmp[idx], width);
      mask[idx] = fminf(1.0f, fmaxf(0.0f, gradient_magnitude / 16.0f));
    }
  }
  dt_masks_extend_border(mask, width, height, 1);
  dt_free_align(tmp);
  return FALSE;
}

static inline float _calcBlendFactor(float val, float ithreshold)
{
    // sigmoid function
    // result is in ]0;1] range
    // inflexion point is at (x, y) (threshold, 0.5)
    return 1.0f / (1.0f + dt_fast_expf(16.0f - ithreshold * val));
}

float *dt_masks_calc_detail_mask(struct dt_dev_pixelpipe_iop_t *piece,
                               const float threshold,
                               const gboolean detail)
{
  dt_dev_pixelpipe_t *pipe = piece->pipe;
  dt_dev_detail_mask_t *details = &pipe->scharr;

  if(!details->data)
    return NULL;

  const size_t msize = (size_t) details->roi.width * details->roi.height;
  float *tmp = dt_alloc_align_float(msize);
  float *mask = dt_alloc_align_float(msize);
  if(!tmp || !mask)
  {
    dt_free_align(tmp);
    dt_free_align(mask);
    return NULL;
  }

  const float ithreshold = 16.0f / (fmaxf(1e-7, threshold));
  float *src = details->data;
  DT_OMP_FOR_SIMD(aligned(src, tmp : 64))
  for(size_t idx = 0; idx < msize; idx++)
  {
    const float blend = CLIP(_calcBlendFactor(src[idx], ithreshold));
    tmp[idx] = detail ? blend : 1.0f - blend;
  }
  // for very small images the blurring should be slightly less to have an effect at all
  const float blurring = (MIN(details->roi.width, details->roi.height) < 500) ? 1.5f : 2.0f;
  dt_masks_blur(tmp, mask, details->roi.width, details->roi.height, blurring, 1.0f, 1.0f);
  dt_free_align(tmp);
  return mask;
}
#undef FAST_BLUR_9


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
