/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011 henrik andersson.

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
#include "common/debug.h"
#include "common/grouping.h"
#include "common/collection.h"
#include "control/control.h"
#include "control/jobs/control_jobs.h"
#include "gui/accelerators.h"
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
  GtkWidget *rotate_cw_button, *rotate_ccw_button, *remove_button, *delete_button, *create_hdr_button,
      *duplicate_button, *reset_button, *move_button, *copy_button, *group_button, *ungroup_button,
      *cache_button, *uncache_button;
} dt_lib_image_t;

const char *name()
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

/** merges all the selected images into a single group.
 * if there is an expanded group, then they will be joined there, otherwise a new one will be created. */
static void _group_helper_function(void)
{
  int new_group_id = darktable.gui->expanded_group_id;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select distinct imgid from selected_images", -1,
                              &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
    if(new_group_id == -1) new_group_id = id;
    dt_grouping_add_to_group(new_group_id, id);
  }
  sqlite3_finalize(stmt);
  if(darktable.gui->grouping)
    darktable.gui->expanded_group_id = new_group_id;
  else
    darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection);
  dt_control_queue_redraw_center();
}

/** removes the selected images from their current group. */
static void _ungroup_helper_function(void)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select distinct imgid from selected_images", -1,
                              &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
    dt_grouping_remove_from_group(id);
  }
  sqlite3_finalize(stmt);
  darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection);
  dt_control_queue_redraw_center();
}

static void button_clicked(GtkWidget *widget, gpointer user_data)
{
  int i = GPOINTER_TO_INT(user_data);
  if(i == 0)
    dt_control_remove_images();
  else if(i == 1)
    dt_control_delete_images();
  // else if(i == 2) dt_control_write_sidecar_files();
  else if(i == 3)
    dt_control_duplicate_images();
  else if(i == 4)
    dt_control_flip_images(0);
  else if(i == 5)
    dt_control_flip_images(1);
  else if(i == 6)
    dt_control_flip_images(2);
  else if(i == 7)
    dt_control_merge_hdr();
  else if(i == 8)
    dt_control_move_images();
  else if(i == 9)
    dt_control_copy_images();
  else if(i == 10)
    _group_helper_function();
  else if(i == 11)
    _ungroup_helper_function();
  else if(i == 12)
    dt_control_set_local_copy_images();
  else if(i == 13)
    dt_control_reset_local_copy_images();
}

int position()
{
  return 700;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_image_t *d = (dt_lib_image_t *)malloc(sizeof(dt_lib_image_t));
  self->data = (void *)d;
  self->widget = gtk_grid_new();
  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_row_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_homogeneous(grid, TRUE);
  int line = 0;

  GtkWidget *button;

  button = gtk_button_new_with_label(_("remove"));
  d->remove_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("remove from the collection"), (char *)NULL);
  gtk_grid_attach(grid, button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(0));

  button = gtk_button_new_with_label(_("delete"));
  d->delete_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("physically delete from disk"), (char *)NULL);
  gtk_grid_attach(grid, button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(1));


  button = gtk_button_new_with_label(_("move"));
  d->move_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("move to other folder"), (char *)NULL);
  gtk_grid_attach(grid, button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(8));

  button = gtk_button_new_with_label(_("copy"));
  d->copy_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("copy to other folder"), (char *)NULL);
  gtk_grid_attach(grid, button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(9));


  button = gtk_button_new_with_label(_("create HDR"));
  d->create_hdr_button = button;
  gtk_grid_attach(grid, button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(7));
  g_object_set(G_OBJECT(button), "tooltip-text", _("create a high dynamic range image from selected shots"),
               (char *)NULL);

  button = gtk_button_new_with_label(_("duplicate"));
  d->duplicate_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text",
               _("add a duplicate to the collection, including its history stack"), (char *)NULL);
  gtk_grid_attach(grid, button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(3));


  button = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_DO_NOT_USE_BORDER);
  d->rotate_ccw_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("rotate selected images 90 degrees CCW"), (char *)NULL);
  gtk_grid_attach(grid, button, 0, line, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(4));

  button = dtgtk_button_new(dtgtk_cairo_paint_refresh, 1 | CPF_DO_NOT_USE_BORDER);
  d->rotate_cw_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("rotate selected images 90 degrees CW"), (char *)NULL);
  gtk_grid_attach(grid, button, 1, line, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(5));

  button = gtk_button_new_with_label(_("reset rotation"));
  d->reset_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("reset rotation to EXIF data"), (char *)NULL);
  gtk_grid_attach(grid, button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(6));


  button = gtk_button_new_with_label(_("copy locally"));
  d->cache_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("copy the image locally"), (char *)NULL);
  gtk_grid_attach(grid, button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(12));

  button = gtk_button_new_with_label(_("resync local copy"));
  d->uncache_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("synchronize the image's XMP and remove the local copy"),
               (char *)NULL);
  gtk_grid_attach(grid, button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(13));


  button = gtk_button_new_with_label(_("group"));
  d->group_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text",
               _("add selected images to expanded group or create a new one"), (char *)NULL);
  gtk_grid_attach(grid, button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(10));

  button = gtk_button_new_with_label(_("ungroup"));
  d->ungroup_button = button;
  g_object_set(G_OBJECT(button), "tooltip-text", _("remove selected images from the group"), (char *)NULL);
  gtk_grid_attach(grid, button, 2, line, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(11));
}

void gui_cleanup(dt_lib_module_t *self)
{
  // free(self->data);
  // self->data = NULL;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "remove from collection"), GDK_KEY_Delete, 0);
  dt_accel_register_lib(self, NC_("accel", "delete from disk"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "rotate selected images 90 degrees CW"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "rotate selected images 90 degrees CCW"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "create HDR"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "duplicate"), GDK_KEY_d, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "reset rotation"), 0, 0);
  // Grouping keys
  dt_accel_register_lib(self, NC_("accel", "group"), GDK_KEY_g, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "ungroup"), GDK_KEY_g, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_image_t *d = (dt_lib_image_t *)self->data;

  dt_accel_connect_button_lib(self, "remove from collection", d->remove_button);
  dt_accel_connect_button_lib(self, "delete from disk", d->delete_button);
  dt_accel_connect_button_lib(self, "rotate selected images 90 degrees CW", d->rotate_cw_button);
  dt_accel_connect_button_lib(self, "rotate selected images 90 degrees CCW", d->rotate_ccw_button);
  dt_accel_connect_button_lib(self, "create HDR", d->create_hdr_button);
  dt_accel_connect_button_lib(self, "duplicate", d->duplicate_button);
  dt_accel_connect_button_lib(self, "reset rotation", d->reset_button);
  // Grouping keys
  dt_accel_connect_button_lib(self, "group", d->group_button);
  dt_accel_connect_button_lib(self, "ungroup", d->ungroup_button);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
