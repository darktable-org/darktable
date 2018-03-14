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
#include "control/signal.h"
#include "control/control.h"
#include <glib.h>
#include <string.h>

typedef struct dt_control_signal_t
{
  /* the sinks for the signals */
  GObject *sink;
} dt_control_signal_t;

/*
                                                         GSignalFlags signal_flags,
                                                         ...);
   */
typedef struct dt_signal_description
{
  const char *name;
  GSignalAccumulator accumulator;
  gpointer accu_data;
  GType return_type;
  GSignalCMarshaller c_marshaller;
  guint n_params;
  GType *param_types;
  GCallback destructor;
  gboolean synchronous;
} dt_signal_description;


static GType uint_arg[] = { G_TYPE_UINT };
static GType pointer_arg[] = { G_TYPE_POINTER };
static GType pointer_2arg[] = { G_TYPE_POINTER, G_TYPE_POINTER };
static GType image_export_arg[]
    = { G_TYPE_UINT, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_POINTER };



static dt_signal_description _signal_description[DT_SIGNAL_COUNT] = {
  /* Global signals */
  { "dt-global-mouse-over-image-change", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE

  { "dt-control-redraw-all", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_CONTROL_REDRAW_ALL
  { "dt-control-redraw-center", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_CONTROL_REDRAW_CENTER

  { "dt-viewmanager-view-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_generic, 2,
    pointer_2arg, NULL, FALSE }, // DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED
  { "dt-viewmanager-filmstrip-activate", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE

  { "dt-collection-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_COLLECTION_CHANGED
  { "dt-tag-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_TAG_CHANGED
  { "dt-style-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_STYLE_CHANGED
  { "dt-filmrolls-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_FILMROLLS_CHANGED
  { "dt-filmrolls-imported", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__UINT, 1,
    uint_arg, NULL, FALSE }, // DT_SIGNAL_FILMROLLS_IMPORTED
  { "dt-filmrolls-removed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_FILMROLLS_REMOVED


  /* Develop related signals */
  { "dt-develop-initialized", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_DEVELOP_INITIALIZED
  { "dt-develop-mipmap-updated", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_DEVELOP_MIPMAP_UPDATED
  { "dt-develop-preview-pipe-finished", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED
  { "dt-develop-ui-pipe-finished", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED
  { "dt-develop-history-change", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_HISTORY_CHANGE
  { "dt-develop-module-remove", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_generic, 1,
    pointer_arg, NULL, TRUE }, // DT_SIGNAL_MODULE_REMOVE
  { "dt-develop-image-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_DEVELOP_IMAGE_CHANGE
  { "dt-control-profile-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_CONTROL_PROFILE_CHANGED
  { "dt-image-import", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__UINT, 1,
    uint_arg, NULL, FALSE }, // DT_SIGNAL_IMAGE_IMPORT
  { "dt-image-export-tmpfile", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_generic, 6,
    image_export_arg, NULL, TRUE }, // DT_SIGNAL_IMAGE_EXPORT_TMPFILE
  { "dt-imageio-storage-change", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_IMAGEIO_STORAGE_CHANGE


  { "dt-preferences-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_PREFERENCES_CHANGE


  { "dt-camera-detected", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL, NULL, FALSE }, // DT_SIGNAL_CAMERA_DETECTED,

};

static GType _signal_type;

dt_control_signal_t *dt_control_signal_init()
{
  dt_control_signal_t *ctlsig = g_malloc0(sizeof(dt_control_signal_t));

  /* setup dummy gobject typeinfo */
  GTypeQuery query;
  GTypeInfo type_info = { 0, (GBaseInitFunc)NULL, (GBaseFinalizeFunc)NULL, (GClassInitFunc)NULL,
                          (GClassFinalizeFunc)NULL, NULL, 0, 0, (GInstanceInitFunc)NULL };

  g_type_query(G_TYPE_OBJECT, &query);
  type_info.class_size = query.class_size;
  type_info.instance_size = query.instance_size;
  _signal_type = g_type_register_static(G_TYPE_OBJECT, "DarktableSignals", &type_info, 0);

  /* create our pretty empty gobject */
  ctlsig->sink = g_object_new(_signal_type, NULL);

  /* create the signals */
  for(int k = 0; k < DT_SIGNAL_COUNT; k++) 
  {
    g_signal_newv(_signal_description[k].name, _signal_type, G_SIGNAL_RUN_LAST, 0,
        _signal_description[k].accumulator, _signal_description[k].accu_data,
        _signal_description[k].c_marshaller, _signal_description[k].return_type,
        _signal_description[k].n_params, _signal_description[k].param_types);
    if(_signal_description[k].destructor) {
      g_signal_connect_after(G_OBJECT(ctlsig->sink), _signal_description[k].name, _signal_description[k].destructor, NULL);
    }
  }
  return ctlsig;
}

typedef struct _signal_param_t
{
  GValue *instance_and_params;
  guint signal_id;
  guint n_params;
} _signal_param_t;

static gboolean _signal_raise(gpointer user_data)
{
  _signal_param_t *params = (_signal_param_t *)user_data;
  g_signal_emitv(params->instance_and_params, params->signal_id, 0, NULL);
  for(int i = 0; i <= params->n_params; i++) g_value_unset(&params->instance_and_params[i]);
  free(params->instance_and_params);
  free(params);
  return FALSE;
}

typedef struct async_com_data {
  GCond end_cond;
  GMutex end_mutex;
  gpointer user_data;
} async_com_data;

gboolean _async_com_callback(gpointer data)
{
  async_com_data *communication = (async_com_data*)data;
  g_mutex_lock(&communication->end_mutex);
  _signal_raise(communication->user_data);

  g_cond_signal(&communication->end_cond);
  g_mutex_unlock(&communication->end_mutex);
  return FALSE;
} 

void dt_control_signal_raise(const dt_control_signal_t *ctlsig, dt_signal_t signal, ...)
{
  // ignore all signals on shutdown
  if(!dt_control_running()) return;

  dt_signal_description *signal_description = &_signal_description[signal];

  _signal_param_t *params = (_signal_param_t *)malloc(sizeof(_signal_param_t));
  if(!params) return;

  GValue *instance_and_params = calloc(1 + signal_description->n_params, sizeof(GValue));
  if(!instance_and_params)
  {
    free(params);
    return;
  }

  // 0th element has to be the instance to call
  g_value_init(instance_and_params, _signal_type);
  g_value_set_object(instance_and_params, ctlsig->sink);

  // the rest of instance_and_params will be the params for the callback
  va_list extra_args;
  va_start(extra_args, signal);

  for(int i = 1; i <= signal_description->n_params; i++)
  {
    GType type = signal_description->param_types[i-1];
    g_value_init(&instance_and_params[i], type);
    switch(type)
    {
      case G_TYPE_UINT:
        g_value_set_uint(&instance_and_params[i], va_arg(extra_args, guint));
        break;
      case G_TYPE_STRING:
        g_value_set_string(&instance_and_params[i], va_arg(extra_args, const char *));
        break;
      case G_TYPE_POINTER:
        g_value_set_pointer(&instance_and_params[i], va_arg(extra_args, void *));
        break;
      default:
        fprintf(stderr, "error: unsupported parameter type `%s' for signal `%s'\n", g_type_name(type), signal_description->name);
        va_end(extra_args);
        for(int j = 0; j <= i; j++) g_value_unset(&instance_and_params[j]);
        free(instance_and_params);
        free(params);
        return;
    }
  }

  va_end(extra_args);

  params->instance_and_params = instance_and_params;
  params->signal_id = g_signal_lookup(_signal_description[signal].name, _signal_type);
  params->n_params = signal_description->n_params;

  if(!signal_description->synchronous)
  {
    g_main_context_invoke(NULL, _signal_raise, params);
  } else {
    if(pthread_equal(darktable.control->gui_thread, pthread_self())) {
      _signal_raise(params);
    } else {
      async_com_data communication;
      g_mutex_init(&communication.end_mutex);
      g_cond_init(&communication.end_cond);
      g_mutex_lock(&communication.end_mutex);
      communication.user_data = params;
      g_main_context_invoke(NULL,_async_com_callback,&communication);
      g_cond_wait(&communication.end_cond,&communication.end_mutex);
      g_mutex_unlock(&communication.end_mutex);
      g_mutex_clear(&communication.end_mutex);
    }
  }
}


void dt_control_signal_connect(const dt_control_signal_t *ctlsig, dt_signal_t signal, GCallback cb,
                               gpointer user_data)
{
  g_signal_connect(G_OBJECT(ctlsig->sink), _signal_description[signal].name, G_CALLBACK(cb), user_data);
}

void dt_control_signal_disconnect(const struct dt_control_signal_t *ctlsig, GCallback cb, gpointer user_data)
{
  g_signal_handlers_disconnect_matched(G_OBJECT(ctlsig->sink), G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0,
                                       0, NULL, cb, user_data);
}

void dt_control_signal_block_by_func(const struct dt_control_signal_t *ctlsig, GCallback cb, gpointer user_data)
{
  g_signal_handlers_block_by_func(G_OBJECT(ctlsig->sink), cb, user_data);
}

void dt_control_signal_unblock_by_func(const struct dt_control_signal_t *ctlsig, GCallback cb, gpointer user_data)
{
  g_signal_handlers_unblock_by_func(G_OBJECT(ctlsig->sink), cb, user_data);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
