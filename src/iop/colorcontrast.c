/* -*- Mode: c; c-basic-offset: 2; -*- */
/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika
    copyright (c) 2011 Sergey Astanin
    copyright (c) 2012 Henrik Andersson
    copyright (c) 2014 Ulrich Pegelow


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
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"
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
  float a_steepness;
  float a_offset;
  float b_steepness;
  float b_offset;
  int unbound;
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
  return _("color contrast");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int groups()
{
  return dt_iop_get_group("color contrast", IOP_GROUP_COLOR);
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

#if 0 // BAUHAUS doesn't support keyaccels yet...
void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "green vs magenta"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "blue vs yellow"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_colorcontrast_gui_data_t *g =
    (dt_iop_colorcontrast_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "green vs magenta",
                              GTK_WIDGET(g->a_scale));
  dt_accel_connect_slider_iop(self, "blue vs yellow",
                              GTK_WIDGET(g->b_scale));
}

#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  assert(dt_iop_module_colorspace(self) == iop_cs_Lab);

  // get our data struct:
  const dt_iop_colorcontrast_params_t *const d = (dt_iop_colorcontrast_params_t *)piece->data;

  // how many colors in our buffer?
  const int ch = piece->colors;

  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  if(d->unbound)
  {
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) schedule(static)
#endif
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
    {
      out[k + 1] = (in[k + 1] * d->a_steepness) + d->a_offset;
      out[k + 2] = (in[k + 2] * d->b_steepness) + d->b_offset;
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) schedule(static)
#endif
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
    {
      out[k + 1] = CLAMP((in[k + 1] * d->a_steepness) + d->a_offset, -128.0f, 128.0f);
      out[k + 2] = CLAMP((in[k + 2] * d->b_steepness) + d->b_offset, -128.0f, 128.0f);
    }
  }
}

#if defined(__SSE__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                  void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  assert(dt_iop_module_colorspace(self) == iop_cs_Lab);

  // get our data struct:
  dt_iop_colorcontrast_params_t *d = (dt_iop_colorcontrast_params_t *)piece->data;

  // how many colors in our buffer?
  const int ch = piece->colors;

  const int unbound = d->unbound;

// iterate over all output pixels (same coordinates as input)
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(d)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {

    float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;

    const __m128 scale = _mm_set_ps(1.0f, d->b_steepness, d->a_steepness, 1.0f);
    const __m128 offset = _mm_set_ps(0.0f, d->b_offset, d->a_offset, 0.0f);
    const __m128 min = _mm_set_ps(-INFINITY, -128.0f, -128.0f, -INFINITY);
    const __m128 max = _mm_set_ps(INFINITY, 128.0f, 128.0f, INFINITY);

    if(unbound)
    {
      for(int i = 0; i < roi_out->width; i++)
      {
        _mm_stream_ps(out, _mm_add_ps(offset, _mm_mul_ps(scale, _mm_load_ps(in))));
        in += ch;
        out += ch;
      }
    }
    else
    {
      for(int i = 0; i < roi_out->width; i++)
      {
        _mm_stream_ps(
            out, _mm_min_ps(max, _mm_max_ps(min, _mm_add_ps(offset, _mm_mul_ps(scale, _mm_load_ps(in))))));
        in += ch;
        out += ch;
      }
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
  dt_iop_colorcontrast_global_data_t *gd = (dt_iop_colorcontrast_global_data_t *)self->data;
  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float scale[4] = { 1.0f, data->a_steepness, data->b_steepness, 1.0f };
  const float offset[4] = { 0.0f, data->a_offset, data->b_offset, 0.0f };
  const int unbound = data->unbound;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcontrast, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcontrast, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcontrast, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcontrast, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcontrast, 4, 4 * sizeof(float), (void *)&scale);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcontrast, 5, 4 * sizeof(float), (void *)&offset);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorcontrast, 6, sizeof(int), (void *)&unbound);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorcontrast, sizes);

  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorcontrast] couldn't enqueue kernel! %d\n", err);
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


void reload_defaults(dt_iop_module_t *module)
{
  // change default_enabled depending on type of image, or set new default_params even.
  // if this callback exists, it has to write default_params and default_enabled.
  dt_iop_colorcontrast_params_t tmp = (dt_iop_colorcontrast_params_t){ 1.0, 0.0, 1.0, 0.0, 1 };
  memcpy(module->params, &tmp, sizeof(dt_iop_colorcontrast_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorcontrast_params_t));
  module->default_enabled = 0;
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_colorcontrast_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_colorcontrast_params_t));
  // our module is disabled by default
  module->default_enabled = 0;
  // we are pretty late in the pipe:
  module->priority = 799; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_colorcontrast_params_t);
  module->gui_data = NULL;
  // init defaults:
  dt_iop_colorcontrast_params_t tmp = (dt_iop_colorcontrast_params_t){ 1.0, 0.0, 1.0, 0.0, 1 };
  memcpy(module->params, &tmp, sizeof(dt_iop_colorcontrast_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorcontrast_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
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
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

/** put your local callbacks here, be sure to make them static so they won't be visible outside this file! */
static void a_slider_callback(GtkRange *range, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_colorcontrast_gui_data_t *g = (dt_iop_colorcontrast_gui_data_t *)self->gui_data;
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)self->params;
  p->a_steepness = dt_bauhaus_slider_get(g->a_scale);
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void b_slider_callback(GtkRange *range, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_colorcontrast_gui_data_t *g = (dt_iop_colorcontrast_gui_data_t *)self->gui_data;
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)self->params;
  p->b_steepness = dt_bauhaus_slider_get(g->b_scale);
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/** gui callbacks, these are needed. */
void gui_update(dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_colorcontrast_gui_data_t *g = (dt_iop_colorcontrast_gui_data_t *)self->gui_data;
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)self->params;
  dt_bauhaus_slider_set(g->a_scale, p->a_steepness);
  dt_bauhaus_slider_set(g->b_scale, p->b_steepness);
}

void gui_init(dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_colorcontrast_gui_data_t));
  dt_iop_colorcontrast_gui_data_t *g = (dt_iop_colorcontrast_gui_data_t *)self->gui_data;
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  /* a scale */
  g->a_scale = dt_bauhaus_slider_new_with_range(self, 0.0, 5.0, 0.01, p->a_steepness, 2);
  dt_bauhaus_widget_set_label(g->a_scale, NULL, _("green vs magenta"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->a_scale), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->a_scale, _("steepness of the a* curve in Lab"));
  g_signal_connect(G_OBJECT(g->a_scale), "value-changed", G_CALLBACK(a_slider_callback), self);


  /* b scale */
  g->b_scale = dt_bauhaus_slider_new_with_range(self, 0.0, 5.0, 0.01, p->b_steepness, 2);
  dt_bauhaus_widget_set_label(g->b_scale, NULL, _("blue vs yellow"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->b_scale), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->b_scale, _("steepness of the b* curve in Lab"));
  g_signal_connect(G_OBJECT(g->b_scale), "value-changed", G_CALLBACK(b_slider_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the sliders.
  free(self->gui_data);
  self->gui_data = NULL;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
