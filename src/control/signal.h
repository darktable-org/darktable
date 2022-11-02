/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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

#pragma once

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

  /** \brief This signal is raised when image shown in the main view change
      no param, no returned value
   */
  DT_SIGNAL_ACTIVE_IMAGES_CHANGE,

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

  /** \brief This signal is raised by viewmanager when a view has changed.
    1 : dt_view_t * the old view
    2 : dt_view_t * the new (current) view
    no returned value
   */
  DT_SIGNAL_VIEWMANAGER_VIEW_CANNOT_CHANGE,

  /** \bief This signal is raised when a thumb is doubleclicked in
    thumbtable (filemananger, filmstrip)
    1 : int the imageid of the thumbnail
    no returned value
   */
  DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,

  /** \brief This signal is raised when collection changed. To avoid leaking the list,
    dt_collection_t is connected to this event and responsible of that.
    1 : dt_collection_change_t the reason why the collection has changed
    2 : dt_collection_properties_t the property that has changed
    3 : GList of imageids that have changed (can be null if it's a global change)
    4 : next untouched imgid in the list (-1 if no list)
    no returned value
    */
  /** image list not to be freed by the caller, automatically freed */
  DT_SIGNAL_COLLECTION_CHANGED,

  /** \brief This signal is raised when the selection is changed
  no param, no returned value
    */
  DT_SIGNAL_SELECTION_CHANGED,

  /** \brief This signal is raised when a tag is added/deleted/changed  */
  DT_SIGNAL_TAG_CHANGED,

  /** \brief This signal is raised when a geotag is added/deleted/changed  */
  // when imgs <> NULL these images have some geotag changes
  // when imgs == NULL locations have changed
  // if locid <> 0 it the new selected location on map
  DT_SIGNAL_GEOTAG_CHANGED,

  /** \brief This signal is raised when metadata status (shown/hidden) or value has changed */
  DT_SIGNAL_METADATA_CHANGED,

  /** \brief This signal is raised when any of image info has changed  */
  /** image list not to be freed by the caller, automatically freed */
  // TODO check if tag and metadata could be included there
  DT_SIGNAL_IMAGE_INFO_CHANGED,

  /** \brief This signal is raised when a style is added/deleted/changed  */
  DT_SIGNAL_STYLE_CHANGED,

  /** \brief This signal is raised to request image order change */
  DT_SIGNAL_IMAGES_ORDER_CHANGE,

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

  /* \brief This signal is raised when a preset is created/updated/deleted */
  DT_SIGNAL_PRESETS_CHANGED,

  /** \brief This signal is raised when darktable.develop is initialized.
      \note any modules that wants to access darktable->develop should connect
      to this signal to be sure darktable.develop is initialized.
  no param, no returned value
   */
  DT_SIGNAL_DEVELOP_INITIALIZE,

  /** \brief This signal is raised when a mipmap has been generated and flushed to cache
    1 :  int the imgid of the mipmap
    no returned value
    */
  DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,

  /** \brief This signal is raised when develop preview pipe process is finished
  no param, no returned value
    */
  DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,

  /** \brief This signal is raised when develop preview2 pipe process is finished
  no param, no returned value
    */
  DT_SIGNAL_DEVELOP_PREVIEW2_PIPE_FINISHED,

  /** \brief This signal is raised when pipe is finished and the gui is attached
  no param, no returned value
    */
  DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,

  /** \brief This signal is raised when develop history is about to be changed
    1 : GList *  the current history
    2 : uint32_t the correpsing history end
    3 : GList *  the current iop-order list
  no returned value
    */
  DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE,

  /** \brief This signal is raised when develop history is changed
  no param, no returned value
    */
  DT_SIGNAL_DEVELOP_HISTORY_CHANGE,

  /** \brief This signal is raised when the history is compressed or removed.
      in this case any module having a reference to the history must be
      clear.
  no param, no returned value
    */
  DT_SIGNAL_DEVELOP_HISTORY_INVALIDATED,

  /** \brief This signal is raised when a module is removed from the history stack
    1 module
    no returned value
    */
  DT_SIGNAL_DEVELOP_MODULE_REMOVE,

  /** \brief This signal is raised when order of modules in pipeline is changed */
  DT_SIGNAL_DEVELOP_MODULE_MOVED,

  /** \brief This signal is raised when image is changed in darkroom */
  DT_SIGNAL_DEVELOP_IMAGE_CHANGED,

  /** \brief This signal is raised when the screen profile has changed
  no param, no returned value
    */
  DT_SIGNAL_CONTROL_PROFILE_CHANGED,

  /** \brief This signal is raised when a profile is changed by the user
    1 uint32_t :  the profile type that has changed
    no return
    */
  DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,

  /** \brief This signal is raised when a new image is imported (not cloned)
    1 uint32_t :  the new image id
    no return
    */
  DT_SIGNAL_IMAGE_IMPORT,

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
    no parameters
    no return
    */
  DT_SIGNAL_PREFERENCES_CHANGE,

  /** \brief This signal is raised when new gphoto2 cameras might have been detected
    no return
   * */
  DT_SIGNAL_CAMERA_DETECTED,

  /** \brief This signal is raised when dt_control_navigation_redraw() is called.
    no param, no returned value
  */
  DT_SIGNAL_CONTROL_NAVIGATION_REDRAW,

  /** \brief This signal is raised when dt_control_log_redraw() is called.
    no param, no returned value
  */
  DT_SIGNAL_CONTROL_LOG_REDRAW,

  /** \brief This signal is raised when dt_control_toast_redraw() is called.
    no param, no returned value
  */
  DT_SIGNAL_CONTROL_TOAST_REDRAW,

  /** \brief This signal is raised when new color picker data are available in the pixelpipe.
    1 module
    2 piece
    no returned value
  */
  DT_SIGNAL_CONTROL_PICKERDATA_READY,

  /* \brief This signal is raised when metadata view needs update */
  DT_SIGNAL_METADATA_UPDATE,

  /* \brief This signal is raised when a module is in trouble and message is to be displayed */
  DT_SIGNAL_TROUBLE_MESSAGE,

  /* \brief This signal is raised when the user choses a new location from map (module location)*/
  DT_SIGNAL_LOCATION_CHANGED,

  /* do not touch !*/
  DT_SIGNAL_COUNT
} dt_signal_t;

typedef enum dt_debug_signal_action_t
{
  // powers of two, masking
  DT_DEBUG_SIGNAL_ACT_RAISE       = 1 << 0,
  DT_DEBUG_SIGNAL_ACT_CONNECT     = 1 << 1,
  DT_DEBUG_SIGNAL_ACT_DISCONNECT  = 1 << 2,
  DT_DEBUG_SIGNAL_ACT_PRINT_TRACE = 1 << 3,
} dt_debug_signal_action_t;

/* inititialize the signal framework */
struct dt_control_signal_t *dt_control_signal_init();
/* raises a signal */
void dt_control_signal_raise(const struct dt_control_signal_t *ctlsig, const dt_signal_t signal, ...);
/* connects a callback to a signal */
void dt_control_signal_connect(const struct dt_control_signal_t *ctlsig, const dt_signal_t signal,
                               GCallback cb, gpointer user_data);
/* disconnects a callback from a sink */
void dt_control_signal_disconnect(const struct dt_control_signal_t *ctlsig, GCallback cb, gpointer user_data);
/* blocks a callback */
void dt_control_signal_block_by_func(const struct dt_control_signal_t *ctlsig, GCallback cb, gpointer user_data);
/* unblocks a callback */
void dt_control_signal_unblock_by_func(const struct dt_control_signal_t *ctlsig, GCallback cb, gpointer user_data);

#define DT_DEBUG_CONTROL_SIGNAL_RAISE(ctlsig, signal, ...)                                                                       \
  do                                                                                                                             \
  {                                                                                                                              \
    if((darktable.unmuted_signal_dbg_acts & DT_DEBUG_SIGNAL_ACT_RAISE) && darktable.unmuted_signal_dbg[signal])                 \
    {                                                                                                                            \
      dt_print(DT_DEBUG_SIGNAL, "[signal] %s:%d, function %s(): raise signal %s\n", __FILE__, __LINE__, __FUNCTION__, #signal);  \
    }                                                                                                                            \
    dt_control_signal_raise(ctlsig, signal, ##__VA_ARGS__);                                                                      \
  } while (0)

#define DT_DEBUG_CONTROL_SIGNAL_CONNECT(ctlsig, signal, cb, user_data)                                                           \
  do                                                                                                                             \
  {                                                                                                                              \
    if((darktable.unmuted_signal_dbg_acts & DT_DEBUG_SIGNAL_ACT_CONNECT) && darktable.unmuted_signal_dbg[signal])                \
    {                                                                                                                            \
      dt_print(DT_DEBUG_SIGNAL, "[signal] %s:%d, function: %s() connect handler %s to signal %s\n", __FILE__, __LINE__,          \
               __FUNCTION__, #cb, #signal);                                                                                      \
    }                                                                                                                            \
    dt_control_signal_connect(ctlsig, signal, cb, user_data);                                                                    \
  } while (0)

#define DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(ctlsig, cb, user_data)                                                                \
  do                                                                                                                             \
  {                                                                                                                              \
    if(darktable.unmuted_signal_dbg_acts & DT_DEBUG_SIGNAL_ACT_DISCONNECT)                                                       \
    {                                                                                                                            \
      dt_print(DT_DEBUG_SIGNAL, "[signal] %s:%d, function: %s() disconnect handler %s\n", __FILE__, __LINE__, __FUNCTION__, #cb);\
    }                                                                                                                            \
    dt_control_signal_disconnect(ctlsig, cb, user_data);                                                                         \
  } while (0)

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
