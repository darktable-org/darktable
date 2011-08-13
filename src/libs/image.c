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
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "libs/lib.h"
#include "control/jobs.h"
#include <stdlib.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef struct dt_lib_image_t
{
}
dt_lib_image_t;

const char*
name ()
{
  return _("selected image[s]");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

void
gui_reset (dt_lib_module_t *self)
{
}


static void
button_clicked(GtkWidget *widget, gpointer user_data)
{
  long int i = (long int)user_data;
  if     (i == 0) dt_control_remove_images();
  else if(i == 1) dt_control_delete_images();
  // else if(i == 2) dt_control_write_sidecar_files();
  else if(i == 3) dt_control_duplicate_images();
  else if(i == 4) dt_control_flip_images(0);
  else if(i == 5) dt_control_flip_images(1);
  else if(i == 6) dt_control_flip_images(2);
  else if(i == 7) dt_control_merge_hdr();
  dt_control_queue_redraw_center();
}

static void key_accel_callback(GtkAccelGroup *accel_group,
                               GObject *acceleratable, guint keyval,
                               GdkModifierType modifier, gpointer data)
{
  button_clicked(NULL, data);
}

int
position ()
{
  return 700;
}

void
gui_init (dt_lib_module_t *self)
{
  // dt_lib_image_t *d = (dt_lib_image_t *)malloc(sizeof(dt_lib_image_t));
  // self->data = (void *)d;
  self->widget = gtk_vbox_new(TRUE, 5);
  GtkBox *hbox;
  GtkWidget *button;
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  button = gtk_button_new_with_label(_("remove"));
  gtk_button_set_accel(GTK_BUTTON(button),darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/remove");
  g_object_set(G_OBJECT(button), "tooltip-text", _("remove from the collection"), (char *)NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)0);

  button = gtk_button_new_with_label(_("delete"));
  gtk_button_set_accel(GTK_BUTTON(button),darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/delete");
  g_object_set(G_OBJECT(button), "tooltip-text", _("physically delete from disk"), (char *)NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)1);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  button = gtk_button_new_with_label(_("create hdr"));
  gtk_button_set_accel(GTK_BUTTON(button),darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/create hdr");
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)7);
  g_object_set(G_OBJECT(button), "tooltip-text", _("create a high dynamic range image from selected shots"), (char *)NULL);

  button = gtk_button_new_with_label(_("duplicate"));
  gtk_button_set_accel(GTK_BUTTON(button),darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/duplicate");
  g_object_set(G_OBJECT(button), "tooltip-text", _("add a duplicate to the collection"), (char *)NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)3);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  GtkBox *hbox2 = GTK_BOX(gtk_hbox_new(TRUE, 5));
  button = dtgtk_button_new(dtgtk_cairo_paint_refresh, 0);
  dtgtk_button_set_accel(DTGTK_BUTTON(button),darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/rotate selected images 90 degrees ccw");
  g_object_set(G_OBJECT(button), "tooltip-text", _("rotate selected images 90 degrees ccw"), (char *)NULL);
  gtk_box_pack_start(hbox2, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)4);

  button = dtgtk_button_new(dtgtk_cairo_paint_refresh, 1);
  dtgtk_button_set_accel(DTGTK_BUTTON(button),darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/rotate selected images 90 degrees cw");
  g_object_set(G_OBJECT(button), "tooltip-text", _("rotate selected images 90 degrees cw"), (char *)NULL);
  gtk_box_pack_start(hbox2, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)5);
  gtk_box_pack_start(hbox, GTK_WIDGET(hbox2), TRUE, TRUE, 0);

  button = gtk_button_new_with_label(_("reset rotation"));
  gtk_button_set_accel(GTK_BUTTON(button),darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/reset rotation");
  g_object_set(G_OBJECT(button), "tooltip-text", _("reset rotation to exif data"), (char *)NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)6);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  // free(self->data);
  // self->data = NULL;
}

void init_key_accels(dt_lib_module_t *self)
{
  gtk_accel_map_add_entry(
      "<Darktable>/lighttable/plugins/image/remove from collection",
      GDK_Delete, 0);
  gtk_accel_map_add_entry(
      "<Darktable>/lighttable/plugins/image/delete from disk",
      0, 0);

  dt_accel_group_connect_by_path(
      darktable.control->accels_lighttable,
      "<Darktable>/lighttable/plugins/image/remove from collection",
      g_cclosure_new(G_CALLBACK(key_accel_callback), (gpointer)0, NULL));
  dt_accel_group_connect_by_path(
      darktable.control->accels_lighttable,
      "<Darktable>/lighttable/plugins/image/delete from disk",
      g_cclosure_new(G_CALLBACK(key_accel_callback), (gpointer)1, NULL));
  dtgtk_button_init_accel(darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/rotate selected images 90 degrees cw");
  dtgtk_button_init_accel(darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/rotate selected images 90 degrees ccw");
  gtk_button_init_accel(darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/remove");
  gtk_button_init_accel(darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/delete");
  gtk_button_init_accel(darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/create hdr");
  gtk_button_init_accel(darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/duplicate");
  gtk_button_init_accel(darktable.control->accels_lighttable,"<Darktable>/lighttable/plugins/image/reset rotation");
}
