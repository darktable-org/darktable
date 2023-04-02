/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/colorspaces_inline_conversions.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_gamma_params_t)


typedef struct dt_iop_gamma_params_t
{
  float gamma, linear;
} dt_iop_gamma_params_t;

const char *name()
{
  return C_("modulename", "display encoding");
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_FENCE | IOP_FLAGS_UNSAFE_COPY;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(in, out, mask_color: 16) uniform(mask_color, alpha)
#endif
static inline void _write_pixel(const float *const restrict in,
                                uint8_t *const restrict out,
                                const float *const restrict mask_color,
                                const float alpha)
{
  // takes a linear RGB pixel as input
  dt_aligned_pixel_t pixel;

  // linear sRGB (REC 709) -> gamma corrected sRGB
  for(size_t c = 0; c < 3; c++)
    pixel[c] = in[c] <= 0.0031308f
      ? 12.92f * in[c]
      : (1.0f + 0.055f) * powf(in[c], 1.0f / 2.4f) - 0.055f;

  // the output of this module is BGR(A) instead of RGBA; can't use
  // for_each_channel here due to the index swap
  for(size_t c = 0; c < 3; c++)
  {
    const float value = roundf(255.0f * (pixel[c] * (1.0f - alpha) + mask_color[c] * alpha));
    out[2 - c] = (uint8_t)(fminf(fmaxf(value, 0.0f), 255.0f));
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(pixel: 16) uniform(norm)
#endif
static void _normalize_color(float *const restrict pixel, const float norm)
{
  // color may not be black!
  const float factor = norm / fmaxf(pixel[0], fmaxf(pixel[1], pixel[2]));
  for_each_channel(x)
    pixel[x] *= factor;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, sRGB: 16) uniform(norm)
#endif
static inline void _XYZ_to_REC_709_normalized(const float *const restrict XYZ,
                                              float *const restrict sRGB,
                                              const float norm)
{
  dt_XYZ_to_Rec709_D50(XYZ, sRGB);
  _normalize_color(sRGB, norm);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in, out: 64) uniform(buffsize, alpha)
#endif
static void _channel_display_monochrome(const float *const restrict in,
                                        uint8_t *const restrict out,
                                        const size_t buffsize,
                                        const float alpha)
{
  // yellow; "unused" element enables vectorization
  const dt_aligned_pixel_t mask_color = { 1.0f, 1.0f, 0.0f };

#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) \
  aligned(in, out: 64) aligned(mask_color: 16)               \
  dt_omp_firstprivate(in,out, buffsize, alpha, mask_color)
#endif
  for(size_t j = 0; j < buffsize; j += 4)
  {
    dt_aligned_pixel_t pixel = { in[j + 1], in[j + 1], in[j + 1], in[j + 1] };
    _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in, out: 64) uniform(buffsize, alpha, channel)
#endif
static void _channel_display_false_color(const float *const restrict in,
                                         uint8_t *const restrict out,
                                         const size_t buffsize,
                                         const float alpha,
                                         dt_dev_pixelpipe_display_mask_t channel)
{
  // yellow, "unused" element aids vectorization
  const dt_aligned_pixel_t mask_color = { 1.0f, 1.0f, 0.0f };

  switch(channel & DT_DEV_PIXELPIPE_DISPLAY_ANY & ~DT_DEV_PIXELPIPE_DISPLAY_OUTPUT)
  {
    case DT_DEV_PIXELPIPE_DISPLAY_a:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) \
  aligned(in, out: 64) aligned(mask_color: 16)                  \
  dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        dt_aligned_pixel_t xyz;
        dt_aligned_pixel_t pixel;
        // colors with "a" exceeding the range [-56,56] range will
        // yield colors not representable in sRGB
        const float value = fminf(fmaxf(in[j + 1] * 256.0f - 128.0f, -56.0f), 56.0f);
        const dt_aligned_pixel_t lab = { 79.0f - value * (11.0f / 56.0f), value, 0.0f, 0.0f };
        dt_Lab_to_XYZ(lab, xyz);
        _XYZ_to_REC_709_normalized(xyz, pixel, 0.75f);
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_b:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) \
  aligned(in, out: 64) aligned(mask_color: 16)                  \
  dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        dt_aligned_pixel_t xyz, pixel;
        // colors with "b" exceeding the range [-65,65] range will
        // yield colors not representable in sRGB
        const float value = fminf(fmaxf(in[j + 1] * 256.0f - 128.0f, -65.0f), 65.0f);
        const dt_aligned_pixel_t lab = { 60.0f + value * (2.0f / 65.0f), 0.0f, value, 0.0f };
        dt_Lab_to_XYZ(lab, xyz);
        _XYZ_to_REC_709_normalized(xyz, pixel, 0.75f);
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_R:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) \
  aligned(in, out: 64) aligned(mask_color: 16)                  \
  dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        const dt_aligned_pixel_t pixel = { in[j + 1], 0.0f, 0.0f, 0.0f };
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_G:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) \
  aligned(in, out: 64) aligned(mask_color: 16)                  \
  dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        const dt_aligned_pixel_t pixel = { 0.0f, in[j + 1], 0.0f, 0.0f };
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_B:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) \
  aligned(in, out: 64) aligned(mask_color: 16)                  \
  dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        const dt_aligned_pixel_t pixel = { 0.0f, 0.0f, in[j + 1], 0.0f };
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_C:
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_S:
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Cz:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) \
  aligned(in, out: 64) aligned(mask_color: 16)                  \
  dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        const dt_aligned_pixel_t pixel = { 0.5f, 0.5f * (1.0f - in[j + 1]), 0.5f, 0.0f };
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_h:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) \
  aligned(in, out: 64) aligned(mask_color: 16)                  \
  dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        dt_aligned_pixel_t lch = { 65.0f, 37.0f, in[j + 1], 0.0f };
        dt_aligned_pixel_t lab, xyz, pixel;
        dt_LCH_2_Lab(lch, lab);
        lab[3] = 0.0f;
        dt_Lab_to_XYZ(lab, xyz);
        _XYZ_to_REC_709_normalized(xyz, pixel, 0.75f);
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_H:
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) \
    dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        dt_aligned_pixel_t hsl = { in[j + 1], 0.5f, 0.5f, 0.0f };
        dt_aligned_pixel_t pixel;
        dt_HSL_2_RGB(hsl, pixel);
        _normalize_color(pixel, 0.75f);
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_hz:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) \
  aligned(in, out: 64) aligned(mask_color: 16)                  \
  dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        const dt_aligned_pixel_t JzCzhz = { 0.011f, 0.01f, in[j + 1] };
        dt_aligned_pixel_t JzAzBz;
        dt_aligned_pixel_t XYZ_D65;
        dt_aligned_pixel_t pixel;
        dt_JzCzhz_2_JzAzBz(JzCzhz, JzAzBz);
        dt_JzAzBz_2_XYZ(JzAzBz, XYZ_D65);
        dt_XYZ_to_Rec709_D65(XYZ_D65, pixel);
        _normalize_color(pixel, 0.75f);
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_L:
    case DT_DEV_PIXELPIPE_DISPLAY_GRAY:
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_l:
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Jz:
    default:
      _channel_display_monochrome(in, out, buffsize, alpha);
      break;
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in, out: 64) uniform(buffsize, alpha)
#endif
static void _mask_display(const float *const restrict in,
                          uint8_t *const restrict out,
                          const size_t buffsize,
                          const float alpha)
{
  // yellow, "unused" element aids vectorization
  const dt_aligned_pixel_t mask_color = { 1.0f, 1.0f, 0.0f };

  #ifdef _OPENMP
  #pragma omp parallel for simd default(none) schedule(static) \
    aligned(in, out: 64) aligned(mask_color: 16)                \
    dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
  #endif
    for(size_t j = 0; j < buffsize; j+= 4)
    {
      const float gray = 0.3f * in[j + 0] + 0.59f * in[j + 1] + 0.11f * in[j + 2];
      const dt_aligned_pixel_t pixel = { gray, gray, gray, gray };
      _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
    }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in, out: 64) uniform(buffsize)
#endif
static void _copy_output(const float *const restrict in,
                         uint8_t *const restrict out,
                         const size_t buffsize)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) aligned(in, out: 64) \
    dt_omp_firstprivate(in, out, buffsize)
#endif
  for(size_t j = 0; j < buffsize; j += 4)
  {
    // the output of this module is BGR(A) instead of RGBA, so we can't use for_each_channel
    for(size_t c = 0; c < 3; c++)
    {
      out[j + 2 - c] = (uint8_t)(fminf(roundf(255.0f * fmaxf(in[j + c], 0.0f)), 255.0f));
    }
  }
}


void process(struct dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const i,
             void *const o,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         i, o, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  // this module also expects the same size of input image as the output image
  if(roi_in->width != roi_out->width || roi_in->height != roi_out->height)
    return;

  const dt_dev_pixelpipe_display_mask_t mask_display = piece->pipe->mask_display;
  const gboolean fcolor = dt_conf_is_equal("channel_display", "false color");

  const size_t buffsize = (size_t)roi_out->width * roi_out->height * 4;
  const float alpha = (mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) ? 1.0f : 0.0f;

  if((mask_display & DT_DEV_PIXELPIPE_DISPLAY_CHANNEL)
     && (mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY))
  {
    if(fcolor)
    {
      _channel_display_false_color((const float *const restrict)i,
                                   (uint8_t *const restrict)o, buffsize, alpha,
                                   mask_display);
    }
    else
    {
      _channel_display_monochrome((const float *const restrict)i,
                                  (uint8_t *const restrict)o, buffsize, alpha);
    }
  }
  else if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
  {
    _mask_display((const float *const restrict)i, (uint8_t *const restrict)o, buffsize, 1.0f);
  }
  else
  {
    _copy_output((const float *const restrict)i, (uint8_t *const restrict)o, buffsize);
  }
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_gamma_data_t));
  module->params = calloc(1, sizeof(dt_iop_gamma_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_gamma_params_t));
  module->params_size = sizeof(dt_iop_gamma_params_t);
  module->gui_data = NULL;
  module->hide_enable_button = TRUE;
  module->default_enabled = TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
