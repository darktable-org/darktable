/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/grouping.h"
#include "common/mipmap_cache.h"
#include "common/tags.h"
#include "common/history.h"
#include "common/imageio_rawspeed.h"
#include "control/control.h"
#include "control/conf.h"
#include "control/jobs.h"
#include "develop/lightroom.h"
#include <math.h>
#include <sqlite3.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <assert.h>
#ifndef __WIN32__
#include <glob.h>
#endif
#include <glib/gstdio.h>

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

int dt_image_is_raw(const dt_image_t *img)
{
  // NULL terminated list of supported non-RAW extensions
  const char *dt_non_raw_extensions[]
      = { ".jpeg", ".jpg",  ".pfm", ".hdr", ".exr", ".pxn", ".tif", ".tiff", ".png",
          ".j2c",  ".j2k",  ".jp2", ".jpc", ".gif", ".jpc", ".jp2", ".bmp",  ".dcm",
          ".jng",  ".miff", ".mng", ".pbm", ".pnm", ".ppm", ".pgm", NULL };

  if(img->flags & DT_IMAGE_RAW) return TRUE;

  const char *c = img->filename + strlen(img->filename);
  while(*c != '.' && c > img->filename) c--;

  gboolean isnonraw = FALSE;
  for(const char **i = dt_non_raw_extensions; *i != NULL; i++)
  {
    if(!g_ascii_strncasecmp(c, *i, strlen(*i)))
    {
      isnonraw = TRUE;
      break;
    }
  }

  return !isnonraw;
}

const char *dt_image_film_roll_name(const char *path)
{
  const char *folder = path + strlen(path);
  int numparts = dt_conf_get_int("show_folder_levels");
  numparts = CLAMPS(numparts, 1, 5);
  int count = 0;
  if(numparts < 1) numparts = 1;
  while(folder > path)
  {
    if(*folder == '/')
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
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select folder from film_rolls where id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->film_id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *f = (char *)sqlite3_column_text(stmt, 0);
    snprintf(pathname, pathname_len, "%s", f);
  }
  sqlite3_finalize(stmt);
  pathname[pathname_len - 1] = '\0';
}


void dt_image_film_roll(const dt_image_t *img, char *pathname, size_t pathname_len)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select folder from film_rolls where id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->film_id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *f = (char *)sqlite3_column_text(stmt, 0);
    const char *c = dt_image_film_roll_name(f);
    snprintf(pathname, pathname_len, "%s", c);
  }
  else
  {
    snprintf(pathname, pathname_len, "%s", _("orphaned image"));
  }
  sqlite3_finalize(stmt);
  pathname[pathname_len - 1] = '\0';
}

gboolean dt_image_safe_remove(const int32_t imgid)
{
  // always safe to remove if we do not have .xmp
  if(!dt_conf_get_bool("write_sidecar_files")) return TRUE;

  // check whether the original file is accessible
  char pathname[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;

  dt_image_full_path(imgid, pathname, sizeof(pathname), &from_cache);

  if(!from_cache)
    return TRUE;

  else
  {
    // finaly check if we have a .xmp for the local copy. If no modification done on the local copy it is safe
    // to remove.
    g_strlcat(pathname, ".xmp", sizeof(pathname));
    return !g_file_test(pathname, G_FILE_TEST_EXISTS);
  }
}

void dt_image_full_path(const int imgid, char *pathname, size_t pathname_len, gboolean *from_cache)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select folder || '/' || filename from images, film_rolls where "
                              "images.film_id = film_rolls.id and images.id = ?1",
                              -1, &stmt, NULL);
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

    if (g_file_test(lc_pathname, G_FILE_TEST_EXISTS))
      g_strlcpy(pathname, (char *)lc_pathname, pathname_len);
    else
      *from_cache = FALSE;
  }
}

static void _image_local_copy_full_path(const int imgid, char *pathname, size_t pathname_len)
{
  sqlite3_stmt *stmt;

  *pathname = '\0';
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT folder || '/' || filename FROM images, film_rolls "
                              "WHERE images.film_id = film_rolls.id AND images.id = ?1",
                              -1, &stmt, NULL);
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
    snprintf(c, pathname + pathname_len - c, "%s", c2);
    g_free(filename);
  }
}

void dt_image_path_append_version(int imgid, char *pathname, size_t pathname_len)
{
  // get duplicate suffix
  int version = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select version from images where id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  dt_image_path_append_version_no_db(version, pathname, pathname_len);
}

void dt_image_print_exif(const dt_image_t *img, char *line, size_t line_len)
{
  if(img->exif_exposure >= 4.0f) // Use whole seconds (e.g., 5" for exposures >= 4s)
    snprintf(line, line_len, "%.0f″ f/%.0f %dmm iso %d", img->exif_exposure, img->exif_aperture,
             (int)img->exif_focal_length, (int)img->exif_iso);
  else if(img->exif_exposure < 0.35f) // Use fractions (e.g., 1/200 all the way up to 1/3)
    snprintf(line, line_len, "1/%.0f f/%.1f %dmm iso %d", 1.0 / img->exif_exposure, img->exif_aperture,
             (int)img->exif_focal_length, (int)img->exif_iso);
  else // Use seconds and tenths (e.g., 1"2 for 1.2s exposure)
  {
    // Round first so we don't end up showing 0"10 instead of 1"0
    float exposure = roundf(img->exif_exposure*10.0f)/10.0f;
    float integral = 0.0f;
    float fractional = modff(exposure, &integral) * 10.0f;
    snprintf(line, line_len, "%.0f″%.0f f/%.1f %dmm iso %d", integral, fractional, img->exif_aperture,
             (int)img->exif_focal_length, (int)img->exif_iso);
  }
}

void dt_image_set_location(const int32_t imgid, double lon, double lat)
{
  /* fetch image from cache */
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  /* set image location */
  image->longitude = lon;
  image->latitude = lat;

  /* store */
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
}

void dt_image_set_location_and_elevation(const int32_t imgid, double lon, double lat, double ele)
{
  /* fetch image from cache */
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  /* set image location and elevation */
  image->longitude = lon;
  image->latitude = lat;
  image->elevation = ele;

  /* store */
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
}

void dt_image_set_flip(const int32_t imgid, const dt_image_orientation_t orientation)
{
  sqlite3_stmt *stmt;
  // push new orientation to sql via additional history entry:
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select IFNULL(MAX(num)+1, 0) from history where imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  const int iop_flip_MODVER = 2;
  int num = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW) num = sqlite3_column_int(stmt, 0);

  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "insert into history (imgid, num, module, operation, op_params, enabled, "
                              "blendop_params, blendop_version, multi_priority, multi_name) values"
                              " (?1, ?2, ?3, 'flip', ?4, 1, null, 0, 0, '') ",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, iop_flip_MODVER);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, &orientation, sizeof(int32_t), SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE images SET history_end = (SELECT MAX(num) + 1 FROM history WHERE imgid = ?1) WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
  // write that through to xmp:
  dt_image_write_sidecar_file(imgid);
}

dt_image_orientation_t dt_image_get_orientation(const int imgid)
{
  // find the flip module -- the pointer stays valid until darktable shuts down
  static dt_iop_module_so_t *flip = NULL;
  if(flip == NULL)
  {
    GList *modules = g_list_first(darktable.iop);
    while(modules)
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)(modules->data);
      if(!strcmp(module->op, "flip"))
      {
        flip = module;
        break;
      }
      modules = g_list_next(modules);
    }
  }

  dt_image_orientation_t orientation = ORIENTATION_NULL;

  // db lookup flip params
  if(flip && flip->get_p)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "SELECT op_params FROM history WHERE imgid=?1 AND operation='flip' ORDER BY num DESC LIMIT 1", -1,
        &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
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
}

int32_t dt_image_duplicate(const int32_t imgid)
{
  return dt_image_duplicate_with_version(imgid, -1);
}


int32_t dt_image_duplicate_with_version(const int32_t imgid, const int32_t newversion)
{
  sqlite3_stmt *stmt;
  int32_t newid = -1;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select a.id from images as a join images as b where "
                              "a.film_id = b.film_id and a.filename = b.filename and "
                              "b.id = ?1 and a.version = ?2 order by a.id desc",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, newversion);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    newid = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  // requested version is already present in DB, so we just return it
  if(newid != -1) return newid;

  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "insert into images "
      "(id, group_id, film_id, width, height, filename, maker, model, lens, exposure, "
      "aperture, iso, focal_length, focus_distance, datetime_taken, flags, "
      "output_width, output_height, crop, raw_parameters, raw_denoise_threshold, "
      "raw_auto_bright_threshold, raw_black, raw_maximum, "
      "caption, description, license, sha1sum, orientation, histogram, lightmap, "
      "longitude, latitude, altitude, color_matrix, colorspace, version, max_version, history_end) "
      "select null, group_id, film_id, width, height, filename, maker, model, lens, "
      "exposure, aperture, iso, focal_length, focus_distance, datetime_taken, "
      "flags, width, height, crop, raw_parameters, raw_denoise_threshold, "
      "raw_auto_bright_threshold, raw_black, raw_maximum, "
      "caption, description, license, sha1sum, orientation, histogram, lightmap, "
      "longitude, latitude, altitude, color_matrix, colorspace, null, null, 0 "
      "from images where id = ?1",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "select a.id, a.film_id, a.filename, b.max_version from images as a join images as b where "
      "a.film_id = b.film_id and a.filename = b.filename and "
      "b.id = ?1 order by a.id desc",
      -1, &stmt, NULL);
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
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "insert into color_labels (imgid, color) select ?1, color from "
                                "color_labels where imgid = ?2",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "insert into meta_data (id, key, value) select ?1, key, value "
                                "from meta_data where id = ?2",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "insert into tagged_images (imgid, tagid) select ?1, tagid from "
                                "tagged_images where imgid = ?2",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // make sure that the duplicate doesn't have some magic darktable| tags
    dt_tag_detach_by_string("darktable|changed", newid);
    dt_tag_detach_by_string("darktable|exported", newid);

    // set version of new entry and max_version of all involved duplicates (with same film_id and filename)
    int32_t version = (newversion != -1) ? newversion : max_version + 1;
    max_version = (newversion != -1) ? MAX(max_version, newversion) : max_version + 1;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update images set version=?1 where id = ?2",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, version);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, newid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "update images set max_version=?1 where film_id = ?2 and filename = ?3", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, max_version);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, film_id);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, filename, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    g_free(filename);

    if(darktable.gui && darktable.gui->grouping)
    {
      const dt_image_t *img = dt_image_cache_get(darktable.image_cache, newid, 'r');
      darktable.gui->expanded_group_id = img->group_id;
      dt_image_cache_read_release(darktable.image_cache, img);
    }
    dt_collection_update_query(darktable.collection);
  }
  return newid;
}

void dt_image_remove(const int32_t imgid)
{
  // if a local copy exists, remove it

  if(dt_image_local_copy_reset(imgid)) return;

  sqlite3_stmt *stmt;
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  int old_group_id = img->group_id;
  dt_image_cache_read_release(darktable.image_cache, img);

  // make sure we remove from the cache first, or else the cache will look for imgid in sql
  dt_image_cache_remove(darktable.image_cache, imgid);

  int new_group_id = dt_grouping_remove_from_group(imgid);
  if(darktable.gui && darktable.gui->expanded_group_id == old_group_id)
    darktable.gui->expanded_group_id = new_group_id;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from images where id = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from tagged_images where imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from history where imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from color_labels where imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from meta_data where id = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from selected_images where imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // also clear all thumbnails in mipmap_cache.
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
}

int dt_image_altered(const uint32_t imgid)
{
  int altered = 0;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select operation from history where imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *op = (const char *)sqlite3_column_text(stmt, 0);
    // FIXME: this is clearly a terrible way to determine which modules
    // are okay to still load the thumbnail and which aren't.
    // (that's currently the only use of this function)
    if(!op) continue; // can happen while importing or something like that
    if(!strcmp(op, "basecurve")) continue;
    if(!strcmp(op, "flip")) continue;
    if(!strcmp(op, "sharpen")) continue;
    if(!strcmp(op, "dither")) continue;
    if(!strcmp(op, "highlights")) continue;
    altered = 1;
    break;
  }
  sqlite3_finalize(stmt);

  return altered;
}


void dt_image_read_duplicates(const uint32_t id, const char *filename)
{
  // Search for duplicate's sidecar files and import them if found and not in DB yet
  gchar *imgfname = g_path_get_basename(filename);
  gchar *imgpath = g_path_get_dirname(filename);
  gchar pattern[PATH_MAX] = { 0 };

  // NULL terminated list of glob patterns; should include "" and can be extended if needed
  static const gchar *glob_patterns[]
      = { "", "_[0-9][0-9]", "_[0-9][0-9][0-9]", "_[0-9][0-9][0-9][0-9]", NULL };

  const gchar **glob_pattern = glob_patterns;
  GList *files = NULL;
  while(*glob_pattern)
  {
    snprintf(pattern, sizeof(pattern), "%s", filename);
    gchar *c1 = pattern + strlen(pattern);
    while(*c1 != '.' && c1 > pattern) c1--;
    snprintf(c1, pattern + sizeof(pattern) - c1, "%s", *glob_pattern);
    const gchar *c2 = filename + strlen(filename);
    while(*c2 != '.' && c2 > filename) c2--;
    snprintf(c1 + strlen(*glob_pattern), pattern + sizeof(pattern) - c1 - strlen(*glob_pattern), "%s.xmp", c2);

#ifdef __WIN32__
    WIN32_FIND_DATA data;
    HANDLE handle = FindFirstFile(pattern, &data);
    if(handle != INVALID_HANDLE_VALUE)
    {
      do
        files = g_list_append(files, g_strdup(data.cFileName));
      while(FindNextFile(handle, &data));
    }
#else
    glob_t globbuf;
    if(!glob(pattern, 0, NULL, &globbuf))
    {
      for(size_t i = 0; i < globbuf.gl_pathc; i++)
        files = g_list_append(files, g_strdup(globbuf.gl_pathv[i]));
      globfree(&globbuf);
    }
#endif

    glob_pattern++;
  }

  // we store the xmp filename without version part in pattern to speed up string comparison later
  g_snprintf(pattern, sizeof(pattern), "%s.xmp", filename);

  GList *file_iter = g_list_first(files);
  while(file_iter != NULL)
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
      // we need to derive the version number from the  filename

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

    int newid = dt_image_duplicate_with_version(id, version);
    dt_image_t *img = dt_image_cache_get(darktable.image_cache, newid, 'w');
    (void)dt_exif_xmp_read(img, xmpfilename, 0);
    dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);

    file_iter = g_list_next(file_iter);
  }

  g_list_free_full(files, g_free);
  g_free(imgfname);
  g_free(imgpath);
}


uint32_t dt_image_import(const int32_t film_id, const char *filename, gboolean override_ignore_jpegs)
{
  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR) || dt_util_get_file_size(filename) == 0) return 0;
  const char *cc = filename + strlen(filename);
  for(; *cc != '.' && cc > filename; cc--)
    ;
  if(!strcmp(cc, ".dt")) return 0;
  if(!strcmp(cc, ".dttags")) return 0;
  if(!strcmp(cc, ".xmp")) return 0;
  char *ext = g_ascii_strdown(cc + 1, -1);
  if(override_ignore_jpegs == FALSE && (!strcmp(ext, "jpg") || !strcmp(ext, "jpeg"))
     && dt_conf_get_bool("ui_last/import_ignore_jpegs"))
  {
    g_free(ext);
    return 0;
  }
  int supported = 0;
  char **extensions = g_strsplit(dt_supported_extensions, ",", 100);
  for(char **i = extensions; *i != NULL; i++)
    if(!strcmp(ext, *i))
    {
      supported = 1;
      break;
    }
  g_strfreev(extensions);
  if(!supported)
  {
    g_free(ext);
    return 0;
  }
  int rc;
  uint32_t id = 0;
  // select from images; if found => return
  gchar *imgfname;
  imgfname = g_path_get_basename((const gchar *)filename);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select id from images where film_id = ?1 and filename = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgfname, -1, SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int(stmt, 0);
    g_free(imgfname);
    sqlite3_finalize(stmt);
    g_free(ext);
    dt_image_t *img = dt_image_cache_get(darktable.image_cache, id, 'w');
    img->flags &= ~DT_IMAGE_REMOVE;
    dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
    dt_image_read_duplicates(id, filename);
    dt_image_synch_all_xmp(filename);
    return id;
  }
  sqlite3_finalize(stmt);

  // also need to set the no-legacy bit, to make sure we get the right presets (new ones)
  uint32_t flags = dt_conf_get_int("ui_last/import_initial_rating");
  if(flags > 5)
  {
    flags = 1;
    dt_conf_set_int("ui_last/import_initial_rating", 1);
  }
  flags |= DT_IMAGE_NO_LEGACY_PRESETS;
  // set the bits in flags that indicate if any of the extra files (.txt, .wav) are present
  char *extra_file = dt_image_get_audio_path_from_path(filename);
  if(extra_file)
  {
    flags |= DT_IMAGE_HAS_WAV;
    g_free(extra_file);
  }
  extra_file = dt_image_get_text_path_from_path(filename);
  if(extra_file)
  {
    flags |= DT_IMAGE_HAS_TXT;
    g_free(extra_file);
  }
  // insert dummy image entry in database
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "insert into images (id, film_id, filename, caption, description, "
      "license, sha1sum, flags, version, max_version, history_end) values (null, ?1, ?2, '', '', '', '', ?3, 0, 0, 0)",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgfname, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, flags);
  rc = sqlite3_step(stmt);
  if(rc != SQLITE_DONE) fprintf(stderr, "sqlite3 error %d\n", rc);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select id from images where film_id = ?1 and filename = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgfname, -1, SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

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
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "select group_id from images where film_id = ?1 and filename like ?2 and id = group_id", -1, &stmt2,
        NULL);
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
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "select id from images where group_id = ?1 and id != ?1", -1, &stmt3,
                                    NULL);
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
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "select group_id from images where film_id = ?1 and filename like ?2 and id != ?3", -1, &stmt2, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, film_id);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 2, sql_pattern, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 3, id);
    if(sqlite3_step(stmt2) == SQLITE_ROW)
      group_id = sqlite3_column_int(stmt2, 0);
    else
      group_id = id;
    sqlite3_finalize(stmt2);
  }
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update images set group_id = ?1 where id = ?2",
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
  (void)dt_exif_read(img, filename);
  char dtfilename[PATH_MAX] = { 0 };
  g_strlcpy(dtfilename, filename, sizeof(dtfilename));
  // dt_image_path_append_version(id, dtfilename, sizeof(dtfilename));
  g_strlcat(dtfilename, ".xmp", sizeof(dtfilename));

  int res = dt_exif_xmp_read(img, dtfilename, 0);

  // write through to db, but not to xmp.
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);

  if(res != 0)
  {
    // Search for Lightroom sidecar file, import tags if found
    dt_lightroom_import(id, NULL, TRUE);
  }

  // add a tag with the file extension
  guint tagid = 0;
  char tagname[512];
  snprintf(tagname, sizeof(tagname), "darktable|format|%s", ext);
  g_free(ext);
  dt_tag_new(tagname, &tagid);
  dt_tag_attach(tagid, id);

  // make sure that there are no stale thumbnails left
  dt_mipmap_cache_remove(darktable.mipmap_cache, id);

  // read all sidecar files
  dt_image_read_duplicates(id, filename);
  dt_image_synch_all_xmp(filename);

  g_free(imgfname);
  g_free(basename);
  g_free(sql_pattern);

  dt_control_signal_raise(darktable.signals, DT_SIGNAL_IMAGE_IMPORT, id);
  // the following line would look logical with new_tags_set being the return value
  // from dt_tag_new above, but this could lead to too rapid signals, being able to lock up the
  // keywords side pane when trying to use it, which can lock up the whole dt GUI ..
  // if (new_tags_set) dt_control_signal_raise(darktable.signals,DT_SIGNAL_TAG_CHANGED);
  return id;
}

void dt_image_init(dt_image_t *img)
{
  img->width = img->height = 0;
  img->crop_x = img->crop_y = img->crop_width = img->crop_height = 0;
  img->orientation = ORIENTATION_NULL;
  img->legacy_flip.legacy = 0;
  img->legacy_flip.user_flip = 0;

  img->filters = 0u;
  img->bpp = 0;
  img->film_id = -1;
  img->group_id = -1;
  img->flags = 0;
  img->id = -1;
  img->version = -1;
  img->loader = LOADER_UNKNOWN;
  img->exif_inited = 0;
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
  img->exif_model[0] = img->exif_maker[0] = img->exif_lens[0] = '\0';
  g_strlcpy(img->exif_datetime_taken, "0000:00:00 00:00:00", sizeof(img->exif_datetime_taken));
  img->exif_crop = 1.0;
  img->exif_exposure = 0;
  img->exif_aperture = 0;
  img->exif_iso = 0;
  img->exif_focal_length = 0;
  img->exif_focus_distance = 0;
  img->latitude = NAN;
  img->longitude = NAN;
  img->elevation = NAN;
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
  img->cache_entry = 0;
}

void dt_image_refresh_makermodel(dt_image_t *img)
{
  if (!img->camera_maker[0] || !img->camera_model[0] || !img->camera_alias[0])
  {
    // We need to use the exif values, so let's get rawspeed to munge them
    dt_rawspeed_lookup_makermodel(img->exif_maker, img->exif_model,
                                  img->camera_maker, sizeof(img->camera_maker),
                                  img->camera_model, sizeof(img->camera_model),
                                  img->camera_alias, sizeof(img->camera_alias));
  }

  // Now we just create a makermodel by concatenation
  g_strlcpy(img->camera_makermodel, img->camera_maker, sizeof(img->camera_makermodel));
  int len = strlen(img->camera_maker);
  img->camera_makermodel[len] = ' ';
  g_strlcpy(img->camera_makermodel+len+1, img->camera_model, sizeof(img->camera_makermodel)-len-1);
}

int32_t dt_image_move(const int32_t imgid, const int32_t filmid)
{
  // TODO: several places where string truncation could occur unnoticed
  int32_t result = -1;
  gchar oldimg[PATH_MAX] = { 0 };
  gchar newimg[PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, oldimg, sizeof(oldimg), &from_cache);
  gchar *newdir = NULL;

  sqlite3_stmt *film_stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select folder from film_rolls where id = ?1",
                              -1, &film_stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(film_stmt, 1, filmid);
  if(sqlite3_step(film_stmt) == SQLITE_ROW) newdir = g_strdup((gchar *)sqlite3_column_text(film_stmt, 0));
  sqlite3_finalize(film_stmt);

  if(newdir)
  {
    gchar copysrcpath[PATH_MAX] = { 0 };
    gchar copydestpath[PATH_MAX] = { 0 };
    gchar *imgbname = g_path_get_basename(oldimg);
    g_snprintf(newimg, sizeof(newimg), "%s%c%s", newdir, G_DIR_SEPARATOR, imgbname);
    g_free(imgbname);
    g_free(newdir);

    // get current local copy if any
    _image_local_copy_full_path(imgid, copysrcpath, sizeof(copysrcpath));

    // move image
    GFile *old, *new;
    old = g_file_new_for_path(oldimg);
    new = g_file_new_for_path(newimg);
    if(!g_file_test(newimg, G_FILE_TEST_EXISTS) && (g_file_move(old, new, 0, NULL, NULL, NULL, NULL) == TRUE))
    {
      // statement for getting ids of the image to be moved and it's duplicates
      sqlite3_stmt *duplicates_stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "select id from images where filename in (select filename from images "
                                  "where id = ?1) and film_id in (select film_id from images where id = ?1)",
                                  -1, &duplicates_stmt, NULL);

      // first move xmp files of image and duplicates
      GList *dup_list = NULL;
      DT_DEBUG_SQLITE3_BIND_INT(duplicates_stmt, 1, imgid);
      while(sqlite3_step(duplicates_stmt) == SQLITE_ROW)
      {
        int32_t id = sqlite3_column_int(duplicates_stmt, 0);
        dup_list = g_list_append(dup_list, GINT_TO_POINTER(id));
        gchar oldxmp[PATH_MAX] = { 0 }, newxmp[PATH_MAX] = { 0 };
        g_strlcpy(oldxmp, oldimg, sizeof(oldxmp));
        g_strlcpy(newxmp, newimg, sizeof(newxmp));
        dt_image_path_append_version(id, oldxmp, sizeof(oldxmp));
        dt_image_path_append_version(id, newxmp, sizeof(newxmp));
        g_strlcat(oldxmp, ".xmp", sizeof(oldxmp));
        g_strlcat(newxmp, ".xmp", sizeof(newxmp));

        GFile *goldxmp = g_file_new_for_path(oldxmp);
        GFile *gnewxmp = g_file_new_for_path(newxmp);

        if(g_file_test(oldxmp, G_FILE_TEST_EXISTS))
          (void)g_file_move(goldxmp, gnewxmp, 0, NULL, NULL, NULL, NULL);

        g_object_unref(goldxmp);
        g_object_unref(gnewxmp);
      }
      sqlite3_finalize(duplicates_stmt);

      // then update database and cache
      // if update was performed in above loop, dt_image_path_append_version()
      // would return wrong version!
      while(dup_list)
      {
        int id = GPOINTER_TO_INT(dup_list->data);
        dt_image_t *img = dt_image_cache_get(darktable.image_cache, id, 'w');
        img->film_id = filmid;
        // write through to db, but not to xmp
        dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
        dup_list = g_list_delete_link(dup_list, dup_list);
      }
      g_list_free(dup_list);

      // finaly, rename local copy if any
      if(g_file_test(copysrcpath, G_FILE_TEST_EXISTS))
      {
        // get new name
        _image_local_copy_full_path(imgid, copydestpath, sizeof(copydestpath));

        GFile *cold = g_file_new_for_path(copysrcpath);
        GFile *cnew = g_file_new_for_path(copydestpath);

        if(g_file_move(cold, cnew, 0, NULL, NULL, NULL, NULL) != TRUE)
          fprintf(stderr, "[dt_image_move] error moving local copy `%s' -> `%s'\n", copysrcpath, copydestpath);

        g_object_unref(cold);
        g_object_unref(cnew);
      }

      result = 0;
    }
    else
    {
      fprintf(stderr, "[dt_image_move] error moving `%s' -> `%s'\n", oldimg, newimg);
    }

    g_object_unref(old);
    g_object_unref(new);
  }

  return result;
}

int32_t dt_image_copy(const int32_t imgid, const int32_t filmid)
{
  int32_t newid = -1;
  sqlite3_stmt *stmt;
  gchar srcpath[PATH_MAX] = { 0 };
  gchar *newdir = NULL;
  gchar *filename = NULL;
  gboolean from_cache = FALSE;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select folder from film_rolls where id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
  if(sqlite3_step(stmt) == SQLITE_ROW) newdir = g_strdup((gchar *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);

  if(newdir)
  {
    dt_image_full_path(imgid, srcpath, sizeof(srcpath), &from_cache);
    gchar *imgbname = g_path_get_basename(srcpath);
    gchar *destpath = g_build_filename(newdir, imgbname, NULL);
    GFile *src = g_file_new_for_path(srcpath);
    GFile *dest = g_file_new_for_path(destpath);
    g_free(imgbname);
    imgbname = NULL;
    g_free(newdir);
    newdir = NULL;
    g_free(destpath);
    destpath = NULL;

    // copy image to new folder
    // if image file already exists, continue
    GError *gerror = NULL;
    g_file_copy(src, dest, G_FILE_COPY_NONE, NULL, NULL, NULL, &gerror);

    if((gerror == NULL) || (gerror != NULL && gerror->code == G_IO_ERROR_EXISTS))
    {
      // update database
      DT_DEBUG_SQLITE3_PREPARE_V2(
          dt_database_get(darktable.db),
          "insert into images "
          "(id, group_id, film_id, width, height, filename, maker, model, lens, exposure, "
          "aperture, iso, focal_length, focus_distance, datetime_taken, flags, "
          "output_width, output_height, crop, raw_parameters, raw_denoise_threshold, "
          "raw_auto_bright_threshold, raw_black, raw_maximum, "
          "caption, description, license, sha1sum, orientation, histogram, lightmap, "
          "longitude, latitude, altitude, color_matrix, colorspace, version, max_version) "
          "select null, group_id, ?1 as film_id, width, height, filename, maker, model, lens, "
          "exposure, aperture, iso, focal_length, focus_distance, datetime_taken, "
          "flags, width, height, crop, raw_parameters, raw_denoise_threshold, "
          "raw_auto_bright_threshold, raw_black, raw_maximum, "
          "caption, description, license, sha1sum, orientation, histogram, lightmap, "
          "longitude, latitude, altitude, color_matrix, colorspace, -1, -1 "
          "from images where id = ?2",
          -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "select a.id, a.filename from images as a join images as b where "
                                  "a.film_id = ?1 and a.filename = b.filename and "
                                  "b.id = ?2 order by a.id desc",
                                  -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);

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
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "insert into color_labels (imgid, color) select ?1, color from "
                                    "color_labels where imgid = ?2",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "insert into meta_data (id, key, value) select ?1, key, value "
                                    "from meta_data where id = ?2",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "insert into tagged_images (imgid, tagid) select ?1, tagid from "
                                    "tagged_images where imgid = ?2",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        // get max_version of image duplicates in destination filmroll
        int32_t max_version = -1;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "select max(a.max_version) from images as a join images as b where "
                                    "a.film_id = b.film_id and a.filename = b.filename and "
                                    "b.id = ?1",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);

        if(sqlite3_step(stmt) == SQLITE_ROW) max_version = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);

        // set version of new entry and max_version of all involved duplicates (with same film_id and
        // filename)
        max_version = (max_version >= 0) ? max_version + 1 : 0;
        int32_t version = max_version;

        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "update images set version=?1 where id = ?2", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, version);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, newid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "update images set max_version=?1 where film_id = ?2 and filename = ?3",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, max_version);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, filmid);
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, filename, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        // image group handling follows
        // get group_id of potential image duplicates in destination filmroll
        int32_t new_group_id = -1;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "select distinct a.group_id from images as a join images as b where "
                                    "a.film_id = b.film_id and a.filename = b.filename and "
                                    "b.id = ?1 and a.id != ?1",
                                    -1, &stmt, NULL);
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
                                    "update images set group_id=?1 where id = ?2", -1, &stmt, NULL);

        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, new_group_id);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, newid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        dt_history_copy_and_paste_on_image(imgid, newid, FALSE, NULL);

        // write xmp file
        dt_image_write_sidecar_file(newid);
      }

      g_free(filename);
    }
    else
    {
      fprintf(stderr, "Failed to copy image %s: %s\n", srcpath, gerror->message);
    }
    g_object_unref(dest);
    g_object_unref(src);
    g_clear_error(&gerror);
  }

  return newid;
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

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT COUNT(*) FROM images WHERE id!=?1 AND "
                                                             "flags&?2=?2 AND film_id=(SELECT film_id FROM "
                                                             "images WHERE id=?1) AND filename=(SELECT "
                                                             "filename FROM images WHERE id=?1);",
                              -1, &stmt, NULL);
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

    // update cache, remove local copy flags
    dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
    img->flags &= ~DT_IMAGE_LOCAL_COPY;
    dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);

    dt_control_queue_redraw_center();
  }

  return 0;
}

// *******************************************************
// xmp stuff
// *******************************************************

void dt_image_write_sidecar_file(int imgid)
{
  // TODO: compute hash and don't write if not needed!
  // write .xmp file
  if(imgid > 0 && dt_conf_get_bool("write_sidecar_files"))
  {
    gboolean from_cache = TRUE;
    char filename[PATH_MAX] = { 0 };
    dt_image_full_path(imgid, filename, sizeof(filename), &from_cache);
    dt_image_path_append_version(imgid, filename, sizeof(filename));
    g_strlcat(filename, ".xmp", sizeof(filename));
    if(!dt_exif_xmp_write(imgid, filename))
    {
      // put the timestamp into db. this can't be done in exif.cc since that code gets called
      // for the copy exporter, too
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE images SET write_timestamp = STRFTIME('%s', 'now') WHERE id = ?1",
                                  -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }
}


void dt_image_synch_xmp(const int selected)
{
  if(selected > 0)
  {
    dt_image_write_sidecar_file(selected);
  }
  else if(dt_conf_get_bool("write_sidecar_files"))
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select imgid from selected_images", -1, &stmt,
                                NULL);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int imgid = sqlite3_column_int(stmt, 0);
      dt_image_write_sidecar_file(imgid);
    }
    sqlite3_finalize(stmt);
  }
}

void dt_image_synch_all_xmp(const gchar *pathname)
{
  if(dt_conf_get_bool("write_sidecar_files"))
  {
    sqlite3_stmt *stmt;
    gchar *imgfname = g_path_get_basename(pathname);
    gchar *imgpath = g_path_get_dirname(pathname);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "select id from images where film_id in (select id from film_rolls "
                                "where folder = ?1) and filename = ?2",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, imgpath, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgfname, -1, SQLITE_TRANSIENT);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int imgid = sqlite3_column_int(stmt, 0);
      dt_image_write_sidecar_file(imgid);
    }
    sqlite3_finalize(stmt);
    g_free(imgfname);
    g_free(imgpath);
  }
}

void dt_image_local_copy_synch(void)
{
  // nothing to do if not creating .xmp
  if(!dt_conf_get_bool("write_sidecar_files")) return;

  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM images WHERE flags&?1=?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, DT_IMAGE_LOCAL_COPY);

  int count = 0;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int imgid = sqlite3_column_int(stmt, 0);
    gboolean from_cache = TRUE;
    char filename[PATH_MAX] = { 0 };
    dt_image_full_path(imgid, filename, sizeof(filename), &from_cache);

    if(!from_cache)
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

void dt_image_add_time_offset(const int imgid, const long int offset)
{
  const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(!cimg) return;

  // get the datetime_taken and calculate the new time
  gint year;
  gint month;
  gint day;
  gint hour;
  gint minute;
  gint seconds;

  if(sscanf(cimg->exif_datetime_taken, "%d:%d:%d %d:%d:%d", (int *)&year, (int *)&month, (int *)&day,
            (int *)&hour, (int *)&minute, (int *)&seconds) != 6)
  {
    fprintf(stderr, "broken exif time in db, '%s', imgid %d\n", cimg->exif_datetime_taken, imgid);
    dt_image_cache_read_release(darktable.image_cache, cimg);
    return;
  }

  GTimeZone *tz = g_time_zone_new_utc();
  GDateTime *datetime_original = g_date_time_new(tz, year, month, day, hour, minute, seconds);
  g_time_zone_unref(tz);
  if(!datetime_original)
  {
    dt_image_cache_read_release(darktable.image_cache, cimg);
    return;
  }

  // let's add our offset
  GDateTime *datetime_new = g_date_time_add_seconds(datetime_original, offset);
  g_date_time_unref(datetime_original);

  if(!datetime_new)
  {
    dt_image_cache_read_release(darktable.image_cache, cimg);
    return;
  }

  gchar *datetime = g_date_time_format(datetime_new, "%Y:%m:%d %H:%M:%S");
  g_date_time_unref(datetime_new);

  // update exif_datetime_taken in img
  if(datetime)
  {
    dt_image_cache_read_release(darktable.image_cache, cimg);
    dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
    g_strlcpy(img->exif_datetime_taken, datetime, sizeof(img->exif_datetime_taken));
    dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);
  }
  else dt_image_cache_read_release(darktable.image_cache, cimg);

  g_free(datetime);
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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
