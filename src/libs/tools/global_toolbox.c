/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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
#include "common/collection.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/preferences.h"
#include "dtgtk/button.h"
#include "dtgtk/togglebutton.h"
#include "control/conf.h"
#include "control/control.h"

DT_MODULE(1)

typedef struct dt_lib_tool_preferences_t
{
  GtkWidget *preferences_button, *grouping_button, *overlays_button;
} dt_lib_tool_preferences_t;

/* callback for grouping button */
static void _lib_filter_grouping_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for preference button */
static void _lib_preferences_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for overlays button */
static void _lib_overlays_button_clicked(GtkWidget *widget, gpointer user_data);

const char *name()
{
  return _("preferences");
}

uint32_t views()
{
  return DT_VIEW_DARKROOM | DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING | DT_VIEW_MAP;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT;
}

int expandable()
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
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)g_malloc0(sizeof(dt_lib_tool_preferences_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

  /* create the grouping button */
  d->grouping_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_grouping, CPF_STYLE_FLAT);
  gtk_widget_set_size_request(d->grouping_button, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_box_pack_start(GTK_BOX(self->widget), d->grouping_button, FALSE, FALSE, 2);
  if(darktable.gui->grouping)
    g_object_set(G_OBJECT(d->grouping_button), "tooltip-text", _("expand grouped images"), (char *)NULL);
  else
    g_object_set(G_OBJECT(d->grouping_button), "tooltip-text", _("collapse grouped images"), (char *)NULL);
  g_signal_connect(G_OBJECT(d->grouping_button), "clicked", G_CALLBACK(_lib_filter_grouping_button_clicked),
                   NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->grouping_button), darktable.gui->grouping);

  /* create the "show/hide overlays" button */
  d->overlays_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_overlays, CPF_STYLE_FLAT);
  gtk_widget_set_size_request(d->overlays_button, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_box_pack_start(GTK_BOX(self->widget), d->overlays_button, FALSE, FALSE, 2);
  if(darktable.gui->show_overlays)
    g_object_set(G_OBJECT(d->overlays_button), "tooltip-text", _("hide image overlays"), (char *)NULL);
  else
    g_object_set(G_OBJECT(d->overlays_button), "tooltip-text", _("show image overlays"), (char *)NULL);
  g_signal_connect(G_OBJECT(d->overlays_button), "clicked", G_CALLBACK(_lib_overlays_button_clicked), NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->overlays_button), darktable.gui->show_overlays);

  /* create the preference button */
  d->preferences_button = dtgtk_button_new(dtgtk_cairo_paint_preferences, CPF_STYLE_FLAT);
  gtk_widget_set_size_request(d->preferences_button, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_box_pack_end(GTK_BOX(self->widget), d->preferences_button, FALSE, FALSE, 2);
  g_object_set(G_OBJECT(d->preferences_button), "tooltip-text", _("show global preferences"), (char *)NULL);
  g_signal_connect(G_OBJECT(d->preferences_button), "clicked", G_CALLBACK(_lib_preferences_button_clicked),
                   NULL);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

#ifdef USE_LUA

typedef struct
{
  gboolean toggle;
  gchar *event_name;
} button_clicked_callback_data_t;

static int32_t _button_clicked_callback_job(dt_job_t *job)
{
  dt_lua_lock();
  button_clicked_callback_data_t *t = dt_control_job_get_params(job);
  lua_pushboolean(darktable.lua_state.state, t->toggle);
  dt_lua_event_trigger(darktable.lua_state.state, t->event_name, 1);
  g_free(t->event_name);
  free(t);
  dt_lua_unlock();

  return 0;
}

#endif // USE_LUA

void _lib_preferences_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_gui_preferences_show();
}

static void _lib_filter_grouping_button_clicked(GtkWidget *widget, gpointer user_data)
{

  darktable.gui->grouping = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(darktable.gui->grouping)
    g_object_set(G_OBJECT(widget), "tooltip-text", _("expand grouped images"), (char *)NULL);
  else
    g_object_set(G_OBJECT(widget), "tooltip-text", _("collapse grouped images"), (char *)NULL);
  dt_conf_set_bool("ui_last/grouping", darktable.gui->grouping);
  darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection);

#ifdef USE_LUA

  dt_job_t *job = dt_control_job_create(&_button_clicked_callback_job, "lua: grouping button toggled");
  if(job)
  {
    button_clicked_callback_data_t *t
        = (button_clicked_callback_data_t *)calloc(1, sizeof(button_clicked_callback_data_t));
    if(!t)
    {
      dt_control_job_dispose(job);
    }
    else
    {
      dt_control_job_set_params(job, t);
      t->toggle = darktable.gui->grouping;
      t->event_name = g_strdup("global_toolbox-grouping_toggle");
      dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
    }
  }

#endif // USE_LUA
}

static void _lib_overlays_button_clicked(GtkWidget *widget, gpointer user_data)
{
  darktable.gui->show_overlays = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(darktable.gui->show_overlays)
    g_object_set(G_OBJECT(widget), "tooltip-text", _("hide image overlays"), (char *)NULL);
  else
    g_object_set(G_OBJECT(widget), "tooltip-text", _("show image overlays"), (char *)NULL);
  dt_conf_set_bool("lighttable/ui/expose_statuses", darktable.gui->show_overlays);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED);

#ifdef USE_LUA

  dt_job_t *job = dt_control_job_create(&_button_clicked_callback_job, "lua: overlay button toggled");
  if(job)
  {
    button_clicked_callback_data_t *t
        = (button_clicked_callback_data_t *)calloc(1, sizeof(button_clicked_callback_data_t));
    if(!t)
    {
      dt_control_job_dispose(job);
    }
    else
    {
      dt_control_job_set_params(job, t);
      t->toggle = darktable.gui->show_overlays;
      t->event_name = g_strdup("global_toolbox-overlay_toggle");
      dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
    }
  }

#endif // USE_LUA
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "grouping"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "preferences"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "show overlays"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)self->data;

  dt_accel_connect_button_lib(self, "grouping", d->grouping_button);
  dt_accel_connect_button_lib(self, "preferences", d->preferences_button);
  dt_accel_connect_button_lib(self, "show overlays", d->overlays_button);
}

#ifdef USE_LUA

static int grouping_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)self->data;
  dt_lua_lib_check_error(L, self);
  if(lua_gettop(L) != 3)
  {
    lua_pushboolean(L, darktable.gui->grouping);
    return 1;
  }
  else
  {
    gboolean value = lua_toboolean(L, 3);
    if(darktable.gui->grouping != value)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->grouping_button), value);
    }
  }
  return 0;
}

static int show_overlays_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)self->data;
  dt_lua_lib_check_error(L, self);
  if(lua_gettop(L) != 3)
  {
    lua_pushboolean(L, darktable.gui->show_overlays);
    return 1;
  }
  else
  {
    gboolean value = lua_toboolean(L, 3);
    if(darktable.gui->show_overlays != value)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->overlays_button), value);
    }
  }
  return 0;
}

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);

  lua_pushcfunction(L, grouping_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register_type(L, my_type, "grouping");
  lua_pushcfunction(L, show_overlays_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register_type(L, my_type, "show_overlays");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "global_toolbox-grouping_toggle");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "global_toolbox-overlay_toggle");
}

#endif // USE_LUA

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
