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
#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif
#include "control/control.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "common/darktable.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "views/view.h"
#include "gui/gtk.h"
#include "gui/contrast.h"
#include "gui/filmview.h"
#include "gui/draw.h"

#ifdef GDK_WINDOWING_QUARTZ
#  include <Carbon/Carbon.h>
#  include <ApplicationServices/ApplicationServices.h>
#  include <CoreServices/CoreServices.h>
#endif

#include <stdlib.h>
#include <strings.h>
#include <assert.h>
#include <math.h>
#include <string.h>
#include <glib/gstdio.h>


void dt_ctl_settings_default(dt_control_t *c)
{
  dt_conf_set_string ("database", ".darktabledb");

  dt_conf_set_int  ("config_version", DT_CONFIG_VERSION);
  dt_conf_set_bool ("write_dt_files", TRUE);
  dt_conf_set_bool ("ask_before_delete", TRUE);
  dt_conf_set_float("preview_subsample", .125f);
  dt_conf_set_int  ("mipmap_cache_thumbnails", 1000);
  dt_conf_set_int  ("mipmap_cache_full_images", 2);
  dt_conf_set_int  ("cache_memory", 536870912);
  dt_conf_set_int  ("database_cache_quality", 89);

  dt_conf_set_bool ("ui_last/fullscreen", FALSE);
  dt_conf_set_int  ("ui_last/view", DT_MODE_NONE);
  dt_conf_set_int  ("ui_last/film_roll", 1);

  dt_conf_set_int  ("ui_last/window_x",      0);
  dt_conf_set_int  ("ui_last/window_y",      0);
  dt_conf_set_int  ("ui_last/window_w",    640);
  dt_conf_set_int  ("ui_last/window_h",    480);

  dt_conf_set_int  ("ui_last/panel_left",   -1);
  dt_conf_set_int  ("ui_last/panel_right",  -1);
  dt_conf_set_int  ("ui_last/panel_top",     0);
  dt_conf_set_int  ("ui_last/panel_bottom",  0);

  dt_conf_set_int  ("ui_last/expander_library",     1<<DT_LIBRARY);
  dt_conf_set_int  ("ui_last/expander_metadata",    0);
  dt_conf_set_int  ("ui_last/expander_navigation", -1);
  dt_conf_set_int  ("ui_last/expander_histogram",  -1);
  dt_conf_set_int  ("ui_last/expander_history",    -1);

  dt_conf_set_int  ("ui_last/combo_sort",     DT_LIB_SORT_FILENAME);
  dt_conf_set_int  ("ui_last/combo_filter",   DT_LIB_FILTER_STAR_1);

  // Import settings
  dt_conf_set_string ("capture/camera/storage/basedirectory", "$(PICTURES_FOLDER)/Darktable");
  dt_conf_set_string ("capture/camera/storage/subpath", "$(YEAR)$(MONTH)$(DAY)_$(JOBCODE)");
  dt_conf_set_string ("capture/camera/storage/namepattern", "$(YEAR)$(MONTH)$(DAY)_$(SEQUENCE).$(FILE_EXTENSION)");
  dt_conf_set_string ("capture/camera/import/jobcode", "noname");
  
  dt_conf_set_float("gamma_linear", .1f);
  dt_conf_set_float("gamma_gamma", .45f);
}

void dt_ctl_settings_init(dt_control_t *s)
{
  // same thread as init
  s->gui_thread = pthread_self();
  // init global defaults.
  pthread_mutex_init(&(s->global_mutex), NULL);
  pthread_mutex_init(&(s->image_mutex), NULL);

  s->global_settings.version = DT_VERSION;

  // TODO: move the mouse_over_id of lighttable to something general in control: gui-thread selected img or so?
  s->global_settings.lib_image_mouse_over_id = -1;

  // TODO: move these to darkroom settings blob:
  s->global_settings.dev_closeup = 0;
  s->global_settings.dev_zoom_x = 0;
  s->global_settings.dev_zoom_y = 0;
  s->global_settings.dev_zoom = DT_ZOOM_FIT;

  memcpy(&(s->global_defaults), &(s->global_settings), sizeof(dt_ctl_settings_t));
}

int dt_control_load_config(dt_control_t *c)
{
  int rc;
  sqlite3_stmt *stmt;
  // unsafe, fast write:
  // sqlite3_exec(darktable.db, "PRAGMA synchronous=off", NULL, NULL, NULL);
  // free memory on disk if we call the line below:
  // rc = sqlite3_exec(darktable.db, "PRAGMA auto_vacuum=INCREMENTAL", NULL, NULL, NULL);
  // rc = sqlite3_exec(darktable.db, "PRAGMA incremental_vacuum(0)", NULL, NULL, NULL);
  rc = sqlite3_prepare_v2(darktable.db, "select settings from settings", -1, &stmt, NULL);
  if(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
  {
#if 1 // global settings not needed anymore
    pthread_mutex_lock(&(darktable.control->global_mutex));
    darktable.control->global_settings.version = -1;
    const void *set = sqlite3_column_blob(stmt, 0);
    int len = sqlite3_column_bytes(stmt, 0);
    if(len == sizeof(dt_ctl_settings_t)) memcpy(&(darktable.control->global_settings), set, len);
#endif
    rc = sqlite3_finalize(stmt);

#if 1
    if(darktable.control->global_settings.version != DT_VERSION)
    {
      fprintf(stderr, "[load_config] wrong version %d (should be %d), substituting defaults.\n", darktable.control->global_settings.version, DT_VERSION);
      memcpy(&(darktable.control->global_settings), &(darktable.control->global_defaults), sizeof(dt_ctl_settings_t));
      pthread_mutex_unlock(&(darktable.control->global_mutex));
      // drop all, restart. TODO: freeze this version or have update facility!
      sqlite3_exec(darktable.db, "drop table settings", NULL, NULL, NULL);
      sqlite3_exec(darktable.db, "drop table film_rolls", NULL, NULL, NULL);
      sqlite3_exec(darktable.db, "drop table images", NULL, NULL, NULL);
      sqlite3_exec(darktable.db, "drop table selected_images", NULL, NULL, NULL);
      sqlite3_exec(darktable.db, "drop table mipmaps", NULL, NULL, NULL);
      sqlite3_exec(darktable.db, "drop table mipmap_timestamps", NULL, NULL, NULL);
      sqlite3_exec(darktable.db, "drop table history", NULL, NULL, NULL);
      sqlite3_exec(darktable.db, "drop table tags", NULL, NULL, NULL);
      sqlite3_exec(darktable.db, "drop table tagxtag", NULL, NULL, NULL);
      sqlite3_exec(darktable.db, "drop table tagged_images", NULL, NULL, NULL);
      goto create_tables;
    }
    else
    {
      // silly check if old table is still present:
      sqlite3_exec(darktable.db, "delete from color_labels where imgid=0", NULL, NULL, NULL);
      sqlite3_exec(darktable.db, "insert into color_labels values (0, 0)", NULL, NULL, NULL);
      sqlite3_exec(darktable.db, "insert into color_labels values (0, 1)", NULL, NULL, NULL);
      sqlite3_prepare_v2(darktable.db, "select color from color_labels where imgid=0", -1, &stmt, NULL);
      int col = 0;
      // still the primary key option set?
      while(sqlite3_step(stmt) == SQLITE_ROW) col = MAX(col, sqlite3_column_int(stmt, 0));
      sqlite3_finalize(stmt);
      if(col != 1) sqlite3_exec(darktable.db, "drop table color_labels", NULL, NULL, NULL);

      // insert new tables, if not there (statement will just fail if so):
      sqlite3_exec(darktable.db, "create table color_labels (imgid integer, color integer)", NULL, NULL, NULL);
      sqlite3_exec(darktable.db, "drop table mipmaps", NULL, NULL, NULL);
      sqlite3_exec(darktable.db, "drop table mipmap_timestamps", NULL, NULL, NULL);
      pthread_mutex_unlock(&(darktable.control->global_mutex));
    }
#endif
  }
  else
  { // db not yet there, create it
    rc = sqlite3_finalize(stmt);
create_tables:
    rc = sqlite3_exec(darktable.db, "create table settings (settings blob)", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table film_rolls (id integer primary key, datetime_accessed char(20), folder varchar(1024))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table images (id integer primary key, film_id integer, width int, height int, filename varchar, maker varchar, model varchar, lens varchar, exposure real, aperture real, iso real, focal_length real, datetime_taken char(20), flags integer, output_width integer, output_height integer, crop real, raw_parameters integer, raw_denoise_threshold real, raw_auto_bright_threshold real, raw_black real, raw_maximum real, caption varchar, description varchar, license varchar, sha1sum char(40))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table selected_images (imgid integer)", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table history (imgid integer, num integer, module integer, operation varchar(256), op_params blob, enabled integer)", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table tags (id integer primary key, name varchar, icon blob, description varchar, flags integer)", NULL, NULL, NULL);
    rc = sqlite3_exec(darktable.db, "create table tagxtag (id1 integer, id2 integer, count integer, primary key(id1, id2))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table tagged_images (imgid integer, tagid integer, primary key(imgid, tagid))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    sqlite3_exec(darktable.db, "create table color_labels (imgid integer, color integer)", NULL, NULL, NULL);

    // add dummy film roll for single images
    char datetime[20];
    dt_gettime(datetime);
    rc = sqlite3_prepare_v2(darktable.db, "insert into film_rolls (id, datetime_accessed, folder) values (null, ?1, 'single images')", -1, &stmt, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_bind_text(stmt, 1, datetime, strlen(datetime), SQLITE_STATIC);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(darktable.db, "insert into settings (settings) values (?1)", -1, &stmt, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_bind_blob(stmt, 1, &(darktable.control->global_defaults), sizeof(dt_ctl_settings_t), SQLITE_STATIC);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
  }
  dt_conf_set_int("ui_last/view", DT_MODE_NONE);
  int width  = dt_conf_get_int("ui_last/window_w");
  int height = dt_conf_get_int("ui_last/window_h");
  gint x = dt_conf_get_int("ui_last/window_x");
  gint y = dt_conf_get_int("ui_last/window_y");
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "main_window");
//   gtk_window_set_position(GTK_WINDOW(widget),GTK_WIN_POS_CENTER_ALWAYS);
  gtk_window_move(GTK_WINDOW(widget),x,y);
  gtk_window_resize(GTK_WINDOW(widget), width, height);
  int fullscreen = dt_conf_get_bool("ui_last/fullscreen");
  if(fullscreen) gtk_window_fullscreen  (GTK_WINDOW(widget));
  else           gtk_window_unfullscreen(GTK_WINDOW(widget));
  dt_control_restore_gui_settings(DT_LIBRARY);
  dt_control_update_recent_films();
  return 0;
}

int dt_control_write_config(dt_control_t *c)
{
  dt_ctl_gui_mode_t gui = dt_conf_get_int("ui_last/view");
  dt_control_save_gui_settings(gui);

  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "main_window");
  gint x, y;
  gtk_window_get_position(GTK_WINDOW(widget), &x, &y);
  dt_conf_set_int ("ui_last/window_x",  x);
  dt_conf_set_int ("ui_last/window_y",  y);
  dt_conf_set_int ("ui_last/window_w",  widget->allocation.width);
  dt_conf_set_int ("ui_last/window_h",  widget->allocation.height);

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

// Get the display ICC profile of the monitor associated with the widget.
// For X display, uses the ICC profile specifications version 0.2 from
// http://burtonini.com/blog/computers/xicc
// Based on code from Gimp's modules/cdisplay_lcms.c
#ifdef GDK_WINDOWING_QUARTZ
typedef struct
{
  guchar *data;
  gsize   len;
}
ProfileTransfer;

enum
{
  openReadSpool  = 1, /* start read data process         */
  openWriteSpool = 2, /* start write data process        */
  readSpool      = 3, /* read specified number of bytes  */
  writeSpool     = 4, /* write specified number of bytes */
  closeSpool     = 5  /* complete data transfer process  */
};

#ifndef __LP64__
static OSErr dt_ctl_lcms_flatten_profile(SInt32  command,
    SInt32 *size, void *data, void *refCon)
{
  // ProfileTransfer *transfer = static_cast<ProfileTransfer*>(refCon);
  ProfileTransfer *transfer = (ProfileTransfer *)refCon;

  switch (command)
  {
    case openWriteSpool:
      g_return_val_if_fail(transfer->data==NULL && transfer->len==0, -1);
      break;

    case writeSpool:
      transfer->data = (guchar *)
          g_realloc(transfer->data, transfer->len + *size);
      memcpy(transfer->data + transfer->len, data, *size);
      transfer->len += *size;
      break;

    default:
      break;
  }
  return 0;
}
#endif /* __LP64__ */
#endif /* GDK_WINDOWING_QUARTZ */

void dt_ctl_get_display_profile(GtkWidget *widget,
    guint8 **buffer, gint *buffer_size)
{ // thanks to ufraw for this!
  *buffer = NULL;
  *buffer_size = 0;
#if defined GDK_WINDOWING_X11
  GdkScreen *screen = gtk_widget_get_screen(widget);
  if ( screen==NULL )
    screen = gdk_screen_get_default();
  int monitor = gdk_screen_get_monitor_at_window (screen, widget->window);
  char *atom_name;
  if (monitor > 0)
    atom_name = g_strdup_printf("_ICC_PROFILE_%d", monitor);
  else
    atom_name = g_strdup("_ICC_PROFILE");

  GdkAtom type = GDK_NONE;
  gint format = 0;
  gdk_property_get(gdk_screen_get_root_window(screen),
      gdk_atom_intern(atom_name, FALSE), GDK_NONE,
      0, 64 * 1024 * 1024, FALSE,
      &type, &format, buffer_size, buffer);
  g_free(atom_name);

#elif defined GDK_WINDOWING_QUARTZ
  GdkScreen *screen = gtk_widget_get_screen(widget);
  if ( screen==NULL )
    screen = gdk_screen_get_default();
  int monitor = gdk_screen_get_monitor_at_window(screen, widget->window);

  CMProfileRef prof = NULL;
  CMGetProfileByAVID(monitor, &prof);
  if ( prof==NULL )
    return;

  ProfileTransfer transfer = { NULL, 0 };
  //The following code does not work on 64bit OSX.  Disable if we are compiling there.
#ifndef __LP64__  
  Boolean foo;
  CMFlattenProfile(prof, 0, dt_ctl_lcms_flatten_profile, &transfer, &foo);
  CMCloseProfile(prof);
#endif
  *buffer = transfer.data;
  *buffer_size = transfer.len;

#elif defined G_OS_WIN32
  (void)widget;
  HDC hdc = GetDC (NULL);
  if ( hdc==NULL )
    return;

  DWORD len = 0;
  GetICMProfile (hdc, &len, NULL);
  gchar *path = g_new (gchar, len);

  if (GetICMProfile (hdc, &len, path)) {
    gsize size;
    g_file_get_contents(path, (gchar**)buffer, &size, NULL);
    *buffer_size = size;
  }
  g_free (path);
  ReleaseDC (NULL, hdc);
#endif
}

void dt_control_init(dt_control_t *s)
{
  dt_ctl_settings_init(s);

  s->key_accelerators_on = 1;
  /* DEPRECATED
  s->esc_shortcut_on = 1;
  s->tab_shortcut_on = 1;*/
  s->log_pos = s->log_ack = 0;
  s->log_busy = 0;
  s->log_message_timeout_id = 0;
  pthread_mutex_init(&(s->log_mutex), NULL);
  s->progress = 200.0f;

  dt_conf_set_int("ui_last/view", DT_MODE_NONE);
  
  // if config is old, replace with new defaults.
  if(DT_CONFIG_VERSION > dt_conf_get_int("config_version"))
    dt_ctl_settings_default(s);

  pthread_cond_init(&s->cond, NULL);
  pthread_mutex_init(&s->cond_mutex, NULL);
  pthread_mutex_init(&s->queue_mutex, NULL);
  pthread_mutex_init(&s->run_mutex, NULL);

  int k; for(k=0;k<DT_CONTROL_MAX_JOBS;k++) s->idle[k] = k;
  s->idle_top = DT_CONTROL_MAX_JOBS;
  s->queued_top = 0;
  // start threads
  s->num_threads = dt_ctl_get_num_procs()+1;
  s->thread = (pthread_t *)malloc(sizeof(pthread_t)*s->num_threads);
  pthread_mutex_lock(&s->run_mutex);
  s->running = 1;
  pthread_mutex_unlock(&s->run_mutex);
  for(k=0;k<s->num_threads;k++)
    pthread_create(s->thread + k, NULL, dt_control_work, s);
  for(k=0;k<DT_CTL_WORKER_RESERVED;k++)
  {
    s->new_res[k] = 0;
    pthread_create(s->thread_res + k, NULL, dt_control_work_res, s);
  }
  s->button_down = 0;
  s->button_down_which = 0;
}

void dt_control_key_accelerators_on(struct dt_control_t *s)
{
    s->key_accelerators_on = 1;
}

void dt_control_key_accelerators_off(struct dt_control_t *s)
{
  s->key_accelerators_on = 0;
}


int dt_control_is_key_accelerators_on(struct dt_control_t *s)
{
  return  s->key_accelerators_on == 1?1:0;
}

/* deprecated 
void dt_control_tab_shortcut_off(dt_control_t *s)
{
  s->tab_shortcut_on = 0;
}

void dt_control_tab_shortcut_on(dt_control_t *s)
{
  s->tab_shortcut_on = 1;
}

void dt_control_esc_shortcut_off(dt_control_t *s)
{
  s->esc_shortcut_on = 0;
}

void dt_control_esc_shortcut_on(dt_control_t *s)
{
  s->esc_shortcut_on = 1;
}
*/

void dt_control_change_cursor(dt_cursor_t curs)
{
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "main_window");
  GdkCursor* cursor = gdk_cursor_new(curs);
  gdk_window_set_cursor(widget->window, cursor);
  gdk_cursor_destroy(cursor);
}

int dt_control_running()
{
  // FIXME: when shutdown, run_mutex is not inited anymore!
  dt_control_t *s = darktable.control;
  pthread_mutex_lock(&s->run_mutex);
  int running = s->running;
  pthread_mutex_unlock(&s->run_mutex);
  return running;
}

void dt_control_shutdown(dt_control_t *s)
{
  pthread_mutex_lock(&s->cond_mutex);
  pthread_mutex_lock(&s->run_mutex);
  s->running = 0;
  pthread_mutex_unlock(&s->run_mutex);
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
  // vacuum TODO: optional?
  // rc = sqlite3_exec(darktable.db, "PRAGMA incremental_vacuum(0)", NULL, NULL, NULL);
  // rc = sqlite3_exec(darktable.db, "vacuum", NULL, NULL, NULL);
  pthread_mutex_destroy(&s->queue_mutex);
  pthread_mutex_destroy(&s->cond_mutex);
  pthread_mutex_destroy(&s->log_mutex);
  pthread_mutex_destroy(&s->run_mutex);
}

void dt_control_job_init(dt_job_t *j, const char *msg, ...)
{
  bzero(j, sizeof(dt_job_t));
#ifdef DT_CONTROL_JOB_DEBUG
  va_list ap;
  va_start(ap, msg);
  vsnprintf(j->description, DT_CONTROL_DESCRIPTION_LEN, msg, ap);
  va_end(ap);
#endif
  j->state = DT_JOB_STATE_INITIALIZED;
  pthread_mutex_init (&j->state_mutex,NULL);
  pthread_mutex_init (&j->wait_mutex,NULL);
}

void dt_control_job_set_state_callback(dt_job_t *j,dt_job_state_change_callback cb,void *user_data)
{
  j->state_changed_cb = cb;
  j->user_data = user_data;
}


void dt_control_job_print(dt_job_t *j)
{
#ifdef DT_CONTROL_JOB_DEBUG
  dt_print(DT_DEBUG_CONTROL, "%s", j->description);
#endif
}

void _control_job_set_state(dt_job_t *j,int state)
{
  pthread_mutex_lock (&j->state_mutex);
  j->state = state;
  /* pass state change to callback */
  if (j->state_changed_cb)
      j->state_changed_cb (j,state);
  pthread_mutex_unlock (&j->state_mutex);

}

int dt_control_job_get_state(dt_job_t *j)
{
  pthread_mutex_lock (&j->state_mutex);
  int state = j->state;
  pthread_mutex_unlock (&j->state_mutex);
  return state;
}

void dt_control_job_cancel(dt_job_t *j)
{
  _control_job_set_state (j,DT_JOB_STATE_CANCELLED);
}

void dt_control_job_wait(dt_job_t *j)
{
  int state = dt_control_job_get_state (j);
 
  /* if job execution not is finished let's wait for signal */
  if (state==DT_JOB_STATE_RUNNING || state==DT_JOB_STATE_CANCELLED)
  {
    pthread_mutex_lock (&j->wait_mutex);
    pthread_mutex_unlock (&j->wait_mutex);
  }
 
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

  dt_print(DT_DEBUG_CONTROL, "[run_job+] %d %f ", res, dt_get_wtime());
  dt_control_job_print(j);
  dt_print(DT_DEBUG_CONTROL, "\n");

  /* change state to running */
  if (dt_control_job_get_state (j) == DT_JOB_STATE_QUEUED)
  {
    _control_job_set_state (j,DT_JOB_STATE_RUNNING); 
    
    /* execute job */
    pthread_mutex_lock (&j->wait_mutex);
    j->result = j->execute (j);
    pthread_mutex_unlock (&j->wait_mutex);
   
    _control_job_set_state (j,DT_JOB_STATE_FINISHED); 
    dt_print(DT_DEBUG_CONTROL, "[run_job-] %d %f ", res, dt_get_wtime());
    dt_control_job_print(j);
    dt_print(DT_DEBUG_CONTROL, "\n");
  
  }
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

  dt_print(DT_DEBUG_CONTROL, "[run_job+] %d %f ", DT_CTL_WORKER_RESERVED+dt_control_get_threadid(), dt_get_wtime());
  dt_control_job_print(j);
  dt_print(DT_DEBUG_CONTROL, "\n");
  
  /* change state to running */
  if (dt_control_job_get_state (j) == DT_JOB_STATE_QUEUED)
  {
    _control_job_set_state (j,DT_JOB_STATE_RUNNING); 
    
    /* execute job */
    pthread_mutex_lock (&j->wait_mutex);
    j->result = j->execute (j);
    pthread_mutex_unlock (&j->wait_mutex);

    _control_job_set_state (j,DT_JOB_STATE_FINISHED); 
    
    dt_print(DT_DEBUG_CONTROL, "[run_job-] %d %f ", DT_CTL_WORKER_RESERVED+dt_control_get_threadid(), dt_get_wtime());
    dt_control_job_print(j);
    dt_print(DT_DEBUG_CONTROL, "\n");
    
  }
  
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
  _control_job_set_state (job,DT_JOB_STATE_QUEUED); 
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
  for(i=0;i<s->queued_top;i++)
  { // find equivalent job and quit if already there
    const int j = s->queued[i];
    if(!memcmp(job, s->job + j, sizeof(dt_job_t)))
    {
      dt_print(DT_DEBUG_CONTROL, "[add_job] found job already in queue\n");
      pthread_mutex_unlock(&s->queue_mutex);
      return -1;
    }
  }
  dt_print(DT_DEBUG_CONTROL, "[add_job] %d ", s->idle_top);
  dt_control_job_print(job);
  dt_print(DT_DEBUG_CONTROL, "\n");
  if(s->idle_top != 0)
  {
    i = --s->idle_top;
    _control_job_set_state (job,DT_JOB_STATE_QUEUED); 
    s->job[s->idle[i]] = *job;
    s->queued[s->queued_top++] = s->idle[i];
    pthread_mutex_unlock(&s->queue_mutex);
  }
  else
  {
    dt_print(DT_DEBUG_CONTROL, "[add_job] too many jobs in queue!\n");
    _control_job_set_state (job,DT_JOB_STATE_DISCARDED); 
    pthread_mutex_unlock(&s->queue_mutex);
    return -1;
  }

  // notify workers
  pthread_mutex_lock(&s->cond_mutex);
  pthread_cond_broadcast(&s->cond);
  pthread_mutex_unlock(&s->cond_mutex);
  return 0;
}

int32_t dt_control_revive_job(dt_control_t *s, dt_job_t *job)
{
  int32_t found_j = -1;
  pthread_mutex_lock(&s->queue_mutex);
  dt_print(DT_DEBUG_CONTROL, "[revive_job] ");
  dt_control_job_print(job);
  dt_print(DT_DEBUG_CONTROL, "\n");
  for(int i=0;i<s->queued_top;i++)
  { // find equivalent job and push it up on top of the stack.
    const int j = s->queued[i];
    if(!memcmp(job, s->job + j, sizeof(dt_job_t)))
    {
      found_j = j;
      dt_print(DT_DEBUG_CONTROL, "[revive_job] found job in queue at position %d, moving to %d\n", i, s->queued_top);
      memmove(s->queued + i, s->queued + i + 1, sizeof(int32_t) * (s->queued_top - i - 1));
      s->queued[s->queued_top-1] = j;
    }
  }
  pthread_mutex_unlock(&s->queue_mutex);

  // notify workers
  pthread_mutex_lock(&s->cond_mutex);
  pthread_cond_broadcast(&s->cond);
  pthread_mutex_unlock(&s->cond_mutex);
  return found_j;
}

int32_t dt_control_get_threadid()
{
  int32_t threadid = 0;
  while(darktable.control->thread[threadid] != pthread_self() && threadid < darktable.control->num_threads-1)
    threadid++;
  assert(darktable.control->thread[threadid] == pthread_self());
  return threadid;
}

int32_t dt_control_get_threadid_res()
{
  int32_t threadid = 0;
  while(darktable.control->thread_res[threadid] != pthread_self() && threadid < DT_CTL_WORKER_RESERVED-1)
    threadid++;
  assert(darktable.control->thread_res[threadid] == pthread_self());
  return threadid;
}

void *dt_control_work_res(void *ptr)
{
  dt_control_t *s = (dt_control_t *)ptr;
  int32_t threadid = dt_control_get_threadid_res();
  while(dt_control_running())
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
  while(dt_control_running())
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
  darktable.control->tabborder = 8;
  int tb = darktable.control->tabborder;
  // re-configure all components:
  dt_view_manager_configure(darktable.view_manager, event->width - 2*tb, event->height - 2*tb);
  return TRUE;
}

void *dt_control_expose(void *voidptr)
{
  int width, height, pointerx, pointery;
  gdk_drawable_get_size(darktable.gui->pixmap, &width, &height);
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "center");
  gtk_widget_get_pointer(widget, &pointerx, &pointery);

  //create a gtk-independant surface to draw on
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // TODO: control_expose: only redraw the part not overlapped by temporary control panel show!
  // 
  float tb = 8;//fmaxf(10, width/100.0);
  darktable.control->tabborder = tb;
  darktable.control->width = width;
  darktable.control->height = height;

  cairo_set_source_rgb (cr, darktable.gui->bgcolor[0]+0.04, darktable.gui->bgcolor[1]+0.04, darktable.gui->bgcolor[2]+0.04);
  cairo_set_line_width(cr, tb);
  cairo_rectangle(cr, tb/2., tb/2., width-tb, height-tb);
  cairo_stroke(cr);
  cairo_set_line_width(cr, 1.5);
  cairo_set_source_rgb (cr, .1, .1, .1);
  cairo_rectangle(cr, tb, tb, width-2*tb, height-2*tb);
  cairo_stroke(cr);

  cairo_save(cr);
  cairo_translate(cr, tb, tb);
  cairo_rectangle(cr, 0, 0, width - 2*tb, height - 2*tb);
  cairo_clip(cr);
  cairo_new_path(cr);
  // draw view
  dt_view_manager_expose(darktable.view_manager, cr, width-2*tb, height-2*tb, pointerx-tb, pointery-tb);
  cairo_restore(cr);

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
  // draw log message, if any
  pthread_mutex_lock(&darktable.control->log_mutex);
  if(darktable.control->log_ack != darktable.control->log_pos)
  {
    cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    const float fontsize = 14;
    cairo_set_font_size (cr, fontsize);
    cairo_text_extents_t ext;
    cairo_text_extents (cr, darktable.control->log_message[darktable.control->log_ack], &ext);
    const float pad = 20.0f, xc = width/2.0, yc = height*0.85+10, wd = pad + ext.width*.5f;
    float rad = 14;
    cairo_set_line_width(cr, 1.);
    cairo_move_to( cr, xc-wd,yc+rad);
    for(int k=0;k<5;k++)
    {
      cairo_arc (cr, xc-wd, yc, rad, M_PI/2.0, 3.0/2.0*M_PI);
      cairo_line_to (cr, xc+wd, yc-rad);
      cairo_arc (cr, xc+wd, yc, rad, 3.0*M_PI/2.0, M_PI/2.0);
      cairo_line_to (cr, xc-wd, yc+rad);
      if(k == 0)
      {
        cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
        cairo_fill_preserve (cr);
      }
      cairo_set_source_rgba(cr, 0., 0., 0., 1.0/(1+k));
      cairo_stroke (cr);
      rad += .5f;
    }
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_move_to (cr, xc-wd+.5f*pad, yc + 1./3.*fontsize);
    cairo_show_text (cr, darktable.control->log_message[darktable.control->log_ack]);
  }
  // draw busy indicator
  if(darktable.control->log_busy > 0)
  {
    cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    const float fontsize = 14;
    cairo_set_font_size (cr, fontsize);
    cairo_text_extents_t ext;
    cairo_text_extents (cr, _("working.."), &ext);
    const float xc = width/2.0, yc = height*0.85-30, wd = ext.width*.5f;
    cairo_move_to (cr, xc-wd, yc + 1./3.*fontsize);
    cairo_text_path (cr, _("working.."));
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 0.7);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_stroke(cr);
  }
  pthread_mutex_unlock(&darktable.control->log_mutex);

  cairo_destroy(cr);

  cairo_t *cr_pixmap = gdk_cairo_create(darktable.gui->pixmap);
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);

  cairo_surface_destroy(cst);
  return NULL;
}

void 
dt_control_size_allocate_endmarker(GtkWidget *w, GtkAllocation *a, gpointer *data)
{
  // Reset size to match panel width
  int height = a->width*0.25;
  gtk_widget_set_size_request(w,a->width,height);
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

void dt_control_mouse_enter()
{
  dt_view_manager_mouse_enter(darktable.view_manager);
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

void dt_ctl_switch_mode_to(dt_ctl_gui_mode_t mode)
{
  dt_ctl_gui_mode_t oldmode = dt_conf_get_int("ui_last/view");
  if(oldmode == mode) return;
  
  /* check sanitiy of mode switch etc*/
  switch (mode)
  {
    case DT_DEVELOP:
    {
      int32_t mouse_over_id=0;
      DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
      if(mouse_over_id <= 0) return;
    }break;
    
    case DT_LIBRARY:
    case DT_CAPTURE:
    default:
    break;
    
  }

  
  /* everythings are ok, let's switch mode */
  dt_control_save_gui_settings(oldmode);
  darktable.control->button_down = 0;
  darktable.control->button_down_which = 0;
  char buf[512];
  snprintf(buf, 512, _("switch to %s mode"), dt_view_manager_name(darktable.view_manager));

  int error = dt_view_manager_switch(darktable.view_manager, mode);
  if(error) return;

  dt_control_restore_gui_settings(mode);
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "view_label");
  gtk_object_set(GTK_OBJECT(widget), "tooltip-text", buf, NULL);
  snprintf(buf, 512, _("<span color=\"#7f7f7f\"><big><b>%s mode</b></big></span>"), dt_view_manager_name(darktable.view_manager));
  gtk_label_set_label(GTK_LABEL(widget), buf);
  dt_conf_set_int ("ui_last/view", mode);
}

void dt_ctl_switch_mode()
{
  dt_ctl_gui_mode_t mode = dt_conf_get_int("ui_last/view");
  if(mode == DT_LIBRARY) mode = DT_DEVELOP;
  else mode = DT_LIBRARY;
  dt_ctl_switch_mode_to(mode);
}

static gboolean _dt_ctl_log_message_timeout_callback (gpointer data)
{
  pthread_mutex_lock(&darktable.control->log_mutex);
  if(darktable.control->log_ack != darktable.control->log_pos)
    darktable.control->log_ack = (darktable.control->log_ack+1)%DT_CTL_LOG_SIZE;
  darktable.control->log_message_timeout_id=0;
  pthread_mutex_unlock(&darktable.control->log_mutex);
  dt_control_queue_draw_all();
  return FALSE;
}


void dt_control_button_pressed(double x, double y, int which, int type, uint32_t state)
{
  float tb = darktable.control->tabborder;
  darktable.control->button_down = 1;
  darktable.control->button_down_which = which;
  darktable.control->button_type = type;
  darktable.control->button_x = x - tb;
  darktable.control->button_y = y - tb;
  float wd = darktable.control->width;
  float ht = darktable.control->height;

  // ack log message:
  pthread_mutex_lock(&darktable.control->log_mutex);
  const float /*xc = wd/4.0-20,*/ yc = ht*0.85+10;
  if(darktable.control->log_ack != darktable.control->log_pos)
  if(which == 1 /*&& x > xc - 10 && x < xc + 10*/ && y > yc - 10 && y < yc + 10)
  {
    if(darktable.control->log_message_timeout_id) g_source_remove(darktable.control->log_message_timeout_id);
    darktable.control->log_ack = (darktable.control->log_ack+1)%DT_CTL_LOG_SIZE;
    pthread_mutex_unlock(&darktable.control->log_mutex);
    return;
  }
  pthread_mutex_unlock(&darktable.control->log_mutex);

  if(x > tb && x < wd-tb && y > tb && y < ht-tb)
  {
    if(!dt_view_manager_button_pressed(darktable.view_manager, x-tb, y-tb, which, type, state))
    if(type == GDK_2BUTTON_PRESS && which == 1) dt_ctl_switch_mode();
  }
}

void dt_control_log(const char* msg, ...)
{
  pthread_mutex_lock(&darktable.control->log_mutex);
  va_list ap;
  va_start(ap, msg);
  vsnprintf(darktable.control->log_message[darktable.control->log_pos], DT_CTL_LOG_MSG_SIZE, msg, ap);
  va_end(ap);
  if(darktable.control->log_message_timeout_id) g_source_remove(darktable.control->log_message_timeout_id);
  darktable.control->log_ack = darktable.control->log_pos;
  darktable.control->log_pos = (darktable.control->log_pos+1)%DT_CTL_LOG_SIZE;
  darktable.control->log_message_timeout_id=g_timeout_add(DT_CTL_LOG_TIMEOUT,_dt_ctl_log_message_timeout_callback,NULL);
  pthread_mutex_unlock(&darktable.control->log_mutex);
  dt_control_queue_draw_all();
}

void dt_control_log_busy_enter()
{
  pthread_mutex_lock(&darktable.control->log_mutex);
  darktable.control->log_busy++;
  pthread_mutex_unlock(&darktable.control->log_mutex);
  dt_control_queue_draw_all();
}

void dt_control_log_busy_leave()
{
  pthread_mutex_lock(&darktable.control->log_mutex);
  darktable.control->log_busy--;
  pthread_mutex_unlock(&darktable.control->log_mutex);
  dt_control_queue_draw_all();
}

void dt_control_gui_queue_draw()
{
  if(dt_control_running())
  {
    GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "center");
    gtk_widget_queue_draw(widget);
  }
}

void dt_control_queue_draw_all()
{
  if(dt_control_running())
  {
    int needlock = pthread_self() != darktable.control->gui_thread;
    if(needlock) gdk_threads_enter();
    GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "center");
    gtk_widget_queue_draw(widget);
    if(needlock) gdk_threads_leave();
  }
}

void dt_control_queue_draw(GtkWidget *widget)
{
  if(dt_control_running())
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

  widget = glade_xml_get_widget (darktable.gui->main_window, "lighttable_layout_combobox");
  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), dt_conf_get_int("plugins/lighttable/layout"));

  widget = glade_xml_get_widget (darktable.gui->main_window, "lighttable_zoom_spinbutton");
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_int("plugins/lighttable/images_in_row"));

  widget = glade_xml_get_widget (darktable.gui->main_window, "image_filter");
  dt_lib_filter_t filter = dt_conf_get_int("ui_last/combo_filter");
  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), (int)filter);

  widget = glade_xml_get_widget (darktable.gui->main_window, "image_sort");
  dt_lib_sort_t sort = dt_conf_get_int("ui_last/combo_sort");
  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), (int)sort);

  bit = dt_conf_get_int("ui_last/panel_left");
  widget = glade_xml_get_widget (darktable.gui->main_window, "left");
  if(bit & (1<<mode)) gtk_widget_show(widget);
  else gtk_widget_hide(widget);

  bit = dt_conf_get_int("ui_last/panel_right");
  widget = glade_xml_get_widget (darktable.gui->main_window, "right");
  if(bit & (1<<mode)) gtk_widget_show(widget);
  else gtk_widget_hide(widget);

  bit = dt_conf_get_int("ui_last/panel_top");
  widget = glade_xml_get_widget (darktable.gui->main_window, "top");
  if(bit & (1<<mode)) gtk_widget_show(widget);
  else gtk_widget_hide(widget);

  bit = dt_conf_get_int("ui_last/panel_bottom");
  widget = glade_xml_get_widget (darktable.gui->main_window, "bottom");
  if(bit & (1<<mode)) gtk_widget_show(widget);
  else gtk_widget_hide(widget);

  bit = dt_conf_get_int("ui_last/expander_navigation");
  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation_expander");
  gtk_expander_set_expanded(GTK_EXPANDER(widget), (bit & (1<<mode)) != 0);

  bit = dt_conf_get_int("ui_last/expander_library");
  widget = glade_xml_get_widget (darktable.gui->main_window, "library_expander");
  gtk_expander_set_expanded(GTK_EXPANDER(widget), (bit & (1<<mode)) != 0);

  bit = dt_conf_get_int("ui_last/expander_snapshots");
  widget = glade_xml_get_widget (darktable.gui->main_window, "snapshots_expander");
  gtk_expander_set_expanded(GTK_EXPANDER(widget), (bit & (1<<mode)) != 0);

  bit = dt_conf_get_int("ui_last/expander_history");
  widget = glade_xml_get_widget (darktable.gui->main_window, "history_expander");
  gtk_expander_set_expanded(GTK_EXPANDER(widget), (bit & (1<<mode)) != 0);

  bit = dt_conf_get_int("ui_last/expander_histogram");
  widget = glade_xml_get_widget (darktable.gui->main_window, "histogram_expander");
  gtk_expander_set_expanded(GTK_EXPANDER(widget), (bit & (1<<mode)) != 0);

  bit = dt_conf_get_int("ui_last/expander_metadata");
  widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_expander");
  gtk_expander_set_expanded(GTK_EXPANDER(widget), (bit & (1<<mode)) != 0);
}

void dt_control_save_gui_settings(dt_ctl_gui_mode_t mode)
{
  int8_t bit;
  GtkWidget *widget;

  bit = dt_conf_get_int("ui_last/panel_left");
  widget = glade_xml_get_widget (darktable.gui->main_window, "left");
  if(GTK_WIDGET_VISIBLE(widget)) bit |=   1<<mode;
  else                           bit &= ~(1<<mode);
  dt_conf_set_int("ui_last/panel_left", bit);

  bit = dt_conf_get_int("ui_last/panel_right");
  widget = glade_xml_get_widget (darktable.gui->main_window, "right");
  if(GTK_WIDGET_VISIBLE(widget)) bit |=   1<<mode;
  else                           bit &= ~(1<<mode);
  dt_conf_set_int("ui_last/panel_right", bit);

  bit = dt_conf_get_int("ui_last/panel_bottom");
  widget = glade_xml_get_widget (darktable.gui->main_window, "bottom");
  if(GTK_WIDGET_VISIBLE(widget)) bit |=   1<<mode;
  else                           bit &= ~(1<<mode);
  dt_conf_set_int("ui_last/panel_bottom", bit);

  bit = dt_conf_get_int("ui_last/panel_top");
  widget = glade_xml_get_widget (darktable.gui->main_window, "top");
  if(GTK_WIDGET_VISIBLE(widget)) bit |=   1<<mode;
  else                           bit &= ~(1<<mode);
  dt_conf_set_int("ui_last/panel_top", bit);

  bit = dt_conf_get_int("ui_last/expander_navigation");
  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation_expander");
  if(gtk_expander_get_expanded(GTK_EXPANDER(widget))) bit |= 1<<mode;
  else bit &= ~(1<<mode);
  dt_conf_set_int("ui_last/expander_navigation", bit);

  bit = dt_conf_get_int("ui_last/expander_library");
  widget = glade_xml_get_widget (darktable.gui->main_window, "library_expander");
  if(gtk_expander_get_expanded(GTK_EXPANDER(widget))) bit |= 1<<mode;
  else bit &= ~(1<<mode);
  dt_conf_set_int("ui_last/expander_library", bit);

  bit = dt_conf_get_int("ui_last/expander_snapshots");
  widget = glade_xml_get_widget (darktable.gui->main_window, "snapshots_expander");
  if(gtk_expander_get_expanded(GTK_EXPANDER(widget))) bit |= 1<<mode;
  else bit &= ~(1<<mode);
  dt_conf_set_int("ui_last/expander_snapshots", bit);

  bit = dt_conf_get_int("ui_last/expander_history");
  widget = glade_xml_get_widget (darktable.gui->main_window, "history_expander");
  if(gtk_expander_get_expanded(GTK_EXPANDER(widget))) bit |= 1<<mode;
  else bit &= ~(1<<mode);
  dt_conf_set_int("ui_last/expander_history", bit);

  bit = dt_conf_get_int("ui_last/expander_histogram");
  widget = glade_xml_get_widget (darktable.gui->main_window, "histogram_expander");
  if(gtk_expander_get_expanded(GTK_EXPANDER(widget))) bit |= 1<<mode;
  else bit &= ~(1<<mode);
  dt_conf_set_int("ui_last/expander_histogram", bit);

  bit = dt_conf_get_int("ui_last/expander_metadata");
  widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_expander");
  if(gtk_expander_get_expanded(GTK_EXPANDER(widget))) bit |= 1<<mode;
  else bit &= ~(1<<mode);
  dt_conf_set_int("ui_last/expander_metadata", bit);
}

int dt_control_key_pressed_override(uint16_t which)
{
  int fullscreen, visible;
  GtkWidget *widget;
  /* check if key accelerators are enabled*/
  if (darktable.control->key_accelerators_on != 1) return 0;
  
  switch (which)
  {
    case KEYCODE_F7:
      dt_gui_contrast_decrease ();
      break;
    case KEYCODE_F8:
      dt_gui_contrast_increase ();
      break;
    
    case KEYCODE_F11:
      widget = glade_xml_get_widget (darktable.gui->main_window, "main_window");
      fullscreen = dt_conf_get_bool("ui_last/fullscreen");
      if(fullscreen) gtk_window_unfullscreen(GTK_WINDOW(widget));
      else           gtk_window_fullscreen  (GTK_WINDOW(widget));
      fullscreen ^= 1;
      dt_conf_set_bool("ui_last/fullscreen", fullscreen);
      dt_dev_invalidate(darktable.develop);
      break;
    case KEYCODE_Escape: case KEYCODE_Caps:
      widget = glade_xml_get_widget (darktable.gui->main_window, "main_window");
      gtk_window_unfullscreen(GTK_WINDOW(widget));
      fullscreen = 0;
      dt_conf_set_bool("ui_last/fullscreen", fullscreen);
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
      return 0;
      break;
  }

  widget = glade_xml_get_widget (darktable.gui->main_window, "center");
  gtk_widget_queue_draw(widget);
  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation");
  gtk_widget_queue_draw(widget);
  return 1;
}

int dt_control_key_pressed(uint16_t which)
{
  // this line is here to find the right key code on different platforms (mac).
  // printf("key code pressed: %d\n", which);
  GtkWidget *widget;
  int needRedraw=0;
  switch (which)
  {
    case KEYCODE_period:
      dt_ctl_switch_mode();
      needRedraw=1;
      break;
    default:
      // propagate to view modules.
      needRedraw = dt_view_manager_key_pressed(darktable.view_manager, which);
      break;
  }
  if( needRedraw ) {
    widget = glade_xml_get_widget (darktable.gui->main_window, "center");
    gtk_widget_queue_draw(widget);
    widget = glade_xml_get_widget (darktable.gui->main_window, "navigation");
    gtk_widget_queue_draw(widget);
  }
  return 1;
}

int dt_control_key_released(uint16_t which)
{
  // this line is here to find the right key code on different platforms (mac).
  // printf("key code pressed: %d\n", which);
  GtkWidget *widget;
  switch (which)
  {
    default:
      // propagate to view modules.
      dt_view_manager_key_released(darktable.view_manager, which);
      break;
  }

  widget = glade_xml_get_widget (darktable.gui->main_window, "center");
  gtk_widget_queue_draw(widget);
  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation");
  gtk_widget_queue_draw(widget);
  return 1;
}
#include "gui/iop_history.h"
void dt_control_add_history_item(int32_t num_in, const char *label)
{
  dt_gui_iop_history_add_item(num_in,label);
}

void dt_control_clear_history_items(int32_t num)
{
  /* reset if empty stack */
  if( num == -1 ) 
  {
  dt_gui_iop_history_reset();
  return;
  }
  
  /* pop items from top of history */
  int size = dt_gui_iop_history_get_top () - MAX(0, num);
  for(int k=1;k<size;k++)
    dt_gui_iop_history_pop_top ();

  dt_gui_iop_history_update_labels ();
  
}

void dt_control_update_recent_films()
{
  int needlock = pthread_self() != darktable.control->gui_thread;
  if(needlock) gdk_threads_enter();
  /* get the recent film vbox */
  GtkWidget *sb = glade_xml_get_widget (darktable.gui->main_window, "recent_used_film_rolls_section_box");
  GtkWidget *recent_used_film_vbox = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (sb)),1);
  
  /* hide all childs and vbox*/
  gtk_widget_hide_all (recent_used_film_vbox);
  GList *childs = gtk_container_get_children (GTK_CONTAINER(recent_used_film_vbox));
  
  /* query database for recent films */
  sqlite3_stmt *stmt;
  int rc, num = 0;
  const char *filename, *cnt;
  const int label_cnt = 256;
  char label[256];
  rc = sqlite3_prepare_v2(darktable.db, "select folder,id from film_rolls order by datetime_accessed desc limit 0, 4", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 1);
    if(id == 1)
    {
      snprintf(label, 256, _("single images"));
      filename = _("single images");
    }
    else
    {
      filename = (char *)sqlite3_column_text(stmt, 0);
      cnt = filename + MIN(512,strlen(filename));
      int i;
      for(i=0;i<label_cnt-4;i++) if(cnt > filename && *cnt != '/') cnt--;
      if(cnt > filename) snprintf(label, label_cnt, "%s", cnt+1);
      else snprintf(label, label_cnt, "%s", cnt);
    }
    GtkWidget *widget = g_list_nth_data (childs,num);
    gtk_button_set_label (GTK_BUTTON (widget), label);
    
    GtkLabel *label = GTK_LABEL(gtk_bin_get_child(GTK_BIN(widget)));
    gtk_label_set_ellipsize (label, PANGO_ELLIPSIZE_START);
    gtk_label_set_max_width_chars (label, 30);
    
    g_object_set(G_OBJECT(widget), "tooltip-text", filename, NULL);
    
    gtk_widget_show(recent_used_film_vbox);
    gtk_widget_show(widget);
    
    num++;
  }
  
  GtkEntry *entry = GTK_ENTRY(glade_xml_get_widget (darktable.gui->main_window, "entry_film"));
  dt_gui_filmview_update(gtk_entry_get_text(entry));
  if(needlock) gdk_threads_leave();
}

