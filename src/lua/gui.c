/*
   This file is part of darktable,
   copyright (c) 2012 Jeremy Rosen

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
#include "lua/call.h"
#include "lua/image.h"
#include "lua/types.h"
#include <glib.h>

/***********************************************************************
  Creating the images global variable
 **********************************************************************/

static int selection_cb(lua_State *L)
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
      int imgid;
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
  while(image)
  {
    luaA_push(L, dt_lua_image_t, &image->data);
    luaL_ref(L, -2);
    image = g_list_delete_link(image, image);
  }
  return 1;
}

static int hovered_cb(lua_State *L)
{
  int32_t mouse_over_id = dt_control_get_mouse_over_id();
  if(mouse_over_id == -1)
  {
    lua_pushnil(L);
  }
  else
  {
    luaA_push(L, dt_lua_image_t, &mouse_over_id);
  }
  return 1;
}

static int act_on_cb(lua_State *L)
{

  int32_t imgid = dt_view_get_image_to_act_on();
  lua_newtable(L);
  if(imgid != -1)
  {
    luaA_push(L, dt_lua_image_t, &imgid);
    luaL_ref(L, -2);
    return 1;
  }
  else
  {
    GList *image = dt_collection_get_selected(darktable.collection, -1);
    while(image)
    {
      luaA_push(L, dt_lua_image_t, &image->data);
      luaL_ref(L, -2);
      image = g_list_delete_link(image, image);
    }
    return 1;
  }
}


static int current_view_cb(lua_State *L)
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



typedef dt_progress_t *dt_lua_backgroundjob_t;

static int job_canceled(lua_State *L)
{
  lua_getuservalue(L, 1);
  lua_getfield(L, -1, "cancel_callback");
  lua_pushvalue(L, -3);
  lua_call(L,1,0);
  lua_pop(L, 2);
  return 0;
}

static void lua_job_cancelled(dt_progress_t *progress, gpointer user_data)
{
  dt_lua_async_call_alien(job_canceled,
      0, NULL,NULL,
      LUA_ASYNC_TYPENAME,"dt_lua_backgroundjob_t",progress,
      LUA_ASYNC_DONE);
}

static int lua_create_job(lua_State *L)
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
    dt_control_progress_make_cancellable(darktable.control, progress, lua_job_cancelled, progress);
  }
  luaA_push(L, dt_lua_backgroundjob_t, &progress);
  if(cancellable)
  {
    lua_getuservalue(L, -1);
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, "cancel_callback");
    lua_pop(L, 1);
  }
  return 1;
}

static int lua_job_progress(lua_State *L)
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

static int lua_job_valid(lua_State *L)
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

static void on_mouse_over_image_changed(gpointer instance, gpointer user_data)
{
  int imgid = dt_control_get_mouse_over_id();
  if(imgid != -1) {
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0, NULL,NULL,
      LUA_ASYNC_TYPENAME,"char*","mouse-over-image-changed",
      LUA_ASYNC_TYPENAME,"dt_lua_image_t",imgid,
      LUA_ASYNC_DONE);
  } else {
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0, NULL,NULL,
      LUA_ASYNC_TYPENAME,"char*","mouse-over-image-changed",
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

    lua_pushcfunction(L, selection_cb);
    dt_lua_gtk_wrap(L);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "selection");
    lua_pushcfunction(L, hovered_cb);
    dt_lua_type_register_const_type(L, type_id, "hovered");
    lua_pushcfunction(L, act_on_cb);
    dt_lua_type_register_const_type(L, type_id, "action_images");
    lua_pushcfunction(L, current_view_cb);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "current_view");
    lua_pushcfunction(L, lua_create_job);
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "create_job");
    dt_lua_module_push(L, "lib");
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "libs");
    dt_lua_module_push(L, "view");
    lua_pushcclosure(L, dt_lua_type_member_common, 1);
    dt_lua_type_register_const_type(L, type_id, "views");



    // create a type describing a job object
    int job_type = dt_lua_init_gpointer_type(L, dt_lua_backgroundjob_t);
    lua_pushcfunction(L, lua_job_progress);
    dt_lua_type_register_type(L, job_type, "percent");
    lua_pushcfunction(L, lua_job_valid);
    dt_lua_type_register_type(L, job_type, "valid");

    // allow to react to highlighting an image
    lua_pushcfunction(L, dt_lua_event_multiinstance_register);
    lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
    dt_lua_event_add(L, "mouse-over-image-changed");
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE, G_CALLBACK(on_mouse_over_image_changed), NULL);
  }
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
