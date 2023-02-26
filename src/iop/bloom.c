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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/box_filters.h"
#include "common/imagebuf.h"
#include "common/math.h"
#include "common/opencl.h"
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
#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define NUM_BUCKETS 4 /* OpenCL bucket chain size for tmp buffers; minimum 2 */

DT_MODULE_INTROSPECTION(1, dt_iop_bloom_params_t)

typedef struct dt_iop_bloom_params_t
{
  float size;       // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 20.0
  float threshold;  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 90.0
  float strength;   // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 25.0
} dt_iop_bloom_params_t;

typedef struct dt_iop_bloom_gui_data_t
{
  GtkWidget *size, *threshold, *strength; // size,threshold,strength
} dt_iop_bloom_gui_data_t;

typedef struct dt_iop_bloom_data_t
{
  float size;
  float threshold;
  float strength;
} dt_iop_bloom_data_t;

typedef struct dt_iop_bloom_global_data_t
{
  int kernel_bloom_threshold;
  int kernel_bloom_hblur;
  int kernel_bloom_vblur;
  int kernel_bloom_mix;
} dt_iop_bloom_global_data_t;

const char *name()
{
  return _("bloom");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("apply Orton effect for a dreamy aetherical look"),
                                      _("creative"),
                                      _("non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
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
  return IOP_CS_LAB;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_bloom_data_t *const data = (dt_iop_bloom_data_t *)piece->data;
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  float *restrict blurlightness;
  if(!dt_iop_alloc_image_buffers(self, roi_in, roi_out, 1, &blurlightness, 0))
  {
    // out of memory, so just copy image through to output
    dt_iop_copy_image_roi(ovoid, ivoid, piece->colors, roi_in, roi_out, TRUE);
    return;
  }

  const float *const restrict in = DT_IS_ALIGNED((float *)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float *)ovoid);
  const size_t npixels = (size_t)roi_out->width * roi_out->height;

  /* gather light by threshold */
  const int rad = 256.0f * (fmin(100.0f, data->size + 1.0f) / 100.0f);
  const float _r = ceilf(rad * roi_in->scale / piece->iscale);
  const int radius = MIN(256.0f, _r);

  const float scale = 1.0f / exp2f(-1.0f * (fmin(100.0f, data->strength + 1.0f) / 100.0f));

  const float threshold = data->threshold;
/* get the thresholded lights into buffer */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, scale, threshold) \
  shared(blurlightness) \
  dt_omp_sharedconst(in) \
  schedule(static)
#endif
  for(size_t k = 0; k < npixels; k++)
  {
    const float L = in[4*k] * scale;
    blurlightness[k] = (L > threshold) ? L : 0.0f;
  }

  /* horizontal blur into memchannel lightness */
  const int range = 2 * radius + 1;
  const int hr = range / 2;

  dt_box_mean(blurlightness, roi_out->height, roi_out->width, 1, hr, BOX_ITERATIONS);

/* screen blend lightness with original */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels) \
  shared(blurlightness) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < npixels; k++)
  {
    out[4*k+0] = 100.0f - (((100.0f - in[4*k]) * (100.0f - blurlightness[k])) / 100.0f); // Screen blend
    out[4*k+1] = in[4*k+1];
    out[4*k+2] = in[4*k+2];
    out[4*k+3] = in[4*k+3];
  }
  dt_free_align(blurlightness);
}

#ifdef HAVE_OPENCL
static int bucket_next(unsigned int *state, unsigned int max)
{
  const unsigned int current = *state;
  const unsigned int next = (current >= max - 1 ? 0 : current + 1);

  *state = next;

  return next;
}

int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_bloom_data_t *d = (dt_iop_bloom_data_t *)piece->data;
  const dt_iop_bloom_global_data_t *gd = (dt_iop_bloom_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem dev_tmp[NUM_BUCKETS] = { NULL };
  cl_mem dev_tmp1;
  cl_mem dev_tmp2;
  unsigned int state = 0;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float threshold = d->threshold;

  const int rad = 256.0f * (fmin(100.0f, d->size + 1.0f) / 100.0f);
  const float _r = ceilf(rad * roi_in->scale / piece->iscale);
  const int radius = MIN(256.0f, _r);
  const float scale = 1.0f / exp2f(-1.0f * (fmin(100.0f, d->strength + 1.0f) / 100.0f));

  int hblocksize;
  dt_opencl_local_buffer_t hlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 2 * radius, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                  .cellsize = sizeof(float), .overhead = 0,
                                  .sizex = 1 << 16, .sizey = 1 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_bloom_hblur, &hlocopt))
    hblocksize = hlocopt.sizex;
  else
    hblocksize = 1;

  int vblocksize;
  dt_opencl_local_buffer_t vlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 1, .xfactor = 1, .yoffset = 2 * radius, .yfactor = 1,
                                  .cellsize = sizeof(float), .overhead = 0,
                                  .sizex = 1, .sizey = 1 << 16 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_bloom_vblur, &vlocopt))
    vblocksize = vlocopt.sizey;
  else
    vblocksize = 1;


  const size_t bwidth = ROUNDUP(width, hblocksize);
  const size_t bheight = ROUNDUP(height, vblocksize);

  size_t sizes[3];
  size_t local[3];

  for(int i = 0; i < NUM_BUCKETS; i++)
  {
    dev_tmp[i] = dt_opencl_alloc_device(devid, width, height, sizeof(float));
    if(dev_tmp[i] == NULL) goto error;
  }

  /* gather light by threshold */
  sizes[0] = ROUNDUPDWD(width, devid);
  sizes[1] = ROUNDUPDHT(height, devid);
  sizes[2] = 1;
  dev_tmp1 = dev_tmp[bucket_next(&state, NUM_BUCKETS)];
  dt_opencl_set_kernel_args(devid, gd->kernel_bloom_threshold, 0, CLARG(dev_in), CLARG(dev_tmp1),
    CLARG(width), CLARG(height), CLARG(scale), CLARG(threshold));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_bloom_threshold, sizes);
  if(err != CL_SUCCESS) goto error;

  if(radius != 0)
    for(int i = 0; i < BOX_ITERATIONS; i++)
    {
      /* horizontal blur */
      sizes[0] = bwidth;
      sizes[1] = ROUNDUPDHT(height, devid);
      sizes[2] = 1;
      local[0] = hblocksize;
      local[1] = 1;
      local[2] = 1;
      dev_tmp2 = dev_tmp[bucket_next(&state, NUM_BUCKETS)];
      dt_opencl_set_kernel_args(devid, gd->kernel_bloom_hblur, 0, CLARG(dev_tmp1), CLARG(dev_tmp2), CLARG(radius),
        CLARG(width), CLARG(height), CLARG(hblocksize), CLLOCAL((hblocksize + 2 * radius) * sizeof(float)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_bloom_hblur, sizes, local);
      if(err != CL_SUCCESS) goto error;


      /* vertical blur */
      sizes[0] = ROUNDUPDWD(width, devid);
      sizes[1] = bheight;
      sizes[2] = 1;
      local[0] = 1;
      local[1] = vblocksize;
      local[2] = 1;
      dev_tmp1 = dev_tmp[bucket_next(&state, NUM_BUCKETS)];
      dt_opencl_set_kernel_args(devid, gd->kernel_bloom_vblur, 0, CLARG(dev_tmp2), CLARG(dev_tmp1), CLARG(radius),
        CLARG(width), CLARG(height), CLARG(vblocksize), CLLOCAL((vblocksize + 2 * radius) * sizeof(float)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_bloom_vblur, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

  /* mixing out and in -> out */
  sizes[0] = ROUNDUPDWD(width, devid);
  sizes[1] = ROUNDUPDHT(height, devid);
  sizes[2] = 1;
  dt_opencl_set_kernel_args(devid, gd->kernel_bloom_mix, 0, CLARG(dev_in), CLARG(dev_tmp1), CLARG(dev_out),
    CLARG(width), CLARG(height));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_bloom_mix, sizes);
  if(err != CL_SUCCESS) goto error;

  for(int i = 0; i < NUM_BUCKETS; i++)
    dt_opencl_release_mem_object(dev_tmp[i]);
  return TRUE;

error:
  for(int i = 0; i < NUM_BUCKETS; i++)
    dt_opencl_release_mem_object(dev_tmp[i]);
  dt_print(DT_DEBUG_OPENCL, "[opencl_bloom] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  const dt_iop_bloom_data_t *d = (dt_iop_bloom_data_t *)piece->data;

  const int rad = 256.0f * (fmin(100.0f, d->size + 1.0f) / 100.0f);
  const float _r = ceilf(rad * roi_in->scale / piece->iscale);
  const int radius = MIN(256.0f, _r);

  tiling->factor = 2.0f + 0.25f + 0.05f; // in + out + blurlightness + slice for dt_box_mean
  tiling->factor_cl = 2.0f + NUM_BUCKETS * 0.25f; // in + out + NUM_BUCKETS * 0.25 tmp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 5 * radius; // This is a guess. TODO: check if that's sufficiently large
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 12; // bloom.cl, from programs.conf
  dt_iop_bloom_global_data_t *gd = (dt_iop_bloom_global_data_t *)malloc(sizeof(dt_iop_bloom_global_data_t));
  module->data = gd;
  gd->kernel_bloom_threshold = dt_opencl_create_kernel(program, "bloom_threshold");
  gd->kernel_bloom_hblur = dt_opencl_create_kernel(program, "bloom_hblur");
  gd->kernel_bloom_vblur = dt_opencl_create_kernel(program, "bloom_vblur");
  gd->kernel_bloom_mix = dt_opencl_create_kernel(program, "bloom_mix");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  const dt_iop_bloom_global_data_t *gd = (dt_iop_bloom_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_bloom_threshold);
  dt_opencl_free_kernel(gd->kernel_bloom_hblur);
  dt_opencl_free_kernel(gd->kernel_bloom_vblur);
  dt_opencl_free_kernel(gd->kernel_bloom_mix);
  free(module->data);
  module->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)p1;
  dt_iop_bloom_data_t *d = (dt_iop_bloom_data_t *)piece->data;

  d->strength = p->strength;
  d->size = p->size;
  d->threshold = p->threshold;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_bloom_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_bloom_gui_data_t *g = IOP_GUI_ALLOC(bloom);

  g->size = dt_bauhaus_slider_from_params(self, N_("size"));
  dt_bauhaus_slider_set_format(g->size, "%");
  gtk_widget_set_tooltip_text(g->size, _("the size of bloom"));

  g->threshold = dt_bauhaus_slider_from_params(self, N_("threshold"));
  dt_bauhaus_slider_set_format(g->threshold, "%");
  gtk_widget_set_tooltip_text(g->threshold, _("the threshold of light"));

  g->strength = dt_bauhaus_slider_from_params(self, N_("strength"));
  dt_bauhaus_slider_set_format(g->strength, "%");
  gtk_widget_set_tooltip_text(g->strength, _("the strength of bloom"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

