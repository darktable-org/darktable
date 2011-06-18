/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include "preferences_gen.h"

// Values for the accelerators treeview

#define ACCEL_COLUMN 0
#define BINDING_COLUMN 1
#define N_COLUMNS 2

static void init_tab_accels(GtkWidget *book);
static void tree_insert_accel(gpointer data, const gchar *accel_path,
                              guint accel_key, GdkModifierType accel_mods,
                              gboolean changed);
static void tree_insert_rec(GtkTreeStore *model, GtkTreeIter *parent,
                            const gchar *accel_path, guint accel_key,
                            GdkModifierType accel_mods, gboolean changed);
static void path_to_accel(GtkTreeModel *model, GtkTreePath *path, gchar *str);
static void update_accels_model(GtkTreeModel *model);
static void update_accels_model_rec(GtkTreeModel *model, GtkTreeIter *parent,
                                    gchar *path);
static void delete_matching_accels(gpointer path, gpointer key_event);
static gint _strcmp(gconstpointer a, gconstpointer b);


// Signal handlers
static void tree_row_activated(GtkTreeView *tree, GtkTreePath *path,
                               GtkTreeViewColumn *column, gpointer data);
static void tree_selection_changed(GtkTreeSelection *selection, gpointer data);
static gboolean tree_key_press(GtkWidget *widget, GdkEventKey *event,
                               gpointer data);


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
  darktable.gui->accel_remap_str = NULL;
  darktable.gui->accel_remap_path = NULL;

  init_tab_gui(notebook);
  init_tab_core(notebook);
  init_tab_accels(notebook);
  gtk_widget_show_all(dialog);
  (void) gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  // Cleaning up any memory still allocated for remapping
  if(darktable.gui->accel_remap_path)
  {
    gtk_tree_path_free(darktable.gui->accel_remap_path);
    darktable.gui->accel_remap_path = NULL;
  }

}

static void init_tab_accels(GtkWidget *book)
{
  GtkWidget *container = gtk_vbox_new(FALSE, 0);
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  GtkWidget *tree = gtk_tree_view_new();
  GtkTreeStore *model = gtk_tree_store_new(N_COLUMNS,
                                           G_TYPE_STRING, G_TYPE_STRING);
  GtkCellRenderer *renderer;
  GtkTreeViewColumn *column;

  // Adding the outer container
  gtk_notebook_append_page(GTK_NOTEBOOK(book), container,
                           gtk_label_new(_("shortcuts")));

  // Building the accelerator tree
  gtk_accel_map_foreach((gpointer)model, tree_insert_accel);
  gtk_tree_sortable_set_sort_column_id(
      GTK_TREE_SORTABLE(model), ACCEL_COLUMN, GTK_SORT_ASCENDING);

  // Setting up the cell renderers
  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(
      _("accelerator"), renderer,
      "text", ACCEL_COLUMN,
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

  // Attaching the model to the treeview
  gtk_tree_view_set_model(GTK_TREE_VIEW(tree), GTK_TREE_MODEL(model));
  g_object_unref(G_OBJECT(model));

  // Adding the treeview to its containers
  gtk_container_add(GTK_CONTAINER(scroll), tree);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(container), scroll, TRUE, TRUE, 10);

}

static void tree_insert_accel(gpointer data, const gchar *accel_path,
                              guint accel_key, GdkModifierType accel_mods,
                              gboolean changed)
{

  char first[256];
  const char *src = accel_path;
  char *dest = first;

  // Stripping off the beginning <Darktable>
  while(*src != '/')
  {
    *dest = *src;
    dest++;
    src++;
  }
  *dest = '\0';

  // Ignore the <Darktable> at the beginning of each path
  if(!strcmp(first, "<Darktable>"))
    tree_insert_accel(data, src + 1, accel_key, accel_mods, changed);
  else
    tree_insert_rec((GtkTreeStore*)data, NULL, accel_path, accel_key,
                    accel_mods, changed);

}

static void tree_insert_rec(GtkTreeStore *model, GtkTreeIter *parent,
                            const gchar *accel_path, guint accel_key,
                            GdkModifierType accel_mods, gboolean changed)
{
  char first[256];
  const char *src = accel_path;
  char *dest = first;
  gchar *name;

  int i;
  gboolean found = FALSE;
  gchar *val_str;
  GtkTreeIter iter;


  // Finding the top-level portion of the path
  while(*src != '\0' && *src != '/')
  {
    *dest = *src;
    dest++;
    src++;
  }
  *dest = '\0';

  if(*src == '\0')
  {
    // Handling a leaf node
    name = gtk_accelerator_name(accel_key, accel_mods);
    gtk_tree_store_append(model, &iter, parent);
    gtk_tree_store_set(model, &iter,
                       ACCEL_COLUMN, first,
                       BINDING_COLUMN, name,
                       -1);
    g_free(name);
  }
  else
  {
    // Handling a branch node

    // First search for an already existing branch
    for(i = 0;
        i < gtk_tree_model_iter_n_children(GTK_TREE_MODEL(model), parent)
        && !found;
        i++)
    {
      gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(model), &iter, parent, i);
      gtk_tree_model_get(GTK_TREE_MODEL(model), &iter,
                         ACCEL_COLUMN, &val_str,
                         -1);

      if(!strcmp(val_str, first))
        found = TRUE;

      g_free(val_str);
    }

    if(!found)
    {
      gtk_tree_store_append(model, &iter, parent);
      gtk_tree_store_set(model, &iter,
                         ACCEL_COLUMN, first,
                         BINDING_COLUMN, "",
                         -1);
    }

    tree_insert_rec(model, &iter, src + 1, accel_key, accel_mods, changed);

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
  indices = gtk_tree_path_get_indices_with_depth(path, &depth);
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

static void update_accels_model(GtkTreeModel *model)
{
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
  if(!strcmp(path, darktable.gui->accel_remap_str))
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
    darktable.gui->accel_remap_str = accel_path;
    darktable.gui->accel_remap_path = gtk_tree_path_copy(path);
  }
}

static void tree_selection_changed(GtkTreeSelection *selection, gpointer data)
{
  GtkTreeModel *model;
  GtkTreeIter iter;

  GtkAccelKey key;

  // If remapping is currently activated, it needs to be deactivated
  if(!darktable.gui->accel_remap_str)
    return;

  model = gtk_tree_view_get_model(gtk_tree_selection_get_tree_view(selection));
  gtk_tree_model_get_iter(model, &iter, darktable.gui->accel_remap_path);

  // Restoring the BINDING_COLUMN text
  gtk_accel_map_lookup_entry(darktable.gui->accel_remap_str, &key);
  gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
                     BINDING_COLUMN,
                     gtk_accelerator_name(key.accel_key, key.accel_mods), -1);

  // Cleaning up the darktable.gui info
  darktable.gui->accel_remap_str = NULL;
  gtk_tree_path_free(darktable.gui->accel_remap_path);
  darktable.gui->accel_remap_path = NULL;
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
  if(darktable.gui->accel_remap_str)
  {
    // First delete any conflicting accelerators

    // If a global accel is changed, modify _every_ group
    global =
        g_slist_find_custom(darktable.gui->accels_list_global,
                            darktable.gui->accel_remap_str, _strcmp) != NULL;

    // Global is always active, so anything that matches there must go
    g_slist_foreach(darktable.gui->accels_list_global, delete_matching_accels,
                    (gpointer)event);

    // Now check for any matching accels in the same group
    if(g_slist_find_custom(darktable.gui->accels_list_lighttable,
                           darktable.gui->accel_remap_str, _strcmp) || global)
      g_slist_foreach(darktable.gui->accels_list_lighttable,
                      delete_matching_accels, (gpointer)event);
    if(g_slist_find_custom(darktable.gui->accels_list_darkroom,
                           darktable.gui->accel_remap_str, _strcmp) || global)
      g_slist_foreach(darktable.gui->accels_list_darkroom,
                      delete_matching_accels, (gpointer)event);
    if(g_slist_find_custom(darktable.gui->accels_list_capture,
                           darktable.gui->accel_remap_str, _strcmp) || global)
      g_slist_foreach(darktable.gui->accels_list_capture,
                      delete_matching_accels, (gpointer)event);

    // Change the accel map entry
    if(gtk_accel_map_change_entry(darktable.gui->accel_remap_str, event->keyval,
                                   event->state & KEY_STATE_MASK, TRUE))
    {
      // If it succeeded delete any conflicting accelerators

      // If a global accel is changed, modify _every_ group
      global =
          g_slist_find_custom(darktable.gui->accels_list_global,
                              darktable.gui->accel_remap_str, _strcmp) != NULL;

      // Global is always active, so anything that matches there must go
      g_slist_foreach(darktable.gui->accels_list_global, delete_matching_accels,
                      (gpointer)event);

      // Now check for any matching accels in the same group
      if(g_slist_find_custom(darktable.gui->accels_list_lighttable,
                             darktable.gui->accel_remap_str, _strcmp) || global)
        g_slist_foreach(darktable.gui->accels_list_lighttable,
                        delete_matching_accels, (gpointer)event);
      if(g_slist_find_custom(darktable.gui->accels_list_darkroom,
                             darktable.gui->accel_remap_str, _strcmp) || global)
        g_slist_foreach(darktable.gui->accels_list_darkroom,
                        delete_matching_accels, (gpointer)event);
      if(g_slist_find_custom(darktable.gui->accels_list_capture,
                             darktable.gui->accel_remap_str, _strcmp) || global)
        g_slist_foreach(darktable.gui->accels_list_capture,
                        delete_matching_accels, (gpointer)event);

    }



    // Then update the text in the BINDING_COLUMN of each row
    update_accels_model(model);

    // Finally clear the remap state
    darktable.gui->accel_remap_str = NULL;
    gtk_tree_path_free(darktable.gui->accel_remap_path);
    darktable.gui->accel_remap_path = NULL;

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
    update_accels_model(model);

    // Saving the changed bindings
    gtk_accel_map_save(accelpath);

    return TRUE;
  }
  else
  {
    return FALSE;
  }
}

#undef ACCEL_COLUMN
#undef BINDING_COLUMN
#undef N_COLUMNS
