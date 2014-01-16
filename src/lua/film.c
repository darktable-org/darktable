
/*
   This file is part of darktable,
   copyright (c) 2013 Jeremy Rosen

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

#include "lua/film.h"
#include "lua/types.h"
#include "lua/image.h"
#include "common/film.h"
#include "common/debug.h"
#include "common/grealpath.h"
#include <errno.h>


typedef enum
{
  PATH,
  ID,
  LAST_FILM_FIELD
} film_fields;
const char *film_fields_name[] =
{
  "path",
  "id",
  NULL
};
static int film_index(lua_State *L)
{
  dt_lua_film_t film_id;
  luaA_to(L,dt_lua_film_t,&film_id,-2);
  switch(luaL_checkoption(L,-1,NULL,film_fields_name))
  {
    case PATH:
      {
        sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select folder from film_rolls where id  = ?1", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
        if(sqlite3_step(stmt) == SQLITE_ROW)
        {
          lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
        }
        else
        {
          sqlite3_finalize(stmt);
          return luaL_error(L,"should never happen");
        }
        sqlite3_finalize(stmt);
        break;
      }
    case ID:
      lua_pushinteger(L,film_id);
      break;
  }
  return 1;
}

static int film_tostring(lua_State *L)
{
  dt_lua_film_t film_id;
  luaA_to(L,dt_lua_film_t,&film_id,-1);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select folder from film_rolls where id  = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
  }
  else
  {
    sqlite3_finalize(stmt);
    return luaL_error(L,"should never happen");
  }
  sqlite3_finalize(stmt);
  return 1;
}


static int film_len(lua_State*L)
{
  dt_lua_film_t film_id;
  luaA_to(L,dt_lua_film_t,&film_id,-1);
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),"select count(*) from images where film_id = ?1  ", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  if(sqlite3_step(stmt) == SQLITE_ROW) {
    lua_pushnumber(L,sqlite3_column_int(stmt, 0));
  }else{
    lua_pushnumber(L,0);
  }
  sqlite3_finalize(stmt);
  return 1;
}

static int film_getnum(lua_State*L)
{
  int index = luaL_checkinteger(L,-1);
  dt_lua_film_t film_id;
  luaA_to(L,dt_lua_film_t,&film_id,-2);
  sqlite3_stmt *stmt = NULL;
  char query[1024];
  sprintf(query,"select id from images where film_id = ?1 order by id limit 1 offset %d",index -1);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_lua_image_t imgid = sqlite3_column_int(stmt, 0);
    luaA_push(L,dt_lua_image_t,&imgid);
    sqlite3_finalize(stmt);
  }
  else
  {
    sqlite3_finalize(stmt);
    return luaL_error(L,"incorrect index in database");
  }
  return 1;
}
static int films_len(lua_State*L)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),"select count(*) from film_rolls ", -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) {
    lua_pushnumber(L,sqlite3_column_int(stmt, 0));
  }else{
    lua_pushnumber(L,0);
  }
  sqlite3_finalize(stmt);
  return 1;
}

static int films_index(lua_State*L)
{
  int index = luaL_checkinteger(L,-1);
  sqlite3_stmt *stmt = NULL;
  char query[1024];
  sprintf(query,"select id from film_rolls order by id limit 1 offset %d",index -1);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int film_id = sqlite3_column_int(stmt, 0);
    luaA_push(L,dt_lua_film_t,&film_id);
    sqlite3_finalize(stmt);
  }
  else
  {
    sqlite3_finalize(stmt);
    return luaL_error(L,"incorrect index in database");
  }
  return 1;
}

static int films_new(lua_State *L)
{
  const char * path = luaL_checkstring(L,-1);
  char * expanded_path = dt_util_fix_path(path);
  char * final_path = g_realpath(expanded_path);
  free(expanded_path);
  if(!final_path) {
    return luaL_error(L,"Couldn't create film for directory '%s' : %s\n",path,strerror(errno));
  }

  dt_film_t my_film;
  dt_film_init(&my_film);
  int film_id = dt_film_new(&my_film,final_path);
  free(final_path);
  if(film_id) {
    luaA_push(L,dt_lua_film_t,&film_id);
    return 1;
  } else {
    return luaL_error(L,"Couldn't create film for directory %s\n",path);
  }
}
///////////////
// toplevel and common
///////////////

int dt_lua_init_film(lua_State * L)
{

  dt_lua_init_int_type(L,dt_lua_film_t);
  dt_lua_register_type_callback_list(L,dt_lua_film_t,film_index,NULL,film_fields_name);
  dt_lua_register_type_callback_number(L,dt_lua_film_t,film_getnum,NULL,film_len);
  luaL_getmetatable(L,"dt_lua_film_t");
  lua_pushcfunction(L,film_tostring);
  lua_setfield(L,-2,"__tostring");
  lua_pop(L,1);

  /* film table */
  dt_lua_push_darktable_lib(L);
  luaA_Type type_id = dt_lua_init_singleton(L,"film_database");
  lua_setfield(L,-2,"films");
  lua_pop(L,1);

  dt_lua_register_type_callback_number_typeid(L,type_id,films_index,NULL,films_len);
  lua_pushcfunction(L,films_new);
  dt_lua_register_type_callback_stack_typeid(L,type_id,"new");

  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
