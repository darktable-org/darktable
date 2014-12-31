/*
    This file is part of darktable,
    copyright (c) 2011 tobias ellinghaus.

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

#include "common/darktable.h"
#include "common/debug.h"
#include "common/grouping.h"
#include "common/image_cache.h"

/** add an image to a group */
void dt_grouping_add_to_group(int group_id, int image_id)
{
  // remove from old group
  dt_grouping_remove_from_group(image_id);

  dt_image_t *img = dt_image_cache_get(darktable.image_cache, image_id, 'w');
  img->group_id = group_id;
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);
}

/** remove an image from a group */
int dt_grouping_remove_from_group(int image_id)
{
  sqlite3_stmt *stmt;
  int new_group_id = -1;

  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, image_id, 'r');
  int img_group_id = img->group_id;
  dt_image_cache_read_release(darktable.image_cache, img);
  if(img_group_id == image_id)
  {
    // get a new group_id for all the others in the group. also write it to the dt_image_t struct.
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "select id from images where group_id = ?1 and id != ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img_group_id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, image_id);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int other_id = sqlite3_column_int(stmt, 0);
      if(new_group_id == -1) new_group_id = other_id;
      dt_image_t *other_img = dt_image_cache_get(darktable.image_cache, other_id, 'w');
      other_img->group_id = new_group_id;
      dt_image_cache_write_release(darktable.image_cache, other_img, DT_IMAGE_CACHE_SAFE);
    }
    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "update images set group_id = ?1 where group_id = ?2 and id != ?3", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, new_group_id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, img_group_id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, image_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else
  {
    // change the group_id for this image.
    dt_image_t *wimg = dt_image_cache_get(darktable.image_cache, image_id, 'w');
    new_group_id = wimg->group_id;
    wimg->group_id = image_id;
    dt_image_cache_write_release(darktable.image_cache, wimg, DT_IMAGE_CACHE_SAFE);
  }
  return new_group_id;
}

/** make an image the representative of the group it is in */
int dt_grouping_change_representative(int image_id)
{
  sqlite3_stmt *stmt;

  dt_image_t *img = dt_image_cache_get(darktable.image_cache, image_id, 'w');
  int group_id = img->group_id;
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where group_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, group_id);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int other_id = sqlite3_column_int(stmt, 0);
    dt_image_t *other_img = dt_image_cache_get(darktable.image_cache, other_id, 'w');
    other_img->group_id = image_id;
    dt_image_cache_write_release(darktable.image_cache, other_img, DT_IMAGE_CACHE_SAFE);
  }
  sqlite3_finalize(stmt);

  return image_id;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
