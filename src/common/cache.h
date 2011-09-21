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
  int cost;
  int cost_quota;
  // one fat lru lock, no use locking segments and possibly rolling back changes.
  uint32_t lru_lock;

  // callback functions for cache misses/garbage collection
  void* (*allocate)(void *data, const uint32_t key, int32_t *cost);
  void* (*cleanup) (void *data, const uint32_t key, void *payload);
  void *allocate_data;
  void *cleanup_data;
}
dt_cache_t;	


void dt_cache_init(dt_cache_t *cache, const int32_t capacity, const int32_t num_threads, int32_t cache_line_size, int32_t optimize_cacheline);
void dt_cache_cleanup(dt_cache_t *cache);

static inline void
dt_cache_set_allocate_callback(
    dt_cache_t *cache,
    void* (*allocate)(void*, const uint32_t, int32_t*),
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
int32_t dt_cache_contains(const dt_cache_t *const cache, const uint32_t key);
void    dt_cache_remove(dt_cache_t *cache, const uint32_t key);
// removes from the end of the lru list, until the fill ratio
// of the hashtable goes below the given parameter, in terms
// of the user defined cost measure.
void    dt_cache_gc(dt_cache_t *cache, const float fill_ratio);

#endif
