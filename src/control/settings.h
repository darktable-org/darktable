#ifndef DT_CTL_SETTINGS_H
#define DT_CTL_SETTINGS_H

#include <pthread.h>

// thread-safe interface between core and gui.
// also serves to store user settings.

#define DT_CTL_GET_GLOBAL(x, attrib) \
  pthread_mutex_lock(&(darktable.control->global_mutex)); \
  x = darktable.control->global_settings.attrib; \
  pthread_mutex_unlock(&(darktable.control->global_mutex))

#define DT_CTL_SET_GLOBAL(attrib, x) \
  pthread_mutex_lock(&(darktable.control->global_mutex)); \
  darktable.control->global_settings.attrib = x; \
  pthread_mutex_unlock(&(darktable.control->global_mutex))

#define DT_CTL_GET_GLOBAL_STR(x, attrib, n) \
  pthread_mutex_lock(&(darktable.control->global_mutex)); \
  strncpy(x, darktable.control->global_settings.attrib, n); \
  pthread_mutex_unlock(&(darktable.control->global_mutex))

#define DT_CTL_SET_GLOBAL_STR(attrib, x, n) \
  pthread_mutex_lock(&(darktable.control->global_mutex)); \
  strncpy(darktable.control->global_settings.attrib, x, n); \
  pthread_mutex_unlock(&(darktable.control->global_mutex))
  
#define DT_CTL_GET_IMAGE(x, attrib) \
  pthread_mutex_lock(&(darktable.control->image_mutex)); \
  x = darktable.control->image_settings.attrib; \
  pthread_mutex_unlock(&(darktable.control->image_mutex))

#define DT_CTL_SET_IMAGE(attrib, x) \
  pthread_mutex_lock(&(darktable.control->image_mutex)); \
  darktable.control->image_settings.attrib = x; \
  pthread_mutex_unlock(&(darktable.control->image_mutex))

typedef enum dt_ctl_gui_mode_t
{
  DT_LIBRARY = 1,
  DT_DEVELOP = 2
}
dt_ctl_gui_mode_t;

typedef enum dt_dev_zoom_t
{
  DT_ZOOM_FIT = 0,
  DT_ZOOM_FILL = 1,
  DT_ZOOM_1 = 2
}
dt_dev_zoom_t;

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
  DT_DEV_EXPORT_JPG,
  DT_DEV_EXPORT_PNG,
  DT_DEV_EXPORT_PPM16,
  DT_DEV_EXPORT_PFM
}
dt_dev_export_format_t;

typedef enum dt_lib_filter_t
{
  DT_LIB_FILTER_ALL = 0,
  DT_LIB_FILTER_STAR_1 = 1,
  DT_LIB_FILTER_STAR_2 = 2,
  DT_LIB_FILTER_STAR_3 = 3,
  DT_LIB_FILTER_STAR_4 = 4,
  DT_LIB_FILTER_STAR_NO = 5
}
dt_lib_filter_t;

typedef enum dt_lib_sort_t
{
  DT_LIB_SORT_FILENAME,
  DT_LIB_SORT_DATETIME,
  DT_LIB_SORT_RATING
}
dt_lib_sort_t;

typedef struct dt_ctl_settings_t
{
  // global
  int32_t version;
  char dbname[512];

  // develop
  float dev_gamma_linear, dev_gamma_gamma;
  dt_dev_operation_t dev_op; // currently active op
  dt_dev_operation_params_t dev_op_params; // params for currently active op
  dt_dev_export_format_t dev_export_format;

  // library
  float lib_zoom, lib_zoom_x, lib_zoom_y;
  int32_t lib_center, lib_pan, lib_track;
  int32_t lib_image_mouse_over_id;
  dt_lib_filter_t lib_filter;
  dt_lib_sort_t lib_sort;

  // navigation
  float dev_zoom_x, dev_zoom_y;
  dt_dev_zoom_t dev_zoom;
  int dev_closeup;

  // gui
  // widget settings for lib/dev view, fullscreen, window geometry
  dt_ctl_gui_mode_t gui;
  int32_t gui_x, gui_y, gui_w, gui_h;
  uint8_t gui_fullscreen;
  uint8_t gui_left, gui_right, gui_bottom, gui_top;
  uint8_t gui_navigation, gui_library, gui_history;
  uint8_t gui_histogram, gui_tonecurve, gui_gamma, gui_hsb, gui_export;
}
dt_ctl_settings_t;

enum dt_dev_zoom_t;
// gui controlled per-image parameters
typedef struct dt_ctl_image_settings_t
{
  // gamma correction
  float dev_gamma_linear, dev_gamma_gamma;
}
dt_ctl_image_settings_t;

#endif
