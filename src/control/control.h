#ifndef DT_CONTROL_H
#define DT_CONTROL_H

#include <inttypes.h>
#include <pthread.h>
#ifdef _OPENMP
#  include <omp.h>
#endif

#include "control/settings.h"
// #include "control/job.def"

#define DT_CONTROL_MAX_JOBS 100
#define DT_CONTROL_JOB_DEBUG
#define DT_CONTROL_DESCRIPTION_LEN 256
// reserved workers
#define DT_CTL_WORKER_RESERVED 6
#define DT_CTL_WORKER_1 0 // dev load raw
#define DT_CTL_WORKER_2 1 // dev zoom 1
#define DT_CTL_WORKER_3 2 // dev zoom fill
#define DT_CTL_WORKER_4 3 // dev zoom fit
#define DT_CTL_WORKER_5 4 // dev small prev
#define DT_CTL_WORKER_6 5 // dev prefetch


// called from gui
void *dt_control_expose(void *voidptr);
void dt_control_button_pressed(double x, double y, int which, int type, uint32_t state);
void dt_control_button_released(double x, double y, int which, uint32_t state);
void dt_control_mouse_moved(double x, double y, int which);
void dt_control_mouse_leave();
int  dt_control_key_pressed(uint32_t which);
void dt_control_configure(int32_t width, int32_t height);
void dt_control_gui_queue_draw();

// called from core
void dt_control_queue_draw();
void dt_control_get_tonecurve(uint16_t *tonecurve, dt_ctl_image_settings_t *settings);
void dt_control_add_history_item(int32_t num, const char *label);
void dt_control_clear_history_items(int32_t num);
void dt_control_update_recent_films();

void dt_control_save_gui_settings(dt_ctl_gui_mode_t mode);
void dt_control_restore_gui_settings(dt_ctl_gui_mode_t mode);

/**
 * smalles unit of work.
 */
struct dt_job_t;
typedef struct dt_job_t
{
  void (*execute)(struct dt_job_t *job);
  int32_t param[32];
#ifdef DT_CONTROL_JOB_DEBUG
  char description[DT_CONTROL_DESCRIPTION_LEN];
#endif
}
dt_job_t;

void dt_control_job_init(dt_job_t *j, const char *msg, ...);
void dt_control_job_print(dt_job_t *j);


/**
 * this manages everything time-consuming.
 * distributes the jobs on all processors,
 * performs scheduling.
 */
typedef struct dt_control_t
{
  // gui related stuff
  double tabborder;
  int32_t width, height;
  float progress;
  pthread_t gui_thread;
  int button_down;
  double button_x, button_y;
  
  // gui settings
  dt_ctl_settings_t global_settings, global_defaults;
  dt_ctl_image_settings_t image_settings, image_defaults;
  pthread_mutex_t global_mutex, image_mutex;

  // job management
  int32_t running;
  pthread_mutex_t queue_mutex, cond_mutex;
  pthread_cond_t cond;
  int32_t num_threads;
  pthread_t *thread;
  dt_job_t job[DT_CONTROL_MAX_JOBS];
  int32_t idle[DT_CONTROL_MAX_JOBS];
  int32_t queued[DT_CONTROL_MAX_JOBS];
  int32_t idle_top, queued_top;
  dt_job_t job_res[DT_CTL_WORKER_RESERVED];
  uint8_t new_res[DT_CTL_WORKER_RESERVED];
  pthread_t thread_res[DT_CTL_WORKER_RESERVED];
}
dt_control_t;

void dt_control_init(dt_control_t *s);
void dt_control_cleanup(dt_control_t *s);

int dt_control_load_config(dt_control_t *c);
int dt_control_write_config(dt_control_t *c);

int32_t dt_control_run_job(dt_control_t *s);
int32_t dt_control_add_job(dt_control_t *s, dt_job_t *job);
int32_t dt_control_run_job_res(dt_control_t *s, int32_t res);
int32_t dt_control_add_job_res(dt_control_t *s, dt_job_t *job, int32_t res);

void *dt_control_work(void *ptr);
void *dt_control_work_res(void *ptr);
int32_t dt_control_get_threadid();
int32_t dt_control_get_threadid_res();

static inline int32_t dt_ctl_get_num_procs()
{
#ifdef _OPENMP
  return omp_get_num_procs();
#else
  return 1;
#endif
}

#endif
