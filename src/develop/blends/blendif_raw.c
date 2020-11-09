/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#include "common/math.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include <math.h>

typedef void(_blend_row_func)(const float *const restrict a, float *const restrict b,
                              const float *const restrict mask, const size_t stride);

/* generate blend mask */
static void _blend_make_mask(const unsigned int mask_combine, const float gopacity, const size_t stride,
                             float *const restrict mask)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float form = mask[j];
    const float conditional = (mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;
    float opacity = (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - (1.0f - form) * (1.0f - conditional)
                                                          : form * conditional;
    opacity = (mask_combine & DEVELOP_COMBINE_INV) ? 1.0f - opacity : opacity;
    mask[j] = opacity * gopacity;
  }
}

void dt_develop_blendif_raw_make_mask(struct dt_dev_pixelpipe_iop_t *piece, const float *const restrict a,
                                      const float *const restrict b, const struct dt_iop_roi_t *const roi_in,
                                      const struct dt_iop_roi_t *const roi_out, float *const restrict mask)
{
  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *const)piece->blendop_data;

  if(piece->colors != 1) return;

  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const unsigned int mask_combine = d->mask_combine;

  // get the clipped opacity value  0 - 1
  const float opacity = fminf(fmaxf(0.0f, (d->opacity / 100.0f)), 1.0f);

  const size_t stride = (size_t)owidth;

  // get parametric mask (if any) and apply global opacity
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(mask_combine, oheight, opacity, mask, owidth, stride)
#endif
  for(size_t y = 0; y < oheight; y++)
  {
    float *const restrict m = mask + y * owidth;
    _blend_make_mask(mask_combine, opacity, stride, m);
  }
}


/* normal blend with clamping */
static void _blend_normal_bounded(const float *const restrict a, float *const restrict b,
                                  const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    b[j] = clamp_range_f(a[j]*(1.0f-local_opacity)+b[j]*local_opacity, 0.0f, 1.0f);
  }
}

/* normal blend without any clamping */
static void _blend_normal_unbounded(const float *const restrict a, float *const restrict b,
                                    const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    b[j] = a[j] * (1.0f - local_opacity) + b[j] * local_opacity;
  }
}

/* lighten */
static void _blend_lighten(const float *const restrict a, float *const restrict b,
                           const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    b[j] = clamp_range_f(a[j]*(1.0f-local_opacity)+fmaxf(a[j], b[j])*local_opacity, 0.0f, 1.0f);
  }
}

/* darken */
static void _blend_darken(const float *const restrict a, float *const restrict b,
                          const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    b[j] = clamp_range_f(a[j]*(1.0f - local_opacity) + fminf(a[j], b[j])*local_opacity, 0.0f, 1.0f);
  }
}

/* multiply */
static void _blend_multiply(const float *const restrict a, float *const restrict b,
                            const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    b[j] = clamp_range_f(a[j]*(1.0f - local_opacity) + (a[j]*b[j])*local_opacity, 0.0f, 1.0f);
  }
}

/* average */
static void _blend_average(const float *const restrict a, float *const restrict b,
                           const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    b[j] = clamp_range_f(a[j]*(1.0f - local_opacity) + (a[j] + b[j])/2.0f*local_opacity, 0.0f, 1.0f);
  }
}

/* add */
static void _blend_add(const float *const restrict a, float *const restrict b,
                       const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    b[j] = clamp_range_f(a[j]*(1.0f - local_opacity) + (a[j] + b[j])*local_opacity, 0.0f, 1.0f);
  }
}

/* subtract */
static void _blend_subtract(const float *const restrict a, float *const restrict b,
                            const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    b[j] = clamp_range_f(a[j]*(1.0f - local_opacity) + ((b[j] + a[j]) - 1.0f)*local_opacity, 0.0f, 1.0f);
  }
}

/* difference */
static void _blend_difference(const float *const restrict a, float *const restrict b,
                              const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    b[j] = clamp_range_f(a[j]*(1.0f - local_opacity) + fabsf(a[j]-b[j])*local_opacity, 0.0f, 1.0f);
  }
}

/* screen */
static void _blend_screen(const float *const restrict a, float *const restrict b,
                          const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float la = clamp_range_f(a[j], 0.0f, 1.0f);
    const float lb = clamp_range_f(b[j], 0.0f, 1.0f);
    b[j] = clamp_range_f(la*(1.0f - local_opacity)+(1.0f - (1.0f - la)*(1.0f - lb))*local_opacity, 0.0f, 1.0f);
  }
}

/* overlay */
static void _blend_overlay(const float *const restrict a, float *const restrict b,
                           const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_range_f(a[j], 0.0f, 1.0f);
    const float lb = clamp_range_f(b[j], 0.0f, 1.0f);
    b[j] = clamp_range_f(
        la*(1.0f - local_opacity2)
        + (la > 0.5f ? 1.0f - (1.0f - 2.0f*(la - 0.5f))*(1.0f - lb) : 2.0f*la*lb)*local_opacity2,
        0.0f, 1.0f);
  }
}

/* softlight */
static void _blend_softlight(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_range_f(a[j], 0.0f, 1.0f);
    const float lb = clamp_range_f(b[j], 0.0f, 1.0f);
    b[j] = clamp_range_f(
        la*(1.0f - local_opacity2)
        + (lb > 0.5f ? 1.0f - (1.0f - la)*(1.0f - (lb - 0.5f)) : la*(lb + 0.5f))*local_opacity2,
        0.0f, 1.0f);
  }
}

/* hardlight */
static void _blend_hardlight(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_range_f(a[j], 0.0f, 1.0f);
    const float lb = clamp_range_f(b[j], 0.0f, 1.0f);
    b[j] = clamp_range_f(
        la*(1.0f - local_opacity2)
        +(lb > 0.5f ? 1.0f - (1.0f - 2.0f*(la - 0.5f))*(1.0f - lb) : 2.0f*la*lb)*local_opacity2,
        0.0f, 1.0f);
  }
}

/* vividlight */
static void _blend_vividlight(const float *const restrict a, float *const restrict b,
                              const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_range_f(a[j], 0.0f, 1.0f);
    const float lb = clamp_range_f(b[j], 0.0f, 1.0f);
    b[j] = clamp_range_f(
        la*(1.0f - local_opacity2)
        + (lb>0.5f ? (lb >= 1.0f ? 1.0f : la/(2.0f*(1.0f - lb)))
                   : (lb <= 0.0f ? 0.0f : 1.0f - (1.0f - la)/(2.0f*lb)))
          *local_opacity2,
        0.0f, 1.0f);
  }
}

/* linearlight */
static void _blend_linearlight(const float *const restrict a, float *const restrict b,
                               const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_range_f(a[j], 0.0f, 1.0f);
    const float lb = clamp_range_f(b[j], 0.0f, 1.0f);
    b[j] = clamp_range_f(la*(1.0f - local_opacity2) + (la + 2.0f*lb - 1.0f)*local_opacity2, 0.0f, 1.0f);
  }
}

/* pinlight */
static void _blend_pinlight(const float *const restrict a, float *const restrict b,
                            const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_range_f(a[j], 0.0f, 1.0f);
    const float lb = clamp_range_f(b[j], 0.0f, 1.0f);
    b[j] = clamp_range_f(
        la*(1.0f - local_opacity2)
        +(lb > 0.5f ? fmaxf(la, 2.0f*(lb - 0.5f)) : fminf(la, 2.0f*lb))*local_opacity2,
        0.0f, 1.0f);
  }
}

/* inverse blend */
static void _blend_inverse(const float *const restrict a, float *const restrict b,
                           const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    b[j] = clamp_range_f(a[j]*(1.0f - local_opacity) + b[j]*local_opacity, 0.0f, 1.0f);
  }
}


static _blend_row_func *_choose_blend_func(const unsigned int blend_mode)
{
  _blend_row_func *blend = NULL;

  /* select the blend operator */
  switch(blend_mode)
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
    case DEVELOP_BLEND_INVERSE:
      blend = _blend_inverse;
      break;
    case DEVELOP_BLEND_NORMAL:
    case DEVELOP_BLEND_BOUNDED:
      blend = _blend_normal_bounded;
      break;

    /* fallback to normal blend */
    case DEVELOP_BLEND_NORMAL2:
    case DEVELOP_BLEND_UNBOUNDED:
    default:
      blend = _blend_normal_unbounded;
      break;
  }

  return blend;
}


static void _display_channel(float *const restrict b, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    b[j] = 0.0f;
  }
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

  _blend_row_func *const blend = _choose_blend_func(d->blend_mode);

#ifdef _OPENMP
#pragma omp parallel default(none) \
  dt_omp_firstprivate(blend, a, b, mask, iwidth, owidth, oheight, xoffs, yoffs, request_mask_display)
#endif
  {
    if(request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
    {
#ifdef _OPENMP
#pragma omp for
#endif
      for(size_t y = 0; y < oheight; y++)
      {
        const size_t stride = (size_t)owidth;
        float *const restrict out = b + y * owidth;
        _display_channel(out, stride);
      }
    }
    else
    {
#ifdef _OPENMP
#pragma omp for
#endif
      for(size_t y = 0; y < oheight; y++)
      {
        const size_t stride = (size_t)owidth;
        const float *const restrict in = a + ((y + yoffs) * iwidth + xoffs);
        float *const restrict out = b + y * owidth;
        const float *const restrict m = mask + y * owidth;
        blend(in, out, m, stride);
      }
    }
  }
}

// tools/update_modelines.sh
// remove-trailing-space on;
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
