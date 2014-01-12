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
#include "lua/lua.h"
#include "lua/init.h"
#include "lua/call.h"
#include "lua/configuration.h"
#include "lua/database.h"
#include "lua/glist.h"
#include "lua/gui.h"
#include "lua/image.h"
#include "lua/preferences.h"
#include "lua/print.h"
#include "lua/types.h"
#include "lua/tags.h"
#include "lua/modules.h"
#include "lua/storage.h"
#include "lua/events.h"
#include "lua/styles.h"
#include "lua/film.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "control/jobs.h"


// closed on GC of the dt lib, usually when the lua interpreter closes
static int dt_luacleanup(lua_State*L)
{
  /* TBSL : redo when DT is a lua lib
  const int init_gui = (darktable.gui != NULL);
  if(!init_gui)
    dt_cleanup();
    */
  return 0;
}


/**
  hardcoded list of types to register
  other types can be added dynamically
 */
static lua_CFunction init_funcs[] =
{
  dt_lua_init_glist,
  dt_lua_init_image,
  dt_lua_init_styles,
  dt_lua_init_print,
  dt_lua_init_configuration,
  dt_lua_init_preferences,
  dt_lua_init_database,
  dt_lua_init_gui,
  dt_lua_init_storages,
  dt_lua_init_tags,
  dt_lua_init_events,
  dt_lua_init_film,
  dt_lua_init_call,
  NULL
};


void dt_lua_init_early(lua_State*L)
{
  if(!L)
    L= luaL_newstate();
  darktable.lua_state.state= L;
  dt_lua_init_lock();
  luaL_openlibs(darktable.lua_state.state);
  luaA_open();
  dt_lua_push_darktable_lib(L);
  // set the metatable
  lua_newtable(L);
  lua_pushcfunction(L,dt_luacleanup);
  lua_setfield(L,-2,"__gc");
  lua_setmetatable(L,-2);

  lua_pop(L,1);

  /* types need to be initialized early */
  dt_lua_initialize_types(L);
  /* modules need to be initialized before the are used */
  dt_lua_init_modules(L);

}

static int32_t run_early_script(struct dt_job_t *job) {
  char tmp_path[PATH_MAX];
  lua_State *L = darktable.lua_state.state;
  gboolean has_lock = dt_lua_lock();
  // run global init script
  dt_loc_get_datadir(tmp_path, PATH_MAX);
  g_strlcat(tmp_path,"/luarc",PATH_MAX);
  dt_lua_dofile(L,tmp_path);
  // run user init script
  dt_loc_get_user_config_dir(tmp_path, PATH_MAX);
  g_strlcat(tmp_path,"/luarc",PATH_MAX);
  dt_lua_dofile(L,tmp_path);
  dt_lua_unlock(has_lock);
  return 0;
}



void dt_lua_init(lua_State*L,const int init_gui)
{
  /*
     Note to reviewers
     this is the only place where lua code is run without the lua lock.
     At this point, no user script has been called,
     so we are completely thread-safe. no need to lock

     This is also the only place where lua code is run with the gdk lock
     held, but this is not a problem because it is very brief, user calls
     are delegated to a secondary job
     */
  char tmp_path[PATH_MAX];
  // init the lua environment
  lua_CFunction* cur_type = init_funcs;
  while(*cur_type)
  {
    (*cur_type)(L);
    cur_type++;
  }
  // build the table containing the configuration info

  lua_getglobal(L,"package");
  dt_lua_goto_subtable(L,"loaded");
  lua_pushstring(L,"darktable");
  dt_lua_push_darktable_lib(L);
  lua_settable(L,-3);
  lua_pop(L,1);

  lua_getglobal(L,"package");
  lua_getfield(L,-1,"path");
  lua_pushstring(L,";");
  dt_loc_get_datadir(tmp_path, PATH_MAX);
  lua_pushstring(L,tmp_path);
  lua_pushstring(L,"/lua/?.lua");
  lua_pushstring(L,";");
  dt_loc_get_user_config_dir(tmp_path, PATH_MAX);
  lua_pushstring(L,tmp_path);
  lua_pushstring(L,"/lua/?.lua");
  lua_concat(L,7);
  lua_setfield(L,-2,"path");
  lua_pop(L,1);



  if(init_gui)
  {
    dt_job_t job;
    dt_control_job_init(&job, "lua: run initial script");
    job.execute = &run_early_script;
    dt_control_add_job(darktable.control, &job);
  }

}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
