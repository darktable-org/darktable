/*
    This file is part of darktable,
    copyright (c) 2012 Jose Carlos Garcia Sogo
    based on keywords.c

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
#include "common/debug.h"
#include "common/tags.h"
#include "common/collection.h"
#include "common/utility.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"

#include "libs/lib.h"


static void _lib_folders_update_collection(const gchar *filmroll);

DT_MODULE(1)

typedef struct dt_lib_folders_t
{
  /* data */
  GtkTreeStore *store;
  GList *mounts;

  /* gui */
  GVolumeMonitor *gv_monitor;
  GtkBox *box_tree;

  GPtrArray *buttons;
  GPtrArray *trees;
}
dt_lib_folders_t;


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
const char* name()
{
  return _("folders");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 410;
}

void init_key_accels(dt_lib_module_t *self)
{
  //  dt_accel_register_lib(self, NC_("accel", "scan for devices"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  /*  dt_lib_import_t *d = (dt_lib_import_t*)self->data;

  dt_accel_connect_button_lib(self, "scan for devices",
                              GTK_WIDGET(d->scan_devices)); */
}

void view_popup_menu_onSync (GtkWidget *menuitem, gpointer userdata)
{
 
}

void view_popup_menu_onSearchFilmroll (GtkWidget *menuitem, gpointer userdata)
{
  GtkTreeView *treeview = GTK_TREE_VIEW(userdata);
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser;

  GtkTreeSelection *selection;
  GtkTreeIter iter, child;
  GtkTreeModel *model;

  gchar *tree_path = NULL;
  gchar *new_path = NULL;

  filechooser = gtk_file_chooser_dialog_new (_("search filmroll"),
                         GTK_WINDOW (win),
                         GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                         GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                         GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                         (char *)NULL);

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  model = gtk_tree_view_get_model(treeview);
  selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
  gtk_tree_selection_get_selected(selection, &model, &iter);
  child = iter;
  gtk_tree_model_iter_parent(model, &iter, &child);
  gtk_tree_model_get(model, &child, 1, &tree_path, -1);

  if(tree_path != NULL)
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER (filechooser), tree_path);
  else
    goto error;

  // run the dialog
  if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gint id = -1;
    sqlite3_stmt *stmt;
    gchar *query = NULL;

    /* If we want to allow the user to just select the folder, we have to use 
     * gtk_file_chooser_get_uri() instead. THe code should be adjusted then,
     * as it returns a file:/// URI and with utf8 characters escaped.
     */
    new_path = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER (filechooser));
    if (new_path)
    {
      gchar *old = NULL;
      query = dt_util_dstrcat(query, "select id,folder from film_rolls where folder like '%s%%'", tree_path);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      g_free(query);

      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
        id = sqlite3_column_int(stmt, 0);
        old = (gchar *) sqlite3_column_text(stmt, 1);
        
        query = NULL;
        query = dt_util_dstrcat(query, "update film_rolls set folder=?1 where id=?2");

        gchar trailing[1024];
        gchar final[1024];

        if (g_strcmp0(old, tree_path))
        {
          g_snprintf(trailing, 1024, "%s", old + strlen(tree_path)+1);
          g_snprintf(final, 1024, "%s/%s", new_path, trailing);
        }
        else
        {
          g_snprintf(final, 1024, "%s", new_path);
        }

        sqlite3_stmt *stmt2;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt2, NULL);
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 1, final, strlen(final), SQLITE_STATIC);
        DT_DEBUG_SQLITE3_BIND_INT(stmt2, 2, id);
        sqlite3_step(stmt2);
        sqlite3_finalize(stmt2);
      }
      g_free(query);

      /* reset filter to display all images, otherwise view may remain empty */
      dt_view_filter_reset_to_show_all(darktable.view_manager);

      /* update collection to view missing filmroll */
      _lib_folders_update_collection(new_path);

      dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
    }
    else
      goto error;
  }
  g_free(tree_path);
  g_free(new_path);
  gtk_widget_destroy (filechooser);
  return;

error:
  /* Something wrong happened */
  gtk_widget_destroy (filechooser);
  dt_control_log(_("Problem selecting new path for the filmroll in %s"), tree_path);

  g_free(tree_path);
  g_free(new_path); 
}

void view_popup_menu_onRemove (GtkWidget *menuitem, gpointer userdata)
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
  gtk_tree_model_get(model, &iter, 1, &filmroll_path, -1);

  /* Clean selected images, and add to the table those which are going to be deleted */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "delete from selected_images", NULL, NULL, NULL);
 
  fullq = dt_util_dstrcat(fullq, "insert into selected_images select id from images where film_id  in (select id from film_rolls where folder like '%s%%')", filmroll_path);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), fullq, NULL, NULL, NULL);

  dt_control_remove_images();
}

void
view_popup_menu (GtkWidget *treeview, GdkEventButton *event, gpointer userdata)
{
  GtkWidget *menu, *menuitem;

  menu = gtk_menu_new();

  menuitem = gtk_menu_item_new_with_label(_("search filmroll..."));
  g_signal_connect(menuitem, "activate",
                   (GCallback) view_popup_menu_onSearchFilmroll, treeview);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

  /* FIXME: give some functionality */
  menuitem = gtk_menu_item_new_with_label(_("sync..."));
  g_signal_connect(menuitem, "activate",
                   (GCallback) view_popup_menu_onSync, treeview);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

  menuitem = gtk_menu_item_new_with_label(_("remove..."));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
  g_signal_connect(menuitem, "activate",
                   (GCallback) view_popup_menu_onRemove, treeview);

  gtk_widget_show_all(menu);

  /* Note: event can be NULL here when called from view_onPopupMenu;
   *  gdk_event_get_time() accepts a NULL argument */
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL,
                 (event != NULL) ? event->button : 0,
                 gdk_event_get_time((GdkEvent*)event));
}

gboolean
view_onButtonPressed (GtkWidget *treeview, GdkEventButton *event, gpointer userdata)
{
  /* single click with the right mouse button? */
  if (event->type == GDK_BUTTON_PRESS  &&  event->button == 3)
  {
    GtkTreeSelection *selection;

    selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));

    /* Note: gtk_tree_selection_count_selected_rows() does not
     *   exist in gtk+-2.0, only in gtk+ >= v2.2 ! */
    if (gtk_tree_selection_count_selected_rows(selection)  <= 1)
    {
       GtkTreePath *path;

       /* Get tree path for row that was clicked */
       if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview),
                                         (gint) event->x,
                                         (gint) event->y,
                                         &path, NULL, NULL, NULL))
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

gboolean
view_onPopupMenu (GtkWidget *treeview, gpointer userdata)
{
  view_popup_menu(treeview, NULL, userdata);

  return TRUE; /* we handled this */
}

static int
_count_images(const char *path)
{
  sqlite3_stmt *stmt = NULL;
  gchar query[1024] = {0};
  int count = 0;

  snprintf(query, 1024, "select count(id) from images where film_id in (select id from film_rolls where folder like '%s%%')", path);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if (sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  return count;
}

static gboolean
_filmroll_is_present(const gchar *path)
{
  return g_file_test(path, G_FILE_TEST_IS_DIR);
}

static void
_show_filmroll_present(GtkTreeViewColumn *column,
                  GtkCellRenderer   *renderer,
                  GtkTreeModel      *model,
                  GtkTreeIter       *iter,
                  gpointer          user_data)
{
  gchar *path, *pch;
  gtk_tree_model_get(model, iter, 1, &path, -1);
  gtk_tree_model_get(model, iter, 0, &pch, -1);

  g_object_set(renderer, "text", pch, NULL);
  g_object_set(renderer, "strikethrough", TRUE, NULL);

  if (!_filmroll_is_present(path))
    g_object_set(renderer, "strikethrough-set", TRUE, NULL);
  else
    g_object_set(renderer, "strikethrough-set", FALSE, NULL);
}


static GtkTreeStore *
_folder_tree ()
{
  /* intialize the tree store */
  char query[1024];
  sqlite3_stmt *stmt;
  snprintf(query, 1024, "select * from film_rolls");
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  GtkTreeStore *store = gtk_tree_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);

  // initialize the model with the paths

  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(strchr((const char *)sqlite3_column_text(stmt, 2),'/')==0)
    {
      // Do nothing here
    }
    else
    {
      int level = 0;
      char *value;
      GtkTreeIter current, iter;
      GtkTreePath *root;
      char *path = g_strdup((char *)sqlite3_column_text(stmt, 2));
      char *pch = strtok((char *)sqlite3_column_text(stmt, 2),"/");
      char *external = g_strdup((char *)sqlite3_column_text(stmt, 3));

      if (external == NULL)
        external = g_strdup("Local");

      gboolean found=FALSE;

      root = gtk_tree_path_new_first();
      gtk_tree_model_get_iter (GTK_TREE_MODEL(store), &iter, root);

      int children = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store),NULL);
      for (int k=0;k<children;k++)
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
      while (pch != NULL)
      {
        found = FALSE;
        int children = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store),level>0?&current:NULL);
        /* find child with name, if not found create and continue */
        for (int k=0;k<children;k++)
        {
          if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(store), &iter, level>0?&current:NULL, k))
          {
            gtk_tree_model_get (GTK_TREE_MODEL(store), &iter, 0, &value, -1);

            if (strcmp(value, pch)==0)
            {
              current = iter;
              found = TRUE;
              break;
            }
          }
        }

        /* lets add new path and assign current */
        if (!found)
        {
          const char *pth = g_strndup (path, strstr(path, pch)-path);
          const char *pth2 = g_strconcat(pth, pch, NULL);

          int count = _count_images(pth2);
          gtk_tree_store_insert(store, &iter, level>0?&current:NULL,0);
          gtk_tree_store_set(store, &iter, 0, pch, 1, pth2, 2, count, -1);
          current = iter;
        }

        level++;
        pch = strtok(NULL, "/");
      }
    }
  }

  return store;
}

static GtkTreeModel *
_create_filtered_model (GtkTreeModel *model, GtkTreeIter iter)
{
  GtkTreeModel *filter = NULL;
  GtkTreePath *path;
  GtkTreeIter child;

  /* Filter level */
  while (gtk_tree_model_iter_has_child(model, &iter))
  {
    if (gtk_tree_model_iter_n_children(model, &iter) == 1)
    {
      gtk_tree_model_iter_children(model, &child, &iter);

      if (gtk_tree_model_iter_n_children(model, &child) != 0)
        iter = child;
      else
        break;
    }
    else
      break;
	}

  path = gtk_tree_model_get_path (model, &iter);

  /* Create filter and set virtual root */
  filter = gtk_tree_model_filter_new (model, path);
  gtk_tree_path_free (path);

  return filter;
}

static GtkTreeView *
_create_treeview_display (GtkTreeModel *model)
{
  GtkTreeView *treeview;
  GtkCellRenderer *renderer, *renderer2;
  GtkTreeViewColumn *column1, *column2;

  treeview = GTK_TREE_VIEW(gtk_tree_view_new ());

  renderer = gtk_cell_renderer_text_new ();
  renderer2 = gtk_cell_renderer_text_new ();

  column1 = gtk_tree_view_column_new();
  column2 = gtk_tree_view_column_new();

  gtk_tree_view_column_set_sizing(column1, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width (column1, 230);
  gtk_tree_view_column_set_max_width (column1, 230);

  gtk_tree_view_column_pack_start(column1, renderer, TRUE);
  gtk_tree_view_column_pack_start(column2, renderer2, TRUE);

  gtk_tree_view_insert_column(treeview, column1, 0);
  gtk_tree_view_insert_column(treeview, column2, 1);

  gtk_tree_view_column_add_attribute(column2, renderer2, "text", 2);
  gtk_tree_view_column_set_cell_data_func(column1, renderer, _show_filmroll_present, NULL, NULL);

  gtk_tree_view_set_level_indentation (treeview, 1);

  gtk_tree_view_set_headers_visible(treeview, FALSE);

  gtk_tree_view_set_model(treeview, GTK_TREE_MODEL(model));

  /* free store, treeview has its own storage now */
  g_object_unref(model);

  return treeview;
}

static void _lib_folders_update_collection(const gchar *filmroll)
{

  gchar *complete_query = NULL;

  complete_query = dt_util_dstrcat(complete_query, "film_id in (select id from film_rolls where folder like '%s%%')", filmroll);

  dt_conf_set_string("plugins/lighttable/where_ext_query", complete_query);
  dt_conf_set_bool("plugins/lighttable/alt_query", 1);

  dt_collection_update_query(darktable.collection);

  g_free(complete_query);

  // remove from selected images where not in this query.
  sqlite3_stmt *stmt = NULL;
  const gchar *cquery = dt_collection_get_query(darktable.collection);
  complete_query = NULL;
  if(cquery && cquery[0] != '\0')
  {
    complete_query = dt_util_dstrcat(complete_query, "delete from selected_images where imgid not in (%s)", cquery);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), complete_query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* free allocated strings */
    g_free(complete_query);
  }

  /* raise signal of collection change, only if this is an orginal */
  if (!darktable.collection->clone)
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED);
}

static void
tree_row_activated (GtkTreeView *view, GtkTreePath *path, gpointer user_data)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  /* Only one row at a time can be selected */
  gchar *filmroll;
  gtk_tree_model_get (model, &iter, 1, &filmroll, -1);

  _lib_folders_update_collection(filmroll);
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

void destroy_widget (gpointer data)
{
  GtkWidget *widget = (GtkWidget *)data;

  gtk_widget_destroy(widget);
}


void
 _lib_folders_gui_update(dt_lib_module_t *self)
{
  dt_lib_folders_t *d = (dt_lib_folders_t *)self->data;

  const int old = darktable.gui->reset;
  darktable.gui->reset = 1;

  d->store = _folder_tree();

  GtkWidget *button;
  GtkTreeView *tree;

  /* We have already inited the GUI once, clean around */
  if (d->buttons != NULL)
  {
    for (int i=0; i<d->buttons->len; i++)
    {
      button = GTK_WIDGET(g_ptr_array_index (d->buttons, i));
      g_ptr_array_free(d->buttons, TRUE);
    }
  }
   
  if (d->trees != NULL)
  {
    for (int i=0; i<d->trees->len; i++)
    {
      tree = GTK_TREE_VIEW(g_ptr_array_index (d->trees, i));
      g_ptr_array_free(d->trees, TRUE);
    }
  }

  /* set the UI */
  GtkTreeModel *model;
  GtkTreeIter iter;
  
  GtkTreePath *root = gtk_tree_path_new_first();
  gtk_tree_model_get_iter (GTK_TREE_MODEL(d->store), &iter, root);

  int children = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(d->store), NULL);
  d->buttons = g_ptr_array_sized_new(children);
  g_ptr_array_set_free_func (d->buttons, destroy_widget);

  d->trees = g_ptr_array_sized_new(children);
  g_ptr_array_set_free_func (d->trees, destroy_widget);

  for (int i=0; i<children; i++)
  {
    GValue value;
    memset(&value,0,sizeof(GValue));
    gtk_tree_model_iter_nth_child (GTK_TREE_MODEL(d->store), &iter, NULL, i);

  	gtk_tree_model_get_value (GTK_TREE_MODEL(d->store), &iter, 0, &value);
    
    gchar *mount_name = g_value_dup_string(&value);

    if (g_strcmp0(mount_name, "Local")==0)
    {
      /* Add a button for local filesystem, to keep UI consistency */
      button = gtk_button_new_with_label (_("Local HDD"));
    }
    else
    {
      button = gtk_button_new_with_label (mount_name);
    }
    g_ptr_array_add(d->buttons, (gpointer) button);
    gtk_container_add(GTK_CONTAINER(d->box_tree), GTK_WIDGET(button));
    
    model = _create_filtered_model(GTK_TREE_MODEL(d->store), iter);
    tree = _create_treeview_display(GTK_TREE_MODEL(model));
    g_ptr_array_add(d->trees, (gpointer) tree);
    gtk_container_add(GTK_CONTAINER(d->box_tree), GTK_WIDGET(tree));

    g_signal_connect(G_OBJECT (tree), "row-activated", G_CALLBACK (tree_row_activated), d);
    g_signal_connect(G_OBJECT (tree), "button-press-event", G_CALLBACK (view_onButtonPressed), NULL);
    g_signal_connect(G_OBJECT (tree), "popup-menu", G_CALLBACK (view_onPopupMenu), NULL);
    
    g_value_unset(&value);
    g_free(mount_name);
  }

  darktable.gui->reset = old;

  gtk_widget_show_all(GTK_WIDGET(d->box_tree));

}

static void
collection_updated(gpointer instance,gpointer self)
{
  _lib_folders_gui_update((dt_lib_module_t *)self);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_folders_t *d = (dt_lib_folders_t *)g_malloc(sizeof(dt_lib_folders_t));
  memset(d,0,sizeof(dt_lib_folders_t));
  self->data = (void *)d;
  self->widget = gtk_vbox_new(FALSE, 5);

  dt_control_signal_connect(darktable.signals, 
			    DT_SIGNAL_FILMROLLS_CHANGED,
			    G_CALLBACK(collection_updated),
			    self);

  d->box_tree = GTK_BOX(gtk_vbox_new(FALSE,5));

  /* set the monitor */
  d->gv_monitor = g_volume_monitor_get ();

//  g_signal_connect(G_OBJECT(d->gv_monitor), "mount-added", G_CALLBACK(mount_changed), self);
//  g_signal_connect(G_OBJECT(d->gv_monitor), "mount-removed", G_CALLBACK(mount_changed), self);
//  g_signal_connect(G_OBJECT(d->gv_monitor), "mount-changed", G_CALLBACK(mount_changed), self);

  d->mounts = g_volume_monitor_get_mounts(d->gv_monitor);
  
  _lib_folders_gui_update(self);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->box_tree), TRUE, TRUE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_folders_t *d = (dt_lib_folders_t*)self->data;

  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(collection_updated), self);

  /* cleanup mem */
  g_ptr_array_free(d->buttons, TRUE);
  g_ptr_array_free(d->trees, TRUE);

  /* TODO: Cleanup gtktreestore and gtktreemodel all arounf the code */
  g_free(self->data);
  self->data = NULL;
}
