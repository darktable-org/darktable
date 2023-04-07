/*
   This file is part of darktable,
   Copyright (C) 2013-2021 darktable developers.

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
#include "lua/gui.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/selection.h"
#include "control/control.h"
#include "control/settings.h"
#include "gui/accelerators.h"
#include "lua/call.h"
#include "lua/image.h"
#include "lua/types.h"
#include <glib.h>

/***********************************************************************
  Creating the images global variable
 **********************************************************************/

static int _selection_cb(lua_State *L)
{
  GList *image = dt_collection_get_selected(darktable.collection, -1);
  if(lua_gettop(L) > 0)
  {
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
  }
  lua_newtable(L);
  int table_index = 1;
  while(image)
  {
    luaA_push(L, dt_lua_image_t, &image->data);
    lua_seti(L, -2, table_index);
    table_index++;
    image = g_list_delete_link(image, image);
  }
  return 1;
}

static int _hovered_cb(lua_State *L)
{
  int32_t mouse_over_id = dt_control_get_mouse_over_id();
  if(!dt_is_valid_imgid(mouse_over_id))
  {
    lua_pushnil(L);
  }
  else
  {
    luaA_push(L, dt_lua_image_t, &mouse_over_id);
  }
  return 1;
}

static int _act_on_cb(lua_State *L)
{
  lua_newtable(L);
  int table_index = 1;
  GList *l = dt_act_on_get_images(FALSE, TRUE, TRUE);
  for(const GList *image = l; image; image = g_list_next(image))
  {
    luaA_push(L, dt_lua_image_t, &image->data);
    lua_seti(L, -2, table_index);
    table_index++;
  }
  g_list_free(l);
  return 1;
}

static int _current_view_cb(lua_State *L)
{
  if(lua_gettop(L) > 0)
  {
    dt_view_t *view;
    luaA_to(L, dt_lua_view_t, &view, 1);
    dt_ctl_switch_mode_to_by_view(view);
  }
  const dt_view_t *current_view = dt_view_manager_get_current_view(darktable.view_manager);
  dt_lua_module_entry_push(L, "view", current_view->module_name);
  return 1;
}

static int _action_cb(lua_State *L)
{
  int arg = 1;

  const gchar *action = luaL_checkstring(L, arg++);

  int instance = 0;

  // support legacy order: action, instance, element, effect, size
  if(lua_type(L, arg) == LUA_TNUMBER && lua_type(L, arg+1) == LUA_TSTRING)
    instance = luaL_checkinteger(L, arg++);

  // new order: instance optionally at end; element, effect and size also optional
  const gchar *element = lua_type(L, arg) == LUA_TSTRING ? luaL_checkstring(L, arg++) : NULL;
  const gchar *effect = lua_type(L, arg) == LUA_TSTRING ? luaL_checkstring(L, arg++) : NULL;

  float move_size = NAN;

  if(lua_type(L, arg) == LUA_TSTRING && strlen(luaL_checkstring(L, arg)) == 0)
    arg++; // "" -> NAN
  else if(lua_type(L, arg) != LUA_TNONE)
    move_size = luaL_checknumber(L, arg++);

  if(lua_type(L, arg) == LUA_TNUMBER)
    instance = luaL_checkinteger(L, arg++);

  float ret_val = dt_action_process(action, instance, element, effect, move_size);

  lua_pushnumber(L, ret_val);

  return 1;
}

static int _mimic_cb(lua_State *L)
{
  const gchar *ac_type  = luaL_checkstring(L, 1);
  const gchar *ac_name = luaL_checkstring(L, 2);

  luaL_checktype(L, 3, LUA_TFUNCTION);

  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_mimic_list");
  if(lua_isnil(L, -1)) goto mimic_end;

  lua_pushvalue(L, 3);
  lua_setfield(L, -2, ac_name);

  // find the action type definition to be simulated (including fallbacks)
  dt_action_def_t *def = NULL;
  for(int i = 0; i < darktable.control->widget_definitions->len; i++)
  {
    def = darktable.control->widget_definitions->pdata[i];
    if(!strcmp(def->name, ac_type)) break;
  }

  lua_getglobal(L, "script_manager_running_script");
  dt_action_define(&darktable.control->actions_lua, lua_tolstring(L,-1,NULL), ac_name, NULL, def);

mimic_end:
  lua_pop(L, 1);
  return 1;
}

static int _panel_visible_cb(lua_State *L)
{
  dt_ui_panel_t p;

  if(lua_gettop(L) > 0)
  {
    gboolean result;
    luaA_to(L, dt_ui_panel_t, &p, 1);
    result = dt_ui_panel_visible(darktable.gui->ui, p);
    lua_pushboolean(L, result);
    return 1;
  }
  else
  {
    return luaL_error(L, "no panel specified");
  }
}

static int _panel_hide_cb(lua_State *L)
{
  dt_ui_panel_t p;
  if(lua_gettop(L) > 0)
  {
    luaA_to(L, dt_ui_panel_t, &p, 1);
    dt_ui_panel_show(darktable.gui->ui, p, FALSE, TRUE);
    return 0;
  }
  else
  {
    return luaL_error(L, "no panel specified");
  }
}

static int _panel_show_cb(lua_State *L)
{
  dt_ui_panel_t p;
  if(lua_gettop(L) > 0)
  {
    luaA_to(L, dt_ui_panel_t, &p, 1);
    dt_ui_panel_show(darktable.gui->ui, p, TRUE, TRUE);
    return 0;
  }
  else
  {
    return luaL_error(L, "no panel specified");
  }
}

static int _panel_hide_all_cb(lua_State *L)
{
  for(int k = 0; k < DT_UI_PANEL_SIZE; k++) dt_ui_panel_show(darktable.gui->ui, k, FALSE, TRUE);
  // code goes here
  return 0;
}

static int _panel_show_all_cb(lua_State *L)
{
  for(int k = 0; k < DT_UI_PANEL_SIZE; k++) dt_ui_panel_show(darktable.gui->ui, k, TRUE, TRUE);
  return 0;
}

static int _panel_get_size_cb(lua_State *L)
{
  dt_ui_panel_t p;
  int size;

  if(lua_gettop(L) > 0)
  {
    luaA_to(L, dt_ui_panel_t, &p, 1);
    if(p == DT_UI_PANEL_LEFT || p == DT_UI_PANEL_RIGHT || p == DT_UI_PANEL_BOTTOM)
    {
      size = dt_ui_panel_get_size(darktable.gui->ui, p);
      lua_pushnumber(L, size);
      return 1;
    }
    else
    {
      return luaL_error(L, "size not supported for specified panel");
    }
  }
  else
  {
    return luaL_error(L, "no panel specified");
  }
}

static int _panel_set_size_cb(lua_State *L)
{
  dt_ui_panel_t p;
  int size;

  if(lua_gettop(L) > 1)
  {
    luaA_to(L, dt_ui_panel_t, &p, 1);
    luaA_to(L, int, &size, 2);
    if(p == DT_UI_PANEL_LEFT || p == DT_UI_PANEL_RIGHT || p == DT_UI_PANEL_BOTTOM)
    {
      dt_ui_panel_set_size(darktable.gui->ui, p, size);
      return 0;
    }
    else
    {
      return luaL_error(L, "changing size not supported for specified panel");
    }
  }
  else
  {
    return luaL_error(L, "no panel specified");
  }
}

typedef dt_progress_t *dt_lua_backgroundjob_t;

static int _job_canceled(lua_State *L)
{
  lua_getiuservalue(L, 1, 1);
  lua_getfield(L, -1, "cancel_callback");
  lua_pushvalue(L, -3);
  lua_call(L, 1, 0);
  lua_pop(L, 2);
  return 0;
}

static void _lua_job_cancelled(dt_progress_t *progress, gpointer user_data)
{
  dt_lua_async_call_alien(_job_canceled,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME, "dt_lua_backgroundjob_t", progress,
      LUA_ASYNC_DONE);
}

static int _lua_create_job(lua_State *L)
{
  const char *message = luaL_checkstring(L, 1);
  gboolean has_progress_bar = lua_toboolean(L, 2);
  int cancellable = FALSE;
  if(!lua_isnoneornil(L, 3))
  {
    luaL_checktype(L, 3, LUA_TFUNCTION);
    cancellable = TRUE;
  }
  dt_progress_t *progress = dt_control_progress_create(darktable.control, has_progress_bar, message);
  if(cancellable)
  {
    dt_control_progress_make_cancellable(darktable.control, progress, _lua_job_cancelled, progress);
  }
  luaA_push(L, dt_lua_backgroundjob_t, &progress);
  if(cancellable)
  {
    lua_getiuservalue(L, -1, 1);
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, "cancel_callback");
    lua_pop(L, 1);
  }
  return 1;
}

static int _lua_job_progress(lua_State *L)
{
  dt_progress_t *progress;
  luaA_to(L, dt_lua_backgroundjob_t, &progress, 1);
  dt_pthread_mutex_lock(&darktable.control->progress_system.mutex);
  GList *iter = g_list_find(darktable.control->progress_system.list, progress);
  dt_pthread_mutex_unlock(&darktable.control->progress_system.mutex);
  if(!iter) luaL_error(L, "Accessing an invalid job");
  if(lua_isnone(L, 3))
  {
    double result = dt_control_progress_get_progress(progress);
    if(!dt_control_progress_has_progress_bar(progress))
      lua_pushnil(L);
    else
      lua_pushnumber(L, result);
    return 1;
  }
  else
  {
    double value;
    luaA_to(L, progress_double, &value, 3);
    dt_control_progress_set_progress(darktable.control, progress, value);
    return 0;
  }
}

static int _lua_job_valid(lua_State *L)
{
  dt_progress_t *progress;
  luaA_to(L, dt_lua_backgroundjob_t, &progress, 1);
  if(lua_isnone(L, 3))
  {
    dt_pthread_mutex_lock(&darktable.control->progress_system.mutex);
    GList *iter = g_list_find(darktable.control->progress_system.list, progress);
    dt_pthread_mutex_unlock(&darktable.control->progress_system.mutex);

    if(iter)
      lua_pushboolean(L, true);
    else
      lua_pushboolean(L, false);

    return 1;
  }
  else
  {
    int validity = lua_toboolean(L, 3);
    if(validity) return luaL_argerror(L, 3, "a job can not be made valid");
    dt_control_progress_destroy(darktable.control, progress);
    return 0;
  }
}

static void _on_mouse_over_image_changed(gpointer instance, gpointer user_data)
{
  dt_imgid_t imgid = dt_control_get_mouse_over_id();
  if(dt_is_valid_imgid(imgid))
  {
    dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
        0, NULL, NULL,
        LUA_ASYNC_TYPENAME, "char*", "mouse-over-image-changed",
        LUA_ASYNC_TYPENAME, "dt_lua_image_t", imgid,
        LUA_ASYNC_DONE);
  }
  else
  {
    dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
        0, NULL, NULL,
        LUA_ASYNC_TYPENAME, "char*", "mouse-over-image-changed",
        LUA_ASYNC_DONE);
  }
}


int dt_lua_init_gui(lua_State *L)
{

  if(darktable.gui != NULL)
  {
    /* images */
    dt_lua_push_darktable_lib(L);
    luaA_Type type_id = dt_lua_init_singleton(L, "gui_lib", NULL);
    lua_setfield(L, -2, "gui");
    lua_pop(L, 1);

    lua_pushcfunction(L, _selection_cb);
    dt_lua_gtk_wrap(L);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "selection");
    lua_pushcfunction(L, _hovered_cb);
    dt_lua_type_register_const_type(L, type_id, "hovered");
    lua_pushcfunction(L, _act_on_cb);
    dt_lua_type_register_const_type(L, type_id, "action_images");
    lua_pushcfunction(L, _current_view_cb);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "current_view");
    lua_pushcfunction(L, _action_cb);
    dt_lua_gtk_wrap(L);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "action");
    lua_pushcfunction(L, _mimic_cb);
    dt_lua_gtk_wrap(L);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "mimic");
    lua_pushcfunction(L, _panel_visible_cb);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "panel_visible");
    lua_pushcfunction(L, _panel_hide_cb);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "panel_hide");
    lua_pushcfunction(L, _panel_show_cb);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "panel_show");
    lua_pushcfunction(L, _panel_hide_all_cb);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "panel_hide_all");
    lua_pushcfunction(L, _panel_show_all_cb);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "panel_show_all");
    lua_pushcfunction(L, _panel_get_size_cb);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "panel_get_size");
    lua_pushcfunction(L, _panel_set_size_cb);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "panel_set_size");
    lua_pushcfunction(L, _lua_create_job);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "create_job");
    dt_lua_module_push(L, "lib");
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "libs");
    dt_lua_module_push(L, "view");
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "views");

    luaA_enum(L, dt_ui_panel_t);
    luaA_enum_value(L, dt_ui_panel_t, DT_UI_PANEL_TOP);
    luaA_enum_value(L, dt_ui_panel_t, DT_UI_PANEL_CENTER_TOP);
    luaA_enum_value(L, dt_ui_panel_t, DT_UI_PANEL_CENTER_BOTTOM);
    luaA_enum_value(L, dt_ui_panel_t, DT_UI_PANEL_LEFT);
    luaA_enum_value(L, dt_ui_panel_t, DT_UI_PANEL_RIGHT);
    luaA_enum_value(L, dt_ui_panel_t, DT_UI_PANEL_BOTTOM);
    luaA_enum_value(L, dt_ui_panel_t, DT_UI_PANEL_SIZE);



    // create a type describing a job object
    int job_type = dt_lua_init_gpointer_type(L, dt_lua_backgroundjob_t);
    lua_pushcfunction(L, _lua_job_progress);
    dt_lua_type_register_type(L, job_type, "percent");
    lua_pushcfunction(L, _lua_job_valid);
    dt_lua_type_register_type(L, job_type, "valid");

    // allow to react to highlighting an image
    lua_pushcfunction(L, dt_lua_event_multiinstance_register);
    lua_pushcfunction(L, dt_lua_event_multiinstance_destroy);
    lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
    dt_lua_event_add(L, "mouse-over-image-changed");
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE, G_CALLBACK(_on_mouse_over_image_changed), NULL);
  }
  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

