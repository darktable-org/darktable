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
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "gui/draw.h"

#ifdef HAVE_UNITY
#  include <unity/unity/unity.h>
#endif
#ifdef MAC_INTEGRATION
#   include <gtkosxapplication.h>
#endif
#ifdef USE_LUA
#include "lua/call.h"
#endif

DT_MODULE(1)

#define DT_MODULE_LIST_SPACING 2

GStaticMutex _lib_backgroundjobs_mutex = G_STATIC_MUTEX_INIT;

typedef struct dt_bgjob_t
{
  uint32_t type;
  GtkWidget *widget,*progressbar,*label;
#ifdef HAVE_UNITY
  UnityLauncherEntry *darktable_launcher;
#endif
} dt_bgjob_t;

typedef struct dt_lib_backgroundjobs_t
{
  GtkWidget *jobbox;
  GHashTable *jobs;
}
dt_lib_backgroundjobs_t;

/* proxy function for creating a ui bgjob plate */
static const guint *_lib_backgroundjobs_create(dt_lib_module_t *self,int type,const gchar *message);
/* proxy function for destroying a ui bgjob plate */
static void _lib_backgroundjobs_destroy(dt_lib_module_t *self, const guint *key);
/* proxy function for assigning and set cancel job for a ui bgjob plate*/
static void _lib_backgroundjobs_set_cancellable(dt_lib_module_t *self, const guint *key, struct dt_job_t *job);
/* proxy function for setting the progress of a ui bgjob plate */
static void _lib_backgroundjobs_progress(dt_lib_module_t *self, const guint *key, double progress);
#ifdef USE_LUA
/* function for getting the progress of a ui bgjob plate */
static double _lib_backgroundjobs_get_progress(dt_lib_module_t *self, const guint *key);
#endif
/* callback when cancel job button is pushed  */
static void _lib_backgroundjobs_cancel_callback(GtkWidget *w, gpointer user_data);

const char* name()
{
  return _("background jobs");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING | DT_VIEW_DARKROOM;
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
  /* initialize ui widgets */
  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t *)g_malloc(sizeof(dt_lib_backgroundjobs_t));
  memset(d,0,sizeof(dt_lib_backgroundjobs_t));
  self->data = (void *)d;

  d->jobs = g_hash_table_new(g_direct_hash,g_direct_equal);

  /* initialize base */
  self->widget = d->jobbox = gtk_vbox_new(FALSE, 0);
  gtk_widget_set_no_show_all(self->widget, TRUE);
  gtk_container_set_border_width(GTK_CONTAINER(self->widget), 5);

  /* setup proxy */
  darktable.control->proxy.backgroundjobs.module = self;
  darktable.control->proxy.backgroundjobs.create = _lib_backgroundjobs_create;
  darktable.control->proxy.backgroundjobs.destroy = _lib_backgroundjobs_destroy;
  darktable.control->proxy.backgroundjobs.progress = _lib_backgroundjobs_progress;
  darktable.control->proxy.backgroundjobs.set_cancellable = _lib_backgroundjobs_set_cancellable;
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* lets kill proxy */
  darktable.control->proxy.backgroundjobs.module = NULL;

  g_free(self->data);
  self->data = NULL;
}

static const guint * _lib_backgroundjobs_create(dt_lib_module_t *self,int type,const gchar *message)
{
  /* lets make this threadsafe */
  gboolean i_own_lock = dt_control_gdk_lock();

  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t *)self->data;

  /* initialize a new job */
  dt_bgjob_t *j=(dt_bgjob_t*)g_malloc(sizeof(dt_bgjob_t));
  j->type = type;
  j->widget = gtk_event_box_new();

  guint *key = g_malloc(sizeof(guint));
  *key = g_direct_hash((gconstpointer)j);

  /* create in hash out of j pointer*/
  g_hash_table_insert(d->jobs, key, j);

  /* initialize the ui elements for job */
  gtk_widget_set_name (GTK_WIDGET (j->widget), "background_job_eventbox");
  GtkBox *vbox = GTK_BOX (gtk_vbox_new (FALSE,0));
  GtkBox *hbox = GTK_BOX (gtk_hbox_new (FALSE,0));
  gtk_container_set_border_width (GTK_CONTAINER(vbox),2);
  gtk_container_add (GTK_CONTAINER(j->widget), GTK_WIDGET(vbox));

  /* add job label */
  j->label = gtk_label_new(message);
  gtk_misc_set_alignment(GTK_MISC(j->label), 0.0, 0.5);
  gtk_box_pack_start( GTK_BOX( hbox ), GTK_WIDGET(j->label), TRUE, TRUE, 0);
  gtk_box_pack_start( GTK_BOX( vbox ), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  /* use progressbar ? */
  if (type == 0)
  {
    j->progressbar = gtk_progress_bar_new();
    gtk_box_pack_start( GTK_BOX( vbox ), j->progressbar, TRUE, FALSE, 2);

#ifdef HAVE_UNITY
    j->darktable_launcher = unity_launcher_entry_get_for_desktop_id("darktable.desktop");
    unity_launcher_entry_set_progress( j->darktable_launcher, 0.0 );
    unity_launcher_entry_set_progress_visible( j->darktable_launcher, TRUE );
#endif
  }

  /* lets show jobbox if its hidden */
  gtk_box_pack_start(GTK_BOX(d->jobbox), j->widget, TRUE, FALSE, 1);
  gtk_box_reorder_child(GTK_BOX(d->jobbox), j->widget, 1);
  gtk_widget_show_all(j->widget);
  gtk_widget_show(d->jobbox);

  if(i_own_lock) dt_control_gdk_unlock();
  return key;
}

static void _lib_backgroundjobs_destroy(dt_lib_module_t *self, const guint *key)
{
  gboolean i_own_lock = dt_control_gdk_lock();

  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t*)self->data;

  dt_bgjob_t *j = (dt_bgjob_t*)g_hash_table_lookup(d->jobs, key);
  if(j)
  {
    g_hash_table_remove(d->jobs, key);

    /* remove job widget from jobbox */
    if(j->widget && GTK_IS_WIDGET(j->widget))
      gtk_container_remove(GTK_CONTAINER(d->jobbox),j->widget);
    j->widget = 0;

#ifdef HAVE_UNITY
    if( j->type == 0 )
    {
      unity_launcher_entry_set_progress( j->darktable_launcher, 1.0 );
      unity_launcher_entry_set_progress_visible( j->darktable_launcher, FALSE );
    }
#endif
#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
    gtk_osxapplication_attention_request(g_object_new(GTK_TYPE_OSX_APPLICATION, NULL), INFO_REQUEST);
#else
    gtkosx_application_attention_request(g_object_new(GTKOSX_TYPE_APPLICATION, NULL), INFO_REQUEST);
#endif
#endif

    /* if jobbox is empty lets hide */
    if(g_list_length(gtk_container_get_children(GTK_CONTAINER(d->jobbox)))==0)
      gtk_widget_hide(d->jobbox);

    /* free allocted mem */
    g_free(j);
    g_free((guint*)key);
  }
  if(i_own_lock) dt_control_gdk_unlock();
}

static void lib_backgroundjobs_set_cancellable(dt_lib_module_t *self, const guint *key, GCallback callback, gpointer data)
{
  if(!darktable.control->running) return;
  gboolean i_own_lock = dt_control_gdk_lock();

  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t*)self->data;

  dt_bgjob_t *j = (dt_bgjob_t*)g_hash_table_lookup(d->jobs, key);
  if (j)
  {
    GtkWidget *w=j->widget;
    GtkBox *hbox = GTK_BOX (g_list_nth_data (gtk_container_get_children (GTK_CONTAINER ( gtk_bin_get_child (GTK_BIN (w) ) ) ), 0));
    GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_cancel,CPF_STYLE_FLAT);
    gtk_widget_set_size_request(button,17,17);
    g_signal_connect (G_OBJECT (button), "clicked",callback, data);
    gtk_box_pack_start (hbox, GTK_WIDGET(button), FALSE, FALSE, 0);
    gtk_widget_show_all(button);
  }

  if(i_own_lock) dt_control_gdk_unlock();
}
static void _lib_backgroundjobs_cancel_callback(GtkWidget *w, gpointer user_data)
{
  dt_job_t *job=(dt_job_t *)user_data;
  dt_control_job_cancel(job);
}

static void _lib_backgroundjobs_set_cancellable(dt_lib_module_t *self, const guint *key, struct dt_job_t *job)
{
  lib_backgroundjobs_set_cancellable(self,key, G_CALLBACK (_lib_backgroundjobs_cancel_callback), (gpointer)job);
}


static void _lib_backgroundjobs_progress(dt_lib_module_t *self, const guint *key, double progress)
{
  if(!darktable.control->running) return;
  gboolean i_own_lock = dt_control_gdk_lock();
  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t*)self->data;

  dt_bgjob_t *j = (dt_bgjob_t*)g_hash_table_lookup(d->jobs, key);
  if(j)
  {
    if( j->type == 0 )
    {
      gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(j->progressbar), CLAMP(progress, 0, 1.0));

#ifdef HAVE_UNITY
      unity_launcher_entry_set_progress( j->darktable_launcher, CLAMP(progress, 0, 1.0));
#endif
    }
  }

  if(i_own_lock) dt_control_gdk_unlock();
}

#ifdef USE_LUA

static double _lib_backgroundjobs_get_progress(dt_lib_module_t *self, const guint *key)
{
  if(!darktable.control->running) return -1.0;
  gboolean i_own_lock = dt_control_gdk_lock();
  double result = -1.0;
  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t*)self->data;

  dt_bgjob_t *j = (dt_bgjob_t*)g_hash_table_lookup(d->jobs, key);
  if(j)
  {
    if( j->type == 0 )
    {
      result = gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(j->progressbar));
    }
  }

  if(i_own_lock) dt_control_gdk_unlock();
  return result;
}

typedef guint* dt_lua_backgroundjob_t;

static int32_t lua_job_canceled_job(struct dt_job_t *job) 
{
  const guint* key = job->user_data;
  lua_State * L = darktable.lua_state.state;
  gboolean has_lock = dt_lua_lock();
  luaA_push(L,dt_lua_backgroundjob_t,&key);
  lua_getuservalue(L,-1);
  lua_getfield(L,-1,"cancel_callback");
  lua_pushvalue(L,-3);
  dt_lua_do_chunk(L,1,0);
  lua_pop(L,2);
  dt_lua_unlock(has_lock);
  return 0;
}

static void lua_job_cancelled(GtkWidget *w, gpointer user_data)
{
  dt_job_t job;
  guint* key = user_data;
  dt_control_job_init(&job, "lua: on background cancel");
  job.execute = &lua_job_canceled_job;
  job.user_data = key;
  dt_control_add_job(darktable.control, &job);
}

static int lua_create_job(lua_State *L){
  dt_lib_module_t*self = lua_touserdata(L,lua_upvalueindex(1));
  const char * message = luaL_checkstring(L,1);
  int type = !lua_toboolean(L,2);//inverted logic, true => no percentage bar
  int cancellable = FALSE;
  if(!lua_isnoneornil(L,3)) {
    luaL_checktype(L,3,LUA_TFUNCTION);
    cancellable = TRUE;
  }
  dt_lua_unlock(false);
  const guint * key  = _lib_backgroundjobs_create(self,type,message);
  if(cancellable) {
    lib_backgroundjobs_set_cancellable(self,key,G_CALLBACK(lua_job_cancelled),(gpointer)key);
  }
  dt_lua_lock();
  luaA_push(L,dt_lua_backgroundjob_t,&key);
  if(cancellable) {
    lua_newtable(L);
    lua_pushvalue(L,3);
    lua_setfield(L,-2,"cancel_callback");
    lua_setuservalue(L,-2);
  }
  return 1;
}
static int lua_job_progress(lua_State *L){
  dt_lib_module_t*self = lua_touserdata(L,lua_upvalueindex(1));
  const guint *key;
  luaA_to(L,dt_lua_backgroundjob_t,&key,1);
  if(lua_isnone(L,3)) {
    dt_lua_unlock(false);
    double result = _lib_backgroundjobs_get_progress(self,key);
    dt_lua_lock();
    if(result == -1.0) {
      lua_pushnil(L);
    } else {
      lua_pushnumber(L,result);
    }
    return 1;
  } else {
    double progress = luaL_checknumber(L,3);
    if( progress < 0.0 || progress > 1.0) {
      return luaL_argerror(L,3,"incorrect value for job percentage (between 0 and 1)");
    }
    dt_lua_unlock(false);
    _lib_backgroundjobs_progress(self,key,progress);
    dt_lua_lock();
    return 0;
  }

}

static int lua_job_valid(lua_State*L){
  dt_lib_module_t*self = lua_touserdata(L,lua_upvalueindex(1));
  const guint *key;
  luaA_to(L,dt_lua_backgroundjob_t,&key,1);
  if(lua_isnone(L,3)) {
    dt_lua_unlock(false);
    gboolean i_own_lock = dt_control_gdk_lock();
    dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t*)self->data;
    dt_bgjob_t *j = (dt_bgjob_t*)g_hash_table_lookup(d->jobs, key);
    if(i_own_lock) dt_control_gdk_unlock();
    dt_lua_lock();

    if(j){
      lua_pushboolean(L,true);
    } else {
      lua_pushboolean(L,false);
    }
    return 1;
  } else {
    int validity = lua_toboolean(L,3);
    if( validity) {
      return luaL_argerror(L,3,"a job can not be made valid");
    }
    dt_lua_unlock(false);
    _lib_backgroundjobs_destroy(self,key);
    dt_lua_lock();
    return 0;
  }
}

#endif //USE_LUA

void init(struct dt_lib_module_t *self)
{
#ifdef USE_LUA
  lua_State *L=darktable.lua_state.state;
  int my_typeid = dt_lua_module_get_entry_typeid(L,"lib",self->plugin_name);
  lua_pushlightuserdata(L,self);
  lua_pushcclosure(L,lua_create_job,1);
  dt_lua_register_type_callback_stack_typeid(L,my_typeid,"create_job");

  // create a type describing a job object
  int job_typeid = dt_lua_init_gpointer_type(L,dt_lua_backgroundjob_t);
  lua_pushlightuserdata(L,self);
  lua_pushcclosure(L,lua_job_progress,1);
  dt_lua_register_type_callback_stack_entry_typeid(L,job_typeid,"percent");
  lua_pushlightuserdata(L,self);
  lua_pushcclosure(L,lua_job_valid,1);
  dt_lua_register_type_callback_stack_entry_typeid(L,job_typeid,"valid");
#endif //USE_LUA
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
