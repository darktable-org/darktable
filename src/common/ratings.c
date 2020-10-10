/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#include "views/view.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/gtk.h"

typedef struct dt_undo_ratings_t
{
  int imgid;
  int before;
  int after;
} dt_undo_ratings_t;

const int dt_ratings_get(const int imgid)
{
  int stars = 0;
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(image)
  {
    if(image->flags & DT_IMAGE_REJECTED)
      stars = DT_VIEW_REJECT;
    else
      stars = DT_VIEW_RATINGS_MASK & image->flags;
    dt_image_cache_read_release(darktable.image_cache, image);
  }
  return stars;
}

static void _ratings_apply_to_image(const int imgid, const int rating)
{
  int new_rating = rating;
  const int previous_rating = dt_ratings_get(imgid);
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  if(image)
  {
    if(new_rating == DT_VIEW_REJECT)
    {
      // this is a toggle, we invert the DT_IMAGE_REJECTED flag
      if(image->flags & DT_IMAGE_REJECTED)
        image->flags = (image->flags & ~DT_IMAGE_REJECTED);
      else
        image->flags = (image->flags | DT_IMAGE_REJECTED);
    }
    else
    {
      if(!dt_conf_get_bool("rating_one_double_tap")
          && (previous_rating == DT_VIEW_STAR_1) && (new_rating == DT_VIEW_STAR_1))
      {
        new_rating = DT_VIEW_DESERT;
      }

      image->flags = (image->flags & ~(DT_IMAGE_REJECTED | DT_VIEW_RATINGS_MASK))
        | (DT_VIEW_RATINGS_MASK & new_rating);
    }
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

void dt_ratings_apply_on_list(const GList *img, const int rating, const gboolean undo_on)
{
  GList *imgs = g_list_copy((GList *)img);
  if(imgs)
  {
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_RATINGS);

    _ratings_apply(imgs, rating, &undo, undo_on);

    g_list_free(imgs);
    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_RATINGS, undo, _pop_undo, _ratings_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }
    dt_collection_hint_message(darktable.collection);
  }
}

void dt_ratings_apply_on_image(const int imgid, const int rating, const gboolean toggle_on,
                               const gboolean undo_on, const gboolean group_on)
{
  GList *imgs = NULL;
  int new_rating = rating;

  if(imgid > 0) imgs = g_list_append(imgs, GINT_TO_POINTER(imgid));

  if(imgs)
  {
    const int previous_rating = dt_ratings_get(GPOINTER_TO_INT(imgs->data));
    // one star is a toggle, so you can easily reject images by removing the last star:
    // The ratings should be consistent for the whole selection, so this logic is only applied to the first image.
    if(toggle_on && !dt_conf_get_bool("rating_one_double_tap") &&
      (previous_rating == DT_VIEW_STAR_1) && (new_rating == DT_VIEW_STAR_1))
    {
      new_rating = DT_VIEW_DESERT;
    }

    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_RATINGS);
    if(group_on) dt_grouping_add_grouped_images(&imgs);

    const guint count = g_list_length(imgs);
    if(count > 1)
    {
      if(new_rating == DT_VIEW_REJECT)
        dt_control_log(ngettext("rejecting %d image", "rejecting %d images", count), count);
      else
        dt_control_log(ngettext("applying rating %d to %d image", "applying rating %d to %d images", count),
                       new_rating, count);
    }

    _ratings_apply(imgs, new_rating, &undo, undo_on);

    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_RATINGS, undo, _pop_undo, _ratings_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }
    g_list_free(imgs);
  }
  else
    dt_control_log(_("no images selected to apply rating"));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
