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
#include "gui/gtk.h"
#include <gdk/gdkkeysyms.h>


void dt_colorlabels_remove_labels_selection ()
{
  sqlite3_exec(darktable.db, "delete from color_labels where imgid in (select imgid from selected_images)", NULL, NULL, NULL);
}

void dt_colorlabels_remove_labels (const int imgid)
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "delete from color_labels where imgid=?1", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_colorlabels_set_label (const int imgid, const int color)
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "insert into color_labels (imgid, color) values (?1, ?2)", -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, imgid);
  sqlite3_bind_int (stmt, 2, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_colorlabels_remove_label (const int imgid, const int color) 
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "delete from color_label where imgid=?1 and color=?2", -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, imgid);
  sqlite3_bind_int (stmt, 2, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}


void dt_colorlabels_toggle_label_selection (const int color)
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

  // label all previously unlabeled images:
  sqlite3_prepare_v2(darktable.db, "insert into color_labels select imgid, ?1 from selected_images where imgid not in (select imgid from color_labels_temp)", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // clean up
  sqlite3_exec(darktable.db, "delete from color_labels_temp", NULL, NULL, NULL);
  sqlite3_exec(darktable.db, "drop table color_labels_temp", NULL, NULL, NULL);
}

void dt_colorlabels_toggle_label (const int imgid, const int color)
{
  if(imgid <= 0) return;
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
  {
    sqlite3_prepare_v2(darktable.db, "insert into color_labels (imgid, color) values (?1, ?2)", -1, &stmt2, NULL);
    sqlite3_bind_int(stmt2, 1, imgid);
    sqlite3_bind_int(stmt2, 2, color);
    sqlite3_step(stmt2);
    sqlite3_finalize(stmt2);
  }
  sqlite3_finalize(stmt);
}


void dt_colorlabels_key_accel_callback(void *user_data)
{
  const long int mode = (long int)user_data;
  int selected;
  DT_CTL_GET_GLOBAL(selected, lib_image_mouse_over_id);
  if(selected <= 0)
  {
    switch(mode)
    {
      case 0: case 1: case 2: // colors red, yellow, green
        dt_colorlabels_toggle_label_selection(mode);
        break;
      case 3: default: // remove all selected
        dt_colorlabels_remove_labels_selection();
        break;
    }
  }
  else
  {
    switch(mode)
    {
      case 0: case 1: case 2: // colors red, yellow, green
        dt_colorlabels_toggle_label(selected, mode);
        break;
      case 3: default: // remove all selected
        dt_colorlabels_remove_labels(selected);
        break;
    }
  }
  dt_control_queue_draw_all();
}

void dt_colorlabels_register_key_accels()
{
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_1, dt_colorlabels_key_accel_callback, (void *)0);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_2, dt_colorlabels_key_accel_callback, (void *)1);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_3, dt_colorlabels_key_accel_callback, (void *)2);
}

void dt_colorlabels_unregister_key_accels()
{
  dt_gui_key_accel_unregister(dt_colorlabels_key_accel_callback);
}

