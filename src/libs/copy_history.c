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
#include "common/image_cache.h"
#include "control/control.h"
#include "control/conf.h"
#include "control/jobs.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include <stdlib.h>
#include <gtk/gtk.h>
#include <glade/glade.h>
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

static void
copy_button_clicked (GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  int rc;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    d->imageid = sqlite3_column_int(stmt, 0);
    gtk_widget_set_sensitive(GTK_WIDGET(d->paste), TRUE);
  }
  sqlite3_finalize(stmt);
}

static void
delete_button_clicked (GtkWidget *widget, gpointer user_data)
{
  int imgid = -1;
  sqlite3_stmt *stmt, *stmt2;
  sqlite3_prepare_v2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    imgid = sqlite3_column_int(stmt, 0);
    sqlite3_prepare_v2(darktable.db, "delete from history where imgid = ?1", -1, &stmt2, NULL);
    sqlite3_bind_int(stmt2, 1, imgid);
    sqlite3_step(stmt2);
    sqlite3_finalize(stmt2);

    dt_image_t tmp;
    dt_image_init(&tmp);
    dt_image_t *img = dt_image_cache_use(imgid, 'r');
    img->force_reimport = 1;
    img->raw_params = tmp.raw_params;
    img->raw_denoise_threshold = tmp.raw_denoise_threshold;
    img->raw_auto_bright_threshold = tmp.raw_auto_bright_threshold;
    img->black = tmp.black;
    img->maximum = tmp.maximum;
    img->output_width = img->width;
    img->output_height = img->height;
    dt_image_cache_flush(img);
    dt_image_write_dt_files(img);
    dt_image_cache_release(img, 'r');
  }
  sqlite3_finalize(stmt);
  dt_control_gui_queue_draw();
}

static void
paste_button_clicked (GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  if(d->imageid < 0) return;

  dt_image_t *oimg = dt_image_cache_use(d->imageid, 'r');
  int rc;
  sqlite3_stmt *stmt, *stmt2;
  rc = sqlite3_prepare_v2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int32_t imgid = sqlite3_column_int(stmt, 0);
    if(imgid == d->imageid) continue;
    int32_t offs = 0; // current history stack height
    // if stacking is requested, count history items.
    int i = gtk_combo_box_get_active(d->pastemode);
    dt_conf_set_int("plugins/lighttable/copy_history/pastemode", i);
    if(i == 0)
    { // stack on top
      rc = sqlite3_prepare_v2(darktable.db, "select num from history where imgid = ?1", -1, &stmt2, NULL);
      rc = sqlite3_bind_int(stmt2, 1, imgid);
      while(sqlite3_step(stmt2) == SQLITE_ROW) offs++;
    }
    else
    { // replace
      rc = sqlite3_prepare_v2(darktable.db, "delete from history where imgid = ?1", -1, &stmt2, NULL);
      rc = sqlite3_bind_int(stmt2, 1, imgid);
      rc = sqlite3_step(stmt2);
    }
    rc = sqlite3_finalize(stmt2);

    rc = sqlite3_prepare_v2(darktable.db, "insert into history (imgid, num, module, operation, op_params, enabled) select ?1, num+?2, module, operation, op_params, enabled from history where imgid = ?3", -1, &stmt2, NULL);
    rc = sqlite3_bind_int(stmt2, 1, imgid);
    rc = sqlite3_bind_int(stmt2, 2, offs);
    rc = sqlite3_bind_int(stmt2, 3, d->imageid);
    rc = sqlite3_step(stmt2);
    rc = sqlite3_finalize(stmt2);
    rc = sqlite3_prepare_v2(darktable.db, "delete from mipmaps where imgid = ?1", -1, &stmt2, NULL);
    rc = sqlite3_bind_int(stmt2, 1, imgid);
    rc = sqlite3_step(stmt2);
    rc = sqlite3_finalize(stmt2);
    dt_image_t *img = dt_image_cache_use(imgid, 'r');
    img->force_reimport = 1;
    img->raw_params = oimg->raw_params;
    img->raw_denoise_threshold = oimg->raw_denoise_threshold;
    img->raw_auto_bright_threshold = oimg->raw_auto_bright_threshold;
    dt_image_cache_flush(img);
    dt_image_write_dt_files(img);
    dt_image_cache_release(img, 'r');
  }
  dt_image_cache_release(oimg, 'r');
  sqlite3_finalize(stmt);
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
key_accel_copy_callback(void *user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  copy_button_clicked(NULL, self);
}

static void
key_accel_paste_callback(void *user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  paste_button_clicked(NULL, self);
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)malloc(sizeof(dt_lib_copy_history_t));
  self->data = (void *)d;
  self->widget = gtk_vbox_new(TRUE, 5);

  GtkBox *hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));
  GtkWidget *copy = gtk_button_new_with_label(_("copy"));
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_c, key_accel_copy_callback, (void *)self);
  gtk_object_set(GTK_OBJECT(copy), "tooltip-text", _("copy history stack of\nfirst selected image (ctrl-c)"), NULL);
  gtk_box_pack_start(hbox, copy, TRUE, TRUE, 0);

  GtkWidget *delete = gtk_button_new_with_label(_("discard"));
  gtk_object_set(GTK_OBJECT(delete), "tooltip-text", _("discard history stack of\nfirst selected image"), NULL);
  gtk_box_pack_start(hbox, delete, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  d->pastemode = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(d->pastemode, _("append"));
  gtk_combo_box_append_text(d->pastemode, _("overwrite"));
  gtk_object_set(GTK_OBJECT(d->pastemode), "tooltip-text", _("how to handle existing history"), NULL);
  gtk_box_pack_start(hbox, GTK_WIDGET(d->pastemode), TRUE, TRUE, 0);
  gtk_combo_box_set_active(d->pastemode, dt_conf_get_int("plugins/lighttable/copy_history/pastemode"));

  d->paste = GTK_BUTTON(gtk_button_new_with_label(_("paste")));
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_v, key_accel_paste_callback, (void *)self);
  gtk_object_set(GTK_OBJECT(d->paste), "tooltip-text", _("paste history stack to\nall selected images (ctrl-v)"), NULL);
  d->imageid = -1;
  gtk_widget_set_sensitive(GTK_WIDGET(d->paste), FALSE);
  gtk_box_pack_start(hbox, GTK_WIDGET(d->paste), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (copy), "pressed",
                    G_CALLBACK (copy_button_clicked),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (delete), "pressed",
                    G_CALLBACK (delete_button_clicked),
                    (gpointer)self);
  g_signal_connect (G_OBJECT (d->paste), "pressed",
                    G_CALLBACK (paste_button_clicked),
                    (gpointer)self);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}


