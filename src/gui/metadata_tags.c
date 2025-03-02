/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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

#include "common/exif.h"
#include "gui/gtk.h"
#include "gui/metadata_tags.h"

typedef enum dt_metadata_tag_cols_t
{
  DT_METADATA_TAGS_COL_XMP = 0,
  DT_METADATA_TAGS_COL_TYPE,
  DT_METADATA_TAGS_COL_VISIBLE,
  DT_METADATA_TAGS_NUM_COLS
} dt_metadata_tag_cols_t;

static GtkListStore *liststore;
static GtkWidget *sel_entry;
static const gchar *sel_entry_text;
static GtkTreeView *sel_view;
static GList *taglist = NULL;
static GtkWidget *add_button;

// routine to set individual visibility flag
static gboolean _set_matching_tag_visibility(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer user_idata)
{
  gboolean visible;
  gchar *tagname = NULL;
  gtk_tree_model_get(model, iter, DT_METADATA_TAGS_COL_XMP, &tagname, -1);
  if(!sel_entry_text[0])
    visible = TRUE;
  else
  {
    gchar *haystack = g_utf8_strdown(tagname, -1);
    gchar *needle = g_utf8_strdown(sel_entry_text, -1);
    visible = (g_strrstr(haystack, needle) != NULL);
    g_free(haystack);
    g_free(needle);
  }
  gtk_list_store_set(GTK_LIST_STORE(model), iter, DT_METADATA_TAGS_COL_VISIBLE, visible, -1);
  g_free(tagname);
  return FALSE;
}

// set the metadata tag visibility aligned with filter
static void _tag_name_changed(GtkEntry *entry, gpointer user_data)
{
  sel_entry_text = gtk_entry_get_text(GTK_ENTRY(sel_entry));
  GtkTreeModel *model = gtk_tree_view_get_model(sel_view);
  GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
  gtk_tree_model_foreach(store, (GtkTreeModelForeachFunc)_set_matching_tag_visibility, NULL);
}

gchar *dt_metadata_tags_get_selected()
{
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(sel_view);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(sel_view);
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    gchar *tagname;
    gtk_tree_model_get(model, &iter, DT_METADATA_TAGS_COL_XMP, &tagname, -1);
    return tagname;
  }
  return NULL;
}

static void _tree_selection_change(GtkTreeSelection *selection, gpointer user_data)
{
  const int nb = gtk_tree_selection_count_selected_rows(selection);
  gtk_widget_set_sensitive(add_button, nb > 0);
}

GtkWidget *dt_metadata_tags_dialog(GtkWidget *parent, gpointer metadata_activated_callback, gpointer user_data)
{
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("select tag"), GTK_WINDOW(parent),
                                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  _("_add"), GTK_RESPONSE_ACCEPT,
                                                  _("_done"), GTK_RESPONSE_NONE, NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_NONE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), DT_PIXEL_APPLY_DPI(500), DT_PIXEL_APPLY_DPI(300));
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_box_pack_start(GTK_BOX(area), vbox, TRUE, TRUE, 0);

  // keep a reference to the "add" button to toggle its sensitivity
  add_button = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

  sel_entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(sel_entry), "");
  gtk_widget_set_tooltip_text(sel_entry, _("list filter"));
  gtk_entry_set_activates_default(GTK_ENTRY(sel_entry), TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), sel_entry, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(sel_entry), "changed", G_CALLBACK(_tag_name_changed), NULL);

  GtkWidget *w = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(vbox), w, TRUE, TRUE, 0);
  sel_view = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(sel_view));
  gtk_widget_set_tooltip_text(GTK_WIDGET(sel_view), _("list of available tags. click 'add' button or double-click on tag to add the selected one"));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(sel_view);
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
  g_signal_connect(selection, "changed", G_CALLBACK(_tree_selection_change), NULL);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(_("tag"), renderer, "text", 0, NULL);
  gtk_tree_view_append_column(sel_view, col);
  renderer = gtk_cell_renderer_text_new();
  col = gtk_tree_view_column_new_with_attributes(_("type"), renderer, "text", 1, NULL);
  gtk_tree_view_append_column(sel_view, col);
  liststore = gtk_list_store_new(DT_METADATA_TAGS_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
  GtkTreeModel *model = gtk_tree_model_filter_new(GTK_TREE_MODEL(liststore), NULL);
  gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(model), DT_METADATA_TAGS_COL_VISIBLE);

  // populate the metadata tag list with exiv2 information
  if(!taglist)
    taglist = (GList *) dt_exif_get_exiv2_taglist();

  for(GList *tag = taglist; tag; tag = g_list_next(tag))
  {
    GtkTreeIter iter;
    gtk_list_store_append(liststore, &iter);
    const char *tagname = tag->data;
    char *type = g_strstr_len(tagname, -1, ",");
    if(type)
    {
      type[0] = '\0';
      type++;
    }

    gtk_list_store_set(liststore,
                       &iter,
                       DT_METADATA_TAGS_COL_XMP, tagname,
                       DT_METADATA_TAGS_COL_TYPE, type,
                       DT_METADATA_TAGS_COL_VISIBLE, TRUE,
                       -1);

    if(type)
    {
      type--;
      type[0] = ',';
    }
  }

  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(liststore), DT_METADATA_TAGS_COL_XMP, GTK_SORT_ASCENDING);
  gtk_tree_view_set_model(sel_view, model);
  g_object_unref(model);
  g_signal_connect(G_OBJECT(sel_view), "row-activated", G_CALLBACK(metadata_activated_callback), user_data);

  return dialog;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

