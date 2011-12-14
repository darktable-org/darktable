/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DT_CTL_SETTINGS_H
#define DT_CTL_SETTINGS_H

#include "control/signal.h"
#include "common/dtpthread.h"

// thread-safe interface between core and gui.
// also serves to store user settings.

#define DT_CTL_GET_GLOBAL(x, attrib) \
{\
  dt_pthread_mutex_lock(&(darktable.control->global_mutex)); \
  x = darktable.control->global_settings.attrib; \
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex)); }

#define DT_CTL_SET_GLOBAL(attrib, x) \
{\
  dt_pthread_mutex_lock(&(darktable.control->global_mutex)); \
  if(darktable.control->global_settings.attrib != x) { \
    darktable.control->global_settings.attrib = x;     \
    dt_pthread_mutex_unlock(&(darktable.control->global_mutex)); \
    if(!strcmp(#attrib,"lib_image_mouse_over_id"))			\
      dt_control_signal_raise(darktable.signals,DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE); \
  } else \
    dt_pthread_mutex_unlock(&(darktable.control->global_mutex)); }

#define DT_CTL_GET_GLOBAL_STR(x, attrib, n) \
{\
  dt_pthread_mutex_lock(&(darktable.control->global_mutex)); \
  g_strlcpy(x, darktable.control->global_settings.attrib, n); \
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex)); }

#define DT_CTL_SET_GLOBAL_STR(attrib, x, n) \
{\
  dt_pthread_mutex_lock(&(darktable.control->global_mutex)); \
  g_strlcpy(darktable.control->global_settings.attrib, x, n); \
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex))


typedef enum dt_ctl_gui_mode_t
{
  DT_LIBRARY = 0,
  DT_DEVELOP = 1,
  DT_CAPTURE = 2,
  DT_MAP = 3,
  DT_MODE_NONE = 4
}
dt_ctl_gui_mode_t;

typedef enum dt_dev_zoom_t
{
  DT_ZOOM_FIT = 0,
  DT_ZOOM_FILL = 1,
  DT_ZOOM_1 = 2,
  DT_ZOOM_FREE = 3
}
dt_dev_zoom_t;

/** The modes of capture view
  \note in the future there will be a scanning mode...
*/
typedef enum dt_capture_mode_t
{
  DT_CAPTURE_MODE_TETHERED=0          // Only one capture mode to start with...
}
dt_capture_mode_t;
typedef char dt_dev_operation_t[20];

#define DEV_NUM_OP_PARAMS 10

typedef union dt_dev_operation_params_t
{
  int32_t i[DEV_NUM_OP_PARAMS];
  float   f[DEV_NUM_OP_PARAMS];
}
dt_dev_operation_params_t;

typedef enum dt_dev_export_format_t
{
  DT_DEV_EXPORT_JPG    = 0,
  DT_DEV_EXPORT_PNG    = 1,
  DT_DEV_EXPORT_PPM16  = 2,
  DT_DEV_EXPORT_PFM    = 3,
  DT_DEV_EXPORT_TIFF8  = 4,
  DT_DEV_EXPORT_TIFF16 = 5,
  DT_DEV_EXPORT_EXR =6
}
dt_dev_export_format_t;

typedef enum dt_lib_filter_t
{
  DT_LIB_FILTER_ALL = 0,
  DT_LIB_FILTER_STAR_NO = 1,
  DT_LIB_FILTER_STAR_1 = 2,
  DT_LIB_FILTER_STAR_2 = 3,
  DT_LIB_FILTER_STAR_3 = 4,
  DT_LIB_FILTER_STAR_4 = 5,
  DT_LIB_FILTER_STAR_5 = 6,
  DT_LIB_FILTER_REJECT = 7
}
dt_lib_filter_t;

typedef enum dt_lib_sort_t
{
  DT_LIB_SORT_FILENAME = 0,
  DT_LIB_SORT_DATETIME = 1,
  DT_LIB_SORT_RATING = 2,
  DT_LIB_SORT_ID = 3,
  DT_LIB_SORT_COLOR = 4
}
dt_lib_sort_t;

typedef struct dt_ctl_settings_t
{
  // TODO: remove most of these options, maybe the whole struct?
  // global
  int32_t version;
  char dbname[512];

  int32_t lib_image_mouse_over_id;

  // synchronized navigation
  float dev_zoom_x, dev_zoom_y, dev_zoom_scale;
  dt_dev_zoom_t dev_zoom;
  int dev_closeup;
}
dt_ctl_settings_t;

enum dt_dev_zoom_t;

#endif

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
