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
#include "lua/init.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "control/jobs.h"
#include "lua/cairo.h"
#include "lua/call.h"
#include "lua/configuration.h"
#include "lua/database.h"
#include "lua/events.h"
#include "lua/film.h"
#include "lua/format.h"
#include "lua/gettext.h"
#include "lua/glist.h"
#include "lua/gui.h"
#include "lua/guides.h"
#include "lua/image.h"
#include "lua/lib.h"
#include "lua/lua.h"
#include "lua/lualib.h"
#include "lua/luastorage.h"
#include "lua/modules.h"
#include "lua/password.h"
#include "lua/preferences.h"
#include "lua/print.h"
#include "lua/storage.h"
#include "lua/styles.h"
#include "lua/tags.h"
#include "lua/types.h"
#include "lua/view.h"
#include "lua/widget/widget.h"

static int dt_lua_init_init(lua_State*L)
{
  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_destroy);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L,"exit");
  return 0;
}
// closed on GC of the dt lib, usually when the lua interpreter closes
static int dt_luacleanup(lua_State *L)
{
  if(darktable.lua_state.ending) return 0;
  darktable.lua_state.ending = true;
  dt_cleanup();
  return 0;
}


static lua_CFunction early_init_funcs[]
    = { dt_lua_init_early_types,  dt_lua_init_early_events,  dt_lua_init_early_modules,
        dt_lua_init_early_format, dt_lua_init_early_storage, dt_lua_init_early_lib,
        dt_lua_init_early_view,   NULL };

static int dt_call_after_load(lua_State *L)
{
  return luaL_error(L, "Attempt to initialize DT twice");
}

void dt_lua_init_early(lua_State *L)
{
  if(!L)
  {
    L = luaL_newstate();
  }
  darktable.lua_state.state = L;
  darktable.lua_state.ending = false;
  darktable.lua_state.loop = NULL;;
  darktable.lua_state.context = NULL;;
  darktable.lua_state.stacked_job_queue = NULL;;
  dt_lua_init_lock(); // lock is initialized in the locked state
  luaL_openlibs(darktable.lua_state.state);
  luaA_open(L);
  dt_lua_push_darktable_lib(L);
  // set the metatable
  lua_getmetatable(L, -1);
  lua_pushcfunction(L, dt_call_after_load);
  lua_setfield(L, -2, "__call");
  lua_pushcfunction(L, dt_luacleanup);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  lua_pop(L, 1);

  lua_CFunction *cur_type = early_init_funcs;
  while(*cur_type)
  {
    (*cur_type)(L);
    cur_type++;
  }
}

static int run_early_script(lua_State* L)
{
  char basedir[PATH_MAX] = { 0 };
  // run global init script
  dt_loc_get_datadir(basedir, sizeof(basedir));
  char *luarc = g_build_filename(basedir, "luarc", NULL);
  dt_lua_check_print_error(L, luaL_dofile(L, luarc));
  g_free(luarc);
  if(darktable.gui != NULL)
  {
    // run user init script
    dt_loc_get_user_config_dir(basedir, sizeof(basedir));
    luarc = g_build_filename(basedir, "luarc", NULL);
    dt_lua_check_print_error(L, luaL_dofile(L, luarc));
    g_free(luarc);
  }
  if(!lua_isnil(L,1)){
    const char *lua_command = lua_tostring(L, 1);
    dt_lua_check_print_error(L,luaL_dostring(L,lua_command));
  }
  dt_lua_redraw_screen();
  return 0;
}


static lua_CFunction init_funcs[]
    = { dt_lua_init_glist,         dt_lua_init_image,       dt_lua_init_styles,   dt_lua_init_print,
        dt_lua_init_configuration, dt_lua_init_preferences, dt_lua_init_database, dt_lua_init_gui,
        dt_lua_init_luastorages,   dt_lua_init_tags,        dt_lua_init_film,     dt_lua_init_call,
        dt_lua_init_view,          dt_lua_init_events,      dt_lua_init_init,     dt_lua_init_widget,
        dt_lua_init_lualib,        dt_lua_init_gettext,     dt_lua_init_guides,   dt_lua_init_cairo,
        dt_lua_init_password,      NULL };


void dt_lua_init(lua_State *L, const char *lua_command)
{
  char tmp_path[PATH_MAX] = { 0 };
  // init the lua environment
  lua_CFunction *cur_type = init_funcs;
  while(*cur_type)
  {
    (*cur_type)(L);
    cur_type++;
  }
  assert(lua_gettop(L)
         == 0); // if you are here, you have probably added an initialisation function that is not stack clean

  // build the table containing the configuration info

  lua_getglobal(L, "package");
  dt_lua_goto_subtable(L, "loaded");
  lua_pushstring(L, "darktable");
  dt_lua_push_darktable_lib(L);
  lua_settable(L, -3);
  lua_pop(L, 1);

  lua_getglobal(L, "package");
  lua_getfield(L, -1, "path");
  lua_pushstring(L, ";");
  dt_loc_get_datadir(tmp_path, sizeof(tmp_path));
  lua_pushstring(L, tmp_path);
  lua_pushstring(L, "/lua/?.lua");
  lua_pushstring(L, ";");
  dt_loc_get_user_config_dir(tmp_path, sizeof(tmp_path));
  lua_pushstring(L, tmp_path);
  lua_pushstring(L, "/lua/?.lua");
  lua_concat(L, 7);
  lua_setfield(L, -2, "path");
  lua_pop(L, 1);



  lua_pushcfunction(L,run_early_script);
  lua_pushstring(L,lua_command);

  dt_lua_async_call(L,1,0,NULL,NULL);
  // allow other threads to wake up and do their job
  dt_lua_unlock();
}


/*
   Note to proofreaders
   argv is a char*[]
   lua strings are const char*
   gtk takes argv and modifies it to remove gtk specific parts

   so we need to copy (strdup) parameters from lua
   but because gtk might do crazy stuff, we keep a copy of our original argv to be able to free() it
   */
static int load_from_lua(lua_State *L)
{
  if(darktable.lua_state.state)
  {
    luaL_error(L, "Attempt to load darktable multiple time.");
  }
  int argc = lua_gettop(L);

  char **argv = calloc(argc + 1, sizeof(char *));
  char **argv_copy = malloc(sizeof(char *) * (argc + 1));
  argv[0] = strdup("lua");
  argv_copy[0] = argv[0];
  for(int i = 1; i < argc; i++)
  {
    argv[i] = strdup(luaL_checkstring(L, i + 1));
    argv_copy[i] = argv[i];
  }
  lua_pop(L, lua_gettop(L));
  argv[argc] = NULL;
  argv_copy[argc] = NULL;
  gtk_init(&argc, &argv);
  if(dt_init(argc, argv, FALSE, TRUE, L)) {
    luaL_error(L,"Starting darktable failed.");
  }
  for(int i = 0; i < argc; i++)
  {
    free(argv_copy[i]);
  }
  free(argv_copy);
  free(argv);
  dt_lua_push_darktable_lib(L);
  return 1;
}
// function used by the lua interpreter to load darktable
int luaopen_darktable(lua_State *L)
{
  dt_lua_push_darktable_lib(L);
  lua_getmetatable(L, -1);
  lua_pushcfunction(L, load_from_lua);
  lua_setfield(L, -2, "__call");
  lua_pop(L, 1);
  return 1;
}
void dt_lua_finalize_early()
{
  darktable.lua_state.ending = true;
  dt_lua_lock();
  dt_lua_event_trigger(darktable.lua_state.state,"exit",0);
  dt_lua_unlock();
  g_main_context_wakeup(darktable.lua_state.context);
}

void dt_lua_finalize()
{
  dt_lua_lock();
  luaA_close(darktable.lua_state.state);
  lua_close(darktable.lua_state.state);
  darktable.lua_state.state = NULL;
  // never unlock
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
