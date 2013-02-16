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
#include <glob.h>
#include <glib/gstdio.h>

int dt_image_is_ldr(const dt_image_t *img)
{
  const char *c = img->filename + strlen(img->filename);
  while(*c != '.' && c > img->filename) c--;
  if((img->flags & DT_IMAGE_LDR) || !strcasecmp(c, ".jpg") ||
      !strcasecmp(c, ".png") || !strcasecmp(c, ".ppm"))
    return 1;
  else return 0;
}

int dt_image_is_hdr(const dt_image_t *img)
{
  const char *c = img->filename + strlen(img->filename);
  while(*c != '.' && c > img->filename) c--;
  if((img->flags & DT_IMAGE_HDR) || !strcasecmp(c, ".exr") ||
      !strcasecmp(c, ".hdr") || !strcasecmp(c, ".pfm"))
    return 1;
  else return 0;
}

int dt_image_is_raw(const dt_image_t *img)
{
  const char *c = img->filename + strlen(img->filename);
  while(*c != '.' && c > img->filename) c--;
  if((img->flags & DT_IMAGE_RAW) || (strcasecmp(c, ".jpg") &&
                                     strcasecmp(c, ".png") && strcasecmp(c, ".ppm") &&
                                     strcasecmp(c, ".hdr") && strcasecmp(c, ".exr") && strcasecmp(c, ".pfm")))
    return 1;
  else return 0;
}

const char *
dt_image_film_roll_name(const char *path)
{
  const char *folder = path + strlen(path);
  int numparts = dt_conf_get_int("show_folder_levels");
  numparts = CLAMPS(numparts, 1, 5);
  int count = 0;
  if (numparts < 1)
    numparts = 1;
  while (folder > path)
  {
    if (*folder == '/')
      if (++count >= numparts)
      {
        ++folder;
        break;
      }
    --folder;
  }
  return folder;
}

void dt_image_film_roll(const dt_image_t *img, char *pathname, int len)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select folder from film_rolls where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->film_id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *f = (char *)sqlite3_column_text(stmt, 0);
    const char *c = dt_image_film_roll_name(f);
    snprintf(pathname, len, "%s", c);
  }
  else
  {
    snprintf(pathname, len, "%s", _("orphaned image"));
  }
  sqlite3_finalize(stmt);
  pathname[len-1] = '\0';
}

void dt_image_full_path(const int imgid, char *pathname, int len)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select folder || '/' || filename from images, film_rolls where "
                              "images.film_id = film_rolls.id and images.id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    g_strlcpy(pathname, (char *)sqlite3_column_text(stmt, 0), len);
  }
  sqlite3_finalize(stmt);
}

void dt_image_path_append_version(int imgid, char *pathname, const int len)
{
  // get duplicate suffix
  int version = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select count(id) from images where filename in "
                              "(select filename from images where id = ?1) and film_id in "
                              "(select film_id from images where id = ?1) and id < ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    version = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  if(version != 0)
  {
    // add version information:
    char *filename = g_strdup(pathname);

    char *c = pathname + strlen(pathname);
    while(*c != '.' && c > pathname) c--;
    snprintf(c, pathname + len - c, "_%02d", version);
    char *c2 = filename + strlen(filename);
    while(*c2 != '.' && c2 > filename) c2--;
    snprintf(c+3, pathname + len - c - 3, "%s", c2);
    g_free(filename);
  }
}

void dt_image_print_exif(const dt_image_t *img, char *line, int len)
{
  if(img->exif_exposure >= 0.1f)
    snprintf(line, len, "%.1f'' f/%.1f %dmm iso %d",
             img->exif_exposure, img->exif_aperture, (int)img->exif_focal_length,
             (int)img->exif_iso);
  else
    snprintf(line, len, "1/%.0f f/%.1f %dmm iso %d", 1.0/img->exif_exposure,
             img->exif_aperture, (int)img->exif_focal_length, (int)img->exif_iso);
}

void dt_image_set_location(const int32_t imgid, double lon, double lat)
{
  /* fetch image from cache */
  const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
  if (!cimg)
    return;
  dt_image_t *image = dt_image_cache_write_get(darktable.image_cache, cimg);

  /* set image location */
  image->longitude = lon;
  image->latitude = lat;

  /* store */
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
  dt_image_cache_read_release(darktable.image_cache, image);
}

void dt_image_set_flip(const int32_t imgid, const int32_t orientation)
{
  sqlite3_stmt *stmt;
  // push new orientation to sql via additional history entry:
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select MAX(num) from history where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  int num = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    num = 1 + sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "insert into history (imgid, num, module, operation, op_params, enabled, "
                              "blendop_params, blendop_version) values"
                              " (?1, ?2, 1, 'flip', ?3, 1, null, 0) ", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, &orientation, sizeof(int32_t),
                             SQLITE_TRANSIENT);
  sqlite3_step (stmt);
  sqlite3_finalize(stmt);
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
  // write that through to xmp:
  dt_image_write_sidecar_file(imgid);
}

void dt_image_flip(const int32_t imgid, const int32_t cw)
{
  // this is light table only:
  if(darktable.develop->image_storage.id == imgid) return;
  int32_t orientation = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select * from history where imgid = ?1 and operation = 'flip' and "
                              "num in (select MAX(num) from history where imgid = ?1 and "
                              "operation = 'flip')", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(sqlite3_column_bytes(stmt, 4) >= 4)
      orientation = *(int32_t *)sqlite3_column_blob(stmt, 4);
  }
  sqlite3_finalize(stmt);

  if(cw == 1)
  {
    if(orientation & 4) orientation ^= 1;
    else                orientation ^= 2; // flip x
  }
  else
  {
    if(orientation & 4) orientation ^= 2;
    else                orientation ^= 1; // flip y
  }
  orientation ^= 4;             // flip axes

  if(cw == 2) orientation = 0; // reset
  dt_image_set_flip(imgid, orientation);
}

int32_t dt_image_duplicate(const int32_t imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "insert into images "
                              "(id, group_id, film_id, width, height, filename, maker, model, lens, exposure, "
                              "aperture, iso, focal_length, focus_distance, datetime_taken, flags, "
                              "output_width, output_height, crop, raw_parameters, raw_denoise_threshold, "
                              "raw_auto_bright_threshold, raw_black, raw_maximum, "
                              "caption, description, license, sha1sum, orientation, histogram, lightmap, "
                              "longitude, latitude, color_matrix, colorspace) "
                              "select null, group_id, film_id, width, height, filename, maker, model, lens, "
                              "exposure, aperture, iso, focal_length, focus_distance, datetime_taken, "
                              "flags, width, height, crop, raw_parameters, raw_denoise_threshold, "
                              "raw_auto_bright_threshold, raw_black, raw_maximum, "
                              "caption, description, license, sha1sum, orientation, histogram, lightmap, "
                              "longitude, latitude, color_matrix, colorspace "
                              "from images where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select a.id from images as a join images as b where "
                              "a.film_id = b.film_id and a.filename = b.filename and "
                              "b.id = ?1 order by a.id desc", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  int32_t newid = -1;
  if(sqlite3_step(stmt) == SQLITE_ROW)
    newid = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  if(newid != -1)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "insert into color_labels (imgid, color) select ?1, color from "
                                "color_labels where imgid = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "insert into meta_data (id, key, value) select ?1, key, value "
                                "from meta_data where id = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "insert into tagged_images (imgid, tagid) select ?1, tagid from "
                                "tagged_images where imgid = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "update tagxtag set count = count + 1 where "
                                "(id1 in (select tagid from tagged_images where imgid = ?1)) or "
                                "(id2 in (select tagid from tagged_images where imgid = ?1))",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(darktable.gui && darktable.gui->grouping)
    {
      const dt_image_t *img = dt_image_cache_read_get(darktable.image_cache, newid);
      darktable.gui->expanded_group_id = img->group_id;
      dt_image_cache_read_release(darktable.image_cache, img);
      dt_collection_update_query(darktable.collection);
    }
  }
  return newid;
}

void dt_image_remove(const int32_t imgid)
{
  sqlite3_stmt *stmt;
  const dt_image_t *img = dt_image_cache_read_get(darktable.image_cache, imgid);
  int old_group_id = img->group_id;
  dt_image_cache_read_release(darktable.image_cache, img);

  // make sure we remove from the cache first, or else the cache will look for imgid in sql
  dt_image_cache_remove(darktable.image_cache, imgid);

  int new_group_id = dt_grouping_remove_from_group(imgid);
  if(darktable.gui && darktable.gui->expanded_group_id == old_group_id)
    darktable.gui->expanded_group_id = new_group_id;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "delete from images where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "update tagxtag set count = count - 1 where "
                              "(id2 in (select tagid from tagged_images where imgid = ?1)) or "
                              "(id1 in (select tagid from tagged_images where imgid = ?1))",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "delete from tagged_images where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "delete from history where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "delete from color_labels where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "delete from meta_data where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "delete from selected_images where imgid = ?1", -1, &stmt, NULL);
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
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select operation from history where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *op = (const char *)sqlite3_column_text(stmt, 0);
    // FIXME: this is clearly a terrible way to determine which modules
    // are okay to still load the thumbnail and which aren't.
    // (that's currently the only use of this function)
    if(!strcmp(op, "basecurve")) continue;
    if(!strcmp(op, "sharpen")) continue;
    if(!strcmp(op, "dither")) continue;
    if(!strcmp(op, "tonecurve")) continue;
    if(!strcmp(op, "highlights")) continue;
    altered = 1;
    break;
  }
  sqlite3_finalize(stmt);
  if(altered) return 1;

  return altered;
}


uint32_t dt_image_import(const int32_t film_id, const char *filename, gboolean override_ignore_jpegs)
{
  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
    return 0;
  const char *cc = filename + strlen(filename);
  for(; *cc!='.'&&cc>filename; cc--);
  if(!strcmp(cc, ".dt")) return 0;
  if(!strcmp(cc, ".dttags")) return 0;
  if(!strcmp(cc, ".xmp")) return 0;
  char *ext = g_ascii_strdown(cc+1, -1);
  if(override_ignore_jpegs == FALSE && (!strcmp(ext, "jpg") ||
                                        !strcmp(ext, "jpeg")) && dt_conf_get_bool("ui_last/import_ignore_jpegs"))
  {
    g_free(ext);
    return 0;
  }
  int supported = 0;
  char **extensions = g_strsplit(dt_supported_extensions, ",", 100);
  for(char **i=extensions; *i!=NULL; i++)
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
  imgfname = g_path_get_basename((const gchar*)filename);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select id from images where film_id = ?1 and filename = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgfname, strlen(imgfname), SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int(stmt, 0);
    g_free(imgfname);
    sqlite3_finalize(stmt);
    g_free(ext);
    const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, id);
    dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
    img->flags &= ~DT_IMAGE_REMOVE;
    dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
    dt_image_cache_read_release(darktable.image_cache, img);
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
  // insert dummy image entry in database
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "insert into images (id, film_id, filename, caption, description, "
                              "license, sha1sum, flags) values (null, ?1, ?2, '', '', '', '', ?3)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgfname, strlen(imgfname),
                             SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, flags);
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) fprintf(stderr, "sqlite3 error %d\n", rc);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select id from images where film_id = ?1 and filename = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgfname, strlen(imgfname),
                             SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // Try to find out if this should be grouped already.
  gchar *basename = g_strdup(imgfname);
  gchar *cc2 = basename + strlen(basename);
  for(; *cc2!='.'&&cc2>basename; cc2--);
  *cc2='\0';
  gchar *sql_pattern = g_strconcat(basename, ".%", NULL);
  int group_id;
  // in case we are not a jpg check if we need to change group representative
  if (strcmp(ext, "jpg") != 0 && strcmp(ext, "jpeg") != 0)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select group_id from images where film_id = ?1 and filename like ?2 and id = group_id", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, sql_pattern, -1, SQLITE_TRANSIENT);
    // if we have a group already
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
      int other_id = sqlite3_column_int(stmt, 0);
      const dt_image_t *cother_img = dt_image_cache_read_get(darktable.image_cache, other_id);
      gchar *other_basename = g_strdup(cother_img->filename);
      gchar *cc3 = other_basename + strlen(cother_img->filename);
      for (; *cc3!='.'&&cc3>other_basename; cc3--);
      ++cc3;
      g_ascii_strdown(cc3, -1);
      // if the group representative is a jpg, change group representative to this new imported image
      if (!strcmp(cc3, "jpg") || !strcmp(cc3, "jpeg"))
      {
        dt_image_t *other_img = dt_image_cache_write_get(darktable.image_cache, cother_img);
        other_img->group_id = id;
        dt_image_cache_write_release(darktable.image_cache, other_img, DT_IMAGE_CACHE_SAFE);
        dt_image_cache_read_release(darktable.image_cache, cother_img);
        sqlite3_finalize(stmt);
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select group_id from images where film_id = ?1 and filename like ?2 and id != ?3 and group_id != id", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, sql_pattern, -1, SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, id);
        while(sqlite3_step(stmt) == SQLITE_ROW)
        {
          other_id = sqlite3_column_int(stmt, 0);
          const dt_image_t *cgroup_img = dt_image_cache_read_get(darktable.image_cache, other_id);
          dt_image_t *group_img = dt_image_cache_write_get(darktable.image_cache, cgroup_img);
          group_img->group_id = id;
          dt_image_cache_write_release(darktable.image_cache, group_img, DT_IMAGE_CACHE_SAFE);
          dt_image_cache_read_release(darktable.image_cache, cgroup_img);
        }
        group_id = id;
      } else {
        dt_image_cache_read_release(darktable.image_cache, cother_img);
        group_id = other_id;
      }
      g_free(other_basename);
    } else {
      group_id = id;
    }
  } else {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select group_id from images where film_id = ?1 and filename like ?2 and id != ?3", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, sql_pattern, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, id);
    if(sqlite3_step(stmt) == SQLITE_ROW) group_id = sqlite3_column_int(stmt, 0);
    else                                 group_id = id;
  }
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update images set group_id = ?1 where id = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, group_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // printf("[image_import] importing `%s' to img id %d\n", imgfname, id);

  // lock as shortly as possible:
  const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, id);
  dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
  img->group_id = group_id;

  // read dttags and exif for database queries!
  (void) dt_exif_read(img, filename);
  char dtfilename[DT_MAX_PATH_LEN];
  g_strlcpy(dtfilename, filename, DT_MAX_PATH_LEN);
  dt_image_path_append_version(id, dtfilename, DT_MAX_PATH_LEN);
  char *c = dtfilename + strlen(dtfilename);
  sprintf(c, ".xmp");
  (void)dt_exif_xmp_read(img, dtfilename, 0);

  // write through to db, but not to xmp.
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
  dt_image_cache_read_release(darktable.image_cache, img);

  // add a tag with the file extension
  guint tagid = 0;
  char tagname[512];
  snprintf(tagname, 512, "darktable|format|%s", ext);
  g_free(ext);
  dt_tag_new(tagname, &tagid);
  dt_tag_attach(tagid,id);

  // Search for sidecar files and import them if found.
  glob_t *globbuf = g_malloc(sizeof(glob_t));

  // Add version wildcard
  gchar *fname = g_strdup(filename);
  gchar pattern[DT_MAX_PATH_LEN];
  g_snprintf(pattern, DT_MAX_PATH_LEN, "%s", filename);
  char *c1 = pattern + strlen(pattern);
  while(*c1 != '.' && c1 > pattern) c1--;
  snprintf(c1, pattern + DT_MAX_PATH_LEN - c1, "_*");
  char *c2 = fname + strlen(fname);
  while(*c2 != '.' && c2 > fname) c2--;
  snprintf(c1+2, pattern + DT_MAX_PATH_LEN - c1 - 2, "%s.xmp", c2);

  if (!glob(pattern, 0, NULL, globbuf))
  {
    for (size_t i=0; i < globbuf->gl_pathc; i++)
    {
      int newid = -1;
      newid = dt_image_duplicate(id);

      const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, newid);
      dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
      (void)dt_exif_xmp_read(img, globbuf->gl_pathv[i], 0);
      dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
      dt_image_cache_read_release(darktable.image_cache, img);
    }
    globfree(globbuf);
  }
  else
  {
    // Search for Lightroom sidecar file, import tags if found
    dt_lightroom_import(id, NULL, TRUE);
  }

  g_free(imgfname);
  g_free(fname);
  g_free(basename);
  g_free(sql_pattern);
  g_free(globbuf);

  dt_control_signal_raise(darktable.signals,DT_SIGNAL_IMAGE_IMPORT,id);
  return id;
}

void dt_image_init(dt_image_t *img)
{
  img->width = img->height = 0;
  img->orientation = -1;
  img->legacy_flip.legacy = 0;
  img->legacy_flip.user_flip = 0;

  img->filters = 0;
  img->bpp = 0;
  img->film_id = -1;
  img->group_id = -1;
  img->flags = 0;
  img->id = -1;
  img->exif_inited = 0;
  memset(img->exif_maker, 0, sizeof(img->exif_maker));
  memset(img->exif_model, 0, sizeof(img->exif_model));
  memset(img->exif_lens, 0, sizeof(img->exif_lens));
  memset(img->filename, 0, sizeof(img->filename));
  g_strlcpy(img->filename, "(unknown)", 10);
  img->exif_model[0] = img->exif_maker[0] = img->exif_lens[0] = '\0';
  g_strlcpy(img->exif_datetime_taken, "0000:00:00 00:00:00",
            sizeof(img->exif_datetime_taken));
  img->exif_crop = 1.0;
  img->exif_exposure = 0;
  img->exif_aperture = 0;
  img->exif_iso = 0;
  img->exif_focal_length = 0;
  img->exif_focus_distance = 0;
  img->latitude = NAN;
  img->longitude = NAN;
  img->d65_color_matrix[0] = NAN;
  img->profile = NULL;
  img->profile_size = 0;
  img->colorspace = DT_IMAGE_COLORSPACE_NONE;
}

int32_t dt_image_move(const int32_t imgid, const int32_t filmid)
{
  //TODO: several places where string truncation could occur unnoticed
  int32_t result = -1;
  gchar oldimg[DT_MAX_PATH_LEN] = {0};
  gchar newimg[DT_MAX_PATH_LEN] = {0};
  dt_image_full_path(imgid, oldimg, DT_MAX_PATH_LEN);
  gchar *newdir = NULL;

  sqlite3_stmt *film_stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select folder from film_rolls where id = ?1", -1, &film_stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(film_stmt, 1, filmid);
  if(sqlite3_step(film_stmt) == SQLITE_ROW)
    newdir = g_strdup((gchar *) sqlite3_column_text(film_stmt, 0));
  sqlite3_finalize(film_stmt);

  if(newdir)
  {
    gchar *imgbname = g_path_get_basename(oldimg);
    g_snprintf(newimg, DT_MAX_PATH_LEN, "%s%c%s", newdir, G_DIR_SEPARATOR, imgbname);
    g_free(imgbname);
    g_free(newdir);

    // statement for getting ids of the image to be moved and it's duplicates
    sqlite3_stmt *duplicates_stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "select id from images where filename in (select filename from images "
                                "where id = ?1) and film_id in (select film_id from images where id = ?1)",
                                -1, &duplicates_stmt, NULL);

    // move image
    GFile *old, *new;
    old = g_file_new_for_path(oldimg);
    new = g_file_new_for_path(newimg);
    if (!g_file_test(newimg, G_FILE_TEST_EXISTS)
        && (g_file_move(old, new, 0, NULL, NULL, NULL, NULL) == TRUE))
    {
      // first move xmp files of image and duplicates
      GList *dup_list = NULL;
      DT_DEBUG_SQLITE3_BIND_INT(duplicates_stmt, 1, imgid);
      while (sqlite3_step(duplicates_stmt) == SQLITE_ROW)
      {
        int32_t id = sqlite3_column_int(duplicates_stmt, 0);
        dup_list = g_list_append(dup_list, GINT_TO_POINTER(id));
        gchar oldxmp[DT_MAX_PATH_LEN], newxmp[DT_MAX_PATH_LEN];
        g_strlcpy(oldxmp, oldimg, DT_MAX_PATH_LEN);
        g_strlcpy(newxmp, newimg, DT_MAX_PATH_LEN);
        dt_image_path_append_version(id, oldxmp, DT_MAX_PATH_LEN);
        dt_image_path_append_version(id, newxmp, DT_MAX_PATH_LEN);
        g_strlcat(oldxmp, ".xmp", DT_MAX_PATH_LEN);
        g_strlcat(newxmp, ".xmp", DT_MAX_PATH_LEN);

        GFile *goldxmp = g_file_new_for_path(oldxmp);
        GFile *gnewxmp = g_file_new_for_path(newxmp);

        if (g_file_test(oldxmp, G_FILE_TEST_EXISTS))
          (void)g_file_move(goldxmp, gnewxmp, 0, NULL, NULL, NULL, NULL);

        g_object_unref(goldxmp);
        g_object_unref(gnewxmp);
      }
      sqlite3_reset(duplicates_stmt);
      sqlite3_clear_bindings(duplicates_stmt);

      // then update database and cache
      // if update was performed in above loop, dt_image_path_append_version()
      // would return wrong version!
      while (dup_list)
      {
        long int id = GPOINTER_TO_INT(dup_list->data);
        const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, id);
        dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
        img->film_id = filmid;
        // write through to db, but not to xmp
        dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
        dt_image_cache_read_release(darktable.image_cache, img);
        dup_list = g_list_delete_link(dup_list, dup_list);
      }
      g_list_free(dup_list);
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
  gchar srcpath[DT_MAX_PATH_LEN] = {0};
  gchar *newdir = NULL;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select folder from film_rolls where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    newdir = g_strdup((gchar *) sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);

  if(newdir)
  {
    dt_image_full_path(imgid, srcpath, DT_MAX_PATH_LEN);
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
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "insert into images "
                                  "(id, group_id, film_id, width, height, filename, maker, model, lens, exposure, "
                                  "aperture, iso, focal_length, focus_distance, datetime_taken, flags, "
                                  "output_width, output_height, crop, raw_parameters, raw_denoise_threshold, "
                                  "raw_auto_bright_threshold, raw_black, raw_maximum, "
                                  "caption, description, license, sha1sum, orientation, histogram, lightmap, "
                                  "longitude, latitude, color_matrix, colorspace) "
                                  "select null, group_id, ?1 as film_id, width, height, filename, maker, model, lens, "
                                  "exposure, aperture, iso, focal_length, focus_distance, datetime_taken, "
                                  "flags, width, height, crop, raw_parameters, raw_denoise_threshold, "
                                  "raw_auto_bright_threshold, raw_black, raw_maximum, "
                                  "caption, description, license, sha1sum, orientation, histogram, lightmap, "
                                  "longitude, latitude, color_matrix, colorspace "
                                  "from images where id = ?2", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "select a.id from images as a join images as b where "
                                  "a.film_id = ?1 and a.filename = b.filename and "
                                  "b.id = ?2 order by a.id desc", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);

      if(sqlite3_step(stmt) == SQLITE_ROW)
        newid = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);

      if(newid != -1)
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "insert into color_labels (imgid, color) select ?1, color from "
                                    "color_labels where imgid = ?2", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "insert into meta_data (id, key, value) select ?1, key, value "
                                    "from meta_data where id = ?2", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "insert into tagged_images (imgid, tagid) select ?1, tagid from "
                                    "tagged_images where imgid = ?2", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "update tagxtag set count = count + 1 where "
                                    "(id1 in (select tagid from tagged_images where imgid = ?1)) or "
                                    "(id2 in (select tagid from tagged_images where imgid = ?1))",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        // write xmp file
        dt_image_write_sidecar_file(newid);
      }
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

// *******************************************************
// xmp stuff
// *******************************************************

void dt_image_write_sidecar_file(int imgid)
{
  // TODO: compute hash and don't write if not needed!
  // write .xmp file
  if(imgid > 0 && dt_conf_get_bool("write_sidecar_files"))
  {
    char filename[DT_MAX_PATH_LEN+8];
    dt_image_full_path(imgid, filename, DT_MAX_PATH_LEN);
    dt_image_path_append_version(imgid, filename, DT_MAX_PATH_LEN);
    char *c = filename + strlen(filename);
    sprintf(c, ".xmp");
    dt_exif_xmp_write(imgid, filename);
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
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "select imgid from selected_images", -1, &stmt, NULL);
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
    // Delete all existing .xmp files.
    glob_t *globbuf = g_malloc(sizeof(glob_t));

    gchar *fname = g_strdup(pathname);
    gchar pattern[1024];
    g_snprintf(pattern, 1024, "%s", pathname);
    char *c1 = pattern + strlen(pattern);
    while(*c1 != '.' && c1 > pattern) c1--;
    g_snprintf(c1, pattern + 1024 - c1, "_*");
    char *c2 = fname + strlen(fname);
    while(*c2 != '.' && c2 > fname) c2--;
    g_snprintf(c1+2, pattern + 1024 - c1 - 2, "%s.xmp", c2);

    if (!glob(pattern, 0, NULL, globbuf))
    {
      for (size_t i=0; i < globbuf->gl_pathc; i++)
      {
        (void)g_unlink(globbuf->gl_pathv[i]);
      }
      globfree(globbuf);
    }

    sqlite3_stmt *stmt;
    gchar *imgfname = g_path_get_basename(pathname);
    gchar *imgpath = g_path_get_dirname(pathname);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "select id from images where film_id in (select id from film_rolls "
                                "where folder = ?1) and filename = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, imgpath, strlen(imgpath),
                               SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, imgfname, strlen(imgfname),
                               SQLITE_TRANSIENT);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int imgid = sqlite3_column_int(stmt, 0);
      dt_image_write_sidecar_file(imgid);
    }
    sqlite3_finalize(stmt);
    g_free(fname);
    g_free(imgfname);
    g_free(imgpath);
    g_free(globbuf);
  }
}

#if GLIB_CHECK_VERSION (2, 26, 0)
void dt_image_add_time_offset(const int imgid, const long int offset)
{
  const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
  if (!cimg)
    return;

  // get the datetime_taken and calculate the new time
  gint  year;
  gint  month;
  gint  day;
  gint  hour;
  gint  minute;
  gint  seconds;

  if (sscanf(cimg->exif_datetime_taken, "%d:%d:%d %d:%d:%d",
             (int*)&year, (int*)&month, (int*)&day,
             (int*)&hour,(int*)&minute,(int*)&seconds) != 6)
  {
    fprintf(stderr,"broken exif time in db, '%s', imgid %d\n", cimg->exif_datetime_taken, imgid);
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
    dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
    g_strlcpy(img->exif_datetime_taken, datetime, 20);
    dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);
  }

  dt_image_cache_read_release(darktable.image_cache, cimg);
  g_free(datetime);
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
