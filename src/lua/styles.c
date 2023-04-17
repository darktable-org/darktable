/*
   This file is part of darktable,
   Copyright (C) 2013-2020 darktable developers.

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

#include "lua/styles.h"
#include "common/debug.h"
#include "common/styles.h"
#include "lua/glist.h"
#include "lua/image.h"
#include "lua/types.h"


// can't use glist functions we need a list of int and glist can only produce a list of int*
static GList *style_item_table_to_id_list(lua_State *L, int index);

/////////////////////////
// dt_style_t
/////////////////////////
static int style_gc(lua_State *L)
{
  dt_style_t style;
  luaA_to(L, dt_style_t, &style, -1);
  g_free(style.name);
  g_free(style.description);
  return 0;
}

static int style_tostring(lua_State *L)
{
  dt_style_t style;
  luaA_to(L, dt_style_t, &style, 1);
  lua_pushstring(L, style.name);
  return 1;
}


static int style_delete(lua_State *L)
{
  dt_style_t style;
  luaA_to(L, dt_style_t, &style, 1);
  dt_styles_delete_by_name(style.name);
  return 0;
}


static int style_duplicate(lua_State *L)
{
  dt_style_t style;
  luaA_to(L, dt_style_t, &style, 1);
  const char *newname = luaL_checkstring(L, 2);
  const char *description = lua_isnoneornil(L, 3) ? style.description : luaL_checkstring(L, 3);
  GList *filter = style_item_table_to_id_list(L, 4);
  dt_styles_create_from_style(style.name, newname, description, filter, -1, NULL, TRUE, FALSE);
  g_list_free(filter);
  return 0;
}

static int style_getnumber(lua_State *L)
{
  const int index = luaL_checknumber(L, -1);
  if(index <= 0)
  {
    return luaL_error(L, "incorrect index for style");
  }
  dt_style_t style;
  luaA_to(L, dt_style_t, &style, -2);
  GList *items = dt_styles_get_item_list(style.name, FALSE, -1, TRUE);
  dt_style_item_t *item = g_list_nth_data(items, index - 1);
  if(!item)
  {
    return luaL_error(L, "incorrect index for style");
  }
  items = g_list_remove(items, item);
  g_list_free_full(items, dt_style_item_free);
  luaA_push(L, dt_style_item_t, item);
  free(item);
  return 1;
}


static int style_length(lua_State *L)
{

  dt_style_t style;
  luaA_to(L, dt_style_t, &style, -1);
  GList *items = dt_styles_get_item_list(style.name, FALSE, -1, TRUE);
  lua_pushinteger(L, g_list_length(items));
  g_list_free_full(items, dt_style_item_free);
  return 1;
}


static int name_member(lua_State *L)
{
  dt_style_t style;
  luaA_to(L, dt_style_t, &style, 1);
  if(lua_gettop(L) != 3)
  {
    lua_pushstring(L, style.name);
    return 1;
  }
  else
  {
    const char *newval;
    newval = luaL_checkstring(L, 3);
    dt_styles_update(style.name, newval, style.description, NULL, -1, NULL, FALSE, FALSE);
    return 0;
  }
}

static int description_member(lua_State *L)
{
  dt_style_t style;
  luaA_to(L, dt_style_t, &style, 1);
  if(lua_gettop(L) != 3)
  {
    lua_pushstring(L, style.description);
    return 1;
  }
  else
  {
    const char *newval;
    newval = luaL_checkstring(L, -1);
    dt_styles_update(style.name, style.name, newval, NULL, -1, NULL, FALSE, FALSE);
    return 0;
  }
}


/////////////////////////
// dt_style_item_t
/////////////////////////

static int style_item_tostring(lua_State *L)
{
  dt_style_item_t *item = luaL_checkudata(L, -1, "dt_style_item_t");
  lua_pushfstring(L, "%d : %s", item->num, item->name);
  return 1;
}

static int style_item_gc(lua_State *L)
{
  // FIXME: Can't we use dt_style_item_free() instead? Or may the pointer itself not be freed?
  dt_style_item_t *item = luaL_checkudata(L, -1, "dt_style_item_t");
  g_free(item->name);
  g_free(item->operation);
  free(item->params);
  free(item->blendop_params);
  return 0;
}

static GList *style_item_table_to_id_list(lua_State *L, const int index)
{
  if(lua_isnoneornil(L, index)) return NULL;
  luaL_checktype(L, index, LUA_TTABLE);
  lua_pushnil(L); /* first key */
  GList *result = NULL;
  while(lua_next(L, index) != 0)
  {
    /* uses 'key' (at index -2) and 'value' (at index -1) */
    dt_style_item_t *item = luaL_checkudata(L, -1, "dt_style_item_t");
    result = g_list_prepend(result, GINT_TO_POINTER(item->num));
    lua_pop(L, 1);
  }
  result = g_list_reverse(result);
  return result;
}

/////////////////////////
// toplevel and common
/////////////////////////
static int style_table_index(lua_State *L)
{
  const int index = luaL_checkinteger(L, -1);
  if(index < 1)
  {
    return luaL_error(L, "incorrect index in database");
  }
  sqlite3_stmt *stmt = NULL;
  char query[1024];
  snprintf(query, sizeof(query), "SELECT name FROM data.styles ORDER BY name LIMIT 1 OFFSET %d", index - 1);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    dt_style_t *style = dt_styles_get_by_name(name);
    luaA_push(L, dt_style_t, style);
    free(style);
  }
  else
  {
    lua_pushnil(L);
  }
  sqlite3_finalize(stmt);
  return 1;
}

static int style_table_len(lua_State *L)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT COUNT(*) FROM data.styles", -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    lua_pushinteger(L, sqlite3_column_int(stmt, 0));
  else
  {
    lua_pushinteger(L, 0);
  }
  sqlite3_finalize(stmt);
  return 1;
}

int dt_lua_style_create_from_image(lua_State *L)
{
  dt_lua_image_t imgid;
  luaA_to(L, dt_lua_image_t, &imgid, -3);
  const char *newname = luaL_checkstring(L, -2);
  const char *description = lua_isnoneornil(L, -1) ? "" : luaL_checkstring(L, -1);
  dt_styles_create_from_image(newname, description, imgid, NULL, TRUE);
  GList *style_list = dt_styles_get_list(newname);
  while(style_list)
  {
    dt_style_t *data = style_list->data;
    if(!strcmp(data->name, newname))
    {
      luaA_push(L, dt_style_t, data);
      g_free(data);
      style_list = g_list_delete_link(style_list, style_list);
    }
  }
  g_list_free_full(style_list, dt_style_free); // deal with what's left
  return 1;
}

int dt_lua_style_apply(lua_State *L)
{
  dt_lua_image_t imgid = NO_IMGID;
  dt_style_t style;
  if(luaL_testudata(L, 1, "dt_lua_image_t"))
  {
    luaA_to(L, dt_lua_image_t, &imgid, 1);
    luaA_to(L, dt_style_t, &style, 2);
  }
  else
  {
    luaA_to(L, dt_style_t, &style, 1);
    luaA_to(L, dt_lua_image_t, &imgid, 2);
  }
  dt_styles_apply_to_image(style.name, FALSE, FALSE, imgid);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  return 1;
}

int dt_lua_style_import(lua_State *L)
{
  const char *filename = luaL_checkstring(L, 1);
  dt_styles_import_from_file(filename);
  return 0;
}

int dt_lua_style_export(lua_State *L)
{
  dt_style_t style;
  luaA_to(L, dt_style_t, &style, 1);
  const char *filename = lua_tostring(L, 2);
  if(!filename) filename = ".";
  gboolean overwrite = lua_toboolean(L, 3);
  dt_styles_save_to_file(style.name, filename, overwrite);
  return 0;
}



int dt_lua_init_styles(lua_State *L)
{
  // dt_style
  dt_lua_init_type(L, dt_style_t);
  lua_pushcfunction(L, name_member);
  dt_lua_type_register_const(L, dt_style_t, "name");
  lua_pushcfunction(L, description_member);
  dt_lua_type_register_const(L, dt_style_t, "description");
  lua_pushcfunction(L, style_length);
  lua_pushcfunction(L, style_getnumber);
  dt_lua_type_register_number_const(L, dt_style_t);
  lua_pushcfunction(L, style_duplicate);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_style_t, "duplicate");
  lua_pushcfunction(L, style_delete);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_style_t, "delete");
  lua_pushcfunction(L, dt_lua_style_apply);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_style_t, "apply");
  lua_pushcfunction(L, dt_lua_style_export);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_style_t, "export");
  lua_pushcfunction(L, style_gc);
  dt_lua_type_setmetafield(L,dt_style_t,"__gc");
  lua_pushcfunction(L, style_tostring);
  dt_lua_type_setmetafield(L,dt_style_t,"__tostring");

  // dt_style_item_t
  dt_lua_init_type(L, dt_style_item_t);
  luaA_struct(L, dt_style_item_t);
  luaA_struct_member(L, dt_style_item_t, num, const int);
  luaA_struct_member(L, dt_style_item_t, name, const_string);
  lua_pushcfunction(L, dt_lua_type_member_luaautoc);
  dt_lua_type_register_struct(L, dt_style_item_t);
  lua_pushcfunction(L, style_item_gc);
  dt_lua_type_setmetafield(L,dt_style_item_t,"__gc");
  lua_pushcfunction(L, style_item_tostring);
  dt_lua_type_setmetafield(L,dt_style_item_t,"__tostring");



  /* style table type */
  dt_lua_push_darktable_lib(L);
  luaA_Type type_id = dt_lua_init_singleton(L, "style_table", NULL);
  lua_setfield(L, -2, "styles");
  lua_pop(L, 1);

  lua_pushcfunction(L, style_table_len);
  lua_pushcfunction(L, style_table_index);
  dt_lua_type_register_number_const_type(L, type_id);
  lua_pushcfunction(L, style_duplicate);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "duplicate");
  lua_pushcfunction(L, style_delete);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "delete");
  lua_pushcfunction(L, dt_lua_style_create_from_image);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "create");
  lua_pushcfunction(L, dt_lua_style_apply);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "apply");
  lua_pushcfunction(L, dt_lua_style_import);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "import");
  lua_pushcfunction(L, dt_lua_style_export);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "export");

  return 0;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
