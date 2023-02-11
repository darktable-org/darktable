/* -*- Mode: c; c-basic-offset: 2; -*- */
/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.


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
#include "common/opencl.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <stdlib.h>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

DT_MODULE_INTROSPECTION(2, dt_iop_colorcontrast_params_t)

typedef struct dt_iop_colorcontrast_params1_t
{
  float a_steepness;
  float a_offset;
  float b_steepness;
  float b_offset;
} dt_iop_colorcontrast_params1_t;

typedef struct dt_iop_colorcontrast_params_t
{
  float a_steepness; // $MIN: 0.0 $MAX: 5.0 $DEFAULT: 1.0 $DESCRIPTION: "Green-magenta contrast"
  float a_offset;
  float b_steepness; // $MIN: 0.0 $MAX: 5.0 $DEFAULT: 1.0 $DESCRIPTION: "Blue-yellow contrast"
  float b_offset;
  int unbound;       // $DEFAULT: 1
} dt_iop_colorcontrast_params_t;

typedef struct dt_iop_colorcontrast_gui_data_t
{
  // whatever you need to make your gui happy.
  // stored in self->gui_data
  GtkBox *vbox;
  GtkWidget *a_scale; // this is needed by gui_update
  GtkWidget *b_scale;
} dt_iop_colorcontrast_gui_data_t;

typedef struct dt_iop_colorcontrast_data_t
{
  // this is stored in the pixelpipeline after a commit (not the db),
  // you can do some precomputation and get this data in process().
  // stored in piece->data
  float a_steepness;
  float a_offset;
  float b_steepness;
  float b_offset;
  int unbound;
} dt_iop_colorcontrast_data_t;

typedef struct dt_iop_colorcontrast_global_data_t
{
  int kernel_colorcontrast;
} dt_iop_colorcontrast_global_data_t;


const char *name()
{
  return _("Color contrast");
}

const char *aliases()
{
  return _("Saturation");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("Increase saturation and separation between\n"
                                        "opposite colors"),
                                      _("Creative"),
                                      _("Non-linear, Lab, display-referred"),
                                      _("Non-linear, Lab"),
                                      _("Non-linear, Lab, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    const dt_iop_colorcontrast_params1_t *old = old_params;
    dt_iop_colorcontrast_params_t *new = new_params;

    new->a_steepness = old->a_steepness;
    new->a_offset = old->a_offset;
    new->b_steepness = old->b_steepness;
    new->b_offset = old->b_offset;
    new->unbound = 0;
    return 0;
  }
  return 1;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in,out:64) aligned(slope,offset,low,high)
#endif
static inline void clamped_scaling(float *const restrict out, const float *const restrict in,
                                   const dt_aligned_pixel_t slope, const dt_aligned_pixel_t offset,
                                   const dt_aligned_pixel_t low, const dt_aligned_pixel_t high)
{
  for_each_channel(c,dt_omp_nontemporal(out))
    out[c] = CLAMPS(in[c] * slope[c] + offset[c], low[c], high[c]);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.

  // get our data struct:
  const dt_iop_colorcontrast_params_t *const d = (dt_iop_colorcontrast_params_t *)piece->data;

  // how many colors in our buffer?
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  const float *const restrict in = DT_IS_ALIGNED((const float *const)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float *const)ovoid);
  const size_t npixels = (size_t)roi_out->width * roi_out->height;

  const dt_aligned_pixel_t slope = { 1.0f, d->a_steepness, d->b_steepness, 1.0f };
  const dt_aligned_pixel_t offset = { 0.0f, d->a_offset, d->b_offset, 0.0f };
  const dt_aligned_pixel_t lowlimit = { -INFINITY, -128.0f, -128.0f, -INFINITY };
  const dt_aligned_pixel_t highlimit = { INFINITY, 128.0f, 128.0f, INFINITY };

  if(d->unbound)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, out, npixels, slope, offset) \
    schedule(static)
#endif
    for(size_t k = 0; k < (size_t)4 * npixels; k += 4)
    {
      for_each_channel(c,dt_omp_nontemporal(out))
      {
        out[k + c] = (in[k + c] * slope[c]) + offset[c];
      }
    }
  }
  else
  {

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, out, npixels, slope, offset, lowlimit, highlimit) \
    schedule(static)
#endif
    for(size_t k = 0; k < npixels; k ++)
    {
      // the inner per-pixel loop needs to be declared in a separate vectorizable function to convince the
      // compiler that it doesn't need to check for overlap or misalignment of the buffers for *every* pixel,
      // which actually makes the code slower than not vectorizing....
      clamped_scaling(out + 4*k, in + 4*k, slope, offset, lowlimit, highlimit);
    }
  }
}

#if defined(__SSE__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
                  void *const restrict ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.

  // get our data struct:
  const dt_iop_colorcontrast_params_t *const d = (dt_iop_colorcontrast_params_t *)piece->data;

  // how many colors in our buffer?
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  const __m128 scale = _mm_set_ps(1.0f, d->b_steepness, d->a_steepness, 1.0f);
  const __m128 offset = _mm_set_ps(0.0f, d->b_offset, d->a_offset, 0.0f);
  const __m128 min = _mm_set_ps(-INFINITY, -128.0f, -128.0f, -INFINITY);
  const __m128 max = _mm_set_ps(INFINITY, 128.0f, 128.0f, INFINITY);

  const float *const restrict in = (float*)ivoid;
  float *const restrict out = (float*)ovoid;

  // iterate over all output pixels (same coordinates as input)
  const int npixels = roi_out->height * roi_out->width;
  if(d->unbound)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, out, offset, npixels, scale) \
  schedule(static)
#endif
    for(int j = 0; j < 4 * npixels; j += 4)
    {
      _mm_stream_ps(out + j, offset + scale * _mm_load_ps(in + j));
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, out, max, min, offset, npixels, scale) \
  schedule(static)
#endif
    for(int j = 0; j < 4 * npixels; j += 4)
    {
      _mm_stream_ps(out + j, _mm_min_ps(max, _mm_max_ps(min, offset + scale * _mm_load_ps(in + j))));
    }
  }
  _mm_sfence();
}
#endif


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorcontrast_data_t *data = (dt_iop_colorcontrast_data_t *)piece->data;
  dt_iop_colorcontrast_global_data_t *gd = (dt_iop_colorcontrast_global_data_t *)self->global_data;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float scale[4] = { 1.0f, data->a_steepness, data->b_steepness, 1.0f };
  const float offset[4] = { 0.0f, data->a_offset, data->b_offset, 0.0f };
  const int unbound = data->unbound;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_colorcontrast, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(scale), CLARG(offset), CLARG(unbound));

  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorcontrast] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif


void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_colorcontrast_global_data_t *gd
      = (dt_iop_colorcontrast_global_data_t *)malloc(sizeof(dt_iop_colorcontrast_global_data_t));
  module->data = gd;
  gd->kernel_colorcontrast = dt_opencl_create_kernel(program, "colorcontrast");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorcontrast_global_data_t *gd = (dt_iop_colorcontrast_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorcontrast);
  free(module->data);
  module->data = NULL;
}


/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)params;
  dt_iop_colorcontrast_data_t *d = (dt_iop_colorcontrast_data_t *)piece->data;
  d->a_steepness = p->a_steepness;
  d->a_offset = p->a_offset;
  d->b_steepness = p->b_steepness;
  d->b_offset = p->b_offset;
  d->unbound = p->unbound;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorcontrast_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_colorcontrast_gui_data_t *g = (dt_iop_colorcontrast_gui_data_t *)self->gui_data;
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)self->params;
  dt_bauhaus_slider_set(g->a_scale, p->a_steepness);
  dt_bauhaus_slider_set(g->b_scale, p->b_steepness);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_colorcontrast_gui_data_t *g = IOP_GUI_ALLOC(colorcontrast);

  g->a_scale = dt_bauhaus_slider_from_params(self, "a_steepness");
  gtk_widget_set_tooltip_text(g->a_scale, _("Steepness of the a* curve in Lab\nlower values desaturate greens and magenta while higher saturate them"));

  g->b_scale = dt_bauhaus_slider_from_params(self, "b_steepness");
  gtk_widget_set_tooltip_text(g->b_scale, _("Steepness of the b* curve in Lab\nlower values desaturate blues and yellows while higher saturate them"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
