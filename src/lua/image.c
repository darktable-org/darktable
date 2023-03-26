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

#include "lua/image.h"
#include "common/colorlabels.h"
#include "common/debug.h"
#include "common/grouping.h"
#include "common/mipmap_cache.h" // for dt_mipmap_size_t, etc
#include "common/file_location.h"
#include "common/history.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/collection.h"
#include "common/metadata.h"
#include "common/ratings.h"
#include "common/datetime.h"
#include "views/view.h"
#include "lua/database.h"
#include "lua/film.h"
#include "lua/glist.h"
#include "lua/styles.h"
#include "lua/tags.h"
#include "lua/types.h"

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
  dt_lua_image_t imgid;
  luaA_to(L, dt_lua_image_t, &imgid, index);
  return dt_image_cache_get(darktable.image_cache, imgid, 'w');
}

static void releasewriteimage(lua_State *L, dt_image_t *image)
{
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
}

void dt_lua_image_push(lua_State *L, int imgid)
{
  // check that id is valid
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM main.images WHERE id = ?1", -1, &stmt,
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
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  return 0;
}

static int drop_cache(lua_State *L)
{
  dt_lua_image_t imgid = -1;
  luaA_to(L, dt_lua_image_t, &imgid, -1);
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
  return 0;
}

static int generate_cache(lua_State *L)
{
  dt_lua_image_t imgid = 1;
  luaA_to(L, dt_lua_image_t, &imgid, 1);
  const gboolean create_dirs = lua_toboolean(L, 2);
  const int min = luaL_checkinteger(L, 3);
  const int max = luaL_checkinteger(L, 4);

  if(create_dirs)
  {
    for(dt_mipmap_size_t k = min; k <= max; k++)
    {
      char dirname[PATH_MAX] = { 0 };
      snprintf(dirname, sizeof(dirname), "%s.d/%d", darktable.mipmap_cache->cachedir, k);

      if(!dt_util_test_writable_dir(dirname))
      {
        if(g_mkdir_with_parents(dirname, 0750))
        {
          dt_print(DT_DEBUG_ALWAYS, "[lua] could not create directory '%s'!\n", dirname);
          return 1;
        }
      }
    }
  }

  for(int k = max; k >= min && k >= 0; k--)
  {
    char filename[PATH_MAX] = { 0 };
    snprintf(filename, sizeof(filename),
             "%s.d/%d/%d.jpg", darktable.mipmap_cache->cachedir, k, imgid);

    // if a valid thumbnail file is already on disc - do nothing
    if(dt_util_test_image_file(filename)) continue;
    // else, generate thumbnail and store in mipmap cache.
    dt_mipmap_buffer_t buf;
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, k, DT_MIPMAP_BLOCKING, 'r');
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  }
  // thumbnail in sync with image
  dt_history_hash_set_mipmap(imgid);

  return 0;
}


static int path_member(lua_State *L)
{
  const dt_image_t *my_image = checkreadimage(L, 1);
  char pathname[PATH_MAX] = { 0 };
  dt_image_film_roll_directory(my_image, pathname, sizeof(pathname));
  lua_pushstring(L, pathname);
  releasereadimage(L, my_image);
  return 1;
}

static int sidecar_member(lua_State *L)
{
  const dt_image_t *my_image = checkreadimage(L, 1);
  gboolean from_cache = TRUE;
  char filename[PATH_MAX] = { 0 };
  dt_image_full_path(my_image->id, filename, sizeof(filename), &from_cache);
  dt_image_path_append_version(my_image->id, filename, sizeof(filename));
  g_strlcat(filename, ".xmp", sizeof(filename));
  lua_pushstring(L, filename);
  releasereadimage(L, my_image);
  return 1;
}

static int duplicate_index_member(lua_State *L)
{
  const dt_image_t *my_image = checkreadimage(L, 1);
  lua_pushinteger(L, my_image->version);
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
    int score = my_image->flags & DT_VIEW_RATINGS_MASK;
    if(score > 6) score = 5;
    if(score == DT_VIEW_REJECT) score = -1;
    // check the reject flag just to be sure
    if(my_image->flags & DT_IMAGE_REJECTED) score = -1;

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
    if(my_score < -1)
    {
      releasewriteimage(L, my_image);
      return luaL_error(L, "rating too low : %d", my_score);
    }
    if(my_score == -1)
    {
      my_score = DT_VIEW_REJECT;
      my_image->flags = my_image->flags | DT_IMAGE_REJECTED;
    }
    if(my_score < DT_VIEW_REJECT && my_image->flags & DT_IMAGE_REJECTED)
      my_image->flags = my_image->flags & ~DT_IMAGE_REJECTED;
    my_image->flags &= ~DT_VIEW_RATINGS_MASK;
    my_image->flags |= my_score;
    releasewriteimage(L, my_image);
    dt_collection_update_query(darktable.collection,
                               DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_RATING,
                               g_list_prepend(NULL, GINT_TO_POINTER(my_image->id)));
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

static int metadata_member(lua_State *L)
{
  const char *member_name = luaL_checkstring(L, 2);
  const char *key= dt_metadata_get_key_by_subkey(member_name);
  if(lua_gettop(L) != 3)
  {
    const dt_image_t *my_image = checkreadimage(L, 1);
    GList *res = dt_metadata_get(my_image->id, key, NULL);
    if(res)
      lua_pushstring(L, (char *)res->data);
    else
      lua_pushstring(L, "");
    releasereadimage(L, my_image);
    g_list_free_full(res, g_free);
    return 1;
  }
  else
  {
    dt_image_t *my_image = checkwriteimage(L, 1);
    dt_metadata_set(my_image->id, key, luaL_checkstring(L, 3), FALSE);
    dt_image_synch_xmp(my_image->id);
    releasewriteimage(L, my_image);
    return 0;
  }
}

static int exif_datetime_taken_member(lua_State *L)
{
  if(lua_gettop(L) != 3)
  {
    const dt_image_t *my_image = checkreadimage(L, 1);
    int datetime_size = dt_conf_get_bool("lighttable/ui/milliseconds") ? DT_DATETIME_LENGTH 
                                                                       : DT_DATETIME_EXIF_LENGTH;
    char *sdt = calloc(datetime_size, sizeof(char));
    dt_datetime_img_to_exif(sdt, datetime_size, my_image);
    lua_pushstring(L, sdt);
    releasereadimage(L, my_image);
    free(sdt);
    return 1;
  }
  else
  {
    dt_image_t *my_image = checkwriteimage(L, 1);
    dt_datetime_exif_to_img(my_image, luaL_checkstring(L, 3));
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
    dt_collection_update_query(darktable.collection,
                               DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_COLORLABEL,
                               g_list_prepend(NULL, GINT_TO_POINTER(imgid)));
    return 0;
  }
}

static int is_altered_member(lua_State *L)
{
  const dt_image_t *my_image = checkreadimage(L, 1);
  lua_pushboolean(L, dt_image_altered(my_image->id));
  releasereadimage(L, my_image);
  return 1;
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
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM main.images WHERE group_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, group_id);
  lua_newtable(L);
  int table_index = 1;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    luaA_push(L, dt_lua_image_t, &imgid);
    lua_seti(L, -2, table_index);
    table_index++;
  }
  sqlite3_finalize(stmt);
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
  luaA_struct_member(L, dt_image_t, exif_exposure_bias, float);
  luaA_struct_member(L, dt_image_t, exif_aperture, float);
  luaA_struct_member(L, dt_image_t, exif_iso, float);
  luaA_struct_member(L, dt_image_t, exif_focal_length, float);
  luaA_struct_member(L, dt_image_t, exif_focus_distance, float);
  luaA_struct_member(L, dt_image_t, exif_crop, float);
  luaA_struct_member(L, dt_image_t, exif_maker, char_64);
  luaA_struct_member(L, dt_image_t, exif_model, char_64);
  luaA_struct_member(L, dt_image_t, exif_lens, char_128);
  luaA_struct_member(L, dt_image_t, filename, const char_filename_length);
  luaA_struct_member(L, dt_image_t, width, const int32_t);
  luaA_struct_member(L, dt_image_t, height, const int32_t);
  luaA_struct_member(L, dt_image_t, final_width, const int32_t);
  luaA_struct_member(L, dt_image_t, final_height, const int32_t);
  luaA_struct_member(L, dt_image_t, p_width, const int32_t);
  luaA_struct_member(L, dt_image_t, p_height, const int32_t);
  luaA_struct_member(L, dt_image_t, aspect_ratio, const float);

  luaA_struct_member_name(L, dt_image_t, geoloc.longitude, protected_double, longitude); // set to NAN if value is not set
  luaA_struct_member_name(L, dt_image_t, geoloc.latitude, protected_double, latitude); // set to NAN if value is not set
  luaA_struct_member_name(L, dt_image_t, geoloc.elevation, protected_double, elevation); // set to NAN if value is not set

  dt_lua_init_int_type(L, dt_lua_image_t);

  const char *member_name = luaA_struct_next_member_name(L, dt_image_t, LUAA_INVALID_MEMBER_NAME);
  while(member_name != LUAA_INVALID_MEMBER_NAME)
  {
    lua_pushcfunction(L, image_luaautoc_member);
    luaA_Type member_type = luaA_struct_typeof_member_name(L, dt_image_t, member_name);
    if(luaA_conversion_to_registered_type(L, member_type)
       || luaA_struct_registered_type(L, member_type)
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
  lua_pushcfunction(L, sidecar_member);
  dt_lua_type_register_const(L, dt_lua_image_t, "sidecar");
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
  lua_pushcfunction(L, is_altered_member);
  dt_lua_type_register_const(L, dt_lua_image_t, "is_altered");
  // read/write functions
  lua_pushcfunction(L, has_txt_member);
  dt_lua_type_register(L, dt_lua_image_t, "has_txt");
  lua_pushcfunction(L, rating_member);
  dt_lua_type_register(L, dt_lua_image_t, "rating");
  lua_pushcfunction(L, local_copy_member);
  dt_lua_type_register(L, dt_lua_image_t, "local_copy");
  const char **name = dt_colorlabels_name;
  while(*name)
  {
    lua_pushcfunction(L, colorlabel_member);
    dt_lua_type_register(L, dt_lua_image_t, *name);
    name++;
  }
  lua_pushcfunction(L, exif_datetime_taken_member);
  dt_lua_type_register(L, dt_lua_image_t, "exif_datetime_taken");
  // metadata
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(dt_metadata_get_type(i) != DT_METADATA_TYPE_INTERNAL)
    {
      lua_pushcfunction(L, metadata_member);
      dt_lua_type_register(L, dt_lua_image_t, dt_metadata_get_subkey(i));
    }
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
  lua_pushcfunction(L, generate_cache);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_image_t, "generate_cache");
  lua_pushcfunction(L, image_tostring);
  dt_lua_type_setmetafield(L,dt_lua_image_t,"__tostring");

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
