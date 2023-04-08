/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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
#include <gdk/gdkkeysyms.h>
#ifdef USE_LUA
#include "lua/call.h"
#include "lua/image.h"
#endif
#include "libs/lib_api.h"

DT_MODULE(1)

const char *name(dt_lib_module_t *self)
{
  return _("select");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

typedef struct dt_lib_select_t
{
  GtkWidget *select_all_button, *select_none_button, *select_invert_button, *select_film_roll_button,
      *select_untouched_button;
} dt_lib_select_t;

static void _update(dt_lib_module_t *self)
{
  dt_lib_select_t *d = (dt_lib_select_t *)self->data;

  const uint32_t collection_cnt =  dt_collection_get_count_no_group(darktable.collection);
  const uint32_t selected_cnt = dt_collection_get_selected_count(darktable.collection);

  gtk_widget_set_sensitive(GTK_WIDGET(d->select_all_button), selected_cnt < collection_cnt);
  gtk_widget_set_sensitive(GTK_WIDGET(d->select_none_button), selected_cnt > 0);

  gtk_widget_set_sensitive(GTK_WIDGET(d->select_invert_button), collection_cnt > 0);

  //theoretically can count if there are unaltered in collection but no need to waste CPU cycles on that.
  gtk_widget_set_sensitive(GTK_WIDGET(d->select_untouched_button), collection_cnt > 0);

  gtk_widget_set_sensitive(GTK_WIDGET(d->select_film_roll_button), selected_cnt > 0);
}

static void _image_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  _update(self);
#ifdef USE_LUA
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
    0, NULL,NULL,
    LUA_ASYNC_TYPENAME,"char*","selection-changed",
    LUA_ASYNC_DONE);
#endif
}

static void _collection_updated_callback(gpointer instance, dt_collection_change_t query_change,
                                         dt_collection_properties_t changed_property, gpointer imgs, int next,
                                         dt_lib_module_t *self)
{
  _update(self);
}

static void button_clicked(GtkWidget *widget, gpointer user_data)
{
  switch(GPOINTER_TO_INT(user_data))
  {
    case 0: // all
      dt_selection_select_all(darktable.selection);
      break;
    case 1: // none
      dt_selection_clear(darktable.selection);
      break;
    case 2: // invert
      dt_selection_invert(darktable.selection);
      break;
    case 4: // untouched
      dt_selection_select_unaltered(darktable.selection);
      break;
    default: // case 3: same film roll
      dt_selection_select_filmroll(darktable.selection);
  }

  dt_control_queue_redraw_center();
}

int position(const dt_lib_module_t *self)
{
  return 800;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_select_t *d = (dt_lib_select_t *)malloc(sizeof(dt_lib_select_t));
  self->data = d;
  self->widget = gtk_grid_new();

  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_column_homogeneous(grid, TRUE);
  int line = 0;

  d->select_all_button = dt_action_button_new(self, N_("select all"), button_clicked, GINT_TO_POINTER(0),
                                              _("select all images in current collection"), GDK_KEY_a, GDK_CONTROL_MASK);
  gtk_grid_attach(grid, d->select_all_button, 0, line, 1, 1);

  d->select_none_button = dt_action_button_new(self, N_("select none"), button_clicked, GINT_TO_POINTER(1),
                                              _("clear selection"), GDK_KEY_a, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  gtk_grid_attach(grid, d->select_none_button, 1, line++, 1, 1);

  d->select_invert_button = dt_action_button_new(self, N_("invert selection"), button_clicked, GINT_TO_POINTER(2),
                                              _("select unselected images\nin current collection"), GDK_KEY_i, GDK_CONTROL_MASK);
  gtk_grid_attach(grid, d->select_invert_button, 0, line, 1, 1);

  d->select_film_roll_button = dt_action_button_new(self, N_("select film roll"), button_clicked, GINT_TO_POINTER(3),
                                              _("select all images which are in the same\nfilm roll as the selected images"), 0, 0);
  gtk_grid_attach(grid, d->select_film_roll_button, 1, line++, 1, 1);

  d->select_untouched_button = dt_action_button_new(self, N_("select untouched"), button_clicked, GINT_TO_POINTER(4),
                                              _("select untouched images in\ncurrent collection"), 0, 0);
  gtk_grid_attach(grid, d->select_untouched_button, 0, line, 2, 1);

  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->select_all_button))), PANGO_ELLIPSIZE_START);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->select_none_button))), PANGO_ELLIPSIZE_START);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->select_film_roll_button))), PANGO_ELLIPSIZE_START);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_collection_updated_callback), self);

  _update(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_collection_updated_callback), self);
  free(self->data);
  self->data = NULL;
}

#ifdef USE_LUA
typedef struct
{
  const char* key;
  dt_lib_module_t * self;
} lua_callback_data;


static int lua_button_clicked_cb(lua_State* L)
{
  lua_callback_data * data = lua_touserdata(L, 1);
  dt_lua_module_entry_push(L, "lib", data->self->plugin_name);
  lua_getiuservalue(L, -1, 1);
  lua_getfield(L, -1, "callbacks");
  lua_getfield(L, -1, data->key);
  lua_pushstring(L, data->key);

  GList *image = dt_collection_get_all(darktable.collection, -1);
  lua_newtable(L);
  int table_index = 1;
  while(image)
  {
    luaA_push(L, dt_lua_image_t, &image->data);
    lua_seti(L, -2, table_index);
    table_index++;
    image = g_list_delete_link(image, image);
  }

  lua_call(L ,2, 1);

  GList *new_selection = NULL;
  luaL_checktype(L, -1, LUA_TTABLE);
  lua_pushnil(L);
  while(lua_next(L, -2) != 0)
  {
    /* uses 'key' (at index -2) and 'value' (at index -1) */
    dt_imgid_t imgid;
    luaA_to(L, dt_lua_image_t, &imgid, -1);
    new_selection = g_list_prepend(new_selection, GINT_TO_POINTER(imgid));
    lua_pop(L, 1);
  }
  new_selection = g_list_reverse(new_selection);
  dt_selection_clear(darktable.selection);
  dt_selection_select_list(darktable.selection, new_selection);
  g_list_free(new_selection);
  return 0;

}

static void lua_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lua_async_call_alien(lua_button_clicked_cb,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME, "void*", user_data,
      LUA_ASYNC_DONE);
}

static int lua_register_selection(lua_State *L)
{
  lua_settop(L, 4);
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lua_module_entry_push(L, "lib", self->plugin_name);
  lua_getiuservalue(L, -1, 1);
  const char* name;
  const char* key;
  name = luaL_checkstring(L, 1);
  key = luaL_checkstring(L, 2);
  luaL_checktype(L, 3, LUA_TFUNCTION);

  lua_getfield(L, -1, "callbacks");
  lua_pushstring(L, name);
  lua_pushvalue(L, 3);
  lua_settable(L, -3);

  GtkWidget* button = gtk_button_new_with_label(key);
  const char * tooltip = lua_tostring(L, 4);
  if(tooltip)
    gtk_widget_set_tooltip_text(button, tooltip);

  gtk_widget_set_name(button, name);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), button, NULL, GTK_POS_BOTTOM, 2, 1);


  lua_callback_data * data = malloc(sizeof(lua_callback_data));
  data->key = strdup(name);
  data->self = self;
  gulong s = g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(lua_button_clicked), data);

  dt_lua_module_entry_push(L, "lib", self->plugin_name);
  lua_getiuservalue(L, -1, 1);
  lua_getfield(L, -1, "signal_handlers");
  lua_pushstring(L, name);
  lua_pushinteger(L, s);
  lua_settable(L, -3);

  gtk_widget_show_all(self->widget);

  return 0;
}

static int lua_destroy_selection(lua_State *L)
{
  lua_settop(L, 3);
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  const char* name = luaL_checkstring(L, 1);

  // find the button named name

  GtkWidget* widget = NULL;
  int row;

  for(row = 2; (widget = gtk_grid_get_child_at(GTK_GRID(self->widget), 0, row)) != NULL; row++)
  {
    if(GTK_IS_BUTTON(widget) && strcmp(gtk_widget_get_name(widget), name) == 0)
    {
      // set the callback to nil

      dt_lua_module_entry_push(L, "lib", self->plugin_name);
      lua_getiuservalue(L, -1, 1);
      lua_getfield(L, -1, "callbacks");
      lua_pushstring(L, name);
      lua_pushnil(L);
      lua_settable(L, -3);

      // disconnect the signal

      dt_lua_module_entry_push(L, "lib", self->plugin_name);
      lua_getiuservalue(L, -1, 1);
      lua_getfield(L, -1, "signal_handlers");
      lua_pushstring(L, name);
      lua_gettable(L, -2);
      gulong handler_id = 0;
      handler_id = luaL_checkinteger(L, -1);
      g_signal_handler_disconnect(G_OBJECT(widget), handler_id);

      // remove the widget

      gtk_grid_remove_row(GTK_GRID(self->widget), row);
      break;
    }
  }

  return 0;
}

static int lua_set_selection_sensitive(lua_State *L)
{
  lua_settop(L, 3);
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  const char* name = luaL_checkstring(L, 1);
  gboolean sensitive = lua_toboolean(L, 2);

  // find the button named name

  GtkWidget* widget = NULL;
  int row;

  for(row = 2; (widget = gtk_grid_get_child_at(GTK_GRID(self->widget), 0, row)) != NULL; row++)
  {
    if(GTK_IS_BUTTON(widget) && strcmp(gtk_widget_get_name(widget), name) == 0)
    {
      gtk_widget_set_sensitive(widget, sensitive);
      break;
    }
  }
  return 0;
}

void init(struct dt_lib_module_t *self)
{

  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_register_selection , 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "register_selection");

  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_destroy_selection, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "destroy_selection");

  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_set_selection_sensitive, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "set_sensitive");

  dt_lua_module_entry_push(L, "lib", self->plugin_name);
  lua_getiuservalue(L, -1, 1);
  lua_newtable(L);
  lua_setfield(L, -2, "callbacks");
  lua_pop(L, 2);

  dt_lua_module_entry_push(L, "lib", self->plugin_name);
  lua_getiuservalue(L, -1, 1);
  lua_newtable(L);
  lua_setfield(L, -2, "signal_handlers");
  lua_pop(L, 2);
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
