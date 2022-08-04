/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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
#include "common/collection.h"
#include "common/styles.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/styles.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <libxml/parser.h>

DT_MODULE(1)

typedef struct dt_lib_styles_t
{
  GtkEntry *entry;
  GtkWidget *duplicate;
  GtkTreeView *tree;
  GtkWidget *create_button, *edit_button, *delete_button, *import_button, *export_button, *applymode, *apply_button;
} dt_lib_styles_t;


const char *name(dt_lib_module_t *self)
{
  return _("Styles");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

int position()
{
  return 599;
}

typedef enum _styles_columns_t
{
  DT_STYLES_COL_NAME = 0,
  DT_STYLES_COL_TOOLTIP,
  DT_STYLES_COL_FULLNAME,
  DT_STYLES_NUM_COLS
} _styles_columns_t;

static gboolean _get_node_for_name(GtkTreeModel *model, gboolean root, GtkTreeIter *iter, const gchar *parent_name)
{
  GtkTreeIter parent = *iter;

  if(root)
  {
    // iter is null, we are at the top level
    // if we have no nodes in this tree, let's create it now
    if(!gtk_tree_model_get_iter_first(model, iter))
    {
      gtk_tree_store_append(GTK_TREE_STORE(model), iter, NULL);
      return FALSE;
    }
  }
  else
  {
    // if we have no children, create one, this is our node
    if(!gtk_tree_model_iter_children(GTK_TREE_MODEL(model), iter, &parent))
    {
      gtk_tree_store_append(GTK_TREE_STORE(model), iter, &parent);
      return FALSE;
    }
  }

  // here we have iter to be on the right level, let's check if we can find parent_name
  do
  {
    gchar *name;
    gtk_tree_model_get(model, iter, DT_STYLES_COL_NAME, &name, -1);
    const gboolean match = !g_strcmp0(name, parent_name);
    g_free(name);
    if(match)
    {
      return TRUE;
    }
  }
  while(gtk_tree_model_iter_next(model, iter));

  // not found, create it under parent
  gtk_tree_store_append(GTK_TREE_STORE(model), iter, root?NULL:&parent);

  return FALSE;
}

static void _gui_styles_update_view(dt_lib_styles_t *d)
{
  /* clear current list */
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->tree));
  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->tree), NULL);
  gtk_tree_store_clear(GTK_TREE_STORE(model));

  GList *result = dt_styles_get_list(gtk_entry_get_text(d->entry));
  if(result)
  {
    for(const GList *res_iter = result; res_iter; res_iter = g_list_next(res_iter))
    {
      dt_style_t *style = (dt_style_t *)res_iter->data;

      gchar *items_string = (gchar *)dt_styles_get_item_list_as_string(style->name);
      gchar *tooltip = NULL;

      if(style->description && *style->description)
      {
        tooltip
            = g_strconcat("<b>", g_markup_escape_text(style->description, -1), "</b>\n", items_string, NULL);
      }
      else
      {
        tooltip = g_strdup(items_string);
      }

      gchar **split = g_strsplit(style->name, "|", 0);
      int k = 0;

      while(split[k])
      {
        const gchar *s = split[k];
        const gboolean node_found = _get_node_for_name(model, k==0, &iter, s);

        if(!node_found)
        {
          if(split[k+1])
          {
            gtk_tree_store_set(GTK_TREE_STORE(model), &iter, DT_STYLES_COL_NAME, s, -1);
          }
          else
          {
            // a leaf
            gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
                               DT_STYLES_COL_NAME, s, DT_STYLES_COL_TOOLTIP, tooltip, DT_STYLES_COL_FULLNAME, style->name, -1);
          }
        }
        k++;
      }
      g_strfreev(split);

      g_free(items_string);
      g_free(tooltip);
    }
    g_list_free_full(result, dt_style_free);
  }

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(d->tree), DT_STYLES_COL_TOOLTIP);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->tree), model);
  g_object_unref(model);
}

static void _styles_row_activated_callback(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col,
                                           gpointer user_data)
{
  // This works on double click, so it's for single style
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;

  GtkTreeModel *model;
  GtkTreeIter iter;
  model = gtk_tree_view_get_model(d->tree);

  if(!gtk_tree_model_get_iter(model, &iter, path)) return;

  gchar *name;
  gtk_tree_model_get(model, &iter, DT_STYLES_COL_FULLNAME, &name, -1);

  GList *list = dt_act_on_get_images(TRUE, TRUE, FALSE);
  if(name)
  {
    dt_styles_apply_to_list(name, list, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate)));
    g_free(name);
  }
  g_list_free(list);
}

// get list of style names from selection
// free returned list with g_list_free_full(list, g_free)
GList* _get_selected_style_names(GList* selected_styles, GtkTreeModel *model)
{
  GtkTreeIter iter;
  GList *style_names = NULL;
  for (const GList *style = selected_styles; style; style = g_list_next(style))
  {
    GValue value = {0,};
    gtk_tree_model_get_iter(model, &iter, (GtkTreePath *)style->data);
    gtk_tree_model_get_value(model, &iter, DT_STYLES_COL_FULLNAME, &value);
    if(G_VALUE_HOLDS_STRING(&value))
      style_names = g_list_prepend(style_names, g_strdup(g_value_get_string(&value)));
    g_value_unset(&value);
  }
  return g_list_reverse(style_names); // list was built in reverse order, so un-reverse it
}

static void apply_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree));

  if(gtk_tree_selection_count_selected_rows(selection) == 0) return;

  GtkTreeModel *model= gtk_tree_view_get_model(d->tree);
  GList *selected_styles = gtk_tree_selection_get_selected_rows(selection, &model);
  GList *style_names = _get_selected_style_names(selected_styles, model);
  g_list_free_full(selected_styles, (GDestroyNotify) gtk_tree_path_free);

  if(style_names == NULL) return;

  GList *list = dt_act_on_get_images(TRUE, TRUE, FALSE);

  if(list) dt_multiple_styles_apply_to_list(style_names, list, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate)));

  g_list_free_full(style_names, g_free);
  g_list_free(list);
}

static void create_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;

  GList *list = dt_act_on_get_images(TRUE, TRUE, FALSE);
  dt_styles_create_from_list(list);
  g_list_free(list);
  _gui_styles_update_view(d);
}

static void edit_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree));

  if(gtk_tree_selection_count_selected_rows(selection) == 0) return;

  GtkTreeIter iter;
  GtkTreeModel *model= gtk_tree_view_get_model(d->tree);

  GList *styles = gtk_tree_selection_get_selected_rows(selection, &model);
  for (const GList *style = styles; style; style = g_list_next(style))
  {
    char *name = NULL;
    GValue value = {0,};
    gtk_tree_model_get_iter(model, &iter, (GtkTreePath *)style->data);
    gtk_tree_model_get_value(model, &iter, DT_STYLES_COL_FULLNAME, &value);
    if(G_VALUE_HOLDS_STRING(&value))
      name = g_strdup(g_value_get_string(&value));
    g_value_unset(&value);

    if(name)
    {
      dt_gui_styles_dialog_edit(name);
      _gui_styles_update_view(d);
      g_free(name);
    }
  }
  g_list_free_full (styles, (GDestroyNotify) gtk_tree_path_free);
}

gboolean _ask_before_delete_style(const gint style_cnt)
{
  gint res = GTK_RESPONSE_YES;

  if(dt_conf_get_bool("plugins/lighttable/style/ask_before_delete_style"))
  {
    const GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
    GtkWidget *dialog = gtk_message_dialog_new
      (GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
       ngettext("Do you really want to remove %d style?", "Do you really want to remove %d styles?", style_cnt),
       style_cnt);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif

    gtk_window_set_title(GTK_WINDOW(dialog), ngettext("Remove style?", "Remove styles?", style_cnt));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }

  return res == GTK_RESPONSE_YES;
}

static void delete_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree));

  if(gtk_tree_selection_count_selected_rows(selection) == 0) return;

  GtkTreeModel *model= gtk_tree_view_get_model(d->tree);
  GList *selected_styles = gtk_tree_selection_get_selected_rows(selection, &model);
  GList *style_names = _get_selected_style_names(selected_styles, model);
  g_list_free_full(selected_styles, (GDestroyNotify) gtk_tree_path_free);

  if(style_names == NULL) return;

  const gint select_cnt = g_list_length(style_names);
  const gboolean single_raise = (select_cnt == 1);

  const gboolean can_delete = _ask_before_delete_style(select_cnt);

  if(can_delete)
  {
    dt_database_start_transaction(darktable.db);

    for (const GList *style = style_names; style; style = g_list_next(style))
    {
      dt_styles_delete_by_name_adv((char*)style->data, single_raise);
    }

    if(!single_raise) {
      // raise signal at the end of processing all styles if we have more than 1 to delete
      // this also calls _gui_styles_update_view
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_STYLE_CHANGED);
    }
    dt_database_release_transaction(darktable.db);
  }
  g_list_free_full(style_names, g_free);
}

static void export_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree));

  if(gtk_tree_selection_count_selected_rows(selection) == 0) return;

  GtkTreeModel *model= gtk_tree_view_get_model(d->tree);
  GList *selected_styles = gtk_tree_selection_get_selected_rows(selection, &model);
  GList *style_names = _get_selected_style_names(selected_styles, model);
  g_list_free_full(selected_styles, (GDestroyNotify) gtk_tree_path_free);

  if(style_names == NULL) return;

  /* variables for overwrite dialog */
  gint overwrite_check_button = 0;
  gint overwrite = 0;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
        _("Select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        _("_Save"), _("_Cancel"));

  dt_conf_get_folder_to_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(filechooser));
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *filedir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));

    for (const GList *style = style_names; style; style = g_list_next(style))
    {
      char stylename[520];

      /* check if file exists before overwriting */
      snprintf(stylename, sizeof(stylename), "%s/%s.dtstyle", filedir, (char*)style->data);

      if(g_file_test(stylename, G_FILE_TEST_EXISTS) == TRUE)
      {
        /* do not run overwrite dialog */
        if(overwrite_check_button == 1)
        {
          if(overwrite == 1)
          {
            // save style with overwrite
            dt_styles_save_to_file((char*)style->data, filedir, TRUE);
          }
          else if(overwrite == 2)
          {
            continue;
          }
          else
          {
            break;
          }
        }
        else
        {
          /* create and run dialog */
          char overwrite_str[256];

          gint overwrite_dialog_res = GTK_RESPONSE_ACCEPT;
          gint overwrite_dialog_check_button_res = TRUE;

          if(dt_conf_get_bool("plugins/lighttable/style/ask_before_delete_style"))
          {
            GtkWidget *dialog_overwrite_export = gtk_dialog_new_with_buttons(_("Overwrite style?"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                _("Cancel"), GTK_RESPONSE_CANCEL,
                _("Skip"), GTK_RESPONSE_NONE,
                _("Overwrite"), GTK_RESPONSE_ACCEPT, NULL);

            // contents for dialog
            GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog_overwrite_export));
            sprintf(overwrite_str, _("Style `%s' already exists.\nDo you want to overwrite existing style?\n"), (char*)style->data);
            GtkWidget *label = gtk_label_new(overwrite_str);
            GtkWidget *overwrite_dialog_check_button = gtk_check_button_new_with_label(_("Apply this option to all existing styles"));

            gtk_container_add(GTK_CONTAINER(content_area), label);
            gtk_container_add(GTK_CONTAINER(content_area), overwrite_dialog_check_button);
            gtk_widget_show_all(dialog_overwrite_export);

            // disable check button and skip button when only one style is selected
            if(g_list_is_singleton(style_names))
            {
              gtk_widget_set_sensitive(overwrite_dialog_check_button, FALSE);
              gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog_overwrite_export), GTK_RESPONSE_NONE, FALSE);
            }

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog_overwrite_export);
#endif

            overwrite_dialog_res = gtk_dialog_run(GTK_DIALOG(dialog_overwrite_export));
            overwrite_dialog_check_button_res = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(overwrite_dialog_check_button));
            gtk_widget_destroy(dialog_overwrite_export);
          }

          if(overwrite_dialog_res == GTK_RESPONSE_ACCEPT)
          {
            overwrite = 1;

            /* do not run dialog on the next conflict when set to 1 */
            if(overwrite_dialog_check_button_res == TRUE)
            {
              overwrite_check_button = 1;
            }
            else
            {
              overwrite_check_button = 0;
            }
          }
          else if(overwrite_dialog_res == GTK_RESPONSE_NONE)
          {
            overwrite = 2;

            /* do not run dialog on the next conflict when set to 1 */
            if(overwrite_dialog_check_button_res == TRUE)
            {
              overwrite_check_button = 1;
            }
            else
            {
              overwrite_check_button = 0;
            }
            continue;
          }
          else
          {
            break;
          }

          dt_styles_save_to_file((char*)style->data, filedir, TRUE);
        }
      }
      else
      {
        dt_styles_save_to_file((char*)style->data, filedir, FALSE);
      }
      dt_control_log(_("Style %s was successfully exported"), (char*)style->data);
    }
    dt_conf_set_folder_from_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(filechooser));
    g_free(filedir);
  }
  g_object_unref(filechooser);
  g_list_free_full(style_names, g_free);
}

static void import_clicked(GtkWidget *w, gpointer user_data)
{
  /* variables for overwrite dialog */
  gint overwrite_check_button = 0;
  gint overwrite = 0;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
        _("Select style"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
        _("_Open"), _("_Cancel"));

  dt_conf_get_folder_to_file_chooser("ui_last/import_path", GTK_FILE_CHOOSER(filechooser));
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), TRUE);

  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.dtstyle");
  gtk_file_filter_add_pattern(filter, "*.DTSTYLE");
  gtk_file_filter_set_name(filter, _("Darktable style files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("All files"));

  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    GSList *filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(filechooser));

    for(const GSList *filename = filenames; filename; filename = g_slist_next(filename))
    {
      /* extract name from xml file */
      gchar *bname = NULL;
      xmlDoc *document = xmlReadFile((char*)filename->data, NULL, XML_PARSE_NOBLANKS);
      xmlNode *root = NULL;
      if(document != NULL)
        root = xmlDocGetRootElement(document);

      if(document == NULL || root == NULL || xmlStrcmp(root->name, BAD_CAST "darktable_style"))
      {
        dt_print(DT_DEBUG_CONTROL,
                 "[styles] file %s is not a style file\n", (char*)filename->data);
        if(document)
          xmlFreeDoc(document);
        continue;
      }

      for(xmlNode *node = root->children->children; node; node = node->next)
      {
        if(node->type == XML_ELEMENT_NODE)
        {
          if(strcmp((char*)node->name, "name") == 0)
          {
            bname = g_strdup((char*)xmlNodeGetContent(node));
            break;
          }
        }
      }

      // xml doc is not necessary after this point
      xmlFreeDoc(document);

      if(!bname){
        dt_print(DT_DEBUG_CONTROL,
                 "[styles] file %s is malformed style file\n", (char*)filename->data);
        continue;
      }

      // check if style exists
      if(dt_styles_exists(bname))
      {
        /* do not run overwrite dialog */
        if(overwrite_check_button == 1)
        {
          if(overwrite == 1)
          {
            // remove style then import
            dt_styles_delete_by_name(bname);
            dt_styles_import_from_file((char*)filename->data);
          }
          else if(overwrite == 2)
          {
            continue;
          }
          else
          {
            break;
          }
        }
        else
        {
          /* create and run dialog */
          char overwrite_str[256];

          gint overwrite_dialog_res = GTK_RESPONSE_ACCEPT;
          gint overwrite_dialog_check_button_res = TRUE;

          // use security check/option
          if(dt_conf_get_bool("plugins/lighttable/style/ask_before_delete_style"))
          {
            GtkWidget *dialog_overwrite_import = gtk_dialog_new_with_buttons(_("Overwrite style?"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                _("Cancel"), GTK_RESPONSE_CANCEL,
                _("Skip"), GTK_RESPONSE_NONE,
                _("Overwrite"), GTK_RESPONSE_ACCEPT, NULL);

            // contents for dialog
            GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog_overwrite_import));
            sprintf(overwrite_str, _("Style `%s' already exists.\nDo you want to overwrite existing style?\n"), (char*)filename->data);
            GtkWidget *label = gtk_label_new(overwrite_str);
            GtkWidget *overwrite_dialog_check_button = gtk_check_button_new_with_label(_("Apply this option to all existing styles"));

            gtk_container_add(GTK_CONTAINER(content_area), label);
            gtk_container_add(GTK_CONTAINER(content_area), overwrite_dialog_check_button);
            gtk_widget_show_all(dialog_overwrite_import);

            // disable check button and skip button when dealing with one style
            if(g_slist_length(filenames) == 1)
            {
              gtk_widget_set_sensitive(overwrite_dialog_check_button, FALSE);
              gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog_overwrite_import), GTK_RESPONSE_NONE, FALSE);
            }

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog_overwrite_import);
#endif

            overwrite_dialog_res = gtk_dialog_run(GTK_DIALOG(dialog_overwrite_import));
            overwrite_dialog_check_button_res = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(overwrite_dialog_check_button));
            gtk_widget_destroy(dialog_overwrite_import);
          }

          if(overwrite_dialog_res == GTK_RESPONSE_ACCEPT)
          {
            overwrite = 1;

            /* do not run dialog on next conflict when set to 1 */
            if(overwrite_dialog_check_button_res == TRUE)
            {
              overwrite_check_button = 1;
            }
            else
            {
              overwrite_check_button = 0;
            }
          }
          else if(overwrite_dialog_res == GTK_RESPONSE_NONE)
          {
            overwrite = 2;


            /* do not run dialog on next conflict when set to 1 */
            if(overwrite_dialog_check_button_res == TRUE)
            {
              overwrite_check_button = 1;
            }
            else
            {
              overwrite_check_button = 0;
            }
            continue;
          }
          else
          {
            break;
          }

          dt_styles_delete_by_name(bname);
          dt_styles_import_from_file((char*)filename->data);
        }
      }
      else
      {
        dt_styles_import_from_file((char*)filename->data);
      }
      g_free(bname);
    }
    g_slist_free_full(filenames, g_free);

    dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
    _gui_styles_update_view(d);
    dt_conf_set_folder_from_file_chooser("ui_last/import_path", GTK_FILE_CHOOSER(filechooser));
  }
  g_object_unref(filechooser);
}

static gboolean entry_callback(GtkEntry *entry, gpointer user_data)
{
  _gui_styles_update_view(user_data);
  return FALSE;
}

static gboolean entry_activated(GtkEntry *entry, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  const gchar *name = gtk_entry_get_text(d->entry);
  if(name)
  {
    GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
    dt_styles_apply_to_list(name, imgs, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate)));
    g_list_free(imgs);
  }

  return FALSE;
}

static gboolean duplicate_callback(GtkEntry *entry, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  dt_conf_set_bool("ui_last/styles_create_duplicate",
                   gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate)));
  return FALSE;
}

static void applymode_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int mode = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/lighttable/style/applymode", mode);
}

static void _update(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_styles_t *d = (dt_lib_styles_t *)self->data;

  const gboolean has_act_on = (dt_act_on_get_images_nb(TRUE, FALSE) > 0);

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree));
  const gint sel_styles_cnt = gtk_tree_selection_count_selected_rows(selection);

  gtk_widget_set_sensitive(GTK_WIDGET(d->create_button), has_act_on);
  gtk_widget_set_sensitive(GTK_WIDGET(d->edit_button), sel_styles_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->delete_button), sel_styles_cnt > 0);

  //import is ALWAYS enabled.
  gtk_widget_set_sensitive(GTK_WIDGET(d->export_button), sel_styles_cnt > 0);

  gtk_widget_set_sensitive(GTK_WIDGET(d->apply_button), has_act_on && sel_styles_cnt > 0);
}

static void _styles_changed_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_styles_t *d = (dt_lib_styles_t *)self->data;
  _gui_styles_update_view(d);
  _update(self);
}

static void _image_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  _update(self);
}

static void _collection_updated_callback(gpointer instance, dt_collection_change_t query_change,
                                         dt_collection_properties_t changed_property, gpointer imgs, int next,
                                         dt_lib_module_t *self)
{
  _update(self);
}

static void _mouse_over_image_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_queue_postponed_update(self, _update);
}

static void _tree_selection_changed(GtkTreeSelection *treeselection, gpointer data)
{
  _update((dt_lib_module_t *)data);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)malloc(sizeof(dt_lib_styles_t));
  self->data = (void *)d;
  self->timeout_handle = 0;
  d->edit_button = NULL;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *w;

  /* tree */
  d->tree = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_tree_view_set_headers_visible(d->tree, FALSE);
  GtkTreeStore *treestore = gtk_tree_store_new(DT_STYLES_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->tree), col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, (gchar *)0);
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", DT_STYLES_COL_NAME);

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree)), GTK_SELECTION_MULTIPLE);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->tree), GTK_TREE_MODEL(treestore));
  g_object_unref(treestore);

  gtk_widget_set_tooltip_text(GTK_WIDGET(d->tree), _("Available styles,\ndouble-click to apply"));
  g_signal_connect(d->tree, "row-activated", G_CALLBACK(_styles_row_activated_callback), d);
  g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree)), "changed", G_CALLBACK(_tree_selection_changed), self);

  /* filter entry */
  w = gtk_entry_new();
  d->entry = GTK_ENTRY(w);
  gtk_entry_set_placeholder_text(GTK_ENTRY(d->entry), _("Filter style names"));
  gtk_widget_set_tooltip_text(w, _("Filter style names"));
  gtk_entry_set_width_chars(GTK_ENTRY(w), 0);
  g_signal_connect(d->entry, "changed", G_CALLBACK(entry_callback), d);
  g_signal_connect(d->entry, "activate", G_CALLBACK(entry_activated), d);


  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->entry), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(GTK_WIDGET(d->tree), 250, "plugins/lighttable/style/windowheight"),
                     FALSE, FALSE, 0);

  d->duplicate = gtk_check_button_new_with_label(_("Create duplicate"));
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->duplicate))), PANGO_ELLIPSIZE_START);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->duplicate), TRUE, FALSE, 0);
  g_signal_connect(d->duplicate, "toggled", G_CALLBACK(duplicate_callback), d);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->duplicate),
                               dt_conf_get_bool("ui_last/styles_create_duplicate"));
  gtk_widget_set_tooltip_text(d->duplicate, _("Creates a duplicate of the image before applying style"));

  d->applymode = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->applymode), TRUE, FALSE, 0);
  dt_bauhaus_widget_set_label(d->applymode, NULL, N_("Mode"));
  dt_bauhaus_combobox_add(d->applymode, _("Append"));
  dt_bauhaus_combobox_add(d->applymode, _("Overwrite"));
  gtk_widget_set_tooltip_text(d->applymode, _("How to handle existing history"));
  dt_bauhaus_combobox_set(d->applymode, dt_conf_get_int("plugins/lighttable/style/applymode"));

  GtkWidget *hbox1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *hbox2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *hbox3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox1, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox2, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox3, TRUE, FALSE, 0);

  // create
  d->create_button = dt_action_button_new(self, N_("Create..."), create_clicked, d, _("Create styles from history stack of selected images"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hbox1), d->create_button, TRUE, TRUE, 0);

  // edit
  d->edit_button = dt_action_button_new(self, N_("Edit..."), edit_clicked, d, _("Edit the selected styles in list above"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hbox1), d->edit_button, TRUE, TRUE, 0);

  // delete
  d->delete_button = dt_action_button_new(self, N_("Remove"), delete_clicked, d, _("Removes the selected styles in list above"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hbox1), d->delete_button, TRUE, TRUE, 0);

  // import button
  d->import_button = dt_action_button_new(self, N_("Import..."), import_clicked, d, _("Import styles from a style files"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hbox2), d->import_button, TRUE, TRUE, 0);

  // export button
  d->export_button = dt_action_button_new(self, N_("Export..."), export_clicked, d, _("Export the selected styles into a style files"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hbox2), d->export_button, TRUE, TRUE, 0);

  // apply button
  d->apply_button = dt_action_button_new(self, N_("Apply"), apply_clicked, d, _("Apply the selected styles in list above to selected images"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hbox3), d->apply_button, TRUE, TRUE, 0);

  // add entry completion
  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, gtk_tree_view_get_model(GTK_TREE_VIEW(d->tree)));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_set_completion(d->entry, completion);

  /* update filtered list */
  _gui_styles_update_view(d);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_STYLE_CHANGED, G_CALLBACK(_styles_changed_callback), self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_collection_updated_callback), self);

  g_signal_connect(G_OBJECT(d->applymode), "value-changed", G_CALLBACK(applymode_combobox_changed), (gpointer)self);

  _update(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_styles_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_collection_updated_callback), self);

  free(self->data);
  self->data = NULL;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_database_start_transaction(darktable.db);

  GList *all_styles = dt_styles_get_list("");

  if(all_styles == NULL)
  {
    dt_database_release_transaction(darktable.db);
    return;
  }

  const gint styles_cnt = g_list_length(all_styles);
  const gboolean can_delete = _ask_before_delete_style(styles_cnt);

  if(can_delete)
  {
    for (const GList *result = all_styles; result; result = g_list_next(result))
    {
      dt_style_t *style = (dt_style_t *)result->data;
      dt_styles_delete_by_name_adv((char*)style->name, FALSE);
    }
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_STYLE_CHANGED);
  }
  g_list_free_full(all_styles, dt_style_free);
  dt_database_release_transaction(darktable.db);
  _update(self);
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

