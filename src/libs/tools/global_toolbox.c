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

DT_MODULE(1)

typedef struct dt_lib_tool_preferences_t
{
  GtkWidget *preferences_button, *grouping_button, *ratings_button, *colorlabels_button, *colorlabels_tint_button, *history_labels_toggle, *rejects_toggle;
}
dt_lib_tool_preferences_t;


/* callback for history label button */
static void _lib_toggle_altered_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for rejects button */
static void _lib_toggle_rejects_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for color labels tint button */
static void _lib_toggle_colorlabels_tint_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for color labels button */
static void _lib_toggle_colorlabels_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for ratings button */
static void _lib_toggle_ratings_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for grouping button */
static void _lib_filter_grouping_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for preference button */
static void _lib_preferences_button_clicked(GtkWidget *widget, gpointer user_data);

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

static inline GtkWidget* create_button(dt_lib_module_t *self, DTGTKCairoPaintIconFunc paint, char* tooltip, GCallback callback, gboolean active)
{
  GtkWidget *button = dtgtk_togglebutton_new(paint, CPF_STYLE_FLAT);
  gtk_widget_set_size_request(button, 18,18);
  gtk_box_pack_start(GTK_BOX(self->widget), button, FALSE, FALSE, 2);
  g_object_set(G_OBJECT(button), "tooltip-text", tooltip, (char *)NULL);
  g_signal_connect (G_OBJECT (button), "clicked", callback, NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), active);
  return button;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)g_malloc(sizeof(dt_lib_tool_preferences_t));
  self->data = (void *)d;

  self->widget = gtk_hbox_new(FALSE,2);
 
  /* create the history label toggle button */
  darktable.gui->show_history_labels = dt_conf_get_bool("ui_last/show_history_labels");
  d->history_labels_toggle = create_button(self,
    dtgtk_cairo_paint_toggle_altered, 
    darktable.gui->show_history_labels ? _("show history labels for active image only") : _("show history labels for all images"), 
    G_CALLBACK (_lib_toggle_altered_button_clicked),
    darktable.gui->show_history_labels);
  
    /* create the rejects toggle button */
  darktable.gui->show_rejects = dt_conf_get_bool("ui_last/show_rejects");
  d->rejects_toggle = create_button(self,
    dtgtk_cairo_paint_toggle_reject, 
    darktable.gui->show_rejects ? _("show reject cross for active image only") : _("show reject cross for all images"), 
    G_CALLBACK (_lib_toggle_rejects_button_clicked),
    darktable.gui->show_rejects);
  
  /* create the label tint toggle button */
  darktable.gui->show_colorlabel_tint = dt_conf_get_bool("ui_last/show_colorlabel_background");
  d->colorlabels_tint_button = create_button(self,
    dtgtk_cairo_paint_toggle_labels_tint, 
    darktable.gui->show_colorlabel_tint ? _("remove label tint on image backgrounds") : _("tint image background with label color"), 
    G_CALLBACK (_lib_toggle_colorlabels_tint_button_clicked),
    darktable.gui->show_colorlabel_tint);
  
  /* create the colorlabel toggle button */
  darktable.gui->show_colorlabel_dots = dt_conf_get_bool("ui_last/show_colorlabel_dots");
  d->colorlabels_button = create_button(self,
    dtgtk_cairo_paint_toggle_labels, 
    darktable.gui->show_colorlabel_dots ? _("hide color label dots") : _("show color label dots"), 
    G_CALLBACK (_lib_toggle_colorlabels_button_clicked),
    darktable.gui->show_colorlabel_dots);
  
  /* create the ratings toggle button */
  darktable.gui->show_ratings_on_all_images = dt_conf_get_bool("ui_last/show_ratings_on_all_images");
  d->ratings_button = create_button(self,
    dtgtk_cairo_paint_toggle_stars, 
    darktable.gui->show_ratings_on_all_images ? _("show ratings for active image only") : _("show ratings for all images"), 
    G_CALLBACK (_lib_toggle_ratings_button_clicked),
    darktable.gui->show_ratings_on_all_images);
  
  /* create the grouping button */
  darktable.gui->show_history_labels = dt_conf_get_bool("ui_last/grouping");
  d->grouping_button = create_button(self,
    dtgtk_cairo_paint_grouping, 
    darktable.gui->grouping ? _("expand grouped images") : _("collapse grouped images"), 
    G_CALLBACK (_lib_filter_grouping_button_clicked),
    darktable.gui->grouping);
  
  /* create the preference button */
  d->preferences_button = create_button(self,
    dtgtk_cairo_paint_preferences, 
    _("show global preferences"), 
    G_CALLBACK (_lib_preferences_button_clicked),
    FALSE);
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

static void _lib_toggle_altered_button_clicked (GtkWidget *widget, gpointer user_data)
{
  darktable.gui->show_history_labels = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(darktable.gui->show_history_labels)
    g_object_set(G_OBJECT(widget), "tooltip-text", _("show history label for active image only"), (char *)NULL);
  else
    g_object_set(G_OBJECT(widget), "tooltip-text", _("show history label for all images"), (char *)NULL);
  dt_conf_set_bool("ui_last/show_history_labels", darktable.gui->show_history_labels);
  darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection);
}

static void _lib_toggle_rejects_button_clicked (GtkWidget *widget, gpointer user_data)
{
  darktable.gui->show_rejects = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(darktable.gui->show_rejects)
    g_object_set(G_OBJECT(widget), "tooltip-text", _("show reject cross for active image only"), (char *)NULL);
  else
    g_object_set(G_OBJECT(widget), "tooltip-text", _("show reject cross for all images"), (char *)NULL);
  dt_conf_set_bool("ui_last/show_rejects", darktable.gui->show_rejects);
  darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection);
}

static void _lib_toggle_colorlabels_tint_button_clicked (GtkWidget *widget, gpointer user_data)
{
  darktable.gui->show_colorlabel_tint = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(darktable.gui->show_colorlabel_tint)
    g_object_set(G_OBJECT(widget), "tooltip-text", _("hide color label background"), (char *)NULL);
  else
    g_object_set(G_OBJECT(widget), "tooltip-text", _("show color label background"), (char *)NULL);
  dt_conf_set_bool("ui_last/show_colorlabel_background", darktable.gui->show_colorlabel_tint);
  darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection);
}

static void _lib_toggle_colorlabels_button_clicked (GtkWidget *widget, gpointer user_data)
{
  darktable.gui->show_colorlabel_dots = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(darktable.gui->show_colorlabel_dots)
    g_object_set(G_OBJECT(widget), "tooltip-text", _("hide color label dots"), (char *)NULL);
  else
    g_object_set(G_OBJECT(widget), "tooltip-text", _("show color label dots"), (char *)NULL);
  dt_conf_set_bool("ui_last/show_colorlabel_dots", darktable.gui->show_colorlabel_dots);
  darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection);
}

static void _lib_toggle_ratings_button_clicked (GtkWidget *widget, gpointer user_data)
{
  darktable.gui->show_ratings_on_all_images = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(darktable.gui->show_ratings_on_all_images)
    g_object_set(G_OBJECT(widget), "tooltip-text", _("show ratings for active image only"), (char *)NULL);
  else
    g_object_set(G_OBJECT(widget), "tooltip-text", _("show ratings for all images"), (char *)NULL);
  dt_conf_set_bool("ui_last/show_ratings_on_all_images", darktable.gui->show_ratings_on_all_images);
  darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection);
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

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "toggle history label display"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "toggle reject cross display"), 0, 0); 
  dt_accel_register_lib(self, NC_("accel", "toggle color label background tint"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "toggle color label display"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "toggle rating star display"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "grouping"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "preferences"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t*)self->data;

  dt_accel_connect_button_lib(self, "toggle history label display", d->history_labels_toggle);
  dt_accel_connect_button_lib(self, "toggle reject cross display", d->rejects_toggle);
  dt_accel_connect_button_lib(self, "toggle color label background tint", d->colorlabels_tint_button);
  dt_accel_connect_button_lib(self, "toggle color label display", d->colorlabels_button);
  dt_accel_connect_button_lib(self, "toggle rating star display", d->ratings_button);
  dt_accel_connect_button_lib(self, "grouping", d->grouping_button);
  dt_accel_connect_button_lib(self, "preferences", d->preferences_button);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
