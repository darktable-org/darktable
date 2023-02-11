/*
   This file is part of darktable,
   Copyright (C) 2013-2020 darktable developers.

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
#include "lua/call.h"
#include "control/control.h"
#include "lua/lua.h"
#ifndef _WIN32
#include <glib-unix.h>
#endif
#include <glib.h>
#include <stdlib.h>
#ifndef _WIN32
#include <sys/select.h>
#endif

int dt_lua_check_print_error(lua_State* L, int result)
{
  if(result == LUA_OK) return result;
  if(darktable.unmuted & DT_DEBUG_LUA)
  {
    dt_print(DT_DEBUG_LUA, "LUA ERROR : %s\n", lua_tostring(L, -1));
  }
  lua_pop(L,1); // remove the error message, it has been handled
  return result;
}

static int create_backtrace(lua_State*L)
{
  luaL_traceback(L,L,lua_tostring(L,-1),0);
  return 1;
}

int dt_lua_treated_pcall(lua_State*L, int nargs, int nresults)
{
  lua_pushcfunction(L,create_backtrace);
  lua_insert(L,1);
  int result = dt_lua_check_print_error(L,lua_pcall(L,nargs,nresults,1));
  lua_remove(L,1);
  return result;
}


//#define _DEBUG 1
/*
   THREAD
   * threads are a way to store some work that will be done later
   * threads are saved in the table at REGISTRY_INDEX "dt_lua_bg_thread", that table is manipulated with save_thread, get_thread, drop_thread. They have a unique integer ID
   * each thread is a lua_State with the following convention for its stack
      -- top of the stack --
      * args
      * lua function : function to call
      * int : nresults
      * void * : callback data
      * dt_lua_finish_callback : called when the thread is finished, with or without errors
      -- bottom of the stack --

  RUNNING A THREAD
  to run a thread, call run_async_thread which does the following
  * must be called with the lua lock taken
  * find/create a new gtk thread
  * run that thread which is in charge of doing the job
  * the ownership of the lock is transferred to the thread
  * wait for the thread to release the lock (the thread might not have finished
  * return to the caller

  QUEUING A JOB
  * there is a GTK thread in charge of running a LOOP of events
  * each job-queuing function has it own callback, but eventually call run_async_thread

  TODO
  * redo the old yield (override lua primitives)
  * check all call, pcall etc... sites
  * lua lock et recursivité
  * réimplémenter l'intégration avec la boucle gtk pour mieux faire le bounce
  ? add a name in the thread stack for debug purpose
  * two pcall with no tracebacks: call.c and lua.c

   */

static int save_thread(lua_State* L)
{
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_bg_threads");
  lua_pushvalue(L,-2);
  const int thread_num = luaL_ref(L,-2);
  lua_pop(L,2);
  return thread_num;
}

static lua_State* get_thread(lua_State* L, int thread_num)
{
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_bg_threads");
  lua_pushinteger(L,thread_num);
  lua_gettable(L,-2);
  lua_State* thread = lua_tothread(L,-1);
  lua_pop(L,2);
  return thread;
}


static void drop_thread(lua_State*L, int thread_num)
{
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_bg_threads");
  lua_pushinteger(L,thread_num);
  lua_pushnil(L);
  lua_settable(L,-3);
  lua_pop(L,1);
}


static void run_async_thread_main(gpointer data,gpointer user_data)
{
  // lua lock ownership transferred from parent thread
  int thread_num = GPOINTER_TO_INT(data);
  lua_State*L = darktable.lua_state.state;
  lua_State* thread = get_thread(L,thread_num);
  if(!thread) {
    dt_print(DT_DEBUG_LUA, "LUA ERROR : no thread found, this should never happen\n");
    return;
  }
  dt_lua_finish_callback  cb = lua_touserdata(thread,1);
  void * cb_data = lua_touserdata(thread,2);
  int nresults = lua_tointeger(thread, 3);
  lua_pushcfunction(thread,create_backtrace);
  lua_insert(thread,4);
  int thread_result = lua_pcall(thread,  lua_gettop(thread)-5,nresults,4);
  if(cb) {
    cb(thread,thread_result,cb_data);
  } else {
    dt_lua_check_print_error(thread,thread_result);
  }
  drop_thread(L,thread_num);
  dt_lua_unlock();
  return;

}


static void run_async_thread(lua_State* L, int thread_num)
{
  g_thread_pool_push(darktable.lua_state.pool,GINT_TO_POINTER(thread_num),NULL);
  // lock ownership is transferred to the new thread. We want to block until it is returned to us
  // either because the other thread finished or because it paused
  dt_lua_lock();
}


/*
   END JOB
   This is a source that deals with DT termination. it triggers when DT exits
   */
static gboolean end_job_prepare (GSource *source, gint    *timeout)
{
  return darktable.lua_state.ending;
}

static gboolean end_job_dispatch (GSource* source, GSourceFunc callback, gpointer user_data)
{
  g_main_loop_quit(darktable.lua_state.loop);
  g_thread_pool_free(darktable.lua_state.pool,false,true);
  return G_SOURCE_REMOVE;
}
static GSourceFuncs end_job_funcs =
{
  end_job_prepare,
  NULL,  /* check */
  end_job_dispatch,
  NULL,
};

static void end_job_init()
{
  GSource *source = g_source_new (&end_job_funcs, sizeof (GSource));
  g_source_set_name (source, "lua_end_job_source");
  // make sure to finish any non-blocking job before we quit
  g_source_set_priority(source,G_PRIORITY_DEFAULT_IDLE);
  g_source_attach(source,darktable.lua_state.context);
}

/*
   STACKED JOB
   This is a source that deals with lua jobs that are put on the stack to
   be run on the lua thread.
   */
static gboolean stacked_job_prepare (GSource *source, gint    *timeout)
{
  return (g_async_queue_length(darktable.lua_state.stacked_job_queue) > 0);
}

static gboolean stacked_job_dispatch (GSource* source, GSourceFunc callback, gpointer user_data)
{
  gpointer message;
  message = g_async_queue_try_pop (darktable.lua_state.stacked_job_queue);
  if(message == NULL)
  {
    return TRUE;
  }

  dt_lua_lock();
  lua_State* L= darktable.lua_state.state;
  int reference = GPOINTER_TO_INT(message);
  run_async_thread(L,reference);
  dt_lua_unlock();
  return G_SOURCE_CONTINUE;
}
static void stacked_job_finalize (GSource *source)
{
  g_async_queue_unref(darktable.lua_state.stacked_job_queue);
  darktable.lua_state.stacked_job_queue = NULL;
}

static GSourceFuncs stacked_job_funcs =
{
  stacked_job_prepare,
  NULL,  /* check */
  stacked_job_dispatch,
  stacked_job_finalize,
};

static void stacked_job_init()
{
  darktable.lua_state.stacked_job_queue = g_async_queue_new();
  GSource *source = g_source_new (&stacked_job_funcs, sizeof (GSource));
  g_source_set_name (source, "lua_stacked_job_source");
  g_source_attach(source,darktable.lua_state.context);
}

/*
   ALIEN JOB
   This is a source that deals with lua jobs that are not on the stack but passed as
   varags. This is used to queue a lua job without owning the lua lock
   */

typedef struct {
  lua_CFunction pusher;
  GList* extra;
  dt_lua_finish_callback cb;
  void * cb_data;
  int nresults;

}async_call_data;

static gboolean alien_job_prepare (GSource *source, gint    *timeout)
{
  return (g_async_queue_length(darktable.lua_state.alien_job_queue) > 0);
}

static void alien_job_destroy(void *data_ptr)
{
  async_call_data* data = data_ptr;

  GList* cur_elt = data->extra;
  while(cur_elt) {
    GList * type_type_elt = cur_elt;
    cur_elt = g_list_next(cur_elt);
    //GList * type_elt = cur_elt;
    cur_elt = g_list_next(cur_elt);
    GList * data_elt = cur_elt;
    cur_elt = g_list_next(cur_elt);
    switch(GPOINTER_TO_INT(type_type_elt->data)) {
      case LUA_ASYNC_TYPEID_WITH_FREE:
      case LUA_ASYNC_TYPENAME_WITH_FREE:
        {
          GList *destructor_elt = cur_elt;
          cur_elt = g_list_next(cur_elt);
          GValue to_free = G_VALUE_INIT;
          g_value_init(&to_free,G_TYPE_POINTER);
          g_value_set_pointer(&to_free,data_elt->data);
          g_closure_invoke(destructor_elt->data,NULL,1,&to_free,NULL);
          g_closure_unref (destructor_elt->data);
        }
        break;
      case LUA_ASYNC_TYPEID:
      case LUA_ASYNC_TYPENAME:
        break;
      case LUA_ASYNC_DONE:
      default:
        // should never happen
        g_assert(false);
        break;
    }
  }
  g_list_free(data->extra);
  free(data);
}

static gboolean alien_job_dispatch (GSource* source, GSourceFunc callback, gpointer user_data)
{
  gpointer message;
  message = g_async_queue_try_pop (darktable.lua_state.alien_job_queue);
  if(message == NULL)
  {
    return TRUE;
  }

  async_call_data* data = (async_call_data*)message;
  dt_lua_lock();
  lua_State* L= darktable.lua_state.state;
  lua_State *new_thread = lua_newthread(L);
  int reference = save_thread(L);
  lua_pushlightuserdata(new_thread,data->cb);
  lua_pushlightuserdata(new_thread,data->cb_data);
  lua_pushinteger(new_thread,data->nresults);
  lua_pushcfunction(new_thread,data->pusher);

  GList* cur_elt = data->extra;
  while(cur_elt) {
    GList * type_type_elt = cur_elt;
    cur_elt = g_list_next(cur_elt);
    GList * type_elt = cur_elt;
    cur_elt = g_list_next(cur_elt);
    GList * data_elt = cur_elt;
    cur_elt = g_list_next(cur_elt);
    switch(GPOINTER_TO_INT(type_type_elt->data)) {
      case LUA_ASYNC_TYPEID_WITH_FREE:
        // skip the destructor
        cur_elt = g_list_next(cur_elt);
        // do not break
      case LUA_ASYNC_TYPEID:
        luaA_push_type(new_thread,GPOINTER_TO_INT(type_elt->data),data_elt->data);
        break;
      case LUA_ASYNC_TYPENAME_WITH_FREE:
        // skip the destructor
        cur_elt = g_list_next(cur_elt);
        // do not break
      case LUA_ASYNC_TYPENAME:
        luaA_push_type(new_thread,luaA_type_find(L,type_elt->data),&data_elt->data);
        break;
      case LUA_ASYNC_DONE:
      default:
        // should never happen
        g_assert(false);
        break;
    }
  }
  run_async_thread(L,reference);
  dt_lua_unlock();
  alien_job_destroy(data);
  return G_SOURCE_CONTINUE;
}
static void alien_job_finalize (GSource *source)
{
  gpointer message = g_async_queue_try_pop (darktable.lua_state.alien_job_queue);
  while(message) {
    alien_job_destroy(message);
    message = g_async_queue_try_pop (darktable.lua_state.alien_job_queue);

  }
  g_async_queue_unref(darktable.lua_state.alien_job_queue);
  darktable.lua_state.alien_job_queue = NULL;
}

static GSourceFuncs alien_job_funcs =
{
  alien_job_prepare,
  NULL,  /* check */
  alien_job_dispatch,
  alien_job_finalize,
};

static void alien_job_init()
{
  darktable.lua_state.alien_job_queue = g_async_queue_new();
  GSource *source = g_source_new (&alien_job_funcs, sizeof (GSource));
  g_source_set_name (source, "lua_alien_job_source");
  g_source_attach(source,darktable.lua_state.context);
}

/*
   STRING JOB
   This is a source that deals with lua jobs that are given as lua strings
   */

typedef struct {
  char* function;
  dt_lua_finish_callback cb;
  void * cb_data;
  int nresults;

}string_call_data;

static gboolean string_job_prepare (GSource *source, gint    *timeout)
{
  return (g_async_queue_length(darktable.lua_state.string_job_queue) > 0);
}


static void string_data_destroy(string_call_data * data)
{
  free(data->function);
  free(data);
}

static gboolean string_job_dispatch (GSource* source, GSourceFunc callback, gpointer user_data)
{
  gpointer message;
  message = g_async_queue_try_pop (darktable.lua_state.string_job_queue);
  if(message == NULL)
  {
    return TRUE;
  }
  string_call_data* data = (string_call_data*)message;

  dt_lua_lock();
  lua_State* L= darktable.lua_state.state;
  lua_State *new_thread = lua_newthread(L);
  int reference = save_thread(L);
  lua_pushlightuserdata(new_thread,data->cb);
  lua_pushlightuserdata(new_thread,data->cb_data);
  lua_pushinteger(new_thread,data->nresults);

  int load_result = luaL_loadstring(new_thread, data->function);
  if(load_result != LUA_OK)
  {
    if(data->cb) {
      data->cb(new_thread,load_result,data->cb_data);
    } else {
      dt_lua_check_print_error(new_thread,load_result);
    }
    drop_thread(L,reference);
    dt_lua_unlock();
    string_data_destroy(data);
    return G_SOURCE_CONTINUE;
  }

  run_async_thread(L,reference);
  dt_lua_unlock();
  string_data_destroy(data);
  return G_SOURCE_CONTINUE;
}
static void string_job_finalize (GSource *source)
{
  gpointer message = g_async_queue_try_pop (darktable.lua_state.string_job_queue);
  while(message) {
    string_data_destroy(message);
    message = g_async_queue_try_pop (darktable.lua_state.string_job_queue);

  }
  g_async_queue_unref(darktable.lua_state.string_job_queue);
  darktable.lua_state.string_job_queue = NULL;
}

static GSourceFuncs string_job_funcs =
{
  string_job_prepare,
  NULL,  /* check */
  string_job_dispatch,
  string_job_finalize,
};

static void string_job_init()
{
  darktable.lua_state.string_job_queue = g_async_queue_new();
  GSource *source = g_source_new (&string_job_funcs, sizeof (GSource));
  g_source_set_name (source, "lua_string_job_source");
  g_source_attach(source,darktable.lua_state.context);
}

void dt_lua_async_call_internal(const char* function, int line,lua_State *L, int nargs,int nresults,dt_lua_finish_callback cb, void*data)
{
#ifdef _DEBUG
  dt_print(DT_DEBUG_LUA,"LUA DEBUG : %s called from %s %d, nargs : %d\n",__FUNCTION__,function,line,nargs);
#endif

  lua_State *new_thread = lua_newthread(L);
  lua_pushlightuserdata(new_thread,cb);
  lua_pushlightuserdata(new_thread,data);
  lua_pushinteger(new_thread,nresults);
  int reference = save_thread(L);
  lua_xmove(L,new_thread,nargs+1);
  g_async_queue_push(darktable.lua_state.stacked_job_queue,GINT_TO_POINTER(reference));
  g_main_context_wakeup(darktable.lua_state.context);
}

void dt_lua_async_call_alien_internal(const char * call_function, int line,lua_CFunction pusher,int nresults,dt_lua_finish_callback cb, void*cb_data, dt_lua_async_call_arg_type arg_type,...)
{
  if(!darktable.lua_state.alien_job_queue) {
    // early call before lua has properly been initialized, ignore
#ifdef _DEBUG
  dt_print(DT_DEBUG_LUA,"LUA DEBUG : %s called early. probably ok.\n",__FUNCTION__);
#endif
    return;
  }
#ifdef _DEBUG
  dt_print(DT_DEBUG_LUA,"LUA DEBUG : %s called from %s %d\n",__FUNCTION__,call_function,line);
#endif
  async_call_data*data = malloc(sizeof(async_call_data));
  data->pusher = pusher;
  data->extra=NULL;
  data->cb = cb;
  data->cb_data = cb_data;
  data->nresults = nresults;
  va_list ap;
  va_start(ap,arg_type);
  dt_lua_async_call_arg_type cur_type = arg_type;
  while(cur_type != LUA_ASYNC_DONE){
    data->extra=g_list_append(data->extra,GINT_TO_POINTER(cur_type));
    switch(cur_type) {
      case LUA_ASYNC_TYPEID:
        data->extra=g_list_append(data->extra,GINT_TO_POINTER(va_arg(ap,luaA_Type)));
        data->extra=g_list_append(data->extra,va_arg(ap,gpointer));
        break;
      case LUA_ASYNC_TYPEID_WITH_FREE:
        {
          data->extra=g_list_append(data->extra,GINT_TO_POINTER(va_arg(ap,luaA_Type)));
          data->extra=g_list_append(data->extra,va_arg(ap,gpointer));
          GClosure* closure = va_arg(ap,GClosure*);
          g_closure_ref (closure);
          g_closure_sink (closure);
          g_closure_set_marshal(closure, g_cclosure_marshal_generic);
          data->extra=g_list_append(data->extra,closure);
        }
        break;
      case LUA_ASYNC_TYPENAME:
        data->extra=g_list_append(data->extra,va_arg(ap,char *));
        data->extra=g_list_append(data->extra,va_arg(ap,gpointer));
        break;
      case LUA_ASYNC_TYPENAME_WITH_FREE:
        {
          data->extra=g_list_append(data->extra,va_arg(ap,char *));
          data->extra=g_list_append(data->extra,va_arg(ap,gpointer));
          GClosure* closure = va_arg(ap,GClosure*);
          g_closure_ref (closure);
          g_closure_sink (closure);
          g_closure_set_marshal(closure, g_cclosure_marshal_generic);
          data->extra=g_list_append(data->extra,closure);
        }
        break;
      default:
        // should never happen
        g_assert(false);
        break;
    }
    cur_type = va_arg(ap,dt_lua_async_call_arg_type);
  }

  va_end(ap);

  g_async_queue_push(darktable.lua_state.alien_job_queue,(gpointer)data);
  g_main_context_wakeup(darktable.lua_state.context);
}


void dt_lua_async_call_string_internal(const char* function, int line,const char* lua_string,int nresults,dt_lua_finish_callback cb, void*cb_data)
{
#ifdef _DEBUG
  dt_print(DT_DEBUG_LUA,"LUA DEBUG : %s called from %s %d, string %s\n",__FUNCTION__,function,line,lua_string);
#endif
  string_call_data*data = malloc(sizeof(string_call_data));
  data->function = strdup(lua_string);
  data->cb = cb;
  data->cb_data = cb_data;
  data->nresults = nresults;

  g_async_queue_push(darktable.lua_state.string_job_queue,(gpointer)data);
  g_main_context_wakeup(darktable.lua_state.context);

}



static gpointer lua_thread_main(gpointer data)
{
  darktable.lua_state.pool = g_thread_pool_new(run_async_thread_main,NULL,-1,false,NULL);
  darktable.lua_state.loop = g_main_loop_new(darktable.lua_state.context,false);
  g_main_loop_run(darktable.lua_state.loop);
  return 0;
}

static int dispatch_cb(lua_State *L)
{
  dt_lua_async_call(L,lua_gettop(L)-1,0,NULL,NULL);
  return 0;
}

static int ending_cb(lua_State *L)
{
  lua_pushboolean(L,darktable.lua_state.ending);
  return 1;
}


static int execute_cb(lua_State*L)
{
  const char *cmd = luaL_optstring(L, 1, NULL);
  dt_lua_unlock();
  int stat = system(cmd);
  dt_lua_lock();
  lua_pushinteger(L,stat);
  return 1;
}

static int sleep_cb(lua_State*L)
{
  const int delay = luaL_optinteger(L, 1, 0);
  dt_lua_unlock();
  g_usleep(delay*1000);
  dt_lua_lock();
  return 0;
}

#if !defined (_WIN32)
static int read_cb(lua_State*L)
{
  luaL_checkudata(L,1,LUA_FILEHANDLE);
  luaL_Stream *stream = lua_touserdata(L, 1);
  int myfileno = fileno(stream->f);
  fd_set fdset;
  FD_ZERO(&fdset);
  FD_SET(myfileno, &fdset);
  dt_lua_unlock();
  select(myfileno + 1, &fdset, NULL, NULL, 0);
  dt_lua_lock();
  return 0;
}
#endif

typedef struct gtk_wrap_communication {
  GCond end_cond;
  GMutex end_mutex;
  lua_State *L;
  int retval;
} gtk_wrap_communication;

gboolean dt_lua_gtk_wrap_callback(gpointer data)
{
  dt_lua_lock_silent();
  gtk_wrap_communication *communication = (gtk_wrap_communication*)data;
  g_mutex_lock(&communication->end_mutex);
  // TODO : propre stack unwinding
  communication->retval = lua_pcall(communication->L,lua_gettop(communication->L)-1,LUA_MULTRET,0);
  g_cond_signal(&communication->end_cond);
  g_mutex_unlock(&communication->end_mutex);
  dt_lua_unlock();
  return false;
}

static int gtk_wrap(lua_State*L)
{
  lua_pushvalue(L,lua_upvalueindex(1));
  lua_insert(L,1);
  if(pthread_equal(darktable.control->gui_thread, pthread_self())) {
    lua_call(L, lua_gettop(L)-1, LUA_MULTRET);
    return lua_gettop(L);
  } else {
#ifdef _DEBUG
    dt_print(DT_DEBUG_LUA, "LUA DEBUG : %s called from %s %llu\n", __FUNCTION__,
             lua_tostring(L, lua_upvalueindex(2)), lua_tointeger(L, lua_upvalueindex(3)));
#endif
    dt_lua_unlock();
    gtk_wrap_communication communication;
    g_mutex_init(&communication.end_mutex);
    g_cond_init(&communication.end_cond);
    communication.L = L;
    g_mutex_lock(&communication.end_mutex);
    g_main_context_invoke(NULL,dt_lua_gtk_wrap_callback,&communication);
    g_cond_wait(&communication.end_cond,&communication.end_mutex);
    g_mutex_unlock(&communication.end_mutex);
    g_mutex_clear(&communication.end_mutex);
    dt_lua_lock();
#ifdef _DEBUG
    dt_print(DT_DEBUG_LUA, "LUA DEBUG : %s return for call from from %s %llu\n", __FUNCTION__,
             lua_tostring(L, lua_upvalueindex(2)), lua_tointeger(L, lua_upvalueindex(3)));
#endif
    if(communication.retval == LUA_OK) {
      return lua_gettop(L);
    } else {
      return lua_error(L);
    }
  }

}

void dt_lua_gtk_wrap_internal(lua_State*L,const char* function, int line)
{
  lua_pushstring(L,function);
  lua_pushinteger(L,line);
  lua_pushcclosure(L,gtk_wrap,3);
}
int dt_lua_init_call(lua_State *L)
{

  dt_lua_push_darktable_lib(L);
  luaA_Type type_id = dt_lua_init_singleton(L, "control", NULL);
  lua_setfield(L, -2, "control");
  lua_pop(L, 1);

  lua_pushcfunction(L, ending_cb);
  dt_lua_type_register_const_type(L, type_id, "ending");
  lua_pushcfunction(L, dispatch_cb);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "dispatch");
  lua_pushcfunction(L,execute_cb);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "execute");
  lua_pushcfunction(L,sleep_cb);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "sleep");
#if !defined (_WIN32)
  lua_pushcfunction(L,read_cb);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, type_id, "read");
#endif

  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "dt_lua_bg_threads");
  // create stuff in init to avoid race conditions
  darktable.lua_state.context = g_main_context_new();
  stacked_job_init();
  alien_job_init();
  string_job_init();
  end_job_init();

  g_thread_new("lua thread",lua_thread_main,NULL);
  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
