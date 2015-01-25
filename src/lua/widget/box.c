/*
   This file is part of darktable,
   copyright (c) 2015 Jeremy Rosen

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
#include "lua/widget/widget.h"
#include "lua/types.h"
#include "gui/gtk.h"

typedef struct {
  dt_lua_widget_t parent;
  GList * children;
} dt_lua_box_t;

typedef dt_lua_box_t* lua_box;

static void box_init(lua_State* L);
static void box_cleanup(lua_State* L,lua_widget widget);
static dt_lua_widget_type_t box_type = {
  .name = "box",
  .gui_init = box_init,
  .gui_cleanup = box_cleanup,
};

static int box_reset(lua_State* L)
{
  lua_box box;
  luaA_to(L,lua_box,&box,1);
  GList*curelt = box->children;
  while(curelt) {
    lua_widget cur_widget = curelt->data;
    dt_lua_widget_trigger_callback(L,cur_widget,"reset");
    curelt = g_list_next(curelt);
  }
  return 0;
}

static void box_init(lua_State* L)
{
  lua_box box = malloc(sizeof(dt_lua_box_t));
  dt_lua_orientation_t orientation;
  luaA_to(L,dt_lua_orientation_t,&orientation,1);
  box->parent.widget = gtk_box_new(orientation, DT_PIXEL_APPLY_DPI(5));
  box->children = NULL;
  box->parent.type = &box_type;
  luaA_push_type(L, box_type.associated_type, &box);
  g_object_ref_sink(box->parent.widget);
  lua_pushcfunction(L,box_reset);
  dt_lua_widget_set_callback(L,-2,"reset");
}


static void box_cleanup(lua_State* L,lua_widget widget)
{
  lua_box box = (lua_box)widget;
  g_list_free(box->children);
  // no need to cleanup the actual widget, __gc does it for us
}

static int box_append(lua_State *L)
{
  lua_box box;
  luaA_to(L,lua_box,&box,1);
  lua_widget widget;
  luaA_to(L, lua_widget,&widget,2),
  gtk_box_pack_start(GTK_BOX(box->parent.widget),widget->widget,TRUE,TRUE, 0);
  box->children = g_list_append(box->children,widget);
  lua_getuservalue(L,1);
  lua_pushvalue(L,2);
  luaL_ref(L,-2);
  lua_pop(L,1);
  return 0;
}

static int box_len(lua_State*L)
{
  lua_box box;
  luaA_to(L,lua_box,&box,1);
  lua_pushinteger(L,g_list_length(box->children));
  return 1;
}

static int box_numindex(lua_State*L)
{
  lua_getuservalue(L,1);
  lua_pushvalue(L,2);
  lua_gettable(L,-2);
  return 1;
}

int dt_lua_init_widget_box(lua_State* L)
{
  dt_lua_init_widget_type(L,&box_type,lua_box);
  lua_pushcfunction(L, box_append);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, lua_box, "append");
  lua_pushcfunction(L,box_len);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  lua_pushcfunction(L,box_numindex);
  dt_lua_type_register_number_const(L,lua_box);

  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
