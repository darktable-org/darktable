/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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

#if defined(__GNUC__)
#pragma GCC optimize("unroll-loops", "tree-loop-if-convert", "tree-loop-distribution", "no-strict-aliasing",      \
                     "loop-interchange", "loop-nest-optimize", "tree-loop-im", "unswitch-loops",                  \
                     "tree-loop-ivcanon", "ira-loop-pressure", "split-ivs-in-unroller", "tree-loop-vectorize",    \
                     "variable-expansion-in-unroller", "split-loops", "ivopts", "predictive-commoning",           \
                     "tree-loop-linear", "loop-block", "loop-strip-mine", "finite-math-only", "fp-contract=fast", \
                     "fast-math", "no-math-errno")
#endif

#include "common/colorspaces_inline_conversions.h"
#include "common/imagebuf.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/openmp_maths.h"
#include <math.h>

#define DT_BLENDIF_RGB_CH 4
#define DT_BLENDIF_RGB_BCH 3


typedef void(_blend_row_func)(const float *const restrict a, const float *const restrict b, const float p,
                              float *const restrict out, const float *const restrict mask, const size_t stride);


#ifdef _OPENMP
#pragma omp declare simd uniform(parameters, invert_mask)
#endif
static inline float _blendif_compute_factor(const float value, const unsigned int invert_mask,
                                            const float *const restrict parameters)
{
  float factor = 0.0f;
  if(value <= parameters[0])
  {
   // we are below the keyframe
   factor = 0.0f;
  }
  else if(value < parameters[1])
  {
   // we are on the bottom slope of the keyframe
   factor = (value - parameters[0]) * parameters[4];
  }
  else if(value <= parameters[2])
  {
   // we are on the ramp - constant part - of the keyframe
   factor = 1.0f;
  }
  else if(value < parameters[3])
  {
   // we are on the top slope of the keyframe
   factor = 1.0f - (value - parameters[2]) * parameters[5];
  }
  else
  {
   // we are above the keyframe
   factor = 0.0f;
  }
  return invert_mask ? 1.0f - factor : factor; // inverted channel?
}

#ifdef _OPENMP
#pragma omp declare simd aligned(pixels: 16) uniform(parameters, invert_mask, stride, profile)
#endif
static inline void _blendif_gray(const float *const restrict pixels, float *const restrict mask,
                                 const size_t stride, const float *const restrict parameters,
                                 const unsigned int invert_mask,
                                 const dt_iop_order_iccprofile_info_t *const restrict profile)
{
  for(size_t x = 0, j = 0; x < stride; x++, j += DT_BLENDIF_RGB_CH)
  {
    const float value = dt_ioppr_get_rgb_matrix_luminance(pixels + j, profile->matrix_in, profile->lut_in,
                                                          profile->unbounded_coeffs_in, profile->lutsize,
                                                          profile->nonlinearlut);
    mask[x] *= _blendif_compute_factor(value, invert_mask, parameters);
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(pixels: 16) uniform(parameters, invert_mask, stride)
#endif
static inline void _blendif_rgb_red(const float *const restrict pixels, float *const restrict mask,
                                    const size_t stride, const float *const restrict parameters,
                                    const unsigned int invert_mask)
{
  for(size_t x = 0, j = 0; x < stride; x++, j += DT_BLENDIF_RGB_CH)
  {
    mask[x] *= _blendif_compute_factor(pixels[j + 0], invert_mask, parameters);
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(pixels: 16) uniform(parameters, invert_mask, stride)
#endif
static inline void _blendif_rgb_green(const float *const restrict pixels, float *const restrict mask,
                                      const size_t stride, const float *const restrict parameters,
                                      const unsigned int invert_mask)
{
  for(size_t x = 0, j = 0; x < stride; x++, j += DT_BLENDIF_RGB_CH)
  {
    mask[x] *= _blendif_compute_factor(pixels[j + 1], invert_mask, parameters);
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(pixels: 16) uniform(parameters, invert_mask, stride)
#endif
static inline void _blendif_rgb_blue(const float *const restrict pixels, float *const restrict mask,
                                     const size_t stride, const float *const restrict parameters,
                                     const unsigned int invert_mask)
{
  for(size_t x = 0, j = 0; x < stride; x++, j += DT_BLENDIF_RGB_CH)
  {
    mask[x] *= _blendif_compute_factor(pixels[j + 2], invert_mask, parameters);
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(pixels, invert_mask: 16) uniform(parameters, invert_mask, stride, profile)
#endif
static inline void _blendif_jzczhz(const float *const restrict pixels, float *const restrict mask,
                                   const size_t stride, const float *const restrict parameters,
                                   const unsigned int *const restrict invert_mask,
                                   const dt_iop_order_iccprofile_info_t *const restrict profile)
{
  for(size_t x = 0, j = 0; x < stride; x++, j += DT_BLENDIF_RGB_CH)
  {
    dt_aligned_pixel_t XYZ_D65;
    dt_aligned_pixel_t JzAzBz;
    dt_aligned_pixel_t JzCzhz;

    // use the matrix_out of the hacked profile for blending to use the
    // conversion from RGB to XYZ D65 (instead of XYZ D50)
    dt_ioppr_rgb_matrix_to_xyz(pixels + j, XYZ_D65, profile->matrix_out_transposed, profile->lut_in,
                               profile->unbounded_coeffs_in, profile->lutsize, profile->nonlinearlut);

    dt_XYZ_2_JzAzBz(XYZ_D65, JzAzBz);
    dt_JzAzBz_2_JzCzhz(JzAzBz, JzCzhz);

    float factor = 1.0f;
    for(size_t i = 0; i < 3; i++)
      factor *= _blendif_compute_factor(JzCzhz[i], invert_mask[i],
                                        parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * i);
    mask[x] *= factor;
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(pixels: 16) uniform(stride, blendif, parameters, profile)
#endif
static void _blendif_combine_channels(const float *const restrict pixels, float *const restrict mask,
                                      const size_t stride, const unsigned int blendif,
                                      const float *const restrict parameters,
                                      const dt_iop_order_iccprofile_info_t *const restrict profile)
{
  if(blendif & (1 << DEVELOP_BLENDIF_GRAY_in))
  {
    const unsigned int invert_mask = (blendif >> 16) & (1 << DEVELOP_BLENDIF_GRAY_in);
    _blendif_gray(pixels, mask, stride, parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_GRAY_in,
                  invert_mask, profile);
  }

  if(blendif & (1 << DEVELOP_BLENDIF_RED_in))
  {
    const unsigned int invert_mask = (blendif >> 16) & (1 << DEVELOP_BLENDIF_RED_in);
    _blendif_rgb_red(pixels, mask, stride, parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_RED_in,
                     invert_mask);
  }

  if(blendif & (1 << DEVELOP_BLENDIF_GREEN_in))
  {
    const unsigned int invert_mask = (blendif >> 16) & (1 << DEVELOP_BLENDIF_GREEN_in);
    _blendif_rgb_green(pixels, mask, stride,
                       parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_GREEN_in, invert_mask);
  }

  if(blendif & (1 << DEVELOP_BLENDIF_BLUE_in))
  {
    const unsigned int invert_mask = (blendif >> 16) & (1 << DEVELOP_BLENDIF_BLUE_in);
    _blendif_rgb_blue(pixels, mask, stride, parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_BLUE_in,
                      invert_mask);
  }

  if(blendif & ((1 << DEVELOP_BLENDIF_Jz_in) | (1 << DEVELOP_BLENDIF_Cz_in) | (1 << DEVELOP_BLENDIF_hz_in)))
  {
    const unsigned int invert_mask[3] DT_ALIGNED_PIXEL = {
        (blendif >> 16) & (1 << DEVELOP_BLENDIF_Jz_in),
        (blendif >> 16) & (1 << DEVELOP_BLENDIF_Cz_in),
        (blendif >> 16) & (1 << DEVELOP_BLENDIF_hz_in),
    };
    _blendif_jzczhz(pixels, mask, stride, parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_Jz_in,
                    invert_mask, profile);
  }
}

void dt_develop_blendif_rgb_jzczhz_make_mask(struct dt_dev_pixelpipe_iop_t *piece,
                                             const float *const restrict a,
                                             const float *const restrict b,
                                             const struct dt_iop_roi_t *const roi_in,
                                             const struct dt_iop_roi_t *const roi_out,
                                             float *const restrict mask)
{
  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *const)piece->blendop_data;

  if(piece->colors != DT_BLENDIF_RGB_CH) return;

  const int xoffs = roi_out->x - roi_in->x;
  const int yoffs = roi_out->y - roi_in->y;
  const int iwidth = roi_in->width;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;

  const unsigned int any_channel_active = d->blendif & DEVELOP_BLENDIF_RGB_MASK;
  const unsigned int mask_inclusive = d->mask_combine & DEVELOP_COMBINE_INCL;
  const unsigned int mask_inversed = d->mask_combine & DEVELOP_COMBINE_INV;

  // invert the individual channels if the combine mode is inclusive
  const unsigned int blendif = d->blendif ^ (mask_inclusive ? DEVELOP_BLENDIF_RGB_MASK << 16 : 0);

  // a channel cancels the mask if the whole span is selected and the channel is inverted
  const unsigned int canceling_channel = (blendif >> 16) & ~blendif & DEVELOP_BLENDIF_RGB_MASK;

  const size_t buffsize = (size_t)owidth * oheight;

  // get the clipped opacity value  0 - 1
  const float global_opacity = clamp_simd(d->opacity / 100.0f);

  if(!(d->mask_mode & DEVELOP_MASK_CONDITIONAL) || (!canceling_channel && !any_channel_active))
  {
    // mask is not conditional, invert the mask if required
    if(mask_inversed)
    {
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(mask, buffsize, global_opacity) schedule(static)
#endif
      for(size_t x = 0; x < buffsize; x++) mask[x] = global_opacity * (1.0f - mask[x]);
    }
    else
    {
      dt_iop_image_mul_const(mask,global_opacity,owidth,oheight,1); // mask[k] *= global_opacity;
    }
  }
  else if(canceling_channel || !any_channel_active)
  {
    // one of the conditional channel selects nothing
    // this means that the conditional opacity of all pixels is the same
    // and depends on whether the mask combination is inclusive and whether the mask is inverted
    const float opac = ((mask_inversed == 0) ^ (mask_inclusive == 0)) ? global_opacity : 0.0f;
    dt_iop_image_fill(mask,opac,owidth,oheight,1); // mask[k] = opac;
  }
  else
  {
    // we need to process all conditional channels

    // parameters, for every channel the 4 limits + pre-computed increasing slope and decreasing slope
    float parameters[DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_SIZE] DT_ALIGNED_ARRAY;
    dt_develop_blendif_process_parameters(parameters, d);

    dt_iop_order_iccprofile_info_t blend_profile;
    if(!dt_develop_blendif_init_masking_profile(piece, &blend_profile, DEVELOP_BLEND_CS_RGB_SCENE))
    {
      return;
    }
    const dt_iop_order_iccprofile_info_t *profile = &blend_profile;

    // allocate space for a temporary mask buffer to split the computation of every channel
    float *const restrict temp_mask = dt_alloc_align_float(buffsize);
    if(!temp_mask)
    {
      return;
    }

#ifdef _OPENMP
#pragma omp parallel default(none) \
  dt_omp_firstprivate(temp_mask, mask, a, b, oheight, owidth, iwidth, yoffs, xoffs, buffsize, \
                      blendif, profile, parameters, mask_inclusive, mask_inversed, global_opacity)
#endif
    {
#ifdef __SSE2__
      // flush denormals to zero to avoid performance penalty if there are a lot of zero values in the mask
      const int oldMode = _MM_GET_FLUSH_ZERO_MODE();
      _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif

      // initialize the parametric mask
#ifdef _OPENMP
#pragma omp for simd schedule(static) aligned(temp_mask:64)
#endif
      for(size_t x = 0; x < buffsize; x++) temp_mask[x] = 1.0f;

      // combine channels
#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
      for(size_t y = 0; y < oheight; y++)
      {
        const size_t start = ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_RGB_CH;
        _blendif_combine_channels(a + start, temp_mask + (y * owidth), owidth, blendif, parameters, profile);
      }
#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
      for(size_t y = 0; y < oheight; y++)
      {
        const size_t start = (y * owidth) * DT_BLENDIF_RGB_CH;
        _blendif_combine_channels(b + start, temp_mask + (y * owidth), owidth, blendif >> DEVELOP_BLENDIF_GRAY_out,
                                  parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_GRAY_out,
                                  profile);
      }

      // apply global opacity
      if(mask_inclusive)
      {
        if(mask_inversed)
        {
#ifdef _OPENMP
#pragma omp for simd schedule(static) aligned(mask, temp_mask:64)
#endif
          for(size_t x = 0; x < buffsize; x++) mask[x] = global_opacity * (1.0f - mask[x]) * temp_mask[x];
        }
        else
        {
#ifdef _OPENMP
#pragma omp for simd schedule(static) aligned(mask, temp_mask:64)
#endif
          for(size_t x = 0; x < buffsize; x++) mask[x] = global_opacity * (1.0f - (1.0f - mask[x]) * temp_mask[x]);
        }
      }
      else
      {
        if(mask_inversed)
        {
#ifdef _OPENMP
#pragma omp for simd schedule(static) aligned(mask, temp_mask:64)
#endif
          for(size_t x = 0; x < buffsize; x++) mask[x] = global_opacity * (1.0f - mask[x] * temp_mask[x]);
        }
        else
        {
#ifdef _OPENMP
#pragma omp for simd schedule(static) aligned(mask, temp_mask:64)
#endif
          for(size_t x = 0; x < buffsize; x++) mask[x] = global_opacity * mask[x] * temp_mask[x];
        }
      }

#ifdef __SSE2__
      _MM_SET_FLUSH_ZERO_MODE(oldMode);
#endif
    }

    dt_free_align(temp_mask);
  }
}


/* normal blend without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_normal(const float *const restrict a, const float *const restrict b, const float p,
                          float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = a[j + k] * (1.0f - local_opacity) + b[j + k] * local_opacity;
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* multiply */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_multiply(const float *const restrict a, const float *const restrict b, const float p,
                            float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = a[j + k] * (1.0f - local_opacity) + (a[j + k] * b[j + k] * p) * local_opacity;
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* add */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_add(const float *const restrict a, const float *const restrict b, const float p,
                       float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = a[j + k] * (1.0f - local_opacity) + (a[j + k] + p * b[j + k]) * local_opacity;
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* subtract */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_subtract(const float *const restrict a, const float *const restrict b, const float p,
                            float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = a[j + k] * (1.0f - local_opacity) + fmaxf(a[j + k] - p * b[j + k], 0.0f) * local_opacity;
    }
    out[j + 3] = local_opacity;
  }
}

/* subtract inverse */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_subtract_inverse(const float *const restrict a, const float *const restrict b, const float p,
                                    float *const restrict out, const float *const restrict mask,
                                    const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = a[j + k] * (1.0f - local_opacity) + fmaxf(b[j + k] - p * a[j + k], 0.0f) * local_opacity;
    }
    out[j + 3] = local_opacity;
  }
}

/* difference */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_difference(const float *const restrict a, const float *const restrict b, const float p,
                              float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = a[j + k] * (1.0f - local_opacity) + fabsf(a[j + k] - b[j + k]) * local_opacity;
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* divide */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_divide(const float *const restrict a, const float *const restrict b, const float p,
                          float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = a[j + k] * (1.0f - local_opacity) + a[j + k] / fmaxf(p * b[j + k], 1e-6f) * local_opacity;
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* divide inverse */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_divide_inverse(const float *const restrict a, const float *const restrict b, const float p,
                                  float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = a[j + k] * (1.0f - local_opacity) + b[j + k] / fmaxf(p * a[j + k], 1e-6f) * local_opacity;
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* average */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_average(const float *const restrict a, const float *const restrict b, const float p,
                           float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = a[j + k] * (1.0f - local_opacity) + (a[j + k] + b[j + k]) / 2.0f * local_opacity;
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* geometric mean */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_geometric_mean(const float *const restrict a, const float *const restrict b, const float p,
                                  float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = a[j + k] * (1.0f - local_opacity) + sqrtf(fmax(a[j + k] * b[j + k], 0.0f)) * local_opacity;
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* harmonic mean */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_harmonic_mean(const float *const restrict a, const float *const restrict b, const float p,
                                 float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      // consider that pixel values should be positive
      out[j + k] = a[j + k] * (1.0f - local_opacity)
          + 2.0f * a[j + k] * b[j + k] / (fmaxf(a[j + k], 5e-7f) + fmaxf(b[j + k], 5e-7f)) * local_opacity;
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* chromaticity */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_chromaticity(const float *const restrict a, const float *const restrict b, const float p,
                                float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    const float norm_a = fmax(sqrtf(sqf(a[j]) + sqf(a[j + 1]) + sqf(a[j + 2])), 1e-6f);
    const float norm_b = fmax(sqrtf(sqf(b[j]) + sqf(b[j + 1]) + sqf(b[j + 2])), 1e-6f);
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = a[j + k] * (1.0f - local_opacity) + b[j + k] * norm_a / norm_b * local_opacity;
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* luminance */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_luminance(const float *const restrict a, const float *const restrict b, const float p,
                             float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    const float norm_a = fmax(sqrtf(sqf(a[j]) + sqf(a[j + 1]) + sqf(a[j + 2])), 1e-6f);
    const float norm_b = fmax(sqrtf(sqf(b[j]) + sqf(b[j + 1]) + sqf(b[j + 2])), 1e-6f);
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = a[j + k] * (1.0f - local_opacity) + a[j + k] * norm_b / norm_a * local_opacity;
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* blend only R-channel in RGB color space without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_RGB_R(const float *const restrict a, const float *const restrict b, const float p,
                         float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    out[j + 0] = a[j + 0] * (1.0f - local_opacity) + p * b[j + 0] * local_opacity;
    out[j + 1] = a[j + 1];
    out[j + 2] = a[j + 2];
    out[j + 3] = local_opacity;
  }
}

/* blend only R-channel in RGB color space without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_RGB_G(const float *const restrict a, const float *const restrict b, const float p,
                         float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    out[j + 0] = a[j + 0];
    out[j + 1] = a[j + 1] * (1.0f - local_opacity) + p * b[j + 1] * local_opacity;
    out[j + 2] = a[j + 2];
    out[j + 3] = local_opacity;
  }
}

/* blend only R-channel in RGB color space without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(p, stride)
#endif
static void _blend_RGB_B(const float *const restrict a, const float *const restrict b, const float p,
                         float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    out[j + 0] = a[j + 0];
    out[j + 1] = a[j + 1];
    out[j + 2] = a[j + 2] * (1.0f - local_opacity) + p * b[j + 2] * local_opacity;
    out[j + 3] = local_opacity;
  }
}


static _blend_row_func *_choose_blend_func(const unsigned int blend_mode)
{
  _blend_row_func *blend = NULL;

  /* select the blend operator */
  switch(blend_mode & DEVELOP_BLEND_MODE_MASK)
  {
    case DEVELOP_BLEND_MULTIPLY:
      blend = _blend_multiply;
      break;
    case DEVELOP_BLEND_AVERAGE:
      blend = _blend_average;
      break;
    case DEVELOP_BLEND_ADD:
      blend = _blend_add;
      break;
    case DEVELOP_BLEND_SUBTRACT:
      blend = _blend_subtract;
      break;
    case DEVELOP_BLEND_SUBTRACT_INVERSE:
      blend = _blend_subtract_inverse;
      break;
    case DEVELOP_BLEND_DIFFERENCE:
    case DEVELOP_BLEND_DIFFERENCE2:
      blend = _blend_difference;
      break;
    case DEVELOP_BLEND_DIVIDE:
      blend = _blend_divide;
      break;
    case DEVELOP_BLEND_DIVIDE_INVERSE:
      blend = _blend_divide_inverse;
      break;
    case DEVELOP_BLEND_LIGHTNESS:
      blend = _blend_luminance;
      break;
    case DEVELOP_BLEND_CHROMATICITY:
      blend = _blend_chromaticity;
      break;
    case DEVELOP_BLEND_RGB_R:
      blend = _blend_RGB_R;
      break;
    case DEVELOP_BLEND_RGB_G:
      blend = _blend_RGB_G;
      break;
    case DEVELOP_BLEND_RGB_B:
      blend = _blend_RGB_B;
      break;
    case DEVELOP_BLEND_GEOMETRIC_MEAN:
      blend = _blend_geometric_mean;
      break;
    case DEVELOP_BLEND_HARMONIC_MEAN:
      blend = _blend_harmonic_mean;
      break;

    /* fallback to normal blend */
    default:
      blend = _blend_normal;
      break;
  }

  return blend;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(rgb: 16) uniform(profile)
#endif
static inline float _rgb_luminance(const float *const restrict rgb,
                                   const dt_iop_order_iccprofile_info_t *const restrict profile)
{
  float value = 0.0f;
  if(profile)
    value = dt_ioppr_get_rgb_matrix_luminance(rgb, profile->matrix_in, profile->lut_in,
                                              profile->unbounded_coeffs_in, profile->lutsize,
                                              profile->nonlinearlut);
  else
    value = 0.3f * rgb[0] + 0.59f * rgb[1] + 0.11f * rgb[2];
  return value;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(rgb, JzCzhz: 16) uniform(profile)
#endif
static inline void _rgb_to_JzCzhz(const dt_aligned_pixel_t rgb, dt_aligned_pixel_t JzCzhz,
                                  const dt_iop_order_iccprofile_info_t *const restrict profile)
{
  dt_aligned_pixel_t JzAzBz = { 0.0f, 0.0f, 0.0f };

  if(profile)
  {
    dt_aligned_pixel_t XYZ_D65 = { 0.0f, 0.0f, 0.0f };
    // use the matrix_out of the hacked profile for blending to use the
    // conversion from RGB to XYZ D65 (instead of XYZ D50)
    dt_ioppr_rgb_matrix_to_xyz(rgb, XYZ_D65, profile->matrix_out_transposed, profile->lut_in, profile->unbounded_coeffs_in,
                               profile->lutsize, profile->nonlinearlut);
    dt_XYZ_2_JzAzBz(XYZ_D65, JzAzBz);
  }
  else
  {
    // This should not happen (we don't know what RGB is), but use this when profile is not defined
    dt_XYZ_2_JzAzBz(rgb, JzAzBz);
  }

  dt_JzAzBz_2_JzCzhz(JzAzBz, JzCzhz);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(a, b:16) uniform(channel, profile, stride)
#endif
static void _display_channel(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride, const int channel,
                             const float *const restrict boost_factors,
                             const dt_iop_order_iccprofile_info_t *const profile)
{
  switch(channel)
  {
    case DT_DEV_PIXELPIPE_DISPLAY_R:
    {
      const float factor = 1.0f / exp2f(boost_factors[DEVELOP_BLENDIF_RED_in]);
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_simd(a[j + 0] * factor);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_R | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT:
    {
      const float factor = 1.0f / exp2f(boost_factors[DEVELOP_BLENDIF_RED_out]);
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_simd(b[j + 0] * factor);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_G:
    {
      const float factor = 1.0f / exp2f(boost_factors[DEVELOP_BLENDIF_GREEN_in]);
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_simd(a[j + 1] * factor);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_G | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT:
    {
      const float factor = 1.0f / exp2f(boost_factors[DEVELOP_BLENDIF_GREEN_out]);
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_simd(b[j + 1] * factor);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_B:
    {
      const float factor = 1.0f / exp2f(boost_factors[DEVELOP_BLENDIF_BLUE_in]);
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_simd(a[j + 2] * factor);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_B | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT:
    {
      const float factor = 1.0f / exp2f(boost_factors[DEVELOP_BLENDIF_BLUE_out]);
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_simd(b[j + 2] * factor);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_GRAY:
    {
      const float factor = 1.0f / exp2f(boost_factors[DEVELOP_BLENDIF_GRAY_in]);
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_simd(_rgb_luminance(a + j, profile) * factor);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_GRAY | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT:
    {
      const float factor = 1.0f / exp2f(boost_factors[DEVELOP_BLENDIF_GRAY_out]);
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_simd(_rgb_luminance(b + j, profile) * factor);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Jz:
    {
      const float factor = 1.0f / exp2f(boost_factors[DEVELOP_BLENDIF_Jz_in]);
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        dt_aligned_pixel_t JzCzhz;
        _rgb_to_JzCzhz(a + j, JzCzhz, profile);
        const float c = clamp_simd(JzCzhz[0] * factor);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Jz | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT:
    {
      const float factor = 1.0f / exp2f(boost_factors[DEVELOP_BLENDIF_Jz_out]);
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        dt_aligned_pixel_t JzCzhz;
        _rgb_to_JzCzhz(b + j, JzCzhz, profile);
        const float c = clamp_simd(JzCzhz[0] * factor);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Cz:
    {
      const float factor = 1.0f / exp2f(boost_factors[DEVELOP_BLENDIF_Cz_in]);
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        dt_aligned_pixel_t JzCzhz;
        _rgb_to_JzCzhz(a + j, JzCzhz, profile);
        const float c = clamp_simd(JzCzhz[1] * factor);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Cz | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT:
    {
      const float factor = 1.0f / exp2f(boost_factors[DEVELOP_BLENDIF_Cz_out]);
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        dt_aligned_pixel_t JzCzhz;
        _rgb_to_JzCzhz(b + j, JzCzhz, profile);
        const float c = clamp_simd(JzCzhz[1] * factor);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_hz:
      // no boost factor for hues
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        dt_aligned_pixel_t JzCzhz;
        _rgb_to_JzCzhz(a + j, JzCzhz, profile);
        const float c = clamp_simd(JzCzhz[2]);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_hz | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT:
      // no boost factor for hues
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        dt_aligned_pixel_t JzCzhz;
        _rgb_to_JzCzhz(b + j, JzCzhz, profile);
        const float c = clamp_simd(JzCzhz[2]);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    default:
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = 0.0f;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(a, b:16) uniform(stride)
#endif
static inline void _copy_mask(const float *const restrict a, float *const restrict b, const size_t stride)
{
#ifdef _OPENMP
#pragma omp simd aligned(a, b: 16)
#endif
  for(size_t x = DT_BLENDIF_RGB_BCH; x < stride; x += DT_BLENDIF_RGB_CH) b[x] = a[x];
}

void dt_develop_blendif_rgb_jzczhz_blend(struct dt_dev_pixelpipe_iop_t *piece, const float *const restrict a,
                                         float *const restrict b, const struct dt_iop_roi_t *const roi_in,
                                         const struct dt_iop_roi_t *const roi_out, const float *const restrict mask,
                                         const dt_dev_pixelpipe_display_mask_t request_mask_display)
{
  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *const)piece->blendop_data;

  if(piece->colors != DT_BLENDIF_RGB_CH) return;

  const int xoffs = roi_out->x - roi_in->x;
  const int yoffs = roi_out->y - roi_in->y;
  const int iwidth = roi_in->width;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;

  // only non-zero if mask_display was set by an _earlier_ module
  const dt_dev_pixelpipe_display_mask_t mask_display = piece->pipe->mask_display;

  // process the blending operator
  if(request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
  {
    dt_iop_order_iccprofile_info_t blend_profile;
    const int use_profile = dt_develop_blendif_init_masking_profile(piece, &blend_profile,
                                                                    DEVELOP_BLEND_CS_RGB_SCENE);
    const dt_iop_order_iccprofile_info_t *profile = use_profile ? &blend_profile : NULL;
    const float *const restrict boost_factors = d->blendif_boost_factors;
    const dt_dev_pixelpipe_display_mask_t channel = request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(a, b, mask, channel, oheight, owidth, iwidth, xoffs, yoffs, boost_factors, profile)
#endif
    for(size_t y = 0; y < oheight; y++)
    {
      const size_t a_start = ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_RGB_CH;
      const size_t b_start = y * owidth * DT_BLENDIF_RGB_CH;
      const size_t m_start = y * owidth;
      _display_channel(a + a_start, b + b_start, mask + m_start, owidth, channel, boost_factors, profile);
    }
  }
  else
  {
    const float p = exp2f(d->blend_parameter);
    _blend_row_func *const blend = _choose_blend_func(d->blend_mode);

    float *tmp_buffer = dt_alloc_align_float((size_t)owidth * oheight * DT_BLENDIF_RGB_CH);
    if(tmp_buffer != NULL)
    {
      dt_iop_image_copy(tmp_buffer, b, (size_t)owidth * oheight * DT_BLENDIF_RGB_CH);
      if((d->blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE)
      {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(a, b, tmp_buffer, mask, blend, oheight, owidth, iwidth, xoffs, yoffs, p)
#endif
        for(size_t y = 0; y < oheight; y++)
        {
          const size_t a_start = ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_RGB_CH;
          const size_t b_start = y * owidth * DT_BLENDIF_RGB_CH;
          const size_t m_start = y * owidth;
          blend(tmp_buffer + b_start, a + a_start, p, b + b_start, mask + m_start, owidth);
        }
      }
      else
      {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(a, b, tmp_buffer, mask, blend, oheight, owidth, iwidth, xoffs, yoffs, p)
#endif
        for(size_t y = 0; y < oheight; y++)
        {
          const size_t a_start = ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_RGB_CH;
          const size_t b_start = y * owidth * DT_BLENDIF_RGB_CH;
          const size_t m_start = y * owidth;
          blend(a + a_start, tmp_buffer + b_start, p, b + b_start, mask + m_start, owidth);
        }
      }
      dt_free_align(tmp_buffer);
    }
  }

  if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
  {
    const size_t stride = owidth * DT_BLENDIF_RGB_CH;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(a, b, oheight, stride, iwidth, xoffs, yoffs)
#endif
    for(size_t y = 0; y < oheight; y++)
    {
      const size_t a_start = ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_RGB_CH;
      const size_t b_start = y * stride;
      _copy_mask(a + a_start, b + b_start, stride);
    }
  }
}

// tools/update_modelines.sh
// remove-trailing-space on;
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
