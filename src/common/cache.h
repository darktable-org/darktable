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
  uint32_t cost_lock;
}
dt_cache_t;	


void dt_cache_init(dt_cache_t *cache, const int32_t capacity, const int32_t num_threads, int32_t cache_line_size, int32_t optimize_cacheline);
void dt_cache_cleanup(dt_cache_t *cache);


// should only ever need to call these (porcelain)


// plumbing:
void*   dt_cache_put(dt_cache_t *cache, const uint32_t key, void *data, const int32_t cost);
int32_t dt_cache_contains(const dt_cache_t *const cache, const uint32_t key);
void*   dt_cache_remove(dt_cache_t *cache, const uint32_t key);
void    dt_cache_gc(dt_cache_t *cache);

#endif
