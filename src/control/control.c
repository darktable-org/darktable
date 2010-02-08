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
  dt_conf_set_float("preview_subsample", .5f);
  dt_conf_set_int  ("mipmap_cache_thumbnails", 500);
  dt_conf_set_int  ("mipmap_cache_full_images", 1);

  dt_conf_set_int  ("ui_last/select_action", 0);
  dt_conf_set_bool ("ui_last/fullscreen", FALSE);
  dt_conf_set_int  ("ui_last/view", DT_MODE_NONE);

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
  rc = sqlite3_exec(darktable.db, "PRAGMA synchronous=off", NULL, NULL, NULL);
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

    // insert new tables, if not there (statement will just fail if so):
    sqlite3_exec(darktable.db, "create table iop_defaults (operation varchar, op_params blob, enabled integer, model varchar, maker varchar, primary key(operation, model, maker))", NULL, NULL, NULL);

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
      sqlite3_exec(darktable.db, "drop table iop_defaults", NULL, NULL, NULL);
      return dt_control_load_config(c);
    }
    else
    {
      pthread_mutex_unlock(&(darktable.control->global_mutex));
      // TODO: get last from sql query!
      // dt_film_roll_open(darktable.library->film, film_id);
      // weg: dt_film_roll_import(darktable.library->film, darktable.control->global_settings.lib_last_film);
    }
#endif
  }
  else
  { // db not yet there, create it
    // dt_ctl_settings_default(darktable.control);
    rc = sqlite3_finalize(stmt);
    rc = sqlite3_exec(darktable.db, "create table settings (settings blob)", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table film_rolls (id integer primary key, datetime_accessed char(20), folder varchar(1024))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table images (id integer primary key, film_id integer, width int, height int, filename varchar(256), maker varchar(30), model varchar(30), lens varchar(30), exposure real, aperture real, iso real, focal_length real, datetime_taken char(20), flags integer, output_width integer, output_height integer, crop real, raw_parameters integer, raw_denoise_threshold real, raw_auto_bright_threshold real, foreign key(film_id) references film_rolls(id))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table mipmaps (imgid int, level int, data blob, foreign key(imgid) references images(id))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table mipmap_timestamps (imgid int, level int, foreign key(imgid) references images(id))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table selected_images (imgid integer, foreign key(imgid) references images(id))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table history (imgid integer, num integer, module integer, operation varchar(256), op_params blob, enabled integer, foreign key(imgid) references images(id))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table tags (id integer primary key, name varchar, icon blob)", NULL, NULL, NULL);
    rc = sqlite3_exec(darktable.db, "create table tagxtag (id1 integer, id2 integer, count integer, foreign key (id1) references tags(id) foreign key (id2) references tags(id) primary key(id1, id2))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_exec(darktable.db, "create table tagged_images (imgid integer, tagid integer, foreign key(imgid) references images(id) foreign key(tagid) references tags(id) primary key(imgid, tagid))", NULL, NULL, NULL);
    HANDLE_SQLITE_ERR(rc);
    sqlite3_exec(darktable.db, "create table iop_defaults (operation varchar, op_params blob, enabled integer, model varchar, maker varchar)", NULL, NULL, NULL);

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
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "main_window");
  gtk_window_resize(GTK_WINDOW(widget), width, height);
  dt_control_restore_gui_settings(DT_LIBRARY);
  dt_control_update_recent_films();
  return 0;
}

int dt_control_write_config(dt_control_t *c)
{
  dt_ctl_gui_mode_t gui = dt_conf_get_int("ui_last/view");
  dt_control_save_gui_settings(gui);

  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "main_window");
  dt_conf_set_int ("ui_last/window_x",  widget->allocation.x);
  dt_conf_set_int ("ui_last/window_y",  widget->allocation.y);
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
  Boolean foo;
  CMFlattenProfile(prof, 0, dt_ctl_lcms_flatten_profile, &transfer, &foo);
  CMCloseProfile(prof);

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

  s->log_pos = s->log_ack = 0;
  pthread_mutex_init(&(s->log_mutex), NULL);
  s->progress = 200.0f;

  dt_conf_set_int("ui_last/view", DT_MODE_NONE);

  // if config is old, replace with new defaults.
  if(DT_CONFIG_VERSION > dt_conf_get_int("config_version"))
    dt_ctl_settings_default(s);

  pthread_cond_init(&s->cond, NULL);
  pthread_mutex_init(&s->cond_mutex, NULL);
  pthread_mutex_init(&s->queue_mutex, NULL);

  int k; for(k=0;k<DT_CONTROL_MAX_JOBS;k++) s->idle[k] = k;
  s->idle_top = DT_CONTROL_MAX_JOBS;
  s->queued_top = 0;
  // start threads
  s->num_threads = dt_ctl_get_num_procs();
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
  int keep  = MAX(0, MIN( 100000, dt_conf_get_int("database_cache_thumbnails")));
  int keep0 = MAX(0, MIN(1000000, dt_conf_get_int("database_cache_thumbnails0")));
  int rc;
  // delete mipmaps
  // ubuntu compiles a lame sqlite3, else we could simply:
  // rc = sqlite3_exec(darktable.db, "delete from mipmaps order by imgid desc limit 2500,-1)", NULL, NULL, NULL);
  printf("[control_cleanup] freeing unused database chunks...\n");
  sqlite3_stmt *stmt, *stmt2;
  rc = sqlite3_prepare_v2(darktable.db, "select imgid, level from mipmap_timestamps where level != 0 order by rowid desc limit ?1,-1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, keep);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int (stmt, 0);
    int level = sqlite3_column_int (stmt, 1);
    rc = sqlite3_prepare_v2(darktable.db, "delete from mipmaps where imgid = ?1 and level = ?2", -1, &stmt2, NULL);
    rc = sqlite3_bind_int (stmt2, 1, imgid);
    rc = sqlite3_bind_int (stmt2, 2, level);
    rc = sqlite3_step(stmt2);
    rc = sqlite3_finalize(stmt2);
  }
  rc = sqlite3_finalize(stmt);

  rc = sqlite3_prepare_v2(darktable.db, "select imgid, level from mipmap_timestamps where level = 0 order by rowid desc limit ?1,-1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, keep0);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int (stmt, 0);
    int level = sqlite3_column_int (stmt, 1);
    rc = sqlite3_prepare_v2(darktable.db, "delete from mipmaps where imgid = ?1 and level = ?2", -1, &stmt2, NULL);
    rc = sqlite3_bind_int (stmt2, 1, imgid);
    rc = sqlite3_bind_int (stmt2, 2, level);
    rc = sqlite3_step(stmt2);
    rc = sqlite3_finalize(stmt2);
  }
  rc = sqlite3_finalize(stmt);
  printf("[control_cleanup] done.\n");

  // vacuum TODO: optional?
  // rc = sqlite3_exec(darktable.db, "PRAGMA incremental_vacuum(0)", NULL, NULL, NULL);
  // rc = sqlite3_exec(darktable.db, "vacuum", NULL, NULL, NULL);
  pthread_mutex_destroy(&s->queue_mutex);
  pthread_mutex_destroy(&s->cond_mutex);
  pthread_mutex_destroy(&s->log_mutex);
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

  dt_print(DT_DEBUG_CONTROL, "[run_job_res %d] ", (size_t)pthread_self());
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
    fprintf(stderr, "[ctl_add_job] too many jobs in queue!\n");
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
  int32_t i;
  pthread_mutex_lock(&s->queue_mutex);
  dt_print(DT_DEBUG_CONTROL, "[revive_job] ");
  dt_control_job_print(job);
  dt_print(DT_DEBUG_CONTROL, "\n");
  for(i=0;i<s->queued_top;i++)
  { // find equivalent job and push it up on top of the stack.
    const int j = s->queued[i];
    if(!memcmp(job, s->job + j, sizeof(dt_job_t)))
    {
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
      cairo_text_extents (cr, darktable.control->log_message[darktable.control->log_pos-1], &ext);
      const float pad = 20.0f, xc = width/2.0, yc = height*0.85+10, wd = pad + ext.width*.5f;
      float rad = 14;
      cairo_set_line_width(cr, 1.);
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
      cairo_show_text (cr, darktable.control->log_message[darktable.control->log_pos-1]);
    }
    pthread_mutex_unlock(&darktable.control->log_mutex);

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

void dt_ctl_switch_mode_to(dt_ctl_gui_mode_t mode)
{
  dt_ctl_gui_mode_t oldmode = dt_conf_get_int("ui_last/view");
  if(oldmode == mode) return;
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
  snprintf(buf, 512, _("<span color=\"#7f7f7f\"><big><b><i>%s mode</i></b></big></span>"), dt_view_manager_name(darktable.view_manager));
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

void dt_control_button_pressed(double x, double y, int which, int type, uint32_t state)
{
  float tb = darktable.control->tabborder;
  darktable.control->button_down = 1;
  darktable.control->button_down_which = which;
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
    darktable.control->log_ack = (darktable.control->log_ack+1)%DT_CTL_LOG_SIZE;
    pthread_mutex_unlock(&darktable.control->log_mutex);
    return;
  }
  pthread_mutex_unlock(&darktable.control->log_mutex);

  if(x > tb && x < wd-tb && y > tb && y < ht-tb)
  {
    if(type == GDK_2BUTTON_PRESS && which == 1) dt_ctl_switch_mode();
    else dt_view_manager_button_pressed(darktable.view_manager, x-tb, y-tb, which, type, state);
  }
}

void dt_control_log(const char* msg, ...)
{
  pthread_mutex_lock(&darktable.control->log_mutex);
  va_list ap;
  va_start(ap, msg);
  vsnprintf(darktable.control->log_message[darktable.control->log_pos], DT_CTL_LOG_MSG_SIZE, msg, ap);
  va_end(ap);
  darktable.control->log_ack = darktable.control->log_pos;
  darktable.control->log_pos = (darktable.control->log_pos+1)%DT_CTL_LOG_SIZE;
  pthread_mutex_unlock(&darktable.control->log_mutex);
  dt_control_queue_draw_all();
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
    int needlock = pthread_self() != darktable.control->gui_thread;
    if(needlock) gdk_threads_enter();
    GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "center");
    gtk_widget_queue_draw(widget);
    if(needlock) gdk_threads_leave();
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

  widget = glade_xml_get_widget (darktable.gui->main_window, "select_action");
  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), dt_conf_get_int("ui_last/select_action"));

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
  switch (which)
  {
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
  switch (which)
  {
    case KEYCODE_period:
      dt_ctl_switch_mode();
      break;
    default:
      // propagate to view modules.
      dt_view_manager_key_pressed(darktable.view_manager, which);
      break;
  }

  widget = glade_xml_get_widget (darktable.gui->main_window, "center");
  gtk_widget_queue_draw(widget);
  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation");
  gtk_widget_queue_draw(widget);
  return 1;
}

void dt_control_add_history_item(int32_t num_in, const char *label)
{
  char wdname[20], numlabel[256];
  const gchar *lbl = NULL;
  int32_t num = num_in + 1; // one after orginal
  GtkWidget *widget = NULL;
  g_snprintf(numlabel, 256, "%d - %s", num, label);
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
  darktable.gui->reset = 1;
  gtk_object_set(GTK_OBJECT(widget), "active", TRUE, NULL);
  darktable.gui->reset = 0;
}

void dt_control_clear_history_items(int32_t num)
{
  // clear history items down to num (leave num-th on stack)
  darktable.control->history_start = MAX(0, num - 8);
  char wdname[20], numlabel[256], numlabel2[256];
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
    dt_dev_get_history_item_label(hist, numlabel2, 256);
    snprintf(numlabel, 256, "%d - %s", curr, numlabel2);
    gtk_button_set_label(GTK_BUTTON(widget), numlabel);
    if(curr == num+1)
    {
      darktable.gui->reset = 1;
      gtk_object_set(GTK_OBJECT(widget), "active", TRUE, NULL);
      darktable.gui->reset = 0;
    }
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
  const int label_cnt = 25;
  char label[256];
  // FIXME: this is a temporary hack to keep the database low:
  // remove all data from db from all other films.
  rc = sqlite3_prepare_v2(darktable.db, "select folder,id from film_rolls order by datetime_accessed desc limit 0, 4", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 1);
    if(id == 1)
    {
      snprintf(label, 256, _("single images"));
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
    num++;
  }
  sqlite3_finalize(stmt);
  GtkEntry *entry = GTK_ENTRY(glade_xml_get_widget (darktable.gui->main_window, "entry_film"));
  dt_gui_filmview_update(gtk_entry_get_text(entry));
}

