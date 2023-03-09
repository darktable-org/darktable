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

#define DT_BLENDIF_LAB_CH 4
#define DT_BLENDIF_LAB_BCH 3


typedef void(_blend_row_func)(const float *const restrict a, const float *const restrict b,
                              float *const restrict out, const float *const restrict mask, const size_t stride,
                              const dt_aligned_pixel_t min, const dt_aligned_pixel_t max);


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float _CLAMP(const float x, const float min, const float max)
{
  return fminf(fmaxf(x, min), max);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, min, max: 16)
#endif
static inline void _CLAMP_XYZ(dt_aligned_pixel_t XYZ, const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for_each_channel(i) XYZ[i] = fminf(fmaxf(XYZ[i], min[i]), max[i]);
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
#pragma omp declare simd aligned(pixels: 16) uniform(parameters, invert_mask, stride)
#endif
static inline void _blendif_lab_l(const float *const restrict pixels, float *const restrict mask,
                                  const size_t stride, const float *const restrict parameters,
                                  const unsigned int invert_mask)
{
  for(size_t x = 0, j = 0; x < stride; x++, j += DT_BLENDIF_LAB_CH)
  {
    mask[x] *= _blendif_compute_factor(pixels[j + 0] / 100.0f, invert_mask, parameters);
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(pixels: 16) uniform(parameters, invert_mask, stride)
#endif
static inline void _blendif_lab_a(const float *const restrict pixels, float *const restrict mask,
                                  const size_t stride, const float *const restrict parameters,
                                  const unsigned int invert_mask)
{
  for(size_t x = 0, j = 0; x < stride; x++, j += DT_BLENDIF_LAB_CH)
  {
    mask[x] *= _blendif_compute_factor(pixels[j + 1] / 256.0f, invert_mask, parameters);
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(pixels: 16) uniform(parameters, invert_mask, stride)
#endif
static inline void _blendif_lab_b(const float *const restrict pixels, float *const restrict mask,
                                  const size_t stride, const float *const restrict parameters,
                                  const unsigned int invert_mask)
{
  for(size_t x = 0, j = 0; x < stride; x++, j += DT_BLENDIF_LAB_CH)
  {
    mask[x] *= _blendif_compute_factor(pixels[j + 2] / 256.0f, invert_mask, parameters);
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(pixels, invert_mask: 16) uniform(parameters, invert_mask, stride)
#endif
static inline void _blendif_lch(const float *const restrict pixels, float *const restrict mask,
                                const size_t stride, const float *const restrict parameters,
                                const unsigned int *const restrict invert_mask)
{
  const float c_scale = 1.0f / (128.0f * sqrtf(2.0f));
  for(size_t x = 0, j = 0; x < stride; x++, j += DT_BLENDIF_LAB_CH)
  {
    dt_aligned_pixel_t LCH;
    dt_Lab_2_LCH(pixels + j, LCH);
    float factor = 1.0f;
    factor *= _blendif_compute_factor(LCH[1] * c_scale, invert_mask[0], parameters);
    factor *= _blendif_compute_factor(LCH[2], invert_mask[1], parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS);
    mask[x] *= factor;
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(pixels: 16) uniform(stride, blendif, parameters)
#endif
static void _blendif_combine_channels(const float *const restrict pixels, float *const restrict mask,
                                      const size_t stride, const unsigned int blendif,
                                      const float *const restrict parameters)
{
  if(blendif & (1 << DEVELOP_BLENDIF_L_in))
  {
    const unsigned int invert_mask = (blendif >> 16) & (1 << DEVELOP_BLENDIF_L_in);
    _blendif_lab_l(pixels, mask, stride, parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_L_in,
                   invert_mask);
  }

  if(blendif & (1 << DEVELOP_BLENDIF_A_in))
  {
    const unsigned int invert_mask = (blendif >> 16) & (1 << DEVELOP_BLENDIF_A_in);
    _blendif_lab_a(pixels, mask, stride, parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_A_in,
                   invert_mask);
  }

  if(blendif & (1 << DEVELOP_BLENDIF_B_in))
  {
    const unsigned int invert_mask = (blendif >> 16) & (1 << DEVELOP_BLENDIF_B_in);
    _blendif_lab_b(pixels, mask, stride, parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_B_in,
                   invert_mask);
  }

  if(blendif & ((1 << DEVELOP_BLENDIF_C_in) | (1 << DEVELOP_BLENDIF_h_in)))
  {
    const unsigned int invert_mask[2] DT_ALIGNED_PIXEL = {
        (blendif >> 16) & (1 << DEVELOP_BLENDIF_C_in),
        (blendif >> 16) & (1 << DEVELOP_BLENDIF_h_in),
    };
    _blendif_lch(pixels, mask, stride, parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_C_in,
                 invert_mask);
  }
}

void dt_develop_blendif_lab_make_mask(struct dt_dev_pixelpipe_iop_t *piece, const float *const restrict a,
                                      const float *const restrict b, const struct dt_iop_roi_t *const roi_in,
                                      const struct dt_iop_roi_t *const roi_out, float *const restrict mask)
{
  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *const)piece->blendop_data;

  if(piece->colors != DT_BLENDIF_LAB_CH) return;

  const int xoffs = roi_out->x - roi_in->x;
  const int yoffs = roi_out->y - roi_in->y;
  const int iwidth = roi_in->width;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;

  const unsigned int any_channel_active = d->blendif & DEVELOP_BLENDIF_Lab_MASK;
  const unsigned int mask_inclusive = d->mask_combine & DEVELOP_COMBINE_INCL;
  const unsigned int mask_inversed = d->mask_combine & DEVELOP_COMBINE_INV;

  // invert the individual channels if the combine mode is inclusive
  const unsigned int blendif = d->blendif ^ (mask_inclusive ? DEVELOP_BLENDIF_Lab_MASK << 16 : 0);

  // a channel cancels the mask if the whole span is selected and the channel is inverted
  const unsigned int canceling_channel = (blendif >> 16) & ~blendif & DEVELOP_BLENDIF_Lab_MASK;

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
      dt_iop_image_mul_const(mask,global_opacity,owidth,oheight,1); //mask[k] *= global_opacity;
    }
  }
  else if(canceling_channel || !any_channel_active)
  {
    // one of the conditional channel selects nothing
    // this means that the conditional opacity of all pixels is the same
    // and depends on whether the mask combination is inclusive and whether the mask is inverted
    if((mask_inversed == 0) ^ (mask_inclusive == 0))
    {
      dt_iop_image_fill(mask,global_opacity,owidth,oheight,1); //mask[k] = global_opacity;
    }
    else
    {
      dt_iop_image_fill(mask,0.0f,owidth,oheight,1); //mask[k] = 0.0f;
    }
  }
  else
  {
    // we need to process all conditional channels

    // parameters, for every channel the 4 limits + pre-computed increasing slope and decreasing slope
    float parameters[DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_SIZE] DT_ALIGNED_ARRAY;
    dt_develop_blendif_process_parameters(parameters, d);

    // allocate space for a temporary mask buffer to split the computation of every channel
    float *const restrict temp_mask = dt_alloc_align_float(buffsize);
    if(!temp_mask)
    {
      return;
    }

#ifdef _OPENMP
#pragma omp parallel default(none) \
  dt_omp_firstprivate(temp_mask, mask, a, b, oheight, owidth, iwidth, yoffs, xoffs, buffsize, \
                      blendif, parameters, mask_inclusive, mask_inversed, global_opacity)
#endif
    {
      // flush denormals to zero to avoid performance penalty if there are a lot of zero values in the mask
      const int oldMode = dt_mm_enable_flush_zero();

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
        const size_t start = ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_LAB_CH;
        _blendif_combine_channels(a + start, temp_mask + (y * owidth), owidth, blendif, parameters);
      }
#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
      for(size_t y = 0; y < oheight; y++)
      {
        const size_t start = (y * owidth) * DT_BLENDIF_LAB_CH;
        _blendif_combine_channels(b + start, temp_mask + (y * owidth), owidth, blendif >> DEVELOP_BLENDIF_L_out,
                                  parameters + DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_L_out);
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

      dt_mm_restore_flush_zero(oldMode);
    }

    dt_free_align(temp_mask);
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(i, o: 16)
#endif
static inline void _blend_Lab_scale(const float *i, float *o)
{
  const dt_aligned_pixel_t scale = { 1/100.0f, 1/128.0f, 1/128.0f, 1.0f };
  for_each_channel(c)
    o[c] = i[c] * scale[c];
}

#ifdef _OPENMP
#pragma omp declare simd aligned(i, o: 16)
#endif
static inline void _blend_Lab_rescale(const float *i, float *o)
{
  const dt_aligned_pixel_t scale = { 100.0f, 128.0f, 128.0f, 1.0f };
  for_each_channel(c)
    o[c] = i[c] * scale[c];
}


/* normal blend with clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_normal_bounded(const float *const restrict a, const float *const restrict b,
                                  float *const restrict out, const float *const restrict mask, const size_t stride,
                                  const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0; i < stride; i++)
  {
    size_t j = i * DT_BLENDIF_LAB_CH;
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    for_each_channel(x)
      tb[x] = _CLAMP(ta[x] * (1.0f - local_opacity) + tb[x] * local_opacity, min[x], max[x]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* normal blend without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_normal_unbounded(const float *const restrict a, const float *const restrict b,
                                    float *const restrict out,
                                    const float *const restrict mask, const size_t stride,
                                    const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0; i < stride; i++)
  {
    size_t j = i * DT_BLENDIF_LAB_CH;
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    for_each_channel(x)
      tb[x] = ta[x] * (1.0f - local_opacity) + tb[x] * local_opacity;

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* lighten */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_lighten(const float *const restrict a, const float *const restrict b,
                           float *const restrict out, const float *const restrict mask, const size_t stride,
                           const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    tb[0] = _CLAMP(ta[0] * (1.0f - local_opacity) + (ta[0] > tb[0] ? ta[0] : tb[0]) * local_opacity,
                   min[0], max[0]);
    tb[1] = _CLAMP(ta[1] * (1.0f - fabsf(tb[0] - ta[0])) + 0.5f * (ta[1] + tb[1]) * fabsf(tb[0] - ta[0]),
                   min[1], max[1]);
    tb[2] = _CLAMP(ta[2] * (1.0f - fabsf(tb[0] - ta[0])) + 0.5f * (ta[2] + tb[2]) * fabsf(tb[0] - ta[0]),
                   min[2], max[2]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* darken */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_darken(const float *const restrict a, const float *const restrict b,
                          float *const restrict out, const float *const restrict mask, const size_t stride,
                          const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    tb[0] = _CLAMP(ta[0] * (1.0f - local_opacity) + (ta[0] < tb[0] ? ta[0] : tb[0]) * local_opacity,
                   min[0], max[0]);
    tb[1] = _CLAMP(ta[1] * (1.0f - fabsf(tb[0] - ta[0])) + 0.5f * (ta[1] + tb[1]) * fabsf(tb[0] - ta[0]),
                   min[1], max[1]);
    tb[2] = _CLAMP(ta[2] * (1.0f - fabsf(tb[0] - ta[0])) + 0.5f * (ta[2] + tb[2]) * fabsf(tb[0] - ta[0]),
                   min[2], max[2]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* multiply */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_multiply(const float *const restrict a, const float *const restrict b,
                            float *const restrict out, const float *const restrict mask, const size_t stride,
                            const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    tb[0] = _CLAMP(ta[0] * (1.0f - local_opacity) + (ta[0] * tb[0]) * local_opacity, min[0], max[0]);

    const float f = fmaxf(ta[0], 0.01f);
    tb[1] = _CLAMP(ta[1] * (1.0f - local_opacity) + (ta[1] + tb[1]) * tb[0] / f * local_opacity, min[1], max[1]);
    tb[2] = _CLAMP(ta[2] * (1.0f - local_opacity) + (ta[2] + tb[2]) * tb[0] / f * local_opacity, min[2], max[2]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* average */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_average(const float *const restrict a, const float *const restrict b,
                           float *const restrict out, const float *const restrict mask, const size_t stride,
                           const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0; i < stride; i++)
  {
    size_t j = i * DT_BLENDIF_LAB_CH;
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    for_each_channel(x)
      tb[x] = _CLAMP(ta[x] * (1.0f - local_opacity) + (ta[x] + tb[x]) / 2.0f * local_opacity, min[x], max[x]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* add */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_add(const float *const restrict a, const float *const restrict b,
                       float *const restrict out, const float *const restrict mask, const size_t stride,
                       const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0; i < stride; i++)
  {
    size_t j = i * DT_BLENDIF_LAB_CH;
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    for_each_channel(x)
      tb[x] = _CLAMP(ta[x] * (1.0f - local_opacity) + (ta[x] + tb[x]) * local_opacity, min[x], max[x]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* subtract */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_subtract(const float *const restrict a, const float *const restrict b,
                            float *const restrict out, const float *const restrict mask, const size_t stride,
                            const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0; i < stride; i++)
  {
    size_t j = i * DT_BLENDIF_LAB_CH;
    float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    for_each_channel(x)
      tb[x] = _CLAMP(ta[x] * (1.0f - local_opacity) + ((tb[x] + ta[x]) - (fabsf(min[x] + max[x]))) * local_opacity,
                     min[x], max[x]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* difference (deprecated) */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_difference(const float *const restrict a, const float *const restrict b,
                              float *const restrict out, const float *const restrict mask, const size_t stride,
                              const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    const float lmin = 0.0f;
    for(size_t x = 0; x < 3; x++)
    {
      float lmax = max[x] + fabsf(min[x]);
      float la = _CLAMP(ta[x] + fabsf(min[x]), lmin, lmax);
      float lb = _CLAMP(tb[x] + fabsf(min[x]), lmin, lmax);
      tb[x] = _CLAMP(la * (1.0f - local_opacity) + fabsf(la - lb) * local_opacity, lmin, lmax) - fabsf(min[x]);
    }

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* difference 2 (new) */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_difference2(const float *const restrict a, const float *const restrict b,
                               float *const restrict out, const float *const restrict mask, const size_t stride,
                               const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    for_each_channel(x)
      tb[x] = fabsf(ta[x] - tb[x]) / fabsf(max[x] - min[x]);
    tb[0] = fmaxf(tb[0], fmaxf(tb[1], tb[2]));

    tb[0] = _CLAMP(ta[0] * (1.0f - local_opacity) + tb[0] * local_opacity, min[0], max[0]);
    tb[1] = 0.0f;
    tb[2] = 0.0f;

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* screen */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_screen(const float *const restrict a, const float *const restrict b,
                          float *const restrict out, const float *const restrict mask, const size_t stride,
                          const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = _CLAMP(ta[0] + fabsf(min[0]), lmin, lmax);
    const float lb = _CLAMP(tb[0] + fabsf(min[0]), lmin, lmax);

    tb[0] = _CLAMP(la * (1.0f - local_opacity) + ((lmax - (lmax - la) * (lmax - lb))) * local_opacity, lmin, lmax)
            - fabsf(min[0]);

    const float f = fmaxf(ta[0], 0.01f);
    tb[1] = _CLAMP(ta[1] * (1.0f - local_opacity) + 0.5f * (ta[1] + tb[1]) * tb[0] / f * local_opacity,
                   min[1], max[1]);
    tb[2] = _CLAMP(ta[2] * (1.0f - local_opacity) + 0.5f * (ta[2] + tb[2]) * tb[0] / f * local_opacity,
                   min[2], max[2]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* overlay */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_overlay(const float *const restrict a, const float *const restrict b,
                           float *const restrict out, const float *const restrict mask, const size_t stride,
                           const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = _CLAMP(ta[0] + fabsf(min[0]), lmin, lmax);
    const float lb = _CLAMP(tb[0] + fabsf(min[0]), lmin, lmax);
    const float halfmax = lmax / 2.0f;
    const float doublemax = lmax * 2.0f;

    tb[0] = _CLAMP(la * (1.0f - local_opacity2)
                   + (la > halfmax ? lmax - (lmax - doublemax * (la - halfmax)) * (lmax - lb)
                                   : (doublemax * la) * lb)
                     * local_opacity2, lmin, lmax)
            - fabsf(min[0]);

    const float f = fmaxf(ta[0], 0.01f);
    tb[1] = _CLAMP(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / f * local_opacity2, min[1], max[1]);
    tb[2] = _CLAMP(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / f * local_opacity2, min[2], max[2]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* softlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_softlight(const float *const restrict a, const float *const restrict b,
                             float *const restrict out, const float *const restrict mask, const size_t stride,
                             const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = _CLAMP(ta[0] + fabsf(min[0]), lmin, lmax);
    const float lb = _CLAMP(tb[0] + fabsf(min[0]), lmin, lmax);
    const float halfmax = lmax / 2.0f;

    tb[0] = _CLAMP(la * (1.0f - local_opacity2)
                   + (lb > halfmax ? lmax - (lmax - la) * (lmax - (lb - halfmax))
                                   : la * (lb + halfmax))
                     * local_opacity2, lmin, lmax)
            - fabsf(min[0]);

    const float f = fmaxf(ta[0], 0.01f);
    tb[1] = _CLAMP(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / f * local_opacity2, min[1], max[1]);
    tb[2] = _CLAMP(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / f * local_opacity2, min[2], max[2]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* hardlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_hardlight(const float *const restrict a, const float *const restrict b,
                             float *const restrict out, const float *const restrict mask, const size_t stride,
                             const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = _CLAMP(ta[0] + fabsf(min[0]), lmin, lmax);
    const float lb = _CLAMP(tb[0] + fabsf(min[0]), lmin, lmax);
    const float halfmax = lmax / 2.0f;
    const float doublemax = lmax * 2.0f;

    tb[0] = _CLAMP(la * (1.0f - local_opacity2)
                   + (lb > halfmax ? lmax - (lmax - doublemax * (la - halfmax)) * (lmax - lb)
                                   : doublemax * la * lb)
                     * local_opacity2, lmin, lmax)
            - fabsf(min[0]);

    const float f = fmaxf(ta[0], 0.01f);
    tb[1] = _CLAMP(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / f * local_opacity2, min[1], max[1]);
    tb[2] = _CLAMP(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / f * local_opacity2, min[2], max[2]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* vividlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_vividlight(const float *const restrict a, const float *const restrict b,
                              float *const restrict out, const float *const restrict mask, const size_t stride,
                              const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = _CLAMP(ta[0] + fabsf(min[0]), lmin, lmax);
    const float lb = _CLAMP(tb[0] + fabsf(min[0]), lmin, lmax);
    const float halfmax = lmax / 2.0f;
    const float doublemax = lmax * 2.0f;

    tb[0] = _CLAMP(la * (1.0f - local_opacity2)
                   + (lb > halfmax ? (lb >= lmax ? lmax : la / (doublemax * (lmax - lb)))
                                   : (lb <= lmin ? lmin : lmax - (lmax - la) / (doublemax * lb)))
                     * local_opacity2, lmin, lmax)
            - fabsf(min[0]);

    const float f = fmaxf(ta[0], 0.01f);
    tb[1] = _CLAMP(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / f * local_opacity2, min[1], max[1]);
    tb[2] = _CLAMP(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / f * local_opacity2, min[2], max[2]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* linearlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_linearlight(const float *const restrict a, const float *const restrict b,
                               float *const restrict out, const float *const restrict mask, const size_t stride,
                               const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = _CLAMP(ta[0] + fabsf(min[0]), lmin, lmax);
    const float lb = _CLAMP(tb[0] + fabsf(min[0]), lmin, lmax);
    const float doublemax = lmax * 2.0f;

    tb[0] = _CLAMP(la * (1.0f - local_opacity2) + (la + doublemax * lb - lmax) * local_opacity2, lmin, lmax)
            - fabsf(min[0]);

    const float f = fmaxf(ta[0], 0.01f);
    tb[1] = _CLAMP(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / f * local_opacity2, min[1], max[1]);
    tb[2] = _CLAMP(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / f * local_opacity2, min[2], max[2]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* pinlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_pinlight(const float *const restrict a, const float *const restrict b,
                            float *const restrict out, const float *const restrict mask, const size_t stride,
                            const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = _CLAMP(ta[0] + fabsf(min[0]), lmin, lmax);
    const float lb = _CLAMP(tb[0] + fabsf(min[0]), lmin, lmax);
    const float halfmax = lmax / 2.0f;
    const float doublemax = lmax * 2.0f;

    tb[0] = _CLAMP(la * (1.0f - local_opacity2)
                   + (lb > halfmax ? fmaxf(la, doublemax * (lb - halfmax))
                                   : fminf(la, doublemax * lb))
                     * local_opacity2, lmin, lmax)
          - fabsf(min[0]);

    tb[1] = _CLAMP(ta[1], min[1], max[1]);
    tb[2] = _CLAMP(ta[2], min[2], max[2]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* lightness blend */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_lightness(const float *const restrict a, const float *const restrict b,
                             float *const restrict out, const float *const restrict mask, const size_t stride,
                             const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    // no need to transfer to LCH as L is the same as in Lab, and C and H
    // remain unchanged
    tb[0] = _CLAMP(ta[0] * (1.0f - local_opacity) + tb[0] * local_opacity, min[0], max[0]);
    tb[1] = _CLAMP(ta[1], min[1], max[1]);
    tb[2] = _CLAMP(ta[2], min[2], max[2]);

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* chroma blend */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_chromaticity(const float *const restrict a, const float *const restrict b,
                                float *const restrict out, const float *const restrict mask, const size_t stride,
                                const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;
    dt_aligned_pixel_t tta, ttb;

    _blend_Lab_scale(a + j, ta);
    _CLAMP_XYZ(ta, min, max);
    dt_Lab_2_LCH(ta, tta);

    _blend_Lab_scale(b + j, tb);
    _CLAMP_XYZ(tb, min, max);
    dt_Lab_2_LCH(tb, ttb);

    ttb[0] = tta[0];
    ttb[1] = (tta[1] * (1.0f - local_opacity)) + ttb[1] * local_opacity;
    ttb[2] = tta[2];

    dt_LCH_2_Lab(ttb, tb);
    _CLAMP_XYZ(tb, min, max);
    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* hue blend */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_hue(const float *const restrict a, const float *const restrict b,
                       float *const restrict out, const float *const restrict mask, const size_t stride,
                       const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;
    dt_aligned_pixel_t tta, ttb;

    _blend_Lab_scale(a + j, ta);
    _CLAMP_XYZ(ta, min, max);
    dt_Lab_2_LCH(ta, tta);

    _blend_Lab_scale(b + j, tb);
    _CLAMP_XYZ(tb, min, max);
    dt_Lab_2_LCH(tb, ttb);

    ttb[0] = tta[0];
    ttb[1] = tta[1];
    /* blend hue along shortest distance on color circle */
    const float d = fabsf(tta[2] - ttb[2]);
    const float s = d > 0.5f ? -local_opacity * (1.0f - d) / d : local_opacity;
    ttb[2] = fmodf((tta[2] * (1.0f - s)) + ttb[2] * s + 1.0f, 1.0f);

    dt_LCH_2_Lab(ttb, tb);
    _CLAMP_XYZ(tb, min, max);
    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* color blend; blend hue and chroma, but not lightness */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_color(const float *const restrict a, const float *const restrict b,
                         float *const restrict out, const float *const restrict mask, const size_t stride,
                         const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;
    dt_aligned_pixel_t tta, ttb;

    _blend_Lab_scale(a + j, ta);
    _CLAMP_XYZ(ta, min, max);
    dt_Lab_2_LCH(ta, tta);

    _blend_Lab_scale(b + j, tb);
    _CLAMP_XYZ(tb, min, max);
    dt_Lab_2_LCH(tb, ttb);

    ttb[0] = tta[0];
    ttb[1] = (tta[1] * (1.0f - local_opacity)) + ttb[1] * local_opacity;

    /* blend hue along shortest distance on color circle */
    const float d = fabsf(tta[2] - ttb[2]);
    const float s = d > 0.5f ? -local_opacity * (1.0f - d) / d : local_opacity;
    ttb[2] = fmodf((tta[2] * (1.0f - s)) + ttb[2] * s + 1.0f, 1.0f);

    dt_LCH_2_Lab(ttb, tb);
    _CLAMP_XYZ(tb, min, max);
    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* color adjustment; blend hue and chroma; take lightness from module output */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_coloradjust(const float *const restrict a, const float *const restrict b,
                               float *const restrict out, const float *const restrict mask, const size_t stride,
                               const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;
    dt_aligned_pixel_t tta, ttb;

    _blend_Lab_scale(a + j, ta);
    _CLAMP_XYZ(ta, min, max);
    dt_Lab_2_LCH(ta, tta);

    _blend_Lab_scale(b + j, tb);
    _CLAMP_XYZ(tb, min, max);
    dt_Lab_2_LCH(tb, ttb);

    // ttb[0] (output lightness) unchanged
    ttb[1] = (tta[1] * (1.0f - local_opacity)) + ttb[1] * local_opacity;

    /* blend hue along shortest distance on color circle */
    const float d = fabsf(tta[2] - ttb[2]);
    const float s = d > 0.5f ? -local_opacity * (1.0f - d) / d : local_opacity;
    ttb[2] = fmodf((tta[2] * (1.0f - s)) + ttb[2] * s + 1.0f, 1.0f);

    dt_LCH_2_Lab(ttb, tb);
    _CLAMP_XYZ(tb, min, max);
    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* blend only lightness in Lab color space without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_Lab_lightness(const float *const restrict a, const float *const restrict b,
                                 float *const restrict out, const float *const restrict mask, const size_t stride,
                                 const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    tb[0] = ta[0] * (1.0f - local_opacity) + tb[0] * local_opacity;
    tb[1] = ta[1];
    tb[2] = ta[2];

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* blend only a-channel in Lab color space without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_Lab_a(const float *const restrict a, const float *const restrict b,
                         float *const restrict out, const float *const restrict mask, const size_t stride,
                         const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    tb[0] = ta[0];
    tb[1] = ta[1] * (1.0f - local_opacity) + tb[1] * local_opacity;
    tb[2] = ta[2];

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* blend only b-channel in Lab color space without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_Lab_b(const float *const restrict a, const float *const restrict b,
                         float *const restrict out, const float *const restrict mask, const size_t stride,
                         const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    tb[0] = ta[0];
    tb[1] = ta[1];
    tb[2] = ta[2] * (1.0f - local_opacity) + tb[2] * local_opacity;

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}


/* blend only color in Lab color space without any clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out, min, max: 16) uniform(stride, min, max)
#endif
static void _blend_Lab_color(const float *const restrict a, const float *const restrict b,
                             float *const restrict out, const float *const restrict mask, const size_t stride,
                             const dt_aligned_pixel_t min, const dt_aligned_pixel_t max)
{
  for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    float local_opacity = mask[i];
    dt_aligned_pixel_t ta, tb;

    _blend_Lab_scale(a + j, ta);
    _blend_Lab_scale(b + j, tb);

    tb[0] = ta[0];
    tb[1] = ta[1] * (1.0f - local_opacity) + tb[1] * local_opacity;
    tb[2] = ta[2] * (1.0f - local_opacity) + tb[2] * local_opacity;

    _blend_Lab_rescale(tb, out + j);
    out[j + DT_BLENDIF_LAB_BCH] = local_opacity;
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
      blend = _blend_difference;
      break;
    case DEVELOP_BLEND_DIFFERENCE2:
      blend = _blend_difference2;
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
    case DEVELOP_BLEND_LAB_LIGHTNESS:
    case DEVELOP_BLEND_LAB_L:
      blend = _blend_Lab_lightness;
      break;
    case DEVELOP_BLEND_LAB_A:
      blend = _blend_Lab_a;
      break;
    case DEVELOP_BLEND_LAB_B:
      blend = _blend_Lab_b;
      break;
    case DEVELOP_BLEND_LAB_COLOR:
      blend = _blend_Lab_color;
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
#pragma omp declare simd aligned(out:16)
#endif
static inline void _display_channel_value(dt_aligned_pixel_t out, const float value, const float mask)
{
  out[0] = value;
  out[1] = value;
  out[2] = value;
  out[3] = mask;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(a, b:16) uniform(channel, stride)
#endif
static void _display_channel(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride, const int channel,
                             const float *const restrict boost_factors)
{
  switch(channel)
  {
    case DT_DEV_PIXELPIPE_DISPLAY_L:
    {
      const float factor = 1.0f / (100.0f * exp2f(boost_factors[DEVELOP_BLENDIF_L_in]));
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        const float c = clamp_simd(a[j + 0] * factor);
        _display_channel_value(b + j, c, mask[i]);
      }
      break;
    }
    case (DT_DEV_PIXELPIPE_DISPLAY_L | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
    {
      const float factor = 1.0f / (100.0f * exp2f(boost_factors[DEVELOP_BLENDIF_L_out]));
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        const float c = clamp_simd(b[j + 0] * factor);
        _display_channel_value(b + j, c, mask[i]);
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_a:
    {
      const float factor = 1.0f / (256.0f * exp2f(boost_factors[DEVELOP_BLENDIF_A_in]));
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        const float c = clamp_simd(a[j + 1] * factor + 0.5f);
        _display_channel_value(b + j, c, mask[i]);
      }
      break;
    }
    case (DT_DEV_PIXELPIPE_DISPLAY_a | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
    {
      const float factor = 1.0f / (256.0f * exp2f(boost_factors[DEVELOP_BLENDIF_A_out]));
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        const float c = clamp_simd(b[j + 1] * factor + 0.5f);
        _display_channel_value(b + j, c, mask[i]);
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_b:
    {
      const float factor = 1.0f / (256.0f * exp2f(boost_factors[DEVELOP_BLENDIF_B_in]));
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        const float c = clamp_simd(a[j + 2] * factor + 0.5f);
        _display_channel_value(b + j, c, mask[i]);
      }
      break;
    }
    case (DT_DEV_PIXELPIPE_DISPLAY_b | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
    {
      const float factor = 1.0f / (256.0f * exp2f(boost_factors[DEVELOP_BLENDIF_B_out]));
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        const float c = clamp_simd(b[j + 2] * factor + 0.5f);
        _display_channel_value(b + j, c, mask[i]);
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_C:
    {
      const float factor = 1.0f / (128.0f * sqrtf(2.0f) * exp2f(boost_factors[DEVELOP_BLENDIF_C_in]));
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        dt_aligned_pixel_t LCH;
        dt_Lab_2_LCH(a + j, LCH);
        const float c = clamp_simd(LCH[1] * factor);
        _display_channel_value(b + j, c, mask[i]);
      }
      break;
    }
    case (DT_DEV_PIXELPIPE_DISPLAY_LCH_C | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
    {
      const float factor = 1.0f / (128.0f * sqrtf(2.0f) * exp2f(boost_factors[DEVELOP_BLENDIF_C_out]));
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        dt_aligned_pixel_t LCH;
        dt_Lab_2_LCH(b + j, LCH);
        const float c = clamp_simd(LCH[1] * factor);
        _display_channel_value(b + j, c, mask[i]);
      }
      break;
    }
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_h:
      // no boost factor for hues
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        dt_aligned_pixel_t LCH;
        dt_Lab_2_LCH(a + j, LCH);
        const float c = clamp_simd(LCH[2]);
        _display_channel_value(b + j, c, mask[i]);
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_LCH_h | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      // no boost factor for hues
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        dt_aligned_pixel_t LCH;
        dt_Lab_2_LCH(b + j, LCH);
        const float c = clamp_simd(LCH[2]);
        _display_channel_value(b + j, c, mask[i]);
      }
      break;
    default:
      for(size_t i = 0, j = 0; i < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        _display_channel_value(b + j, 0.0f, mask[i]);
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
  for(size_t x = DT_BLENDIF_LAB_BCH; x < stride; x += DT_BLENDIF_LAB_CH) b[x] = a[x];
}

void dt_develop_blendif_lab_blend(struct dt_dev_pixelpipe_iop_t *piece,
                                  const float *const a, float *const b,
                                  const struct dt_iop_roi_t *const roi_in,
                                  const struct dt_iop_roi_t *const roi_out,
                                  const float *const restrict mask,
                                  const dt_dev_pixelpipe_display_mask_t request_mask_display)
{
  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *const)piece->blendop_data;

  if(piece->colors != DT_BLENDIF_LAB_CH) return;

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
    const float *const restrict boost_factors = d->blendif_boost_factors;
    const dt_dev_pixelpipe_display_mask_t channel = request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY;
    const dt_iop_order_iccprofile_info_t *const profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(a, b, mask, channel, oheight, owidth, iwidth, xoffs, yoffs, boost_factors)
#endif
    for(size_t y = 0; y < oheight; y++)
    {
      const size_t a_start = ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_LAB_CH;
      const size_t b_start = y * owidth * DT_BLENDIF_LAB_CH;
      const size_t m_start = y * owidth;
      _display_channel(a + a_start, b + b_start, mask + m_start, owidth, channel, boost_factors);
    }

    // the generated output of the channel masks is expressed in RGB but this blending needs to output pixels in
    // the Lab color space. A conversion needs thus to be performed. As the pipe is using the work profile to
    // convert between Lab and the gamma module (which works in RGB), we need to use use that profile for the
    // conversion.
    const size_t buffsize = (size_t)owidth * oheight * DT_BLENDIF_LAB_CH;
    if(profile)
    {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(b, buffsize, profile)
#endif
      for(size_t j = 0; j < buffsize; j += DT_BLENDIF_LAB_CH)
      {
        dt_aligned_pixel_t pixel;
        for_each_channel(c,aligned(b))
          pixel[c] = b[j+c];
        const float yellow_mask = b[j+3]; // preserve alpha for code which does in-place conversion
        dt_ioppr_rgb_matrix_to_lab(pixel, b + j, profile->matrix_in_transposed, profile->lut_in,
                                   profile->unbounded_coeffs_in, profile->lutsize, profile->nonlinearlut);
        b[j+3] = yellow_mask;
      }
    }
    else
    {
#ifdef _OPENMP
#pragma omp parallel for simd schedule(static) default(none) aligned(b:64) \
  dt_omp_firstprivate(b, buffsize, profile)
#endif
      for(size_t j = 0; j < buffsize; j += DT_BLENDIF_LAB_CH)
      {
        dt_aligned_pixel_t XYZ;
        const float yellow_mask = b[j+3]; // preserve alpha for code which does in-place conversion
        dt_Rec709_to_XYZ_D50(b + j, XYZ);
        dt_XYZ_to_Lab(XYZ, b + j);
        b[j+3] = yellow_mask;
      }
    }
  }
  else
  {
    _blend_row_func *const blend = _choose_blend_func(d->blend_mode);
    // minimum and maximum values after scaling !!!
    const dt_aligned_pixel_t min = { 0.0f, -1.0f, -1.0f, 0.0f };
    const dt_aligned_pixel_t max = { 1.0f, 1.0f, 1.0f, 1.0f };

    float *tmp_buffer = dt_alloc_align_float((size_t)owidth * oheight * DT_BLENDIF_LAB_CH);
    if(tmp_buffer != NULL)
    {
      dt_iop_image_copy(tmp_buffer, b, (size_t)owidth * oheight * DT_BLENDIF_LAB_CH);
      if((d->blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE)
      {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(a, b, tmp_buffer, mask, blend, oheight, owidth, iwidth, xoffs, yoffs, min, max)
#endif
        for(size_t y = 0; y < oheight; y++)
        {
          const size_t a_start = ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_LAB_CH;
          const size_t b_start = y * owidth * DT_BLENDIF_LAB_CH;
          const size_t m_start = y * owidth;
          blend(tmp_buffer + b_start, a + a_start, b + b_start, mask + m_start, owidth, min, max);
        }
      }
      else
      {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(a, b, tmp_buffer, mask, blend, oheight, owidth, iwidth, xoffs, yoffs, min, max)
#endif
        for(size_t y = 0; y < oheight; y++)
        {
          const size_t a_start = ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_LAB_CH;
          const size_t b_start = y * owidth * DT_BLENDIF_LAB_CH;
          const size_t m_start = y * owidth;
          blend(a + a_start, tmp_buffer + b_start, b + b_start, mask + m_start, owidth, min, max);
        }
      }
      dt_free_align(tmp_buffer);
    }
  }

  if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
  {
    const size_t stride = owidth * DT_BLENDIF_LAB_CH;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(a, b, oheight, stride, iwidth, xoffs, yoffs)
#endif
    for(size_t y = 0; y < oheight; y++)
    {
      const size_t a_start = ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_LAB_CH;
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

