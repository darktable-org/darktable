/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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
// our includes go first:
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
} dt_iop_cacorrectrgb_params_t;

typedef struct dt_iop_cacorrectrgb_gui_data_t
{
  GtkWidget *guide_channel, *radius, *strength, *mode;
} dt_iop_cacorrectrgb_gui_data_t;

const char *name()
{
  return _("chromatic aberrations rgb");
}

const char *description(struct dt_iop_module_t *self)
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
  return iop_cs_rgb;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, p1, self->params_size);
}

static void normalize_manifolds(const float *const restrict blurred_in, float *const restrict blurred_manifold_lower, float *const restrict blurred_manifold_higher, const size_t width, const size_t height, const dt_iop_cacorrectrgb_guide_channel_t guide)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(blurred_in, blurred_manifold_lower, blurred_manifold_higher, width, height, guide) \
  schedule(simd:static) aligned(blurred_in, blurred_manifold_lower, blurred_manifold_higher:64)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    const float weighth = fmaxf(blurred_manifold_higher[k * 4 + 3], 1E-2);
    const float weightl = fmaxf(blurred_manifold_lower[k * 4 + 3], 1E-2);

    // normalize guide
    float highg = blurred_manifold_higher[k * 4 + guide] / weighth;
    float lowg = blurred_manifold_lower[k * 4 + guide] / weightl;

    blurred_manifold_higher[k * 4 + guide] = highg;
    blurred_manifold_lower[k * 4 + guide] = lowg;

    // normalize and unlog other channels
    for(size_t kc = 0; kc <= 1; kc++)
    {
      size_t c = (kc + guide + 1) % 3;
      float highc = blurred_manifold_higher[k * 4 + c] / weighth;
      float lowc = blurred_manifold_lower[k * 4 + c] / weightl;
      blurred_manifold_higher[k * 4 + c] = exp2f(highc) * highg;
      blurred_manifold_lower[k * 4 + c] = exp2f(lowc) * lowg;
    }

    // replace by average if weight is too small
    if(weighth < 0.05f)
    {
      // we make a smooth transition between full manifold at
      // weighth = 0.05f to full average at weighth = 0.01f
      const float w = (weighth - 0.01f) / (0.05f - 0.01f);
      for(size_t c = 0; c < 3; c++)
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
      for(size_t c = 0; c < 3; c++)
      {
        blurred_manifold_lower[k * 4 + c] = w * blurred_manifold_lower[k * 4 + c]
                                          + (1.0f - w) * blurred_in[k * 4 + c];
      }
    }
  }
}

static void get_manifolds(const float* const restrict in, const size_t width, const size_t height,
                          const size_t ch, const float sigma,
                          const dt_iop_cacorrectrgb_guide_channel_t guide,
                          float* const restrict manifolds)
{
  float *const restrict blurred_in = dt_alloc_sse_ps(dt_round_size_sse(width * height * ch));
  float *const restrict manifold_higher = dt_alloc_sse_ps(dt_round_size_sse(width * height * ch));
  float *const restrict manifold_lower = dt_alloc_sse_ps(dt_round_size_sse(width * height * ch));
  float *const restrict blurred_manifold_higher = dt_alloc_sse_ps(dt_round_size_sse(width * height * ch));
  float *const restrict blurred_manifold_lower = dt_alloc_sse_ps(dt_round_size_sse(width * height * ch));

  float max[4] = {INFINITY, INFINITY, INFINITY, INFINITY};
  float min[4] = {-INFINITY, -INFINITY, -INFINITY, 0.0f};
  dt_gaussian_t *g = dt_gaussian_init(width, height, 4, max, min, sigma, 0);
  if(!g) return;
  dt_gaussian_blur_4c(g, in, blurred_in);

  // construct the manifolds
  // higher manifold is the blur of all pixels that are above average,
  // lower manifold is the blur of all pixels that are below average
  // we use the guide channel to categorize the pixels as above or below average
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(in, blurred_in, manifold_lower, manifold_higher, width, height, guide) \
  schedule(simd:static) aligned(in, blurred_in, manifold_lower, manifold_higher:64)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    const float pixelg = fmaxf(in[k * 4 + guide], 1E-6);
    const float avg = blurred_in[k * 4 + guide];
    const float weighth = (pixelg >= avg);
    const float weightl = (pixelg <= avg);
    for(size_t kc = 0; kc <= 1; kc++)
    {
      size_t c = (kc + guide + 1) % 3;
      const float pixel = fmaxf(in[k * 4 + c], 1E-6);
      const float log_diff = log2f(pixel / pixelg);
      manifold_higher[k * 4 + c] = log_diff * weighth;
      manifold_lower[k * 4 + c] = log_diff * weightl;
    }
    manifold_higher[k * 4 + guide] = pixelg * weighth;
    manifold_lower[k * 4 + guide] = pixelg * weightl;
    manifold_higher[k * 4 + 3] = weighth;
    manifold_lower[k * 4 + 3] = weightl;
  }

  dt_gaussian_blur_4c(g, manifold_higher, blurred_manifold_higher);
  dt_gaussian_blur_4c(g, manifold_lower, blurred_manifold_lower);

  normalize_manifolds(blurred_in, blurred_manifold_lower, blurred_manifold_higher, width, height, guide);
  dt_gaussian_free(g);
  dt_free_align(manifold_lower);
  dt_free_align(manifold_higher);

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

static void apply_correction(const float* const restrict in,
                          const float* const restrict manifolds,
                          const size_t width, const size_t height,
                          const size_t ch, const float sigma,
                          const dt_iop_cacorrectrgb_guide_channel_t guide,
                          const dt_iop_cacorrectrgb_mode_t mode,
                          float* const restrict out)

{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(in, width, height, guide, manifolds, out, sigma, mode) \
  schedule(simd:static) aligned(in, manifolds, out)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    const float high_guide = fmaxf(manifolds[k * 6 + guide], 1E-6);
    const float low_guide = fmaxf(manifolds[k * 6 + 3 + guide], 1E-6);
    const float log_high = log2f(high_guide);
    const float log_low = log2f(low_guide);
    const float pixelg = fmaxf(in[k * 4 + guide], 0.0f);
    const float log_pixg = log2f(fminf(fmaxf(pixelg, low_guide), high_guide));
    float dist = fabsf(log_high - log_pixg) / fmaxf(fabsf(log_high - log_low), 1E-6);

    // if the manifolds are very close, we are likely to introduce discontinuities
    // and to have a meaningless "dist".
    // thus in these cases make dist closer to 0.5.
    // we set a threshold of 0.25f EV min.
    const float dist_low_high = (log_high - log_low) * 4.0f;
    if(dist_low_high < 1.0f)
    {
      dist = dist * dist_low_high + 0.5f * (1.0f - dist_low_high);
    }

    for(size_t kc = 0; kc <= 1; kc++)
    {
      const size_t c = (guide + kc + 1) % 3;
      const float pixelc = fmaxf(in[k * 4 + c], 0.0f);

      const float ratio_high_manifolds = manifolds[k * 6 + c] / high_guide;
      const float ratio_low_manifolds = manifolds[k * 6 + 3 + c] / low_guide;
      const float ratio = powf(ratio_low_manifolds, dist) * powf(ratio_high_manifolds, fmaxf(1.0f - dist, 0.0f));

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
                          const size_t width, const size_t height,
                          const size_t ch, const float sigma,
                          const dt_iop_cacorrectrgb_guide_channel_t guide,
                          const float safety,
                          float* const restrict out)

{
  // in_out contains 2 guided channels of in, and of out
  float *const restrict in_out = dt_alloc_sse_ps(dt_round_size_sse(width * height * ch));
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(in, out, in_out, width, height, guide, ch) \
  schedule(simd:static) aligned(in, out, in_out:64)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    for(size_t kc = 0; kc <= 1; kc++)
    {
      size_t c = (guide + kc + 1) % 3;
      in_out[k * ch + kc * 2 + 0] = in[k * 4 + c];
      in_out[k * ch + kc * 2 + 1] = out[k * 4 + c];
    }
  }

  float *const restrict blurred_in_out = dt_alloc_sse_ps(dt_round_size_sse(width * height * ch));
  float max[4] = {INFINITY, INFINITY, INFINITY, INFINITY};
  float min[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  dt_gaussian_t *g = dt_gaussian_init(width, height, 4, max, min, sigma, 0);
  if(!g) return;
  dt_gaussian_blur_4c(g, in_out, blurred_in_out);
  dt_gaussian_free(g);
  dt_free_align(in_out);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(in, out, blurred_in_out, width, height, guide, safety, ch) \
  schedule(simd:static) aligned(in, out, blurred_in_out:64)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    float w = 1.0f;
    for(size_t kc = 0; kc <= 1; kc++)
    {
      const float avg_in = log2f(fmaxf(blurred_in_out[k * ch + kc * 2 + 0], 1E-6));
      const float avg_out = log2f(fmaxf(blurred_in_out[k * ch + kc * 2 + 1], 1E-6));
      w *= expf(-fmaxf(fabsf(avg_out - avg_in), 0.01f) * safety);
    }
    for(size_t kc = 0; kc <= 1; kc++)
    {
      size_t c = (guide + kc + 1) % 3;
      out[k * ch + c] = fmaxf(1.0f - w, 0.0f) * fmaxf(in[k * ch + c], 0.0f) + w * fmaxf(out[k * ch + c], 0.0f);
    }
  }
  dt_free_align(blurred_in_out);
}

static void reduce_chromatic_aberrations(const float* const restrict in,
                          const size_t width, const size_t height,
                          const size_t ch, const float sigma,
                          const dt_iop_cacorrectrgb_guide_channel_t guide,
                          const dt_iop_cacorrectrgb_mode_t mode,
                          const float safety,
                          float* const restrict out)

{
  const float downsize = 3.0f;
  const size_t ds_width = width / downsize;
  const size_t ds_height = height / downsize;
  float *const restrict ds_in = dt_alloc_sse_ps(dt_round_size_sse(ds_width * ds_height * ch));
  // we use only one variable for both higher and lower manifolds in order
  // to save time by doing only one bilinear interpolation instead of 2.
  float *const restrict ds_manifolds = dt_alloc_sse_ps(dt_round_size_sse(ds_width * ds_height * 6));
  // Downsample the image for speed-up
  interpolate_bilinear(in, width, height, ds_in, ds_width, ds_height, 4);

  // Compute manifolds
  get_manifolds(ds_in, ds_width, ds_height, ch, sigma / downsize, guide, ds_manifolds);
  dt_free_align(ds_in);

  // upscale manifolds
  float *const restrict manifolds = dt_alloc_sse_ps(dt_round_size_sse(width * height * 6));
  interpolate_bilinear(ds_manifolds, ds_width, ds_height, manifolds, width, height, 6);
  dt_free_align(ds_manifolds);

  apply_correction(in, manifolds, width, height, ch, sigma, guide, mode, out);
  dt_free_align(manifolds);

  reduce_artifacts(in, width, height, ch, sigma, guide, safety, out);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_cacorrectrgb_params_t *d = (dt_iop_cacorrectrgb_params_t *)piece->data;
  const float scale = piece->iscale / roi_in->scale;
  const int ch = piece->colors;
  const size_t width = roi_out->width;
  const size_t height = roi_out->height;
  const float* in = (float*)ivoid;
  float* out = (float*)ovoid;
  const float sigma = MAX(d->radius / scale, 1.0f);

  if(ch != 4)
  {
    memcpy(out, in, width * height * ch * sizeof(float));
    return;
  }

  const dt_iop_cacorrectrgb_guide_channel_t guide = d->guide_channel;
  const dt_iop_cacorrectrgb_mode_t mode = d->mode;
  // whether to be very conservative in preserving the original image, or to
  // keep algorithm result even if it overshoots
  const float safety = powf(20.0f, 1.0f - d->strength);
  reduce_chromatic_aberrations(in, width, height, ch, sigma, guide, mode, safety, out);
}

/** gui setup and update, these are needed. */
void gui_update(dt_iop_module_t *self)
{
  dt_iop_cacorrectrgb_gui_data_t *g = (dt_iop_cacorrectrgb_gui_data_t *)self->gui_data;
  dt_iop_cacorrectrgb_params_t *p = (dt_iop_cacorrectrgb_params_t *)self->params;

  dt_bauhaus_combobox_set_from_value(g->guide_channel, p->guide_channel);
  dt_bauhaus_slider_set_soft(g->radius, p->radius);
  dt_bauhaus_slider_set_soft(g->strength, p->strength);
  dt_bauhaus_combobox_set_from_value(g->mode, p->mode);
}

/** optional: if this exists, it will be called to init new defaults if a new image is
 * loaded from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_cacorrectrgb_params_t *d = (dt_iop_cacorrectrgb_params_t *)module->default_params;

  d->guide_channel = DT_CACORRECT_RGB_G;
  d->radius = 5.0f;
  d->strength = 0.5f;
  d->mode = DT_CACORRECT_MODE_STANDARD;

  dt_iop_cacorrectrgb_gui_data_t *g = (dt_iop_cacorrectrgb_gui_data_t *)module->gui_data;
  if(g)
  {
    dt_bauhaus_combobox_set_default(g->guide_channel, d->guide_channel);
    dt_bauhaus_slider_set_default(g->radius, d->radius);
    dt_bauhaus_slider_set_soft_range(g->radius, 1.0, 20.0);
    dt_bauhaus_slider_set_default(g->strength, d->strength);
    dt_bauhaus_combobox_set_default(g->mode, d->mode);
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
                                           "have artefacts."));
  g->radius = dt_bauhaus_slider_from_params(self, "radius");
  gtk_widget_set_tooltip_text(g->radius, _("increase for stronger correction\n"));
  g->strength = dt_bauhaus_slider_from_params(self, "strength");
  gtk_widget_set_tooltip_text(g->strength, _("balance between smoothing colors\n"
                                             "and preserving them.\n"
                                             "high values can lead to overshooting\n"
                                             "and edge bleeding."));

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_label_new(_("advanced parameters:")), TRUE, TRUE, 0);
  g->mode = dt_bauhaus_combobox_from_params(self, "mode");
  gtk_widget_set_tooltip_text(g->mode, _("correction mode to use.\n"
                                         "can help with multiple\n"
                                         "instances for very damaged\n"
                                         "images.\n"
                                         "darken only is particularly\n"
                                         "efficient to correct blue\n"
                                         "chromatic aberration."));
}
