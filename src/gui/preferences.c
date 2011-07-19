/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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

#include <gdk/gdkkeysyms.h>

#include "common/darktable.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "gui/preferences.h"
#include "libs/lib.h"
#include "preferences_gen.h"

// Values for the accelerators treeview

#define ACCEL_COLUMN 0
#define BINDING_COLUMN 1
#define TRANS_COLUMN 2
#define N_COLUMNS 3

static void init_tab_accels(GtkWidget *book);
static void tree_insert_accel(gpointer data, const gchar *accel_path,
                              guint accel_key, GdkModifierType accel_mods,
                              gboolean changed);
static void tree_insert_rec(GtkTreeStore *model, GtkTreeIter *parent,
                            const gchar *accel_path, guint accel_key,
                            GdkModifierType accel_mods, gboolean changed);
static void path_to_accel(GtkTreeModel *model, GtkTreePath *path, gchar *str);
static void update_accels_model(gpointer widget, gpointer data);
static void update_accels_model_rec(GtkTreeModel *model, GtkTreeIter *parent,
                                    gchar *path);
static void delete_matching_accels(gpointer path, gpointer key_event);
static gint _strcmp(gconstpointer a, gconstpointer b);
static void import_export(GtkButton *button, gpointer data);
static void restore_defaults(GtkButton *button, gpointer data);
static gint compare_rows(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
                         gpointer data);

// Signal handlers
static void tree_row_activated(GtkTreeView *tree, GtkTreePath *path,
                               GtkTreeViewColumn *column, gpointer data);
static void tree_selection_changed(GtkTreeSelection *selection, gpointer data);
static gboolean tree_key_press(GtkWidget *widget, GdkEventKey *event,
                               gpointer data);
static gboolean prefix_search(GtkTreeModel *model, gint column,
                              const gchar *key, GtkTreeIter *iter, gpointer d);



void dt_gui_preferences_show()
{
  GtkWidget *win = darktable.gui->widgets.main_window;
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      _("darktable preferences"), GTK_WINDOW (win),
      GTK_DIALOG_MODAL,
      _("close"),
      GTK_RESPONSE_ACCEPT,
      NULL);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ALWAYS);
  gtk_window_resize(GTK_WINDOW(dialog), 600, 300);
  GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  GtkWidget *notebook = gtk_notebook_new();
  gtk_box_pack_start(GTK_BOX(content), notebook, TRUE, TRUE, 0);

  // Make sure remap mode is off initially
  darktable.control->accel_remap_str = NULL;
  darktable.control->accel_remap_path = NULL;

  init_tab_gui(notebook);
  init_tab_core(notebook);
  init_tab_accels(notebook);
  gtk_widget_show_all(dialog);
  (void) gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  // Cleaning up any memory still allocated for remapping
  if(darktable.control->accel_remap_path)
  {
    gtk_tree_path_free(darktable.control->accel_remap_path);
    darktable.control->accel_remap_path = NULL;
  }

}

static void init_tab_accels(GtkWidget *book)
{
  GtkWidget *alignment = gtk_alignment_new(0.5, 0.0, 0.9, 1.0);
  GtkWidget *container = gtk_vbox_new(FALSE, 5);
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  GtkWidget *tree = gtk_tree_view_new();
  GtkWidget *button;
  GtkWidget *hbox;
  GtkTreeStore *model = gtk_tree_store_new(N_COLUMNS,
                                           G_TYPE_STRING, G_TYPE_STRING,
                                           G_TYPE_STRING);
  GtkCellRenderer *renderer;
  GtkTreeViewColumn *column;

  // Adding the outer container
  gtk_alignment_set_padding(GTK_ALIGNMENT(alignment), 20, 20, 20, 20);
  gtk_container_add(GTK_CONTAINER(alignment), container);
  gtk_notebook_append_page(GTK_NOTEBOOK(book), alignment,
                           gtk_label_new(_("shortcuts")));

  // Building the accelerator tree
  gtk_accel_map_foreach((gpointer)model, tree_insert_accel);

  // Seting a custom sort functions so expandable groups rise to the top
  gtk_tree_sortable_set_sort_column_id(
      GTK_TREE_SORTABLE(model), TRANS_COLUMN, GTK_SORT_ASCENDING);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(model), TRANS_COLUMN,
                                  compare_rows, NULL, NULL);

  // Setting up the cell renderers
  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(
      _("shortcut"), renderer,
      "text", TRANS_COLUMN,
      NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(
      _("binding"), renderer,
      "text", BINDING_COLUMN,
      NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  // Attaching treeview signals

  // row-activated either expands/collapses a row or activates remapping
  g_signal_connect(G_OBJECT(tree), "row-activated",
                   G_CALLBACK(tree_row_activated), NULL);

  // A selection change will cancel a currently active remapping
  g_signal_connect(G_OBJECT(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree))),
                   "changed",
                   G_CALLBACK(tree_selection_changed), NULL);

  // A keypress may remap an accel or delete one
  g_signal_connect(G_OBJECT(tree), "key-press-event",
                   G_CALLBACK(tree_key_press), (gpointer)model);

  // Setting up the search functionality
  gtk_tree_view_set_search_column(GTK_TREE_VIEW(tree), TRANS_COLUMN);
  gtk_tree_view_set_search_equal_func(GTK_TREE_VIEW(tree), prefix_search,
                                      NULL, NULL);
  gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tree), TRUE);

  // Attaching the model to the treeview
  gtk_tree_view_set_model(GTK_TREE_VIEW(tree), GTK_TREE_MODEL(model));

  // Adding the treeview to its containers
  gtk_container_add(GTK_CONTAINER(scroll), tree);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(container), scroll, TRUE, TRUE, 0);

  hbox = gtk_hbox_new(FALSE, 5);

  // Adding the restore defaults button
  button = gtk_button_new_with_label(_("default"));
  gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked",
                   G_CALLBACK(restore_defaults), NULL);
  g_signal_connect(G_OBJECT(button), "clicked",
                   G_CALLBACK(update_accels_model), (gpointer)model);

  // Adding the import/export buttons

  button = gtk_button_new_with_label(C_("preferences", "import"));
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked",
                   G_CALLBACK(import_export), (gpointer)0);
  g_signal_connect(G_OBJECT(button), "clicked",
                   G_CALLBACK(update_accels_model), (gpointer)model);

  button = gtk_button_new_with_label(_("export"));
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked",
                   G_CALLBACK(import_export), (gpointer)1);

  gtk_box_pack_start(GTK_BOX(container), hbox, FALSE, FALSE, 0);

  g_object_unref(G_OBJECT(model));

}

static void tree_insert_accel(gpointer data, const gchar *accel_path,
                              guint accel_key, GdkModifierType accel_mods,
                              gboolean changed)
{
  char *lap = (char *)accel_path;
  
  /* if prefixed lets forward pointer */
  if (!strncmp(lap,"<Darktable>",strlen("<Darktable>")))
    lap += strlen("<Darktable>") + 1;

  /* lets recurse path */
  tree_insert_rec((GtkTreeStore*)data, NULL, lap, accel_key,
                  accel_mods, changed);
}

static void tree_insert_rec(GtkTreeStore *model, GtkTreeIter *parent,
                            const gchar *accel_path, guint accel_key,
                            GdkModifierType accel_mods, gboolean changed)
{
  const gchar *translation = NULL;
  GList *iops = darktable.iop;
  GList *libs = darktable.lib->plugins;

  int i;
  gboolean found = FALSE;
  gchar *val_str;
  GtkTreeIter iter;

  /* if we are on end of path lets bail out of recursive insert */
  if (*accel_path==0) return;

  /* check if we are on a leaf or a branch  */
  if (!g_strrstr(accel_path,"/")) 
  {
    /* we are on a leaf lets add */
    gchar *name = gtk_accelerator_name(accel_key, accel_mods);
    gtk_tree_store_append(model, &iter, parent);
    gtk_tree_store_set(model, &iter,
                       ACCEL_COLUMN, accel_path,
                       BINDING_COLUMN, g_dpgettext2("gtk20", "keyboard label", name),
                       TRANS_COLUMN, _(accel_path),
                       -1);
    g_free(name);    
  } 
  else
  {
    /* we are on a branch let's get the node name */
    gchar *eon = g_strstr_len(accel_path,strlen(accel_path),"/");
    gchar *node = g_strndup(accel_path,eon-accel_path);

    /* search the tree if we alread have an sibiling with node name */
    int sibilings = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(model), parent);
    for (i = 0; i < sibilings; i++)
    {
      gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(model), &iter, parent, i);
      gtk_tree_model_get(GTK_TREE_MODEL(model), &iter,
                         ACCEL_COLUMN, &val_str,
                         -1);
      
      /* do we match current sibiling */
      if (!strcmp(val_str, node)) found = TRUE;
      
      g_free(val_str);
      
      /* if we found a matching node let's break out */
      if(found) break;
    }

    /* if not found let's add a branch */
    if(!found)
    {
      /* check if current branch is a iop then get translation */
      gchar *opname;
      while(iops)
      {
        opname = ((dt_iop_module_so_t*)iops->data)->op;

        /* if iop module found, lets get translated name */
        if (!strcmp(opname, node))
        {
          translation = ((dt_iop_module_so_t*)iops->data)->name();
          break;
        }

        iops = g_list_next(iops);
      }

      /* Check for libs as well */
      
      while(libs)
      {
        opname = ((dt_lib_module_t*)libs->data)->plugin_name;

        if(!strcmp(opname, node))
        {
          translation = ((dt_lib_module_t*)libs->data)->name();
          break;
        }

        libs = g_list_next(libs);
      }

      /* not an iop or lib, do a translation.. */
      if(!translation)
        translation = _(node);
      
      gtk_tree_store_append(model, &iter, parent);
      gtk_tree_store_set(model, &iter,
                         ACCEL_COLUMN, node,
                         BINDING_COLUMN, "",
                         TRANS_COLUMN, translation,
                         -1);
    }

    /* recurse further down the path */
    tree_insert_rec(model, &iter, accel_path + strlen(node) + 1, accel_key, accel_mods, changed);
    
    /* free up data */
    g_free(node);
  }
}

static void path_to_accel(GtkTreeModel *model, GtkTreePath *path, gchar *str)
{
  gint depth;
  gint *indices;
  GtkTreeIter parent;
  GtkTreeIter child;
  gint i;
  gchar *data_str;

  // Start out with the base <Darktable>
  strcpy(str, "<Darktable>");

  // For each index in the path, append a '/' and that section of the path
  depth = gtk_tree_path_get_depth(path);
  indices = gtk_tree_path_get_indices(path);
  for(i = 0; i < depth; i++)
  {
    strcat(str, "/");
    gtk_tree_model_iter_nth_child(model, &child,  i == 0 ? NULL : &parent,
                                  indices[i]);
    gtk_tree_model_get(model, &child,
                       ACCEL_COLUMN, &data_str,
                       -1);
    strcat(str, data_str);
    g_free(data_str);
    parent = child;
  }
}

static void update_accels_model(gpointer widget, gpointer data)
{
  GtkTreeModel *model = (GtkTreeModel*)data;
  GtkTreeIter iter;
  gchar path[256];
  gchar *end;
  gint i;

  strcpy(path, "<Darktable>");
  end = path + strlen(path);

  for(i = 0; i < gtk_tree_model_iter_n_children(model, NULL); i++)
  {
    gtk_tree_model_iter_nth_child(model, &iter, NULL, i);
    update_accels_model_rec(model, &iter, path);
    *end = '\0'; // Trimming the string back to the base for the next iteration
  }

}

static void update_accels_model_rec(GtkTreeModel *model, GtkTreeIter *parent,
                                    gchar *path)
{
  GtkAccelKey key;
  GtkTreeIter iter;
  gchar *str_data;
  gchar *end;
  gint i;

  // First concatenating this part of the key
  strcat(path, "/");
  gtk_tree_model_get(model, parent, ACCEL_COLUMN, &str_data, -1);
  strcat(path, str_data);
  g_free(str_data);

  if(gtk_tree_model_iter_has_child(model, parent))
  {
    // Branch node, carry on with recursion
    end = path + strlen(path);

    for(i = 0; i < gtk_tree_model_iter_n_children(model, parent); i++)
    {
      gtk_tree_model_iter_nth_child(model, &iter, parent, i);
      update_accels_model_rec(model, &iter, path);
      *end = '\0';
    }
  }
  else
  {
    // Leaf node, update the text

    gtk_accel_map_lookup_entry(path, &key);
    gtk_tree_store_set(
        GTK_TREE_STORE(model), parent,
        BINDING_COLUMN, gtk_accelerator_name(key.accel_key, key.accel_mods),
        -1);
  }
}

static void delete_matching_accels(gpointer path, gpointer key_event)
{
  GtkAccelKey key;
  GdkEventKey *event = (GdkEventKey*)key_event;

  // Make sure we're not deleting the key we just remapped
  if(!strcmp(path, darktable.control->accel_remap_str))
    return;

  gtk_accel_map_lookup_entry(path, &key);

  if(key.accel_key == event->keyval
     && key.accel_mods == (event->state & KEY_STATE_MASK))
    gtk_accel_map_change_entry(path, 0, 0, TRUE);

}

static gint _strcmp(gconstpointer a, gconstpointer b)
{
  return (gint)strcmp((const char*)b, (const char*)a);
}

static void tree_row_activated(GtkTreeView *tree, GtkTreePath *path,
                               GtkTreeViewColumn *column, gpointer data)
{
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(tree);

  static gchar accel_path[256];

  gtk_tree_model_get_iter(model, &iter, path);

  if(gtk_tree_model_iter_has_child(model, &iter))
  {
    // For branch nodes, toggle expansion on activation
    if(gtk_tree_view_row_expanded(tree, path))
      gtk_tree_view_collapse_row(tree, path);
    else
      gtk_tree_view_expand_row(tree, path, FALSE);
  }
  else
  {
    // For leaf nodes, enter remapping mode

    // Assembling the full accelerator path
    path_to_accel(model, path, accel_path);

    // Setting the notification text
    gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
                       BINDING_COLUMN, _("press key combination to remap..."),
                       -1);

    // Activating remapping
    darktable.control->accel_remap_str = accel_path;
    darktable.control->accel_remap_path = gtk_tree_path_copy(path);
  }
}

static void tree_selection_changed(GtkTreeSelection *selection, gpointer data)
{
  GtkTreeModel *model;
  GtkTreeIter iter;

  GtkAccelKey key;

  // If remapping is currently activated, it needs to be deactivated
  if(!darktable.control->accel_remap_str)
    return;

  model = gtk_tree_view_get_model(gtk_tree_selection_get_tree_view(selection));
  gtk_tree_model_get_iter(model, &iter, darktable.control->accel_remap_path);

  // Restoring the BINDING_COLUMN text
  gtk_accel_map_lookup_entry(darktable.control->accel_remap_str, &key);
  gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
                     BINDING_COLUMN,
                     gtk_accelerator_name(key.accel_key, key.accel_mods), -1);

  // Cleaning up the darktable.gui info
  darktable.control->accel_remap_str = NULL;
  gtk_tree_path_free(darktable.control->accel_remap_path);
  darktable.control->accel_remap_path = NULL;
}

static gboolean tree_key_press(GtkWidget *widget, GdkEventKey *event,
                               gpointer data)
{

  GtkTreeModel *model = (GtkTreeModel*)data;
  GtkTreeIter iter;
  GtkTreeSelection *selection =
      gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
  GtkTreePath *path;
  gboolean global;

  gchar accel[256];
  gchar datadir[1024];
  gchar accelpath[1024];

  // We can just ignore mod key presses outright
  if(event->is_modifier)
    return FALSE;

  dt_get_user_config_dir(datadir, 1024);
  snprintf(accelpath, 1024, "%s/keyboardrc", datadir);

  // Otherwise, determine whether we're in remap mode or not
  if(darktable.control->accel_remap_str)
  {
    // First delete any conflicting accelerators

    // If a global accel is changed, modify _every_ group
    global =
        g_slist_find_custom(darktable.control->accels_list_global,
                            darktable.control->accel_remap_str, _strcmp) != NULL;

    // Global is always active, so anything that matches there must go
    g_slist_foreach(darktable.control->accels_list_global, delete_matching_accels,
                    (gpointer)event);

    // Now check for any matching accels in the same group
    if(g_slist_find_custom(darktable.control->accels_list_lighttable,
                           darktable.control->accel_remap_str, _strcmp) || global)
      g_slist_foreach(darktable.control->accels_list_lighttable,
                      delete_matching_accels, (gpointer)event);
    if(g_slist_find_custom(darktable.control->accels_list_darkroom,
                           darktable.control->accel_remap_str, _strcmp) || global)
    {
      g_slist_foreach(darktable.control->accels_list_darkroom,
                      delete_matching_accels, (gpointer)event);
      g_slist_foreach(darktable.control->accels_list_filmstrip,
                      delete_matching_accels, (gpointer)event);
    }
    if(g_slist_find_custom(darktable.control->accels_list_capture,
                           darktable.control->accel_remap_str, _strcmp) || global)
    {
      g_slist_foreach(darktable.control->accels_list_capture,
                      delete_matching_accels, (gpointer)event);
      g_slist_foreach(darktable.control->accels_list_filmstrip,
                      delete_matching_accels, (gpointer)event);
    }
    if(g_slist_find_custom(darktable.control->accels_list_filmstrip,
                           darktable.control->accel_remap_str, _strcmp) || global)
    {
      g_slist_foreach(darktable.control->accels_list_darkroom,
                      delete_matching_accels, (gpointer)event);
      g_slist_foreach(darktable.control->accels_list_capture,
                      delete_matching_accels, (gpointer)event);
    }
    // Change the accel map entry
    if(gtk_accel_map_change_entry(darktable.control->accel_remap_str, event->keyval,
                                  event->state & KEY_STATE_MASK, TRUE))
    {
      // If it succeeded delete any conflicting accelerators

      // If a global accel is changed, modify _every_ group
      global =
          g_slist_find_custom(darktable.control->accels_list_global,
                              darktable.control->accel_remap_str, _strcmp) != NULL;

      // Global is always active, so anything that matches there must go
      g_slist_foreach(darktable.control->accels_list_global, delete_matching_accels,
                      (gpointer)event);

      // Now check for any matching accels in the same group
      if(g_slist_find_custom(darktable.control->accels_list_lighttable,
                             darktable.control->accel_remap_str, _strcmp) || global)
        g_slist_foreach(darktable.control->accels_list_lighttable,
                        delete_matching_accels, (gpointer)event);
      if(g_slist_find_custom(darktable.control->accels_list_darkroom,
                             darktable.control->accel_remap_str, _strcmp) || global)
        g_slist_foreach(darktable.control->accels_list_darkroom,
                        delete_matching_accels, (gpointer)event);
      if(g_slist_find_custom(darktable.control->accels_list_capture,
                             darktable.control->accel_remap_str, _strcmp) || global)
        g_slist_foreach(darktable.control->accels_list_capture,
                        delete_matching_accels, (gpointer)event);

    }



    // Then update the text in the BINDING_COLUMN of each row
    update_accels_model(NULL, model);

    // Finally clear the remap state
    darktable.control->accel_remap_str = NULL;
    gtk_tree_path_free(darktable.control->accel_remap_path);
    darktable.control->accel_remap_path = NULL;

    // Save the changed keybindings
    gtk_accel_map_save(accelpath);

    return TRUE;
  }
  else if(event->keyval == GDK_BackSpace)
  {
    // If a leaf node is selected, clear that accelerator

    // If nothing is selected, or branch node selected, just return
    if(!gtk_tree_selection_get_selected(selection, &model, &iter)
      || gtk_tree_model_iter_has_child(model, &iter))
      return FALSE;

    // Otherwise, construct the proper accelerator path and delete its entry
    strcpy(accel, "<Darktable>");
    path = gtk_tree_model_get_path(model, &iter);
    path_to_accel(model, path, accel);
    gtk_tree_path_free(path);

    gtk_accel_map_change_entry(accel, 0, 0, TRUE);
    update_accels_model(NULL, model);

    // Saving the changed bindings
    gtk_accel_map_save(accelpath);

    return TRUE;
  }
  else
  {
    return FALSE;
  }
}

static void import_export(GtkButton *button, gpointer data)
{
  GtkWidget *chooser;
  gchar confdir[1024];
  gchar accelpath[1024];

  if(data)
  {
    // Non-zero value indicates export
    chooser = gtk_file_chooser_dialog_new(
        _("select file to export"),
        NULL,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
        GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser),
                                                   TRUE);
    if(dt_conf_get_string("ui_last/export_path"))
      gtk_file_chooser_set_current_folder(
          GTK_FILE_CHOOSER(chooser),
          dt_conf_get_string("ui_last/export_path"));
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), "keyboardrc");
    if(gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
    {
      gtk_accel_map_save(
          gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)));
      dt_conf_set_string(
          "ui_last/export_path",
          gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(chooser)));
    }
    gtk_widget_destroy(chooser);
  }
  else
  {
    // Zero value indicates import
    chooser = gtk_file_chooser_dialog_new(
        _("select file to import"),
        NULL,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
        GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
        NULL);

    if(dt_conf_get_string("ui_last/import_path"))
      gtk_file_chooser_set_current_folder(
          GTK_FILE_CHOOSER(chooser),
          dt_conf_get_string("ui_last/import_path"));
    if(gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
    {
      if(g_file_test(gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)),
                     G_FILE_TEST_EXISTS))
      {
        // Loading the file
        gtk_accel_map_load(
            gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)));

        // Saving to the permanent keyboardrc
        dt_get_user_config_dir(confdir, 1024);
        snprintf(accelpath, 1024, "%s/keyboardrc", confdir);
        gtk_accel_map_save(accelpath);

        dt_conf_set_string(
            "ui_last/import_path",
            gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(chooser)));
      }
    }
    gtk_widget_destroy(chooser);
  }
}

static void restore_defaults(GtkButton *button, gpointer data)
{
  GList *ops;
  dt_iop_module_so_t *op;
  gchar accelpath[256];
  gchar dir[1024];
  gchar path[1024];

  GtkWidget *message = gtk_message_dialog_new(
      NULL, GTK_DIALOG_MODAL,
      GTK_MESSAGE_WARNING,
      GTK_BUTTONS_OK_CANCEL,
      _("Are you sure you want to restore the default keybindings?  This will "
        "erase any modifications you have made."));
  if(gtk_dialog_run(GTK_DIALOG(message)) == GTK_RESPONSE_OK)
  {
    // First load the default keybindings for immediate effect
    dt_get_user_config_dir(dir, 1024);
    snprintf(path, 1024, "%s/keyboardrc_default", dir);
    gtk_accel_map_load(path);

    // Now deleting any iop show shortcuts
    ops = darktable.iop;
    while(ops)
    {
      op = (dt_iop_module_so_t*)ops->data;
      snprintf(accelpath, 256, "<Darktable>/darkroom/plugins/%s/show", op->op);
      gtk_accel_map_change_entry(accelpath, 0, 0, TRUE);
      ops = g_list_next(ops);
    }

    // Then delete any changes to the user's keyboardrc so it gets reset
    // on next startup
    dt_get_user_config_dir(dir, 1024);
    snprintf(path, 1024, "%s/keyboardrc", dir);
    g_file_delete(g_file_new_for_path(path), NULL, NULL);
  }
  gtk_widget_destroy(message);
}

static gboolean prefix_search(GtkTreeModel *model, gint column,
                              const gchar *key, GtkTreeIter *iter, gpointer d)
{
  gchar *row_data;

  gtk_tree_model_get(model, iter, TRANS_COLUMN, &row_data, -1);
  while(*key != '\0')
  {
    if(*row_data != *key)
      return TRUE;
    key++;
    row_data++;
  }
  return FALSE;
}

// Custom sort function for TreeModel entries
static gint compare_rows(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
                         gpointer data)
{
  gchar *a_text;
  gchar *b_text;

  // First prioritize branch nodes over leaves
  if(gtk_tree_model_iter_has_child(model, a)
    && !gtk_tree_model_iter_has_child(model, b))
    return -1;

  if(gtk_tree_model_iter_has_child(model, b)
    && !gtk_tree_model_iter_has_child(model, a))
    return 1;

  // Otherwise just return alphabetical order
  gtk_tree_model_get(model, a, TRANS_COLUMN, &a_text, -1);
  gtk_tree_model_get(model, b, TRANS_COLUMN, &b_text, -1);
  return strcmp(a_text, b_text);

}

#undef ACCEL_COLUMN
#undef BINDING_COLUMN
#undef N_COLUMNS
