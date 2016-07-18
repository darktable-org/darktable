/*
 *  This file is part of darktable,
 *  copyright (c) 2016 Wolfgang Mader.
 *
 *  darktable is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  darktable is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_bw_params_t)

typedef enum _iop_operator_t
{
  OPERATOR_LIGHTNESS,
  OPERETOR_APPARENT_GRAYSCALE
} _iop_operator_t;

typedef struct dt_iop_bw_params_t
{
  _iop_operator_t operator;
} dt_iop_bw_params_t;

//-> Why do other modules, e.g., global tonemap use
//->    dt_iop_global_tonemap_data_t
//-> when it holds the same data as
//->    dt_iop_global_tonemap_params_t
//-> already does? Does any change to params_t trigger a write to database?

typedef struct dt_iop_bw_gui_data_t
{
  GtkWidget *operator;
} dt_iop_bw_gui_data_t;

typedef struct dt_iop_bw_global_data_t
{
} dt_iop_bw_global_data_t;

const char *name()
{
  return _("modern monochrome");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int groups()
{
  return IOP_GROUP_COLOR;
}

static inline void process_lightness(dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
                                     const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const int ch = piece->colors;

  for(int j = 0; j < roi_out->height; ++j)
  {
    float *in = ((float *)i) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)o) + (size_t)ch * roi_out->width * j;
    for(int i = 0; i < roi_out->width; ++i)
    {
      out[1] = 0;
      out[2] = 0;

      in += ch;
      out += ch;
    }
  }
}

static inline void process_apparent_grayscale(dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
                                     const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const int ch = piece->colors;

  for(int j = 0; j < roi_out->height; ++j)
  {
    float *in = ((float *)i) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)o) + (size_t)ch * roi_out->width * j;
    for(int i = 0; i < roi_out->width; ++i)
    {
      // Just a dummy implementation in order to see s.th. while testing the operator combobox
      out[1] = in[2];
      out[2] =in[1];

      in += ch;
      out += ch;
    }
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_bw_params_t *d = (dt_iop_bw_params_t *)piece->data;

  switch(d->operator)
  {
    case OPERATOR_LIGHTNESS:
      process_lightness(piece, i, o, roi_in, roi_out);
      break;

    case OPERETOR_APPARENT_GRAYSCALE:
      process_apparent_grayscale(piece, i, o, roi_in, roi_out);
      break;
  }
}

/** optional: if this exists, it will be called to init new defaults if a new image is loaded from film strip
 * mode. */
void reload_defaults(dt_iop_module_t *module)
{
  // change default_enabled depending on type of image, or set new default_params even.

  // if this callback exists, it has to write default_params and default_enabled.
}

void init(dt_iop_module_t *module)
{
  // we don't need global data:
  module->data = NULL; // malloc(sizeof(dt_iop_bw_global_data_t));
  module->params = calloc(1, sizeof(dt_iop_bw_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_bw_params_t));
  module->default_enabled = 0;
  module->priority = 630; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_bw_params_t);
  module->gui_data = NULL;
  //-> Why do we set module-data and module-gui_data to NULL when we then initialize them in
  //-> init_global() and init_gui()?
  // init defaults:
  dt_iop_bw_params_t tmp = (dt_iop_bw_params_t){ OPERATOR_LIGHTNESS };

  memcpy(module->params, &tmp, sizeof(dt_iop_bw_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_bw_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  module->data = malloc(sizeof(dt_iop_bw_global_data_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  free(module->data);
  module->data = NULL;
}

/** gui callbacks */
static void operator_callback(GtkWidget *combobox, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_bw_params_t *p = (dt_iop_bw_params_t *)self->params;
  p->operator= dt_bauhaus_combobox_get(combobox);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_bw_gui_data_t *g = (dt_iop_bw_gui_data_t *)self->gui_data;
  dt_iop_bw_params_t *p = (dt_iop_bw_params_t *)self->params;
  dt_bauhaus_combobox_set(g->operator, p->operator);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_bw_gui_data_t));
  dt_iop_bw_gui_data_t *g = (dt_iop_bw_gui_data_t *)self->gui_data;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  /* operator */
  g->operator= dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->operator, NULL, _("operator"));

  dt_bauhaus_combobox_add(g->operator, "lightness");
  dt_bauhaus_combobox_add(g->operator, "apparent grayscale");

  gtk_widget_set_tooltip_text(g->operator, _("method for conversion to grayscale"));
  g_signal_connect(G_OBJECT(g->operator), "value-changed", G_CALLBACK(operator_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->operator), TRUE, TRUE, 0);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
