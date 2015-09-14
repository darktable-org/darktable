/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2010--2011 henrik andersson.

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
#include "common/selection.h"
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gdk/gdkkeysyms.h>
#include "dtgtk/button.h"
#ifdef USE_LUA
#include "lua/image.h"
#include "lua/call.h"
#endif

DT_MODULE(1)

const char *name()
{
  return _("select");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

typedef struct dt_lib_select_t
{
  GtkWidget *select_all_button, *select_none_button, *select_invert_button, *select_film_roll_button,
      *select_untouched_button;
} dt_lib_select_t;

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

int position()
{
  return 800;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_select_t *d = (dt_lib_select_t *)malloc(sizeof(dt_lib_select_t));
  self->data = d;
  self->widget = gtk_grid_new();
  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_row_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_homogeneous(grid, TRUE);
  int line = 0;
  GtkWidget *button;

  button = gtk_button_new_with_label(_("select all"));
  d->select_all_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("select all images in current collection (ctrl-a)"),
               (char *)NULL);
  gtk_grid_attach(grid, button, 0, line, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(0));

  button = gtk_button_new_with_label(_("select none"));
  d->select_none_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("clear selection (ctrl-shift-a)"), (char *)NULL);
  gtk_grid_attach(grid, button, 1, line++, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(1));


  button = gtk_button_new_with_label(_("invert selection"));
  g_object_set(G_OBJECT(button), "tooltip-text",
               _("select unselected images\nin current collection (ctrl-!)"), (char *)NULL);
  d->select_invert_button = button;
  gtk_grid_attach(grid, button, 0, line, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(2));

  button = gtk_button_new_with_label(_("select film roll"));
  d->select_film_roll_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text",
               _("select all images which are in the same\nfilm roll as the selected images"), (char *)NULL);
  gtk_grid_attach(grid, button, 1, line++, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(3));


  button = gtk_button_new_with_label(_("select untouched"));
  d->select_untouched_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("select untouched images in\ncurrent collection"),
               (char *)NULL);
  gtk_grid_attach(grid, button, 0, line, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(4));
}

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

#ifdef USE_LUA
typedef struct {
  const char* key;
  dt_lib_module_t * self;
} lua_callback_data;


static int lua_button_clicked_cb(lua_State* L)
{
  lua_callback_data * data = lua_touserdata(L,1);
  dt_lua_module_entry_push(L,"lib",data->self->plugin_name);
  lua_getuservalue(L,-1);
  lua_getfield(L,-1,"callbacks");
  lua_getfield(L,-1,data->key);
  lua_pushstring(L,data->key);

  GList *image = dt_collection_get_all(darktable.collection, -1);
  lua_newtable(L);
  while(image)
  {
    luaA_push(L, dt_lua_image_t, &image->data);
    luaL_ref(L, -2);
    image = g_list_delete_link(image, image);
  }

  dt_lua_do_chunk_raise(L,2,1);

  GList *new_selection = NULL;
  luaL_checktype(L, -1, LUA_TTABLE);
  lua_pushnil(L);
  while(lua_next(L, -2) != 0)
  {
    /* uses 'key' (at index -2) and 'value' (at index -1) */
    int imgid;
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
  dt_lua_do_chunk_async(lua_button_clicked_cb,
      LUA_ASYNC_TYPENAME,"void*", user_data,
      LUA_ASYNC_DONE);
}

static int lua_register_selection(lua_State *L)
{
  lua_settop(L,3);
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,-1);
  const char* key = luaL_checkstring(L,1);
  luaL_checktype(L,2,LUA_TFUNCTION);

  lua_getfield(L,-1,"callbacks");
  lua_pushstring(L,key);
  lua_pushvalue(L,2);
  lua_settable(L,-3);

  GtkWidget* button = gtk_button_new_with_label(key);
  const char * tooltip = lua_tostring(L,3);
  if(tooltip)  {
    g_object_set(G_OBJECT(button), "tooltip-text", tooltip, (char *)NULL);
  }
  gtk_grid_attach_next_to(GTK_GRID(self->widget), button, NULL, GTK_POS_BOTTOM, 2, 1);


  lua_callback_data * data = malloc(sizeof(lua_callback_data));
  data->key = strdup(key);
  data->self = self;
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(lua_button_clicked), data);
  gtk_widget_show_all(self->widget);
  return 0;
}

void init(struct dt_lib_module_t *self)
{

  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_register_selection ,1);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "register_selection");

  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,-1);
  lua_newtable(L);
  lua_setfield(L,-2,"callbacks");
  lua_pop(L,2);
}
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
