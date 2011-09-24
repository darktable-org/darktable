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

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <assert.h>

#include "common/darktable.h"
#include "common/cache.h"

// this implements a concurrent LRU cache using
// a concurrent doubly linked list
// and a hopscotch hashmap, source following the paper and
// the additional material (GPLv2+ c++ concurrency package source)
// `Hopscotch Hashing' by Maurice Herlihy, Nir Shavit and Moran Tzafrir

#define DT_CACHE_NULL_DELTA SHRT_MIN
#define DT_CACHE_EMPTY_HASH -1
#define DT_CACHE_EMPTY_KEY  -1
#define DT_CACHE_EMPTY_DATA  NULL
#define DT_CACHE_INSERT_RANGE (1024*4)


typedef struct dt_cache_bucket_t
{
  int16_t  first_delta;
  int16_t  next_delta;
  int16_t  read;   // number of readers
  int16_t  write;  // number of writers (0 or 1)
  int32_t  lru;    // for garbage collection: lru list
  int32_t  mru;
  int32_t  cost;   // cost associated with this entry (such as byte size)
  uint32_t hash;
  uint32_t key;
  void*    data;
}
dt_cache_bucket_t;

typedef struct dt_cache_segment_t
{
  uint32_t timestamp;
  uint32_t lock;
}
dt_cache_segment_t;


static inline void
dt_cache_lock(uint32_t *lock)
{
  while(__sync_val_compare_and_swap(lock, 0, 1));
}

static inline void
dt_cache_unlock(uint32_t *lock)
{
  __sync_val_compare_and_swap(lock, 1, 0);
}

static uint32_t
nearest_power_of_two(const uint32_t value)
{
  uint32_t rc = 1;
  while(rc < value) rc <<= 1;
  return rc;
}

static uint32_t
calc_div_shift(const uint32_t value)
{
  uint32_t shift = 0;
  uint32_t curr = 1;
  while (curr < value)
  {
    curr <<= 1;
    shift ++;
  }
  return shift;
}

static dt_cache_bucket_t*
get_start_cacheline_bucket(const dt_cache_t *const cache, dt_cache_bucket_t *const bucket)
{
  return bucket - ((bucket - cache->table) & cache->cache_mask);
}

static void
remove_key(dt_cache_segment_t *segment,
           dt_cache_bucket_t *const from_bucket,
           dt_cache_bucket_t *const key_bucket,
           dt_cache_bucket_t *const prev_key_bucket,
           const uint32_t hash)
{
  key_bucket->hash = DT_CACHE_EMPTY_HASH;
  key_bucket->key  = DT_CACHE_EMPTY_KEY;
  key_bucket->data = DT_CACHE_EMPTY_DATA;

  if(prev_key_bucket == NULL)
  {
    if(key_bucket->next_delta == DT_CACHE_NULL_DELTA)
      from_bucket->first_delta = DT_CACHE_NULL_DELTA;
    else
      from_bucket->first_delta = (from_bucket->first_delta + key_bucket->next_delta);
  }
  else
  {
    if(key_bucket->next_delta == DT_CACHE_NULL_DELTA)
      prev_key_bucket->next_delta = DT_CACHE_NULL_DELTA;
    else
      prev_key_bucket->next_delta = (prev_key_bucket->next_delta + key_bucket->next_delta);
  }
  segment->timestamp ++;
  key_bucket->next_delta = DT_CACHE_NULL_DELTA;
}

static void
add_cost(dt_cache_t    *cache,
         const int32_t  cost)
{
  __sync_fetch_and_add(&cache->cost, cost);
}

static void
add_key_to_beginning_of_list(
    dt_cache_t        *cache,
    dt_cache_bucket_t *const keys_bucket,
    dt_cache_bucket_t *const free_bucket,
    const uint32_t     hash,
    const uint32_t     key)
{
  int32_t cost = 1;
  if(cache->allocate)
  {
    fprintf(stderr, "calling alloc beg\n");
    cache->allocate(cache->allocate_data, key, &cost, &free_bucket->data);
  }
  add_cost(cache, cost);

  free_bucket->key  = key;
  free_bucket->hash = hash;
  free_bucket->cost = cost;

  if(keys_bucket->first_delta == 0)
  {
    if(keys_bucket->next_delta == DT_CACHE_NULL_DELTA)
      free_bucket->next_delta = DT_CACHE_NULL_DELTA;
    else
      free_bucket->next_delta = (int16_t)((keys_bucket + keys_bucket->next_delta) - free_bucket);
    keys_bucket->next_delta = (int16_t)(free_bucket - keys_bucket);
  }
  else
  {
    if(keys_bucket->first_delta == DT_CACHE_NULL_DELTA)
      free_bucket->next_delta = DT_CACHE_NULL_DELTA;
    else
      free_bucket->next_delta = (int16_t)((keys_bucket + keys_bucket->first_delta) - free_bucket);
    keys_bucket->first_delta = (int16_t)(free_bucket - keys_bucket);
  }
}

static void
add_key_to_end_of_list(
    dt_cache_t        *cache,
    dt_cache_bucket_t *const keys_bucket,
    dt_cache_bucket_t *const free_bucket,
    const uint32_t     hash,
    const uint32_t     key,
    dt_cache_bucket_t *const last_bucket)
{
  int32_t cost = 1;
  if(cache->allocate)
  {
    fprintf(stderr, "calling alloc end\n");
    cache->allocate(cache->allocate_data, key, &cost, &free_bucket->data);
  }
  add_cost(cache, cost);

  free_bucket->key  = key;
  free_bucket->hash = hash;
  free_bucket->cost = cost;
  free_bucket->next_delta = DT_CACHE_NULL_DELTA;

  if(last_bucket == NULL)
    keys_bucket->first_delta = (int16_t)(free_bucket - keys_bucket);
  else 
    last_bucket->next_delta = (int16_t)(free_bucket - last_bucket);
}

static void
optimize_cacheline_use(dt_cache_t         *cache,
                       dt_cache_segment_t *segment,
                       dt_cache_bucket_t  *const free_bucket)
{
  dt_cache_bucket_t *const start_cacheline_bucket = get_start_cacheline_bucket(cache, free_bucket);
  dt_cache_bucket_t *const end_cacheline_bucket = start_cacheline_bucket + cache->cache_mask;
  dt_cache_bucket_t *opt_bucket = start_cacheline_bucket;

  do
  {
    if(opt_bucket->first_delta != DT_CACHE_NULL_DELTA)
    {
      dt_cache_bucket_t *relocate_key_last = NULL;
      int curr_delta = opt_bucket->first_delta;
      dt_cache_bucket_t *relocate_key = opt_bucket + curr_delta;
      do
      {
        if( curr_delta < 0 || curr_delta > cache->cache_mask )
        {
          free_bucket->data = relocate_key->data;
          free_bucket->key  = relocate_key->key;
          free_bucket->hash = relocate_key->hash;

          if(relocate_key->next_delta == DT_CACHE_NULL_DELTA)
            free_bucket->next_delta = DT_CACHE_NULL_DELTA;
          else
            free_bucket->next_delta = (int16_t)( (relocate_key + relocate_key->next_delta) - free_bucket );

          if(relocate_key_last == NULL)
            opt_bucket->first_delta = (int16_t)( free_bucket - opt_bucket );
          else
            relocate_key_last->next_delta = (int16_t)( free_bucket - relocate_key_last );

          segment->timestamp ++;
          relocate_key->hash = DT_CACHE_EMPTY_HASH;
          relocate_key->key  = DT_CACHE_EMPTY_KEY;
          relocate_key->data = DT_CACHE_EMPTY_DATA;
          relocate_key->next_delta = DT_CACHE_NULL_DELTA;
          return;
        }

        if(relocate_key->next_delta == DT_CACHE_NULL_DELTA)
          break;
        relocate_key_last = relocate_key;
        curr_delta += relocate_key->next_delta;
        relocate_key += relocate_key->next_delta;
      }
      while(1);
    }
    ++opt_bucket;
  }
  while (opt_bucket <= end_cacheline_bucket);
}


void
dt_cache_init(dt_cache_t *cache, const int32_t capacity, const int32_t num_threads, int32_t cache_line_size, int32_t cost_quota)
{
  const uint32_t adj_num_threads = nearest_power_of_two(num_threads);
  cache->cache_mask = cache_line_size / sizeof(dt_cache_bucket_t) - 1;
  cache->optimize_cacheline = 1;
  cache->segment_mask = adj_num_threads - 1;
  cache->segment_shift = calc_div_shift(nearest_power_of_two(num_threads/adj_num_threads)-1);
  const uint32_t adj_init_cap = nearest_power_of_two(capacity);
  const uint32_t num_buckets = adj_init_cap + DT_CACHE_INSERT_RANGE + 1;
  cache->bucket_mask = adj_init_cap - 1;
  cache->segment_shift = __builtin_clz(cache->bucket_mask) - __builtin_clz(cache->segment_mask);

  cache->segments = (dt_cache_segment_t *)dt_alloc_align(64, (cache->segment_mask + 1) * sizeof(dt_cache_segment_t));
  cache->table    = (dt_cache_bucket_t  *)dt_alloc_align(64, num_buckets * sizeof(dt_cache_bucket_t));

  cache->cost = 0;
  cache->cost_quota = cost_quota;
  cache->lru_lock = 0;
  cache->allocate = NULL;
  cache->allocate_data = NULL;
  cache->cleanup = NULL;
  cache->cleanup_data = NULL;

  for(int k=0;k<=cache->segment_mask;k++)
  {
    cache->segments[k].timestamp = 0;
    cache->segments[k].lock = 0;
  }
  for(int k=0;k<num_buckets;k++)
  {
    cache->table[k].first_delta = DT_CACHE_NULL_DELTA;
    cache->table[k].next_delta  = DT_CACHE_NULL_DELTA;
    cache->table[k].hash        = DT_CACHE_EMPTY_HASH;
    cache->table[k].key         = DT_CACHE_EMPTY_KEY;
    cache->table[k].data        = DT_CACHE_EMPTY_DATA;
    cache->table[k].read        = 0;
    cache->table[k].write       = 0;
    cache->table[k].lru         = -2;
    cache->table[k].mru         = -2;
  }
  cache->lru = cache->mru = -1;
}

void
dt_cache_cleanup(dt_cache_t *cache)
{
  // TODO: make sure data* cleanup stuff is called!
  free(cache->table);
  free(cache->segments);
}


int32_t
dt_cache_contains(const dt_cache_t *const cache, const uint32_t key)
{
  // calculate hash from the key:
  const uint32_t hash = key;
  const dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);

  uint32_t start_timestamp;
  do
  {
    start_timestamp = segment->timestamp;
    const dt_cache_bucket_t *curr_bucket = cache->table + (hash & cache->bucket_mask);
    int16_t next_delta = curr_bucket->first_delta;
    while(next_delta != DT_CACHE_NULL_DELTA)
    {
      curr_bucket += next_delta;
      if(hash == curr_bucket->hash && key == curr_bucket->key) return 1;
      next_delta = curr_bucket->next_delta;
    }
  }
  while(start_timestamp != segment->timestamp);

  return 0;
}


uint32_t
dt_cache_size(const dt_cache_t *const cache)
{
  uint32_t cnt = 0;
  const uint32_t num = cache->bucket_mask + DT_CACHE_INSERT_RANGE;
  for(int k=0;k<num;k++)
  {
    if(cache->table[k].hash != DT_CACHE_EMPTY_HASH) cnt++;
  }
  return cnt;
}

#if 0 // not sure we need this diagnostic tool:
  float
dt_cache_percent_keys_in_cache_line(const dt_cache_t *const cache)
{
  uint32_t total_in_cache = 0;
  uint32_t total = 0;
  for(int k=0;k<=cache->bucket_mask;k++)
  {
    const dt_cache_bucket_t *curr_bucket = cache->table + k;
    if(curr_bucket->first_delta != DT_CACHE_NULL_DELTA)
    {
      const dt_cache_bucket_t *start_cache_line_bucket = get_start_cacheline_bucket(curr_bucket);
      const dt_cache_bucket_t *check_bucket = curr_bucket + curr_bucket->first_delta;
      int curr_dist = curr_bucket->first_delta;
      do
      {
        total++;
        if(check_bucket - start_cache_line_bucket >= 0 && check_bucket - start_cache_line_bucket <= cache->cache_mask) total_in_cache++;
        if(check_bucket->next_delta == DT_CACHE_NULL_DELTA) break;
        curr_dist += check_bucket->next_delta;
        check_bucket += check_bucket->next_delta;
      }
      while(1);
    }
  }
  return (float)total_in_cache/(float)total*100.0f;
}
#endif

// rip out at entry from the lru list.
// must already hold the lock!
void
lru_remove(dt_cache_t        *cache,
           dt_cache_bucket_t *bucket)
{
  if(bucket->mru >= -1 && bucket->lru >= -1)
  {
    if(bucket->lru == -1) cache->lru   = bucket->mru;
    else cache->table[bucket->lru].mru = bucket->mru;
    if(bucket->mru == -1) cache->mru   = bucket->lru;
    else cache->table[bucket->mru].lru = bucket->lru;
  }
  // mark as not in the list:
  bucket->mru = bucket->lru = -2;
}

// insert an entry, must already hold the lock! 
void
lru_insert(dt_cache_t        *cache,
           dt_cache_bucket_t *bucket)
{
  // could use the segment locks for better scalability.
  // would need to roll back changes in proximity after all (up to) three locks have been obtained.
  const int idx = bucket - cache->table;

  // only if it's not in front already:
  if(cache->mru != idx)
  {
    // rip out bucket from lru list, if it's still in there:
    lru_remove(cache, bucket);

    // re-attach to most recently used end:
    bucket->mru = -1;
    bucket->lru = cache->mru;
    if(cache->mru >= 0)
      cache->table[cache->mru].mru = idx;
    cache->mru = idx;
    // be consistent if cache was empty before:
    if(cache->lru == -1) cache->lru = idx;
  }
}

void
lru_remove_locked(dt_cache_t        *cache,
                  dt_cache_bucket_t *bucket)
{
  dt_cache_lock(&cache->lru_lock);
  lru_remove(cache, bucket);
  dt_cache_unlock(&cache->lru_lock);
}

void
lru_insert_locked(dt_cache_t        *cache,
                  dt_cache_bucket_t *bucket)
{
  dt_cache_lock(&cache->lru_lock);
  lru_insert(cache, bucket);
  dt_cache_unlock(&cache->lru_lock);
}

// does a consistency check of the lru list.
// returns how many entries it finds.
// hangs infinitely if the list has cycles.
int32_t
lru_check_consistency(dt_cache_t *cache)
{
  dt_cache_lock(&cache->lru_lock);
  int32_t curr = cache->lru;
  int32_t cnt = 1;
  while(curr >= 0 && curr != cache->mru)
  {
    int32_t next = cache->table[curr].mru;
    assert(cache->table[next].lru == curr);
    curr = next;
    cnt++;
  }
  dt_cache_unlock(&cache->lru_lock);
  return cnt;
}
int32_t
lru_check_consistency_reverse(dt_cache_t *cache)
{
  dt_cache_lock(&cache->lru_lock);
  int32_t curr = cache->mru;
  int32_t cnt = 1;
  while(curr >= 0 && curr != cache->lru)
  {
    int32_t next = cache->table[curr].lru;
    assert(cache->table[next].mru == curr);
    curr = next;
    cnt++;
  }
  dt_cache_unlock(&cache->lru_lock);
  return cnt;
}

// unexposed helpers to increase the read lock count.
// the segment needs to be locked by the caller.
void
dt_cache_bucket_read_lock(dt_cache_bucket_t *bucket)
{
  assert(bucket->read < 0x7ffe);
  assert(bucket->write == 0);
  bucket->read ++;
}
void
dt_cache_bucket_read_release(dt_cache_bucket_t *bucket)
{
  assert(bucket->read > 0);
  assert(bucket->write == 0);
  bucket->read --;
}
void
dt_cache_bucket_write_lock(dt_cache_bucket_t *bucket)
{
  assert(bucket->read == 1);
  assert(bucket->write < 0x7ffe);
  bucket->write ++;
}
void
dt_cache_bucket_write_release(dt_cache_bucket_t *bucket)
{
  assert(bucket->read == 1);
  assert(bucket->write > 0);
  bucket->write --;
}


// return read locked bucket, or NULL if it's not already there.
// never attempt to allocate a new slot.
void*
dt_cache_read_testget(
    dt_cache_t     *cache,
    const uint32_t  key)
{
  // just to support different keys:
  const uint32_t hash = key;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);

  dt_cache_lock(&segment->lock);

  dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
  dt_cache_bucket_t *last_bucket = NULL;
  dt_cache_bucket_t *compare_bucket = start_bucket;
  int16_t next_delta = compare_bucket->first_delta;
  while(next_delta != DT_CACHE_NULL_DELTA)
  {
    compare_bucket += next_delta;
    if(hash == compare_bucket->hash && (key == compare_bucket->key))
    {
      void *rc = compare_bucket->data;
      dt_cache_bucket_read_lock(compare_bucket);
      dt_cache_unlock(&segment->lock);
      // move this to the  most recently used slot, too:
      lru_insert_locked(cache, compare_bucket);
      return rc;
    }
    last_bucket = compare_bucket;
    next_delta = compare_bucket->next_delta;
  }
  dt_cache_unlock(&segment->lock);
  return NULL;
}

// if found, the data void* is returned. if not, it is set to be
// the given *data and a new hash table entry is created, which can be
// found using the given key later on.
// 
void*
dt_cache_read_get(
    dt_cache_t     *cache,
    const uint32_t  key)
{
  // just to support different keys:
  const uint32_t hash = key;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);

  dt_cache_lock(&segment->lock);

  dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
  dt_cache_bucket_t *last_bucket = NULL;
  dt_cache_bucket_t *compare_bucket = start_bucket;
  int16_t next_delta = compare_bucket->first_delta;
  while(next_delta != DT_CACHE_NULL_DELTA)
  {
    compare_bucket += next_delta;
    if(hash == compare_bucket->hash && (key == compare_bucket->key))
    {
      void *rc = compare_bucket->data;
      dt_cache_bucket_read_lock(compare_bucket);
      dt_cache_unlock(&segment->lock);
      // move this to the  most recently used slot, too:
      lru_insert_locked(cache, compare_bucket);
      return rc;
    }
    last_bucket = compare_bucket;
    next_delta = compare_bucket->next_delta;
  }

  if(cache->optimize_cacheline)
  {
    dt_cache_bucket_t *free_bucket = start_bucket;
    dt_cache_bucket_t *start_cacheline_bucket = get_start_cacheline_bucket(cache, start_bucket);
    dt_cache_bucket_t *end_cacheline_bucket   = start_cacheline_bucket + cache->cache_mask;
    do
    {
      if(free_bucket->hash == DT_CACHE_EMPTY_HASH)
      {
        add_key_to_beginning_of_list(cache, start_bucket, free_bucket, hash, key);
        void *data = free_bucket->data;
        dt_cache_bucket_read_lock(free_bucket);
        dt_cache_unlock(&segment->lock);
        lru_insert_locked(cache, free_bucket);
        return data;
      }
      ++free_bucket;
      if(free_bucket > end_cacheline_bucket)
        free_bucket = start_cacheline_bucket;
    }
    while(start_bucket != free_bucket);
  }

  // place key in arbitrary free forward bucket
  dt_cache_bucket_t *max_bucket = start_bucket + (SHRT_MAX-1);
  dt_cache_bucket_t *last_table_bucket = cache->table + cache->bucket_mask;
  if(max_bucket > last_table_bucket)
    max_bucket = last_table_bucket;
  dt_cache_bucket_t *free_max_bucket = start_bucket + (cache->cache_mask + 1);
  while (free_max_bucket <= max_bucket)
  {
    if(free_max_bucket->hash == DT_CACHE_EMPTY_HASH)
    {
      add_key_to_end_of_list(cache, start_bucket, free_max_bucket, hash, key, last_bucket);
      void *data = free_max_bucket->data;
      dt_cache_bucket_read_lock(free_max_bucket);
      dt_cache_unlock(&segment->lock);
      lru_insert_locked(cache, free_max_bucket);
      return data;
    }
    ++free_max_bucket;
  }

  // place key in arbitrary free backward bucket
  dt_cache_bucket_t *min_bucket = start_bucket - (SHRT_MAX-1);
  if(min_bucket < cache->table)
    min_bucket = cache->table;
  dt_cache_bucket_t *free_min_bucket = start_bucket - (cache->cache_mask + 1);
  while (free_min_bucket >= min_bucket)
  {
    if(free_min_bucket->hash == DT_CACHE_EMPTY_HASH)
    {
      add_key_to_end_of_list(cache, start_bucket, free_min_bucket, hash, key, last_bucket);
      void *data = free_min_bucket->data;
      dt_cache_bucket_read_lock(free_min_bucket);
      dt_cache_unlock(&segment->lock);
      lru_insert_locked(cache, free_min_bucket);
      return data;
    }
    --free_min_bucket;
  }
  // TODO: trigger a garbage collection to free some more room!
  // TODO: if fail to insert key, cost will be in an inconsistent state here.
  // dt_cache_unlock(&segment->lock);
  fprintf(stderr, "[cache] failed to find a free spot for new data!\n");
  exit(1);
  return DT_CACHE_EMPTY_DATA;
}


void
dt_cache_remove(dt_cache_t *cache, const uint32_t key)
{
  const uint32_t hash = key;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);
  dt_cache_lock(&segment->lock);

  dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
  dt_cache_bucket_t *last_bucket = NULL;
  dt_cache_bucket_t *curr_bucket = start_bucket;
  int16_t next_delta = curr_bucket->first_delta;
  while(1)
  {
    if(next_delta == DT_CACHE_NULL_DELTA)
    {
      dt_cache_unlock(&segment->lock);
      return;
    }
    curr_bucket += next_delta;

    if(hash == curr_bucket->hash && key == curr_bucket->key)
    {
      assert(curr_bucket->read  == 0);
      assert(curr_bucket->write == 0);
      void *rc = curr_bucket->data;
      const int32_t cost = curr_bucket->cost;
      remove_key(segment, start_bucket, curr_bucket, last_bucket, hash);
      if(cache->optimize_cacheline)
        optimize_cacheline_use(cache, segment, curr_bucket);
      dt_cache_unlock(&segment->lock);
      // put back into unused part of the cache: remove from lru list.
      lru_remove_locked(cache, curr_bucket);
      // clean up the user data
      if(cache->cleanup)
        cache->cleanup(cache->cleanup_data, key, rc);
      // keep track of cost
      add_cost(cache, -cost);
      return;
    }
    last_bucket = curr_bucket;
    next_delta = curr_bucket->next_delta;
  }
  return;
}

void
dt_cache_gc(dt_cache_t *cache, const float fill_ratio)
{
  assert(fill_ratio <= 1.0f);
  // while still too full:
  while(cache->cost > fill_ratio * cache->cost_quota)
  {
    // get least recently used bucket
    dt_cache_lock(&cache->lru_lock);
    dt_cache_bucket_t *bucket = cache->table + cache->lru;
    const uint32_t key = bucket->key;
    dt_cache_unlock(&cache->lru_lock);
    // remove it. takes care of lru, cost, user cleanup, and hashtable
    dt_cache_remove(cache, key);
  }
}

void
dt_cache_read_release(dt_cache_t *cache, const uint32_t key)
{
  // just to support different keys:
  const uint32_t hash = key;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);

  dt_cache_lock(&segment->lock);

  dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
  dt_cache_bucket_t *last_bucket = NULL;
  dt_cache_bucket_t *compare_bucket = start_bucket;
  int16_t next_delta = compare_bucket->first_delta;
  while(next_delta != DT_CACHE_NULL_DELTA)
  {
    compare_bucket += next_delta;
    if(hash == compare_bucket->hash && (key == compare_bucket->key))
    {
      dt_cache_bucket_read_release(compare_bucket);
      dt_cache_unlock(&segment->lock);
      return;
    }
    last_bucket = compare_bucket;
    next_delta = compare_bucket->next_delta;
  }
  fprintf(stderr, "[cache] read_release: bucket not found!\n");
}

// augments an already acquired read lock to a write lock. blocks until
// all readers have released the image.
void*
dt_cache_write_get(dt_cache_t *cache, const uint32_t key)
{
  // just to support different keys:
  const uint32_t hash = key;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);

  dt_cache_lock(&segment->lock);

  dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
  dt_cache_bucket_t *last_bucket = NULL;
  dt_cache_bucket_t *compare_bucket = start_bucket;
  int16_t next_delta = compare_bucket->first_delta;
  while(next_delta != DT_CACHE_NULL_DELTA)
  {
    compare_bucket += next_delta;
    if(hash == compare_bucket->hash && (key == compare_bucket->key))
    {
      void *rc = compare_bucket->data;
      dt_cache_bucket_write_lock(compare_bucket);
      dt_cache_unlock(&segment->lock);
      return rc;
    }
    last_bucket = compare_bucket;
    next_delta = compare_bucket->next_delta;
  }
  // clear user error, he should hold a read lock already, so this has to be there.
  fprintf(stderr, "[cache] write_get: bucket not found!\n");
  return NULL;
}

void
dt_cache_write_release(dt_cache_t *cache, const uint32_t key)
{
  // just to support different keys:
  const uint32_t hash = key;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);

  dt_cache_lock(&segment->lock);

  dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
  dt_cache_bucket_t *last_bucket = NULL;
  dt_cache_bucket_t *compare_bucket = start_bucket;
  int16_t next_delta = compare_bucket->first_delta;
  while(next_delta != DT_CACHE_NULL_DELTA)
  {
    compare_bucket += next_delta;
    if(hash == compare_bucket->hash && (key == compare_bucket->key))
    {
      dt_cache_bucket_write_release(compare_bucket);
      dt_cache_unlock(&segment->lock);
      return;
    }
    last_bucket = compare_bucket;
    next_delta = compare_bucket->next_delta;
  }
  fprintf(stderr, "[cache] write_release: bucket not found!\n");
}

