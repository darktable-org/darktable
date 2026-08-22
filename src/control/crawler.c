/*
    This file is part of darktable,
    Copyright (C) 2014-2025 darktable developers.

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

#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#include "common/collection.h"
#include "common/darktable.h"
#include "common/database.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/image.h"
#include "control/conf.h"
#include "control/control.h"
#include "crawler.h"
#include "gui/gtk.h"
#include "gui/splash.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

// how many seconds may the sidecar file's timestamp differ from that recorded in the database?
#define MAX_TIME_SKEW 2

typedef enum dt_control_crawler_cols_t
{
  DT_CONTROL_CRAWLER_COL_ID = 0,
  DT_CONTROL_CRAWLER_COL_IMAGE_PATH,
  DT_CONTROL_CRAWLER_COL_XMP_PATH,
  DT_CONTROL_CRAWLER_COL_TS_XMP,
  DT_CONTROL_CRAWLER_COL_TS_DB,
  DT_CONTROL_CRAWLER_COL_TS_XMP_INT, // new timestamp to db
  DT_CONTROL_CRAWLER_COL_TS_DB_INT,
  DT_CONTROL_CRAWLER_COL_REPORT,
  DT_CONTROL_CRAWLER_COL_TIME_DELTA,
  DT_CONTROL_CRAWLER_NUM_COLS
} dt_control_crawler_cols_t;

typedef struct dt_control_crawler_result_t
{
  dt_imgid_t id;
  time_t timestamp_xmp;
  time_t timestamp_db;
  char *image_path, *xmp_path;
} dt_control_crawler_result_t;

static void _free_crawler_result(dt_control_crawler_result_t *entry)
{
  g_free(entry->image_path);
  g_free(entry->xmp_path);
  entry->image_path = entry->xmp_path = NULL;
}

static void _set_modification_time(char *filename,
                                   const time_t timestamp)
{
  GFile *gfile = g_file_new_for_path(filename);

  GFileInfo *info = g_file_query_info(
    gfile,
    G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
    G_FILE_QUERY_INFO_NONE,
    NULL,
    NULL);

  // For reference, we could use the following lines but for some
  // reasons there is a deprecated message raised even though this
  // routine is not marked as deprecated in the documentation.
  //
  // GDateTime *datetime = g_date_time_new_from_unix_local(timestamp);
  // g_file_info_set_modification_date_time(info, datetime);

  if(info)
  {
    g_file_info_set_attribute_uint64
      (info,
       G_FILE_ATTRIBUTE_TIME_MODIFIED,
       timestamp);

    g_file_set_attributes_from_info(
      gfile,
      info,
      G_FILE_QUERY_INFO_NONE,
      NULL,
      NULL);
  }

  g_object_unref(gfile);
  if(info) g_clear_object(&info);
}

// progress update intervals in seconds
#define FAST_UPDATE 0.2
#define SLOW_UPDATE 1.0

// number of concurrent workers used to examine the filesystem.  the
// crawl is dominated by filesystem latency rather than cpu work --
// overwhelmingly so for libraries stored on a network share -- so we
// deliberately oversubscribe instead of scaling with the core count.
#define DEFAULT_CRAWLER_THREADS 16
#define MAX_CRAWLER_THREADS 64

// reported back to the caller while a scan is in progress.  the splash
// screen uses this during startup, the background job uses it to drive
// a progress bar in the gui.
typedef void (*dt_crawler_progress_cb)(const double fraction,
                                       const double elapsed,
                                       void *data);

// one image to be examined.  everything up to and including `flags' is
// filled in by the collecting pass, the remaining fields are written by
// the worker threads -- each worker touches only its own item, so no
// locking is required.
typedef struct dt_crawler_item_t
{
  dt_imgid_t id;
  time_t timestamp_db;
  int version;
  int flags;
  const char *folder;           // shared, owned by the directory list
  char *filename;               // owned, basename within `folder'

  int new_flags;
  char *xmp_path;               // only set when xmp_newer is TRUE
  time_t timestamp_xmp;
  gboolean missing;
  gboolean xmp_newer;
} dt_crawler_item_t;

// one directory holding a contiguous run of the items above.  a film
// roll is exactly one directory -- image filenames never contain a path
// separator -- so this is also the unit the background crawl works in.
typedef struct dt_crawler_dir_t
{
  char *folder;                 // owned
  int first;                    // index of its first item
  int count;
  GHashTable *entries;          // set of the names the directory holds
} dt_crawler_dir_t;

typedef struct dt_crawler_scan_t
{
  dt_crawler_item_t *items;
  int num_items;
  dt_crawler_dir_t *dirs;
  int num_dirs;
  gboolean look_for_xmp;
  gint next;                    // atomic: next index to hand out
  gint completed;               // atomic: items finished, drives progress
  const gint *abort;            // optional, checked atomically by the workers

  // the directory currently being examined, set by the driver before it
  // releases the workers on to that directory's range of items
  const dt_crawler_dir_t *dir;
} dt_crawler_scan_t;

static inline gboolean _crawler_aborted(const dt_crawler_scan_t *scan)
{
  return scan->abort && g_atomic_int_get(scan->abort);
}

/* Read the names a directory holds.
 *
 * Listing the directory once replaces the six filesystem probes per
 * image that this used to do -- an existence check, a stat() of the xmp
 * and four probes for .txt/.wav sidecars -- with a single pass plus one
 * stat() per xmp actually present.  On a library of ~85k images that is
 * 506k filesystem calls reduced to 85k, which matters most when the
 * images live on a network share.
 *
 * Returns NULL if the directory cannot be read, which is treated the
 * same way as it was before: every image in it counts as missing.
 */
static GHashTable *_crawler_read_dir(const char *folder)
{
  GError *error = NULL;
  GDir *dir = g_dir_open(folder, 0, &error);
  if(!dir)
  {
    if(error) g_error_free(error);
    return NULL;
  }

  GHashTable *entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  const gchar *name;
  while((name = g_dir_read_name(dir)))
    g_hash_table_add(entries, g_strdup(name));

  g_dir_close(dir);
  return entries;
}

// examine a single image against the listing of the directory holding
// it.  this performs no database access and no gui access whatsoever so
// that it is safe to run from a worker thread.  the early returns mirror
// the `continue' statements of the original serial implementation -- in
// particular an image whose xmp is absent deliberately skips the
// .txt/.wav probe below.
static void _crawler_examine(dt_crawler_item_t *item,
                             GHashTable *entries,
                             const gboolean look_for_xmp)
{
  // if the image is missing we ignore it.
  if(!entries || !g_hash_table_contains(entries, item->filename))
  {
    item->missing = TRUE;
    return;
  }

  // no need to look for xmp files if none get written anyway.
  if(look_for_xmp)
  {
    // construct the xmp filename for this image
    gchar xmp_name[PATH_MAX] = { 0 };
    g_strlcpy(xmp_name, item->filename, sizeof(xmp_name));
    dt_image_path_append_version_no_db(item->version, xmp_name, sizeof(xmp_name));
    size_t len = strlen(xmp_name);
    if(len + 4 >= PATH_MAX) return;
    xmp_name[len++] = '.';
    xmp_name[len++] = 'x';
    xmp_name[len++] = 'm';
    xmp_name[len++] = 'p';
    xmp_name[len] = '\0';

    // the listing tells us whether it is there, so the only image we
    // still have to touch is the one that is
    if(!g_hash_table_contains(entries, xmp_name)) return;

    gchar *xmp_path = g_build_filename(item->folder, xmp_name, NULL);

    // on Windows the encoding might not be UTF8
    gchar *xmp_path_locale = dt_util_normalize_path(xmp_path);
    int stat_res = -1;
#ifdef _WIN32
    // UTF8 paths fail in this context, but converting to UTF16 works
    struct _stati64 statbuf;
    if(xmp_path_locale) // in Windows dt_util_normalize_path returns
                        // NULL if file does not exist
    {
      wchar_t *wfilename = g_utf8_to_utf16(xmp_path_locale, -1, NULL, NULL, NULL);
      stat_res = _wstati64(wfilename, &statbuf);
      g_free(wfilename);
    }
#else
    struct stat statbuf;
    stat_res = stat(xmp_path_locale, &statbuf);
#endif
    g_free(xmp_path_locale);
    if(stat_res)
    {
      g_free(xmp_path);
      return; // TODO: shall we report these?
    }

    // step 1: check if the xmp is newer than our db entry
    if(item->timestamp_db + MAX_TIME_SKEW < statbuf.st_mtime)
    {
      item->xmp_newer = TRUE;
      item->timestamp_xmp = statbuf.st_mtime;
      item->xmp_path = xmp_path;
    }
    else
      g_free(xmp_path);
    // older timestamps are the case for all images after the db
    // upgrade. better not report these
  }

  // step 2: check if the image has associated files (.txt, .wav).  these
  // are now answered out of the directory listing rather than by probing
  // the filesystem four more times.
  size_t len = strlen(item->filename);
  const char *c = item->filename + len;
  while((c > item->filename) && (*c != '.')) c--;
  len = c - item->filename + 1;

  char *extra_name = calloc(len + 3 + 1, sizeof(char));
  if(extra_name)
  {
    g_strlcpy(extra_name, item->filename, len + 1);

    extra_name[len] = 't';
    extra_name[len + 1] = 'x';
    extra_name[len + 2] = 't';
    gboolean has_txt = g_hash_table_contains(entries, extra_name);

    if(!has_txt)
    {
      extra_name[len] = 'T';
      extra_name[len + 1] = 'X';
      extra_name[len + 2] = 'T';
      has_txt = g_hash_table_contains(entries, extra_name);
    }

    extra_name[len] = 'w';
    extra_name[len + 1] = 'a';
    extra_name[len + 2] = 'v';
    gboolean has_wav = g_hash_table_contains(entries, extra_name);

    if(!has_wav)
    {
      extra_name[len] = 'W';
      extra_name[len + 1] = 'A';
      extra_name[len + 2] = 'V';
      has_wav = g_hash_table_contains(entries, extra_name);
    }

    // TODO: decide if we want to remove the flag for images that lost
    // their extra file. currently we do (the else cases)
    int new_flags = item->flags;
    if(has_txt)
      new_flags |= DT_IMAGE_HAS_TXT;
    else
      new_flags &= ~DT_IMAGE_HAS_TXT;
    if(has_wav)
      new_flags |= DT_IMAGE_HAS_WAV;
    else
      new_flags &= ~DT_IMAGE_HAS_WAV;
    item->new_flags = new_flags;

    free(extra_name);
  }
}

static gpointer _crawler_scan_thread(gpointer arg)
{
  dt_crawler_scan_t *scan = (dt_crawler_scan_t *)arg;
  const dt_crawler_dir_t *dir = scan->dir;
  const int last = dir->first + dir->count;

  while(!_crawler_aborted(scan))
  {
    const int idx = g_atomic_int_add(&scan->next, 1);
    if(idx >= last) break;
    _crawler_examine(&scan->items[idx], dir->entries, scan->look_for_xmp);
    g_atomic_int_inc(&scan->completed);
  }
  return NULL;
}

static int _crawler_num_threads(void)
{
  int num_threads = dt_conf_get_int("crawler_threads");
  if(num_threads < 1) num_threads = DEFAULT_CRAWLER_THREADS;
  return MIN(num_threads, MAX_CRAWLER_THREADS);
}

// examine the items of one directory, spreading them over the worker
// pool.  the directory listing itself is read by the calling thread.
static void _crawler_scan_dir(dt_crawler_scan_t *scan,
                              dt_crawler_dir_t *dir)
{
  dir->entries = _crawler_read_dir(dir->folder);

  scan->dir = dir;
  g_atomic_int_set(&scan->next, dir->first);

  const int num_threads = MIN(_crawler_num_threads(), MAX(dir->count, 1));

  if(num_threads <= 1)
  {
    // serial path, kept so that crawler_threads=1 reproduces the
    // original behaviour exactly for comparison purposes
    for(int i = dir->first; i < dir->first + dir->count && !_crawler_aborted(scan); i++)
    {
      _crawler_examine(&scan->items[i], dir->entries, scan->look_for_xmp);
      g_atomic_int_inc(&scan->completed);
    }
  }
  else
  {
    GThread **threads = calloc(num_threads, sizeof(GThread *));
    if(threads)
    {
      for(int t = 0; t < num_threads; t++)
        threads[t] = g_thread_new("crawler", _crawler_scan_thread, scan);
      for(int t = 0; t < num_threads; t++)
        if(threads[t]) g_thread_join(threads[t]);
      free(threads);
    }
    else
    {
      // out of memory: fall back to examining everything inline
      for(int i = dir->first; i < dir->first + dir->count; i++)
        _crawler_examine(&scan->items[i], dir->entries, scan->look_for_xmp);
      g_atomic_int_set(&scan->completed, dir->first + dir->count);
    }
  }

  if(dir->entries)
  {
    // the listing is only needed while its own directory is examined
    g_hash_table_destroy(dir->entries);
    dir->entries = NULL;
  }
}

// run the filesystem examination over all collected items, one directory
// at a time.  the calling thread stays responsible for progress
// reporting so that no gui call is ever made from a worker.
static void _crawler_scan_items(dt_crawler_scan_t *scan,
                                const dt_crawler_progress_cb progress,
                                void *progress_data)
{
  if(scan->num_items <= 0) return;

  const double start_time = dt_get_wtime();
  // set the "previous update" time to 10ms after a notional previous
  // update to ensure visibility of the first update (which might not
  // appear when done with zero delay) while minimizing the delay
  double last_time = start_time - (FAST_UPDATE - 0.01);

  for(int d = 0; d < scan->num_dirs && !_crawler_aborted(scan); d++)
  {
    _crawler_scan_dir(scan, &scan->dirs[d]);

    const double curr_time = dt_get_wtime();
    if(progress
       && curr_time >= last_time + ((curr_time - start_time > 4.0)
                                    ? SLOW_UPDATE : FAST_UPDATE))
    {
      progress(g_atomic_int_get(&scan->completed) / (double)scan->num_items,
               curr_time - start_time, progress_data);
      last_time = curr_time;
    }
  }
}

// collect the images to be examined.  when filmid is a valid film roll
// only that roll is collected, otherwise the whole library is.
static dt_crawler_item_t *_crawler_collect_items(const dt_filmid_t filmid,
                                                 int *num_items,
                                                 dt_crawler_dir_t **dirs_out,
                                                 int *num_dirs_out)
{
  sqlite3_stmt *stmt;
  int capacity = 1024;

  // reserve based on the number of images we expect to see
  // clang-format off
  if(dt_is_valid_filmid(filmid))
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT COUNT(*) FROM main.images WHERE film_id = ?1",
                                -1, &stmt, 0);
  else
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT COUNT(*) FROM main.images", -1, &stmt, 0);
  // clang-format on
  if(dt_is_valid_filmid(filmid)) DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    capacity = MAX(1, sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);

  dt_crawler_item_t *items = calloc(capacity, sizeof(dt_crawler_item_t));
  if(!items)
  {
    *num_items = 0;
    *num_dirs_out = 0;
    *dirs_out = NULL;
    return NULL;
  }

  // ordering by film roll keeps the images of one directory in a
  // contiguous run, which is what lets them be examined a directory at a
  // time against a single listing of it
  // clang-format off
  if(dt_is_valid_filmid(filmid))
    sqlite3_prepare_v2(dt_database_get(darktable.db),
                       "SELECT i.id, write_timestamp, version,"
                       "       folder, filename, flags"
                       " FROM main.images i, main.film_rolls f"
                       " ON i.film_id = f.id"
                       " WHERE f.id = ?1"
                       " ORDER BY f.id, filename",
                       -1, &stmt, NULL);
  else
    sqlite3_prepare_v2(dt_database_get(darktable.db),
                       "SELECT i.id, write_timestamp, version,"
                       "       folder, filename, flags"
                       " FROM main.images i, main.film_rolls f"
                       " ON i.film_id = f.id"
                       " ORDER BY f.id, filename",
                       -1, &stmt, NULL);
  // clang-format on
  if(dt_is_valid_filmid(filmid)) DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);

  int count = 0;
  int num_dirs = 0, dirs_capacity = 0;
  dt_crawler_dir_t *dirs = NULL;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(count >= capacity)
    {
      // the database changed underneath us, grow to fit
      const int new_capacity = capacity * 2;
      dt_crawler_item_t *grown =
        realloc(items, new_capacity * sizeof(dt_crawler_item_t));
      if(!grown) break;
      memset(grown + capacity, 0,
             (new_capacity - capacity) * sizeof(dt_crawler_item_t));
      items = grown;
      capacity = new_capacity;
    }

    const char *folder = (const char *)sqlite3_column_text(stmt, 3);
    const char *filename = (const char *)sqlite3_column_text(stmt, 4);
    if(!folder || !filename) continue;

    // start a new directory whenever the folder changes
    if(num_dirs == 0 || strcmp(dirs[num_dirs - 1].folder, folder))
    {
      if(num_dirs >= dirs_capacity)
      {
        const int new_capacity = MAX(16, dirs_capacity * 2);
        dt_crawler_dir_t *grown =
          realloc(dirs, new_capacity * sizeof(dt_crawler_dir_t));
        if(!grown) break;
        memset(grown + dirs_capacity, 0,
               (new_capacity - dirs_capacity) * sizeof(dt_crawler_dir_t));
        dirs = grown;
        dirs_capacity = new_capacity;
      }
      dirs[num_dirs].folder = g_strdup(folder);
      dirs[num_dirs].first = count;
      dirs[num_dirs].count = 0;
      dirs[num_dirs].entries = NULL;
      num_dirs++;
    }

    dt_crawler_item_t *item = &items[count];
    item->id = sqlite3_column_int(stmt, 0);
    item->timestamp_db = sqlite3_column_int64(stmt, 1);
    item->version = sqlite3_column_int(stmt, 2);
    item->folder = dirs[num_dirs - 1].folder;
    item->filename = g_strdup(filename);
    item->flags = sqlite3_column_int(stmt, 5);
    item->new_flags = item->flags;
    dirs[num_dirs - 1].count++;
    count++;
  }
  sqlite3_finalize(stmt);

  *num_items = count;
  *num_dirs_out = num_dirs;
  *dirs_out = dirs;
  return items;
}

static void _crawler_free_items(dt_crawler_item_t *items,
                                const int num_items,
                                dt_crawler_dir_t *dirs,
                                const int num_dirs)
{
  for(int i = 0; i < num_items; i++)
  {
    g_free(items[i].filename);
    g_free(items[i].xmp_path);
  }
  free(items);

  for(int d = 0; d < num_dirs; d++)
  {
    g_free(dirs[d].folder);
    if(dirs[d].entries) g_hash_table_destroy(dirs[d].entries);
  }
  free(dirs);
}

// apply the results gathered by the workers: write back changed flags
// and build the list of images whose xmp is newer than the database.
// this runs single threaded on the caller's thread and walks the items
// in collection order, so the resulting list is identical to the one
// produced by the original serial implementation.
static GList *_crawler_apply_results(dt_crawler_item_t *items,
                                     const int num_items,
                                     int *flags_changed)
{
  GList *result = NULL;
  sqlite3_stmt *inner_stmt;
  int changed = 0;

  sqlite3_prepare_v2(dt_database_get(darktable.db),
                     "UPDATE main.images SET flags = ?1 WHERE id = ?2", -1,
                     &inner_stmt, NULL);

  // let's wrap this into a transaction, it might make it a little faster.
  dt_database_start_transaction(darktable.db);

  for(int i = 0; i < num_items; i++)
  {
    dt_crawler_item_t *item = &items[i];

    if(item->missing)
    {
      gchar *image_path = g_build_filename(item->folder, item->filename, NULL);
      dt_print(DT_DEBUG_CONTROL, "[crawler] `%s' (id: %d) is missing",
               image_path, item->id);
      g_free(image_path);
      continue;
    }

    if(item->xmp_newer)
    {
      dt_control_crawler_result_t *entry = malloc(sizeof(dt_control_crawler_result_t));
      if(entry)
      {
        entry->id = item->id;
        entry->timestamp_xmp = item->timestamp_xmp;
        entry->timestamp_db = item->timestamp_db;
        entry->image_path = g_build_filename(item->folder, item->filename, NULL);
        entry->xmp_path = g_strdup(item->xmp_path);

        result = g_list_prepend(result, entry);
        dt_print(DT_DEBUG_CONTROL,
                 "[crawler] `%s' (id: %d) is a newer XMP file",
                 item->xmp_path, item->id);
      }
    }

    if(item->new_flags != item->flags)
    {
      sqlite3_bind_int(inner_stmt, 1, item->new_flags);
      sqlite3_bind_int(inner_stmt, 2, item->id);
      sqlite3_step(inner_stmt);
      sqlite3_reset(inner_stmt);
      sqlite3_clear_bindings(inner_stmt);
      changed++;
    }
  }

  dt_database_release_transaction(darktable.db);
  sqlite3_finalize(inner_stmt);

  if(flags_changed) *flags_changed = changed;

  return g_list_reverse(result); // list was built in reverse order, so un-reverse it
}

static void _splash_progress(const double fraction,
                             const double elapsed,
                             void *data)
{
  dt_splash_screen_set_progress_percent(_("checking for updated sidecar files (%d%%)"),
                                        fraction, elapsed);
}

// examine one film roll (or the whole library when filmid is NO_FILMID)
// and return the list of images with a newer xmp file on disk.
static GList *_crawler_run_filmroll(const dt_filmid_t filmid,
                                    const dt_crawler_progress_cb progress,
                                    void *progress_data,
                                    const gint *abort,
                                    int *flags_changed)
{
  dt_crawler_scan_t scan = { 0 };
  scan.look_for_xmp = dt_image_get_xmp_mode() != DT_WRITE_XMP_NEVER;
  scan.abort = abort;
  scan.items = _crawler_collect_items(filmid, &scan.num_items,
                                      &scan.dirs, &scan.num_dirs);
  if(!scan.items) return NULL;

  const double start_time = dt_get_wtime();
  _crawler_scan_items(&scan, progress, progress_data);
  const double scan_time = dt_get_wtime() - start_time;

  GList *result = _crawler_apply_results(scan.items, scan.num_items, flags_changed);
  _crawler_free_items(scan.items, scan.num_items, scan.dirs, scan.num_dirs);

  if(dt_is_valid_filmid(filmid))
    dt_print(DT_DEBUG_CONTROL,
             "[crawler] film roll %d: examined %d images in %.2fs,"
             " %d updated XMP files found",
             filmid, scan.num_items, scan_time, g_list_length(result));
  else
    dt_print(DT_DEBUG_CONTROL,
             "[crawler] examined %d images in %d directories in %.2fs"
             " using %d threads, %d updated XMP files found",
             scan.num_items, scan.num_dirs, scan_time,
             MIN(_crawler_num_threads(), MAX(scan.num_items, 1)),
             g_list_length(result));

  return result;
}

GList *dt_control_crawler_run(void)
{
  return _crawler_run_filmroll(NO_FILMID, _splash_progress, NULL, NULL, NULL);
}

/******************** background crawling ********************/

static void _crawler_show_image_list(GList *images, const gboolean modal);
static void _crawler_ensure_current_collection(void);
static void _crawler_collection_changed(gpointer instance,
                                        dt_collection_change_t query_change,
                                        dt_collection_properties_t changed_property,
                                        gpointer imgs,
                                        const int next,
                                        gpointer user_data);

/* The crawl is split up per film roll so that its order can be changed
 * while it is running: opening a film roll that has not been examined
 * yet moves it to the front of the queue rather than making the user
 * wait for the rolls queued ahead of it.
 *
 * The queue is deliberately not persisted across restarts.  An XMP file
 * can be modified while darktable is not running, so every session has
 * to examine every image eventually -- the point of this queue is to
 * take that work off the critical path, not to skip it.
 */
typedef struct dt_crawler_bg_t
{
  GList *pending;       // film roll ids, head is examined next
  GList *conflicts;     // dt_control_crawler_result_t *, accumulated
  int num_rolls;
  int num_done;
  gint abort;           // read atomically by the scan workers
  gboolean running;

  /* Only one film roll may be examined at a time, by either the
   * background job or a caller waiting for a specific roll.  Besides
   * keeping the two from examining the same roll twice, this keeps all
   * of the crawler's database work on one thread at a time so that its
   * transactions are never interleaved with each other.
   */
  gboolean scanning;    // someone is inside a scan right now
  dt_filmid_t current;  // which film roll that is, NO_FILMID if none
} dt_crawler_bg_t;

// static storage, so these are zero-initialised as glib requires
static GMutex _crawler_bg_lock;
static GCond _crawler_bg_cond;
static dt_crawler_bg_t _crawler_bg = { 0 };

static void _crawler_free_result_full(gpointer data)
{
  dt_control_crawler_result_t *item = (dt_control_crawler_result_t *)data;
  _free_crawler_result(item);
  free(item);
}

// runs on the gui thread once the crawl has stopped, for whatever reason
static gboolean _crawler_bg_finished(gpointer data)
{
  GList *conflicts = (GList *)data;

  // nothing left to reprioritize
  DT_CONTROL_SIGNAL_DISCONNECT(_crawler_collection_changed, NULL);

  if(conflicts)
  {
    const guint count = g_list_length(conflicts);
    dt_control_log(ngettext("%u updated XMP sidecar file found",
                            "%u updated XMP sidecar files found", count), count);

    // shown non-modally: the crawl finishes long after startup, so this
    // must not interrupt whatever the user is currently doing
    _crawler_show_image_list(conflicts, FALSE);
  }
  return G_SOURCE_REMOVE;
}

/* Examine one film roll and fold the findings into the accumulated list.
 * The caller must already have claimed the scan (scanning == TRUE and
 * current == filmid) so that nothing else touches the database at the
 * same time.  Returns the conflicts found for this film roll alone,
 * still owned by the accumulated list unless `take' is TRUE.
 */
static GList *_crawler_scan_claimed_roll(const dt_filmid_t filmid,
                                         const gboolean take)
{
  int flags_changed = 0;
  GList *found = _crawler_run_filmroll(filmid, NULL, NULL,
                                       &_crawler_bg.abort, &flags_changed);

  g_mutex_lock(&_crawler_bg_lock);
  if(!take)
    _crawler_bg.conflicts = g_list_concat(_crawler_bg.conflicts, found);
  _crawler_bg.num_done++;
  _crawler_bg.scanning = FALSE;
  _crawler_bg.current = NO_FILMID;
  g_cond_broadcast(&_crawler_bg_cond);
  g_mutex_unlock(&_crawler_bg_lock);

  // the .txt/.wav flags are drawn as thumbnail overlays, so a change
  // needs to reach the screen.  this practically never fires.
  if(flags_changed) dt_control_queue_redraw_center();

  return take ? found : NULL;
}

static int32_t _crawler_bg_job_run(dt_job_t *job)
{
  dt_control_job_set_progress_message(job, "%s",
                                      _("checking for updated sidecar files"));

  while(TRUE)
  {
    g_mutex_lock(&_crawler_bg_lock);
    // let anyone waiting for a specific film roll go first
    while(_crawler_bg.scanning && !g_atomic_int_get(&_crawler_bg.abort))
      g_cond_wait(&_crawler_bg_cond, &_crawler_bg_lock);

    if(g_atomic_int_get(&_crawler_bg.abort) || !_crawler_bg.pending)
    {
      g_mutex_unlock(&_crawler_bg_lock);
      break;
    }
    const dt_filmid_t filmid = GPOINTER_TO_INT(_crawler_bg.pending->data);
    _crawler_bg.pending = g_list_delete_link(_crawler_bg.pending,
                                             _crawler_bg.pending);
    _crawler_bg.scanning = TRUE;
    _crawler_bg.current = filmid;
    const double fraction =
      (_crawler_bg.num_done + 1) / (double)MAX(_crawler_bg.num_rolls, 1);
    g_mutex_unlock(&_crawler_bg_lock);

    _crawler_scan_claimed_roll(filmid, FALSE);

    dt_control_job_set_progress(job, fraction);
  }

  g_mutex_lock(&_crawler_bg_lock);
  GList *conflicts = _crawler_bg.conflicts;
  _crawler_bg.conflicts = NULL;
  const gboolean aborted = g_atomic_int_get(&_crawler_bg.abort) != 0;
  const int num_done = _crawler_bg.num_done;
  const int num_rolls = _crawler_bg.num_rolls;
  _crawler_bg.running = FALSE;
  g_mutex_unlock(&_crawler_bg_lock);

  dt_print(DT_DEBUG_CONTROL,
           "[crawler] background crawl %s after %d of %d film rolls",
           aborted ? "cancelled" : "finished", num_done, num_rolls);

  if(aborted)
  {
    // darktable is shutting down: drop the findings rather than trying
    // to put a dialog on screen on the way out
    g_list_free_full(conflicts, _crawler_free_result_full);
    conflicts = NULL;
  }
  g_main_context_invoke(NULL, _crawler_bg_finished, conflicts);

  return 0;
}

static dt_job_t *_crawler_bg_job_create(void)
{
  dt_job_t *job = dt_control_job_create(&_crawler_bg_job_run, "crawl for updated sidecar files");
  if(!job) return NULL;
  dt_control_job_set_params(job, NULL, NULL);
  return job;
}

void dt_control_crawler_start_background(void)
{
  if(!dt_conf_get_bool("run_crawler_on_start") || dt_gimpmode()) return;

  g_mutex_lock(&_crawler_bg_lock);
  if(_crawler_bg.running)
  {
    g_mutex_unlock(&_crawler_bg_lock);
    return;
  }

  // most recently opened film rolls first -- those are the ones the user
  // is most likely to look at in this session.  rolls that have never
  // been opened have a NULL access_timestamp and sort last.
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM main.film_rolls"
                              " ORDER BY access_timestamp DESC, id DESC",
                              -1, &stmt, NULL);
  // clang-format on
  GList *rolls = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
    rolls = g_list_prepend(rolls, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);

  _crawler_bg.pending = g_list_reverse(rolls);
  _crawler_bg.num_rolls = g_list_length(_crawler_bg.pending);
  _crawler_bg.num_done = 0;
  _crawler_bg.conflicts = NULL;
  g_atomic_int_set(&_crawler_bg.abort, 0);
  _crawler_bg.running = _crawler_bg.num_rolls > 0;
  const gboolean start = _crawler_bg.running;
  const int num_rolls = _crawler_bg.num_rolls;
  g_mutex_unlock(&_crawler_bg_lock);

  if(!start) return;

  dt_print(DT_DEBUG_CONTROL,
           "[crawler] queued %d film rolls for background crawling", num_rolls);

  // follow the user around the library while the crawl runs
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_COLLECTION_CHANGED,
                            _crawler_collection_changed, NULL);

  // the collection restored at startup was set up before the signal above
  // was connected, and it is the one the user is looking at right now
  _crawler_ensure_current_collection();

  // note: dt_control_add_job() returns TRUE to report a *failure*
  if(dt_control_add_job(DT_JOB_QUEUE_SYSTEM_BG, _crawler_bg_job_create()))
  {
    // nothing will run, so do not leave the queue looking busy -- that
    // would make dt_control_crawler_stop() wait for a job that never was
    dt_print(DT_DEBUG_CONTROL, "[crawler] could not queue the background job");
    DT_CONTROL_SIGNAL_DISCONNECT(_crawler_collection_changed, NULL);
    g_mutex_lock(&_crawler_bg_lock);
    g_list_free(_crawler_bg.pending);
    _crawler_bg.pending = NULL;
    _crawler_bg.running = FALSE;
    g_mutex_unlock(&_crawler_bg_lock);
  }
}

/* Make sure a film roll has been examined before its images are shown.
 *
 * The images of a film roll must not be edited before its sidecar files
 * have been checked: darktable does not re-read an xmp when an image is
 * opened, and dt_image_write_sidecar_file() overwrites whatever is on
 * disk, so editing an image whose xmp was updated elsewhere would
 * silently discard those changes.  The blocking startup crawl this
 * replaces guaranteed that could not happen, and so must this.
 *
 * Examining a single film roll is cheap -- a few tens of milliseconds
 * for a typical roll -- so only the roll being opened is waited for,
 * rather than the whole library.
 */
static GList *_crawler_ensure_roll(const dt_filmid_t filmid)
{
  if(!dt_is_valid_filmid(filmid)) return NULL;

  g_mutex_lock(&_crawler_bg_lock);
  if(!_crawler_bg.running)
  {
    g_mutex_unlock(&_crawler_bg_lock);
    return NULL;
  }

  // if the background job is examining this very film roll, wait for it
  while(_crawler_bg.current == filmid
        && _crawler_bg.running
        && !g_atomic_int_get(&_crawler_bg.abort))
    g_cond_wait(&_crawler_bg_cond, &_crawler_bg_lock);

  // claim it, waiting for any other roll being examined to finish first
  while(_crawler_bg.scanning && !g_atomic_int_get(&_crawler_bg.abort))
    g_cond_wait(&_crawler_bg_cond, &_crawler_bg_lock);

  GList *link = g_list_find(_crawler_bg.pending, GINT_TO_POINTER(filmid));
  if(!link || g_atomic_int_get(&_crawler_bg.abort))
  {
    // already examined in this session, or we are shutting down
    g_mutex_unlock(&_crawler_bg_lock);
    return NULL;
  }
  _crawler_bg.pending = g_list_delete_link(_crawler_bg.pending, link);
  _crawler_bg.scanning = TRUE;
  _crawler_bg.current = filmid;
  g_mutex_unlock(&_crawler_bg_lock);

  const double start_time = dt_get_wtime();
  GList *found = _crawler_scan_claimed_roll(filmid, TRUE);
  dt_print(DT_DEBUG_CONTROL,
           "[crawler] film roll %d examined on demand in %.2fs, %d updated"
           " XMP files found",
           filmid, dt_get_wtime() - start_time, g_list_length(found));
  return found;
}

/* Report on demand findings straight away rather than leaving them for
 * the end of the crawl: the whole point of having waited is to tell the
 * user before they start editing these images.
 */
static void _crawler_report(GList *found)
{
  if(!found) return;

  const guint count = g_list_length(found);
  dt_control_log(ngettext("%u updated XMP sidecar file found",
                          "%u updated XMP sidecar files found", count), count);
  _crawler_show_image_list(found, FALSE);
}

void dt_control_crawler_ensure_filmroll(const dt_filmid_t filmid)
{
  _crawler_report(_crawler_ensure_roll(filmid));
}

/* Selecting a film roll in the collect module does not go through
 * dt_film_open(), it just changes the collection.  So follow the
 * collection as well, and for the same reason wait for it: the images it
 * contains are about to be shown and may be edited, so their sidecar
 * files have to have been checked first.
 *
 * Every film roll the collection covers is waited for, not a sample of
 * them, since the user can scroll to any of the images.  In the worst
 * case -- a collection spanning the whole library, opened immediately at
 * startup -- this costs the same as the blocking crawl it replaces, and
 * in every other case far less.  Once the background crawl has finished
 * this does nothing at all.
 */
static void _crawler_ensure_current_collection(void)
{
  g_mutex_lock(&_crawler_bg_lock);
  const gboolean running = _crawler_bg.running;
  g_mutex_unlock(&_crawler_bg_lock);
  if(!running) return;

  // the collection is materialised in memory.collected_images, so its
  // film rolls come out of a single query
  GList *rolls = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT i.film_id"
                              " FROM memory.collected_images AS c, main.images AS i"
                              " ON i.id = c.imgid"
                              " GROUP BY i.film_id"
                              " ORDER BY MIN(c.rowid)",
                              -1, &stmt, NULL);
  // clang-format on
  while(sqlite3_step(stmt) == SQLITE_ROW)
    rolls = g_list_prepend(rolls, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);
  rolls = g_list_reverse(rolls);

  GList *found = NULL;
  for(GList *iter = rolls; iter; iter = g_list_next(iter))
    found = g_list_concat(found,
                          _crawler_ensure_roll(GPOINTER_TO_INT(iter->data)));

  g_list_free(rolls);
  _crawler_report(found);
}

static void _crawler_collection_changed(gpointer instance,
                                        dt_collection_change_t query_change,
                                        dt_collection_properties_t changed_property,
                                        gpointer imgs,
                                        const int next,
                                        gpointer user_data)
{
  _crawler_ensure_current_collection();
}

void dt_control_crawler_stop(const gboolean wait)
{
  g_mutex_lock(&_crawler_bg_lock);
  const gboolean running = _crawler_bg.running;
  g_mutex_unlock(&_crawler_bg_lock);
  if(!running) return;

  g_atomic_int_set(&_crawler_bg.abort, 1);

  if(wait)
  {
    // the workers check the abort flag per image, so this returns
    // quickly unless a single stat() is stuck on an unresponsive mount
    for(int i = 0; i < 1000; i++)
    {
      g_mutex_lock(&_crawler_bg_lock);
      const gboolean still_running = _crawler_bg.running;
      g_mutex_unlock(&_crawler_bg_lock);
      if(!still_running) break;
      g_usleep(10000);
    }
  }
}


/********************* the gui stuff *********************/

typedef struct dt_control_crawler_gui_t
{
  GtkTreeView *tree;
  GtkTreeModel *model;
  GtkWidget *log;
  GtkWidget *spinner;
  GList *rows_to_remove;
} dt_control_crawler_gui_t;

// close the window and clean up
static void dt_control_crawler_response_callback(GtkWidget *dialog,
                                                 const gint response_id,
                                                 gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  g_object_unref(G_OBJECT(gui->model));
  gtk_widget_destroy(dialog);
  free(gui);
}


static void _delete_selected_rows(dt_control_crawler_gui_t *gui)
{
  GList *rr_list = gui->rows_to_remove;
  GtkTreeModel *model = gui->model;

  // Remove TreeView rows from rr_list. It needs to be populated before
  for(GList *node = rr_list; node != NULL; node = g_list_next(node))
  {
    GtkTreePath *path = gtk_tree_row_reference_get_path((GtkTreeRowReference*)node->data);

    if(path)
    {
      GtkTreeIter  iter;
      if(gtk_tree_model_get_iter(model, &iter, path))
        gtk_list_store_remove(GTK_LIST_STORE(model), &iter);
    }
  }

  // Cleanup the list of rows
  g_list_foreach(rr_list, (GFunc) gtk_tree_row_reference_free, NULL);
  g_list_free(rr_list);
}


static void _select_all_callback(GtkButton *button,
                                 gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gtk_tree_selection_select_all(selection);
}


static void _select_none_callback(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gtk_tree_selection_unselect_all(selection);
}


static void _select_invert_callback(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);

  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(gui->model, &iter);
  while(valid)
  {
    if(gtk_tree_selection_iter_is_selected(selection, &iter))
      gtk_tree_selection_unselect_iter(selection, &iter);
    else
      gtk_tree_selection_select_iter(selection, &iter);

    valid = gtk_tree_model_iter_next(gui->model, &iter);
  }
}


static void _db_update_timestamp(const dt_imgid_t id, const time_t timestamp)
{
  // Update DB writing timestamp with XMP file timestamp
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "UPDATE main.images"
     " SET write_timestamp = ?2"
     " WHERE id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 2, timestamp);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}


static void _get_crawler_entry_from_model(GtkTreeModel *model,
                                          GtkTreeIter *iter,
                                          dt_control_crawler_result_t *entry)
{
  gtk_tree_model_get(model, iter,
                     DT_CONTROL_CRAWLER_COL_IMAGE_PATH, &entry->image_path,
                     DT_CONTROL_CRAWLER_COL_ID,         &entry->id,
                     DT_CONTROL_CRAWLER_COL_XMP_PATH,   &entry->xmp_path,
                     DT_CONTROL_CRAWLER_COL_TS_DB_INT,  &entry->timestamp_db,
                     DT_CONTROL_CRAWLER_COL_TS_XMP_INT, &entry->timestamp_xmp,
                     -1); // marks list end
}


static void _append_row_to_remove(GtkTreeModel *model,
                                  GtkTreePath *path,
                                  GList **rowref_list)
{
  // append TreeModel rows to the list to remove
  GtkTreeRowReference *rowref = gtk_tree_row_reference_new(model, path);
  *rowref_list = g_list_append(*rowref_list, rowref);
}

static void _log_synchronization(dt_control_crawler_gui_t *gui,
                                 gchar *pattern,
                                 gchar *filepath)
{
  gchar *message = g_markup_printf_escaped(pattern, filepath ? filepath : "");

  // add a new line in the log TreeView
  GtkTreeModel *model_log = gtk_tree_view_get_model(GTK_TREE_VIEW(gui->log));
  gtk_list_store_insert_with_values(GTK_LIST_STORE(model_log), NULL, -1, 0, message, -1);

  g_free(message);
}


static void sync_xmp_to_db(GtkTreeModel *model,
                           GtkTreePath *path,
                           GtkTreeIter *iter,
                           gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { NO_IMGID };
  _get_crawler_entry_from_model(model, iter, &entry);
  _db_update_timestamp(entry.id, entry.timestamp_xmp);

  const gboolean error = dt_history_load_and_apply(entry.id, entry.xmp_path, 0);

  if(error)
  {
    _log_synchronization(gui, _("ERROR: %s NOT synced XMP → DB"), entry.image_path);
    _log_synchronization(gui, _("ERROR: cannot write the database."
                                " the destination may be full, offline or read-only."),
                         NULL);
  }
  else
  {
    _append_row_to_remove(model, path, &gui->rows_to_remove);
    _log_synchronization(gui, _("SUCCESS: %s synced XMP → DB"), entry.image_path);
  }

  _free_crawler_result(&entry);
}


static void sync_db_to_xmp(GtkTreeModel *model,
                           GtkTreePath *path,
                           GtkTreeIter *iter,
                           gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { NO_IMGID };
  _get_crawler_entry_from_model(model, iter, &entry);

  // write the XMP and make sure it get the last modified timestamp of the db
  const gboolean error = dt_image_write_sidecar_file(entry.id);
  _set_modification_time(entry.xmp_path, entry.timestamp_db);

  if(error)
  {
    _log_synchronization(gui, _("ERROR: %s NOT synced DB → XMP"), entry.image_path);
    _log_synchronization(gui,
                         _("ERROR: cannot write %s \nthe destination may be full,"
                           " offline or read-only."), entry.xmp_path);
  }
  else
  {
    _append_row_to_remove(model, path, &gui->rows_to_remove);
    _log_synchronization(gui, _("SUCCESS: %s synced DB → XMP"), entry.image_path);
  }

  _free_crawler_result(&entry);
}

static void sync_newest_to_oldest(GtkTreeModel *model,
                                  GtkTreePath *path,
                                  GtkTreeIter *iter,
                                  gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { NO_IMGID };
  _get_crawler_entry_from_model(model, iter, &entry);

  gboolean error = FALSE;

  if(entry.timestamp_xmp > entry.timestamp_db)
  {
    // WRITE XMP in DB
    _db_update_timestamp(entry.id, entry.timestamp_xmp);
    error = dt_history_load_and_apply(entry.id, entry.xmp_path, 0);
    if(error)
    {
      _log_synchronization
        (gui,
         _("ERROR: %s NOT synced new (XMP) → old (DB)"), entry.image_path);
      _log_synchronization
        (gui,
         _("ERROR: cannot write the database. the destination may be full,"
           " offline or read-only."), NULL);
    }
    else
    {
      _log_synchronization
        (gui,
         _("SUCCESS: %s synced new (XMP) → old (DB)"), entry.image_path);
    }
  }
  else if(entry.timestamp_xmp < entry.timestamp_db)
  {
    // write the XMP and make sure it get the last modified timestamp of the db
    error = dt_image_write_sidecar_file(entry.id);
    _set_modification_time(entry.xmp_path, entry.timestamp_db);

    dt_print(DT_DEBUG_ALWAYS, "%s synced DB (new) → XMP (old)", entry.image_path);
    if(error)
    {
      _log_synchronization
        (gui,
         _("ERROR: %s NOT synced new (DB) → old (XMP)"), entry.image_path);
      _log_synchronization
        (gui,
         _("ERROR: cannot write %s \nthe destination may be full, offline or read-only."),
         entry.xmp_path);
    }
    else
    {
      _log_synchronization(gui, _("SUCCESS: %s synced new (DB) → old (XMP)"),
                           entry.image_path);
    }
  }
  else
  {
    // we should never reach that part of the code
    // if both timestamps are equal, they should not be in this list in the first place
    error = TRUE;
    _log_synchronization(gui, _("EXCEPTION: %s has inconsistent timestamps"),
                         entry.image_path);
  }

  if(!error) _append_row_to_remove(model, path, &gui->rows_to_remove);

  _free_crawler_result(&entry);
}


static void sync_oldest_to_newest(GtkTreeModel *model,
                                  GtkTreePath *path,
                                  GtkTreeIter *iter,
                                  gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { NO_IMGID };
  _get_crawler_entry_from_model(model, iter, &entry);
  gboolean error = FALSE;

  if(entry.timestamp_xmp < entry.timestamp_db)
  {
    // WRITE XMP in DB
    _db_update_timestamp(entry.id, entry.timestamp_xmp);
    error = dt_history_load_and_apply(entry.id, entry.xmp_path, 0);
    if(error)
    {
      _log_synchronization(gui,
                           _("ERROR: %s NOT synced old (XMP) → new (DB)"),
                           entry.image_path);
    _log_synchronization(gui,
                         _("ERROR: cannot write the database."
                           " the destination may be full, offline or read-only."), NULL);
    }
    else
    {
      _log_synchronization(gui,
                           _("SUCCESS: %s synced old (XMP) → new (DB)"),
                           entry.image_path);
    }
  }
  else if(entry.timestamp_xmp > entry.timestamp_db)
  {
    // WRITE DB in XMP
    error = dt_image_write_sidecar_file(entry.id);
    _set_modification_time(entry.xmp_path, entry.timestamp_db);
    if(error)
    {
      _log_synchronization(gui,
                           _("ERROR: %s NOT synced old (DB) → new (XMP)"),
                           entry.image_path);
      _log_synchronization(gui,
                           _("ERROR: cannot write %s \nthe destination may be full,"
                             " offline or read-only."), entry.xmp_path);
    }
    else
    {
      _log_synchronization(gui,
                           _("SUCCESS: %s synced old (DB) → new (XMP)"),
                           entry.image_path);
    }
  }
  else
  {
    // we should never reach that part of the code
    // if both timestamps are equal, they should not be in this list in the first place
    error = TRUE;
    _log_synchronization(gui,
                         _("EXCEPTION: %s has inconsistent timestamps"),
                         entry.image_path);
  }

  if(!error)
    _append_row_to_remove(model, path, &gui->rows_to_remove);

  _free_crawler_result(&entry);
}

// overwrite database with xmp
static void _reload_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gui->rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_xmp_to_db, gui);
  _delete_selected_rows(gui);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

// overwrite xmp with database
void _overwrite_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gui->rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_db_to_xmp, gui);
  _delete_selected_rows(gui);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

// overwrite the oldest with the newest
static void _newest_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gui->rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_newest_to_oldest, gui);
  _delete_selected_rows(gui);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

// overwrite the newest with the oldest
static void _oldest_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gui->rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_oldest_to_newest, gui);
  _delete_selected_rows(gui);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

static gchar* str_time_delta(const int time_delta)
{
  // display the time difference as a legible string
  int seconds = time_delta;

  int minutes = seconds / 60;
  seconds -= 60 * minutes;

  int hours = minutes / 60;
  minutes -= 60 * hours;

  const int days = hours / 24;
  hours -= 24 * days;

  return g_strdup_printf(_("%id %02dh %02dm %02ds"), days, hours, minutes, seconds);
}

// show a popup window with a list of updated images/xmp files and allow the user to tell dt what to do about them
static void _crawler_show_image_list(GList *images, const gboolean modal)
{
  if(!images) return;

  dt_control_crawler_gui_t *gui = malloc(sizeof(dt_control_crawler_gui_t));

  // a list with all the images
  GtkTreeViewColumn *column;
  GtkListStore *store = gtk_list_store_new(DT_CONTROL_CRAWLER_NUM_COLS,
                                           G_TYPE_INT,    // id
                                           G_TYPE_STRING, // image path
                                           G_TYPE_STRING, // xmp path
                                           G_TYPE_STRING, // timestamp from xmp
                                           G_TYPE_STRING, // timestamp from db
                                           G_TYPE_INT,    // timestamp to db
                                           G_TYPE_INT,
                                           G_TYPE_STRING, // report: newer version
                                           G_TYPE_STRING);// time delta

  gui->model = GTK_TREE_MODEL(store);

  for(GList *list_iter = images; list_iter; list_iter = g_list_next(list_iter))
  {
    dt_control_crawler_result_t *item = list_iter->data;
    char timestamp_db[64], timestamp_xmp[64];
    struct tm tm_stamp;
    strftime(timestamp_db, sizeof(timestamp_db),
             "%c", localtime_r(&item->timestamp_db, &tm_stamp));
    strftime(timestamp_xmp, sizeof(timestamp_xmp),
             "%c", localtime_r(&item->timestamp_xmp, &tm_stamp));

    const time_t time_delta = llabs(item->timestamp_db - item->timestamp_xmp);
    gchar *timestamp_delta = str_time_delta(time_delta);

    gtk_list_store_insert_with_values(store, NULL, -1,
       DT_CONTROL_CRAWLER_COL_ID, item->id,
       DT_CONTROL_CRAWLER_COL_IMAGE_PATH, item->image_path,
       DT_CONTROL_CRAWLER_COL_XMP_PATH, item->xmp_path,
       DT_CONTROL_CRAWLER_COL_TS_XMP, timestamp_xmp,
       DT_CONTROL_CRAWLER_COL_TS_DB, timestamp_db,
       DT_CONTROL_CRAWLER_COL_TS_XMP_INT, item->timestamp_xmp,
       DT_CONTROL_CRAWLER_COL_TS_DB_INT, item->timestamp_db,
       DT_CONTROL_CRAWLER_COL_REPORT, (item->timestamp_xmp > item->timestamp_db)
                                      ? _("XMP")
                                      : _("database"),
       DT_CONTROL_CRAWLER_COL_TIME_DELTA, timestamp_delta,
       -1);
    _free_crawler_result(item);
    g_free(timestamp_delta);
  }
  g_list_free_full(images, g_free);

  GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);

  gui->tree = GTK_TREE_VIEW(tree); // FIXME: do we need to free that later ?

  GtkCellRenderer *renderer_text = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes
    (_("path"), renderer_text, "text",
     DT_CONTROL_CRAWLER_COL_IMAGE_PATH, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(200));
  g_object_set(renderer_text, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);

  column = gtk_tree_view_column_new_with_attributes
    (_("XMP timestamp"), gtk_cell_renderer_text_new(), "text",
     DT_CONTROL_CRAWLER_COL_TS_XMP, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  column = gtk_tree_view_column_new_with_attributes
    (_("database timestamp"), gtk_cell_renderer_text_new(), "text",
     DT_CONTROL_CRAWLER_COL_TS_DB, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  column = gtk_tree_view_column_new_with_attributes
    (_("newest"), gtk_cell_renderer_text_new(), "text",
     DT_CONTROL_CRAWLER_COL_REPORT, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  GtkCellRenderer *renderer_date = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes
    (_("time difference"), renderer_date, "text",
     DT_CONTROL_CRAWLER_COL_TIME_DELTA, NULL);
  g_object_set(renderer_date, "xalign", 1., NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  GtkWidget *scroll = dt_gui_scroll_wrap(tree);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  // build a dialog window that contains the list of images
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons
    (_("updated XMP sidecar files found"), GTK_WINDOW(win),
     GTK_DIALOG_DESTROY_WITH_PARENT | (modal ? GTK_DIALOG_MODAL : 0), _("_close"),
     GTK_RESPONSE_CLOSE, NULL);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_set_size_request(dialog, -1, DT_PIXEL_APPLY_DPI(400));
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(win));


  GtkWidget *select_all = gtk_button_new_with_label(_("select all"));
  GtkWidget *select_none = gtk_button_new_with_label(_("select none"));
  GtkWidget *select_invert = gtk_button_new_with_label(_("invert selection"));
  g_signal_connect(select_all, "clicked", G_CALLBACK(_select_all_callback), gui);
  g_signal_connect(select_none, "clicked", G_CALLBACK(_select_none_callback), gui);
  g_signal_connect(select_invert, "clicked", G_CALLBACK(_select_invert_callback), gui);

  GtkWidget *label = gtk_label_new_with_mnemonic(_("on the selection:"));
  GtkWidget *reload_button = gtk_button_new_with_label(_("keep the XMP edit"));
  GtkWidget *overwrite_button = gtk_button_new_with_label(_("keep the database edit"));
  GtkWidget *newest_button = gtk_button_new_with_label(_("keep the newest edit"));
  GtkWidget *oldest_button = gtk_button_new_with_label(_("keep the oldest edit"));
  g_signal_connect(reload_button, "clicked", G_CALLBACK(_reload_button_clicked), gui);
  g_signal_connect(overwrite_button, "clicked", G_CALLBACK(_overwrite_button_clicked), gui);
  g_signal_connect(newest_button, "clicked", G_CALLBACK(_newest_button_clicked), gui);
  g_signal_connect(oldest_button, "clicked", G_CALLBACK(_oldest_button_clicked), gui);

  /* Feedback spinner in case synch happens over network and stales */
  gui->spinner = gtk_spinner_new();

  /* Log report */
  gui->log = gtk_tree_view_new();

  gtk_tree_view_insert_column_with_attributes
    (GTK_TREE_VIEW(gui->log), -1,
     _("synchronization log"), renderer_text,
     "markup", 0, NULL);

  GtkListStore *store_log = gtk_list_store_new (1, G_TYPE_STRING);
  GtkTreeModel *model_log = GTK_TREE_MODEL(store_log);
  gtk_tree_view_set_model(GTK_TREE_VIEW(gui->log), model_log);
  g_object_unref(model_log);

  dt_gui_dialog_add(GTK_DIALOG(dialog),
    dt_gui_hbox(select_all, select_none, select_invert),
    scroll,
    dt_gui_hbox(label, reload_button, overwrite_button, newest_button, oldest_button, gui->spinner),
    dt_gui_scroll_wrap(gui->log));
  gtk_widget_show_all(dialog);

  g_signal_connect(dialog, "response",
                   G_CALLBACK(dt_control_crawler_response_callback), gui);
}

void dt_control_crawler_show_image_list(GList *images)
{
  // the synchronous startup crawl blocks the ui anyway, so keep it modal
  _crawler_show_image_list(images, TRUE);
}

/* backthumb crawler */

static inline gboolean _lighttable_silent(void)
{
  return dt_view_get_current() == DT_VIEW_LIGHTTABLE
          && dt_get_wtime() > darktable.backthumbs.time;
}

static inline gboolean _valid_mip(dt_mipmap_size_t mip)
{
  return mip > DT_MIPMAP_0 && mip < DT_MIPMAP_LDR_MAX;
}

static inline gboolean _still_thumbing(void)
{
  return darktable.backthumbs.state == DT_JOB_STATE_RUNNING
      && _lighttable_silent();
}

static int _update_img_thumbs(const dt_imgid_t imgid,
                              const dt_mipmap_size_t max_mip,
                              const int64_t stamp)
{
  dt_backthumb_t *bt = &darktable.backthumbs;

  /* as generating the thumb might take some time watch out for a non-running state
      and possibly return asap without updating the database.
  */
  for(dt_mipmap_size_t k = max_mip; k >= DT_MIPMAP_1 && bt->state == DT_JOB_STATE_RUNNING; k--)
  {
    dt_mipmap_buffer_t buf;
    dt_mipmap_cache_get(&buf, imgid, k, DT_MIPMAP_BLOCKING, 'r');
    dt_mipmap_cache_release(&buf);
  }

  if(bt->state != DT_JOB_STATE_RUNNING)
    return 0;

  // we have written all thumbs and are in running state so it's safe to write timestamp, hash and mipsize
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images"
                              " SET thumb_maxmip = ?2, thumb_timestamp = ?3"
                              " WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, max_mip);
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 3, stamp);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  dt_mipmap_cache_evict(imgid);
  dt_history_hash_set_mipmap(imgid);
  return 1;
}

static int _update_all_thumbs(const dt_mipmap_size_t max_mip)
{
  int missed = 0;
  int updated = 0;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id, import_timestamp, change_timestamp"
                              " FROM main.images"
                              " WHERE thumb_timestamp < import_timestamp"
                              "  OR thumb_timestamp < change_timestamp"
                              "  OR thumb_maxmip < ?1"
                              " ORDER BY id DESC",
                                -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, max_mip);
  while(sqlite3_step(stmt) == SQLITE_ROW && _still_thumbing())
  {
    const dt_imgid_t imgid = sqlite3_column_int(stmt, 0);
    const int64_t stamp = MAX(sqlite3_column_int64(stmt, 1), sqlite3_column_int64(stmt, 2));

    char path[PATH_MAX] = { 0 };
    dt_image_full_path(imgid, path, sizeof(path), NULL);
    const gboolean available = dt_util_test_image_file(path);

    if(available)
      updated += _update_img_thumbs(imgid, max_mip, stamp);
    else
    {
      missed++;
      dt_print(DT_DEBUG_CACHE, "[thumb crawler] '%s' ID=%d NOT available", path, imgid);
    }
  }
  sqlite3_finalize(stmt);

  if(updated)
    dt_print(DT_DEBUG_CACHE,
      "[thumb crawler] max_mip=%d, %d thumbs updated, %d not found, %s",
      max_mip, updated, missed,
      _still_thumbing() ? "all done" : "interrupted by user activity");

  return updated;
}

static void _reinitialize_thumbs_database(void)
{
  dt_print(DT_DEBUG_CACHE, "[thumb crawler] initialize database");
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),                              "UPDATE main.images"
                              " SET thumb_maxmip = 0, thumb_timestamp = -1",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

/* public */
void dt_set_backthumb_time(const double next)
{
  dt_backthumb_t *bt = &darktable.backthumbs;
  if(next > 0.5)
    bt->time = dt_get_wtime() + next;
  else
    bt->time = fmax(bt->time, dt_get_wtime() + bt->idle);
}

void dt_update_thumbs_thread(void *p)
{
  dt_pthread_setname("thumbs_update");
  dt_backthumb_t *bt = &darktable.backthumbs;

  bt->idle = (double)dt_conf_get_float("backthumbs_inactivity");
  const dt_mipmap_size_t mipsize = dt_mipmap_cache_get_min_mip_from_pref(dt_conf_get_string_const("backthumbs_mipsize"));
  const gboolean dwriting = dt_conf_get_bool("cache_disk_backend");
  const gboolean service = dt_conf_get_bool("backthumbs_initialize");

  bt->state = DT_JOB_STATE_FINISHED;

  if(!dwriting || !_valid_mip(mipsize))
  {
    dt_print(DT_DEBUG_CACHE, "[thumb crawler] closing due to preferences setting");
    return;
  }

  // return if any thumbcache dir is not writable
  for(dt_mipmap_size_t k = DT_MIPMAP_1; k <= DT_MIPMAP_LDR_MAX-1; k++)
  {
    char dirname[PATH_MAX] = { 0 };
    snprintf(dirname, sizeof(dirname), "%s.d/%d", darktable.mipmap_cache->cachedir, k);
    if(g_mkdir_with_parents(dirname, 0750))
    {
      dt_print(DT_DEBUG_CACHE, "[thumb crawler] can't create mipmap dir '%s'", dirname);
      return;
    }
  }

  dt_print(DT_DEBUG_CACHE, "[thumb crawler] started");
  bt->state = DT_JOB_STATE_RUNNING;
  int updated = 0;

  if(service)
  {
    _reinitialize_thumbs_database();
    dt_conf_set_bool("backthumbs_initialize", FALSE);
  }

  dt_set_backthumb_time(1.0);
  while(bt->state == DT_JOB_STATE_RUNNING)
  {
    for(int i = 0; i < 12 && bt->state == DT_JOB_STATE_RUNNING; i++)
      g_usleep(250000);

    if(bt->state != DT_JOB_STATE_RUNNING)
      break;

    if(_lighttable_silent())
      updated += _update_all_thumbs(mipsize);
  }

  dt_print(DT_DEBUG_CACHE, "[thumb crawler] closing, %d mipmaps updated", updated);
  bt->state = DT_JOB_STATE_FINISHED;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
