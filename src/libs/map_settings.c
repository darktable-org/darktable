/*
    This file is part of darktable,
    Copyright (C) 2012-2021 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "gui/preferences.h"
#include <gdk/gdkkeysyms.h>

#include <osm-gps-map-source.h>

DT_MODULE(1)

const char *name(dt_lib_module_t *self)
{
  return _("map settings");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_MAP;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

typedef struct dt_lib_map_settings_t
{
  GtkWidget *show_osd_checkbutton, *filtered_images_checkbutton, *map_source_dropdown;
  GtkWidget *images_thumb, *max_images_entry, *epsilon_factor, *min_images, *max_outline_nodes;
} dt_lib_map_settings_t;

int position(const dt_lib_module_t *self)
{
  return 990;
}

static void _show_osd_toggled(GtkToggleButton *button, gpointer data)
{
  dt_view_map_show_osd(darktable.view_manager);
}

static void _parameter_changed(GtkToggleButton *button, gpointer data)
{
  if(darktable.view_manager->proxy.map.view)
  {
    darktable.view_manager->proxy.map.redraw(darktable.view_manager->proxy.map.view);
  }
}

static void _map_source_changed(GtkWidget *widget, gpointer data)
{
  dt_view_map_set_map_source(darktable.view_manager,
                             GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(widget)));
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_map_settings_t *d = malloc(sizeof(dt_lib_map_settings_t));
  self->data = d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  d->map_source_dropdown = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->map_source_dropdown, NULL, N_("map source"));
  gtk_widget_set_tooltip_text(d->map_source_dropdown, _("select the source of the map. some entries might not work"));

  const char *map_source = dt_conf_get_string_const("plugins/map/map_source");
  int selection = OSM_GPS_MAP_SOURCE_OPENSTREETMAP - 1, entry = 0;
  for(int i = 1; i < OSM_GPS_MAP_SOURCE_LAST; i++)
  {
    if(osm_gps_map_source_is_valid(i))
    {
      const gchar *name = osm_gps_map_source_get_friendly_name(i);
      dt_bauhaus_combobox_add_full(d->map_source_dropdown, name,
                                   DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                   GINT_TO_POINTER(i), NULL, TRUE);
      if(!g_strcmp0(name, map_source)) selection = entry;
      entry++;
    }
  }
  dt_bauhaus_combobox_set(d->map_source_dropdown, selection);
  g_signal_connect(G_OBJECT(d->map_source_dropdown), "value-changed", G_CALLBACK(_map_source_changed), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->map_source_dropdown), FALSE, TRUE, 0);

  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));

  int line = 0;
  d->max_outline_nodes = dt_gui_preferences_int(grid, "plugins/map/max_outline_nodes", 0, line++);
  d->show_osd_checkbutton = dt_gui_preferences_bool(grid, "plugins/map/show_map_osd", 0, line++, FALSE);
  g_signal_connect(G_OBJECT(d->show_osd_checkbutton), "toggled", G_CALLBACK(_show_osd_toggled), NULL);
  d->filtered_images_checkbutton = dt_gui_preferences_bool(grid, "plugins/map/filter_images_drawn", 0, line++, FALSE);
  g_signal_connect(G_OBJECT(d->filtered_images_checkbutton), "toggled", G_CALLBACK(_parameter_changed), NULL);
  dt_shortcut_register(dt_action_define(DT_ACTION(self), NULL, N_("filtered images"),
                                        d->filtered_images_checkbutton, &dt_action_def_button),
                       0, 0, GDK_KEY_s, GDK_CONTROL_MASK);
  d->max_images_entry = dt_gui_preferences_int(grid, "plugins/map/max_images_drawn", 0, line++);
  g_signal_connect(G_OBJECT(d->max_images_entry), "value-changed", G_CALLBACK(_parameter_changed), self);
  d->epsilon_factor = dt_gui_preferences_int(grid, "plugins/map/epsilon_factor", 0, line++);
  g_signal_connect(G_OBJECT(d->epsilon_factor), "value-changed", G_CALLBACK(_parameter_changed), self);
  d->min_images = dt_gui_preferences_int(grid, "plugins/map/min_images_per_group", 0, line++);
  g_signal_connect(G_OBJECT(d->min_images), "value-changed", G_CALLBACK(_parameter_changed), self);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(grid), FALSE, TRUE, 0);

  d->images_thumb = dt_gui_preferences_enum(DT_ACTION(self), "plugins/map/images_thumbnail");
  dt_action_t *ac = dt_bauhaus_widget_set_label(d->images_thumb, NULL, dt_confgen_get_label("plugins/map/images_thumbnail"));
  dt_shortcut_register(ac, 0, 0, GDK_KEY_s, GDK_SHIFT_MASK);
  g_signal_connect(G_OBJECT(d->images_thumb), "value-changed", G_CALLBACK(_parameter_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), d->images_thumb, FALSE, TRUE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_map_settings_t *d = self->data;
  dt_gui_preferences_bool_reset(d->show_osd_checkbutton);
  dt_gui_preferences_bool_reset(d->filtered_images_checkbutton);
  dt_gui_preferences_int_reset(d->max_outline_nodes);
  dt_gui_preferences_int_reset(d->max_images_entry);
  dt_gui_preferences_int_reset(d->epsilon_factor);
  dt_gui_preferences_int_reset(d->min_images);
  dt_bauhaus_widget_reset(d->images_thumb);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
