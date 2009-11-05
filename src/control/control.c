#include "control/control.h"
#include "develop/develop.h"
#include "common/darktable.h"
#include "views/view.h"
#include "gui/gtk.h"
#include "gui/draw.h"

#include <stdlib.h>
#include <strings.h>
#include <assert.h>
#include <math.h>
#include <string.h>
#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif


void dt_ctl_settings_init(dt_control_t *s)
{
  // same thread as init
  s->gui_thread = pthread_self();
  // init global defaults.
  pthread_mutex_init(&(s->global_mutex), NULL);
  pthread_mutex_init(&(s->image_mutex), NULL);

  s->global_settings.version = DT_VERSION;

  s->global_settings.gui = DT_LIBRARY;
  s->global_settings.gui_fullscreen = 0;
  // expand everything
  // except top/bottm, retract everything but the lib button in lib mode
  // retract lib button in dev mode
  s->global_settings.gui_x = 0;
  s->global_settings.gui_y = 0;
  s->global_settings.gui_w = 640;
  s->global_settings.gui_h = 480;
  s->global_settings.gui_top = s->global_settings.gui_bottom = 0;
  s->global_settings.gui_left = s->global_settings.gui_right = -1;
  s->global_settings.gui_export = 
  s->global_settings.gui_library = 1<<DT_LIBRARY;
  s->global_settings.gui_navigation = 
  s->global_settings.gui_history = s->global_settings.gui_histogram =
  s->global_settings.gui_tonecurve = s->global_settings.gui_gamma =
  s->global_settings.gui_hsb = 1<<DT_DEVELOP;

  s->global_settings.lib_zoom = 13;
  s->global_settings.lib_zoom_x = 0.0f;
  s->global_settings.lib_zoom_y = 0.0f;
  s->global_settings.lib_center = 0;
  s->global_settings.lib_pan = 0;
  s->global_settings.lib_track = 0;
  s->global_settings.lib_image_mouse_over_id = -1;
  s->global_settings.lib_sort = DT_LIB_SORT_FILENAME;
  s->global_settings.lib_filter = DT_LIB_FILTER_ALL;

  s->global_settings.dev_closeup = 0;
  s->global_settings.dev_zoom_x = 0;
  s->global_settings.dev_zoom_y = 0;
  s->global_settings.dev_zoom = DT_ZOOM_FIT;

  s->global_settings.dev_export_format = DT_DEV_EXPORT_JPG;
  s->global_settings.dev_export_quality = 97;

  strncpy(s->global_settings.dev_op, "original", 20);
  
  s->global_settings.dev_gamma_linear = 0.1;
  s->global_settings.dev_gamma_gamma = 0.45;
  
  memcpy(&(s->global_defaults), &(s->global_settings), sizeof(dt_ctl_settings_t));
}

int dt_control_load_config(dt_control_t *c)
{
  int rc;
  sqlite3_stmt *stmt;
  // unsafe, fast write:
  rc = sqlite3_exec(darktable.db, "PRAGMA synchronous=off", NULL, NULL, NULL);
  rc = sqlite3_prepare_v2(darktable.db, "select settings from settings", -1, &stmt, NULL);
  if(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
  {
    pthread_mutex_lock(&(darktable.control->global_mutex));
    darktable.control->global_settings.version = -1;
    const void *set = sqlite3_column_blob(stmt, 0);
    int len = sqlite3_column_bytes(stmt, 0);
    if(len == sizeof(dt_ctl_settings_t)) memcpy(&(darktable.control->global_settings), set, len);
    rc = sqlite3_finalize(stmt);

    if(darktable.control->global_settings.version != DT_VERSION)
    {
      fprintf(stderr, "[load_config] wrong version %d (should be %d), substituting defaults.\n", darktable.control->global_settings.version, DT_VERSION);
      memcpy(&(darktable.control->global_settings), &(darktable.control->global_defaults), sizeof(dt_ctl_settings_t));
      pthread_mutex_unlock(&(darktable.control->global_mutex));
      // drop all, restart. TODO: freeze this version or have update facility!
      rc = sqlite3_prepare_v2(darktable.db, "drop table settings", -1, &stmt, NULL); rc = sqlite3_step(stmt); rc = sqlite3_finalize(stmt);
      rc = sqlite3_prepare_v2(darktable.db, "drop table film_rolls", -1, &stmt, NULL); rc = sqlite3_step(stmt); rc = sqlite3_finalize(stmt);
      rc = sqlite3_prepare_v2(darktable.db, "drop table images", -1, &stmt, NULL); rc = sqlite3_step(stmt); rc = sqlite3_finalize(stmt);
      rc = sqlite3_prepare_v2(darktable.db, "drop table selected_images", -1, &stmt, NULL); rc = sqlite3_step(stmt); rc = sqlite3_finalize(stmt);
      rc = sqlite3_prepare_v2(darktable.db, "drop table mipmaps", -1, &stmt, NULL); rc = sqlite3_step(stmt); rc = sqlite3_finalize(stmt);
      rc = sqlite3_prepare_v2(darktable.db, "drop table history", -1, &stmt, NULL); rc = sqlite3_step(stmt); rc = sqlite3_finalize(stmt);
      return dt_control_load_config(c);
    }
    else
    {
      pthread_mutex_unlock(&(darktable.control->global_mutex));
      // TODO: get last from sql query!
      // dt_film_roll_open(darktable.library->film, film_id);
      // weg: dt_film_roll_import(darktable.library->film, darktable.control->global_settings.lib_last_film);
    }
  }
  else
  { // db not yet there, create it
    rc = sqlite3_finalize(stmt);
    rc = sqlite3_exec(darktable.db, "create table settings (settings blob)", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table film_rolls (id integer primary key, datetime_accessed char(20), folder varchar(1024))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table images (id integer primary key, film_id integer, width int, height int, filename varchar(256), maker varchar(30), model varchar(30), lens varchar(30), exposure real, aperture real, iso real, focal_length real, datetime_taken char(20), flags integer, output_width integer, output_height integer, foreign key(film_id) references film_rolls(id))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table mipmaps (imgid int, level int, data blob, primary key(imgid, level), foreign key(imgid) references images(id))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table selected_images (imgid integer, foreign key(imgid) references images(id))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table history (imgid integer, num integer, module integer, operation varchar(256), op_params blob, enabled integer, foreign key(imgid) references images(id))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);

    // add dummy film roll for single images
    char datetime[20];
    dt_gettime(datetime);
    rc = sqlite3_prepare_v2(darktable.db, "insert into film_rolls (id, datetime_accessed, folder) values (null, ?1, 'single images')", -1, &stmt, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_bind_text(stmt, 1, datetime, strlen(datetime), SQLITE_STATIC);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);

    // TODO: - table tags "tag str" "key#"
    // TODO: - table frequency tagXtag?
    // TODO: - table tag X film_roll
    // TODO: - table tag X image
    rc = sqlite3_prepare_v2(darktable.db, "insert into settings (settings) values (?1)", -1, &stmt, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_bind_blob(stmt, 1, &(darktable.control->global_defaults), sizeof(dt_ctl_settings_t), SQLITE_STATIC);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
  }
  DT_CTL_SET_GLOBAL(gui, DT_LIBRARY);
  int width, height;
  DT_CTL_GET_GLOBAL(width, gui_w);
  DT_CTL_GET_GLOBAL(height, gui_h);
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "main_window");
  gtk_window_resize(GTK_WINDOW(widget), width, height);
  dt_control_restore_gui_settings(DT_LIBRARY);
  dt_control_update_recent_films();
  return 0;
}

int dt_control_write_config(dt_control_t *c)
{
  dt_ctl_gui_mode_t gui;
  DT_CTL_GET_GLOBAL(gui, gui);
  dt_control_save_gui_settings(gui);

  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "main_window");
  DT_CTL_SET_GLOBAL(gui_x, widget->allocation.x);
  DT_CTL_SET_GLOBAL(gui_y, widget->allocation.y);
  DT_CTL_SET_GLOBAL(gui_w, widget->allocation.width);
  DT_CTL_SET_GLOBAL(gui_h, widget->allocation.height);

  int rc;
  sqlite3_stmt *stmt;
  pthread_mutex_lock(&(darktable.control->global_mutex));
  rc = sqlite3_prepare_v2(darktable.db, "update settings set settings = ?1 where rowid = 1", -1, &stmt, NULL);
  rc = sqlite3_bind_blob(stmt, 1, &(darktable.control->global_settings), sizeof(dt_ctl_settings_t), SQLITE_STATIC);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  pthread_mutex_unlock(&(darktable.control->global_mutex));
  return 0;
}

void dt_control_init(dt_control_t *s)
{
  dt_ctl_settings_init(s);
  s->progress = 200.0f;

  pthread_cond_init(&s->cond, NULL);
  pthread_mutex_init(&s->cond_mutex, NULL);
  pthread_mutex_init(&s->queue_mutex, NULL);

  int k; for(k=0;k<DT_CONTROL_MAX_JOBS;k++) s->idle[k] = k;
  s->idle_top = DT_CONTROL_MAX_JOBS;
  s->queued_top = 0;
  // start threads
  s->num_threads = DT_CTL_WORKER_RESERVED + 6;//4; // TODO: omp_get procs equiv.!
  s->thread = (pthread_t *)malloc(sizeof(pthread_t)*s->num_threads);
  s->running = 1;
  for(k=0;k<s->num_threads;k++)
    pthread_create(s->thread + k, NULL, dt_control_work, s);
  for(k=0;k<DT_CTL_WORKER_RESERVED;k++)
  {
    s->new_res[k] = 0;
    pthread_create(s->thread_res + k, NULL, dt_control_work_res, s);
  }
  s->button_down = 0;
  s->button_down_which = 0;
  s->history_start = 1;
}

void dt_control_change_cursor(dt_cursor_t curs)
{
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "main_window");
  GdkCursor* cursor = gdk_cursor_new(curs);
  gdk_window_set_cursor(widget->window, cursor);
  gdk_cursor_destroy(cursor);
}

void dt_control_shutdown(dt_control_t *s)
{
  pthread_mutex_lock(&s->cond_mutex);
  s->running = 0;
  pthread_mutex_unlock(&s->cond_mutex);
  pthread_cond_broadcast(&s->cond);
  // gdk_threads_leave();
  int k; for(k=0;k<s->num_threads;k++) 
    // pthread_kill(s->thread[k], 9);
    pthread_join(s->thread[k], NULL);
  for(k=0;k<DT_CTL_WORKER_RESERVED;k++)
    // pthread_kill(s->thread_res[k], 9);
    pthread_join(s->thread_res[k], NULL);
  // gdk_threads_enter();
}

void dt_control_cleanup(dt_control_t *s)
{
  pthread_mutex_destroy(&s->queue_mutex);
  pthread_mutex_destroy(&s->cond_mutex);
}

void dt_control_job_init(dt_job_t *j, const char *msg, ...)
{
#ifdef DT_CONTROL_JOB_DEBUG
  va_list ap;
  va_start(ap, msg);
  vsnprintf(j->description, DT_CONTROL_DESCRIPTION_LEN, msg, ap);
  va_end(ap);
#endif
}

void dt_control_job_print(dt_job_t *j)
{
#ifdef DT_CONTROL_JOB_DEBUG
  dt_print(DT_DEBUG_CONTROL, "%s", j->description);
#endif
}

int32_t dt_control_run_job_res(dt_control_t *s, int32_t res)
{
  assert(res < DT_CTL_WORKER_RESERVED && res >= 0);
  dt_job_t *j = NULL;
  pthread_mutex_lock(&s->queue_mutex);
  if(s->new_res[res]) j = s->job_res + res;
  s->new_res[res] = 0;
  pthread_mutex_unlock(&s->queue_mutex);
  if(!j) return -1;

  dt_print(DT_DEBUG_CONTROL, "[run_job_res %d] ", (int)pthread_self());
  dt_control_job_print(j);
  dt_print(DT_DEBUG_CONTROL, "\n");

  j->execute(j);
  return 0;
}

int32_t dt_control_run_job(dt_control_t *s)
{
  dt_job_t *j;
  int32_t i;
  pthread_mutex_lock(&s->queue_mutex);
  // dt_print(DT_DEBUG_CONTROL, "[run_job] %d\n", s->queued_top);
  if(s->queued_top == 0)
  {
    pthread_mutex_unlock(&s->queue_mutex);
    return -1;
  }
  i = s->queued[--s->queued_top];
  j = s->job + i;
  pthread_mutex_unlock(&s->queue_mutex);

  dt_print(DT_DEBUG_CONTROL, "[run_job %d] ", dt_control_get_threadid());
  dt_control_job_print(j);
  dt_print(DT_DEBUG_CONTROL, "\n");
  j->execute(j);

  pthread_mutex_lock(&s->queue_mutex);
  assert(s->idle_top < DT_CONTROL_MAX_JOBS);
  s->idle[s->idle_top++] = i;
  pthread_mutex_unlock(&s->queue_mutex);
  return 0;
}

int32_t dt_control_add_job_res(dt_control_t *s, dt_job_t *job, int32_t res)
{
  // TODO: pthread cancel and restart in tough cases?
  pthread_mutex_lock(&s->queue_mutex);
  dt_print(DT_DEBUG_CONTROL, "[add_job_res] %d ", res);
  dt_control_job_print(job);
  dt_print(DT_DEBUG_CONTROL, "\n");
  s->job_res[res] = *job;
  s->new_res[res] = 1;
  pthread_mutex_unlock(&s->queue_mutex);
  pthread_mutex_lock(&s->cond_mutex);
  pthread_cond_broadcast(&s->cond);
  pthread_mutex_unlock(&s->cond_mutex);
  return 0;
}

int32_t dt_control_add_job(dt_control_t *s, dt_job_t *job)
{
  int32_t i;
  pthread_mutex_lock(&s->queue_mutex);
  dt_print(DT_DEBUG_CONTROL, "[add_job] %d ", s->idle_top);
  dt_control_job_print(job);
  dt_print(DT_DEBUG_CONTROL, "\n");
  if(s->idle_top != 0)
  {
    i = --s->idle_top;
    s->job[s->idle[i]] = *job;
    s->queued[s->queued_top++] = s->idle[i];
    pthread_mutex_unlock(&s->queue_mutex);
  }
  else
  {
    pthread_mutex_unlock(&s->queue_mutex);
    return -1;
  }

  // notify workers
  pthread_mutex_lock(&s->cond_mutex);
  pthread_cond_broadcast(&s->cond);
  pthread_mutex_unlock(&s->cond_mutex);
  return 0;
}

int32_t dt_control_get_threadid()
{
  int32_t threadid = 0;
  while(darktable.control->thread[threadid] != pthread_self()) threadid++;
  assert(threadid < darktable.control->num_threads);
  return threadid;
}

int32_t dt_control_get_threadid_res()
{
  int32_t threadid = 0;
  while(darktable.control->thread_res[threadid] != pthread_self()) threadid++;
  assert(threadid < DT_CTL_WORKER_RESERVED);
  return threadid;
}

void *dt_control_work_res(void *ptr)
{
  dt_control_t *s = (dt_control_t *)ptr;
  int32_t threadid = dt_control_get_threadid_res();
  while(s->running)
  {
    // dt_print(DT_DEBUG_CONTROL, "[control_work] %d\n", threadid);
    if(dt_control_run_job_res(s, threadid) < 0)
    {
      // wait for a new job.
      int old;
      pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
      pthread_mutex_lock(&s->cond_mutex);
      pthread_cond_wait(&s->cond, &s->cond_mutex);
      pthread_mutex_unlock(&s->cond_mutex);
      pthread_setcancelstate(old, NULL);
    }
  }
  return NULL;
}

void *dt_control_work(void *ptr)
{
  dt_control_t *s = (dt_control_t *)ptr;
  // int32_t threadid = dt_control_get_threadid();
  while(s->running)
  {
    // dt_print(DT_DEBUG_CONTROL, "[control_work] %d\n", threadid);
    if(dt_control_run_job(s) < 0)
    {
      // wait for a new job.
      pthread_mutex_lock(&s->cond_mutex);
      pthread_cond_wait(&s->cond, &s->cond_mutex);
      pthread_mutex_unlock(&s->cond_mutex);
    }
  }
  return NULL;
}


// ================================================================================
//  gui functions:
// ================================================================================

gboolean dt_control_configure(GtkWidget *da, GdkEventConfigure *event, gpointer user_data)
{
  darktable.control->tabborder = fmaxf(10, event->width/100.0);
  int tb = darktable.control->tabborder;
  // re-configure all components:
  dt_view_manager_configure(darktable.view_manager, event->width - 2*tb, event->height - 2*tb);
  return TRUE;
}

void *dt_control_expose(void *voidptr)
{
  // darktable.control->gui_thread = pthread_self();
  while(1)
  {
    int width, height, pointerx, pointery;
    // ! gdk_threads_enter();
    gdk_drawable_get_size(darktable.gui->pixmap, &width, &height);
    GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "center");
    gtk_widget_get_pointer(widget, &pointerx, &pointery);
    // ! gdk_threads_leave();

    //create a gtk-independant surface to draw on
    cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(cst);

    // TODO: control_expose: only redraw the part not overlapped by temporary control panel show!
    // 
    float tb = 10;//fmaxf(10, width/100.0);
    darktable.control->tabborder = tb;
    darktable.control->width = width;
    darktable.control->height = height;

    // cairo_set_source_rgb (cr, .2, .2, .2);
    cairo_set_source_rgb (cr, .25, .25, .25);
    cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_rectangle(cr, tb, tb, width-2*tb, height-2*tb);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.5);
    cairo_set_source_rgb(cr, .1, .1, .1);
    cairo_stroke(cr);

    cairo_save(cr);
    cairo_translate(cr, tb, tb);
    cairo_rectangle(cr, 0, 0, width - 2*tb, height - 2*tb);
    cairo_clip(cr);
    cairo_new_path(cr);
    // draw view
    dt_view_manager_expose(darktable.view_manager, cr, width-2*tb, height-2*tb, pointerx-tb, pointery-tb);
    cairo_restore(cr);

    // draw gui arrows.
    cairo_set_source_rgb (cr, .6, .6, .6);

    cairo_move_to (cr, 0.0, height/2-tb);
    cairo_rel_line_to (cr, 0.0, 2*tb);
    cairo_rel_line_to (cr, tb, -tb);
    cairo_close_path (cr);
    cairo_fill(cr);

    cairo_move_to (cr, width, height/2-tb);
    cairo_rel_line_to (cr, 0.0, 2*tb);
    cairo_rel_line_to (cr, -tb, -tb);
    cairo_close_path (cr);
    cairo_fill(cr);

    cairo_move_to (cr, width/2-tb, height);
    cairo_rel_line_to (cr, 2*tb, 0.0);
    cairo_rel_line_to (cr, -tb, -tb);
    cairo_close_path (cr);
    cairo_fill(cr);

    cairo_move_to (cr, width/2-tb, 0);
    cairo_rel_line_to (cr, 2*tb, 0.0);
    cairo_rel_line_to (cr, -tb, tb);
    cairo_close_path (cr);
    cairo_fill(cr);

    // draw status bar, if any
    if(darktable.control->progress < 100.0)
    {
      tb = fmaxf(20, width/40.0);
      char num[10];
      cairo_rectangle(cr, width*0.4, height*0.85, width*0.2*darktable.control->progress/100.0f, tb);
      cairo_fill(cr);
      cairo_set_source_rgb(cr, 0., 0., 0.);
      cairo_rectangle(cr, width*0.4, height*0.85, width*0.2, tb);
      cairo_stroke(cr);
      cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
      cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size (cr, tb/3);
      cairo_move_to (cr, width/2.0-10, height*0.85+2.*tb/3.);
      snprintf(num, 10, "%d%%", (int)darktable.control->progress);
      cairo_show_text (cr, num);
    }

    cairo_destroy(cr);

    // ! gdk_threads_enter();
    cairo_t *cr_pixmap = gdk_cairo_create(darktable.gui->pixmap);
    cairo_set_source_surface (cr_pixmap, cst, 0, 0);
    cairo_paint(cr_pixmap);
    cairo_destroy(cr_pixmap);

    // GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "center");
    // gtk_widget_queue_draw(widget);
    // ! gdk_threads_leave();

    cairo_surface_destroy(cst);
    return NULL;
  }
  return NULL;
}

gboolean dt_control_expose_endmarker(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  const int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  dt_draw_endmarker(cr, width, height, (long int)user_data);
  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

void dt_control_mouse_leave()
{
  dt_view_manager_mouse_leave(darktable.view_manager);
}

void dt_control_mouse_moved(double x, double y, int which)
{
  float tb = darktable.control->tabborder;
  float wd = darktable.control->width;
  float ht = darktable.control->height;

  if(x > tb && x < wd-tb && y > tb && y < ht-tb)
    dt_view_manager_mouse_moved(darktable.view_manager, x-tb, y-tb, which);
}

void dt_control_button_released(double x, double y, int which, uint32_t state)
{
  darktable.control->button_down = 0;
  darktable.control->button_down_which = 0;
  float tb = darktable.control->tabborder;
  // float wd = darktable.control->width;
  // float ht = darktable.control->height;

  // always do this, to avoid missing some events.
  // if(x > tb && x < wd-tb && y > tb && y < ht-tb)
  dt_view_manager_button_released(darktable.view_manager, x-tb, y-tb, which, state);
}

void dt_ctl_switch_mode()
{
  int selected;
  DT_CTL_GET_GLOBAL(selected, lib_image_mouse_over_id);
  if(selected >= 0)
  {
    darktable.control->button_down = 0;
    darktable.control->button_down_which = 0;
    dt_ctl_gui_mode_t gui;
    DT_CTL_GET_GLOBAL(gui, gui);
    dt_control_save_gui_settings(gui);
    gui ^= 1;  // FIXME: cycle through more modules!
    dt_control_restore_gui_settings(gui);
    dt_view_manager_switch(darktable.view_manager, gui);
    DT_CTL_SET_GLOBAL(gui, gui);
  }
}

void dt_control_button_pressed(double x, double y, int which, int type, uint32_t state)
{
  darktable.control->button_down = 1;
  darktable.control->button_down_which = which;
  darktable.control->button_x = x;
  darktable.control->button_y = y;
  float tb = darktable.control->tabborder;
  float wd = darktable.control->width;
  float ht = darktable.control->height;
  GtkWidget *widget;

  if(x > tb && x < wd-tb && y > tb && y < ht-tb)
  {
    if(type == GDK_2BUTTON_PRESS) dt_ctl_switch_mode();
    else dt_view_manager_button_pressed(darktable.view_manager, x-tb, y-tb, which, type, state);
  }
  else if(x < tb)
  {
    widget = glade_xml_get_widget (darktable.gui->main_window, "left");
    if(GTK_WIDGET_VISIBLE(widget)) gtk_widget_hide(widget);
    else gtk_widget_show(widget);
  }
  else if(x > wd-tb)
  {
    widget = glade_xml_get_widget (darktable.gui->main_window, "right");
    if(GTK_WIDGET_VISIBLE(widget)) gtk_widget_hide(widget);
    else gtk_widget_show(widget);
  }
  else if(y < tb)
  {
    widget = glade_xml_get_widget (darktable.gui->main_window, "top");
    if(GTK_WIDGET_VISIBLE(widget)) gtk_widget_hide(widget);
    else gtk_widget_show(widget);
  }
  else if(y > ht-tb)
  {
    /* nothing useful down there:
    widget = glade_xml_get_widget (darktable.gui->main_window, "bottom");
    if(GTK_WIDGET_VISIBLE(widget)) gtk_widget_hide(widget);
    else gtk_widget_show(widget); */
  }
}

void dt_control_gui_queue_draw()
{
  if(darktable.control->running)
  {
    GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "center");
    gtk_widget_queue_draw(widget);
  }
}

void dt_control_queue_draw_all()
{
  if(darktable.control->running)
  {
    if(pthread_self() != darktable.control->gui_thread) gdk_threads_enter();
    GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "center");
    gtk_widget_queue_draw(widget);
    if(pthread_self() != darktable.control->gui_thread) gdk_threads_leave();
  }
}

void dt_control_queue_draw(GtkWidget *widget)
{
  if(darktable.control->running)
  {
    if(pthread_self() != darktable.control->gui_thread) gdk_threads_enter();
    gtk_widget_queue_draw(widget);
    if(pthread_self() != darktable.control->gui_thread) gdk_threads_leave();
  }
}

void dt_control_restore_gui_settings(dt_ctl_gui_mode_t mode)
{
  int8_t bit;
  GtkWidget *widget;

  widget = glade_xml_get_widget (darktable.gui->main_window, "image_filter");
  dt_lib_filter_t filter;
  DT_CTL_GET_GLOBAL(filter, lib_filter);
  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), (int)filter);

  widget = glade_xml_get_widget (darktable.gui->main_window, "image_sort");
  dt_lib_sort_t sort;
  DT_CTL_GET_GLOBAL(sort, lib_sort);
  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), (int)sort);

  widget = glade_xml_get_widget (darktable.gui->main_window, "export_format");
  dt_dev_export_format_t format;
  DT_CTL_GET_GLOBAL(format, dev_export_format);
  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), (int)format);

  widget = glade_xml_get_widget (darktable.gui->main_window, "export_quality");
  int quality = 100;
  DT_CTL_GET_GLOBAL(quality, dev_export_quality);
  gtk_range_set_value(GTK_RANGE(widget), quality);

  DT_CTL_GET_GLOBAL(bit, gui_left);
  widget = glade_xml_get_widget (darktable.gui->main_window, "left");
  if(bit & (1<<mode)) gtk_widget_show(widget);
  else gtk_widget_hide(widget);

  DT_CTL_GET_GLOBAL(bit, gui_right);
  widget = glade_xml_get_widget (darktable.gui->main_window, "right");
  if(bit & (1<<mode)) gtk_widget_show(widget);
  else gtk_widget_hide(widget);

  DT_CTL_GET_GLOBAL(bit, gui_top);
  widget = glade_xml_get_widget (darktable.gui->main_window, "top");
  if(bit & (1<<mode)) gtk_widget_show(widget);
  else gtk_widget_hide(widget);

  DT_CTL_GET_GLOBAL(bit, gui_bottom);
  widget = glade_xml_get_widget (darktable.gui->main_window, "bottom");
  if(bit & (1<<mode)) gtk_widget_show(widget);
  else gtk_widget_hide(widget);

  DT_CTL_GET_GLOBAL(bit, gui_navigation);
  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation_expander");
  gtk_expander_set_expanded(GTK_EXPANDER(widget), (bit & (1<<mode)) != 0);

  DT_CTL_GET_GLOBAL(bit, gui_library);
  widget = glade_xml_get_widget (darktable.gui->main_window, "library_expander");
  gtk_expander_set_expanded(GTK_EXPANDER(widget), (bit & (1<<mode)) != 0);

  DT_CTL_GET_GLOBAL(bit, gui_history);
  widget = glade_xml_get_widget (darktable.gui->main_window, "history_expander");
  gtk_expander_set_expanded(GTK_EXPANDER(widget), (bit & (1<<mode)) != 0);

  DT_CTL_GET_GLOBAL(bit, gui_histogram);
  widget = glade_xml_get_widget (darktable.gui->main_window, "histogram_expander");
  gtk_expander_set_expanded(GTK_EXPANDER(widget), (bit & (1<<mode)) != 0);

  DT_CTL_GET_GLOBAL(bit, gui_export);
  widget = glade_xml_get_widget (darktable.gui->main_window, "export_expander");
  gtk_expander_set_expanded(GTK_EXPANDER(widget), (bit & (1<<mode)) != 0);
}

void dt_control_save_gui_settings(dt_ctl_gui_mode_t mode)
{
  int8_t bit;
  GtkWidget *widget;

  DT_CTL_GET_GLOBAL(bit, gui_left);
  widget = glade_xml_get_widget (darktable.gui->main_window, "left");
  if(GTK_WIDGET_VISIBLE(widget)) bit |=   1<<mode;
  else                           bit &= ~(1<<mode);
  DT_CTL_SET_GLOBAL(gui_left, bit);

  DT_CTL_GET_GLOBAL(bit, gui_right);
  widget = glade_xml_get_widget (darktable.gui->main_window, "right");
  if(GTK_WIDGET_VISIBLE(widget)) bit |=   1<<mode;
  else                           bit &= ~(1<<mode);
  DT_CTL_SET_GLOBAL(gui_right, bit);

  DT_CTL_GET_GLOBAL(bit, gui_bottom);
  widget = glade_xml_get_widget (darktable.gui->main_window, "bottom");
  if(GTK_WIDGET_VISIBLE(widget)) bit |=   1<<mode;
  else                           bit &= ~(1<<mode);
  DT_CTL_SET_GLOBAL(gui_bottom, bit);

  DT_CTL_GET_GLOBAL(bit, gui_top);
  widget = glade_xml_get_widget (darktable.gui->main_window, "top");
  if(GTK_WIDGET_VISIBLE(widget)) bit |=   1<<mode;
  else                           bit &= ~(1<<mode);
  DT_CTL_SET_GLOBAL(gui_top, bit);

  DT_CTL_GET_GLOBAL(bit, gui_navigation);
  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation_expander");
  if(gtk_expander_get_expanded(GTK_EXPANDER(widget))) bit |= 1<<mode;
  else bit &= ~(1<<mode);
  DT_CTL_SET_GLOBAL(gui_navigation, bit);

  DT_CTL_GET_GLOBAL(bit, gui_library);
  widget = glade_xml_get_widget (darktable.gui->main_window, "library_expander");
  if(gtk_expander_get_expanded(GTK_EXPANDER(widget))) bit |= 1<<mode;
  else bit &= ~(1<<mode);
  DT_CTL_SET_GLOBAL(gui_library, bit);

  DT_CTL_GET_GLOBAL(bit, gui_history);
  widget = glade_xml_get_widget (darktable.gui->main_window, "history_expander");
  if(gtk_expander_get_expanded(GTK_EXPANDER(widget))) bit |= 1<<mode;
  else bit &= ~(1<<mode);
  DT_CTL_SET_GLOBAL(gui_history, bit);

  DT_CTL_GET_GLOBAL(bit, gui_histogram);
  widget = glade_xml_get_widget (darktable.gui->main_window, "histogram_expander");
  if(gtk_expander_get_expanded(GTK_EXPANDER(widget))) bit |= 1<<mode;
  else bit &= ~(1<<mode);
  DT_CTL_SET_GLOBAL(gui_histogram, bit);

  DT_CTL_GET_GLOBAL(bit, gui_export);
  widget = glade_xml_get_widget (darktable.gui->main_window, "export_expander");
  if(gtk_expander_get_expanded(GTK_EXPANDER(widget))) bit |= 1<<mode;
  else bit &= ~(1<<mode);
  DT_CTL_SET_GLOBAL(gui_export, bit);
}

int dt_control_key_pressed(uint16_t which)
{
  // this line is here to find the right key code on different platforms (mac).
  // printf("key code pressed: %d\n", which);
  int fullscreen, visible;
  GtkWidget *widget;
  dt_ctl_gui_mode_t gui;
  DT_CTL_GET_GLOBAL(gui, gui);
  switch (which)
  {
    case KEYCODE_period:
      dt_ctl_switch_mode();
      break;
    case KEYCODE_F11:
      widget = glade_xml_get_widget (darktable.gui->main_window, "main_window");
      DT_CTL_GET_GLOBAL(fullscreen, gui_fullscreen);
      if(fullscreen) gtk_window_unfullscreen(GTK_WINDOW(widget));
      else           gtk_window_fullscreen  (GTK_WINDOW(widget));
      fullscreen ^= 1;
      DT_CTL_SET_GLOBAL(gui_fullscreen, fullscreen);
      dt_dev_invalidate(darktable.develop);
      break;
    case KEYCODE_Escape: case KEYCODE_Caps:
      widget = glade_xml_get_widget (darktable.gui->main_window, "main_window");
      gtk_window_unfullscreen(GTK_WINDOW(widget));
      fullscreen = 0;
      DT_CTL_SET_GLOBAL(gui_fullscreen, fullscreen);
      dt_dev_invalidate(darktable.develop);
      break;
    case KEYCODE_Tab:
      widget = glade_xml_get_widget (darktable.gui->main_window, "left");
      visible = GTK_WIDGET_VISIBLE(widget);
      if(visible) gtk_widget_hide(widget);
      else gtk_widget_show(widget);

      widget = glade_xml_get_widget (darktable.gui->main_window, "right");
      if(visible) gtk_widget_hide(widget);
      else gtk_widget_show(widget);

      /*widget = glade_xml_get_widget (darktable.gui->main_window, "bottom");
      if(visible) gtk_widget_hide(widget);
      else gtk_widget_show(widget);

      widget = glade_xml_get_widget (darktable.gui->main_window, "top");
      if(visible) gtk_widget_hide(widget);
      else gtk_widget_show(widget);*/
      dt_dev_invalidate(darktable.develop);
      break;
    default:
      break;
  }
  // propagate to view modules.
  dt_view_manager_key_pressed(darktable.view_manager, which);

  widget = glade_xml_get_widget (darktable.gui->main_window, "center");
  gtk_widget_queue_draw(widget);
  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation");
  gtk_widget_queue_draw(widget);
  return 1;
}

void dt_control_add_history_item(int32_t num_in, const char *label)
{
  char wdname[20], numlabel[50];
  const gchar *lbl = NULL;
  int32_t num = num_in + 1; // one after orginal
  GtkWidget *widget = NULL;
  snprintf(numlabel, 50, "%d - %s", num, label);
  if(num >= 10) for(int i=1;i<9;i++)
  {
    darktable.control->history_start = num - 9;
    snprintf(wdname, 20, "history_%02d", i+1);
    widget = glade_xml_get_widget (darktable.gui->main_window, wdname);
    lbl = gtk_button_get_label(GTK_BUTTON(widget));
    snprintf(wdname, 20, "history_%02d", i);
    widget = glade_xml_get_widget (darktable.gui->main_window, wdname);
    gtk_button_set_label(GTK_BUTTON(widget), lbl);
    snprintf(wdname, 20, "history_%02d", 9);
  }
  else snprintf(wdname, 20, "history_%02d", num);
  widget = glade_xml_get_widget (darktable.gui->main_window, wdname);
  gtk_widget_show(widget);
  gtk_button_set_label(GTK_BUTTON(widget), numlabel);
}

void dt_control_clear_history_items(int32_t num)
{
  // clear history items down to num (leave num-th on stack)
  darktable.control->history_start = MAX(0, num - 8);
  char wdname[20], numlabel[50], numlabel2[50];
  // hide all but original
  for(int k=1;k<10;k++)
  {
    snprintf(wdname, 20, "history_%02d", k);
    GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, wdname);
    gtk_widget_hide(widget);
  }
  // 0 - original
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "history_00");
  gtk_widget_show(widget);
  gtk_button_set_label(GTK_BUTTON(widget), _("0 - original"));
  GList *history = g_list_nth(darktable.develop->history, darktable.control->history_start);
  for(int k=1;k<9;k++)
  { // k is button number: history_0k
    int curr = darktable.control->history_start + k;   // curr: curr-th history item in list in dev
    if(curr > num+1 || !history) break;                  // curr > num+1: history item stays hidden
    snprintf(wdname, 20, "history_%02d", k);
    GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, wdname);
    gtk_widget_show(widget);
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
    dt_dev_get_history_item_label(hist, numlabel2);
    snprintf(numlabel, 50, "%d - %s", curr, numlabel2);
    gtk_button_set_label(GTK_BUTTON(widget), numlabel);
    history = g_list_next(history);
  }
}

void dt_control_update_recent_films()
{
  char wdname[20];
  for(int k=1;k<5;k++)
  {
    snprintf(wdname, 20, "recent_film_%d", k);
    GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, wdname);
    gtk_widget_hide(widget);
  }
  sqlite3_stmt *stmt;
  int rc, num = 1;
  const char *filename, *cnt;
  const int label_cnt = 26;
  char label[label_cnt];
  // rc = sqlite3_prepare_v2(darktable.db, "select * from (select folder from film_rolls order by datetime_accessed) as dreggn limit 0, 4", -1, &stmt, NULL);
#if 0
  rc = sqlite3_prepare_v2(darktable.db, "select folder from film_rolls order by datetime_accessed desc limit 0,4", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    filename = (char *)sqlite3_column_text(stmt, 0);
    cnt = filename + MIN(512,strlen(filename));
    int i;
    for(i=0;i<label_cnt-1;i++) if(cnt > filename) cnt--;
    if(i == label_cnt-1) snprintf(label, label_cnt, "...%s", cnt+3);
    else snprintf(label, label_cnt, "%s", cnt);
    snprintf(wdname, 20, "recent_film_%d", num);
    GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, wdname);
    gtk_button_set_label(GTK_BUTTON(widget), label);
    gtk_widget_show(widget);
    num++;
  }
  sqlite3_finalize(stmt);
#else
  // FIXME: this is a temporary hack to keep the database low:
  // remove all data from db from all other films.
  rc = sqlite3_prepare_v2(darktable.db, "select folder,id from film_rolls order by datetime_accessed desc", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(num < 5)
    {
      const int id = sqlite3_column_int(stmt, 1);
      if(id == 0)
      {
        snprintf(label, label_cnt, _("single images"));
      }
      else
      {
        filename = (char *)sqlite3_column_text(stmt, 0);
        cnt = filename + MIN(512,strlen(filename));
        int i;
        for(i=0;i<label_cnt-1;i++) if(cnt > filename) cnt--;
        if(cnt > filename) snprintf(label, label_cnt, "...%s", cnt+3);
        else snprintf(label, label_cnt, "%s", cnt);
      }
      snprintf(wdname, 20, "recent_film_%d", num);
      GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, wdname);
      gtk_button_set_label(GTK_BUTTON(widget), label);
      gtk_widget_show(widget);
    }
    else
    {
      int film_id = sqlite3_column_int(stmt, 1);
      printf("removing %d\n", film_id);
      sqlite3_stmt *stmt2;
      int rc2;
      rc2 = sqlite3_prepare_v2(darktable.db, "select id from images where film_id = ?1", -1, &stmt2, NULL);
      rc2 = sqlite3_bind_int (stmt, 1, film_id);
      while(sqlite3_step(stmt2) == SQLITE_ROW)
      {
        int img_id = sqlite3_column_int(stmt2, 0);
        sqlite3_stmt *stmt3;
        int rc3;
        rc3 = sqlite3_prepare_v2(darktable.db, "delete from mipmaps where imgid = ?1", -1, &stmt3, NULL);
        rc3 = sqlite3_bind_int (stmt3, 1, img_id);
        rc3 = sqlite3_step(stmt3);
        sqlite3_finalize(stmt3);
        rc3 = sqlite3_prepare_v2(darktable.db, "delete from selected_images where imgid = ?1", -1, &stmt3, NULL);
        rc3 = sqlite3_bind_int (stmt3, 1, img_id);
        rc3 = sqlite3_step(stmt3);
        sqlite3_finalize(stmt3);
        rc3 = sqlite3_prepare_v2(darktable.db, "delete from history where imgid = ?1", -1, &stmt3, NULL);
        rc3 = sqlite3_bind_int (stmt3, 1, img_id);
        rc3 = sqlite3_step(stmt3);
        sqlite3_finalize(stmt3);
      }
      sqlite3_finalize(stmt2);
      rc2 = sqlite3_prepare_v2(darktable.db, "delete from images where film_id = ?1", -1, &stmt2, NULL);
      rc2 = sqlite3_bind_int (stmt2, 1, film_id);
      rc2 = sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);
      rc2 = sqlite3_prepare_v2(darktable.db, "delete from film_rolls where id = ?1", -1, &stmt2, NULL);
      rc2 = sqlite3_bind_int (stmt2, 1, film_id);
      rc2 = sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);
    }
    num++;
  }
  sqlite3_finalize(stmt);
#endif
}

