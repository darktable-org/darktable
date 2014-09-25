/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.

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
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "bauhaus/bauhaus.h"
#include "common/bilateral.h"
#include "common/bilateralcl.h"
#include "gui/gtk.h"

#include <gtk/gtk.h>
#include <stdlib.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(1, dt_iop_bilat_params_t)

typedef struct dt_iop_bilat_params_t
{
  float sigma_r;
  float sigma_s;
  float detail;
} dt_iop_bilat_params_t;

typedef struct dt_iop_bilat_data_t
{
  float sigma_r;
  float sigma_s;
  float detail;
} dt_iop_bilat_data_t;

typedef struct dt_iop_bilat_gui_data_t
{
  GtkWidget *spatial;
  GtkWidget *range;
  GtkWidget *detail;
} dt_iop_bilat_gui_data_t;

typedef struct dt_iop_bilat_global_data_t
{
  // we don't need it for this example (as for most dt plugins)
} dt_iop_bilat_global_data_t;

// this returns a translatable name
const char *name()
{
  return _("local contrast");
}

// some additional flags (self explanatory i think):
int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

// where does it appear in the gui?
int groups()
{
  return IOP_GROUP_TONE;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_bilat_data_t *d = (dt_iop_bilat_data_t *)piece->data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.
  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = d->sigma_r; // does not depend on scale
  const float sigma_s = d->sigma_s / scale;
  cl_int err = -666;

  dt_bilateral_cl_t *b
      = dt_bilateral_init_cl(piece->pipe->devid, roi_in->width, roi_in->height, sigma_s, sigma_r);
  if(!b) goto error;
  err = dt_bilateral_splat_cl(b, dev_in);
  if(err != CL_SUCCESS) goto error;
  err = dt_bilateral_blur_cl(b);
  if(err != CL_SUCCESS) goto error;
  err = dt_bilateral_slice_cl(b, dev_in, dev_out, d->detail);
  if(err != CL_SUCCESS) goto error;
  dt_bilateral_free_cl(b);
  return TRUE;
error:
  dt_bilateral_free_cl(b);
  dt_print(DT_DEBUG_OPENCL, "[opencl_bilateral] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_bilat_data_t *d = (dt_iop_bilat_data_t *)piece->data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.
  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = d->sigma_r;
  const float sigma_s = d->sigma_s / scale;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const size_t basebuffer = width * height * channels * sizeof(float);

  tiling->factor = 2.0f + (float)dt_bilateral_memory_use(width, height, sigma_s, sigma_r) / basebuffer;
  tiling->maxbuf
      = fmax(1.0f, (float)dt_bilateral_singlebuffer_size(width, height, sigma_s, sigma_r) / basebuffer);
  tiling->overhead = 0;
  tiling->overlap = ceilf(4 * sigma_s);
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)p1;
  dt_iop_bilat_data_t *d = (dt_iop_bilat_data_t *)piece->data;

  d->sigma_r = p->sigma_r;
  d->sigma_s = p->sigma_s;
  d->detail = p->detail;

#ifdef HAVE_OPENCL
  piece->process_cl_ready = (piece->process_cl_ready && !(darktable.opencl->avoid_atomics));
#endif
}


void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_bilat_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}


void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


/** process, all real work is done here. */
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_bilat_data_t *d = (dt_iop_bilat_data_t *)piece->data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.
  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = d->sigma_r; // does not depend on scale
  const float sigma_s = d->sigma_s / scale;

  // TODO: better memory management.
  dt_bilateral_t *b = dt_bilateral_init(roi_in->width, roi_in->height, sigma_s, sigma_r);
  dt_bilateral_splat(b, (float *)i);
  dt_bilateral_blur(b);
  dt_bilateral_slice(b, (float *)i, (float *)o, d->detail);
  dt_bilateral_free(b);
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  // we don't need global data:
  module->data = malloc(sizeof(dt_iop_bilat_global_data_t));
  module->params = malloc(sizeof(dt_iop_bilat_params_t));
  module->default_params = malloc(sizeof(dt_iop_bilat_params_t));
  // our module is disabled by default
  // by default:
  module->default_enabled = 0;
  // order has to be changed by editing the dependencies in tools/iop_dependencies.py
  module->priority = 566; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_bilat_params_t);
  module->gui_data = NULL;
  // init defaults:
  dt_iop_bilat_params_t tmp = (dt_iop_bilat_params_t){ 20, 50, 0.2 };

  memcpy(module->params, &tmp, sizeof(dt_iop_bilat_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_bilat_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL; // just to be sure
  free(module->params);
  module->params = NULL;
  free(module->data); // just to be sure
  module->data = NULL;
}

static void spatial_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)self->params;
  p->sigma_s = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void range_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)self->params;
  p->sigma_r = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void detail_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)self->params;
  p->detail = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/** gui callbacks, these are needed. */
void gui_update(dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_bilat_gui_data_t *g = (dt_iop_bilat_gui_data_t *)self->gui_data;
  dt_iop_bilat_params_t *p = (dt_iop_bilat_params_t *)self->params;
  dt_bauhaus_slider_set(g->spatial, p->sigma_s);
  dt_bauhaus_slider_set(g->range, p->sigma_r);
  dt_bauhaus_slider_set(g->detail, p->detail);
}

void gui_init(dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_bilat_gui_data_t));
  dt_iop_bilat_gui_data_t *g = (dt_iop_bilat_gui_data_t *)self->gui_data;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->spatial = dt_bauhaus_slider_new_with_range(self, 1, 100, 1, 50, 0);
  dt_bauhaus_widget_set_label(g->spatial, NULL, _("coarseness"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->spatial, TRUE, TRUE, 0);

  g->range = dt_bauhaus_slider_new_with_range(self, 1, 100, 1, 20, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->range, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->range, NULL, _("contrast"));

  g->detail = dt_bauhaus_slider_new_with_range(self, -1.0, 2.0, 0.01, 0.2, 3);
  gtk_box_pack_start(GTK_BOX(self->widget), g->detail, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->detail, NULL, _("detail"));

  g_signal_connect(G_OBJECT(g->spatial), "value-changed", G_CALLBACK(spatial_callback), self);
  g_signal_connect(G_OBJECT(g->range), "value-changed", G_CALLBACK(range_callback), self);
  g_signal_connect(G_OBJECT(g->detail), "value-changed", G_CALLBACK(detail_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
