/*
    This file is part of darktable,
    Copyright (C) 2016-2024 darktable developers.

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

#pragma once
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_OPENCL
#include <CL/cl.h>           // for cl_mem
#endif

#include "common/image.h"    // for dt_image_t, dt_image_orientation_t
#include "develop/imageop.h" // for dt_iop_roi_t
#include <glib.h>            // for inline
#include <math.h>            // for log, logf, powf
#include <stddef.h>          // for size_t, NULL
#include <stdint.h>          // for uint8_t, uint16_t, uint32_t

/** flip according to orientation bits, also zoom to given size. */
void dt_iop_flip_and_zoom_8(const uint8_t *in, int32_t iw, int32_t ih, uint8_t *out, int32_t ow, int32_t oh,
                            const dt_image_orientation_t orientation, uint32_t *width, uint32_t *height);

/** for homebrew pixel pipe: zoom pixel array. */
void dt_iop_clip_and_zoom(float *out, const float *const in, const struct dt_iop_roi_t *const roi_out,
                          const struct dt_iop_roi_t *const roi_in);

/** zoom pixel array for roi buffers. */
void dt_iop_clip_and_zoom_roi(float *out, const float *const in, const struct dt_iop_roi_t *const roi_out,
                              const struct dt_iop_roi_t *const roi_in);
#ifdef HAVE_OPENCL
int dt_iop_clip_and_zoom_cl(int devid, cl_mem dev_out, cl_mem dev_in,
                            const struct dt_iop_roi_t *const roi_out,
                            const struct dt_iop_roi_t *const roi_in);

int dt_iop_clip_and_zoom_roi_cl(int devid, cl_mem dev_out, cl_mem dev_in,
                                const struct dt_iop_roi_t *const roi_out,
                                const struct dt_iop_roi_t *const roi_in);
#endif

void dt_iop_clip_and_zoom_mosaic_half_size_f(float *const out, const float *const in,
                                             const dt_iop_roi_t *const roi_out, const dt_iop_roi_t *const roi_in,
                                             const int32_t out_stride, const int32_t in_stride,
                                             const uint32_t filters);

void dt_iop_clip_and_zoom_mosaic_half_size(uint16_t *const out, const uint16_t *const in,
                                           const dt_iop_roi_t *const roi_out, const dt_iop_roi_t *const roi_in,
                                           const int32_t out_stride, const int32_t in_stride,
                                           const uint32_t filters);

void dt_iop_clip_and_zoom_mosaic_third_size_xtrans(uint16_t *const out, const uint16_t *const in,
                                                   const dt_iop_roi_t *const roi_out,
                                                   const dt_iop_roi_t *const roi_in, const int32_t out_stride,
                                                   const int32_t in_stride, const uint8_t (*const xtrans)[6]);

void dt_iop_clip_and_zoom_mosaic_third_size_xtrans_f(float *const out, const float *const in,
                                                     const dt_iop_roi_t *const roi_out,
                                                     const dt_iop_roi_t *const roi_in, const int32_t out_stride,
                                                     const int32_t in_stride, const uint8_t (*const xtrans)[6]);

void dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f(float *out, const float *const in,
                                                            const struct dt_iop_roi_t *const roi_out,
                                                            const struct dt_iop_roi_t *const roi_in,
                                                            const int32_t out_stride,
                                                            const int32_t in_stride);

void dt_iop_clip_and_zoom_demosaic_half_size_f(float *out, const float *const in,
                                               const struct dt_iop_roi_t *const roi_out,
                                               const struct dt_iop_roi_t *const roi_in, const int32_t out_stride,
                                               const int32_t in_stride, const uint32_t filters);

/** x-trans sensor downscaling */

void dt_iop_clip_and_zoom_demosaic_third_size_xtrans_f(float *out, const float *const in,
                                                       const struct dt_iop_roi_t *const roi_out,
                                                       const struct dt_iop_roi_t *const roi_in,
                                                       const int32_t out_stride, const int32_t in_stride,
                                                       const uint8_t (*const xtrans)[6]);

/** as dt_iop_clip_and_zoom, but for rgba 8-bit channels. */
void dt_iop_clip_and_zoom_8(const uint8_t *i, int32_t ix, int32_t iy, int32_t iw, int32_t ih, int32_t ibw,
                            int32_t ibh, uint8_t *o, int32_t ox, int32_t oy, int32_t ow, int32_t oh,
                            int32_t obw, int32_t obh);

/** estimates an exponential form f(x) = a*x^g from a few (num) points (x, y).
 *  the largest point should be (1.0, y) to really get good data. */
static inline void dt_iop_estimate_exp(const float *const x, const float *const y, const int num, float *coeff)
{
  // map every thing to y = y0*(x/x0)^g
  // and fix (x0,y0) as the last point.
  // assume (x,y) pairs are ordered by ascending x, so this is the last point:
  const float x0 = x[num - 1], y0 = y[num - 1];

  float g = 0.0f;
  int cnt = 0;
  // solving for g yields
  // g = log(y/y0)/log(x/x0)
  //
  // average that over the course of the other samples:
  for(int k = 0; k < num - 1; k++)
  {
    const float yy = y[k] / y0, xx = x[k] / x0;
    if(yy > 0.0f && xx > 0.0f)
    {
      const float gg = logf(y[k] / y0) / logf(x[k] / x0);
      g += gg;
      cnt++;
    }
  }
  if(cnt)
    g *= 1.0f / cnt;
  else
    g = 1.0f;
  coeff[0] = 1.0f / x0;
  coeff[1] = y0;
  coeff[2] = g;
}


/** evaluates the exp fit. */
DT_OMP_DECLARE_SIMD()
static inline float dt_iop_eval_exp(const float *const coeff, const float x)
{
  return coeff[1] * powf(x * coeff[0], coeff[2]);
}


/** Copy alpha channel 1:1 from input to output */
DT_OMP_DECLARE_SIMD(uniform(width, height) aligned(ivoid, ovoid:64))
static inline void dt_iop_alpha_copy(const void *const ivoid,
                                     void *const ovoid,
                                     const size_t width, const size_t height)
{
  const float *const __restrict__ in = (const float *const)ivoid;
  float *const __restrict__ out = (float *const)ovoid;

  DT_OMP_FOR()
  for(size_t k = 3; k < width * height * 4; k += 4)
    out[k] = in[k];
}

/** Calculate the bayer pattern color from the row and column **/
static inline int FC(const size_t row, const size_t col, const uint32_t filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

#define RED 0
#define GREEN 1
#define BLUE 2
#define ALPHA 3

static inline float dt_iop_get_processed_maximum(dt_dev_pixelpipe_iop_t *piece)
{
  return  fmaxf(1.0f,
          fmaxf(piece->pipe->dsc.processed_maximum[0],
          fmaxf(piece->pipe->dsc.processed_maximum[1],
                piece->pipe->dsc.processed_maximum[2])));
}

static inline float dt_iop_get_processed_minimum(dt_dev_pixelpipe_iop_t *piece)
{
  return  fmaxf(1.0f,
          fminf(piece->pipe->dsc.processed_maximum[0],
          fminf(piece->pipe->dsc.processed_maximum[1],
                piece->pipe->dsc.processed_maximum[2])));
}

/** Calculate the xtrans pattern color from the row and column **/
static inline int FCxtrans(const int row, const int col, const dt_iop_roi_t *const roi,
                           const uint8_t (*const xtrans)[6])
{
  // Add +600 (which must be a multiple of CFA width 6) as offset can
  // be negative and need to ensure a non-negative array index. The
  // negative offsets in current code come from the demosaic iop:
  // Markesteijn 1-pass (-12), Markesteijn 3-pass (-17), and VNG (-2).
  int irow = row + 600;
  int icol = col + 600;
  assert(irow >= 0 && icol >= 0);

  if(roi)
  {
    irow += roi->y;
    icol += roi->x;
  }

  return xtrans[irow % 6][icol % 6];
}


DT_OMP_DECLARE_SIMD()
static inline int fcol(const int row, const int col, const uint32_t filters, const uint8_t (*const xtrans)[6])
{
  if(filters == 9)
    return FCxtrans(row, col, NULL, xtrans);
  else
    return FC(row, col, filters);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

