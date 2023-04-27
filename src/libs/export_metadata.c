/*
    This file is part of darktable,
    Copyright (C) 2019-2023 darktable developers.


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
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/signal.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/gtkentry.h"
#include "imageio/imageio_module.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdlib.h>

typedef enum dt_lib_tagging_cols_t
{
  DT_LIB_EXPORT_METADATA_COL_XMP = 0,
  DT_LIB_EXPORT_METADATA_COL_TYPE,
  DT_LIB_EXPORT_METADATA_COL_FORMULA,
  DT_LIB_EXPORT_METADATA_COL_VISIBLE,
  DT_LIB_EXPORT_METADATA_NUM_COLS
} dt_lib_tagging_cols_t;

typedef struct dt_lib_export_metadata_t
{
  GtkTreeView *view;
  GtkListStore *liststore;
  GtkWidget *dialog;
  GtkTreeView *sel_view;
  GtkWidget *sel_entry;
  const gchar *sel_entry_text;
  GList *taglist;
  GtkWidget *private, *synonyms, *omithierarchy;
} dt_lib_export_metadata_t;

const GList *dt_exif_get_exiv2_taglist();

// find a string on the list
static gboolean _find_metadata_iter_per_text(GtkTreeModel *model, GtkTreeIter *iter, gint col, const char *text)
{
  if(!text) return FALSE;
  GtkTreeIter it;
  gboolean valid = gtk_tree_model_get_iter_first(model, &it);
  char *name;
  while(valid)
  {
    gtk_tree_model_get(model, &it, col, &name, -1);
    const gboolean found = g_strcmp0(text, name) == 0;
    g_free(name);
    if(found)
    {
      if(iter) *iter = it;
      return TRUE;
    }
    valid = gtk_tree_model_iter_next(model, &it);
  }
  return FALSE;
}

// add selected metadata tag to formula list
static void _add_selected_metadata(GtkTreeView *view, dt_lib_export_metadata_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(view);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(view);
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    char *tagname;
    gtk_tree_model_get(model, &iter, DT_LIB_EXPORT_METADATA_COL_XMP, &tagname, -1);
    if(!_find_metadata_iter_per_text(GTK_TREE_MODEL(d->liststore), NULL, DT_LIB_EXPORT_METADATA_COL_XMP, tagname))
    {
      gtk_list_store_append(d->liststore, &iter);
      gtk_list_store_set(d->liststore, &iter, DT_LIB_EXPORT_METADATA_COL_XMP, tagname,
                            DT_LIB_EXPORT_METADATA_COL_FORMULA, "", -1);
      selection = gtk_tree_view_get_selection(d->view);
      gtk_tree_selection_select_iter(selection, &iter);
    }
    g_free(tagname);
  }
}

// choice of a metadata tag
static gboolean _click_on_metadata_list(GtkWidget *view, GdkEventButton *event, dt_lib_export_metadata_t *d)
{
  if(event->type == GDK_2BUTTON_PRESS && event->button == 1)
  {

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    GtkTreePath *path = NULL;
    // Get tree path for row that was clicked
    if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL))
    {
      gtk_tree_selection_select_path(selection, path);
      if(event->type == GDK_2BUTTON_PRESS && event->button == 1)
      {
        _add_selected_metadata(GTK_TREE_VIEW(view), d);
        gtk_tree_path_free(path);
        return TRUE;
      }
    }
    gtk_tree_path_free(path);
  }
  return FALSE;
}

// routine to set individual visibility flag
static gboolean _set_matching_tag_visibility(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, dt_lib_export_metadata_t *d)
{
  gboolean visible;
  gchar *tagname = NULL;
  gtk_tree_model_get(model, iter, DT_LIB_EXPORT_METADATA_COL_XMP, &tagname, -1);
  if(!d->sel_entry_text[0])
    visible = TRUE;
  else
  {
    gchar *haystack = g_utf8_strdown(tagname, -1);
    gchar *needle = g_utf8_strdown(d->sel_entry_text, -1);
    visible = (g_strrstr(haystack, needle) != NULL);
    g_free(haystack);
    g_free(needle);
  }
  gtk_list_store_set(GTK_LIST_STORE(model), iter, DT_LIB_EXPORT_METADATA_COL_VISIBLE, visible, -1);
  g_free(tagname);
  return FALSE;
}

// set the metadata tag visibility aligned with filter
static void _tag_name_changed(GtkEntry *entry, dt_lib_export_metadata_t *d)
{
  d->sel_entry_text = gtk_entry_get_text(GTK_ENTRY(d->sel_entry));
  GtkTreeModel *model = gtk_tree_view_get_model(d->sel_view);
  GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
  gtk_tree_model_foreach(store, (GtkTreeModelForeachFunc)_set_matching_tag_visibility, d);
}

// dialog to add metadata tag into the formula list
static void _add_tag_button_clicked(GtkButton *button, dt_lib_export_metadata_t *d)
{
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("select tag"), GTK_WINDOW(d->dialog), GTK_DIALOG_DESTROY_WITH_PARENT,
                                       _("add"), GTK_RESPONSE_ACCEPT, _("done"), GTK_RESPONSE_NONE, NULL);
  g_signal_connect(dialog, "key-press-event", G_CALLBACK(dt_handle_dialog_enter), NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 300, -1);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_container_add(GTK_CONTAINER(area), vbox);

  GtkWidget *entry = gtk_entry_new();
  d->sel_entry = entry;
  gtk_entry_set_text(GTK_ENTRY(entry), "");
  gtk_widget_set_tooltip_text(entry, _("list filter"));
  gtk_box_pack_start(GTK_BOX(vbox), entry, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(entry), "changed", G_CALLBACK(_tag_name_changed), d);

  GtkWidget *w = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(w, DT_PIXEL_APPLY_DPI(500), DT_PIXEL_APPLY_DPI(300));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(vbox), w, TRUE, TRUE, 0);
  GtkTreeView *view = GTK_TREE_VIEW(gtk_tree_view_new());
  d->sel_view = view;
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(view));
  gtk_widget_set_tooltip_text(GTK_WIDGET(view), _("list of available tags. click 'add' button or double-click on tag to add the selected one"));
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(view), GTK_SELECTION_SINGLE);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(_("tag"), renderer, "text", 0, NULL);
  gtk_tree_view_append_column(view, col);
  renderer = gtk_cell_renderer_text_new();
  col = gtk_tree_view_column_new_with_attributes(_("type"), renderer, "text", 1, NULL);
  gtk_tree_view_append_column(view, col);
  GtkListStore *liststore = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
  GtkTreeModel *model = gtk_tree_model_filter_new(GTK_TREE_MODEL(liststore), NULL);
  gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(model), DT_LIB_EXPORT_METADATA_COL_VISIBLE);

  // populate the metadata tag list with exiv2 information
  for(GList *tag = d->taglist; tag; tag = g_list_next(tag))
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
    gtk_list_store_set(liststore, &iter, DT_LIB_EXPORT_METADATA_COL_XMP, tagname, DT_LIB_EXPORT_METADATA_COL_TYPE, type,
        DT_LIB_EXPORT_METADATA_COL_VISIBLE, TRUE, -1);
    if(type)
    {
      type--;
      type[0] = ',';
    }
  }

  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(liststore), DT_LIB_EXPORT_METADATA_COL_XMP, GTK_SORT_ASCENDING);
  gtk_tree_view_set_model(view, model);
  g_object_unref(model);
  g_signal_connect(G_OBJECT(view), "button-press-event", G_CALLBACK(_click_on_metadata_list), (gpointer)d);

  #ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
  #endif
    gtk_widget_show_all(dialog);
  while(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    _add_selected_metadata(view, d);
  }
  gtk_widget_destroy(dialog);
}

static void _remove_tag_from_list(dt_lib_export_metadata_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = GTK_TREE_MODEL(d->liststore);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(d->view);
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    gtk_list_store_remove(d->liststore, &iter);
  }
}

static void _delete_tag_button_clicked(GtkButton *button, dt_lib_export_metadata_t *d)
{
  _remove_tag_from_list(d);
}

static gboolean _key_press_on_list(GtkWidget *widget, GdkEventKey *event, dt_lib_export_metadata_t *d)
{
  if(event->type == GDK_KEY_PRESS && event->keyval == GDK_KEY_Delete && !event->state)
  {
    _remove_tag_from_list(d);
    return TRUE;
  }
  return FALSE;
}

static void _tags_toggled(GtkToggleButton *dttag, dt_lib_export_metadata_t *d)
{
  const gboolean tags = gtk_toggle_button_get_active(dttag);
  gtk_widget_set_sensitive(d->private, tags);
  gtk_widget_set_sensitive(d->synonyms, tags);
  gtk_widget_set_sensitive(d->omithierarchy, tags);
}

static void _formula_edited(GtkCellRenderer *renderer, gchar *path, gchar *new_text, dt_lib_export_metadata_t *d)
{
  GtkTreeIter iter;
  if(gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(d->liststore), &iter, path))
    gtk_list_store_set(d->liststore, &iter, DT_LIB_EXPORT_METADATA_COL_FORMULA, new_text, -1);
}

static void _formula_editing_started(GtkCellRenderer *renderer, GtkCellEditable *editable,
                                     char *path, dt_lib_export_metadata_t *d)
{
  dt_gtkentry_setup_completion(GTK_ENTRY(editable), dt_gtkentry_get_default_path_compl_list());
}

char *dt_lib_export_metadata_configuration_dialog(char *metadata_presets, const gboolean ondisk)
{
  dt_lib_export_metadata_t *d = calloc(1, sizeof(dt_lib_export_metadata_t));

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("edit metadata exportation"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                       _("cancel"), GTK_RESPONSE_NONE, _("save"), GTK_RESPONSE_ACCEPT, NULL);
  d->dialog = dialog;
  g_signal_connect(dialog, "key-press-event", G_CALLBACK(dt_handle_dialog_enter), NULL);

  GtkWidget *help = gtk_dialog_add_button(GTK_DIALOG(dialog), _("help"), GTK_RESPONSE_NONE); //GTK_RESPONSE_HELP aligns left
  dt_gui_add_help_link(help, "export_dialog");
  g_signal_handlers_disconnect_by_data(help, dialog);
  g_signal_connect(help, "clicked", G_CALLBACK(dt_gui_show_help), NULL);

  gtk_window_set_default_size(GTK_WINDOW(dialog), 300, -1);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add(GTK_CONTAINER(area), hbox);

  // general info
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_container_add(GTK_CONTAINER(hbox), vbox);
  GtkWidget *label = gtk_label_new(_("general settings"));
  gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, TRUE, 0);
  GtkWidget *vbox2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), vbox2, FALSE, TRUE, 0);

  GtkWidget *exiftag = gtk_check_button_new_with_label(_("EXIF data"));
  gtk_widget_set_tooltip_text(exiftag, _("export EXIF metadata"));
  gtk_box_pack_start(GTK_BOX(vbox2), exiftag, FALSE, TRUE, 0);
  GtkWidget *dtmetadata = gtk_check_button_new_with_label(_("metadata"));
  gtk_widget_set_tooltip_text(dtmetadata, _("export darktable XMP metadata (from metadata editor module)"));
  gtk_box_pack_start(GTK_BOX(vbox2), dtmetadata, FALSE, TRUE, 0);

  GtkWidget *calculated;
  if(!ondisk)
  {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), box, FALSE, TRUE, 0);
    GtkWidget *vbox3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), vbox3, FALSE, TRUE, 10);
    calculated = gtk_check_button_new_with_label(_("only embedded"));
    gtk_widget_set_tooltip_text(calculated, _("per default the interface sends some (limited) metadata beside the image to remote storage.\n"
        "to avoid this and let only image embedded darktable XMP metadata, check this flag.\n"
        "if remote storage doesn't understand darktable XMP metadata, you can use calculated metadata instead"));
    gtk_box_pack_start(GTK_BOX(vbox3), calculated, FALSE, TRUE, 0);
  }

  GtkWidget *geotag = gtk_check_button_new_with_label(_("geo tags"));
  gtk_widget_set_tooltip_text(geotag, _("export geo tags"));
  gtk_box_pack_start(GTK_BOX(vbox2), geotag, FALSE, TRUE, 0);
  GtkWidget *dttag = gtk_check_button_new_with_label(_("tags"));
  gtk_widget_set_tooltip_text(dttag, _("export tags (to Xmp.dc.Subject)"));
  gtk_box_pack_start(GTK_BOX(vbox2), dttag, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(dttag), "clicked", G_CALLBACK(_tags_toggled), (gpointer)d);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), box, FALSE, TRUE, 0);
  GtkWidget *vbox3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(box), vbox3, FALSE, TRUE, 10);
  d->private = gtk_check_button_new_with_label(_("private tags"));
  gtk_widget_set_tooltip_text(d->private, _("export private tags"));
  gtk_box_pack_start(GTK_BOX(vbox3), d->private, FALSE, TRUE, 0);
  d->synonyms = gtk_check_button_new_with_label(_("synonyms"));
  gtk_widget_set_tooltip_text(d->synonyms, _("export tags synonyms"));
  gtk_box_pack_start(GTK_BOX(vbox3), d->synonyms, FALSE, TRUE, 0);
  d->omithierarchy = gtk_check_button_new_with_label(_("omit hierarchy"));
  gtk_widget_set_tooltip_text(d->omithierarchy, _("only the last part of the hierarchical tags is included. can be useful if categories are not used"));
  gtk_box_pack_start(GTK_BOX(vbox3), d->omithierarchy, FALSE, TRUE, 0);

  GtkWidget *hierarchical = gtk_check_button_new_with_label(_("hierarchical tags"));
  gtk_widget_set_tooltip_text(hierarchical, _("export hierarchical tags (to Xmp.lr.Hierarchical Subject)"));
  gtk_box_pack_start(GTK_BOX(vbox2), hierarchical, FALSE, TRUE, 0);
  GtkWidget *dthistory = gtk_check_button_new_with_label(_("develop history"));
  gtk_widget_set_tooltip_text(dthistory, _("export darktable development data (recovery purpose in case of loss of database or XMP file)"));
  gtk_box_pack_start(GTK_BOX(vbox2), dthistory, FALSE, TRUE, 0);

  // specific rules
  vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_container_add(GTK_CONTAINER(hbox), vbox);
  label = gtk_label_new(_("per metadata settings"));
  gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, TRUE, 0);

  GtkWidget *w = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(w, DT_PIXEL_APPLY_DPI(450), DT_PIXEL_APPLY_DPI(100));
  gtk_widget_set_hexpand(w, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(vbox), w, TRUE, TRUE, 0);
  GtkTreeView *view = GTK_TREE_VIEW(gtk_tree_view_new());
  d->view = view;
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(view));
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(view), GTK_SELECTION_SINGLE);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(_("redefined tag"), renderer, "text", 0, NULL);
  gtk_tree_view_append_column(view, col);
  renderer = gtk_cell_renderer_text_new();
  g_object_set(renderer, "editable", TRUE, NULL);
  g_signal_connect(G_OBJECT(renderer), "edited", G_CALLBACK(_formula_edited), (gpointer)d);
  g_signal_connect(renderer, "editing-started" , G_CALLBACK(_formula_editing_started), (gpointer)d);
  col = gtk_tree_view_column_new_with_attributes(_("formula"), renderer, "text", 2, NULL);
  gtk_tree_view_append_column(view, col);
  gtk_widget_set_tooltip_text(GTK_WIDGET(view),
                _("list of calculated metadata\n"
                "click on '+' button to select and add new metadata\n"
                "if formula is empty, the corresponding metadata is removed from exported file,\n"
                "if formula is \'=\', the EXIF metadata is exported even if EXIF data are disabled\n"
                "otherwise the corresponding metadata is calculated and added to exported file\n"
                "click on formula cell to edit\n"
                "type '$(' to activate the completion and see the list of variables"));
  g_signal_connect(G_OBJECT(view), "key_press_event", G_CALLBACK(_key_press_on_list), (gpointer)d);

  GtkListStore *liststore = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  d->liststore = liststore;
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(liststore), DT_LIB_EXPORT_METADATA_COL_XMP, GTK_SORT_ASCENDING);
  gtk_tree_view_set_model(view, GTK_TREE_MODEL(liststore));
  g_object_unref(liststore);
  d->taglist = (GList *)dt_exif_get_exiv2_taglist();
  GList *list = dt_util_str_to_glist("\1", metadata_presets);
  int32_t flags = 0;
  if(list)
  {
    char *flags_hexa = list->data;
    flags = strtol(flags_hexa, NULL, 16);
    list = g_list_remove(list, flags_hexa);
    g_free(flags_hexa);
    if(list)
    {
      for(GList *tags = list; tags; tags = g_list_next(tags))
      {
        GtkTreeIter iter;
        const char *tagname = (char *)tags->data;
        tags = g_list_next(tags);
        if(!tags) break;
        const char *formula = (char *)tags->data;
        gtk_list_store_append(d->liststore, &iter);
        gtk_list_store_set(d->liststore, &iter, DT_LIB_EXPORT_METADATA_COL_XMP, tagname,
          DT_LIB_EXPORT_METADATA_COL_FORMULA, formula, -1);
      }
    }
  }
  g_list_free_full(list, g_free);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(exiftag), flags & DT_META_EXIF);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dtmetadata), flags & DT_META_METADATA);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(geotag), flags & DT_META_GEOTAG);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dttag), flags & DT_META_TAG);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->private), flags & DT_META_PRIVATE_TAG);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->synonyms), flags & DT_META_SYNONYMS_TAG);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->omithierarchy), flags & DT_META_OMIT_HIERARCHY);
  _tags_toggled(GTK_TOGGLE_BUTTON(dttag), d);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hierarchical), flags & DT_META_HIERARCHICAL_TAG);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dthistory), flags & DT_META_DT_HISTORY);
  if(!ondisk)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(calculated), flags & DT_META_CALCULATED);

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, TRUE, 0);

  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_plus_simple, 0, NULL);
  gtk_widget_set_tooltip_text(button, _("add an output metadata tag"));
  gtk_box_pack_end(GTK_BOX(box), button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_add_tag_button_clicked), (gpointer)d);

  button = dtgtk_button_new(dtgtk_cairo_paint_minus_simple, 0, NULL);
  gtk_widget_set_tooltip_text(button, _("delete metadata tag"));
  gtk_box_pack_end(GTK_BOX(box), button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_delete_tag_button_clicked), (gpointer)d);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  char *newlist = metadata_presets;
  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    const gint newflags = (
                    (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(exiftag)) ? DT_META_EXIF : 0) |
                    (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dtmetadata)) ? DT_META_METADATA : 0) |
                    (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(geotag)) ? DT_META_GEOTAG : 0) |
                    (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dttag)) ? DT_META_TAG : 0) |
                    (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->private)) ? DT_META_PRIVATE_TAG : 0) |
                    (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->synonyms)) ? DT_META_SYNONYMS_TAG : 0) |
                    (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->omithierarchy)) ? DT_META_OMIT_HIERARCHY : 0) |
                    (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hierarchical)) ? DT_META_HIERARCHICAL_TAG : 0) |
                    (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dthistory)) ? DT_META_DT_HISTORY : 0) |
                    (!ondisk  ? (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(calculated)) ? DT_META_CALCULATED : 0) : 0)
                    );

    newlist = g_strdup_printf("%x", newflags);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(d->liststore), &iter);
    while(valid)
    {
      char *tagname, *formula;
      gtk_tree_model_get(GTK_TREE_MODEL(d->liststore), &iter, DT_LIB_EXPORT_METADATA_COL_XMP, &tagname,
          DT_LIB_EXPORT_METADATA_COL_FORMULA, &formula, -1);
      // metadata presets are stored into a single string with '\1' as a separator
      newlist = dt_util_dstrcat(newlist,"\1%s\1%s", tagname, formula);
      g_free(tagname);
      g_free(formula);
      valid = gtk_tree_model_iter_next (GTK_TREE_MODEL(d->liststore), &iter);
    }
    g_free(metadata_presets);
    dt_lib_export_metadata_set_conf(newlist);
  }
  gtk_widget_destroy(dialog);
  free(d);
  return newlist;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

