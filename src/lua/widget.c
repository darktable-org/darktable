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
#include "lua/widget.h"
#include "lua/types.h"
#include "lua/modules.h"
#include "lua/call.h"
#include "control/control.h"
/**
  TODO
  gui_cleanup not double checked
  */

static int get_widget_params(lua_State *L)
{
  struct dt_lua_widget_type_t *widget_type = lua_touserdata(L, lua_upvalueindex(1));
  lua_widget widget = widget_type->gui_init(L);
  widget->type = widget_type;
  g_object_ref_sink(widget->widget);
  luaA_push_type(L, widget_type->associated_type, &widget);
  return 1;
}

static int widget_gc(lua_State *L)
{
  lua_widget widget = *(lua_widget*)lua_touserdata(L,1);
  widget->type->gui_cleanup(L,widget);
  g_object_unref(widget->widget);
  return 0;
}

void dt_lua_register_widget_type_type(lua_State *L, dt_lua_widget_type_t* widget_type, luaA_Type type_id)
{
  widget_type->associated_type = type_id;
  dt_lua_type_register_parent_type(L, type_id, luaA_type_find(L, "lua_widget"));

  // add to the table
  lua_pushlightuserdata(L, widget_type);
  lua_pushcclosure(L, get_widget_params, 1);
  dt_lua_module_entry_new(L, -1, "widget", widget_type->name);
  lua_pop(L, 1);
};



static int new_widget(lua_State *L)
{
  const char *entry_name = luaL_checkstring(L, 1);
  dt_lua_module_entry_push(L, "widget", entry_name);
  lua_insert(L,2);
  lua_call(L, lua_gettop(L)-2, 1);
  return 1;
}

void dt_lua_widget_setcallback(lua_State *L,int index,const char* name)
{
  luaL_argcheck(L, dt_lua_isa(L, index, lua_widget), index, "lua_widget expected");
  luaL_checktype(L,-1,LUA_TFUNCTION);
  lua_getuservalue(L,index);
  lua_pushvalue(L,-2);
  lua_setfield(L,-2,name);
  lua_pop(L,2);
}

void dt_lua_widget_getcallback(lua_State *L,int index,const char* name)
{
  luaL_argcheck(L, dt_lua_isa(L, index, lua_widget), index, "lua_widget expected");
  lua_getuservalue(L,index);
  lua_getfield(L,-1,name);
  lua_remove(L,-2);
}

typedef struct {
  gpointer object;
  char * event_name;
  luaA_Type object_type;
}widget_callback_data;


static int32_t widget_callback_job(dt_job_t *job)
{
  dt_lua_lock();
  lua_State* L= darktable.lua_state.state;
  widget_callback_data* data = (widget_callback_data*)dt_control_job_get_params(job);
  luaA_push_type(L,data->object_type,&data->object);
  lua_getuservalue(L,-1);
  lua_getfield(L,-1,data->event_name);
  lua_pushvalue(L,-3);
  dt_lua_do_chunk_later(L,1);
  lua_pop(L,2);
  free(data->event_name);
  free(data);
  dt_lua_unlock();
  return 0;

}

void dt_lua_widget_trigger_callback(gpointer object,luaA_Type object_type,const char* name)
{
  dt_job_t *job = dt_control_job_create(&widget_callback_job, "lua: widget event");
  if(job)
  {
    widget_callback_data*data = malloc(sizeof(widget_callback_data));
    data->object = object;
    data->object_type = object_type;
    data->event_name = strdup(name);
    dt_control_job_set_params(job, data);
    dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
  }
}


static int tooltip_member(lua_State *L)
{
  lua_widget widget;
  luaA_to(L,lua_widget,&widget,1);
  if(lua_gettop(L) > 2) {
    if(lua_isnil(L,3)) {
      gtk_widget_set_tooltip_text(widget->widget,NULL);
    } else {
      const char * text = luaL_checkstring(L,3);
      gtk_widget_set_tooltip_text(widget->widget,text);
    }
    return 0;
  }
  char* result = gtk_widget_get_tooltip_text(widget->widget);
  lua_pushstring(L,result);
  free(result);
  return 1;
}

extern int dt_lua_init_widget_box(lua_State* L);
extern int dt_lua_init_widget_button(lua_State* L);
extern int dt_lua_init_widget_check_button(lua_State* L);
extern int dt_lua_init_widget_label(lua_State* L);
extern int dt_lua_init_widget_entry(lua_State* L);
extern int dt_lua_init_widget_separator(lua_State* L);
extern int dt_lua_init_widget_combo_box_text(lua_State* L);

int dt_lua_init_widget(lua_State* L)
{
  dt_lua_module_new(L,"widget");

  dt_lua_init_gpointer_type(L,lua_widget);
  lua_pushcfunction(L,tooltip_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_widget, "tooltip");
  lua_pushcfunction(L,widget_gc);
  dt_lua_type_setmetafield(L,lua_widget,"__gc");
  
  dt_lua_init_widget_box(L);
  dt_lua_init_widget_button(L);
  dt_lua_init_widget_check_button(L);
  dt_lua_init_widget_label(L);
  dt_lua_init_widget_entry(L);
  dt_lua_init_widget_separator(L);
  dt_lua_init_widget_combo_box_text(L);

  luaA_enum(L,dt_lua_orientation_t);
  luaA_enum_value_name(L,dt_lua_orientation_t,GTK_ORIENTATION_HORIZONTAL,"horizontal");
  luaA_enum_value_name(L,dt_lua_orientation_t,GTK_ORIENTATION_VERTICAL,"vertical");



  dt_lua_push_darktable_lib(L);
  lua_pushstring(L, "new_widget");
  lua_pushcfunction(L, &new_widget);
  lua_settable(L, -3);
  lua_pop(L, 1);
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
