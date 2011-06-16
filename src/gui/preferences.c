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

#include "common/darktable.h"
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

  init_tab_gui(notebook);
  init_tab_core(notebook);
  init_tab_accels(notebook);
  gtk_widget_show_all(dialog);
  (void) gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
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

  // Setting up the treeview
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

  gtk_tree_view_set_model(GTK_TREE_VIEW(tree), GTK_TREE_MODEL(model));
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

#undef ACCEL_COLUMN
#undef BINDING_COLUMN
#undef N_COLUMNS
