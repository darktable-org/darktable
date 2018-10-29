/*
    This file is part of darktable,
    copyright (c) 2012 tobias ellinghaus.

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
#include <gdk/gdkkeysyms.h>

#include <osm-gps-map-source.h>

DT_MODULE(1)

const char *name(dt_lib_module_t *self)
{
  return _("map settings");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"map", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

typedef struct dt_lib_map_settings_t
{
  GtkWidget *show_osd_checkbutton, *map_source_dropdown;
} dt_lib_map_settings_t;

int position()
{
  return 990;
}

static void _show_osd_toggled(GtkToggleButton *button, gpointer data)
{
  dt_view_map_show_osd(darktable.view_manager, gtk_toggle_button_get_active(button));
}

static void _map_source_changed(GtkWidget *widget, gpointer data)
{
  GtkTreeIter iter;

  if(gtk_combo_box_get_active_iter(GTK_COMBO_BOX(widget), &iter) == TRUE)
  {
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
    GValue value = {
      0,
    };
    OsmGpsMapSource_t map_source;

    gtk_tree_model_get_value(model, &iter, 1, &value);
    map_source = g_value_get_int(&value);
    g_value_unset(&value);
    dt_view_map_set_map_source(darktable.view_manager, map_source);
  }
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_map_settings_t *d = (dt_lib_map_settings_t *)malloc(sizeof(dt_lib_map_settings_t));
  self->data = d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));
  GtkBox *hbox;
  GtkWidget *label;

  d->show_osd_checkbutton = gtk_check_button_new_with_label(_("show OSD"));
  gtk_widget_set_tooltip_text(d->show_osd_checkbutton, _("toggle the visibility of the map overlays"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->show_osd_checkbutton),
                               dt_conf_get_bool("plugins/map/show_map_osd"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->show_osd_checkbutton, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->show_osd_checkbutton), "toggled", G_CALLBACK(_show_osd_toggled), NULL);

  hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5));

  label = gtk_label_new(_("map source"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_box_pack_start(hbox, label, TRUE, TRUE, 0);

  GtkListStore *model = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
  d->map_source_dropdown = gtk_combo_box_new_with_model(GTK_TREE_MODEL(model));
  gtk_widget_set_tooltip_text(d->map_source_dropdown, _("select the source of the map. some entries might not work"));
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(d->map_source_dropdown), renderer, FALSE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(d->map_source_dropdown), renderer, "text", 0, NULL);

  gchar *map_source = dt_conf_get_string("plugins/map/map_source");
  int selection = OSM_GPS_MAP_SOURCE_OPENSTREETMAP - 1, entry = 0;
  GtkTreeIter iter;
  for(int i = 1; i < OSM_GPS_MAP_SOURCE_LAST; i++)
  {
    if(osm_gps_map_source_is_valid(i))
    {
      const gchar *name = osm_gps_map_source_get_friendly_name(i);
      gtk_list_store_append(model, &iter);
      gtk_list_store_set(model, &iter, 0, name, 1, i, -1);
      if(!g_strcmp0(name, map_source)) selection = entry;
      entry++;
    }
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(d->map_source_dropdown), selection);
  gtk_box_pack_start(hbox, d->map_source_dropdown, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->map_source_dropdown), "changed", G_CALLBACK(_map_source_changed), NULL);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  g_object_unref(model);
  g_free(map_source);
}

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
