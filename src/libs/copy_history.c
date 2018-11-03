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
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/history.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/hist_dialog.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE(1)

typedef struct dt_lib_copy_history_t
{
  int32_t imageid;
  GtkWidget *pastemode;
  GtkButton *paste, *paste_parts;
  GtkWidget *copy_button, *delete_button, *load_button, *write_button;
  GtkWidget *copy_parts_button;

  dt_gui_hist_dialog_t dg;
} dt_lib_copy_history_t;

const char *name(dt_lib_module_t *self)
{
  return _("history stack");
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

static void write_button_clicked(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_control_write_sidecar_files();
}

static void load_button_clicked(GtkWidget *widget, dt_lib_module_t *self)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("open sidecar file"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN, _("_cancel"),
      GTK_RESPONSE_CANCEL, _("_open"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif

  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.xmp");
  gtk_file_filter_add_pattern(filter, "*.XMP");
  gtk_file_filter_set_name(filter, _("XMP sidecar files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *dtfilename;
    dtfilename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));

    if(dt_history_load_and_apply_on_selection(dtfilename) != 0)
    {
      GtkWidget *dialog
          = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
                                   GTK_BUTTONS_CLOSE, _("error loading file '%s'"), dtfilename);
      gtk_dialog_run(GTK_DIALOG(dialog));
      gtk_widget_destroy(dialog);
    }

    g_free(dtfilename);
  }
  gtk_widget_destroy(filechooser);
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
}

static int get_selected_image(void)
{
  int imgid;

  /* get imageid for source if history past */
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images",
                              -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    /* copy history of first image in selection */
    imgid = sqlite3_column_int(stmt, 0);
    // dt_control_log(_("history of first image in selection copied"));
  }
  else
  {
    /* no selection is used, use mouse over id */
    imgid = dt_control_get_mouse_over_id();
  }
  sqlite3_finalize(stmt);

  return imgid;
}

static void copy_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  d->imageid = get_selected_image();

  if(d->imageid > 0)
  {
    d->dg.selops = NULL;
    d->dg.copied_imageid = d->imageid;

    gtk_widget_set_sensitive(GTK_WIDGET(d->paste), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(d->paste_parts), TRUE);
  }
}

static void copy_parts_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  d->imageid = get_selected_image();

  if(d->imageid > 0)
  {
    d->dg.copied_imageid = d->imageid;

    // launch dialog to select the ops to copy
    int res = dt_gui_hist_dialog_new(&(d->dg), d->imageid, TRUE);

    if(res != GTK_RESPONSE_CANCEL && d->dg.selops)
    {
      gtk_widget_set_sensitive(GTK_WIDGET(d->paste), TRUE);
      gtk_widget_set_sensitive(GTK_WIDGET(d->paste_parts), TRUE);
    }
  }
}

static void delete_button_clicked(GtkWidget *widget, gpointer user_data)
{
  gint res = GTK_RESPONSE_YES;

  if(dt_conf_get_bool("ask_before_delete"))
  {
    const GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

    int number;
    if (dt_view_get_image_to_act_on() != -1)
      number = 1;
    else
      number = dt_collection_get_selected_count(darktable.collection);

    if (number == 0) return;

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        ngettext("do you really want to clear history of %d selected image?",
                 "do you really want to clear history of %d selected images?", number), number);

    gtk_window_set_title(GTK_WINDOW(dialog), _("delete images' history?"));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }

  if(res == GTK_RESPONSE_YES)
  {
    dt_history_delete_on_selection();
    dt_control_queue_redraw_center();
  }
}

static void paste_button_clicked(GtkWidget *widget, gpointer user_data)
{

  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  /* get past mode and store, overwrite / merge */
  int mode = dt_bauhaus_combobox_get(d->pastemode);
  dt_conf_set_int("plugins/lighttable/copy_history/pastemode", mode);

  /* copy history from d->imageid and past onto selection */
  if(dt_history_copy_and_paste_on_selection(d->imageid, (mode == 0) ? TRUE : FALSE, d->dg.selops) != 0)
  {
    /* no selection is used, use mouse over id */
    int32_t mouse_over_id = dt_control_get_mouse_over_id();
    if(mouse_over_id <= 0) return;

    dt_history_copy_and_paste_on_image(d->imageid, mouse_over_id, (mode == 0) ? TRUE : FALSE, d->dg.selops);
  }

  /* redraw */
  dt_control_queue_redraw_center();
}

static void paste_parts_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  // launch dialog to select the ops to paste
  if(dt_gui_hist_dialog_new(&(d->dg), d->dg.copied_imageid, FALSE) == GTK_RESPONSE_OK)
    paste_button_clicked(widget, user_data);
}

static void pastemode_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  int mode = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/lighttable/copy_history/pastemode", mode);
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;
  d->imageid = -1;
  gtk_widget_set_sensitive(GTK_WIDGET(d->paste), FALSE);
}

int position()
{
  return 600;
}

#define ellipsize_button(button) gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), PANGO_ELLIPSIZE_END);
void gui_init(dt_lib_module_t *self)
{
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)malloc(sizeof(dt_lib_copy_history_t));
  self->data = (void *)d;
  self->widget = gtk_grid_new();
  GtkGrid *grid = GTK_GRID(self->widget);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));
  gtk_grid_set_row_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_homogeneous(grid, TRUE);
  int line = 0;
  dt_gui_hist_dialog_init(&d->dg);


  GtkWidget *copy_parts = gtk_button_new_with_label(_("copy"));
  ellipsize_button(copy_parts);
  d->copy_parts_button = copy_parts;
  gtk_widget_set_tooltip_text(copy_parts, _("copy part history stack of\nfirst selected image"));
  dt_gui_add_help_link(copy_parts, "history_stack.html#history_stack_usage");
  gtk_grid_attach(grid, copy_parts, 0, line, 2, 1);

  GtkWidget *copy = gtk_button_new_with_label(_("copy all"));
  ellipsize_button(copy);
  d->copy_button = copy;
  gtk_widget_set_tooltip_text(copy, _("copy history stack of\nfirst selected image"));
  dt_gui_add_help_link(copy, "history_stack.html#history_stack_usage");
  gtk_grid_attach(grid, copy, 2, line, 2, 1);

  GtkWidget *delete = gtk_button_new_with_label(_("discard"));
  ellipsize_button(delete);
  d->delete_button = delete;
  gtk_widget_set_tooltip_text(delete, _("discard history stack of\nall selected images"));
  dt_gui_add_help_link(delete, "history_stack.html#history_stack_usage");
  gtk_grid_attach(grid, delete, 4, line++, 2, 1);


  d->paste_parts = GTK_BUTTON(gtk_button_new_with_label(_("paste")));
  ellipsize_button(d->paste_parts);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->paste_parts), _("paste part history stack to\nall selected images"));
  dt_gui_add_help_link(GTK_WIDGET(d->paste_parts), "history_stack.html#history_stack_usage");
  d->imageid = -1;
  gtk_widget_set_sensitive(GTK_WIDGET(d->paste_parts), FALSE);
  gtk_grid_attach(grid, GTK_WIDGET(d->paste_parts), 0, line, 3, 1);

  d->paste = GTK_BUTTON(gtk_button_new_with_label(_("paste all")));
  ellipsize_button(d->paste);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->paste), _("paste history stack to\nall selected images"));
  dt_gui_add_help_link(GTK_WIDGET(d->paste), "history_stack.html#history_stack_usage");
  gtk_widget_set_sensitive(GTK_WIDGET(d->paste), FALSE);
  gtk_grid_attach(grid, GTK_WIDGET(d->paste), 3, line++, 3, 1);

  d->pastemode = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->pastemode, NULL, _("mode"));
  dt_bauhaus_combobox_add(d->pastemode, _("append"));
  dt_bauhaus_combobox_add(d->pastemode, _("overwrite"));
  gtk_widget_set_tooltip_text(d->pastemode, _("how to handle existing history"));
  dt_gui_add_help_link(d->pastemode, "history_stack.html#history_stack_usage");
  gtk_grid_attach(grid, d->pastemode, 0, line++, 6, 1);
  dt_bauhaus_combobox_set(d->pastemode, dt_conf_get_int("plugins/lighttable/copy_history/pastemode"));


  GtkWidget *loadbutton = gtk_button_new_with_label(_("load sidecar file"));
  ellipsize_button(loadbutton);
  d->load_button = loadbutton;
  gtk_widget_set_tooltip_text(loadbutton, _("open an XMP sidecar file\nand apply it to selected images"));
  dt_gui_add_help_link(loadbutton, "history_stack.html#history_stack_usage");
  gtk_grid_attach(grid, loadbutton, 0, line, 3, 1);

  GtkWidget *button = gtk_button_new_with_label(_("write sidecar files"));
  ellipsize_button(button);
  d->write_button = button;
  gtk_widget_set_tooltip_text(button, _("write history stack and tags to XMP sidecar files"));
  dt_gui_add_help_link(button, "history_stack.html#history_stack_usage");
  gtk_grid_attach(grid, button, 3, line, 3, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(write_button_clicked), (gpointer)self);


  g_signal_connect(G_OBJECT(copy), "clicked", G_CALLBACK(copy_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(copy_parts), "clicked", G_CALLBACK(copy_parts_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(delete), "clicked", G_CALLBACK(delete_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(d->paste_parts), "clicked", G_CALLBACK(paste_parts_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(d->paste), "clicked", G_CALLBACK(paste_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(loadbutton), "clicked", G_CALLBACK(load_button_clicked), (gpointer)self);
  g_signal_connect(G_OBJECT(d->pastemode), "value-changed", G_CALLBACK(pastemode_combobox_changed), (gpointer)self);
}
#undef ellipsize_button

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "copy all"), GDK_KEY_c, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "copy"), GDK_KEY_c, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_lib(self, NC_("accel", "discard"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "paste all"), GDK_KEY_v, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "paste"), GDK_KEY_v, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_lib(self, NC_("accel", "load sidecar files"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "write sidecar files"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  dt_accel_connect_button_lib(self, "copy all", GTK_WIDGET(d->copy_button));
  dt_accel_connect_button_lib(self, "copy", GTK_WIDGET(d->copy_parts_button));
  dt_accel_connect_button_lib(self, "discard", GTK_WIDGET(d->delete_button));
  dt_accel_connect_button_lib(self, "paste all", GTK_WIDGET(d->paste));
  dt_accel_connect_button_lib(self, "paste", GTK_WIDGET(d->paste_parts));
  dt_accel_connect_button_lib(self, "load sidecar files", GTK_WIDGET(d->load_button));
  dt_accel_connect_button_lib(self, "write sidecar files", GTK_WIDGET(d->write_button));
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
