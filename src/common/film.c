/*
   This file is part of darktable,
   copyright (c) 2009--2010 johannes hanika.
   copyright (c) 2011-2012 henrik andersson.

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
#include "common/darktable.h"
#include "control/control.h"
#include "control/conf.h"
#include "control/jobs.h"
#include "control/progress.h"
#include "common/film.h"
#include "common/dtpthread.h"
#include "common/collection.h"
#include "common/image_cache.h"
#include "common/debug.h"
#include "views/view.h"

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <assert.h>

void dt_film_init(dt_film_t *film)
{
  dt_pthread_mutex_init(&film->images_mutex, NULL);
  film->last_loaded = film->num_images = 0;
  film->dirname[0] = '\0';
  film->dir = NULL;
  film->id = -1;
  film->ref = 0;
}

void dt_film_cleanup(dt_film_t *film)
{
  dt_pthread_mutex_destroy(&film->images_mutex);
  if(film->dir)
  {
    g_dir_close(film->dir);
    film->dir = NULL;
  }
}

void dt_film_set_query(const int32_t id)
{
  /* enable film id filter and set film id */
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
  dt_conf_set_int("plugins/lighttable/collect/item0", 0);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select id, folder from film_rolls where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_conf_set_string("plugins/lighttable/collect/string0", (gchar *)sqlite3_column_text(stmt, 1));
  }
  sqlite3_finalize(stmt);
  dt_collection_update_query(darktable.collection);
}

/** open film with given id. */
int dt_film_open2(dt_film_t *film)
{
  /* check if we got a decent film id */
  if(film->id < 0) return 1;

  /* query database for id and folder */
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select id, folder from film_rolls where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film->id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    /* fill out the film dirname */
    snprintf(film->dirname, sizeof(film->dirname), "%s", (gchar *)sqlite3_column_text(stmt, 1));
    sqlite3_finalize(stmt);
    char datetime[20];
    dt_gettime(datetime, sizeof(datetime));

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "update film_rolls set datetime_accessed = ?1 where id = ?2", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, datetime, -1, SQLITE_STATIC);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, film->id);
    sqlite3_step(stmt);

    sqlite3_finalize(stmt);
    dt_film_set_query(film->id);
    dt_control_queue_redraw_center();
    dt_view_manager_reset(darktable.view_manager);
    return 0;
  }
  else
    sqlite3_finalize(stmt);

  /* failure */
  return 1;
}

int dt_film_open(const int32_t id)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select id, folder from film_rolls where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    char datetime[20];
    dt_gettime(datetime, sizeof(datetime));

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "update film_rolls set datetime_accessed = ?1 where id = ?2", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, datetime, -1, SQLITE_STATIC);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, id);
    sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  // TODO: prefetch to cache using image_open
  dt_film_set_query(id);
  dt_control_queue_redraw_center();
  dt_view_manager_reset(darktable.view_manager);
  return 0;
}

// FIXME: needs a rewrite
int dt_film_open_recent(const int num)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select id from film_rolls order by datetime_accessed desc limit ?1,1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, num);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if(dt_film_open(id)) return 1;
    char datetime[20];
    dt_gettime(datetime, sizeof(datetime));
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "update film_rolls set datetime_accessed = ?1 where id = ?2", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, datetime, -1, SQLITE_STATIC);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, id);
    sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  // dt_control_update_recent_films();
  return 0;
}

int dt_film_new(dt_film_t *film, const char *directory)
{
  // Try open filmroll for folder if exists
  film->id = -1;
  int rc;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from film_rolls where folder = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, directory, -1, SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW) film->id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if(film->id <= 0)
  {
    // create a new filmroll
    sqlite3_stmt *stmt;
    char datetime[20];
    dt_gettime(datetime, sizeof(datetime));
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "insert into film_rolls (id, datetime_accessed, folder) "
                                "values (null, ?1, ?2)",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, datetime, -1, SQLITE_STATIC);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, directory, -1, SQLITE_STATIC);
    dt_pthread_mutex_lock(&darktable.db_insert);
    rc = sqlite3_step(stmt);
    if(rc != SQLITE_DONE)
      fprintf(stderr, "[film_new] failed to insert film roll! %s\n",
              sqlite3_errmsg(dt_database_get(darktable.db)));
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from film_rolls where folder=?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, directory, -1, SQLITE_STATIC);
    if(sqlite3_step(stmt) == SQLITE_ROW) film->id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    dt_pthread_mutex_unlock(&darktable.db_insert);
  }

  if(film->id <= 0) return 0;
  g_strlcpy(film->dirname, directory, sizeof(film->dirname));
  film->last_loaded = 0;
  return film->id;
}

int dt_film_import(const char *dirname)
{
  int rc;
  sqlite3_stmt *stmt;

  /* initialize a film object*/
  dt_film_t *film = (dt_film_t *)malloc(sizeof(dt_film_t));
  dt_film_init(film);
  film->id = -1;

  /* lookup if film exists and reuse id */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from film_rolls where folder = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, dirname, -1, SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW) film->id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  /* if we didn't find a id, lets instansiate a new filmroll */
  if(film->id <= 0)
  {
    char datetime[20];
    dt_gettime(datetime, sizeof(datetime));
    /* insert a new film roll into database */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "insert into film_rolls (id, datetime_accessed, folder) values "
                                "(null, ?1, ?2)",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, datetime, -1, SQLITE_STATIC);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, dirname, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if(rc != SQLITE_DONE)
      fprintf(stderr, "[film_import] failed to insert film roll! %s\n",
              sqlite3_errmsg(dt_database_get(darktable.db)));
    sqlite3_finalize(stmt);

    /* requery for filmroll and fetch new id */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from film_rolls where folder=?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, dirname, -1, SQLITE_STATIC);
    if(sqlite3_step(stmt) == SQLITE_ROW) film->id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }

  /* bail out if we got troubles */
  if(film->id <= 0)
  {
    // if the film is empty => remove it again.
    if(dt_film_is_empty(film->id))
    {
      dt_film_remove(film->id);
    }
    dt_film_cleanup(film);
    free(film);
    return 0;
  }

  /* at last put import film job on queue */
  film->last_loaded = 0;
  g_strlcpy(film->dirname, dirname, sizeof(film->dirname));
  film->dir = g_dir_open(film->dirname, 0, NULL);
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, dt_film_import1_create(film));

  return film->id;
}


static GList *_film_recursive_get_files(const gchar *path, gboolean recursive, GList **result)
{
  gchar *fullname;

  /* let's try open current dir */
  GDir *cdir = g_dir_open(path, 0, NULL);
  if(!cdir) return *result;

  /* lets read all files in current dir, recurse
     into directories if we should import recursive.
   */
  do
  {
    /* get the current filename */
    const gchar *filename = g_dir_read_name(cdir);

    /* return if no more files are in current dir */
    if(!filename) break;
    if(filename[0] == '.') continue;

    /* build full path for filename */
    fullname = g_build_filename(G_DIR_SEPARATOR_S, path, filename, NULL);

    /* recurse into directory if we hit one and we doing a recursive import */
    if(recursive && g_file_test(fullname, G_FILE_TEST_IS_DIR))
    {
      *result = _film_recursive_get_files(fullname, recursive, result);
      g_free(fullname);
    }
    /* or test if we found a supported image format to import */
    else if(!g_file_test(fullname, G_FILE_TEST_IS_DIR) && dt_supported_image(filename))
      *result = g_list_append(*result, fullname);
    else
      g_free(fullname);

  } while(TRUE);

  /* cleanup and return results */
  g_dir_close(cdir);

  return *result;
}

/* compare used for sorting the list of files to import
   only sort on basename of full path eg. the actually filename.
*/
int _film_filename_cmp(gchar *a, gchar *b)
{
  gchar *a_basename = g_path_get_basename(a);
  gchar *b_basename = g_path_get_basename(b);
  int ret = g_strcmp0(a_basename, b_basename);
  g_free(a_basename);
  g_free(b_basename);
  return ret;
}

void dt_film_import1(dt_film_t *film)
{
  gboolean recursive = dt_conf_get_bool("ui_last/import_recursive");

  /* first of all gather all images to import */
  GList *images = NULL;
  images = _film_recursive_get_files(film->dirname, recursive, &images);
  if(g_list_length(images) == 0)
  {
    dt_control_log(_("no supported images were found to be imported"));
    return;
  }

  /* we got ourself a list of images, lets sort and start import */
  images = g_list_sort(images, (GCompareFunc)_film_filename_cmp);

  /* let's start import of images */
  gchar message[512] = { 0 };
  double fraction = 0;
  guint total = g_list_length(images);
  g_snprintf(message, sizeof(message) - 1, ngettext("importing %d image", "importing %d images", total),
             total);
  dt_progress_t *progress = dt_control_progress_create(darktable.control, TRUE, message);


  /* loop thru the images and import to current film roll */
  dt_film_t *cfr = film;
  GList *image = g_list_first(images);
  do
  {
    gchar *cdn = g_path_get_dirname((const gchar *)image->data);

    /* check if we need to initialize a new filmroll */
    if(!cfr || g_strcmp0(cfr->dirname, cdn) != 0)
    {
      // FIXME: maybe refactor into function and call it?
      if(cfr && cfr->dir)
      {
        /* check if we can find a gpx data file to be auto applied
           to images in the jsut imported filmroll */
        g_dir_rewind(cfr->dir);
        const gchar *dfn = NULL;
        while((dfn = g_dir_read_name(cfr->dir)) != NULL)
        {
          /* check if we have a gpx to be auto applied to filmroll */
          size_t len = strlen(dfn);
          if(strcmp(dfn + len - 4, ".gpx") == 0 || strcmp(dfn + len - 4, ".GPX") == 0)
          {
            gchar *gpx_file = g_build_path(G_DIR_SEPARATOR_S, cfr->dirname, dfn, NULL);
            gchar *tz = dt_conf_get_string("plugins/lighttable/geotagging/tz");
            dt_control_gpx_apply(gpx_file, cfr->id, tz);
            g_free(gpx_file);
            g_free(tz);
          }
        }
      }

      /* cleanup previously imported filmroll*/
      if(cfr && cfr != film)
      {
        if(dt_film_is_empty(cfr->id))
        {
          dt_film_remove(cfr->id);
        }
        dt_film_cleanup(cfr);
        g_free(cfr);
        cfr = NULL;
      }

      /* initialize and create a new film to import to */
      cfr = g_malloc(sizeof(dt_film_t));
      dt_film_init(cfr);
      dt_film_new(cfr, cdn);
    }

    g_free(cdn);

    /* import image */
    dt_image_import(cfr->id, (const gchar *)image->data, FALSE);

    fraction += 1.0 / total;
    dt_control_progress_set_progress(darktable.control, progress, fraction);


  } while((image = g_list_next(image)) != NULL);

  // only redraw at the end, to not spam the cpu with exposure events
  dt_control_queue_redraw_center();
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);

  dt_control_progress_destroy(darktable.control, progress);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_IMPORTED, film->id);

  // FIXME: maybe refactor into function and call it?
  if(cfr && cfr->dir)
  {
    /* check if we can find a gpx data file to be auto applied
       to images in the just imported filmroll */
    g_dir_rewind(cfr->dir);
    const gchar *dfn = NULL;
    while((dfn = g_dir_read_name(cfr->dir)) != NULL)
    {
      /* check if we have a gpx to be auto applied to filmroll */
      size_t len = strlen(dfn);
      if(strcmp(dfn + len - 4, ".gpx") == 0 || strcmp(dfn + len - 4, ".GPX") == 0)
      {
        gchar *gpx_file = g_build_path(G_DIR_SEPARATOR_S, cfr->dirname, dfn, NULL);
        gchar *tz = dt_conf_get_string("plugins/lighttable/geotagging/tz");
        dt_control_gpx_apply(gpx_file, cfr->id, tz);
        g_free(gpx_file);
        g_free(tz);
      }
    }
  }
}



void dt_film_remove_empty()
{
  // remove all empty film rolls from db:
  gboolean raise_signal = FALSE;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id,folder FROM film_rolls AS B WHERE "
                                                             "(SELECT COUNT(A.id) FROM images AS A WHERE "
                                                             "A.film_id=B.id)=0",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    sqlite3_stmt *inner_stmt;
    raise_signal = TRUE;
    const gint id = sqlite3_column_int(stmt, 0);
    const gchar *folder = (const gchar *)sqlite3_column_text(stmt, 1);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM film_rolls WHERE id=?1", -1,
                                &inner_stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(inner_stmt, 1, id);
    sqlite3_step(inner_stmt);
    sqlite3_finalize(inner_stmt);

    if(dt_util_is_dir_empty(folder)) rmdir(folder);
  }
  sqlite3_finalize(stmt);
  if(raise_signal) dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_REMOVED);
}

int dt_film_is_empty(const int id)
{
  int empty = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where film_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  if(sqlite3_step(stmt) != SQLITE_ROW) empty = 1;
  sqlite3_finalize(stmt);
  return empty;
}

// This is basically the same as dt_image_remove() from common/image.c.
// It just does the iteration over all images in the SQL statement
void dt_film_remove(const int id)
{
  // only allowed if local copies have their original accessible

  sqlite3_stmt *stmt;

  gboolean remove_ok = TRUE;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM images WHERE film_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    if(!dt_image_safe_remove(imgid))
    {
      remove_ok = FALSE;
      break;
    }
  }
  sqlite3_finalize(stmt);

  if(!remove_ok)
  {
    dt_control_log(_("cannot remove film roll having local copies with non accessible originals"));
    return;
  }

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from tagged_images where imgid in "
                                                             "(select id from images where film_id = ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from history where imgid in "
                                                             "(select id from images where film_id = ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from color_labels where imgid in "
                                                             "(select id from images where film_id = ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from meta_data where id in "
                                                             "(select id from images where film_id = ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from selected_images where imgid in "
                                                             "(select id from images where film_id = ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where film_id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const uint32_t imgid = sqlite3_column_int(stmt, 0);
    dt_image_local_copy_reset(imgid);
    dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
    dt_image_cache_remove(darktable.image_cache, imgid);
  }
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from images where id in "
                                                             "(select id from images where film_id = ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from film_rolls where id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // dt_control_update_recent_films();
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
