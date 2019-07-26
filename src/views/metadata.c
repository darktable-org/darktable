/*
    This file is part of darktable,
    copyright (c) 2019 sam smith.

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
#include "gui/accelerators.h"
#include "views/view.h"
#include "views/view_api.h"

#include "control/conf.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

const char *name(const dt_view_t *self)
{
  return C_("view", "metadata");
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_METADATA;
}

static void _mipmaps_updated_signal_callback(gpointer instance, gpointer user_data)
{
  dt_control_queue_redraw_center();
}

/*
static void _film_strip_activated(const int imgid, void *data)
{
  //const dt_view_t *self = (dt_view_t *)data;

  dt_view_filmstrip_scroll_to_image(darktable.view_manager, imgid, FALSE);
  dt_view_lighttable_set_position(darktable.view_manager, dt_collection_image_offset(imgid));

  dt_control_queue_redraw();
}
*/

void init(dt_view_t *self)
{
  dt_view_filmstrip_prefetch();
}

void cleanup(dt_view_t *self)
{
}

void expose(dt_view_t *self, cairo_t *cri, int32_t width_i, int32_t height_i, int32_t pointerx, int32_t pointery)
{
  cairo_set_source_rgb(cri, 0.1, 0.1, 0.1);
  cairo_paint(cri);
}

int try_enter(dt_view_t *self)
{
  int selected = dt_control_get_mouse_over_id();
  if(selected < 0)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt,
                                NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW) selected = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT OR IGNORE INTO main.selected_images VALUES (?1)", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, selected);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  if(selected < 0)
  {
    dt_control_log(_("no image selected!"));
    return 1;
  }

  return 0;
}

void enter(dt_view_t *self)
{
  /* scroll filmstrip to first selected image */
  GList *selected_images = dt_collection_get_selected(darktable.collection, 1);
  if(selected_images)
  {
    const int imgid = GPOINTER_TO_INT(selected_images->data);
    dt_view_filmstrip_scroll_to_image(darktable.view_manager, imgid, TRUE);
  }
  g_list_free(selected_images);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_mipmaps_updated_signal_callback),
                            (gpointer)self);

  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

  dt_view_filmstrip_prefetch();

  darktable.control->mouse_over_id = -1;
}

void leave(dt_view_t *self)
{
}

static gboolean film_strip_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                     GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *m = darktable.view_manager->proxy.filmstrip.module;
  const gboolean vs = dt_lib_is_visible(m);
  dt_lib_set_visible(m, !vs);
  return TRUE;
}

void init_key_accels(dt_view_t *self)
{
  // Film strip shortcuts
  dt_accel_register_view(self, NC_("accel", "toggle film strip"), GDK_KEY_f, GDK_CONTROL_MASK);
}

void connect_key_accels(dt_view_t *self)
{
  GClosure *closure;

  // Film strip shortcuts
  closure = g_cclosure_new(G_CALLBACK(film_strip_key_accel), (gpointer)self, NULL);
  dt_accel_connect_view(self, "toggle film strip", closure);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
