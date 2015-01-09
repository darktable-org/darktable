/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika, henrik andersson.
    copyright (c) 2012 Jose Carlos Garcia Sogo

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
#include "common/film.h"
#include "common/collection.h"
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "libs/lib.h"
#include "common/metadata.h"
#include "common/utility.h"
#include "libs/collect.h"
#include "views/view.h"

DT_MODULE(1)

#define MAX_RULES 10

#define PARAM_STRING_SIZE 256 // FIXME: is this enough !?

typedef struct dt_lib_collect_rule_t
{
  int num;
  GtkWidget *hbox;
  GtkComboBox *combo;
  GtkWidget *text;
  GtkWidget *button;
  gboolean typing;
} dt_lib_collect_rule_t;

typedef struct dt_lib_collect_t
{
  dt_lib_collect_rule_t rule[MAX_RULES];
  int active_rule;

  GtkTreeView *view;
  GtkTreeModel *treemodel_folders;
  GtkTreeModel *treemodel_tags;
  gboolean tree_new;
  GtkTreeModel *listmodel;
  GtkScrolledWindow *scrolledwindow;

  //  GVolumeMonitor *gv_monitor;

  GtkBox *box;
  GtkScrolledWindow *sw2;
  GPtrArray *labels;
  GPtrArray *trees;

  struct dt_lib_collect_params_t *params;
} dt_lib_collect_t;

typedef struct dt_lib_collect_params_rule_t 
{
    uint32_t item : 16;
    uint32_t mode : 16;
    char string[PARAM_STRING_SIZE];
} dt_lib_collect_params_rule_t;

typedef struct dt_lib_collect_params_t
{
  uint32_t rules;
  dt_lib_collect_params_rule_t rule[MAX_RULES];
} dt_lib_collect_params_t;

typedef enum dt_lib_collect_cols_t
{
  DT_LIB_COLLECT_COL_TEXT = 0,
  DT_LIB_COLLECT_COL_ID,
  DT_LIB_COLLECT_COL_TOOLTIP,
  DT_LIB_COLLECT_COL_PATH,
  DT_LIB_COLLECT_COL_COUNT,
  DT_LIB_COLLECT_COL_VISIBLE,
  DT_LIB_COLLECT_NUM_COLS
} dt_lib_collect_cols_t;

typedef struct _image_t
{
  int id;
  int filmid;
  gchar *path;
  gchar *filename;
  int exists;
} _image_t;

static void _lib_collect_gui_update(dt_lib_module_t *self);
static void _lib_folders_update_collection(const gchar *filmroll);
static void entry_changed(GtkWidget *entry, gchar *new_text, gint new_length, gpointer *position,
                          dt_lib_collect_rule_t *d);

const char *name()
{
  return _("collect images");
}

void init_presets(dt_lib_module_t *self)
{
}

static void row_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, dt_lib_collect_t *d);

/* Update the params struct with active ruleset */
static void _lib_collect_update_params(dt_lib_collect_t *d)
{
  /* reset params */
  dt_lib_collect_params_t *p = d->params;
  memset(p, 0, sizeof(dt_lib_collect_params_t));

  /* for each active rule set update params */
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules") - 1;
  const int active = CLAMP(_a, 0, (MAX_RULES - 1));
  char confname[200] = { 0 };
  for(int i = 0; i <= active; i++)
  {
    /* get item */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
    p->rule[i].item = dt_conf_get_int(confname);

    /* get mode */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i);
    p->rule[i].mode = dt_conf_get_int(confname);

    /* get string */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
    gchar *string = dt_conf_get_string(confname);
    if(string != NULL)
    {
      snprintf(p->rule[i].string, PARAM_STRING_SIZE, "%s", string);
      g_free(string);
    }

    // fprintf(stderr,"[%i] %d,%d,%s\n",i, p->rule[i].item, p->rule[i].mode,  p->rule[i].string);
  }

  p->rules = active + 1;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  _lib_collect_update_params(self->data);

  /* allocate a copy of params to return, freed by caller */
  *size = sizeof(dt_lib_collect_params_t);
  void *p = malloc(*size);
  memcpy(p, ((dt_lib_collect_t *)self->data)->params, *size);
  return p;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  /* update conf settings from params */
  dt_lib_collect_params_t *p = (dt_lib_collect_params_t *)params;
  char confname[200] = { 0 };

  for(uint32_t i = 0; i < p->rules; i++)
  {
    /* set item */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
    dt_conf_set_int(confname, p->rule[i].item);

    /* set mode */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i);
    dt_conf_set_int(confname, p->rule[i].mode);

    /* set string */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
    dt_conf_set_string(confname, p->rule[i].string);
  }

  /* set number of rules */
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/num_rules");
  dt_conf_set_int(confname, p->rules);

  /* update internal params */
  _lib_collect_update_params(self->data);

  /* update ui */
  _lib_collect_gui_update(self);

  /* update view */
  dt_collection_update_query(darktable.collection);

  return 0;
}


uint32_t views()
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_MAP;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}
/* callback for drag and drop */
/*static void _lib_keywords_drag_data_received_callback(GtkWidget *w,
            GdkDragContext *dctx,
            guint x,
            guint y,
            GtkSelectionData *data,
            guint info,
            guint time,
            gpointer user_data);
*/
/* set the data for drag and drop, eg the treeview path of drag source */
/*static void _lib_keywords_drag_data_get_callback(GtkWidget *w,
             GdkDragContext *dctx,
             GtkSelectionData *data,
             guint info,
             guint time,
             gpointer user_data);
*/
/* add keyword to collection rules */
/*static void _lib_keywords_add_collection_rule(GtkTreeView *view, GtkTreePath *tp,
                GtkTreeViewColumn *tvc, gpointer user_data);
*/

void _sync_list(gpointer *data, gpointer *user_data)
{
  _image_t *img = (_image_t *)data;

  if(img->exists == 0)
  {
    // remove file
    dt_image_remove(img->id);
    return;
  }

  if(img->id == -1)
  {
    // add file
    gchar *fullpath = NULL;
    fullpath = dt_util_dstrcat(fullpath, "%s/%s", img->path, img->filename);
    /* TODO: Check if JPEGs are set to be ignored */
    dt_image_import(img->filmid, fullpath, 1);
    g_free(fullpath);
    return;
  }
}

#if 0
void view_popup_menu_onSync (GtkWidget *menuitem, gpointer userdata)
{
  GtkTreeView *treeview = GTK_TREE_VIEW(userdata);
  GtkTreeSelection *selection;
  GtkTreeIter iter;
  GtkTreeModel *model;
  gchar *tree_path = NULL;
  gchar *query = NULL;
  sqlite3_stmt *stmt, *stmt2;
  GList *filelist = NULL;
  guint count_new = 0;
  guint count_found = 0;

  model = gtk_tree_view_get_model(treeview);
  selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
  gtk_tree_selection_get_selected(selection, &model, &iter);
  gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &tree_path, -1);

  query = dt_util_dstrcat(query, "select id,folder from film_rolls where folder like '%s%%'", tree_path);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  g_free(query);
  query = NULL;

  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    int film_id;
    gchar *path;
    GDir *dir;
    GError *error;

    film_id = sqlite3_column_int(stmt, 0);
    path = (gchar *) sqlite3_column_text(stmt, 1);

    query = dt_util_dstrcat(query, "select filename,id from images where film_id=%d", film_id);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt2, NULL);
    g_free(query);

    while (sqlite3_step(stmt2) == SQLITE_ROW)
    {
      _image_t *img = malloc(sizeof(_image_t));

      img->id = sqlite3_column_int(stmt, 1);
      img->filmid = film_id;
      img->path = path;
      img->filename = g_strdup((gchar *)sqlite3_column_text(stmt2, 0));
      img->exists = 0;

      filelist = g_list_prepend (filelist, (gpointer *)img);
      g_free(img->filename);
      g_free(img);
    }

    dir = g_dir_open(path, 0, &error);
    /* TODO: check here for error output */

    gboolean found = 0;

    /* TODO: what happens if there are new subdirs? */
    const gchar *name = g_dir_read_name(dir);
    while (name != NULL)
    {
      for (guint i=0; i<g_list_length(filelist); i++)
      {
        _image_t *tmp;
        tmp = g_list_nth_data(filelist, i);
        if(!g_strcmp0(tmp->filename, name))
        {
          // Should we check the path as well ??
          tmp->exists = 1;
          found = 1;
          count_found++;
          break;
        }
      }

      if (!found)
      {
        /* TODO: Check if file is supported.
         * If it is JPEG check if we should import it */
        _image_t *new = malloc(sizeof(_image_t));
        new->id = -1;
        new->path = g_strdup(path);
        new->filename = g_strdup(name);
        new->exists = 1;

        filelist = g_list_append(filelist, (gpointer *)new);

        count_new++;
      }

      name = g_dir_read_name(dir);
    }
  }

  /* Call now the foreach function that gives the total data */
  guint count_missing = g_list_length(filelist) - count_new - count_found;

  /* Produce the dialog */
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_message_dialog_new (GTK_WINDOW(win),
                      GTK_DIALOG_DESTROY_WITH_PARENT,
                      GTK_MESSAGE_QUESTION,
                      GTK_BUTTONS_YES_NO,
                      "_(There are %d new images and %d deleted images. Do you want to sync this folder?)", count_new,
                      count_missing);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    /* TODO: Get dialog returned options so we can choose only adding or deleting*/

    /* Proceed with sync */
    for (guint j=0; j < g_list_length(filelist); j++)
    {
      _image_t *img;
      img = (_image_t *)g_list_nth_data(filelist, j);
      if (img->id == -1)
      {
        /* This is a new image */
        gchar *filename = NULL;
        filename = dt_util_dstrcat(filename, "%s/%s", img->path, img->filename);

        if(dt_image_import(img->filmid, filename, 0))
          dt_control_queue_redraw_center();           //TODO: Set ignore JPEGs according to prefs.
      }
      else if (img->id != -1 && img->exists == 0)
      {
        dt_image_remove(img->id);
      }

    }

  }
  gtk_widget_destroy (dialog);

}
#endif

void view_popup_menu_onSearchFilmroll(GtkWidget *menuitem, gpointer userdata)
{
  GtkTreeView *treeview = GTK_TREE_VIEW(userdata);
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser;

  GtkTreeSelection *selection;
  GtkTreeIter iter, child;
  GtkTreeModel *model;

  gchar *tree_path = NULL;
  gchar *new_path = NULL;

  filechooser = gtk_file_chooser_dialog_new(
      _("search filmroll"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_Cancel"),
      GTK_RESPONSE_CANCEL, _("_Open"), GTK_RESPONSE_ACCEPT, (char *)NULL);

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  model = gtk_tree_view_get_model(treeview);
  selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
  gtk_tree_selection_get_selected(selection, &model, &iter);
  child = iter;
  gtk_tree_model_iter_parent(model, &iter, &child);
  gtk_tree_model_get(model, &child, DT_LIB_COLLECT_COL_PATH, &tree_path, -1);

  if(tree_path != NULL)
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), tree_path);
  else
    goto error;

  // run the dialog
  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gint id = -1;
    sqlite3_stmt *stmt;
    gchar *query = NULL;

    gchar *uri = NULL;
    uri = gtk_file_chooser_get_uri(GTK_FILE_CHOOSER(filechooser));
    new_path = g_filename_from_uri(uri, NULL, NULL);
    g_free(uri);
    if(new_path)
    {
      gchar *old = NULL;
      query = dt_util_dstrcat(query, "select id,folder from film_rolls where folder like '%s%%'", tree_path);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      g_free(query);
      query = NULL;

      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        id = sqlite3_column_int(stmt, 0);
        old = (gchar *)sqlite3_column_text(stmt, 1);

        query = NULL;
        query = dt_util_dstrcat(query, "update film_rolls set folder=?1 where id=?2");

        gchar trailing[1024] = { 0 };
        gchar final[1024] = { 0 };

        if(g_strcmp0(old, tree_path))
        {
          g_snprintf(trailing, sizeof(trailing), "%s", old + strlen(tree_path) + 1);
          g_snprintf(final, sizeof(final), "%s/%s", new_path, trailing);
        }
        else
        {
          g_snprintf(final, sizeof(final), "%s", new_path);
        }

        sqlite3_stmt *stmt2;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt2, NULL);
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 1, final, -1, SQLITE_STATIC);
        DT_DEBUG_SQLITE3_BIND_INT(stmt2, 2, id);
        sqlite3_step(stmt2);
        sqlite3_finalize(stmt2);
      }
      g_free(query);

      /* reset filter so that view isn't empty */
      dt_view_filter_reset(darktable.view_manager, FALSE);

      /* update collection to view missing filmroll */
      _lib_folders_update_collection(new_path);

      dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
    }
    else
      goto error;
  }
  g_free(tree_path);
  g_free(new_path);
  gtk_widget_destroy(filechooser);
  return;

error:
  /* Something wrong happened */
  gtk_widget_destroy(filechooser);
  dt_control_log(_("problem selecting new path for the filmroll in %s"), tree_path);

  g_free(tree_path);
  g_free(new_path);
}

void view_popup_menu_onRemove(GtkWidget *menuitem, gpointer userdata)
{
  GtkTreeView *treeview = GTK_TREE_VIEW(userdata);

  GtkTreeSelection *selection;
  GtkTreeIter iter;
  GtkTreeModel *model;

  gchar *filmroll_path = NULL;
  gchar *fullq = NULL;

  /* Get info about the filmroll (or parent) selected */
  model = gtk_tree_view_get_model(treeview);
  selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
  gtk_tree_selection_get_selected(selection, &model, &iter);
  gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &filmroll_path, -1);

  /* Clean selected images, and add to the table those which are going to be deleted */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "delete from selected_images", NULL, NULL, NULL);

  fullq = dt_util_dstrcat(fullq, "insert into selected_images select id from images where film_id  in "
                                 "(select id from film_rolls where folder like '%s%%')",
                          filmroll_path);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), fullq, NULL, NULL, NULL);

  dt_control_remove_images();
}

void view_popup_menu(GtkWidget *treeview, GdkEventButton *event, gpointer userdata)
{
  GtkWidget *menu, *menuitem;

  menu = gtk_menu_new();

  menuitem = gtk_menu_item_new_with_label(_("search filmroll..."));
  g_signal_connect(menuitem, "activate", (GCallback)view_popup_menu_onSearchFilmroll, treeview);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

#if 0
  menuitem = gtk_menu_item_new_with_label(_("sync..."));
  g_signal_connect(menuitem, "activate",
                   (GCallback) view_popup_menu_onSync, treeview);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
#endif

  menuitem = gtk_menu_item_new_with_label(_("remove..."));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
  g_signal_connect(menuitem, "activate", (GCallback)view_popup_menu_onRemove, treeview);

  gtk_widget_show_all(menu);

  /* Note: event can be NULL here when called from view_onPopupMenu;
   *  gdk_event_get_time() accepts a NULL argument */
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, (event != NULL) ? event->button : 0,
                 gdk_event_get_time((GdkEvent *)event));
}

gboolean view_onButtonPressed(GtkWidget *treeview, GdkEventButton *event, gpointer userdata)
{
  /* single click with the right mouse button? */
  if(event->type == GDK_BUTTON_PRESS && event->button == 3)
  {
    GtkTreeSelection *selection;

    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));

    /* Note: gtk_tree_selection_count_selected_rows() does not
     *   exist in gtk+-2.0, only in gtk+ >= v2.2 ! */
    if(gtk_tree_selection_count_selected_rows(selection) <= 1)
    {
      GtkTreePath *path;

      /* Get tree path for row that was clicked */
      if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), (gint)event->x, (gint)event->y, &path, NULL,
                                       NULL, NULL))
      {
        gtk_tree_selection_unselect_all(selection);
        gtk_tree_selection_select_path(selection, path);
        gtk_tree_path_free(path);
      }
    }
    view_popup_menu(treeview, event, userdata);

    return TRUE; /* we handled this */
  }
  return FALSE; /* we did not handle this */
}

gboolean view_onPopupMenu(GtkWidget *treeview, gpointer userdata)
{
  view_popup_menu(treeview, NULL, userdata);

  return TRUE; /* we handled this */
}

static int _count_images(const char *path)
{
  // FIXME: this function is a major performance problem
  //        if many folders are counted. until it's cached somehow, it's switched off:
  return 0;
#if 0
  sqlite3_stmt *stmt = NULL;
  gchar query[1024] = {0};
  int count = 0;

  gchar *escaped_text = NULL;
  escaped_text = dt_util_str_replace(path, "'", "''");

  snprintf (query, sizeof(query), "select count(id) from images where film_id in (select id from film_rolls where folder like '%s%%')", escaped_text);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if (sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  g_free(escaped_text);

  return count;
#endif
}

static gboolean _filmroll_is_present(const gchar *path)
{
  return g_file_test(path, G_FILE_TEST_IS_DIR);
}

static void _show_filmroll_present(GtkTreeViewColumn *column, GtkCellRenderer *renderer, GtkTreeModel *model,
                                   GtkTreeIter *iter, gpointer user_data)
{
  gchar *path, *pch;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &path, -1);
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_TEXT, &pch, -1);

  g_object_set(renderer, "text", pch, NULL);
  g_object_set(renderer, "strikethrough", TRUE, NULL);

  if(!_filmroll_is_present(path))
    g_object_set(renderer, "strikethrough-set", TRUE, NULL);
  else
    g_object_set(renderer, "strikethrough-set", FALSE, NULL);
}

static GtkTreeStore *_folder_tree()
{
  /* initialize the tree store */
  sqlite3_stmt *stmt;
  //  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select folder,external_drive from film_rolls
  //  order by folder desc", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select folder from film_rolls order by folder desc", -1, &stmt, NULL);
  GtkTreeStore *store = gtk_tree_store_new(DT_LIB_COLLECT_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
                                           G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN);

  // initialize the model with the paths

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int level = 0;
    char *value;
    GtkTreeIter current, iter;
    GtkTreePath *root;
    char *folder = (char *)sqlite3_column_text(stmt, 0);
    if(folder == NULL) continue; // safeguard against degenerated db entries
    char **pch = g_strsplit(folder, "/", -1);
#if 0
    char *external = g_strdup((char *)sqlite3_column_text(stmt, 1));

    if (external == NULL)
      external = g_strdup("Local");
#endif
    gboolean found = FALSE;

    root = gtk_tree_path_new_first();
    gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, root);
// current = iter; // This needs to be deleted if the following code is enabled
#if 0
    int children = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store),NULL);
    for (int k=0; k<children; k++)
    {
      if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(store), &iter, NULL, k))
      {
        gtk_tree_model_get (GTK_TREE_MODEL(store), &iter, 0, &value, -1);

        if (strcmp(value, external)==0)
        {
          found = TRUE;
          current = iter;
          break;
        }
      }
    }

    if (!found)
    {
      gtk_tree_store_insert(store, &iter, NULL, 0);
      gtk_tree_store_set(store, &iter, 0, external, -1);
      current = iter;
    }

    level=1;
#endif

    // g_strsplit returns pch[0] always as an empty string ""
    while(pch[level] != NULL)
    {
      found = FALSE;
      int children = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store), level > 0 ? &current : NULL);
      /* find child with name, if not found create and continue */
      for(int k = 0; k < children; k++)
      {
        if(gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(store), &iter, level > 0 ? &current : NULL, k))
        {
          gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, 0, &value, -1);

          if(strcmp(value, pch[level]) == 0)
          {
            current = iter;
            found = TRUE;
            break;
          }
        }
      }

      /* lets add new path and assign current */
      if(!found)
      {
        gchar *pth2 = NULL;
        pth2 = dt_util_dstrcat(pth2, "/");

        for(int i = 0; i <= level; i++)
        {
          if(level > 0 && i != 0) pth2 = dt_util_dstrcat(pth2, "%s/", pch[i]);
        }

        snprintf(pth2 + strlen(pth2) - 1, 1, "%s", "\0");

        int count = _count_images(pth2);
        gtk_tree_store_insert(store, &iter, level > 0 ? &current : NULL, 0);
        gtk_tree_store_set(store, &iter, DT_LIB_COLLECT_COL_TEXT, pch[level], DT_LIB_COLLECT_COL_PATH, pth2,
                           DT_LIB_COLLECT_COL_COUNT, count, DT_LIB_COLLECT_COL_VISIBLE, TRUE, -1);
        current = iter;
      }

      level++;
    }
    g_strfreev(pch);
  }
  return store;
}

static gboolean match_string(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  dt_lib_collect_rule_t *dr = (dt_lib_collect_rule_t *)data;
  gchar *str = NULL;
  gboolean cur_state, visible;

  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &str, DT_LIB_COLLECT_COL_VISIBLE, &cur_state, -1);

  if(dr->typing == FALSE && !cur_state)
  {
    visible = TRUE;
  }
  else
  {
    gchar *haystack = g_utf8_strdown(str, -1),
          *needle = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(dr->text)), -1);
    visible = (g_strrstr(haystack, needle) != NULL);
    g_free(haystack);
    g_free(needle);
  }

  gtk_tree_store_set(GTK_TREE_STORE(model), iter, DT_LIB_COLLECT_COL_VISIBLE, visible, -1);
  return FALSE;
}


static gboolean reveal_func(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  gboolean state;
  GtkTreeIter parent, child = *iter;
  gchar *str;

  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &str, DT_LIB_COLLECT_COL_VISIBLE, &state, -1);
  if(!state) return FALSE;

  while(gtk_tree_model_iter_parent(model, &parent, &child))
  {
    gtk_tree_model_get(model, &parent, DT_LIB_COLLECT_COL_PATH, &str, DT_LIB_COLLECT_COL_VISIBLE, &state, -1);
    gtk_tree_store_set(GTK_TREE_STORE(model), &parent, DT_LIB_COLLECT_COL_VISIBLE, TRUE, -1);
    child = parent;
  }

  return FALSE;
}

static gboolean expand_row(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, GtkTreeView *view)
{
  gboolean state;

  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_VISIBLE, &state, -1);

  if(state) gtk_tree_view_expand_to_path(view, path);

  return FALSE;
}

static void expand_tree(GtkTreeView *view, dt_lib_collect_rule_t *dr)
{
  GtkTreeModel *model = gtk_tree_view_get_model(view);

  if(dr->typing)
  {
    gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)expand_row, view);
  }
}

static void refilter(GtkTreeModel *model, gpointer data)
{
  gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)match_string, data);

  gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)reveal_func, NULL);
}


static GtkTreeModel *_create_filtered_model(GtkTreeModel *model, GtkTreeIter iter, dt_lib_collect_rule_t *dr)
{
  GtkTreeModel *filter = NULL;
  GtkTreePath *path;
  GtkTreeIter child;

  sqlite3_stmt *stmt = NULL;
  gchar *pth, *query = NULL;
  int id = -1;


  /* Filter level */
  while(gtk_tree_model_iter_has_child(model, &iter))
  {
    int n_children = gtk_tree_model_iter_n_children(model, &iter);
    if(n_children >= 1)
    {
      /* Check if this path also matches a filmroll */
      gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &pth, -1);

      query = dt_util_dstrcat(query, "select id from film_rolls where folder like '%s'", pth);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);

      g_free(pth);
      g_free(query);
      query = NULL;

      if(id != -1)
      {
        child = iter;
        gtk_tree_model_iter_parent(model, &iter, &child);
        break;
      }
    }

    if(n_children == 1)
    {
      gtk_tree_model_iter_children(model, &child, &iter);

      if(gtk_tree_model_iter_n_children(model, &child) != 0)
        iter = child;
      else
        break;
    }
    else
      break;
  }

  path = gtk_tree_model_get_path(model, &iter);

  /* Create filter and set virtual root */
  filter = gtk_tree_model_filter_new(model, path);
  gtk_tree_path_free(path);

  gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(filter), DT_LIB_COLLECT_COL_VISIBLE);

  if(dr != NULL) refilter(model, dr);

  return filter;
}

static GtkTreeView *_create_treeview_display(GtkTreeModel *model)
{
  GtkTreeView *tree;
  GtkCellRenderer *renderer;
  GtkTreeViewColumn *col1;

  tree = GTK_TREE_VIEW(gtk_tree_view_new());

  /* Create columns */
  col1 = gtk_tree_view_column_new();
  gtk_tree_view_append_column(tree, col1);
  gtk_tree_view_column_set_sizing(col1, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(col1, 230);
  gtk_tree_view_column_set_max_width(col1, 230);

  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col1, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col1, renderer, "text", DT_LIB_COLLECT_COL_TEXT);

  gtk_tree_view_column_set_cell_data_func(col1, renderer, _show_filmroll_present, NULL, NULL);

#if 0 // FIXME: count switched off for now, as it is a performance regression (see #8981).
  GtkTreeViewColumn *col2 = gtk_tree_view_column_new();
  gtk_tree_view_append_column(tree,col2);

  GtkCellRenderer *renderer2 = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col2, renderer2, TRUE);
  gtk_tree_view_column_add_attribute(col2, renderer2, "text", DT_LIB_COLLECT_COL_COUNT);
#endif

  gtk_tree_view_set_model(tree, GTK_TREE_MODEL(model));

  gtk_tree_view_set_headers_visible(tree, FALSE);

  /* free store, treeview has its own storage now */
  g_object_unref(model);

  return tree;
}

static void _lib_folders_update_collection(const gchar *filmroll)
{

  gchar *complete_query = NULL;

  // remove from selected images where not in this query.
  sqlite3_stmt *stmt = NULL;
  const gchar *cquery = dt_collection_get_query(darktable.collection);
  // complete_query = NULL;
  if(cquery && cquery[0] != '\0')
  {
    complete_query
        = dt_util_dstrcat(complete_query, "delete from selected_images where imgid not in (%s)", cquery);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), complete_query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* free allocated strings */
    g_free(complete_query);
  }

  /* raise signal of collection change, only if this is an original */
  if(!darktable.collection->clone) dt_control_signal_raise(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED);
}


#if 0
static void mount_changed (GVolumeMonitor *volume_monitor, GMount *mount, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_folders_t *d = (dt_lib_folders_t *)self->data;

  d->mounts = g_volume_monitor_get_mounts(d->gv_monitor);
  _draw_tree_gui(self);
}
#endif

void destroy_widget(gpointer data)
{
  GtkWidget *widget = (GtkWidget *)data;

  gtk_widget_destroy(widget);
}

static dt_lib_collect_t *get_collect(dt_lib_collect_rule_t *r)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)(((char *)r) - r->num * sizeof(dt_lib_collect_rule_t));
  return d;
}

static void set_properties(dt_lib_collect_rule_t *dr)
{
  int property = gtk_combo_box_get_active(dr->combo);
  const gchar *text = NULL;
  text = gtk_entry_get_text(GTK_ENTRY(dr->text));

  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", dr->num);
  dt_conf_set_string(confname, text);
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", dr->num);
  dt_conf_set_int(confname, property);
}

static void folders_view(dt_lib_collect_rule_t *dr)
{
  dt_lib_collect_t *d = get_collect(dr);

  GtkTreeView *tree;

  gtk_widget_hide(GTK_WIDGET(d->sw2));
  gtk_widget_hide(GTK_WIDGET(d->scrolledwindow));

  set_properties(dr);

  if(d->trees != NULL)
  {
    if(dr->typing == FALSE)
    {
      // Do nothing here
    }
    else
    {
      for(guint i = 0; i < d->trees->len; i++)
      {
        tree = GTK_TREE_VIEW(g_ptr_array_index(d->trees, i));
        GtkTreeModelFilter *modelfilter = GTK_TREE_MODEL_FILTER(gtk_tree_view_get_model(tree));
        GtkTreeModel *model = gtk_tree_model_filter_get_model(modelfilter);
        refilter(model, dr);
        expand_tree(tree, dr);
      }
    }
  }
  gtk_widget_show(GTK_WIDGET(d->box));
  gtk_widget_show(GTK_WIDGET(d->sw2));
}

static const char *UNCATEGORIZED_TAG = N_("uncategorized");
static void tags_view(dt_lib_collect_rule_t *dr)
{
  // update related list
  dt_lib_collect_t *d = get_collect(dr);
  sqlite3_stmt *stmt;
  GtkTreeIter uncategorized, temp;
  memset(&uncategorized, 0, sizeof(GtkTreeIter));

  GtkTreeView *view;
  GtkTreeModel *tagsmodel;

  view = d->view;
  tagsmodel = d->treemodel_tags;
  g_object_ref(tagsmodel);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);
  gtk_tree_store_clear(GTK_TREE_STORE(tagsmodel));
  gtk_widget_hide(GTK_WIDGET(d->scrolledwindow));
  gtk_widget_hide(GTK_WIDGET(d->sw2));

  set_properties(dr);

  /* query construction */
  char query[1024] = { 0 };
  const gchar *text = NULL;
  text = gtk_entry_get_text(GTK_ENTRY(dr->text));
  gchar *escaped_text = NULL;
  escaped_text = dt_util_str_replace(text, "'", "''");
  snprintf(query, sizeof(query),
           "SELECT distinct name, id, name LIKE '%%%s%%' FROM tags ORDER BY UPPER(name) DESC", escaped_text);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(strchr((const char *)sqlite3_column_text(stmt, 0), '|') == 0)
    {
      /* add uncategorized root iter if not exists */
      if(!uncategorized.stamp)
      {
        gtk_tree_store_insert(GTK_TREE_STORE(tagsmodel), &uncategorized, NULL, 0);
        gtk_tree_store_set(GTK_TREE_STORE(tagsmodel), &uncategorized, DT_LIB_COLLECT_COL_TEXT,
                           _(UNCATEGORIZED_TAG), DT_LIB_COLLECT_COL_PATH, "", DT_LIB_COLLECT_COL_VISIBLE,
                           FALSE, -1);
      }

      /* adding an uncategorized tag */
      gtk_tree_store_insert(GTK_TREE_STORE(tagsmodel), &temp, &uncategorized, 0);
      gtk_tree_store_set(GTK_TREE_STORE(tagsmodel), &temp, DT_LIB_COLLECT_COL_TEXT,
                         sqlite3_column_text(stmt, 0), DT_LIB_COLLECT_COL_PATH, sqlite3_column_text(stmt, 0),
                         DT_LIB_COLLECT_COL_VISIBLE, sqlite3_column_int(stmt, 2), -1);
    }
    else
    {
      int level = 0;
      char *value;
      GtkTreeIter current, iter;
      char **pch = g_strsplit((char *)sqlite3_column_text(stmt, 0), "|", -1);

      if(pch != NULL)
      {
        int max_level = 0;
        int j = 0;
        while(pch[j] != NULL)
        {
          max_level++;
          j++;
        }
        max_level--;
        j = 0;
        while(pch[j] != NULL)
        {
          gboolean found = FALSE;
          int children = gtk_tree_model_iter_n_children(tagsmodel, level > 0 ? &current : NULL);
          /* find child with name, if not found create and continue */
          for(int k = 0; k < children; k++)
          {
            if(gtk_tree_model_iter_nth_child(tagsmodel, &iter, level > 0 ? &current : NULL, k))
            {
              gtk_tree_model_get(tagsmodel, &iter, 0, &value, -1);

              if(strcmp(value, pch[j]) == 0)
              {
                current = iter;
                found = TRUE;
                break;
              }
            }
          }

          /* lets add new keyword and assign current */
          if(!found && pch[j] && *pch[j])
          {
            gchar *pth2 = NULL;
            pth2 = dt_util_dstrcat(pth2, "");

            for(int i = 0; i <= level; i++)
            {
              pth2 = dt_util_dstrcat(pth2, "%s|", pch[i]);
            }
            if(level == max_level)
              pth2[strlen(pth2) - 1] = '\0';
            else
              pth2 = dt_util_dstrcat(pth2, "%%");

            int count = _count_images(pth2);
            gtk_tree_store_insert(GTK_TREE_STORE(tagsmodel), &iter, level > 0 ? &current : NULL, 0);
            gtk_tree_store_set(GTK_TREE_STORE(tagsmodel), &iter, DT_LIB_COLLECT_COL_TEXT, pch[j],
                               DT_LIB_COLLECT_COL_PATH, pth2, DT_LIB_COLLECT_COL_COUNT, count,
                               DT_LIB_COLLECT_COL_VISIBLE, sqlite3_column_int(stmt, 2), -1);
            current = iter;
          }

          level++;
          j++;
        }

        g_strfreev(pch);
      }
    }
  }
  sqlite3_finalize(stmt);

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(view), DT_LIB_COLLECT_COL_TOOLTIP);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), tagsmodel);
  gtk_widget_set_no_show_all(GTK_WIDGET(d->scrolledwindow), FALSE);
  gtk_widget_show_all(GTK_WIDGET(d->scrolledwindow));

  if(text[0] == '\0')
  {
    if(uncategorized.stamp)
    {
      GtkTreePath *path = gtk_tree_model_get_path(tagsmodel, &uncategorized);
      gtk_tree_view_expand_row(GTK_TREE_VIEW(view), path, TRUE);
      gtk_tree_path_free(path);
    }
  }
  else
    gtk_tree_model_foreach(tagsmodel, (GtkTreeModelForeachFunc)expand_row, view);
  g_object_unref(tagsmodel);
}

static void list_view(dt_lib_collect_rule_t *dr)
{
  // update related list
  dt_lib_collect_t *d = get_collect(dr);
  sqlite3_stmt *stmt;
  GtkTreeIter iter;

  GtkTreeView *view;
  GtkTreeModel *listmodel;

  view = d->view;
  listmodel = d->listmodel;
  g_object_ref(listmodel);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);
  gtk_list_store_clear(GTK_LIST_STORE(listmodel));
  gtk_widget_hide(GTK_WIDGET(d->scrolledwindow));
  gtk_widget_hide(GTK_WIDGET(d->sw2));

  set_properties(dr);

  char query[1024] = { 0 };
  int property = gtk_combo_box_get_active(dr->combo);
  const gchar *text = NULL;
  text = gtk_entry_get_text(GTK_ENTRY(dr->text));
  gchar *escaped_text = NULL;

  escaped_text = dt_util_str_replace(text, "'", "''");

  switch(property)
  {
    case DT_COLLECTION_PROP_FILMROLL: // film roll
      snprintf(query, sizeof(query),
               "select distinct folder, id from film_rolls where folder like '%%%s%%'  order by folder desc",
               escaped_text);
      break;
    case DT_COLLECTION_PROP_CAMERA: // camera
      snprintf(query, sizeof(query), "select distinct maker || ' ' || model as model, 1 from images where "
                                     "maker || ' ' || model like '%%%s%%' order by model",
               escaped_text);
      break;
    case DT_COLLECTION_PROP_TAG: // tag
      snprintf(query, sizeof(query),
               "SELECT distinct name, id FROM tags WHERE name LIKE '%%%s%%' ORDER BY UPPER(name)",
               escaped_text);
      break;
    case DT_COLLECTION_PROP_HISTORY: // History, 2 hardcoded alternatives
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set(GTK_LIST_STORE(listmodel), &iter, DT_LIB_COLLECT_COL_TEXT, _("altered"),
                         DT_LIB_COLLECT_COL_ID, 0, DT_LIB_COLLECT_COL_TOOLTIP, _("altered"), -1);
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set(GTK_LIST_STORE(listmodel), &iter, DT_LIB_COLLECT_COL_TEXT, _("not altered"),
                         DT_LIB_COLLECT_COL_ID, 1, DT_LIB_COLLECT_COL_TOOLTIP, _("not altered"), -1);
      goto entry_key_press_exit;
      break;

    case DT_COLLECTION_PROP_GEOTAGGING: // Geotagging, 2 hardcoded alternatives
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set(GTK_LIST_STORE(listmodel), &iter, DT_LIB_COLLECT_COL_TEXT, _("tagged"),
                         DT_LIB_COLLECT_COL_ID, 0, DT_LIB_COLLECT_COL_TOOLTIP, _("tagged"), -1);
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set(GTK_LIST_STORE(listmodel), &iter, DT_LIB_COLLECT_COL_TEXT, _("not tagged"),
                         DT_LIB_COLLECT_COL_ID, 1, DT_LIB_COLLECT_COL_TOOLTIP, _("not tagged"), -1);
      goto entry_key_press_exit;
      break;

    case DT_COLLECTION_PROP_COLORLABEL: // colorlabels
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set(GTK_LIST_STORE(listmodel), &iter, DT_LIB_COLLECT_COL_TEXT, _("red"),
                         DT_LIB_COLLECT_COL_ID, 0, DT_LIB_COLLECT_COL_TOOLTIP, _("red"), -1);
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set(GTK_LIST_STORE(listmodel), &iter, DT_LIB_COLLECT_COL_TEXT, _("yellow"),
                         DT_LIB_COLLECT_COL_ID, 1, DT_LIB_COLLECT_COL_TOOLTIP, _("yellow"), -1);
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set(GTK_LIST_STORE(listmodel), &iter, DT_LIB_COLLECT_COL_TEXT, _("green"),
                         DT_LIB_COLLECT_COL_ID, 2, DT_LIB_COLLECT_COL_TOOLTIP, _("green"), -1);
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set(GTK_LIST_STORE(listmodel), &iter, DT_LIB_COLLECT_COL_TEXT, _("blue"),
                         DT_LIB_COLLECT_COL_ID, 3, DT_LIB_COLLECT_COL_TOOLTIP, _("blue"), -1);
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set(GTK_LIST_STORE(listmodel), &iter, DT_LIB_COLLECT_COL_TEXT, _("purple"),
                         DT_LIB_COLLECT_COL_ID, 4, DT_LIB_COLLECT_COL_TOOLTIP, _("purple"), -1);
      goto entry_key_press_exit;
      break;

    // TODO: Add empty string for metadata?
    // TODO: Autogenerate this code?
    case DT_COLLECTION_PROP_TITLE: // title
      snprintf(
          query, sizeof(query),
          "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%' order by value",
          DT_METADATA_XMP_DC_TITLE, escaped_text);
      break;
    case DT_COLLECTION_PROP_DESCRIPTION: // description
      snprintf(
          query, sizeof(query),
          "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%' order by value",
          DT_METADATA_XMP_DC_DESCRIPTION, escaped_text);
      break;
    case DT_COLLECTION_PROP_CREATOR: // creator
      snprintf(
          query, sizeof(query),
          "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%' order by value",
          DT_METADATA_XMP_DC_CREATOR, escaped_text);
      break;
    case DT_COLLECTION_PROP_PUBLISHER: // publisher
      snprintf(
          query, sizeof(query),
          "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%' order by value",
          DT_METADATA_XMP_DC_PUBLISHER, escaped_text);
      break;
    case DT_COLLECTION_PROP_RIGHTS: // rights
      snprintf(
          query, sizeof(query),
          "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%'order by value ",
          DT_METADATA_XMP_DC_RIGHTS, escaped_text);
      break;
    case DT_COLLECTION_PROP_LENS: // lens
      snprintf(query, sizeof(query),
               "select distinct lens, 1 from images where lens like '%%%s%%' order by lens", escaped_text);
      break;
    case DT_COLLECTION_PROP_ISO: // iso
    {
      gchar *operator, *number;
      dt_collection_split_operator_number(escaped_text, &number, &operator);

      if(operator&& number)
        snprintf(query, sizeof(query),
                 "select distinct cast(iso as integer) as iso, 1 from images where iso %s %s order by iso",
                 operator, number);
      else if(number)
        snprintf(query, sizeof(query),
                 "select distinct cast(iso as integer) as iso, 1 from images where iso = %s order by iso",
                 number);
      else
        snprintf(
            query, sizeof(query),
            "select distinct cast(iso as integer) as iso, 1 from images where iso like '%%%s%%' order by iso",
            escaped_text);

      g_free(operator);
      g_free(number);
    }
    break;

    case DT_COLLECTION_PROP_APERTURE: // aperture
    {
      gchar *operator, *number;
      dt_collection_split_operator_number(escaped_text, &number, &operator);

      if(operator&& number)
        snprintf(query, sizeof(query), "select distinct round(aperture,1) as aperture, 1 from images where "
                                       "aperture %s %s order by aperture",
                                       operator, number);
      else if(number)
        snprintf(query, sizeof(query), "select distinct round(aperture,1) as aperture, 1 from images where "
                                       "aperture = %s order by aperture",
                 number);
      else
        snprintf(query, sizeof(query), "select distinct round(aperture,1) as aperture, 1 from images where "
                                       "aperture like '%%%s%%' order by aperture",
                 escaped_text);

      g_free(operator);
      g_free(number);
    }
    break;

    case DT_COLLECTION_PROP_FILENAME: // filename
      snprintf(query, sizeof(query),
               "select distinct filename, 1 from images where filename like '%%%s%%' order by filename",
               escaped_text);
      break;

    case DT_COLLECTION_PROP_FOLDERS: // folders
      // We shouldn't ever be here
      break;

    case DT_COLLECTION_PROP_DAY:
      snprintf(query, sizeof(query), "SELECT DISTINCT substr(datetime_taken, 1, 10), 1 FROM images WHERE "
                                     "datetime_taken LIKE '%%%s%%' ORDER BY datetime_taken DESC",
               escaped_text);
      break;

    default: // time
      snprintf(query, sizeof(query), "SELECT DISTINCT datetime_taken, 1 FROM images WHERE datetime_taken "
                                     "LIKE '%%%s%%' ORDER BY datetime_taken DESC",
               escaped_text);
      break;
  }
  g_free(escaped_text);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
    const char *folder = (const char *)sqlite3_column_text(stmt, 0);
    if(property == 0) // film roll
    {
      folder = dt_image_film_roll_name(folder);
    }
    gchar *value = (gchar *)sqlite3_column_text(stmt, 0);

    // replace invalid utf8 characters if any
    gchar *text = g_strdup(value);
    gchar *ptr = text;
    while(!g_utf8_validate(ptr, -1, (const gchar **)&ptr)) ptr[0] = '?';

    gchar *escaped_text = g_markup_escape_text(text, -1);


    gtk_list_store_set(GTK_LIST_STORE(listmodel), &iter, DT_LIB_COLLECT_COL_TEXT, folder,
                       DT_LIB_COLLECT_COL_ID, sqlite3_column_int(stmt, 1), DT_LIB_COLLECT_COL_TOOLTIP,
                       escaped_text, DT_LIB_COLLECT_COL_PATH, value, -1);
    g_free(text);
    g_free(escaped_text);
  }
  sqlite3_finalize(stmt);

  goto entry_key_press_exit;

entry_key_press_exit:
  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(view), DT_LIB_COLLECT_COL_TOOLTIP);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), listmodel);
  gtk_widget_set_no_show_all(GTK_WIDGET(d->scrolledwindow), FALSE);
  gtk_widget_show_all(GTK_WIDGET(d->scrolledwindow));
  g_object_unref(listmodel);
}

static void update_view(GtkEntry *entry, dt_lib_collect_rule_t *dr)
{
  int property = gtk_combo_box_get_active(dr->combo);

  if(property == DT_COLLECTION_PROP_FOLDERS)
    folders_view(dr);
  else if(property == DT_COLLECTION_PROP_TAG)
    tags_view(dr);
  else
    list_view(dr);
}

static void create_folders_gui(dt_lib_collect_rule_t *dr)
{
  GtkTreeView *tree;
  GtkTreeModel *treemodel_folders;
  GtkTreeIter iter;
  dt_lib_collect_t *d = get_collect(dr);

  dt_lib_collect_rule_t *rule = NULL;

  treemodel_folders = d->treemodel_folders;

  if(d->tree_new)
  {
/* We have already inited the GUI once, clean around */
#if 0
    if (d->labels != NULL)
    {
      for (int i=0; i<d->labels->len; i++)
      {
        label = GTK_WIDGET(g_ptr_array_index (d->labels, i));
        g_ptr_array_free(d->labels, TRUE);
      }
      d->labels = NULL;
    }
#endif

    if(d->trees != NULL)
    {
      for(guint i = 0; i < d->trees->len; i++)
      {
        tree = GTK_TREE_VIEW(g_ptr_array_index(d->trees, i));
        g_ptr_array_free(d->trees, TRUE); // FIXME: this looks strange, should that be s/d->trees/tree/ ?
      }
      d->trees = NULL;
    }

    /* set the UI */
    GtkTreeModel *model2;

    GtkTreePath *root = gtk_tree_path_new_first();
    if(!gtk_tree_model_get_iter(GTK_TREE_MODEL(treemodel_folders), &iter, root))
      // something went wrong, get out.
      return;
    int children = 1; // To be deleted if the following code in enabled
#if 0
    int children = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(treemodel_folders), NULL);
    d->labels = g_ptr_array_sized_new(children);
    g_ptr_array_set_free_func (d->labels, destroy_widget);
#endif
    d->trees = g_ptr_array_sized_new(children);
    g_ptr_array_set_free_func(d->trees, destroy_widget);

    for(int i = 0; i < children; i++)
    {
#if 0
      GValue value;
      memset(&value,0,sizeof(GValue));
      gtk_tree_model_iter_nth_child (GTK_TREE_MODEL(treemodel_folders), &iter, NULL, i);

      gtk_tree_model_get_value (GTK_TREE_MODEL(treemodel_folders), &iter, 0, &value);

      gchar *mount_name = g_value_dup_string(&value);

      if (g_strcmp0(mount_name, "Local")==0)
      {
        label = gtk_label_new (_("local HDD"));
      }
      else
      {
        label = gtk_label_new (g_ascii_strdown(mount_name, -1));
      }
      g_ptr_array_add(d->labels, (gpointer) label);
      gtk_box_pack_start(d->box, GTK_WIDGET(label), FALSE, FALSE, 0);
      gtk_widget_show (label);
#endif
      /* Only pass a rule (and filter the tree) if the typing property is TRUE */
      if(dr->typing != FALSE) rule = dr;
      model2 = _create_filtered_model(GTK_TREE_MODEL(treemodel_folders), iter, rule);
      tree = _create_treeview_display(GTK_TREE_MODEL(model2));
      g_ptr_array_add(d->trees, (gpointer)tree);
      gtk_box_pack_start(d->box, GTK_WIDGET(tree), FALSE, FALSE, 0);
      gtk_widget_show(GTK_WIDGET(tree));

      gtk_tree_view_set_headers_visible(tree, FALSE);

      gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tree), GTK_SELECTION_SINGLE);

      gtk_tree_view_set_enable_search(tree, TRUE);
      gtk_tree_view_set_search_column(tree, DT_LIB_COLLECT_COL_PATH);

      g_signal_connect(G_OBJECT(tree), "row-activated", G_CALLBACK(row_activated), d);
      g_signal_connect(G_OBJECT(tree), "button-press-event", G_CALLBACK(view_onButtonPressed), NULL);
      g_signal_connect(G_OBJECT(tree), "popup-menu", G_CALLBACK(view_onPopupMenu), NULL);

#if 0
      g_value_unset(&value);
      g_free(mount_name);
#endif
      d->tree_new = FALSE;
    }
  }
}


static void _lib_collect_gui_update(dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;

  const int old = darktable.gui->reset;
  darktable.gui->reset = 1;
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules") - 1;
  const int active = CLAMP(_a, 0, (MAX_RULES - 1));
  char confname[200] = { 0 };

  gtk_widget_set_no_show_all(GTK_WIDGET(d->scrolledwindow), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(d->sw2), TRUE);

  for(int i = 0; i < MAX_RULES; i++)
  {
    gtk_widget_set_no_show_all(d->rule[i].hbox, TRUE);
    gtk_widget_set_visible(d->rule[i].hbox, FALSE);
  }
  for(int i = 0; i <= active; i++)
  {
    gtk_widget_set_no_show_all(d->rule[i].hbox, FALSE);
    gtk_widget_set_visible(d->rule[i].hbox, TRUE);
    gtk_widget_show_all(d->rule[i].hbox);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->rule[i].combo), dt_conf_get_int(confname));
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
    gchar *text = dt_conf_get_string(confname);
    if(text)
    {
      g_signal_handlers_block_matched(d->rule[i].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, update_view, NULL);
      g_signal_handlers_block_matched(d->rule[i].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
      gtk_entry_set_text(GTK_ENTRY(d->rule[i].text), text);
      gtk_editable_set_position(GTK_EDITABLE(d->rule[i].text), -1);
      g_signal_handlers_unblock_matched(d->rule[i].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, update_view, NULL);
      g_signal_handlers_unblock_matched(d->rule[i].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
      g_free(text);
      d->rule[i].typing = FALSE;
    }

    GtkDarktableButton *button = DTGTK_BUTTON(d->rule[i].button);
    if(i == MAX_RULES - 1)
    {
      // only clear
      button->icon = dtgtk_cairo_paint_cancel;
      g_object_set(G_OBJECT(button), "tooltip-text", _("clear this rule"), (char *)NULL);
    }
    else if(i == active)
    {
      button->icon = dtgtk_cairo_paint_dropdown;
      g_object_set(G_OBJECT(button), "tooltip-text", _("clear this rule or add new rules"), (char *)NULL);
    }
    else
    {
      snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i + 1);
      const int mode = dt_conf_get_int(confname);
      if(mode == DT_LIB_COLLECT_MODE_AND) button->icon = dtgtk_cairo_paint_and;
      if(mode == DT_LIB_COLLECT_MODE_OR) button->icon = dtgtk_cairo_paint_or;
      if(mode == DT_LIB_COLLECT_MODE_AND_NOT) button->icon = dtgtk_cairo_paint_andnot;
      g_object_set(G_OBJECT(button), "tooltip-text", _("clear this rule"), (char *)NULL);
    }
  }

  // update list of proposals
  create_folders_gui(d->rule + d->active_rule);
  update_view(NULL, d->rule + d->active_rule);
  darktable.gui->reset = old;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
  dt_conf_set_int("plugins/lighttable/collect/item0", DT_COLLECTION_PROP_FILMROLL);
  dt_conf_set_string("plugins/lighttable/collect/string0", "");
  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);
  dt_collection_update_query(darktable.collection);
}

static void combo_changed(GtkComboBox *combo, dt_lib_collect_rule_t *d)
{
  if(darktable.gui->reset) return;
  g_signal_handlers_block_matched(d->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
  gtk_entry_set_text(GTK_ENTRY(d->text), "");
  g_signal_handlers_unblock_matched(d->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
  dt_lib_collect_t *c = get_collect(d);
  c->active_rule = d->num;

  int property = gtk_combo_box_get_active(d->combo);

  if(property == DT_COLLECTION_PROP_FOLDERS)
  {
    d->typing = FALSE;
    refilter(c->treemodel_folders, d);
  }
  else if(property == DT_COLLECTION_PROP_TAG)
  {
    d->typing = FALSE;
    refilter(c->treemodel_tags, d);
  }


  update_view(NULL, d);
  dt_collection_update_query(darktable.collection);
}

static void row_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, dt_lib_collect_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  gchar *text;

  const int active = d->active_rule;
  d->rule[active].typing = FALSE;

  const int item = gtk_combo_box_get_active(GTK_COMBO_BOX(d->rule[active].combo));
  if(item == DT_COLLECTION_PROP_FILMROLL || // get full path for film rolls
     item == DT_COLLECTION_PROP_TAG ||      // or tags
     item == DT_COLLECTION_PROP_FOLDERS)    // or folders
    gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &text, -1);
  else
    gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_TEXT, &text, -1);

  g_signal_handlers_block_matched(d->rule[active].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
  gtk_entry_set_text(GTK_ENTRY(d->rule[active].text), text);
  gtk_editable_set_position(GTK_EDITABLE(d->rule[active].text), -1);
  g_signal_handlers_unblock_matched(d->rule[active].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed,
                                    NULL);
  g_free(text);

  update_view(NULL, d->rule + active);
  dt_collection_update_query(darktable.collection);
  dt_control_queue_redraw_center();
}

static void entry_activated(GtkWidget *entry, dt_lib_collect_rule_t *d)
{
  GtkTreeView *view;
  GtkTreeModel *model;
  int property, rows;

  update_view(NULL, d);
  dt_lib_collect_t *c = get_collect(d);

  property = gtk_combo_box_get_active(d->combo);

  if(property != DT_COLLECTION_PROP_FOLDERS && property != DT_COLLECTION_PROP_TAG)
  {
    view = c->view;
    model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));

    rows = gtk_tree_model_iter_n_children(model, NULL);

    if(rows == 1)
    {
      GtkTreeIter iter;
      if(gtk_tree_model_get_iter_first(model, &iter))
      {
        gchar *text;
        const int item = gtk_combo_box_get_active(GTK_COMBO_BOX(d->combo));
        if(item == DT_COLLECTION_PROP_FILMROLL || // get full path for film rolls
           item == DT_COLLECTION_PROP_TAG || item == DT_COLLECTION_PROP_FOLDERS) // or folders
          gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &text, -1);
        else
          gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_TEXT, &text, -1);

        g_signal_handlers_block_matched(d->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
        gtk_entry_set_text(GTK_ENTRY(d->text), text);
        gtk_editable_set_position(GTK_EDITABLE(d->text), -1);
        g_signal_handlers_unblock_matched(d->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
        g_free(text);
        d->typing = FALSE;
        update_view(NULL, d);
      }
    }
  }
  dt_collection_update_query(darktable.collection);
}

static void entry_changed(GtkWidget *entry, gchar *new_text, gint new_length, gpointer *position,
                          dt_lib_collect_rule_t *d)
{
  d->typing = TRUE;
}

int position()
{
  return 400;
}

static void entry_focus_in_callback(GtkWidget *w, GdkEventFocus *event, dt_lib_collect_rule_t *d)
{
  dt_lib_collect_t *c = get_collect(d);
  c->active_rule = d->num;
  update_view(NULL, c->rule + c->active_rule);
}

#if 0
static void
focus_in_callback (GtkWidget *w, GdkEventFocus *event, dt_lib_module_t *self)
{
  GtkWidget *win = darktable.gui->widgets.main_window;
  GtkEntry *entry = GTK_ENTRY(self->text);
  GtkTreeView *view;
  int count = 1 + count_film_rolls(gtk_entry_get_text(entry));
  int ht = get_font_height(view, "Dreggn");
  const int size = MAX(2*ht, MIN(win->allocation.height/2, count*ht));
  gtk_widget_set_size_request(view, -1, size);
}

static void
hide_callback (GObject    *object,
               GParamSpec *param_spec,
               GtkWidget *view)
{
  GtkExpander *expander;
  expander = GTK_EXPANDER (object);
  if (!gtk_expander_get_expanded (expander))
    gtk_widget_set_size_request(view, -1, -1);
}
#endif

static void menuitem_and(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with and operator
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int active = CLAMP(_a, 1, MAX_RULES);
  if(active < 10)
  {
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", active);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", active);
    dt_conf_set_string(confname, "");
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active + 1);
    dt_lib_collect_t *c = get_collect(d);
    c->active_rule = active;
  }
  dt_collection_update_query(darktable.collection);
}

static void menuitem_or(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with or operator
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int active = CLAMP(_a, 1, MAX_RULES);
  if(active < 10)
  {
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", active);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_OR);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", active);
    dt_conf_set_string(confname, "");
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active + 1);
    dt_lib_collect_t *c = get_collect(d);
    c->active_rule = active;
  }
  dt_collection_update_query(darktable.collection);
}

static void menuitem_and_not(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with and not operator
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int active = CLAMP(_a, 1, MAX_RULES);
  if(active < 10)
  {
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", active);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND_NOT);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", active);
    dt_conf_set_string(confname, "");
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active + 1);
    dt_lib_collect_t *c = get_collect(d);
    c->active_rule = active;
  }
  dt_collection_update_query(darktable.collection);
}

static void menuitem_change_and(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with and operator
  const int num = d->num + 1;
  if(num < 10 && num > 0)
  {
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", num);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND);
  }
  dt_collection_update_query(darktable.collection);
}

static void menuitem_change_or(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with or operator
  const int num = d->num + 1;
  if(num < 10 && num > 0)
  {
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", num);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_OR);
  }
  dt_collection_update_query(darktable.collection);
}

static void menuitem_change_and_not(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with and not operator
  const int num = d->num + 1;
  if(num < 10 && num > 0)
  {
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", num);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND_NOT);
  }
  dt_collection_update_query(darktable.collection);
}

static void collection_updated(gpointer instance, gpointer self)
{
  _lib_collect_gui_update(self);
}


static void filmrolls_updated(gpointer instance, gpointer self)
{
  //  dt_lib_module_t *dm = (dt_lib_module_t *)self;

  //  dt_lib_collect_t *d = (dt_lib_collect_t *)dm->data;
  //  int active = d->active_rule;

  // TODO: We should update the count of images here
  _lib_collect_gui_update(self);
}

static void filmrolls_imported(gpointer instance, int film_id, gpointer self)
{
  dt_lib_module_t *dm = (dt_lib_module_t *)self;

  dt_lib_collect_t *d = (dt_lib_collect_t *)dm->data;
  int active = 0;
  d->active_rule = active;

  // update tree
  d->treemodel_folders = GTK_TREE_MODEL(_folder_tree());
  d->tree_new = TRUE;
  d->rule[active].typing = FALSE;

  // reset active rules
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
  dt_conf_set_int("plugins/lighttable/collect/item0", DT_COLLECTION_PROP_FILMROLL);
  _lib_collect_gui_update(self);
}

static void filmrolls_removed(gpointer instance, gpointer self)
{
  dt_lib_module_t *dm = (dt_lib_module_t *)self;

  dt_lib_collect_t *d = (dt_lib_collect_t *)dm->data;
  int active = 0;
  d->active_rule = active;

  // update tree
  d->treemodel_folders = GTK_TREE_MODEL(_folder_tree());
  d->tree_new = TRUE;
  d->rule[active].typing = FALSE;

  // reset active rules
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
  dt_conf_set_int("plugins/lighttable/collect/item0", DT_COLLECTION_PROP_FILMROLL);
  dt_conf_set_string("plugins/lighttable/collect/string0", "");
  _lib_collect_gui_update(self);
}

static void menuitem_clear(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // remove this row, or if 1st, clear text entry box
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int active = CLAMP(_a, 1, MAX_RULES);
  dt_lib_collect_t *c = get_collect(d);
  if(active > 1)
  {
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active - 1);
    if(c->active_rule >= active - 1) c->active_rule = active - 2;
  }
  else
  {
    dt_conf_set_int("plugins/lighttable/collect/mode0", DT_LIB_COLLECT_MODE_AND);
    dt_conf_set_int("plugins/lighttable/collect/item0", 0);
    dt_conf_set_string("plugins/lighttable/collect/string0", "");
    d->typing = FALSE;
    gtk_combo_box_set_active(d->combo, 0);
    gtk_entry_set_text(GTK_ENTRY(d->text), "");
  }
  // move up all still active rules by one.
  for(int i = d->num; i < MAX_RULES - 1; i++)
  {
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i + 1);
    const int mode = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i + 1);
    const int item = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i + 1);
    gchar *string = dt_conf_get_string(confname);
    if(string)
    {
      snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i);
      dt_conf_set_int(confname, mode);
      snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
      dt_conf_set_int(confname, item);
      snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
      dt_conf_set_string(confname, string);
      g_free(string);
    }
  }
  refilter(c->treemodel_folders, d);

  dt_collection_update_query(darktable.collection);
}

static gboolean popup_button_callback(GtkWidget *widget, GdkEventButton *event, dt_lib_collect_rule_t *d)
{
  if(event->button != 1) return FALSE;

  GtkWidget *menu = gtk_menu_new();
  GtkWidget *mi;
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int active = CLAMP(_a, 1, MAX_RULES);

  mi = gtk_menu_item_new_with_label(_("clear this rule"));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_clear), d);

  if(d->num == active - 1)
  {
    mi = gtk_menu_item_new_with_label(_("narrow down search"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_and), d);

    mi = gtk_menu_item_new_with_label(_("add more images"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_or), d);

    mi = gtk_menu_item_new_with_label(_("exclude images"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_and_not), d);
  }
  else if(d->num < active - 1)
  {
    mi = gtk_menu_item_new_with_label(_("change to: and"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_change_and), d);

    mi = gtk_menu_item_new_with_label(_("change to: or"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_change_or), d);

    mi = gtk_menu_item_new_with_label(_("change to: except"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_change_and_not), d);
  }

  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, event->button, event->time);
  gtk_widget_show_all(menu);

  return TRUE;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)calloc(1, sizeof(dt_lib_collect_t));

  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  //   gtk_widget_set_size_request(self->widget, 100, -1);
  d->active_rule = 0;
  d->params = (dt_lib_collect_params_t *)malloc(sizeof(dt_lib_collect_params_t));

  GtkBox *box;
  GtkWidget *w;

  for(int i = 0; i < MAX_RULES; i++)
  {
    d->rule[i].num = i;
    d->rule[i].typing = FALSE;
    box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5));
    d->rule[i].hbox = GTK_WIDGET(box);
    gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);
    w = gtk_combo_box_text_new();
    d->rule[i].combo = GTK_COMBO_BOX(w);
    for(int k = 0; k < dt_lib_collect_string_cnt; k++)
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w), _(dt_lib_collect_string[k]));
    g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(combo_changed), d->rule + i);
    gtk_box_pack_start(box, w, FALSE, FALSE, 0);

    w = gtk_entry_new();
    d->rule[i].text = w;
    dt_gui_key_accel_block_on_focus_connect(d->rule[i].text);
    gtk_widget_add_events(w, GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(G_OBJECT(w), "focus-in-event", G_CALLBACK(entry_focus_in_callback), d->rule + i);

    /* xgettext:no-c-format */
    g_object_set(G_OBJECT(w), "tooltip-text", _("type your query, use `%' as wildcard"), (char *)NULL);
    gtk_widget_add_events(w, GDK_KEY_PRESS_MASK);
    g_signal_connect(G_OBJECT(w), "insert-text", G_CALLBACK(entry_changed), d->rule + i);
    g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(update_view), d->rule + i);
    g_signal_connect(G_OBJECT(w), "activate", G_CALLBACK(entry_activated), d->rule + i);
    gtk_box_pack_start(box, w, TRUE, TRUE, 0);
    gtk_entry_set_width_chars(GTK_ENTRY(w), 0);

    w = dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER);
    d->rule[i].button = w;
    gtk_widget_set_events(w, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(G_OBJECT(w), "button-press-event", G_CALLBACK(popup_button_callback), d->rule + i);
    gtk_box_pack_start(box, w, FALSE, FALSE, 0);
    gtk_widget_set_size_request(w, DT_PIXEL_APPLY_DPI(13), DT_PIXEL_APPLY_DPI(13));
  }

  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  d->scrolledwindow = GTK_SCROLLED_WINDOW(sw);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(d->scrolledwindow), DT_PIXEL_APPLY_DPI(300));
  GtkTreeView *view = GTK_TREE_VIEW(gtk_tree_view_new());
  d->view = view;
  gtk_tree_view_set_headers_visible(view, FALSE);
  gtk_container_add(GTK_CONTAINER(sw), GTK_WIDGET(view));
  g_signal_connect(G_OBJECT(view), "row-activated", G_CALLBACK(row_activated), d);

  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(view, col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", DT_LIB_COLLECT_COL_TEXT);

  GtkTreeModel *listmodel
      = GTK_TREE_MODEL(gtk_list_store_new(DT_LIB_COLLECT_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
                                          G_TYPE_STRING, G_TYPE_UINT, G_TYPE_BOOLEAN));
  d->listmodel = listmodel;
  GtkTreeModel *tagsmodel
      = GTK_TREE_MODEL(gtk_tree_store_new(DT_LIB_COLLECT_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
                                          G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN));
  d->treemodel_tags = tagsmodel;

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(sw), TRUE, TRUE, 0);


  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  d->box = GTK_BOX(vbox);

  GtkWidget *sw2 = gtk_scrolled_window_new(NULL, NULL);
  d->sw2 = GTK_SCROLLED_WINDOW(sw2);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw2), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(sw2), GTK_WIDGET(d->box));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(sw2), TRUE, TRUE, 0);

  d->labels = NULL;
  d->trees = NULL;

  /* setup proxy */
  darktable.view_manager->proxy.module_collect.module = self;
  darktable.view_manager->proxy.module_collect.update = _lib_collect_gui_update;

  /* set the monitor */
  /* TODO: probably we should be using the same for the import code */
  //  d->gv_monitor = g_volume_monitor_get ();
  //  g_signal_connect(G_OBJECT(d->gv_monitor), "mount-added", G_CALLBACK(mount_changed), self);
  //  g_signal_connect(G_OBJECT(d->gv_monitor), "mount-removed", G_CALLBACK(mount_changed), self);
  //  g_signal_connect(G_OBJECT(d->gv_monitor), "mount-changed", G_CALLBACK(mount_changed), self);

  // TODO: This should be done in a more generic place, not gui_init
  d->treemodel_folders = GTK_TREE_MODEL(_folder_tree());
  d->tree_new = TRUE;
  _lib_collect_gui_update(self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED, G_CALLBACK(collection_updated),
                            self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED, G_CALLBACK(filmrolls_updated),
                            self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_FILMROLLS_IMPORTED, G_CALLBACK(filmrolls_imported),
                            self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_FILMROLLS_REMOVED, G_CALLBACK(filmrolls_removed),
                            self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;

  for(int i = 0; i < MAX_RULES; i++) dt_gui_key_accel_block_on_focus_disconnect(d->rule[i].text);

  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(collection_updated), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(filmrolls_updated), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(filmrolls_imported), self);
  darktable.view_manager->proxy.module_collect.module = NULL;
  g_free(((dt_lib_collect_t *)self->data)->params);

  /* cleanup mem */
  // g_ptr_array_free(d->labels, TRUE);
  if(d->trees != NULL) g_ptr_array_free(d->trees, TRUE);

  /* TODO: Make sure we are cleaning up all allocations */

  g_free(self->data);
  self->data = NULL;
}


#ifdef USE_LUA
typedef dt_lib_collect_params_rule_t* dt_lua_lib_collect_params_rule_t;
static int filter_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lua_lib_check_error(L,self);
  dt_lib_collect_params_t old_params;
  int size;
  
  dt_lib_collect_params_t *p = get_params(self, &size);
  // put it in stack so memory is not lost if a lua exception is raised
  memcpy(&old_params, p,size);
  free(p);
  if(lua_gettop(L) > 0) {
    dt_lib_collect_params_t params;
    luaA_to(L,dt_lib_collect_params_t,&params,1);
    set_params(self, &params,size);
    lua_pop(L,1);
  }
  luaA_push(L,dt_lib_collect_params_t,&old_params);
  return 1;
}

static int param_len(lua_State *L)
{
  dt_lib_collect_params_t params;
  luaA_to(L,dt_lib_collect_params_t,&params,1);
  lua_pushnumber(L, params.rules);
  return 1;
}

static int param_index(lua_State *L)
{
  dt_lib_collect_params_t* params =  lua_touserdata(L,1);
  int index = luaL_checkinteger(L,2);
  if(lua_gettop(L) > 2) {
    if(index < 1 || index > params->rules+1 || index > MAX_RULES) {
      return luaL_error(L,"incorrect write index for object of type dt_lib_collect_params_t\n");
    }
    if(lua_isnil(L,3)) {
      for(int i = index; i< params->rules -1 ;i++){
        memcpy(&params->rule[index-1],&params->rule[index],sizeof(dt_lib_collect_params_rule_t));
      }
      params->rules--;
    } else if(dt_lua_isa(L,3,dt_lua_lib_collect_params_rule_t)){
      if(index == params->rules+1) {
        params->rules++;
      }
      dt_lib_collect_params_rule_t *rule;
      luaA_to(L,dt_lua_lib_collect_params_rule_t,&rule,3);
      memcpy(&params->rule[index-1],rule,sizeof(dt_lib_collect_params_rule_t));
    } else {
      return luaL_error(L,"incorrect type for field of dt_lib_collect_params_t\n");
    }
  }
  if(index < 1 || index > params->rules) {
    return luaL_error(L,"incorrect read index for object of type dt_lib_collect_params_t\n");
  }
  dt_lib_collect_params_rule_t* tmp = &params->rule[index-1];
  luaA_push(L,dt_lua_lib_collect_params_rule_t,&tmp);
  lua_getuservalue(L,-1);
  lua_pushvalue(L,1);
  lua_setfield(L,-2,"containing_object");//prevent GC from killing the child object
  lua_pop(L,1);
  return 1;
}

static int mode_member(lua_State *L)
{
  dt_lib_collect_params_rule_t *rule;
  luaA_to(L,dt_lua_lib_collect_params_rule_t,&rule,1);
  if(lua_gettop(L) > 2) {
    dt_lib_collect_mode_t value;
    luaA_to(L,dt_lib_collect_mode_t,&value,3);
    rule->mode = value;
    return 0;
  }
  const dt_lib_collect_mode_t tmp = rule->mode; // temp buffer because of bitfield in the original struct
  luaA_push(L,dt_lib_collect_mode_t,&tmp);
  return 1;
}

static int item_member(lua_State *L)
{
  dt_lib_collect_params_rule_t *rule;
  luaA_to(L,dt_lua_lib_collect_params_rule_t,&rule,1);

  if(lua_gettop(L) > 2) {
    dt_collection_properties_t value;
    luaA_to(L,dt_collection_properties_t,&value,3);
    rule->item = value;
    return 0;
  }
  const dt_collection_properties_t tmp = rule->item; // temp buffer because of bitfield in the original struct
  luaA_push(L,dt_collection_properties_t,&tmp);
  return 1;
}

static int data_member(lua_State *L)
{
  dt_lib_collect_params_rule_t *rule;
  luaA_to(L,dt_lua_lib_collect_params_rule_t,&rule,1);

  if(lua_gettop(L) > 2) {
    size_t tgt_size;
    const char*data = luaL_checklstring(L,3,&tgt_size);
    if(tgt_size > PARAM_STRING_SIZE)
    {
      return luaL_error(L, "string '%s' too long (max is %d)", data, PARAM_STRING_SIZE);
    }
    memcpy(rule->string,data,strlen(data));
    return 0;
  }
  lua_pushstring(L,rule->string);
  return 1;
}



void init(struct dt_lib_module_t *self)
{

  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, filter_cb,1);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "filter");

  dt_lua_init_type(L,dt_lib_collect_params_t);
  lua_pushcfunction(L,param_len);
  lua_pushcfunction(L,param_index);
  dt_lua_type_register_number(L,dt_lib_collect_params_t);

  dt_lua_init_type(L,dt_lua_lib_collect_params_rule_t);
  lua_pushcfunction(L,mode_member);
  dt_lua_type_register(L, dt_lua_lib_collect_params_rule_t, "mode");
  lua_pushcfunction(L,item_member);
  dt_lua_type_register(L, dt_lua_lib_collect_params_rule_t, "item");
  lua_pushcfunction(L,data_member);
  dt_lua_type_register(L, dt_lua_lib_collect_params_rule_t, "data");
  

  luaA_enum(L,dt_lib_collect_mode_t);
  luaA_enum_value(L,dt_lib_collect_mode_t,DT_LIB_COLLECT_MODE_AND);
  luaA_enum_value(L,dt_lib_collect_mode_t,DT_LIB_COLLECT_MODE_OR);
  luaA_enum_value(L,dt_lib_collect_mode_t,DT_LIB_COLLECT_MODE_AND_NOT);

  luaA_enum(L,dt_collection_properties_t);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_FILMROLL);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_FOLDERS);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_CAMERA);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_TAG);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_DAY);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_TIME);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_HISTORY);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_COLORLABEL);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_TITLE);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_DESCRIPTION);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_CREATOR);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_PUBLISHER);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_RIGHTS);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_LENS);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_ISO);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_APERTURE);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_FILENAME);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_GEOTAGGING);

}
#endif
#undef MAX_RULES
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
