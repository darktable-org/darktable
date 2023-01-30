/*
  This file is part of darktable,
  Copyright (C) 2009-2022 darktable developers.

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

#include "common/image.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/file_location.h"
#include "common/grouping.h"
#include "common/history.h"
#include "common/history_snapshot.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/ratings.h"
#include "common/tags.h"
#include "common/undo.h"
#include "common/selection.h"
#include "common/datetime.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/lightroom.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_rawspeed.h"
#include "imageio/imageio_libraw.h"
#include "win/filepath.h"
#ifdef USE_LUA
#include "lua/image.h"
#endif
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifndef _WIN32
#include <glob.h>
#endif
#include <glib/gstdio.h>

typedef struct dt_undo_monochrome_t
{
  int32_t imgid;
  gboolean before;
  gboolean after;
} dt_undo_monochrome_t;

typedef struct dt_undo_datetime_t
{
  int32_t imgid;
  char before[DT_DATETIME_LENGTH];
  char after[DT_DATETIME_LENGTH];
} dt_undo_datetime_t;

typedef struct dt_undo_geotag_t
{
  int32_t imgid;
  dt_image_geoloc_t before;
  dt_image_geoloc_t after;
} dt_undo_geotag_t;

typedef struct dt_undo_duplicate_t
{
  int32_t orig_imgid;
  int32_t version;
  int32_t new_imgid;
} dt_undo_duplicate_t;

static void _pop_undo_execute(const int imgid, const gboolean before, const gboolean after);
static int32_t _image_duplicate_with_version(const int32_t imgid, const int32_t newversion, const gboolean undo);
static void _pop_undo(gpointer user_data, const dt_undo_type_t type, dt_undo_data_t data, const dt_undo_action_t action, GList **imgs);

static int64_t max_image_position()
{
  sqlite3_stmt *stmt = NULL;

  // get last position
  int64_t max_position = 0;

  const gchar *max_position_query = "SELECT MAX(position) FROM main.images";
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), max_position_query, -1, &stmt, NULL);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    max_position = sqlite3_column_int64(stmt, 0);
  }

  sqlite3_finalize(stmt);
  return max_position;
}

static int64_t create_next_image_position()
{
  /* The sequence pictures come in (import) define the initial sequence.
   *
   * The upper int32_t of the last image position is increased by one
   * while the lower 32 bits are masked out.
   *
   * Example:
   * last image position: (Hex)
   * 0000 0002 0000 0001
   *
   * next image position
   * 0000 0003 0000 0000
   */
  return (max_image_position() & 0xFFFFFFFF00000000) + (1ll << 32);
}

static void _image_local_copy_full_path(const int imgid, char *pathname, size_t pathname_len);

int dt_image_is_ldr(const dt_image_t *img)
{
  const char *c = img->filename + strlen(img->filename);
  while(*c != '.' && c > img->filename) c--;
  if((img->flags & DT_IMAGE_LDR) || !strcasecmp(c, ".jpg") || !strcasecmp(c, ".png")
     || !strcasecmp(c, ".ppm"))
    return 1;
  else
    return 0;
}

int dt_image_is_hdr(const dt_image_t *img)
{
  const char *c = img->filename + strlen(img->filename);
  while(*c != '.' && c > img->filename) c--;
  if((img->flags & DT_IMAGE_HDR) || !strcasecmp(c, ".exr") || !strcasecmp(c, ".hdr")
     || !strcasecmp(c, ".pfm"))
    return 1;
  else
    return 0;
}

// NULL terminated list of supported non-RAW extensions
//  const char *dt_non_raw_extensions[]
//    = { ".jpeg", ".jpg",  ".pfm", ".hdr", ".exr", ".pxn", ".tif", ".tiff", ".png",
//        ".j2c",  ".j2k",  ".jp2", ".jpc", ".gif", ".jpc", ".jp2", ".bmp",  ".dcm",
//        ".jng",  ".miff", ".mng", ".pbm", ".pnm", ".ppm", ".pgm", NULL };
int dt_image_is_raw(const dt_image_t *img)
{
  return (img->flags & DT_IMAGE_RAW);
}

gboolean dt_image_is_monochrome(const dt_image_t *img)
{
  return (img->flags & (DT_IMAGE_MONOCHROME | DT_IMAGE_MONOCHROME_BAYER)) ? TRUE : FALSE;
}

static void _image_set_monochrome_flag(const int32_t imgid, gboolean monochrome, gboolean undo_on)
{
  dt_image_t *img = NULL;
  gboolean changed = FALSE;

  img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(img)
  {
    const int mask_bw = dt_image_monochrome_flags(img);
    dt_image_cache_read_release(darktable.image_cache, img);

    if((!monochrome) && (mask_bw & DT_IMAGE_MONOCHROME_PREVIEW))
    {
      // wanting it to be color found preview
      img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
      img->flags &= ~(DT_IMAGE_MONOCHROME_PREVIEW | DT_IMAGE_MONOCHROME_WORKFLOW);
      changed = TRUE;
    }
    if(monochrome && ((mask_bw == 0) || (mask_bw == DT_IMAGE_MONOCHROME_PREVIEW)))
    {
      // wanting monochrome and found color or just preview without workflow activation
      img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
      img->flags |= (DT_IMAGE_MONOCHROME_PREVIEW | DT_IMAGE_MONOCHROME_WORKFLOW);
      changed = TRUE;
    }
    if(changed)
    {
      const int mask = dt_image_monochrome_flags(img);
      dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
      dt_imageio_update_monochrome_workflow_tag(imgid, mask);

      if(undo_on)
      {
        dt_undo_monochrome_t *undomono = (dt_undo_monochrome_t *)malloc(sizeof(dt_undo_monochrome_t));
        undomono->imgid = imgid;
        undomono->before = mask_bw;
        undomono->after = mask;
        dt_undo_record(darktable.undo, NULL, DT_UNDO_FLAGS, undomono, _pop_undo, g_free);
      }
    }
  }
  else
    dt_print(DT_DEBUG_ALWAYS,"[image_set_monochrome_flag] could not get imgid=%i from cache\n", imgid);
}

void dt_image_set_monochrome_flag(const int32_t imgid, gboolean monochrome)
{
  _image_set_monochrome_flag(imgid, monochrome, TRUE);
}

static void _pop_undo_execute(const int32_t imgid, const gboolean before, const gboolean after)
{
  _image_set_monochrome_flag(imgid, after, FALSE);
}

gboolean dt_image_is_matrix_correction_supported(const dt_image_t *img)
{
  return ((img->flags & (DT_IMAGE_RAW | DT_IMAGE_S_RAW )) && !(img->flags & DT_IMAGE_MONOCHROME)) ? TRUE : FALSE;
}

gboolean dt_image_is_rawprepare_supported(const dt_image_t *img)
{
  return (img->flags & (DT_IMAGE_RAW | DT_IMAGE_S_RAW)) ? TRUE : FALSE;
}

gboolean dt_image_use_monochrome_workflow(const dt_image_t *img)
{
  return ((img->flags & (DT_IMAGE_MONOCHROME | DT_IMAGE_MONOCHROME_BAYER)) ||
          ((img->flags & DT_IMAGE_MONOCHROME_PREVIEW) && (img->flags & DT_IMAGE_MONOCHROME_WORKFLOW)));
}

int dt_image_monochrome_flags(const dt_image_t *img)
{
  return (img->flags & (DT_IMAGE_MONOCHROME | DT_IMAGE_MONOCHROME_PREVIEW | DT_IMAGE_MONOCHROME_BAYER));
}

const char *dt_image_film_roll_name(const char *path)
{
  const char *folder = path + strlen(path);
  const int numparts = CLAMPS(dt_conf_get_int("show_folder_levels"), 1, 5);
  int count = 0;
  while(folder > path)
  {

#ifdef _WIN32
    // in Windows, both \ and / can be folder separator
    if(*folder == G_DIR_SEPARATOR || *folder == '/')
#else
    if(*folder == G_DIR_SEPARATOR)
#endif

      if(++count >= numparts)
      {
        ++folder;
        break;
      }
    --folder;
  }
  return folder;
}

void dt_image_film_roll_directory(const dt_image_t *img, char *pathname, size_t pathname_len)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT folder FROM main.film_rolls WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->film_id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *f = (char *)sqlite3_column_text(stmt, 0);
    g_strlcpy(pathname, f, pathname_len);
  }
  sqlite3_finalize(stmt);
  pathname[pathname_len - 1] = '\0';
}


void dt_image_film_roll(const dt_image_t *img, char *pathname, size_t pathname_len)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT folder FROM main.film_rolls WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->film_id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *f = (char *)sqlite3_column_text(stmt, 0);
    const char *c = dt_image_film_roll_name(f);
    g_strlcpy(pathname, c, pathname_len);
  }
  else
  {
    g_strlcpy(pathname, _("orphaned image"), pathname_len);
  }
  sqlite3_finalize(stmt);
  pathname[pathname_len - 1] = '\0';
}

dt_imageio_write_xmp_t dt_image_get_xmp_mode()
{
  dt_imageio_write_xmp_t res = DT_WRITE_XMP_NEVER;
  const char *config = dt_conf_get_string_const("write_sidecar_files");
  if(config)
  {
    if(!strcmp(config, "after edit"))
      res = DT_WRITE_XMP_LAZY;
    else if(!strcmp(config, "on import"))
      res = DT_WRITE_XMP_ALWAYS;
    else if(!strcmp(config, "TRUE"))
    {
      // migration path from boolean settings in <= 3.6, lazy mode was introduced in 3.8
      // as scripts or tools might use FALSE we can only update TRUE in a safe way.
      // This leaves others like "false" or "FALSE" as DT_WRITE_XMP_NEVER without conf string update
      dt_conf_set_string("write_sidecar_files", "on import");
      res = DT_WRITE_XMP_ALWAYS;
    }
  }
  else
  {
    res = DT_WRITE_XMP_ALWAYS;
    dt_conf_set_string("write_sidecar_files", "on import");
  }
  return res;
}

gboolean dt_image_safe_remove(const int32_t imgid)
{
  // always safe to remove if we do not have .xmp
  if(dt_image_get_xmp_mode() == DT_WRITE_XMP_NEVER) return TRUE;

  // check whether the original file is accessible
  char pathname[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;

  dt_image_full_path(imgid, pathname, sizeof(pathname), &from_cache);

  if(!from_cache)
    return TRUE;

  else
  {
    // finally check if we have a .xmp for the local copy. If no modification done on the local copy it is safe
    // to remove.
    g_strlcat(pathname, ".xmp", sizeof(pathname));
    return !g_file_test(pathname, G_FILE_TEST_EXISTS);
  }
}

void dt_image_full_path(const int32_t imgid, char *pathname, size_t pathname_len, gboolean *from_cache)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT folder || '" G_DIR_SEPARATOR_S "' || filename FROM main.images i, main.film_rolls f WHERE "
                              "i.film_id = f.id and i.id = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    g_strlcpy(pathname, (char *)sqlite3_column_text(stmt, 0), pathname_len);
  }
  sqlite3_finalize(stmt);

  if(*from_cache)
  {
    char lc_pathname[PATH_MAX] = { 0 };
    _image_local_copy_full_path(imgid, lc_pathname, sizeof(lc_pathname));

    if(g_file_test(lc_pathname, G_FILE_TEST_EXISTS))
      g_strlcpy(pathname, (char *)lc_pathname, pathname_len);
    else
      *from_cache = FALSE;
  }
}

char *dt_image_get_filename(const int32_t imgid)
{
  sqlite3_stmt *stmt;

  char filename[PATH_MAX] = { 0 };

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT filename FROM main.images"
                              " WHERE id = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    g_strlcpy(filename, (char *)sqlite3_column_text(stmt, 0), PATH_MAX);
  }
  sqlite3_finalize(stmt);

  return g_strdup(filename);
}

static void _image_local_copy_full_path(const int32_t imgid, char *pathname, size_t pathname_len)
{
  sqlite3_stmt *stmt;

  *pathname = '\0';
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT folder || '" G_DIR_SEPARATOR_S "' || filename FROM main.images i, main.film_rolls f "
                              "WHERE i.film_id = f.id AND i.id = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char filename[PATH_MAX] = { 0 };
    char cachedir[PATH_MAX] = { 0 };
    g_strlcpy(filename, (char *)sqlite3_column_text(stmt, 0), pathname_len);
    char *md5_filename = g_compute_checksum_for_string(G_CHECKSUM_MD5, filename, strlen(filename));
    dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

    // and finally, add extension, needed as some part of the code is looking for the extension
    char *c = filename + strlen(filename);
    while(*c != '.' && c > filename) c--;

    // cache filename old format: <cachedir>/img-<id>-<MD5>.<ext>
    // for upward compatibility we check for the old name, if found we return it
    snprintf(pathname, pathname_len, "%s/img-%d-%s%s", cachedir, imgid, md5_filename, c);

    // if it does not exist, we return the new naming
    if(!g_file_test(pathname, G_FILE_TEST_EXISTS))
    {
      // cache filename format: <cachedir>/img-<MD5>.<ext>
      snprintf(pathname, pathname_len, "%s/img-%s%s", cachedir, md5_filename, c);
    }

    g_free(md5_filename);
  }
  sqlite3_finalize(stmt);
}

void dt_image_path_append_version_no_db(int version, char *pathname, size_t pathname_len)
{
  // the "first" instance (version zero) does not get a version suffix
  if(version > 0)
  {
    // add version information:
    char *filename = g_strdup(pathname);

    char *c = pathname + strlen(pathname);
    while(*c != '.' && c > pathname) c--;
    snprintf(c, pathname + pathname_len - c, "_%02d", version);
    c = pathname + strlen(pathname);
    char *c2 = filename + strlen(filename);
    while(*c2 != '.' && c2 > filename) c2--;
    g_strlcpy(c, c2, pathname + pathname_len - c);
    g_free(filename);
  }
}

void dt_image_path_append_version(const int32_t imgid, char *pathname, size_t pathname_len)
{
  // get duplicate suffix
  int version = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT version FROM main.images WHERE id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  dt_image_path_append_version_no_db(version, pathname, pathname_len);
}

void dt_image_print_exif(const dt_image_t *img, char *line, size_t line_len)
{
  char *exposure_str = dt_util_format_exposure(img->exif_exposure);

  snprintf(line, line_len, "%s f/%.1f %dmm ISO %d", exposure_str, img->exif_aperture, (int)img->exif_focal_length,
           (int)img->exif_iso);

  g_free(exposure_str);
}

int dt_image_get_xmp_rating_from_flags(const int flags)
{
  return (flags & DT_IMAGE_REJECTED)
    ? -1                              // rejected image = -1
    : (flags & DT_VIEW_RATINGS_MASK); // others = 0 .. 5
}

int dt_image_get_xmp_rating(const dt_image_t *img)
{
  return dt_image_get_xmp_rating_from_flags(img->flags);
}

void dt_image_set_xmp_rating(dt_image_t *img, const int rating)
{
  // clean flags stars and rejected
  img->flags &= ~(DT_IMAGE_REJECTED | DT_VIEW_RATINGS_MASK);

  if(rating == -2) // assuming that value -2 cannot be found
  {
    img->flags |= (DT_VIEW_RATINGS_MASK & dt_conf_get_int("ui_last/import_initial_rating"));
  }
  else if(rating == -1)
  {
    img->flags |= DT_IMAGE_REJECTED;
  }
  else
  {
    img->flags |= (DT_VIEW_RATINGS_MASK & rating);
  }
}

void dt_image_get_location(const int32_t imgid, dt_image_geoloc_t *geoloc)
{
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  geoloc->longitude = img->geoloc.longitude;
  geoloc->latitude = img->geoloc.latitude;
  geoloc->elevation = img->geoloc.elevation;
  dt_image_cache_read_release(darktable.image_cache, img);
}

static void _set_location(const int32_t imgid, const dt_image_geoloc_t *geoloc)
{
  /* fetch image from cache */
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  memcpy(&image->geoloc, geoloc, sizeof(dt_image_geoloc_t));

  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
}

static void _set_datetime(const int32_t imgid, const char *datetime)
{
  /* fetch image from cache */
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  dt_datetime_exif_to_img(image, datetime);

  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
}

static void _pop_undo(gpointer user_data, const dt_undo_type_t type, dt_undo_data_t data, const dt_undo_action_t action, GList **imgs)
{
  if(type == DT_UNDO_GEOTAG)
  {
    int i = 0;

    for(GList *list = (GList *)data; list; list = g_list_next(list))
    {
      dt_undo_geotag_t *undogeotag = (dt_undo_geotag_t *)list->data;
      const dt_image_geoloc_t *geoloc = (action == DT_ACTION_UNDO) ? &undogeotag->before : &undogeotag->after;

      _set_location(undogeotag->imgid, geoloc);

      *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(undogeotag->imgid));
      i++;
    }
    if(i > 1) dt_control_log((action == DT_ACTION_UNDO)
                              ? _("geo-location undone for %d images")
                              : _("geo-location re-applied to %d images"), i);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, g_list_copy(*imgs), 0);
  }
  else if(type == DT_UNDO_DATETIME)
  {
    int i = 0;

    for(GList *list = (GList *)data; list; list = g_list_next(list))
    {
      dt_undo_datetime_t *undodatetime = (dt_undo_datetime_t *)list->data;

      _set_datetime(undodatetime->imgid, (action == DT_ACTION_UNDO)
                                         ? undodatetime->before : undodatetime->after);

      *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(undodatetime->imgid));
      i++;
    }
    if(i > 1) dt_control_log((action == DT_ACTION_UNDO)
                              ? _("date/time undone for %d images")
                              : _("date/time re-applied to %d images"), i);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED, g_list_copy(*imgs));
  }
  else if(type == DT_UNDO_DUPLICATE)
  {
    dt_undo_duplicate_t *undo = (dt_undo_duplicate_t *)data;

    if(action == DT_ACTION_UNDO)
    {
      // remove image
      dt_image_remove(undo->new_imgid);
    }
    else
    {
      // restore image, note that we record the new imgid created while
      // restoring the duplicate.
      undo->new_imgid = _image_duplicate_with_version(undo->orig_imgid, undo->version, FALSE);
      *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(undo->new_imgid));
    }
  }
  else if(type == DT_UNDO_FLAGS)
  {
    dt_undo_monochrome_t *undomono = (dt_undo_monochrome_t *)data;

    const gboolean before = (action == DT_ACTION_UNDO) ? undomono->after : undomono->before;
    const gboolean after  = (action == DT_ACTION_UNDO) ? undomono->before : undomono->after;
    _pop_undo_execute(undomono->imgid, before, after);
    *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(undomono->imgid));
  }
}

static void _geotag_undo_data_free(gpointer data)
{
  GList *l = (GList *)data;
  g_list_free_full(l, g_free);
}

static void _image_set_location(GList *imgs, const dt_image_geoloc_t *geoloc, GList **undo, const gboolean undo_on)
{
  for(GList *images = imgs; images; images = g_list_next(images))
  {
    const int32_t imgid = GPOINTER_TO_INT(images->data);

    if(undo_on)
    {
      dt_undo_geotag_t *undogeotag = (dt_undo_geotag_t *)malloc(sizeof(dt_undo_geotag_t));
      undogeotag->imgid = imgid;
      dt_image_get_location(imgid, &undogeotag->before);

      memcpy(&undogeotag->after, geoloc, sizeof(dt_image_geoloc_t));

      *undo = g_list_append(*undo, undogeotag);
    }

    _set_location(imgid, geoloc);
  }
}

void dt_image_set_locations(const GList *imgs, const dt_image_geoloc_t *geoloc, const gboolean undo_on)
{
  if(imgs)
  {
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_GEOTAG);

    _image_set_location((GList *)imgs, geoloc, &undo, undo_on);

    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_GEOTAG, undo, _pop_undo, _geotag_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  }
}

void dt_image_set_location(const int32_t imgid, const dt_image_geoloc_t *geoloc, const gboolean undo_on, const gboolean group_on)
{
  GList *imgs = NULL;
  if(imgid == -1)
    imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
  else
    imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgid));
  if(group_on) dt_grouping_add_grouped_images(&imgs);
  dt_image_set_locations(imgs, geoloc, undo_on);
  g_list_free(imgs);
}

static void _image_set_images_locations(const GList *img, const GArray *gloc,
                                        GList **undo, const gboolean undo_on)
{
  int i = 0;
  for(GList *imgs = (GList *)img; imgs; imgs = g_list_next(imgs))
  {
    const int32_t imgid = GPOINTER_TO_INT(imgs->data);
    const dt_image_geoloc_t *geoloc = &g_array_index(gloc, dt_image_geoloc_t, i);
    if(undo_on)
    {
      dt_undo_geotag_t *undogeotag = (dt_undo_geotag_t *)malloc(sizeof(dt_undo_geotag_t));
      undogeotag->imgid = imgid;
      dt_image_get_location(imgid, &undogeotag->before);

      memcpy(&undogeotag->after, geoloc, sizeof(dt_image_geoloc_t));

      *undo = g_list_prepend(*undo, undogeotag);
    }

    _set_location(imgid, geoloc);
    i++;
  }
}

void dt_image_set_images_locations(const GList *imgs, const GArray *gloc, const gboolean undo_on)
{
  if(!imgs || !gloc || (g_list_length((GList *)imgs) != gloc->len))
    return;
  GList *undo = NULL;
  if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_GEOTAG);

  _image_set_images_locations(imgs, gloc, &undo, undo_on);

  if(undo_on)
  {
    dt_undo_record(darktable.undo, NULL, DT_UNDO_GEOTAG, undo, _pop_undo, _geotag_undo_data_free);
    dt_undo_end_group(darktable.undo);
  }
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
}

void dt_image_update_final_size(const int32_t imgid)
{
  if(imgid <= 0) return;
  int ww = 0, hh = 0;
  if(darktable.develop && darktable.develop->pipe && darktable.develop->pipe->output_imgid == imgid)
  {
    dt_dev_pixelpipe_get_dimensions(darktable.develop->pipe, darktable.develop, darktable.develop->pipe->iwidth,
                                    darktable.develop->pipe->iheight, &ww, &hh);
  }
  dt_image_t *imgtmp = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(ww == imgtmp->final_width && hh == imgtmp->final_height)
    dt_cache_release(&darktable.image_cache->cache, imgtmp->cache_entry);
  else
  {
    imgtmp->final_width = imgtmp->crop_width = ww;
    imgtmp->final_height = imgtmp->crop_height = hh;
    dt_image_cache_write_release(darktable.image_cache, imgtmp, DT_IMAGE_CACHE_RELAXED);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_METADATA_UPDATE);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED);
  }
}

gboolean dt_image_get_final_size(const int32_t imgid, int *width, int *height)
{
  // get the img strcut
  dt_image_t *imgtmp = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  dt_image_t img = *imgtmp;
  dt_image_cache_read_release(darktable.image_cache, imgtmp);
  // if we already have computed them
  if(img.final_height > 0 && img.final_width > 0)
  {
    *width = img.final_width;
    *height = img.final_height;
    return 0;
  }

  // and now we can do the pipe stuff to get final image size
  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, imgid);

  dt_dev_pixelpipe_t pipe;
  int wd = dev.image_storage.width, ht = dev.image_storage.height;
  gboolean res = dt_dev_pixelpipe_init_dummy(&pipe, wd, ht);
  if(res)
  {
    // set mem pointer to 0, won't be used.
    dt_dev_pixelpipe_set_input(&pipe, &dev, NULL, wd, ht, 1.0f);
    dt_dev_pixelpipe_create_nodes(&pipe, &dev);
    dt_dev_pixelpipe_synch_all(&pipe, &dev);
    dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight, &pipe.processed_width,
                                    &pipe.processed_height);
    wd = pipe.processed_width;
    ht = pipe.processed_height;
    res = TRUE;
    dt_dev_pixelpipe_cleanup(&pipe);
  }
  dt_dev_cleanup(&dev);

  imgtmp = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  imgtmp->final_width = *width = wd;
  imgtmp->final_height = *height = ht;
  dt_image_cache_write_release(darktable.image_cache, imgtmp, DT_IMAGE_CACHE_RELAXED);

  return res;
}

void dt_image_set_flip(const int32_t imgid, const dt_image_orientation_t orientation)
{
  sqlite3_stmt *stmt;
  // push new orientation to sql via additional history entry:
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT IFNULL(MAX(num)+1, 0) FROM main.history"
                              " WHERE imgid = ?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  const int iop_flip_MODVER = 2;
  int num = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW) num = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.history"
                              "  (imgid, num, module, operation, op_params, enabled, "
                              "   blendop_params, blendop_version, multi_priority, multi_name)"
                              " VALUES (?1, ?2, ?3, 'flip', ?4, 1, NULL, 0, 0, '') ",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, iop_flip_MODVER);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, &orientation, sizeof(int32_t), SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images"
                              " SET history_end = (SELECT MAX(num) + 1"
                              "                    FROM main.history "
                              "                    WHERE imgid = ?1) WHERE id = ?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);

  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
  dt_image_update_final_size(imgid);
  // write that through to xmp:
  dt_image_write_sidecar_file(imgid);
}

dt_image_orientation_t dt_image_get_orientation(const int32_t imgid)
{
  // find the flip module -- the pointer stays valid until darktable shuts down
  static dt_iop_module_so_t *flip = NULL;
  if(flip == NULL)
  {
    for(const GList *modules = darktable.iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules->data);
      if(!strcmp(module->op, "flip"))
      {
        flip = module;
        break;
      }
    }
  }

  dt_image_orientation_t orientation = ORIENTATION_NULL;

  // db lookup flip params
  if(flip && flip->have_introspection && flip->get_p)
  {
    sqlite3_stmt *stmt;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT op_params, enabled"
      " FROM main.history"
      " WHERE imgid=?1 AND operation='flip'"
      " ORDER BY num DESC LIMIT 1", -1,
      &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 1) != 0)
    {
      // use introspection to get the orientation from the binary params blob
      const void *params = sqlite3_column_blob(stmt, 0);
      orientation = *((dt_image_orientation_t *)flip->get_p(params, "orientation"));
    }
    sqlite3_finalize(stmt);
  }

  if(orientation == ORIENTATION_NULL)
  {
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    orientation = dt_image_orientation(img);
    dt_image_cache_read_release(darktable.image_cache, img);
  }

  return orientation;
}

void dt_image_flip(const int32_t imgid, const int32_t cw)
{
  // this is light table only:
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(darktable.develop->image_storage.id == imgid && cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM) return;

  dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();
  hist->imgid = imgid;
  dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

  dt_image_orientation_t orientation = dt_image_get_orientation(imgid);

  if(cw == 1)
  {
    if(orientation & ORIENTATION_SWAP_XY)
      orientation ^= ORIENTATION_FLIP_Y;
    else
      orientation ^= ORIENTATION_FLIP_X;
  }
  else
  {
    if(orientation & ORIENTATION_SWAP_XY)
      orientation ^= ORIENTATION_FLIP_X;
    else
      orientation ^= ORIENTATION_FLIP_Y;
  }
  orientation ^= ORIENTATION_SWAP_XY;

  if(cw == 2) orientation = ORIENTATION_NULL;
  dt_image_set_flip(imgid, orientation);

  dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
  dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                 dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);
}

/* About the image size ratio
   It has been calculated from the exif data width&height, this is not exact as we crop
   the sensor data in most cases for raws.
   This is managed by
   rawspeed - knowing about default crops
   rawprepare - modify the defaults

   The database does **not** hold the cropped width & height so we fill the data
   when starting to develop.
*/
float dt_image_get_sensor_ratio(const struct dt_image_t *img)
{
  if(img->p_height >0)
    return (double)img->p_width / (double)img->p_height;

  return (double)img->width / (double)img->height;
}

void dt_image_set_raw_aspect_ratio(const int32_t imgid)
{
  /* fetch image from cache */
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  /* set image aspect ratio */
  if(image->orientation < ORIENTATION_SWAP_XY)
    image->aspect_ratio = (float )image->width / (float )image->height;
  else
    image->aspect_ratio = (float )image->height / (float )image->width;

  /* store */
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
}

void dt_image_set_aspect_ratio_to(const int32_t imgid, const float aspect_ratio, const gboolean raise)
{
  if(aspect_ratio > .0f)
  {
    /* fetch image from cache */
    dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

    /* set image aspect ratio */
    image->aspect_ratio = aspect_ratio;

    /* store but don't save xmp*/
    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);

    if(raise && darktable.collection->params.sorts[DT_COLLECTION_SORT_ASPECT_RATIO])
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                                 DT_COLLECTION_PROP_ASPECT_RATIO, g_list_prepend(NULL, GINT_TO_POINTER(imgid)));
  }
}

void dt_image_set_aspect_ratio_if_different(const int32_t imgid, const float aspect_ratio, const gboolean raise)
{
  if(aspect_ratio > .0f)
  {
    /* fetch image from cache */
    dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');

    /* set image aspect ratio */
    if(fabs(image->aspect_ratio - aspect_ratio) > 0.1)
    {
      dt_image_cache_read_release(darktable.image_cache, image);
      dt_image_t *wimage = dt_image_cache_get(darktable.image_cache, imgid, 'w');
      wimage->aspect_ratio = aspect_ratio;
      dt_image_cache_write_release(darktable.image_cache, wimage, DT_IMAGE_CACHE_RELAXED);
    }
    else
      dt_image_cache_read_release(darktable.image_cache, image);

    if(raise && darktable.collection->params.sorts[DT_COLLECTION_SORT_ASPECT_RATIO])
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                                 DT_COLLECTION_PROP_ASPECT_RATIO, g_list_prepend(NULL, GINT_TO_POINTER(imgid)));
  }
}

void dt_image_reset_aspect_ratio(const int32_t imgid, const gboolean raise)
{
  /* fetch image from cache */
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  /* set image aspect ratio */
  image->aspect_ratio = 0.f;

  /* store */
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);

  if(raise && darktable.collection->params.sorts[DT_COLLECTION_SORT_ASPECT_RATIO])
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_ASPECT_RATIO,
                               g_list_prepend(NULL, GINT_TO_POINTER(imgid)));
}

float dt_image_set_aspect_ratio(const int32_t imgid, const gboolean raise)
{
  dt_mipmap_buffer_t buf;
  float aspect_ratio = 0.0;

  // mipmap cache must be initialized, otherwise we'll update next call
  if(darktable.mipmap_cache)
  {
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_0, DT_MIPMAP_BLOCKING, 'r');

    if(buf.buf && buf.height && buf.width)
    {
      aspect_ratio = (float)buf.width / (float)buf.height;
      dt_image_set_aspect_ratio_to(imgid, aspect_ratio, raise);
    }

    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  }

  return aspect_ratio;
}

int32_t dt_image_duplicate(const int32_t imgid)
{
  return dt_image_duplicate_with_version(imgid, -1);
}

static int32_t _image_duplicate_with_version_ext(const int32_t imgid, const int32_t newversion)
{
  sqlite3_stmt *stmt;
  int32_t newid = -1;
  const int64_t image_position = dt_collection_get_image_position(imgid, 0);
  const int64_t new_image_position = (image_position < 0) ? max_image_position() : image_position + 1;

  dt_collection_shift_image_positions(1, new_image_position, 0);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT a.id"
                              "  FROM main.images AS a JOIN main.images AS b"
                              "  WHERE a.film_id = b.film_id AND a.filename = b.filename"
                              "   AND b.id = ?1 AND a.version = ?2"
                              "  ORDER BY a.id DESC",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, newversion);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    newid = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  // requested version is already present in DB, so we just return it
  if(newid != -1) return newid;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "INSERT INTO main.images"
     "  (id, group_id, film_id, width, height, filename, maker, model, lens, exposure,"
     "   aperture, iso, focal_length, focus_distance, datetime_taken, flags,"
     "   output_width, output_height, crop, raw_parameters, raw_denoise_threshold,"
     "   raw_auto_bright_threshold, raw_black, raw_maximum,"
     "   license, sha1sum, orientation, histogram, lightmap,"
     "   longitude, latitude, altitude, color_matrix, colorspace, version, max_version, history_end,"
     "   position, aspect_ratio, exposure_bias, import_timestamp)"
     " SELECT NULL, group_id, film_id, width, height, filename, maker, model, lens,"
     "       exposure, aperture, iso, focal_length, focus_distance, datetime_taken,"
     "       flags, output_width, output_height, crop, raw_parameters, raw_denoise_threshold,"
     "       raw_auto_bright_threshold, raw_black, raw_maximum,"
     "       license, sha1sum, orientation, histogram, lightmap,"
     "       longitude, latitude, altitude, color_matrix, colorspace, NULL, NULL, 0, ?1,"
     "       aspect_ratio, exposure_bias, import_timestamp"
     " FROM main.images WHERE id = ?2",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 1, new_image_position);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT a.id, a.film_id, a.filename, b.max_version"
                              "  FROM main.images AS a JOIN main.images AS b"
                              "  WHERE a.film_id = b.film_id AND a.filename = b.filename AND b.id = ?1"
                              "  ORDER BY a.id DESC",
    -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  int32_t film_id = 1;
  int32_t max_version = -1;
  gchar *filename = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    newid = sqlite3_column_int(stmt, 0);
    film_id = sqlite3_column_int(stmt, 1);
    filename = g_strdup((gchar *)sqlite3_column_text(stmt, 2));
    max_version = sqlite3_column_int(stmt, 3);
  }
  sqlite3_finalize(stmt);

  if(newid != -1)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO main.color_labels (imgid, color)"
                                "  SELECT ?1, color FROM main.color_labels WHERE imgid = ?2",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO main.meta_data (id, key, value)"
                                "  SELECT ?1, key, value FROM main.meta_data WHERE id = ?2",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

#ifdef HAVE_SQLITE_324_OR_NEWER
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO main.tagged_images (imgid, tagid, position)"
                                "  SELECT ?1, tagid, "
                                "        (SELECT (IFNULL(MAX(position),0) & 0xFFFFFFFF00000000)"
                                "         FROM main.tagged_images)"
                                "         + (ROW_NUMBER() OVER (ORDER BY imgid) << 32)"
                                " FROM main.tagged_images AS ti"
                                " WHERE imgid = ?2",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
#else // break down the tagged_images insert per tag
    GList *tags = dt_tag_get_tags(imgid, FALSE);
    for(GList *tag = tags; tag; tag = g_list_next(tag))
    {
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "INSERT INTO main.tagged_images (imgid, tagid, position)"
                                  "  VALUES (?1, ?2, "
                                  "   (SELECT (IFNULL(MAX(position),0) & 0xFFFFFFFF00000000)"
                                  "     + (1 << 32)"
                                  "   FROM main.tagged_images))",
                                  -1, &stmt, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, GPOINTER_TO_INT(tag->data));
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
    g_list_free(tags);
#endif

    if(darktable.develop->image_storage.id == imgid)
    {
      // make sure the current iop-order list is written as this will be duplicated from the db
      dt_ioppr_write_iop_order_list(darktable.develop->iop_order_list, imgid);
      dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);
    }

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO main.module_order (imgid, iop_list, version)"
                                "  SELECT ?1, iop_list, version FROM main.module_order WHERE imgid = ?2",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // set version of new entry and max_version of all involved duplicates (with same film_id and filename)
    // this needs to happen before we do anything with the image cache, as version isn't updated through the cache
    const int32_t version = (newversion != -1) ? newversion : max_version + 1;
    max_version = (newversion != -1) ? MAX(max_version, newversion) : max_version + 1;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "UPDATE main.images SET version=?1 WHERE id = ?2",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, version);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, newid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE main.images SET max_version=?1 WHERE film_id = ?2 AND filename = ?3", -1,
                                &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, max_version);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, film_id);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, filename, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    g_free(filename);
  }
  return newid;
}

static int32_t _image_duplicate_with_version(const int32_t imgid, const int32_t newversion, const gboolean undo)
{
  const int32_t newid = _image_duplicate_with_version_ext(imgid, newversion);

  if(newid != -1)
  {
    if(undo)
    {
      dt_undo_duplicate_t *dupundo = (dt_undo_duplicate_t *)malloc(sizeof(dt_undo_duplicate_t));
      dupundo->orig_imgid = imgid;
      dupundo->version = newversion;
      dupundo->new_imgid = newid;
      dt_undo_record(darktable.undo, NULL, DT_UNDO_DUPLICATE, dupundo, _pop_undo, NULL);
    }

    // make sure that the duplicate doesn't have some magic darktable| tags
    if(dt_tag_detach_by_string("darktable|changed", newid, FALSE, FALSE)
       || dt_tag_detach_by_string("darktable|exported", newid, FALSE, FALSE))
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);

    /* unset change timestamp */
    dt_image_cache_unset_change_timestamp(darktable.image_cache, newid);

    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    const int grpid = img->group_id;
    dt_image_cache_read_release(darktable.image_cache, img);
    if(darktable.gui && darktable.gui->grouping)
    {
      darktable.gui->expanded_group_id = grpid;
    }
    dt_grouping_add_to_group(grpid, newid);

    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
  }
  return newid;
}

int32_t dt_image_duplicate_with_version(const int32_t imgid, const int32_t newversion)
{
  return _image_duplicate_with_version(imgid, newversion, TRUE);
}

void dt_image_remove(const int32_t imgid)
{
  // if a local copy exists, remove it

  if(dt_image_local_copy_reset(imgid)) return;

  sqlite3_stmt *stmt;
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  const int old_group_id = img->group_id;
  dt_image_cache_read_release(darktable.image_cache, img);

  // make sure we remove from the cache first, or else the cache will look for imgid in sql
  dt_image_cache_remove(darktable.image_cache, imgid);

  const int new_group_id = dt_grouping_remove_from_group(imgid);
  if(darktable.gui && darktable.gui->expanded_group_id == old_group_id)
    darktable.gui->expanded_group_id = new_group_id;

  // due to foreign keys added in db version 33,
  // all entries from tables having references to the images are deleted as well
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.images WHERE id = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // also clear all thumbnails in mipmap_cache.
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
}

gboolean dt_image_altered(const int32_t imgid)
{
  dt_history_hash_t status = dt_history_hash_get_status(imgid);
  return status & DT_HISTORY_HASH_CURRENT;
}

gboolean dt_image_basic(const int32_t imgid)
{
  dt_history_hash_t status = dt_history_hash_get_status(imgid);
  return status & DT_HISTORY_HASH_BASIC;
}

#ifndef _WIN32
static int _valid_glob_match(const char *const name, size_t offset)
{
  // verify that the name matched by glob() is a valid sidecar name by checking whether we have an underscore
  // followed by a sequence of digits followed by a period at the given offset in the name
  if(strlen(name) < offset || name[offset] != '_')
    return FALSE;
  size_t i;
  for(i = offset+1; name[i] && name[i] != '.'; i++)
  {
    if(!isdigit(name[i]))
      return FALSE;
  }
  return name[i] == '.';
}
#endif /* !_WIN32 */

GList* dt_image_find_duplicates(const char* filename)
{
  // find all duplicates of an image by looking for all possible sidecars for the file: file.ext.xmp, file_NN.ext.xmp,
  //   file_NNN.ext.xmp, and file_NNNN.ext.xmp
  // because a glob() needs to scan the entire directory, we minimize work for large directories by doing a single
  //   glob which might generate false matches (if the image name contains an underscore followed by a digit) and
  //   filter out the false positives afterward
#ifndef _WIN32
  // start by locating the extension, which we'll be referencing multiple times
  const size_t fn_len = strlen(filename);
  const char* ext = strrchr(filename,'.');  // find last dot
  if(!ext) ext = filename;
  const size_t ext_offset = ext - filename;

  gchar pattern[PATH_MAX] = { 0 };
  GList* files = NULL;

  // check for file.ext.xmp
  static const char xmp[] = ".xmp";
  const size_t xmp_len = strlen(xmp);
  // concatenate filename and sidecar extension
  g_strlcpy(pattern,  filename, sizeof(pattern));
  g_strlcpy(pattern + fn_len, xmp, sizeof(pattern) - fn_len);
  if(dt_util_test_image_file(pattern))
  {
    // the default sidecar exists, is readable and is a regular file with lenght > 0, so add it to the list
    files = g_list_prepend(files, g_strdup(pattern));
  }

  // now collect all file_N*N.ext.xmp matches
  static const char glob_pattern[] = "_[0-9]*[0-9]";
  const size_t gp_len = strlen(glob_pattern);
  if(fn_len + gp_len + xmp_len < sizeof(pattern)) // enough space to build pattern?
  {
    // add GLOB.ext.xmp to the root of the basename
    g_strlcpy(pattern + ext_offset, glob_pattern, sizeof(pattern) - fn_len);
    g_strlcpy(pattern + ext_offset + gp_len, ext, sizeof(pattern) - ext_offset - gp_len);
    g_strlcpy(pattern + fn_len + gp_len, xmp, sizeof(pattern) - fn_len - gp_len);
    glob_t globbuf;
    if(!glob(pattern, 0, NULL, &globbuf))
    {
      // for each match of the pattern
      for(size_t i = 0; i < globbuf.gl_pathc; i++)
      {
        if(_valid_glob_match(globbuf.gl_pathv[i], ext_offset))
        {
          // it's not a false positive, so add it to the list of sidecars
          files = g_list_prepend(files, g_strdup(globbuf.gl_pathv[i]));
        }
      }
      globfree(&globbuf);
    }
  }
  // we built the list in reverse order for speed, so un-reverse it
  return g_list_reverse(files);

#else
  return win_image_find_duplicates(filename);
#endif
}

// Search for duplicate's sidecar files and import them if found and not in DB yet
static int _image_read_duplicates(const uint32_t id, const char *filename, const gboolean clear_selection)
{
  int count_xmps_processed = 0;
  gchar pattern[PATH_MAX] = { 0 };

  GList *files = dt_image_find_duplicates(filename);

  // we store the xmp filename without version part in pattern to speed up string comparison later
  g_snprintf(pattern, sizeof(pattern), "%s.xmp", filename);

  for(GList *file_iter = files; file_iter; file_iter = g_list_next(file_iter))
  {
    gchar *xmpfilename = file_iter->data;
    int version = -1;

    // we need to get the version number of the sidecar filename
    if(!strncmp(xmpfilename, pattern, sizeof(pattern)))
    {
      // this is an xmp file without version number which corresponds to version 0
      version = 0;
    }
    else
    {
      // we need to derive the version number from the filename

      gchar *c3 = xmpfilename + strlen(xmpfilename)
        - 5; // skip over .xmp extension; position c3 at character before the '.'
      while(*c3 != '.' && c3 > xmpfilename)
        c3--; // skip over filename extension; position c3 is at character '.'
      gchar *c4 = c3;
      while(*c4 != '_' && c4 > xmpfilename) c4--; // move to beginning of version number
      c4++;

      gchar *idfield = g_strndup(c4, c3 - c4);

      version = atoi(idfield);
      g_free(idfield);
    }

    int newid = id;
    int grpid = -1;

    if(count_xmps_processed == 0)
    {
      // this is the first xmp processed, just update the passed-in id
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2
        (dt_database_get(darktable.db),
         "UPDATE main.images SET version=?1, max_version = ?1 WHERE id = ?2", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, version);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, id);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
    else
    {
      // create a new duplicate based on the passed-in id. Note that we do not call
      // dt_image_duplicate_with_version() as this version also set the group which
      // is using DT_IMAGE_CACHE_SAFE and so will write the .XMP. But we must avoid
      // this has the xmp for the duplicate is read just below.
      newid = _image_duplicate_with_version_ext(id, version);
      const dt_image_t *img = dt_image_cache_get(darktable.image_cache, id, 'r');
      grpid = img->group_id;
      dt_image_cache_read_release(darktable.image_cache, img);
    }
    // make sure newid is not selected
    if(clear_selection) dt_selection_clear(darktable.selection);

    dt_image_t *img = dt_image_cache_get(darktable.image_cache, newid, 'w');
    (void)dt_exif_xmp_read(img, xmpfilename, 0);
    img->version = version;
    dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);

    if(grpid != -1)
    {
      // now it is safe to set the duplicate group-id
      dt_grouping_add_to_group(grpid, newid);
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
    }

    count_xmps_processed++;
  }

  g_list_free_full(files, g_free);
  return count_xmps_processed;
}

static uint32_t _image_import_internal(const int32_t film_id, const char *filename, gboolean override_ignore_jpegs,
                                       gboolean lua_locking, gboolean raise_signals)
{
  const dt_imageio_write_xmp_t xmp_mode = dt_image_get_xmp_mode();
  char *normalized_filename = dt_util_normalize_path(filename);
  if(!normalized_filename || !dt_util_test_image_file(normalized_filename))
  {
    g_free(normalized_filename);
    return 0;
  }
  const char *cc = normalized_filename + strlen(normalized_filename);
  for(; *cc != '.' && cc > normalized_filename; cc--)
    ;
  if(!strcasecmp(cc, ".dt") || !strcasecmp(cc, ".dttags") || !strcasecmp(cc, ".xmp"))
  {
    g_free(normalized_filename);
    return 0;
  }
  char *ext = g_ascii_strdown(cc + 1, -1);
  if(override_ignore_jpegs == FALSE && (!strcmp(ext, "jpg") || !strcmp(ext, "jpeg"))
     && dt_conf_get_bool("ui_last/import_ignore_jpegs"))
  {
    g_free(normalized_filename);
    g_free(ext);
    return 0;
  }
  int supported = 0;
  for(const char **i = dt_supported_extensions; *i != NULL; i++)
    if(!strcmp(ext, *i))
    {
      supported = 1;
      break;
    }
  if(!supported)
  {
    g_free(normalized_filename);
    g_free(ext);
    return 0;
  }
  int rc;
  sqlite3_stmt *stmt;
  // select from images; if found => return
  gchar *imgfname = g_path_get_basename(normalized_filename);
  int32_t id = dt_image_get_id(film_id, imgfname);
  if(id >= 0)
  {
    g_free(imgfname);
    dt_image_t *img = dt_image_cache_get(darktable.image_cache, id, 'w');
    img->flags &= ~DT_IMAGE_REMOVE;
    dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
    _image_read_duplicates(id, normalized_filename, raise_signals);
    dt_image_synch_all_xmp(normalized_filename);
    g_free(ext);
    g_free(normalized_filename);
    if(raise_signals)
    {
      GList *imgs = g_list_prepend(NULL, GINT_TO_POINTER(id));
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, imgs, 0);
    }
    return id;
  }

  // also need to set the no-legacy bit, to make sure we get the right presets (new ones)
  uint32_t flags = dt_conf_get_int("ui_last/import_initial_rating");
  flags |= DT_IMAGE_NO_LEGACY_PRESETS;
  // and we set the type of image flag (from extension for now)
  gchar *extension = g_strrstr(imgfname, ".");
  flags |= dt_imageio_get_type_from_extension(extension);
  // set the bits in flags that indicate if any of the extra files (.txt, .wav) are present
  char *extra_file = dt_image_get_audio_path_from_path(normalized_filename);
  if(extra_file)
  {
    flags |= DT_IMAGE_HAS_WAV;
    g_free(extra_file);
  }
  extra_file = dt_image_get_text_path_from_path(normalized_filename);
  if(extra_file)
  {
    flags |= DT_IMAGE_HAS_TXT;
    g_free(extra_file);
  }

  //insert a v0 record (which may be updated later if no v0 xmp exists)
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "INSERT INTO main.images (id, film_id, filename, license, sha1sum, flags, version, "
     "                         max_version, history_end, position, import_timestamp)"
     " SELECT NULL, ?1, ?2, '', '', ?3, 0, 0, 0, (IFNULL(MAX(position),0) & 0xFFFFFFFF00000000)  + (1 << 32), ?4 "
     " FROM images",
     -1, &stmt, NULL);
  // clang-format on

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgfname, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, flags);
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 4, dt_datetime_now_to_gtimespan());

  rc = sqlite3_step(stmt);
  if(rc != SQLITE_DONE)
    dt_print(DT_DEBUG_ALWAYS, "[image_import_internal] sqlite3 error %d in `%s`\n", rc, filename);
  sqlite3_finalize(stmt);

  id = dt_image_get_id(film_id, imgfname);

  // Try to find out if this should be grouped already.
  gchar *basename = g_strdup(imgfname);
  gchar *cc2 = basename + strlen(basename);
  for(; *cc2 != '.' && cc2 > basename; cc2--)
    ;
  *cc2 = '\0';
  gchar *sql_pattern = g_strconcat(basename, ".%", NULL);
  int group_id;
  // in case we are not a jpg check if we need to change group representative
  if(strcmp(ext, "jpg") != 0 && strcmp(ext, "jpeg") != 0)
  {
    sqlite3_stmt *stmt2;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "SELECT group_id"
       " FROM main.images"
       " WHERE film_id = ?1 AND filename LIKE ?2 AND id = group_id", -1, &stmt2,
      NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, film_id);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 2, sql_pattern, -1, SQLITE_TRANSIENT);
    // if we have a group already
    if(sqlite3_step(stmt2) == SQLITE_ROW)
    {
      int other_id = sqlite3_column_int(stmt2, 0);
      dt_image_t *other_img = dt_image_cache_get(darktable.image_cache, other_id, 'w');
      gchar *other_basename = g_strdup(other_img->filename);
      gchar *cc3 = other_basename + strlen(other_img->filename);
      for(; *cc3 != '.' && cc3 > other_basename; cc3--)
        ;
      ++cc3;
      gchar *ext_lowercase = g_ascii_strdown(cc3, -1);
      // if the group representative is a jpg, change group representative to this new imported image
      if(!strcmp(ext_lowercase, "jpg") || !strcmp(ext_lowercase, "jpeg"))
      {
        other_img->group_id = id;
        dt_image_cache_write_release(darktable.image_cache, other_img, DT_IMAGE_CACHE_SAFE);
        sqlite3_stmt *stmt3;
        DT_DEBUG_SQLITE3_PREPARE_V2
          (dt_database_get(darktable.db),
           "SELECT id FROM main.images WHERE group_id = ?1 AND id != ?1", -1, &stmt3, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt3, 1, other_id);
        while(sqlite3_step(stmt3) == SQLITE_ROW)
        {
          other_id = sqlite3_column_int(stmt3, 0);
          dt_image_t *group_img = dt_image_cache_get(darktable.image_cache, other_id, 'w');
          group_img->group_id = id;
          dt_image_cache_write_release(darktable.image_cache, group_img, DT_IMAGE_CACHE_SAFE);
        }
        group_id = id;
        sqlite3_finalize(stmt3);
      }
      else
      {
        dt_image_cache_write_release(darktable.image_cache, other_img, DT_IMAGE_CACHE_RELAXED);
        group_id = other_id;
      }
      g_free(ext_lowercase);
      g_free(other_basename);
    }
    else
    {
      group_id = id;
    }
    sqlite3_finalize(stmt2);
  }
  else
  {
    sqlite3_stmt *stmt2;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "SELECT group_id"
       " FROM main.images"
       " WHERE film_id = ?1 AND filename LIKE ?2 AND id != ?3", -1, &stmt2, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, film_id);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 2, sql_pattern, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 3, id);
    if(sqlite3_step(stmt2) == SQLITE_ROW)
      group_id = sqlite3_column_int(stmt2, 0);
    else
      group_id = id;
    sqlite3_finalize(stmt2);
  }
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "UPDATE main.images SET group_id = ?1 WHERE id = ?2",
     -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, group_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // printf("[image_import] importing `%s' to img id %d\n", imgfname, id);

  // lock as shortly as possible:
  dt_image_t *img = dt_image_cache_get(darktable.image_cache, id, 'w');
  img->group_id = group_id;

  // read dttags and exif for database queries!
  if(dt_exif_read(img, normalized_filename)) img->exif_inited = 0;
  char dtfilename[PATH_MAX] = { 0 };
  g_strlcpy(dtfilename, normalized_filename, sizeof(dtfilename));
  // dt_image_path_append_version(id, dtfilename, sizeof(dtfilename));
  g_strlcat(dtfilename, ".xmp", sizeof(dtfilename));

  const int res = dt_exif_xmp_read(img, dtfilename, 0);

  // write through to db, but not to xmp.
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);

  // read all sidecar files
  const int nb_xmp = _image_read_duplicates(id, normalized_filename, raise_signals);

  if((res != 0) && (nb_xmp == 0))
  {
    // Search for Lightroom sidecar file, import tags if found
    const gboolean lr_xmp = dt_lightroom_import(id, NULL, TRUE);
    // Make sure that lightroom xmp data (label in particular) are saved in dt xmp
    if(lr_xmp)
      dt_image_write_sidecar_file(id);
  }

  // add a tag with the file extension
  guint tagid = 0;
  char tagname[512];
  snprintf(tagname, sizeof(tagname), "darktable|format|%s", ext);
  g_free(ext);
  dt_tag_new(tagname, &tagid);
  dt_tag_attach(tagid, id, FALSE, FALSE);

  // make sure that there are no stale thumbnails left
  dt_mipmap_cache_remove(darktable.mipmap_cache, id);

  //synch database entries to xmp
  if(xmp_mode == DT_WRITE_XMP_ALWAYS)
    dt_image_synch_all_xmp(normalized_filename);

  g_free(imgfname);
  g_free(basename);
  g_free(sql_pattern);
  g_free(normalized_filename);

#ifdef USE_LUA
  //Synchronous calling of lua post-import-image events
  if(lua_locking)
    dt_lua_lock();

  lua_State *L = darktable.lua_state.state;

  luaA_push(L, dt_lua_image_t, &id);
  dt_lua_event_trigger(L, "post-import-image", 1);

  if(lua_locking)
    dt_lua_unlock();
#endif

  if(raise_signals)
  {
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_IMPORT, id);
    GList *imgs = g_list_prepend(NULL, GINT_TO_POINTER(id));
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, imgs, 0);
  }

  // the following line would look logical with new_tags_set being the return value
  // from dt_tag_new above, but this could lead to too rapid signals, being able to lock up the
  // keywords side pane when trying to use it, which can lock up the whole dt GUI ..
  // if(new_tags_set) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals,DT_SIGNAL_TAG_CHANGED);
  return id;
}

int32_t dt_image_get_id_full_path(const gchar *filename)
{
  int32_t id = -1;
  gchar *dir = g_path_get_dirname(filename);
  gchar *file = g_path_get_basename(filename);
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT images.id"
                              " FROM main.images, main.film_rolls"
                              " WHERE film_rolls.folder = ?1"
                              "       AND images.film_id = film_rolls.id"
                              "       AND images.filename = ?2",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, dir, -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, file, -1, SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW) id=sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  g_free(dir);
  g_free(file);

  return id;
}

int32_t dt_image_get_id(uint32_t film_id, const gchar *filename)
{
  int32_t id = -1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM main.images WHERE film_id = ?1 AND filename = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, filename, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW) id=sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return id;
}

uint32_t dt_image_import(const int32_t film_id, const char *filename, gboolean override_ignore_jpegs,
                         gboolean raise_signals)
{
  return _image_import_internal(film_id, filename, override_ignore_jpegs, TRUE, raise_signals);
}

uint32_t dt_image_import_lua(const int32_t film_id, const char *filename, gboolean override_ignore_jpegs)
{
  return _image_import_internal(film_id, filename, override_ignore_jpegs, FALSE, TRUE);
}

void dt_image_init(dt_image_t *img)
{
  img->width = img->height = 0;
  img->final_width = img->final_height = img->p_width = img->p_height = 0;
  img->aspect_ratio = 0.f;
  img->crop_x = img->crop_y = img->crop_width = img->crop_height = 0;
  img->orientation = ORIENTATION_NULL;

  img->import_timestamp = img->change_timestamp = img->export_timestamp = img->print_timestamp = 0;

  img->legacy_flip.legacy = 0;
  img->legacy_flip.user_flip = 0;

  img->buf_dsc.filters = 0u;
  img->buf_dsc = (dt_iop_buffer_dsc_t){.channels = 0, .datatype = TYPE_UNKNOWN };
  img->film_id = -1;
  img->group_id = -1;
  img->flags = 0;
  img->id = -1;
  img->version = -1;
  img->loader = LOADER_UNKNOWN;
  img->exif_inited = 0;
  img->camera_missing_sample = FALSE;
  dt_datetime_exif_to_img(img, "");
  memset(img->exif_maker, 0, sizeof(img->exif_maker));
  memset(img->exif_model, 0, sizeof(img->exif_model));
  memset(img->exif_lens, 0, sizeof(img->exif_lens));
  memset(img->camera_maker, 0, sizeof(img->camera_maker));
  memset(img->camera_model, 0, sizeof(img->camera_model));
  memset(img->camera_alias, 0, sizeof(img->camera_alias));
  memset(img->camera_makermodel, 0, sizeof(img->camera_makermodel));
  memset(img->camera_legacy_makermodel, 0, sizeof(img->camera_legacy_makermodel));
  memset(img->filename, 0, sizeof(img->filename));
  g_strlcpy(img->filename, "(unknown)", sizeof(img->filename));
  img->exif_crop = 1.0;
  img->exif_exposure = 0;
  img->exif_exposure_bias = NAN;
  img->exif_aperture = 0;
  img->exif_iso = 0;
  img->exif_focal_length = 0;
  img->exif_focus_distance = 0;
  img->geoloc.latitude = NAN;
  img->geoloc.longitude = NAN;
  img->geoloc.elevation = NAN;
  img->raw_black_level = 0;
  for(uint8_t i = 0; i < 4; i++) img->raw_black_level_separate[i] = 0;
  img->raw_white_point = 16384; // 2^14
  img->d65_color_matrix[0] = NAN;
  img->profile = NULL;
  img->profile_size = 0;
  img->colorspace = DT_IMAGE_COLORSPACE_NONE;
  img->fuji_rotation_pos = 0;
  img->pixel_aspect_ratio = 1.0f;
  img->wb_coeffs[0] = NAN;
  img->wb_coeffs[1] = NAN;
  img->wb_coeffs[2] = NAN;
  img->wb_coeffs[3] = NAN;
  img->usercrop[0] = img->usercrop[1] = 0;
  img->usercrop[2] = img->usercrop[3] = 1;
  img->dng_gain_maps = NULL;
  img->exif_correction_type = CORRECTION_TYPE_NONE;
  img->cache_entry = 0;

  for(int k=0; k<4; k++)
    for(int i=0; i<3; i++)
      img->adobe_XYZ_to_CAM[k][i] = NAN;
}

void dt_image_refresh_makermodel(dt_image_t *img)
{
  if(!img->camera_maker[0] || !img->camera_model[0] || !img->camera_alias[0])
  {
    // We need to use the exif values, so let's get rawspeed to munge them
    dt_imageio_lookup_makermodel(img->exif_maker, img->exif_model,
                                 img->camera_maker, sizeof(img->camera_maker),
                                 img->camera_model, sizeof(img->camera_model),
                                 img->camera_alias, sizeof(img->camera_alias));
  }

  // Now we just create a makermodel by concatenation
  g_strlcpy(img->camera_makermodel, img->camera_maker, sizeof(img->camera_makermodel));
  const int len = strlen(img->camera_maker);
  img->camera_makermodel[len] = ' ';
  g_strlcpy(img->camera_makermodel+len+1, img->camera_model, sizeof(img->camera_makermodel)-len-1);
}

int32_t dt_image_rename(const int32_t imgid, const int32_t filmid, const gchar *newname)
{
  // TODO: several places where string truncation could occur unnoticed
  int32_t result = -1;
  gchar oldimg[PATH_MAX] = { 0 };
  gchar newimg[PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, oldimg, sizeof(oldimg), &from_cache);
  gchar *newdir = NULL;

  sqlite3_stmt *film_stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT folder FROM main.film_rolls WHERE id = ?1",
                              -1, &film_stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(film_stmt, 1, filmid);
  if(sqlite3_step(film_stmt) == SQLITE_ROW) newdir = g_strdup((gchar *)sqlite3_column_text(film_stmt, 0));
  sqlite3_finalize(film_stmt);

  gchar copysrcpath[PATH_MAX] = { 0 };
  gchar copydestpath[PATH_MAX] = { 0 };
  GFile *old = NULL, *new = NULL;
  if(newdir)
  {
    old = g_file_new_for_path(oldimg);

    if(newname)
    {
      g_snprintf(newimg, sizeof(newimg), "%s%c%s", newdir, G_DIR_SEPARATOR, newname);
      new = g_file_new_for_path(newimg);
      // 'newname' represents the file's new *basename* -- it must not
      // refer to a file outside of 'newdir'.
      gchar *newBasename = g_file_get_basename(new);
      if(g_strcmp0(newname, newBasename) != 0)
      {
        g_object_unref(old);
        old = NULL;
        g_object_unref(new);
        new = NULL;
      }
      g_free(newBasename);
    }
    else
    {
      gchar *imgbname = g_path_get_basename(oldimg);
      g_snprintf(newimg, sizeof(newimg), "%s%c%s", newdir, G_DIR_SEPARATOR, imgbname);
      new = g_file_new_for_path(newimg);
      g_free(imgbname);
    }
    g_free(newdir);
  }

  if(new)
  {
    // get current local copy if any
    _image_local_copy_full_path(imgid, copysrcpath, sizeof(copysrcpath));

    // move image
    GError *moveError = NULL;
    gboolean moveStatus = g_file_move(old, new, 0, NULL, NULL, NULL, &moveError);

    if(moveStatus)
    {
      // statement for getting ids of the image to be moved and its duplicates
      sqlite3_stmt *duplicates_stmt;
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2
        (dt_database_get(darktable.db),
         "SELECT id"
         " FROM main.images"
         " WHERE filename IN (SELECT filename FROM main.images WHERE id = ?1)"
         "   AND film_id IN (SELECT film_id FROM main.images WHERE id = ?1)",
         -1, &duplicates_stmt, NULL);
      // clang-format on

      // first move xmp files of image and duplicates
      GList *dup_list = NULL;
      DT_DEBUG_SQLITE3_BIND_INT(duplicates_stmt, 1, imgid);
      while(sqlite3_step(duplicates_stmt) == SQLITE_ROW)
      {
        const int32_t id = sqlite3_column_int(duplicates_stmt, 0);
        dup_list = g_list_prepend(dup_list, GINT_TO_POINTER(id));
        gchar oldxmp[PATH_MAX] = { 0 }, newxmp[PATH_MAX] = { 0 };
        g_strlcpy(oldxmp, oldimg, sizeof(oldxmp));
        g_strlcpy(newxmp, newimg, sizeof(newxmp));
        dt_image_path_append_version(id, oldxmp, sizeof(oldxmp));
        dt_image_path_append_version(id, newxmp, sizeof(newxmp));
        g_strlcat(oldxmp, ".xmp", sizeof(oldxmp));
        g_strlcat(newxmp, ".xmp", sizeof(newxmp));

        GFile *goldxmp = g_file_new_for_path(oldxmp);
        GFile *gnewxmp = g_file_new_for_path(newxmp);

        g_file_move(goldxmp, gnewxmp, 0, NULL, NULL, NULL, NULL);

        g_object_unref(goldxmp);
        g_object_unref(gnewxmp);
      }
      sqlite3_finalize(duplicates_stmt);

      dup_list = g_list_reverse(dup_list);  // list was built in reverse order, so un-reverse it

      // then update database and cache
      // if update was performed in above loop, dt_image_path_append_version()
      // would return wrong version!
      while(dup_list)
      {
        const int id = GPOINTER_TO_INT(dup_list->data);
        dt_image_t *img = dt_image_cache_get(darktable.image_cache, id, 'w');
        img->film_id = filmid;
        if(newname) g_strlcpy(img->filename, newname, DT_MAX_FILENAME_LEN);
        // write through to db, but not to xmp
        dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
        dup_list = g_list_delete_link(dup_list, dup_list);
        // write xmp file
        dt_image_write_sidecar_file(id);
      }
      g_list_free(dup_list);

      // finally, rename local copy if any
      if(g_file_test(copysrcpath, G_FILE_TEST_EXISTS))
      {
        // get new name
        _image_local_copy_full_path(imgid, copydestpath, sizeof(copydestpath));

        GFile *cold = g_file_new_for_path(copysrcpath);
        GFile *cnew = g_file_new_for_path(copydestpath);

        g_clear_error(&moveError);
        moveStatus = g_file_move(cold, cnew, 0, NULL, NULL, NULL, &moveError);
        if(!moveStatus)
        {
          dt_print(DT_DEBUG_ALWAYS, "[dt_image_rename] error moving local copy `%s' -> `%s'\n", copysrcpath, copydestpath);

          if(g_error_matches(moveError, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
          {
            gchar *oldBasename = g_path_get_basename(copysrcpath);
            dt_control_log(_("cannot access local copy `%s'"), oldBasename);
            g_free(oldBasename);
          }
          else if(g_error_matches(moveError, G_IO_ERROR, G_IO_ERROR_EXISTS)
                  || g_error_matches(moveError, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY))
          {
            gchar *newBasename = g_path_get_basename(copydestpath);
            dt_control_log(_("cannot write local copy `%s'"), newBasename);
            g_free(newBasename);
          }
          else
          {
            gchar *oldBasename = g_path_get_basename(copysrcpath);
            gchar *newBasename = g_path_get_basename(copydestpath);
            dt_control_log(_("error moving local copy `%s' -> `%s'"), oldBasename, newBasename);
            g_free(oldBasename);
            g_free(newBasename);
          }
        }

        g_object_unref(cold);
        g_object_unref(cnew);
      }

      result = 0;
    }
    else
    {
      if(g_error_matches(moveError, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
      {
        dt_control_log(_("error moving `%s': file not found"), oldimg);
      }
      // only display error message if newname is set (renaming and
      // not moving) as when moving it can be the case where a
      // duplicate is being moved, so only the .xmp are present but
      // the original file may already have been moved.
      else if(newname
              && (g_error_matches(moveError, G_IO_ERROR, G_IO_ERROR_EXISTS)
                  || g_error_matches(moveError, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY)))
      {
        dt_control_log(_("error moving `%s' -> `%s': file exists"), oldimg, newimg);
      }
      else if(newname)
      {
        dt_control_log(_("error moving `%s' -> `%s'"), oldimg, newimg);
      }
    }

    g_clear_error(&moveError);
    g_object_unref(old);
    g_object_unref(new);
  }

  return result;
}

int32_t dt_image_move(const int32_t imgid, const int32_t filmid)
{
  return dt_image_rename(imgid, filmid, NULL);
}

int32_t dt_image_copy_rename(const int32_t imgid, const int32_t filmid, const gchar *newname)
{
  int32_t newid = -1;
  sqlite3_stmt *stmt;
  gchar srcpath[PATH_MAX] = { 0 };
  gchar *newdir = NULL;
  gchar *filename = NULL;
  gboolean from_cache = FALSE;
  gchar *oldFilename = NULL;
  gchar *newFilename = NULL;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT folder FROM main.film_rolls WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
  if(sqlite3_step(stmt) == SQLITE_ROW) newdir = g_strdup((gchar *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);

  GFile *src = NULL, *dest = NULL;
  if(newdir)
  {
    dt_image_full_path(imgid, srcpath, sizeof(srcpath), &from_cache);
    oldFilename = g_path_get_basename(srcpath);
    gchar *destpath;
    if(newname)
    {
      newFilename = g_strdup(newname);
      destpath = g_build_filename(newdir, newname, NULL);
      dest = g_file_new_for_path(destpath);
      // 'newname' represents the file's new *basename* -- it must not
      // refer to a file outside of 'newdir'.
      gchar *destBasename = g_file_get_basename(dest);
      if(g_strcmp0(newname, destBasename) != 0)
      {
        g_object_unref(dest);
        dest = NULL;
      }
      g_free(destBasename);
    }
    else
    {
      newFilename = g_path_get_basename(srcpath);
      destpath = g_build_filename(newdir, newFilename, NULL);
      dest = g_file_new_for_path(destpath);
    }
    if(dest)
    {
      src = g_file_new_for_path(srcpath);
    }
    g_free(newdir);
    newdir = NULL;
    g_free(destpath);
    destpath = NULL;
  }

  if(dest)
  {
    // copy image to new folder
    // if image file already exists, continue
    GError *gerror = NULL;
    gboolean copyStatus = g_file_copy(src, dest, G_FILE_COPY_NONE, NULL, NULL, NULL, &gerror);

    if(copyStatus || g_error_matches(gerror, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      const int64_t new_image_position = create_next_image_position();

      // update database
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2
        (dt_database_get(darktable.db),
         "INSERT INTO main.images"
         "  (id, group_id, film_id, width, height, filename, maker, model, lens, exposure,"
         "   aperture, iso, focal_length, focus_distance, datetime_taken, flags,"
         "   output_width, output_height, crop, raw_parameters, raw_denoise_threshold,"
         "   raw_auto_bright_threshold, raw_black, raw_maximum,"
         "   license, sha1sum, orientation, histogram, lightmap,"
         "   longitude, latitude, altitude, color_matrix, colorspace, version, max_version,"
         "   position, aspect_ratio, exposure_bias)"
         " SELECT NULL, group_id, ?1 as film_id, width, height, ?2 as filename, maker, model, lens,"
         "        exposure, aperture, iso, focal_length, focus_distance, datetime_taken,"
         "        flags, width, height, crop, raw_parameters, raw_denoise_threshold,"
         "        raw_auto_bright_threshold, raw_black, raw_maximum,"
         "        license, sha1sum, orientation, histogram, lightmap,"
         "        longitude, latitude, altitude, color_matrix, colorspace, -1, -1,"
         "        ?3, aspect_ratio, exposure_bias"
         " FROM main.images"
         " WHERE id = ?4",
        -1, &stmt, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, newFilename, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT64(stmt, 3, new_image_position);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, imgid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2
        (dt_database_get(darktable.db),
         "SELECT a.id, a.filename"
         " FROM main.images AS a"
         " JOIN main.images AS b"
         "   WHERE a.film_id = ?1 AND a.filename = ?2 AND b.filename = ?3 AND b.id = ?4"
         "   ORDER BY a.id DESC",
         -1, &stmt, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, newFilename, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, oldFilename, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, imgid);

      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        newid = sqlite3_column_int(stmt, 0);
        filename = g_strdup((gchar *)sqlite3_column_text(stmt, 1));
      }
      sqlite3_finalize(stmt);

      if(newid != -1)
      {
        // also copy over on-disk thumbnails, if any
        dt_mipmap_cache_copy_thumbnails(darktable.mipmap_cache, newid, imgid);
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "INSERT INTO main.color_labels (imgid, color)"
                                    " SELECT ?1, color"
                                    " FROM main.color_labels"
                                    " WHERE imgid = ?2",
                                    -1, &stmt, NULL);
        // clang-format on
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "INSERT INTO main.meta_data (id, key, value)"
                                    " SELECT ?1, key, value"
                                    " FROM main.meta_data"
                                    " WHERE id = ?2",
                                    -1, &stmt, NULL);
        // clang-format on
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
#ifdef HAVE_SQLITE_324_OR_NEWER
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "INSERT INTO main.tagged_images (imgid, tagid, position)"
                                    " SELECT ?1, tagid, "
                                    "        (SELECT (IFNULL(MAX(position),0) & 0xFFFFFFFF00000000)"
                                    "         FROM main.tagged_images)"
                                    "         + (ROW_NUMBER() OVER (ORDER BY imgid) << 32)"
                                    " FROM main.tagged_images AS ti"
                                    " WHERE imgid = ?2",
                                    -1, &stmt, NULL);
        // clang-format on
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
#else   // break down the tagged_images insert per tag
        GList *tags = dt_tag_get_tags(imgid, FALSE);
        for(GList *tag = tags; tag; tag = g_list_next(tag))
        {
          // clang-format off
          DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                      "INSERT INTO main.tagged_images (imgid, tagid, position)"
                                      "  VALUES (?1, ?2, "
                                      "   (SELECT (IFNULL(MAX(position),0) & 0xFFFFFFFF00000000)"
                                      "     + (1 << 32)"
                                      "   FROM main.tagged_images))",
                                      -1, &stmt, NULL);
          // clang-format on
          DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
          DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, GPOINTER_TO_INT(tag->data));
          sqlite3_step(stmt);
          sqlite3_finalize(stmt);
        }
        g_list_free(tags);
#endif
        // get max_version of image duplicates in destination filmroll
        int32_t max_version = -1;
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2
          (dt_database_get(darktable.db),
           "SELECT MAX(a.max_version)"
           " FROM main.images AS a"
           " JOIN main.images AS b"
           "   WHERE a.film_id = b.film_id AND a.filename = b.filename AND b.id = ?1",
           -1, &stmt, NULL);
        // clang-format on
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);

        if(sqlite3_step(stmt) == SQLITE_ROW) max_version = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);

        // set version of new entry and max_version of all involved duplicates (with same film_id and
        // filename)
        max_version = (max_version >= 0) ? max_version + 1 : 0;
        int32_t version = max_version;

        DT_DEBUG_SQLITE3_PREPARE_V2
          (dt_database_get(darktable.db),
           "UPDATE main.images SET version=?1 WHERE id = ?2", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, version);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, newid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        DT_DEBUG_SQLITE3_PREPARE_V2
          (dt_database_get(darktable.db),
           "UPDATE main.images SET max_version=?1 WHERE film_id = ?2 AND filename = ?3",
           -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, max_version);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, filmid);
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, filename, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        // image group handling follows
        // get group_id of potential image duplicates in destination filmroll
        int32_t new_group_id = -1;
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2
          (dt_database_get(darktable.db),
           "SELECT DISTINCT a.group_id"
           " FROM main.images AS a"
           " JOIN main.images AS b"
           "   WHERE a.film_id = b.film_id AND a.filename = b.filename"
           "     AND b.id = ?1 AND a.id != ?1",
           -1, &stmt, NULL);
        // clang-format on
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);

        if(sqlite3_step(stmt) == SQLITE_ROW) new_group_id = sqlite3_column_int(stmt, 0);

        // then check if there are further duplicates belonging to different group(s)
        if(sqlite3_step(stmt) == SQLITE_ROW) new_group_id = -1;
        sqlite3_finalize(stmt);

        // rationale:
        // if no group exists or if the image duplicates belong to multiple groups, then the
        // new image builds a group of its own, else it is added to the (one) existing group
        if(new_group_id == -1) new_group_id = newid;

        // make copied image belong to a group
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "UPDATE main.images SET group_id=?1 WHERE id = ?2", -1, &stmt, NULL);

        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, new_group_id);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, newid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        dt_history_copy_and_paste_on_image(imgid, newid, FALSE, NULL, TRUE, TRUE);

        // write xmp file
        dt_image_write_sidecar_file(newid);

        dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                                   NULL);
      }

      g_free(filename);
    }
    else
    {
      dt_print(DT_DEBUG_ALWAYS, "[dt_image_copy_rename] Failed to copy image %s: %s\n", srcpath, gerror->message);
    }
    g_object_unref(dest);
    g_object_unref(src);
    g_clear_error(&gerror);
  }
  g_free(oldFilename);
  g_free(newFilename);

  return newid;
}

int32_t dt_image_copy(const int32_t imgid, const int32_t filmid)
{
  return dt_image_copy_rename(imgid, filmid, NULL);
}

int dt_image_local_copy_set(const int32_t imgid)
{
  gchar srcpath[PATH_MAX] = { 0 };
  gchar destpath[PATH_MAX] = { 0 };

  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, srcpath, sizeof(srcpath), &from_cache);

  _image_local_copy_full_path(imgid, destpath, sizeof(destpath));

  // check that the src file is readable
  if(!g_file_test(srcpath, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("cannot create local copy when the original file is not accessible."));
    return 1;
  }

  if(!g_file_test(destpath, G_FILE_TEST_EXISTS))
  {
    GFile *src = g_file_new_for_path(srcpath);
    GFile *dest = g_file_new_for_path(destpath);

    // copy image to cache directory
    GError *gerror = NULL;

    if(!g_file_copy(src, dest, G_FILE_COPY_NONE, NULL, NULL, NULL, &gerror))
    {
      dt_control_log(_("cannot create local copy."));
      g_object_unref(dest);
      g_object_unref(src);
      return 1;
    }

    g_object_unref(dest);
    g_object_unref(src);
  }

  // update cache local copy flags, do this even if the local copy already exists as we need to set the flags
  // for duplicate
  dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  img->flags |= DT_IMAGE_LOCAL_COPY;
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);

  dt_control_queue_redraw_center();
  return 0;
}

static int _nb_other_local_copy_for(const int32_t imgid)
{
  sqlite3_stmt *stmt;
  int result = 1;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*)"
                              " FROM main.images"
                              " WHERE id!=?1 AND flags&?2=?2"
                              "   AND film_id=(SELECT film_id"
                              "                FROM main.images"
                              "                WHERE id=?1)"
                              "   AND filename=(SELECT filename"
                              "                 FROM main.images"
                              "                 WHERE id=?1);",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_IMAGE_LOCAL_COPY);
  if(sqlite3_step(stmt) == SQLITE_ROW) result = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  return result;
}

int dt_image_local_copy_reset(const int32_t imgid)
{
  gchar destpath[PATH_MAX] = { 0 };
  gchar locppath[PATH_MAX] = { 0 };
  gchar cachedir[PATH_MAX] = { 0 };

  // check that a local copy exists, otherwise there is nothing to do
  dt_image_t *imgr = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  const gboolean local_copy_exists = (imgr->flags & DT_IMAGE_LOCAL_COPY) == DT_IMAGE_LOCAL_COPY ? TRUE : FALSE;
  dt_image_cache_read_release(darktable.image_cache, imgr);

  if(!local_copy_exists)
    return 0;

  // check that the original file is accessible

  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, destpath, sizeof(destpath), &from_cache);

  from_cache = TRUE;
  dt_image_full_path(imgid, locppath, sizeof(locppath), &from_cache);
  dt_image_path_append_version(imgid, locppath, sizeof(locppath));
  g_strlcat(locppath, ".xmp", sizeof(locppath));

  // a local copy exists, but the original is not accessible

  if(g_file_test(locppath, G_FILE_TEST_EXISTS) && !g_file_test(destpath, G_FILE_TEST_EXISTS))
  {
    dt_control_log(_("cannot remove local copy when the original file is not accessible."));
    return 1;
  }

  // get name of local copy

  _image_local_copy_full_path(imgid, locppath, sizeof(locppath));

  // remove cached file, but double check that this is really into the cache. We really want to avoid deleting
  // a user's original file.

  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  if(g_file_test(locppath, G_FILE_TEST_EXISTS) && strstr(locppath, cachedir))
  {
    GFile *dest = g_file_new_for_path(locppath);

    // first sync the xmp with the original picture

    dt_image_write_sidecar_file(imgid);

    // delete image from cache directory only if there is no other local cache image referencing it
    // for example duplicates are all referencing the same base picture.

    if(_nb_other_local_copy_for(imgid) == 0) g_file_delete(dest, NULL, NULL);

    g_object_unref(dest);

    // delete xmp if any
    dt_image_path_append_version(imgid, locppath, sizeof(locppath));
    g_strlcat(locppath, ".xmp", sizeof(locppath));
    dest = g_file_new_for_path(locppath);

    if(g_file_test(locppath, G_FILE_TEST_EXISTS)) g_file_delete(dest, NULL, NULL);
    g_object_unref(dest);
  }

  // update cache, remove local copy flags, this is done in all cases here as when we
  // reach this point the local-copy flag is present and the file has been either removed
  // or is not present.

  dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  img->flags &= ~DT_IMAGE_LOCAL_COPY;
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);

  dt_control_queue_redraw_center();

  return 0;
}

// *******************************************************
// xmp stuff
// *******************************************************

int dt_image_write_sidecar_file(const int32_t imgid)
{
  // TODO: compute hash and don't write if not needed!
  // write .xmp file
  if((imgid > 0) && (dt_image_get_xmp_mode() != DT_WRITE_XMP_NEVER))
  {
    char filename[PATH_MAX] = { 0 };

    // FIRST: check if the original file is present
    gboolean from_cache = FALSE;
    dt_image_full_path(imgid, filename, sizeof(filename), &from_cache);

    if(!g_file_test(filename, G_FILE_TEST_EXISTS))
    {
      // OTHERWISE: check if the local copy exists
      from_cache = TRUE;
      dt_image_full_path(imgid, filename, sizeof(filename), &from_cache);

      //  nothing to do, the original is not accessible and there is no local copy
      if(!from_cache) return 1;
    }

    dt_image_path_append_version(imgid, filename, sizeof(filename));
    g_strlcat(filename, ".xmp", sizeof(filename));

    if(!dt_exif_xmp_write(imgid, filename))
    {
      // put the timestamp into db. this can't be done in exif.cc since that code gets called
      // for the copy exporter, too
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2
        (dt_database_get(darktable.db),
         "UPDATE main.images SET write_timestamp = STRFTIME('%s', 'now') WHERE id = ?1",
         -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      return 0;
    }
  }

  return 1; // error : nothing written
}

void dt_image_synch_xmps(const GList *img)
{
  if(!img) return;
  if(dt_image_get_xmp_mode() != DT_WRITE_XMP_NEVER)
  {
    for(const GList *imgs = img; imgs; imgs = g_list_next(imgs))
    {
      dt_image_write_sidecar_file(GPOINTER_TO_INT(imgs->data));
    }
  }
}

void dt_image_synch_xmp(const int selected)
{
  if(selected > 0)
  {
    dt_image_write_sidecar_file(selected);
  }
  else
  {
    GList *imgs = dt_act_on_get_images(FALSE, TRUE, FALSE);
    dt_image_synch_xmps(imgs);
    g_list_free(imgs);
  }
}

void dt_image_synch_all_xmp(const gchar *pathname)
{
  if(dt_image_get_xmp_mode() != DT_WRITE_XMP_NEVER)
  {
    const int imgid = dt_image_get_id_full_path(pathname);
    if(imgid != -1)
    {
      dt_image_write_sidecar_file(imgid);
    }
  }
}

void dt_image_local_copy_synch(void)
{
  // nothing to do if not creating .xmp
  if(dt_image_get_xmp_mode() == DT_WRITE_XMP_NEVER) return;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM main.images WHERE flags&?1=?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, DT_IMAGE_LOCAL_COPY);

  int count = 0;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int32_t imgid = sqlite3_column_int(stmt, 0);
    gboolean from_cache = FALSE;
    char filename[PATH_MAX] = { 0 };
    dt_image_full_path(imgid, filename, sizeof(filename), &from_cache);

    if(g_file_test(filename, G_FILE_TEST_EXISTS))
    {
      dt_image_write_sidecar_file(imgid);
      count++;
    }
  }
  sqlite3_finalize(stmt);

  if(count > 0)
  {
    dt_control_log(ngettext("%d local copy has been synchronized",
                            "%d local copies have been synchronized", count),
                   count);
  }
}

void dt_image_get_datetime(const int32_t imgid, char *datetime)
{
  if(!datetime) return;
  datetime[0] = '\0';
  const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(!cimg) return;
  dt_datetime_img_to_exif(datetime, DT_DATETIME_LENGTH, cimg);
  dt_image_cache_read_release(darktable.image_cache, cimg);
}

static void _datetime_undo_data_free(gpointer data)
{
  GList *l = (GList *)data;
  g_list_free_full(l, g_free);
}

typedef struct _datetime_t
{
  char dt[DT_DATETIME_LENGTH];
} _datetime_t;

static void _image_set_datetimes(const GList *img, const GArray *dtime,
                                 GList **undo, const gboolean undo_on)
{
  int i = 0;
  for(GList *imgs = (GList *)img; imgs; imgs = g_list_next(imgs))
  {
    const int32_t imgid = GPOINTER_TO_INT(imgs->data);
    // if char *datetime, the returned pointer is not correct => use of _datetime_t
    const _datetime_t *datetime = &g_array_index(dtime, _datetime_t, i);
    if(undo_on)
    {
      dt_undo_datetime_t *undodatetime = (dt_undo_datetime_t *)malloc(sizeof(dt_undo_datetime_t));
      undodatetime->imgid = imgid;
      dt_image_get_datetime(imgid, undodatetime->before);

      memcpy(&undodatetime->after, datetime->dt, DT_DATETIME_LENGTH);

      *undo = g_list_prepend(*undo, undodatetime);
    }

    _set_datetime(imgid, datetime->dt);
    i++;
  }
}

void dt_image_set_datetimes(const GList *imgs, const GArray *dtime, const gboolean undo_on)
{
  if(!imgs || !dtime || (g_list_length((GList *)imgs) != dtime->len))
    return;
  GList *undo = NULL;
  if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_DATETIME);

  _image_set_datetimes(imgs, dtime, &undo, undo_on);

  if(undo_on)
  {
    dt_undo_record(darktable.undo, NULL, DT_UNDO_DATETIME, undo, _pop_undo, _datetime_undo_data_free);
    dt_undo_end_group(darktable.undo);
  }
}

static void _image_set_datetime(const GList *img, const char *datetime,
                                GList **undo, const gboolean undo_on)
{
  for(GList *imgs = (GList *)img; imgs;  imgs = g_list_next(imgs))
  {
    const int32_t imgid = GPOINTER_TO_INT(imgs->data);
    if(undo_on)
    {
      dt_undo_datetime_t *undodatetime = (dt_undo_datetime_t *)malloc(sizeof(dt_undo_datetime_t));
      undodatetime->imgid = imgid;
      dt_image_get_datetime(imgid, undodatetime->before);

      memcpy(&undodatetime->after, datetime, DT_DATETIME_LENGTH);

      *undo = g_list_prepend(*undo, undodatetime);
    }

    _set_datetime(imgid, datetime);
  }
}

void dt_image_set_datetime(const GList *imgs, const char *datetime, const gboolean undo_on)
{
  if(!imgs)
    return;
  GList *undo = NULL;
  if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_DATETIME);

  _image_set_datetime(imgs, datetime, &undo, undo_on);

  if(undo_on)
  {
    dt_undo_record(darktable.undo, NULL, DT_UNDO_DATETIME, undo, _pop_undo, _datetime_undo_data_free);
    dt_undo_end_group(darktable.undo);
  }
}

char *dt_image_get_audio_path_from_path(const char *image_path)
{
  size_t len = strlen(image_path);
  const char *c = image_path + len;
  while((c > image_path) && (*c != '.')) c--;
  len = c - image_path + 1;

  char *result = g_strndup(image_path, len + 3);

  result[len] = 'w';
  result[len + 1] = 'a';
  result[len + 2] = 'v';
  if(g_file_test(result, G_FILE_TEST_EXISTS)) return result;

  result[len] = 'W';
  result[len + 1] = 'A';
  result[len + 2] = 'V';
  if(g_file_test(result, G_FILE_TEST_EXISTS)) return result;

  g_free(result);
  return NULL;
}

char *dt_image_get_audio_path(const int32_t imgid)
{
  gboolean from_cache = FALSE;
  char image_path[PATH_MAX] = { 0 };
  dt_image_full_path(imgid, image_path, sizeof(image_path), &from_cache);

  return dt_image_get_audio_path_from_path(image_path);
}

char *dt_image_get_text_path_from_path(const char *image_path)
{
  size_t len = strlen(image_path);
  const char *c = image_path + len;
  while((c > image_path) && (*c != '.')) c--;
  len = c - image_path + 1;

  char *result = g_strndup(image_path, len + 3);

  result[len] = 't';
  result[len + 1] = 'x';
  result[len + 2] = 't';
  if(g_file_test(result, G_FILE_TEST_EXISTS)) return result;

  result[len] = 'T';
  result[len + 1] = 'X';
  result[len + 2] = 'T';
  if(g_file_test(result, G_FILE_TEST_EXISTS)) return result;

  g_free(result);
  return NULL;
}

char *dt_image_get_text_path(const int32_t imgid)
{
  gboolean from_cache = FALSE;
  char image_path[PATH_MAX] = { 0 };
  dt_image_full_path(imgid, image_path, sizeof(image_path), &from_cache);

  return dt_image_get_text_path_from_path(image_path);
}

float dt_image_get_exposure_bias(const struct dt_image_t *image_storage)
{
  // just check that pointers exist and are initialized
  if((image_storage) && (image_storage->exif_exposure_bias))
  {
    // sanity checks because I don't trust exif tags too much
    if(image_storage->exif_exposure_bias == NAN
       || image_storage->exif_exposure_bias != image_storage->exif_exposure_bias
       || isnan(image_storage->exif_exposure_bias)
       || CLAMP(image_storage->exif_exposure_bias, -5.0f, 5.0f) != image_storage->exif_exposure_bias)
      return 0.0f; // isnan
    else
      return CLAMP(image_storage->exif_exposure_bias, -5.0f, 5.0f);
  }
  else
    return 0.0f;
}

char *dt_image_camera_missing_sample_message(const struct dt_image_t *img, gboolean logmsg)
{
  const char *T1 = _("<b>WARNING</b>: camera is missing samples!");
  const char *T2 = _("You must provide samples in <a href='https://raw.pixls.us/'>https://raw.pixls.us/</a>");
  char *T3 = g_strdup_printf(_("for `%s' `%s'\n"
                               "in as many format/compression/bit depths as possible"),
                             img->camera_maker, img->camera_model);
  const char *T4 = _("or the <b>RAW won't be readable</b> in next version.");

  char *NL     = logmsg ? "\n\n" : "\n";
  char *PREFIX = logmsg ? "<big>" : "";
  char *SUFFIX = logmsg ? "</big>" : "";

  char *msg = g_strconcat(PREFIX, T1, NL, T2, NL, T3, NL, T4, SUFFIX, NULL);

  if(logmsg)
  {
    char *newmsg = dt_util_str_replace(msg, "<b>", "<span foreground='red'><b>");
    g_free(msg);
    msg = dt_util_str_replace(newmsg, "</b>", "</b></span>");
    g_free(newmsg);
  }

  g_free(T3);
  return msg;
}

void dt_image_check_camera_missing_sample(const struct dt_image_t *img)
{
  if(img->camera_missing_sample)
  {
    char *msg = dt_image_camera_missing_sample_message(img, TRUE);
    dt_control_log(msg, (char *)NULL);
    g_free(msg);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
