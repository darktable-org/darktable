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
      no param, no returned value
   */
  DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,

  /** \brief This signal is raised when dt_control_queue_redraw() is called.
    no param, no returned value
  */
  DT_SIGNAL_CONTROL_REDRAW_ALL,

  /** \brief This signal is raised when dt_control_queue_redraw_center() is called.
    no param, no returned value
   */
  DT_SIGNAL_CONTROL_REDRAW_CENTER,

  /** \brief This signal is raised by viewmanager when a view has changed.
    1 : dt_view_t * the old view
    2 : dt_view_t * the new (current) view
    no returned value
   */
  DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,

  /** \bief This signal is raised when a thumb is doubleclicked in
    no param, no returned value
      filmstrip module.
   */
  DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE,

  /** \brief This signal is raised when collection query is changed
  no param, no returned value
    */
  DT_SIGNAL_COLLECTION_CHANGED,

  /** \brief This signal is raised when tags is added/deleted/changed  */
  DT_SIGNAL_TAG_CHANGED,

  /** \brief This signal is raised when a filmroll is deleted/changed but not imported
      \note when a filmroll is imported, use DT_SIGNALS_FILMOLLS_IMPORTED, as the gui
       has to behave differently
  */
  DT_SIGNAL_FILMROLLS_CHANGED,

  /** \brief This signal is raised only when a filmroll is imported
    1 :  int the film_id for the film that triggered the import. in case of recursion, other filmrolls might
    be affected
    no return
   */
  DT_SIGNAL_FILMROLLS_IMPORTED,

  /** \brief This signal is raised only when a filmroll is removed */
  DT_SIGNAL_FILMROLLS_REMOVED,

  /** \brief This signal is raised when darktable.develop is initialized.
      \note any modules that wants to access darktable->develop should connect
      to this signal to be sure darktable.develop is initialized.
  no param, no returned value
   */
  DT_SIGNAL_DEVELOP_INITIALIZE,

  /** \brief This signal is raised when a mipmap has been generated and flushed to cache
  no param, no returned value
    */
  DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,

  /** \brief This signal is raised when develop preview pipe process is finished
  no param, no returned value
    */
  DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,

  /** \brief This signal is rasied when pipe is finished and the gui is attached
  no param, no returned value
    */
  DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,

  /** \brief This signal is raised when develop history is changed
  no param, no returned value
    */
  DT_SIGNAL_DEVELOP_HISTORY_CHANGE,

  /** \brief This signal is rasied when image is changed in darkroom */
  DT_SIGNAL_DEVELOP_IMAGE_CHANGED,

  /** \brief This signal is raised when the screen profile has changed
  no param, no returned value
    */
  DT_SIGNAL_CONTROL_PROFILE_CHANGED,
  /** \brief This signal is raised when a new image is imported (not cloned)
    1 uint32_t :  the new image id
    no return
    */
  DT_SIGNAL_IMAGE_IMPORT,

  /** \brief This signal is raised when multiple images are exported
    1 dt_image_export_t *: structure describing the export. the content can be edited
    no return
    */
  DT_SIGNAL_IMAGE_EXPORT_MULTIPLE,

  /** \brief This signal is raised after an image has been exported
    to a file, but before it is sent to facebook/picasa etc...
    export won't happen until this function returns
    1 int : the imgid exported
    2 char* : the filename we exported to
    3 dt_imageio_module_format_t* : the format used for export
    4 dt_imageio_module_data_t* : the format's data
    5 dt_imageio_module_storage_t* : the storage used for export (can be NULL)
    6 dt_imageio_module_data_t* : the storage's data (can be NULL)
    no return
    */
  DT_SIGNAL_IMAGE_EXPORT_TMPFILE,
  /** \brief This signal is raised when a new storage module is loaded
    noparameters
    no return
    */
  DT_SIGNAL_IMAGEIO_STORAGE_CHANGE,

  /** \brief This signal is raised after preferences have been changed
    1 dt_view_t* : the view
    no return
    */
  DT_SIGNAL_PREFERENCES_CHANGE,

  /** \brief This signal is raised when new gphoto2 cameras might have been detected
    no return
   * */
  DT_SIGNAL_CAMERA_DETECTED,

  /* do not touch !*/
  DT_SIGNAL_COUNT
} dt_signal_t;

/* inititialize the signal framework */
struct dt_control_signal_t *dt_control_signal_init();
/* raises a signal */
void dt_control_signal_raise(const struct dt_control_signal_t *ctlsig, const dt_signal_t signal, ...);
/* connects a callback to a signal */
void dt_control_signal_connect(const struct dt_control_signal_t *ctlsig, const dt_signal_t signal,
                               GCallback cb, gpointer user_data);
/* disconnects a callback from a sink */
void dt_control_signal_disconnect(const struct dt_control_signal_t *ctlsig, GCallback cb, gpointer user_data);
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
