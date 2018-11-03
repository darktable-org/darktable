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

DT_MODULE(1)

typedef struct dt_lib_styles_t
{
  GtkEntry *entry;
  GtkWidget *duplicate;
  GtkTreeView *list;
  GtkWidget *delete_button, *import_button, *export_button, *edit_button;
} dt_lib_styles_t;


const char *name(dt_lib_module_t *self)
{
  return _("styles");
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

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "delete"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "export"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "import"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "edit"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)self->data;

  dt_accel_connect_button_lib(self, "delete", d->delete_button);
  dt_accel_connect_button_lib(self, "export", d->export_button);
  dt_accel_connect_button_lib(self, "import", d->import_button);
  if(d->edit_button) dt_accel_connect_button_lib(self, "edit", d->edit_button);
}

typedef enum _styles_columns_t
{
  DT_STYLES_COL_NAME = 0,
  DT_STYLES_COL_TOOLTIP,
  DT_STYLES_NUM_COLS
} _styles_columns_t;

static void _gui_styles_update_view(dt_lib_styles_t *d)
{
  /* clear current list */
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->list));
  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->list), NULL);
  gtk_list_store_clear(GTK_LIST_STORE(model));

  GList *result = dt_styles_get_list(gtk_entry_get_text(d->entry));
  if(result)
  {
    do
    {
      dt_style_t *style = (dt_style_t *)result->data;

      char *items_string = dt_styles_get_item_list_as_string(style->name);
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

      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_STYLES_COL_NAME, style->name, DT_STYLES_COL_TOOLTIP,
                         tooltip, -1);

      g_free(items_string);
      g_free(tooltip);
    } while((result = g_list_next(result)) != NULL);
    g_list_free_full(result, dt_style_free);
  }

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(d->list), DT_STYLES_COL_TOOLTIP);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->list), model);
  g_object_unref(model);
}

static void _styles_row_activated_callback(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col,
                                           gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;

  GtkTreeModel *model;
  GtkTreeIter iter;
  model = gtk_tree_view_get_model(d->list);

  if(!gtk_tree_model_get_iter(model, &iter, path)) return;

  gchar *name;
  gtk_tree_model_get(model, &iter, DT_STYLES_COL_NAME, &name, -1);

  if(name) dt_styles_apply_to_selection(name, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate)));
}

static void create_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;

  dt_styles_create_from_selection();
  _gui_styles_update_view(d);
}

static void edit_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;

  GtkTreeIter iter;
  GtkTreeModel *model;
  model = gtk_tree_view_get_model(d->list);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->list));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  char *name = NULL;
  gtk_tree_model_get(model, &iter, DT_STYLES_COL_NAME, &name, -1);
  if(name)
  {
    dt_gui_styles_dialog_edit(name);
    _gui_styles_update_view(d);
  }
}

static char *get_style_name(dt_lib_styles_t *list_style)
{
  GtkTreeIter iter;
  GtkTreeModel *model;
  model = gtk_tree_view_get_model(list_style->list);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(list_style->list));

  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return NULL;

  char *name = NULL;
  gtk_tree_model_get(model, &iter, DT_STYLES_COL_NAME, &name, -1);
  return name;
}

static void delete_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  char *name = get_style_name(d);
  if(name)
  {
    dt_styles_delete_by_name(name);
    _gui_styles_update_view(d);
  }
}

static void export_clicked(GtkWidget *w, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  char *name = get_style_name(d);
  if(name)
  {
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
    GtkWidget *filechooser = gtk_file_chooser_dialog_new(
        _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_cancel"),
        GTK_RESPONSE_CANCEL, _("_save"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(filechooser);
#endif
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), g_get_home_dir());
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);
    if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
    {
      char *filedir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
      dt_styles_save_to_file(name, filedir, FALSE);
      dt_control_log(_("style %s was successfully saved"), name);
      g_free(filedir);
    }
    g_free(name);
    gtk_widget_destroy(filechooser);
  }
}

static void import_clicked(GtkWidget *w, gpointer user_data)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("select style"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN, _("_cancel"), GTK_RESPONSE_CANCEL,
      _("_open"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), TRUE);
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), g_get_home_dir());

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

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    GSList *filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(filechooser));
    g_slist_foreach(filenames, (GFunc)dt_styles_import_from_file, NULL);
    g_slist_free_full(filenames, g_free);

    dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
    _gui_styles_update_view(d);
  }
  gtk_widget_destroy(filechooser);
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
  if(name) dt_styles_apply_to_selection(name, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate)));

  return FALSE;
}

static gboolean duplicate_callback(GtkEntry *entry, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  dt_conf_set_bool("ui_last/styles_create_duplicate",
                   gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->duplicate)));
  return FALSE;
}


static void _styles_changed_callback(gpointer instance, gpointer user_data)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)user_data;
  _gui_styles_update_view(d);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)malloc(sizeof(dt_lib_styles_t));
  self->data = (void *)d;
  d->edit_button = NULL;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  dt_gui_add_help_link(self->widget, "styles.html#styles_usage");
  GtkWidget *w;
  GtkWidget *scrolled;

  /* list */
  d->list = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_tree_view_set_headers_visible(d->list, FALSE);
  GtkListStore *liststore = gtk_list_store_new(DT_STYLES_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING);
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->list), col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", DT_STYLES_COL_NAME);

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->list)), GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->list), GTK_TREE_MODEL(liststore));
  g_object_unref(liststore);

  gtk_widget_set_tooltip_text(GTK_WIDGET(d->list), _("available styles,\ndoubleclick to apply"));
  g_signal_connect(d->list, "row-activated", G_CALLBACK(_styles_row_activated_callback), d);

  /* filter entry */
  w = gtk_entry_new();
  d->entry = GTK_ENTRY(w);
  gtk_widget_set_tooltip_text(w, _("enter style name"));
  g_signal_connect(d->entry, "changed", G_CALLBACK(entry_callback), d);
  g_signal_connect(d->entry, "activate", G_CALLBACK(entry_activated), d);

  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->entry));

  scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), DT_PIXEL_APPLY_DPI(150));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->entry), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(scrolled), TRUE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(scrolled), GTK_WIDGET(d->list));

  d->duplicate = gtk_check_button_new_with_label(_("create duplicate"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->duplicate), TRUE, FALSE, 0);
  g_signal_connect(d->duplicate, "toggled", G_CALLBACK(duplicate_callback), d);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->duplicate),
                               dt_conf_get_bool("ui_last/styles_create_duplicate"));
  gtk_widget_set_tooltip_text(d->duplicate, _("creates a duplicate of the image before applying style"));

  GtkWidget *hbox1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  GtkWidget *hbox2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox1, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox2, TRUE, FALSE, 0);

  // create
  GtkWidget *cbutton = gtk_button_new_with_label(_("create"));
  g_signal_connect(G_OBJECT(cbutton), "clicked", G_CALLBACK(create_clicked), d);
  gtk_widget_set_tooltip_text(cbutton, _("create styles from history stack of selected images"));
  gtk_box_pack_start(GTK_BOX(hbox1), cbutton, TRUE, TRUE, 0);

  // edit
  GtkWidget *widget = gtk_button_new_with_label(_("edit"));
  d->edit_button = widget;
  g_signal_connect(widget, "clicked", G_CALLBACK(edit_clicked), d);
  gtk_widget_set_tooltip_text(widget, _("edit the selected style in list above"));
  gtk_box_pack_start(GTK_BOX(hbox1), widget, TRUE, TRUE, 0);

  // delete
  widget = gtk_button_new_with_label(_("delete"));
  d->delete_button = widget;
  g_signal_connect(widget, "clicked", G_CALLBACK(delete_clicked), d);
  gtk_widget_set_tooltip_text(widget, _("deletes the selected style in list above"));
  gtk_box_pack_start(GTK_BOX(hbox1), widget, TRUE, TRUE, 0);

  // import button
  GtkWidget *importButton = gtk_button_new_with_label(C_("styles", "import"));
  d->import_button = importButton;
  gtk_widget_set_tooltip_text(importButton, _("import style from a style file"));
  g_signal_connect(importButton, "clicked", G_CALLBACK(import_clicked), d);
  gtk_box_pack_start(GTK_BOX(hbox2), importButton, TRUE, TRUE, 0);

  // export button
  GtkWidget *exportButton = gtk_button_new_with_label(_("export"));
  d->export_button = exportButton;
  gtk_widget_set_tooltip_text(exportButton, _("export the selected style into a style file"));
  g_signal_connect(exportButton, "clicked", G_CALLBACK(export_clicked), d);
  gtk_box_pack_start(GTK_BOX(hbox2), exportButton, TRUE, TRUE, 0);

  // add entry completion
  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, gtk_tree_view_get_model(GTK_TREE_VIEW(d->list)));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_set_completion(d->entry, completion);

  /* update filtered list */
  _gui_styles_update_view(d);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_STYLE_CHANGED, G_CALLBACK(_styles_changed_callback), d);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_styles_t *d = (dt_lib_styles_t *)self->data;
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_styles_changed_callback), self);
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(d->entry));
  free(self->data);
  self->data = NULL;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
