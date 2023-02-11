/*
    This file is part of darktable,
    Copyright (C) 2009-2021 darktable developers.

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

#pragma once

#include "common/cache.h"
#include "common/image.h"

typedef struct dt_image_cache_t
{
  dt_cache_t cache;
}
dt_image_cache_t;

// what to do if an image struct is
// released after writing.
typedef enum dt_image_cache_write_mode_t
{
  // always write to database and xmp
  DT_IMAGE_CACHE_SAFE = 0,
  // only write to db and do xmp only during shutdown
  DT_IMAGE_CACHE_RELAXED = 1
}
dt_image_cache_write_mode_t;

void dt_image_cache_init(dt_image_cache_t *cache);
void dt_image_cache_cleanup(dt_image_cache_t *cache);
void dt_image_cache_print(dt_image_cache_t *cache);

// blocks until it gets the image struct with this id for reading.
// also does the sql query if the image is not in cache atm.
// if id < 0, a newly wiped image struct shall be returned (for import).
// this will silently start the garbage collector and free long-unused
// cachelines to free up space if necessary.
// if an entry is swapped out like this in the background, this is the latest
// point where sql and xmp can be synched (unsafe setting).
dt_image_t *dt_image_cache_get(dt_image_cache_t *cache, const int32_t imgid, char mode);

// same as read_get, but doesn't block and returns NULL if the image
// is currently unavailable.
dt_image_t *dt_image_cache_testget(dt_image_cache_t *cache, const int32_t imgid, char mode);

// drops the read lock on an image struct
void dt_image_cache_read_release(dt_image_cache_t *cache, const dt_image_t *img);

// drops the write privileges on an image struct.
// this triggers a write-through to sql, and if the setting
// is present, also to xmp sidecar files (safe setting).
void dt_image_cache_write_release(dt_image_cache_t *cache, dt_image_t *img, dt_image_cache_write_mode_t mode);

// remove the image from the cache
void dt_image_cache_remove(dt_image_cache_t *cache, const int32_t imgid);

// register timestamps in cache
void dt_image_cache_set_change_timestamp(dt_image_cache_t *cache, const int32_t imgid);
void dt_image_cache_set_change_timestamp_from_image(dt_image_cache_t *cache, const int32_t imgid, const int32_t sourceid);
void dt_image_cache_unset_change_timestamp(dt_image_cache_t *cache, const int32_t imgid);
void dt_image_cache_set_export_timestamp(dt_image_cache_t *cache, const int32_t imgid);
void dt_image_cache_set_print_timestamp(dt_image_cache_t *cache, const int32_t imgid);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
