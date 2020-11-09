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

#define DT_BLENDIF_LAB_CH 4
#define DT_BLENDIF_LAB_BCH 3

typedef void(_blend_row_func)(const float *const restrict a, float *const restrict b,
                              const float *const restrict mask, const size_t stride);

static inline void _CLAMP_XYZ(float *XYZ, const float *min, const float *max)
{
  XYZ[0] = clamp_range_f(XYZ[0], min[0], max[0]);
  XYZ[1] = clamp_range_f(XYZ[1], min[1], max[1]);
  XYZ[2] = clamp_range_f(XYZ[2], min[2], max[2]);
}


static inline float _blendif_factor(const float *const restrict input, const float *const restrict output,
                                    const unsigned int blendif, const float *const restrict parameters,
                                    const unsigned int mask_mode, const unsigned int mask_combine)
{
  float result = 1.0f;
  float scaled[DEVELOP_BLENDIF_SIZE] = { 0.5f };

  if(!(mask_mode & DEVELOP_MASK_CONDITIONAL)) return (mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;
  scaled[DEVELOP_BLENDIF_L_in] =clamp_range_f(input[0]/100.0f, 0.0f, 1.0f); // L scaled to 0..1
  scaled[DEVELOP_BLENDIF_A_in]
      =clamp_range_f((input[1]+128.0f)/256.0f, 0.0f, 1.0f); // a scaled to 0..1
  scaled[DEVELOP_BLENDIF_B_in]
      =clamp_range_f((input[2]+128.0f)/256.0f, 0.0f, 1.0f);                 // b scaled to 0..1
  scaled[DEVELOP_BLENDIF_L_out] =clamp_range_f(output[0]/100.0f, 0.0f, 1.0f); // L scaled to 0..1
  scaled[DEVELOP_BLENDIF_A_out]
      =clamp_range_f((output[1]+128.0f)/256.0f, 0.0f, 1.0f); // a scaled to 0..1
  scaled[DEVELOP_BLENDIF_B_out]
      =clamp_range_f((output[2]+128.0f)/256.0f, 0.0f, 1.0f); // b scaled to 0..1

  if(blendif & 0x7f00) // do we need to consider LCh ?
  {
    float LCH_input[3];
    float LCH_output[3];
    dt_Lab_2_LCH(input, LCH_input);
    dt_Lab_2_LCH(output, LCH_output);

    scaled[DEVELOP_BLENDIF_C_in] =clamp_range_f(LCH_input[1]/(128.0f*sqrtf(2.0f)), 0.0f,
                                                1.0f);                     // C scaled to 0..1
    scaled[DEVELOP_BLENDIF_h_in] =clamp_range_f(LCH_input[2], 0.0f, 1.0f); // h scaled to 0..1

    scaled[DEVELOP_BLENDIF_C_out] =clamp_range_f(LCH_output[1]/(128.0f*sqrtf(2.0f)), 0.0f,
                                                 1.0f);                      // C scaled to 0..1
    scaled[DEVELOP_BLENDIF_h_out] =clamp_range_f(LCH_output[2], 0.0f, 1.0f); // h scaled to 0..1
  }

  for(int ch = 0; ch <= DEVELOP_BLENDIF_MAX; ch++)
  {
    if((DEVELOP_BLENDIF_Lab_MASK & (1 << ch)) == 0) continue; // skip blendif channels not used in this color space

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
                             float *const restrict mask)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float form = mask[i];
    const float conditional = _blendif_factor(&a[j], &b[j], blendif, blendif_parameters, mask_mode, mask_combine);
    float opacity = (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - (1.0f - form) * (1.0f - conditional)
                                                          : form * conditional;
    opacity = (mask_combine & DEVELOP_COMBINE_INV) ? 1.0f - opacity : opacity;
    mask[i] = opacity * gopacity;
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
  const unsigned int blendif = d->blendif;
  const unsigned int mask_mode = d->mask_mode;
  const unsigned int mask_combine = d-> mask_combine;
  const float *const parameters = d->blendif_parameters;

  // get the clipped opacity value  0 - 1
  const float opacity = fminf(fmaxf(0.0f, (d->opacity / 100.0f)), 1.0f);

  const size_t stride = (size_t)owidth * DT_BLENDIF_LAB_CH;

  // get parametric mask (if any) and apply global opacity
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(opacity, a, b, mask, iwidth, owidth, oheight, stride, xoffs, yoffs, blendif, parameters, \
                      mask_mode, mask_combine)
#endif
  for(size_t y = 0; y < oheight; y++)
  {
    const float *const restrict in = a + ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_LAB_CH;
    const float *const restrict out = b + y * owidth * DT_BLENDIF_LAB_CH;
    float *const restrict m = mask + y * owidth;
    _blend_make_mask(blendif, parameters, mask_mode, mask_combine, opacity, in, out, stride, m);
  }
}


static inline void _blend_colorspace_channel_range(float *min, float *max)
{
  // after scaling !!!
  min[0] = 0.0f;
  max[0] = 1.0f;
  min[1] = -1.0f;
  max[1] = 1.0f;
  min[2] = -1.0f;
  max[2] = 1.0f;
  min[3] = 0.0f;
  max[3] = 1.0f;
}

static inline void _blend_Lab_scale(const float *i, float *o)
{
  o[0] = i[0] / 100.0f;
  o[1] = i[1] / 128.0f;
  o[2] = i[2] / 128.0f;
}

static inline void _blend_Lab_rescale(const float *i, float *o)
{
  o[0] = i[0] * 100.0f;
  o[1] = i[1] * 128.0f;
  o[2] = i[2] * 128.0f;
}


/* normal blend with clamping */
static void _blend_normal_bounded(const float *const restrict a, float *const restrict b,
                                  const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    tb[0] = clamp_range_f(ta[0] * (1.0f - local_opacity) + tb[0] * local_opacity, min[0], max[0]);
    tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity) + tb[1] * local_opacity, min[1], max[1]);
    tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity) + tb[2] * local_opacity, min[2], max[2]);

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* normal blend without any clamping */
static void _blend_normal_unbounded(const float *const restrict a, float *const restrict b,
                                    const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    tb[0] = ta[0] * (1.0f - local_opacity) + tb[0] * local_opacity;
    tb[1] = ta[1] * (1.0f - local_opacity) + tb[1] * local_opacity;
    tb[2] = ta[2] * (1.0f - local_opacity) + tb[2] * local_opacity;

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* lighten */
static void _blend_lighten(const float *const restrict a, float *const restrict b,
                           const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3], tbo;
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    tbo = tb[0];
    tb[0] = clamp_range_f(ta[0] * (1.0f - local_opacity) + (ta[0] > tb[0] ? ta[0] : tb[0]) * local_opacity,
                          min[0], max[0]);
    tb[1] = clamp_range_f(ta[1] * (1.0f - fabsf(tbo - tb[0])) + 0.5f * (ta[1] + tb[1]) * fabsf(tbo - tb[0]),
                          min[1], max[1]);
    tb[2] = clamp_range_f(ta[2] * (1.0f - fabsf(tbo - tb[0])) + 0.5f * (ta[2] + tb[2]) * fabsf(tbo - tb[0]),
                          min[2], max[2]);

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* darken */
static void _blend_darken(const float *const restrict a, float *const restrict b,
                          const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3], tbo;
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    tbo = tb[0];
    tb[0] = clamp_range_f(ta[0] * (1.0f - local_opacity) + (ta[0] < tb[0] ? ta[0] : tb[0]) * local_opacity,
                          min[0], max[0]);
    tb[1] = clamp_range_f(ta[1] * (1.0f - fabsf(tbo - tb[0])) + 0.5f * (ta[1] + tb[1]) * fabsf(tbo - tb[0]),
                          min[1], max[1]);
    tb[2] = clamp_range_f(ta[2] * (1.0f - fabsf(tbo - tb[0])) + 0.5f * (ta[2] + tb[2]) * fabsf(tbo - tb[0]),
                          min[2], max[2]);

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* multiply */
static void _blend_multiply(const float *const restrict a, float *const restrict b,
                            const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];

    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);
    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = clamp_range_f(ta[0] + fabsf(min[0]), lmin, lmax);
    const float lb = clamp_range_f(tb[0] + fabsf(min[0]), lmin, lmax);

    tb[0] = clamp_range_f((la * (1.0f - local_opacity)) + ((la * lb) * local_opacity), min[0], max[0])
            - fabsf(min[0]);

    if(ta[0] > 0.01f)
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity) + (ta[1] + tb[1]) * tb[0] / ta[0] * local_opacity,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity) + (ta[2] + tb[2]) * tb[0] / ta[0] * local_opacity,
                            min[2], max[2]);
    }
    else
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity) + (ta[1] + tb[1]) * tb[0] / 0.01f * local_opacity,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity) + (ta[2] + tb[2]) * tb[0] / 0.01f * local_opacity,
                            min[2], max[2]);
    }

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* average */
static void _blend_average(const float *const restrict a, float *const restrict b,
                           const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    tb[0] = clamp_range_f(ta[0] * (1.0f - local_opacity) + (ta[0] + tb[0]) / 2.0f * local_opacity, min[0], max[0]);
    tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity) + (ta[1] + tb[1]) / 2.0f * local_opacity, min[1], max[1]);
    tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity) + (ta[2] + tb[2]) / 2.0f * local_opacity, min[2], max[2]);

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* add */
static void _blend_add(const float *const restrict a, float *const restrict b,
                       const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    tb[0] = clamp_range_f(ta[0] * (1.0f - local_opacity) + (ta[0] + tb[0]) * local_opacity, min[0], max[0]);
    tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity) + (ta[1] + tb[1]) * local_opacity, min[1], max[1]);
    tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity) + (ta[2] + tb[2]) * local_opacity, min[2], max[2]);

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* subtract */
static void _blend_subtract(const float *const restrict a, float *const restrict b,
                            const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    tb[0] = clamp_range_f(ta[0] * (1.0f - local_opacity)
                              + ((tb[0] + ta[0]) - (fabsf(min[0] + max[0]))) * local_opacity,
                          min[0], max[0]);
    tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity)
                              + ((tb[1] + ta[1]) - (fabsf(min[1] + max[1]))) * local_opacity,
                          min[1], max[1]);
    tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity)
                              + ((tb[2] + ta[2]) - (fabsf(min[2] + max[2]))) * local_opacity,
                          min[2], max[2]);

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* difference (deprecated) */
static void _blend_difference(const float *const restrict a, float *const restrict b,
                              const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);
    float lmin = 0.0f;
    float lmax = max[0] + fabsf(min[0]);
    float la = clamp_range_f(ta[0]+fabsf(min[0]), lmin, lmax);
    float lb = clamp_range_f(tb[0]+fabsf(min[0]), lmin, lmax);

    tb[0] = clamp_range_f(la*(1.0f-local_opacity)+fabsf(la-lb)*local_opacity, lmin, lmax)
            - fabsf(min[0]);

    lmax = max[1] + fabsf(min[1]);
    la = clamp_range_f(ta[1] + fabsf(min[1]), lmin, lmax);
    lb = clamp_range_f(tb[1] + fabsf(min[1]), lmin, lmax);
    tb[1] = clamp_range_f(la * (1.0f - local_opacity) + fabsf(la - lb) * local_opacity, lmin, lmax)
            - fabsf(min[1]);
    lmax = max[2] + fabsf(min[2]);
    la = clamp_range_f(ta[2] + fabsf(min[2]), lmin, lmax);
    lb = clamp_range_f(tb[2] + fabsf(min[2]), lmin, lmax);
    tb[2] = clamp_range_f(la * (1.0f - local_opacity) + fabsf(la - lb) * local_opacity, lmin, lmax)
            - fabsf(min[2]);

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* difference 2 (new) */
static void _blend_difference2(const float *const restrict a, float *const restrict b,
                               const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    tb[0] = fabsf(ta[0] - tb[0]) / fabsf(max[0] - min[0]);
    tb[1] = fabsf(ta[1] - tb[1]) / fabsf(max[1] - min[1]);
    tb[2] = fabsf(ta[2] - tb[2]) / fabsf(max[2] - min[2]);
    tb[0] = fmaxf(tb[0], fmaxf(tb[1], tb[2]));

    tb[0] = clamp_range_f(ta[0] * (1.0f - local_opacity) + tb[0] * local_opacity, min[0], max[0]);
    tb[1] = 0.0f;
    tb[2] = 0.0f;

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* screen */
static void _blend_screen(const float *const restrict a, float *const restrict b,
                          const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);
    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = clamp_range_f(ta[0]+fabsf(min[0]), lmin, lmax);
    const float lb = clamp_range_f(tb[0]+fabsf(min[0]), lmin, lmax);

    tb[0] = clamp_range_f(la*(1.0f-local_opacity)+((lmax-(lmax-la)*(lmax-lb)))*local_opacity,
                         lmin, lmax)
            - fabsf(min[0]);

    if(ta[0] > 0.01f)
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity)
                                + 0.5f * (ta[1] + tb[1]) * tb[0] / ta[0] * local_opacity,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity)
                                + 0.5f * (ta[2] + tb[2]) * tb[0] / ta[0] * local_opacity,
                            min[2], max[2]);
    }
    else
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity)
                                + 0.5f * (ta[1] + tb[1]) * tb[0] / 0.01f * local_opacity,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity)
                                + 0.5f * (ta[2] + tb[2]) * tb[0] / 0.01f * local_opacity,
                            min[2], max[2]);
    }

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* overlay */
static void _blend_overlay(const float *const restrict a, float *const restrict b,
                           const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);
    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = clamp_range_f(ta[0]+fabsf(min[0]), lmin, lmax);
    const float lb = clamp_range_f(tb[0]+fabsf(min[0]), lmin, lmax);
    const float halfmax = lmax / 2.0f;
    const float doublemax = lmax * 2.0f;

    tb[0] =clamp_range_f(la*(1.0f-local_opacity2)
                         +(la>halfmax ? lmax-(lmax-doublemax*(la-halfmax))*(lmax-lb)
                                      : (doublemax*la)*lb)
                          *local_opacity2,
                         lmin, lmax)
            - fabsf(min[0]);

    if(ta[0] > 0.01f)
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / ta[0] * local_opacity2,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / ta[0] * local_opacity2,
                            min[2], max[2]);
    }
    else
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / 0.01f * local_opacity2,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / 0.01f * local_opacity2,
                            min[2], max[2]);
    }

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* softlight */
static void _blend_softlight(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);
    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = clamp_range_f(ta[0]+fabsf(min[0]), lmin, lmax);
    const float lb = clamp_range_f(tb[0]+fabsf(min[0]), lmin, lmax);
    const float halfmax = lmax / 2.0f;

    tb[0] = clamp_range_f(
        la*(1.0f-local_opacity2)
        +(lb>halfmax ? lmax-(lmax-la)*(lmax-(lb-halfmax)) : la*(lb+halfmax))
         *local_opacity2,
        lmin, lmax)
            - fabsf(min[0]);

    if(ta[0] > 0.01f)
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / ta[0] * local_opacity2,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / ta[0] * local_opacity2,
                            min[2], max[2]);
    }
    else
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / 0.01f * local_opacity2,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / 0.01f * local_opacity2,
                            min[2], max[2]);
    }

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* hardlight */
static void _blend_hardlight(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);
    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = clamp_range_f(ta[0]+fabsf(min[0]), lmin, lmax);
    const float lb = clamp_range_f(tb[0]+fabsf(min[0]), lmin, lmax);
    const float halfmax = lmax / 2.0f;
    const float doublemax = lmax * 2.0f;

    tb[0] =clamp_range_f((la*(1.0f-local_opacity2))
                         +(lb>halfmax ? lmax-(lmax-doublemax*(la-halfmax))*(lmax-lb)
                                      : doublemax*la*lb)
                          *local_opacity2,
                         lmin, lmax)
            - fabsf(min[0]);

    if(ta[0] > 0.01f)
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / ta[0] * local_opacity2,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / ta[0] * local_opacity2,
                            min[2], max[2]);
    }
    else
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / 0.01f * local_opacity2,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / 0.01f * local_opacity2,
                            min[2], max[2]);
    }

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* vividlight */
static void _blend_vividlight(const float *const restrict a, float *const restrict b,
                              const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);
    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = clamp_range_f(ta[0]+fabsf(min[0]), lmin, lmax);
    const float lb = clamp_range_f(tb[0]+fabsf(min[0]), lmin, lmax);
    const float halfmax = lmax / 2.0f;
    const float doublemax = lmax * 2.0f;

    tb[0] = clamp_range_f(
        la*(1.0f - local_opacity2)
        + (lb > halfmax ? (lb >= lmax ? lmax : la/(doublemax*(lmax - lb)))
                        : (lb <= lmin ? lmin : lmax - (lmax - la)/(doublemax*lb)))
          *local_opacity2,
        lmin, lmax) - fabsf(min[0]);

    if(ta[0] > 0.01f)
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / ta[0] * local_opacity2,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / ta[0] * local_opacity2,
                            min[2], max[2]);
    }
    else
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / 0.01f * local_opacity2,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / 0.01f * local_opacity2,
                            min[2], max[2]);
    }

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* linearlight */
static void _blend_linearlight(const float *const restrict a, float *const restrict b,
                               const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);
    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = clamp_range_f(ta[0]+fabsf(min[0]), lmin, lmax);
    const float lb = clamp_range_f(tb[0]+fabsf(min[0]), lmin, lmax);
    const float doublemax = lmax * 2.0f;

    tb[0] = clamp_range_f(la*(1.0f-local_opacity2)+(la+doublemax*lb-lmax)*local_opacity2, lmin,
                          lmax)
            - fabsf(min[0]);

    if(ta[0] > 0.01f)
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / ta[0] * local_opacity2,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / ta[0] * local_opacity2,
                            min[2], max[2]);
    }
    else
    {
      tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity2) + (ta[1] + tb[1]) * tb[0] / 0.01f * local_opacity2,
                            min[1], max[1]);
      tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity2) + (ta[2] + tb[2]) * tb[0] / 0.01f * local_opacity2,
                            min[2], max[2]);
    }

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* pinlight */
static void _blend_pinlight(const float *const restrict a, float *const restrict b,
                            const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    const float local_opacity2 = local_opacity * local_opacity;
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);
    const float lmin = 0.0f;
    const float lmax = max[0] + fabsf(min[0]);
    const float la = clamp_range_f(ta[0]+fabsf(min[0]), lmin, lmax);
    const float lb = clamp_range_f(tb[0]+fabsf(min[0]), lmin, lmax);
    const float halfmax = lmax / 2.0f;
    const float doublemax = lmax * 2.0f;

    tb[0]
        =clamp_range_f(la*(1.0f-local_opacity2)
                       +(lb>halfmax ? fmaxf(la, doublemax*(lb-halfmax)) : fminf(la, doublemax*lb))
                        *local_opacity2,
                       lmin, lmax)
          - fabsf(min[0]);

    tb[1] =clamp_range_f(ta[1], min[1], max[1]);
    tb[2] =clamp_range_f(ta[2], min[2], max[2]);

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* lightness blend */
static void _blend_lightness(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    // no need to transfer to LCH as L is the same as in Lab, and C and H
    // remain unchanged
    tb[0] = clamp_range_f(ta[0]*(1.0f-local_opacity)+tb[0]*local_opacity, min[0], max[0]);
    tb[1] = clamp_range_f(ta[1], min[1], max[1]);
    tb[2] = clamp_range_f(ta[2], min[2], max[2]);

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* chroma blend */
static void _blend_chroma(const float *const restrict a, float *const restrict b,
                          const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    float tta[3], ttb[3];
    _blend_Lab_scale(&a[j], ta);
    _CLAMP_XYZ(ta, min, max);
    dt_Lab_2_LCH(ta, tta);

    _blend_Lab_scale(&b[j], tb);
    _CLAMP_XYZ(tb, min, max);
    dt_Lab_2_LCH(tb, ttb);

    ttb[0] = tta[0];
    ttb[1] = (tta[1] * (1.0f - local_opacity)) + ttb[1] * local_opacity;
    ttb[2] = tta[2];

    dt_LCH_2_Lab(ttb, tb);
    _CLAMP_XYZ(tb, min, max);
    _blend_Lab_rescale(tb, &b[j]);

    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* hue blend */
static void _blend_hue(const float *const restrict a, float *const restrict b,
                       const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    float tta[3], ttb[3];
    _blend_Lab_scale(&a[j], ta);
    _CLAMP_XYZ(ta, min, max);
    dt_Lab_2_LCH(ta, tta);

    _blend_Lab_scale(&b[j], tb);
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
    _blend_Lab_rescale(tb, &b[j]);

    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* color blend; blend hue and chroma, but not lightness */
static void _blend_color(const float *const restrict a, float *const restrict b,
                         const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    float tta[3], ttb[3];
    _blend_Lab_scale(&a[j], ta);
    _CLAMP_XYZ(ta, min, max);
    dt_Lab_2_LCH(ta, tta);

    _blend_Lab_scale(&b[j], tb);
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
    _blend_Lab_rescale(tb, &b[j]);


    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* color adjustment; blend hue and chroma; take lightness from module output */
static void _blend_coloradjust(const float *const restrict a, float *const restrict b,
                               const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    float tta[3], ttb[3];
    _blend_Lab_scale(&a[j], ta);
    _CLAMP_XYZ(ta, min, max);
    dt_Lab_2_LCH(ta, tta);

    _blend_Lab_scale(&b[j], tb);
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
    _blend_Lab_rescale(tb, &b[j]);

    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* inverse blend */
static void _blend_inverse(const float *const restrict a, float *const restrict b,
                           const float *const restrict mask, const size_t stride)
{
  float max[4] = { 0 }, min[4] = { 0 };
  _blend_colorspace_channel_range(min, max);

  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    tb[0] = clamp_range_f(ta[0] * (1.0f - local_opacity) + tb[0] * local_opacity, min[0], max[0]);
    tb[1] = clamp_range_f(ta[1] * (1.0f - local_opacity) + tb[1] * local_opacity, min[1], max[1]);
    tb[2] = clamp_range_f(ta[2] * (1.0f - local_opacity) + tb[2] * local_opacity, min[2], max[2]);

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* blend only lightness in Lab color space without any clamping (a noop for
 * other color spaces) */
static void _blend_Lab_lightness(const float *const restrict a, float *const restrict b,
                                 const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    tb[0] = ta[0] * (1.0f - local_opacity) + tb[0] * local_opacity;
    tb[1] = ta[1];
    tb[2] = ta[2];

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* blend only a-channel in Lab color space without any clamping (a noop for
 * other color spaces) */
static void _blend_Lab_a(const float *const restrict a, float *const restrict b,
                         const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    tb[0] = ta[0];
    tb[1] = ta[1] * (1.0f - local_opacity) + tb[1] * local_opacity;
    tb[2] = ta[2];

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}

/* blend only b-channel in Lab color space without any clamping (a noop for
 * other color spaces) */
static void _blend_Lab_b(const float *const restrict a, float *const restrict b,
                         const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    const float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    tb[0] = ta[0];
    tb[1] = ta[1];
    tb[2] = ta[2] * (1.0f - local_opacity) + tb[2] * local_opacity;

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
  }
}


/* blend only color in Lab color space without any clamping (a noop for other
 * color spaces) */
static void _blend_Lab_color(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride)
{
  for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
  {
    float local_opacity = mask[i];
    float ta[3], tb[3];
    _blend_Lab_scale(&a[j], ta);
    _blend_Lab_scale(&b[j], tb);

    tb[0] = ta[0];
    tb[1] = ta[1] * (1.0f - local_opacity) + tb[1] * local_opacity;
    tb[2] = ta[2] * (1.0f - local_opacity) + tb[2] * local_opacity;

    _blend_Lab_rescale(tb, &b[j]);
    b[j + DT_BLENDIF_LAB_BCH] = local_opacity;
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
    case DEVELOP_BLEND_UNBOUNDED:
    default:
      blend = _blend_normal_unbounded;
      break;
  }

  return blend;
}


static inline void _display_channel_value(float *const restrict out, const float value, const float mask)
{
  // We are in the lab color space, write only the luminance
  out[0] = value * 100.0f;
  out[1] = 0.0f;
  out[2] = 0.0f;
  out[3] = mask;
}

static void _display_channel(const float *const restrict a, float *const restrict b,
                             const float *const restrict mask, const size_t stride,
                             const dt_dev_pixelpipe_display_mask_t channel)
{
  switch(channel & DT_DEV_PIXELPIPE_DISPLAY_ANY)
  {
    case DT_DEV_PIXELPIPE_DISPLAY_L:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        const float c = clamp_range_f(a[j]/100.0f, 0.0f, 1.0f);
        _display_channel_value(&b[j], c, mask[i]);
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_L | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        const float c = clamp_range_f(b[j]/100.0f, 0.0f, 1.0f);
        _display_channel_value(&b[j], c, mask[i]);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_a:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        const float c = clamp_range_f((a[j+1]+128.0f)/256.0f, 0.0f, 1.0f);
        _display_channel_value(&b[j], c, mask[i]);
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_a | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        const float c = clamp_range_f((b[j+1]+128.0f)/256.0f, 0.0f, 1.0f);
        _display_channel_value(&b[j], c, mask[i]);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_b:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        const float c = clamp_range_f((a[j+2]+128.0f)/256.0f, 0.0f, 1.0f);
        _display_channel_value(&b[j], c, mask[i]);
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_b | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        const float c = clamp_range_f((b[j+2]+128.0f)/256.0f, 0.0f, 1.0f);
        _display_channel_value(&b[j], c, mask[i]);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_C:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        float LCH[3];
        dt_Lab_2_LCH(a + j, LCH);
        const float c = clamp_range_f(LCH[1]/(128.0f*sqrtf(2.0f)), 0.0f, 1.0f);
        _display_channel_value(&b[j], c, mask[i]);
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_LCH_C | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        float LCH[3];
        dt_Lab_2_LCH(b + j, LCH);
        const float c = clamp_range_f(LCH[1]/(128.0f*sqrtf(2.0f)), 0.0f, 1.0f);
        _display_channel_value(&b[j], c, mask[i]);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_h:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        float LCH[3];
        dt_Lab_2_LCH(a + j, LCH);
        const float c = clamp_range_f(LCH[2], 0.0f, 1.0f);
        _display_channel_value(&b[j], c, mask[i]);
      }
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_LCH_h | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        float LCH[3];
        dt_Lab_2_LCH(b + j, LCH);
        const float c = clamp_range_f(LCH[2], 0.0f, 1.0f);
        _display_channel_value(&b[j], c, mask[i]);
      }
      break;
    default:
      for(size_t i = 0, j = 0; j < stride; i++, j += DT_BLENDIF_LAB_CH)
      {
        _display_channel_value(&b[j], 0.0f, mask[i]);
      }
      break;
  }
}


void dt_develop_blendif_lab_blend(struct dt_dev_pixelpipe_iop_t *piece,
                                  const float *const restrict a, float *const restrict b,
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

  _blend_row_func *const blend = _choose_blend_func(d->blend_mode);

#ifdef _OPENMP
#pragma omp parallel default(none) \
  dt_omp_firstprivate(blend, a, b, mask, iwidth, owidth, oheight, xoffs, yoffs, mask_display, request_mask_display)
#endif
  {
    const size_t stride = (size_t)owidth * DT_BLENDIF_LAB_CH;
    if(request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
    {
#ifdef _OPENMP
#pragma omp for
#endif
      for(size_t y = 0; y < oheight; y++)
      {
        const float *const restrict in = a + ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_LAB_CH;
        float *const restrict out = b + y * owidth * DT_BLENDIF_LAB_CH;
        const float *const restrict m = mask + y * owidth;
        _display_channel(in, out, m, stride, request_mask_display);
      }
    }
    else
    {
#ifdef _OPENMP
#pragma omp for
#endif
      for(size_t y = 0; y < oheight; y++)
      {
        const float *const restrict in = a + ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_LAB_CH;
        float *const restrict out = b + y * owidth * DT_BLENDIF_LAB_CH;
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
        const float *const restrict in = a + ((y + yoffs) * iwidth + xoffs) * DT_BLENDIF_LAB_CH;
        float *const restrict out = b + y * owidth * DT_BLENDIF_LAB_CH;
        for(size_t j = 0; j < stride; j += DT_BLENDIF_LAB_CH) out[j + DT_BLENDIF_LAB_BCH] = in[j + DT_BLENDIF_LAB_BCH];
      }
    }
  }
}

// tools/update_modelines.sh
// remove-trailing-space on;
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
