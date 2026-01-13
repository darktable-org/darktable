/*
    This file is part of darktable,
    Copyright (C) 2019-2026 darktable developers.


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
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "gui/gtkentry.h"
#include "gui/metadata_tags.h"
#include "imageio/imageio_module.h"
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
  GtkWidget *private, *synonyms, *omithierarchy;
} dt_lib_export_metadata_t;

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
static void _add_selected_metadata(gchar *tagname, dt_lib_export_metadata_t *d)
{
  GtkTreeIter iter;
  if(!_find_metadata_iter_per_text(GTK_TREE_MODEL(d->liststore), NULL, DT_LIB_EXPORT_METADATA_COL_XMP, tagname))
  {
    gtk_list_store_insert_with_values(d->liststore, &iter, -1,
                       DT_LIB_EXPORT_METADATA_COL_XMP, tagname,
                       DT_LIB_EXPORT_METADATA_COL_FORMULA, "",
                       -1);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(d->view);
    gtk_tree_selection_select_iter(selection, &iter);
  }
  g_free(tagname);
}

// choice of a metadata tag
static void _metadata_activated(GtkTreeView *tree_view,
                                GtkTreePath *path,
                                GtkTreeViewColumn *column,
                                dt_lib_export_metadata_t *d)
{
  gchar *tagname = dt_metadata_tags_get_selected();
  _add_selected_metadata(tagname, d);
}

// dialog to add metadata tag into the formula list
static void _add_tag_button_clicked(GtkButton *button, dt_lib_export_metadata_t *d)
{
  GtkWidget *dialog = dt_metadata_tags_dialog(d->dialog, FALSE, _metadata_activated, d);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif

  gtk_widget_show_all(dialog);
  while(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *tagname = dt_metadata_tags_get_selected();
    _add_selected_metadata(tagname, d);
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

char *dt_lib_export_metadata_configuration_dialog(char *metadata_presets, const gboolean ondisk)
{
  GtkCellEditable *active_editable = NULL;

  dt_lib_export_metadata_t *d = calloc(1, sizeof(dt_lib_export_metadata_t));

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("edit metadata exportation"), GTK_WINDOW(win),
                                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  _("_cancel"), GTK_RESPONSE_NONE,
                                                  _("_save"), GTK_RESPONSE_ACCEPT, NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  dt_gui_dialog_add_help(GTK_DIALOG(dialog), "export_dialog");

  d->dialog = dialog;

  gtk_window_set_default_size(GTK_WINDOW(dialog), DT_PIXEL_APPLY_DPI(500), -1);

  // general info
  GtkWidget *exiftag = gtk_check_button_new_with_label(_("EXIF data"));
  gtk_widget_set_tooltip_text(exiftag, _("export EXIF metadata"));
  GtkWidget *dtmetadata = gtk_check_button_new_with_label(_("metadata"));
  gtk_widget_set_tooltip_text(dtmetadata, _("export darktable XMP metadata (from metadata editor module)"));

  GtkWidget *calculated;
  if(!ondisk)
  {
    calculated = gtk_check_button_new_with_label(_("only embedded"));
    gtk_widget_set_tooltip_text(calculated, _("per default the interface sends some (limited) metadata beside the image to remote storage.\n"
        "to avoid this and let only image embedded darktable XMP metadata, check this flag.\n"
        "if remote storage doesn't understand darktable XMP metadata, you can use calculated metadata instead"));
    gtk_widget_set_margin_start(calculated, DT_PIXEL_APPLY_DPI(10));
  }
  else
    calculated = gtk_grid_new(); // just empty widget

  GtkWidget *geotag = gtk_check_button_new_with_label(_("geo tags"));
  gtk_widget_set_tooltip_text(geotag, _("export geo tags"));

  GtkWidget *dttag = gtk_check_button_new_with_label(_("tags"));
  gtk_widget_set_tooltip_text(dttag, _("export tags (to Xmp.dc.Subject)"));
  g_signal_connect(G_OBJECT(dttag), "clicked", G_CALLBACK(_tags_toggled), (gpointer)d);

  d->private = gtk_check_button_new_with_label(_("private tags"));
  gtk_widget_set_tooltip_text(d->private, _("export private tags"));
  gtk_widget_set_margin_start(d->private, DT_PIXEL_APPLY_DPI(10));
  d->synonyms = gtk_check_button_new_with_label(_("synonyms"));
  gtk_widget_set_tooltip_text(d->synonyms, _("export tags synonyms"));
  gtk_widget_set_margin_start(d->synonyms, DT_PIXEL_APPLY_DPI(10));
  d->omithierarchy = gtk_check_button_new_with_label(_("omit hierarchy"));
  gtk_widget_set_tooltip_text(d->omithierarchy, _("only the last part of the hierarchical tags is included. can be useful if categories are not used"));
  gtk_widget_set_margin_start(d->omithierarchy, DT_PIXEL_APPLY_DPI(10));

  GtkWidget *hierarchical = gtk_check_button_new_with_label(_("hierarchical tags"));
  gtk_widget_set_tooltip_text(hierarchical, _("export hierarchical tags (to Xmp.lr.Hierarchical Subject)"));
  GtkWidget *dthistory = gtk_check_button_new_with_label(_("develop history"));
  gtk_widget_set_tooltip_text(dthistory, _("export darktable development data (recovery purpose in case of loss of database or XMP file)"));

  // specific rules
  GtkTreeView *view = d->view = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(view), GTK_SELECTION_SINGLE);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(_("redefined tag"), renderer, "text", 0, NULL);
  gtk_tree_view_append_column(view, col);
  renderer = gtk_cell_renderer_text_new();
  g_object_set(renderer, "editable", TRUE, NULL);
  g_signal_connect(G_OBJECT(renderer), "edited", G_CALLBACK(_formula_edited), (gpointer)d);
  dt_gui_commit_on_focus_loss(renderer, &active_editable);
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
  GList *list = dt_util_str_to_glist("\1", metadata_presets);
  int32_t flags = 0;
  if(!g_list_is_empty(list))
  {
    char *flags_hexa = list->data;
    flags = strtol(flags_hexa, NULL, 16);
    list = g_list_remove(list, flags_hexa);
    g_free(flags_hexa);
    if(!g_list_is_empty(list))
    {
      for(GList *tags = list; tags; tags = g_list_next(tags))
      {
        const char *tagname = (char *)tags->data;
        tags = g_list_next(tags);
        if(!tags) break;
        const char *formula = (char *)tags->data;
        gtk_list_store_insert_with_values(d->liststore, NULL, -1,
          DT_LIB_EXPORT_METADATA_COL_XMP, tagname,
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

  GtkWidget *plus = dtgtk_button_new(dtgtk_cairo_paint_plus_simple, 0, NULL);
  gtk_widget_set_tooltip_text(plus, _("add an output metadata tag"));
  g_signal_connect(G_OBJECT(plus), "clicked", G_CALLBACK(_add_tag_button_clicked), (gpointer)d);

  GtkWidget *minus = dtgtk_button_new(dtgtk_cairo_paint_minus_simple, 0, NULL);
  gtk_widget_set_tooltip_text(minus, _("delete metadata tag"));
  g_signal_connect(G_OBJECT(minus), "clicked", G_CALLBACK(_delete_tag_button_clicked), (gpointer)d);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_set_margin_top(exiftag, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_top(GTK_WIDGET(view), DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_start(GTK_WIDGET(view), DT_PIXEL_APPLY_DPI(8));

  dt_gui_dialog_add(GTK_DIALOG(dialog), dt_gui_hbox(
                    dt_gui_vbox(gtk_label_new(_("general settings")),
                                exiftag, dtmetadata, calculated, geotag,
                                dttag, d->private, d->synonyms, d->omithierarchy,
                                hierarchical, dthistory),
                    dt_gui_vbox(gtk_label_new(_("per metadata settings")),
                                dt_gui_scroll_wrap(GTK_WIDGET(view)),
                                dt_gui_hbox(dt_gui_expand(dt_gui_align_right(minus)), plus))));
  gtk_widget_show_all(dialog);

  char *newlist = metadata_presets;
  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    if(active_editable)
      gtk_cell_editable_editing_done(active_editable);

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
      dt_util_str_cat(&newlist,"\1%s\1%s", tagname, formula);
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
