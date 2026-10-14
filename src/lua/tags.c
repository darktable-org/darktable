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
  lua_pushinteger(L, dt_tag_images_count(tagid));
  return 1;
}
static int tag_index(lua_State *L)
{
  dt_lua_tag_t tagid;
  luaA_to(L, dt_lua_tag_t, &tagid, -2);
  int index = luaL_checkinteger(L, -1);
  if(index < 1)
  {
    lua_pushnil(L);
    return 1;
  }
  dt_imgid_t imgid = dt_tag_get_nth_image(tagid, index - 1);
  if(dt_is_valid_imgid(imgid))
  {
    luaA_push(L, dt_lua_image_t, &imgid);
  }
  else
  {
    lua_pushnil(L);
  }
  return 1;
}


static int tag_lib_length(lua_State *L)
{
  lua_pushinteger(L, dt_tag_count_tags());
  return 1;
}
static int tag_lib_index(lua_State *L)
{
  int index = luaL_checkinteger(L, -1);
  int tagid = dt_tag_get_nth_tag(index - 1);
  if(tagid != -1)
  {
    luaA_push(L, dt_lua_tag_t, &tagid);
  }
  else
  {
    lua_pushnil(L);
  }
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

  GList *tagged_images = dt_tag_get_images(tagid);

  if(dt_tag_remove(tagid, TRUE))
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);

  for(const GList *list_iter = tagged_images; list_iter; list_iter = g_list_next(list_iter))
  {
    dt_image_synch_xmp(GPOINTER_TO_INT(list_iter->data));
  }
  g_list_free(tagged_images);

  return 0;
}


int dt_lua_tag_attach(lua_State *L)
{
  dt_lua_image_t imgid = NO_IMGID;
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
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
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
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
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

  GList *tags = dt_tag_get_tag_ids(imgid);
  lua_newtable(L);
  for(GList *iter = tags; iter; iter = g_list_next(iter))
  {
    int tagid = GPOINTER_TO_INT(iter->data);
    luaA_push(L, dt_lua_tag_t, &tagid);
    lua_seti(L, -2, table_index);
    table_index++;
  }
  g_list_free(tags);
  return 1;
}


int dt_lua_tag_get_tagged_images(lua_State *L)
{
  dt_lua_tag_t tagid;
  int table_index = 1;
  luaA_to(L, dt_lua_tag_t, &tagid, 1);

  GList *images = dt_tag_get_images(tagid);
  lua_newtable(L);
  for(GList *iter = images; iter; iter = g_list_next(iter))
  {
    dt_imgid_t imgid = GPOINTER_TO_INT(iter->data);
    luaA_push(L, dt_lua_image_t, &imgid);
    lua_seti(L, -2, table_index);
    table_index++;
  }
  g_list_free(images);
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

