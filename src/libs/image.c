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
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/grouping.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/jobs/control_jobs.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#ifdef USE_LUA
#include "lua/call.h"
#include "lua/image.h"
#endif
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_image_t
{
  GtkWidget *rotate_cw_button, *rotate_ccw_button, *remove_button, *delete_button, *create_hdr_button,
      *duplicate_button, *reset_button, *move_button, *copy_button, *group_button, *ungroup_button,
      *cache_button, *uncache_button;
} dt_lib_image_t;

const char *name(dt_lib_module_t *self)
{
  return _("selected image[s]");
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

/** merges all the selected images into a single group.
 * if there is an expanded group, then they will be joined there, otherwise a new one will be created. */
static void _group_helper_function(void)
{
  int new_group_id = darktable.gui->expanded_group_id;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1,
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
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1,
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

static const char* _image_get_delete_button_label()
{
if (dt_conf_get_bool("send_to_trash"))
  return _("trash");
else
  return _("delete");
}

static const char* _image_get_delete_button_tooltip()
{
if (dt_conf_get_bool("send_to_trash"))
  return _("send file to trash");
else
  return _("physically delete from disk");
}


static void _image_preference_changed(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t*)user_data;
  dt_lib_image_t *d = (dt_lib_image_t *)self->data;
  gtk_button_set_label(GTK_BUTTON(d->delete_button), _image_get_delete_button_label());
  gtk_widget_set_tooltip_text(d->delete_button, _image_get_delete_button_tooltip());
}

int position()
{
  return 700;
}

#define ellipsize_button(button) gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), PANGO_ELLIPSIZE_END);
void gui_init(dt_lib_module_t *self)
{
  dt_lib_image_t *d = (dt_lib_image_t *)malloc(sizeof(dt_lib_image_t));
  self->data = (void *)d;
  self->widget = gtk_grid_new();
  dt_gui_add_help_link(self->widget, "selected_images.html#selected_images_usage");
  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_row_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_homogeneous(grid, TRUE);
  int line = 0;

  GtkWidget *button;

  button = gtk_button_new_with_label(_("remove"));
  ellipsize_button(button);
  d->remove_button = button;
  gtk_widget_set_tooltip_text(button, _("remove from the collection"));
  gtk_grid_attach(grid, button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(0));

  button = gtk_button_new_with_label(_image_get_delete_button_label());
  ellipsize_button(button);
  d->delete_button = button;
  gtk_widget_set_tooltip_text(button, _image_get_delete_button_tooltip());
  gtk_grid_attach(grid, button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(1));


  button = gtk_button_new_with_label(_("move"));
  ellipsize_button(button);
  d->move_button = button;
  gtk_widget_set_tooltip_text(button, _("move to other folder"));
  gtk_grid_attach(grid, button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(8));

  button = gtk_button_new_with_label(_("copy"));
  ellipsize_button(button);
  d->copy_button = button;
  gtk_widget_set_tooltip_text(button, _("copy to other folder"));
  gtk_grid_attach(grid, button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(9));


  button = gtk_button_new_with_label(_("create HDR"));
  ellipsize_button(button);
  d->create_hdr_button = button;
  gtk_grid_attach(grid, button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(7));
  gtk_widget_set_tooltip_text(button, _("create a high dynamic range image from selected shots"));

  button = gtk_button_new_with_label(_("duplicate"));
  ellipsize_button(button);
  d->duplicate_button = button;
  gtk_widget_set_tooltip_text(button, _("add a duplicate to the collection, including its history stack"));
  gtk_grid_attach(grid, button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(3));


  button = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_DO_NOT_USE_BORDER, NULL);
  d->rotate_ccw_button = button;
  gtk_widget_set_tooltip_text(button, _("rotate selected images 90 degrees CCW"));
  gtk_grid_attach(grid, button, 0, line, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(4));

  button = dtgtk_button_new(dtgtk_cairo_paint_refresh, 1 | CPF_DO_NOT_USE_BORDER, NULL);
  d->rotate_cw_button = button;
  gtk_widget_set_tooltip_text(button, _("rotate selected images 90 degrees CW"));
  gtk_grid_attach(grid, button, 1, line, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(5));

  button = gtk_button_new_with_label(_("reset rotation"));
  ellipsize_button(button);
  d->reset_button = button;
  gtk_widget_set_tooltip_text(button, _("reset rotation to EXIF data"));
  gtk_grid_attach(grid, button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(6));


  button = gtk_button_new_with_label(_("copy locally"));
  ellipsize_button(button);
  d->cache_button = button;
  gtk_widget_set_tooltip_text(button, _("copy the image locally"));
  gtk_grid_attach(grid, button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(12));

  button = gtk_button_new_with_label(_("resync local copy"));
  ellipsize_button(button);
  d->uncache_button = button;
  gtk_widget_set_tooltip_text(button, _("synchronize the image's XMP and remove the local copy"));
  gtk_grid_attach(grid, button, 2, line++, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(13));


  button = gtk_button_new_with_label(_("group"));
  ellipsize_button(button);
  d->group_button = button;
  gtk_widget_set_tooltip_text(button, _("add selected images to expanded group or create a new one"));
  gtk_grid_attach(grid, button, 0, line, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(10));

  button = gtk_button_new_with_label(_("ungroup"));
  ellipsize_button(button);
  d->ungroup_button = button;
  gtk_widget_set_tooltip_text(button, _("remove selected images from the group"));
  gtk_grid_attach(grid, button, 2, line, 2, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), GINT_TO_POINTER(11));

  /* connect preference changed signal */
  dt_control_signal_connect(
      darktable.signals,
      DT_SIGNAL_PREFERENCES_CHANGE,
      G_CALLBACK(_image_preference_changed),
      (gpointer)self);
}
#undef ellipsize_button

void gui_cleanup(dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_image_preference_changed), self);

  free(self->data);
  self->data = NULL;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "remove from collection"), GDK_KEY_Delete, 0);
  dt_accel_register_lib(self, NC_("accel", "delete from disk or send to trash"), 0, 0);
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
  dt_accel_connect_button_lib(self, "delete from disk or send to trash", d->delete_button);
  dt_accel_connect_button_lib(self, "rotate selected images 90 degrees CW", d->rotate_cw_button);
  dt_accel_connect_button_lib(self, "rotate selected images 90 degrees CCW", d->rotate_ccw_button);
  dt_accel_connect_button_lib(self, "create HDR", d->create_hdr_button);
  dt_accel_connect_button_lib(self, "duplicate", d->duplicate_button);
  dt_accel_connect_button_lib(self, "reset rotation", d->reset_button);
  // Grouping keys
  dt_accel_connect_button_lib(self, "group", d->group_button);
  dt_accel_connect_button_lib(self, "ungroup", d->ungroup_button);
}

#ifdef USE_LUA
typedef struct {
  const char* key;
  dt_lib_module_t * self;
} lua_callback_data;


static int lua_button_clicked_cb(lua_State* L)
{
  lua_callback_data * data = lua_touserdata(L,1);
  dt_lua_module_entry_push(L,"lib",data->self->plugin_name);
  lua_getuservalue(L,-1);
  lua_getfield(L,-1,"callbacks");
  lua_getfield(L,-1,data->key);
  lua_pushstring(L,data->key);

  GList *image = dt_collection_get_selected(darktable.collection, -1);
  lua_newtable(L);
  while(image)
  {
    luaA_push(L, dt_lua_image_t, &image->data);
    luaL_ref(L, -2);
    image = g_list_delete_link(image, image);
  }

  lua_call(L,2,0);
  return 0;
}

static void lua_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lua_async_call_alien(lua_button_clicked_cb,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"void*", user_data,
      LUA_ASYNC_DONE);
}

static int lua_register_action(lua_State *L)
{
  lua_settop(L,3);
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,-1);
  const char* key = luaL_checkstring(L,1);
  luaL_checktype(L,2,LUA_TFUNCTION);

  lua_getfield(L,-1,"callbacks");
  lua_pushstring(L,key);
  lua_pushvalue(L,2);
  lua_settable(L,-3);

  GtkWidget* button = gtk_button_new_with_label(key);
  const char * tooltip = lua_tostring(L,3);
  if(tooltip)  {
    gtk_widget_set_tooltip_text(button, tooltip);
  }
  gtk_grid_attach_next_to(GTK_GRID(self->widget), button, NULL, GTK_POS_BOTTOM, 4, 1);


  lua_callback_data * data = malloc(sizeof(lua_callback_data));
  data->key = strdup(key);
  data->self = self;
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(lua_button_clicked), data);
  gtk_widget_show_all(button);
  return 0;
}

void init(struct dt_lib_module_t *self)
{

  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_register_action,1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "register_action");

  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,-1);
  lua_newtable(L);
  lua_setfield(L,-2,"callbacks");
  lua_pop(L,2);
}
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
