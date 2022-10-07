/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include "common/gaussian.h"
#include "common/fast_guided_filter.h"

#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_cacorrectrgb_params_t)

/**
 * Description of the approach
 *
 ** The problem
 * chromatic aberration appear when:
 * (1) channels are misaligned
 * (2) or if some channel is more blurry than another.
 *
 * example case (1):
 *           _________
 * _________|               first channel
 *             _______
 * ___________|             second channel
 *          ^^ chromatic aberration
 *
 * other example case (1):
 *           _________
 * _________|               first channel
 * ___________
 *            |_______      second channel
 *          ^^ chromatic aberration
 *
 * example case (2):
 *           _________
 *          |               first channel
 * _________|
 *            ________
 *           /              second channel
 *          /
 * ________/
 *         ^^^ chromatic aberration
 *
 * note that case (1) can already be partially corrected using the lens
 * correction module.
 *
 ** Requirements for the solution
 * - handle both cases
 * - preserve borders as much as possible
 * - be fast to compute
 *
 ** The solution
 * The main idea is to represent 2 channels as a function of the third one.
 *
 * a very simple function is: guided = a * guide
 * where a = blur(guided) / blur(guide)
 * But this function is too simple to cope with borders.
 *
 * We stick with the idea of having guided channel as a factor of
 * the guide channel, but instead of having a locally constant factor
 * a, we use a factor that depends on the value of the guide pixel:
 * guided = a(guide) * guide
 *
 * Our function a(guide) is pretty simple, it is a weighted average
 * between 2 values (one high and one low), where the weights are
 * dependent on the guide pixel value.
 *
 * Now, how do we determine these high and low value.
 *
 * We compute 2 manifolds.
 * manifolds are partial local averages:
 * some pixels are not used in the averages.
 *
 * for the lower manifold, we average only pixels whose guide value are below
 * a local average of the guide.
 * for the higher manifold, we average only pixels whose guide value are above
 * a local average of the guide.
 *
 * for example here:
 *           _________
 * _ _ _ _ _| _ _ _ _ _ _ _ average
 * _________|
 *
 * ^^^^^^^^^ pixels below average (will be used to compute lower manifold)
 *
 *           ^^^^^^^^^ pixels above average (will be used to compute higher manifold)
 *
 * As we want to write the guided channel as a ratio of the guide channel,
 * we compute the manifolds on:
 * - the guide channel
 * - log difference between guide and guided
 *
 * using the log difference gives much better result than using directly the
 * guided channel in the manifolds computation and computing the ratio after
 * that, because averaging in linear makes lower manifolds harder to estimate
 * accurately.
 * Note that the repartition of pixels into higher and lower manifold
 * computation is done by taking into account ONLY the guide channel.
 *
 * Once we have our 2 manifolds, with an average log difference for each of them
 * (i.e. an average ratio), we can do a weighted mean to get the result.
 * We weight more one ratio or the other depending to how close the guide pixel
 * is from one manifold or another.
 *
 **/

typedef enum dt_iop_cacorrectrgb_guide_channel_t
{
  DT_CACORRECT_RGB_R = 0,    // $DESCRIPTION: "red"
  DT_CACORRECT_RGB_G = 1,    // $DESCRIPTION: "green"
  DT_CACORRECT_RGB_B = 2     // $DESCRIPTION: "blue"
} dt_iop_cacorrectrgb_guide_channel_t;

typedef enum dt_iop_cacorrectrgb_mode_t
{
  DT_CACORRECT_MODE_STANDARD = 0,  // $DESCRIPTION: "standard"
  DT_CACORRECT_MODE_DARKEN = 1,    // $DESCRIPTION: "darken only"
  DT_CACORRECT_MODE_BRIGHTEN = 2   // $DESCRIPTION: "brighten only"
} dt_iop_cacorrectrgb_mode_t;

typedef struct dt_iop_cacorrectrgb_params_t
{
  dt_iop_cacorrectrgb_guide_channel_t guide_channel; // $DEFAULT: DT_CACORRECT_RGB_G $DESCRIPTION: "guide"
  float radius; // $MIN: 1 $MAX: 500 $DEFAULT: 5 $DESCRIPTION: "radius"
  float strength; // $MIN: 0 $MAX: 4 $DEFAULT: 0.5 $DESCRIPTION: "strength"
  dt_iop_cacorrectrgb_mode_t mode; // $DEFAULT: DT_CACORRECT_MODE_STANDARD $DESCRIPTION: "correction mode"
  gboolean refine_manifolds; // $MIN: FALSE $MAX: TRUE $DEFAULT: FALSE $DESCRIPTION: "very large chromatic aberration"
} dt_iop_cacorrectrgb_params_t;

typedef struct dt_iop_cacorrectrgb_gui_data_t
{
  GtkWidget *guide_channel, *radius, *strength, *mode, *refine_manifolds;
} dt_iop_cacorrectrgb_gui_data_t;

const char *name()
{
  return _("chromatic aberrations");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("correct chromatic aberrations"),
                                      _("corrective"),
                                      _("linear, raw, scene-referred"),
                                      _("linear, raw"),
                                      _("linear, raw, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, p1, self->params_size);
}

static void normalize_manifolds(const float *const restrict blurred_in, float *const restrict blurred_manifold_lower, float *const restrict blurred_manifold_higher, const size_t width, const size_t height, const dt_iop_cacorrectrgb_guide_channel_t guide)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
dt_omp_firstprivate(blurred_in, blurred_manifold_lower, blurred_manifold_higher, width, height, guide) \
  schedule(simd:static)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    const float weighth = fmaxf(blurred_manifold_higher[k * 4 + 3], 1E-2f);
    const float weightl = fmaxf(blurred_manifold_lower[k * 4 + 3], 1E-2f);

    // normalize guide
    const float highg = blurred_manifold_higher[k * 4 + guide] / weighth;
    const float lowg = blurred_manifold_lower[k * 4 + guide] / weightl;

    blurred_manifold_higher[k * 4 + guide] = highg;
    blurred_manifold_lower[k * 4 + guide] = lowg;

    // normalize and unlog other channels
    for(size_t kc = 0; kc <= 1; kc++)
    {
      const size_t c = (kc + guide + 1) % 3;
      const float highc = blurred_manifold_higher[k * 4 + c] / weighth;
      const float lowc = blurred_manifold_lower[k * 4 + c] / weightl;
      blurred_manifold_higher[k * 4 + c] = exp2f(highc) * highg;
      blurred_manifold_lower[k * 4 + c] = exp2f(lowc) * lowg;
    }

    // replace by average if weight is too small
    if(weighth < 0.05f)
    {
      // we make a smooth transition between full manifold at
      // weighth = 0.05f to full average at weighth = 0.01f
      const float w = (weighth - 0.01f) / (0.05f - 0.01f);
      for_each_channel(c,aligned(blurred_manifold_higher,blurred_in))
      {
        blurred_manifold_higher[k * 4 + c] = w * blurred_manifold_higher[k * 4 + c]
                                           + (1.0f - w) * blurred_in[k * 4 + c];
      }
    }
    if(weightl < 0.05f)
    {
      // we make a smooth transition between full manifold at
      // weightl = 0.05f to full average at weightl = 0.01f
      const float w = (weightl - 0.01f) / (0.05f - 0.01f);
      for_each_channel(c,aligned(blurred_manifold_lower,blurred_in))
      {
        blurred_manifold_lower[k * 4 + c] = w * blurred_manifold_lower[k * 4 + c]
                                          + (1.0f - w) * blurred_in[k * 4 + c];
      }
    }
  }
}

#define DT_CACORRECTRGB_MAX_EV_DIFF 2.0f
static void get_manifolds(const float* const restrict in, const size_t width, const size_t height,
                          const float sigma, const float sigma2,
                          const dt_iop_cacorrectrgb_guide_channel_t guide,
                          float* const restrict manifolds, gboolean refine_manifolds)
{
  float *const restrict blurred_in = dt_alloc_align_float(width * height * 4);
  float *const restrict manifold_higher = dt_alloc_align_float(width * height * 4);
  float *const restrict manifold_lower = dt_alloc_align_float(width * height * 4);
  float *const restrict blurred_manifold_higher = dt_alloc_align_float(width * height * 4);
  float *const restrict blurred_manifold_lower = dt_alloc_align_float(width * height * 4);

  dt_aligned_pixel_t max = {INFINITY, INFINITY, INFINITY, INFINITY};
  dt_aligned_pixel_t min = {-INFINITY, -INFINITY, -INFINITY, 0.0f};
  // start with a larger blur to estimate the manifolds if we refine them
  // later on
  const float blur_size = refine_manifolds ? sigma2 : sigma;
  dt_gaussian_t *g = dt_gaussian_init(width, height, 4, max, min, blur_size, 0);
  if(!g) return;
  dt_gaussian_blur_4c(g, in, blurred_in);

  // construct the manifolds
  // higher manifold is the blur of all pixels that are above average,
  // lower manifold is the blur of all pixels that are below average
  // we use the guide channel to categorize the pixels as above or below average
#ifdef _OPENMP
#pragma omp parallel for default(none) \
dt_omp_firstprivate(in, blurred_in, manifold_lower, manifold_higher, width, height, guide) \
  schedule(simd:static)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    const float pixelg = fmaxf(in[k * 4 + guide], 1E-6f);
    const float avg = blurred_in[k * 4 + guide];
    float weighth = (pixelg >= avg);
    float weightl = (pixelg <= avg);
    float logdiffs[2];
    for(size_t kc = 0; kc <= 1; kc++)
    {
      const size_t c = (kc + guide + 1) % 3;
      const float pixel = fmaxf(in[k * 4 + c], 1E-6f);
      const float log_diff = log2f(pixel / pixelg);
      logdiffs[kc] = log_diff;
    }
    // regularization of logdiff to avoid too many problems with noise:
    // we lower the weights of pixels with too high logdiff
    const float maxlogdiff = fmaxf(fabsf(logdiffs[0]), fabsf(logdiffs[1]));
    if(maxlogdiff > DT_CACORRECTRGB_MAX_EV_DIFF)
    {
      const float correction_weight = DT_CACORRECTRGB_MAX_EV_DIFF / maxlogdiff;
      weightl *= correction_weight;
      weighth *= correction_weight;
    }
    for(size_t kc = 0; kc <= 1; kc++)
    {
      const size_t c = (kc + guide + 1) % 3;
      manifold_higher[k * 4 + c] = logdiffs[kc] * weighth;
      manifold_lower[k * 4 + c] = logdiffs[kc] * weightl;
    }
    manifold_higher[k * 4 + guide] = pixelg * weighth;
    manifold_lower[k * 4 + guide] = pixelg * weightl;
    manifold_higher[k * 4 + 3] = weighth;
    manifold_lower[k * 4 + 3] = weightl;
  }

  dt_gaussian_blur_4c(g, manifold_higher, blurred_manifold_higher);
  dt_gaussian_blur_4c(g, manifold_lower, blurred_manifold_lower);
  dt_gaussian_free(g);

  normalize_manifolds(blurred_in, blurred_manifold_lower, blurred_manifold_higher, width, height, guide);

  // note that manifolds were constructed based on the value and average
  // of the guide channel ONLY.
  // this implies that the "higher" manifold in the channel c may be
  // actually lower than the "lower" manifold of that channel.
  // This happens in the following example:
  // guide:  1_____
  //               |_____0
  // guided:        _____1
  //         0_____|
  // here the higher manifold of guide is equal to 1, its lower manifold is
  // equal to 0. The higher manifold of the guided channel is equal to 0
  // as it is the average of the values where the guide is higher than its
  // average, and the lower manifold of the guided channel is equal to 1.

  if(refine_manifolds)
  {
    g = dt_gaussian_init(width, height, 4, max, min, sigma, 0);
    if(!g) return;
    dt_gaussian_blur_4c(g, in, blurred_in);

    // refine the manifolds
    // improve result especially on very degraded images
    // we use a blur of normal size for this step
  #ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, blurred_in, manifold_lower, manifold_higher, blurred_manifold_lower, blurred_manifold_higher, width, height, guide) \
    schedule(simd:static)
  #endif
    for(size_t k = 0; k < width * height; k++)
    {
      // in order to refine the manifolds, we will compute weights
      // for which all channels will have a contribution.
      // this will allow to avoid taking too much into account pixels
      // that have wrong values due to the chromatic aberration
      //
      // for example, here:
      // guide:  1_____
      //               |_____0
      // guided: 1______
      //                |____0
      //               ^ this pixel makes the estimated lower manifold erroneous
      // here, the higher and lower manifolds values computed are:
      // _______|_higher_|________lower_________|
      // guide  |    1   |   0                  |
      // guided |    1   |(1 + 4 * 0) / 5 = 0.2 |
      //
      // the lower manifold of the guided is 0.2 if we consider only the guide
      //
      // at this step of the algorithm, we know estimates of manifolds
      //
      // we can refine the manifolds by computing weights that reduce the influence
      // of pixels that are probably suffering from chromatic aberrations
      const float pixelg = log2f(fmaxf(in[k * 4 + guide], 1E-6f));
      const float highg = log2f(fmaxf(blurred_manifold_higher[k * 4 + guide], 1E-6f));
      const float lowg = log2f(fmaxf(blurred_manifold_lower[k * 4 + guide], 1E-6f));
      const float avgg = log2f(fmaxf(blurred_in[k * 4 + guide], 1E-6f));

      float w = 1.0f;
      for(size_t kc = 0; kc <= 1; kc++)
      {
        const size_t c = (guide + kc + 1) % 3;
        // weight by considering how close pixel is for a manifold,
        // and how close the log difference between the channels is
        // close to the wrong log difference between the channels.

        const float pixel = log2f(fmaxf(in[k * 4 + c], 1E-6f));
        const float highc = log2f(fmaxf(blurred_manifold_higher[k * 4 + c], 1E-6f));
        const float lowc = log2f(fmaxf(blurred_manifold_lower[k * 4 + c], 1E-6f));

        // find how likely the pixel is part of a chromatic aberration
        // (lowc, lowg) and (highc, highg) are valid points
        // (lowc, highg) and (highc, lowg) are chromatic aberrations
        const float dist_to_ll = fabsf(pixelg - lowg - pixel + lowc);
        const float dist_to_hh = fabsf(pixelg - highg - pixel + highc);
        const float dist_to_lh = fabsf((pixelg - pixel) - (highg - lowc));
        const float dist_to_hl = fabsf((pixelg - pixel) - (lowg - highc));

        float dist_to_good = 1.0f;
        if(fabsf(pixelg - lowg) < fabsf(pixelg - highg))
          dist_to_good = dist_to_ll;
        else
          dist_to_good = dist_to_hh;

        float dist_to_bad = 1.0f;
        if(fabsf(pixelg - lowg) < fabsf(pixelg - highg))
          dist_to_bad = dist_to_hl;
        else
          dist_to_bad = dist_to_lh;

        // make w higher if close to good, and smaller if close to bad.
        w *= 1.0f * (0.2f + 1.0f / fmaxf(dist_to_good, 0.1f)) / (0.2f + 1.0f / fmaxf(dist_to_bad, 0.1f));
      }

      if(pixelg > avgg)
      {
        float logdiffs[2];
        for(size_t kc = 0; kc <= 1; kc++)
        {
          const size_t c = (guide + kc + 1) % 3;
          const float pixel = fmaxf(in[k * 4 + c], 1E-6f);
          const float log_diff = log2f(pixel) - pixelg;
          logdiffs[kc] = log_diff;
        }
        // regularization of logdiff to avoid too many problems with noise:
        // we lower the weights of pixels with too high logdiff
        const float maxlogdiff = fmaxf(fabsf(logdiffs[0]), fabsf(logdiffs[1]));
        if(maxlogdiff > DT_CACORRECTRGB_MAX_EV_DIFF)
        {
          const float correction_weight = DT_CACORRECTRGB_MAX_EV_DIFF / maxlogdiff;
          w *= correction_weight;
        }
        for(size_t kc = 0; kc <= 1; kc++)
        {
          const size_t c = (kc + guide + 1) % 3;
          manifold_higher[k * 4 + c] = logdiffs[kc] * w;
        }
        manifold_higher[k * 4 + guide] = fmaxf(in[k * 4 + guide], 0.0f) * w;
        manifold_higher[k * 4 + 3] = w;
        // manifold_lower still contains the values from first iteration
        // -> reset it.
        for_four_channels(c)
        {
          manifold_lower[k * 4 + c] = 0.0f;
        }
      }
      else
      {
        float logdiffs[2];
        for(size_t kc = 0; kc <= 1; kc++)
        {
          const size_t c = (guide + kc + 1) % 3;
          const float pixel = fmaxf(in[k * 4 + c], 1E-6f);
          const float log_diff = log2f(pixel) - pixelg;
          logdiffs[kc] = log_diff;
        }
        // regularization of logdiff to avoid too many problems with noise:
        // we lower the weights of pixels with too high logdiff
        const float maxlogdiff = fmaxf(fabsf(logdiffs[0]), fabsf(logdiffs[1]));
        if(maxlogdiff > DT_CACORRECTRGB_MAX_EV_DIFF)
        {
          const float correction_weight = DT_CACORRECTRGB_MAX_EV_DIFF / maxlogdiff;
          w *= correction_weight;
        }
        for(size_t kc = 0; kc <= 1; kc++)
        {
          const size_t c = (kc + guide + 1) % 3;
          manifold_lower[k * 4 + c] = logdiffs[kc] * w;
        }
        manifold_lower[k * 4 + guide] = fmaxf(in[k * 4 + guide], 0.0f) * w;
        manifold_lower[k * 4 + 3] = w;
        // manifold_higher still contains the values from first iteration
        // -> reset it.
        for(size_t c = 0; c < 4; c++)
        {
          manifold_higher[k * 4 + c] = 0.0f;
        }
      }
    }

    dt_gaussian_blur_4c(g, manifold_higher, blurred_manifold_higher);
    dt_gaussian_blur_4c(g, manifold_lower, blurred_manifold_lower);
    normalize_manifolds(blurred_in, blurred_manifold_lower, blurred_manifold_higher, width, height, guide);
    dt_gaussian_free(g);
  }

  dt_free_align(manifold_lower);
  dt_free_align(manifold_higher);

  // store all manifolds in the same structure to make upscaling faster
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(manifolds, blurred_manifold_lower, blurred_manifold_higher, width, height, guide) \
  schedule(simd:static) aligned(manifolds, blurred_manifold_lower, blurred_manifold_higher:64)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    for(size_t c = 0; c < 3; c++)
    {
      manifolds[k * 6 + c] = blurred_manifold_higher[k * 4 + c];
      manifolds[k * 6 + 3 + c] = blurred_manifold_lower[k * 4 + c];
    }
  }
  dt_free_align(blurred_in);
  dt_free_align(blurred_manifold_lower);
  dt_free_align(blurred_manifold_higher);
}
#undef DT_CACORRECTRGB_MAX_EV_DIFF

static void apply_correction(const float* const restrict in,
                          const float* const restrict manifolds,
                          const size_t width, const size_t height, const float sigma,
                          const dt_iop_cacorrectrgb_guide_channel_t guide,
                          const dt_iop_cacorrectrgb_mode_t mode,
                          float* const restrict out)

{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
dt_omp_firstprivate(in, width, height, guide, manifolds, out, sigma, mode) \
  schedule(simd:static)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    const float high_guide = fmaxf(manifolds[k * 6 + guide], 1E-6f);
    const float low_guide = fmaxf(manifolds[k * 6 + 3 + guide], 1E-6f);
    const float log_high = log2f(high_guide);
    const float log_low = log2f(low_guide);
    const float dist_low_high = log_high - log_low;
    const float pixelg = fmaxf(in[k * 4 + guide], 0.0f);
    const float log_pixg = log2f(fminf(fmaxf(pixelg, low_guide), high_guide));

    // determine how close our pixel is from the low manifold compared to the
    // high manifold.
    // if pixel value is lower or equal to the low manifold, weight_low = 1.0f
    // if pixel value is higher or equal to the high manifold, weight_low = 0.0f
    float weight_low = fabsf(log_high - log_pixg) / fmaxf(dist_low_high, 1E-6f);
    // if the manifolds are very close, we are likely to introduce discontinuities
    // and to have a meaningless "weight_low".
    // thus in these cases make dist closer to 0.5.
    // we set a threshold of 0.25f EV min.
    const float threshold_dist_low_high = 0.25f;
    if(dist_low_high < threshold_dist_low_high)
    {
      const float weight = dist_low_high / threshold_dist_low_high;
      // dist_low_high = threshold_dist_low_high => dist
      // dist_low_high = 0.0 => 0.5f
      weight_low = weight_low * weight + 0.5f * (1.0f - weight);
    }
    const float weight_high = fmaxf(1.0f - weight_low, 0.0f);

    for(size_t kc = 0; kc <= 1; kc++)
    {
      const size_t c = (guide + kc + 1) % 3;
      const float pixelc = fmaxf(in[k * 4 + c], 0.0f);

      const float ratio_high_manifolds = manifolds[k * 6 + c] / high_guide;
      const float ratio_low_manifolds = manifolds[k * 6 + 3 + c] / low_guide;
      // weighted geometric mean between the ratios.
      const float ratio = powf(ratio_low_manifolds, weight_low) * powf(ratio_high_manifolds, weight_high);

      const float outp = pixelg * ratio;

      switch(mode)
      {
        case DT_CACORRECT_MODE_STANDARD:
          out[k * 4 + c] = outp;
          break;
        case DT_CACORRECT_MODE_DARKEN:
          out[k * 4 + c] = fminf(outp, pixelc);
          break;
        case DT_CACORRECT_MODE_BRIGHTEN:
          out[k * 4 + c] = fmaxf(outp, pixelc);
          break;
      }
    }

    out[k * 4 + guide] = pixelg;
    out[k * 4 + 3] = in[k * 4 + 3];
  }
}

static void reduce_artifacts(const float* const restrict in,
                          const size_t width, const size_t height, const float sigma,
                          const dt_iop_cacorrectrgb_guide_channel_t guide,
                          const float safety,
                          float* const restrict out)

{
  // in_out contains the 2 guided channels of in, and the 2 guided channels of out
  // it allows to blur all channels in one 4-channel gaussian blur instead of 2
  float *const restrict DT_ALIGNED_PIXEL in_out = dt_alloc_align_float(width * height * 4);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, out, in_out, width, height, guide)        \
  schedule(simd:static)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    for(size_t kc = 0; kc <= 1; kc++)
    {
      const size_t c = (guide + kc + 1) % 3;
      in_out[k * 4 + kc * 2 + 0] = in[k * 4 + c];
      in_out[k * 4 + kc * 2 + 1] = out[k * 4 + c];
    }
  }

  float *const restrict blurred_in_out = dt_alloc_align_float(width * height * 4);
  dt_aligned_pixel_t max = {INFINITY, INFINITY, INFINITY, INFINITY};
  dt_aligned_pixel_t min = {0.0f, 0.0f, 0.0f, 0.0f};
  dt_gaussian_t *g = dt_gaussian_init(width, height, 4, max, min, sigma, 0);
  if(!g) return;
  dt_gaussian_blur_4c(g, in_out, blurred_in_out);
  dt_gaussian_free(g);
  dt_free_align(in_out);

  // we consider that even with chromatic aberration, local average should
  // be close to be accurate.
  // thus, the local average of output should be similar to the one of the input
  // if they are not, the algorithm probably washed out colors too much or
  // may have produced artifacts.
  // we do a weighted average between input and output, keeping more input if
  // the local averages are very different.
  // we use the same weight for all channels, as using different weights
  // introduces artifacts in practice.
#ifdef _OPENMP
#pragma omp parallel for default(none) \
dt_omp_firstprivate(in, out, blurred_in_out, width, height, guide, safety) \
  schedule(simd:static)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    float w = 1.0f;
    for(size_t kc = 0; kc <= 1; kc++)
    {
      const float avg_in = log2f(fmaxf(blurred_in_out[k * 4 + kc * 2 + 0], 1E-6f));
      const float avg_out = log2f(fmaxf(blurred_in_out[k * 4 + kc * 2 + 1], 1E-6f));
      w *= expf(-fmaxf(fabsf(avg_out - avg_in), 0.01f) * safety);
    }
    for(size_t kc = 0; kc <= 1; kc++)
    {
      const size_t c = (guide + kc + 1) % 3;
      out[k * 4 + c] = fmaxf(1.0f - w, 0.0f) * fmaxf(in[k * 4 + c], 0.0f) + w * fmaxf(out[k * 4 + c], 0.0f);
    }
  }
  dt_free_align(blurred_in_out);
}

static void reduce_chromatic_aberrations(const float* const restrict in,
                          const size_t width, const size_t height,
                          const size_t ch, const float sigma, const float sigma2,
                          const dt_iop_cacorrectrgb_guide_channel_t guide,
                          const dt_iop_cacorrectrgb_mode_t mode,
                          const gboolean refine_manifolds,
                          const float safety,
                          float* const restrict out)

{
  const float downsize = fminf(3.0f, sigma);
  const size_t ds_width = width / downsize;
  const size_t ds_height = height / downsize;
  float *const restrict ds_in = dt_alloc_align_float(ds_width * ds_height * 4);
  // we use only one variable for both higher and lower manifolds in order
  // to save time by doing only one bilinear interpolation instead of 2.
  float *const restrict ds_manifolds = dt_alloc_align_float(ds_width * ds_height * 6);
  // Downsample the image for speed-up
  interpolate_bilinear(in, width, height, ds_in, ds_width, ds_height, 4);

  // Compute manifolds
  get_manifolds(ds_in, ds_width, ds_height, sigma / downsize, sigma2 / downsize, guide, ds_manifolds, refine_manifolds);
  dt_free_align(ds_in);

  // upscale manifolds
  float *const restrict manifolds = dt_alloc_align_float(width * height * 6);
  interpolate_bilinear(ds_manifolds, ds_width, ds_height, manifolds, width, height, 6);
  dt_free_align(ds_manifolds);

  apply_correction(in, manifolds, width, height, sigma, guide, mode, out);
  dt_free_align(manifolds);

  reduce_artifacts(in, width, height, sigma, guide, safety, out);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return; // ivoid has been copied to ovoid and the module's trouble flag has been set

  dt_iop_cacorrectrgb_params_t *d = (dt_iop_cacorrectrgb_params_t *)piece->data;
  // used to adjuste blur level depending on size. Don't amplify noise if magnified > 100%
  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const int ch = piece->colors;
  const size_t width = roi_out->width;
  const size_t height = roi_out->height;
  const float* in = (float*)ivoid;
  float* out = (float*)ovoid;
  const float sigma = fmaxf(d->radius / scale, 1.0f);
  const float sigma2 = fmaxf(d->radius * d->radius / scale, 1.0f);

  // whether to be very conservative in preserving the original image, or to
  // keep algorithm result even if it overshoots
  const float safety = powf(20.0f, 1.0f - d->strength);
  reduce_chromatic_aberrations(in, width, height, ch, sigma, sigma2, d->guide_channel, d->mode, d->refine_manifolds, safety, out);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_cacorrectrgb_gui_data_t *g = (dt_iop_cacorrectrgb_gui_data_t *)self->gui_data;
  dt_iop_cacorrectrgb_params_t *p = (dt_iop_cacorrectrgb_params_t *)self->params;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->refine_manifolds), p->refine_manifolds);
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_cacorrectrgb_params_t *d = (dt_iop_cacorrectrgb_params_t *)module->default_params;

  d->guide_channel = DT_CACORRECT_RGB_G;
  d->radius = 5.0f;
  d->strength = 0.5f;
  d->mode = DT_CACORRECT_MODE_STANDARD;
  d->refine_manifolds = FALSE;

  dt_iop_cacorrectrgb_gui_data_t *g = (dt_iop_cacorrectrgb_gui_data_t *)module->gui_data;
  if(g)
  {
    dt_bauhaus_combobox_set_default(g->guide_channel, d->guide_channel);
    dt_bauhaus_slider_set_default(g->radius, d->radius);
    dt_bauhaus_slider_set_soft_range(g->radius, 1.0, 20.0);
    dt_bauhaus_slider_set_default(g->strength, d->strength);
    dt_bauhaus_combobox_set_default(g->mode, d->mode);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->refine_manifolds), d->refine_manifolds);
  }
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_cacorrectrgb_gui_data_t *g = IOP_GUI_ALLOC(cacorrectrgb);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  g->guide_channel = dt_bauhaus_combobox_from_params(self, "guide_channel");
  gtk_widget_set_tooltip_text(g->guide_channel, _("channel used as a reference to\n"
                                           "correct the other channels.\n"
                                           "use sharpest channel if some\n"
                                           "channels are blurry.\n"
                                           "try changing guide channel if you\n"
                                           "have artifacts."));
  g->radius = dt_bauhaus_slider_from_params(self, "radius");
  gtk_widget_set_tooltip_text(g->radius, _("increase for stronger correction"));
  g->strength = dt_bauhaus_slider_from_params(self, "strength");
  gtk_widget_set_tooltip_text(g->strength, _("balance between smoothing colors\n"
                                             "and preserving them.\n"
                                             "high values can lead to overshooting\n"
                                             "and edge bleeding."));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("advanced parameters")), TRUE, TRUE, 0);
  g->mode = dt_bauhaus_combobox_from_params(self, "mode");
  gtk_widget_set_tooltip_text(g->mode, _("correction mode to use.\n"
                                         "can help with multiple\n"
                                         "instances for very damaged\n"
                                         "images.\n"
                                         "darken only is particularly\n"
                                         "efficient to correct blue\n"
                                         "chromatic aberration."));
  g->refine_manifolds = dt_bauhaus_toggle_from_params(self, "refine_manifolds");
  gtk_widget_set_tooltip_text(g->refine_manifolds, _("runs an iterative approach\n"
                                                    "with several radii.\n"
                                                    "improves result on images\n"
                                                    "with very large chromatic\n"
                                                    "aberrations, but can smooth\n"
                                                    "colors too much on other\n"
                                                    "images."));
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

