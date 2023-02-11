/*
    This file is part of darktable,
    Copyright (C) 2009-2021 darktable developers.

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

#include "common/darktable.h"
#include "common/dtpthread.h"
#include "common/action.h"
#include "control/settings.h"

#include <gtk/gtk.h>
#include <inttypes.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#include "control/jobs.h"
#include "control/progress.h"
#include "libs/lib.h"
#include <gtk/gtk.h>

#ifdef _WIN32
#include <shobjidl.h>
#endif

struct dt_lib_backgroundjob_element_t;

typedef GdkCursorType dt_cursor_t;

// called from gui
void *dt_control_expose(void *voidptr);
gboolean dt_control_draw_endmarker(GtkWidget *widget, cairo_t *crf, gpointer user_data);
void dt_control_button_pressed(double x, double y, double pressure, int which, int type, uint32_t state);
void dt_control_button_released(double x, double y, int which, uint32_t state);
void dt_control_mouse_moved(double x, double y, double pressure, int which);
void dt_control_mouse_leave();
void dt_control_mouse_enter();
int dt_control_key_pressed_override(guint key, guint state);
gboolean dt_control_configure(GtkWidget *da, GdkEventConfigure *event, gpointer user_data);
void dt_control_log(const char *msg, ...) __attribute__((format(printf, 1, 2)));
void dt_toast_log(const char *msg, ...) __attribute__((format(printf, 1, 2)));
void dt_toast_markup_log(const char *msg, ...) __attribute__((format(printf, 1, 2)));
void dt_control_log_busy_enter();
void dt_control_toast_busy_enter();
void dt_control_log_busy_leave();
void dt_control_toast_busy_leave();
void dt_control_draw_busy_msg(cairo_t *cr, int width, int height);
// disable the possibility to change the cursor shape with dt_control_change_cursor
void dt_control_forbid_change_cursor();
// enable the possibility to change the cursor shape with dt_control_change_cursor
void dt_control_allow_change_cursor();
void dt_control_change_cursor(dt_cursor_t cursor);
void dt_control_write_sidecar_files();
void dt_control_delete_images();

/** \brief request redraw of the workspace.
    This redraws the whole workspace within a gdk critical
    section to prevent several threads to carry out a redraw
    which will end up in crashes.
 */
void dt_control_queue_redraw();

/** \brief request redraw of center window.
    This redraws the center view within a gdk critical section
    to prevent several threads to carry out the redraw.
*/
void dt_control_queue_redraw_center();

/** \brief threadsafe request of redraw of specific widget.
    Use this function if you need to redraw a specific widget
    if your current thread context is not gtk main thread.
*/
void dt_control_queue_redraw_widget(GtkWidget *widget);

/** \brief request redraw of the navigation widget.
    This redraws the wiget of the navigation module.
 */
void dt_control_navigation_redraw();

/** \brief request redraw of the log widget.
    This redraws the message label.
 */
void dt_control_log_redraw();

/** \brief request redraw of the toast widget.
    This redraws the message label.
 */
void dt_control_toast_redraw();

void dt_ctl_switch_mode();
void dt_ctl_switch_mode_to(const char *mode);
void dt_ctl_switch_mode_to_by_view(const dt_view_t *view);

struct dt_control_t;

/** sets the hinter message */
void dt_control_hinter_message(const struct dt_control_t *s, const char *message);

#define DT_CTL_LOG_SIZE 10
#define DT_CTL_LOG_MSG_SIZE 1000
#define DT_CTL_LOG_TIMEOUT 5000
#define DT_CTL_TOAST_SIZE 10
#define DT_CTL_TOAST_MSG_SIZE 300
#define DT_CTL_TOAST_TIMEOUT 1500
/**
 * this manages everything time-consuming.
 * distributes the jobs on all processors,
 * performs scheduling.
 */
typedef struct dt_control_t
{
  gboolean accel_initialising;

  dt_action_t *actions, actions_global,
               actions_views, actions_thumb,
               actions_libs, actions_format, actions_storage,
               actions_iops, actions_blend, actions_focus,
               actions_lua, actions_fallbacks, *actions_modifiers;

  GHashTable *widgets, *combo_introspection, *combo_list;
  GSequence *shortcuts;
  gboolean enable_fallbacks;
  GtkWidget *mapping_widget;
  gboolean confirm_mapping;
  dt_action_element_t element;
  GPtrArray *widget_definitions;
  GSList *input_drivers;

  char vimkey[256];
  int vimkey_cnt;

  // gui related stuff
  double tabborder;
  int32_t width, height;
  pthread_t gui_thread;
  int button_down, button_down_which, button_type;
  double button_x, button_y;
  int history_start;
  int32_t mouse_over_id;
  gboolean lock_cursor_shape;

  // TODO: move these to some darkroom struct
  // synchronized navigation
  float dev_zoom_x, dev_zoom_y, dev_zoom_scale;
  dt_dev_zoom_t dev_zoom;
  int dev_closeup;

  // message log
  int log_pos, log_ack;
  char log_message[DT_CTL_LOG_SIZE][DT_CTL_LOG_MSG_SIZE];
  guint log_message_timeout_id;
  int log_busy;
  dt_pthread_mutex_t log_mutex;

  // toast log
  int toast_pos, toast_ack;
  char toast_message[DT_CTL_TOAST_SIZE][DT_CTL_TOAST_MSG_SIZE];
  guint toast_message_timeout_id;
  int toast_busy;
  dt_pthread_mutex_t toast_mutex;

  // gui settings
  dt_pthread_mutex_t global_mutex, image_mutex;
  double last_expose_time;

  // job management
  int32_t running;
  gboolean export_scheduled;
  dt_pthread_mutex_t queue_mutex, cond_mutex, run_mutex;
  pthread_cond_t cond;
  int32_t num_threads;
  pthread_t *thread, kick_on_workers_thread, update_gphoto_thread;
  dt_job_t **job;

  GList *queues[DT_JOB_QUEUE_MAX];
  size_t queue_length[DT_JOB_QUEUE_MAX];

  dt_pthread_mutex_t res_mutex;
  dt_job_t *job_res[DT_CTL_WORKER_RESERVED];
  uint8_t new_res[DT_CTL_WORKER_RESERVED];
  pthread_t thread_res[DT_CTL_WORKER_RESERVED];

  struct
  {
    GList *list;
    size_t list_length;
    size_t n_progress_bar;
    double global_progress;
    dt_pthread_mutex_t mutex;

#ifdef _WIN32
    ITaskbarList3 *taskbarlist;
#endif

    // these proxy functions should ONLY be used by control/process.c!
    struct
    {
      dt_lib_module_t *module;
      void *(*added)(dt_lib_module_t *self, gboolean has_progress_bar, const gchar *message);
      void (*destroyed)(dt_lib_module_t *self, struct dt_lib_backgroundjob_element_t *instance);
      void (*cancellable)(dt_lib_module_t *self, struct dt_lib_backgroundjob_element_t *instance,
                          dt_progress_t *progress);
      void (*updated)(dt_lib_module_t *self, struct dt_lib_backgroundjob_element_t *instance, double value);
      void (*message_updated)(dt_lib_module_t *self, struct dt_lib_backgroundjob_element_t *instance,
                              const char *message);
    } proxy;

  } progress_system;

  /* proxy */
  struct
  {

    struct
    {
      dt_lib_module_t *module;
      void (*set_message)(dt_lib_module_t *self, const gchar *message);
    } hinter;

  } proxy;

} dt_control_t;

void dt_control_init(dt_control_t *s);

// join all worker threads.
void dt_control_shutdown(dt_control_t *s);
void dt_control_cleanup(dt_control_t *s);

// call this to quit dt
void dt_control_quit();

/** get threadsafe running state. */
int dt_control_running();

// thread-safe interface between core and gui.
// is the locking really needed?
int32_t dt_control_get_mouse_over_id();
void dt_control_set_mouse_over_id(int32_t value);

float dt_control_get_dev_zoom_x();
void dt_control_set_dev_zoom_x(float value);

float dt_control_get_dev_zoom_y();
void dt_control_set_dev_zoom_y(float value);

float dt_control_get_dev_zoom_scale();
void dt_control_set_dev_zoom_scale(float value);

int dt_control_get_dev_closeup();
void dt_control_set_dev_closeup(int value);

dt_dev_zoom_t dt_control_get_dev_zoom();
void dt_control_set_dev_zoom(dt_dev_zoom_t value);

static inline int32_t dt_ctl_get_num_procs()
{
#ifdef _OPENMP
  return omp_get_num_procs();
#else
#ifdef _SC_NPROCESSORS_ONLN
  return sysconf(_SC_NPROCESSORS_ONLN);
#else
  return 1;
#endif
#endif
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
