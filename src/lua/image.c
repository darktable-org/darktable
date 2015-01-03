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

#include "lua/image.h"
#include "lua/types.h"
#include "lua/glist.h"
#include "lua/tags.h"
#include "lua/database.h"
#include "lua/styles.h"
#include "lua/film.h"
#include "common/colorlabels.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/metadata.h"
#include "common/grouping.h"
#include "common/history.h"
#include "common/mipmap_cache.h"
#include "metadata_gen.h"

/***********************************************************************
  handling of dt_image_t
 **********************************************************************/

static const dt_image_t *checkreadimage(lua_State *L, int index)
{
  dt_lua_image_t imgid;
  luaA_to(L, dt_lua_image_t, &imgid, index);
  return dt_image_cache_get(darktable.image_cache, imgid, 'r');
}

static void releasereadimage(lua_State *L, const dt_image_t *image)
{
  dt_image_cache_read_release(darktable.image_cache, image);
}

static dt_image_t *checkwriteimage(lua_State *L, int index)
{
  return dt_image_cache_get(darktable.image_cache, index, 'w');
}

static void releasewriteimage(lua_State *L, dt_image_t *image)
{
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
}

void dt_lua_image_push(lua_State *L, int imgid)
{
  // check that id is valid
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where id = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    luaL_error(L, "invalid id for image : %d", imgid);
    return;
  }
  sqlite3_finalize(stmt);
  luaA_push(L, dt_lua_image_t, &imgid);
}


static int history_delete(lua_State *L)
{
  dt_lua_image_t imgid = -1;
  luaA_to(L, dt_lua_image_t, &imgid, -1);
  dt_history_delete_on_image(imgid);
  return 0;
}


static int drop_cache(lua_State *L)
{
  dt_lua_image_t imgid = -1;
  luaA_to(L, dt_lua_image_t, &imgid, -1);
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
  return 0;
}



static int path_member(lua_State *L)
{
  const dt_image_t *my_image = checkreadimage(L, 1);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select folder from images, film_rolls where "
                              "images.film_id = film_rolls.id and images.id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    lua_pushstring(L, (char *)sqlite3_column_text(stmt, 0));
  }
  else
  {
    sqlite3_finalize(stmt);
    releasereadimage(L, my_image);
    return luaL_error(L, "should never happen");
  }
  sqlite3_finalize(stmt);
  releasereadimage(L, my_image);
  return 1;
}

static int duplicate_index_member(lua_State *L)
{
  const dt_image_t *my_image = checkreadimage(L, 1);
  // get duplicate suffix
  int version = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select count(id) from images where filename in "
                              "(select filename from images where id = ?1) and film_id in "
                              "(select film_id from images where id = ?1) and id < ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
  if(sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  lua_pushinteger(L, version);
  releasereadimage(L, my_image);
  return 1;
}

static int is_ldr_member(lua_State *L)
{
  const dt_image_t *my_image = checkreadimage(L, 1);
  lua_pushboolean(L, dt_image_is_ldr(my_image));
  releasereadimage(L, my_image);
  return 1;
}

static int is_hdr_member(lua_State *L)
{
  const dt_image_t *my_image = checkreadimage(L, 1);
  lua_pushboolean(L, dt_image_is_hdr(my_image));
  releasereadimage(L, my_image);
  return 1;
}

static int is_raw_member(lua_State *L)
{
  const dt_image_t *my_image = checkreadimage(L, 1);
  lua_pushboolean(L, dt_image_is_raw(my_image));
  releasereadimage(L, my_image);
  return 1;
}

static int id_member(lua_State *L)
{
  const dt_image_t *my_image = checkreadimage(L, 1);
  lua_pushinteger(L, my_image->id);
  releasereadimage(L, my_image);
  return 1;
}

static int film_member(lua_State *L)
{
  const dt_image_t *my_image = checkreadimage(L, 1);
  luaA_push(L, dt_lua_film_t, &my_image->film_id);
  releasereadimage(L, my_image);
  return 1;
}

static int group_leader_member(lua_State *L)
{
  const dt_image_t *my_image = checkreadimage(L, 1);
  luaA_push(L, dt_lua_image_t, &(my_image->group_id));
  releasereadimage(L, my_image);
  return 1;
}


static int rating_member(lua_State *L)
{
  if(lua_gettop(L) != 3)
  {
    const dt_image_t *my_image = checkreadimage(L, 1);
    int score = my_image->flags & 0x7;
    if(score > 6) score = 5;
    if(score == 6) score = -1;

    lua_pushinteger(L, score);
    releasereadimage(L, my_image);
    return 1;
  }
  else
  {
    dt_image_t *my_image = checkwriteimage(L, 1);
    int my_score = luaL_checkinteger(L, 3);
    if(my_score > 5)
    {
      releasewriteimage(L, my_image);
      return luaL_error(L, "rating too high : %d", my_score);
    }
    if(my_score == -1) my_score = 6;
    if(my_score < -1)
    {
      releasewriteimage(L, my_image);
      return luaL_error(L, "rating too low : %d", my_score);
    }
    my_image->flags &= ~0x7;
    my_image->flags |= my_score;
    releasewriteimage(L, my_image);
    return 0;
  }
}

static int has_txt_member(lua_State *L)
{
  if(lua_gettop(L) != 3)
  {
    const dt_image_t *my_image = checkreadimage(L, 1);
    int has_txt = my_image->flags & DT_IMAGE_HAS_TXT;
    lua_pushboolean(L, has_txt);
    releasereadimage(L, my_image);
    return 1;
  }
  else
  {
    dt_image_t *my_image = checkwriteimage(L, 1);
    if(lua_toboolean(L, 3))
      my_image->flags |= DT_IMAGE_HAS_TXT;
    else
      my_image->flags &= ~DT_IMAGE_HAS_TXT;
    releasewriteimage(L, my_image);
    return 0;
  }
}

static int creator_member(lua_State *L)
{
  if(lua_gettop(L) != 3)
  {
    const dt_image_t *my_image = checkreadimage(L, 1);
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_CREATOR);
    if(sqlite3_step(stmt) != SQLITE_ROW)
    {
      lua_pushstring(L, "");
    }
    else
    {
      lua_pushstring(L, (char *)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    releasereadimage(L, my_image);
    return 1;
  }
  else
  {
    dt_image_t *my_image = checkwriteimage(L, 1);
    dt_metadata_set(my_image->id, "Xmp.dc.creator", luaL_checkstring(L, 3));
    dt_image_synch_xmp(my_image->id);
    releasewriteimage(L, my_image);
    return 0;
  }
}

static int publisher_member(lua_State *L)
{
  if(lua_gettop(L) != 3)
  {
    const dt_image_t *my_image = checkreadimage(L, 1);
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_PUBLISHER);
    if(sqlite3_step(stmt) != SQLITE_ROW)
    {
      lua_pushstring(L, "");
    }
    else
    {
      lua_pushstring(L, (char *)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    releasereadimage(L, my_image);
    return 1;
  }
  else
  {
    dt_image_t *my_image = checkwriteimage(L, 1);
    dt_metadata_set(my_image->id, "Xmp.dc.publisher", luaL_checkstring(L, 3));
    dt_image_synch_xmp(my_image->id);
    releasewriteimage(L, my_image);
    return 0;
  }
}

static int title_member(lua_State *L)
{
  if(lua_gettop(L) != 3)
  {
    const dt_image_t *my_image = checkreadimage(L, 1);
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_TITLE);
    if(sqlite3_step(stmt) != SQLITE_ROW)
    {
      lua_pushstring(L, "");
    }
    else
    {
      lua_pushstring(L, (char *)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    releasereadimage(L, my_image);
    return 1;
  }
  else
  {
    dt_image_t *my_image = checkwriteimage(L, 1);
    dt_metadata_set(my_image->id, "Xmp.dc.title", luaL_checkstring(L, 3));
    dt_image_synch_xmp(my_image->id);
    releasewriteimage(L, my_image);
    return 0;
  }
}

static int description_member(lua_State *L)
{
  if(lua_gettop(L) != 3)
  {
    const dt_image_t *my_image = checkreadimage(L, 1);
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_DESCRIPTION);
    if(sqlite3_step(stmt) != SQLITE_ROW)
    {
      lua_pushstring(L, "");
    }
    else
    {
      lua_pushstring(L, (char *)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    releasereadimage(L, my_image);
    return 1;
  }
  else
  {
    dt_image_t *my_image = checkwriteimage(L, 1);
    dt_metadata_set(my_image->id, "Xmp.dc.description", luaL_checkstring(L, 3));
    dt_image_synch_xmp(my_image->id);
    releasewriteimage(L, my_image);
    return 0;
  }
}

static int rights_member(lua_State *L)
{
  if(lua_gettop(L) != 3)
  {
    const dt_image_t *my_image = checkreadimage(L, 1);
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_RIGHTS);
    if(sqlite3_step(stmt) != SQLITE_ROW)
    {
      lua_pushstring(L, "");
    }
    else
    {
      lua_pushstring(L, (char *)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    releasereadimage(L, my_image);
    return 1;
  }
  else
  {
    dt_image_t *my_image = checkwriteimage(L, 1);
    dt_metadata_set(my_image->id, "Xmp.dc.title", luaL_checkstring(L, 3));
    dt_image_synch_xmp(my_image->id);
    releasewriteimage(L, my_image);
    return 0;
  }
}

static int local_copy_member(lua_State *L)
{
  if(lua_gettop(L) != 3)
  {
    const dt_image_t *my_image = checkreadimage(L, 1);
    lua_pushboolean(L, my_image->flags & DT_IMAGE_LOCAL_COPY);
    releasereadimage(L, my_image);
    return 1;
  }
  else
  {
    dt_image_t *my_image = checkwriteimage(L, 1);
    int imgid = my_image->id;
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    // we need to release write image for the other functions to use it
    releasewriteimage(L, my_image);
    if(lua_toboolean(L, 3))
    {
      dt_image_local_copy_set(imgid);
    }
    else
    {
      dt_image_local_copy_reset(imgid);
    }
    return 0;
  }
}

static int colorlabel_member(lua_State *L)
{
  int imgid;
  luaA_to(L, dt_lua_image_t, &imgid, 1);
  int colorlabel_index = luaL_checkoption(L, 2, NULL, dt_colorlabels_name);
  if(lua_gettop(L) != 3)
  {
    lua_pushboolean(L, dt_colorlabels_check_label(imgid, colorlabel_index));
    return 1;
  }
  else
  {
    if(lua_toboolean(L, 3)) // no testing of type so we can benefit from all types of values
    {
      dt_colorlabels_set_label(imgid, colorlabel_index);
    }
    else
    {
      dt_colorlabels_remove_label(imgid, colorlabel_index);
    }
    return 0;
  }
}


static int image_tostring(lua_State *L)
{
  const dt_image_t *my_image = checkreadimage(L, -1);
  char image_name[PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  dt_image_full_path(my_image->id, image_name, sizeof(image_name), &from_cache);
  dt_image_path_append_version(my_image->id, image_name, sizeof(image_name));
  lua_pushstring(L, image_name);
  releasereadimage(L, my_image);
  return 1;
}


int group_with(lua_State *L)
{
  dt_lua_image_t first_image;
  luaA_to(L, dt_lua_image_t, &first_image, 1);
  if(lua_isnoneornil(L, 2))
  {
    dt_grouping_remove_from_group(first_image);
    return 0;
  }
  dt_lua_image_t second_image;
  luaA_to(L, dt_lua_image_t, &second_image, 2);

  const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, second_image, 'r');
  int group_id = cimg->group_id;
  dt_image_cache_read_release(darktable.image_cache, cimg);

  dt_grouping_add_to_group(group_id, first_image);
  return 0;
}

int make_group_leader(lua_State *L)
{
  dt_lua_image_t first_image;
  luaA_to(L, dt_lua_image_t, &first_image, 1);
  dt_grouping_change_representative(first_image);
  return 0;
}


int get_group(lua_State *L)
{
  dt_lua_image_t first_image;
  luaA_to(L, dt_lua_image_t, &first_image, 1);
  const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, first_image, 'r');
  int group_id = cimg->group_id;
  dt_image_cache_read_release(darktable.image_cache, cimg);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where group_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, group_id);
  lua_newtable(L);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    luaA_push(L, dt_lua_image_t, &imgid);
    luaL_ref(L, -2);
  }
  luaA_push(L, dt_lua_image_t, &group_id);
  lua_setfield(L, -2, "leader");
  return 1;
}

///////////////
// toplevel and common
///////////////
static int image_luaautoc_member(lua_State *L)
{
  dt_lua_image_t imgid;
  luaA_to(L, dt_lua_image_t, &imgid, 1);
  const char *member_name = luaL_checkstring(L, 2);
  if(lua_gettop(L) != 3)
  {
    const dt_image_t *image = checkreadimage(L, 1);
    luaA_struct_push_member_name(L, dt_image_t, member_name, image);
    releasereadimage(L, image);
    return 1;
  }
  else
  {
    dt_image_t *image = checkwriteimage(L, 1);
    luaA_struct_to_member_name(L, dt_image_t, member_name, image, 3);
    releasewriteimage(L, image);
    return 0;
  }
}

int dt_lua_init_image(lua_State *L)
{
  luaA_struct(L, dt_image_t);
  luaA_struct_member(L, dt_image_t, exif_exposure, float);
  luaA_struct_member(L, dt_image_t, exif_aperture, float);
  luaA_struct_member(L, dt_image_t, exif_iso, float);
  luaA_struct_member(L, dt_image_t, exif_focal_length, float);
  luaA_struct_member(L, dt_image_t, exif_focus_distance, float);
  luaA_struct_member(L, dt_image_t, exif_crop, float);
  luaA_struct_member(L, dt_image_t, exif_maker, char_64);
  luaA_struct_member(L, dt_image_t, exif_model, char_64);
  luaA_struct_member(L, dt_image_t, exif_lens, char_128);
  luaA_struct_member(L, dt_image_t, exif_datetime_taken, char_20);
  luaA_struct_member(L, dt_image_t, filename, const char_filename_length);
  luaA_struct_member(L, dt_image_t, width, const int32_t);
  luaA_struct_member(L, dt_image_t, height, const int32_t);
  luaA_struct_member(L, dt_image_t, longitude, protected_double); // set to NAN if value is not set
  luaA_struct_member(L, dt_image_t, latitude, protected_double); // set to NAN if value is not set

  dt_lua_init_int_type(L, dt_lua_image_t);

  const char *member_name = luaA_struct_next_member_name(L, dt_image_t, LUAA_INVALID_MEMBER_NAME);
  while(member_name != LUAA_INVALID_MEMBER_NAME)
  {
    lua_pushcfunction(L, image_luaautoc_member);
    luaA_Type member_type = luaA_struct_typeof_member_name(L, dt_image_t, member_name);
    if(luaA_conversion_to_registered_type(L, member_type) || luaA_struct_registered_type(L, member_type)
       || luaA_enum_registered_type(L, member_type))
    {
      dt_lua_type_register(L, dt_lua_image_t, member_name);
    }
    else
    {
      dt_lua_type_register_const(L, dt_lua_image_t, member_name);
    }
    member_name = luaA_struct_next_member_name(L, dt_image_t, member_name);
  }

  // read only members
  lua_pushcfunction(L, path_member);
  dt_lua_type_register_const(L, dt_lua_image_t, "path");
  lua_pushcfunction(L, duplicate_index_member);
  dt_lua_type_register_const(L, dt_lua_image_t, "duplicate_index");
  lua_pushcfunction(L, is_ldr_member);
  dt_lua_type_register_const(L, dt_lua_image_t, "is_ldr");
  lua_pushcfunction(L, is_hdr_member);
  dt_lua_type_register_const(L, dt_lua_image_t, "is_hdr");
  lua_pushcfunction(L, is_raw_member);
  dt_lua_type_register_const(L, dt_lua_image_t, "is_raw");
  lua_pushcfunction(L, id_member);
  dt_lua_type_register_const(L, dt_lua_image_t, "id");
  lua_pushcfunction(L, film_member);
  dt_lua_type_register_const(L, dt_lua_image_t, "film");
  lua_pushcfunction(L, group_leader_member);
  dt_lua_type_register_const(L, dt_lua_image_t, "group_leader");
  // read/write functions
  lua_pushcfunction(L, has_txt_member);
  dt_lua_type_register(L, dt_lua_image_t, "has_txt");
  lua_pushcfunction(L, rating_member);
  dt_lua_type_register(L, dt_lua_image_t, "rating");
  lua_pushcfunction(L, creator_member);
  dt_lua_type_register(L, dt_lua_image_t, "creator");
  lua_pushcfunction(L, publisher_member);
  dt_lua_type_register(L, dt_lua_image_t, "publisher");
  lua_pushcfunction(L, title_member);
  dt_lua_type_register(L, dt_lua_image_t, "title");
  lua_pushcfunction(L, description_member);
  dt_lua_type_register(L, dt_lua_image_t, "description");
  lua_pushcfunction(L, rights_member);
  dt_lua_type_register(L, dt_lua_image_t, "rights");
  lua_pushcfunction(L, local_copy_member);
  dt_lua_type_register(L, dt_lua_image_t, "local_copy");
  const char **name = dt_colorlabels_name;
  while(*name)
  {
    lua_pushcfunction(L, colorlabel_member);
    dt_lua_type_register(L, dt_lua_image_t, *name);
    name++;
  }
  // constant functions (i.e class methods)
  lua_pushcfunction(L, dt_lua_duplicate_image);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "duplicate");
  lua_pushcfunction(L, dt_lua_delete_image);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "delete");
  lua_pushcfunction(L, group_with);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "group_with");
  lua_pushcfunction(L, make_group_leader);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "make_group_leader");
  lua_pushcfunction(L, get_group);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "get_group_members");
  lua_pushcfunction(L, dt_lua_tag_attach);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "attach_tag");
  lua_pushcfunction(L, dt_lua_tag_detach);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "detach_tag");
  lua_pushcfunction(L, dt_lua_tag_get_attached);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "get_tags");
  lua_pushcfunction(L, dt_lua_style_apply);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "apply_style");
  lua_pushcfunction(L, dt_lua_style_create_from_image);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "create_style");
  lua_pushcfunction(L, history_delete);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "reset");
  lua_pushcfunction(L, dt_lua_move_image);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "move");
  lua_pushcfunction(L, dt_lua_copy_image);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "copy");
  lua_pushcfunction(L, drop_cache);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "drop_cache");
  lua_pushcfunction(L, image_tostring);
  dt_lua_type_setmetafield(L,dt_lua_image_t,"__tostring");

  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
