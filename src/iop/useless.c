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
#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(2, dt_iop_useless_params_t)

// TODO: some build system to support dt-less compilation and translation!

typedef struct dt_iop_useless_params_t
{
  // these are stored in db.
  // make sure everything is in here does not
  // depend on temporary memory (pointers etc)
  // stored in self->params and self->default_params
  // also, since this is stored in db, you should keep changes to this struct
  // to a minimum. if you have to change this struct, it will break
  // users data bases, and you should increment the version
  // of DT_MODULE(VERSION) above!
  int checker_scale;
  float factor;
} dt_iop_useless_params_t;

typedef struct dt_iop_useless_gui_data_t
{
  // whatever you need to make your gui happy.
  // stored in self->gui_data
  GtkWidget *scale, *factor; // this is needed by gui_update
} dt_iop_useless_gui_data_t;

typedef struct dt_iop_useless_global_data_t
{
  // this is optionally stored in self->global_data
  // and can be used to alloc globally needed stuff
  // which is needed in gui mode and during processing.

  // we don't need it for this example (as for most dt plugins)
} dt_iop_useless_global_data_t;

// this returns a translatable name
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("silly example");
}

// some additional flags (self explanatory i think):
int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

// where does it appear in the gui?
int default_group()
{
  return IOP_GROUP_BASIC;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_Lab;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_iop_useless_params_v1_t
    {
      int checker_scale;
    } dt_iop_useless_params_v1_t;

    dt_iop_useless_params_v1_t *o = (dt_iop_useless_params_v1_t *)old_params;
    dt_iop_useless_params_t *n = (dt_iop_useless_params_t *)new_params;

    n->checker_scale = o->checker_scale;
    n->factor = 0.0;
    return 0;
  }
  return 1;
}

static const int mask_id = 1; // key "0" is reserved for the pipe
static const char *mask_name = "useless checkerboard";

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, p1, self->params_size);

  // there is no real need for this, but if the number of masks can be changed by the user this is the way to go.
  // otherwise we can have old stale masks floating around
  g_hash_table_remove_all(self->raster_mask.source.masks);
  g_hash_table_insert(self->raster_mask.source.masks, GINT_TO_POINTER(mask_id), g_strdup(mask_name));
}

/** modify regions of interest (optional, per pixel ops don't need this) */
// void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t
// *roi_out, const dt_iop_roi_t *roi_in);
// void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t
// *roi_out, dt_iop_roi_t *roi_in);
// void distort_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
// float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out);

/** process, all real work is done here. */
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_useless_params_t *d = (dt_iop_useless_params_t *)piece->data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.
  const float scale = piece->iscale / roi_in->scale;
  // how many colors in our buffer?
  const int ch = piece->colors;

  // we create a raster mask as an example
  float *mask = NULL;
  if(piece->pipe->store_all_raster_masks || dt_iop_is_raster_mask_used(piece->module, mask_id))
  {
    mask = (float *)dt_alloc_align(64, (size_t)roi_out->width * roi_out->height * sizeof(float));
    memset(mask, 0, sizeof(float) * roi_out->width * roi_out->height);
  }
  else
    g_hash_table_remove(piece->raster_masks, GINT_TO_POINTER(mask_id));

// iterate over all output pixels (same coordinates as input)
#ifdef _OPENMP
// optional: parallelize it!
#pragma omp parallel for default(none) schedule(static) shared(d) dt_omp_firstprivate(scale, ivoid, ovoid, roi_in, roi_out, ch, mask)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = ((float *)ivoid)
                + (size_t)ch * roi_in->width
                  * j; // make sure to address input, output and temp buffers with size_t as we want to also
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j; // correctly handle huge images
    float *out_mask = mask ? &(mask[(size_t)roi_out->width * j]) : NULL;
    for(int i = 0; i < roi_out->width; i++)
    {
      // calculate world space coordinates:
      int wi = (roi_in->x + i) * scale, wj = (roi_in->y + j) * scale;
      if((wi / d->checker_scale + wj / d->checker_scale) & 1)
      {
        for(int c = 0; c < 3; c++) out[c] = in[c] * (1.0 - d->factor);
        if(out_mask) out_mask[i] = 1.0;
      }
      else
        for(int c = 0; c < 3; c++) out[c] = in[c];
      in += ch;
      out += ch;
    }
  }

  // now that the mask is generated we can publish it
  if(mask)
    g_hash_table_replace(piece->raster_masks, GINT_TO_POINTER(mask_id), mask);
}

/** optional: if this exists, it will be called to init new defaults if a new image is loaded from film strip
 * mode. */
void reload_defaults(dt_iop_module_t *module)
{
  // change default_enabled depending on type of image, or set new default_params even.

  // if this callback exists, it has to write default_params and default_enabled.
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  // we don't need global data:
  module->data = NULL; // malloc(sizeof(dt_iop_useless_global_data_t));
  module->params = calloc(1, sizeof(dt_iop_useless_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_useless_params_t));
  // our module is disabled by default
  // by default:
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_useless_params_t);
  module->gui_data = NULL;
  // init defaults:
  dt_iop_useless_params_t tmp = (dt_iop_useless_params_t){ .checker_scale = 50, .factor = 0.5 };

  memcpy(module->params, &tmp, sizeof(dt_iop_useless_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_useless_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  module->data = malloc(sizeof(dt_iop_useless_global_data_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  free(module->data);
  module->data = NULL;
}

/** put your local callbacks here, be sure to make them static so they won't be visible outside this file! */
static void scale_callback(GtkWidget *w, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_useless_params_t *p = (dt_iop_useless_params_t *)self->params;
  p->checker_scale = dt_bauhaus_slider_get(w);
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void factor_callback(GtkWidget *w, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_useless_params_t *p = (dt_iop_useless_params_t *)self->params;
  p->factor = dt_bauhaus_slider_get(w);
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/** gui callbacks, these are needed. */
void gui_update(dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_useless_gui_data_t *g = (dt_iop_useless_gui_data_t *)self->gui_data;
  dt_iop_useless_params_t *p = (dt_iop_useless_params_t *)self->params;
  dt_bauhaus_slider_set(g->scale, p->checker_scale);
  dt_bauhaus_slider_set(g->factor, p->factor);
}

void gui_init(dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_useless_gui_data_t));
  dt_iop_useless_gui_data_t *g = (dt_iop_useless_gui_data_t *)self->gui_data;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->scale = dt_bauhaus_slider_new_with_range(self, 1, 100, 1, 50, 0);
  dt_bauhaus_widget_set_label(g->scale, NULL, _("size"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->scale), "value-changed", G_CALLBACK(scale_callback), self);

  g->factor = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.1, 0.5, 2);
  dt_bauhaus_widget_set_label(g->factor, NULL, _("factor"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->factor), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->factor), "value-changed", G_CALLBACK(factor_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

/** additional, optional callbacks to capture darkroom center events. */
// void gui_post_expose(dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
// int32_t pointery);
// int mouse_moved(dt_iop_module_t *self, double x, double y, double pressure, int which);
// int button_pressed(dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
// uint32_t state);
// int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
// int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
