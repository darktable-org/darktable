/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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
#include "common/utility.h"
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
  GtkWidget *create_button, *edit_button, *delete_button;
  GtkWidget *import_button, *export_button, *applymode, *apply_button;
} dt_lib_styles_t;


const char *name(dt_lib_module_t *self)
{
  return _("styles");
}

const char *description(dt_lib_module_t *self)
{
  return _("apply styles to the currently selected\n"
           "images or manage your styles");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_MULTI;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 599;
}

typedef enum _styles_columns_t
{
  DT_STYLES_COL_NAME = 0,
  DT_STYLES_COL_FULLNAME,
  DT_STYLES_NUM_COLS
} _styles_columns_t;

static gboolean _get_node_for_name(GtkTreeModel *model,
                                   const gboolean root,
                                   GtkTreeIter *iter,
                                   const gchar *parent_name)
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

gboolean _styles_tooltip_callback(GtkWidget* self,
                                  gint x,
                                  gint y,
                                  gboolean keyboard_mode,
                                  GtkTooltip* tooltip,
                                  dt_lib_styles_t *d)
{
  GtkTreeModel* model;
  GtkTreePath* path;
  GtkTreeIter iter;
  dt_imgid_t imgid = NO_IMGID;

  if(gtk_tree_view_get_tooltip_context(GTK_TREE_VIEW(self), &x, &y, FALSE, &model, &path, &iter))
  {
    gchar *name = NULL;
    gtk_tree_model_get(model, &iter, DT_STYLES_COL_FULLNAME, &name, -1);

    // only on leaf node
    if(!name) return FALSE;

    GList *selected_image = dt_collection_get_selected(darktable.collection, 1);

    if(selected_image)
    {
      imgid = GPOINTER_TO_INT(selected_image->data);
      g_list_free(selected_image);
    }

    GtkWidget *ht = dt_gui_style_content_dialog(name, imgid);
    gtk_widget_show_all(ht);

    gtk_tooltip_set_custom(tooltip, ht);
    return TRUE;
  }

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
      dt_style_t *style = res_iter->data;

      gchar **split = g_strsplit(style->name, "|", 0);
      int k = 0;

      while(split[k])
      {
        const gchar *s = dt_util_localize_string(split[k]);
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
                               DT_STYLES_COL_NAME, s,
                               DT_STYLES_COL_FULLNAME, style->name, -1);
          }
        }
        k++;
      }
      g_strfreev(split);
    }
    g_list_free_full(result, dt_style_free);
  }

  g_signal_connect(GTK_TREE_VIEW(d->tree), "query-tooltip",
                   G_CALLBACK(_styles_tooltip_callback), d);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->tree), model);
  g_object_unref(model);
}

static void _styles_row_activated_callback(GtkTreeView *view,
                                           GtkTreePath *path,
                                           GtkTreeViewColumn *col,
                                           dt_lib_styles_t *d)
{
  // This works on double click, so it's for single style
  GtkTreeModel *model;
  GtkTreeIter iter;
  model = gtk_tree_view_get_model(d->tree);

  if(!gtk_tree_model_get_iter(model, &iter, path)) return;

  gchar *name;
  gtk_tree_model_get(model, &iter, DT_STYLES_COL_FULLNAME, &name, -1);

  if(name)
  {
    GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
    if(imgs)
    {
      GList *styles = g_list_prepend(NULL, g_strdup(name));
      gboolean duplicate = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate));
      dt_control_apply_styles(imgs, styles, duplicate);
    }
    else
      dt_control_log(_("no images selected"));
  }
}

// get list of style names from selection
// free returned list with g_list_free_full(list, g_free)
GList* _get_selected_style_names(GList* selected_styles, GtkTreeModel *model)
{
  GtkTreeIter iter;
  GList *style_names = NULL;
  for(const GList *style = selected_styles; style; style = g_list_next(style))
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

static void _apply_clicked(GtkWidget *w, dt_lib_styles_t *d)
{
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree));

  if(gtk_tree_selection_count_selected_rows(selection) == 0) return;

  GtkTreeModel *model= gtk_tree_view_get_model(d->tree);
  GList *selected_styles = gtk_tree_selection_get_selected_rows(selection, &model);
  GList *style_names = _get_selected_style_names(selected_styles, model);
  g_list_free_full(selected_styles, (GDestroyNotify) gtk_tree_path_free);

  if(style_names == NULL) return;

  GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
  if(!g_list_is_empty(imgs))
  {
    gboolean duplicate = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate));
    dt_control_apply_styles(imgs, style_names, duplicate);
  }
  else
    g_list_free_full(style_names, g_free);
}

static void _create_clicked(GtkWidget *w, dt_lib_styles_t *d)
{
  GList *list = dt_act_on_get_images(TRUE, TRUE, FALSE);
  dt_styles_create_from_list(list);
  g_list_free(list);
  _gui_styles_update_view(d);
}

static void _edit_clicked(GtkWidget *w, dt_lib_styles_t *d)
{
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree));

  if(gtk_tree_selection_count_selected_rows(selection) == 0) return;

  GtkTreeIter iter;
  GtkTreeModel *model= gtk_tree_view_get_model(d->tree);

  GList *styles = gtk_tree_selection_get_selected_rows(selection, &model);
  GList *new_name_list = NULL;

  for(const GList *style = styles; style; style = g_list_next(style))
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
      char *new_name = NULL;
      // update view is necessary as we may have changed the style name
      dt_gui_styles_dialog_edit(name, &new_name);
      new_name_list = g_list_prepend(new_name_list, new_name);
      _gui_styles_update_view(d);
      g_free(name);
    }
  }

  // we need to iterate over all styles
  if(new_name_list)
  {
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    for(; valid; valid = gtk_tree_model_iter_next(model, &iter))
    {
      char *name = NULL;
      GValue value = {0,};
      gtk_tree_model_get_value(model, &iter, DT_STYLES_COL_FULLNAME, &value);
      if(G_VALUE_HOLDS_STRING(&value))
        name = g_strdup(g_value_get_string(&value));
      g_value_unset(&value);

      if(name)
      {
        // and select back all the previously selected paths
        for(const GList *sname = new_name_list; sname; sname = g_list_next(sname))
        {
          const char *newname = (char *)sname->data;
          if(newname && !strcmp(name, newname))
          {
            gtk_tree_selection_select_iter(selection, &iter);
            break;
          }
        }
        g_free(name);
      }
    }
  }
  g_list_free_full(new_name_list, g_free);
  g_list_free_full(styles, (GDestroyNotify) gtk_tree_path_free);
}

gboolean _ask_before_delete_style(const gint style_cnt)
{
  return !dt_conf_get_bool("plugins/lighttable/style/ask_before_delete_style")
    || dt_gui_show_yes_no_dialog(
      ngettext("remove style?", "remove styles?", style_cnt),
      ngettext("do you really want to remove %d style?",
               "do you really want to remove %d styles?", style_cnt),
      style_cnt);
}

static void _delete_clicked(GtkWidget *w, dt_lib_styles_t *d)
{
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

    for(const GList *style = style_names; style; style = g_list_next(style))
    {
      dt_styles_delete_by_name_adv((char*)style->data, single_raise);
    }

    if(!single_raise) {
      // raise signal at the end of processing all styles if we have more than 1 to delete
      // this also calls _gui_styles_update_view
      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_STYLE_CHANGED);
    }
    dt_database_release_transaction(darktable.db);
  }
  g_list_free_full(style_names, g_free);
}

static void _export_clicked(GtkWidget *w, dt_lib_styles_t *d)
{
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
        _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        _("_save"), _("_cancel"));

  dt_conf_get_folder_to_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(filechooser));
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *filedir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));

    for(const GList *style = style_names; style; style = g_list_next(style))
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
            GtkWidget *dialog_overwrite_export =
              gtk_dialog_new_with_buttons(_("overwrite style?"), GTK_WINDOW(win),
                                          GTK_DIALOG_DESTROY_WITH_PARENT,
                                          _("_cancel"), GTK_RESPONSE_CANCEL,
                                          _("_skip"), GTK_RESPONSE_NONE,
                                          _("_overwrite"), GTK_RESPONSE_ACCEPT, NULL);
            gtk_dialog_set_default_response(GTK_DIALOG(dialog_overwrite_export),
                                            GTK_RESPONSE_CANCEL);

            // contents for dialog
            GtkWidget *content_area =
              gtk_dialog_get_content_area(GTK_DIALOG(dialog_overwrite_export));
            sprintf(overwrite_str, _("style `%s' already exists.\ndo you want to overwrite existing style?\n"), stylename);
            GtkWidget *label = gtk_label_new(overwrite_str);
            GtkWidget *overwrite_dialog_check_button =
              gtk_check_button_new_with_label(_("apply this option to all existing styles"));

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
      dt_control_log(_("style %s was successfully exported"), (char*)style->data);
    }
    dt_conf_set_folder_from_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(filechooser));
    g_free(filedir);
  }
  g_object_unref(filechooser);
  g_list_free_full(style_names, g_free);
}

static void _import_clicked(GtkWidget *w, dt_lib_styles_t *d)
{
  /* variables for overwrite dialog */
  gint overwrite_check_button = 0;
  gint overwrite = 0;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
        _("select style"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
        _("_open"), _("_cancel"));

  dt_conf_get_folder_to_file_chooser("ui_last/import_path", GTK_FILE_CHOOSER(filechooser));
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), TRUE);

  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.dtstyle");
  gtk_file_filter_add_pattern(filter, "*.DTSTYLE");
  gtk_file_filter_set_name(filter, _("darktable style files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));

  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    GSList *filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(filechooser));

    for(const GSList *filename = filenames; filename; filename = g_slist_next(filename))
    {
      /* extract name from xml file */
      gchar *bname = dt_get_style_name(filename->data);
      if (!bname)
        continue;

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
            GtkWidget *dialog_overwrite_import = gtk_dialog_new_with_buttons(_("overwrite style?"), GTK_WINDOW(win),
                                                                             GTK_DIALOG_DESTROY_WITH_PARENT,
                                                                             _("_cancel"), GTK_RESPONSE_CANCEL,
                                                                             _("_skip"), GTK_RESPONSE_NONE,
                                                                             _("_overwrite"), GTK_RESPONSE_ACCEPT, NULL);
            gtk_dialog_set_default_response(GTK_DIALOG(dialog_overwrite_import), GTK_RESPONSE_CANCEL);

            // contents for dialog
            GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog_overwrite_import));
            sprintf(overwrite_str, _("style `%s' already exists.\ndo you want to overwrite existing style?\n"), bname);
            GtkWidget *label = gtk_label_new(overwrite_str);
            GtkWidget *overwrite_dialog_check_button = gtk_check_button_new_with_label(_("apply this option to all existing styles"));

            gtk_container_add(GTK_CONTAINER(content_area), label);
            gtk_container_add(GTK_CONTAINER(content_area), overwrite_dialog_check_button);
            gtk_widget_show_all(dialog_overwrite_import);

            // disable check button and skip button when dealing with one style
            if(g_list_is_singleton(filenames))
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

    _gui_styles_update_view(d);
    dt_conf_set_folder_from_file_chooser("ui_last/import_path", GTK_FILE_CHOOSER(filechooser));
  }
  g_object_unref(filechooser);
}

static gboolean _entry_callback(GtkEntry *entry, dt_lib_styles_t *d)
{
  _gui_styles_update_view(d);
  return FALSE;
}

static gboolean _entry_activated(GtkEntry *entry, dt_lib_styles_t *d)
{
  const gchar *name = gtk_entry_get_text(d->entry);
  if(name)
  {
    GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
    if(imgs)
    {
      GList *styles = g_list_prepend(NULL, g_strdup(name));
      gboolean duplicate = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate));
      dt_control_apply_styles(imgs, styles, duplicate);
    }
  }

  return FALSE;
}

static gboolean _duplicate_callback(GtkEntry *entry, dt_lib_styles_t *d)
{
  dt_conf_set_bool("ui_last/styles_create_duplicate",
                   gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate)));
  return FALSE;
}

static void _applymode_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int mode = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/lighttable/style/applymode", mode);
}

void gui_update(dt_lib_module_t *self)
{
  dt_lib_styles_t *d = self->data;

  const gboolean has_act_on = dt_act_on_get_images_nb(TRUE, FALSE) > 0;

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree));
  const gboolean any_style = gtk_tree_selection_count_selected_rows(selection) > 0;

  gtk_widget_set_sensitive(GTK_WIDGET(d->create_button), has_act_on);
  gtk_widget_set_sensitive(GTK_WIDGET(d->edit_button), any_style);
  gtk_widget_set_sensitive(GTK_WIDGET(d->delete_button), any_style);

  //import is ALWAYS enabled.
  gtk_widget_set_sensitive(GTK_WIDGET(d->export_button), any_style);

  gtk_widget_set_sensitive(GTK_WIDGET(d->apply_button), has_act_on && any_style);
}

static void _styles_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_styles_t *d = self->data;
  _gui_styles_update_view(d);
  dt_lib_gui_queue_update(self);
}

static void _image_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_gui_queue_update(self);
}

static void _collection_updated_callback(gpointer instance,
                                         const dt_collection_change_t query_change,
                                         const dt_collection_properties_t changed_property,
                                         gpointer imgs,
                                         const int next,
                                         dt_lib_module_t *self)
{
  dt_lib_gui_queue_update(self);
}

static void _mouse_over_image_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_gui_queue_update(self);
}

static void _tree_selection_changed(GtkTreeSelection *treeselection, gpointer data)
{
  dt_lib_gui_queue_update((dt_lib_module_t *)data);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_styles_t *d = malloc(sizeof(dt_lib_styles_t));
  self->data = (void *)d;
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

  gtk_widget_set_tooltip_text(GTK_WIDGET(d->tree),
                              _("available styles,\ndouble-click to apply"));
  g_signal_connect(d->tree, "row-activated",
                   G_CALLBACK(_styles_row_activated_callback), d);
  g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->tree)), "changed",
                   G_CALLBACK(_tree_selection_changed), self);

  /* filter entry */
  w = dt_ui_entry_new(0);
  d->entry = GTK_ENTRY(w);
  gtk_entry_set_placeholder_text(GTK_ENTRY(d->entry), _("filter style names"));
  gtk_widget_set_tooltip_text(w, _("filter style names"));
  g_signal_connect(d->entry, "changed", G_CALLBACK(_entry_callback), d);
  g_signal_connect(d->entry, "activate", G_CALLBACK(_entry_activated), d);


  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->entry), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_resize_wrap(GTK_WIDGET(d->tree), 250,
                                       "plugins/lighttable/style/windowheight"),
                     FALSE, FALSE, 0);

  d->duplicate = gtk_check_button_new_with_label(_("create duplicate"));
  dt_action_define(DT_ACTION(self), NULL, N_("create duplicate"),
                   d->duplicate, &dt_action_def_toggle);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->duplicate))), PANGO_ELLIPSIZE_START);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->duplicate), TRUE, FALSE, 0);
  g_signal_connect(d->duplicate, "toggled", G_CALLBACK(_duplicate_callback), d);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->duplicate),
                               dt_conf_get_bool("ui_last/styles_create_duplicate"));
  gtk_widget_set_tooltip_text(d->duplicate,
                              _("creates a duplicate of the image before applying style"));

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->applymode, self, NULL, N_("mode"),
                               _("how to handle existing history"),
                               dt_conf_get_int("plugins/lighttable/style/applymode"),
                               _applymode_combobox_changed, self,
                               N_("append"), N_("overwrite"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->applymode), TRUE, FALSE, 0);

  GtkWidget *hbox1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *hbox2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *hbox3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox1, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox2, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox3, TRUE, FALSE, 0);

  // create
  d->create_button = dt_action_button_new
    (self, N_("create..."),
     _create_clicked, d,
     _("create styles from history stack of selected images"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hbox1), d->create_button, TRUE, TRUE, 0);

  // edit
  d->edit_button = dt_action_button_new
    (self, N_("edit..."),
     _edit_clicked, d,
     _("edit the selected styles in list above"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hbox1), d->edit_button, TRUE, TRUE, 0);

  // delete
  d->delete_button = dt_action_button_new
    (self, N_("remove"),
     _delete_clicked, d,
     _("removes the selected styles in list above"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hbox1), d->delete_button, TRUE, TRUE, 0);

  // import button
  d->import_button = dt_action_button_new
    (self, N_("import..."),
     _import_clicked, d,
     _("import styles from a style files"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hbox2), d->import_button, TRUE, TRUE, 0);

  // export button
  d->export_button = dt_action_button_new
    (self, N_("export..."),
     _export_clicked, d,
     _("export the selected styles into a style files"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hbox2), d->export_button, TRUE, TRUE, 0);

  // apply button
  d->apply_button = dt_action_button_new
    (self, N_("apply"),
     _apply_clicked, d,
     _("apply the selected styles in list above to selected images"), 0, 0);
  gtk_box_pack_start(GTK_BOX(hbox3), d->apply_button, TRUE, TRUE, 0);

  // add entry completion
  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion,
                                 gtk_tree_view_get_model(GTK_TREE_VIEW(d->tree)));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_set_completion(d->entry, completion);

  /* update filtered list */
  _gui_styles_update_view(d);

  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_STYLE_CHANGED, _styles_changed_callback, self);

  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_SELECTION_CHANGED, _image_selection_changed_callback, self);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE, _mouse_over_image_callback, self);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_COLLECTION_CHANGED, _collection_updated_callback, self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_CONTROL_SIGNAL_DISCONNECT(_styles_changed_callback, self);
  DT_CONTROL_SIGNAL_DISCONNECT(_image_selection_changed_callback, self);
  DT_CONTROL_SIGNAL_DISCONNECT(_mouse_over_image_callback, self);
  DT_CONTROL_SIGNAL_DISCONNECT(_collection_updated_callback, self);

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
    for(const GList *result = all_styles; result; result = g_list_next(result))
    {
      dt_style_t *style = result->data;
      dt_styles_delete_by_name_adv((char*)style->name, FALSE);
    }
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_STYLE_CHANGED);
  }
  g_list_free_full(all_styles, dt_style_free);
  dt_database_release_transaction(darktable.db);
  dt_lib_gui_queue_update(self);
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
