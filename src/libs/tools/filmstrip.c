/*
    This file is part of darktable,
    Copyright (C) 2011-2025 darktable developers.

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

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE
    | DT_VIEW_DARKROOM
    | DT_VIEW_TETHERING
    | DT_VIEW_MAP
    | DT_VIEW_PRINT;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_BOTTOM;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 1001;
}

static gboolean _lib_filmstrip_draw_callback(GtkWidget *widget,
                                             cairo_t *wcr,
                                             gpointer user_data)
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

static void _filmstrip_pin_in_second_window(dt_action_t *action)
{
  if(dt_view_get_current() != DT_VIEW_DARKROOM) return;

  dt_develop_t *dev = darktable.develop;
  if(!dev) return;

  // Use the hovered filmstrip image; fall back to the currently edited image
  dt_imgid_t imgid = dt_control_get_mouse_over_id();
  if(!dt_is_valid_imgid(imgid))
    imgid = dev->image_storage.id;
  if(!dt_is_valid_imgid(imgid)) return;

  // Open the 2nd window if it is not already visible
  if(!dev->second_wnd && dev->second_wnd_button)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->second_wnd_button), TRUE);

  dt_dev_pin_image(dev, imgid);
}

void gui_init(dt_lib_module_t *self)
{
  /* creating container area */
  self->widget = gtk_event_box_new();

  /* connect callbacks */
  g_signal_connect(G_OBJECT(self->widget), "draw",
                   G_CALLBACK(_lib_filmstrip_draw_callback), self);

  /* initialize view manager proxy */
  darktable.view_manager->proxy.filmstrip.module = self;


  /* register action and attach it to self->widget so the quick-shortcut
     button can discover it by hovering anywhere over the filmstrip */
  dt_action_register(DT_ACTION(self), N_("pin in second window"),
                     _filmstrip_pin_in_second_window, 0, 0);
  dt_action_define(DT_ACTION(self), NULL, N_("pin in second window"),
                   self->widget, NULL);
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
