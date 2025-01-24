/*
    This file is part of darktable,
    Copyright (C) 2011-2024 darktable developers.

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

#include "common/grouping.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/selection.h"
#include "control/signal.h"
#include "gui/gtk.h"

#ifdef USE_LUA
#include "lua/call.h"
#include "lua/events.h"
#include "lua/image.h"
#endif

/** add an image to a group */
void dt_grouping_add_to_group(const dt_imgid_t group_id,
                              const dt_imgid_t image_id)
{
  // remove from old group
  dt_grouping_remove_from_group(image_id);

  dt_image_t *img = dt_image_cache_get(darktable.image_cache, image_id, 'w');
  img->group_id = group_id;
  dt_image_cache_write_release_info(darktable.image_cache, img,
                                    DT_IMAGE_CACHE_SAFE, "dt_grouping_add_to_group");
  GList *imgs = NULL;
  imgs = g_list_prepend(imgs, GINT_TO_POINTER(image_id));
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_IMAGE_INFO_CHANGED, imgs);

#ifdef USE_LUA
   dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME, "const char*", "image-group-information-changed",
      LUA_ASYNC_TYPENAME, "const char*", "add",
      LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(image_id),
      LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(group_id),
      LUA_ASYNC_DONE);
#endif
}

/** remove an image from a group */
dt_imgid_t dt_grouping_remove_from_group(const dt_imgid_t image_id)
{
  sqlite3_stmt *stmt;
  dt_imgid_t new_group_id = NO_IMGID;
  GList *imgs = NULL;

  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, image_id, 'r');
  const int img_group_id = img->group_id;
  dt_image_cache_read_release(darktable.image_cache, img);
  if(img_group_id == image_id)
  {
    // get a new group_id for all the others in the group. also write
    // it to the dt_image_t struct.
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT id"
                                " FROM main.images"
                                " WHERE group_id = ?1 AND id != ?2",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img_group_id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, image_id);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      dt_imgid_t other_id = sqlite3_column_int(stmt, 0);
      if(!dt_is_valid_imgid(new_group_id))
        new_group_id = other_id;
      dt_image_t *other_img = dt_image_cache_get(darktable.image_cache, other_id, 'w');
      other_img->group_id = new_group_id;
      dt_image_cache_write_release_info(darktable.image_cache, other_img,
                                        DT_IMAGE_CACHE_SAFE, "dt_grouping_add_to_group");
      imgs = g_list_prepend(imgs, GINT_TO_POINTER(other_id));
    }
    sqlite3_finalize(stmt);
    if(dt_is_valid_imgid(new_group_id))
    {
      DT_DEBUG_SQLITE3_PREPARE_V2
        (dt_database_get(darktable.db),
         "UPDATE main.images SET group_id = ?1 WHERE group_id = ?2 AND id != ?3",
         -1, &stmt,
         NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, new_group_id);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, img_group_id);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, image_id);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
#ifdef USE_LUA
      dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
          0, NULL, NULL,
          LUA_ASYNC_TYPENAME, "const char*", "image-group-information-changed",
          LUA_ASYNC_TYPENAME, "const char*", "remove-leader",
          LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(image_id),
          LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(new_group_id),
          LUA_ASYNC_DONE);
#endif
    }
    else
    {
      // no change was made, no point in raising signal, bailing early
      return -1;
    }
  }
  else
  {
    // change the group_id for this image.
    dt_image_t *wimg = dt_image_cache_get(darktable.image_cache, image_id, 'w');
    new_group_id = wimg->group_id;
    wimg->group_id = image_id;
    dt_image_cache_write_release_info(darktable.image_cache, wimg,
                                      DT_IMAGE_CACHE_SAFE, "dt_grouping_add_to_group");
    imgs = g_list_prepend(imgs, GINT_TO_POINTER(image_id));
    // refresh also the group leader which may be alone now
    imgs = g_list_prepend(imgs, GINT_TO_POINTER(img_group_id));
#ifdef USE_LUA
    dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME, "const char*", "image-group-information-changed",
      LUA_ASYNC_TYPENAME, "const char*", "remove",
      LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(image_id),
      LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(img_group_id),
      LUA_ASYNC_DONE);
#endif
  }
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_IMAGE_INFO_CHANGED, imgs);

  return new_group_id;
}

/** make an image the representative of the group it is in */
dt_imgid_t dt_grouping_change_representative(const dt_imgid_t image_id)
{
  sqlite3_stmt *stmt;

  dt_image_t *img = dt_image_cache_get(darktable.image_cache, image_id, 'r');
  const dt_imgid_t group_id = img->group_id;
  dt_image_cache_read_release(darktable.image_cache, img);

  GList *imgs = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM main.images WHERE group_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, group_id);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const dt_imgid_t other_id = sqlite3_column_int(stmt, 0);
    dt_image_t *other_img = dt_image_cache_get(darktable.image_cache, other_id, 'w');
    other_img->group_id = image_id;
    dt_image_cache_write_release_info(darktable.image_cache, other_img,
                                      DT_IMAGE_CACHE_SAFE,
                                      "dt_grouping_change_representative");
    imgs = g_list_prepend(imgs, GINT_TO_POINTER(other_id));
  }
  sqlite3_finalize(stmt);
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_IMAGE_INFO_CHANGED, imgs);

#ifdef USE_LUA
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME, "const char*", "image-group-information-changed",
      LUA_ASYNC_TYPENAME, "const char*", "leader-change",
      LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(image_id),
      LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(image_id),
      LUA_ASYNC_DONE);
#endif

  return image_id;
}

/** get images of the group */
GList *dt_grouping_get_group_images(const dt_imgid_t imgid)
{
  GList *imgs = NULL;
  const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(image)
  {
    const dt_imgid_t img_group_id = image->group_id;
    dt_image_cache_read_release(darktable.image_cache, image);
    if(darktable.gui && darktable.gui->grouping && darktable.gui->expanded_group_id != img_group_id)
    {
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT id FROM main.images WHERE group_id = ?1", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img_group_id);

      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const dt_imgid_t image_id = sqlite3_column_int(stmt, 0);
        imgs = g_list_prepend(imgs, GINT_TO_POINTER(image_id));
      }
      sqlite3_finalize(stmt);
    }
    else imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgid));
  }
  return g_list_reverse(imgs);
}

/** add grouped images to images list */
void dt_grouping_add_grouped_images(GList **images)
{
  if(!*images) return;
  GList *gimgs = NULL;
  for(GList *imgs = *images; imgs; imgs = g_list_next(imgs))
  {
    const dt_image_t *image = dt_image_cache_get(darktable.image_cache, GPOINTER_TO_INT(imgs->data), 'r');
    if(image)
    {
      const dt_imgid_t img_group_id = image->group_id;
      dt_image_cache_read_release(darktable.image_cache, image);
      if(darktable.gui && darktable.gui->grouping && darktable.gui->expanded_group_id != img_group_id
         && dt_selection_get_collection(darktable.selection))
      {
        sqlite3_stmt *stmt;
        // clang-format off
        gchar *query = g_strdup_printf(
            "SELECT id"
            "  FROM main.images"
            "  WHERE group_id = %d AND id IN (%s)",
            img_group_id, dt_collection_get_query_no_group(dt_selection_get_collection(darktable.selection)));
        // clang-format on
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

        while(sqlite3_step(stmt) == SQLITE_ROW)
        {
          const dt_imgid_t image_id = sqlite3_column_int(stmt, 0);
          if(image_id != GPOINTER_TO_INT(imgs->data))
            gimgs = g_list_prepend(gimgs, GINT_TO_POINTER(image_id));
        }
        sqlite3_finalize(stmt);
        g_free(query);
      }
    }
  }

  if(gimgs)
    *images = g_list_concat(*images, g_list_reverse(gimgs));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
