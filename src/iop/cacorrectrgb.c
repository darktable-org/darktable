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

typedef enum dt_iop_cacorrectrgb_guide_channel_t
{
  DT_CACORRECT_RGB_R = 0,    // $DESCRIPTION: "red"
  DT_CACORRECT_RGB_G = 1,    // $DESCRIPTION: "green"
  DT_CACORRECT_RGB_B = 2     // $DESCRIPTION: "blue"
} dt_iop_cacorrectrgb_guide_channel_t;

typedef struct dt_iop_cacorrectrgb_params_t
{
  dt_iop_cacorrectrgb_guide_channel_t guide_channel; // $DEFAULT: DT_CACORRECT_RGB_G $DESCRIPTION: "guide"
  float radius; // $MIN: 1 $MAX: 400 $DEFAULT: 1 $DESCRIPTION: "radius"
} dt_iop_cacorrectrgb_params_t;

typedef struct dt_iop_cacorrectrgb_gui_data_t
{
  GtkWidget *guide_channel, *radius;
} dt_iop_cacorrectrgb_gui_data_t;

// this returns a translatable name
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("chromatic aberrations rgb");
}

// some additional flags (self explanatory i think):
int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

// where does it appear in the gui?
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

static void normalize_manifolds(float *const restrict blurred_in, float *const restrict blurred_manifold_lower, float *const restrict blurred_manifold_higher, const size_t width, const size_t height, const dt_iop_cacorrectrgb_guide_channel_t guide)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(blurred_in, blurred_manifold_lower, blurred_manifold_higher, width, height, guide) \
  schedule(simd:static) aligned(blurred_in, blurred_manifold_lower, blurred_manifold_higher:64)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    // normalize
    const float weighth = fmaxf(blurred_manifold_higher[k * 4 + 3], 1E-6);
    const float weightl = fmaxf(blurred_manifold_lower[k * 4 + 3], 1E-6);
    for(size_t c = 0; c < 3; c++)
    {
      blurred_manifold_higher[k * 4 + c] /= weighth;
      blurred_manifold_lower[k * 4 + c] /= weightl;
    }
    // replace by average if weight is too small
    if(weighth < 0.05f)
    {
      for(size_t c = 0; c < 3; c++)
      {
        blurred_manifold_higher[k * 4 + c] = blurred_in[k * 4 + c];
      }
    }
    if(weightl < 0.05f)
    {
      for(size_t c = 0; c < 3; c++)
      {
        blurred_manifold_lower[k * 4 + c] = blurred_in[k * 4 + c];
      }
    }
  }
}

static void ca_correct_rgb(const float* const restrict in, const size_t width, const size_t height,
                          const size_t ch, const float sigma,
                          const dt_iop_cacorrectrgb_guide_channel_t guide,
                          float* const restrict out)
{
  float *const restrict blurred_in = dt_alloc_sse_ps(dt_round_size_sse(width * height * ch));
  float *const restrict manifold_higher = dt_alloc_sse_ps(dt_round_size_sse(width * height * ch));
  float *const restrict manifold_lower = dt_alloc_sse_ps(dt_round_size_sse(width * height * ch));
  float *const restrict blurred_manifold_higher = dt_alloc_sse_ps(dt_round_size_sse(width * height * ch));
  float *const restrict blurred_manifold_lower = dt_alloc_sse_ps(dt_round_size_sse(width * height * ch));

  float max[4] = {INFINITY, INFINITY, INFINITY, 1.0f};
  float min[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  dt_gaussian_t *g = dt_gaussian_init(width, height, 4, max, min, sigma, 0);
  if(!g) return;
  dt_gaussian_blur_4c(g, in, blurred_in);

  // construct the manifolds
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(in, blurred_in, manifold_lower, manifold_higher, width, height, guide) \
  schedule(simd:static) aligned(in, blurred_in, manifold_lower, manifold_higher:64)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    const float pixelg = in[k * 4 + guide];
    const float avg = blurred_in[k * 4 + guide];
    const float weighth = pixelg >= avg;
    const float weightl = pixelg <= avg;
    for(size_t c = 0; c < 3; c++)
    {
      const float pixel = in[k * 4 + c];
      manifold_higher[k * 4 + c] = pixel * weighth;
      manifold_lower[k * 4 + c] = pixel * weightl;
    }
    manifold_higher[k * 4 + 3] = weighth;
    manifold_lower[k * 4 + 3] = weightl;
  }

  dt_gaussian_blur_4c(g, manifold_higher, blurred_manifold_higher);
  dt_gaussian_blur_4c(g, manifold_lower, blurred_manifold_lower);

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

  // refine the manifolds
  // improve result especially on very degraded images
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(in, blurred_in, manifold_lower, manifold_higher, blurred_manifold_lower, blurred_manifold_higher, width, height, guide) \
  schedule(simd:static) aligned(in, blurred_in, manifold_lower, manifold_higher, blurred_manifold_lower, blurred_manifold_higher:64)
#endif
  for(size_t k = 0; k < width * height; k++)
  {
    const float pixelg = in[k * 4 + guide];
    const float avgg = blurred_in[k * 4 + guide];
    float weighth = (pixelg >= avgg);
    float weightl = (pixelg <= avgg);

    // in order to refine the manifolds, we will compute weights
    // for which all channel will have a contribution.
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
    // to determine weighth and weightl.
    //
    // at this step of the algorithm, we know if the higher manifold of guided
    // is actually higher or lower than the lower manifold.
    //
    // we can refine the manifolds by computing weights that promote pixels that
    // stretch the interval between the manifolds.
    // i.e., in our case, we give higher weights to the pixels that are equal to
    // 0 than to the pixel that is equal to 1 for the computation of the lower
    // manifold
    for(size_t c = 0; c < 3; c++)
    {
      // reminder: manifolds were constructed based on the value and average
      // of the guide channel ONLY.
      // this implies that the "higher" manifold in the channel c may be
      // actually lower than the "lower" manifold of that channel.
      const float pixel = in[k * 4 + c];
      float high = blurred_manifold_higher[k * 4 + c];
      float low = blurred_manifold_lower[k * 4 + c];
      float highc = fmaxf(high, low);
      float lowc = fminf(high, low);
      float log_diff_low = (pixel < lowc) ? 1.0f : fminf(fmaxf(pixel, 1E-6) / fmaxf(lowc, 1E-6), 2.0f);
      float log_diff_high = (pixel > highc) ? 1.0f : fminf(fmaxf(highc, 1E-6) / fmaxf(pixel, 1E-6), 2.0f);
      log_diff_low *= log_diff_low;
      log_diff_high *= log_diff_high;
      if(high > low)
      {
        weighth /= log_diff_high;
        weightl /= log_diff_low;
      }
      else
      {
        weighth /= log_diff_low;
        weightl /= log_diff_high;
      }
    }
    for(size_t c = 0; c < 3; c++)
    {
      const float pixel = in[k * 4 + c];
      manifold_higher[k * 4 + c] = pixel * weighth;
      manifold_lower[k * 4 + c] = pixel * weightl;
    }
    manifold_higher[k * 4 + 3] = weighth;
    manifold_lower[k * 4 + 3] = weightl;
  }

  dt_gaussian_blur_4c(g, manifold_higher, blurred_manifold_higher);
  dt_gaussian_blur_4c(g, manifold_lower, blurred_manifold_lower);
  dt_gaussian_free(g);
  dt_free_align(manifold_lower);
  dt_free_align(manifold_higher);

  normalize_manifolds(blurred_in, blurred_manifold_lower, blurred_manifold_higher, width, height, guide);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(in, width, height, guide, blurred_in, blurred_manifold_higher, blurred_manifold_lower, out, sigma) \
  schedule(simd:static) aligned(in, blurred_in, blurred_manifold_higher, blurred_manifold_lower, out)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const float high_guide = fmaxf(blurred_manifold_higher[(i * width + j) * 4 + guide], 1E-6);
      const float low_guide = fmaxf(blurred_manifold_lower[(i * width + j) * 4 + guide], 1E-6);
      const float log_high = logf(high_guide);
      const float log_low = logf(low_guide);
      for(size_t kc = 1; kc <= 2; kc++)
      {
        size_t c = (guide + kc) % 3;
        const float pixelg = in[(i * width + j) * 4 + guide];

        const float log_pixg = logf(fminf(fmaxf(pixelg, low_guide), high_guide));
        float ratio_high_manifolds = blurred_manifold_higher[(i * width + j) * 4 + c] / high_guide;
        float ratio_low_manifolds = blurred_manifold_lower[(i * width + j) * 4 + c] / low_guide;

        float dist = fabsf(log_high - log_pixg) / fmaxf(fabsf(log_high - log_low), 1E-6);
        dist = fminf(dist, 1.0f);

        float ratio = powf(ratio_low_manifolds, dist) * powf(ratio_high_manifolds, 1.0f - dist);
        out[(i * width + j) * 4 + c] =  in[(i * width + j) * 4 + guide] * ratio;
      }
      out[(i * width + j) * 4 + guide] = in[(i * width + j) * 4 + guide];
      out[(i * width + j) * 4 + 3] = in[(i * width + j) * 4 + 3];
    }
  }

  dt_free_align(blurred_in);
  dt_free_align(blurred_manifold_lower);
  dt_free_align(blurred_manifold_higher);
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
  const float sigma = MAX(d->radius / scale, 1);

  if(ch != 4 || sigma < 0.5f)
  {
    memcpy(out, in, width * height * ch * sizeof(float));
    return;
  }

  const dt_iop_cacorrectrgb_guide_channel_t guide = d->guide_channel;
  ca_correct_rgb(in, width, height, ch, sigma, guide, out);
}

/** gui setup and update, these are needed. */
void gui_update(dt_iop_module_t *self)
{
  dt_iop_cacorrectrgb_gui_data_t *g = (dt_iop_cacorrectrgb_gui_data_t *)self->gui_data;
  dt_iop_cacorrectrgb_params_t *p = (dt_iop_cacorrectrgb_params_t *)self->params;

  dt_bauhaus_combobox_set_from_value(g->guide_channel, p->guide_channel);
  dt_bauhaus_slider_set(g->radius, p->radius);
}

/** optional: if this exists, it will be called to init new defaults if a new image is
 * loaded from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_cacorrectrgb_params_t *d = (dt_iop_cacorrectrgb_params_t *)module->default_params;

  d->guide_channel = DT_CACORRECT_RGB_G;
  d->radius = 1.0f;

  dt_iop_cacorrectrgb_gui_data_t *g = (dt_iop_cacorrectrgb_gui_data_t *)module->gui_data;
  if(g)
  {
    dt_bauhaus_combobox_set_default(g->guide_channel, d->guide_channel);
    dt_bauhaus_slider_set_default(g->radius, d->radius);
    dt_bauhaus_slider_set_soft_range(g->radius, 1.0, 20.0);
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
}
