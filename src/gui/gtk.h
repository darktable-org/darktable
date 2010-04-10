/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DT_GUI_GTK_H
#define DT_GUI_GTK_H

#include <gtk/gtk.h>
#include <glade/glade.h>
#include "gui/navigation.h"
#include "gui/histogram.h"

typedef struct dt_gui_key_accel_t
{
  guint   state;
  guint   keyval;
  guint16 hardware_keycode;
  void (*callback)(void *);
  void *data;
}
dt_gui_key_accel_t;

typedef struct dt_gui_snapshot_t
{
  float zoom_x, zoom_y, zoom_scale;
  int32_t zoom, closeup;
  char filename[30];
}
dt_gui_snapshot_t;

typedef struct dt_gui_gtk_t
{
  GladeXML *main_window;
  GdkPixmap *pixmap;
  GList *redraw_widgets;
  GList *key_accels;
  dt_gui_navigation_t navigation;
  dt_gui_histogram_t histogram;

  int32_t num_snapshots, request_snapshot, selected_snapshot;
  dt_gui_snapshot_t snapshot[4];
  cairo_surface_t *snapshot_image;

  int32_t reset;
  float bgcolor[3];
}
dt_gui_gtk_t;

int dt_gui_gtk_init(dt_gui_gtk_t *gui, int argc, char *argv[]);
void dt_gui_gtk_run(dt_gui_gtk_t *gui);
void dt_gui_gtk_cleanup(dt_gui_gtk_t *gui);

/** register an accel callback for the whole window. data is not freed on unregister. */
void dt_gui_key_accel_register(guint state, guint keyval, void (*callback)(void *), void *data);
void dt_gui_key_accel_unregister(void (*callback)(void *));

#endif
