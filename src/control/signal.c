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
#include <string.h>
#include <glib.h>
#include "control/control.h"
#include "control/signal.h"

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
} dt_signal_description;

static GType uint_arg[] = { G_TYPE_UINT };
static GType pointer_arg[] = { G_TYPE_POINTER };
static GType pointer_2arg[] = { G_TYPE_POINTER, G_TYPE_POINTER };
static GType image_export_arg[]
    = { G_TYPE_UINT, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_POINTER };



static dt_signal_description _signal_description[DT_SIGNAL_COUNT] = {
  /* Global signals */
  { "dt-global-mouse-over-image-change", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE

  { "dt-control-redraw-all", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_CONTROL_REDRAW_ALL
  { "dt-control-redraw-center", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_CONTROL_REDRAW_CENTER

  { "dt-viewmanager-view-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_generic, 2,
    pointer_2arg }, // DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED
  { "dt-viewmanager-filmstrip-activate", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE

  { "dt-collection-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_COLLECTION_CHANGED
  { "dt-tag-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_TAG_CHANGED
  { "dt-filmrolls-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_FILMROLLS_CHANGED
  { "dt-filmrolls-imported", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__UINT, 1,
    uint_arg }, // DT_SIGNAL_FILMROLLS_IMPORTED
  { "dt-filmrolls-removed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_FILMROLLS_REMOVED


  /* Develop related signals */
  { "dt-develop-initialized", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_DEVELOP_INITIALIZED
  { "dt-develop-mipmap-updated", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_DEVELOP_MIPMAP_UPDATED
  { "dt-develop-preview-pipe-finished", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED
  { "dt-develop-ui-pipe-finished", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED
  { "dt-develop-history-change", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_HISTORY_CHANGE
  { "dt-develop-image-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_DEVELOP_IMAGE_CHANGE
  { "dt-control-profile-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_CONTROL_PROFILE_CHANGED
  { "dt-image-import", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__UINT, 1,
    uint_arg }, // DT_SIGNAL_IMAGE_IMPORT
  { "dt-image-export-multiple", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__POINTER, 1,
    pointer_arg }, // DT_SIGNAL_IMAGE_EXPORT_MULTIPLE
  { "dt-image-export-tmpfile", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_generic, 6,
    image_export_arg }, // DT_SIGNAL_IMAGE_EXPORT_TMPFILE
  { "dt-imageio-storage-change", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_IMAGEIO_STORAGE_CHANGE


  { "dt-preferences-changed", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_PREFERENCES_CHANGE


  { "dt-camera-detected", NULL, NULL, G_TYPE_NONE, g_cclosure_marshal_VOID__VOID, 0,
    NULL }, // DT_SIGNAL_CAMERA_DETECTED,

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
    g_signal_newv(_signal_description[k].name, _signal_type, G_SIGNAL_RUN_LAST, 0,
                  _signal_description[k].accumulator, _signal_description[k].accu_data,
                  _signal_description[k].c_marshaller, _signal_description[k].return_type,
                  _signal_description[k].n_params, _signal_description[k].param_types);

  return ctlsig;
}

void dt_control_signal_raise(const dt_control_signal_t *ctlsig, dt_signal_t signal, ...)
{
  va_list extra_args;
  // ignore all signals on shutdown, especially don't lock anything..
  if(!dt_control_running()) return;
  va_start(extra_args, signal);
  gboolean i_own_lock = dt_control_gdk_lock();
  // g_signal_emit_by_name(G_OBJECT(ctlsig->sink), _signal_description[signal].name);
  g_signal_emit_valist(G_OBJECT(ctlsig->sink),
                       g_signal_lookup(_signal_description[signal].name, _signal_type), 0, extra_args);
  va_end(extra_args);
  if(i_own_lock) dt_control_gdk_unlock();
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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
