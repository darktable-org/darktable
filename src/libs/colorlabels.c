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
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

const char*
name ()
{
  return _("color labels");
}

static void
remove_labels_selection ()
{
  sqlite3_exec(darktable.db, "delete from color_labels where imgid in (select imgid from selected_images)", NULL, NULL, NULL);
}

static void
remove_labels (const int imgid)
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "delete from color_labels where imgid=?1", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void
toggle_label_selection (const int color)
{
  sqlite3_stmt *stmt;
  // store away all previously unlabeled images in selection:
  sqlite3_exec(darktable.db, "create temp table color_labels_temp (imgid integer primary key)", NULL, NULL, NULL);
  sqlite3_prepare_v2(darktable.db, "insert into color_labels_temp select a.imgid from selected_images as a join color_labels as b on a.imgid = b.imgid where b.color = ?1", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // delete all currently colored image labels in selection 
  sqlite3_prepare_v2(darktable.db, "delete from color_labels where imgid in (select imgid from selected_images) and color=?1", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // label al previously unlabeled images:
  sqlite3_prepare_v2(darktable.db, "insert or replace into color_labels select imgid, ?1 from selected_images where imgid not in (select imgid from color_labels_temp)", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // clean up
  sqlite3_exec(darktable.db, "delete from color_labels_temp", NULL, NULL, NULL);
  sqlite3_exec(darktable.db, "drop table color_labels_temp", NULL, NULL, NULL);
}

static void
toggle_label (const int imgid, const int color)
{
  sqlite3_stmt *stmt, *stmt2;
  sqlite3_prepare_v2(darktable.db, "select * from color_labels where imgid=?1 and color=?2", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, imgid);
  sqlite3_bind_int(stmt, 2, color);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    sqlite3_prepare_v2(darktable.db, "delete from color_labels where imgid=?1 and color=?2", -1, &stmt2, NULL);
    sqlite3_bind_int(stmt2, 1, imgid);
    sqlite3_bind_int(stmt2, 2, color);
    sqlite3_step(stmt2);
    sqlite3_finalize(stmt2);
  }
  else
  { // red label replaces green etc.
    sqlite3_prepare_v2(darktable.db, "insert or replace into color_labels (imgid, color) values (?1, ?2)", -1, &stmt2, NULL);
    sqlite3_bind_int(stmt2, 1, imgid);
    sqlite3_bind_int(stmt2, 2, color);
    sqlite3_step(stmt2);
    sqlite3_finalize(stmt2);
  }
  sqlite3_finalize(stmt);
}

static void
button_clicked(GtkWidget *widget, gpointer user_data)
{
  const long int mode = (long int)user_data;
  int selected;
  DT_CTL_GET_GLOBAL(selected, lib_image_mouse_over_id);
  if(selected < 0)
  {
    switch(mode)
    {
      case 0: case 1: case 2: // colors red, yellow, green
        toggle_label_selection(mode);
        break;
      case 3: default: // remove all selected
        remove_labels_selection();
        break;
    }
  }
  else
  {
    switch(mode)
    {
      case 0: case 1: case 2: // colors red, yellow, green
        toggle_label(selected, mode);
        break;
      case 3: default: // remove all selected
        remove_labels(selected);
        break;
    }
  }
  dt_control_queue_draw_all();
}

static void key_accel_callback(void *d)
{
  button_clicked(NULL, d);
}

void
gui_reset (dt_lib_module_t *self)
{
  // maybe we want to maintain these for a while longer:
  // sqlite3_exec(darktable.db, "delete from color_labels", NULL, NULL, NULL);
}

int
position ()
{
  return 850;
}

void
gui_init (dt_lib_module_t *self)
{
  self->data = NULL;
  self->widget = gtk_vbox_new(TRUE, 5);
  GtkBox *hbox;
  GtkWidget *button;
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  button = gtk_button_new_with_label(_("red"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("toggle red label\nof selected images (ctrl-1)"), NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_1, key_accel_callback, (void *)0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)0);

  button = gtk_button_new_with_label(_("yellow"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("toggle yellow label\nof selected images (ctrl-2)"), NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_2, key_accel_callback, (void *)1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)1);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  button = gtk_button_new_with_label(_("green"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("toggle green label\nof selected images (ctrl-3)"), NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_3, key_accel_callback, (void *)2);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)2);

  button = gtk_button_new_with_label(_("clear"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("clear all labels of selected images"), NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)3);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  dt_gui_key_accel_unregister(key_accel_callback);
}

