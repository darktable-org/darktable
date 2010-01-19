#ifndef DT_CONTROL_H
#define DT_CONTROL_H

#include <inttypes.h>
#include <pthread.h>
#ifdef _OPENMP
#  include <omp.h>
#endif

#include "control/settings.h"
#include <gtk/gtk.h>
// #include "control/job.def"

#define DT_CONTROL_MAX_JOBS 300
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

// keycodes mapped to dvorak keyboard layout for easier usage
#if defined(__APPLE__) || defined(__MACH__)
#if 0
#define KEYCODE_a            8   // mac keycodes X11 :(
#define KEYCODE_o            9
#define KEYCODE_e           10
#define KEYCODE_apostrophe  20
#define KEYCODE_comma       21
#define KEYCODE_period      22
#define KEYCODE_1           26
#define KEYCODE_2           27
#define KEYCODE_3           28
#define KEYCODE_Escape      61
#define KEYCODE_Caps        -1
#define KEYCODE_F11        111
#define KEYCODE_Up         134
#define KEYCODE_Down       133
#define KEYCODE_Left        78
#define KEYCODE_Right       74
#define KEYCODE_Tab         56
#else
#define KEYCODE_a           0   // mac keycodes carbon :)
#define KEYCODE_o           1   // s
#define KEYCODE_e           2   // d
#define KEYCODE_apostrophe  12  // q
#define KEYCODE_comma       13  // w
#define KEYCODE_period      14  // e .. in qwerty :)
#define KEYCODE_1           18
#define KEYCODE_2           19
#define KEYCODE_3           20
#define KEYCODE_Escape      53
#define KEYCODE_Caps        57
#define KEYCODE_F11        103
#define KEYCODE_Up         126
#define KEYCODE_Down       125
#define KEYCODE_Left       123
#define KEYCODE_Right      124
#define KEYCODE_Tab         48
#define KEYCODE_space       49
#endif
#else
#define KEYCODE_a           38
#define KEYCODE_o           39  
#define KEYCODE_e           40  
#define KEYCODE_apostrophe  24  
#define KEYCODE_comma       25  
#define KEYCODE_period      26  
#define KEYCODE_1           10  
#define KEYCODE_2           11  
#define KEYCODE_3           12  
#define KEYCODE_Escape       9  
#define KEYCODE_Caps        66
#define KEYCODE_F11         95  
#define KEYCODE_Up         111  
#define KEYCODE_Down       116  
#define KEYCODE_Left       113  
#define KEYCODE_Right      114  
#define KEYCODE_Tab         23  
#endif

typedef GdkCursorType dt_cursor_t;

// called from gui
void *dt_control_expose(void *voidptr);
gboolean dt_control_expose_endmarker(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
void dt_control_button_pressed(double x, double y, int which, int type, uint32_t state);
void dt_control_button_released(double x, double y, int which, uint32_t state);
void dt_control_mouse_moved(double x, double y, int which);
void dt_control_mouse_leave();
int  dt_control_key_pressed(uint16_t which);
int  dt_control_key_pressed_override(uint16_t which);
gboolean dt_control_configure (GtkWidget *da, GdkEventConfigure *event, gpointer user_data);
void dt_control_gui_queue_draw();
void dt_control_log(const char* msg, ...);
void dt_control_change_cursor(dt_cursor_t cursor);
void dt_control_write_dt_files();
void dt_control_delete_images();
void dt_ctl_get_display_profile(GtkWidget *widget, guint8 **buffer, gint *buffer_size);

// called from core
void dt_control_add_history_item(int32_t num, const char *label);
void dt_control_clear_history_items(int32_t num);
void dt_control_update_recent_films();

// could be both
void dt_control_queue_draw_all();
void dt_control_queue_draw(GtkWidget *widget);
void dt_ctl_switch_mode();
void dt_ctl_switch_mode_to(dt_ctl_gui_mode_t mode);

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


#define DT_CTL_LOG_SIZE 10
#define DT_CTL_LOG_MSG_SIZE 200
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
  int button_down, button_down_which;
  double button_x, button_y;
  int history_start;

  // message log
  int  log_pos, log_ack;
  char log_message[DT_CTL_LOG_SIZE][DT_CTL_LOG_MSG_SIZE];
  pthread_mutex_t log_mutex;
  
  // gui settings
  dt_ctl_settings_t global_settings, global_defaults;
  pthread_mutex_t global_mutex, image_mutex;

  // xatom color profile:
  uint8_t *xprofile_data;
  int xprofile_size;

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
// join all worker threads.
void dt_control_shutdown(dt_control_t *s);
void dt_control_cleanup(dt_control_t *s);

int dt_control_load_config(dt_control_t *c);
int dt_control_write_config(dt_control_t *c);

int32_t dt_control_run_job(dt_control_t *s);
int32_t dt_control_add_job(dt_control_t *s, dt_job_t *job);
int32_t dt_control_revive_job(dt_control_t *s, dt_job_t *job);
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
#ifdef _SC_NPROCESSORS_ONLN
  return sysconf (_SC_NPROCESSORS_ONLN);
#else
  return 1;
#endif
#endif
}

#endif
