/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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

#define __STDC_FORMAT_MACROS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/imagebuf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "iop/Permutohedral.h"
#include <gtk/gtk.h>
#include <inttypes.h>

extern "C" {

/**
 * implementation of the 5d-color bilateral filter using andrew adams et al.'s
 * permutohedral lattice, which they kindly provided online as c++ code, under new bsd license.
 */

#define MAX_DIRECT_STAMP_RADIUS 6.0f

DT_MODULE_INTROSPECTION(1, dt_iop_bilateral_params_t)

typedef struct dt_iop_bilateral_params_t
{
  // standard deviations of the gauss to use for blurring in the dimensions x,y,r,g,b (or L*,a*,b*)
  float radius;           // $MIN: 1.0 $MAX: 50.0 $DEFAULT: 15.0
  float reserved;         // $DEFAULT: 15.0
  float red, green, blue; // $MIN: 0.0001 $MAX: 1.0 $DEFAULT: 0.005
} dt_iop_bilateral_params_t;

typedef struct dt_iop_bilateral_gui_data_t
{
  GtkWidget *radius, *red, *green, *blue;
} dt_iop_bilateral_gui_data_t;

typedef struct dt_iop_bilateral_data_t
{
  float sigma[5];
} dt_iop_bilateral_data_t;

const char *name()
{
  return _("surface blur");
}

const char *aliases()
{
  return _("denoise (bilateral filter)");
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("apply edge-aware surface blur to denoise or smoothen textures"),
                                      _("corrective and creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("linear, RGB"),
                                      _("linear, RGB, scene-referred"));
}

static void _compute_sigmas(float sigma[5],
                            struct dt_iop_bilateral_data_t *data,
                            float scale,
                            float iscale)
{
  sigma[0] = data->sigma[0] * scale / iscale;
  sigma[1] = data->sigma[1] * scale / iscale;
  sigma[2] = data->sigma[2];
  sigma[3] = data->sigma[3];
  sigma[4] = data->sigma[4];
}

void process(struct dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;
  assert(roi_in->width == roi_out->width);
  assert(roi_in->height == roi_out->height);
  dt_iop_bilateral_data_t *data = (dt_iop_bilateral_data_t *)piece->data;

  float sigma[5];
  _compute_sigmas(sigma, data, roi_in->scale, piece->iscale);
  if(fmaxf(sigma[0], sigma[1]) < .1)
  {
    dt_iop_image_copy_by_size((float*)ovoid, (float*)ivoid, roi_out->width, roi_out->height, 4);
    return;
  }

  // if rad <= 6 use naive version!
  const int rad = (int)(3.0f * fmaxf(sigma[0], sigma[1]) + 1.0f);
  if(rad <= MAX_DIRECT_STAMP_RADIUS && (piece->pipe->type & DT_DEV_PIXELPIPE_THUMBNAIL))
  {
    // no use denoising the thumbnail. takes ages without permutohedral
    dt_iop_image_copy_by_size((float*)ovoid, (float*)ivoid, roi_out->width, roi_out->height, 4);
  }
  else if(rad <= MAX_DIRECT_STAMP_RADIUS)
  {
    static const size_t weights_size = 2 * (6 + 1) * 2 * (6 + 1);
    float mat[weights_size];
    const int wd = 2 * rad + 1;
    float *m = mat + rad * wd + rad;
    float weight = 0.0f;
    const dt_aligned_pixel_t isig2col = { 1.f / (2.0f * sigma[2] * sigma[2]),
                                          1.f / (2.0f * sigma[3] * sigma[3]),
					  1.f / (2.0f * sigma[4] * sigma[4]),
					  0.0f };
    // init gaussian kernel
    for(int l = -rad; l <= rad; l++)
      for(int k = -rad; k <= rad; k++)
        weight += m[l * wd + k] = expf(-(l * l + k * k) / (2.f * sigma[0] * sigma[0]));
    for(int l = -rad; l <= rad; l++)
      for(int k = -rad; k <= rad; k++) m[l * wd + k] /= weight;

    const size_t width = roi_out->width;
    const size_t height = roi_out->height;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ivoid, ovoid, rad, height, width, wd, m, isig2col)				\
    schedule(static)
#endif
    for(size_t j = 0; j < height; j++)
    {
      const float *in = ((float *)ivoid) + 4 * (j * width);
      float *out = ((float *)ovoid) + 4 * (j * width);
      if(j < (unsigned)rad || j >= height - rad)
      {
        // copy the unprocessed top/bottom border rows
        for(size_t i = 0; i < width; i++)
	{
	  copy_pixel_nontemporal(out + 4*i, in + 4*i);
	}
        continue;
      }
      for(size_t i = 0; i < (size_t)rad; i++, in += 4)
      {
        // copy the unprocessed left border pixels
        copy_pixel_nontemporal(out + 4*i, in);
      }
      // apply blur to main body of image
      for(size_t i = rad; i < width - rad; i++, in += 4)
      {
        float sumw = 0.0f;
	dt_aligned_pixel_t res = { 0.0f, 0.0f, 0.0f, 0.0f };
	dt_aligned_pixel_t pixel;
	copy_pixel(pixel, in);
        for(ssize_t l = -rad; l <= rad; l++)
          for(ssize_t k = -rad; k <= rad; k++)
          {
	    const float *inp = in + 4 * (l * width + k);
	    dt_aligned_pixel_t chandiff;
	    for_each_channel(c)
	       chandiff[c] = (pixel[c] - inp[c]) * (pixel[c] - inp[c]) * isig2col[c];
	    float diff = chandiff[0] + chandiff[1] + chandiff[2];
	    float pix_weight = m[l * wd + k] * expf(-diff);
            for_each_channel(c)
	       res[c] += inp[c] * pix_weight;
            sumw += pix_weight;
          }
	for_each_channel(c)
	   res[c] /= sumw;
	copy_pixel_nontemporal(out + 4*i, res);
      }
      for(size_t i = width - rad; i < width; i++, in += 4)
      {
        // copy the unprocessed right border pixels
        copy_pixel_nontemporal(out + 4*i, in);
      }
    }
    dt_omploop_sfence();
  }
  else
  {
    for(int k = 0; k < 5; k++) sigma[k] = 1.0f / sigma[k];
    
    const size_t height = roi_in->height;
    const size_t width = roi_in->width;

    const size_t grid_points = (height*sigma[0]) * (width*sigma[1]) * sigma[2] * sigma[3] * sigma[4];
    PermutohedralLattice<5, 4> lattice(width * height, dt_get_num_threads(), grid_points);

// splat into the lattice
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ivoid, height, width, sigma)	\
    shared(lattice)					\
    schedule(static)
#endif
    for(size_t j = 0; j < height; j++)
    {
      const float *in = (const float *)ivoid + j * width * 4;
      const int thread = dt_get_thread_num();
      size_t index = j * width;
      for(size_t i = 0; i < width; i++)
      {
        float pos[5] = { i * sigma[0], j * sigma[1], in[0] * sigma[2], in[1] * sigma[3], in[2] * sigma[4] };
        dt_aligned_pixel_t val = { in[0], in[1], in[2], 1.0f };
        lattice.splat(pos, val, index + i, thread);
        in += 4;
      }
    }

    lattice.merge_splat_threads();

    // blur the lattice
    lattice.blur();

    // slice from the lattice
    float *const out = (float*)ovoid;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(out, height, width)	\
    shared(lattice) \
    schedule(static)
#endif
    for(size_t index = 0; index < height*width; index++)
    {
    float DT_ALIGNED_PIXEL val[4];
    lattice.slice(val, index);
    for_each_channel(k)
       out[(size_t)4*index + k] = val[k] / val[3];
    }
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_bilateral_params_t *p = (dt_iop_bilateral_params_t *)p1;
  dt_iop_bilateral_data_t *d = (dt_iop_bilateral_data_t *)piece->data;
  d->sigma[0] = p->radius;
  d->sigma[1] = p->radius;
  d->sigma[2] = p->red;
  d->sigma[3] = p->green;
  d->sigma[4] = p->blue;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_bilateral_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_bilateral_data_t *data = (dt_iop_bilateral_data_t *)piece->data;
  float sigma[5];
  _compute_sigmas(sigma, data, roi_in->scale, piece->iscale);
  const int rad = (int)(3.0f * fmaxf(sigma[0], sigma[1]) + 1.0f);
  if(rad <= MAX_DIRECT_STAMP_RADIUS)
    tiling->factor = 2.0f;  // direct stamp, no intermediate buffers used
  else
  {
    // permutohedral needs LOTS of memory
    // start with the fixed memory requirements
    tiling->factor = 2.0f /*input+output*/ + 52.0f/16.0f /*52 bytes per pixel for ReplayEntry array*/;
    // now try to estimate the variable needs for the hashtable based on the current parameters
    size_t npixels = (size_t)roi_out->height * roi_out->width;
    size_t grid_points = (roi_out->height/sigma[0]) * (roi_out->width/sigma[1]) / sigma[2] / sigma[3] / sigma[4];
    size_t hash_bytes = PermutohedralLattice<5, 4>::estimatedBytes(grid_points, npixels);
    tiling->factor += (hash_bytes / (16.0f*npixels));
    std::cerr<<"tiling->factor = "<<tiling->factor<<", hash_bytes="<<hash_bytes<<", npixels="<<npixels<<std::endl; //DEBUG
  }
  tiling->overhead = 0;
  tiling->overlap = rad;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_bilateral_gui_data_t *g = IOP_GUI_ALLOC(bilateral);

  g->radius = dt_bauhaus_slider_from_params(self, N_("radius"));
  gtk_widget_set_tooltip_text(g->radius, _("spatial extent of the gaussian"));
  dt_bauhaus_slider_set_soft_range(g->radius, 1.0, 30.0);

  g->red = dt_bauhaus_slider_from_params(self, N_("red"));
  gtk_widget_set_tooltip_text(g->red, _("how much to blur red"));
  dt_bauhaus_slider_set_soft_max(g->red, 0.1);
  dt_bauhaus_slider_set_digits(g->red, 4);

  g->green = dt_bauhaus_slider_from_params(self, N_("green"));
  gtk_widget_set_tooltip_text(g->green, _("how much to blur green"));
  dt_bauhaus_slider_set_soft_max(g->green, 0.1);
  dt_bauhaus_slider_set_digits(g->green, 4);

  g->blue = dt_bauhaus_slider_from_params(self, N_("blue"));
  gtk_widget_set_tooltip_text(g->blue, _("how much to blur blue"));
  dt_bauhaus_slider_set_soft_max(g->blue, 0.1);
  dt_bauhaus_slider_set_digits(g->blue, 4);
}
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

