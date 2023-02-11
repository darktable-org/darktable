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
#include "lua/tags.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/tags.h"
#include "control/signal.h"
#include "lua/image.h"
#include "lua/types.h"


static int tag_name(lua_State *L)
{
  dt_lua_tag_t tagid1;
  luaA_to(L, dt_lua_tag_t, &tagid1, -2);
  gchar *name = dt_tag_get_name(tagid1);
  lua_pushstring(L, name);
  free(name);
  return 1;
}

static int tag_flags(lua_State *L)
{
  dt_lua_tag_t tagid1;
  luaA_to(L, dt_lua_tag_t, &tagid1, -2);
  gint flags = dt_tag_get_flags(tagid1);
  lua_pushinteger(L, flags);
  return 1;
}

static int tag_synonyms(lua_State *L)
{
  dt_lua_tag_t tagid1;
  luaA_to(L, dt_lua_tag_t, &tagid1, -2);
  gchar *synonyms = dt_tag_get_synonyms(tagid1);
  lua_pushstring(L, synonyms);
  free(synonyms);
  return 1;
}

static int tag_tostring(lua_State *L)
{
  dt_lua_tag_t tagid1;
  luaA_to(L, dt_lua_tag_t, &tagid1, -1);
  gchar *name = dt_tag_get_name(tagid1);
  lua_pushstring(L, name);
  free(name);
  return 1;
}

static int tag_length(lua_State *L)
{
  dt_lua_tag_t tagid;
  luaA_to(L, dt_lua_tag_t, &tagid, -1);
  sqlite3_stmt *stmt;
  int rv, count = -1;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.tagged_images WHERE tagid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  rv = sqlite3_step(stmt);
  if(rv != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    return luaL_error(L, "unknown SQL error");
  }
  count = sqlite3_column_int(stmt, 0);
  lua_pushinteger(L, count);
  sqlite3_finalize(stmt);
  return 1;
}
static int tag_index(lua_State *L)
{
  dt_lua_tag_t tagid;
  luaA_to(L, dt_lua_tag_t, &tagid, -2);
  int index = luaL_checkinteger(L, -1);
  if(index < 1)
  {
    return luaL_error(L, "incorrect index in database");
  }
  sqlite3_stmt *stmt = NULL;
  char query[1024];
  snprintf(query, sizeof(query),
           "SELECT imgid FROM main.tagged_images WHERE tagid=?1 ORDER BY imgid LIMIT 1 OFFSET %d", index - 1);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    luaA_push(L, dt_lua_image_t, &imgid);
  }
  else
  {
    sqlite3_finalize(stmt);
    luaL_error(L, "incorrect index in database");
  }
  sqlite3_finalize(stmt);
  return 1;
}


static int tag_lib_length(lua_State *L)
{
  sqlite3_stmt *stmt;
  int rv, count = -1;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT COUNT(*) FROM data.tags", -1, &stmt, NULL);
  rv = sqlite3_step(stmt);
  if(rv != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    return luaL_error(L, "unknown SQL error");
  }
  count = sqlite3_column_int(stmt, 0);
  lua_pushinteger(L, count);
  sqlite3_finalize(stmt);
  return 1;
}
static int tag_lib_index(lua_State *L)
{
  int index = luaL_checkinteger(L, -1);
  sqlite3_stmt *stmt = NULL;
  char query[1024];
  snprintf(query, sizeof(query), "SELECT id FROM data.tags ORDER BY id LIMIT 1 OFFSET %d", index - 1);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int tagid = sqlite3_column_int(stmt, 0);
    luaA_push(L, dt_lua_tag_t, &tagid);
  }
  else
  {
    lua_pushnil(L);
  }
  sqlite3_finalize(stmt);
  return 1;
}

static int tag_lib_create(lua_State *L)
{
  const char *name = luaL_checkstring(L, 1);
  dt_lua_tag_t tagid;
  if(!dt_tag_new_from_gui(name, &tagid))
  {
    return luaL_error(L, "error creating tag %s\n", name);
  }
  luaA_push(L, dt_lua_tag_t, &tagid);
  return 1;
}

static int tag_delete(lua_State *L)
{
  dt_lua_tag_t tagid;
  luaA_to(L, dt_lua_tag_t, &tagid, -1);

  GList *tagged_images = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.tagged_images WHERE tagid=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    tagged_images = g_list_append(tagged_images, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  }
  sqlite3_finalize(stmt);

  if(dt_tag_remove(tagid, TRUE))
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);

  for(const GList *list_iter = tagged_images; list_iter; list_iter = g_list_next(list_iter))
  {
    dt_image_synch_xmp(GPOINTER_TO_INT(list_iter->data));
  }
  g_list_free(tagged_images);

  return 0;
}


int dt_lua_tag_attach(lua_State *L)
{
  dt_lua_image_t imgid = -1;
  dt_lua_tag_t tagid = 0;
  if(luaL_testudata(L, 1, "dt_lua_image_t"))
  {
    luaA_to(L, dt_lua_image_t, &imgid, 1);
    luaA_to(L, dt_lua_tag_t, &tagid, 2);
  }
  else
  {
    luaA_to(L, dt_lua_tag_t, &tagid, 1);
    luaA_to(L, dt_lua_image_t, &imgid, 2);
  }
  if(dt_tag_attach(tagid, imgid, TRUE, TRUE))
  {
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
    dt_image_synch_xmp(imgid);
  }
  return 0;
}

int dt_lua_tag_detach(lua_State *L)
{
  dt_lua_image_t imgid;
  dt_lua_tag_t tagid;
  if(luaL_testudata(L, 1, "dt_lua_image_t"))
  {
    luaA_to(L, dt_lua_image_t, &imgid, 1);
    luaA_to(L, dt_lua_tag_t, &tagid, 2);
  }
  else
  {
    luaA_to(L, dt_lua_tag_t, &tagid, 1);
    luaA_to(L, dt_lua_image_t, &imgid, 2);
  }
  if(dt_tag_detach(tagid, imgid, TRUE, TRUE))
  {
    dt_image_synch_xmp(imgid);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  }
  return 0;
}

static int tag_lib_find(lua_State *L)
{
  const char *name = luaL_checkstring(L, 1);
  dt_lua_tag_t tagid;
  if(!dt_tag_exists(name, &tagid))
  {
    lua_pushnil(L);
    return 1;
  }
  luaA_push(L, dt_lua_tag_t, &tagid);
  return 1;
}

int dt_lua_tag_get_attached(lua_State *L)
{
  dt_lua_image_t imgid;
  int table_index = 1;
  luaA_to(L, dt_lua_image_t, &imgid, 1);
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT tagid FROM main.tagged_images WHERE imgid=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  int rv = sqlite3_step(stmt);
  lua_newtable(L);
  while(rv == SQLITE_ROW)
  {
    int tagid = sqlite3_column_int(stmt, 0);
    luaA_push(L, dt_lua_tag_t, &tagid);
    lua_seti(L, -2, table_index);
    table_index++;
    rv = sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  return 1;
}


int dt_lua_tag_get_tagged_images(lua_State *L)
{
  dt_lua_tag_t tagid;
  int table_index = 1;
  luaA_to(L, dt_lua_tag_t, &tagid, 1);
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.tagged_images WHERE tagid=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  int rv = sqlite3_step(stmt);
  lua_newtable(L);
  while(rv == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    luaA_push(L, dt_lua_image_t, &imgid);
    lua_seti(L, -2, table_index);
    table_index++;
    rv = sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  return 1;
}


int dt_lua_init_tags(lua_State *L)
{
  dt_lua_init_int_type(L, dt_lua_tag_t);
  lua_pushcfunction(L, tag_length);
  lua_pushcfunction(L, tag_index);
  dt_lua_type_register_number_const(L, dt_lua_tag_t);
  lua_pushcfunction(L, tag_name);
  dt_lua_type_register_const(L, dt_lua_tag_t, "name");
  lua_pushcfunction(L, tag_flags);
  dt_lua_type_register_const(L, dt_lua_tag_t, "flags");
  lua_pushcfunction(L, tag_synonyms);
  dt_lua_type_register_const(L, dt_lua_tag_t, "synonyms");
  lua_pushcfunction(L, tag_delete);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_tag_t, "delete");
  lua_pushcfunction(L, dt_lua_tag_attach);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_tag_t, "attach");
  lua_pushcfunction(L, dt_lua_tag_detach);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_tag_t, "detach");
  lua_pushcfunction(L, tag_tostring);
  dt_lua_type_setmetafield(L, dt_lua_tag_t, "__tostring");

  /* tags */
  dt_lua_push_darktable_lib(L);
  luaA_Type type_id = dt_lua_init_singleton(L, "tag_table", NULL);
  lua_setfield(L, -2, "tags");
  lua_pop(L, 1);

  lua_pushcfunction(L, tag_lib_length);
  lua_pushcfunction(L, tag_lib_index);
  dt_lua_type_register_number_const_type(L, type_id);
  lua_pushcfunction(L, tag_lib_create);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "create");
  lua_pushcfunction(L, tag_lib_find);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "find");
  lua_pushcfunction(L, tag_delete);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "delete");
  lua_pushcfunction(L, dt_lua_tag_attach);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "attach");
  lua_pushcfunction(L, dt_lua_tag_detach);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "detach");
  lua_pushcfunction(L, dt_lua_tag_get_attached);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "get_tags");
  lua_pushcfunction(L, dt_lua_tag_get_tagged_images);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "get_tagged_images");


  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
