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
#include "common/math.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/openmp_maths.h"
#include <math.h>

#define DT_BLENDIF_RGB_CH 4
#define DT_BLENDIF_RGB_BCH 3


typedef void(_blend_row_func)(const float *const restrict a, const float *const restrict b,
                              float *const restrict out, const float *const restrict mask, const size_t stride);


#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ: 16)
#endif
static inline void _CLAMP_XYZ(float *const restrict XYZ)
{
  for(size_t i = 0; i < 3; i++) XYZ[i] = clamp_simd(XYZ[i]);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(src, dst: 16)
#endif
static inline void _PX_COPY(const float *const restrict src, float *const restrict dst)
{
  for(size_t i = 0; i < 3; i++) dst[i] = src[i];
}


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
static inline void _blendif_gray_fb(const float *const restrict pixels, float *const restrict mask,
                                    const size_t stride, const float *const restrict parameters,
                                    const unsigned int invert_mask)
{
  for(size_t x = 0, j = 0; x < stride; x++, j += DT_BLENDIF_RGB_CH)
  {
    const float value = 0.3f * pixels[j + 0] + 0.59f * pixels[j + 1] + 0.11f * pixels[j + 2];
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
#pragma omp declare simd aligned(pixels, invert_mask: 16) uniform(parameters, invert_mask, stride)
#endif
static inline void _blendif_hsl(const float *const restrict pixels, float *const restrict mask,
                                const size_t stride, const float *const restrict parameters,
                                const unsigned int *const restrict invert_mask)
{
  for(size_t x = 0, j = 0; x < stride; x++, j += DT_BLENDIF_RGB_CH)
  {
    dt_aligned_pixel_t HSL;
    dt_RGB_2_HSL(pixels + j, HSL);
    float factor = 1.0f;
    for(size_t i = 0; i < 3; i++)
      factor *= _blendif_compute_factor(HSL[i], invert_mask[i], parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * i);
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
    if(profile)
    {
      _blendif_gray(pixels, mask, stride, parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_GRAY_in,
                    invert_mask, profile);
    }
    else
    {
      _blendif_gray_fb(pixels, mask, stride,
                       parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_GRAY_in, invert_mask);
    }
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

  if(blendif & ((1 << DEVELOP_BLENDIF_H_in) | (1 << DEVELOP_BLENDIF_S_in) | (1 << DEVELOP_BLENDIF_l_in)))
  {
    const unsigned int invert_mask[3] DT_ALIGNED_PIXEL = {
        (blendif >> 16) & (1 << DEVELOP_BLENDIF_H_in),
        (blendif >> 16) & (1 << DEVELOP_BLENDIF_S_in),
        (blendif >> 16) & (1 << DEVELOP_BLENDIF_l_in),
    };
    _blendif_hsl(pixels, mask, stride, parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_H_in,
                 invert_mask);
  }
}

void dt_develop_blendif_rgb_hsl_make_mask(struct dt_dev_pixelpipe_iop_t *piece, const float *const restrict a,
                                          const float *const restrict b, const struct dt_iop_roi_t *const roi_in,
                                          const struct dt_iop_roi_t *const roi_out, float *const restrict mask)
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
    const int use_profile = dt_develop_blendif_init_masking_profile(piece, &blend_profile,
                                                                    DEVELOP_BLEND_CS_RGB_DISPLAY);
    const dt_iop_order_iccprofile_info_t *profile = use_profile ? &blend_profile : NULL;

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


/* normal blend with clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_normal_bounded(const float *const restrict a, const float *const restrict b,
                                  float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = clamp_simd(a[j + k] * (1.0f - local_opacity) + b[j + k] * local_opacity);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* normal blend without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_normal_unbounded(const float *const restrict a, const float *const restrict b,
                                    float *const restrict out, const float *const restrict mask,
                                    const size_t stride)
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

/* lighten */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_lighten(const float *const restrict a, const float *const restrict b,
                           float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = clamp_simd(a[j + k] * (1.0f - local_opacity) + fmaxf(a[j + k], b[j + k]) * local_opacity);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* darken */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_darken(const float *const restrict a, const float *const restrict b,
                          float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = clamp_simd(a[j + k] * (1.0f - local_opacity) + fminf(a[j + k], b[j + k]) * local_opacity);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* multiply */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_multiply(const float *const restrict a, const float *const restrict b,
                            float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = clamp_simd(a[j + k] * (1.0f - local_opacity) + (a[j + k] * b[j + k]) * local_opacity);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* average */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_average(const float *const restrict a, const float *const restrict b,
                           float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = clamp_simd(a[j + k] * (1.0f - local_opacity) + (a[j + k] + b[j + k]) / 2.0f * local_opacity);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* add */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_add(const float *const restrict a, const float *const restrict b,
                       float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = clamp_simd(a[j + k] * (1.0f - local_opacity) + (a[j + k] + b[j + k]) * local_opacity);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* subtract */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_subtract(const float *const restrict a, const float *const restrict b,
                            float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = clamp_simd(a[j + k] * (1.0f - local_opacity) + ((b[j + k] + a[j + k]) - 1.0f) * local_opacity);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* difference */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_difference(const float *const restrict a, const float *const restrict b,
                              float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      out[j + k] = clamp_simd(a[j + k] * (1.0f - local_opacity) + fabsf(a[j + k] - b[j + k]) * local_opacity);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* screen */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_screen(const float *const restrict a, const float *const restrict b,
                          float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_simd(a[j + k]);
      const float lb = clamp_simd(b[j + k]);
      out[j + k] = clamp_simd(la * (1.0f - local_opacity) + (1.0f - (1.0f - la) * (1.0f - lb)) * local_opacity);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* overlay */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_overlay(const float *const restrict a, const float *const restrict b,
                           float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_simd(a[j + k]);
      const float lb = clamp_simd(b[j + k]);
      out[j + k] = clamp_simd(
          la * (1.0f - local_opacity2)
          + (la > 0.5f ? 1.0f - (1.0f - 2.0f * (la - 0.5f)) * (1.0f - lb)
                       : 2.0f * la * lb)
            * local_opacity2);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* softlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_softlight(const float *const restrict a, const float *const restrict b,
                             float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_simd(a[j + k]);
      const float lb = clamp_simd(b[j + k]);
      out[j + k] = clamp_simd(
          la * (1.0f - local_opacity2)
          + (lb > 0.5f ? 1.0f - (1.0f - la) * (1.0f - (lb - 0.5f))
                       : la * (lb + 0.5f))
            * local_opacity2);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* hardlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_hardlight(const float *const restrict a, const float *const restrict b,
                             float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_simd(a[j + k]);
      const float lb = clamp_simd(b[j + k]);
      out[j + k] = clamp_simd(
          la * (1.0f - local_opacity2)
          + (lb > 0.5f ? 1.0f - (1.0f - 2.0f * (la - 0.5f)) * (1.0f - lb)
                       : 2.0f * la * lb)
            * local_opacity2);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* vividlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_vividlight(const float *const restrict a, const float *const restrict b,
                              float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    float local_opacity = mask[i];
    float local_opacity2 = local_opacity * local_opacity;
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_simd(a[j + k]);
      const float lb = clamp_simd(b[j + k]);
      out[j + k] = clamp_simd(
          la * (1.0f - local_opacity2)
          + (lb > 0.5f ? (lb >= 1.0f ? 1.0f : la / (2.0f * (1.0f - lb)))
                       : (lb <= 0.0f ? 0.0f : 1.0f - (1.0f - la) / (2.0f * lb)))
            * local_opacity2);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* linearlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_linearlight(const float *const restrict a, const float *const restrict b,
                               float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_simd(a[j + k]);
      const float lb = clamp_simd(b[j + k]);
      out[j + k] = clamp_simd(la * (1.0f - local_opacity2) + (la + 2.0f * lb - 1.0f) * local_opacity2);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* pinlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_pinlight(const float *const restrict a, const float *const restrict b,
                            float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_simd(a[j + k]);
      const float lb = clamp_simd(b[j + k]);
      out[j + k] = clamp_simd(
          la * (1.0f - local_opacity2)
          + (lb > 0.5f ? fmaxf(la, 2.0f * (lb - 0.5f))
                       : fminf(la, 2.0f * lb))
            * local_opacity2);
    }
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* lightness blend */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_lightness(const float *const restrict a, const float *const restrict b,
                             float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;
    dt_aligned_pixel_t tta, ttb;

    _PX_COPY(a + j, ta);
    _PX_COPY(b + j, tb);

    _CLAMP_XYZ(ta);
    _CLAMP_XYZ(tb);

    dt_RGB_2_HSL(ta, tta);
    dt_RGB_2_HSL(tb, ttb);

    ttb[0] = tta[0];
    ttb[1] = tta[1];
    ttb[2] = (tta[2] * (1.0f - local_opacity)) + ttb[2] * local_opacity;

    dt_HSL_2_RGB(ttb, out + j);
    _CLAMP_XYZ(out + j);

    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* chromaticity blend */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_chromaticity(const float *const restrict a, const float *const restrict b,
                                float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;
    dt_aligned_pixel_t tta, ttb;

    _PX_COPY(a + j, ta);
    _PX_COPY(b + j, tb);

    _CLAMP_XYZ(ta);
    _CLAMP_XYZ(tb);

    dt_RGB_2_HSL(ta, tta);
    dt_RGB_2_HSL(tb, ttb);

    ttb[0] = tta[0];
    ttb[1] = (tta[1] * (1.0f - local_opacity)) + ttb[1] * local_opacity;
    ttb[2] = tta[2];

    dt_HSL_2_RGB(ttb, out + j);
    _CLAMP_XYZ(out + j);

    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* hue blend */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_hue(const float *const restrict a, const float *const restrict b,
                       float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;
    dt_aligned_pixel_t tta, ttb;

    _PX_COPY(a + j, ta);
    _PX_COPY(b + j, tb);

    _CLAMP_XYZ(ta);
    _CLAMP_XYZ(tb);

    dt_RGB_2_HSL(ta, tta);
    dt_RGB_2_HSL(tb, ttb);

    /* blend hue along shortest distance on color circle */
    float d = fabsf(tta[0] - ttb[0]);
    float s = d > 0.5f ? -local_opacity * (1.0f - d) / d : local_opacity;
    ttb[0] = fmodf((tta[0] * (1.0f - s)) + ttb[0] * s + 1.0f, 1.0f);
    ttb[1] = tta[1];
    ttb[2] = tta[2];

    dt_HSL_2_RGB(ttb, out + j);
    _CLAMP_XYZ(out + j);

    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* color blend; blend hue and chroma, but not lightness */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_color(const float *const restrict a, const float *const restrict b,
                         float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;
    dt_aligned_pixel_t tta, ttb;

    _PX_COPY(a + j, ta);
    _PX_COPY(b + j, tb);

    _CLAMP_XYZ(ta);
    _CLAMP_XYZ(tb);

    dt_RGB_2_HSL(ta, tta);
    dt_RGB_2_HSL(tb, ttb);

    /* blend hue along shortest distance on color circle */
    float d = fabsf(tta[0] - ttb[0]);
    float s = d > 0.5f ? -local_opacity * (1.0f - d) / d : local_opacity;
    ttb[0] = fmodf((tta[0] * (1.0f - s)) + ttb[0] * s + 1.0f, 1.0f);

    ttb[1] = (tta[1] * (1.0f - local_opacity)) + ttb[1] * local_opacity;
    ttb[2] = tta[2];

    dt_HSL_2_RGB(ttb, out + j);
    _CLAMP_XYZ(out + j);

    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* color adjustment; blend hue and chroma; take lightness from module output */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_coloradjust(const float *const restrict a, const float *const restrict b,
                               float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;
    dt_aligned_pixel_t tta, ttb;

    _PX_COPY(a + j, ta);
    _PX_COPY(b + j, tb);

    _CLAMP_XYZ(ta);
    _CLAMP_XYZ(tb);

    dt_RGB_2_HSL(ta, tta);
    dt_RGB_2_HSL(tb, ttb);

    /* blend hue along shortest distance on color circle */
    const float d = fabsf(tta[0] - ttb[0]);
    const float s = d > 0.5f ? -local_opacity * (1.0f - d) / d : local_opacity;
    ttb[0] = fmodf((tta[0] * (1.0f - s)) + ttb[0] * s + 1.0f, 1.0f);

    ttb[1] = (tta[1] * (1.0f - local_opacity)) + ttb[1] * local_opacity;
    // ttb[2] (output lightness) unchanged

    dt_HSL_2_RGB(ttb, out + j);
    _CLAMP_XYZ(out + j);

    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* blend only lightness in HSV color space without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_HSV_value(const float *const restrict a, const float *const restrict b,
                             float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    dt_RGB_2_HSV(a + j, ta);
    dt_RGB_2_HSV(b + j, tb);

    // hue and saturation from input image
    tb[0] = ta[0];
    tb[1] = ta[1];

    // blend lightness between input and output
    tb[2] = ta[2] * (1.0f - local_opacity) + tb[2] * local_opacity;

    dt_HSV_2_RGB(tb, out + j);
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* blend only color in HSV color space without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b:16) uniform(stride)
#endif
static void _blend_HSV_color(const float *const restrict a, const float *const restrict b,
                             float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    dt_RGB_2_HSV(a + j, ta);
    dt_RGB_2_HSV(b + j, tb);

    // convert from polar to cartesian coordinates
    const float xa = ta[1] * cosf(2.0f * DT_M_PI_F * ta[0]);
    const float ya = ta[1] * sinf(2.0f * DT_M_PI_F * ta[0]);
    const float xb = tb[1] * cosf(2.0f * DT_M_PI_F * tb[0]);
    const float yb = tb[1] * sinf(2.0f * DT_M_PI_F * tb[0]);

    // blend color vectors of input and output
    const float xc = xa * (1.0f - local_opacity) + xb * local_opacity;
    const float yc = ya * (1.0f - local_opacity) + yb * local_opacity;

    tb[0] = atan2f(yc, xc) / (2.0f * DT_M_PI_F);
    if(tb[0] < 0.0f) tb[0] += 1.0f;
    tb[1] = sqrtf(xc * xc + yc * yc);

    // lightness from input image
    tb[2] = ta[2];

    dt_HSV_2_RGB(tb, out + j);
    out[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* blend only R-channel in RGB color space without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_RGB_R(const float *const restrict a, const float *const restrict b,
                         float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    out[j + 0] = a[j + 0] * (1.0f - local_opacity) + b[j + 0] * local_opacity;
    out[j + 1] = a[j + 1];
    out[j + 2] = a[j + 2];
    out[j + 3] = local_opacity;
  }
}

/* blend only R-channel in RGB color space without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_RGB_G(const float *const restrict a, const float *const restrict b,
                         float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    out[j + 0] = a[j + 0];
    out[j + 1] = a[j + 1] * (1.0f - local_opacity) + b[j + 1] * local_opacity;
    out[j + 2] = a[j + 2];
    out[j + 3] = local_opacity;
  }
}

/* blend only R-channel in RGB color space without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_RGB_B(const float *const restrict a, const float *const restrict b,
                         float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    out[j + 0] = a[j + 0];
    out[j + 1] = a[j + 1];
    out[j + 2] = a[j + 2] * (1.0f - local_opacity) + b[j + 2] * local_opacity;
    out[j + 3] = local_opacity;
  }
}


static _blend_row_func *_choose_blend_func(const unsigned int blend_mode)
{
  _blend_row_func *blend = NULL;

  /* select the blend operator */
  switch(blend_mode & DEVELOP_BLEND_MODE_MASK)
  {
    case DEVELOP_BLEND_LIGHTEN:
      blend = _blend_lighten;
      break;
    case DEVELOP_BLEND_DARKEN:
      blend = _blend_darken;
      break;
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
    case DEVELOP_BLEND_DIFFERENCE:
    case DEVELOP_BLEND_DIFFERENCE2:
      blend = _blend_difference;
      break;
    case DEVELOP_BLEND_SCREEN:
      blend = _blend_screen;
      break;
    case DEVELOP_BLEND_OVERLAY:
      blend = _blend_overlay;
      break;
    case DEVELOP_BLEND_SOFTLIGHT:
      blend = _blend_softlight;
      break;
    case DEVELOP_BLEND_HARDLIGHT:
      blend = _blend_hardlight;
      break;
    case DEVELOP_BLEND_VIVIDLIGHT:
      blend = _blend_vividlight;
      break;
    case DEVELOP_BLEND_LINEARLIGHT:
      blend = _blend_linearlight;
      break;
    case DEVELOP_BLEND_PINLIGHT:
      blend = _blend_pinlight;
      break;
    case DEVELOP_BLEND_LIGHTNESS:
      blend = _blend_lightness;
      break;
    case DEVELOP_BLEND_CHROMATICITY:
      blend = _blend_chromaticity;
      break;
    case DEVELOP_BLEND_HUE:
      blend = _blend_hue;
      break;
    case DEVELOP_BLEND_COLOR:
      blend = _blend_color;
      break;
    case DEVELOP_BLEND_BOUNDED:
      blend = _blend_normal_bounded;
      break;
    case DEVELOP_BLEND_COLORADJUST:
      blend = _blend_coloradjust;
      break;
    case DEVELOP_BLEND_HSV_VALUE:
      blend = _blend_HSV_value;
      break;
    case DEVELOP_BLEND_HSV_COLOR:
      blend = _blend_HSV_color;
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

    /* fallback to normal blend */
    case DEVELOP_BLEND_NORMAL2:
    default:
      blend = _blend_normal_unbounded;
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
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_H:
      // no boost factors for HSL
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        dt_aligned_pixel_t HSL;
        dt_RGB_2_HSL(a + j, HSL);
        const float c = clamp_simd(HSL[0]);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_H | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT:
      // no boost factors for HSL
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        dt_aligned_pixel_t HSL;
        dt_RGB_2_HSL(b + j, HSL);
        const float c = clamp_simd(HSL[0]);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_S:
      // no boost factors for HSL
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        dt_aligned_pixel_t HSL;
        dt_RGB_2_HSL(a + j, HSL);
        const float c = clamp_simd(HSL[1]);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_S | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT:
      // no boost factors for HSL
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        dt_aligned_pixel_t HSL;
        dt_RGB_2_HSL(b + j, HSL);
        const float c = clamp_simd(HSL[1]);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_l:
      // no boost factors for HSL
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        dt_aligned_pixel_t HSL;
        dt_RGB_2_HSL(a + j, HSL);
        const float c = clamp_simd(HSL[2]);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_l | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT:
      // no boost factors for HSL
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        dt_aligned_pixel_t HSL;
        dt_RGB_2_HSL(b + j, HSL);
        const float c = clamp_simd(HSL[2]);
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

void dt_develop_blendif_rgb_hsl_blend(struct dt_dev_pixelpipe_iop_t *piece,
                                      const float *const restrict a, float *const restrict b,
                                      const struct dt_iop_roi_t *const roi_in,
                                      const struct dt_iop_roi_t *const roi_out,
                                      const float *const restrict mask,
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
                                                                    DEVELOP_BLEND_CS_RGB_DISPLAY);
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
    _blend_row_func *const blend = _choose_blend_func(d->blend_mode);

    float *tmp_buffer = dt_alloc_align_float((size_t)owidth * oheight * DT_BLENDIF_RGB_CH);
    if(tmp_buffer != NULL)
    {
      dt_iop_image_copy(tmp_buffer, b, (size_t)owidth * oheight * DT_BLENDIF_RGB_CH);
      if((d->blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE)
      {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(a, b, tmp_buffer, mask, blend, oheight, owidth, iwidth, xoffs, yoffs)
#endif
        for(size_t y = 0; y < oheight; y++)
        {
          const size_t a_start = ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_RGB_CH;
          const size_t b_start = y * owidth * DT_BLENDIF_RGB_CH;
          const size_t m_start = y * owidth;
          blend(tmp_buffer + b_start, a + a_start, b + b_start, mask + m_start, owidth);
        }
      }
      else
      {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(a, b, tmp_buffer, mask, blend, oheight, owidth, iwidth, xoffs, yoffs)
#endif
        for(size_t y = 0; y < oheight; y++)
        {
          const size_t a_start = ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_RGB_CH;
          const size_t b_start = y * owidth * DT_BLENDIF_RGB_CH;
          const size_t m_start = y * owidth;
          blend(a + a_start, tmp_buffer + b_start, b + b_start, mask + m_start, owidth);
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
