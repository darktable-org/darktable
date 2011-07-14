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
#include "common/history.h"
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "control/jobs.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include <stdlib.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef struct dt_lib_copy_history_t
{
  int32_t imageid;
  GtkComboBox *pastemode;
  GtkButton *paste;
}
dt_lib_copy_history_t;


const char*
name ()
{
  return _("history stack");
}

uint32_t views()
{
  return DT_LIGHTTABLE_VIEW;
}

static void
write_button_clicked (GtkWidget *widget, dt_lib_module_t *self)
{
  dt_control_write_sidecar_files();
}

static void
load_button_clicked (GtkWidget *widget, dt_lib_module_t *self)
{
  GtkWidget *win = darktable.gui->widgets.main_window;
  GtkWidget *filechooser = gtk_file_chooser_dialog_new (_("open sidecar file"),
                           GTK_WINDOW (win),
                           GTK_FILE_CHOOSER_ACTION_OPEN,
                           GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                           GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                           (char *)NULL);

  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.xmp");
  gtk_file_filter_set_name(filter, _("xmp sidecar files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *dtfilename;
    dtfilename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (filechooser));

    if (dt_history_load_and_apply_on_selection (dtfilename)!=0)
    {
      GtkWidget *dialog = gtk_message_dialog_new (GTK_WINDOW(win),
                          GTK_DIALOG_DESTROY_WITH_PARENT,
                          GTK_MESSAGE_ERROR,
                          GTK_BUTTONS_CLOSE,
                          _("error loading file '%s'"),
                          dtfilename);
      gtk_dialog_run (GTK_DIALOG (dialog));
      gtk_widget_destroy (dialog);
    }

    g_free (dtfilename);
  }
  gtk_widget_destroy (filechooser);
  win = darktable.gui->widgets.center;
  gtk_widget_queue_draw(win);
}

static void
copy_button_clicked (GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  /* get imageid for source if history past */
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    /* copy history of first image in selection */
    d->imageid = sqlite3_column_int(stmt, 0);
    gtk_widget_set_sensitive(GTK_WIDGET(d->paste), TRUE);
    //dt_control_log(_("history of first image in selection copied"));
  }
  else
  {
    /* no selection is used, use mouse over id */
    int32_t mouse_over_id=0;
    DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
    if(mouse_over_id <= 0) return;
    d->imageid = mouse_over_id;
  }
  sqlite3_finalize(stmt);
}

static void
delete_button_clicked (GtkWidget *widget, gpointer user_data)
{
  dt_history_delete_on_selection ();
  dt_control_gui_queue_draw ();
}

static void
paste_button_clicked (GtkWidget *widget, gpointer user_data)
{

  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  /* get past mode and store, overwrite / merge */
  int mode = gtk_combo_box_get_active(d->pastemode);
  dt_conf_set_int("plugins/lighttable/copy_history/pastemode", mode);

  /* copy history from d->imageid and past onto selection */
  if (dt_history_copy_and_paste_on_selection (d->imageid, (mode==0)?TRUE:FALSE )!=0)
  {
    /* no selection is used, use mouse over id */
    int32_t mouse_over_id=0;
    DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
    if(mouse_over_id <= 0) return;

    dt_history_copy_and_paste_on_image(d->imageid,mouse_over_id,(mode==0)?TRUE:FALSE);

  }

  /* redraw */
  dt_control_gui_queue_draw();
}

void
gui_reset (dt_lib_module_t *self)
{
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;
  d->imageid = -1;
  gtk_widget_set_sensitive(GTK_WIDGET(d->paste), FALSE);
}

static void
key_accel_copy_callback(GtkAccelGroup *accel_group,
                        GObject *acceleratable, guint keyval,
                        GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  copy_button_clicked(NULL, self);
}

static void
key_accel_paste_callback(GtkAccelGroup *accel_group,
                         GObject *acceleratable, guint keyval,
                         GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  paste_button_clicked(NULL, self);
}

int
position ()
{
  return 600;
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)malloc(sizeof(dt_lib_copy_history_t));
  self->data = (void *)d;
  self->widget = gtk_vbox_new(TRUE, 5);

  GtkBox *hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));
  GtkWidget *copy = gtk_button_new_with_label(_("copy"));
  dt_accel_group_connect_by_path(
      darktable.control->accels_lighttable,
      "<Darktable>/lighttable/plugins/copy_history/copy",
      g_cclosure_new(G_CALLBACK(key_accel_copy_callback),
                     (gpointer)self, NULL));
  g_object_set(G_OBJECT(copy), "tooltip-text", _("copy history stack of\nfirst selected image (ctrl-c)"), (char *)NULL);
  gtk_box_pack_start(hbox, copy, TRUE, TRUE, 0);

  GtkWidget *delete = gtk_button_new_with_label(_("discard"));
  g_object_set(G_OBJECT(delete), "tooltip-text", _("discard history stack of\nall selected images"), (char *)NULL);
  gtk_box_pack_start(hbox, delete, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  d->pastemode = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(d->pastemode, _("append"));
  gtk_combo_box_append_text(d->pastemode, _("overwrite"));
  g_object_set(G_OBJECT(d->pastemode), "tooltip-text", _("how to handle existing history"), (char *)NULL);
  gtk_box_pack_start(hbox, GTK_WIDGET(d->pastemode), TRUE, TRUE, 0);
  gtk_combo_box_set_active(d->pastemode, dt_conf_get_int("plugins/lighttable/copy_history/pastemode"));

  d->paste = GTK_BUTTON(gtk_button_new_with_label(_("paste")));
  dt_accel_group_connect_by_path(
      darktable.control->accels_lighttable,
      "<Darktable>/lighttable/plugins/copy_history/paste",
      g_cclosure_new(G_CALLBACK(key_accel_paste_callback),
                     (gpointer)self, NULL));
  g_object_set(G_OBJECT(d->paste), "tooltip-text", _("paste history stack to\nall selected images (ctrl-v)"), (char *)NULL);
  d->imageid = -1;
  gtk_widget_set_sensitive(GTK_WIDGET(d->paste), FALSE);
  gtk_box_pack_start(hbox, GTK_WIDGET(d->paste), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));
  GtkWidget *loadbutton = gtk_button_new_with_label(_("load sidecar file"));
  g_object_set(G_OBJECT(loadbutton), "tooltip-text", _("open an xmp sidecar file\nand apply it to selected images"), (char *)NULL);
  gtk_box_pack_start(hbox, loadbutton, TRUE, TRUE, 0);

  GtkWidget *button = gtk_button_new_with_label(_("write sidecar files"));
  g_object_set(G_OBJECT(button), "tooltip-text", _("write history stack and tags to xmp sidecar files"), (char *)NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(write_button_clicked), (gpointer)self);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (copy), "clicked",
                    G_CALLBACK (copy_button_clicked),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (delete), "clicked",
                    G_CALLBACK (delete_button_clicked),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (d->paste), "clicked",
                    G_CALLBACK (paste_button_clicked),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (loadbutton), "clicked",
                    G_CALLBACK (load_button_clicked),
                    (gpointer)self);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

void init_key_accels()
{
  gtk_accel_map_add_entry("<Darktable>/lighttable/plugins/copy_history/copy",
                          GDK_c, GDK_CONTROL_MASK);
  gtk_accel_map_add_entry("<Darktable>/lighttable/plugins/copy_history/paste",
                          GDK_v, GDK_CONTROL_MASK);
}
