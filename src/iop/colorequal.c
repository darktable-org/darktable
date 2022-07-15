/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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

#include "common/extra_optimizations.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bauhaus/bauhaus.h"
#include "common/chromatic_adaptation.h"
#include "common/darktable_ucs_22_helpers.h"
#include "common/darktable.h"
#include "common/fast_guided_filter.h"
#include "common/eigf.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include "iop/choleski.h"
#include "common/iop_group.h"
#include "common/colorspaces_inline_conversions.h"

#ifdef _OPENMP
#include <omp.h>
#endif

// sRGB primary red records at 20° of hue in darktable UCS 22, so we offset the whole hue range
// such that red is the origin hues in the GUI. This is consistent with HSV/HSL color wheels UI.
#define ANGLE_SHIFT +20.f
#define DEG_TO_RAD(x) ((x + ANGLE_SHIFT) * M_PI / 180.f)
#define RAD_TO_DEG(x) (x * 180.f / M_PI - ANGLE_SHIFT)

#define NODES 8

#define SLIDER_BRIGHTNESS 0.50f // 50 %

#define GRAPH_GRADIENTS 64

DT_MODULE_INTROSPECTION(1, dt_iop_colorequal_params_t)

typedef struct dt_iop_colorequal_params_t
{
  float smoothing_saturation;    // $MIN: 0.05 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "curve smoothing"
  float smoothing_hue;           // $MIN: 0.05 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "curve smoothing"
  float smoothing_brightness;    // $MIN: 0.05 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "curve smoothing"

  float white_level;  // $MIN: -2.0 $MAX: 16.0 $DEFAULT: 1.0 $DESCRIPTION: "white level"
  float size;         // $MIN: 0.01 $MAX: 250 $DEFAULT: 5 $DESCRIPTION: "chroma smoothing size"
  float feathering;   // $MIN: 0.0 $MAX: 1000. $DEFAULT: 5 $DESCRIPTION: "chroma feathering"

  gboolean use_filter; // $DEFAULT: FALSE $DESCRIPTION: "use guided filter"

  // Note: what follows is tedious because each param needs to be declared separately.
  // A more efficient way would be to use 3 arrays of 8 elements,
  // but then GUI sliders would need to be wired manually to the correct array index.
  // So we do it the tedious way here, and let the introspection magic connect sliders to params automatically,
  // then we pack the params in arrays in commit_params().

  float sat_red;       // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "red"
  float sat_orange;    // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "orange"
  float sat_lime;      // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "lime"
  float sat_green;     // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "green"
  float sat_turquoise; // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "turquoise"
  float sat_blue;      // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "blue"
  float sat_lavender;  // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "lavender"
  float sat_purple;    // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "purple"

  float hue_red;       // $MIN: -180. $MAX: 180 $DEFAULT: 0. $DESCRIPTION: "red"
  float hue_orange;    // $MIN: -180. $MAX: 180 $DEFAULT: 0. $DESCRIPTION: "orange"
  float hue_lime;      // $MIN: -180. $MAX: 180 $DEFAULT: 0. $DESCRIPTION: "lime"
  float hue_green;     // $MIN: -180. $MAX: 180 $DEFAULT: 0. $DESCRIPTION: "green"
  float hue_turquoise; // $MIN: -180. $MAX: 180 $DEFAULT: 0. $DESCRIPTION: "turquoise"
  float hue_blue;      // $MIN: -180. $MAX: 180 $DEFAULT: 0. $DESCRIPTION: "blue"
  float hue_lavender;  // $MIN: -180. $MAX: 180 $DEFAULT: 0. $DESCRIPTION: "lavender"
  float hue_purple;    // $MIN: -180. $MAX: 180 $DEFAULT: 0. $DESCRIPTION: "purple"

  float bright_red;       // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "red"
  float bright_orange;    // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "orange"
  float bright_lime;      // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "lime"
  float bright_green;     // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "green"
  float bright_turquoise; // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "turquoise"
  float bright_blue;      // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "blue"
  float bright_lavender;  // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "lavender"
  float bright_purple;    // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "purple"
} dt_iop_colorequal_params_t;


typedef enum dt_iop_colorequal_channel_t
{
  SATURATION = 0,
  HUE = 1,
  BRIGHTNESS = 2,
  NUM_CHANNELS = 3,
} dt_iop_colorequal_channel_t;


typedef struct dt_iop_colorequal_data_t
{
  float *LUT_saturation;
  float *LUT_hue;
  float *LUT_brightness;
  float *gamut_LUT;
  gboolean lut_inited;
  float white_level;
  float size;
  float feathering;
  gboolean use_filter;
  dt_iop_order_iccprofile_info_t *work_profile;
} dt_iop_colorequal_data_t;


const char *name()
{
  return _("color equalizer");
}

const char *aliases()
{
  return _("color zones");
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_GRADING;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

typedef struct dt_iop_colorequal_gui_data_t
{
  GtkWidget *white_level;
  GtkWidget *sat_red, *sat_orange, *sat_lime, *sat_green, *sat_turquoise, *sat_blue, *sat_lavender, *sat_purple;
  GtkWidget *hue_red, *hue_orange, *hue_lime, *hue_green, *hue_turquoise, *hue_blue, *hue_lavender, *hue_purple;
  GtkWidget *bright_red, *bright_orange, *bright_lime, *bright_green, *bright_turquoise, *bright_blue, *bright_lavender, *bright_purple;

  GtkWidget *smoothing_saturation, *smoothing_bright, *smoothing_hue;
  GtkWidget *size, *feathering, *use_filter;

  // Array-like re-indexing of the above for efficient uniform handling in loops
  // Populate the array in gui_init()
  GtkWidget *sat_sliders[NODES];
  GtkWidget *hue_sliders[NODES];
  GtkWidget *bright_sliders[NODES];

  GtkNotebook *notebook;
  GtkDrawingArea *area;
  float *LUT;
  dt_iop_colorequal_channel_t channel;

  dt_iop_order_iccprofile_info_t *work_profile;
  dt_iop_order_iccprofile_info_t *white_adapted_profile;

  cairo_pattern_t *gradients[NUM_CHANNELS][GRAPH_GRADIENTS];

  float max_saturation;
  gboolean gradients_cached;

  float *gamut_LUT;
} dt_iop_colorequal_gui_data_t;


void _guide_with_chromaticity(const float *const restrict U, const float *const restrict V,
                              float *const restrict corrections,
                              const size_t width, const size_t height,
                              const float sigma, const float epsilon)
{
  // We guide the 3-channels corrections with the 2-channels chromaticity coordinates UV
  // aka we express corrections = a * UV + b where a is a 2×2 matrix and b a constant
  // Therefore the guided filter computation is a bit more complicated than the typical 1-channel case.
  // We use by-the-book 3-channels fast guided filter as in http://kaiminghe.com/eccv10/ but obviously reduced to 2.
  // We know that it tends to oversmooth the input where its intensity is close to 0,
  // but this is actually desirable here since chromaticity -> 0 means neutral greys
  // and we want to discard them as much as possible from any color equalization.

  // Downsample for speed-up
  const size_t pixels = width * height;
  const float scaling = fmaxf(fminf(sigma, 4.0f), 1.0f);
  const float ds_sigma = fmaxf(sigma / scaling, 1.0f);
  const size_t ds_height = height / scaling;
  const size_t ds_width = width / scaling;
  const size_t ds_pixels = ds_width * ds_height;

  float *const restrict ds_U = dt_alloc_align_float(ds_pixels);
  float *const restrict ds_V = dt_alloc_align_float(ds_pixels);
  float *const restrict ds_corrections = dt_alloc_align_float(ds_pixels * 4);

  interpolate_bilinear(U, width, height, ds_U, ds_width, ds_height, 1);
  interpolate_bilinear(V, width, height, ds_V, ds_width, ds_height, 1);
  interpolate_bilinear(corrections, width, height, ds_corrections, ds_width, ds_height, 4);

  // Init the symmetric covariance matrix of the guide (4 elements by pixel) :
  // covar = [[ covar(U, U), covar(U, V)],
  //          [ covar(V, U), covar(V, V)]]
  // with covar(x, y) = avg(x * y) - avg(x) * avg(y), corr(x, y) = x * y
  // so here, we init it with x * y, compute all the avg() at the next step
  // and subtract avg(x) * avg(y) later
  float *const restrict covariance = dt_alloc_align_float(ds_pixels * 4);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ds_pixels, ds_U, ds_V, covariance)  \
  schedule(simd:static) aligned(ds_U, ds_V, covariance: 64)
#endif
  for(size_t k = 0; k < ds_pixels; k++)
  {
    // corr(U, U)
    covariance[4 * k + 0] = ds_U[k] * ds_U[k];
    // corr(U, V)
    covariance[4 * k + 1] = covariance[4 * k + 2] = ds_U[k] * ds_V[k];
    // corr(V, V)
    covariance[4 * k + 3] = ds_V[k] * ds_V[k];
  }

  // We actually compute 3 guided filters here :
  // saturation = corrections[0] = a_sat * UV + b_sat
  // hue        = corrections[1] = a_hue * UV + b_hue
  // brightness = corrections[2] = a_bright * UV + b_bright
  // Generate here the 6 correlations between saturation, hue, brightness
  // and U, V.
  float *const restrict sat_uv = dt_alloc_align_float(ds_pixels * 2);
  float *const restrict hue_uv = dt_alloc_align_float(ds_pixels * 2);
  float *const restrict bri_uv = dt_alloc_align_float(ds_pixels * 2);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ds_pixels, sat_uv, hue_uv, bri_uv, ds_U, ds_V, ds_corrections, stdout)  \
  schedule(simd:static) aligned(sat_uv, hue_uv, bri_uv, ds_U, ds_V, ds_corrections: 64)
#endif
  for(size_t k = 0; k < ds_pixels; k++)
  {
    // corr(U, saturation)
    sat_uv[k * 2 + 0] = ds_corrections[k * 4 + 0] * ds_U[k];
    // corr(V, saturation)
    sat_uv[k * 2 + 1] = ds_corrections[k * 4 + 0] * ds_V[k];

    //fprintf(stdout, "sat_uv : %f - %f\n", sat_uv[k * 2 + 0], sat_uv[k * 2 + 1]);

    // corr(U, hue)
    hue_uv[k * 2 + 0] = ds_corrections[k * 4 + 1] * ds_U[k];
    // corr(V, hue)
    hue_uv[k * 2 + 1] = ds_corrections[k * 4 + 1] * ds_V[k];

    // corr(U, brightness)
    bri_uv[k * 2 + 0] = ds_corrections[k * 4 + 2] * ds_U[k];
    // corr(V, brightness)
    bri_uv[k * 2 + 1] = ds_corrections[k * 4 + 2] * ds_V[k];
  }

  // Compute the local averages of everything over the window size
  // We use a gaussian blur as a weighted local average because it's a radial function
  // so it will not favour vertical and horizontal edges over diagonal ones
  // as the by-the-book box blur (unweighted local average) would.

  // We use unbounded signals, so don't care for the internal value clipping
  float max[4] = { 1000.f };
  float min[4] = { -1000.f };

  dt_gaussian_t *gauss_1c = dt_gaussian_init(ds_width, ds_height, 1, max, min, ds_sigma, 0);
  dt_gaussian_blur(gauss_1c, ds_U, ds_U);
  dt_gaussian_blur(gauss_1c, ds_V, ds_V);

  dt_gaussian_t *gauss_2c = dt_gaussian_init(ds_width, ds_height, 2, max, min, ds_sigma, 0);
  dt_gaussian_blur(gauss_2c, sat_uv, sat_uv);
  dt_gaussian_blur(gauss_2c, hue_uv, hue_uv);
  dt_gaussian_blur(gauss_2c, bri_uv, bri_uv);

  dt_gaussian_t *gauss_4c = dt_gaussian_init(ds_width, ds_height, 4, max, min, ds_sigma, 0);
  dt_gaussian_blur(gauss_4c, ds_corrections, ds_corrections);
  dt_gaussian_blur(gauss_4c, covariance, covariance);
  dt_gaussian_free(gauss_4c);

  // Finish the UV covariance matrix computation by subtracting avg(x) * avg(y)
  // to avg(x * y) already computed
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ds_pixels, ds_U, ds_V, covariance)  \
  schedule(simd:static) aligned(ds_U, ds_V, covariance: 64)
#endif
  for(size_t k = 0; k < ds_pixels; k++)
  {
    // covar(U, U) = var(U)
    covariance[4 * k + 0] -= ds_U[k] * ds_U[k];
    // covar(U, V)
    covariance[4 * k + 1] -= ds_U[k] * ds_V[k];
    covariance[4 * k + 2] -= ds_U[k] * ds_V[k];
    // covar(V, V) = var(V)
    covariance[4 * k + 3] -= ds_V[k] * ds_V[k];
  }

  // Compute the 3 2D covariances between UV and (saturation, hue, brightness)
  // Remember these are actually 3 different guided filters from the same UV
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ds_pixels, sat_uv, hue_uv, bri_uv, ds_U, ds_V, ds_corrections, stdout)  \
  schedule(simd:static) aligned(sat_uv, hue_uv, bri_uv, ds_U, ds_V, ds_corrections: 64)
#endif
  for(size_t k = 0; k < ds_pixels; k++)
  {
    // covar(U, sat)
    sat_uv[k * 2 + 0] -= ds_corrections[k * 4 + 0] * ds_U[k];
    // covar(V, sat)
    sat_uv[k * 2 + 1] -= ds_corrections[k * 4 + 0] * ds_V[k];

    //fprintf(stdout, "sat_uv : %f - %f\n", sat_uv[k * 2 + 0], sat_uv[k * 2 + 1]);

    // covar(U, hue)
    hue_uv[k * 2 + 0] -= ds_corrections[k * 4 + 1] * ds_U[k];
    // covar(V, hue)
    hue_uv[k * 2 + 1] -= ds_corrections[k * 4 + 1] * ds_V[k];

    // covar(U, brightness)
    bri_uv[k * 2 + 0] -= ds_corrections[k * 4 + 2] * ds_U[k];
    // covar(V, brightness)
    bri_uv[k * 2 + 1] -= ds_corrections[k * 4 + 2] * ds_V[k];
  }

  // Compute a and b the params of the guided filters
  float *const restrict a_hue = dt_alloc_align_float(2 * ds_pixels);
  float *const restrict a_sat = dt_alloc_align_float(2 * ds_pixels);
  float *const restrict a_bri = dt_alloc_align_float(2 * ds_pixels);
  float *const restrict b_hue = dt_alloc_align_float(ds_pixels);
  float *const restrict b_sat = dt_alloc_align_float(ds_pixels);
  float *const restrict b_bri = dt_alloc_align_float(ds_pixels);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ds_pixels, sat_uv, hue_uv, bri_uv, ds_U, ds_V, ds_corrections, covariance, a_sat, a_hue, a_bri, b_sat, b_hue, b_bri, epsilon, stdout)  \
  schedule(simd:static) aligned(sat_uv, hue_uv, bri_uv, ds_U, ds_V, ds_corrections, covariance, a_sat, a_hue, a_bri, b_sat, b_hue, b_bri: 64)
#endif
  for(size_t k = 0; k < ds_pixels; k++)
  {
    // Extract the 2×2 covariance matrix sigma = cov(U, V) at current pixel
    float *const restrict Sigma = covariance + 4 * k;

    // Add the covariance threshold : sigma' = sigma + epsilon * Identity
    Sigma[0] += epsilon;
    Sigma[3] += epsilon;

    // Invert the 2×2 sigma matrix algebraically
    // see https://www.mathcentre.ac.uk/resources/uploaded/sigma-matrices7-2009-1.pdf
    const float det = (Sigma[0] * Sigma[3] - Sigma[1] * Sigma[2]);
    dt_aligned_pixel_t sigma_inv = { Sigma[3] / det, -Sigma[1] / det,
                                    -Sigma[2] / det,  Sigma[0] / det };
    // Note : epsilon prevents determinant == 0 so the invert exists all the time

    // a_sat = dot_product(cov(sat, uv), sigma_inv)
    a_sat[2 * k + 0] = (sat_uv[2 * k + 0] * sigma_inv[0] + sat_uv[2 * k + 1] * sigma_inv[1]);
    a_sat[2 * k + 1] = (sat_uv[2 * k + 0] * sigma_inv[2] + sat_uv[2 * k + 1] * sigma_inv[3]);

    // a_hue = dot_product(cov(hue, uv), sigma_inv)
    a_hue[2 * k + 0] = (hue_uv[2 * k + 0] * sigma_inv[0] + hue_uv[2 * k + 1] * sigma_inv[1]);
    a_hue[2 * k + 1] = (hue_uv[2 * k + 0] * sigma_inv[2] + hue_uv[2 * k + 1] * sigma_inv[3]);

    // a_bri = dot_product(cov(bright, uv), sigma_inv)
    a_bri[2 * k + 0] = (bri_uv[2 * k + 0] * sigma_inv[0] + bri_uv[2 * k + 1] * sigma_inv[1]);
    a_bri[2 * k + 1] = (bri_uv[2 * k + 0] * sigma_inv[2] + bri_uv[2 * k + 1] * sigma_inv[3]);

    b_sat[k] = ds_corrections[4 * k + 0] - a_sat[2 * k + 0] * ds_U[k] - a_sat[2 * k + 1] * ds_V[k];
    b_hue[k] = ds_corrections[4 * k + 1] - a_hue[2 * k + 0] * ds_U[k] - a_hue[2 * k + 1] * ds_V[k];
    b_bri[k] = ds_corrections[4 * k + 2] - a_bri[2 * k + 0] * ds_U[k] - a_bri[2 * k + 1] * ds_V[k];

    /*
    fprintf(stdout, "sigma : %f - %f - %f - %f\n", Sigma[0], Sigma[1], Sigma[2], Sigma[3]);
    fprintf(stdout, "cov : %f - %f\n", sat_uv[2 * k + 0], sat_uv[2 * k + 0]);
    fprintf(stdout, "a, b : %f - %f - %f\n", a_sat[2 * k + 0], a_sat[2 * k + 1], b_sat[k]);
    */
  }

  dt_free_align(covariance);
  dt_free_align(ds_U);
  dt_free_align(ds_V);
  dt_free_align(ds_corrections);
  dt_free_align(sat_uv);
  dt_free_align(hue_uv);
  dt_free_align(bri_uv);

  // Compute the averages of a and b for each filter
  dt_gaussian_blur(gauss_2c, a_sat, a_sat);
  dt_gaussian_blur(gauss_2c, a_hue, a_hue);
  dt_gaussian_blur(gauss_2c, a_bri, a_bri);
  dt_gaussian_free(gauss_2c);

  dt_gaussian_blur(gauss_1c, b_sat, b_sat);
  dt_gaussian_blur(gauss_1c, b_hue, b_hue);
  dt_gaussian_blur(gauss_1c, b_bri, b_bri);
  dt_gaussian_free(gauss_1c);

  // Upsample a and b to real-size image
  float *const restrict a_sat_full = dt_alloc_align_float(pixels * 2);
  float *const restrict a_hue_full = dt_alloc_align_float(pixels * 2);
  float *const restrict a_bri_full = dt_alloc_align_float(pixels * 2);
  float *const restrict b_sat_full = dt_alloc_align_float(pixels);
  float *const restrict b_hue_full = dt_alloc_align_float(pixels);
  float *const restrict b_bri_full = dt_alloc_align_float(pixels);

  interpolate_bilinear(a_sat, ds_width, ds_height, a_sat_full, width, height, 2);
  interpolate_bilinear(a_hue, ds_width, ds_height, a_hue_full, width, height, 2);
  interpolate_bilinear(a_bri, ds_width, ds_height, a_bri_full, width, height, 2);
  interpolate_bilinear(b_sat, ds_width, ds_height, b_sat_full, width, height, 1);
  interpolate_bilinear(b_hue, ds_width, ds_height, b_hue_full, width, height, 1);
  interpolate_bilinear(b_bri, ds_width, ds_height, b_bri_full, width, height, 1);

  dt_free_align(a_sat);
  dt_free_align(a_hue);
  dt_free_align(a_bri);
  dt_free_align(b_sat);
  dt_free_align(b_hue);
  dt_free_align(b_bri);

  // Apply the guided filter
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(pixels, corrections, a_sat_full, a_hue_full, a_bri_full, b_sat_full, b_hue_full, b_bri_full, U, V, stdout)  \
  schedule(simd:static) aligned(corrections, a_sat_full, a_hue_full, a_bri_full, b_sat_full, b_hue_full, b_bri_full, U, V: 64)
#endif
  for(size_t k = 0; k < pixels; k++)
  {
    // For each correction factor, we re-express it as a[0] * U + a[1] * V + b
    corrections[4 * k + 0] = a_sat_full[2 * k + 0] * U[k] + a_sat_full[2 * k + 1] * V[k] + b_sat_full[k];
    corrections[4 * k + 1] = a_hue_full[2 * k + 0] * U[k] + a_hue_full[2 * k + 1] * V[k] + b_hue_full[k];
    corrections[4 * k + 2] = a_bri_full[2 * k + 0] * U[k] + a_bri_full[2 * k + 1] * V[k] + b_bri_full[k];
  }

  dt_free_align(a_sat_full);
  dt_free_align(a_hue_full);
  dt_free_align(a_bri_full);
  dt_free_align(b_sat_full);
  dt_free_align(b_hue_full);
  dt_free_align(b_bri_full);
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorequal_data_t *d = (dt_iop_colorequal_data_t *)piece->data;

  const int ch = piece->colors;
  const float *const restrict in = (float*)i;
  float *const restrict out = (float*)o;

  const size_t npixels = (size_t)roi_out->width * roi_out->height;

  // STEP 0: prepare the RGB <-> XYZ D65 matrices
  // see colorbalancergb.c process() for the details, it's exactly the same
  const struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return; // no point

  dt_colormatrix_t input_matrix;
  dt_colormatrix_t output_matrix;
  dt_colormatrix_mul(input_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
  dt_colormatrix_mul(output_matrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);

  float *const restrict U = dt_alloc_align_float(npixels);
  float *const restrict V = dt_alloc_align_float(npixels);
  float *const restrict corrections = dt_alloc_align_float(npixels * 4);

  const float white = Y_to_dt_UCS_L_star(d->white_level);

  // STEP 1: convert image from RGB to darktable UCS LUV then HSB
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ch, npixels, in, out, U, V, corrections, input_matrix, d, white)  \
  schedule(simd:static) aligned(in, out, U, V, corrections, input_matrix : 64)
#endif
  for(size_t k = 0; k < npixels; k++)
  {
    const float *const restrict pix_in = __builtin_assume_aligned(in + k * ch, 16);
    float *const restrict pix_out = __builtin_assume_aligned(out + k * ch, 16);
    float *const restrict corrections_out = __builtin_assume_aligned(corrections + k * ch, 16);

    // Convert to XYZ D65
    dt_aligned_pixel_t XYZ_D65 = { 0.f };
    dot_product(pix_in, input_matrix, XYZ_D65);

    // Convert to dt UCS 22 UV and store UV
    dt_aligned_pixel_t xyY = { 0.f };
    dt_XYZ_to_xyY(XYZ_D65, xyY);
    float UV[2];
    xyY_to_dt_UCS_UV(xyY, UV);
    U[k] = UV[0];
    V[k] = UV[1];

    // Finish the conversion to dt UCS JCH then HSB
    dt_aligned_pixel_t JCH = { 0.f };
    dt_UCS_LUV_to_JCH(Y_to_dt_UCS_L_star(xyY[2]), white, UV, JCH);
    dt_UCS_JCH_to_HSB(JCH, pix_out);

    // Get the boosts
    corrections_out[1] = lookup_gamut(d->LUT_saturation, pix_out[0]);
    corrections_out[0] = lookup_gamut(d->LUT_hue, pix_out[0]);
    corrections_out[2] = lookup_gamut(d->LUT_brightness, pix_out[0]);

    // Copy alpha
    pix_out[3] = pix_in[3];
  }

  // STEP 2: apply a guided filter on the corrections, guided with UV chromaticity, to ensure
  // spatially-contiguous corrections even though the hue is not perfectly constant
  // this will help avoiding chroma noise.
  if(d->use_filter)
    _guide_with_chromaticity(U, V, corrections, roi_out->width, roi_out->height, d->size, 1.f / d->feathering);

  fprintf(stdout, "use filter: %i, feathering: %f, size: %f\n", d->use_filter, d->feathering, d->size);

  // STEP 3: apply the corrections and convert back to RGB
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ch, npixels, out, corrections, output_matrix, white, d)  \
  schedule(simd:static) aligned(out, corrections, output_matrix: 64)
#endif
  for(size_t k = 0; k < npixels; k++)
  {
    float *const restrict corrections_out = __builtin_assume_aligned(corrections + k * ch, 16);
    float *const restrict pix_out = __builtin_assume_aligned(out + k * ch, 16);

    // Apply the corrections
    pix_out[0] += corrections_out[0]; // WARNING: hue is an offset
    pix_out[1] *= 1.f + corrections_out[1]; // the brightness and saturation are gains
    pix_out[2] *= exp2f(2.f * corrections_out[2]);

    // Sanitize gamut
    gamut_map_HSB(pix_out, d->gamut_LUT, white);

    // Convert back to XYZ D65
    dt_aligned_pixel_t XYZ_D65 = { 0.f };
    dt_UCS_HSB_to_XYZ(pix_out, white, XYZ_D65);

    // And back to pipe RGB through XYZ D50
    dot_product(XYZ_D65, output_matrix, pix_out);
  }

  dt_free_align(corrections);
  dt_free_align(U);
  dt_free_align(V);
}

static inline float _get_hue_node(const int k)
{
  // Get the angular coordinate of the k-th hue node, including hue offset
  return DEG_TO_RAD(((float)k) * 360.f / ((float)NODES));
}


static inline float _cosine_coeffs(const float l, const float c)
{
  return expf(-l * l / c);
}


static inline void _periodic_RBF_interpolate(float nodes[NODES], const float smoothing, float *const LUT, const gboolean clip)
{
  // Perform a periodic interpolation across hue angles using radial-basis functions
  // see https://eng.aurelienpierre.com/2022/06/interpolating-hue-angles/#Refined-approach
  // for the theory and Python demo

  // Number of terms for the cosine series
  const int m = (int)ceilf(3.f * sqrtf(smoothing));

  float DT_ALIGNED_ARRAY A[NODES][NODES] = { { 0.f } };

  // Build the A matrix with nodes
  for(int i = 0; i < NODES; i++)
    for(int j = 0; j < NODES; j++)
    {
      for(int l = 0; l < m; l++)
      {
        A[i][j] += _cosine_coeffs(l, smoothing) * \
          cosf(((float)l) * fabsf(_get_hue_node(i) - _get_hue_node(j)));
      }
      A[i][j] = expf(A[i][j]);
    }

  // Solve A * x = y for lambdas
  pseudo_solve((float *)A, nodes, NODES, NODES, 0);

  // Interpolate data for all x : generate the LUT
  // WARNING: the LUT spans from [-pi; pi[ for consistency with the output of atan2f()
  for(int i = 0; i < LUT_ELEM; i++)
  {
    // i is directly the hue angle in degree since we sample the LUT every degree.
    // We use un-offset angles here, since thue hue offset is merely a GUI thing,
    // only relevant for user-defined nodes.
    const float hue = (float)i * M_PI_F / 180.f - M_PI_F;
    LUT[i] = 0.f;

    for(int k = 0; k < NODES; k++)
    {
      float result = 0;
      for(int l = 0; l < m; l++)
      {
        result += _cosine_coeffs(l, smoothing) * cosf(((float)l) * fabsf(hue - _get_hue_node(k)));
      }
      LUT[i] += nodes[k] * expf(result);
    }

    if(clip) LUT[i] = fmaxf(0.f, LUT[i]);
  }
}


void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(64, sizeof(dt_iop_colorequal_data_t));
  dt_iop_colorequal_data_t *d = (dt_iop_colorequal_data_t *)piece->data;
  d->LUT_saturation = dt_alloc_align_float(LUT_ELEM);
  d->LUT_hue = dt_alloc_align_float(LUT_ELEM);
  d->LUT_brightness = dt_alloc_align_float(LUT_ELEM);
  d->gamut_LUT = dt_alloc_align_float(LUT_ELEM);
  d->lut_inited = FALSE;
  d->work_profile = NULL;
}


void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorequal_data_t *d = (dt_iop_colorequal_data_t *)piece->data;
  dt_free_align(d->LUT_saturation);
  dt_free_align(d->LUT_hue);
  dt_free_align(d->LUT_brightness);
  dt_free_align(d->gamut_LUT);
  dt_free_align(piece->data);
  piece->data = NULL;
}

static inline void _pack_saturation(struct dt_iop_colorequal_params_t *p, float array[NODES])
{
  array[0] = p->sat_red;
  array[1] = p->sat_orange;
  array[2] = p->sat_lime;
  array[3] = p->sat_green;
  array[4] = p->sat_turquoise;
  array[5] = p->sat_blue;
  array[6] = p->sat_lavender;
  array[7] = p->sat_purple;
}

static inline void _pack_hue(struct dt_iop_colorequal_params_t *p, float array[NODES])
{
  array[0] = p->hue_red;
  array[1] = p->hue_orange;
  array[2] = p->hue_lime;
  array[3] = p->hue_green;
  array[4] = p->hue_turquoise;
  array[5] = p->hue_blue;
  array[6] = p->hue_lavender;
  array[7] = p->hue_purple;

  for(int i = 0; i < NODES; i++) array[i] = array[i] / 180.f * M_PI_F; // Convert to radians
}

static inline void _pack_brightness(struct dt_iop_colorequal_params_t *p, float array[NODES])
{
  array[0] = p->bright_red;
  array[1] = p->bright_orange;
  array[2] = p->bright_lime;
  array[3] = p->bright_green;
  array[4] = p->bright_turquoise;
  array[5] = p->bright_blue;
  array[6] = p->bright_lavender;
  array[7] = p->bright_purple;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorequal_params_t *p = (dt_iop_colorequal_params_t *)p1;
  dt_iop_colorequal_data_t *d = (dt_iop_colorequal_data_t *)piece->data;

  d->white_level = exp2f(p->white_level);
  d->size = p->size;
  d->feathering = p->feathering;
  d->use_filter = p->use_filter;

  float sat_values[NODES];
  float hue_values[NODES];
  float bright_values[NODES];

  _pack_saturation(p, sat_values);
  _periodic_RBF_interpolate(sat_values, 1.f / p->smoothing_saturation * M_PI_F, d->LUT_saturation, TRUE);

  _pack_hue(p, hue_values);
  _periodic_RBF_interpolate(hue_values, 1.f / p->smoothing_hue * M_PI_F, d->LUT_hue, FALSE);

  _pack_brightness(p, bright_values);
  _periodic_RBF_interpolate(bright_values, 1.f / p->smoothing_brightness * M_PI_F, d->LUT_brightness, TRUE);

  // Check if the RGB working profile has changed in pipe
  // WARNING: this function is not triggered upon working profile change,
  // so the gamut boundaries are wrong until we change some param in this module
  struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return;
  if(work_profile != d->work_profile)
  {
    d->lut_inited = FALSE;
    d->work_profile = work_profile;
  }

  // find the maximum chroma allowed by the current working gamut in conjunction to hue
  // this will be used to prevent users to mess up their images by pushing chroma out of gamut
  if(!d->lut_inited)
  {
    dt_colormatrix_t input_matrix;
    dt_colormatrix_mul(input_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
    dt_UCS_22_build_gamut_LUT(input_matrix, d->gamut_LUT);
    d->lut_inited = TRUE;
  }
}


static inline void _build_dt_UCS_HSB_gradients(dt_aligned_pixel_t HSB, dt_aligned_pixel_t RGB,
                                               const struct dt_iop_order_iccprofile_info_t *work_profile,
                                               const float *const gamut_LUT)
{
  // Generate synthetic HSB gradients and convert to display RGB

  // First, gamut-map to ensure the requested HSB color is available in display gamut
  gamut_map_HSB(HSB, gamut_LUT, 1.f);

  // Then, convert to XYZ D65
  dt_aligned_pixel_t XYZ_D65 = { 1.f };
  dt_UCS_HSB_to_XYZ(HSB, 1.f, XYZ_D65);

  if(work_profile)
  {
    dt_ioppr_xyz_to_rgb_matrix(XYZ_D65, RGB, work_profile->matrix_out_transposed, work_profile->lut_out,
                               work_profile->unbounded_coeffs_out, work_profile->lutsize,
                               work_profile->nonlinearlut);
  }
  else
  {
    // Fall back to sRGB output and slow white point conversion
    dt_aligned_pixel_t XYZ_D50;
    XYZ_D65_to_D50(XYZ_D65, XYZ_D50);
    dt_XYZ_to_sRGB(XYZ_D50, RGB);
  }

  for_each_channel(c, aligned(RGB)) RGB[c] = CLAMP(RGB[c], 0.f, 1.f);
}


static inline void _draw_sliders_saturation_gradient(const float sat_min, const float sat_max, const float hue, const float brightness,
                                                     GtkWidget *const slider, const struct dt_iop_order_iccprofile_info_t *work_profile,
                                                     const float *const gamut_LUT)
{
  const float range = sat_max - sat_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float sat = sat_min + stop * range;
    dt_aligned_pixel_t RGB = { 1.f };
    _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue, sat, brightness, 0.f }, RGB, work_profile, gamut_LUT);
    dt_bauhaus_slider_set_stop(slider, stop, RGB[0], RGB[1], RGB[2]);
  }
}

static inline void _draw_sliders_hue_gradient(const float sat, const float hue, const float brightness,
                                              GtkWidget *const slider, const struct dt_iop_order_iccprofile_info_t *work_profile,
                                              const float *const gamut_LUT)
{
  const float hue_min = hue - M_PI_F;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float hue_temp = hue_min + stop * 2.f * M_PI_F;
    dt_aligned_pixel_t RGB = { 1.f };
    _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue_temp, sat, brightness, 0.f }, RGB, work_profile, gamut_LUT);
    dt_bauhaus_slider_set_stop(slider, stop, RGB[0], RGB[1], RGB[2]);
  }
}

static inline void _draw_sliders_brightness_gradient(const float sat, const float hue,
                                                     GtkWidget *const slider, const struct dt_iop_order_iccprofile_info_t *work_profile,
                                                     const float *const gamut_LUT)
{
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1)) * (1.f - 0.001f);
    dt_aligned_pixel_t RGB = { 1.f };
    _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue, sat, stop + 0.001f, 0.f }, RGB, work_profile, gamut_LUT);
    dt_bauhaus_slider_set_stop(slider, stop, RGB[0], RGB[1], RGB[2]);
  }
}


static inline void _init_sliders(dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;

  // Saturation sliders
  for(int k = 0; k < NODES; k++)
  {
    GtkWidget *const slider = g->sat_sliders[k];
    _draw_sliders_saturation_gradient(0.f, g->max_saturation, _get_hue_node(k), SLIDER_BRIGHTNESS, slider, g->white_adapted_profile, g->gamut_LUT);
    dt_bauhaus_slider_set_feedback(slider, 0);
    dt_bauhaus_slider_set_format(slider, " %");
    dt_bauhaus_slider_set_offset(slider, -100.0f);
    dt_bauhaus_slider_set_digits(slider, 2);
    gtk_widget_queue_draw(slider);
  }

  // Hue sliders
  for(int k = 0; k < NODES; k++)
  {
    GtkWidget *const slider = g->hue_sliders[k];
    _draw_sliders_hue_gradient(g->max_saturation, _get_hue_node(k), SLIDER_BRIGHTNESS, slider, g->white_adapted_profile, g->gamut_LUT);
    dt_bauhaus_slider_set_feedback(slider, 0);
    dt_bauhaus_slider_set_format(slider, " °");
    dt_bauhaus_slider_set_digits(slider, 2);
    gtk_widget_queue_draw(slider);
  }

  // Brightness sliders
  for(int k = 0; k < NODES; k++)
  {
    GtkWidget *const slider = g->bright_sliders[k];
    _draw_sliders_brightness_gradient(g->max_saturation, _get_hue_node(k), slider, g->white_adapted_profile, g->gamut_LUT);
    dt_bauhaus_slider_set_feedback(slider, 0);
    dt_bauhaus_slider_set_format(slider, " %");
    dt_bauhaus_slider_set_offset(slider, -100.0f);
    dt_bauhaus_slider_set_digits(slider, 2);
    gtk_widget_queue_draw(slider);
  }
}


static void _init_graph_backgrounds(cairo_pattern_t *gradients[GRAPH_GRADIENTS], dt_iop_colorequal_channel_t channel,
                                    struct dt_iop_order_iccprofile_info_t *work_profile,
                                    const size_t graph_width, const float *const restrict gamut_LUT, const float max_saturation)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(graph_width, channel, work_profile, gamut_LUT, max_saturation) \
  schedule(static) shared(gradients)
#endif
  for(int i = 0; i < GRAPH_GRADIENTS; i++)
  {
    // parallelize the gradients color stop generation
    gradients[i] = cairo_pattern_create_linear(0.0, 0.0, graph_width, 0.0);
    for(int k = 0; k < LUT_ELEM; k++)
    {
      const float x = (float)k / (float)(LUT_ELEM);
      const float y = (float)(GRAPH_GRADIENTS - i) / (float)(GRAPH_GRADIENTS);
      float hue = DEG_TO_RAD((float)k);
      dt_aligned_pixel_t RGB = { 1.f };

      switch(channel)
      {
        case(SATURATION):
        {
          _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue, max_saturation * y, SLIDER_BRIGHTNESS, 1.f }, RGB, work_profile, gamut_LUT);
          break;
        }
        case(HUE):
        {
          _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue + (y - 0.5f) * 2.f * M_PI_F, max_saturation, SLIDER_BRIGHTNESS, 1.f }, RGB, work_profile, gamut_LUT);
          break;
        }
        case(BRIGHTNESS):
        {
          _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue, max_saturation, y, 1.f }, RGB, work_profile, gamut_LUT);
          break;
        }
        default:
        {
          break;
        }
      }
      cairo_pattern_add_color_stop_rgba(gradients[i], x, RGB[0], RGB[1], RGB[2], 1.0);
    }
  }
}


static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_iop_colorequal_params_t *p = (dt_iop_colorequal_params_t *)self->params;

  // Cache the graph objects to avoid recomputing all the view at each redraw
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  cairo_t *cr = cairo_create(cst);
  PangoLayout *layout = pango_cairo_create_layout(cr);

  const gint font_size = pango_font_description_get_size(desc);
  pango_font_description_set_size(desc, 0.95 * font_size);
  pango_layout_set_font_description(layout, desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);

  char text[256];

  // Get the text line height for spacing
  PangoRectangle ink;
  snprintf(text, sizeof(text), "X");
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  const float line_height = ink.height;

  const float inset = DT_PIXEL_APPLY_DPI(4);
  const float margin_top = inset;
  const float margin_bottom = line_height + 2 * inset;
  const float margin_left = 0;
  const float margin_right = 0;

  const float graph_width = allocation.width - margin_right - margin_left;   // align the right border on sliders
  const float graph_height = allocation.height - margin_bottom - margin_top; // give room to nodes

  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);

  // draw x gradient as axis legend
  cairo_pattern_t *grad = cairo_pattern_create_linear(margin_left, 0.0, graph_width, 0.0);
  for(int k = 0; k < LUT_ELEM; k++)
  {
    const float x = (float)k / (float)(LUT_ELEM);
    float hue = DEG_TO_RAD((float)k);
    dt_aligned_pixel_t RGB = { 1.f };
    _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue, g->max_saturation, SLIDER_BRIGHTNESS, 1.f }, RGB, g->white_adapted_profile, g->gamut_LUT);
    cairo_pattern_add_color_stop_rgba(grad, x, RGB[0], RGB[1], RGB[2], 1.0);
  }

  cairo_set_line_width(cr, 0.0);
  cairo_rectangle(cr, margin_left, graph_height + 2 * inset, graph_width, line_height);
  cairo_set_source(cr, grad);
  cairo_fill(cr);
  cairo_pattern_destroy(grad);

  // set the graph as the origin of the coordinates
  cairo_translate(cr, margin_left, margin_top);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  // draw background 2D gradients

  /* This should work and yet it does not.
  * Colors are shifted in hue and in saturation. I suspect some CMS is kicking in and changes the white point.
  * Or the conversion to 8 bits uint is messed up.

  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, graph_width);
  unsigned char *data = malloc(stride * line_height);
  cairo_surface_t *surface = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_RGB24, (size_t)graph_width, (size_t)line_height, stride);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(data, graph_height, graph_width, line_height) \
  schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < (size_t)line_height; i++)
    for(size_t j = 0; j < (size_t)graph_width; j++)
    {
      const size_t k = ((i * (size_t)graph_width) + j) * 4;
      const float x = (float)(graph_width - j - 1) * 360.f / (float)(graph_width - 1);
      //const float y = 1.f - (float)i / (float)(graph_height - 1);
      float hue = DEG_TO_RAD((float)x - 120.f);
      dt_aligned_pixel_t RGB = { 1.f };
      _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue, 0.08f, 0.5f, 1.f }, RGB);
      for(size_t c = 0; c < 3; ++c) data[k + c] = roundf(RGB[c] * 255.f);
    }

  cairo_rectangle(cr, margin_left, graph_height - line_height, graph_width, line_height);
  cairo_set_source_surface(cr, surface, 0, graph_height - line_height);

  cairo_fill(cr);
  free(data);
  cairo_surface_destroy(surface);
  */

  // instead of the above, we simply generate 16 linear horizontal gradients and stack them vertically
  if(!g->gradients_cached)
  {
    // Refresh the cache of gradients
    for(dt_iop_colorequal_channel_t chan = 0; chan < NUM_CHANNELS; chan++)
      _init_graph_backgrounds(g->gradients[chan], chan, g->white_adapted_profile, graph_width, g->gamut_LUT, g->max_saturation);

    g->gradients_cached = TRUE;
  }

  cairo_set_line_width(cr, 0.0);

  for(int i = 0; i < GRAPH_GRADIENTS; i++)
  {
    // cairo painting is not thread-safe, so we need to paint the gradients in sequence
    cairo_rectangle(cr, 0.0, graph_height / (float)GRAPH_GRADIENTS * (float)i, graph_width, graph_height / (float)GRAPH_GRADIENTS);
    cairo_set_source(cr, g->gradients[g->channel][i]);
    cairo_fill(cr);
  }

  cairo_rectangle(cr, 0, 0, graph_width, graph_height);
  cairo_clip(cr);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));
  set_color(cr, darktable.bauhaus->graph_border);
  dt_draw_grid(cr, 8, 0, 0, graph_width, graph_height);

  // draw ground level
  set_color(cr, darktable.bauhaus->graph_fg);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
  cairo_move_to(cr, 0, 0.5 * graph_height);
  cairo_line_to(cr, graph_width, 0.5 * graph_height);
  cairo_stroke(cr);

  GdkRGBA fg_color = darktable.bauhaus->graph_fg;
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  set_color(cr, fg_color);

  // Build the curve LUT and plotting params for the current channel
  g->LUT = dt_alloc_align_float(LUT_ELEM);
  float values[NODES];
  float smoothing;
  float offset;
  float factor;
  gboolean clip;

  switch(g->channel)
  {
    case SATURATION:
    {
      _pack_saturation(p, values);
      smoothing = p->smoothing_saturation;
      clip = TRUE;
      offset = 1.f;
      factor = 0.5f;
      break;
    }
    case HUE:
    {
      _pack_hue(p, values);
      smoothing = p->smoothing_hue;
      clip = FALSE;
      offset = 0.5f;
      factor = 1.f / (2.f * M_PI_F);
      break;
    }
    case BRIGHTNESS:
    default:
    {
      _pack_brightness(p, values);
      smoothing = p->smoothing_brightness;
      clip = TRUE;
      offset = 1.0f;
      factor = 0.5f;
      break;
    }
  }

  _periodic_RBF_interpolate(values, 1.f / smoothing * M_PI_F, g->LUT, clip);

  for(int k = 0; k < LUT_ELEM; k++)
  {
    const float x = (float)k / (float)(LUT_ELEM - 1) * graph_width;
    float hue = DEG_TO_RAD(k);
    hue = (hue < M_PI_F) ? hue : -2.f * M_PI_F + hue; // The LUT is defined in [-pi; pi[
    const float y = (offset - lookup_gamut(g->LUT, hue) * factor) * graph_height;

    if(k == 0)
      cairo_move_to(cr, x, y);
    else
      cairo_line_to(cr, x, y);
  }
  cairo_stroke(cr);

  // draw nodes positions
  for(int k = 0; k < NODES + 1; k++)
  {
    float hue = _get_hue_node(k); // in radians
    const float xn = k / ((float)NODES) * graph_width;
    hue = (hue < M_PI_F) ? hue : -2.f * M_PI_F + hue; // The LUT is defined in [-pi; pi[
    const float yn = (offset - lookup_gamut(g->LUT, hue) * factor) * graph_height;

    // fill bars
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(6));
    set_color(cr, darktable.bauhaus->color_fill);
    cairo_move_to(cr, xn, 0.5 * graph_height);
    cairo_line_to(cr, xn, yn);
    cairo_stroke(cr);

    // bullets
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));
    cairo_arc(cr, xn, yn, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_stroke_preserve(cr);

    /*
    if(g->area_active_node == k)
      set_color(g->cr, darktable.bauhaus->graph_fg);
    else
      */
    set_color(cr, darktable.bauhaus->graph_bg);

    cairo_fill(cr);
  }

  dt_free_align(g->LUT);
  cairo_restore(cr);

  // restore font size
  pango_font_description_set_size(desc, font_size);
  pango_layout_set_font_description(layout, desc);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  g_object_unref(layout);
  pango_font_description_free(desc);
  return TRUE;
}

void pipe_RGB_to_Ych(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const dt_aligned_pixel_t RGB,
                     dt_aligned_pixel_t Ych)
{
  const struct dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return; // no point

  dt_aligned_pixel_t XYZ_D50 = { 0.f };
  dt_aligned_pixel_t XYZ_D65 = { 0.f };

  dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ_D50, work_profile->matrix_in_transposed, work_profile->lut_in,
                             work_profile->unbounded_coeffs_in, work_profile->lutsize,
                             work_profile->nonlinearlut);
  XYZ_D50_to_D65(XYZ_D50, XYZ_D65);
  XYZ_to_Ych(XYZ_D65, Ych);

  if(Ych[2] < 0.f)
    Ych[2] = 2.f * M_PI + Ych[2];
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_iop_colorequal_params_t *p = (dt_iop_colorequal_params_t *)self->params;

  dt_aligned_pixel_t max_Ych = { 0.f };
  pipe_RGB_to_Ych(self, piece, (const float *)self->picked_color_max, max_Ych);

  ++darktable.gui->reset;
  if(picker == g->white_level)
  {
    p->white_level = log2f(max_Ych[0]);
    dt_bauhaus_slider_set(g->white_level, p->white_level);
  }
  else
    fprintf(stderr, "[colorequal] unknown color picker\n");
  --darktable.gui->reset;

  gui_changed(self, picker, NULL);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void _channel_tabs_switch_callback(GtkNotebook *notebook, GtkWidget *page, guint page_num,
                                          dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;

  // The 4th tab is options, in which case we do nothing
  // For the first 3 tabs, update color channel and redraw the graph
  if(page_num < NUM_CHANNELS)
  {
    g->channel = (dt_iop_colorequal_channel_t)page_num;
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;

  // Get the current display profile
  struct dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_output_profile_info(self->dev->pipe);

  // Check if it is different than the one in cache, and update it if needed
  if(work_profile != g->work_profile)
  {
    // Re-init the profiles
    if(g->white_adapted_profile) free(g->white_adapted_profile);
    g->white_adapted_profile = D65_adapt_iccprofile(work_profile);
    g->work_profile = work_profile;
    g->gradients_cached = FALSE;

    // Regenerate the display gamut LUT - Default to Rec709 D65 aka linear sRGB
    dt_colormatrix_t input_matrix = { { 0.4124564f, 0.3575761f, 0.1804375f, 0.f },
                                      { 0.2126729f, 0.7151522f, 0.0721750f, 0.f },
                                      { 0.0193339f, 0.1191920f, 0.9503041f, 0.f },
                                      { 0.f } };
    if(g->white_adapted_profile != NULL)
      memcpy(input_matrix, g->white_adapted_profile->matrix_in, sizeof(dt_colormatrix_t));
    else
      fprintf(stderr, "[colorequal] display color space falls back to sRGB\n");

    dt_UCS_22_build_gamut_LUT(input_matrix, g->gamut_LUT);
    g->max_saturation = get_minimum_saturation(g->gamut_LUT, SLIDER_BRIGHTNESS, 1.f);

    // We need to redraw sliders
    ++darktable.gui->reset;
    _init_sliders(self);
    --darktable.gui->reset;
  }

  ++darktable.gui->reset;
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  --darktable.gui->reset;
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  if(g->white_adapted_profile)
  {
    free(g->white_adapted_profile);
    g->white_adapted_profile = NULL;
  }

  dt_free_align(g->gamut_LUT);

  // Destroy the gradients cache
  for(dt_iop_colorequal_channel_t chan = 0; chan < NUM_CHANNELS; chan++)
    for(int i = 0; i < GRAPH_GRADIENTS; i++)
      cairo_pattern_destroy(g->gradients[chan][i]);

  dt_conf_set_int("plugins/darkroom/colorequal/gui_page", gtk_notebook_get_current_page (g->notebook));

  IOP_GUI_FREE;
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = IOP_GUI_ALLOC(colorequal);

  // Init the color profiles and cache them
  struct dt_iop_order_iccprofile_info_t *work_profile = NULL;
  if(self->dev) work_profile = dt_ioppr_get_pipe_output_profile_info(self->dev->pipe);
  if(g->white_adapted_profile) free(g->white_adapted_profile);
  g->white_adapted_profile = D65_adapt_iccprofile(work_profile);
  g->work_profile = work_profile;
  g->gradients_cached = FALSE;

  // Init the display gamut LUT - Default to Rec709 D65 aka linear sRGB
  g->gamut_LUT = dt_alloc_align_float(LUT_ELEM);
  dt_colormatrix_t input_matrix = { { 0.4124564f, 0.3575761f, 0.1804375f, 0.f },
                                    { 0.2126729f, 0.7151522f, 0.0721750f, 0.f },
                                    { 0.0193339f, 0.1191920f, 0.9503041f, 0.f },
                                    { 0.f } };
  if(g->white_adapted_profile)
    memcpy(input_matrix, g->white_adapted_profile->matrix_in, sizeof(dt_colormatrix_t));

  dt_UCS_22_build_gamut_LUT(input_matrix, g->gamut_LUT);
  g->max_saturation = get_minimum_saturation(g->gamut_LUT, SLIDER_BRIGHTNESS, 1.f);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  const float aspect = 2.f / 3.f;
  //dt_conf_get_int("plugins/darkroom/colorequal/aspect_percent") / 100.0;
  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(aspect));
  g_object_set_data(G_OBJECT(g->area), "iop-instance", self);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->area), TRUE, TRUE, 0);

  // start building top level widget
  static dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);
  g_signal_connect(G_OBJECT(g->notebook), "switch_page", G_CALLBACK(_channel_tabs_switch_callback), self);

  self->widget = dt_ui_notebook_page(g->notebook, N_("saturation"), _("change saturation hue-wise"));

  g->smoothing_saturation = dt_bauhaus_slider_from_params(self, "smoothing_saturation");

  g->sat_sliders[0] = g->sat_red = dt_bauhaus_slider_from_params(self, "sat_red");
  g->sat_sliders[1] = g->sat_orange = dt_bauhaus_slider_from_params(self, "sat_orange");
  g->sat_sliders[2] = g->sat_lime = dt_bauhaus_slider_from_params(self, "sat_lime");
  g->sat_sliders[3] = g->sat_green = dt_bauhaus_slider_from_params(self, "sat_green");
  g->sat_sliders[4] = g->sat_turquoise = dt_bauhaus_slider_from_params(self, "sat_turquoise");
  g->sat_sliders[5] = g->sat_blue = dt_bauhaus_slider_from_params(self, "sat_blue");
  g->sat_sliders[6] = g->sat_lavender = dt_bauhaus_slider_from_params(self, "sat_lavender");
  g->sat_sliders[7] = g->sat_purple = dt_bauhaus_slider_from_params(self, "sat_purple");

  self->widget = dt_ui_notebook_page(g->notebook, N_("hue"), _("change hue hue-wise"));

  g->smoothing_hue = dt_bauhaus_slider_from_params(self, "smoothing_hue");

  g->hue_sliders[0] = g->hue_red = dt_bauhaus_slider_from_params(self, "hue_red");
  g->hue_sliders[1] = g->hue_orange = dt_bauhaus_slider_from_params(self, "hue_orange");
  g->hue_sliders[2] = g->hue_lime = dt_bauhaus_slider_from_params(self, "hue_lime");
  g->hue_sliders[3] = g->hue_green = dt_bauhaus_slider_from_params(self, "hue_green");
  g->hue_sliders[4] = g->hue_turquoise = dt_bauhaus_slider_from_params(self, "hue_turquoise");
  g->hue_sliders[5] = g->hue_blue = dt_bauhaus_slider_from_params(self, "hue_blue");
  g->hue_sliders[6] = g->hue_lavender = dt_bauhaus_slider_from_params(self, "hue_lavender");
  g->hue_sliders[7] = g->hue_purple = dt_bauhaus_slider_from_params(self, "hue_purple");

  self->widget = dt_ui_notebook_page(g->notebook, N_("brightness"), _("change brightness hue-wise"));

  g->smoothing_bright = dt_bauhaus_slider_from_params(self, "smoothing_brightness");

  g->bright_sliders[0] = g->bright_red = dt_bauhaus_slider_from_params(self, "bright_red");
  g->bright_sliders[1] = g->bright_orange = dt_bauhaus_slider_from_params(self, "bright_orange");
  g->bright_sliders[2] = g->bright_lime = dt_bauhaus_slider_from_params(self, "bright_lime");
  g->bright_sliders[3] = g->bright_green = dt_bauhaus_slider_from_params(self, "bright_green");
  g->bright_sliders[4] = g->bright_turquoise = dt_bauhaus_slider_from_params(self, "bright_turquoise");
  g->bright_sliders[5] = g->bright_blue = dt_bauhaus_slider_from_params(self, "bright_blue");
  g->bright_sliders[6] = g->bright_lavender = dt_bauhaus_slider_from_params(self, "bright_lavender");
  g->bright_sliders[7] = g->bright_purple = dt_bauhaus_slider_from_params(self, "bright_purple");

  self->widget = dt_ui_notebook_page(g->notebook, N_("options"), _(""));
  g->white_level = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "white_level"));
  dt_bauhaus_slider_set_soft_range(g->white_level, -2., +2.);
  dt_bauhaus_slider_set_format(g->white_level, _(" EV"));

  g->use_filter = dt_bauhaus_toggle_from_params(self, "use_filter");
  g->size = dt_bauhaus_slider_from_params(self, "size");
  g->feathering = dt_bauhaus_slider_from_params(self, "feathering");

  _init_sliders(self);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->notebook), TRUE, TRUE, 0);

  // restore the previously saved active tab
  const int active_page = dt_conf_get_int("plugins/darkroom/colorequal/gui_page");
  gtk_widget_show(gtk_notebook_get_nth_page(g->notebook, active_page));
  gtk_notebook_set_current_page(g->notebook, active_page);
  g->channel = (active_page == NUM_CHANNELS) ? SATURATION : active_page;

  self->widget = GTK_WIDGET(box);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
