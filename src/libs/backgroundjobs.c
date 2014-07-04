/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.
    copyright (c) 2014 tobias ellinghaus.

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
#include "control/control.h"
#include "control/conf.h"
#include "control/progress.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "gui/draw.h"

#ifdef USE_LUA
#include "lua/call.h"
#endif

DT_MODULE(1)

typedef struct dt_lib_backgroundjob_element_t
{
  GtkWidget *widget, *progressbar, *hbox;
} dt_lib_backgroundjob_element_t;

/* proxy functions */
static void * _lib_backgroundjobs_added(dt_lib_module_t *self, gboolean has_progress_bar, const gchar *message);
static void   _lib_backgroundjobs_destroyed(dt_lib_module_t * self, dt_lib_backgroundjob_element_t * instance);
static void   _lib_backgroundjobs_cancellable(dt_lib_module_t * self, dt_lib_backgroundjob_element_t * instance, dt_progress_t * progress);
static void   _lib_backgroundjobs_updated(dt_lib_module_t * self, dt_lib_backgroundjob_element_t * instance, double value);


const char* name()
{
  return _("background jobs");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING | DT_VIEW_DARKROOM | DT_VIEW_MAP;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_BOTTOM;
}

int position()
{
  return 1;
}

int expandable()
{
  return 0;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize base */
  self->widget = gtk_vbox_new(FALSE, 0);
  gtk_widget_set_no_show_all(self->widget, TRUE);
  gtk_container_set_border_width(GTK_CONTAINER(self->widget), 5);

  /* setup proxy */
  dt_pthread_mutex_lock(&darktable.control->progress_system.mutex);

  darktable.control->progress_system.proxy.module = self;
  darktable.control->progress_system.proxy.added = _lib_backgroundjobs_added;
  darktable.control->progress_system.proxy.destroyed = _lib_backgroundjobs_destroyed;
  darktable.control->progress_system.proxy.cancellable = _lib_backgroundjobs_cancellable;
  darktable.control->progress_system.proxy.updated = _lib_backgroundjobs_updated;

  // iterate over darktable.control->progress_system.list and add everything that is already there and update its gui_data!
  GList *iter = darktable.control->progress_system.list;
  while(iter)
  {
    dt_progress_t * progress = (dt_progress_t*)iter->data;
    void * gui_data = dt_control_progress_get_gui_data(progress);
    free(gui_data);
    gui_data = _lib_backgroundjobs_added(self, dt_control_progress_has_progress_bar(progress), dt_control_progress_get_message(progress));
    dt_control_progress_set_gui_data(progress, gui_data);
    if(dt_control_progress_cancellable(progress))
      _lib_backgroundjobs_cancellable(self, gui_data, progress);
    _lib_backgroundjobs_updated(self, gui_data, dt_control_progress_get_progress(progress));
    iter = g_list_next(iter);
  }

  dt_pthread_mutex_unlock(&darktable.control->progress_system.mutex);

}

void gui_cleanup(dt_lib_module_t *self)
{
  /* lets kill proxy */
  dt_pthread_mutex_lock(&darktable.control->progress_system.mutex);
  darktable.control->progress_system.proxy.module = NULL;
  darktable.control->progress_system.proxy.added = NULL;
  darktable.control->progress_system.proxy.destroyed = NULL;
  darktable.control->progress_system.proxy.cancellable = NULL;
  darktable.control->progress_system.proxy.updated = NULL;
  dt_pthread_mutex_unlock(&darktable.control->progress_system.mutex);
}

/** the proxy functions */

static void * _lib_backgroundjobs_added(dt_lib_module_t *self, gboolean has_progress_bar, const gchar *message)
{
  // add a new gui thingy
  dt_lib_backgroundjob_element_t *instance = (dt_lib_backgroundjob_element_t*)calloc(1, sizeof(dt_lib_backgroundjob_element_t));
  if(!instance) return NULL;

  /* lets make this threadsafe */
  gboolean i_own_lock = dt_control_gdk_lock();

  instance->widget = gtk_event_box_new();

  /* initialize the ui elements for job */
  gtk_widget_set_name (GTK_WIDGET (instance->widget), "background_job_eventbox");
  GtkBox *vbox = GTK_BOX (gtk_vbox_new (FALSE,0));
  instance->hbox = gtk_hbox_new (FALSE,0);
  gtk_container_set_border_width (GTK_CONTAINER(vbox),2);
  gtk_container_add (GTK_CONTAINER(instance->widget), GTK_WIDGET(vbox));

  /* add job label */
  GtkWidget *label = gtk_label_new(message);
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start( GTK_BOX( instance->hbox ), GTK_WIDGET(label), TRUE, TRUE, 0);
  gtk_box_pack_start( GTK_BOX( vbox ), GTK_WIDGET(instance->hbox), TRUE, TRUE, 0);

  /* use progressbar ? */
  if(has_progress_bar)
  {
    instance->progressbar = gtk_progress_bar_new();
    gtk_box_pack_start( GTK_BOX( vbox ), instance->progressbar, TRUE, FALSE, 2);
  }

  /* lets show jobbox if its hidden */
  gtk_box_pack_start(GTK_BOX(self->widget), instance->widget, TRUE, FALSE, 1);
  gtk_box_reorder_child(GTK_BOX(self->widget), instance->widget, 1);
  gtk_widget_show_all(instance->widget);
  gtk_widget_show(self->widget);

  if(i_own_lock) dt_control_gdk_unlock();

  // return the gui thingy container
  return instance;
}

static void _lib_backgroundjobs_destroyed(dt_lib_module_t * self, dt_lib_backgroundjob_element_t * instance)
{
  // remove the gui that is pointed to in instance
  gboolean i_own_lock = dt_control_gdk_lock();

  /* remove job widget from jobbox */
  if(instance->widget && GTK_IS_WIDGET(instance->widget))
    gtk_container_remove(GTK_CONTAINER(self->widget), instance->widget);
  instance->widget = NULL;

  /* if jobbox is empty lets hide */
  if(g_list_length(gtk_container_get_children(GTK_CONTAINER(self->widget))) == 0)
    gtk_widget_hide(self->widget);

  if(i_own_lock) dt_control_gdk_unlock();

  // free data
  free(instance);
}

static void _lib_backgroundjobs_cancel_callback_new(GtkWidget *w, gpointer user_data)
{
  dt_progress_t *progress = (dt_progress_t *)user_data;
  dt_control_progress_cancel(darktable.control, progress);
}

static void _lib_backgroundjobs_cancellable(dt_lib_module_t * self, dt_lib_backgroundjob_element_t * instance, dt_progress_t * progress)
{
  // add a cancel button to the gui. when clicked we want dt_control_progress_cancel(darktable.control, progress); to be called
  if(!darktable.control->running) return;
  gboolean i_own_lock = dt_control_gdk_lock();

  GtkBox *hbox = GTK_BOX(instance->hbox);
  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_cancel,CPF_STYLE_FLAT);
  gtk_widget_set_size_request(button, DT_PIXEL_APPLY_DPI(17), DT_PIXEL_APPLY_DPI(17));
  g_signal_connect(G_OBJECT (button), "clicked", G_CALLBACK(_lib_backgroundjobs_cancel_callback_new), progress);
  gtk_box_pack_start(hbox, GTK_WIDGET(button), FALSE, FALSE, 0);
  gtk_widget_show_all(button);

  if(i_own_lock) dt_control_gdk_unlock();
}

static void _lib_backgroundjobs_updated(dt_lib_module_t * self, dt_lib_backgroundjob_element_t * instance, double value)
{
  // update the progress bar
  if(!darktable.control->running) return;
  gboolean i_own_lock = dt_control_gdk_lock();

  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(instance->progressbar), CLAMP(value, 0, 1.0));

  if(i_own_lock) dt_control_gdk_unlock();
}

////////////////////////////////////////////////////////////////////////
// TODO: everything below this line should move to src/lua/progress.c //
////////////////////////////////////////////////////////////////////////

#ifdef USE_LUA

typedef dt_progress_t* dt_lua_backgroundjob_t;

static int32_t lua_job_canceled_job(dt_job_t *job)
{
  dt_progress_t *progress = dt_control_job_get_params(job);
  lua_State * L = darktable.lua_state.state;
  gboolean has_lock = dt_lua_lock();
  luaA_push(L, dt_lua_backgroundjob_t, &progress);
  lua_getuservalue(L, -1);
  lua_getfield(L, -1, "cancel_callback");
  lua_pushvalue(L, -3);
  dt_lua_do_chunk(L, 1, 0);
  lua_pop(L, 2);
  dt_lua_unlock(has_lock);
  return 0;
}

static void lua_job_cancelled(dt_progress_t *progress, gpointer user_data)
{
  dt_job_t *job = dt_control_job_create(&lua_job_canceled_job, "lua: on background cancel");
  if(!job) return;
  dt_control_job_set_params(job, progress);
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_BG, job);
}

static int lua_create_job(lua_State *L)
{
  dt_lib_module_t*self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lua_lib_check_error(L, self);
  const char * message = luaL_checkstring(L, 1);
  gboolean has_progress_bar = lua_toboolean(L, 2);
  int cancellable = FALSE;
  if(!lua_isnoneornil(L,3))
  {
    luaL_checktype(L, 3, LUA_TFUNCTION);
    cancellable = TRUE;
  }
  dt_lua_unlock(false);
  dt_progress_t *progress = dt_control_progress_create(darktable.control, has_progress_bar, message);
  if(cancellable)
  {
    dt_control_progress_make_cancellable(darktable.control, progress, lua_job_cancelled, progress);
  }
  dt_lua_lock();
  luaA_push(L, dt_lua_backgroundjob_t, &progress);
  if(cancellable)
  {
    lua_getuservalue(L, -1);
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, "cancel_callback");
    lua_pop(L, 1);
  }
  return 1;
}

static int lua_job_progress(lua_State *L)
{
  dt_lib_module_t*self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lua_lib_check_error(L, self);
  dt_progress_t *progress;
  luaA_to(L, dt_lua_backgroundjob_t, &progress, 1);
  if(lua_isnone(L, 3))
  {
    dt_lua_unlock(false);
    double result = dt_control_progress_get_progress(progress);
    dt_lua_lock();
    if(!dt_control_progress_has_progress_bar(progress))
      lua_pushnil(L);
    else
      lua_pushnumber(L, result);
    return 1;
  }
  else
  {
    double value = luaL_checknumber(L, 3);
    if( value < 0.0 || value > 1.0)
      return luaL_argerror(L, 3, "incorrect value for job percentage (between 0 and 1)");
    dt_lua_unlock(false);
    dt_control_progress_set_progress(darktable.control, progress, value);
    dt_lua_lock();
    return 0;
  }
}

static int lua_job_valid(lua_State*L)
{
  dt_lib_module_t*self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lua_lib_check_error(L, self);
  dt_progress_t *progress;
  luaA_to(L, dt_lua_backgroundjob_t, &progress, 1);
  if(lua_isnone(L, 3))
  {
    dt_lua_unlock(false);
    gboolean i_own_lock = dt_control_gdk_lock();
    dt_pthread_mutex_lock(&darktable.control->progress_system.mutex);
    GList *iter = g_list_find(darktable.control->progress_system.list, progress);
    dt_pthread_mutex_unlock(&darktable.control->progress_system.mutex);
    if(i_own_lock) dt_control_gdk_unlock();
    dt_lua_lock();

    if(iter)
      lua_pushboolean(L, true);
    else
      lua_pushboolean(L, false);

    return 1;
  }
  else
  {
    int validity = lua_toboolean(L, 3);
    if(validity)
      return luaL_argerror(L, 3, "a job can not be made valid");
    dt_lua_unlock(false);
    dt_control_progress_destroy(darktable.control, progress);
    dt_lua_lock();
    return 0;
  }
}

#endif //USE_LUA

void init(struct dt_lib_module_t *self)
{
#ifdef USE_LUA
  lua_State *L = darktable.lua_state.state;
  int my_typeid = dt_lua_module_get_entry_typeid(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_create_job, 1);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_typeid(L, my_typeid, "create_job");

  // create a type describing a job object
  int job_typeid = dt_lua_init_gpointer_type(L, dt_lua_backgroundjob_t);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_job_progress, 1);
  dt_lua_type_register_typeid(L, job_typeid, "percent");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_job_valid, 1);
  dt_lua_type_register_typeid(L, job_typeid, "valid");
#endif //USE_LUA
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
