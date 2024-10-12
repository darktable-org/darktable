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
  there still is no scaling we can use a constant sigma.
  Now we have an unscaled detail mask which requires to be transformed
  through the pipeline using

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
     original idea. (in the rt world this is known as details mask)

  4. Thanks to rawfiner for pointing out how to use Y0 and scharr for better maths.

  hanno@schwalm-bremen.de 21/04/29
*/

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
  for(size_t row = 0; row < height; row++)
  {
    const int irow = CLAMP(row, 1, height -2);
    for(size_t col = 0; col < width; col++)
    {
      const int icol = CLAMP(col, 1, width -2);
      const size_t idx = (size_t)irow * width + icol;

      const float gradient_magnitude = scharr_gradient(&tmp[idx], width);
      mask[(size_t)row * width + col] = fminf(1.0f, fmaxf(0.0f, gradient_magnitude / 16.0f));
    }
  }
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

float *dt_masks_calc_detail_mask(dt_dev_pixelpipe_iop_t *piece,
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
  const float sigma = (MIN(details->roi.width, details->roi.height) < 500) ? 1.5f : 2.0f;
  dt_gaussian_fast_blur(tmp, mask, details->roi.width, details->roi.height, sigma, 0.0f, 1.0f, 1);
  dt_free_align(tmp);
  return mask;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
