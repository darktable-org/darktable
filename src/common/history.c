/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson,
    copyright (c) 2011-2012 johannes hanika

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
#include "develop/develop.h"
#include "control/control.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/history.h"
#include "common/imageio.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/tags.h"
#include "common/utility.h"

static void remove_preset_flag(const int imgid)
{
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  // clear flag
  image->flags &= ~DT_IMAGE_AUTO_PRESETS_APPLIED;

  // write through to sql+xmp
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
}

static void _dt_history_cleanup_multi_instance(int imgid, int minnum)
{
  /* as we let the user decide which history item to copy, we can end with some gaps in multi-instance
     numbering.
     for ex., if user decide to not copy the 2nd instance of a module which as 3 instances.
     let's clean-up the history multi-instance. What we want to do is have a unique multi_priority value for
     each iop.
     Furthermore this value must start to 0 and increment one by one for each multi-instance of the same
     module. On
     SQLite there is no notion of ROW_NUMBER, so we use rather resource consuming SQL statement, but as an
     history has
     never a huge number of items that's not a real issue.

     We only do this for the given imgid and only for num>minnum, that is we only handle new history items
     just copied.
  */
  typedef struct _history_item_t
  {
    int num;
    char op[1024];
    int mi;
    int new_mi;
  } _history_item_t;

  // we first reload all the newly added history item
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select num, operation, multi_priority from "
                                                             "history where imgid=?1 and num>=?2 order by "
                                                             "operation, multi_priority",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, minnum);
  GList *hitems = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    _history_item_t *hi = (_history_item_t *)calloc(1, sizeof(_history_item_t));
    hi->num = sqlite3_column_int(stmt, 0);
    snprintf(hi->op, sizeof(hi->op), "%s", sqlite3_column_text(stmt, 1));
    hi->mi = sqlite3_column_int(stmt, 2);
    hi->new_mi = -5; // means : not changed atm
    hitems = g_list_append(hitems, hi);
  }
  sqlite3_finalize(stmt);

  // then we change the multi-priority to be sure to have a correct numbering
  char op[1024] = "";
  int c_mi = 0;
  int nb_change = 0;
  GList *items = g_list_first(hitems);
  while(items)
  {
    _history_item_t *hi = (_history_item_t *)(items->data);
    if(strcmp(op, hi->op) != 0)
    {
      strncpy(op, hi->op, sizeof(op));
      c_mi = 0;
    }
    if(hi->mi != c_mi) nb_change++;
    hi->new_mi = c_mi;
    c_mi++;
    items = g_list_next(items);
  }

  if(nb_change == 0)
  {
    // everything is ok, nothing to change
    g_list_free_full(hitems, free);
    return;
  }

  // and we update the history items
  char *req = NULL;
  req = dt_util_dstrcat(req, "%s", "update history set multi_priority = case num ");
  items = g_list_first(hitems);
  while(items)
  {
    _history_item_t *hi = (_history_item_t *)(items->data);
    if(hi->mi != hi->new_mi)
    {
      req = dt_util_dstrcat(req, "when %d then %d ", hi->num, hi->new_mi);
    }
    items = g_list_next(items);
  }
  req = dt_util_dstrcat(req, "%s", "else multi_priority end where imgid=?1 and num>=?2");
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), req, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, minnum);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  g_free(req);
  g_list_free_full(hitems, free);
}

void dt_history_delete_on_image(int32_t imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from history where imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from mask where imgid = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  remove_preset_flag(imgid);

  /* if current image in develop reload history */
  if(dt_dev_is_current_image(darktable.develop, imgid)) dt_dev_reload_history_items(darktable.develop);

  /* make sure mipmaps are recomputed */
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);

  /* remove darktable|style|* tags */
  dt_tag_detach_by_string("darktable|style%", imgid);
}

void dt_history_delete_on_selection()
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    dt_history_delete_on_image(imgid);
  }
  sqlite3_finalize(stmt);
}

int dt_history_load_and_apply(int imgid, gchar *filename, int history_only)
{
  int res = 0;
  dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(img)
  {
    if(dt_exif_xmp_read(img, filename, history_only)) return 1;

    /* if current image in develop reload history */
    if(dt_dev_is_current_image(darktable.develop, imgid)) dt_dev_reload_history_items(darktable.develop);

    dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);
    dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
  }
  return res;
}

int dt_history_load_and_apply_on_selection(gchar *filename)
{
  int res = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    if(dt_history_load_and_apply(imgid, filename, 1)) res = 1;
  }
  sqlite3_finalize(stmt);
  return res;
}

int dt_history_copy_and_paste_on_image(int32_t imgid, int32_t dest_imgid, gboolean merge, GList *ops)
{
  sqlite3_stmt *stmt;
  if(imgid == dest_imgid) return 1;

  if(imgid == -1)
  {
    dt_control_log(_("you need to copy history from an image before you paste it onto another"));
    return 1;
  }

  // be sure the current history is written before pasting some other history data
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM) dt_dev_write_history(darktable.develop);

  /* if merge onto history stack, lets find history offest in destination image */
  int32_t offs = 0;
  if(merge)
  {
    /* apply on top of history stack */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT MAX(num)+1 FROM history WHERE imgid = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW) offs = sqlite3_column_int(stmt, 0);
  }
  else
  {
    /* replace history stack */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from history where imgid = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);

  //  prepare SQL request
  char req[2048];
  g_strlcpy(req, "insert into history (imgid, num, module, operation, op_params, enabled, blendop_params, "
                 "blendop_version, multi_name, multi_priority) select ?1, num+?2, module, operation, "
                 "op_params, enabled, blendop_params, blendop_version, multi_name, multi_priority from "
                 "history where imgid = ?3",
            sizeof(req));

  //  Add ops selection if any format: ... and num in (val1, val2)
  if(ops)
  {
    GList *l = ops;
    int first = 1;
    g_strlcat(req, " and num in (", sizeof(req));

    while(l)
    {
      unsigned int value = GPOINTER_TO_UINT(l->data);
      char v[30];

      if(!first) g_strlcat(req, ",", sizeof(req));
      snprintf(v, sizeof(v), "%u", value);
      g_strlcat(req, v, sizeof(req));
      first = 0;
      l = g_list_next(l);
    }
    g_strlcat(req, ")", sizeof(req));
  }

  /* add the history items to stack offest */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), req, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, offs);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if(merge && ops) _dt_history_cleanup_multi_instance(dest_imgid, offs);

  // we have to copy masks too
  // what to do with existing masks ?
  if(merge)
  {
    // there's very little chance that we will have same shapes id.
    // but we may want to handle this case anyway
    // and it's not trivial at all !
  }
  else
  {
    // let's remove all existing shapes
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from mask where imgid = ?1", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  // let's copy now
  g_strlcpy(req, "insert into mask (imgid, formid, form, name, version, points, points_count, source) select "
                 "?1, formid, form, name, version, points, points_count, source from mask where imgid = ?2",
            sizeof(req));
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), req, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  /* if current image in develop reload history */
  if(dt_dev_is_current_image(darktable.develop, dest_imgid))
  {
    dt_dev_reload_history_items(darktable.develop);
    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
  }

  /* update xmp file */
  dt_image_synch_xmp(dest_imgid);

  dt_mipmap_cache_remove(darktable.mipmap_cache, dest_imgid);

  return 0;
}

GList *dt_history_get_items(int32_t imgid, gboolean enabled)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select num, operation, enabled, multi_name from history where imgid=?1 and "
                              "num in (select MAX(num) from history hst2 where hst2.imgid=?1 and "
                              "hst2.operation=history.operation group by multi_priority) order by num desc",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char name[512] = { 0 };
    const int is_active = sqlite3_column_int(stmt, 2);

    if(enabled == FALSE || is_active)
    {
      dt_history_item_t *item = g_malloc(sizeof(dt_history_item_t));
      item->num = sqlite3_column_int(stmt, 0);
      char *mname = NULL;
      mname = g_strdup((gchar *)sqlite3_column_text(stmt, 3));
      if(enabled)
      {
        if(strcmp(mname, "0") == 0)
          g_snprintf(name, sizeof(name), "%s",
                     dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 1)));
        else
          g_snprintf(name, sizeof(name), "%s %s",
                     dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 1)),
                     (char *)sqlite3_column_text(stmt, 3));
      }
      else
      {
        if(strcmp(mname, "0") == 0)
          g_snprintf(name, sizeof(name), "%s (%s)",
                     dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 1)),
                     (is_active != 0) ? _("on") : _("off"));
        g_snprintf(name, sizeof(name), "%s %s (%s)",
                   dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 1)),
                   (char *)sqlite3_column_text(stmt, 3), (is_active != 0) ? _("on") : _("off"));
      }
      item->name = g_strdup(name);
      item->op = g_strdup((gchar *)sqlite3_column_text(stmt, 1));
      result = g_list_append(result, item);

      g_free(mname);
    }
  }
  return result;
}

char *dt_history_get_items_as_string(int32_t imgid)
{
  GList *items = NULL;
  const char *onoff[2] = { _("off"), _("on") };
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "select operation, enabled, multi_name from history where imgid=?1 order by num desc", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  // collect all the entries in the history from the db
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *name = NULL, *multi_name = NULL;
    const char *mn = (char *)sqlite3_column_text(stmt, 2);
    if(mn && *mn && g_strcmp0(mn, " ") != 0 && g_strcmp0(mn, "0") != 0)
      multi_name = g_strconcat(" ", sqlite3_column_text(stmt, 2), NULL);
    name = g_strconcat(dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 0)),
                       multi_name ? multi_name : "", " (",
                       (sqlite3_column_int(stmt, 1) == 0) ? onoff[0] : onoff[1], ")", NULL);
    items = g_list_append(items, name);
    g_free(multi_name);
  }
  char *result = dt_util_glist_to_str("\n", items);
  g_list_free_full(items, g_free);
  return result;
}

int dt_history_copy_and_paste_on_selection(int32_t imgid, gboolean merge, GList *ops)
{
  if(imgid < 0) return 1;

  int res = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select * from selected_images where imgid != ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    do
    {
      /* get imgid of selected image */
      int32_t dest_imgid = sqlite3_column_int(stmt, 0);

      /* paste history stack onto image id */
      dt_history_copy_and_paste_on_image(imgid, dest_imgid, merge, ops);

    } while(sqlite3_step(stmt) == SQLITE_ROW);
  }
  else
    res = 1;

  sqlite3_finalize(stmt);
  return res;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
