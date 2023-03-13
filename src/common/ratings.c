/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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
#include "gui/accelerators.h"

#define DT_RATINGS_UPGRADE -1
#define DT_RATINGS_DOWNGRADE -2
#define DT_RATINGS_REJECT -3
#define DT_RATINGS_UNREJECT -4

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
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  if(image)
  {
    // apply or remove rejection
    if(new_rating == DT_RATINGS_REJECT)
      image->flags = (image->flags | DT_IMAGE_REJECTED);
    else if(new_rating == DT_RATINGS_UNREJECT)
      image->flags = (image->flags & ~DT_IMAGE_REJECTED);
    else
    {
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
    for(GList *list = (GList *)data; list; list = g_list_next(list))
    {
      dt_undo_ratings_t *ratings = (dt_undo_ratings_t *)list->data;
      _ratings_apply_to_image(ratings->imgid, (action == DT_ACTION_UNDO) ? ratings->before : ratings->after);
      *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(ratings->imgid));
    }
    dt_collection_hint_message(darktable.collection);
  }
}

static void _ratings_undo_data_free(gpointer data)
{
  GList *l = (GList *)data;
  g_list_free(l);
}

// wrapper that does some precalculation to deal with toggle effects and rating increase/decrease
static void _ratings_apply(const GList *imgs, const int rating, GList **undo, const gboolean undo_on)
{
  // REJECTION and SINGLE_STAR rating can have a toggle effect
  // but we only toggle off if ALL images have that rating
  // so we need to check every image first
  gboolean toggle = FALSE;

  if(rating == DT_VIEW_REJECT)
  {
    toggle = TRUE;
    for(const GList *images = imgs; images; images = g_list_next(images))
    {
      if(dt_ratings_get(GPOINTER_TO_INT(images->data)) != DT_VIEW_REJECT)
      {
        toggle = FALSE;
        break;
      }
    }
  }
  else if(!dt_conf_get_bool("rating_one_double_tap") && (rating == DT_VIEW_STAR_1))
  {
    toggle = TRUE;
    for(const GList *images = imgs; images; images = g_list_next(images))
    {
      if(dt_ratings_get(GPOINTER_TO_INT(images->data)) != DT_VIEW_STAR_1)
      {
        toggle = FALSE;
        break;
      }
    }
  }

  for(const GList *images = imgs; images; images = g_list_next(images))
  {
    const int image_id = GPOINTER_TO_INT(images->data);
    const int old_rating = dt_ratings_get(image_id);
    if(undo_on)
    {
      dt_undo_ratings_t *undoratings = (dt_undo_ratings_t *)malloc(sizeof(dt_undo_ratings_t));
      undoratings->imgid = image_id;
      undoratings->before = old_rating;
      undoratings->after = rating;
      *undo = g_list_append(*undo, undoratings);
    }

    int new_rating = rating;
    // do not 'DT_RATINGS_UPGRADE' or 'DT_RATINGS_UPGRADE' if image was rejected
    if(old_rating == DT_VIEW_REJECT && rating < DT_VIEW_DESERT)
      new_rating = DT_VIEW_REJECT;
    else if(rating == DT_RATINGS_UPGRADE)
      new_rating = MIN(DT_VIEW_STAR_5, old_rating + 1);
    else if(rating == DT_RATINGS_DOWNGRADE)
      new_rating = MAX(DT_VIEW_DESERT, old_rating - 1);
    else if(rating == DT_VIEW_STAR_1 && toggle)
      new_rating = DT_VIEW_DESERT;
    else if(rating == DT_VIEW_REJECT && toggle)
      new_rating = DT_RATINGS_UNREJECT;
    else if(rating == DT_VIEW_REJECT && !toggle)
      new_rating = DT_RATINGS_REJECT;

    _ratings_apply_to_image(image_id, new_rating);
  }
}

void dt_ratings_apply_on_list(const GList *img, const int rating, const gboolean undo_on)
{
  if(img)
  {
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_RATINGS);

    _ratings_apply(img, rating, &undo, undo_on);

    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_RATINGS, undo, _pop_undo, _ratings_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }
    dt_collection_hint_message(darktable.collection);
  }
}

void dt_ratings_apply_on_image(const int imgid, const int rating, const gboolean single_star_toggle,
                               const gboolean undo_on, const gboolean group_on)
{
  GList *imgs = NULL;
  int new_rating = rating;

  if(imgid > 0) imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgid));

  if(imgs)
  {
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_RATINGS);
    if(group_on) dt_grouping_add_grouped_images(&imgs);

    if(!g_list_shorter_than(imgs, 2)) // pop up a toast if rating multiple images at once
    {
      const guint count = g_list_length(imgs);
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

enum
{
  DT_ACTION_EFFECT_SELECT = DT_ACTION_EFFECT_DEFAULT_KEY,
  DT_ACTION_EFFECT_UPGRADE = DT_ACTION_EFFECT_DEFAULT_UP,
  DT_ACTION_EFFECT_DOWNGRADE = DT_ACTION_EFFECT_DEFAULT_DOWN,
};

static float _action_process_rating(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  float return_value = NAN;

  if(!isnan(move_size))
  {
    if(element != DT_VIEW_REJECT)
    {
      switch(effect)
      {
      case DT_ACTION_EFFECT_SELECT:
        break;
      case DT_ACTION_EFFECT_UPGRADE:
        element = DT_RATINGS_UPGRADE;
        break;
      case DT_ACTION_EFFECT_DOWNGRADE:
        element = DT_RATINGS_DOWNGRADE;
        break;
      default:
        dt_print(DT_DEBUG_ALWAYS,
                 "[_action_process_rating] unknown shortcut effect (%d) for rating\n", effect);
        break;
      }
    }

    GList *imgs = dt_act_on_get_images(FALSE, TRUE, FALSE);
    dt_ratings_apply_on_list(imgs, element, TRUE);

    // if we are in darkroom we show a message as there might be no other indication
    const dt_view_t *v = dt_view_manager_get_current_view(darktable.view_manager);
    if(v->view(v) == DT_VIEW_DARKROOM && g_list_is_singleton(imgs) && darktable.develop->preview_pipe)
    {
      // we verify that the image is the active one
      const int id = GPOINTER_TO_INT(imgs->data);
      if(id == darktable.develop->preview_pipe->output_imgid)
      {
        const dt_image_t *img = dt_image_cache_get(darktable.image_cache, id, 'r');
        if(img)
        {
          const int r = img->flags & DT_IMAGE_REJECTED ? DT_VIEW_REJECT : (img->flags & DT_VIEW_RATINGS_MASK);
          dt_image_cache_read_release(darktable.image_cache, img);

          // translate in human readable value
          if(r == DT_VIEW_REJECT)
            dt_toast_log(_("image rejected"));
          else if(r == 0)
            dt_toast_log(_("image rated to 0 star"));
          else
            dt_toast_log(_("image rated to %s"), r == 1 ? "★" :
                                                 r == 2 ? "★★" :
                                                 r == 3 ? "★★★" :
                                                 r == 4 ? "★★★★" :
                                                 r == 5 ? "★★★★★" :
                                                 _("unknown"));
          return_value = - r + (r >= element ? DT_VALUE_PATTERN_ACTIVE : 0);
        }
      }
    }

    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_RATING_RANGE, imgs);
  }
  else if(darktable.develop)
  {
    const int image_id = darktable.develop->image_storage.id;
    if(image_id != -1)
    {
      int rating = dt_ratings_get(image_id);
      return_value = - rating + (rating >= element ? DT_VALUE_PATTERN_ACTIVE : 0);
    }
  }

  return return_value + DT_VALUE_PATTERN_SUM;
}

const gchar *dt_action_effect_rating[]
  = { N_("select"),
      N_("upgrade"),
      N_("downgrade"),
      NULL };

const dt_action_element_def_t _action_elements_rating[]
  = { { N_("zero"  ), dt_action_effect_rating },
      { N_("one"   ), dt_action_effect_rating },
      { N_("two"   ), dt_action_effect_rating },
      { N_("three" ), dt_action_effect_rating },
      { N_("four"  ), dt_action_effect_rating },
      { N_("five"  ), dt_action_effect_rating },
      { N_("reject"), dt_action_effect_activate },
      { NULL } };

const dt_action_def_t dt_action_def_rating
  = { N_("rating"),
      _action_process_rating,
      _action_elements_rating };

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

