/*
    This file is part of darktable,
    Copyright (C) 2011-2022 darktable developers.

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
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/filters/filters.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_tool_filter_t
{
  GtkWidget *filter_box;
  GtkWidget *sort_box;
  GtkWidget *count;
  GtkWidget *menu_btn;

  GList *filters;
  GList *sorts;

  gchar *last_where_ext;
} dt_lib_tool_filter_t;

const char *name(dt_lib_module_t *self)
{
  return _("filter");
}

const char **views(dt_lib_module_t *self)
{
  /* for now, show in all view due this affects filmroll too

     TODO: Consider to add flag for all views, which prevents
           unloading/loading a module while switching views.

   */
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 2001;
}

static GtkWidget *_lib_filter_get_filter_box(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  return d->filter_box;
}
static GtkWidget *_lib_filter_get_sort_box(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  return d->sort_box;
}

static GtkWidget *_lib_filter_get_count(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  return d->count;
}

static void _dt_collection_updated(gpointer instance, dt_collection_change_t query_change,
                                   dt_collection_properties_t changed_property, gpointer imgs, int next,
                                   gpointer self)
{
  dt_lib_module_t *dm = (dt_lib_module_t *)self;
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)dm->data;

  gchar *where_ext = dt_collection_get_extended_where(darktable.collection, 99999);
  if(g_strcmp0(where_ext, d->last_where_ext))
  {
    g_free(d->last_where_ext);
    d->last_where_ext = g_strdup(where_ext);
    for(GList *iter = d->filters; iter; iter = g_list_next(iter))
    {
      dt_lib_filters_rule_t *rule = (dt_lib_filters_rule_t *)iter->data;
      dt_filters_update(rule, d->last_where_ext);
    }
  }
}

static void _filters_changed(void *data)
{
  dt_lib_filters_rule_t *rule = (dt_lib_filters_rule_t *)data;
  dt_lib_tool_filter_t *d = rule->parent;

  // save the values
  const int num = g_list_index(d->filters, rule);
  if(num < 0) return;
  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/item%1d", num);
  dt_conf_set_int(confname, rule->prop);
  snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/string%1d", num);
  dt_conf_set_string(confname, rule->raw_text);

  // update the query without throwing signal everywhere
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_dt_collection_updated),
                                  darktable.view_manager->proxy.module_collect.module);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, rule->prop, NULL);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_dt_collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
}

static void _filter_free(gpointer data)
{
  dt_lib_filters_rule_t *rule = (dt_lib_filters_rule_t *)data;
  dt_filters_free(rule);
}

static dt_lib_filters_rule_t *_filter_create_new(const dt_collection_properties_t prop, const gchar *raw_text,
                                                 dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  if(!dt_filters_exists(prop)) return NULL;

  // create a new filter structure
  dt_lib_filters_rule_t *rule = (dt_lib_filters_rule_t *)g_malloc0(sizeof(dt_lib_filters_rule_t));
  rule->parent = d;
  rule->rule_changed = _filters_changed;
  rule->w_special_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  dt_filters_init(rule, prop, raw_text, self, TRUE);

  gtk_box_pack_start(GTK_BOX(d->filter_box), rule->w_special_box, FALSE, TRUE, 0);
  gtk_widget_show_all(rule->w_special_box);

  d->filters = g_list_append(d->filters, rule);

  return rule;
}

static void _filters_init(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;

  // first, let's reset all remaining filters
  g_list_free_full(d->filters, _filter_free);
  d->filters = NULL;

  // then we read the number of existing filters
  ++darktable.gui->reset;
  const int nb = MAX(dt_conf_get_int("plugins/lighttable/topbar/num_rules"), 0);
  char confname[200] = { 0 };

  // create or update defined rules
  for(int i = 0; i < nb; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/item%1d", i);
    const int prop = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/string%1d", i);
    _filter_create_new(prop, dt_conf_get_string_const(confname), self);
  }

  gtk_widget_show_all(d->filter_box);

  --darktable.gui->reset;
}

static gboolean _event_filter_remove(GtkWidget *widget, GdkEventButton *event, dt_lib_filters_rule_t *rule)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)rule->parent;

  // remove the filter from the GList and destroy it
  const int num = g_list_index(d->filters, rule);
  if(num < 0) return TRUE;
  d->filters = g_list_remove(d->filters, rule);
  _filter_free(rule);

  // remove the filter from the saved properties
  const int nb = MAX(dt_conf_get_int("plugins/lighttable/topbar/num_rules"), 0);
  char confname[200] = { 0 };

  // create or update defined rules
  for(int i = num + 1; i < nb; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/item%1d", i);
    const int prop = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/string%1d", i);
    gchar *txt = dt_conf_get_string(confname);

    snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/item%1d", i - 1);
    dt_conf_set_int(confname, prop);
    snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/string%1d", i - 1);
    dt_conf_set_string(confname, txt);

    g_free(txt);
  }
  dt_conf_set_int("plugins/lighttable/topbar/num_rules", nb - 1);

  // remove the entry in the popup
  gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(gtk_widget_get_parent(widget))),
                       gtk_widget_get_parent(widget));

  return TRUE;
}

static GtkWidget *_popup_get_new_filter_line(dt_lib_filters_rule_t *rule, dt_lib_module_t *self)
{
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *eb = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(eb), gtk_label_new(dt_collection_name(rule->prop)));
  gtk_box_pack_start(GTK_BOX(hbox), eb, TRUE, TRUE, 0);
  GtkWidget *btn = dtgtk_button_new(dtgtk_cairo_paint_remove, 0, NULL);
  gtk_widget_set_tooltip_text(btn, _("remove the filter"));
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_event_filter_remove), rule);
  gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, TRUE, 0);
  gtk_widget_show_all(hbox);
  return hbox;
}

static void _event_add_filter(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;

  // create the filter and add it to the GList
  const int prop = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(widget));
  dt_lib_filters_rule_t *rule = _filter_create_new(prop, "", self);
  if(!rule) return;

  // save the properties
  const int nb = g_list_length(d->filters);
  dt_conf_set_int("plugins/lighttable/topbar/num_rules", nb);
  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/item%1d", nb - 1);
  dt_conf_set_int(confname, prop);
  snprintf(confname, sizeof(confname), "plugins/lighttable/topbar/string%1d", nb - 1);
  dt_conf_set_string(confname, "");

  // add the line in the popup
  GtkWidget *hbox = _popup_get_new_filter_line(rule, self);
  gtk_box_pack_start(GTK_BOX(gtk_widget_get_parent(widget)), hbox, FALSE, TRUE, 0);

  // reset the combobox
  dt_bauhaus_combobox_set(widget, 0);
}

static void _rule_populate_prop_combo_add(GtkWidget *w, const dt_collection_properties_t prop)
{
  if(!dt_filters_exists(prop)) return;
  dt_bauhaus_combobox_add_full(w, dt_collection_name(prop), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                               GUINT_TO_POINTER(prop), NULL, TRUE);
}

static gboolean _event_menu_show(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;

  GtkWidget *pop = gtk_popover_new(widget);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(pop), vbox);

  // we show already added filters
  for(GList *iter = d->filters; iter; iter = g_list_next(iter))
  {
    dt_lib_filters_rule_t *rule = (dt_lib_filters_rule_t *)iter->data;
    GtkWidget *hbox = _popup_get_new_filter_line(rule, self);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);
  }

  // the combobox to add a new filter
  GtkWidget *w = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(w, NULL, _("add new filter"));

#define ADD_COLLECT_ENTRY(value) _rule_populate_prop_combo_add(w, value);

  // otherwise we add all implemented rules
  gtk_widget_set_tooltip_text(w, _("choose new filter property"));
  dt_bauhaus_combobox_add_section(w, "");

  dt_bauhaus_combobox_add_section(w, _("files"));
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FILMROLL);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FOLDERS);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FILENAME);

  dt_bauhaus_combobox_add_section(w, _("metadata"));
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_TAG);
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    const gchar *name = dt_metadata_get_name(keyid);
    gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
    const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
    g_free(setting);
    const int meta_type = dt_metadata_get_type(keyid);
    if(meta_type != DT_METADATA_TYPE_INTERNAL && !hidden)
    {
      ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_METADATA + i);
    }
  }
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_RATING_RANGE);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_RATING);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_COLORLABEL);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_TEXTSEARCH);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_GEOTAGGING);

  dt_bauhaus_combobox_add_section(w, _("times"));
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_DAY);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_TIME);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_CHANGE_TIMESTAMP);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_EXPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_PRINT_TIMESTAMP);

  dt_bauhaus_combobox_add_section(w, _("capture details"));
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_CAMERA);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_LENS);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_APERTURE);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_EXPOSURE);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FOCAL_LENGTH);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ISO);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ASPECT_RATIO);

  dt_bauhaus_combobox_add_section(w, _("darktable"));
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_GROUPING);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_LOCAL_COPY);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_HISTORY);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_MODULE);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ORDER);

#undef ADD_COLLECT_ENTRY
  g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(_event_add_filter), self);
  gtk_box_pack_end(GTK_BOX(vbox), w, FALSE, TRUE, 0);

  // let's show the popup
  GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));

  int x, y;
  GdkWindow *pointer_window = gdk_device_get_window_at_position(pointer, &x, &y);
  gpointer pointer_widget = NULL;
  if(pointer_window) gdk_window_get_user_data(pointer_window, &pointer_widget);

  GdkRectangle rect
      = { gtk_widget_get_allocated_width(widget) / 2, gtk_widget_get_allocated_height(widget), 1, 1 };

  if(pointer_widget && widget != pointer_widget)
    gtk_widget_translate_coordinates(pointer_widget, widget, x, y, &rect.x, &rect.y);

  gtk_popover_set_pointing_to(GTK_POPOVER(pop), &rect);

  gtk_widget_show_all(pop);

  return TRUE;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)g_malloc0(sizeof(dt_lib_tool_filter_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(self->widget, GTK_ALIGN_CENTER);

  // the hamburger button
  d->menu_btn = dtgtk_button_new(dtgtk_cairo_paint_presets, 0, NULL);
  g_signal_connect(G_OBJECT(d->menu_btn), "button-press-event", G_CALLBACK(_event_menu_show), self);
  gtk_box_pack_start(GTK_BOX(self->widget), d->menu_btn, TRUE, TRUE, 0);

  // the filter box
  d->filter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(d->filter_box, "header-rule-box");
  gtk_box_pack_start(GTK_BOX(self->widget), d->filter_box, FALSE, FALSE, 0);

  // the sort box
  d->sort_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(d->sort_box, "header-sort-box");
  gtk_box_pack_start(GTK_BOX(self->widget), d->sort_box, FALSE, FALSE, 0);
  GtkWidget *label = gtk_label_new(_("sort by"));
  gtk_box_pack_start(GTK_BOX(d->sort_box), label, TRUE, TRUE, 0);

  /* label to display selected count */
  d->count = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(d->count), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(self->widget), d->count, TRUE, FALSE, 0);

  /* initialize proxy */
  darktable.view_manager->proxy.filter.module = self;
  darktable.view_manager->proxy.filter.get_filter_box = _lib_filter_get_filter_box;
  darktable.view_manager->proxy.filter.get_sort_box = _lib_filter_get_sort_box;
<<<<<<< HEAD
  darktable.view_manager->proxy.filter.get_count = _lib_filter_get_count;
=======

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                                  G_CALLBACK(_dt_collection_updated), self);

  // initialize the filters
  _filters_init(self);
>>>>>>> 07f7975e37 (filters : reactivate topbar)
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  g_list_free_full(d->filters, _filter_free);
  g_free(self->data);
  self->data = NULL;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_collection_updated), self);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
