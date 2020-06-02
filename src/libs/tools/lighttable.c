/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

#include <gdk/gdkkeysyms.h>

#include "common/collection.h"
#include "common/debug.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_tool_lighttable_t
{
  GtkWidget *zoom;
  GtkWidget *zoom_entry;
  GtkWidget *layout_combo;
  GtkWidget *zoom_mode_cb;
  dt_lighttable_layout_t layout, base_layout;
  int current_zoom;
  dt_lighttable_culling_zoom_mode_t zoom_mode;
  gboolean combo_evt_reset;
} dt_lib_tool_lighttable_t;

/* set zoom proxy function */
static void _lib_lighttable_set_zoom(dt_lib_module_t *self, gint zoom);
static gint _lib_lighttable_get_zoom(dt_lib_module_t *self);
static dt_lighttable_culling_zoom_mode_t _lib_lighttable_get_zoom_mode(dt_lib_module_t *self);

/* get/set layout proxy function */
static dt_lighttable_layout_t _lib_lighttable_get_layout(dt_lib_module_t *self);
static void _lib_lighttable_set_layout(dt_lib_module_t *self, dt_lighttable_layout_t layout);

/* lightable layout changed */
static void _lib_lighttable_layout_changed(GtkComboBox *widget, gpointer user_data);
/* lightable culling zoom mode changed */
static void _lib_lighttable_zoom_mode_changed(GtkComboBox *widget, gpointer user_data);
/* zoom slider change callback */
static void _lib_lighttable_zoom_slider_changed(GtkRange *range, gpointer user_data);
/* zoom entry change callback */
static gboolean _lib_lighttable_zoom_entry_changed(GtkWidget *entry, GdkEventKey *event,
                                                   dt_lib_module_t *self);

/* zoom key accel callback */
static gboolean _lib_lighttable_key_accel_toggle_culling_dynamic_mode(GtkAccelGroup *accel_group,
                                                                      GObject *acceleratable, guint keyval,
                                                                      GdkModifierType modifier, gpointer data);
static gboolean _lib_lighttable_key_accel_toggle_culling_mode(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                              guint keyval, GdkModifierType modifier,
                                                              gpointer data);
static gboolean _lib_lighttable_key_accel_toggle_culling_zoom_mode(GtkAccelGroup *accel_group,
                                                                   GObject *acceleratable, guint keyval,
                                                                   GdkModifierType modifier, gpointer data);
static void _set_zoom(dt_lib_module_t *self, int zoom);

const char *name(dt_lib_module_t *self)
{
  return _("lighttable");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)g_malloc0(sizeof(dt_lib_tool_lighttable_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  d->layout = MIN(DT_LIGHTTABLE_LAYOUT_LAST - 1, dt_conf_get_int("plugins/lighttable/layout"));
  d->base_layout = MIN(DT_LIGHTTABLE_LAYOUT_LAST - 1, dt_conf_get_int("plugins/lighttable/base_layout"));

  if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    d->zoom_mode = dt_conf_get_int("plugins/lighttable/culling_zoom_mode");
    if(d->zoom_mode == DT_LIGHTTABLE_ZOOM_DYNAMIC && darktable.collection)
    {
      d->current_zoom = MAX(1, MIN(DT_LIGHTTABLE_MAX_ZOOM, dt_collection_get_selected_count(darktable.collection)));
      if(d->current_zoom == 1) d->current_zoom = dt_conf_get_int("plugins/lighttable/culling_num_images");
    }
    else
    {
      d->current_zoom = dt_conf_get_int("plugins/lighttable/culling_num_images");
    }
  }
  else
    d->current_zoom = dt_conf_get_int("plugins/lighttable/images_in_row");

  /* create layout selection combobox */
  d->layout_combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->layout_combo), _("zoomable light table"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->layout_combo), _("file manager"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->layout_combo), _("culling"));

  gtk_combo_box_set_active(GTK_COMBO_BOX(d->layout_combo), d->layout);

  g_signal_connect(G_OBJECT(d->layout_combo), "changed", G_CALLBACK(_lib_lighttable_layout_changed), (gpointer)self);

  gtk_box_pack_start(GTK_BOX(self->widget), d->layout_combo, TRUE, TRUE, 0);


  /* create horizontal zoom slider */
  d->zoom = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, DT_LIGHTTABLE_MAX_ZOOM, 1);
  gtk_widget_set_size_request(GTK_WIDGET(d->zoom), DT_PIXEL_APPLY_DPI(140), -1);
  gtk_scale_set_draw_value(GTK_SCALE(d->zoom), FALSE);
  gtk_range_set_increments(GTK_RANGE(d->zoom), 1, 1);
  gtk_box_pack_start(GTK_BOX(self->widget), d->zoom, TRUE, TRUE, 0);

  /* manual entry of the zoom level */
  d->zoom_entry = gtk_entry_new();
  gtk_entry_set_alignment(GTK_ENTRY(d->zoom_entry), 1.0);
  gtk_entry_set_max_length(GTK_ENTRY(d->zoom_entry), 2);
  gtk_entry_set_width_chars(GTK_ENTRY(d->zoom_entry), 3);
  gtk_entry_set_max_width_chars(GTK_ENTRY(d->zoom_entry), 3);
  dt_gui_key_accel_block_on_focus_connect(d->zoom_entry);
  gtk_box_pack_start(GTK_BOX(self->widget), d->zoom_entry, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(d->zoom), "value-changed", G_CALLBACK(_lib_lighttable_zoom_slider_changed),
                   (gpointer)self);
  g_signal_connect(d->zoom_entry, "key-press-event", G_CALLBACK(_lib_lighttable_zoom_entry_changed), self);
  gtk_range_set_value(GTK_RANGE(d->zoom), d->current_zoom);

  d->zoom_mode_cb = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->zoom_mode_cb), _("fixed zoom"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->zoom_mode_cb), _("dynamic zoom"));
  g_signal_connect(G_OBJECT(d->zoom_mode_cb), "changed", G_CALLBACK(_lib_lighttable_zoom_mode_changed),
                   (gpointer)self);
  gtk_box_pack_start(GTK_BOX(self->widget), d->zoom_mode_cb, TRUE, TRUE, 0);
  if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    gtk_widget_show(d->zoom_mode_cb);
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->zoom_mode_cb), d->zoom_mode == DT_LIGHTTABLE_ZOOM_DYNAMIC);
  }
  else
  {
    gtk_widget_hide(d->zoom_mode_cb);
  }
  gtk_widget_set_no_show_all(d->zoom_mode_cb, TRUE);

  _lib_lighttable_zoom_slider_changed(GTK_RANGE(d->zoom), self); // the slider defaults to 1 and GTK doesn't
                                                                 // fire a value-changed signal when setting
                                                                 // it to 1 => empty text box

  _lib_lighttable_layout_changed(GTK_COMBO_BOX(d->layout_combo), self);

  gtk_widget_set_sensitive(
      d->zoom_entry, (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING || d->zoom_mode != DT_LIGHTTABLE_ZOOM_DYNAMIC));
  gtk_widget_set_sensitive(
      d->zoom, (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING || d->zoom_mode != DT_LIGHTTABLE_ZOOM_DYNAMIC));

  darktable.view_manager->proxy.lighttable.module = self;
  darktable.view_manager->proxy.lighttable.set_zoom = _lib_lighttable_set_zoom;
  darktable.view_manager->proxy.lighttable.get_zoom = _lib_lighttable_get_zoom;
  darktable.view_manager->proxy.lighttable.get_zoom_mode = _lib_lighttable_get_zoom_mode;
  darktable.view_manager->proxy.lighttable.get_layout = _lib_lighttable_get_layout;
  darktable.view_manager->proxy.lighttable.set_layout = _lib_lighttable_set_layout;
}

void init_key_accels(dt_lib_module_t *self)
{
  // view accels
  dt_accel_register_lib_as_view("lighttable", NC_("accel", "toggle culling mode"), GDK_KEY_x, 0);
  dt_accel_register_lib_as_view("lighttable", NC_("accel", "toggle culling dynamic mode"), GDK_KEY_x, GDK_CONTROL_MASK);
  dt_accel_register_lib_as_view("lighttable", NC_("accel", "toggle culling zoom mode"), GDK_KEY_less, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  /* setup key accelerators */

  // view accels
  dt_accel_connect_lib_as_view(
      self,"lighttable", "toggle culling dynamic mode",
      g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_toggle_culling_dynamic_mode), self, NULL));
  dt_accel_connect_lib_as_view(self,"lighttable", "toggle culling mode",
                       g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_toggle_culling_mode), self, NULL));
  dt_accel_connect_lib_as_view(self,"lighttable", "toggle culling zoom mode",
                       g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_toggle_culling_zoom_mode), self, NULL));
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  dt_gui_key_accel_block_on_focus_disconnect(d->zoom_entry);
  g_free(self->data);
  self->data = NULL;
}

static void _set_zoom(dt_lib_module_t *self, int zoom)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    if(d->zoom_mode == DT_LIGHTTABLE_ZOOM_FIXED) dt_conf_set_int("plugins/lighttable/culling_num_images", zoom);
  }
  else
    dt_conf_set_int("plugins/lighttable/images_in_row", zoom);

  if(d->layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER || d->layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
  {
    dt_thumbtable_zoom_changed(dt_ui_thumbtable(darktable.gui->ui), d->current_zoom, zoom);
  }
}

static void _lib_lighttable_zoom_slider_changed(GtkRange *range, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  const int i = gtk_range_get_value(range);
  _set_zoom(self, i);
  gchar *i_as_str = g_strdup_printf("%d", i);
  gtk_entry_set_text(GTK_ENTRY(d->zoom_entry), i_as_str);
  d->current_zoom = i;
  g_free(i_as_str);
  dt_control_queue_redraw_center();
}

static gboolean _lib_lighttable_zoom_entry_changed(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  switch(event->keyval)
  {
    case GDK_KEY_Escape:
    case GDK_KEY_Tab:
    {
      // reset
      int i = 0;
      if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
        i = dt_conf_get_int("plugins/lighttable/culling_num_images");
      else
        i = dt_conf_get_int("plugins/lighttable/images_in_row");
      gchar *i_as_str = g_strdup_printf("%d", i);
      gtk_entry_set_text(GTK_ENTRY(d->zoom_entry), i_as_str);
      g_free(i_as_str);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      // apply zoom level
      const gchar *value = gtk_entry_get_text(GTK_ENTRY(d->zoom_entry));
      int i = atoi(value);
      gtk_range_set_value(GTK_RANGE(d->zoom), i);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }

    // allow 0 .. 9, left/right movement using arrow keys and del/backspace
    case GDK_KEY_0:
    case GDK_KEY_KP_0:
    case GDK_KEY_1:
    case GDK_KEY_KP_1:
    case GDK_KEY_2:
    case GDK_KEY_KP_2:
    case GDK_KEY_3:
    case GDK_KEY_KP_3:
    case GDK_KEY_4:
    case GDK_KEY_KP_4:
    case GDK_KEY_5:
    case GDK_KEY_KP_5:
    case GDK_KEY_6:
    case GDK_KEY_KP_6:
    case GDK_KEY_7:
    case GDK_KEY_KP_7:
    case GDK_KEY_8:
    case GDK_KEY_KP_8:
    case GDK_KEY_9:
    case GDK_KEY_KP_9:

    case GDK_KEY_Left:
    case GDK_KEY_Right:
    case GDK_KEY_Delete:
    case GDK_KEY_BackSpace:
      return FALSE;

    default: // block everything else
      return TRUE;
  }
}

static void _lib_lighttable_change_layout(dt_lib_module_t *self, dt_lighttable_layout_t layout)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  const int current_layout = dt_conf_get_int("plugins/lighttable/layout");
  d->layout = layout;

  if(current_layout != layout)
  {
    if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    {
      gtk_widget_show(d->zoom_mode_cb);
      if(d->zoom_mode == DT_LIGHTTABLE_ZOOM_DYNAMIC)
      {
        d->current_zoom = MAX(1, MIN(30, dt_collection_get_selected_count(darktable.collection)));
        if(d->current_zoom == 1) d->current_zoom = dt_conf_get_int("plugins/lighttable/culling_num_images");
      }
      else
      {
        d->current_zoom = dt_conf_get_int("plugins/lighttable/culling_num_images");
      }
      gtk_combo_box_set_active(GTK_COMBO_BOX(d->zoom_mode_cb), d->zoom_mode == DT_LIGHTTABLE_ZOOM_DYNAMIC);
    }
    else
    {
      gtk_widget_hide(d->zoom_mode_cb);
      d->zoom_mode = DT_LIGHTTABLE_ZOOM_FIXED;
      d->current_zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
    }
    gtk_widget_set_sensitive(
        d->zoom_entry, (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING || d->zoom_mode != DT_LIGHTTABLE_ZOOM_DYNAMIC));
    gtk_widget_set_sensitive(
        d->zoom, (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING || d->zoom_mode != DT_LIGHTTABLE_ZOOM_DYNAMIC));
    gtk_range_set_value(GTK_RANGE(d->zoom), d->current_zoom);

    dt_conf_set_int("plugins/lighttable/layout", layout);
    if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER || layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
    {
      d->base_layout = layout;
      dt_conf_set_int("plugins/lighttable/base_layout", layout);
    }
    d->combo_evt_reset = TRUE;
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->layout_combo), layout);
    d->combo_evt_reset = FALSE;
    dt_control_queue_redraw_center();
  }
  else
  {
    dt_control_queue_redraw_center();
  }
}

static void _lib_lighttable_layout_changed(GtkComboBox *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  if(d->combo_evt_reset) return;
  const int new_layout = gtk_combo_box_get_active(widget);
  _lib_lighttable_change_layout(self, new_layout);
}

static void _lib_lighttable_zoom_mode_changed(GtkComboBox *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  dt_lighttable_culling_zoom_mode_t new_mode = DT_LIGHTTABLE_ZOOM_FIXED;
  if(gtk_combo_box_get_active(GTK_COMBO_BOX(d->zoom_mode_cb)) == 1) new_mode = DT_LIGHTTABLE_ZOOM_DYNAMIC;
  if(new_mode == d->zoom_mode) return;

  d->zoom_mode = new_mode;
  if(new_mode == DT_LIGHTTABLE_ZOOM_FIXED)
  {
    _lib_lighttable_set_zoom(self, dt_conf_get_int("plugins/lighttable/culling_num_images"));
  }
  else
  {
    int selnb = dt_collection_get_selected_count(darktable.collection);
    if(selnb <= 1) selnb = dt_conf_get_int("plugins/lighttable/culling_num_images");
    _lib_lighttable_set_zoom(self, selnb);
    // and we set the offset to the first selected image
    if(selnb != 0)
    {
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT m.imgid FROM memory.collected_images as m, main.selected_images as s "
                                  "WHERE m.imgid=s.imgid "
                                  "ORDER BY m.rowid LIMIT 1",
                                  -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        dt_view_lighttable_change_offset(darktable.view_manager, FALSE, sqlite3_column_int(stmt, 0));
      }
      sqlite3_finalize(stmt);
    }
  }
  dt_view_lighttable_culling_init_mode(darktable.view_manager);

  gtk_widget_set_sensitive(
      d->zoom_entry, (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING || d->zoom_mode != DT_LIGHTTABLE_ZOOM_DYNAMIC));
  gtk_widget_set_sensitive(
      d->zoom, (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING || d->zoom_mode != DT_LIGHTTABLE_ZOOM_DYNAMIC));

  if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    dt_conf_set_int("plugins/lighttable/culling_zoom_mode", d->zoom_mode);
  }
}

static void _lib_lighttable_set_layout(dt_lib_module_t *self, dt_lighttable_layout_t layout)
{
  _lib_lighttable_change_layout(self, layout);
}

static dt_lighttable_layout_t _lib_lighttable_get_layout(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  return d->layout;
}

static void _lib_lighttable_set_zoom(dt_lib_module_t *self, gint zoom)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  gtk_range_set_value(GTK_RANGE(d->zoom), zoom);
  d->current_zoom = zoom;
}

static gint _lib_lighttable_get_zoom(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  return d->current_zoom;
}

static dt_lighttable_culling_zoom_mode_t _lib_lighttable_get_zoom_mode(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  return d->zoom_mode;
}

static gboolean _lib_lighttable_key_accel_toggle_culling_dynamic_mode(GtkAccelGroup *accel_group,
                                                                      GObject *acceleratable, guint keyval,
                                                                      GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  if(d->layout != DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    d->zoom_mode = DT_LIGHTTABLE_ZOOM_DYNAMIC;
    dt_conf_set_int("plugins/lighttable/culling_zoom_mode", d->zoom_mode);
    _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING);
  }
  else
  {
    d->zoom_mode = DT_LIGHTTABLE_ZOOM_FIXED;
    _lib_lighttable_set_layout(self, d->base_layout);
  }

  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean _lib_lighttable_key_accel_toggle_culling_mode(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                              guint keyval, GdkModifierType modifier,
                                                              gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  d->zoom_mode = DT_LIGHTTABLE_ZOOM_FIXED;
  if(d->layout != DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    dt_conf_set_int("plugins/lighttable/culling_zoom_mode", d->zoom_mode);
    _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING);
  }
  else
    _lib_lighttable_set_layout(self, d->base_layout);

  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean _lib_lighttable_key_accel_toggle_culling_zoom_mode(GtkAccelGroup *accel_group,
                                                                   GObject *acceleratable, guint keyval,
                                                                   GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  if(d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->zoom_mode_cb), d->zoom_mode == DT_LIGHTTABLE_ZOOM_FIXED);
  return TRUE;
}

#ifdef USE_LUA
static int layout_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  const dt_lighttable_layout_t tmp = _lib_lighttable_get_layout(self);
  if(lua_gettop(L) > 0){
    dt_lighttable_layout_t value;
    luaA_to(L,dt_lighttable_layout_t,&value,1);
    _lib_lighttable_set_layout(self, value);
  }
  luaA_push(L, dt_lighttable_layout_t, &tmp);
  return 1;
}
static int zoom_level_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  const gint tmp = _lib_lighttable_get_zoom(self);
  if(lua_gettop(L) > 0){
    int value;
    luaA_to(L,int,&value,1);
    _lib_lighttable_set_zoom(self, value);
  }
  luaA_push(L, int, &tmp);
  return 1;
}

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, layout_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "layout");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, zoom_level_cb,1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "zoom_level");

  luaA_enum(L,dt_lighttable_layout_t);
  luaA_enum_value(L,dt_lighttable_layout_t,DT_LIGHTTABLE_LAYOUT_FIRST);
  luaA_enum_value(L,dt_lighttable_layout_t,DT_LIGHTTABLE_LAYOUT_ZOOMABLE);
  luaA_enum_value(L,dt_lighttable_layout_t,DT_LIGHTTABLE_LAYOUT_FILEMANAGER);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_CULLING);
  luaA_enum_value(L,dt_lighttable_layout_t,DT_LIGHTTABLE_LAYOUT_LAST);
}
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
