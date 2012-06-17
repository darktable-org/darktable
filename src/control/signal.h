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

#ifndef DT_CONTROL_SIGNAL
#define DT_CONTROL_SIGNAL

#include <glib-object.h>

/** \brief enum of signals to listen for in darktable.
    \note To add a new signal, first off add a enum and 
    document what it's used for, then add a matching signal string
    name to _strings in signal.c
*/
typedef enum dt_signal_t
{
  /** \brief This signal is raised when mouse hovers over image thumbs
      both on lighttable and in the filmstrip.
   */
  DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,

  /** \brief This signal is raised when dt_control_queue_redraw() is called.
  */
  DT_SIGNAL_CONTROL_REDRAW_ALL,

  /** \brief This signal is raid when dt_control_queue_redraw_center() is called. 
   */
  DT_SIGNAL_CONTROL_REDRAW_CENTER,

  /** \brief This signal is raised by viewmanager when a view has changed.
   */
  DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,

  /** \bief This signal is rasied when a thumb is doubleclicked in
      filmstrip module.
   */
  DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE,

  /** \brief This signal is raised when darktable.develop is initialized.
      \note any modules that wants to acces darktable->develop should connect
      to this signal to be sure darktable.develop is initialized.
   */
  DT_SIGNAL_DEVELOP_INITIALIZE,

  /** \brief This signal is raised when develop preview pipe process is finished */
  DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,

  /** \brief This signal is rasied when pipe is finished and the gui is attached */
  DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,

  /** \brief This signal is raised when develop history is changed */
  DT_SIGNAL_DEVELOP_HISTORY_CHANGE,

  /** \brief This signal is raised when a mipmap has been generated and flushed to cache */
  DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,

  /** \brief This signal is raised when collection query is changed */
  DT_SIGNAL_COLLECTION_CHANGED,

  /* do not touch !*/
  DT_SIGNAL_COUNT
}
dt_signal_t;

/* intitialize the signal framework */
struct dt_control_signal_t *dt_control_signal_init();
/* raises a signal */
void dt_control_signal_raise(const struct dt_control_signal_t *ctlsig, const dt_signal_t signal);
/* connects a callback to a signal */
void dt_control_signal_connect(const struct dt_control_signal_t *ctlsig,const dt_signal_t signal, GCallback cb, gpointer user_data); 
/* disconnects a callback from a sink */
void dt_control_signal_disconnect(const struct dt_control_signal_t *ctlsig, GCallback cb,gpointer user_data);
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
