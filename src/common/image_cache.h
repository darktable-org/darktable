/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DT_IMAGE_CACHE_H
#define DT_IMAGE_CACHE_H

#include "common/image.h"

#include <inttypes.h>
#include <pthread.h>

/**
 * image cache to hold temporary representations
 * from sql queries.
 * fast access by img->id via sorted index,
 * which is updated each time a new image is alloc'ed or
 * an old image is kicked.
 * lru list maintained via linked list for fast updates.
 */

typedef struct dt_image_cache_line_t
{
  dt_image_t image;
  dt_image_lock_t lock;
  int16_t mru, lru;
}
dt_image_cache_line_t;

typedef struct dt_image_cache_t
{
  pthread_mutex_t mutex;
  int32_t num_lines;
  dt_image_cache_line_t *line;
  int16_t *by_id;
  int16_t lru, mru;
}
dt_image_cache_t;

void dt_image_cache_init(dt_image_cache_t *cache, int32_t entries);
void dt_image_cache_cleanup(dt_image_cache_t *cache);
/** print some debug info. */
void dt_image_cache_print(dt_image_cache_t *cache);

/** returns alloc'ed image (newly or from cache) or NULL on failure.
 * lru is freed instead. there is no explicit interface for free.
 * result will have users lock incremented.
 * init image from db if it was not already loaded. */
dt_image_t *dt_image_cache_get(int32_t id, const char mode);
/** only use for import. */
dt_image_t *dt_image_cache_get_uninited(int32_t id, const char mode);
/** decrements users lock. */
void dt_image_cache_release(dt_image_t *img, const char mode);
/** synches this image and the db entry. */
void dt_image_cache_flush(dt_image_t *img);
/** same as above, but doesn't write the redundant sidecar files. */
void dt_image_cache_flush_no_sidecars(dt_image_t *img);
/** invalidates resources occupied by this image. */
void dt_image_cache_clear(int32_t id);

#endif
