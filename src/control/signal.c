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
}
dt_control_signal_t;

static char *_signal_name[DT_SIGNAL_COUNT] = 
{
  /* Global signals */
  "dt-global-mouse-over-image-change",            // DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE

  "dt-control-draw-all",                          // DT_SIGNAL_CONTROL_DRAW_ALL
  "dt-control-draw-center",                       // DT_SIGNAL_CONTROL_DRAW_CENTER

  "dt-viewmanager-view-changed",                  // DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED
  "dt-viewmanager-filmstrip-activate",            // DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE

  "dt-collection-changed",                        // DT_SIGNAL_COLLECTION_CHANGED

  /* Develop related signals */
  "dt-develop-initialized",                       // DT_SIGNAL_DEVELOP_INITIALIZED
  "dt-develop-mipmap-updated",                    // DT_SIGNAL_DEVELOP_MIPMAP_UPDATED
  "dt-develop-preview-pipe-finished",             // DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED
  "dt-develop-ui-pipe-finished",                  // DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED
  "dt-develop-history-change"                     // DT_SIGNAL_HISTORY_CHANGE
};


dt_control_signal_t *dt_control_signal_init()
{
  dt_control_signal_t *ctlsig = g_malloc(sizeof(dt_control_signal_t));
  memset(ctlsig, 0, sizeof(dt_control_signal_t));

  /* setup dummy gobject typeinfo */
  GTypeQuery query;
  GType type;
  GTypeInfo type_info = {
    0, (GBaseInitFunc) NULL, (GBaseFinalizeFunc) NULL,
    (GClassInitFunc) NULL, (GClassFinalizeFunc) NULL,
    NULL, 0,0, (GInstanceInitFunc) NULL};

  g_type_query(G_TYPE_OBJECT, &query);
  type_info.class_size = query.class_size;
  type_info.instance_size = query.instance_size;
  type = g_type_register_static(G_TYPE_OBJECT, "DarktableSignals", &type_info, 0);
  
  /* create our pretty empty gobject */
  ctlsig->sink = g_object_new(type,NULL); 

  /* create the signals */
  for (int k=0;k<DT_SIGNAL_COUNT;k++)
    g_signal_new(_signal_name[k], G_TYPE_OBJECT, G_SIGNAL_RUN_LAST,0,NULL,NULL,
		  g_cclosure_marshal_VOID__VOID,G_TYPE_NONE,0);
  
  return ctlsig;
}

void dt_control_signal_raise(const dt_control_signal_t *ctlsig, dt_signal_t signal) 
{
  // ignore all signals on shutdown, especially don't lock anything..
  if(!dt_control_running()) return;
  gboolean i_own_lock = dt_control_gdk_lock();
  g_signal_emit_by_name(G_OBJECT(ctlsig->sink), _signal_name[signal]);
  if (i_own_lock) dt_control_gdk_unlock();
}

void dt_control_signal_connect(const dt_control_signal_t *ctlsig,dt_signal_t signal, GCallback cb, gpointer user_data)
{
  g_signal_connect(G_OBJECT(ctlsig->sink), _signal_name[signal], G_CALLBACK(cb),user_data);
}

void dt_control_signal_disconnect(const struct dt_control_signal_t *ctlsig, GCallback cb,gpointer user_data)
{
  g_signal_handlers_disconnect_matched(G_OBJECT(ctlsig->sink), 
				       G_SIGNAL_MATCH_FUNC|G_SIGNAL_MATCH_DATA, 
				       0, 0, 
				       NULL , 
				       cb, user_data);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
