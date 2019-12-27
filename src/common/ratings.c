/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.
    copyright (c) 2019 philippe weyland.

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
#include "common/grouping.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/gtk.h"

typedef struct dt_undo_ratings_t
{
  int imgid;
  int before;
  int after;
} dt_undo_ratings_t;

int dt_ratings_get(const int imgid)
{
  int stars = 0;
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(image)
  {
    stars = 0x7 & image->flags;
    dt_image_cache_read_release(darktable.image_cache, image);
  }
  return stars;
}

static void _ratings_apply_to_image(int imgid, int rating)
{
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  if(image)
  {
    image->flags = (image->flags & ~0x7) | (0x7 & rating);
    // synch through:
    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
  }
  else
  {
    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);
  }
}

static void _pop_undo(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action, GList **imgs)
{
  if(type == DT_UNDO_RATINGS)
  {
    GList *list = (GList *)data;

    while(list)
    {
      dt_undo_ratings_t *ratings = (dt_undo_ratings_t *)list->data;
      _ratings_apply_to_image(ratings->imgid, (action == DT_ACTION_UNDO) ? ratings->before : ratings->after);
      *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(ratings->imgid));
      list = g_list_next(list);
    }
    dt_collection_hint_message(darktable.collection);
  }
}

static void _ratings_undo_data_free(gpointer data)
{
  GList *l = (GList *)data;
  g_list_free(l);
}

static void _ratings_apply(GList *imgs, const int rating, GList **undo, const gboolean undo_on)
{
  GList *images = imgs;
  while(images)
  {
    const int image_id = GPOINTER_TO_INT(images->data);
    if(undo_on)
    {
      dt_undo_ratings_t *undoratings = (dt_undo_ratings_t *)malloc(sizeof(dt_undo_ratings_t));
      undoratings->imgid = image_id;
      undoratings->before = dt_ratings_get(image_id);
      undoratings->after = rating;
      *undo = g_list_append(*undo, undoratings);
    }

    _ratings_apply_to_image(image_id, rating);

    images = g_list_next(images);
  }
}

void dt_ratings_apply(const int imgid, const int rating, const gboolean toggle_on, const gboolean undo_on, const gboolean group_on)
{
  GList *undo = NULL;
  GList *imgs = NULL;
  guint count = 0;
  if(imgid == -1)
    imgs = dt_collection_get_selected(darktable.collection, -1);
  else
    imgs = g_list_append(imgs, GINT_TO_POINTER(imgid));

  int new_rating = rating;
  // one star is a toggle, so you can easily reject images by removing the last star:
  // The ratings should be consistent for the whole selection, so this logic is only applied to the first image.
  if(toggle_on && !dt_conf_get_bool("rating_one_double_tap") && (rating == 1))
  {
    if(dt_ratings_get(GPOINTER_TO_INT(imgs->data)) == 1)
      new_rating = 0;
  }

  if(imgs)
  {
    if(group_on) dt_grouping_add_grouped_images(&imgs);
    count = g_list_length(imgs);
    if(rating == 6)
      dt_control_log(ngettext("rejecting %d image", "rejecting %d images", count), count);
    else
      dt_control_log(ngettext("applying rating %d to %d image", "applying rating %d to %d images", count),
                     rating, count);

    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_RATINGS);
    _ratings_apply(imgs, new_rating, &undo, undo_on);

    g_list_free(imgs);
    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_RATINGS, undo, _pop_undo, _ratings_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }
    dt_collection_hint_message(darktable.collection);
  }
  else
    dt_control_log(_("no images selected to apply rating"));
  /* redraw view */
  /* dt_control_queue_redraw_center() */
  /* needs to be called in the caller function */
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
