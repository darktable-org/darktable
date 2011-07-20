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
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

const char*
name ()
{
  return _("select");
}

uint32_t views()
{
  return DT_LIGHTTABLE_VIEW;
}

static void
button_clicked(GtkWidget *widget, gpointer user_data)
{
  char fullq[2048];

  /* create a copy of darktable collection */
  const dt_collection_t *collection = dt_collection_new (darktable.collection);

  /* set query flags to not include order or limit part */
  dt_collection_set_query_flags (collection, (dt_collection_get_query_flags(collection)&(~(COLLECTION_QUERY_USE_SORT|COLLECTION_QUERY_USE_LIMIT))));
  dt_collection_update (collection);
  snprintf (fullq, 2048, "insert into selected_images %s", dt_collection_get_query (collection));

  switch((long int)user_data)
  {
    case 0: // all
      DT_DEBUG_SQLITE3_EXEC(darktable.db, "delete from selected_images", NULL, NULL, NULL);
      DT_DEBUG_SQLITE3_EXEC(darktable.db, fullq, NULL, NULL, NULL);
      break;
    case 1: // none
      DT_DEBUG_SQLITE3_EXEC(darktable.db, "delete from selected_images", NULL, NULL, NULL);
      break;
    case 2: // invert
      DT_DEBUG_SQLITE3_EXEC(darktable.db, "insert into tmp_selection select imgid from selected_images", NULL, NULL, NULL);
      DT_DEBUG_SQLITE3_EXEC(darktable.db, "delete from selected_images", NULL, NULL, NULL);
      DT_DEBUG_SQLITE3_EXEC(darktable.db, fullq, NULL, NULL, NULL);
      DT_DEBUG_SQLITE3_EXEC(darktable.db, "delete from selected_images where imgid in (select imgid from tmp_selection)", NULL, NULL, NULL);
      DT_DEBUG_SQLITE3_EXEC(darktable.db, "delete from tmp_selection", NULL, NULL, NULL);
      break;
    case 4: // untouched
      dt_collection_set_filter_flags (collection, (dt_collection_get_filter_flags(collection)|COLLECTION_FILTER_UNALTERED));
      dt_collection_update (collection);
      snprintf (fullq, 2048, "insert into selected_images %s", dt_collection_get_query (collection));
      DT_DEBUG_SQLITE3_EXEC(darktable.db, "delete from selected_images", NULL, NULL, NULL);
      DT_DEBUG_SQLITE3_EXEC(darktable.db, fullq, NULL, NULL, NULL);
      break;
    default: // case 3: same film roll
      DT_DEBUG_SQLITE3_EXEC(darktable.db, "create temp table tmp_selection (imgid integer)", NULL, NULL, NULL);
      DT_DEBUG_SQLITE3_EXEC(darktable.db, "insert into tmp_selection select imgid from selected_images", NULL, NULL, NULL);
      DT_DEBUG_SQLITE3_EXEC(darktable.db, "delete from selected_images", NULL, NULL, NULL);
      DT_DEBUG_SQLITE3_EXEC(darktable.db, "insert into selected_images select id from images where film_id in (select film_id from images as a join tmp_selection as b on a.id = b.imgid)", NULL, NULL, NULL);
      DT_DEBUG_SQLITE3_EXEC(darktable.db, "delete from tmp_selection", NULL, NULL, NULL);
      DT_DEBUG_SQLITE3_EXEC(darktable.db, "drop table tmp_selection", NULL, NULL, NULL);
      break;
  }

  /* free temporary collection and redraw visual*/
  dt_collection_free(collection);

  dt_control_queue_draw_all();
}

static void key_accel_callback(GtkAccelGroup *accel_group,
                               GObject *acceleratable, guint keyval,
                               GdkModifierType modifier, gpointer data)
{
  button_clicked(NULL, data);
}

void
gui_reset (dt_lib_module_t *self)
{
}

int
position ()
{
  return 800;
}

void
gui_init (dt_lib_module_t *self)
{
  self->data = NULL;
  self->widget = gtk_vbox_new(TRUE, 5);
  GtkBox *hbox;
  GtkWidget *button;
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  button = gtk_button_new_with_label(_("select all"));
  g_object_set(G_OBJECT(button), "tooltip-text", _("select all images in current collection (ctrl-a)"), (char *)NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)0);

  button = gtk_button_new_with_label(_("select none"));
  g_object_set(G_OBJECT(button), "tooltip-text", _("clear selection (ctrl-shift-a)"), (char *)NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)1);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  button = gtk_button_new_with_label(_("invert selection"));
  g_object_set(G_OBJECT(button), "tooltip-text", _("select unselected images\nin current collection (ctrl-!)"), (char *)NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)2);

  button = gtk_button_new_with_label(_("select film roll"));
  g_object_set(G_OBJECT(button), "tooltip-text", _("select all images which are in the same\nfilm roll as the selected images"), (char *)NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)3);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));

  button = gtk_button_new_with_label(_("select untouched"));
  g_object_set(G_OBJECT(button), "tooltip-text", _("select untouched images in\ncurrent collection"), (char *)NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)4);
  // Just a filler, remove if a new button is added
  gtk_box_pack_start(hbox,gtk_hbox_new(TRUE, 5),TRUE,TRUE,0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
}

void
gui_cleanup (dt_lib_module_t *self)
{
}

void init_key_accels()
{
  gtk_accel_map_add_entry("<Darktable>/lighttable/plugins/select/all",
                          GDK_a, GDK_CONTROL_MASK);
  gtk_accel_map_add_entry("<Darktable>/lighttable/plugins/select/none",
                          GDK_a, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  gtk_accel_map_add_entry("<Darktable>/lighttable/plugins/select/invert",
                          GDK_exclam, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  dt_accel_group_connect_by_path(
      darktable.control->accels_lighttable,
      "<Darktable>/lighttable/plugins/select/all",
      g_cclosure_new(G_CALLBACK(key_accel_callback),
                     (gpointer)0, NULL));
  dt_accel_group_connect_by_path(
      darktable.control->accels_lighttable,
      "<Darktable>/lighttable/plugins/select/none",
      g_cclosure_new(G_CALLBACK(key_accel_callback),
                     (gpointer)1, NULL));
  dt_accel_group_connect_by_path(
      darktable.control->accels_lighttable,
      "<Darktable>/lighttable/plugins/select/invert",
      g_cclosure_new(G_CALLBACK(key_accel_callback),
                     (gpointer)2, NULL));
}
