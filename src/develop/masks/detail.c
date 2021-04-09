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
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(src, out) \
  dt_omp_sharedconst(c42, c41, c40, c33, c32, c31, c30, c22, c21, c20, c11, c10, c00, w1, w2, w3, w4, width, height) \
  schedule(simd:static) aligned(src, out : 64)
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
}

void dt_masks_calc_luminance_mask(float *const restrict src, float *const restrict mask, const int width, const int height)
{
  const int msize = width * height;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(mask, src, msize) \
  schedule(simd:static) aligned(mask, src : 64)
#endif
  for(int idx =0; idx < msize; idx++)
  {
    const float val = 0.333333333f * (src[4 * idx] + src[4 * idx + 1] + src[4 * idx + 2]);
    mask[idx] = lab_f(val);
  }
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
  const float scale = 1.0f / 16.0f;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(src, tmp, width, height, scale) \
  schedule(simd:static) aligned(src, tmp : 64)
 #endif
  for(int row = 2; row < height - 2; row++)
  {
    for(int col = 2, idx = row * width + col; col < width - 2; col++, idx++)
    {
      tmp[idx] = scale * sqrtf(sqrf(src[idx+1] - src[idx-1]) + sqrf(src[idx + width]   - src[idx - width]) +
                               sqrf(src[idx+2] - src[idx-2]) + sqrf(src[idx + 2*width] - src[idx - 2*width]));
    }
  }
  dt_masks_extend_border(tmp, width, height, 2);

  const int msize = width * height;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(tmp, msize, threshold, detail) \
  schedule(simd:static) aligned(tmp : 64)
#endif
  for(int idx = 0; idx < msize; idx++)
  {
    const float blend = calcBlendFactor(tmp[idx], threshold);
    tmp[idx] = detail ? blend : 1.0f - blend;
  }
  dt_masks_blur_9x9(tmp, out, width, height, 2.0f);
}
