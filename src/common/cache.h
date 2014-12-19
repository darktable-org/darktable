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

#include "common/dtpthread.h"
#include <inttypes.h>
#include <stddef.h>
#include <glib.h>

typedef struct dt_cache_entry_t
{
  void *data;
  size_t cost;
  GList *link;
  dt_pthread_rwlock_t lock;
  uint32_t key;
}
dt_cache_entry_t;

typedef struct dt_cache_t
{
  dt_pthread_mutex_t lock; // big fat lock. we're only expecting a couple hand full of cpu threads to use this concurrently.

  size_t entry_size; // cache line allocation
  size_t cost;       // user supplied cost per cache line (bytes?)
  size_t cost_quota; // quota to try and meet. but don't use as hard limit.

  GHashTable *hashtable; // stores (key, entry) pairs
  GList *lru;            // last element is most recently used, first is about to be kicked from cache.

  // callback functions for cache misses/garbage collection
  void (*allocate)(void *userdata, dt_cache_entry_t *entry);
  void (*cleanup)(void *userdata, dt_cache_entry_t *entry);
  void *allocate_data;
  void *cleanup_data;
}
dt_cache_t;

// entry size is only used if alloc callback is 0
void dt_cache_init(dt_cache_t *cache, size_t entry_size, size_t cost_quota);
void dt_cache_cleanup(dt_cache_t *cache);

static inline void dt_cache_set_allocate_callback(
    dt_cache_t *cache,
    void (*allocate)(void *, dt_cache_entry_t *entry),
    void *allocate_data)
{
  cache->allocate = allocate;
  cache->allocate_data = allocate_data;
}
static inline void dt_cache_set_cleanup_callback(
    dt_cache_t *cache,
    void (*cleanup)(void *, dt_cache_entry_t *entry),
    void *cleanup_data)
{
  cache->cleanup = cleanup;
  cache->cleanup_data = cleanup_data;
}

// returns a slot in the cache for this key (newly allocated if need be), locked according to mode (r, w)
#define dt_cache_get(A, B, C)  dt_cache_get_with_caller(A, B, C, __FILE__, __LINE__)
dt_cache_entry_t *dt_cache_get_with_caller(dt_cache_t *cache, const uint32_t key, char mode, const char *file, int line);
// same but returns 0 if not allocated yet (both will block and wait for entry rw locks to be released)
dt_cache_entry_t *dt_cache_testget(dt_cache_t *cache, const uint32_t key, char mode);
// release a lock on a cache entry. the cache knows which one you mean (r or w).
void dt_cache_release(dt_cache_t *cache, dt_cache_entry_t *entry);

// 0: not contained
int32_t dt_cache_contains(dt_cache_t *cache, const uint32_t key);
// returns 0 on success, 1 if the key was not found.
int32_t dt_cache_remove(dt_cache_t *cache, const uint32_t key);
// removes from the tip of the lru list, until the fill ratio of the hashtable
// goes below the given parameter, in terms of the user defined cost measure.
// will never lock and never fail, but sometimes not free memory (in case all
// is locked)
void dt_cache_gc(dt_cache_t *cache, const float fill_ratio);

// iterate over all currently contained data blocks.
// not thread safe! only use this for init/cleanup!
// returns non zero the first time process() returns non zero.
int dt_cache_for_all(dt_cache_t *cache,
    int (*process)(const uint32_t key, const void *data, void *user_data),
    void *user_data);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
