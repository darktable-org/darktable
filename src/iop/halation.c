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

/* halation is light that passed through the emulsion, reflected off the film
   base and re-exposed it from behind. The red-sensitive layer sits closest to
   the base, so red both travels furthest and is re-exposed most, which is what
   makes the halo warm */

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

typedef struct dt_iop_halation_global_data_t
{
  int kernel_halation_scatter;
  int kernel_halation_plane;
  int kernel_halation_accumulate;
  int kernel_halation_merge;
  int kernel_halation_write;
} dt_iop_halation_global_data_t;

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

/* per-channel radii in pixels. red spreads furthest, blue least; at spread 0
   all three match and the module degrades to a plain scene-linear bloom */
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

/* scatter is a round, exponential falloff, and a separable kernel f(x)*f(y) is
   only round when f is a Gaussian. An exponential is a scale mixture of
   Gaussians, so three of them, weighted, are both isotropic and exponential in
   profile: within 1% of exp(-d/r) on average out to 3r, against 25% for a
   separable exponential, which halates a point light as a cross.

   Light does not cross to the base and return once either. Each round trip is
   a further bounce: it travels through the emulsion again, so its spread grows
   as sqrt(k) (independent scatters add in variance, not in radius), and it
   loses roughly half its energy per bounce at the two interfaces. Three
   bounces is where the fourth is already below 7% and lost under the tail of
   the first. Modelled after spektrafilm's sf_halation(), which uses the same
   rho and the same sqrt(k) growth for its back-reflection stage.

   Three terms over three bounces is nine Gaussians per channel, and their sum
   is reproduced to 0.3% of its own peak by the four below, whose weights were
   refit to it under the same unit sum. That is twelve blurs a frame rather
   than 27, and the blur loop needs to know about neither terms nor bounces */
#define HALATION_KERNELS 4
static const float _halation_scale[HALATION_KERNELS] =
  { 0.373000f, 0.888000f, 1.800000f, 2.545584f };
static const float _halation_weight[HALATION_KERNELS] =
  { 0.030595f, 0.229307f, 0.442774f, 0.297324f };

/* the first term to land seeds the plane, so the bank needs no memset */
static void _accumulate(float *const restrict halo,
                        const float *const restrict term,
                        const size_t npixels,
                        const float w,
                        const gboolean seeded)
{
  if(seeded)
  {
    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
      halo[k] += w * term[k];
  }
  else
  {
    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
      halo[k] = w * term[k];
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
  /* the scatter mask is the same for every channel and every kernel, and the
     luminance behind it is the costly part, so it is built once */
  float *const restrict scatter = dt_alloc_align_float(npixels);
  /* the bank accumulates here one channel at a time and is merged into out
     once: adding straight into out[4k+c] touches one float per cache line */
  float *const restrict halo = dt_alloc_align_float(npixels);

  if(!src || !blurred || !scatter || !halo)
  {
    dt_free_align(src);
    dt_free_align(blurred);
    dt_free_align(scatter);
    dt_free_align(halo);
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
    scatter[k] = scattered;

    for(size_t c = 0; c < 3; c++)
      out[4 * k + c] = px[c] - (1.0f - keep) * gain[c] * px[c] * scattered;
    out[4 * k + 3] = px[3];
  }

  const float gmax = FLT_MAX, gmin = -FLT_MAX;
  gboolean failed = FALSE;

  for(size_t c = 0; c < 3 && !failed; c++)
  {
    /* weight on luminance, not on the channel, so the halo keeps the color
       of the light that cast it */
    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
      src[k] = in[4 * k + c] * scatter[k];

    /* a kernel too narrow to blur goes in unblurred, which is its own limit
       as sigma falls to zero. dropping it would lose its share of the weight,
       and the seeding pass above has already taken that energy out */
    float direct = 0.0f;
    gboolean seeded = FALSE;

    for(int t = 0; t < HALATION_KERNELS; t++)
    {
      const float w = amount * gain[c] * _halation_weight[t];
      const float sigma = radius[c] * _halation_scale[t];

      if(sigma < 0.1f)
      {
        direct += w;
        continue;
      }

      dt_gaussian_t *g = dt_gaussian_init(width, height, 1, &gmax, &gmin, sigma, 0);
      if(!g)
      {
        /* a wide term added unblurred would stamp a hard copy of the
           highlights where a soft halo belongs, so pass the input through
           instead, as the gpu path does */
        failed = TRUE;
        break;
      }
      dt_gaussian_blur(g, src, blurred);
      dt_gaussian_free(g);

      _accumulate(halo, blurred, npixels, w, seeded);
      seeded = TRUE;
    }

    if(direct > 0.0f)
    {
      _accumulate(halo, src, npixels, direct, seeded);
      seeded = TRUE;
    }

    if(seeded)
    {
      DT_OMP_FOR()
      for(size_t k = 0; k < npixels; k++)
        out[4 * k + c] += halo[k];
    }
  }

  /* the copy overwrites everything the seeding pass wrote, so bailing out of
     the middle of the bank is safe */
  if(failed) dt_iop_copy_image_roi(out, in, 4, roi_in, roi_out);

  dt_free_align(src);
  dt_free_align(blurred);
  dt_free_align(scatter);
  dt_free_align(halo);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_halation_data_t *const d = piece->data;
  const dt_iop_halation_global_data_t *const gd = self->global_data;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t npixels = (size_t)width * height;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem dev_acc = NULL, dev_scatter = NULL, dev_plane = NULL, dev_blurred = NULL;
  cl_mem dev_halo = NULL;
  cl_mem dev_profile_info = NULL, dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl = NULL;
  cl_float *profile_lut_cl = NULL;
  dt_gaussian_cl_t *g = NULL;

  const dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  if(!work_profile) return err;

  float radius[3];
  float gain3[3];
  _channel_radii(d, piece, roi_in->scale / piece->iscale, radius);
  _channel_gains(d, gain3);

  const float amount = d->strength * 0.01f;
  const float knee = 0.1845f * exp2f(d->threshold);
  const float knee2 = knee * knee;
  const float keep = d->preserve ? 1.0f - amount : 1.0f;
  const cl_float4 gain = { { gain3[0], gain3[1], gain3[2], 0.0f } };

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl,
                                            &profile_lut_cl, &dev_profile_info,
                                            &dev_profile_lut);
  if(err != CL_SUCCESS) goto cleanup;

  err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  dev_acc = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * npixels);
  dev_scatter = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npixels);
  dev_plane = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npixels);
  dev_blurred = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npixels);
  dev_halo = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npixels);
  if(!dev_acc || !dev_scatter || !dev_plane || !dev_blurred || !dev_halo) goto cleanup;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_halation_scatter, width, height,
    CLARG(dev_in), CLARG(dev_acc), CLARG(dev_scatter), CLARG(width), CLARG(height),
    CLARG(knee2), CLARG(keep), CLARG(gain), CLARG(dev_profile_info),
    CLARG(dev_profile_lut));
  if(err != CL_SUCCESS) goto cleanup;

  const float gmax = FLT_MAX, gmin = -FLT_MAX;

  for(int c = 0; c < 3; c++)
  {
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_halation_plane, width, height,
      CLARG(dev_in), CLARG(dev_scatter), CLARG(dev_plane), CLARG(width), CLARG(height),
      CLARG(c));
    if(err != CL_SUCCESS) goto cleanup;

    /* a kernel too narrow to blur goes in unblurred, which is its own limit
       as sigma falls to zero. dropping it would lose its share of the weight,
       and the scatter kernel has already taken that energy out */
    float direct = 0.0f;
    int seeded = 0;

    for(int t = 0; t < HALATION_KERNELS; t++)
    {
      const float w = amount * gain3[c] * _halation_weight[t];
      const float sigma = radius[c] * _halation_scale[t];

      if(sigma < 0.1f)
      {
        direct += w;
        continue;
      }

      g = dt_gaussian_init_cl(devid, width, height, 1, &gmax, &gmin, sigma, 0);
      if(!g)
      {
        err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
        goto cleanup;
      }
      err = dt_gaussian_blur_cl_buffer(g, dev_plane, dev_blurred);
      dt_gaussian_free_cl(g);
      g = NULL;
      if(err != CL_SUCCESS) goto cleanup;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_halation_accumulate,
        width, height, CLARG(dev_halo), CLARG(dev_blurred), CLARG(width), CLARG(height),
        CLARG(seeded), CLARG(w));
      if(err != CL_SUCCESS) goto cleanup;
      seeded = 1;
    }

    if(direct > 0.0f)
    {
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_halation_accumulate,
        width, height, CLARG(dev_halo), CLARG(dev_plane), CLARG(width), CLARG(height),
        CLARG(seeded), CLARG(direct));
      if(err != CL_SUCCESS) goto cleanup;
      seeded = 1;
    }

    if(seeded)
    {
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_halation_merge,
        width, height, CLARG(dev_acc), CLARG(dev_halo), CLARG(width), CLARG(height),
        CLARG(c));
      if(err != CL_SUCCESS) goto cleanup;
    }
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_halation_write, width, height,
    CLARG(dev_acc), CLARG(dev_out), CLARG(width), CLARG(height));

cleanup:
  if(g) dt_gaussian_free_cl(g);
  dt_opencl_release_mem_object(dev_acc);
  dt_opencl_release_mem_object(dev_scatter);
  dt_opencl_release_mem_object(dev_plane);
  dt_opencl_release_mem_object(dev_blurred);
  dt_opencl_release_mem_object(dev_halo);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl,
                                     &dev_profile_info, &dev_profile_lut);
  return err;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 45; // halation.cl, from programs.conf
  dt_iop_halation_global_data_t *gd = malloc(sizeof(dt_iop_halation_global_data_t));
  self->data = gd;
  gd->kernel_halation_scatter = dt_opencl_create_kernel(program, "halation_scatter");
  gd->kernel_halation_plane = dt_opencl_create_kernel(program, "halation_plane");
  gd->kernel_halation_accumulate = dt_opencl_create_kernel(program, "halation_accumulate");
  gd->kernel_halation_merge = dt_opencl_create_kernel(program, "halation_merge");
  gd->kernel_halation_write = dt_opencl_create_kernel(program, "halation_write");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_halation_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_halation_scatter);
  dt_opencl_free_kernel(gd->kernel_halation_plane);
  dt_opencl_free_kernel(gd->kernel_halation_accumulate);
  dt_opencl_free_kernel(gd->kernel_halation_merge);
  dt_opencl_free_kernel(gd->kernel_halation_write);
  free(self->data);
  self->data = NULL;
}
#endif

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  const dt_iop_halation_data_t *const d = piece->data;

  float radius[3];
  _channel_radii(d, piece, roi_in->scale / piece->iscale, radius);

  /* in + out + the four single-channel planes, and on top of those whatever
     the gaussian keeps for itself. the cl one pads by a block in each
     direction, so its share depends on the tile size: measure, do not guess */
  const size_t basebuffer = sizeof(float) * 4 * roi_in->width * roi_in->height;

  tiling->factor = 3.0f
    + (float)dt_gaussian_memory_use(roi_in->width, roi_in->height, 1) / basebuffer;
  /* the gpu path accumulates into an rgba buffer of its own rather than
     writing the output image directly */
#ifdef HAVE_OPENCL
  tiling->factor_cl = 4.0f
    + (float)dt_gaussian_memory_use_cl(roi_in->width, roi_in->height, 1) / basebuffer;
#endif
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  /* the exponential tail never truly reaches zero: 3 radii is where it is
     down to 5%, which is below what shows as a tile seam. the widest kernel
     is the last bounce's longest term, so pad for that and not for the
     first bounce alone */
  float widest = 0.0f;
  for(int t = 0; t < HALATION_KERNELS; t++) widest = fmaxf(widest, _halation_scale[t]);
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
