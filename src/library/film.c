#include "library/library.h"
#include "common/imageio.h"
#include "control/control.h"
#include "control/jobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <assert.h>

void dt_film_roll_init(dt_film_roll_t *film)
{
  pthread_mutex_init(&film->images_mutex, NULL);
  film->last_loaded = film->num_images = film->last_exported = 0;
  film->dirname[0] = '\0';
  film->dir = NULL;
}

void dt_film_import1(dt_film_roll_t *film)
{
  const gchar *d_name;
  char filename[1024];
  dt_image_t image;

  while(1)
  {
    pthread_mutex_lock(&film->images_mutex);
    if (film->dir && (d_name = g_dir_read_name(film->dir)) && darktable.control->running)
    {
      snprintf(filename, 1024, "%s/%s", film->dirname, d_name);
      image.film_id = film->id;
      film->last_loaded++;
    }
    else
    {
      if(film->dir)
      {
        g_dir_close(film->dir);
        film->dir = NULL;
      }
      darktable.control->progress = 200.0f;
      pthread_mutex_unlock(&film->images_mutex);
      return;
    }
    pthread_mutex_unlock(&film->images_mutex);

    if(!dt_image_import(film->id, filename))
    {
      pthread_mutex_lock(&film->images_mutex);
      darktable.control->progress = 100.0f*film->last_loaded/(float)film->num_images;
      pthread_mutex_unlock(&film->images_mutex);
      dt_control_queue_draw();
    } // else not an image.
  }
}

void dt_film_roll_cleanup(dt_film_roll_t *film)
{
  pthread_mutex_destroy(&film->images_mutex);
}

int dt_film_roll_open(dt_film_roll_t *film, const int32_t id)
{
  // TODO:
  // select id from images where film_id = id
  // and prefetch to cache using image_open
  // TODO: update last used film date stamp
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
  return 1;
}
int dt_film_roll_import(dt_film_roll_t *film, const char *dirname)
{
  film->id = -1;
  int rc;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "select id from film_rolls where folder = ?1", -1, &stmt, NULL);
  HANDLE_SQLITE_ERR(rc);
  rc = sqlite3_bind_text(stmt, 1, dirname, strlen(dirname), SQLITE_STATIC);
  HANDLE_SQLITE_ERR(rc);
  if(sqlite3_step(stmt) == SQLITE_ROW) film->id = sqlite3_column_int(stmt, 0);
  rc = sqlite3_finalize(stmt);
  if(film->id <= 0)
  {
    // TODO: insert timestamp
    // gsize gret = g_date_strftime(gchar *s, gsize slen, "%Y:%m:%d %H:%M:%S", const GDate *date);

    rc = sqlite3_prepare_v2(darktable.db, "insert into film_rolls (id, folder) values (null, ?1)", -1, &stmt, NULL);
    HANDLE_SQLITE_ERR(rc);
    rc = sqlite3_bind_text(stmt, 1, dirname, strlen(dirname), SQLITE_STATIC);
    HANDLE_SQLITE_ERR(rc);
    pthread_mutex_lock(&(darktable.db_insert));
    rc = sqlite3_step(stmt);
    if(rc != SQLITE_DONE) fprintf(stderr, "failed to insert film roll! %s\n", sqlite3_errmsg(darktable.db));
    rc = sqlite3_finalize(stmt);
    film->id = sqlite3_last_insert_rowid(darktable.db);
    pthread_mutex_unlock(&(darktable.db_insert));
  }
  if(film->id <= 0) return 1;

  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);

  film->last_loaded = 0;
  strncpy(film->dirname, dirname, 512);
  film->dir = g_dir_open(film->dirname, 0, NULL);
  darktable.control->progress = .001f;
  for(int k=0;k<MAX(1,dt_ctl_get_num_procs()-1);k++) // keep one proc for the user.
  {
    dt_job_t j;
    dt_film_import1_init(&j, film);
    dt_control_add_job(darktable.control, &j);
  }
  // TODO: update last used film date stamp
  return 0;
}

