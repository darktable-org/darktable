/*
  This file is part of darktable,
  Copyright (C) 2020-2023 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/gaussian.h"
#include "common/opencl.h"
#include "common/imagebuf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/noise_generator.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <inttypes.h>

DT_MODULE_INTROSPECTION(1, dt_iop_censorize_params_t)

typedef struct dt_iop_censorize_params_t
{
  float radius_1;              // $MIN: 0.0 $MAX: 500.0 $DEFAULT: 0.0  $DESCRIPTION: "input blur radius"
  float pixelate;              // $MIN: 0.0 $MAX: 500.0 $DEFAULT: 0.0 $DESCRIPTION: "pixellation radius"
  float radius_2;              // $MIN: 0.0 $MAX: 500.0 $DEFAULT: 0.0  $DESCRIPTION: "output blur radius"
  float noise;                 // $MIN: 0.0 $MAX: 1.0   $DEFAULT: 0.0   $DESCRIPTION: "noise level"
} dt_iop_censorize_params_t;


typedef struct dt_iop_censorize_gui_data_t
{
  GtkWidget *radius_1, *pixelate, *radius_2, *noise;
} dt_iop_censorize_gui_data_t;

typedef dt_iop_censorize_params_t dt_iop_censorize_data_t;

typedef struct dt_iop_censorize_global_data_t
{
  int kernel_lowpass_mix;
} dt_iop_censorize_global_data_t;

typedef struct point_t
{
  size_t x, y;
} point_t;

const char *
name()
{
  return _("censorize");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("censorize license plates and body parts for privacy"),
                                      _("creative"),
                                      _("linear or non-linear, RGB, scene-referred"),
                                      _("frequential, RGB"),
                                      _("special, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static inline void make_noise(float *const output, const float noise, const size_t width, const size_t height)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(output, width, height, noise) \
  schedule(simd:static) aligned(output:64) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      // Init random number generator
      uint32_t DT_ALIGNED_ARRAY state[4] = { splitmix32(j + 1), splitmix32((j + 1) * (i + 3)), splitmix32(1337), splitmix32(666) };
      xoshiro128plus(state);
      xoshiro128plus(state);
      xoshiro128plus(state);
      xoshiro128plus(state);

      const size_t index = (i * width + j) * 4;
      float *const restrict pix_out = __builtin_assume_aligned(output + index, 16);
      const float norm = pix_out[1];

      // create statistical noise
      const float epsilon = gaussian_noise(norm, noise * norm, i % 2 || j % 2, state) / norm;

      // add noise to output
      for(size_t c = 0; c < 3; c++) pix_out[c] = fmaxf(pix_out[c] * epsilon, 0.f);
    }
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  float *restrict temp;
  if(!dt_iop_alloc_image_buffers(self,roi_in,roi_out,
                                 4 | DT_IMGSZ_INPUT, &temp,
                                 0, NULL))
  {
    dt_iop_copy_image_roi(ovoid, ivoid, piece->colors, roi_in, roi_out, 0);
    return;
  }
  dt_iop_censorize_data_t *data = (dt_iop_censorize_data_t *)piece->data;
  const float *const restrict in = DT_IS_ALIGNED((const float *const restrict)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float *const restrict)ovoid);

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = 4;

  const float sigma_1 = data->radius_1 * roi_in->scale / piece->iscale;
  const float sigma_2 = data->radius_2 * roi_in->scale / piece->iscale;
  const size_t pixel_radius = data->pixelate * roi_in->scale / piece->iscale;

  // used to adjuste blur level depending on size. Don't amplify noise if magnified > 100%
  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float noise = data->noise / scale;

  dt_aligned_pixel_t RGBmax, RGBmin;
  for(int k = 0; k < 4; k++)
  {
    RGBmax[k] = INFINITY;
    RGBmin[k] = 0.f;
  }

  const float *restrict input = in;
  float *restrict output = out;

  // first blurring step
  if(sigma_1 != 0.f)
  {
    dt_gaussian_t *g = dt_gaussian_init(width, height, ch, RGBmax, RGBmin, sigma_1, 0);
    if(!g) return;
    dt_gaussian_blur_4c(g, input, output);
    dt_gaussian_free(g);

    input = output;
  }

  output = temp;

  // pixelate
  if(pixel_radius != 0)
  {
    const size_t pixels_x = width / (2 * pixel_radius);
    const size_t pixels_y = height / (2 * pixel_radius);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, ch, input, output, pixel_radius, pixels_y, pixels_x) \
  schedule(simd:static) collapse(2)
#endif
    for(size_t j = 0; j < pixels_y + 1; j++)
      for(size_t i = 0; i < pixels_x + 1; i++)
      {
        // get the top left coordinate of the big pixel
        const point_t tl = { CLAMP(2 * pixel_radius * i, 0, width - 1),
                             CLAMP(2 * pixel_radius * j, 0, height - 1) };
        // get the center of the big pixel
        const point_t cc = { CLAMP(tl.x + pixel_radius, 0, width - 1),
                             CLAMP(tl.y + pixel_radius, 0, height - 1) };
        // get the bottom right coordinate of the big pixel
        const point_t br = { CLAMP(cc.x + pixel_radius, 0, width - 1),
                             CLAMP(cc.y + pixel_radius, 0, height - 1) };

        // get the bounding box + center point coordinates
        const point_t box[5] = { tl, { br.x, tl.y }, cc, { tl.x, br.y }, br };

        // find the average color over the big pixel
        dt_aligned_pixel_t RGB = { 0.f };
        for(size_t k = 0; k < 5; k++)
        {
          const float *const restrict pix_in = __builtin_assume_aligned(input + (width * box[k].y + box[k].x) * 4, 16);
          for_four_channels(c)
            RGB[c] += pix_in[c] / 5.f;
        }

        // paint the big pixel with solid color == average
        for(size_t jj = tl.y; jj < br.y; jj++)
          for(size_t ii = tl.x; ii < br.x; ii++)
          {
            float *const restrict pix_out = __builtin_assume_aligned(output + (jj * width + ii) * 4, 16);
            for_four_channels(c)
              pix_out[c] = RGB[c];
          }
      }

    input = output;
  }

  // second blurring step
  if(sigma_2 != 0.f)
  {
    output = out;

    if(noise != 0.f)
      make_noise(output, noise, width, height);

    dt_gaussian_t *g = dt_gaussian_init(width, height, ch, RGBmax, RGBmin, sigma_2, 0);
    if(!g) return;
    dt_gaussian_blur_4c(g, input, output);
    dt_gaussian_free(g);
  }
  else
  {
    output = out;
    dt_simd_memcpy(input, output, (size_t)width * height * ch);
  }

  if(noise != 0.f)
    make_noise(output, noise, width, height);

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);

  dt_free_align(temp);
}


// OpenCL not implemented yet, but the following only needs a slight modification to get it working
#if FALSE
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_censorize_data_t *d = (dt_iop_censorize_data_t *)piece->data;
  dt_iop_censorize_global_data_t *gd = (dt_iop_censorize_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const float radius_1 = fmax(0.1f, d->radius_1);
  const float sigma = radius_1 * roi_in->scale / piece->iscale;
  const float saturation = d->saturation;
  const int order = d->order;
  const int unbound = d->unbound;

  cl_mem dev_cm = NULL;
  cl_mem dev_ccoeffs = NULL;
  cl_mem dev_lm = NULL;
  cl_mem dev_lcoeffs = NULL;
  cl_mem dev_tmp = NULL;

  dt_gaussian_cl_t *g = NULL;
  dt_bilateral_cl_t *b = NULL;

  float RGBmax[] = { 100.0f, 128.0f, 128.0f, 1.0f };
  float RGBmin[] = { 0.0f, -128.0f, -128.0f, 0.0f };

  if(unbound)
  {
    for(int k = 0; k < 4; k++) RGBmax[k] = INFINITY;
    for(int k = 0; k < 4; k++) RGBmin[k] = -INFINITY;
  }

  if(d->lowpass_algo == LOWPASS_ALGO_GAUSSIAN)
  {
    g = dt_gaussian_init_cl(devid, width, height, channels, RGBmax, RGBmin, sigma, order);
    if(!g) goto error;
    err = dt_gaussian_blur_cl(g, dev_in, dev_out);
    if(err != CL_SUCCESS) goto error;
    dt_gaussian_free_cl(g);
    g = NULL;
  }
  else
  {
    const float sigma_r = 100.0f; // does not depend on scale
    const float sigma_s = sigma;
    const float detail = -1.0f; // we want the bilateral base layer

    b = dt_bilateral_init_cl(devid, width, height, sigma_s, sigma_r);
    if(!b) goto error;
    err = dt_bilateral_splat_cl(b, dev_in);
    if(err != CL_SUCCESS) goto error;
    err = dt_bilateral_blur_cl(b);
    if(err != CL_SUCCESS) goto error;
    err = dt_bilateral_slice_cl(b, dev_in, dev_out, detail);
    if(err != CL_SUCCESS) goto error;
    dt_bilateral_free_cl(b);
    b = NULL; // make sure we don't clean it up twice
  }

  dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
  if(dev_tmp == NULL) goto error;

  dev_cm = dt_opencl_copy_host_to_device(devid, d->ctable, 256, 256, sizeof(float));
  if(dev_cm == NULL) goto error;

  dev_ccoeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->cunbounded_coeffs);
  if(dev_ccoeffs == NULL) goto error;

  dev_lm = dt_opencl_copy_host_to_device(devid, d->ltable, 256, 256, sizeof(float));
  if(dev_lm == NULL) goto error;

  dev_lcoeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->lunbounded_coeffs);
  if(dev_lcoeffs == NULL) goto error;

  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };
  err = dt_opencl_enqueue_copy_image(devid, dev_out, dev_tmp, origin, origin, region);
  if(err != CL_SUCCESS) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lowpass_mix, width, height,
    CLARG(dev_tmp), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(saturation), CLARG(dev_cm),
    CLARG(dev_ccoeffs), CLARG(dev_lm), CLARG(dev_lcoeffs), CLARG(unbound));
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_lcoeffs);
  dt_opencl_release_mem_object(dev_lm);
  dt_opencl_release_mem_object(dev_ccoeffs);
  dt_opencl_release_mem_object(dev_cm);

  return TRUE;

error:
  if(g) dt_gaussian_free_cl(g);
  if(b) dt_bilateral_free_cl(b);

  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_lcoeffs);
  dt_opencl_release_mem_object(dev_lm);
  dt_opencl_release_mem_object(dev_ccoeffs);
  dt_opencl_release_mem_object(dev_cm);
  dt_print(DT_DEBUG_OPENCL, "[opencl_lowpass] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 3.0f;
  tiling->factor_cl = 5.0f;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 1;
  tiling->yalign = 1;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 6; // gaussian.cl, from programs.conf
  dt_iop_censorize_global_data_t *gd
      = (dt_iop_censorize_global_data_t *)malloc(sizeof(dt_iop_censorize_global_data_t));
  module->data = gd;
  gd->kernel_lowpass_mix = dt_opencl_create_kernel(program, "lowpass_mix");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_censorize_global_data_t *gd = (dt_iop_censorize_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_lowpass_mix);
  free(module->data);
  module->data = NULL;
}

#endif

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_censorize_gui_data_t *g = IOP_GUI_ALLOC(censorize);

  g->radius_1 = dt_bauhaus_slider_from_params(self, N_("radius_1"));

  g->pixelate = dt_bauhaus_slider_from_params(self, N_("pixelate"));

  g->radius_2 = dt_bauhaus_slider_from_params(self, N_("radius_2"));

  g->noise = dt_bauhaus_slider_from_params(self, N_("noise"));

  gtk_widget_set_tooltip_text(g->radius_1, _("radius of gaussian blur before pixellation"));
  gtk_widget_set_tooltip_text(g->radius_2, _("radius of gaussian blur after pixellation"));
  gtk_widget_set_tooltip_text(g->pixelate, _("radius of the intermediate pixellation"));
  gtk_widget_set_tooltip_text(g->noise, _("amount of noise to add at the end"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

