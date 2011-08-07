/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.

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
#ifndef DT_MIPMAP_CACHE_H
#define DT_MIPMAP_CACHE_H

#include "common/cache.h"


typedef enum dt_mipmap_size_t
{
  DT_MIPMAP_0    = 0,
  DT_MIPMAP_1    = 1,
  DT_MIPMAP_2    = 2,
  DT_MIPMAP_4    = 3,
  DT_MIPMAP_FULL = 4,
  DT_MIPMAP_NONE = 5
}
dt_mipmap_size_t;


typedef struct dt_mipmap_buffer_t
{
  int32_t width, height;
  void *buf;
}
dt_mipmap_buffer_t;

typedef struct dt_mipmap_cache_t
{
  // TODO: use this cache to dynamically allocate mipmap buffers
  //       (so it will grow slowly)
  // TODO: one cache per mipmap scale!
  // TODO: implement our own garbage collection to free large buffers first!
  // TODO: need clever hashing img->mip (not just id..?)
  // TODO: say folder id + img filename hash
  dt_cache_t *cache;
}
dt_mipmap_cache_t;

void dt_mipmap_cache_init   (dt_image_cache_t *cache);
void dt_mipmap_cache_cleanup(dt_image_cache_t *cache);
void dt_mipmap_cache_print  (dt_image_cache_t *cache);

// get a buffer for reading. this has best effort/bad luck
// semantics, so you'll get a smaller mipmap or NULL if
// your request is not found in the cache.
const dt_mipmap_buffer_t*
dt_image_cache_read_get(dt_cache_image_t *cache, const uint32_t key, dt_mipmap_size_t mip);

// you need to hold a read lock for this buffer before you lock it for
// writing:
dt_mipmap_buffer_t*
dt_image_cache_write_get(dt_cache_image_t *cache, const uint32_t key, dt_mipmap_size_t mip);

// TODO: pass mip, too? pack it all into buffer_t? return bucket instead?
// drop a read lock
void dt_image_cache_read_release (dt_cache_image_t *cache, const uint32_t key);
// drop a write lock, read will still remain.
void dt_image_cache_write_release(dt_cache_image_t *cache, const uint32_t key);

// TODO: read_get_blocking
// TODO: prefetch (no lock, no return)
// TODO: lock_if_available



#endif
