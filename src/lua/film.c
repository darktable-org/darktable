
/*
   This file is part of darktable,
   Copyright (C) 2013-2023 darktable developers.

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
#include "common/debug.h"
#include "common/film.h"
#includd "common/utility.h"
#include "common/grealpath.h"
#include "lua/database.h"
#include "lua/image.h"
#include "lua/types.h"
#include <errno.h>

static int path_member(lua_State *L)
{
  dt_lua_film_t film_id;
  luaA_to(L, dt_lua_film_t, &film_id, 1);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT folder FROM main.film_rolls WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    lua_pushstring(L, (char *)sqlite3_column_text(stmt, 0));
  }
  else
  {
    sqlite3_finalize(stmt);
    return luaL_error(L, "should never happen");
  }
  sqlite3_finalize(stmt);
  return 1;
}

static int id_member(lua_State *L)
{
  dt_lua_film_t film_id;
  luaA_to(L, dt_lua_film_t, &film_id, 1);
  lua_pushinteger(L, film_id);
  return 1;
}


static int film_delete(lua_State *L)
{
  dt_lua_film_t film_id;
  luaA_to(L, dt_lua_film_t, &film_id, 1);
  gboolean force = lua_toboolean(L, 2);
  if(force || dt_film_is_empty(film_id))
  {
    dt_film_remove(film_id);
  }
  else
  {
    return luaL_error(L, "Can't delete film, film is not empty");
  }
  return 0;
}

static int film_tostring(lua_State *L)
{
  dt_lua_film_t film_id;
  luaA_to(L, dt_lua_film_t, &film_id, -1);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT folder FROM main.film_rolls WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    lua_pushstring(L, (char *)sqlite3_column_text(stmt, 0));
  }
  else
  {
    sqlite3_finalize(stmt);
    return luaL_error(L, "should never happen");
  }
  sqlite3_finalize(stmt);
  return 1;
}


static int film_len(lua_State *L)
{
  dt_lua_film_t film_id;
  luaA_to(L, dt_lua_film_t, &film_id, -1);
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.images WHERE film_id = ?1  ", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    lua_pushinteger(L, sqlite3_column_int(stmt, 0));
  }
  else
  {
    lua_pushinteger(L, 0);
  }
  sqlite3_finalize(stmt);
  return 1;
}

static int film_getnum(lua_State *L)
{
  int index = luaL_checkinteger(L, -1);
  if(index < 1)
  {
    return luaL_error(L, "incorrect index in database");
  }
  dt_lua_film_t film_id;
  luaA_to(L, dt_lua_film_t, &film_id, -2);
  sqlite3_stmt *stmt = NULL;
  char query[1024];
  snprintf(query, sizeof(query), "SELECT id FROM main.images WHERE film_id = ?1 ORDER BY id LIMIT 1 OFFSET %d",
           index - 1);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_lua_image_t imgid = sqlite3_column_int(stmt, 0);
    luaA_push(L, dt_lua_image_t, &imgid);
    sqlite3_finalize(stmt);
  }
  else
  {
    sqlite3_finalize(stmt);
    return luaL_error(L, "incorrect index in database");
  }
  return 1;
}
static int films_len(lua_State *L)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT COUNT(*) FROM main.film_rolls ", -1, &stmt,
                              NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    lua_pushinteger(L, sqlite3_column_int(stmt, 0));
  }
  else
  {
    lua_pushinteger(L, 0);
  }
  sqlite3_finalize(stmt);
  return 1;
}

static int films_index(lua_State *L)
{
  int index = luaL_checkinteger(L, -1);
  if(index < 1)
  {
    return luaL_error(L, "incorrect index in database");
  }
  sqlite3_stmt *stmt = NULL;
  char query[1024];
  snprintf(query, sizeof(query), "SELECT id FROM main.film_rolls ORDER BY id LIMIT 1 OFFSET %d", index - 1);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int film_id = sqlite3_column_int(stmt, 0);
    luaA_push(L, dt_lua_film_t, &film_id);
  }
  else
  {
    lua_pushnil(L);
  }
  sqlite3_finalize(stmt);
  return 1;
}

static int films_new(lua_State *L)
{
  const char *path = luaL_checkstring(L, -1);
  char *expanded_path = dt_util_fix_path(path);
  char *final_path = g_realpath(expanded_path);
  g_free(expanded_path);
  if(!final_path)
  {
    return luaL_error(L, "Couldn't create film for directory '%s' : %s\n", path, strerror(errno));
  }

  dt_film_t my_film;
  dt_film_init(&my_film);
  int film_id = dt_film_new(&my_film, final_path);
  g_free(final_path);
  if(film_id)
  {
    luaA_push(L, dt_lua_film_t, &film_id);
    return 1;
  }
  else
  {
    return luaL_error(L, "Couldn't create film for directory %s\n", path);
  }
}
///////////////
// toplevel and common
///////////////

int dt_lua_init_film(lua_State *L)
{

  dt_lua_init_int_type(L, dt_lua_film_t);
  lua_pushcfunction(L, film_delete);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_film_t, "delete");
  lua_pushcfunction(L, path_member);
  dt_lua_type_register(L, dt_lua_film_t, "path");
  lua_pushcfunction(L, id_member);
  dt_lua_type_register(L, dt_lua_film_t, "id");

  lua_pushcfunction(L, film_len);
  lua_pushcfunction(L, film_getnum);
  dt_lua_type_register_number_const(L, dt_lua_film_t);
  lua_pushcfunction(L, dt_lua_move_image);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_film_t, "move_image");
  lua_pushcfunction(L, dt_lua_copy_image);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_film_t, "copy_image");
  lua_pushcfunction(L, film_tostring);
  dt_lua_type_setmetafield(L,dt_lua_film_t,"__tostring");

  /* film table */
  dt_lua_push_darktable_lib(L);
  luaA_Type type_id = dt_lua_init_singleton(L, "film_database", NULL);
  lua_setfield(L, -2, "films");
  lua_pop(L, 1);

  lua_pushcfunction(L, films_len);
  lua_pushcfunction(L, films_index);
  dt_lua_type_register_number_const_type(L, type_id);
  lua_pushcfunction(L, films_new);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "new");
  lua_pushcfunction(L, film_delete);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "delete");

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

