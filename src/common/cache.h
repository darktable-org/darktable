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

#include <stdbool.h> // for bool
#include <stddef.h>  // for size_t
#include <stdint.h>  // for uint32_t, int32_t

typedef struct dt_cache_entry_t dt_cache_entry_t;
typedef struct dt_cache_t dt_cache_t;

typedef void((*dt_cache_allocate_callback_t)(void *, dt_cache_entry_t *entry));
typedef void((*dt_cache_cleanup_callback_t)(void *, dt_cache_entry_t *entry));

void dt_cache_entry_set_data(dt_cache_entry_t *entry, void *data);
void *dt_cache_entry_get_data(dt_cache_entry_t *entry);

void dt_cache_entry_set_cost(dt_cache_entry_t *entry, size_t cost);
size_t dt_cache_entry_get_cost(dt_cache_entry_t *entry);

uint32_t dt_cache_entry_get_key(dt_cache_entry_t *entry);

bool dt_cache_entry_locked_writer(dt_cache_entry_t *entry);

// entry size is only used if alloc callback is 0
dt_cache_t *dt_cache_init(size_t entry_size, size_t cost_quota);
void dt_cache_cleanup(dt_cache_t *cache);

void dt_cache_set_allocate_callback(dt_cache_t *cache, dt_cache_allocate_callback_t allocate,
                                    void *allocate_data);

void dt_cache_set_cleanup_callback(dt_cache_t *cache, dt_cache_cleanup_callback_t cleanup, void *cleanup_data);

dt_cache_cleanup_callback_t dt_cache_get_cleanup_callback(dt_cache_t *cache);

float dt_cache_get_usage_percentage(dt_cache_t *cache);
size_t dt_cache_get_cost(dt_cache_t *cache);
size_t dt_cache_get_cost_quota(dt_cache_t *cache);

// returns a slot in the cache for this key (newly allocated if need be), locked according to mode (r, w)
#define dt_cache_get(A, B, C)  dt_cache_get_with_caller(A, B, C, __FILE__, __LINE__)
dt_cache_entry_t *dt_cache_get_with_caller(dt_cache_t *cache, const uint32_t key, char mode, const char *file, int line);
// same but returns 0 if not allocated yet (both will block and wait for entry rw locks to be released)
dt_cache_entry_t *dt_cache_testget(dt_cache_t *cache, const uint32_t key, char mode);
// degrades the caller's write-side acquisition to a read-side acquisition
void dt_cache_downgrade(dt_cache_t *cache, dt_cache_entry_t *entry);
// release a lock on a cache entry.
void dt_cache_release(dt_cache_t *cache, dt_cache_entry_t *entry, char mode);

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
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
