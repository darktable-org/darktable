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

#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#include "common/darktable.h"
#include "common/database.h"
#include "common/history.h"
#include "common/image.h"
#include "control/conf.h"
#include "crawler.h"
#include "gui/gtk.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif


typedef enum dt_control_crawler_cols_t
{
  DT_CONTROL_CRAWLER_COL_SELECTED = 0,
  DT_CONTROL_CRAWLER_COL_ID,
  DT_CONTROL_CRAWLER_COL_IMAGE_PATH,
  DT_CONTROL_CRAWLER_COL_XMP_PATH,
  DT_CONTROL_CRAWLER_COL_TS_XMP,
  DT_CONTROL_CRAWLER_COL_TS_DB,
  DT_CONTROL_CRAWLER_NUM_COLS
} dt_control_crawler_cols_t;

typedef struct dt_control_crawler_result_t
{
  int id;
  time_t timestamp_xmp;
  time_t timestamp_db;
  char *image_path, *xmp_path;
} dt_control_crawler_result_t;


GList *dt_control_crawler_run()
{
  sqlite3_stmt *stmt, *inner_stmt;
  GList *result = NULL;
  gboolean look_for_xmp = dt_conf_get_bool("write_sidecar_files");

  sqlite3_prepare_v2(dt_database_get(darktable.db),
                     "SELECT i.id, write_timestamp, version, folder || '" G_DIR_SEPARATOR_S "' || filename, flags "
                     "FROM main.images i, main.film_rolls f ON i.film_id = f.id ORDER BY f.id, filename",
                     -1, &stmt, NULL);
  sqlite3_prepare_v2(dt_database_get(darktable.db), "UPDATE main.images SET flags = ?1 WHERE id = ?2", -1,
                     &inner_stmt, NULL);

  // let's wrap this into a transaction, it might make it a little faster.
  sqlite3_exec(dt_database_get(darktable.db), "BEGIN TRANSACTION", NULL, NULL, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    const time_t timestamp = sqlite3_column_int(stmt, 1);
    const int version = sqlite3_column_int(stmt, 2);
    gchar *image_path = (gchar *)sqlite3_column_text(stmt, 3);
    int flags = sqlite3_column_int(stmt, 4);

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

      struct stat statbuf;
      if(stat(xmp_path, &statbuf) == -1) continue; // TODO: shall we report these?

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

    // TODO: decide if we want to remove the flag for images that lost their extra file. currently we do (the
    // else cases)
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

    g_free(extra_path);
  }

  sqlite3_exec(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);

  sqlite3_finalize(stmt);
  sqlite3_finalize(inner_stmt);

  return result;
}


/********************* the gui stuff *********************/

typedef struct dt_control_crawler_gui_t
{
  GtkTreeModel *model;
  GtkWidget *select_all;
  gulong select_all_handler_id;
} dt_control_crawler_gui_t;

// close the window and clean up
static void dt_control_crawler_response_callback(GtkWidget *dialog, gint response_id, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  g_object_unref(G_OBJECT(gui->model));
  gtk_widget_destroy(dialog);
  free(gui);
}

// unselect the "select all" toggle
static void _clear_select_all(dt_control_crawler_gui_t *gui)
{
  g_signal_handler_block(G_OBJECT(gui->select_all), gui->select_all_handler_id);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gui->select_all), FALSE);
  g_signal_handler_unblock(G_OBJECT(gui->select_all), gui->select_all_handler_id);
}

// set the "selected" flag in the list model when an image gets (un)selected
static void _select_toggled_callback(GtkCellRendererToggle *cell_renderer, gchar *path_str, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeIter iter;
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  gboolean selected;

  gtk_tree_model_get_iter(gui->model, &iter, path);
  gtk_tree_model_get(gui->model, &iter, DT_CONTROL_CRAWLER_COL_SELECTED, &selected, -1);
  gtk_list_store_set(GTK_LIST_STORE(gui->model), &iter, DT_CONTROL_CRAWLER_COL_SELECTED, !selected, -1);

  gtk_tree_path_free(path);

  // we also want to disable the "select all" thing
  _clear_select_all(gui);
}

// (un)select all images in the list
static void _select_all_callback(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;

  gboolean selected = gtk_toggle_button_get_active(togglebutton);

  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(gui->model, &iter);
  while(valid)
  {
    gtk_list_store_set(GTK_LIST_STORE(gui->model), &iter, DT_CONTROL_CRAWLER_COL_SELECTED, selected, -1);
    valid = gtk_tree_model_iter_next(gui->model, &iter);
  }
}

// reload xmp files of the selected images
static void _reload_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;

  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(gui->model, &iter);
  while(valid)
  {
    gboolean selected;
    int id;
    gchar *xmp_path;
    gtk_tree_model_get(gui->model, &iter, DT_CONTROL_CRAWLER_COL_SELECTED, &selected,
                       DT_CONTROL_CRAWLER_COL_ID, &id, DT_CONTROL_CRAWLER_COL_XMP_PATH, &xmp_path, -1);
    if(selected)
    {
      dt_history_load_and_apply(id, xmp_path, 0);
      valid = gtk_list_store_remove(GTK_LIST_STORE(gui->model), &iter);
    }
    else
      valid = gtk_tree_model_iter_next(gui->model, &iter);
  }
  // we also want to disable the "select all" thing
  _clear_select_all(gui);
}

// overwrite xmp files of the selected images
void _overwrite_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;

  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(gui->model, &iter);
  while(valid)
  {
    gboolean selected;
    int id;
    gtk_tree_model_get(gui->model, &iter, DT_CONTROL_CRAWLER_COL_SELECTED, &selected,
                       DT_CONTROL_CRAWLER_COL_ID, &id, -1);
    if(selected)
    {
      dt_image_write_sidecar_file(id);
      valid = gtk_list_store_remove(GTK_LIST_STORE(gui->model), &iter);
    }
    else
      valid = gtk_tree_model_iter_next(gui->model, &iter);
  }
  // we also want to disable the "select all" thing
  _clear_select_all(gui);
}

// show a popup window with a list of updated images/xmp files and allow the user to tell dt what to do about
// them
void dt_control_crawler_show_image_list(GList *images)
{
  if(!images) return;

  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)malloc(sizeof(dt_control_crawler_gui_t));

  // a list with all the images
  GtkTreeViewColumn *column;
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_vexpand(scroll, TRUE);
  GtkListStore *store = gtk_list_store_new(DT_CONTROL_CRAWLER_NUM_COLS,
                                           G_TYPE_BOOLEAN, // selection toggle
                                           G_TYPE_INT,     // id
                                           G_TYPE_STRING,  // image path
                                           G_TYPE_STRING,  // xmp path
                                           G_TYPE_STRING,  // timestamp from xmp
                                           G_TYPE_STRING   // timestamp from db
                                           );

  gui->model = GTK_TREE_MODEL(store);

  GList *list_iter = g_list_first(images);
  while(list_iter)
  {
    GtkTreeIter iter;
    dt_control_crawler_result_t *item = list_iter->data;
    char timestamp_db[64], timestamp_xmp[64];
    strftime(timestamp_db, sizeof(timestamp_db), "%c", localtime(&item->timestamp_db));
    strftime(timestamp_xmp, sizeof(timestamp_xmp), "%c", localtime(&item->timestamp_xmp));
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, DT_CONTROL_CRAWLER_COL_SELECTED, 0, DT_CONTROL_CRAWLER_COL_ID, item->id,
                       DT_CONTROL_CRAWLER_COL_IMAGE_PATH, item->image_path, DT_CONTROL_CRAWLER_COL_XMP_PATH,
                       item->xmp_path, DT_CONTROL_CRAWLER_COL_TS_XMP, timestamp_xmp,
                       DT_CONTROL_CRAWLER_COL_TS_DB, timestamp_db, -1);
    g_free(item->image_path);
    g_free(item->xmp_path);
    list_iter = g_list_next(list_iter);
  }
  g_list_free_full(images, g_free);

  GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));

  GtkCellRenderer *renderer = gtk_cell_renderer_toggle_new();
  g_signal_connect(renderer, "toggled", G_CALLBACK(_select_toggled_callback), gui);
  column = gtk_tree_view_column_new_with_attributes(_("select"), renderer, "active",
                                                    DT_CONTROL_CRAWLER_COL_SELECTED, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  column = gtk_tree_view_column_new_with_attributes(_("path"), gtk_cell_renderer_text_new(), "text",
                                                    DT_CONTROL_CRAWLER_COL_IMAGE_PATH, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  column = gtk_tree_view_column_new_with_attributes(_("xmp timestamp"), gtk_cell_renderer_text_new(), "text",
                                                    DT_CONTROL_CRAWLER_COL_TS_XMP, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  column = gtk_tree_view_column_new_with_attributes(_("database timestamp"), gtk_cell_renderer_text_new(),
                                                    "text", DT_CONTROL_CRAWLER_COL_TS_DB, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  gtk_container_add(GTK_CONTAINER(scroll), tree);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  // build a dialog window that contains the list of images
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("updated xmp sidecar files found"), GTK_WINDOW(win),
                                                  GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                                  _("_close"), GTK_RESPONSE_CLOSE, NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_set_size_request(dialog, -1, DT_PIXEL_APPLY_DPI(400));
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(win));
  GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_set_margin_start(content_box, DT_PIXEL_APPLY_DPI(10));
  gtk_widget_set_margin_end(content_box, DT_PIXEL_APPLY_DPI(10));
  gtk_widget_set_margin_top(content_box, DT_PIXEL_APPLY_DPI(5));
  gtk_widget_set_margin_bottom(content_box, DT_PIXEL_APPLY_DPI(0));
  gtk_container_add(GTK_CONTAINER(content_area), content_box);

  gtk_box_pack_start(GTK_BOX(content_box), scroll, TRUE, TRUE, 0);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_pack_start(GTK_BOX(content_box), box, FALSE, FALSE, 0);
  GtkWidget *select_all = gtk_check_button_new_with_label(_("select all"));
  gtk_box_pack_start(GTK_BOX(box), select_all, FALSE, FALSE, 0);
  gui->select_all_handler_id = g_signal_connect(select_all, "toggled", G_CALLBACK(_select_all_callback), gui);
  gui->select_all = select_all;

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_pack_start(GTK_BOX(content_box), box, FALSE, FALSE, 0);
  GtkWidget *reload_button = gtk_button_new_with_label(_("reload selected xmp files"));
  GtkWidget *overwrite_button = gtk_button_new_with_label(_("overwrite selected xmp files"));
  gtk_box_pack_start(GTK_BOX(box), reload_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), overwrite_button, FALSE, FALSE, 0);
  g_signal_connect(reload_button, "clicked", G_CALLBACK(_reload_button_clicked), gui);
  g_signal_connect(overwrite_button, "clicked", G_CALLBACK(_overwrite_button_clicked), gui);

  gtk_widget_show_all(dialog);

  g_signal_connect(dialog, "response", G_CALLBACK(dt_control_crawler_response_callback), gui);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
