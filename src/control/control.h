/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011 henrik andersson.

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

// A mask to strip out the Ctrl, Shift, and Alt mod keys for shortcuts
#define KEY_STATE_MASK (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK)

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
int dt_control_key_pressed(guint key, guint state);
int dt_control_key_released(guint key, guint state);
int dt_control_key_pressed_override(guint key, guint state);
gboolean dt_control_configure(GtkWidget *da, GdkEventConfigure *event, gpointer user_data);
void dt_control_log(const char *msg, ...) __attribute__((format(printf, 1, 2)));
void dt_control_log_busy_enter();
void dt_control_log_busy_leave();
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

void dt_ctl_switch_mode();
void dt_ctl_switch_mode_to(const char *mode);
void dt_ctl_switch_mode_to_by_view(const dt_view_t *view);

struct dt_control_t;

/** sets the hinter message */
void dt_control_hinter_message(const struct dt_control_t *s, const char *message);

/** turn the use of key accelerators on */
void dt_control_key_accelerators_on(struct dt_control_t *s);
/** turn the use of key accelerators on */
void dt_control_key_accelerators_off(struct dt_control_t *s);

int dt_control_is_key_accelerators_on(struct dt_control_t *s);

// All the accelerator keys for the key_pressed style shortcuts
typedef struct dt_control_accels_t
{
  GtkAccelKey filmstrip_forward, filmstrip_back, lighttable_up, lighttable_down, lighttable_right, lighttable_left,
      lighttable_center, lighttable_preview, lighttable_preview_display_focus, lighttable_preview_sticky,
      lighttable_preview_sticky_focus, lighttable_preview_sticky_exit, global_sideborders, global_header,
      darkroom_preview, slideshow_start, global_zoom_in, global_zoom_out, darkroom_skip_mouse_events;
} dt_control_accels_t;

#define DT_CTL_LOG_SIZE 10
#define DT_CTL_LOG_MSG_SIZE 200
#define DT_CTL_LOG_TIMEOUT 5000
/**
 * this manages everything time-consuming.
 * distributes the jobs on all processors,
 * performs scheduling.
 */
typedef struct dt_control_t
{
  // Keyboard accelerator groups
  GtkAccelGroup *accelerators;

  // Accelerator group path lists
  GSList *accelerator_list;

  // Cached accelerator keys for key_pressed shortcuts
  dt_control_accels_t accels;

  // Accel remapping data
  gchar *accel_remap_str;
  GtkTreePath *accel_remap_path;

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

  // gui settings
  dt_pthread_mutex_t global_mutex, image_mutex;
  double last_expose_time;
  int key_accelerators_on;

  // job management
  int32_t running;
  gboolean export_scheduled;
  dt_pthread_mutex_t queue_mutex, cond_mutex, run_mutex;
  pthread_cond_t cond;
  int32_t num_threads;
  pthread_t *thread, kick_on_workers_thread;
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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
