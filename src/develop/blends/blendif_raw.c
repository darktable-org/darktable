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

#include "common/imagebuf.h"
#include "common/math.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/openmp_maths.h"
#include <math.h>


typedef void(_blend_row_func)(const float *const restrict a, const float *const restrict b,
                              float *const restrict out, const float *const restrict mask, const size_t stride);


void dt_develop_blendif_raw_make_mask(struct dt_dev_pixelpipe_iop_t *piece, const float *const restrict a,
                                      const float *const restrict b, const struct dt_iop_roi_t *const roi_in,
                                      const struct dt_iop_roi_t *const roi_out, float *const restrict mask)
{
  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *const)piece->blendop_data;

  if(piece->colors != 1) return;

  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const size_t buffsize = (size_t)owidth * oheight;

  // get the clipped opacity value  0 - 1
  const float global_opacity = fminf(fmaxf(0.0f, (d->opacity / 100.0f)), 1.0f);

  // get parametric mask (if any) and apply global opacity
  if(d->mask_combine & DEVELOP_COMBINE_INV)
  {
#ifdef _OPENMP
#pragma omp parallel for simd schedule(static) default(none) aligned(mask: 64) \
    dt_omp_firstprivate(mask, buffsize, global_opacity)
#endif
    for(size_t x = 0; x < buffsize; x++) mask[x] = global_opacity * (1.0f - mask[x]);
  }
  else
  {
    dt_iop_image_mul_const(mask,global_opacity,owidth,oheight,1); // mask[k] *= global_opacity;
  }
}


/* normal blend with clamping */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_normal_bounded(const float *const restrict a, const float *const restrict b,
                                  float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + b[j] * local_opacity);
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
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = a[j] * (1.0f - local_opacity) + b[j] * local_opacity;
  }
}

/* lighten */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_lighten(const float *const restrict a, const float *const restrict b,
                           float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + fmaxf(a[j], b[j]) * local_opacity);
  }
}

/* darken */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_darken(const float *const restrict a, const float *const restrict b,
                          float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + fminf(a[j], b[j]) * local_opacity);
  }
}

/* multiply */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_multiply(const float *const restrict a, const float *const restrict b,
                            float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + (a[j] * b[j]) * local_opacity);
  }
}

/* average */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_average(const float *const restrict a, const float *const restrict b,
                           float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + (a[j] + b[j]) / 2.0f * local_opacity);
  }
}

/* add */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_add(const float *const restrict a, const float *const restrict b,
                       float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + (a[j] + b[j]) * local_opacity);
  }
}

/* subtract */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_subtract(const float *const restrict a, const float *const restrict b,
                            float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + ((b[j] + a[j]) - 1.0f) * local_opacity);
  }
}

/* difference */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_difference(const float *const restrict a, const float *const restrict b,
                              float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + fabsf(a[j] - b[j]) * local_opacity);
  }
}

/* screen */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_screen(const float *const restrict a, const float *const restrict b,
                          float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(la * (1.0f - local_opacity) + (1.0f - (1.0f - la) * (1.0f - lb)) * local_opacity);
  }
}

/* overlay */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_overlay(const float *const restrict a, const float *const restrict b,
                           float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(
        la * (1.0f - local_opacity2)
        + (la > 0.5f ? 1.0f - (1.0f - 2.0f * (la - 0.5f)) * (1.0f - lb) : 2.0f * la * lb) * local_opacity2);
  }
}

/* softlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_softlight(const float *const restrict a, const float *const restrict b,
                             float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(
        la * (1.0f - local_opacity2)
        + (lb > 0.5f ? 1.0f - (1.0f - la) * (1.0f - (lb - 0.5f)) : la * (lb + 0.5f)) * local_opacity2);
  }
}

/* hardlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_hardlight(const float *const restrict a, const float *const restrict b,
                             float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(
        la * (1.0f - local_opacity2)
        + (lb > 0.5f ? 1.0f - (1.0f - 2.0f * (la - 0.5f)) * (1.0f - lb) : 2.0f * la * lb) * local_opacity2);
  }
}

/* vividlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_vividlight(const float *const restrict a, const float *const restrict b,
                              float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(
        la * (1.0f - local_opacity2)
        + (lb > 0.5f ? (lb >= 1.0f ? 1.0f : la / (2.0f * (1.0f - lb)))
                     : (lb <= 0.0f ? 0.0f : 1.0f - (1.0f - la) / (2.0f * lb)))
          * local_opacity2);
  }
}

/* linearlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_linearlight(const float *const restrict a, const float *const restrict b,
                               float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(la * (1.0f - local_opacity2) + (la + 2.0f * lb - 1.0f) * local_opacity2);
  }
}

/* pinlight */
#ifdef _OPENMP
#pragma omp declare simd aligned(a, b, out:16) uniform(stride)
#endif
static void _blend_pinlight(const float *const restrict a, const float *const restrict b,
                            float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(
        la * (1.0f - local_opacity2)
        + (lb > 0.5f ? fmaxf(la, 2.0f * (lb - 0.5f)) : fminf(la, 2.0f * lb)) * local_opacity2);
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
    case DEVELOP_BLEND_BOUNDED:
      blend = _blend_normal_bounded;
      break;

    /* fallback to normal blend */
    case DEVELOP_BLEND_NORMAL2:
    default:
      blend = _blend_normal_unbounded;
      break;
  }

  return blend;
}


void dt_develop_blendif_raw_blend(struct dt_dev_pixelpipe_iop_t *piece,
                                  const float *const restrict a, float *const restrict b,
                                  const struct dt_iop_roi_t *const roi_in,
                                  const struct dt_iop_roi_t *const roi_out,
                                  const float *const restrict mask,
                                  const dt_dev_pixelpipe_display_mask_t request_mask_display)
{
  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *const)piece->blendop_data;

  if(piece->colors != 1) return;

  const int xoffs = roi_out->x - roi_in->x;
  const int yoffs = roi_out->y - roi_in->y;
  const int iwidth = roi_in->width;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;

  if(request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
  {
    dt_iop_image_fill(b, 0.0f, owidth, oheight, 1); //b[k] = 0.0f;
  }
  else
  {
    _blend_row_func *const blend = _choose_blend_func(d->blend_mode);

    float *tmp_buffer = dt_alloc_align_float((size_t)owidth * oheight);
    if(tmp_buffer != NULL)
    {
      dt_iop_image_copy(tmp_buffer, b, (size_t)owidth * oheight);
      if((d->blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE)
      {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(blend, a, b, tmp_buffer, mask, oheight, owidth, iwidth, xoffs, yoffs)
#endif
        for(size_t y = 0; y < oheight; y++)
        {
          const size_t a_start = (y + yoffs) * iwidth + xoffs;
          const size_t bm_start = y * owidth;
          blend(tmp_buffer + bm_start, a + a_start, b + bm_start, mask + bm_start, owidth);
        }
      }
      else
      {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
  dt_omp_firstprivate(blend, a, b, tmp_buffer, mask, oheight, owidth, iwidth, xoffs, yoffs)
#endif
        for(size_t y = 0; y < oheight; y++)
        {
          const size_t a_start = (y + yoffs) * iwidth + xoffs;
          const size_t bm_start = y * owidth;
          blend(a + a_start, tmp_buffer + bm_start, b + bm_start, mask + bm_start, owidth);
        }
      }
      dt_free_align(tmp_buffer);
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

