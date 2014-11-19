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

#ifndef DT_COMMON_CACHE_H
#define DT_COMMON_CACHE_H

#include <inttypes.h>
#include <stddef.h>

struct dt_cache_segment_t;
struct dt_cache_bucket_t;

typedef struct dt_cache_t
{
  uint32_t segment_shift;
  uint32_t segment_mask;
  uint32_t bucket_mask;
  struct dt_cache_segment_t *segments;
  struct dt_cache_bucket_t  *table;

  int32_t lru, mru;
  int cache_mask;
  int optimize_cacheline;
  size_t cost;
  size_t cost_quota;
  // one fat lru lock, no use locking segments and possibly rolling back changes.
  uint32_t lru_lock;

  // callback functions for cache misses/garbage collection
  // allocate should return != 0 if a write lock on alloc is needed.
  // this might be useful for cases where the allocation takes a lot of time and you don't want
  // the hashtable spinlocks to block and wait for it.
  int32_t (*allocate)(void *userdata, const uint32_t key, size_t *cost, void **payload);
  void    (*cleanup) (void *userdata, const uint32_t key, void *payload);
  void *allocate_data;
  void *cleanup_data;
}
dt_cache_t;


void dt_cache_init(dt_cache_t *cache, const int32_t capacity, const int32_t num_threads, size_t cache_line_size, size_t cost_quota);
void dt_cache_cleanup(dt_cache_t *cache);

// don't do memory allocation, but assign static memory to the buckets, given
// in this contiguous block of memory.
// buf has to be large enough to hold the cache's capacity * stride bytes.
void dt_cache_static_allocation(dt_cache_t *cache, uint8_t *buf, const uint32_t stride);

static inline void
dt_cache_set_allocate_callback(
  dt_cache_t *cache,
  int32_t (*allocate)(void*, const uint32_t, size_t*, void**),
  void *allocate_data)
{
  cache->allocate = allocate;
  cache->allocate_data = allocate_data;
}
static inline void
dt_cache_set_cleanup_callback(
  dt_cache_t *cache,
  void (*cleanup)(void*, const uint32_t, void*),
  void *cleanup_data)
{
  cache->cleanup = cleanup;
  cache->cleanup_data = cleanup_data;
}

void  dt_cache_read_release(dt_cache_t *cache, const uint32_t key);
// augments an already acquired read lock to a write lock. blocks until
// all readers have released the image.
void* dt_cache_write_get    (dt_cache_t *cache, const uint32_t key);
void  dt_cache_write_release(dt_cache_t *cache, const uint32_t key);

// gets you a slot in the cache for the given key, read locked.
// will only contain valid data if it was there before.
void*   dt_cache_read_get(dt_cache_t *cache, const uint32_t key);
void*   dt_cache_read_testget(dt_cache_t *cache, const uint32_t key);
int32_t dt_cache_contains(const dt_cache_t *const cache, const uint32_t key);
// returns 0 on success, 1 if the key was not found.
int32_t dt_cache_remove(dt_cache_t *cache, const uint32_t key);
// removes from the end of the lru list, until the fill ratio
// of the hashtable goes below the given parameter, in terms
// of the user defined cost measure.
int32_t dt_cache_gc(dt_cache_t *cache, const float fill_ratio);

// returns the number of elements currently stored in the cache.
// O(N), where N is the total capacity. don't use!
uint32_t dt_cache_size(const dt_cache_t *const cache);

// returns the maximum capacity of this cache:
static inline uint32_t
dt_cache_capacity(dt_cache_t *cache)
{
  return cache->bucket_mask + 1;
}

// very verbose dump of the cache contents
void dt_cache_print(dt_cache_t *cache);
// only print currently locked buckets:
void dt_cache_print_locked(dt_cache_t *cache);

// replace data pointer, cleanup has to be done by the user.
void dt_cache_realloc(dt_cache_t *cache, const uint32_t key, const size_t cost, void *data);

// iterate over all currently contained data blocks.
// not thread safe! only use this for init/cleanup!
// returns non zero the first time process() returns non zero.
int
dt_cache_for_all(
  dt_cache_t *cache,
  int (*process)(const uint32_t key, const void *data, void *user_data),
  void *user_data);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
