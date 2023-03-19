/*
    This file is part of darktable,
    Copyright (C) 2014-2023 darktable developers.

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

#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#include "common/darktable.h"
#include "common/database.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/image.h"
#include "control/conf.h"
#include "control/control.h"
#include "crawler.h"
#include "gui/gtk.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif


typedef enum dt_control_crawler_cols_t
{
  DT_CONTROL_CRAWLER_COL_ID = 0,
  DT_CONTROL_CRAWLER_COL_IMAGE_PATH,
  DT_CONTROL_CRAWLER_COL_XMP_PATH,
  DT_CONTROL_CRAWLER_COL_TS_XMP,
  DT_CONTROL_CRAWLER_COL_TS_DB,
  DT_CONTROL_CRAWLER_COL_TS_XMP_INT, // new timestamp to db
  DT_CONTROL_CRAWLER_COL_TS_DB_INT,
  DT_CONTROL_CRAWLER_COL_REPORT,
  DT_CONTROL_CRAWLER_COL_TIME_DELTA,
  DT_CONTROL_CRAWLER_NUM_COLS
} dt_control_crawler_cols_t;

typedef struct dt_control_crawler_result_t
{
  int id;
  time_t timestamp_xmp;
  time_t timestamp_db;
  char *image_path, *xmp_path;
} dt_control_crawler_result_t;

static void _free_crawler_result(dt_control_crawler_result_t *entry)
{
  g_free(entry->image_path);
  g_free(entry->xmp_path);
  entry->image_path = entry->xmp_path = NULL;
}

static void _set_modification_time(char *filename,
                                   const time_t timestamp)
{
  GFile *gfile = g_file_new_for_path(filename);

  GFileInfo *info = g_file_query_info(
    gfile,
    G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
    G_FILE_QUERY_INFO_NONE,
    NULL,
    NULL);

  // For reference, we could use the following lines but for some
  // reasons there is a deprecated message raised even though this
  // routine is not marked as deprecated in the documentation.
  //
  // GDateTime *datetime = g_date_time_new_from_unix_local(timestamp);
  // g_file_info_set_modification_date_time(info, datetime);

  if(info)
  {
    g_file_info_set_attribute_uint64
      (info,
       G_FILE_ATTRIBUTE_TIME_MODIFIED,
       timestamp);

    g_file_set_attributes_from_info(
      gfile,
      info,
      G_FILE_QUERY_INFO_NONE,
      NULL,
      NULL);
  }

  g_object_unref(gfile);
  if(info) g_clear_object(&info);
}

GList *dt_control_crawler_run(void)
{
  sqlite3_stmt *stmt, *inner_stmt;
  GList *result = NULL;
  gboolean look_for_xmp = (dt_image_get_xmp_mode() != DT_WRITE_XMP_NEVER);

  // clang-format off
  sqlite3_prepare_v2(dt_database_get(darktable.db),
                     "SELECT i.id, write_timestamp, version,"
                     "       folder || '" G_DIR_SEPARATOR_S "' || filename, flags"
                     " FROM main.images i, main.film_rolls f"
                     " ON i.film_id = f.id"
                     " ORDER BY f.id, filename",
                     -1, &stmt, NULL);
  // clang-format on
  sqlite3_prepare_v2(dt_database_get(darktable.db),
                     "UPDATE main.images SET flags = ?1 WHERE id = ?2", -1,
                     &inner_stmt, NULL);

  // let's wrap this into a transaction, it might make it a little faster.
  dt_database_start_transaction(darktable.db);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    const time_t timestamp = sqlite3_column_int(stmt, 1);
    const int version = sqlite3_column_int(stmt, 2);
    const gchar *image_path = (char *)sqlite3_column_text(stmt, 3);
    int flags = sqlite3_column_int(stmt, 4);

    // if the image is missing we ignore it.
    if(!g_file_test(image_path, G_FILE_TEST_EXISTS))
    {
      dt_print(DT_DEBUG_CONTROL, "[crawler] `%s' (id: %d) is missing.\n", image_path, id);
      continue;
    }

    // no need to look for xmp files if none get written anyway.
    if(look_for_xmp)
    {
      // construct the xmp filename for this image
      gchar xmp_path[PATH_MAX] = { 0 };
      g_strlcpy(xmp_path, image_path, sizeof(xmp_path));
      dt_image_path_append_version_no_db(version, xmp_path, sizeof(xmp_path));
      size_t len = strlen(xmp_path);
      if(len + 4 >= PATH_MAX) continue;
      xmp_path[len++] = '.';
      xmp_path[len++] = 'x';
      xmp_path[len++] = 'm';
      xmp_path[len++] = 'p';
      xmp_path[len] = '\0';

      // on Windows the encoding might not be UTF8
      gchar *xmp_path_locale = dt_util_normalize_path(xmp_path);
      int stat_res = -1;
#ifdef _WIN32
      // UTF8 paths fail in this context, but converting to UTF16 works
      struct _stati64 statbuf;
      if(xmp_path_locale) // in Windows dt_util_normalize_path returns
                          // NULL if file does not exist
      {
        wchar_t *wfilename = g_utf8_to_utf16(xmp_path_locale, -1, NULL, NULL, NULL);
        stat_res = _wstati64(wfilename, &statbuf);
        g_free(wfilename);
      }
 #else
      struct stat statbuf;
      stat_res = stat(xmp_path_locale, &statbuf);
#endif
      g_free(xmp_path_locale);
      if(stat_res) continue; // TODO: shall we report these?

      // step 1: check if the xmp is newer than our db entry
      // FIXME: allow for a few seconds difference?
      if(timestamp < statbuf.st_mtime)
      {
        dt_control_crawler_result_t *item
            = (dt_control_crawler_result_t *)malloc(sizeof(dt_control_crawler_result_t));
        item->id = id;
        item->timestamp_xmp = statbuf.st_mtime;
        item->timestamp_db = timestamp;
        item->image_path = g_strdup(image_path);
        item->xmp_path = g_strdup(xmp_path);

        result = g_list_prepend(result, item);
        dt_print(DT_DEBUG_CONTROL,
                 "[crawler] `%s' (id: %d) is a newer XMP file.\n", xmp_path, id);
      }
      // older timestamps are the case for all images after the db
      // upgrade. better not report these
    }

    // step 2: check if the image has associated files (.txt, .wav)
    size_t len = strlen(image_path);
    const char *c = image_path + len;
    while((c > image_path) && (*c != '.')) c--;
    len = c - image_path + 1;

    char *extra_path = (char *)calloc(len + 3 + 1, sizeof(char));
    g_strlcpy(extra_path, image_path, len + 1);

    extra_path[len] = 't';
    extra_path[len + 1] = 'x';
    extra_path[len + 2] = 't';
    gboolean has_txt = g_file_test(extra_path, G_FILE_TEST_EXISTS);

    if(!has_txt)
    {
      extra_path[len] = 'T';
      extra_path[len + 1] = 'X';
      extra_path[len + 2] = 'T';
      has_txt = g_file_test(extra_path, G_FILE_TEST_EXISTS);
    }

    extra_path[len] = 'w';
    extra_path[len + 1] = 'a';
    extra_path[len + 2] = 'v';
    gboolean has_wav = g_file_test(extra_path, G_FILE_TEST_EXISTS);

    if(!has_wav)
    {
      extra_path[len] = 'W';
      extra_path[len + 1] = 'A';
      extra_path[len + 2] = 'V';
      has_wav = g_file_test(extra_path, G_FILE_TEST_EXISTS);
    }

    // TODO: decide if we want to remove the flag for images that lost
    // their extra file. currently we do (the else cases)
    int new_flags = flags;
    if(has_txt)
      new_flags |= DT_IMAGE_HAS_TXT;
    else
      new_flags &= ~DT_IMAGE_HAS_TXT;
    if(has_wav)
      new_flags |= DT_IMAGE_HAS_WAV;
    else
      new_flags &= ~DT_IMAGE_HAS_WAV;
    if(flags != new_flags)
    {
      sqlite3_bind_int(inner_stmt, 1, new_flags);
      sqlite3_bind_int(inner_stmt, 2, id);
      sqlite3_step(inner_stmt);
      sqlite3_reset(inner_stmt);
      sqlite3_clear_bindings(inner_stmt);
    }

    free(extra_path);
  }

  dt_database_release_transaction(darktable.db);

  sqlite3_finalize(stmt);
  sqlite3_finalize(inner_stmt);

  return g_list_reverse(result); // list was built in reverse order, so un-reverse it
}


/********************* the gui stuff *********************/

typedef struct dt_control_crawler_gui_t
{
  GtkTreeView *tree;
  GtkTreeModel *model;
  GtkWidget *log;
  GtkWidget *spinner;
  GList *rows_to_remove;
} dt_control_crawler_gui_t;

// close the window and clean up
static void dt_control_crawler_response_callback(GtkWidget *dialog,
                                                 const gint response_id,
                                                 gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  g_object_unref(G_OBJECT(gui->model));
  gtk_widget_destroy(dialog);
  free(gui);
}


static void _delete_selected_rows(dt_control_crawler_gui_t *gui)
{
  GList *rr_list = gui->rows_to_remove;
  GtkTreeModel *model = gui->model;

  // Remove TreeView rows from rr_list. It needs to be populated before
  for(GList *node = rr_list; node != NULL; node = g_list_next(node))
  {
    GtkTreePath *path = gtk_tree_row_reference_get_path((GtkTreeRowReference*)node->data);

    if(path)
    {
      GtkTreeIter  iter;
      if(gtk_tree_model_get_iter(model, &iter, path))
        gtk_list_store_remove(GTK_LIST_STORE(model), &iter);
    }
  }

  // Cleanup the list of rows
  g_list_foreach(rr_list, (GFunc) gtk_tree_row_reference_free, NULL);
  g_list_free(rr_list);
}


static void _select_all_callback(GtkButton *button,
                                 gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gtk_tree_selection_select_all(selection);
}


static void _select_none_callback(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gtk_tree_selection_unselect_all(selection);
}


static void _select_invert_callback(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);

  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(gui->model, &iter);
  while(valid)
  {
    if(gtk_tree_selection_iter_is_selected(selection, &iter))
      gtk_tree_selection_unselect_iter(selection, &iter);
    else
      gtk_tree_selection_select_iter(selection, &iter);

    valid = gtk_tree_model_iter_next(gui->model, &iter);
  }
}


static void _db_update_timestamp(const int id, const time_t timestamp)
{
  // Update DB writing timestamp with XMP file timestamp
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "UPDATE main.images"
     " SET write_timestamp = ?2"
     " WHERE id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, timestamp);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}


static void _get_crawler_entry_from_model(GtkTreeModel *model,
                                          GtkTreeIter *iter,
                                          dt_control_crawler_result_t *entry)
{
  gtk_tree_model_get(model, iter,
                     DT_CONTROL_CRAWLER_COL_IMAGE_PATH, &entry->image_path,
                     DT_CONTROL_CRAWLER_COL_ID,         &entry->id,
                     DT_CONTROL_CRAWLER_COL_XMP_PATH,   &entry->xmp_path,
                     DT_CONTROL_CRAWLER_COL_TS_DB_INT,  &entry->timestamp_db,
                     DT_CONTROL_CRAWLER_COL_TS_XMP_INT, &entry->timestamp_xmp, -1);
}


static void _append_row_to_remove(GtkTreeModel *model,
                                  GtkTreePath *path,
                                  GList **rowref_list)
{
  // append TreeModel rows to the list to remove
  GtkTreeRowReference *rowref = gtk_tree_row_reference_new(model, path);
  *rowref_list = g_list_append(*rowref_list, rowref);
}

static void _log_synchronization(dt_control_crawler_gui_t *gui,
                                 gchar *pattern,
                                 gchar *filepath)
{
  gchar *message = pattern;
  gboolean to_free = FALSE;

  if(filepath)
  {
    message = g_strdup_printf(pattern, filepath);
    to_free = TRUE;
  }

  // add a new line in the log TreeView
  GtkTreeIter iter_log;
  GtkTreeModel *model_log = gtk_tree_view_get_model(GTK_TREE_VIEW(gui->log));
  gtk_list_store_append(GTK_LIST_STORE(model_log), &iter_log);
  gtk_list_store_set(GTK_LIST_STORE(model_log), &iter_log,
                     0, message,
                     -1);

  if(to_free) g_free(message);
}


static void sync_xmp_to_db(GtkTreeModel *model,
                           GtkTreePath *path,
                           GtkTreeIter *iter,
                           gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { 0 };
  _get_crawler_entry_from_model(model, iter, &entry);
  _db_update_timestamp(entry.id, entry.timestamp_xmp);

  const gboolean error = dt_history_load_and_apply(entry.id, entry.xmp_path, 0);

  if(error)
  {
    _log_synchronization(gui, _("ERROR: %s NOT synced XMP → DB"), entry.image_path);
    _log_synchronization(gui, _("ERROR: cannot write the database."
                                " the destination may be full, offline or read-only."),
                         NULL);
  }
  else
  {
    _append_row_to_remove(model, path, &gui->rows_to_remove);
    _log_synchronization(gui, _("SUCCESS: %s synced XMP → DB"), entry.image_path);
  }

  _free_crawler_result(&entry);
}


static void sync_db_to_xmp(GtkTreeModel *model,
                           GtkTreePath *path,
                           GtkTreeIter *iter,
                           gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { 0 };
  _get_crawler_entry_from_model(model, iter, &entry);

  // write the XMP and make sure it get the last modified timestamp of the db
  const gboolean error = dt_image_write_sidecar_file(entry.id);
  _set_modification_time(entry.xmp_path, entry.timestamp_db);

  if(error)
  {
    _log_synchronization(gui, _("ERROR: %s NOT synced DB → XMP"), entry.image_path);
    _log_synchronization(gui,
                         _("ERROR: cannot write %s \nthe destination may be full,"
                           " offline or read-only."), entry.xmp_path);
  }
  else
  {
    _append_row_to_remove(model, path, &gui->rows_to_remove);
    _log_synchronization(gui, _("SUCCESS: %s synced DB → XMP"), entry.image_path);
  }

  _free_crawler_result(&entry);
}

static void sync_newest_to_oldest(GtkTreeModel *model,
                                  GtkTreePath *path,
                                  GtkTreeIter *iter,
                                  gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { 0 };
  _get_crawler_entry_from_model(model, iter, &entry);

  gboolean error = FALSE;

  if(entry.timestamp_xmp > entry.timestamp_db)
  {
    // WRITE XMP in DB
    _db_update_timestamp(entry.id, entry.timestamp_xmp);
    error = dt_history_load_and_apply(entry.id, entry.xmp_path, 0);
    if(error)
    {
      _log_synchronization
        (gui,
         _("ERROR: %s NOT synced new (XMP) → old (DB)"), entry.image_path);
      _log_synchronization
        (gui,
         _("ERROR: cannot write the database. the destination may be full,"
           " offline or read-only."), NULL);
    }
    else
    {
      _log_synchronization
        (gui,
         _("SUCCESS: %s synced new (XMP) → old (DB)"), entry.image_path);
    }
  }
  else if(entry.timestamp_xmp < entry.timestamp_db)
  {
    // write the XMP and make sure it get the last modified timestamp of the db
    error = dt_image_write_sidecar_file(entry.id);
    _set_modification_time(entry.xmp_path, entry.timestamp_db);

    dt_print(DT_DEBUG_ALWAYS, "%s synced DB (new) → XMP (old)\n", entry.image_path);
    if(error)
    {
      _log_synchronization
        (gui,
         _("ERROR: %s NOT synced new (DB) → old (XMP)"), entry.image_path);
      _log_synchronization
        (gui,
         _("ERROR: cannot write %s \nthe destination may be full, offline or read-only."),
         entry.xmp_path);
    }
    else
    {
      _log_synchronization(gui, _("SUCCESS: %s synced new (DB) → old (XMP)"),
                           entry.image_path);
    }
  }
  else
  {
    // we should never reach that part of the code
    // if both timestamps are equal, they should not be in this list in the first place
    error = TRUE;
    _log_synchronization(gui, _("EXCEPTION: %s has inconsistent timestamps"),
                         entry.image_path);
  }

  if(!error) _append_row_to_remove(model, path, &gui->rows_to_remove);

  _free_crawler_result(&entry);
}


static void sync_oldest_to_newest(GtkTreeModel *model,
                                  GtkTreePath *path,
                                  GtkTreeIter *iter,
                                  gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { 0 };
  _get_crawler_entry_from_model(model, iter, &entry);
  gboolean error = FALSE;

  if(entry.timestamp_xmp < entry.timestamp_db)
  {
    // WRITE XMP in DB
    _db_update_timestamp(entry.id, entry.timestamp_xmp);
    error = dt_history_load_and_apply(entry.id, entry.xmp_path, 0);
    if(error)
    {
      _log_synchronization(gui,
                           _("ERROR: %s NOT synced old (XMP) → new (DB)"),
                           entry.image_path);
    _log_synchronization(gui,
                         _("ERROR: cannot write the database."
                           " the destination may be full, offline or read-only."), NULL);
    }
    else
    {
      _log_synchronization(gui,
                           _("SUCCESS: %s synced old (XMP) → new (DB)"),
                           entry.image_path);
    }
  }
  else if(entry.timestamp_xmp > entry.timestamp_db)
  {
    // WRITE DB in XMP
    error = dt_image_write_sidecar_file(entry.id);
    _set_modification_time(entry.xmp_path, entry.timestamp_db);
    if(error)
    {
      _log_synchronization(gui,
                           _("ERROR: %s NOT synced old (DB) → new (XMP)"),
                           entry.image_path);
      _log_synchronization(gui,
                           _("ERROR: cannot write %s \nthe destination may be full,"
                             " offline or read-only."), entry.xmp_path);
    }
    else
    {
      _log_synchronization(gui,
                           _("SUCCESS: %s synced old (DB) → new (XMP)"),
                           entry.image_path);
    }
  }
  else
  {
    // we should never reach that part of the code
    // if both timestamps are equal, they should not be in this list in the first place
    error = TRUE;
    _log_synchronization(gui,
                         _("EXCEPTION: %s has inconsistent timestamps"),
                         entry.image_path);
  }

  if(!error)
    _append_row_to_remove(model, path, &gui->rows_to_remove);

  _free_crawler_result(&entry);
}

// overwrite database with xmp
static void _reload_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gui->rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_xmp_to_db, gui);
  _delete_selected_rows(gui);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

// overwrite xmp with database
void _overwrite_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gui->rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_db_to_xmp, gui);
  _delete_selected_rows(gui);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

// overwrite the oldest with the newest
static void _newest_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gui->rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_newest_to_oldest, gui);
  _delete_selected_rows(gui);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

// overwrite the newest with the oldest
static void _oldest_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gui->rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_oldest_to_newest, gui);
  _delete_selected_rows(gui);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

static gchar* str_time_delta(const int time_delta)
{
  // display the time difference as a legible string
  int seconds = time_delta;

  int minutes = seconds / 60;
  seconds -= 60 * minutes;

  int hours = minutes / 60;
  minutes -= 60 * hours;

  const int days = hours / 24;
  hours -= 24 * days;

  return g_strdup_printf(_("%id %02dh %02dm %02ds"), days, hours, minutes, seconds);
}

// show a popup window with a list of updated images/xmp files and allow the user to tell dt what to do about them
void dt_control_crawler_show_image_list(GList *images)
{
  if(!images) return;

  dt_control_crawler_gui_t *gui =
    (dt_control_crawler_gui_t *)malloc(sizeof(dt_control_crawler_gui_t));

  // a list with all the images
  GtkTreeViewColumn *column;
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_vexpand(scroll, TRUE);
  GtkListStore *store = gtk_list_store_new(DT_CONTROL_CRAWLER_NUM_COLS,
                                           G_TYPE_INT,    // id
                                           G_TYPE_STRING, // image path
                                           G_TYPE_STRING, // xmp path
                                           G_TYPE_STRING, // timestamp from xmp
                                           G_TYPE_STRING, // timestamp from db
                                           G_TYPE_INT,    // timestamp to db
                                           G_TYPE_INT,
                                           G_TYPE_STRING, // report: newer version
                                           G_TYPE_STRING);// time delta

  gui->model = GTK_TREE_MODEL(store);

  for(GList *list_iter = images; list_iter; list_iter = g_list_next(list_iter))
  {
    GtkTreeIter iter;
    dt_control_crawler_result_t *item = list_iter->data;
    char timestamp_db[64], timestamp_xmp[64];
    struct tm tm_stamp;
    strftime(timestamp_db, sizeof(timestamp_db),
             "%c", localtime_r(&item->timestamp_db, &tm_stamp));
    strftime(timestamp_xmp, sizeof(timestamp_xmp),
             "%c", localtime_r(&item->timestamp_xmp, &tm_stamp));

    const time_t time_delta = llabs(item->timestamp_db - item->timestamp_xmp);
    gchar *timestamp_delta = str_time_delta(time_delta);

    gtk_list_store_append(store, &iter);
    gtk_list_store_set
      (store, &iter,
       DT_CONTROL_CRAWLER_COL_ID, item->id,
       DT_CONTROL_CRAWLER_COL_IMAGE_PATH, item->image_path,
       DT_CONTROL_CRAWLER_COL_XMP_PATH, item->xmp_path,
       DT_CONTROL_CRAWLER_COL_TS_XMP, timestamp_xmp,
       DT_CONTROL_CRAWLER_COL_TS_DB, timestamp_db,
       DT_CONTROL_CRAWLER_COL_TS_XMP_INT, item->timestamp_xmp,
       DT_CONTROL_CRAWLER_COL_TS_DB_INT, item->timestamp_db,
       DT_CONTROL_CRAWLER_COL_REPORT, (item->timestamp_xmp > item->timestamp_db)
                                      ? _("XMP")
                                      : _("database"),
       DT_CONTROL_CRAWLER_COL_TIME_DELTA, timestamp_delta,
       -1);
    _free_crawler_result(item);
    g_free(timestamp_delta);
  }
  g_list_free_full(images, g_free);

  GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);

  gui->tree = GTK_TREE_VIEW(tree); // FIXME: do we need to free that later ?

  GtkCellRenderer *renderer_text = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes
    (_("path"), renderer_text, "text",
     DT_CONTROL_CRAWLER_COL_IMAGE_PATH, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(200));
  g_object_set(renderer_text, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);

  column = gtk_tree_view_column_new_with_attributes
    (_("XMP timestamp"), gtk_cell_renderer_text_new(), "text",
     DT_CONTROL_CRAWLER_COL_TS_XMP, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  column = gtk_tree_view_column_new_with_attributes
    (_("database timestamp"), gtk_cell_renderer_text_new(), "text",
     DT_CONTROL_CRAWLER_COL_TS_DB, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  column = gtk_tree_view_column_new_with_attributes
    (_("newest"), gtk_cell_renderer_text_new(), "text",
     DT_CONTROL_CRAWLER_COL_REPORT, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  GtkCellRenderer *renderer_date = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes
    (_("time difference"), renderer_date, "text",
     DT_CONTROL_CRAWLER_COL_TIME_DELTA, NULL);
  g_object_set(renderer_date, "xalign", 1., NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  gtk_container_add(GTK_CONTAINER(scroll), tree);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  // build a dialog window that contains the list of images
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons
    (_("updated XMP sidecar files found"), GTK_WINDOW(win),
     GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, _("_close"),
     GTK_RESPONSE_CLOSE, NULL);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_set_size_request(dialog, -1, DT_PIXEL_APPLY_DPI(400));
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(win));
  GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(content_area), content_box);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(content_box), box, FALSE, FALSE, 0);
  GtkWidget *select_all = gtk_button_new_with_label(_("select all"));
  GtkWidget *select_none = gtk_button_new_with_label(_("select none"));
  GtkWidget *select_invert = gtk_button_new_with_label(_("invert selection"));
  gtk_box_pack_start(GTK_BOX(box), select_all, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), select_none, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), select_invert, FALSE, FALSE, 0);
  g_signal_connect(select_all, "clicked", G_CALLBACK(_select_all_callback), gui);
  g_signal_connect(select_none, "clicked", G_CALLBACK(_select_none_callback), gui);
  g_signal_connect(select_invert, "clicked", G_CALLBACK(_select_invert_callback), gui);

  gtk_box_pack_start(GTK_BOX(content_box), scroll, TRUE, TRUE, 0);

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(content_box), box, FALSE, FALSE, 1);
  GtkWidget *label = gtk_label_new_with_mnemonic(_("on the selection:"));
  GtkWidget *reload_button = gtk_button_new_with_label(_("keep the XMP edit"));
  GtkWidget *overwrite_button = gtk_button_new_with_label(_("keep the database edit"));
  GtkWidget *newest_button = gtk_button_new_with_label(_("keep the newest edit"));
  GtkWidget *oldest_button = gtk_button_new_with_label(_("keep the oldest edit"));
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), reload_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), overwrite_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), newest_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), oldest_button, FALSE, FALSE, 0);
  g_signal_connect(reload_button, "clicked", G_CALLBACK(_reload_button_clicked), gui);
  g_signal_connect(overwrite_button, "clicked", G_CALLBACK(_overwrite_button_clicked), gui);
  g_signal_connect(newest_button, "clicked", G_CALLBACK(_newest_button_clicked), gui);
  g_signal_connect(oldest_button, "clicked", G_CALLBACK(_oldest_button_clicked), gui);

  /* Feedback spinner in case synch happens over network and stales */
  gui->spinner = gtk_spinner_new();
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(gui->spinner), FALSE, FALSE, 0);

  /* Log report */
  scroll = gtk_scrolled_window_new(NULL, NULL);
  gui->log = gtk_tree_view_new();
  gtk_box_pack_start(GTK_BOX(content_box), scroll, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(scroll), gui->log);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  gtk_tree_view_insert_column_with_attributes
    (GTK_TREE_VIEW(gui->log), -1,
     _("synchronization log"), renderer_text,
     "text", 0, NULL);

  GtkListStore *store_log = gtk_list_store_new (1, G_TYPE_STRING);
  GtkTreeModel *model_log = GTK_TREE_MODEL(store_log);
  gtk_tree_view_set_model(GTK_TREE_VIEW(gui->log), model_log);
  g_object_unref(model_log);

  gtk_widget_show_all(dialog);

  g_signal_connect(dialog, "response",
                   G_CALLBACK(dt_control_crawler_response_callback), gui);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
