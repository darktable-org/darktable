/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>

#include "bauhaus/bauhaus.h"
#include "common/gaussian.h"
#include "common/iop_profile.h"
#include "common/imagebuf.h"
#include "common/math.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_halation_params_t)

/* Halation is light that passed through the emulsion, reflected off the film
   base and re-exposed it from behind. The red-sensitive layer sits closest to
   the base, so red both travels furthest and is re-exposed most, which is what
   makes the halo warm. */

typedef struct dt_iop_halation_params_t
{
  float strength;  // $MIN: 0.0 $MAX: 50.0 $DEFAULT: 5.0 $DESCRIPTION: "strength"
  float threshold; // $MIN: -4.0 $MAX: 4.0 $DEFAULT: 0.0 $DESCRIPTION: "threshold"
  float size;      // $MIN: 0.0005 $MAX: 0.1 $DEFAULT: 0.01 $DESCRIPTION: "size"
  float spread;    // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 60.0 $DESCRIPTION: "chromatic spread"
  gboolean preserve; // $DEFAULT: TRUE $DESCRIPTION: "preserve energy"
} dt_iop_halation_params_t;

typedef struct dt_iop_halation_gui_data_t
{
  GtkWidget *strength, *threshold, *size, *spread, *preserve;
} dt_iop_halation_gui_data_t;

typedef dt_iop_halation_params_t dt_iop_halation_data_t;

const char *name()
{
  return _("halation");
}

const char *aliases()
{
  return _("glow|bloom|film|halo");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("spread light from highlights the way film halation does"),
                                      _("creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("linear, RGB"),
                                      _("linear, RGB, scene-referred"));
}

/* per-channel radii in pixels. red spreads furthest, blue least; at spread 0
   all three match and the module degrades to a plain scene-linear bloom */
/* how much of the halated light each channel receives. the red-sensitive layer
   lies against the base, so it both spreads furthest and is re-exposed most.
   varying the radius alone is not enough: each channel's blur conserves its own
   energy, so a narrower blue would simply pile up nearer the source and turn
   the visible part of the halo cyan */
static void _channel_gains(const dt_iop_halation_data_t *const d,
                           float gain[3])
{
  const float s = d->spread * 0.01f;

  gain[0] = 1.0f;
  gain[1] = 1.0f - 0.45f * s;
  gain[2] = 1.0f - 0.70f * s;
}

static void _channel_radii(const dt_iop_halation_data_t *const d,
                           const dt_dev_pixelpipe_iop_t *const piece,
                           const float scale,
                           float radius[3])
{
  /* size is a share of the full image diagonal, so the halo keeps its
     proportions between the preview and the exported file */
  const float iw = piece->iwidth * piece->iscale;
  const float ih = piece->iheight * piece->iscale;
  const float w = dt_fast_hypotf(iw, ih) * d->size * scale;
  const float s = d->spread * 0.01f;

  radius[0] = w;
  radius[1] = w * (1.0f - 0.35f * s);
  radius[2] = w * (1.0f - 0.60f * s);
}

/* Scatter is a round, exponential falloff, and a separable kernel f(x)*f(y) is
   only round when f is a Gaussian. An exponential is a scale mixture of
   Gaussians, so three of them, weighted, are both isotropic and exponential in
   profile: within 1% of exp(-d/r) on average out to 3r, against 25% for a
   separable exponential, which halates a point light as a cross. */
#define HALATION_TERMS 3
static const float _halation_sigma[HALATION_TERMS]  = { 0.373f, 0.888f, 1.800f };
static const float _halation_weight[HALATION_TERMS] = { 0.0468f, 0.2892f, 0.6640f };

/* Light does not cross to the base and return once. Each round trip is a
   further bounce: it travels through the emulsion again, so its spread grows
   as sqrt(k) (independent scatters add in variance, not in radius), and it
   loses roughly half its energy per bounce at the two interfaces. Three
   bounces is where the fourth is already below 7% and lost under the tail of
   the first. Modelled after spektrafilm's sf_halation(), which uses the same
   rho and the same sqrt(k) growth for its back-reflection stage. */
#define HALATION_BOUNCES 3
#define HALATION_BOUNCE_DECAY 0.5f

/* the bounces and the exponential terms collapse into one weighted bank of
   Gaussians, so the blur loop does not need to know about either */
#define HALATION_KERNELS (HALATION_TERMS * HALATION_BOUNCES)

static void _halation_kernel(float scale[HALATION_KERNELS],
                             float weight[HALATION_KERNELS])
{
  float decay[HALATION_BOUNCES];
  float dsum = 0.0f;
  for(int k = 0; k < HALATION_BOUNCES; k++)
  {
    decay[k] = powf(HALATION_BOUNCE_DECAY, (float)k);
    dsum += decay[k];
  }

  int n = 0;
  for(int k = 0; k < HALATION_BOUNCES; k++)
  {
    const float grow = sqrtf((float)(k + 1));
    for(int t = 0; t < HALATION_TERMS; t++, n++)
    {
      scale[n] = _halation_sigma[t] * grow;
      weight[n] = _halation_weight[t] * decay[k] / dsum;
    }
  }
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_halation_data_t *const d = piece->data;

  if(!dt_iop_have_required_input_format(4, self, piece->colors,
                                        ivoid, ovoid, roi_in, roi_out))
    return;

  const float *const restrict in = (const float *const)ivoid;
  float *const restrict out = (float *const)ovoid;
  const size_t width = roi_out->width;
  const size_t height = roi_out->height;
  const size_t npixels = width * height;

  /* the pipe works in the user's profile, which is not necessarily Rec.709,
     so take the luminance weights from it rather than hard-coding them */
  const dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  if(!work_profile)
  {
    dt_iop_copy_image_roi(out, in, 4, roi_in, roi_out);
    return;
  }

  float radius[3];
  float gain[3];
  _channel_radii(d, piece, roi_in->scale / piece->iscale, radius);
  _channel_gains(d, gain);

  const float amount = d->strength * 0.01f;
  /* only strong light penetrates the emulsion far enough to reflect off the
     base. without this the blur of the whole frame is mixed back in and the
     result is a veiling haze rather than a halo. the knee is soft, so there is
     no visible boundary where the effect starts */
  const float knee = 0.1845f * exp2f(d->threshold);
  const float knee2 = knee * knee;
  /* preserving energy removes the scattered light from where it came from,
     so overall exposure does not drift with strength */
  const float keep = d->preserve ? 1.0f - amount : 1.0f;

  float *const restrict src = dt_alloc_align_float(npixels);
  float *const restrict blurred = dt_alloc_align_float(npixels);

  if(!src || !blurred)
  {
    dt_free_align(src);
    dt_free_align(blurred);
    dt_iop_copy_image_roi(out, in, 4, roi_in, roi_out);
    return;
  }

  /* start from the source, less whatever leaves it */
  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    const float *const px = in + 4 * k;
    const float lum = dt_ioppr_get_rgb_matrix_luminance
      (px, work_profile->matrix_in, work_profile->lut_in,
       work_profile->unbounded_coeffs_in, work_profile->lutsize,
       work_profile->nonlinearlut);
    const float l2 = lum * lum;
    const float scattered = l2 / (l2 + knee2);

    for(size_t c = 0; c < 3; c++)
      out[4 * k + c] = px[c] - (1.0f - keep) * gain[c] * px[c] * scattered;
    out[4 * k + 3] = px[3];
  }

  const float gmax = FLT_MAX, gmin = -FLT_MAX;

  float kscale[HALATION_KERNELS], kweight[HALATION_KERNELS];
  _halation_kernel(kscale, kweight);

  for(size_t c = 0; c < 3; c++)
  {
    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
    {
      /* weight on luminance, not on the channel, so the halo keeps the colour
         of the light that cast it */
      const float *const px = in + 4 * k;
      const float lum = dt_ioppr_get_rgb_matrix_luminance
        (px, work_profile->matrix_in, work_profile->lut_in,
         work_profile->unbounded_coeffs_in, work_profile->lutsize,
         work_profile->nonlinearlut);
      const float l2 = lum * lum;
      src[k] = px[c] * l2 / (l2 + knee2);
    }

    for(int t = 0; t < HALATION_KERNELS; t++)
    {
      const float sigma = radius[c] * kscale[t];
      if(sigma < 0.1f) continue;

      dt_gaussian_t *g = dt_gaussian_init(width, height, 1, &gmax, &gmin, sigma, 0);
      if(!g) continue;
      dt_gaussian_blur(g, src, blurred);
      dt_gaussian_free(g);

      const float w = amount * gain[c] * kweight[t];

      DT_OMP_FOR()
      for(size_t k = 0; k < npixels; k++)
        out[4 * k + c] += w * blurred[k];
    }
  }

  dt_free_align(src);
  dt_free_align(blurred);
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  const dt_iop_halation_data_t *const d = piece->data;

  float radius[3];
  _channel_radii(d, piece, roi_in->scale / piece->iscale, radius);

  tiling->factor = 2.5f; // in + out + source and blurred planes
  tiling->factor_cl = 2.5f;
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  /* the exponential tail never truly reaches zero: 3 radii is where it is
     down to 5%, which is below what shows as a tile seam. the widest kernel
     is the last bounce's longest term, so pad for that and not for the
     first bounce alone */
  float kscale[HALATION_KERNELS], kweight[HALATION_KERNELS];
  _halation_kernel(kscale, kweight);
  float widest = 0.0f;
  for(int t = 0; t < HALATION_KERNELS; t++) widest = fmaxf(widest, kscale[t]);
  tiling->overlap = ceilf(3.0f * radius[0] * widest);
  tiling->align = 1;
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, p1, sizeof(dt_iop_halation_params_t));
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_halation_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_gui_presets_add_generic
    (_("warm"), self->op, self->version(),
     &(dt_iop_halation_params_t){ .strength = 6.0f, .threshold = 0.0f, .size = 0.01f,
                                  .spread = 60.0f, .preserve = TRUE },
     sizeof(dt_iop_halation_params_t), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic
    (_("neutral"), self->op, self->version(),
     &(dt_iop_halation_params_t){ .strength = 5.0f, .threshold = 0.5f, .size = 0.007f,
                                  .spread = 0.0f, .preserve = TRUE },
     sizeof(dt_iop_halation_params_t), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic
    (_("print"), self->op, self->version(),
     &(dt_iop_halation_params_t){ .strength = 4.0f, .threshold = -0.5f, .size = 0.03f,
                                  .spread = 40.0f, .preserve = TRUE },
     sizeof(dt_iop_halation_params_t), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_halation_gui_data_t *g = IOP_GUI_ALLOC(halation);

  g->strength = dt_bauhaus_slider_from_params(self, "strength");
  dt_bauhaus_slider_set_format(g->strength, "%");
  gtk_widget_set_tooltip_text(g->strength, _("how much light is spread into the halo"));

  g->threshold = dt_bauhaus_slider_from_params(self, "threshold");
  dt_bauhaus_slider_set_format(g->threshold, _(" EV"));
  gtk_widget_set_tooltip_text(g->threshold, _("how bright light must be before it scatters,\n"
                                              "relative to middle gray"));

  g->size = dt_bauhaus_slider_from_params(self, "size");
  dt_bauhaus_slider_set_format(g->size, "%");
  /* radius is a scale, so give it a log slider: the useful halos live in the
     first couple of percent and get most of the travel, while the extremes
     stay on the slider instead of needing a typed value */
  dt_bauhaus_slider_set_log_curve(g->size);
  dt_bauhaus_slider_set_digits(g->size, 2);
  gtk_widget_set_tooltip_text(g->size, _("radius of the halo, as a share of the image size"));

  g->spread = dt_bauhaus_slider_from_params(self, "spread");
  dt_bauhaus_slider_set_format(g->spread, "%");
  gtk_widget_set_tooltip_text(g->spread, _("how much further red spreads than blue.\n"
                                           "at 0 all channels match and the halo is neutral"));

  g->preserve = dt_bauhaus_toggle_from_params(self, "preserve");
  gtk_widget_set_tooltip_text(g->preserve, _("take the halated light out of the highlights\n"
                                             "instead of adding it on top"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
