/*
    This file is part of darktable,
    Copyright (C) 2014-2026 darktable developers.

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
#include "common/film.h"
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

typedef enum xmp_condition
{
  DT_XMP_CONDITION_MISSING = 1,
  DT_XMP_CONDITION_CHANGED,
  DT_XMP_CONDITION_NEW
} xmp_condition;


typedef struct dt_control_crawler_result_t
{
  dt_imgid_t id, version, count;
  time_t timestamp_xmp;
  time_t timestamp_db;
  char *image_path, *xmp_path;
  xmp_condition condition;
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

GList *_get_list_dir(void);
GList *_get_list_xmp(void);
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
  xmp_condition condition;
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
  int reserved_capacity;
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
    item->xmp_path = g_strdup("");
    item->condition = DT_XMP_CONDITION_MISSING;
    return;
  }

  // no need to look for xmp files if none get written anyway.
  if(look_for_xmp)
  {
    // construct the xmp filename for this image
    gchar xmp_name[PATH_MAX] = { 0 };
    g_strlcpy(xmp_name, item->filename, sizeof(xmp_name));
    dt_image_path_append_version_no_db(item->version, xmp_name, sizeof(xmp_name));
    size_t filename_len = strlen(xmp_name);
    if(filename_len + 4 >= PATH_MAX) return;
    xmp_name[filename_len++] = '.';
    xmp_name[filename_len++] = 'x';
    xmp_name[filename_len++] = 'm';
    xmp_name[filename_len++] = 'p';
    xmp_name[filename_len] = '\0';

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
      if(item->version) //xmp-duplicate is missing: let’s mark it for removal from db.
      {
        item->condition = DT_XMP_CONDITION_MISSING;
        item->timestamp_xmp = 0;
        item->xmp_path = g_strdup("");
      }
      else // xmp (v.0) is missing
      {
        // check disk for existence of XMP-duplicates
        char *_ext = g_strrstr(item->filename, ".");
        filename_len = strlen(item->filename) - strlen(_ext);
        char *_dup_name_st = calloc(filename_len + 1, sizeof(char));
        if(_dup_name_st)
          g_strlcpy(_dup_name_st, item->filename, filename_len + 1);
        _dup_name_st = g_strconcat(_dup_name_st, "_", NULL);
        GError *_error = NULL;
        GDir *_dir = g_dir_open(item->folder, 0, &_error);
        gchar *_name;
        while((_name = (gchar *)g_dir_read_name(_dir)))
        {
          if(!strcmp(_name + strlen(_name) - 4, ".xmp"))
          {
            char *_cropped_name = calloc(filename_len + 2, sizeof(char));
            if(_cropped_name)
              g_strlcpy(_cropped_name, _name, filename_len + 2);

            // if duplicates exist = xmp (v.0) has been deleted -> mark as missing
            if(g_strcmp0(_dup_name_st, _cropped_name) == 0)
            {
              item->condition = DT_XMP_CONDITION_MISSING;
              item->timestamp_xmp = 0;
              item->xmp_path = g_strdup("");
              break;
            }
          }
        }
        // if there are no duplicates = the xmp has been lost or was never created ->
        // ignore this. xmp will appear after next edit
      }
      g_free(xmp_path);
      return;
    }

    // step 1: check if the xmp is newer than our db entry
    if(item->timestamp_db + MAX_TIME_SKEW < statbuf.st_mtime)
    {
      item->condition = DT_XMP_CONDITION_CHANGED;
      item->timestamp_xmp = statbuf.st_mtime;
      item->xmp_path = g_strdup(xmp_path);
      g_free(xmp_path);
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

  GError *_error = NULL;
  GDir *_dir = g_dir_open(dir->folder, 0, &_error);
  gchar *_name;

  GList *_db_entries = NULL;
  for(int i = dir->first; i < dir->first + dir->count; i++)
  {
    dt_crawler_item_t *_item = &scan->items[i];
    // construct the xmp filename for this image
    gchar _xmp_db_name[PATH_MAX] = { 0 };
    g_strlcpy(_xmp_db_name, _item->filename, sizeof(_xmp_db_name));
    dt_image_path_append_version_no_db(_item->version, _xmp_db_name, sizeof(_xmp_db_name));
    size_t len = strlen(_xmp_db_name);
    if(len + 4 >= PATH_MAX) return;
    _xmp_db_name[len++] = '.';
    _xmp_db_name[len++] = 'x';
    _xmp_db_name[len++] = 'm';
    _xmp_db_name[len++] = 'p';
    _xmp_db_name[len] = '\0';
    _db_entries = g_list_prepend(_db_entries, g_strdup(_xmp_db_name));
  }

  GList *_images_on_disk = NULL;
  GList *_xmp_on_disk = NULL;
  while((_name = (gchar *)g_dir_read_name(_dir)))
  {
    if(!strcmp(_name + strlen(_name) - 4, ".xmp"))
    {
      _db_entries = g_list_first(_db_entries);
      GList *found = g_list_find_custom(_db_entries, _name, (GCompareFunc)g_strcmp0);
      if(found == NULL)
        _xmp_on_disk = g_list_prepend(_xmp_on_disk, g_strdup(_name));
    }
    else
      _images_on_disk = g_list_prepend(_images_on_disk, g_strdup(_name));
  }
  g_list_free(_db_entries);
  g_dir_close(_dir);

  _xmp_on_disk = g_list_first(_xmp_on_disk);
  while (_xmp_on_disk)
  {
    gint len;

    len = strlen((gchar *)_xmp_on_disk->data) - 4;
    char *_image_name = calloc(len + 1, sizeof(char));
        if(_image_name)
      g_strlcpy(_image_name, _xmp_on_disk->data, len + 1);
    _images_on_disk = g_list_first(_images_on_disk);
    GList *_found = g_list_find_custom(_images_on_disk, _image_name, (GCompareFunc)g_strcmp0);
    if(_found != NULL)
    {
      if(scan->num_items == scan->reserved_capacity)
      {
        dt_crawler_item_t *grown =
            realloc(scan->items, (scan->reserved_capacity + 10) * sizeof(dt_crawler_item_t));
        if(!grown) break;
        memset(grown + scan->reserved_capacity, 0, 10 * sizeof(dt_crawler_item_t));
        scan->items = grown;
        scan->reserved_capacity = scan->reserved_capacity + 10;
      }

      dt_crawler_item_t *item = &scan->items[scan->num_items];
      item->id = 0;
      item->timestamp_db = 0;
      item->version = 0;
      item->folder = g_strdup(dir->folder);
      item->filename = g_strdup(_image_name);
      item->xmp_path = g_strdup(g_build_filename(dir->folder, _xmp_on_disk->data, NULL));
      item->flags = 0;
      item->new_flags = 0;
      item->condition = DT_XMP_CONDITION_NEW;
      _images_on_disk = g_list_next(_images_on_disk);
      scan->num_items = scan->num_items + 1;
    }
    else
    {
      char *_ext = g_strrstr(_image_name, ".");
      if(_ext)
      {
        len = strlen(_image_name) - strlen(_ext);
        char *_dup_image_name = calloc(len + 1, sizeof(char));
        if(_dup_image_name)
          g_strlcpy(_dup_image_name, _image_name, len + 1 - 3);
        _dup_image_name = g_strconcat(_dup_image_name, _ext, NULL);
        _images_on_disk = g_list_first(_images_on_disk);
        GList *_dup_found = g_list_find_custom(_images_on_disk, _dup_image_name, (GCompareFunc)g_strcmp0);
        if(_dup_found != NULL)
        {
          if(scan->num_items == scan->reserved_capacity)
          {
            dt_crawler_item_t *grown =
                realloc(scan->items, (scan->reserved_capacity + 10) * sizeof(dt_crawler_item_t));
            if(!grown) break;
            memset(grown + scan->reserved_capacity, 0, 10 * sizeof(dt_crawler_item_t));
            scan->items = grown;
            scan->reserved_capacity = scan->reserved_capacity + 10;
          }

          dt_crawler_item_t *item = &scan->items[scan->num_items];
          item->id = 0;
          item->timestamp_db = 0;
          item->version = 0;
          item->folder = g_strdup(dir->folder);
          item->filename = g_strdup(_dup_image_name);
          item->xmp_path = g_strdup(g_build_filename(dir->folder, _xmp_on_disk->data, NULL));
          item->flags = 0;
          item->new_flags = 0;
          item->condition = DT_XMP_CONDITION_NEW;
          _images_on_disk = g_list_next(_images_on_disk);
          scan->num_items = scan->num_items + 1;
        }
        else
        {
          printf("[crawler] %s \n", (gchar *)_xmp_on_disk->data);
          //TODO xmp can be deleted
        }
      }
    }
    _xmp_on_disk = g_list_next(_xmp_on_disk);
    g_free(_image_name);
  }
  g_list_free(_images_on_disk);
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

typedef struct _crawler_collect_t
{
  dt_crawler_item_t *items;
  int capacity;
  int count;
  dt_crawler_dir_t *dirs;
  int num_dirs;
  int dirs_capacity;
} _crawler_collect_t;

static gboolean _crawler_collect_row(const dt_image_sidecar_row_t *row, void *data)
{
  _crawler_collect_t *c = data;
  if(c->count >= c->capacity)
  {
    // the database changed underneath us, grow to fit
    const int new_capacity = c->capacity * 2;
    dt_crawler_item_t *grown =
      realloc(c->items, new_capacity * sizeof(dt_crawler_item_t));
    if(!grown) return FALSE;
    memset(grown + c->capacity, 0,
           (new_capacity - c->capacity) * sizeof(dt_crawler_item_t));
    c->items = grown;
    c->capacity = new_capacity;
  }

  if(!row->folder || !row->filename) return TRUE;

  // start a new directory whenever the folder changes
  if(c->num_dirs == 0 || strcmp(c->dirs[c->num_dirs - 1].folder, row->folder))
  {
    if(c->num_dirs >= c->dirs_capacity)
    {
      const int new_capacity = MAX(16, c->dirs_capacity * 2);
      dt_crawler_dir_t *grown =
        realloc(c->dirs, new_capacity * sizeof(dt_crawler_dir_t));
      if(!grown) return FALSE;
      memset(grown + c->dirs_capacity, 0,
             (new_capacity - c->dirs_capacity) * sizeof(dt_crawler_dir_t));
      c->dirs = grown;
      c->dirs_capacity = new_capacity;
    }
    c->dirs[c->num_dirs].folder = g_strdup(row->folder);
    c->dirs[c->num_dirs].first = c->count;
    c->dirs[c->num_dirs].count = 0;
    c->dirs[c->num_dirs].entries = NULL;
    c->num_dirs++;
  }

  dt_crawler_item_t *item = &c->items[c->count];
  item->id = row->id;
  item->timestamp_db = row->write_timestamp;
  item->version = row->version;
  item->folder = c->dirs[c->num_dirs - 1].folder;
  item->filename = g_strdup(row->filename);
  item->flags = row->flags;
  item->new_flags = item->flags;
  c->dirs[c->num_dirs - 1].count++;
  c->count++;
  return TRUE;
}

// collect the images to be examined.  when filmid is a valid film roll
// only that roll is collected, otherwise the whole library is.
static dt_crawler_item_t *_crawler_collect_items(const dt_filmid_t filmid,
                                                    int *num_items,
                                                    dt_crawler_dir_t **dirs_out,
                                                    int *num_dirs_out,
                                                    int *reserved_capacity)
{
  // reserve based on the number of images we expect to see
  const int capacity = MAX(1, (int)(dt_is_valid_filmid(filmid)
                                    ? dt_film_count_images(filmid)
                                    : dt_image_count()));

  dt_crawler_item_t *items = calloc(capacity, sizeof(dt_crawler_item_t));
  if(!items)
  {
    *num_items = 0;
    *num_dirs_out = 0;
    *dirs_out = NULL;
    return NULL;
  }

  _crawler_collect_t c = { .items = items, .capacity = capacity };
  // ordered by film roll, so the images of one directory come in a
  // contiguous run, which is what lets them be examined a directory at a
  // time against a single listing of it
  dt_image_foreach_sidecar_row(filmid, _crawler_collect_row, &c);

  *num_items = c.count;
  *num_dirs_out = c.num_dirs;
  *dirs_out = c.dirs;
  *reserved_capacity = c.capacity;
  return c.items;
}

static void _crawler_free_items(dt_crawler_item_t *items,
                                const int num_items,
                                dt_crawler_dir_t *dirs,
                                const int num_dirs)
{
  for(int i = 0; i < num_items; i++)
  {
    g_free(items[i].filename);
    if(items[i].xmp_path)
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
  int changed = 0;
  dt_imgid_t *changed_ids = g_new(dt_imgid_t, MAX(1, num_items));
  int *changed_flags = g_new(int, MAX(1, num_items));

  for(int i = 0; i < num_items; i++)
  {
    dt_crawler_item_t *item = &items[i];
    gchar *image_path = g_build_filename(item->folder, item->filename, NULL);

    if(item->condition == DT_XMP_CONDITION_MISSING)
    {
      dt_control_crawler_result_t *entry = malloc(sizeof(dt_control_crawler_result_t));
      if(entry)
      {
        entry->id = item->id;
        entry->timestamp_xmp = 0;
        entry->timestamp_db = item->timestamp_db;
        entry->image_path = g_strdup(image_path);
        entry->xmp_path = g_strdup("");
        entry->condition = DT_XMP_CONDITION_MISSING;
        entry->version = item->version;

        result = g_list_prepend(result, entry);
        dt_print(DT_DEBUG_CONTROL,
                 "[crawler] `%s' (id: %d) removed from storage",
                 item->xmp_path, item->id);
      }
      g_free(image_path);
      continue;
    }

    if(item->condition == DT_XMP_CONDITION_CHANGED)
    {
      dt_control_crawler_result_t *entry = malloc(sizeof(dt_control_crawler_result_t));
      if(entry)
      {
        entry->id = item->id;
        entry->timestamp_xmp = item->timestamp_xmp;
        entry->timestamp_db = item->timestamp_db;
        entry->image_path = g_strdup(image_path);
        entry->xmp_path = g_strdup(item->xmp_path);
        entry->condition = DT_XMP_CONDITION_CHANGED;
        entry->version = item->version;

        result = g_list_prepend(result, entry);
        dt_print(DT_DEBUG_CONTROL,
                 "[crawler] `%s' (id: %d) is a newer XMP file",
                 item->xmp_path, item->id);
        g_free(image_path);
        continue;
      }
    }

    if(item->condition == DT_XMP_CONDITION_NEW)
    {
      dt_control_crawler_result_t *entry = malloc(sizeof(dt_control_crawler_result_t));
      if(entry)
      {
        entry->id = item->id;
        entry->timestamp_xmp = 0;
        entry->timestamp_db = 0;
        entry->image_path = g_strdup(image_path);
        entry->xmp_path = g_strdup(item->xmp_path);
        entry->condition = DT_XMP_CONDITION_NEW;
        entry->version = 0;

        result = g_list_prepend(result, entry);
        dt_print(DT_DEBUG_CONTROL,
                 "[crawler] `%s' found on storage but not in image library",
                 item->xmp_path);
        g_free(image_path);
        continue;
      }
    }

    if(item->new_flags != item->flags)
    {
      changed_ids[changed] = item->id;
      changed_flags[changed] = item->new_flags;
      changed++;
    }
  }

  dt_image_set_flags_list(changed_ids, changed_flags, changed);
  g_free(changed_ids);
  g_free(changed_flags);

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
                                      &scan.dirs, &scan.num_dirs, &scan.reserved_capacity);
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
  _crawler_bg.pending = dt_film_get_by_access();
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
  GtkTreeView *changed_tree;
  GtkTreeModel *changed_model;
  GList *changed_rows_to_remove;
  GtkTreeView *missing_tree;
  GtkTreeModel *missing_model;
  GList *missing_rows_to_remove;
  GtkTreeView *new_dups_tree;
  GtkTreeModel *new_dups_model;
  GList *new_dups_rows_to_remove;
  GtkNotebook *nb;
  GtkWidget *log;
  GtkWidget *spinner;
} dt_control_crawler_gui_t;

// close the window and clean up
static void dt_control_crawler_response_callback(GtkWidget *dialog,
                                                 const gint response_id,
                                                 gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  int _number = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(gui->missing_model), NULL);
  _number = _number + gtk_tree_model_iter_n_children(GTK_TREE_MODEL(gui->changed_model), NULL);
  _number = _number + gtk_tree_model_iter_n_children(GTK_TREE_MODEL(gui->new_dups_model), NULL);

  if(_number == 0) // db is synchronized
  {
    struct timeval time;
    gettimeofday(&time, NULL);
    time_t __time = time.tv_sec;
    dt_conf_set_int64("db_synchronized", __time);
  }

  g_object_unref(G_OBJECT(gui->changed_model));
  g_object_unref(G_OBJECT(gui->missing_model));
  g_object_unref(G_OBJECT(gui->new_dups_model));
  gtk_widget_destroy(dialog);
  free(gui);
}


static void _delete_selected_rows(GList *rr_list,
                                  GtkTreeModel *model)
{
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
  GtkTreeSelection *selection = NULL;
  switch (gtk_notebook_get_current_page(gui->nb))
  {
    case 0:
      selection = gtk_tree_view_get_selection(gui->missing_tree);
      break;
    case 1:
     selection = gtk_tree_view_get_selection(gui->changed_tree);
      break;
    case 2:
      selection = gtk_tree_view_get_selection(gui->new_dups_tree);
  }
  gtk_tree_selection_select_all(selection);
}


static void _select_none_callback(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = NULL;
  switch (gtk_notebook_get_current_page(gui->nb))
  {
    case 0:
      selection = gtk_tree_view_get_selection(gui->missing_tree);
      break;
    case 1:
      selection = gtk_tree_view_get_selection(gui->changed_tree);
      break;
    case 2:
      selection = gtk_tree_view_get_selection(gui->new_dups_tree);
  }
  gtk_tree_selection_unselect_all(selection);
}


static void _select_invert_callback(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeModel *model = NULL;
  GtkTreeSelection *selection = NULL;
  GtkTreeIter iter;
  gboolean valid = FALSE;
  switch (gtk_notebook_get_current_page(gui->nb))
  {
    case 0:
      selection = gtk_tree_view_get_selection(gui->missing_tree);
      model = gui->missing_model;
      break;
    case 1:
      selection = gtk_tree_view_get_selection(gui->changed_tree);
      model = gui->changed_model;
      break;
    case 2:
      selection = gtk_tree_view_get_selection(gui->new_dups_tree);
      model = gui->new_dups_model;
  }

  valid = gtk_tree_model_get_iter_first(model, &iter);
  while(valid)
  {
    if(gtk_tree_selection_iter_is_selected(selection, &iter))
      gtk_tree_selection_unselect_iter(selection, &iter);
    else
      gtk_tree_selection_select_iter(selection, &iter);

    valid = gtk_tree_model_iter_next(model, &iter);
  }
}


static void _db_update_timestamp(const dt_imgid_t id, const time_t timestamp)
{
  // Update DB writing timestamp with XMP file timestamp
  dt_image_set_write_timestamp(id, timestamp);
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

static void _set_remove_flag(char *imgs)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "UPDATE main.images SET flags = (flags|?1) WHERE id IN (?2)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, DT_IMAGE_REMOVE);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgs, -1, SQLITE_STATIC);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void _remove_from_db(GtkTreeModel *model,
                           GtkTreePath *path,
                           GtkTreeIter *iter,
                           gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { NO_IMGID };
  gtk_tree_model_get(model, iter,
                     0, &entry.id,
                     1, &entry.image_path,
                     2, &entry.version,
                     -1);

  dt_image_remove(entry.id);

  // update remove status
  _set_remove_flag(g_strdup_printf("%i", entry.id));

  dt_collection_update(darktable.collection);

  dt_image_synch_all_xmp(entry.image_path);

  dt_film_remove_empty();

  GList *l = NULL;
  l = g_list_append(l, g_strdup_printf("%i", entry.id));
  dt_collection_update_query(darktable.collection,
                             DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                             g_list_copy(l));
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();

  _append_row_to_remove(model, path, &gui->missing_rows_to_remove);
  _log_synchronization(gui, _("SUCCESS: %s removed from DB"), entry.image_path);

  _free_crawler_result(&entry);
}

static void _add_to_db(GtkTreeModel *model,
                           GtkTreePath *path,
                           GtkTreeIter *iter,
                           gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { NO_IMGID };
  gtk_tree_model_get(model, iter,
                     0, &entry.xmp_path,
                     1, &entry.image_path,
                     -1);

  while(gtk_events_pending()) gtk_main_iteration(); // TODO hook for dialog refresh: make it better

  dt_load_from_string(entry.image_path, FALSE, NULL);

  _append_row_to_remove(model, path, &gui->new_dups_rows_to_remove);
  _log_synchronization(gui, _("SUCCESS: %s added to DB"), entry.xmp_path);

  _free_crawler_result(&entry);
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

//ab  while(gtk_events_pending()) gtk_main_iteration(); // TODO hook for dialog refresh: make it better

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
    _append_row_to_remove(model, path, &gui->changed_rows_to_remove);
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

//ab  while(gtk_events_pending()) gtk_main_iteration(); // TODO hook for dialog refresh: make it better

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
    _append_row_to_remove(model, path, &gui->changed_rows_to_remove);
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

//ab  while(gtk_events_pending()) gtk_main_iteration(); // TODO hook for dialog refresh: make it better

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

  if(!error) _append_row_to_remove(model, path, &gui->changed_rows_to_remove);

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

//ab  while(gtk_events_pending()) gtk_main_iteration(); // TODO hook for dialog refresh: make it better

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
    _append_row_to_remove(model, path, &gui->changed_rows_to_remove);

  _free_crawler_result(&entry);
}

static void _add_dups_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->new_dups_tree);
  gui->new_dups_rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, _add_to_db, gui);
  _delete_selected_rows(gui->new_dups_rows_to_remove, gui->new_dups_model);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

static void _remove_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->missing_tree);
  gui->missing_rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, _remove_from_db, gui);
  _delete_selected_rows(gui->missing_rows_to_remove, gui->missing_model);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

// overwrite database with xmp
static void _reload_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->changed_tree);
  gui->changed_rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_xmp_to_db, gui);
  _delete_selected_rows(gui->changed_rows_to_remove, gui->changed_model);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

// overwrite xmp with database
static void _overwrite_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->changed_tree);
  gui->changed_rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_db_to_xmp, gui);
  _delete_selected_rows(gui->changed_rows_to_remove, gui->changed_model);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

// overwrite the oldest with the newest
static void _newest_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->changed_tree);
  gui->changed_rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_newest_to_oldest, gui);
  _delete_selected_rows(gui->changed_rows_to_remove, gui->changed_model);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

// overwrite the newest with the oldest
static void _oldest_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->changed_tree);
  gui->changed_rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_oldest_to_newest, gui);
  _delete_selected_rows(gui->changed_rows_to_remove, gui->changed_model);
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

  GtkNotebook *nb = GTK_NOTEBOOK(gtk_notebook_new());
  GtkWidget *page1 = dt_ui_notebook_page(nb, N_("missing"), NULL);
  GtkWidget *page2 = dt_ui_notebook_page(nb, N_("changed"), NULL);
  GtkWidget *page3 = dt_ui_notebook_page(nb, N_("new"), NULL);
  gtk_widget_show(gtk_notebook_get_nth_page(nb, 1));
  gtk_notebook_set_current_page(nb, 1);
  gui->nb = GTK_NOTEBOOK(nb);

  // a list with all the images
  GtkTreeViewColumn *column;
  GtkListStore *changed_store = gtk_list_store_new(DT_CONTROL_CRAWLER_NUM_COLS,
                                           G_TYPE_INT,    // id
                                           G_TYPE_STRING, // image path
                                           G_TYPE_STRING, // xmp path
                                           G_TYPE_STRING, // timestamp from xmp
                                           G_TYPE_STRING, // timestamp from db
                                           G_TYPE_INT,    // timestamp to db
                                           G_TYPE_INT,
                                           G_TYPE_STRING, // report: newer version
                                           G_TYPE_STRING);// time delta

  gui->changed_model = GTK_TREE_MODEL(changed_store);

  GtkWidget *missing_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_vexpand(missing_scroll, TRUE);
  GtkListStore *missing_store = gtk_list_store_new(3,
                                           G_TYPE_INT,     // id
                                           G_TYPE_STRING,  // image path
                                           G_TYPE_INT);    // version
  gui->missing_model = GTK_TREE_MODEL(missing_store);

  GtkWidget *new_dups_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_vexpand(new_dups_scroll, TRUE);
  GtkListStore *new_dups_store = gtk_list_store_new(2,
                                           G_TYPE_STRING,  // xmp path
                                           G_TYPE_STRING); // image path
  gui->new_dups_model = GTK_TREE_MODEL(new_dups_store);

  for(GList *list_iter = images; list_iter; list_iter = g_list_next(list_iter))
  {
    GtkTreeIter iter;
    dt_control_crawler_result_t *item = list_iter->data;
    char timestamp_db[64], timestamp_xmp[64];
    struct tm tm_stamp;
    strftime(timestamp_db, sizeof(timestamp_db),
             "%c", localtime_r(&item->timestamp_db, &tm_stamp));
    strftime(timestamp_xmp, sizeof(timestamp_xmp),
             "%c", localtime_r(&item->timestamp_xmp, &tm_stamp));

    const time_t time_delta = llabs(item->timestamp_db - item->timestamp_xmp);
    gchar *timestamp_delta = str_time_delta(time_delta);

    if(item->condition == DT_XMP_CONDITION_CHANGED)
    {
      gtk_list_store_append(changed_store, &iter);
      gtk_list_store_set
          (changed_store, &iter,
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
    }
    else if(item->condition == DT_XMP_CONDITION_MISSING)
    {
      gtk_list_store_append(missing_store, &iter);
      gtk_list_store_set
          (missing_store, &iter,
           0, item->id,
           1, item->image_path,
           2, item->version,
           -1);
    }
    else if(item->condition == DT_XMP_CONDITION_NEW)
    {
      gtk_list_store_append(new_dups_store, &iter);
      gtk_list_store_set
          (new_dups_store, &iter,
           0, item->xmp_path,
           1, item->image_path,
           -1);
    }
    _free_crawler_result(item);
    g_free(timestamp_delta);
  }
  g_list_free_full(images, g_free);

  GtkWidget *new_dups_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(new_dups_store));
  GtkTreeSelection *new_dups_selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(new_dups_tree));
  gtk_tree_selection_set_mode(new_dups_selection, GTK_SELECTION_MULTIPLE);
  gui->new_dups_tree = GTK_TREE_VIEW(new_dups_tree); // FIXME: do we need to free that later ?
  GtkCellRenderer *new_dups_renderer_text = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes
    (_("new images"), new_dups_renderer_text, "text", 0, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(new_dups_tree), column);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(200));
  g_object_set(new_dups_renderer_text, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);

  gtk_container_add(GTK_CONTAINER(new_dups_scroll), new_dups_tree);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(new_dups_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  GtkWidget *missing_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(missing_store));
  GtkTreeSelection *missing_selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(missing_tree));
  gtk_tree_selection_set_mode(missing_selection, GTK_SELECTION_MULTIPLE);
  gui->missing_tree = GTK_TREE_VIEW(missing_tree); // FIXME: do we need to free that later ?
  GtkCellRenderer *missing_renderer_text = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes
    (_("missing images"), missing_renderer_text, "text", 1, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(missing_tree), column);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(200));
  g_object_set(missing_renderer_text, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);

  column = gtk_tree_view_column_new_with_attributes
    (_("version (duplicate)"), gtk_cell_renderer_text_new(), "text", 2, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(missing_tree), column);

  gtk_container_add(GTK_CONTAINER(missing_scroll), missing_tree);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(missing_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  GtkWidget *changed_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(changed_store));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(changed_tree));
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);

  gui->changed_tree = GTK_TREE_VIEW(changed_tree); // FIXME: do we need to free that later ?

  GtkCellRenderer *renderer_text = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes
    (_("path"), renderer_text, "text",
     DT_CONTROL_CRAWLER_COL_IMAGE_PATH, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(changed_tree), column);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(200));
  g_object_set(renderer_text, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);

  column = gtk_tree_view_column_new_with_attributes
    (_("XMP timestamp"), gtk_cell_renderer_text_new(), "text",
     DT_CONTROL_CRAWLER_COL_TS_XMP, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(changed_tree), column);

  column = gtk_tree_view_column_new_with_attributes
    (_("database timestamp"), gtk_cell_renderer_text_new(), "text",
     DT_CONTROL_CRAWLER_COL_TS_DB, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(changed_tree), column);

  column = gtk_tree_view_column_new_with_attributes
    (_("newest"), gtk_cell_renderer_text_new(), "text",
     DT_CONTROL_CRAWLER_COL_REPORT, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(changed_tree), column);

  GtkCellRenderer *renderer_date = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes
    (_("time difference"), renderer_date, "text",
     DT_CONTROL_CRAWLER_COL_TIME_DELTA, NULL);
  g_object_set(renderer_date, "xalign", 1., NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(changed_tree), column);

  GtkWidget *scroll = dt_gui_scroll_wrap(changed_tree);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  // build a dialog window that contains the list of images
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons
    (_("manage XMP sidecar files"), GTK_WINDOW(win),
     GTK_DIALOG_DESTROY_WITH_PARENT | (modal ? GTK_DIALOG_MODAL : 0), _("_close"),
     GTK_RESPONSE_CLOSE, NULL);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_set_size_request(dialog, -1, DT_PIXEL_APPLY_DPI(400));
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(win));

  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_dialog_add(GTK_DIALOG(dialog), content_box);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(content_box), box, FALSE, FALSE, 0);
  gtk_widget_set_margin_bottom(GTK_WIDGET(box), 10);
  GtkWidget *select_all = gtk_button_new_with_label(_("select all"));
  GtkWidget *select_none = gtk_button_new_with_label(_("select none"));
  GtkWidget *select_invert = gtk_button_new_with_label(_("invert selection"));
  gtk_box_pack_start(GTK_BOX(box), select_all, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), select_none, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), select_invert, FALSE, FALSE, 0);
  g_signal_connect(select_all, "clicked", G_CALLBACK(_select_all_callback), gui);
  g_signal_connect(select_none, "clicked", G_CALLBACK(_select_none_callback), gui);
  g_signal_connect(select_invert, "clicked", G_CALLBACK(_select_invert_callback), gui);

  gtk_box_pack_start(GTK_BOX(content_box), GTK_WIDGET(nb), TRUE, TRUE, 0);

  gtk_container_add(GTK_CONTAINER(page1), missing_scroll);
  GtkWidget *missing_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  missing_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(page1), missing_box, FALSE, FALSE, 1);
  GtkWidget *remove_button = gtk_button_new_with_label(
        _("remove selected entries from image library"));
  gtk_box_pack_start(GTK_BOX(missing_box), remove_button, FALSE, FALSE, 0);
  g_signal_connect(remove_button, "clicked", G_CALLBACK(_remove_button_clicked), gui);

  gtk_container_add(GTK_CONTAINER(page2), scroll);
  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(page2), box, FALSE, FALSE, 1);
  GtkWidget *label = gtk_label_new_with_mnemonic(_("on the selection:"));
  GtkWidget *reload_button = gtk_button_new_with_label(_("keep the XMP edit"));
  GtkWidget *overwrite_button = gtk_button_new_with_label(_("keep the database edit"));
  GtkWidget *newest_button = gtk_button_new_with_label(_("keep the newest edit"));
  GtkWidget *oldest_button = gtk_button_new_with_label(_("keep the oldest edit"));
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), reload_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), overwrite_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), newest_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), oldest_button, FALSE, FALSE, 0);
  g_signal_connect(reload_button, "clicked", G_CALLBACK(_reload_button_clicked), gui);
  g_signal_connect(overwrite_button, "clicked", G_CALLBACK(_overwrite_button_clicked), gui);
  g_signal_connect(newest_button, "clicked", G_CALLBACK(_newest_button_clicked), gui);
  g_signal_connect(oldest_button, "clicked", G_CALLBACK(_oldest_button_clicked), gui);

  gtk_container_add(GTK_CONTAINER(page3), new_dups_scroll);
  GtkWidget *new_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  new_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(page3), new_box, FALSE, FALSE, 1);
  GtkWidget *add_dups_button = gtk_button_new_with_label(
        _("add selected entries to image library"));
  gtk_box_pack_start(GTK_BOX(new_box), add_dups_button, FALSE, FALSE, 0);
  g_signal_connect(add_dups_button, "clicked", G_CALLBACK(_add_dups_button_clicked), gui);

  /* Log report */
  gui->log = gtk_tree_view_new();
  scroll = dt_gui_scroll_wrap(gui->log);
  gtk_box_pack_start(GTK_BOX(content_box), scroll, TRUE, TRUE, 0);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  /* Feedback spinner in case synch happens over network and stales */
  gui->spinner = gtk_spinner_new();
  gtk_box_pack_start(GTK_BOX(content_box), GTK_WIDGET(gui->spinner), FALSE, FALSE, 0);

  gtk_tree_view_insert_column_with_attributes
    (GTK_TREE_VIEW(gui->log), -1,
     _("synchronization log"), renderer_text,
     "markup", 0, NULL);

  GtkListStore *store_log = gtk_list_store_new (1, G_TYPE_STRING);
  GtkTreeModel *model_log = GTK_TREE_MODEL(store_log);
  gtk_tree_view_set_model(GTK_TREE_VIEW(gui->log), model_log);
  g_object_unref(model_log);

  GdkRectangle workarea;
  GdkDisplay *display = gtk_widget_get_display(dt_ui_main_window(darktable.gui->ui));
  GdkMonitor *mon = gdk_display_get_monitor_at_window(display, gtk_widget_get_window(win));
  gdk_monitor_get_workarea(mon, &workarea);
  gtk_window_resize(GTK_WINDOW(dialog),
                    workarea.width*3/4, workarea.height*3/4); //TODO save and restore
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
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
  dt_image_set_thumb_state(imgid, max_mip, stamp);

  dt_mipmap_cache_evict(imgid);
  dt_history_hash_set_mipmap(imgid);
  return 1;
}

static int _update_all_thumbs(const dt_mipmap_size_t max_mip)
{
  int missed = 0;
  int updated = 0;

  GList *imgs = dt_image_get_outdated_thumbs(max_mip);
  for(const GList *l = imgs; l && _still_thumbing(); l = g_list_next(l))
  {
    const dt_image_thumb_stamp_t *t = l->data;
    const dt_imgid_t imgid = t->id;
    const int64_t stamp = t->stamp;

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
  g_list_free_full(imgs, g_free);

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
  dt_image_reset_thumb_state();
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
