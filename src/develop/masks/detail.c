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

void dt_masks_extend_border(float *const mask, const int width, const int height, const int border)
{
  if(border <= 0) return;
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(mask) \
  dt_omp_sharedconst(width, height, border) \
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
  dt_omp_firstprivate(mask) \
  dt_omp_sharedconst(width, height, border) \
  schedule(static)
 #endif
  for(size_t col = 0; col < width; col++)
  {
    const float top = mask[border * width + MIN(width - border - 1, MAX(col, border))];
    const float bot = mask[(height - border - 1) * width + MIN(width - border - 1, MAX(col, border))];
    for(size_t i = 0; i < border; i++)
    {
      mask[col + i * width] = top;
      mask[col + (height - i - 1) * width] = bot;
    }
  }
}

void _masks_blur_5x5_coeff(float *c, const float sigma)
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
  blurmat[0] * ((src[i - w2 - 1] + src[i - w2 + 1]) + (src[i - w1 - 2] + src[i - w1 + 2]) + (src[i + w1 - 2] + src[i + w1 + 2]) + (src[i + w2 - 1] + src[i + w2 + 1])) + \
  blurmat[1] * (src[i - w2] + src[i - 2] + src[i + 2] + src[i + w2]) + \
  blurmat[2] * (src[i - w1 - 1] + src[i - w1 + 1] + src[i + w1 - 1] + src[i + w1 + 1]) + \
  blurmat[3] * (src[i - w1] + src[i - 1] + src[i + 1] + src[i + w1]) + \
  blurmat[4] * src[i] )

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

void dt_masks_blur_9x9(float *const restrict src, float *const restrict out, const int width, const int height, const float sigma)
{
  float blurmat[13];
  dt_masks_blur_9x9_coeff(blurmat, sigma);

  const size_t w1 = width;
  const size_t w2 = 2*width;
  const size_t w3 = 3*width;
  const size_t w4 = 4*width;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(blurmat, src, out) \
  dt_omp_sharedconst(width, height, w1, w2, w3, w4) \
  schedule(simd:static) aligned(src, out : 64)
 #endif
  for(size_t row = 4; row < height - 4; row++)
  {
    for(size_t col = 4; col < width - 4; col++)
    {
      const size_t i = row * width + col;
      out[row * width + col] = fminf(1.0f, fmaxf(0.0f, FAST_BLUR_9));
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

int dt_masks_blur_fast(float *const restrict src, float *const restrict out, const int width, const int height, const float sigma, const float gain, const float clip)
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
  dt_omp_firstprivate(src, out) \
  dt_omp_sharedconst(gain, width, height, clip) \
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
  dt_omp_firstprivate(src, out) \
  dt_omp_sharedconst(gain, width, height, w1, w2, clip) \
  shared(blurmat) \
  schedule(simd:static) aligned(src, out : 64)
#endif
    for(size_t row = 2; row < height - 2; row++)
    {
      for(size_t col = 2; col < width - 2; col++)
      {
        const size_t i = row * width + col;
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
  dt_omp_firstprivate(src, out) \
  dt_omp_sharedconst(gain, width, height, w1, w2, w3, w4, clip) \
  shared(blurmat) \
  schedule(simd:static) aligned(src, out : 64)
 #endif
    for(size_t row = 4; row < height - 4; row++)
    {
      for(size_t col = 4; col < width - 4; col++)
      {
        const size_t i = row * width + col;
        out[i] = fmaxf(0.0f, fminf(clip, gain * FAST_BLUR_9));
      }
    }
    return 4;
  }
  _masks_blur_13x13_coeff(blurmat, sigma);
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(src, out) \
  dt_omp_sharedconst(gain, width, height, w1, w2, w3, w4, w5, w6, clip) \
  shared(blurmat) \
  schedule(simd:static) aligned(src, out : 64)
 #endif
  for(size_t row = 6; row < height - 6; row++)
  {
    for(size_t col = 6; col < width - 6; col++)
    {
      const size_t i = row * width + col;
      out[i] = fmaxf(0.0f, fminf(clip, gain * FAST_BLUR_13));
    }
  }
  return 6;
}

void dt_masks_calc_rawdetail_mask(float *const restrict src, float *const restrict mask, float *const restrict tmp,
                                  const int width, const int height, const dt_aligned_pixel_t wb)
{
  const size_t msize = width * height;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(tmp, src, msize, wb) \
  schedule(simd:static) aligned(tmp, src : 64)
#endif
  for(size_t idx =0; idx < msize; idx++)
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
  for(size_t row = 1; row < height - 1; row++)
  {
    for(size_t col = 1; col < width - 1; col++)
    {
      const size_t idx = row * width + col;
      // scharr operator
      const float gx = 47.0f * (tmp[idx-width-1] - tmp[idx-width+1])
                    + 162.0f * (tmp[idx-1]       - tmp[idx+1])
                     + 47.0f * (tmp[idx+width-1] - tmp[idx+width+1]);
      const float gy = 47.0f * (tmp[idx-width-1] - tmp[idx+width-1])
                    + 162.0f * (tmp[idx-width]   - tmp[idx+width])
                     + 47.0f * (tmp[idx-width+1] - tmp[idx+width+1]);
      const float gradient_magnitude = sqrtf(sqrf(gx / 256.0f) + sqrf(gy / 256.0f));
      mask[idx] = scale * gradient_magnitude;
    }
  }
  dt_masks_extend_border(mask, width, height, 1);
}

static inline float _calcBlendFactor(float val, float threshold)
{
    // sigmoid function
    // result is in ]0;1] range
    // inflexion point is at (x, y) (threshold, 0.5)
    return 1.0f / (1.0f + dt_fast_expf(16.0f - (16.0f / threshold) * val));
}

void dt_masks_calc_detail_mask(float *const restrict src, float *const restrict out, float *const restrict tmp, const int width, const int height, const float threshold, const gboolean detail)
{
  const size_t msize = width * height;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(src, tmp, msize, threshold, detail, out) \
  schedule(simd:static) aligned(src, tmp, out : 64)
#endif
  for(size_t idx = 0; idx < msize; idx++)
  {
    const float blend = _calcBlendFactor(src[idx], threshold);
    tmp[idx] = detail ? blend : 1.0f - blend;
  }
  dt_masks_blur_9x9(tmp, out, width, height, 2.0f);
}
#undef FAST_BLUR_5
#undef FAST_BLUR_9
#undef FAST_BLUR_13

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

