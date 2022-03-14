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

#include "common/collection.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "bauhaus/bauhaus.h"

DT_MODULE(1)

typedef struct dt_lib_tool_filter_t
{
  GtkWidget *filter_box;
  GtkWidget *sort_box;
  GtkWidget *sort;
  GtkWidget *reverse;

  gboolean manual_update;
} dt_lib_tool_filter_t;

#ifdef USE_LUA
typedef enum dt_collection_sort_order_t
{
  DT_COLLECTION_SORT_ORDER_ASCENDING = 0,
  DT_COLLECTION_SORT_ORDER_DESCENDING
} dt_collection_sort_order_t;
#endif

/* proxy function to update the sort widgets without throwing events */
static void _lib_filter_update_sort(dt_lib_module_t *self, dt_collection_sort_t sort, gboolean asc);

/* callback for sort combobox change */
static void _lib_filter_sort_combobox_changed(GtkWidget *widget, gpointer user_data);
/* callback for reverse sort check button change */
static void _lib_filter_reverse_button_changed(GtkDarktableToggleButton *widget, dt_lib_module_t *self);
/* images order change from outside */
static void _lib_filter_images_order_change(gpointer instance, int order, dt_lib_module_t *self);

const dt_collection_sort_t items[] =
{
  DT_COLLECTION_SORT_FILENAME,
  DT_COLLECTION_SORT_DATETIME,
  DT_COLLECTION_SORT_IMPORT_TIMESTAMP,
  DT_COLLECTION_SORT_CHANGE_TIMESTAMP,
  DT_COLLECTION_SORT_EXPORT_TIMESTAMP,
  DT_COLLECTION_SORT_PRINT_TIMESTAMP,
  DT_COLLECTION_SORT_RATING,
  DT_COLLECTION_SORT_ID,
  DT_COLLECTION_SORT_COLOR,
  DT_COLLECTION_SORT_GROUP,
  DT_COLLECTION_SORT_PATH,
  DT_COLLECTION_SORT_CUSTOM_ORDER,
  DT_COLLECTION_SORT_TITLE,
  DT_COLLECTION_SORT_DESCRIPTION,
  DT_COLLECTION_SORT_ASPECT_RATIO,
  DT_COLLECTION_SORT_SHUFFLE,
};
#define NB_ITEMS (sizeof(items) / sizeof(dt_collection_sort_t))

static const char *_sort_names[]
  = { N_("filename"),
      N_("capture time"),
      N_("import time"),
      N_("last modification time"),
      N_("last export time"),
      N_("last print time"),
      N_("rating"),
      N_("id"),
      N_("color label"),
      N_("group"),
      N_("full path"),
      N_("custom sort"),
      N_("title"),
      N_("description"),
      N_("aspect ratio"),
      N_("shuffle"),
      NULL };

static int _filter_get_items(const dt_collection_sort_t sort)
{
  for(int i = 0; i < NB_ITEMS; i++)
  {
    if(sort == items[i])
    return i;
  }
  return 0;
}

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
  return DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
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

static void _reset_filters(dt_action_t *action)
{
  _lib_filter_reset(dt_action_lib(action), FALSE);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_SORT, NULL);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)g_malloc0(sizeof(dt_lib_tool_filter_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign(self->widget, GTK_ALIGN_START);
  gtk_widget_set_valign(self->widget, GTK_ALIGN_CENTER);

  d->filter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->filter_box, TRUE, TRUE, 0);

  /* sort combobox */
  d->sort_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->sort_box, TRUE, TRUE, 0);
  GtkWidget *label = gtk_label_new(_("sort by"));
  gtk_box_pack_start(GTK_BOX(d->sort_box), label, TRUE, TRUE, 0);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(d->sort_box), hbox, TRUE, TRUE, 4);
  const dt_collection_sort_t sort = dt_collection_get_sort_field(darktable.collection);
  d->sort = dt_bauhaus_combobox_new_full(DT_ACTION(self), NULL, N_("sort by"),
                                         _("determine the sort order of shown images"),
                                         _filter_get_items(sort), _lib_filter_sort_combobox_changed, self,
                                         _sort_names);
  gtk_box_pack_start(GTK_BOX(hbox), d->sort, FALSE, FALSE, 0);
  dt_gui_add_class(hbox, "quick_filter_box");
  dt_gui_add_class(hbox, "dt_font_resize_07");

  /* reverse order checkbutton */
  d->reverse = dtgtk_togglebutton_new(dtgtk_cairo_paint_sortby, CPF_DIRECTION_UP, NULL);
  if(darktable.collection->params.descending)
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(d->reverse), dtgtk_cairo_paint_sortby,
                                 CPF_DIRECTION_DOWN, NULL);
  gtk_box_pack_start(GTK_BOX(hbox), d->reverse, FALSE, FALSE, 0);
  dt_gui_add_class(d->reverse, "dt_ignore_fg_state");

  /* select the last value and connect callback */
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->reverse),
                               dt_collection_get_sort_descending(darktable.collection));
  g_signal_connect(G_OBJECT(d->reverse), "toggled", G_CALLBACK(_lib_filter_reverse_button_changed),
                   (gpointer)self);

  /* initialize proxy */
  darktable.view_manager->proxy.filter.module = self;
  darktable.view_manager->proxy.filter.update_sort = _lib_filter_update_sort;
  darktable.view_manager->proxy.filter.get_filter_box = _lib_filter_get_filter_box;
  darktable.view_manager->proxy.filter.get_sort_box = _lib_filter_get_sort_box;

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_IMAGES_ORDER_CHANGE,
                            G_CALLBACK(_lib_filter_images_order_change), self);
  dt_action_register(DT_ACTION(self), N_("reset filters"), _reset_filters, 0, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

static void _lib_filter_images_order_change(gpointer instance, const int order, dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  dt_bauhaus_combobox_set(d->sort, _filter_get_items(order & ~DT_COLLECTION_ORDER_FLAG));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->reverse), order & DT_COLLECTION_ORDER_FLAG);
}

static void _lib_filter_reverse_button_changed(GtkDarktableToggleButton *widget, dt_lib_module_t *self)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  const gboolean reverse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

  if(reverse)
    dtgtk_togglebutton_set_paint(widget, dtgtk_cairo_paint_sortby, CPF_DIRECTION_DOWN, NULL);
  else
    dtgtk_togglebutton_set_paint(widget, dtgtk_cairo_paint_sortby, CPF_DIRECTION_UP, NULL);
  gtk_widget_queue_draw(GTK_WIDGET(widget));

  if(d->manual_update) return;

  dt_view_filtering_set_sort(darktable.view_manager, items[dt_bauhaus_combobox_get(d->sort)], reverse);
}

static void _lib_filter_sort_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  if(d->manual_update) return;
  const gboolean reverse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->reverse));
  dt_view_filtering_set_sort(darktable.view_manager, items[dt_bauhaus_combobox_get(d->sort)], reverse);
}

static void _lib_filter_update_sort(dt_lib_module_t *self, dt_collection_sort_t sort, gboolean asc)
{
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  d->manual_update = TRUE;
  dt_bauhaus_combobox_set(d->sort, sort);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->reverse), asc);
  d->manual_update = FALSE;
}

#ifdef USE_LUA
static void _lib_filter_update_query(dt_lib_module_t *self, dt_collection_properties_t changed_property)
{
  /* sometimes changes */
  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);

  /* updates query */
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, changed_property, NULL);
}

static int sort_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  const dt_collection_sort_t tmp = dt_collection_get_sort_field(darktable.collection);

  if(lua_gettop(L) > 0)
  {
    dt_collection_sort_t value;
    luaA_to(L,dt_collection_sort_t,&value,1);
    dt_collection_set_sort(darktable.collection, (uint32_t)value, 0);
    const dt_collection_sort_t sort = dt_collection_get_sort_field(darktable.collection);
    dt_bauhaus_combobox_set(d->sort, _filter_get_items(sort));
    _lib_filter_update_query(self, DT_COLLECTION_PROP_SORT);
  }
  luaA_push(L, dt_collection_sort_t, &tmp);
  return 1;
}
static int sort_order_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lib_tool_filter_t *d = (dt_lib_tool_filter_t *)self->data;
  const gboolean tmp = dt_collection_get_sort_descending(darktable.collection);

  if(lua_gettop(L) > 0)
  {
    dt_collection_sort_order_t value;
    luaA_to(L,dt_collection_sort_order_t,&value,1);
    dt_collection_sort_t sort_value = dt_collection_get_sort_field(darktable.collection);
    dt_collection_set_sort(darktable.collection, sort_value, value);
    const dt_collection_sort_t sort = dt_collection_get_sort_field(darktable.collection);
    dt_bauhaus_combobox_set(d->sort, _filter_get_items(sort));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->reverse),
                               dt_collection_get_sort_descending(darktable.collection));
    _lib_filter_update_query(self, DT_COLLECTION_PROP_SORT);
  }
  luaA_push(L, dt_collection_sort_order_t, &tmp);
  return 1;
}

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, sort_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "sort");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, sort_order_cb,1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "sort_order");

  luaA_enum(L,dt_collection_sort_t);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_NONE);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_FILENAME);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_DATETIME);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_IMPORT_TIMESTAMP);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_CHANGE_TIMESTAMP);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_EXPORT_TIMESTAMP);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_PRINT_TIMESTAMP);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_RATING);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_ID);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_COLOR);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_GROUP);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_PATH);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_CUSTOM_ORDER);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_TITLE);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_DESCRIPTION);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_ASPECT_RATIO);
  luaA_enum_value(L,dt_collection_sort_t,DT_COLLECTION_SORT_SHUFFLE);

  luaA_enum(L,dt_collection_sort_order_t);
  luaA_enum_value(L,dt_collection_sort_order_t,DT_COLLECTION_SORT_ORDER_ASCENDING);
  luaA_enum_value(L,dt_collection_sort_order_t,DT_COLLECTION_SORT_ORDER_DESCENDING);

}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

