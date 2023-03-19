/* --------------------------------------------------------------------------
    This file is part of darktable,
    Copyright (C) 2012-2023 darktable developers.

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
* ------------------------------------------------------------------------*/

#pragma once

#include "common/opencl.h"
#include "develop/pixelpipe_hb.h"

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** Available interpolations */
enum dt_interpolation_type
{
  DT_INTERPOLATION_FIRST = 0,                         /**< Helper for easy iteration on interpolators */
  DT_INTERPOLATION_BILINEAR = DT_INTERPOLATION_FIRST, /**< Bilinear interpolation (aka tent filter) */
  DT_INTERPOLATION_BICUBIC,                           /**< Bicubic interpolation (with -0.5 parameter) */
  DT_INTERPOLATION_LANCZOS2,                          /**< Lanczos interpolation (with 2 lobes) */
  DT_INTERPOLATION_LANCZOS3,                          /**< Lanczos interpolation (with 3 lobes) */
  DT_INTERPOLATION_LAST,                              /**< Helper for easy iteration on interpolators */
  DT_INTERPOLATION_DEFAULT = DT_INTERPOLATION_BILINEAR,
  DT_INTERPOLATION_DEFAULT_WARP = DT_INTERPOLATION_BICUBIC,
  DT_INTERPOLATION_USERPREF,  /**< can be specified so that user setting is chosen */
  DT_INTERPOLATION_USERPREF_WARP  /**< can be specified so that user setting is chosen */
};

/** Interpolation function */
typedef float (*dt_interpolation_func)(float *taps,
                                       size_t num_taps,
                                       float width,
                                       float first_tap,
                                       float interval);

/** Interpolation structure */
struct dt_interpolation
{
  enum dt_interpolation_type id;     /**< Id such as defined by the dt_interpolation_type */
  const char *name;                  /**< internal name  */
  size_t width;                      /**< Half width of its kernel support */
  dt_interpolation_func maketaps;    /**< Kernel function */
};

/** Compute a single interpolated sample.
 *
 * This function computes a single interpolated sample. Implied costs are:
 * <ul>
 * <li>Horizontal filtering kernel computation</li>
 * <li>Vertical filtering kernel computation</li>
 * <li>Sample computation</li>
 * </ul>
 *
 * @param in Input image
 * @param itor interpolator to be used
 * @param x X-Coordinate of the requested sample
 * @param y Y-Coordinate of the requested sample
 * @param width Width of the input image
 * @param height Width of the input image
 * @param samplestride Stride in bytes for a sample
 * @param linestride Stride in bytes for complete line
 *
 * @return computed sample
 */
float dt_interpolation_compute_sample(const struct dt_interpolation *itor, const float *in, const float x,
                                      const float y, const int width, const int height,
                                      const int samplestride, const int linestride);

/** Compute an interpolated 4 component pixel.
 *
 * This function computes a full 4 component pixel. This helps a bit speedwise
 * as interpolation coordinates are supposed to be the same for all components.
 * Thus we can share horizontal and vertical interpolation kernels across all
 * components
 *
 * NB: a pixel is to be four floats big in stride
 *
 * @param in Pointer to the input image
 * @param out Pointer to the output sample
 * @param itor interpolator to be used
 * @param x X-Coordinate of the requested sample
 * @param y Y-Coordinate of the requested sample
 * @param width Width of the input image
 * @param height Width of the input image
 * @param linestride Stride in bytes for complete line
 *
 */
void dt_interpolation_compute_pixel4c(const struct dt_interpolation *itor, const float *in, float *out,
                                      const float x, const float y, const int width, const int height,
                                      const int linestride);

// same as above for single channel images (i.e., masks). no SSE or CPU code paths for now
void dt_interpolation_compute_pixel1c(const struct dt_interpolation *itor, const float *in, float *out,
                                      const float x, const float y, const int width, const int height,
                                      const int linestride);

/** Get an interpolator from type
 * @param type Interpolator to search for
 * @return requested interpolator or default if not found (this function can't fail)
 */
const struct dt_interpolation *dt_interpolation_new(enum dt_interpolation_type type);

/** Image resampler.
 *
 * Resamples the image "in" to "out" according to roi values. Here is the
 * exact contract:
 * <ul>
 * <li>The resampling is isotropic (same for both x and y directions),
 * represented by roi_out->scale</li>
 * <li>It generates roi_out->width samples horizontally whose positions span
 * from roi_out->x to roi_out->x + roi_out->width - 1</li>
 * <li>It generates roi_out->height samples vertically whose positions span
 * from roi_out->y to roi_out->y + roi_out->height - 1</li>
 * </ul>
 *
 * @param itor [in] Interpolator to use
 * @param out [out] Will hold the resampled image
 * @param roi_out [in] Region of interest of the resampled image
 * @param out_stride [in] Output line stride in <strong>bytes</strong>
 * @param in [in] Will hold the resampled image
 * @param roi_in [in] Region of interest of the original image
 * @param in_stride [in] Input line stride in <strong>bytes</strong>
 */
void dt_interpolation_resample(const struct dt_interpolation *itor, float *out,
                               const dt_iop_roi_t *const roi_out, const int32_t out_stride,
                               const float *const in, const dt_iop_roi_t *const roi_in,
                               const int32_t in_stride);

void dt_interpolation_resample_roi(const struct dt_interpolation *itor, float *out,
                                   const dt_iop_roi_t *const roi_out, const int32_t out_stride,
                                   const float *const in, const dt_iop_roi_t *const roi_in,
                                   const int32_t in_stride);

#ifdef HAVE_OPENCL
typedef struct dt_interpolation_cl_global_t
{
  int kernel_interpolation_resample;
} dt_interpolation_cl_global_t;

dt_interpolation_cl_global_t *dt_interpolation_init_cl_global(void);

void dt_interpolation_free_cl_global(dt_interpolation_cl_global_t *g);


/** Image resampler OpenCL version.
 *
 * Resamples the image "in" to "out" according to roi values. Here is the
 * exact contract:
 * <ul>
 * <li>The resampling is isotropic (same for both x and y directions),
 * represented by roi_out->scale</li>
 * <li>It generates roi_out->width samples horizontally whose positions span
 * from roi_out->x to roi_out->x + roi_out->width - 1</li>
 * <li>It generates roi_out->height samples vertically whose positions span
 * from roi_out->y to roi_out->y + roi_out->height - 1</li>
 * </ul>
 *
 * @param itor [in] Interpolator to use
 * @param devid [in] The device to run on
 * @param dev_out [out] Will hold the resampled image
 * @param roi_out [in] Region of interest of the resampled image
 * @param out_stride [in] Output line stride in <strong>bytes</strong>
 * @param dev_in [in] Will hold the resampled image
 * @param roi_in [in] Region of interest of the original image
 * @param in_stride [in] Input line stride in <strong>bytes</strong>
 */
int dt_interpolation_resample_cl(const struct dt_interpolation *itor, int devid, cl_mem dev_out,
                                 const dt_iop_roi_t *const roi_out, cl_mem dev_in,
                                 const dt_iop_roi_t *const roi_in);

int dt_interpolation_resample_roi_cl(const struct dt_interpolation *itor, int devid, cl_mem dev_out,
                                     const dt_iop_roi_t *const roi_out, cl_mem dev_in,
                                     const dt_iop_roi_t *const roi_in);
#endif

// same as above for single channel images (i.e., masks). no SSE or CPU code paths for now
void dt_interpolation_resample_1c(const struct dt_interpolation *itor, float *out,
                                  const dt_iop_roi_t *const roi_out, const int32_t out_stride,
                                  const float *const in, const dt_iop_roi_t *const roi_in,
                                  const int32_t in_stride);

void dt_interpolation_resample_roi_1c(const struct dt_interpolation *itor, float *out,
                                      const dt_iop_roi_t *const roi_out, const int32_t out_stride,
                                      const float *const in, const dt_iop_roi_t *const roi_in,
                                      const int32_t in_stride);

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

