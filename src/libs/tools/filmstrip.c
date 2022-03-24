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

#include "common/darktable.h"
#include "common/debug.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"

#include <gdk/gdkkeysyms.h>

/**
 * This module is merely just a simple container
 * which can contains thumbtable widget
 *
 * all the stuff is located in the thumbtable and its thumbnails childs
 */

DT_MODULE(1)

const char *name(dt_lib_module_t *self)
{
  return _("filmstrip");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", "darkroom", "tethering", "map", "print", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_BOTTOM;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}

static gboolean _lib_filmstrip_draw_callback(GtkWidget *widget, cairo_t *wcr, gpointer user_data)
{
  // we only ensure that the thumbtable is inside our container
  if(!gtk_bin_get_child(GTK_BIN(widget)))
  {
    dt_thumbtable_t *tt = dt_ui_thumbtable(darktable.gui->ui);
    dt_thumbtable_set_parent(tt, widget, DT_THUMBTABLE_MODE_FILMSTRIP);
    gtk_widget_show(widget);
    gtk_widget_show(tt->widget);
    gtk_widget_queue_draw(tt->widget);
  }
  return FALSE;
}

void gui_init(dt_lib_module_t *self)
{
  /* creating container area */
  self->widget = gtk_event_box_new();

  /* connect callbacks */
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(_lib_filmstrip_draw_callback), self);

  /* initialize view manager proxy */
  darktable.view_manager->proxy.filmstrip.module = self;
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* unset viewmanager proxy */
  darktable.view_manager->proxy.filmstrip.module = NULL;

  /* cleanup */
  free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

