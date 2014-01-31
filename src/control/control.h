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
#ifndef DT_CONTROL_H
#define DT_CONTROL_H

#include "common/darktable.h"
#include "common/dtpthread.h"
#include "control/settings.h"

#include <inttypes.h>
#include <gtk/gtk.h>
#ifdef _OPENMP
#  include <omp.h>
#endif

#include <gtk/gtk.h>
#include "libs/lib.h"
// #include "control/job.def"

#define DT_CONTROL_MAX_JOBS 30
#define DT_CONTROL_JOB_DEBUG
#define DT_CONTROL_DESCRIPTION_LEN 256
// reserved workers
#define DT_CTL_WORKER_RESERVED 8
#define DT_CTL_WORKER_1 0 // dev load raw
#define DT_CTL_WORKER_2 1 // dev zoom 1
#define DT_CTL_WORKER_3 2 // dev zoom fill
#define DT_CTL_WORKER_4 3 // dev zoom fit
#define DT_CTL_WORKER_5 4 // dev small prev
#define DT_CTL_WORKER_6 5 // dev prefetch
#define DT_CTL_WORKER_7 6 // scheduled jobs nice level

// A mask to strip out the Ctrl, Shift, and Alt mod keys for shortcuts
#define KEY_STATE_MASK (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK)

typedef enum dt_control_gui_mode_t
{
  DT_LIBRARY = 0,
  DT_DEVELOP,
#ifdef HAVE_GPHOTO2
  DT_CAPTURE,
#endif
#ifdef HAVE_MAP
  DT_MAP,
#endif
  DT_SLIDESHOW,
  DT_MODE_NONE
}
dt_control_gui_mode_t;

typedef GdkCursorType dt_cursor_t;

// called from gui
void *dt_control_expose(void *voidptr);
gboolean dt_control_expose_endmarker(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
void dt_control_button_pressed(double x, double y, double pressure, int which, int type, uint32_t state);
void dt_control_button_released(double x, double y, int which, uint32_t state);
void dt_control_mouse_moved(double x, double y, double pressure, int which);
void dt_control_mouse_leave();
void dt_control_mouse_enter();
int  dt_control_key_pressed(guint key, guint state);
int  dt_control_key_released(guint key, guint state);
int  dt_control_key_pressed_override(guint key, guint state);
gboolean dt_control_configure (GtkWidget *da, GdkEventConfigure *event, gpointer user_data);
void dt_control_log(const char* msg, ...);
void dt_control_log_busy_enter();
void dt_control_log_busy_leave();
void dt_control_change_cursor(dt_cursor_t cursor);
void dt_control_write_sidecar_files();
void dt_control_delete_images();
void dt_ctl_set_display_profile();

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

/** \brief smart wrapper for entering gdk critical section.
    This wrapper check is current thread context already have
    entered a gdk critical section to prevent entering the critical
    section that will reduce a application lock.

    \return true if current call have the lock, see usage in note.

    \note It's very important that dt_control_gdk_unlock()
    not is called if its locked on current thread in another place
    so its very important to use the following code semantics:
    \code
    gboolean i_have_lock = dt_control_gdk_lock();
    gtk_widget_queue_redraw();
    if(i_have_lock) dt_control_gdk_unlock();
    \endcode
*/
gboolean dt_control_gdk_lock();

/** \brief smart wrapper for leaving a gdk critical section */
void dt_control_gdk_unlock();

/** \brief returns true if we have the gdk lock */
gboolean dt_control_gdk_haslock();

void dt_ctl_switch_mode();
void dt_ctl_switch_mode_to(dt_control_gui_mode_t mode);

struct dt_control_t;
struct dt_job_t;

/* backgroundjobs proxy funcs */
/** creates a background job and returns hash id reference */
const guint *dt_control_backgroundjobs_create(const struct dt_control_t *s,guint type,const gchar *message);
/** destroys a backgroundjob using hash id reference */
void dt_control_backgroundjobs_destroy(const struct dt_control_t *s, const guint *key);
/** sets the progress of a backgroundjob using hash id reference */
void dt_control_backgroundjobs_progress(const struct dt_control_t *s, const guint *key, double progress);
/** assign a dt_job_t to a bgjob which makes it cancellable thru ui interaction */
void dt_control_backgroundjobs_set_cancellable(const struct dt_control_t *s, const guint *key,struct dt_job_t *job);

/** sets the hinter message */
void dt_control_hinter_message(const struct dt_control_t *s, const char *message);

/** turn the use of key accelerators on */
void dt_control_key_accelerators_on(struct dt_control_t *s);
/** turn the use of key accelerators on */
void dt_control_key_accelerators_off(struct dt_control_t *s);

int dt_control_is_key_accelerators_on(struct dt_control_t *s);

/**
 * smallest unit of work.
 */
struct dt_job_t;
typedef void (*dt_job_state_change_callback)(struct dt_job_t*,int state);
#define DT_JOB_STATE_INITIALIZED    0
#define DT_JOB_STATE_QUEUED         1
#define DT_JOB_STATE_RUNNING        2
#define DT_JOB_STATE_FINISHED       3
#define DT_JOB_STATE_CANCELLED      4
#define DT_JOB_STATE_DISCARDED      5
typedef struct dt_job_t
{
  int32_t (*execute) (struct dt_job_t *job);
  int32_t result;

  /* timestamp of job added to queue */
  time_t ts_added;
  /* if job is a delayed job it will be run as a backgroundjob
      and ts_execute will be the timestamp of when to start job */
  time_t ts_execute;

  dt_pthread_mutex_t state_mutex;
  dt_pthread_mutex_t wait_mutex;

  int32_t state;
  dt_job_state_change_callback state_changed_cb;
  void *user_data;

  int32_t param[32];
#ifdef DT_CONTROL_JOB_DEBUG
  char description[DT_CONTROL_DESCRIPTION_LEN];
#endif
}
dt_job_t;

/** initializes a job */
void dt_control_job_init(dt_job_t *j, const char *msg, ...);
/** setup a state callback for job. */
void dt_control_job_set_state_callback(dt_job_t *j,dt_job_state_change_callback cb,void *user_data);
void dt_control_job_print(dt_job_t *j);
/** cancel a job, running or in queue. */
void dt_control_job_cancel(dt_job_t *j);
int dt_control_job_get_state(dt_job_t *j);
/** wait for a job to finish execution. */
void dt_control_job_wait(dt_job_t *j);

//z All the accelerator keys for the key_pressed style shortcuts
typedef struct dt_control_accels_t
{
  GtkAccelKey
  filmstrip_forward, filmstrip_back,
                     lighttable_up, lighttable_down, lighttable_right,
                     lighttable_left, lighttable_center, lighttable_preview,
                     lighttable_preview_display_focus, global_sideborders, global_header,
                     slideshow_start;

} dt_control_accels_t;

#define DT_CTL_LOG_SIZE 10
#define DT_CTL_LOG_MSG_SIZE 200
#define DT_CTL_LOG_TIMEOUT 20000
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
  float progress;
  pthread_t gui_thread;
  int button_down, button_down_which, button_type;
  double button_x, button_y;
  int history_start;
  int32_t mouse_over_id;

  // TODO: move these to some darkroom struct
  // synchronized navigation
  float dev_zoom_x, dev_zoom_y, dev_zoom_scale;
  dt_dev_zoom_t dev_zoom;
  int dev_closeup;

  // message log
  int  log_pos, log_ack;
  char log_message[DT_CTL_LOG_SIZE][DT_CTL_LOG_MSG_SIZE];
  guint log_message_timeout_id;
  int  log_busy;
  dt_pthread_mutex_t log_mutex;

  // gui settings
  dt_pthread_mutex_t global_mutex, image_mutex;
  double last_expose_time;
  int key_accelerators_on;

  // xatom color profile:
  pthread_rwlock_t xprofile_lock;
  gchar *colord_profile_file;
  uint8_t *xprofile_data;
  int xprofile_size;

  // job management
  int32_t running;
  dt_pthread_mutex_t queue_mutex, cond_mutex, run_mutex;
  pthread_cond_t cond;
  int32_t num_threads;
  pthread_t *thread,kick_on_workers_thread;
  GList *queue;
  dt_job_t job_res[DT_CTL_WORKER_RESERVED];
  uint8_t new_res[DT_CTL_WORKER_RESERVED];
  pthread_t thread_res[DT_CTL_WORKER_RESERVED];

  /* proxy */
  struct
  {
    /* proxy functions for backgroundjobs ui*/
    struct
    {
      dt_lib_module_t *module;
      const guint *(*create)(dt_lib_module_t *self, int type, const gchar *message);
      void (*destroy)(dt_lib_module_t *self, const guint *key);
      void (*progress)(dt_lib_module_t *self, const guint *key, double progress);
      void (*set_cancellable)(dt_lib_module_t *self, const guint *key, dt_job_t *job);
    } backgroundjobs;

    struct
    {
      dt_lib_module_t *module;
      void (*set_message)(dt_lib_module_t *self, const gchar *message);
    } hinter;

  } proxy;

}
dt_control_t;

void dt_control_init(dt_control_t *s);

// join all worker threads.
void dt_control_shutdown(dt_control_t *s);
void dt_control_cleanup(dt_control_t *s);

// call this to quit dt
void dt_control_quit();

int dt_control_load_config(dt_control_t *c);
int dt_control_write_config(dt_control_t *c);

int32_t dt_control_run_job(dt_control_t *s);
int32_t dt_control_add_job(dt_control_t *s, dt_job_t *job);
/** adds a job to queue tagged as background job and with a delay */
int32_t dt_control_add_background_job(dt_control_t *s, dt_job_t *job, time_t delay);
int32_t dt_control_revive_job(dt_control_t *s, dt_job_t *job);
int32_t dt_control_run_job_res(dt_control_t *s, int32_t res);
int32_t dt_control_add_job_res(dt_control_t *s, dt_job_t *job, int32_t res);

/** get threadsafe running state. */
int dt_control_running();
void *dt_control_work(void *ptr);
void *dt_control_work_res(void *ptr);
int32_t dt_control_get_threadid();
int32_t dt_control_get_threadid_res();

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
  return sysconf (_SC_NPROCESSORS_ONLN);
#else
  return 1;
#endif
#endif
}

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
