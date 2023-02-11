
/*
   This file is part of darktable,
   Copyright (C) 2015-2020 darktable developers.

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
#include "gui/gtk.h"
#include "lua/types.h"
#include "lua/widget/common.h"

static void slider_init(lua_State *L);
static dt_lua_widget_type_t slider_type = {
  .name = "slider",
  .gui_init = slider_init,
  .gui_cleanup = NULL,
  .alloc_size = sizeof(dt_lua_widget_t),
  .parent= &widget_type
};


static void slider_init(lua_State*L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,-1);
  // as the sliders setup calls are asynchronous, we weed to initialize
  // min to -INF and max to INF, sort of, in order not to cut prematurely soft_min and soft_max
  dt_bauhaus_slider_from_widget(DT_BAUHAUS_WIDGET(slider->widget),NULL, -1.0E9, 1.0E9, 1.0, 0.0, 3,0);
}


static int label_member(lua_State *L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    char tmp[256];
    luaA_to(L,char_256,&tmp,3);
    dt_bauhaus_widget_set_label(slider->widget,NULL,tmp);
    return 0;
  }
  lua_pushstring(L,dt_bauhaus_widget_get_label(slider->widget));
  return 1;
}

static int digits_member(lua_State*L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    int value = lua_tointeger(L,3);
    dt_bauhaus_slider_set_digits(slider->widget,value);
    return 0;
  }
  lua_pushinteger(L,dt_bauhaus_slider_get_digits(slider->widget));
  return 1;
}

static int step_member(lua_State*L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    float value = luaL_checknumber(L,3);
    dt_bauhaus_slider_set_step(slider->widget,value);
    return 0;
  }
  lua_pushnumber(L,dt_bauhaus_slider_get_step(slider->widget));
  return 1;
}

static int hard_min_member(lua_State*L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    float value = luaL_checknumber(L,3);
    dt_bauhaus_slider_set_hard_min(slider->widget,value);
    return 0;
  }
  lua_pushnumber(L,dt_bauhaus_slider_get_hard_min(slider->widget));
  return 1;
}

static int hard_max_member(lua_State*L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    float value = luaL_checknumber(L,3);
    dt_bauhaus_slider_set_hard_max(slider->widget,value);
    return 0;
  }
  lua_pushnumber(L,dt_bauhaus_slider_get_hard_max(slider->widget));
  return 1;
}

static int soft_min_member(lua_State*L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    float value = luaL_checknumber(L,3);
    dt_bauhaus_slider_set_soft_min(slider->widget,value);
    return 0;
  }
  lua_pushnumber(L,dt_bauhaus_slider_get_soft_min(slider->widget));
  return 1;
}

static int soft_max_member(lua_State*L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    float value = luaL_checknumber(L,3);
    dt_bauhaus_slider_set_soft_max(slider->widget,value);
    return 0;
  }
  lua_pushnumber(L,dt_bauhaus_slider_get_soft_max(slider->widget));
  return 1;
}


static int value_member(lua_State *L)
{
  lua_slider slider;
  luaA_to(L,lua_slider,&slider,1);
  if(lua_gettop(L) > 2) {
    float value = luaL_checknumber(L,3);
    dt_bauhaus_slider_set(slider->widget,value);
    return 0;
  }
  lua_pushnumber(L,dt_bauhaus_slider_get(slider->widget));
  return 1;
}

static int tostring_member(lua_State *L)
{
  lua_slider widget;
  luaA_to(L, lua_slider, &widget, 1);
  const gchar *text = dt_bauhaus_widget_get_label(widget->widget);
  gchar *res = g_strdup_printf("%s (\"%s\")", G_OBJECT_TYPE_NAME(widget->widget), text ? text : "");
  lua_pushstring(L, res);
  g_free(res);
  return 1;
}

int dt_lua_init_widget_slider(lua_State* L)
{
  dt_lua_init_widget_type(L,&slider_type,lua_slider,DT_BAUHAUS_WIDGET_TYPE);

  lua_pushcfunction(L, tostring_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_setmetafield(L, lua_slider, "__tostring");
  lua_pushcfunction(L,digits_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_slider, "digits");
  lua_pushcfunction(L,step_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_slider, "step");
  lua_pushcfunction(L,hard_min_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_slider, "hard_min");
  lua_pushcfunction(L,hard_max_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_slider, "hard_max");
  lua_pushcfunction(L,soft_min_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_slider, "soft_min");
  lua_pushcfunction(L,soft_max_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_slider, "soft_max");
  lua_pushcfunction(L,value_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_slider, "value");
  lua_pushcfunction(L,label_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_slider, "label");
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
