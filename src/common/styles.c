/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

#include "common/styles.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/file_location.h"
#include "common/history.h"
#include "common/history_snapshot.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/tags.h"
#include "control/control.h"
#include "develop/develop.h"

#include "gui/accelerators.h"
#include "gui/styles.h"
#include <libxml/encoding.h>
#include <libxml/xmlwriter.h>

#include <glib.h>
#include <stdio.h>
#include <string.h>

#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

typedef struct
{
  GString *name;
  GString *description;
  GList *iop_list;
} StyleInfoData;

typedef struct
{
  int num;
  int module;
  GString *operation;
  GString *op_params;
  GString *blendop_params;
  int blendop_version;
  int multi_priority;
  GString *multi_name;
  int enabled;
  double iop_order;
} StylePluginData;

typedef struct
{
  StyleInfoData *info;
  GList *plugins;
  gboolean in_plugin;
} StyleData;

void dt_style_free(gpointer data)
{
  dt_style_t *style = (dt_style_t *)data;
  g_free(style->name);
  g_free(style->description);
  style->name = NULL;
  style->description = NULL;
  g_free(style);
}

void dt_style_item_free(gpointer data)
{
  dt_style_item_t *item = (dt_style_item_t *)data;
  g_free(item->name);
  g_free(item->operation);
  g_free(item->multi_name);
  free(item->params);
  free(item->blendop_params);
  item->name = NULL;
  item->operation = NULL;
  item->multi_name = NULL;
  item->params = NULL;
  item->blendop_params = NULL;
  free(item);
}

static void _apply_style_shortcut_callback(dt_action_t *action)
{
  GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
  dt_styles_apply_to_list(action->label, imgs, FALSE);
  g_list_free(imgs);
}

static int32_t dt_styles_get_id_by_name(const char *name);

gboolean dt_styles_exists(const char *name)
{
  if(name)
    return (dt_styles_get_id_by_name(name)) != 0 ? TRUE : FALSE;
  return FALSE;
}

static void _dt_style_cleanup_multi_instance(int id)
{
  sqlite3_stmt *stmt;
  GList *list = NULL;
  struct _data
  {
    int rowid;
    int mi;
  };
  char last_operation[128] = { 0 };
  int last_mi = 0;

  /* let's clean-up the style multi-instance. What we want to do is have a unique multi_priority value for
     each iop.
     Furthermore this value must start to 0 and increment one by one for each multi-instance of the same
     module. On
     SQLite there is no notion of ROW_NUMBER, so we use rather resource consuming SQL statement, but as a
     style has
     never a huge number of items that's not a real issue. */

  /* 1. read all data for the style and record multi_instance value. */

  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT rowid,operation FROM data.style_items WHERE styleid=?1 ORDER BY operation, multi_priority ASC", -1,
      &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    struct _data *d = malloc(sizeof(struct _data));
    const char *operation = (const char *)sqlite3_column_text(stmt, 1);

    if(strncmp(last_operation, operation, 128) != 0)
    {
      last_mi = 0;
      g_strlcpy(last_operation, operation, sizeof(last_operation));
    }
    else
      last_mi++;

    d->rowid = sqlite3_column_int(stmt, 0);
    d->mi = last_mi;
    list = g_list_prepend(list, d);
  }
  sqlite3_finalize(stmt);
  list = g_list_reverse(list);   // list was built in reverse order, so un-reverse it

  /* 2. now update all multi_instance values previously recorded */

  for(GList *list_iter = list; list_iter; list_iter = g_list_next(list_iter))
  {
    struct _data *d = (struct _data *)list_iter->data;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE data.style_items SET multi_priority=?1 WHERE rowid=?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, d->mi);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, d->rowid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  /* 3. free the list we built in step 1 */
  g_list_free_full(list, free);
}

gboolean dt_styles_has_module_order(const char *name)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT iop_list"
                              " FROM data.styles"
                              " WHERE name=?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  const gboolean has_iop_list = (sqlite3_column_type(stmt, 0) != SQLITE_NULL);
  sqlite3_finalize(stmt);
  return has_iop_list;
}

GList *dt_styles_module_order_list(const char *name)
{
  GList *iop_list = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT iop_list"
                              " FROM data.styles"
                              " WHERE name=?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  if(sqlite3_column_type(stmt, 0) != SQLITE_NULL)
  {
    const char *iop_list_txt = (char *)sqlite3_column_text(stmt, 0);
    iop_list = dt_ioppr_deserialize_text_iop_order_list(iop_list_txt);
  }
  sqlite3_finalize(stmt);
  return iop_list;
}

static gboolean dt_styles_create_style_header(const char *name, const char *description, GList *iop_list)
{
  sqlite3_stmt *stmt;

  if(dt_styles_get_id_by_name(name) != 0)
  {
    dt_control_log(_("Style with name '%s' already exists"), name);
    return FALSE;
  }

  char *iop_list_txt = NULL;

  /* first create the style header */
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "INSERT INTO data.styles (name, description, id, iop_list)"
      " VALUES (?1, ?2, (SELECT COALESCE(MAX(id),0)+1 FROM data.styles), ?3)", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, description, -1, SQLITE_STATIC);
  if(iop_list)
  {
    iop_list_txt = dt_ioppr_serialize_text_iop_order_list(iop_list);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, iop_list_txt, -1, SQLITE_STATIC);
  }
  else
    sqlite3_bind_null(stmt, 3);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  g_free(iop_list_txt);
  return TRUE;
}

static void _dt_style_update_from_image(int id, int imgid, GList *filter, GList *update)
{
  if(update && imgid != -1)
  {
    GList *list = filter;
    GList *upd = update;
    char query[4096] = { 0 };
    char tmp[500];
    char *fields[] = { "op_params",       "module",         "enabled",    "blendop_params",
                       "blendop_version", "multi_priority", "multi_name", 0 };

    do
    {
      query[0] = '\0';

      // included and update set, we then need to update the corresponding style item
      if(GPOINTER_TO_INT(upd->data) != -1 && GPOINTER_TO_INT(list->data) != -1)
      {
        g_strlcpy(query, "UPDATE data.style_items SET ", sizeof(query));

        for(int k = 0; fields[k]; k++)
        {
          if(k != 0) g_strlcat(query, ",", sizeof(query));
          snprintf(tmp, sizeof(tmp),
                   "%s=(SELECT %s FROM main.history WHERE imgid=%d AND num=%d)",
                   fields[k], fields[k], imgid, GPOINTER_TO_INT(upd->data));
          g_strlcat(query, tmp, sizeof(query));
        }
        snprintf(tmp, sizeof(tmp), " WHERE styleid=%d AND data.style_items.num=%d", id,
                 GPOINTER_TO_INT(list->data));
        g_strlcat(query, tmp, sizeof(query));
      }
      // update only, so we want to insert the new style item
      else if(GPOINTER_TO_INT(upd->data) != -1)
        // clang-format off
        snprintf(query, sizeof(query),
                 "INSERT INTO data.style_items "
                 "  (styleid, num, module, operation, op_params, enabled, blendop_params,"
                 "   blendop_version, multi_priority, multi_name)"
                 " SELECT %d,"
                 "    (SELECT num+1 "
                 "     FROM data.style_items"
                 "     WHERE styleid=%d"
                 "     ORDER BY num DESC LIMIT 1), "
                 "   module, operation, op_params, enabled, blendop_params, blendop_version,"
                 "   multi_priority, multi_name"
                 " FROM main.history"
                 " WHERE imgid=%d AND num=%d",
                 id, id, imgid, GPOINTER_TO_INT(upd->data));
        // clang-format on

      if(*query) DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);

      list = g_list_next(list);
      upd = g_list_next(upd);
    } while(list);
  }
}

static void  _dt_style_update_iop_order(const gchar *name, const int id, const int32_t imgid,
                                        const gboolean copy_iop_order, const gboolean update_iop_order)
{
  sqlite3_stmt *stmt;

  GList *iop_list = dt_styles_module_order_list(name);

  // if we update or if the style does not contains an order then the
  // copy must be done using the imgid iop-order.

  if(update_iop_order || iop_list == NULL)
    iop_list = dt_ioppr_get_iop_order_list(imgid, FALSE);

  gchar *iop_list_txt = dt_ioppr_serialize_text_iop_order_list(iop_list);

  if(copy_iop_order || update_iop_order)
  {
    // copy from style name to style id
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE data.styles SET iop_list=?1 WHERE id=?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, iop_list_txt, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, id);
  }
  else
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE data.styles SET iop_list=NULL WHERE id=?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  }

  g_list_free_full(iop_list, free);
  g_free(iop_list_txt);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_styles_update(const char *name, const char *newname, const char *newdescription, GList *filter,
                      const int32_t imgid, GList *update,
                      const gboolean copy_iop_order, const gboolean update_iop_order)
{
  sqlite3_stmt *stmt;

  const int id = dt_styles_get_id_by_name(name);
  if(id == 0) return;

  gchar *desc = dt_styles_get_description(name);

  if((g_strcmp0(name, newname)) || (g_strcmp0(desc, newdescription)))
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE data.styles SET name=?1, description=?2 WHERE id=?3", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, newname, -1, SQLITE_STATIC);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, newdescription, -1, SQLITE_STATIC);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  if(filter)
  {
    char tmp[64];
    char include[2048] = { 0 };
    g_strlcat(include, "num NOT IN (", sizeof(include));
    for(GList *list = filter; list; list = g_list_next(list))
    {
      if(list != filter) g_strlcat(include, ",", sizeof(include));
      snprintf(tmp, sizeof(tmp), "%d", GPOINTER_TO_INT(list->data));
      g_strlcat(include, tmp, sizeof(include));
    }
    g_strlcat(include, ")", sizeof(include));

    char query[4096] = { 0 };
    snprintf(query, sizeof(query), "DELETE FROM data.style_items WHERE styleid=?1 AND %s", include);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  _dt_style_update_from_image(id, imgid, filter, update);

  _dt_style_update_iop_order(name, id, imgid, copy_iop_order, update_iop_order);

  _dt_style_cleanup_multi_instance(id);

  /* backup style to disk */
  dt_styles_save_to_file(newname, NULL, TRUE);

  if(g_strcmp0(name, newname))
  {
    dt_action_t *old = dt_action_locate(&darktable.control->actions_global,
                                        (gchar **)(const gchar *[]){"styles", name, NULL}, FALSE);
    dt_action_rename(old, newname);
  }

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_STYLE_CHANGED);

  g_free(desc);
}

void dt_styles_create_from_style(const char *name, const char *newname, const char *description,
                                 GList *filter, const int32_t imgid, GList *update,
                                 const gboolean copy_iop_order, const gboolean update_iop_order)
{
  sqlite3_stmt *stmt;
  int id = 0;

  const int oldid = dt_styles_get_id_by_name(name);
  if(oldid == 0) return;

  /* create the style header */
  if(!dt_styles_create_style_header(newname, description, NULL)) return;

  if((id = dt_styles_get_id_by_name(newname)) != 0)
  {
    if(filter)
    {
      char tmp[64];
      char include[2048] = { 0 };
      g_strlcat(include, "num IN (", sizeof(include));
      for(GList *list = filter; list; list = g_list_next(list))
      {
        if(list != filter) g_strlcat(include, ",", sizeof(include));
        snprintf(tmp, sizeof(tmp), "%d", GPOINTER_TO_INT(list->data));
        g_strlcat(include, tmp, sizeof(include));
      }
      g_strlcat(include, ")", sizeof(include));
      char query[4096] = { 0 };

      // clang-format off
      snprintf(query, sizeof(query),
               "INSERT INTO data.style_items "
               "  (styleid,num,module,operation,op_params,enabled,blendop_params,blendop_version,"
               "   multi_priority,multi_name)"
               " SELECT ?1, num,module,operation,op_params,enabled,blendop_params,blendop_version,"
               "   multi_priority,multi_name"
               " FROM data.style_items"
               " WHERE styleid=?2 AND %s",
               include);
      // clang-format on
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    }
    else
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "INSERT INTO data.style_items "
                                  "  (styleid,num,module,operation,op_params,enabled,blendop_params,"
                                  "   blendop_version,multi_priority,multi_name)"
                                  " SELECT ?1, num,module,operation,op_params,enabled,blendop_params,"
                                  "        blendop_version,multi_priority,multi_name"
                                  " FROM data.style_items"
                                  " WHERE styleid=?2",
                                  -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, oldid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* insert items from imgid if defined */

    _dt_style_update_from_image(id, imgid, filter, update);

    _dt_style_update_iop_order(name, id, imgid, copy_iop_order, update_iop_order);

    _dt_style_cleanup_multi_instance(id);

    /* backup style to disk */
    dt_styles_save_to_file(newname, NULL, FALSE);

    dt_action_t *stl = dt_action_section(&darktable.control->actions_global, N_("Styles"));
    dt_action_register(stl, newname, _apply_style_shortcut_callback, 0, 0);

    dt_control_log(_("Style named '%s' successfully created"), newname);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_STYLE_CHANGED);
  }
}

gboolean dt_styles_create_from_image(const char *name, const char *description,
                                     const int32_t imgid, GList *filter, gboolean copy_iop_order)
{
  int id = 0;
  sqlite3_stmt *stmt;

  GList *iop_list = NULL;
  if(copy_iop_order)
  {
    iop_list = dt_ioppr_get_iop_order_list(imgid, FALSE);
  }

  /* first create the style header */
  if(!dt_styles_create_style_header(name, description, iop_list)) return FALSE;

  g_list_free_full(iop_list, g_free);

  if((id = dt_styles_get_id_by_name(name)) != 0)
  {
    /* create the style_items from source image history stack */
    if(filter)
    {
      char tmp[64];
      char include[2048] = { 0 };
      g_strlcat(include, "num IN (", sizeof(include));
      for(GList *list = filter; list; list = g_list_next(list))
      {
        if(list != filter) g_strlcat(include, ",", sizeof(include));
        snprintf(tmp, sizeof(tmp), "%d", GPOINTER_TO_INT(list->data));
        g_strlcat(include, tmp, sizeof(include));
      }

      g_strlcat(include, ")", sizeof(include));
      char query[4096] = { 0 };
      // clang-format off
      snprintf(query, sizeof(query),
               "INSERT INTO data.style_items"
               " (styleid,num,module,operation,op_params,enabled,blendop_params,"
               "  blendop_version,multi_priority,multi_name)"
               " SELECT ?1, num,module,operation,op_params,enabled,blendop_params,blendop_version,"
               "  multi_priority,multi_name"
               " FROM main.history"
               " WHERE imgid=?2 AND %s",
               include);
      // clang-format on
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    }
    else
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "INSERT INTO data.style_items"
                                  "  (styleid,num,module,operation,op_params,enabled,blendop_params,"
                                  "   blendop_version,multi_priority,multi_name)"
                                  " SELECT ?1, num,module,operation,op_params,enabled,blendop_params,blendop_version,"
                                  "   multi_priority,multi_name"
                                  " FROM main.history"
                                  " WHERE imgid=?2",
                                  -1, &stmt, NULL);
      // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    _dt_style_cleanup_multi_instance(id);

    /* backup style to disk */
    dt_styles_save_to_file(name, NULL, FALSE);

    dt_action_t *stl = dt_action_section(&darktable.control->actions_global, N_("Styles"));
    dt_action_register(stl, name, _apply_style_shortcut_callback, 0, 0);

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_STYLE_CHANGED);
    return TRUE;
  }
  return FALSE;
}

void dt_styles_apply_to_list(const char *name, const GList *list, gboolean duplicate)
{
  gboolean selected = FALSE;

  /* write current history changes so nothing gets lost,
     do that only in the darkroom as there is nothing to be saved
     when in the lighttable (and it would write over current history stack) */
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_DARKROOM) dt_dev_write_history(darktable.develop);

  const int mode = dt_conf_get_int("plugins/lighttable/style/applymode");
  const gboolean is_overwrite = (mode == DT_STYLE_HISTORY_OVERWRITE);

  /* for each selected image apply style */
  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);

  dt_undo_lt_history_t *hist = NULL;

  for(const GList *l = list; l; l = g_list_next(l))
  {
    const int32_t imgid = GPOINTER_TO_INT(l->data);
    if(is_overwrite)
    {
      hist = dt_history_snapshot_item_init();
      hist->imgid = imgid;
      dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

      dt_undo_disable_next(darktable.undo);
      if(!duplicate) dt_history_delete_on_image_ext(imgid, FALSE);
    }

    dt_styles_apply_to_image(name, duplicate, is_overwrite, imgid);

    if(is_overwrite)
    {
      dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
      dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                     dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);
    }

    selected = TRUE;
  }

  dt_undo_end_group(darktable.undo);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);

  if(!selected)
  {
    dt_control_log(_("No image selected!"));
  }
  else
  {
    dt_control_log(_("Style %s successfully applied!"), name);
  }
}

void dt_multiple_styles_apply_to_list(GList *styles, const GList *list, gboolean duplicate)
{
  /* write current history changes so nothing gets lost,
     do that only in the darkroom as there is nothing to be saved
     when in the lighttable (and it would write over current history stack) */
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM) dt_dev_write_history(darktable.develop);

  if(!styles && !list)
  {
    dt_control_log(_("No images nor styles selected!"));
    return;
  }
  else if(!styles)
  {
    dt_control_log(_("No styles selected!"));
    return;
  }
  else if(!list)
  {
    dt_control_log(_("No image selected!"));
    return;
  }

  const int mode = dt_conf_get_int("plugins/lighttable/style/applymode");
  const gboolean is_overwrite = (mode == DT_STYLE_HISTORY_OVERWRITE);

  /* for each selected image apply style */
  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
  for(const GList *l = list; l; l = g_list_next(l))
  {
    const int32_t imgid = GPOINTER_TO_INT(l->data);
    if(is_overwrite && !duplicate)
      dt_history_delete_on_image_ext(imgid, FALSE);

    for (GList *style = styles; style; style = g_list_next(style))
    {
      dt_styles_apply_to_image((char*)style->data, duplicate, is_overwrite, imgid);
    }
  }
  dt_undo_end_group(darktable.undo);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);

  const guint styles_cnt = g_list_length(styles);
  dt_control_log(ngettext("Style successfully applied!", "Styles successfully applied!", styles_cnt));
}

void dt_styles_create_from_list(const GList *list)
{
  gboolean selected = FALSE;
  /* for each image create style */
  for(const GList *l = list; l; l = g_list_next(l))
  {
    const int imgid = GPOINTER_TO_INT(l->data);
    dt_gui_styles_dialog_new(imgid);
    selected = TRUE;
  }

  if(!selected) dt_control_log(_("No image selected!"));
}

void dt_styles_apply_style_item(dt_develop_t *dev, dt_style_item_t *style_item, GList **modules_used, const gboolean append)
{
  // get any instance of the same operation so we can copy it
  dt_iop_module_t *mod_src = dt_iop_get_module_by_op_priority(dev->iop, style_item->operation, -1);

  if(mod_src)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));

    module->dev = dev;

    if(dt_iop_load_module(module, mod_src->so, dev))
    {
      module = NULL;
      fprintf(stderr, "[dt_styles_apply_style_item] can't load module %s %s\n", style_item->operation,
              style_item->multi_name);
    }
    else
    {
      gboolean do_merge = TRUE;

      module->instance = mod_src->instance;
      module->multi_priority = style_item->multi_priority;
      module->iop_order = style_item->iop_order;

      module->enabled = style_item->enabled;
      g_strlcpy(module->multi_name, style_item->multi_name, sizeof(module->multi_name));

      // TODO: this is copied from dt_dev_read_history_ext(), maybe do a helper with this?
      if(style_item->blendop_params && (style_item->blendop_version == dt_develop_blend_version())
         && (style_item->blendop_params_size == sizeof(dt_develop_blend_params_t)))
      {
        memcpy(module->blend_params, style_item->blendop_params, sizeof(dt_develop_blend_params_t));
      }
      else if(style_item->blendop_params
              && dt_develop_blend_legacy_params(module, style_item->blendop_params, style_item->blendop_version,
                  module->blend_params, dt_develop_blend_version(), style_item->blendop_params_size) == 0)
      {
        // do nothing
      }
      else
      {
        memcpy(module->blend_params, module->default_blendop_params, sizeof(dt_develop_blend_params_t));
      }

      if(module->version() != style_item->module_version || module->params_size != style_item->params_size
         || strcmp(style_item->operation, module->op))
      {
        if(!module->legacy_params
           || module->legacy_params(module, style_item->params, labs(style_item->module_version),
                                          module->params, labs(module->version())))
        {
          fprintf(stderr, "[dt_styles_apply_style_item] module `%s' version mismatch: history is %d, dt %d.\n",
                  module->op, style_item->module_version, module->version());
          dt_control_log(_("Module `%s' version mismatch: %d != %d"), module->op,
                         module->version(), style_item->module_version);

          do_merge = FALSE;
        }
        else
        {
          if(!strcmp(module->op, "spots") && style_item->module_version == 1)
          {
            // FIXME: not sure how to handle this here...
            // quick and dirty hack to handle spot removal legacy_params
            /* memcpy(module->blend_params, module->blend_params, sizeof(dt_develop_blend_params_t));
            memcpy(module->blend_params, module->default_blendop_params,
                   sizeof(dt_develop_blend_params_t)); */
          }
        }

        /*
         * Fix for flip iop: previously it was not always needed, but it might be
         * in history stack as "orientation (off)", but now we always want it
         * by default, so if it is disabled, enable it, and replace params with
         * default_params. if user want to, he can disable it.
         */
        if(!strcmp(module->op, "flip") && module->enabled == 0 && labs(style_item->module_version) == 1)
        {
          memcpy(module->params, module->default_params, module->params_size);
          module->enabled = 1;
        }
      }
      else
      {
        memcpy(module->params, style_item->params, module->params_size);
      }

      if(do_merge)
        dt_history_merge_module_into_history(dev, NULL, module, modules_used, append);
    }

    if(module)
    {
      dt_iop_cleanup_module(module);
      free(module);
    }
  }
}

void dt_styles_apply_to_image(const char *name, const gboolean duplicate, const gboolean overwrite, const int32_t imgid)
{
  int id = 0;
  sqlite3_stmt *stmt;

  if((id = dt_styles_get_id_by_name(name)) != 0)
  {
    int32_t newimgid;
    /* check if we should make a duplicate before applying style */
    if(duplicate)
    {
      newimgid = dt_image_duplicate(imgid);
      if(newimgid != -1)
      {
        if(overwrite)
          dt_history_delete_on_image_ext(newimgid, FALSE);
        else
          dt_history_copy_and_paste_on_image(imgid, newimgid, FALSE, NULL, TRUE, TRUE);
      }
    }
    else
      newimgid = imgid;

    // now deal with the history
    GList *modules_used = NULL;

    dt_develop_t _dev_dest = { 0 };

    dt_develop_t *dev_dest = &_dev_dest;

    dt_dev_init(dev_dest, FALSE);

    dev_dest->iop = dt_iop_load_modules_ext(dev_dest, TRUE);
    dev_dest->image_storage.id = imgid;

    // now let's deal with the iop-order (possibly merging style & target lists)
    GList *iop_list = dt_styles_module_order_list(name);
    if(iop_list)
    {
      // the style has an iop-order, we need to merge the multi-instance from target image
      // get target image iop-order list:
      GList *img_iop_order_list = dt_ioppr_get_iop_order_list(newimgid, FALSE);
      // get multi-instance modules if any:
      GList *mi = dt_ioppr_extract_multi_instances_list(img_iop_order_list);
      // if some where found merge them with the style list
      if(mi) iop_list = dt_ioppr_merge_multi_instance_iop_order_list(iop_list, mi);
      // finally we have the final list for the image
      dt_ioppr_write_iop_order_list(iop_list, newimgid);
      g_list_free_full(iop_list, g_free);
      g_list_free_full(img_iop_order_list, g_free);
    }

    dt_dev_read_history_ext(dev_dest, newimgid, TRUE);

    dt_ioppr_check_iop_order(dev_dest, newimgid, "dt_styles_apply_to_image ");

    dt_dev_pop_history_items_ext(dev_dest, dev_dest->history_end);

    dt_ioppr_check_iop_order(dev_dest, newimgid, "dt_styles_apply_to_image 1");

    if (DT_IOP_ORDER_INFO)
      fprintf(stderr,"\n^^^^^ Apply style on image %i, history size %i",imgid,dev_dest->history_end);

    // go through all entries in style
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT num, module, operation, op_params, enabled,"
                                "  blendop_params, blendop_version, multi_priority, multi_name"
                                " FROM data.style_items WHERE styleid=?1 "
                                " ORDER BY operation, multi_priority",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    GList *si_list = NULL;
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      dt_style_item_t *style_item = (dt_style_item_t *)malloc(sizeof(dt_style_item_t));

      style_item->num = sqlite3_column_int(stmt, 0);
      style_item->selimg_num = 0;
      style_item->enabled = sqlite3_column_int(stmt, 4);
      style_item->multi_priority = sqlite3_column_int(stmt, 7);
      style_item->name = NULL;
      style_item->operation = g_strdup((char *)sqlite3_column_text(stmt, 2));
      style_item->multi_name = g_strdup((char *)sqlite3_column_text(stmt, 8));
      style_item->module_version = sqlite3_column_int(stmt, 1);
      style_item->blendop_version = sqlite3_column_int(stmt, 6);
      style_item->params_size = sqlite3_column_bytes(stmt, 3);
      style_item->params = (void *)malloc(style_item->params_size);
      memcpy(style_item->params, (void *)sqlite3_column_blob(stmt, 3), style_item->params_size);
      style_item->blendop_params_size = sqlite3_column_bytes(stmt, 5);
      style_item->blendop_params = (void *)malloc(style_item->blendop_params_size);
      memcpy(style_item->blendop_params, (void *)sqlite3_column_blob(stmt, 5), style_item->blendop_params_size);
      style_item->iop_order = 0;

      si_list = g_list_prepend(si_list, style_item);
    }
    sqlite3_finalize(stmt);
    si_list = g_list_reverse(si_list);  // list was built in reverse order, so un-reverse it

    dt_ioppr_update_for_style_items(dev_dest, si_list, FALSE);

    for(GList *l = si_list; l; l = g_list_next(l))
    {
      dt_style_item_t *style_item = (dt_style_item_t *)l->data;
      dt_styles_apply_style_item(dev_dest, style_item, &modules_used, FALSE);
    }

    g_list_free_full(si_list, dt_style_item_free);

    if (DT_IOP_ORDER_INFO) fprintf(stderr,"\nvvvvv --> look for written history below\n");

    dt_ioppr_check_iop_order(dev_dest, newimgid, "dt_styles_apply_to_image 2");

    dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();
    hist->imgid = newimgid;
    dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

    // write history and forms to db
    dt_dev_write_history_ext(dev_dest, newimgid);

    dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
    dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
    dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                   dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);
    dt_undo_end_group(darktable.undo);

    dt_dev_cleanup(dev_dest);

    g_list_free(modules_used);

    /* add tag */
    guint tagid = 0;
    gchar ntag[512] = { 0 };
    g_snprintf(ntag, sizeof(ntag), "darktable|style|%s", name);
    if(dt_tag_new(ntag, &tagid)) dt_tag_attach(tagid, newimgid, FALSE, FALSE);
    if(dt_tag_new("darktable|changed", &tagid))
    {
      dt_tag_attach(tagid, newimgid, FALSE, FALSE);
      dt_image_cache_set_change_timestamp(darktable.image_cache, imgid);
    }

    /* if current image in develop reload history */
    if(dt_dev_is_current_image(darktable.develop, newimgid))
    {
      dt_dev_reload_history_items(darktable.develop);
      dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
      dt_dev_modules_update_multishow(darktable.develop);
    }

    /* update xmp file */
    dt_image_synch_xmp(newimgid);

    /* remove old obsolete thumbnails */
    dt_mipmap_cache_remove(darktable.mipmap_cache, newimgid);
    dt_image_update_final_size(newimgid);

    /* update the aspect ratio. recompute only if really needed for performance reasons */
    if(darktable.collection->params.sorts[DT_COLLECTION_SORT_ASPECT_RATIO])
      dt_image_set_aspect_ratio(newimgid, TRUE);
    else
      dt_image_reset_aspect_ratio(newimgid, TRUE);

    /* redraw center view to update visible mipmaps */
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, newimgid);
  }
}

void dt_styles_delete_by_name_adv(const char *name, const gboolean raise)
{
  int id = 0;
  if((id = dt_styles_get_id_by_name(name)) != 0)
  {
    /* delete the style */
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM data.styles WHERE id = ?1", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* delete style_items belonging to style */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM data.style_items WHERE styleid = ?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    dt_action_t *old = dt_action_locate(&darktable.control->actions_global,
                                        (gchar **)(const gchar *[]){"styles", name, NULL}, FALSE);
    if(old) dt_action_rename(old, NULL);

    if(raise)
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_STYLE_CHANGED);
  }
}

void dt_styles_delete_by_name(const char *name)
{
  dt_styles_delete_by_name_adv(name, TRUE);
}

GList *dt_styles_get_item_list(const char *name, gboolean params, int imgid)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;
  int id = 0;
  if((id = dt_styles_get_id_by_name(name)) != 0)
  {
    if(params)
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT num, multi_priority, module, operation, enabled, op_params, blendop_params, "
                                  "       multi_name, blendop_version"
                                  " FROM data.style_items"
                                  " WHERE styleid=?1 ORDER BY num DESC",
                                  -1, &stmt, NULL);
      // clang-format on
    else if(imgid != -1)
    {
      // get all items from the style
      //    UNION
      // get all items from history, not in the style : select only the last operation, that is max(num)
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(
          dt_database_get(darktable.db),
          "SELECT num, multi_priority, module, operation, enabled,"
          "       (SELECT MAX(num)"
          "        FROM main.history"
          "        WHERE imgid=?2 "
          "          AND operation=data.style_items.operation"
          "          AND multi_priority=data.style_items.multi_priority),"
          "       0, multi_name, blendop_version"
          " FROM data.style_items"
          " WHERE styleid=?1"
          " UNION"
          " SELECT -1,main.history.multi_priority,main.history.module,main.history.operation,main.history.enabled, "
          "        main.history.num,0,multi_name, blendop_version"
          " FROM main.history"
          " WHERE imgid=?2 AND main.history.enabled=1"
          "   AND (main.history.operation NOT IN (SELECT operation FROM data.style_items WHERE styleid=?1))"
          " GROUP BY operation HAVING MAX(num) ORDER BY num DESC", -1, &stmt, NULL);
        // clang-format on
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    }
    else
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT num, multi_priority, module, operation, enabled, 0, 0, multi_name"
                                  " FROM data.style_items"
                                  " WHERE styleid=?1 ORDER BY num DESC",
                                  -1, &stmt, NULL);
      // clang-format on

    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      if(strcmp((const char*)sqlite3_column_text(stmt, 3), "mask_manager") == 0) continue;

      // name of current item of style
      char iname[512] = { 0 };
      dt_style_item_t *item = calloc(1, sizeof(dt_style_item_t));

      if(sqlite3_column_type(stmt, 0) == SQLITE_NULL)
        item->num = -1;
      else
        item->num = sqlite3_column_int(stmt, 0);

      item->multi_priority = sqlite3_column_int(stmt, 1);

      item->selimg_num = -1;
      item->module_version = sqlite3_column_int(stmt, 2);

      item->enabled = sqlite3_column_int(stmt, 4);

      const char *multi_name = (const char *)sqlite3_column_text(stmt, 7);
      const gboolean has_multi_name = multi_name && *multi_name && (strcmp(multi_name, "0") != 0);

      if(params)
      {
        // when we get the parameters we do not want to get the operation localized as this
        // is used to compare against the internal module name.

        if(has_multi_name)
          g_snprintf(iname, sizeof(iname), "%s %s", sqlite3_column_text(stmt, 3), multi_name);
        else
          g_snprintf(iname, sizeof(iname), "%s", sqlite3_column_text(stmt, 3));

        const unsigned char *op_blob = sqlite3_column_blob(stmt, 5);
        const int32_t op_len = sqlite3_column_bytes(stmt, 5);
        const unsigned char *bop_blob = sqlite3_column_blob(stmt, 6);
        const int32_t bop_len = sqlite3_column_bytes(stmt, 6);
        const int32_t bop_ver = sqlite3_column_int(stmt, 8);

        item->params = malloc(op_len);
        item->params_size = op_len;
        memcpy(item->params, op_blob, op_len);

        item->blendop_params = malloc(bop_len);
        item->blendop_params_size = bop_len;
        item->blendop_version = bop_ver;
        memcpy(item->blendop_params, bop_blob, bop_len);
      }
      else
      {
        const gchar *itname = dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 3));

        if(has_multi_name)
          g_snprintf(iname, sizeof(iname), "%s %s", itname, multi_name);
        else
          g_snprintf(iname, sizeof(iname), "%s", itname);

        item->params = NULL;
        item->blendop_params = NULL;
        item->params_size = 0;
        item->blendop_params_size = 0;
        item->blendop_version = 0;
        if(imgid != -1 && sqlite3_column_type(stmt, 5) != SQLITE_NULL)
          item->selimg_num = sqlite3_column_int(stmt, 5);
      }
      item->name = g_strdup(iname);
      item->operation = g_strdup((char *)sqlite3_column_text(stmt, 3));
      item->multi_name = g_strdup((char *)sqlite3_column_text(stmt, 7));
      item->iop_order = sqlite3_column_double(stmt, 8);
      result = g_list_prepend(result, item);
    }
    sqlite3_finalize(stmt);
  }
  return g_list_reverse(result);   // list was built in reverse order, so un-reverse it
}

char *dt_styles_get_item_list_as_string(const char *name)
{
  GList *items = dt_styles_get_item_list(name, FALSE, -1);
  if(items == NULL) return NULL;

  GList *names = NULL;
  for(GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    dt_style_item_t *item = (dt_style_item_t *)items_iter->data;
    names = g_list_prepend(names, g_strdup(item->name));
  }
  names = g_list_reverse(names);  // list was built in reverse order, so un-reverse it

  char *result = dt_util_glist_to_str("\n", names);
  g_list_free_full(names, g_free);
  g_list_free_full(items, dt_style_item_free);
  return result;
}

GList *dt_styles_get_list(const char *filter)
{
  char filterstring[512] = { 0 };
  sqlite3_stmt *stmt;
  snprintf(filterstring, sizeof(filterstring), "%%%s%%", filter);
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT name, description FROM data.styles WHERE name LIKE ?1 OR description LIKE ?1 ORDER BY name", -1,
      &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, filterstring, -1, SQLITE_TRANSIENT);
  GList *result = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    const char *description = (const char *)sqlite3_column_text(stmt, 1);
    dt_style_t *s = g_malloc(sizeof(dt_style_t));
    s->name = g_strdup(name);
    s->description = g_strdup(description);
    result = g_list_prepend(result, s);
  }
  sqlite3_finalize(stmt);
  return g_list_reverse(result);  // list was built in reverse order, so un-reverse it
}

static char *dt_style_encode(sqlite3_stmt *stmt, int row)
{
  const int32_t len = sqlite3_column_bytes(stmt, row);
  char *vparams = dt_exif_xmp_encode((const unsigned char *)sqlite3_column_blob(stmt, row), len, NULL);
  return vparams;
}

void dt_styles_save_to_file(const char *style_name, const char *filedir, gboolean overwrite)
{
  char stylesdir[PATH_MAX] = { 0 };
  if(!filedir)
  {
    dt_loc_get_user_config_dir(stylesdir, sizeof(stylesdir));
    g_strlcat(stylesdir, "/styles", sizeof(stylesdir));
    g_mkdir_with_parents(stylesdir, 00755);
    filedir = stylesdir;
  }

  int rc = 0;
  char stylename[520];
  sqlite3_stmt *stmt;

  // generate filename based on name of style
  // convert all characters to underscore which are not allowed in filenames
  char *filename = g_strdup(style_name);
  snprintf(stylename, sizeof(stylename), "%s/%s.dtstyle", filedir, g_strdelimit(filename, "/<>:\"\\|*?[]", '_'));
  g_free(filename);

  // check if file exists
  if(g_file_test(stylename, G_FILE_TEST_EXISTS) == TRUE)
  {
    if(overwrite)
    {
      if(g_unlink(stylename))
      {
        dt_control_log(_("Failed to overwrite style file for %s"), style_name);
        return;
      }
    }
    else
    {
      dt_control_log(_("Style file for %s exists"), style_name);
      return;
    }
  }

  if(!dt_styles_exists(style_name)) return;

  xmlTextWriterPtr writer = xmlNewTextWriterFilename(stylename, 0);
  if(writer == NULL)
  {
    fprintf(stderr, "[dt_styles_save_to_file] Error creating the xml writer\n, path: %s", stylename);
    return;
  }
  rc = xmlTextWriterStartDocument(writer, NULL, "UTF-8", NULL);
  if(rc < 0)
  {
    fprintf(stderr, "[dt_styles_save_to_file]: Error on encoding setting");
    return;
  }
  xmlTextWriterStartElement(writer, BAD_CAST "darktable_style");
  xmlTextWriterWriteAttribute(writer, BAD_CAST "version", BAD_CAST "1.0");

  xmlTextWriterStartElement(writer, BAD_CAST "info");
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "name", "%s", style_name);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "description", "%s", dt_styles_get_description(style_name));
  GList *iop_list = dt_styles_module_order_list(style_name);
  if(iop_list)
  {
    char *iop_list_text = dt_ioppr_serialize_text_iop_order_list(iop_list);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "iop_list", "%s", iop_list_text);
    g_free(iop_list_text);
    g_list_free_full(iop_list, g_free);
  }
  xmlTextWriterEndElement(writer);

  xmlTextWriterStartElement(writer, BAD_CAST "style");
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT num, module, operation, op_params, enabled,"
                              "  blendop_params, blendop_version, multi_priority, multi_name"
                              " FROM data.style_items"
                              " WHERE styleid =?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dt_styles_get_id_by_name(style_name));
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    xmlTextWriterStartElement(writer, BAD_CAST "plugin");
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "num", "%d", sqlite3_column_int(stmt, 0));
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "module", "%d", sqlite3_column_int(stmt, 1));
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "operation", "%s", sqlite3_column_text(stmt, 2));
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "op_params", "%s", dt_style_encode(stmt, 3));
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "enabled", "%d", sqlite3_column_int(stmt, 4));
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "blendop_params", "%s", dt_style_encode(stmt, 5));
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "blendop_version", "%d", sqlite3_column_int(stmt, 6));
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "multi_priority", "%d", sqlite3_column_int(stmt, 7));
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "multi_name", "%s", sqlite3_column_text(stmt, 8));
    xmlTextWriterEndElement(writer);
  }
  sqlite3_finalize(stmt);
  xmlTextWriterEndDocument(writer);
  xmlFreeTextWriter(writer);
}

static StyleData *dt_styles_style_data_new()
{
  StyleInfoData *info = g_new0(StyleInfoData, 1);
  info->name = g_string_new("");
  info->description = g_string_new("");

  StyleData *data = g_new0(StyleData, 1);
  data->info = info;
  data->in_plugin = FALSE;
  data->plugins = NULL;

  return data;
}

static StylePluginData *dt_styles_style_plugin_new()
{
  StylePluginData *plugin = g_new0(StylePluginData, 1);
  plugin->operation = g_string_new("");
  plugin->op_params = g_string_new("");
  plugin->blendop_params = g_string_new("");
  plugin->multi_name = g_string_new("");
  plugin->iop_order = -1.0;
  return plugin;
}

static void dt_styles_style_data_free(StyleData *style, gboolean free_segments)
{
  g_string_free(style->info->name, free_segments);
  g_string_free(style->info->description, free_segments);
  g_list_free_full(style->info->iop_list, g_free);
  g_list_free(style->plugins);
  g_free(style);
}

static void dt_styles_start_tag_handler(GMarkupParseContext *context, const gchar *element_name,
                                        const gchar **attribute_names, const gchar **attribute_values,
                                        gpointer user_data, GError **error)
{
  StyleData *style = user_data;
  const gchar *elt = g_markup_parse_context_get_element(context);

  // We need to append the contents of any subtags to the content field
  // for this we need to know when we are inside the note-content tag
  if(g_ascii_strcasecmp(elt, "plugin") == 0)
  {
    style->in_plugin = TRUE;
    style->plugins = g_list_prepend(style->plugins, dt_styles_style_plugin_new());
  }
}

static void dt_styles_end_tag_handler(GMarkupParseContext *context, const gchar *element_name,
                                      gpointer user_data, GError **error)
{
  StyleData *style = user_data;
  const gchar *elt = g_markup_parse_context_get_element(context);

  // We need to append the contents of any subtags to the content field
  // for this we need to know when we are inside the note-content tag
  if(g_ascii_strcasecmp(elt, "plugin") == 0)
  {
    style->in_plugin = FALSE;
  }
}

static void dt_styles_style_text_handler(GMarkupParseContext *context, const gchar *text, gsize text_len,
                                         gpointer user_data, GError **error)
{
  StyleData *style = user_data;
  const gchar *elt = g_markup_parse_context_get_element(context);

  if(g_ascii_strcasecmp(elt, "name") == 0)
  {
    g_string_append_len(style->info->name, text, text_len);
  }
  else if(g_ascii_strcasecmp(elt, "description") == 0)
  {
    g_string_append_len(style->info->description, text, text_len);
  }
  else if(g_ascii_strcasecmp(elt, "iop_list") == 0)
  {
    style->info->iop_list = dt_ioppr_deserialize_text_iop_order_list(text);
  }
  else if(style->in_plugin)
  {
    StylePluginData *plug = style->plugins->data;
    if(g_ascii_strcasecmp(elt, "operation") == 0)
    {
      g_string_append_len(plug->operation, text, text_len);
    }
    else if(g_ascii_strcasecmp(elt, "op_params") == 0)
    {
      g_string_append_len(plug->op_params, text, text_len);
    }
    else if(g_ascii_strcasecmp(elt, "blendop_params") == 0)
    {
      g_string_append_len(plug->blendop_params, text, text_len);
    }
    else if(g_ascii_strcasecmp(elt, "blendop_version") == 0)
    {
      plug->blendop_version = atoi(text);
    }
    else if(g_ascii_strcasecmp(elt, "multi_priority") == 0)
    {
      plug->multi_priority = atoi(text);
    }
    else if(g_ascii_strcasecmp(elt, "multi_name") == 0)
    {
      g_string_append_len(plug->multi_name, text, text_len);
    }
    else if(g_ascii_strcasecmp(elt, "num") == 0)
    {
      plug->num = atoi(text);
    }
    else if(g_ascii_strcasecmp(elt, "module") == 0)
    {
      plug->module = atoi(text);
    }
    else if(g_ascii_strcasecmp(elt, "enabled") == 0)
    {
      plug->enabled = atoi(text);
    }
    else if(g_ascii_strcasecmp(elt, "iop_order") == 0)
    {
      plug->iop_order = atof(text);
    }
  }
}

static GMarkupParser dt_style_parser = {
  dt_styles_start_tag_handler,  // Start element handler
  dt_styles_end_tag_handler,    // End element handler
  dt_styles_style_text_handler, // Text element handler
  NULL,                         // Passthrough handler
  NULL                          // Error handler
};

static void dt_style_plugin_save(StylePluginData *plugin, gpointer styleId)
{
  int id = GPOINTER_TO_INT(styleId);
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO data.style_items "
                              " (styleid, num, module, operation, op_params, enabled, blendop_params,"
                              "  blendop_version, multi_priority, multi_name)"
                              " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, plugin->num);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, plugin->module);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, plugin->operation->str, plugin->operation->len, SQLITE_TRANSIENT);
  //
  const char *param_c = plugin->op_params->str;
  const int param_c_len = strlen(param_c);
  int params_len = 0;
  unsigned char *params = dt_exif_xmp_decode(param_c, param_c_len, &params_len);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, params, params_len, SQLITE_TRANSIENT);
  //
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, plugin->enabled);

  /* decode and store blendop params */
  int blendop_params_len = 0;
  unsigned char *blendop_params = dt_exif_xmp_decode(
      plugin->blendop_params->str, strlen(plugin->blendop_params->str), &blendop_params_len);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 7, blendop_params, blendop_params_len, SQLITE_TRANSIENT);

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 8, plugin->blendop_version);

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 9, plugin->multi_priority);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 10, plugin->multi_name->str, plugin->multi_name->len, SQLITE_TRANSIENT);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  free(params);
}

static void dt_style_save(StyleData *style)
{
  int id = 0;
  if(style == NULL) return;

  /* first create the style header */
  if(!dt_styles_create_style_header(style->info->name->str, style->info->description->str, style->info->iop_list)) return;

  if((id = dt_styles_get_id_by_name(style->info->name->str)) != 0)
  {
    g_list_foreach(style->plugins, (GFunc)dt_style_plugin_save, GINT_TO_POINTER(id));
    dt_control_log(_("Style %s was successfully imported"), style->info->name->str);
  }
}

void dt_styles_import_from_file(const char *style_path)
{
  FILE *style_file;
  StyleData *style;
  GMarkupParseContext *parser;
  gchar buf[1024];

  style = dt_styles_style_data_new();
  parser = g_markup_parse_context_new(&dt_style_parser, 0, style, NULL);

  if((style_file = g_fopen(style_path, "r")))
  {

    while(!feof(style_file))
    {
      const size_t num_read = fread(buf, sizeof(gchar), sizeof(buf), style_file);

      if(num_read == 0)
      {
        break;
      }
      else if(num_read == -1)
      {
        // FIXME: ferror?
        // ERROR !
        break;
      }

      if(!g_markup_parse_context_parse(parser, buf, num_read, NULL))
      {
        g_markup_parse_context_free(parser);
        dt_styles_style_data_free(style, TRUE);
        fclose(style_file);
        return;
      }
    }
  }
  else
  {
    // Failed to open file, clean up.
    dt_control_log(_("Could not read file `%s'"), style_path);
    g_markup_parse_context_free(parser);
    dt_styles_style_data_free(style, TRUE);
    return;
  }

  if(!g_markup_parse_context_end_parse(parser, NULL))
  {
    g_markup_parse_context_free(parser);
    dt_styles_style_data_free(style, TRUE);
    fclose(style_file);
    return;
  }
  g_markup_parse_context_free(parser);
  // save data
  dt_style_save(style);
  //
  dt_styles_style_data_free(style, TRUE);
  fclose(style_file);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_STYLE_CHANGED);
}

gchar *dt_styles_get_description(const char *name)
{
  sqlite3_stmt *stmt;
  int id = 0;
  gchar *description = NULL;
  if((id = dt_styles_get_id_by_name(name)) != 0)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT description FROM data.styles WHERE id=?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    sqlite3_step(stmt);
    description = (char *)sqlite3_column_text(stmt, 0);
    if(description) description = g_strdup(description);
    sqlite3_finalize(stmt);
  }
  return description;
}

static int32_t dt_styles_get_id_by_name(const char *name)
{
  int id = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM data.styles WHERE name=?1 ORDER BY id DESC LIMIT 1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return id;
}

void dt_init_styles_actions()
{
  GList *result = dt_styles_get_list("");
  if(result)
  {
    dt_action_t *stl = dt_action_section(&darktable.control->actions_global, N_("Styles"));
    for(GList *res_iter = result; res_iter; res_iter = g_list_next(res_iter))
    {
      dt_style_t *style = (dt_style_t *)res_iter->data;
      dt_action_register(stl, style->name, _apply_style_shortcut_callback, 0, 0);
    }
    g_list_free_full(result, dt_style_free);
  }
}

dt_style_t *dt_styles_get_by_name(const char *name)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, description FROM data.styles WHERE name = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *style_name = (const char *)sqlite3_column_text(stmt, 0);
    const char *description = (const char *)sqlite3_column_text(stmt, 1);
    dt_style_t *s = g_malloc(sizeof(dt_style_t));
    s->name = g_strdup(style_name);
    s->description = g_strdup(description);
    sqlite3_finalize(stmt);
    return s;
  }
  else
  {

    sqlite3_finalize(stmt);
    return NULL;
  }
}

#undef DT_IOP_ORDER_INFO
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
