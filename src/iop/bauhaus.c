/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "gui/presets.h"
#include "bauhaus/bauhaus.h"

#include <sys/select.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)




typedef struct dt_iop_bauhaus_params_t
{
  int nothing;
}
dt_iop_bauhaus_params_t;

typedef struct dt_iop_bauhaus_gui_data_t
{
  GtkWidget *combobox;
  GtkWidget *slider;
  GtkWidget *slider2;
}
dt_iop_bauhaus_gui_data_t;

typedef struct dt_iop_bauhaus_data_t
{
}
dt_iop_bauhaus_data_t;

const char *name()
{
  return _("bauhaus controls test");
}

int
groups ()
{
  return IOP_GROUP_BASIC;
}


void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  memcpy(o, i, sizeof(float)*4*roi_in->width*roi_in->height);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void gui_update(struct dt_iop_module_t *self)
{
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_bauhaus_params_t));
  module->default_params = malloc(sizeof(dt_iop_bauhaus_params_t));
  module->default_enabled = 0;
  module->priority = 245; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_bauhaus_params_t);
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void
value_changed(GtkWidget *widget, gpointer user_data)
{
  fprintf(stderr, "value changed to %f!\n", dt_bauhaus_slider_get(widget));
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_bauhaus_gui_data_t));
  dt_iop_bauhaus_gui_data_t *c = (dt_iop_bauhaus_gui_data_t *)self->gui_data;

  self->widget = gtk_vbox_new(TRUE, 15);//DT_GUI_IOP_MODULE_CONTROL_SPACING);

  c->slider = dt_bauhaus_slider_new(self);
  gtk_box_pack_start(GTK_BOX(self->widget), c->slider, TRUE, TRUE, 0);

  c->slider2 = dt_bauhaus_slider_new(self);
  gtk_box_pack_start(GTK_BOX(self->widget), c->slider2, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(c->slider2), "value-changed", G_CALLBACK(value_changed), (gpointer)NULL);

  c->combobox = dt_bauhaus_combobox_new(self);
  gtk_box_pack_start(GTK_BOX(self->widget), c->combobox, TRUE, TRUE, 0);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  // dt_iop_bauhaus_gui_data_t *c = (dt_iop_bauhaus_gui_data_t *)self->gui_data;
  // TODO: need to clean up bauhaus structs!
  free(self->gui_data);
  self->gui_data = NULL;
}

