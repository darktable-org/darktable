/*
    This file is part of darktable,
    Copyright (C) 2021-2024 darktable developers.

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

#include "common/act_on.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/selection.h"
#include "control/control.h"
#include "dtgtk/thumbtable.h"
#include <glib.h>
#include <sqlite3.h>

// get the algorithm set in preference
dt_act_on_algorithm_t dt_act_on_get_algorithm()
{
  if(dt_conf_get_bool("plugins/lighttable/act_on"))
    return DT_ACT_ON_HOVER;
  else
    return DT_ACT_ON_SELECTION;
}

gboolean dt_act_on_use_culling_selection()
{
  return (dt_act_on_get_algorithm() == DT_ACT_ON_SELECTION);
}

static int _find_custom(gconstpointer a, gconstpointer b)
{
  return (GPOINTER_TO_INT(a) != GPOINTER_TO_INT(b));
}

static void _insert_in_list(GList **list,
                            const dt_imgid_t imgid,
                            gboolean only_visible)
{
  if(only_visible)
  {
    if(!g_list_find_custom(*list, GINT_TO_POINTER(imgid), _find_custom))
      *list = g_list_append(*list, GINT_TO_POINTER(imgid));
    return;
  }

  const dt_image_t *image = dt_image_cache_get(imgid, 'r');
  if(image)
  {
    const dt_imgid_t img_group_id = image->group_id;
    dt_image_cache_read_release(image);

    if(!darktable.gui
       || !darktable.gui->grouping
       || darktable.gui->expanded_group_id == img_group_id
       || !dt_selection_get_collection(darktable.selection))
    {
      if(!g_list_find_custom(*list, GINT_TO_POINTER(imgid), _find_custom))
        *list = g_list_append(*list, GINT_TO_POINTER(imgid));
    }
    else
    {
      sqlite3_stmt *stmt;
      // clang-format off
      gchar *query = g_strdup_printf(
          "SELECT id"
          "  FROM main.images"
          "  WHERE group_id = %d AND id IN (%s)",
          img_group_id,
          dt_collection_get_query_no_group(dt_selection_get_collection(darktable.selection)));
      // clang-format on
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const dt_imgid_t imgidg = sqlite3_column_int(stmt, 0);
        if(!g_list_find_custom(*list, GINT_TO_POINTER(imgidg), _find_custom))
          *list = g_list_append(*list, GINT_TO_POINTER(imgidg));
      }
      sqlite3_finalize(stmt);
      g_free(query);
    }
  }
}

// insert all the active images in the given list
static void _insert_active_images_in_list(GList **list,
                                          gboolean only_visible)
{
  // in the "selection" algo, we have a specific selection system in culling
  if(dt_act_on_get_algorithm() == DT_ACT_ON_SELECTION
     && !dt_view_lighttable_preview_state(darktable.view_manager)
     && (dt_view_lighttable_get_layout(darktable.view_manager) == DT_LIGHTTABLE_LAYOUT_CULLING
         || dt_view_lighttable_get_layout(darktable.view_manager) == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC))
  {
    const dt_imgid_t imgid = dt_view_lighttable_get_culling_selection(darktable.view_manager);
    _insert_in_list(list, imgid, only_visible);
  }
  else
  {
    for(GSList *ll = darktable.view_manager->active_images;
            ll;
            ll = g_slist_next(ll))
    {
      const dt_imgid_t imgid = GPOINTER_TO_INT(ll->data);
      _insert_in_list(list, imgid, only_visible);
      // be absolutely sure we have the id in the list (in darkroom,
      // the active image can be out of collection)
      if(!only_visible)
        _insert_in_list(list, imgid, TRUE);
    }
  }
}

// test if the cache is still valid
static gboolean _test_cache(dt_act_on_cache_t *cache)
{
  const dt_imgid_t mouseover = dt_control_get_mouse_over_id();

  if(cache->ok
     && (cache->image_over == mouseover || dt_act_on_get_algorithm() == DT_ACT_ON_SELECTION)
     && cache->inside_table == dt_ui_thumbtable(darktable.gui->ui)->mouse_inside
     && dt_slist_length_equal(cache->active_imgs, darktable.view_manager->active_images))
  {
    // we test active images if mouse is outside table
    gboolean ok = TRUE;
    if(!dt_ui_thumbtable(darktable.gui->ui)->mouse_inside && cache->active_imgs)
    {
      GSList *l1 = cache->active_imgs;
      GSList *l2 = darktable.view_manager->active_images;
      while(l1 && l2)
      {
        if(GPOINTER_TO_INT(l1->data) != GPOINTER_TO_INT(l2->data))
        {
          ok = FALSE;
          break;
        }
        l2 = g_slist_next(l2);
        l1 = g_slist_next(l1);
      }
    }
    if(ok) return TRUE;
  }
  return FALSE;
}

// register a new cache with the given values
static void _cache_update_register(const gboolean only_visible,
                                   const gboolean ordered,
                                   const gboolean inside_sel,
                                   const dt_imgid_t mouseover,
                                   GList *images)
{
  dt_act_on_cache_t *cache;
  if(only_visible)
    cache = &darktable.view_manager->act_on_cache_visible;
  else
    cache = &darktable.view_manager->act_on_cache_all;

  // let's register the new list as cached
  cache->image_over_inside_sel = inside_sel;
  cache->ordered = ordered;
  cache->image_over = mouseover;
  GList *ltmp = cache->images;
  cache->images = images;
  g_list_free(ltmp);
  cache->images_nb = g_list_length(cache->images);
  GSList *sl = cache->active_imgs;
  cache->active_imgs = g_slist_copy(darktable.view_manager->active_images);
  g_slist_free(sl);
  cache->inside_table = dt_ui_thumbtable(darktable.gui->ui)->mouse_inside;
  cache->ok = TRUE;

  // if needed, we show the list of cached images in terminal
  if((darktable.unmuted & DT_DEBUG_ACT_ON) == DT_DEBUG_ACT_ON)
  {
    gchar *tx = g_strdup_printf("[images to act on - %s] new cache (%s) : ",
                                (dt_act_on_get_algorithm() == DT_ACT_ON_HOVER) ? "hover" : "selection",
                                only_visible ? "visible" : "all");

    for(GList *ll = images;
        ll;
        ll = g_list_next(ll)) dt_util_str_cat(&tx, "%d ", GPOINTER_TO_INT(ll->data));
    dt_print(DT_DEBUG_ACT_ON, "%s", tx);
    g_free(tx);
  }
}

// cache the list of images to act on during global changes (libs, accels)
// return TRUE if the cache is updated, FALSE if it's still up to date
static gboolean _cache_update_hover(const gboolean only_visible,
                                    const gboolean force,
                                    const gboolean ordered)
{
  /** Here's how it works
   *
   *             mouse over| x | x | x |   |   |
   *     mouse inside table| x | x |   |   |   |
   * mouse inside selection| x |   |   |   |   |
   *          active images| ? | ? | x |   | x |
   *                       |   |   |   |   |   |
   *                       | S | O | O | S | A |
   *  S = selection ; O = mouseover ; A = active images
   *  the mouse can be outside thumbtable in case of filmstrip + mouse in center widget
   *
   *  if only_visible is FALSE, then it will add also not visible
   *  images because of grouping force define if we try to use cache
   *  or not if ordered is TRUE, we return the list in the gui
   *  order. Otherwise the order is undefined (but quicker)
   **/

  const dt_imgid_t mouseover = dt_control_get_mouse_over_id();

  dt_act_on_cache_t *cache;
  if(only_visible)
    cache = &darktable.view_manager->act_on_cache_visible;
  else
    cache = &darktable.view_manager->act_on_cache_all;

  // if possible, we return the cached list
  if(!force
     && cache->ordered == ordered
     && _test_cache(cache))
  {
    return FALSE;
  }

  GList *l = NULL;
  gboolean inside_sel = FALSE;

  if(dt_is_valid_imgid(mouseover))
  {
    // column 1,2,3
    if(dt_ui_thumbtable(darktable.gui->ui)->mouse_inside
       || dt_ui_thumbtable(darktable.gui->ui)->key_inside)
    {
      // column 1,2
      sqlite3_stmt *stmt;
      gchar *query = g_strdup_printf("SELECT imgid"
                                     " FROM main.selected_images"
                                     " WHERE imgid=%d", mouseover);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      if(stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
      {
        inside_sel = TRUE;
        sqlite3_finalize(stmt);
      }
      g_free(query);

      if(inside_sel)
      {
        // column 1

        // first, we try to return cached list if we were already
        // inside sel and the selection has not changed
        if(!force
           && cache->ok
           && cache->image_over_inside_sel
           && cache->inside_table
           && cache->ordered == ordered)
        {
          return FALSE;
        }

        // we return the list of the selection
        l = dt_selection_get_list(darktable.selection, only_visible, ordered);
      }
      else
      {
        // column 2
        _insert_in_list(&l, mouseover, only_visible);
      }
    }
    else
    {
      // column 3
      _insert_in_list(&l, mouseover, only_visible);
      // be absolutely sure we have the id in the list (in darkroom,
      // the active image can be out of collection)
      if(!only_visible)
        _insert_in_list(&l, mouseover, TRUE);
    }
  }
  else
  {
    // column 4,5
    if(darktable.view_manager->active_images)
    {
      // column 5
      _insert_active_images_in_list(&l, only_visible);
    }
    else
    {
      // column 4
      // we return the list of the selection
      l = dt_selection_get_list(darktable.selection, only_visible, ordered);
    }
  }

  // let's register the new list as cached
  _cache_update_register(only_visible, ordered, inside_sel, mouseover, l);

  return TRUE;
}
static gboolean _cache_update_selection(const gboolean only_visible,
                                        const gboolean force,
                                        const gboolean ordered)
{
  /** Here's how it works
   *
   *              selection| x | ? | ? |
   *          active images|   | x | x |
   *           culling mode|   |   | x |
   *                       |   |   |   |
   *                       | S | A | C |
   *  S = selection ; A = active images ; C = culling selection
   *
   *  if only_visible is FALSE, then it will add also not visible
   *  images because of grouping force define if we try to use cache
   *  or not if ordered is TRUE, we return the list in the gui
   *  order. Otherwise the order is undefined (but quicker)
   **/

  dt_act_on_cache_t *cache;
  if(only_visible)
    cache = &darktable.view_manager->act_on_cache_visible;
  else
    cache = &darktable.view_manager->act_on_cache_all;

  // if possible, we return the cached list
  if(!force
     && cache->ordered == ordered
     && _test_cache(cache))
  {
    return FALSE;
  }

  GList *l = NULL;

  if(darktable.view_manager->active_images)
  {
    // column 2 & 3 (culling specifity is taken into account by the insertion routine)
    _insert_active_images_in_list(&l, only_visible);
  }
  else
  {
    // column 1
    l = dt_selection_get_list(darktable.selection, only_visible, ordered);
  }

  // let's register the new list as cached
  _cache_update_register(only_visible, ordered, FALSE, NO_IMGID, l);

  return TRUE;
}
static gboolean _cache_update(const gboolean only_visible,
                              const gboolean force,
                              const gboolean ordered)
{
  if(dt_act_on_get_algorithm() == DT_ACT_ON_HOVER)
    return _cache_update_hover(only_visible, force, ordered);
  else
    return _cache_update_selection(only_visible, force, ordered);
}

// get the list of images to act on during global changes (libs, accels)
GList *dt_act_on_get_images(const gboolean only_visible,
                            const gboolean force,
                            const gboolean ordered)
{
  // we first update the cache if needed
  _cache_update(only_visible, force, ordered);

  GList *l = NULL;
  if(only_visible
     && darktable.view_manager->act_on_cache_visible.ok)
  {
    l = g_list_copy((GList *)darktable.view_manager->act_on_cache_visible.images);
  }
  else if(!only_visible
          && darktable.view_manager->act_on_cache_all.ok)
  {
    l = g_list_copy((GList *)darktable.view_manager->act_on_cache_all.images);
  }

  // and we return a copy of the cached list
  return l;
}

// return the list of imageid separated by a comma
// it's used in query
static gchar *_get_query_from_list(GList *l)
{
  gchar *images = NULL;
  for(; l; l = g_list_next(l))
  {
    dt_util_str_cat(&images, "%d,", GPOINTER_TO_INT(l->data));
  }
  if(images)
  {
    // remove trailing comma
    images[strlen(images) - 1] = '\0';
  }
  else
    images = g_strdup(" ");
  return images;
}

// get the query to retrieve images to act on. this is useful to
// speedup actions if they already use sqlite queries
static gchar *_get_query_hover(const gboolean only_visible)
{
  /** Here's how it works
   *
   *             mouse over| x | x | x |   |   |
   *     mouse inside table| x | x |   |   |   |
   * mouse inside selection| x |   |   |   |   |
   *          active images| ? | ? | x |   | x |
   *                       |   |   |   |   |   |
   *                       | S | O | O | S | A |
   *  S = selection ; O = mouseover ; A = active images
   *  the mouse can be outside thumbtable in case of filmstrip + mouse in center widget
   *
   *  if only_visible is FALSE, then it will add also not visible
   *  images because of grouping due to dt_selection_get_list_query
   *  limitation, order is always considered as undefined
   **/

  const dt_imgid_t mouseover = dt_control_get_mouse_over_id();

  GList *l = NULL;
  gboolean inside_sel = FALSE;
  if(dt_is_valid_imgid(mouseover))
  {
    // column 1,2,3
    if(dt_ui_thumbtable(darktable.gui->ui)->mouse_inside
       || dt_ui_thumbtable(darktable.gui->ui)->key_inside)
    {
      // column 1,2
      sqlite3_stmt *stmt;
      gchar *query = g_strdup_printf("SELECT imgid"
                                     " FROM main.selected_images"
                                     " WHERE imgid =%d", mouseover);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      if(stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
      {
        inside_sel = TRUE;
        sqlite3_finalize(stmt);
      }
      g_free(query);

      if(inside_sel)
      {
        // column 1
        return dt_selection_get_list_query(darktable.selection, only_visible, FALSE);
      }
      else
      {
        // column 2
        _insert_in_list(&l, mouseover, only_visible);
      }
    }
    else
    {
      // column 3
      _insert_in_list(&l, mouseover, only_visible);
      // be absolutely sure we have the id in the list (in darkroom,
      // the active image can be out of collection)
      if(!only_visible)
        _insert_in_list(&l, mouseover, TRUE);
    }
  }
  else
  {
    // column 4,5
    if(darktable.view_manager->active_images)
    {
      // column 5
      _insert_active_images_in_list(&l, only_visible);
    }
    else
    {
      // column 4
      return dt_selection_get_list_query(darktable.selection, only_visible, FALSE);
    }
  }

  // if we don't return the selection, we return the list of imgid separated by comma
  // in the form it can be used inside queries
  return _get_query_from_list(l);
}
static gchar *_get_query_selection(const gboolean only_visible)
{
  /** Here's how it works
   *
   *              selection| ? | ? | ? |
   *          active images|   | x | x |
   *           culling mode|   |   | x |
   *                       |   |   |   |
   *                       | S | A | C |
   *  S = selection ; A = active images ; C = culling selection
   *
   *  if only_visible is FALSE, then it will add also not visible
   *  images because of grouping force define if we try to use cache
   *  or not if ordered is TRUE, we return the list in the gui
   *  order. Otherwise the order is undefined (but quicker)
   **/

  GList *l = NULL;

  if(darktable.view_manager->active_images)
  {
    // column 2 & 3 (culling specifity is taken into account by the insertion routine)
    _insert_active_images_in_list(&l, only_visible);
  }
  else
  {
    // column 1
    return dt_selection_get_list_query(darktable.selection, only_visible, FALSE);
  }

  // if we don't return the selection, we return the list of imgid separated by comma
  // in the form it can be used inside queries
  return _get_query_from_list(l);
}
gchar *dt_act_on_get_query(const gboolean only_visible)
{
  if(dt_act_on_get_algorithm() == DT_ACT_ON_HOVER)
    return _get_query_hover(only_visible);
  else
    return _get_query_selection(only_visible);
}

// get the main image to act on during global changes (libs, accels)
static dt_imgid_t _get_main_image_hover()
{
  /** Here's how it works -- same as for list, except we don't care
   * about mouse inside selection or table
   *
   *             mouse over| x |   |   |
   *          active images| ? |   | x |
   *                       |   |   |   |
   *                       | O | S | A |
   *  First image of ...
   *  S = selection ; O = mouseover ; A = active images
   **/

  dt_imgid_t imgid = NO_IMGID;

  const dt_imgid_t mouseover = dt_control_get_mouse_over_id();

  if(dt_is_valid_imgid(mouseover))
  {
    imgid = mouseover;
  }
  else
  {
    if(darktable.view_manager->active_images)
    {
      imgid = GPOINTER_TO_INT(darktable.view_manager->active_images->data);
    }
    else
    {
      sqlite3_stmt *stmt;
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2
        (dt_database_get(darktable.db),
         "SELECT s.imgid"
         " FROM main.selected_images as s, memory.collected_images as c"
         " WHERE s.imgid=c.imgid"
         " ORDER BY c.rowid LIMIT 1",
         -1, &stmt, NULL);
      // clang-format on
      if(stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
      {
        imgid = sqlite3_column_int(stmt, 0);
      }
      if(stmt) sqlite3_finalize(stmt);
    }
  }

  if((darktable.unmuted & DT_DEBUG_ACT_ON) == DT_DEBUG_ACT_ON)
    dt_print(DT_DEBUG_ACT_ON, "[images to act on - hover] single image : %d", imgid);

  return imgid;
}
static dt_imgid_t _get_main_image_selection()
{
  /** Here's how it works -- same as for list
   *
   *              selection| ? | ? | ? |
   *          active images|   | x | x |
   *           culling mode|   |   | x |
   *                       |   |   |   |
   *                       | S | A | C |
   *  First image of ...
   *  S = selection ; A = active images ; C = culling selection
   **/

  dt_imgid_t imgid = NO_IMGID;

  if(darktable.view_manager->active_images)
  {
    if(!dt_view_lighttable_preview_state(darktable.view_manager)
       && (dt_view_lighttable_get_layout(darktable.view_manager) == DT_LIGHTTABLE_LAYOUT_CULLING
           || dt_view_lighttable_get_layout(darktable.view_manager) == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC))
    {
      imgid = dt_view_lighttable_get_culling_selection(darktable.view_manager);
    }
    else
      imgid = GPOINTER_TO_INT(darktable.view_manager->active_images->data);
  }
  else
  {
    sqlite3_stmt *stmt;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
        "SELECT s.imgid"
        " FROM main.selected_images as s, memory.collected_images as c"
        " WHERE s.imgid=c.imgid"
        " ORDER BY c.rowid LIMIT 1",
        -1, &stmt, NULL);
    // clang-format on
    if(stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
    {
      imgid = sqlite3_column_int(stmt, 0);
    }
    if(stmt) sqlite3_finalize(stmt);
  }

  if((darktable.unmuted & DT_DEBUG_ACT_ON) == DT_DEBUG_ACT_ON)
    dt_print(DT_DEBUG_ACT_ON, "[images to act on - selection] single image : %d", imgid);

  return imgid;
}
dt_imgid_t dt_act_on_get_main_image()
{
  if(dt_act_on_get_algorithm() == DT_ACT_ON_HOVER)
    return _get_main_image_hover();
  else
    return _get_main_image_selection();
}

// get only the number of images to act on
int dt_act_on_get_images_nb(const gboolean only_visible,
                            const gboolean force)
{
  // if the cache is valid (whatever the ordering) we return its value
  if(!force)
  {
    dt_act_on_cache_t *cache;
    if(only_visible)
      cache = &darktable.view_manager->act_on_cache_visible;
    else
      cache = &darktable.view_manager->act_on_cache_all;

    if(_test_cache(cache))
      return cache->images_nb;
  }


  // otherwise we update the cache
  _cache_update(only_visible, force, FALSE);

  // and we return the number of images in cache
  if(only_visible && darktable.view_manager->act_on_cache_visible.ok)
    return darktable.view_manager->act_on_cache_visible.images_nb;
  else if(!only_visible && darktable.view_manager->act_on_cache_all.ok)
    return darktable.view_manager->act_on_cache_all.images_nb;
  else
    return 0;
}

// reset the cache
void dt_act_on_reset_cache(const gboolean only_visible)
{
  if(only_visible)
    darktable.view_manager->act_on_cache_visible.ok = FALSE;
  else
    darktable.view_manager->act_on_cache_all.ok = FALSE;
}

// set the right class for the widget
void dt_act_on_set_class(GtkWidget *widget)
{
  if(dt_act_on_get_algorithm() == DT_ACT_ON_SELECTION)
  {
    dt_gui_remove_class(widget, "dt_act_on_hover");
    dt_gui_add_class(widget, "dt_act_on_selection");
  }
  else
  {
    dt_gui_add_class(widget, "dt_act_on_hover");
    dt_gui_remove_class(widget, "dt_act_on_selection");
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
