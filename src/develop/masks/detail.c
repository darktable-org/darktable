/*
    This file is part of darktable,
    Copyright (C) 2013-2021 darktable developers.

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

  The detail masks (DM) are used by the dual demosaicer and as a further refinement step for
  shape / parametric masks.
  They contain threshold weighed values of pixel-wise local signal changes so they can be
  understood as "areas with or without local detail". 
  
  As the DM using algorithms (like dual demosaicing, sharpening ...) are all pixel peeping we
  want the "original data" from the sensor to calculate it.
  (Calculating the mask from the modules roi might not detect such regions at all because of
  scaling / rotating artifacts, some blurring earlier in the pipeline, color changes ...)

  In all cases the user interface is pretty simple, we just pass a threshold value, which
  is in the range of -1.0 to 1.0 by an additional slider in the masks refinement section.
  Positive values will select regions with lots of local detail, negatives select for flat areas.
  (The dual demosaicer only wants positives as we always look for high frequency content.)
  A threshold value of 0.0 means bypassing.
  
  So the first important point is:
  We make sure taking the input data for the DM right from the demosaicer for normal raws
  or from rawprepare in case of monochromes. This means some additional housekeeping for the
  pixelpipe.
  If any mask in any module selects a threshold of != 0.0 we leave a flag in the pipe struct
  telling a) we want a DM and b) we want it from either demosaic or from rawprepare.
  If such a flag has not been previously set we will force a pipeline reprocessing.
  
  gboolean dt_dev_write_rawdetail_mask(dt_dev_pixelpipe_iop_t *piece, float *const rgb, const dt_iop_roi_t *const roi_in, const int mode, const dt_aligned_pixel_t wb);
  or it's _cl equivalent write a preliminary mask holding signal-change values for every pixel.
  These mask values are calculated as
  a) get Y0 for every pixel
  b) apply a scharr operator on it

  This raw detail mask (RM) is not scaled but only cropped to the roi of the writing module (demosaic
  or rawprepare).
  The pipe gets roi copy of the writing module so we can later scale/distort the LM.

  Calculating the RM is done for performance and lower mem pressure reasons, so we don't have to
  pass full data to the module. Also the RM can be used by other modules. 
 
  If a mask uses the details refinement step it takes the raw details mask RM and calculates an
  intermediate mask (IM) which is still not scaled but has the roi of the writing module.
 
  For every pixel we calculate the IM value via a sigmoid function with the threshold and RM as parameters.

  At last the IM is slightly blurred to avoid hard transitions, as there still is no scaling we can use
  a constant sigma. As the blur_9x9 is pretty fast both in openmp/cl code paths - much faster than dt
  gaussians - it is used here.
  Now we have an unscaled detail mask which requires to be transformed through the pipeline using

  float *dt_dev_distort_detail_mask(const dt_dev_pixelpipe_t *pipe, float *src, const dt_iop_module_t *target_module)

  returning a pointer to a distorted mask (DT) with same size as used in the module wanting the refinement.
  This DM is finally used to refine the original mask.

  All other refinements and parametric parameters are untouched.

  Some additional comments:
  1. intentionally this details mask refinement has only been implemented for raws. Especially for compressed
     inmages like jpegs or 8bit input the algo didn't work as good because of input precision and compression artifacts.
  2. In the gui the slider is above the rest of the refinemt sliders to emphasize that blurring & feathering use the
     mask corrected by detail refinemnt.
  3. Of course credit goes to Ingo @heckflosse from rt team for the original idea. (in the rt world this is knowb
     as details mask)
  4. Thanks to rawfiner for pointing out how to use Y0 and scharr for better maths.

  hanno@schwalm-bremen.de 21/04/29
*/

// We don't want to use the SIMD version as we might access unaligned memory
static inline float sqrf(float a)
{
  return a * a;
}

void dt_masks_extend_border(float *mask, const int width, const int height, const int border)
{
  if(border <= 0) return;
  for(int row = border; row < height - border; row++)
  {
    const int idx = row * width;
    for(int i = 0; i < border; i++)
    {
      mask[idx + i] = mask[idx + border];
      mask[idx + width - i - 1] = mask[idx + width - border -1];
    }
  }
  for(int col = 0; col < width; col++)
  {
    const float top = mask[border * width + MIN(width - border - 1, MAX(col, border))];
    const float bot = mask[(height - border - 1) * width + MIN(width - border - 1, MAX(col, border))];
    for(int i = 0; i < border; i++)
    {
      mask[col + i * width] = top;
      mask[col + (height - i - 1) * width] = bot;
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(src, out : 64)
#endif
void dt_masks_blur_9x9(float *const restrict src, float *const restrict out, const int width, const int height, const float sigma)
{
  // For a blurring sigma of 2.0f a 13x13 kernel would be optimally required but the 9x9 is by far good enough here
  float kernel[9][9];
  const float temp = 2.0f * sqrf(sigma);
  float sum = 0.0f;
  for(int i = -4; i <= 4; i++)
  {
    for(int j = -4; j <= 4; j++)
    {
      kernel[i + 4][j + 4] = expf( -(sqrf(i) + sqrf(j)) / temp);
      sum += kernel[i + 4][j + 4];
    }
  }
  for(int i = 0; i < 9; i++)
  {
    for(int j = 0; j < 9; j++)
      kernel[i][j] /= sum;
  }
  const float c42 = kernel[0][2];
  const float c41 = kernel[0][3];
  const float c40 = kernel[0][4];
  const float c33 = kernel[1][1];
  const float c32 = kernel[1][2];
  const float c31 = kernel[1][3];
  const float c30 = kernel[1][4];
  const float c22 = kernel[2][2];
  const float c21 = kernel[2][3];
  const float c20 = kernel[2][4];
  const float c11 = kernel[3][3];
  const float c10 = kernel[3][4];
  const float c00 = kernel[4][4];
  const int w1 = width;
  const int w2 = 2*width;
  const int w3 = 3*width;
  const int w4 = 4*width;
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(src, out) \
  dt_omp_sharedconst(c42, c41, c40, c33, c32, c31, c30, c22, c21, c20, c11, c10, c00, w1, w2, w3, w4, width, height) \
  schedule(simd:static)
 #endif
  for(int row = 4; row < height - 4; row++)
  {
#if defined(__clang__)
        #pragma clang loop vectorize(assume_safety)
#elif defined(__GNUC__)
        #pragma GCC ivdep
#endif
    for(int col = 4; col < width - 4; col++)
    {
      const int i = row * width + col;
      const float val = c42 * (src[i - w4 - 2] + src[i - w4 + 2] + src[i - w2 - 4] + src[i - w2 + 4] + src[i + w2 - 4] + src[i + w2 + 4] + src[i + w4 - 2] + src[i + w4 + 2]) +
                        c41 * (src[i - w4 - 1] + src[i - w4 + 1] + src[i - w1 - 4] + src[i - w1 + 4] + src[i + w1 - 4] + src[i + w1 + 4] + src[i + w4 - 1] + src[i + w4 + 1]) +
                        c40 * (src[i - w4] + src[i - 4] + src[i + 4] + src[i + w4]) +
                        c33 * (src[i - w3 - 3] + src[i - w3 + 3] + src[i + w3 - 3] + src[i + w3 + 3]) +
                        c32 * (src[i - w3 - 2] + src[i - w3 + 2] + src[i - w2 - 3] + src[i - w2 + 3] + src[i + w2 - 3] + src[i + w2 + 3] + src[i + w3 - 2] + src[i + w3 + 2]) +
                        c31 * (src[i - w3 - 1] + src[i - w3 + 1] + src[i - w1 - 3] + src[i - w1 + 3] + src[i + w1 - 3] + src[i + w1 + 3] + src[i + w3 - 1] + src[i + w3 + 1]) +
                        c30 * (src[i - w3] + src[i - 3] + src[i + 3] + src[i + w3]) +
                        c22 * (src[i - w2 - 2] + src[i - w2 + 2] + src[i + w2 - 2] + src[i + w2 + 2]) +
                        c21 * (src[i - w2 - 1] + src[i - w2 + 1] + src[i - w1 - 2] + src[i - w1 + 2] + src[i + w1 - 2] + src[i + w1 + 2] + src[i + w2 - 1] + src[i + w2 + 1]) +
                        c20 * (src[i - w2] + src[i - 2] + src[i + 2] + src[i + w2]) +
                        c11 * (src[i - w1 - 1] + src[i - w1 + 1] + src[i + w1 - 1] + src[i + w1 + 1]) +
                        c10 * (src[i - w1] + src[i - 1] + src[i + 1] + src[i + w1]) +
                        c00 * src[i];
      out[i] = fminf(1.0f, fmaxf(0.0f, val));
    }
  }
  dt_masks_extend_border(out, width, height, 4);
}

void dt_masks_calc_rawdetail_mask(float *const restrict src, float *const restrict mask, float *const restrict tmp,
                                  const int width, const int height, const dt_aligned_pixel_t wb)
{
  const int msize = width * height;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(tmp, src, msize, wb) \
  schedule(simd:static) aligned(tmp, src : 64)
#endif
  for(int idx =0; idx < msize; idx++)
  {
    const float val = 0.333333333f * (fmaxf(src[4 * idx], 0.0f) / wb[0] + fmaxf(src[4 * idx + 1], 0.0f) / wb[1] + fmaxf(src[4 * idx + 2], 0.0f) / wb[2]);
    tmp[idx] = sqrtf(val); // add a gamma. sqrtf should make noise variance the same for all image
  }

  const float scale = 1.0f / 16.0f;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(mask, tmp, width, height, scale) \
  schedule(simd:static) aligned(mask, tmp : 64)
 #endif
  for(int row = 1; row < height - 1; row++)
  {
    for(int col = 1, idx = row * width + col; col < width - 1; col++, idx++)
    {
      // scharr operator
      const float gx = 47.0f * (tmp[idx-width-1] - tmp[idx-width+1])
                    + 162.0f * (tmp[idx-1]       - tmp[idx+1])
                     + 47.0f * (tmp[idx+width-1] - tmp[idx+width+1]);
      const float gy = 47.0f * (tmp[idx-width-1] - tmp[idx+width-1])
                    + 162.0f * (tmp[idx-width]   - tmp[idx+width])
                     + 47.0f * (tmp[idx-width+1] - tmp[idx+width+1]);
      const float gradient_magnitude = sqrtf(sqrf(gx / 256.0f) + sqrf(gy / 256.0f));
      mask[idx] = scale * gradient_magnitude;
      // Original code from rt
      // tmp[idx] = scale * sqrtf(sqrf(src[idx+1] - src[idx-1]) + sqrf(src[idx + width]   - src[idx - width]) +
      //                          sqrf(src[idx+2] - src[idx-2]) + sqrf(src[idx + 2*width] - src[idx - 2*width]));
    }
  }
  dt_masks_extend_border(mask, width, height, 1);
}

static inline float calcBlendFactor(float val, float threshold)
{
    // sigmoid function
    // result is in ]0;1] range
    // inflexion point is at (x, y) (threshold, 0.5)
    return 1.0f / (1.0f + dt_fast_expf(16.0f - (16.0f / threshold) * val));
}

void dt_masks_calc_detail_mask(float *const restrict src, float *const restrict out, float *const restrict tmp, const int width, const int height, const float threshold, const gboolean detail)
{
  const int msize = width * height;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(src, tmp, msize, threshold, detail) \
  schedule(simd:static) aligned(src, tmp : 64)
#endif
  for(int idx = 0; idx < msize; idx++)
  {
    const float blend = calcBlendFactor(src[idx], threshold);
    tmp[idx] = detail ? blend : 1.0f - blend;
  }
  dt_masks_blur_9x9(tmp, out, width, height, 2.0f);
}
