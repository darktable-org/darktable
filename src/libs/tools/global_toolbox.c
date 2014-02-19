/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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
#include "common/collection.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/preferences.h"
#include "dtgtk/button.h"
#include "dtgtk/togglebutton.h"
#include "control/conf.h"
#include "control/control.h"

DT_MODULE(1)

typedef struct dt_lib_tool_preferences_t
{
  GtkWidget *preferences_button, *grouping_button, *overlays_button;
}
dt_lib_tool_preferences_t;

/* callback for grouping button */
static void _lib_filter_grouping_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for preference button */
static void _lib_preferences_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for overlays button */
static void _lib_overlays_button_clicked(GtkWidget *widget, gpointer user_data);

const char* name()
{
  return _("preferences");
}

uint32_t views()
{
  return DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING | DT_VIEW_MAP;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT;
}

int expandable()
{
  return 0;
}

int position()
{
  return 1001;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)g_malloc(sizeof(dt_lib_tool_preferences_t));
  self->data = (void *)d;

  self->widget = gtk_hbox_new(FALSE,2);

  /* create the grouping button */
  d->grouping_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_grouping, CPF_STYLE_FLAT);
  gtk_widget_set_size_request(d->grouping_button, 18,18);
  gtk_box_pack_start(GTK_BOX(self->widget), d->grouping_button, FALSE, FALSE, 2);
  if(darktable.gui->grouping)
    g_object_set(G_OBJECT(d->grouping_button), "tooltip-text", _("expand grouped images"), (char *)NULL);
  else
    g_object_set(G_OBJECT(d->grouping_button), "tooltip-text", _("collapse grouped images"), (char *)NULL);
  g_signal_connect (G_OBJECT (d->grouping_button), "clicked",
                    G_CALLBACK (_lib_filter_grouping_button_clicked),
                    NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->grouping_button), darktable.gui->grouping);

  /* create the "show/hide overlays" button */
  d->overlays_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_overlays, CPF_STYLE_FLAT);
  gtk_widget_set_size_request(d->overlays_button, 18,18);
  gtk_box_pack_start(GTK_BOX(self->widget), d->overlays_button, FALSE, FALSE, 2);
  if(darktable.gui->show_overlays)
    g_object_set(G_OBJECT(d->overlays_button), "tooltip-text", _("hide image overlays"), (char *)NULL);
  else
    g_object_set(G_OBJECT(d->overlays_button), "tooltip-text", _("show image overlays"), (char *)NULL);
  g_signal_connect (G_OBJECT (d->overlays_button), "clicked",
                    G_CALLBACK (_lib_overlays_button_clicked),
                    NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->overlays_button), darktable.gui->show_overlays);

  /* create the preference button */
  d->preferences_button = dtgtk_button_new(dtgtk_cairo_paint_preferences, CPF_STYLE_FLAT);
  gtk_widget_set_size_request(d->preferences_button, 18,18);
  gtk_box_pack_end(GTK_BOX(self->widget), d->preferences_button, FALSE, FALSE, 2);
  g_object_set(G_OBJECT(d->preferences_button), "tooltip-text", _("show global preferences"),
               (char *)NULL);
  g_signal_connect (G_OBJECT (d->preferences_button), "clicked",
                    G_CALLBACK (_lib_preferences_button_clicked),
                    NULL);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

void _lib_preferences_button_clicked (GtkWidget *widget, gpointer user_data)
{
  dt_gui_preferences_show();
}

static void _lib_filter_grouping_button_clicked (GtkWidget *widget, gpointer user_data)
{
  darktable.gui->grouping = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(darktable.gui->grouping)
    g_object_set(G_OBJECT(widget), "tooltip-text", _("expand grouped images"), (char *)NULL);
  else
    g_object_set(G_OBJECT(widget), "tooltip-text", _("collapse grouped images"), (char *)NULL);
  dt_conf_set_bool("ui_last/grouping", darktable.gui->grouping);
  darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection);
}

static void _lib_overlays_button_clicked (GtkWidget *widget, gpointer user_data)
{
  darktable.gui->show_overlays = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(darktable.gui->show_overlays)
    g_object_set(G_OBJECT(widget), "tooltip-text", _("hide image overlays"), (char *)NULL);
  else
    g_object_set(G_OBJECT(widget), "tooltip-text", _("show image overlays"), (char *)NULL);
  dt_conf_set_bool("lighttable/ui/expose_statuses", darktable.gui->show_overlays);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED);
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "grouping"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "preferences"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "show overlays"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t*)self->data;

  dt_accel_connect_button_lib(self, "grouping", d->grouping_button);
  dt_accel_connect_button_lib(self, "preferences", d->preferences_button);
  dt_accel_connect_button_lib(self, "show overlays", d->overlays_button);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
