/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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

#include "common/image_cache.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/image.h"
#include "common/datetime.h"
#include "control/conf.h"
#include "develop/develop.h"

#include <sqlite3.h>
#include <inttypes.h>

static void _image_cache_allocate(void *data,
                             dt_cache_entry_t *entry)
{
  entry->cost = sizeof(dt_image_t);

  dt_image_t *img = g_malloc0(sizeof(dt_image_t));
  dt_image_init(img);
  entry->data = img;
  // load stuff from db and store in cache:
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT mi.id, group_id, film_id, width, height, filename,"
      "       mk.name, md.name, ln.name,"
      "       exposure, aperture, iso, focal_length, datetime_taken, flags,"
      "       crop, orientation, focus_distance, raw_parameters,"
      "       longitude, latitude, altitude, color_matrix, colorspace, version,"
      "       raw_black, raw_maximum, aspect_ratio, exposure_bias,"
      "       import_timestamp, change_timestamp, export_timestamp, print_timestamp,"
      "       output_width, output_height, cm.maker, cm.model, cm.alias,"
      "       wb.name, fl.name, ep.name, mm.name"
      "  FROM main.images AS mi"
      "       LEFT JOIN main.cameras AS cm ON cm.id = mi.camera_id"
      "       LEFT JOIN main.makers AS mk ON mk.id = mi.maker_id"
      "       LEFT JOIN main.models AS md ON md.id = mi.model_id"
      "       LEFT JOIN main.lens AS ln ON ln.id = mi.lens_id"
      "       LEFT JOIN main.whitebalance AS wb ON wb.id = mi.whitebalance_id"
      "       LEFT JOIN main.flash AS fl ON fl.id = mi.flash_id"
      "       LEFT JOIN main.exposure_program AS ep ON ep.id = mi.exposure_program_id"
      "       LEFT JOIN main.metering_mode AS mm ON mm.id = mi.metering_mode_id"
      "  WHERE mi.id = ?1",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, entry->key);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    img->id = sqlite3_column_int(stmt, 0);
    img->group_id = sqlite3_column_int(stmt, 1);
    img->film_id = sqlite3_column_int(stmt, 2);
    img->p_width = img->width = sqlite3_column_int(stmt, 3);
    img->p_height = img->height = sqlite3_column_int(stmt, 4);
    img->crop_x = img->crop_y = img->crop_right = img->crop_bottom = 0;
    img->filename[0] = img->exif_maker[0] = img->exif_model[0] = img->exif_lens[0] = '\0';
    dt_datetime_exif_to_img(img, "");
    char *str;
    str = (char *)sqlite3_column_text(stmt, 5);
    if(str) g_strlcpy(img->filename, str, sizeof(img->filename));
    str = (char *)sqlite3_column_text(stmt, 6);
    if(str) g_strlcpy(img->exif_maker, str, sizeof(img->exif_maker));
    str = (char *)sqlite3_column_text(stmt, 7);
    if(str) g_strlcpy(img->exif_model, str, sizeof(img->exif_model));
    str = (char *)sqlite3_column_text(stmt, 8);
    if(str) g_strlcpy(img->exif_lens, str, sizeof(img->exif_lens));
    img->exif_exposure = sqlite3_column_double(stmt, 9);
    img->exif_aperture = sqlite3_column_double(stmt, 10);
    img->exif_iso = sqlite3_column_double(stmt, 11);
    img->exif_focal_length = sqlite3_column_double(stmt, 12);
    img->exif_datetime_taken = sqlite3_column_int64(stmt, 13);
    img->flags = sqlite3_column_int(stmt, 14);
    img->loader = LOADER_UNKNOWN;
    img->exif_crop = sqlite3_column_double(stmt, 15);
    img->orientation = sqlite3_column_int(stmt, 16);
    img->exif_focus_distance = sqlite3_column_double(stmt, 17);
    if(img->exif_focus_distance >= 0 && img->orientation >= 0) img->exif_inited = TRUE;
    uint32_t tmp = sqlite3_column_int(stmt, 18);
    memcpy(&img->legacy_flip, &tmp, sizeof(dt_image_raw_parameters_t));
    if(sqlite3_column_type(stmt, 19) == SQLITE_FLOAT)
      img->geoloc.longitude = sqlite3_column_double(stmt, 19);
    else
      img->geoloc.longitude = NAN;
    if(sqlite3_column_type(stmt, 20) == SQLITE_FLOAT)
      img->geoloc.latitude = sqlite3_column_double(stmt, 20);
    else
      img->geoloc.latitude = NAN;
    if(sqlite3_column_type(stmt, 21) == SQLITE_FLOAT)
      img->geoloc.elevation = sqlite3_column_double(stmt, 21);
    else
      img->geoloc.elevation = NAN;
    const void *color_matrix = sqlite3_column_blob(stmt, 22);
    if(color_matrix)
      memcpy(img->d65_color_matrix, color_matrix, sizeof(img->d65_color_matrix));
    else
      dt_mark_colormatrix_invalid(&img->d65_color_matrix[0]);
    g_free(img->profile);
    img->profile = NULL;
    img->profile_size = 0;
    img->colorspace = sqlite3_column_int(stmt, 23);
    img->version = sqlite3_column_int(stmt, 24);
    img->raw_black_level = sqlite3_column_int(stmt, 25);
    for(uint8_t i = 0; i < 4; i++) img->raw_black_level_separate[i] = 0;
    img->raw_white_point = sqlite3_column_int(stmt, 26);
    if(sqlite3_column_type(stmt, 27) == SQLITE_FLOAT)
      img->aspect_ratio = sqlite3_column_double(stmt, 27);
    else
      img->aspect_ratio = 0.0;
    if(sqlite3_column_type(stmt, 28) == SQLITE_FLOAT)
      img->exif_exposure_bias = sqlite3_column_double(stmt, 28);
    else
      img->exif_exposure_bias = DT_EXIF_TAG_UNINITIALIZED;
    img->import_timestamp = sqlite3_column_int64(stmt, 29);
    img->change_timestamp = sqlite3_column_int64(stmt, 30);
    img->export_timestamp = sqlite3_column_int64(stmt, 31);
    img->print_timestamp = sqlite3_column_int64(stmt, 32);
    img->final_width = sqlite3_column_int(stmt, 33);
    img->final_height = sqlite3_column_int(stmt, 34);

    // normalized camera names
    str = (char *)sqlite3_column_text(stmt, 35);
    if(str) g_strlcpy(img->camera_maker, str, sizeof(img->camera_maker));
    char *str2 = (char *)sqlite3_column_text(stmt, 36);
    if(str2) g_strlcpy(img->camera_model, str2, sizeof(img->camera_model));
    g_snprintf(img->camera_makermodel, sizeof(img->camera_makermodel), "%s %s", str, str2);
    str = (char *)sqlite3_column_text(stmt, 37);
    if(str) g_strlcpy(img->camera_alias, str, sizeof(img->camera_alias));

    str = (char *)sqlite3_column_text(stmt, 38);
    if(str) g_strlcpy(img->exif_whitebalance, str, sizeof(img->exif_whitebalance));
    str = (char *)sqlite3_column_text(stmt, 39);
    if(str) g_strlcpy(img->exif_flash, str, sizeof(img->exif_flash));
    str = (char *)sqlite3_column_text(stmt, 40);
    if(str) g_strlcpy(img->exif_exposure_program, str, sizeof(img->exif_exposure_program));
    str = (char *)sqlite3_column_text(stmt, 41);
    if(str) g_strlcpy(img->exif_metering_mode, str, sizeof(img->exif_metering_mode));

    dt_color_harmony_get(entry->key, &img->color_harmony_guide);

    // buffer size? colorspace?
    if(img->flags & DT_IMAGE_LDR)
    {
      img->buf_dsc.channels = 4;
      img->buf_dsc.datatype = TYPE_FLOAT;
      img->buf_dsc.cst = IOP_CS_RGB;
    }
    else if(img->flags & DT_IMAGE_HDR)
    {
      if(img->flags & DT_IMAGE_RAW)
      {
        img->buf_dsc.channels = 1;
        img->buf_dsc.datatype = TYPE_FLOAT;
        img->buf_dsc.cst = IOP_CS_RAW;
      }
      else
      {
        img->buf_dsc.channels = 4;
        img->buf_dsc.datatype = TYPE_FLOAT;
        img->buf_dsc.cst = IOP_CS_RGB;
      }
    }
    else
    {
      // raw
      img->buf_dsc.channels = 1;
      img->buf_dsc.datatype = TYPE_UINT16;
      img->buf_dsc.cst = IOP_CS_RAW;
    }
  }
  else
  {
    img->id = NO_IMGID;
    dt_print(DT_DEBUG_ALWAYS,
             "[image_cache_allocate] failed to open image %" PRIu32 " from database: %s",
             entry->key, sqlite3_errmsg(dt_database_get(darktable.db)));
  }
  sqlite3_finalize(stmt);
  img->cache_entry = entry; // init backref
  // could downgrade lock write->read on entry->lock if we were using
  // concurrencykit..
}

static void _image_cache_deallocate(void *data, dt_cache_entry_t *entry)
{
  dt_image_t *img = entry->data;
  g_free(img->profile);
  g_list_free_full(img->dng_gain_maps, g_free);
  g_free(img);
}

void dt_image_cache_init(dt_image_cache_t *cache)
{
  // the image cache does no serialization.
  // (unsafe. data should be in db/xmp, not in any other additional cache,
  // also, it should be relatively fast to get the image_t structs from sql.)
  // TODO: actually an independent conf var?
  //       too large: dangerous and wasteful?
  //       can we get away with a fixed size?
  const uint32_t max_mem = 50 * 1024 * 1024;
  const uint32_t num = (uint32_t)(1.5f * max_mem / sizeof(dt_image_t));
  dt_cache_init(&cache->cache, sizeof(dt_image_t), max_mem);
  dt_cache_set_allocate_callback(&cache->cache, &_image_cache_allocate, cache);
  dt_cache_set_cleanup_callback(&cache->cache, &_image_cache_deallocate, cache);

  dt_print(DT_DEBUG_CACHE, "[image_cache] has %d entries", num);
}

void dt_image_cache_cleanup(dt_image_cache_t *cache)
{
  dt_cache_cleanup(&cache->cache);
}

void dt_image_cache_print(dt_image_cache_t *cache)
{
  dt_print(DT_DEBUG_ALWAYS,
           "[image cache] fill %.2f/%.2f MB (%.2f%%)",
           cache->cache.cost / (1024.0 * 1024.0),
           cache->cache.cost_quota / (1024.0 * 1024.0),
           (float)cache->cache.cost / (float)cache->cache.cost_quota);
}

dt_image_t *dt_image_cache_get(dt_image_cache_t *cache,
                               const dt_imgid_t imgid,
                               const char mode)
{
  if(!dt_is_valid_imgid(imgid))
  {
    dt_print(DT_DEBUG_CACHE, "[dt_image_cache_get] failed as not a valid imgid=%d", imgid);
    return NULL;
  }
  dt_cache_entry_t *entry = dt_cache_get(&cache->cache, imgid, mode);
  ASAN_UNPOISON_MEMORY_REGION(entry->data, sizeof(dt_image_t));
  dt_image_t *img = entry->data;
  img->cache_entry = entry;
  return img;
}

dt_image_t *dt_image_cache_testget(dt_image_cache_t *cache,
                                   const dt_imgid_t imgid,
                                   const char mode)
{
  if(!dt_is_valid_imgid(imgid))
  {
    dt_print(DT_DEBUG_CACHE, "[dt_image_cache_testget] failed as not a valid imgid=%d", imgid);
    return NULL;
  }
  dt_cache_entry_t *entry = dt_cache_testget(&cache->cache, imgid, mode);
  if(!entry)
  {
    dt_print(DT_DEBUG_CACHE, "[dt_image_cache_testget] for imgid=%d failed in dt_cache_testget", imgid);
    return NULL;
  }

  ASAN_UNPOISON_MEMORY_REGION(entry->data, sizeof(dt_image_t));
  dt_image_t *img = entry->data;
  img->cache_entry = entry;
  return img;
}

// drops the read lock on an image struct
void dt_image_cache_read_release(dt_image_cache_t *cache,
                                 const dt_image_t *img)
{
  if(!img || !dt_is_valid_imgid(img->id)) return;
  // just force the dt_image_t struct to make sure it has been locked before.
  dt_cache_release(&cache->cache, img->cache_entry);
}

// drops the write privileges on an image struct.
// this triggers a write-through to sql, and if
// a) mode == DT_IMAGE_CACHE_SAFE
// b) sidecar writing is desired via conf setting
// also to xmp sidecar files.
void dt_image_cache_write_release_info(dt_image_cache_t *cache,
                                       dt_image_t *img,
                                       const dt_image_cache_write_mode_t mode,
                                       const char *info)
{
  if(!img)  // nothing to release
    return;

  if(!dt_is_valid_imgid(img->id))
  {
    dt_cache_release(&cache->cache, img->cache_entry);
    dt_print(DT_DEBUG_ALWAYS,
             "[image_cache_write_release] from `%s`. FATAL invalid image id %d",
             info, img->id);
    return;
  }

  const double start = dt_get_debug_wtime();
  union {
      struct dt_image_raw_parameters_t s;
      uint32_t u;
  } flip;

  if(img->aspect_ratio < .0001)
  {
    if(img->orientation < ORIENTATION_SWAP_XY)
      img->aspect_ratio = (float )img->width / (float )(MAX(1, img->height));
    else
      img->aspect_ratio = (float )img->height / (float )(MAX(1, img->width));
  }

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "UPDATE main.images"
     " SET width = ?1, height = ?2, filename = ?3,"
     "     maker_id = ?4, model_id = ?5, lens_id = ?6, camera_id = ?35,"
     "     exposure = ?7, aperture = ?8, iso = ?9, focal_length = ?10,"
     "     focus_distance = ?11, film_id = ?12, datetime_taken = ?13, flags = ?14,"
     "     crop = ?15, orientation = ?16, raw_parameters = ?17, group_id = ?18,"
     "     longitude = ?19, latitude = ?20, altitude = ?21, color_matrix = ?22,"
     "     colorspace = ?23, raw_black = ?24, raw_maximum = ?25,"
     "     aspect_ratio = ROUND(?26,1), exposure_bias = ?27,"
     "     import_timestamp = ?28, change_timestamp = ?29, export_timestamp = ?30,"
     "     print_timestamp = ?31, output_width = ?32, output_height = ?33,"
     "     whitebalance_id = ?36, flash_id = ?37,"
     "     exposure_program_id = ?38, metering_mode_id = ?39"
     " WHERE id = ?40",
     -1, &stmt, NULL);

  const int32_t maker_id = dt_image_get_camera_maker_id(img->exif_maker);
  const int32_t model_id = dt_image_get_camera_model_id(img->exif_model);
  const int32_t lens_id = dt_image_get_camera_lens_id(img->exif_lens);
  const int32_t whitebalance_id = dt_image_get_whitebalance_id(img->exif_whitebalance);
  const int32_t flash_id = dt_image_get_flash_id(img->exif_flash);
  const int32_t exposure_program_id = dt_image_get_exposure_program_id(img->exif_exposure_program);
  const int32_t metering_mode_id = dt_image_get_metering_mode_id(img->exif_metering_mode);

  // also make sure we update the camera_id and possibly the associated data
  // in cameras table.

  const int32_t camera_id = dt_image_get_camera_id(img->exif_maker, img->exif_model);

  dt_color_harmony_set(img->id, img->color_harmony_guide);

  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->width);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, img->height);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, img->filename, -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, maker_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, model_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, lens_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 35, camera_id);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, img->exif_exposure);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, img->exif_aperture);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, img->exif_iso);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, img->exif_focal_length);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 11, img->exif_focus_distance);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, img->film_id);
  if(img->exif_datetime_taken)
    DT_DEBUG_SQLITE3_BIND_INT64(stmt, 13, img->exif_datetime_taken);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 14, img->flags);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 15, img->exif_crop);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 16, img->orientation);
  flip.s = img->legacy_flip;
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 17, flip.u);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 18, img->group_id);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 19, img->geoloc.longitude);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 20, img->geoloc.latitude);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 21, img->geoloc.elevation);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 22, &img->d65_color_matrix,
                             sizeof(img->d65_color_matrix), SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 23, img->colorspace);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 24, img->raw_black_level);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 25, img->raw_white_point);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 26, img->aspect_ratio);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 27, img->exif_exposure_bias);
  if(img->import_timestamp)
    DT_DEBUG_SQLITE3_BIND_INT64(stmt, 28, img->import_timestamp);
  if(img->change_timestamp)
    DT_DEBUG_SQLITE3_BIND_INT64(stmt, 29, img->change_timestamp);
  if(img->export_timestamp)
    DT_DEBUG_SQLITE3_BIND_INT64(stmt, 30, img->export_timestamp);
  if(img->print_timestamp)
    DT_DEBUG_SQLITE3_BIND_INT64(stmt, 31, img->print_timestamp);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 32, img->final_width);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 33, img->final_height);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 36, whitebalance_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 37, flash_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 38, exposure_program_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 39, metering_mode_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 40, img->id);

  const int rc = sqlite3_step(stmt);
  if(rc != SQLITE_DONE)
    dt_print(DT_DEBUG_ALWAYS,
             "[image_cache_write_release] from `%s' sqlite3 error %d (%s) for imgid %d",
             info,
             rc,
             sqlite3_errmsg(dt_database_get(darktable.db)),
             img->id);
  sqlite3_finalize(stmt);

  if(mode == DT_IMAGE_CACHE_SAFE)
  {
    dt_image_synch_xmp(img->id);
    if(info)
    {
      const double spent = dt_get_debug_wtime() - start;
      dt_print(DT_DEBUG_CACHE,
               "[image_cache_write_release] from `%s', imgid=%i took %.3fs",
               info, img->id, spent);
    }
  }
  dt_cache_release(&cache->cache, img->cache_entry);
}

void dt_image_cache_write_release(dt_image_cache_t *cache,
                                  dt_image_t *img,
                                  const dt_image_cache_write_mode_t mode)
{
  dt_image_cache_write_release_info(cache, img, mode, NULL);
}

// remove the image from the cache
void dt_image_cache_remove(dt_image_cache_t *cache,
                           const dt_imgid_t imgid)
{
  dt_cache_remove(&cache->cache, imgid);
}

/* set timestamps */
void dt_image_cache_set_change_timestamp(dt_image_cache_t *cache,
                                         const dt_imgid_t imgid)
{
  if(!dt_is_valid_imgid(imgid)) return;
  dt_cache_entry_t *entry = dt_cache_get(&cache->cache, imgid, 'w');
  if(!entry) return;
  ASAN_UNPOISON_MEMORY_REGION(entry->data, sizeof(dt_image_t));
  dt_image_t *img = entry->data;
  img->cache_entry = entry;
  img->change_timestamp = dt_datetime_now_to_gtimespan();
  dt_image_cache_write_release(cache, img, DT_IMAGE_CACHE_RELAXED);
}

void dt_image_cache_set_change_timestamp_from_image(dt_image_cache_t *cache,
                                                    const dt_imgid_t imgid,
                                                    const dt_imgid_t sourceid)
{
  if(!dt_is_valid_imgid(imgid) || !dt_is_valid_imgid(sourceid)) return;

  // get source timestamp
  const dt_image_t *simg = dt_image_cache_get(cache, sourceid, 'r');
  const GTimeSpan change_timestamp = simg->change_timestamp;
  dt_image_cache_read_release(cache, simg);

  dt_cache_entry_t *entry = dt_cache_get(&cache->cache, imgid, 'w');
  if(!entry) return;
  ASAN_UNPOISON_MEMORY_REGION(entry->data, sizeof(dt_image_t));
  dt_image_t *img = entry->data;
  img->cache_entry = entry;
  img->change_timestamp = change_timestamp;
  dt_image_cache_write_release(cache, img, DT_IMAGE_CACHE_RELAXED);
}

void dt_image_cache_unset_change_timestamp(dt_image_cache_t *cache,
                                           const dt_imgid_t imgid)
{
  if(!dt_is_valid_imgid(imgid)) return;
  dt_cache_entry_t *entry = dt_cache_get(&cache->cache, imgid, 'w');
  if(!entry) return;
  ASAN_UNPOISON_MEMORY_REGION(entry->data, sizeof(dt_image_t));
  dt_image_t *img = entry->data;
  img->cache_entry = entry;
  img->change_timestamp = 0;
  dt_image_cache_write_release(cache, img, DT_IMAGE_CACHE_RELAXED);
}

void dt_image_cache_set_export_timestamp(dt_image_cache_t *cache,
                                         const dt_imgid_t imgid)
{
  if(!dt_is_valid_imgid(imgid)) return;
  dt_cache_entry_t *entry = dt_cache_get(&cache->cache, imgid, 'w');
  if(!entry) return;
  ASAN_UNPOISON_MEMORY_REGION(entry->data, sizeof(dt_image_t));
  dt_image_t *img = entry->data;
  img->cache_entry = entry;
  img->export_timestamp = dt_datetime_now_to_gtimespan();
  dt_image_cache_write_release(cache, img, DT_IMAGE_CACHE_RELAXED);
}

void dt_image_cache_set_print_timestamp(dt_image_cache_t *cache,
                                        const dt_imgid_t imgid)
{
  if(!dt_is_valid_imgid(imgid)) return;
  dt_cache_entry_t *entry = dt_cache_get(&cache->cache, imgid, 'w');
  if(!entry) return;
  ASAN_UNPOISON_MEMORY_REGION(entry->data, sizeof(dt_image_t));
  dt_image_t *img = entry->data;
  img->cache_entry = entry;
  img->print_timestamp = dt_datetime_now_to_gtimespan();
  dt_image_cache_write_release(cache, img, DT_IMAGE_CACHE_RELAXED);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
