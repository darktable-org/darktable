/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.

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
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/ratings.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/gtk.h"

typedef struct dt_undo_ratings_t
{
  int imgid;
  int before_rating;
  int after_rating;
} dt_undo_ratings_t;

static void _ratings_apply_to_image(int imgid, int rating, gboolean undo);

static void _pop_undo(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action)
{
  if(type == DT_UNDO_RATINGS)
  {
    dt_undo_ratings_t *ratings = (dt_undo_ratings_t *)data;

    if(action == DT_ACTION_UNDO)
      _ratings_apply_to_image(ratings->imgid, ratings->before_rating, FALSE);
    else
      _ratings_apply_to_image(ratings->imgid, ratings->after_rating, FALSE);
  }
}

static void _ratings_undo_data_free(gpointer data)
{
  free(data);
}

static void _ratings_apply_to_image(int imgid, int rating, gboolean undo)
{
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  if(image)
  {
    if(undo)
    {
      dt_undo_ratings_t *ratings = malloc(sizeof(dt_undo_ratings_t));
      ratings->imgid = imgid;
      ratings->before_rating = 0x7 & image->flags;
      ratings->after_rating = rating;
      dt_undo_record(darktable.undo, NULL, DT_UNDO_RATINGS, (dt_undo_data_t)ratings,
                     _pop_undo, _ratings_undo_data_free);
    }

    image->flags = (image->flags & ~0x7) | (0x7 & rating);
    // synch through:
    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);

    dt_collection_hint_message(darktable.collection);
  }
  else
  {
    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);
  }
}

void dt_ratings_apply_to_image(int imgid, int rating)
{
  _ratings_apply_to_image(imgid, rating, TRUE);
}

void dt_ratings_apply_to_image_or_group(int imgid, int rating)
{
  const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(image)
  {
    int img_group_id = image->group_id;

    // one star is a toggle, so you can easily reject images by removing the last star:
    if(((image->flags & 0x7) == 1) && !dt_conf_get_bool("rating_one_double_tap") && (rating == 1))
    {
      rating = 0;
    }
    else if(((image->flags & 0x7) == 6) && (rating == 6))
    {
      rating = 0;
    }

    dt_image_cache_read_release(darktable.image_cache, image);

    dt_undo_start_group(darktable.undo, DT_UNDO_RATINGS);

    // If we're clicking on a grouped image, apply the rating to all images in the group.
    if(darktable.gui && darktable.gui->grouping && darktable.gui->expanded_group_id != img_group_id)
    {
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT id FROM main.images WHERE group_id = ?1", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img_group_id);
      int count = 0;

      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        dt_ratings_apply_to_image(sqlite3_column_int(stmt, 0), rating);
        count++;
      }
      sqlite3_finalize(stmt);

      if(count > 1)
      {
        if(rating == 6)
          dt_control_log(ngettext("rejecting %d image", "rejecting %d images", count), count);
        else
          dt_control_log(ngettext("applying rating %d to %d image", "applying rating %d to %d images", count),
                         rating, count);
      }
    }
    else
    {
      dt_ratings_apply_to_image(imgid, rating);
    }

    dt_undo_end_group(darktable.undo);
  }
}

void dt_ratings_apply_to_selection(int rating)
{
  uint32_t count = dt_collection_get_selected_count(darktable.collection);
  if(count)
  {
    if(rating == 6)
      dt_control_log(ngettext("rejecting %d image", "rejecting %d images", count), count);
    else
      dt_control_log(ngettext("applying rating %d to %d image", "applying rating %d to %d images", count),
                     rating, count);
#if 0 // not updating cache
    gchar query[1024]= {0};
    g_snprintf(query,sizeof(query), "UPDATE main.images SET flags=(flags & ~7) | (7 & %d) WHERE id IN "
                                    "(SELECT imgid FROM main.selected_images)", rating);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);
#endif

    /* for each selected image update rating */
    sqlite3_stmt *stmt;
    gboolean first = TRUE;
    dt_undo_start_group(darktable.undo, DT_UNDO_RATINGS);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt,
                                NULL);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      if(first == TRUE)
      {
        first = FALSE;
        const dt_image_t *image = dt_image_cache_get(darktable.image_cache, sqlite3_column_int(stmt, 0), 'r');

        // one star is a toggle, so you can easily reject images by removing the last star:
        // The ratings should be consistent for the whole selection, so this logic is only applied to the first image.
        if(((image->flags & 0x7) == 1) && !dt_conf_get_bool("rating_one_double_tap") && (rating == 1))
        {
          rating = 0;
        }

        dt_image_cache_read_release(darktable.image_cache, image);
      }

      dt_ratings_apply_to_image(sqlite3_column_int(stmt, 0), rating);
    }
    sqlite3_finalize(stmt);
    dt_undo_end_group(darktable.undo);

    /* redraw view */
    /* dt_control_queue_redraw_center() */
    /* needs to be called in the caller function */
  }
  else
    dt_control_log(_("no images selected to apply rating"));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
