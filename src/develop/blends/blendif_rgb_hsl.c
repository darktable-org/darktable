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
#include "common/colorspaces_inline_conversions.h"
#include "common/math.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include <math.h>

#define DT_BLENDIF_RGB_CH 4
#define DT_BLENDIF_RGB_BCH 3

typedef void(_blend_row_func)(const float *const restrict a, float *const restrict b,
                              const float *const restrict mask, const size_t stride);

static inline float _Hue_2_RGB(float v1, float v2, float vH)
{
  if(vH < 0.0f) vH += 1.0f;
  if(vH > 1.0f) vH -= 1.0f;
  if((6.0f * vH) < 1.0f) return (v1 + (v2 - v1) * 6.0f * vH);
  if((2.0f * vH) < 1.0f) return (v2);
  if((3.0f * vH) < 2.0f) return (v1 + (v2 - v1) * ((2.0f / 3.0f) - vH) * 6.0f);
  return (v1);
}

static inline void _HSL_2_RGB(const float *HSL, float *RGB)
{
  float H = HSL[0];
  float S = HSL[1];
  float L = HSL[2];

  float var_1, var_2;

  if(S < 1e-6f)
  {
    RGB[0] = RGB[1] = RGB[2] = L;
  }
  else
  {
    if(L < 0.5f)
      var_2 = L * (1.0f + S);
    else
      var_2 = (L + S) - (S * L);

    var_1 = 2.0f * L - var_2;

    RGB[0] = _Hue_2_RGB(var_1, var_2, H + (1.0f / 3.0f));
    RGB[1] = _Hue_2_RGB(var_1, var_2, H);
    RGB[2] = _Hue_2_RGB(var_1, var_2, H - (1.0f / 3.0f));
  }
}

static inline void _RGB_2_HSV(const float *RGB, float *HSV)
{
  float r = RGB[0], g = RGB[1], b = RGB[2];
  float *h = HSV, *s = HSV + 1, *v = HSV + 2;

  float min = fminf(r, fminf(g, b));
  float max = fmaxf(r, fmaxf(g, b));
  float delta = max - min;

  *v = max;

  if(fabsf(max) > 1e-6f && fabsf(delta) > 1e-6f)
  {
    *s = delta / max;
  }
  else
  {
    *s = 0.0f;
    *h = 0.0f;
    return;
  }

  if(r == max)
    *h = (g - b) / delta;
  else if(g == max)
    *h = 2.0f + (b - r) / delta;
  else
    *h = 4.0f + (r - g) / delta;

  *h /= 6.0f;

  if(*h < 0) *h += 1.0f;
}

static inline void _HSV_2_RGB(const float *HSV, float *RGB)
{
  float h = 6.0f * HSV[0], s = HSV[1], v = HSV[2];
  float *r = RGB, *g = RGB + 1, *b = RGB + 2;

  if(fabsf(s) < 1e-6f)
  {
    *r = *g = *b = v;
    return;
  }

  float i = floorf(h);
  float f = h - i;
  float p = v * (1.0f - s);
  float q = v * (1.0f - s * f);
  float t = v * (1.0f - s * (1.0f - f));

  switch((int)i)
  {
    case 0:
      *r = v;
      *g = t;
      *b = p;
      break;
    case 1:
      *r = q;
      *g = v;
      *b = p;
      break;
    case 2:
      *r = p;
      *g = v;
      *b = t;
      break;
    case 3:
      *r = p;
      *g = q;
      *b = v;
      break;
    case 4:
      *r = t;
      *g = p;
      *b = v;
      break;
    case 5:
    default:
      *r = v;
      *g = p;
      *b = q;
      break;
  }
}

static inline void _CLAMP_XYZ(float *XYZ)
{
  XYZ[0] = clamp_range_f(XYZ[0], 0.0f, 1.0f);
  XYZ[1] = clamp_range_f(XYZ[1], 0.0f, 1.0f);
  XYZ[2] = clamp_range_f(XYZ[2], 0.0f, 1.0f);
}

static inline void _PX_COPY(const float *src, float *dst)
{
  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
}

static inline float _blendif_factor(const float *const restrict input, const float *const restrict output,
                                    const unsigned int blendif, const float *const restrict parameters,
                                    const unsigned int mask_mode, const unsigned int mask_combine,
                                    const dt_iop_order_iccprofile_info_t *const work_profile)
{
  float result = 1.0f;
  float scaled[DEVELOP_BLENDIF_SIZE] = { 0.5f };

  if(!(mask_mode & DEVELOP_MASK_CONDITIONAL)) return (mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;
  if(work_profile == NULL)
    scaled[DEVELOP_BLENDIF_GRAY_in] =clamp_range_f(0.3f*input[0]+0.59f*input[1]+0.11f*input[2], 0.0f,
                                                   1.0f); // Gray scaled to 0..1
  else
    scaled[DEVELOP_BLENDIF_GRAY_in] =clamp_range_f(dt_ioppr_get_rgb_matrix_luminance(input,
                                                                                     work_profile->matrix_in,
                                                                                     work_profile->lut_in,
                                                                                     work_profile->unbounded_coeffs_in,
                                                                                     work_profile->lutsize,
                                                                                     work_profile->nonlinearlut), 0.0f,
                                                   1.0f);                // Gray scaled to 0..1
  scaled[DEVELOP_BLENDIF_RED_in] =clamp_range_f(input[0], 0.0f, 1.0f);   // Red
  scaled[DEVELOP_BLENDIF_GREEN_in] =clamp_range_f(input[1], 0.0f, 1.0f); // Green
  scaled[DEVELOP_BLENDIF_BLUE_in] =clamp_range_f(input[2], 0.0f, 1.0f);  // Blue
  if(work_profile == NULL)
    scaled[DEVELOP_BLENDIF_GRAY_out] =clamp_range_f(0.3f*output[0]+0.59f*output[1]+0.11f*output[2],
                                                    0.0f, 1.0f); // Gray scaled to 0..1
  else
    scaled[DEVELOP_BLENDIF_GRAY_out] =clamp_range_f(dt_ioppr_get_rgb_matrix_luminance(output,
                                                                                      work_profile->matrix_in,
                                                                                      work_profile->lut_in,
                                                                                      work_profile->unbounded_coeffs_in,
                                                                                      work_profile->lutsize,
                                                                                      work_profile->nonlinearlut),
                                                    0.0f, 1.0f);           // Gray scaled to 0..1
  scaled[DEVELOP_BLENDIF_RED_out] =clamp_range_f(output[0], 0.0f, 1.0f);   // Red
  scaled[DEVELOP_BLENDIF_GREEN_out] =clamp_range_f(output[1], 0.0f, 1.0f); // Green
  scaled[DEVELOP_BLENDIF_BLUE_out] =clamp_range_f(output[2], 0.0f, 1.0f);  // Blue

  if(blendif & 0x7f00) // do we need to consider HSL ?
  {
    float HSL_input[3];
    float HSL_output[3];
    dt_RGB_2_HSL(input, HSL_input);
    dt_RGB_2_HSL(output, HSL_output);

    scaled[DEVELOP_BLENDIF_H_in] =clamp_range_f(HSL_input[0], 0.0f, 1.0f); // H scaled to 0..1
    scaled[DEVELOP_BLENDIF_S_in] =clamp_range_f(HSL_input[1], 0.0f, 1.0f); // S scaled to 0..1
    scaled[DEVELOP_BLENDIF_l_in] =clamp_range_f(HSL_input[2], 0.0f, 1.0f); // L scaled to 0..1

    scaled[DEVELOP_BLENDIF_H_out] =clamp_range_f(HSL_output[0], 0.0f, 1.0f); // H scaled to 0..1
    scaled[DEVELOP_BLENDIF_S_out] =clamp_range_f(HSL_output[1], 0.0f, 1.0f); // S scaled to 0..1
    scaled[DEVELOP_BLENDIF_l_out] =clamp_range_f(HSL_output[2], 0.0f, 1.0f); // L scaled to 0..1
  }

  for(int ch = 0; ch <= DEVELOP_BLENDIF_MAX; ch++)
  {
    if((DEVELOP_BLENDIF_RGB_MASK & (1 << ch)) == 0) continue; // skip blendif channels not used in this color space

    if((blendif & (1 << ch)) == 0) // deal with channels where sliders span the whole range
    {
      result *= !(blendif & (1 << (ch + 16))) == !(mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f : 0.0f;
      continue;
    }

    if(result <= 0.000001f) break; // no need to continue if we are already at or close to zero

    float factor;
    if(scaled[ch] >= parameters[4 * ch + 1] && scaled[ch] <= parameters[4 * ch + 2])
    {
      factor = 1.0f;
    }
    else if(scaled[ch] > parameters[4 * ch + 0] && scaled[ch] < parameters[4 * ch + 1])
    {
      factor
          = (scaled[ch] - parameters[4 * ch + 0]) / fmaxf(0.01f, parameters[4 * ch + 1] - parameters[4 * ch + 0]);
    }
    else if(scaled[ch] > parameters[4 * ch + 2] && scaled[ch] < parameters[4 * ch + 3])
    {
      factor = 1.0f
               - (scaled[ch] - parameters[4 * ch + 2])
                     / fmaxf(0.01f, parameters[4 * ch + 3] - parameters[4 * ch + 2]);
    }
    else
      factor = 0.0f;

    if((blendif & (1 << (ch + 16))) != 0) factor = 1.0f - factor; // inverted channel?

    result *= ((mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - factor : factor);
  }

  return (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - result : result;
}

/* generate blend mask */
static void _blend_make_mask(const unsigned int blendif, const float *blendif_parameters,
                             const unsigned int mask_mode, const unsigned int mask_combine, const float gopacity,
                             const float *const restrict a, const float *const restrict b, const size_t stride,
                             float *const restrict mask, const dt_iop_order_iccprofile_info_t *const work_profile)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float form = mask[i];
    const float conditional = _blendif_factor(&a[j], &b[j], blendif, blendif_parameters, mask_mode,
                                              mask_combine, work_profile);
    float opacity = (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - (1.0f - form) * (1.0f - conditional)
                                                          : form * conditional;
    opacity = (mask_combine & DEVELOP_COMBINE_INV) ? 1.0f - opacity : opacity;
    mask[i] = opacity * gopacity;
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
  const unsigned int blendif = d->blendif;
  const unsigned int mask_mode = d->mask_mode;
  const unsigned int mask_combine = d-> mask_combine;
  const float *const parameters = d->blendif_parameters;

  // get the clipped opacity value  0 - 1
  const float opacity = fminf(fmaxf(0.0f, (d->opacity / 100.0f)), 1.0f);

  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  const size_t stride = (size_t)owidth * DT_BLENDIF_RGB_CH;

  // get parametric mask (if any) and apply global opacity
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(opacity, a, b, mask, iwidth, owidth, oheight, stride, xoffs, yoffs, work_profile, blendif, \
                        parameters, mask_mode, mask_combine)
#endif
  for(size_t y = 0; y < oheight; y++)
  {
    const float *const restrict in = a + ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_RGB_CH;
    const float *const restrict out = b + y * owidth * DT_BLENDIF_RGB_CH;
    float *const restrict m = mask + y * owidth;
    _blend_make_mask(blendif, parameters, mask_mode, mask_combine, opacity, in, out, stride, m, work_profile);
  }
}


/* normal blend with clamping */
static void _blend_normal_bounded(const float *const restrict a, float *const restrict b,
                                  const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      b[j + k] = clamp_range_f(a[j + k] * (1.0f - local_opacity) + b[j + k] * local_opacity, 0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* normal blend without any clamping */
static void _blend_normal_unbounded(const float *const restrict a, float *const restrict b,
                                    const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      b[j + k] = a[j + k] * (1.0f - local_opacity) + b[j + k] * local_opacity;
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* lighten */
static void _blend_lighten(const float *const restrict a, float *const restrict b,
                           const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      b[j + k] = clamp_range_f(a[j+k]*(1.0f - local_opacity) + fmaxf(a[j+k], b[j+k])*local_opacity, 0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* darken */
static void _blend_darken(const float *const restrict a, float *const restrict b,
                          const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      b[j + k] = clamp_range_f(a[j+k]*(1.0f - local_opacity) + fminf(a[j+k], b[j+k])*local_opacity, 0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* multiply */
static void _blend_multiply(const float *const restrict a, float *const restrict b,
                            const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      b[j + k] = clamp_range_f(a[j+k]*(1.0f - local_opacity) + (a[j+k]*b[j+k])*local_opacity, 0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* average */
static void _blend_average(const float *const restrict a, float *const restrict b,
                           const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      b[j + k] = clamp_range_f(a[j+k]*(1.0f - local_opacity) + (a[j+k] + b[j+k])/2.0f*local_opacity, 0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* add */
static void _blend_add(const float *const restrict a, float *const restrict b,
                       const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      b[j + k] = clamp_range_f(a[j+k]*(1.0f - local_opacity) + (a[j+k] + b[j+k])*local_opacity, 0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* substract */
static void _blend_substract(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      b[j + k] = clamp_range_f(a[j+k]*(1.0f - local_opacity) + ((b[j+k] + a[j+k]) - 1.0f)*local_opacity, 0.0f, 1.0f);
    }
    b[j + 3] = local_opacity;
  }
}

/* difference */
static void _blend_difference(const float *const restrict a, float *const restrict b,
                              const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      b[j + k] = clamp_range_f(a[j+k]*(1.0f - local_opacity) + fabsf(a[j+k] - b[j+k])*local_opacity, 0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* screen */
static void _blend_screen(const float *const restrict a, float *const restrict b,
                          const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_range_f(a[j+k], 0.0f, 1.0f);
      const float lb = clamp_range_f(b[j+k], 0.0f, 1.0f);
      b[j + k] = clamp_range_f(la*(1.0f - local_opacity) + (1.0f-(1.0f-la)*(1.0f-lb))*local_opacity, 0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* overlay */
static void _blend_overlay(const float *const restrict a, float *const restrict b,
                           const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_range_f(a[j+k], 0.0f, 1.0f);
      const float lb = clamp_range_f(b[j+k], 0.0f, 1.0f);
      b[j + k] = clamp_range_f(
          la*(1.0f - local_opacity2)
          + (la > 0.5f ? 1.0f - (1.0f - 2.0f*(la - 0.5f))*(1.0f - lb) : 2.0f*la*lb)*local_opacity2,
          0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* softlight */
static void _blend_softlight(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_range_f(a[j+k], 0.0f, 1.0f);
      const float lb = clamp_range_f(b[j+k], 0.0f, 1.0f);
      b[j + k] = clamp_range_f(
          la*(1.0f - local_opacity2)
          + (lb > 0.5f ? 1.0f - (1.0f - la)*(1.0f - (lb - 0.5f)) : la*(lb + 0.5f))*local_opacity2,
          0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* hardlight */
static void _blend_hardlight(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_range_f(a[j+k], 0.0f, 1.0f);
      const float lb = clamp_range_f(b[j+k], 0.0f, 1.0f);
      b[j + k] = clamp_range_f(
          la*(1.0f-local_opacity2)
          + (lb > 0.5f ? 1.0f - (1.0f - 2.0f*(la - 0.5f))*(1.0f - lb) : 2.0f*la*lb)*local_opacity2,
          0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* vividlight */
static void _blend_vividlight(const float *const restrict a, float *const restrict b,
                              const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    float local_opacity = mask[i];
    float local_opacity2 = local_opacity * local_opacity;
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_range_f(a[j+k], 0.0f, 1.0f);
      const float lb = clamp_range_f(b[j+k], 0.0f, 1.0f);
      b[j + k] = clamp_range_f(
          la*(1.0f - local_opacity2)
          + (lb > 0.5f ? (lb >= 1.0f ? 1.0f : la/(2.0f*(1.0f - lb)))
                       : (lb <= 0.0f ? 0.0f : 1.0f - (1.0f - la)/(2.0f*lb)))
            *local_opacity2,
          0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* linearlight */
static void _blend_linearlight(const float *const restrict a, float *const restrict b,
                               const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_range_f(a[j+k], 0.0f, 1.0f);
      const float lb = clamp_range_f(b[j+k], 0.0f, 1.0f);
      b[j + k] = clamp_range_f(la*(1.0f - local_opacity2) + (la + 2.0f*lb - 1.0f)*local_opacity2, 0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* pinlight */
static void _blend_pinlight(const float *const restrict a, float *const restrict b,
                            const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      const float la = clamp_range_f(a[j+k], 0.0f, 1.0f);
      const float lb = clamp_range_f(b[j+k], 0.0f, 1.0f);
      b[j + k] = clamp_range_f(
          la*(1.0f - local_opacity2)
          + (lb > 0.5f ? fmaxf(la, 2.0f*(lb - 0.5f)) : fminf(la, 2.0f*lb))*local_opacity2,
          0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* lightness blend */
static void _blend_lightness(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tta[3], ttb[3];
    _PX_COPY(&a[j], ta);
    _CLAMP_XYZ(ta);
    _CLAMP_XYZ(&b[j]);

    dt_RGB_2_HSL(ta, tta);
    dt_RGB_2_HSL(&b[j], ttb);

    ttb[0] = tta[0];
    ttb[1] = tta[1];
    ttb[2] = (tta[2] * (1.0f - local_opacity)) + ttb[2] * local_opacity;

    _HSL_2_RGB(ttb, &b[j]);
    _CLAMP_XYZ(&b[j]);

    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* chroma blend */
static void _blend_chroma(const float *const restrict a, float *const restrict b,
                          const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tta[3], ttb[3];
    _PX_COPY(&a[j], ta);

    _CLAMP_XYZ(ta);
    _CLAMP_XYZ(&b[j]);

    dt_RGB_2_HSL(ta, tta);
    dt_RGB_2_HSL(&b[j], ttb);

    ttb[0] = tta[0];
    ttb[1] = (tta[1] * (1.0f - local_opacity)) + ttb[1] * local_opacity;
    ttb[2] = tta[2];

    _HSL_2_RGB(ttb, &b[j]);
    _CLAMP_XYZ(&b[j]);

    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* hue blend */
static void _blend_hue(const float *const restrict a, float *const restrict b,
                       const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tta[3], ttb[3];
    _PX_COPY(&a[j], ta);

    _CLAMP_XYZ(ta);
    _CLAMP_XYZ(&b[j]);

    dt_RGB_2_HSL(ta, tta);
    dt_RGB_2_HSL(&b[j], ttb);

    /* blend hue along shortest distance on color circle */
    float d = fabsf(tta[0] - ttb[0]);
    float s = d > 0.5f ? -local_opacity * (1.0f - d) / d : local_opacity;
    ttb[0] = fmodf((tta[0] * (1.0f - s)) + ttb[0] * s + 1.0f, 1.0f);
    ttb[1] = tta[1];
    ttb[2] = tta[2];

    _HSL_2_RGB(ttb, &b[j]);
    _CLAMP_XYZ(&b[j]);

    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* color blend; blend hue and chroma, but not lightness */
static void _blend_color(const float *const restrict a, float *const restrict b,
                         const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    float local_opacity = mask[i];
    float ta[3], tta[3], ttb[3];
    _PX_COPY(&a[j], ta);

    _CLAMP_XYZ(ta);
    _CLAMP_XYZ(&b[j]);

    dt_RGB_2_HSL(ta, tta);
    dt_RGB_2_HSL(&b[j], ttb);

    /* blend hue along shortest distance on color circle */
    float d = fabsf(tta[0] - ttb[0]);
    float s = d > 0.5f ? -local_opacity * (1.0f - d) / d : local_opacity;
    ttb[0] = fmodf((tta[0] * (1.0f - s)) + ttb[0] * s + 1.0f, 1.0f);

    ttb[1] = (tta[1] * (1.0f - local_opacity)) + ttb[1] * local_opacity;
    ttb[2] = tta[2];

    _HSL_2_RGB(ttb, &b[j]);
    _CLAMP_XYZ(&b[j]);

    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* color adjustment; blend hue and chroma; take lightness from module output */
static void _blend_coloradjust(const float *const restrict a, float *const restrict b,
                               const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tta[3], ttb[3];
    _PX_COPY(&a[j], ta);

    _CLAMP_XYZ(ta);
    _CLAMP_XYZ(&b[j]);

    dt_RGB_2_HSL(ta, tta);
    dt_RGB_2_HSL(&b[j], ttb);

    /* blend hue along shortest distance on color circle */
    const float d = fabsf(tta[0] - ttb[0]);
    const float s = d > 0.5f ? -local_opacity * (1.0f - d) / d : local_opacity;
    ttb[0] = fmodf((tta[0] * (1.0f - s)) + ttb[0] * s + 1.0f, 1.0f);

    ttb[1] = (tta[1] * (1.0f - local_opacity)) + ttb[1] * local_opacity;
    // ttb[2] (output lightness) unchanged

    _HSL_2_RGB(ttb, &b[j]);
    _CLAMP_XYZ(&b[j]);

    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* inverse blend */
static void _blend_inverse(const float *const restrict a, float *const restrict b,
                           const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++)
    {
      b[j + k] = clamp_range_f(a[j+k]*(1.0f - local_opacity) + b[j+k]*local_opacity, 0.0f, 1.0f);
    }
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* blend only lightness in HSV color space without any clamping (a noop for
 * other color spaces) */
static void _blend_HSV_lightness(const float *const restrict a, float *const restrict b,
                                 const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _RGB_2_HSV(&a[j], ta);
    _RGB_2_HSV(&b[j], tb);

    // hue and saturation from input image
    tb[0] = ta[0];
    tb[1] = ta[1];

    // blend lightness between input and output
    tb[2] = ta[2] * (1.0f - local_opacity) + tb[2] * local_opacity;

    _HSV_2_RGB(tb, &b[j]);
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* blend only color in HSV color space without any clamping (a noop for other
 * color spaces) */
static void _blend_HSV_color(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _RGB_2_HSV(&a[j], ta);
    _RGB_2_HSV(&b[j], tb);

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

    _HSV_2_RGB(tb, &b[j]);
    b[j + DT_BLENDIF_RGB_BCH] = local_opacity;
  }
}

/* blend only R-channel in RGB color space without any clamping (a noop for
 * other color spaces) */
static void _blend_RGB_R(const float *const restrict a, float *const restrict b,
                         const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    b[j + 0] = a[j + 0] * (1.0f - local_opacity) + b[j + 0] * local_opacity;
    b[j + 1] = a[j + 1];
    b[j + 2] = a[j + 2];
    b[j + 3] = local_opacity;
  }
}

/* blend only R-channel in RGB color space without any clamping (a noop for
 * other color spaces) */
static void _blend_RGB_G(const float *const restrict a, float *const restrict b,
                         const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    b[j + 0] = a[j + 0];
    b[j + 1] = a[j + 1] * (1.0f - local_opacity) + b[j + 1] * local_opacity;
    b[j + 2] = a[j + 2];
    b[j + 3] = local_opacity;
  }
}

/* blend only R-channel in RGB color space without any clamping (a noop for
 * other color spaces) */
static void _blend_RGB_B(const float *const restrict a, float *const restrict b,
                         const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
  {
    const float local_opacity = mask[i];
    b[j + 0] = a[j + 0];
    b[j + 1] = a[j + 1];
    b[j + 2] = a[j + 2] * (1.0f - local_opacity) + b[j + 2] * local_opacity;
    b[j + 3] = local_opacity;
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
    case DEVELOP_BLEND_SUBSTRACT:
      blend = _blend_substract;
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
    case DEVELOP_BLEND_CHROMA:
      blend = _blend_chroma;
      break;
    case DEVELOP_BLEND_HUE:
      blend = _blend_hue;
      break;
    case DEVELOP_BLEND_COLOR:
      blend = _blend_color;
      break;
    case DEVELOP_BLEND_INVERSE:
      blend = _blend_inverse;
      break;
    case DEVELOP_BLEND_NORMAL:
    case DEVELOP_BLEND_BOUNDED:
      blend = _blend_normal_bounded;
      break;
    case DEVELOP_BLEND_COLORADJUST:
      blend = _blend_coloradjust;
      break;
    case DEVELOP_BLEND_HSV_LIGHTNESS:
      blend = _blend_HSV_lightness;
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
    case DEVELOP_BLEND_UNBOUNDED:
    default:
      blend = _blend_normal_unbounded;
      break;
  }

  return blend;
}


static void _display_channel(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride,
                             const dt_dev_pixelpipe_display_mask_t channel,
                             const dt_iop_order_iccprofile_info_t *const work_profile)
{
  switch(channel & DT_DEV_PIXELPIPE_DISPLAY_ANY)
  {
    case DT_DEV_PIXELPIPE_DISPLAY_R:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_range_f(a[j], 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_R | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_range_f(b[j], 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_G:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_range_f(a[j+1], 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_G | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_range_f(b[j+1], 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_B:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_range_f(a[j+2], 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_B | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = clamp_range_f(b[j+2], 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_GRAY:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = (work_profile == NULL)
                            ? clamp_range_f(0.3f*a[j]+0.59f*a[j+1]+0.11f*a[j+2], 0.0f, 1.0f)
                            : clamp_range_f(dt_ioppr_get_rgb_matrix_luminance(a+j,
                                                                              work_profile->matrix_in,
                                                                              work_profile->lut_in,
                                                                              work_profile->unbounded_coeffs_in,
                                                                              work_profile->lutsize,
                                                                              work_profile->nonlinearlut), 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_GRAY | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        const float c = (work_profile == NULL)
                            ? clamp_range_f(0.3f*b[j]+0.59f*b[j+1]+0.11f*b[j+2], 0.0f, 1.0f)
                            : clamp_range_f(dt_ioppr_get_rgb_matrix_luminance(b+j,
                                                                              work_profile->matrix_in,
                                                                              work_profile->lut_in,
                                                                              work_profile->unbounded_coeffs_in,
                                                                              work_profile->lutsize,
                                                                              work_profile->nonlinearlut), 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_H:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        float HSL[3];
        dt_RGB_2_HSL(a + j, HSL);
        const float c = clamp_range_f(HSL[0], 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_HSL_H | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        float HSL[3];
        dt_RGB_2_HSL(b + j, HSL);
        const float c = clamp_range_f(HSL[0], 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_S:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        float HSL[3];
        dt_RGB_2_HSL(a + j, HSL);
        const float c = clamp_range_f(HSL[1], 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_HSL_S | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        float HSL[3];
        dt_RGB_2_HSL(b + j, HSL);
        const float c = clamp_range_f(HSL[1], 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_l:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        float HSL[3];
        dt_RGB_2_HSL(a + j, HSL);
        const float c = clamp_range_f(HSL[2], 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_HSL_l | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        float HSL[3];
        dt_RGB_2_HSL(b + j, HSL);
        const float c = clamp_range_f(HSL[2], 0.0f, 1.0f);
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = c;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
    default:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_RGB_CH)
      {
        for(int k = 0; k < DT_BLENDIF_RGB_BCH; k++) b[j + k] = 0.0f;
        b[j + DT_BLENDIF_RGB_BCH] = mask[i];
      }
      break;
  }
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

  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  // only non-zero if mask_display was set by an _earlier_ module
  const dt_dev_pixelpipe_display_mask_t mask_display = piece->pipe->mask_display;

  _blend_row_func *const blend = _choose_blend_func(d->blend_mode);

#ifdef _OPENMP
#pragma omp parallel default(none) \
  dt_omp_firstprivate(blend, a, b, mask, iwidth, owidth, oheight, work_profile, xoffs, yoffs, mask_display, \
                      request_mask_display)
#endif
  {
    const size_t stride = (size_t)owidth * DT_BLENDIF_RGB_CH;
    if(request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
    {
#ifdef _OPENMP
#pragma omp for
#endif
      for(size_t y = 0; y < oheight; y++)
      {
        const float *const restrict in = a + ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_RGB_CH;
        float *const restrict out = b + y * owidth * DT_BLENDIF_RGB_CH;
        const float *const restrict m = mask + y * owidth;
        _display_channel(in, out, m, stride, request_mask_display, work_profile);
      }
    }
    else
    {
#ifdef _OPENMP
#pragma omp for
#endif
      for(size_t y = 0; y < oheight; y++)
      {
        const float *const restrict in = a + ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_RGB_CH;
        float *const restrict out = b + y * owidth * DT_BLENDIF_RGB_CH;
        const float *const restrict m = mask + y * owidth;
        blend(in, out, m, stride);
      }
    }
    if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    {
#ifdef _OPENMP
#pragma omp for
#endif
      for(size_t y = 0; y < oheight; y++)
      {
        const float *const restrict in = a + ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_RGB_CH;
        float *const restrict out = b + y * owidth * DT_BLENDIF_RGB_CH;
        for(size_t j = 0; j < stride; j += DT_BLENDIF_RGB_CH) out[j + DT_BLENDIF_RGB_BCH] = in[j + DT_BLENDIF_RGB_BCH];
      }
    }
  }
}

// tools/update_modelines.sh
// remove-trailing-space on;
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
