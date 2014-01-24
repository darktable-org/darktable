/*
    This file is part of darktable,
    copyright (c) 2014 tobias ellinghaus.

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

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#include "crawler.h"
#include "common/darktable.h"
#include "common/database.h"
#include "common/history.h"
#include "common/image.h"
#include "control/conf.h"
#include "gui/gtk.h"


typedef enum dt_control_crawler_cols_t
{
  DT_CONTROL_CRAWLER_COL_IMAGE_PATH = 0,
  DT_CONTROL_CRAWLER_COL_TS_XMP,
  DT_CONTROL_CRAWLER_COL_TS_DB,
  DT_CONTROL_CRAWLER_NUM_COLS
}
dt_control_crawler_cols_t;

typedef struct dt_control_crawler_result_t
{
  int id;
  time_t timestamp_xmp;
  time_t timestamp_db;
  char *image_path, *xmp_path;
} dt_control_crawler_result_t;


GList * dt_control_crawler_run()
{
  sqlite3_stmt *stmt, *inner_stmt;
  GList *result = NULL;
  gboolean look_for_xmp = dt_conf_get_bool("write_sidecar_files");

  sqlite3_prepare_v2(dt_database_get(darktable.db),
                     "SELECT images.id, write_timestamp, version, folder || '/' || filename, flags "
                     "FROM images, film_rolls WHERE images.film_id = film_rolls.id "
                     "ORDER BY film_rolls.id, filename", -1, &stmt, NULL);
  sqlite3_prepare_v2(dt_database_get(darktable.db), "UPDATE images SET flags = ?1 WHERE id = ?2", -1, &inner_stmt, NULL);

  // let's wrap this into a transaction, it might make it a little faster.
  sqlite3_exec(dt_database_get(darktable.db), "BEGIN TRANSACTION", NULL, NULL, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    const time_t timestamp = sqlite3_column_int(stmt, 1);
    const int version = sqlite3_column_int(stmt, 2);
    gchar* image_path = (gchar*)sqlite3_column_text(stmt, 3);
    int flags = sqlite3_column_int(stmt, 4);

    // no need to look for xmp files if none get written anyway.
    if(look_for_xmp)
    {
      // construct the xmp filename for this image
      gchar xmp_path[DT_MAX_PATH_LEN];
      g_strlcpy(xmp_path, image_path, DT_MAX_PATH_LEN);
      dt_image_path_append_version_no_db(version, xmp_path, DT_MAX_PATH_LEN);
      size_t len = strlen(xmp_path);
      if(len + 4 >= DT_MAX_PATH_LEN) continue;
      xmp_path[len++] = '.';
      xmp_path[len++] = 'x';
      xmp_path[len++] = 'm';
      xmp_path[len++] = 'p';
      xmp_path[len] = '\0';

      struct stat statbuf;
      if(stat(xmp_path, &statbuf) == -1) continue; // TODO: shall we report these?

      // step 1: check if the xmp is newer than our db entry
      // FIXME: allow for a few seconds difference?
      if(timestamp < statbuf.st_mtime)
      {
        dt_control_crawler_result_t *item = (dt_control_crawler_result_t*)malloc(sizeof(dt_control_crawler_result_t));
        item->id = id;
        item->timestamp_xmp = statbuf.st_mtime;
        item->timestamp_db  = timestamp;
        item->image_path = g_strdup(image_path);
        item->xmp_path = g_strdup(xmp_path);

        result = g_list_append(result, item);
        dt_print(DT_DEBUG_CONTROL, "[crawler] `%s' (id: %d) is a newer xmp file.\n", xmp_path, id);
      }
      // older timestamps are the case for all images after the db upgrade. better not report these
//       else if(timestamp > statbuf.st_mtime)
//         printf("`%s' (%d) has an older xmp file.\n", image_path, id);
    }

    // step 2: check if the image has associated files (.txt, .wav)
    size_t len = strlen(image_path);
    char *c = image_path + len;
    while((c > image_path) && (*c != '.')) *c-- = '\0';
    len = c - image_path + 1;

    char *extra_path = g_strndup(image_path, len + 3);

    extra_path[len]   = 't';
    extra_path[len+1] = 'x';
    extra_path[len+2] = 't';
    gboolean has_txt = g_file_test(extra_path, G_FILE_TEST_EXISTS);

    if(!has_txt)
    {
      extra_path[len]   = 'T';
      extra_path[len+1] = 'X';
      extra_path[len+2] = 'T';
      has_txt = g_file_test(extra_path, G_FILE_TEST_EXISTS);
    }

    extra_path[len]   = 'w';
    extra_path[len+1] = 'a';
    extra_path[len+2] = 'v';
    gboolean has_wav = g_file_test(extra_path, G_FILE_TEST_EXISTS);

    if(!has_wav)
    {
      extra_path[len]   = 'W';
      extra_path[len+1] = 'A';
      extra_path[len+2] = 'V';
      has_wav = g_file_test(extra_path, G_FILE_TEST_EXISTS);
    }

    // TODO: decide if we want to remove the flag for images that lost their extra file. currently we do (the else cases)
    int new_flags = flags;
    if(has_txt) new_flags |= DT_IMAGE_HAS_TXT;
    else        new_flags &= ~DT_IMAGE_HAS_TXT;
    if(has_wav) new_flags |= DT_IMAGE_HAS_WAV;
    else        new_flags &= ~DT_IMAGE_HAS_WAV;
    if(flags != new_flags)
    {
      sqlite3_bind_int(inner_stmt, 1, new_flags);
      sqlite3_bind_int(inner_stmt, 2, id);
      sqlite3_step(inner_stmt);
      sqlite3_reset(inner_stmt);
      sqlite3_clear_bindings(inner_stmt);
    }

    g_free(extra_path);
  }

  sqlite3_exec(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);

  sqlite3_finalize(stmt);
  sqlite3_finalize(inner_stmt);

  return result;
}

static void dt_control_crawler_response_callback(GtkWidget *dialog, gint response_id, gpointer user_data)
{
  if(!user_data) return;
  GList *images = g_list_first((GList*)user_data);
  GList *iter = images;

  switch(response_id)
  {
    case GTK_RESPONSE_APPLY:
      // reload xmp file
      while(iter)
      {
        dt_control_crawler_result_t *item = (dt_control_crawler_result_t*)iter->data;
        dt_history_load_and_apply(item->id, item->xmp_path, 0);
        g_free(item->image_path);
        g_free(item->xmp_path);
        iter = g_list_next(iter);
      }
      break;
    case GTK_RESPONSE_REJECT:
      // overwriting xmp files
      while(iter)
      {
        dt_control_crawler_result_t *item = (dt_control_crawler_result_t*)iter->data;
        dt_image_write_sidecar_file(item->id);
        g_free(item->image_path);
        g_free(item->xmp_path);
        iter = g_list_next(iter);
      }
      break;
    default:
      // ignore
      while(iter)
      {
        dt_control_crawler_result_t *item = (dt_control_crawler_result_t*)iter->data;
        g_free(item->image_path);
        g_free(item->xmp_path);
        iter = g_list_next(iter);
      }
      break;
  }
  g_list_free_full(images, g_free);
  gtk_widget_destroy(dialog);
}

void dt_control_crawler_show_image_list(GList *images)
{
  if(!images) return;

  // a list with all the images
  GtkTreeViewColumn *column;
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  GtkWidget *tree = gtk_tree_view_new();
  GtkTreeStore *model = gtk_tree_store_new(DT_CONTROL_CRAWLER_NUM_COLS,
                                           G_TYPE_STRING,                // image path
                                           G_TYPE_STRING,                // timestamp from xmp
                                           G_TYPE_STRING                 // timestamp from db
                                          );


  GList *list_iter = g_list_first(images);
  while(list_iter)
  {
    GtkTreeIter iter;
    dt_control_crawler_result_t *item = list_iter->data;
    char timestamp_db[64], timestamp_xmp[64];
    strftime(timestamp_db, sizeof(timestamp_db), "%c", localtime(&item->timestamp_db));
    strftime(timestamp_xmp, sizeof(timestamp_xmp), "%c", localtime(&item->timestamp_xmp));
    gtk_tree_store_append(model, &iter, NULL);
    gtk_tree_store_set(model, &iter,
                       DT_CONTROL_CRAWLER_COL_IMAGE_PATH, item->image_path,
                       DT_CONTROL_CRAWLER_COL_TS_XMP, timestamp_xmp,
                       DT_CONTROL_CRAWLER_COL_TS_DB, timestamp_db,
                       -1);
    list_iter = g_list_next(list_iter);
  }


  column = gtk_tree_view_column_new_with_attributes(
             _("path"), gtk_cell_renderer_text_new(),
             "text", DT_CONTROL_CRAWLER_COL_IMAGE_PATH,
             NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  column = gtk_tree_view_column_new_with_attributes(
             _("xmp timestamp"), gtk_cell_renderer_text_new(),
             "text", DT_CONTROL_CRAWLER_COL_TS_XMP,
             NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  column = gtk_tree_view_column_new_with_attributes(
             _("database timestamp"), gtk_cell_renderer_text_new(),
             "text", DT_CONTROL_CRAWLER_COL_TS_DB,
             NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  gtk_tree_view_set_model(GTK_TREE_VIEW(tree), GTK_TREE_MODEL(model));

  gtk_container_add(GTK_CONTAINER(scroll), tree);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  g_object_unref(G_OBJECT(model));

  // build a dialog window that contains the list of images
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("updated xmp sidecar files found"), GTK_WINDOW(win),
                                 GTK_DIALOG_DESTROY_WITH_PARENT|GTK_DIALOG_MODAL,
                                 _("reload xmp files"), GTK_RESPONSE_APPLY,
                                 _("overwrite xmp files"), GTK_RESPONSE_REJECT,
                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                 NULL);
  gtk_widget_set_size_request(dialog, -1, 400);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(win));
  GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_add (GTK_CONTAINER(content_area), scroll);
  gtk_widget_show_all(dialog);

  g_signal_connect(dialog, "response", G_CALLBACK(dt_control_crawler_response_callback), images);

}
