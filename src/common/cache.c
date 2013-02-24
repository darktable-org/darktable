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

#ifndef DT_UNIT_TEST
#include "common/darktable.h"
#endif
#include "common/cache.h"

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <assert.h>
#include <sched.h>

#include <sys/select.h>

// this implements a concurrent LRU cache using
// a concurrent doubly linked list
// and a hopscotch hashmap, source following the paper and
// the additional material (GPLv2+ c++ concurrency package source)
// `Hopscotch Hashing' by Maurice Herlihy, Nir Shavit and Moran Tzafrir

#define DT_CACHE_NULL_DELTA SHRT_MIN
#define DT_CACHE_EMPTY_HASH -1
#define DT_CACHE_EMPTY_KEY  -1
#define DT_CACHE_EMPTY_DATA  NULL


typedef struct dt_cache_bucket_t
{
  int16_t  first_delta;
  int16_t  next_delta;
  int16_t  read;   // number of readers
  int16_t  write;  // number of writers (0 or 1)
  int32_t  lru;    // for garbage collection: lru list
  int32_t  mru;
  int32_t  cost;   // cost associated with this entry (such as byte size)
  uint32_t hash;   // hash of the element
  uint32_t key;    // key of the element
  // due to alignment, we waste 32 bits here
  void*    data;   // actual data
}
dt_cache_bucket_t;

typedef struct dt_cache_segment_t
{
  uint32_t timestamp;
  uint32_t lock;
}
dt_cache_segment_t;


static inline int
dt_cache_testlock(uint32_t *lock)
{
  if(__sync_val_compare_and_swap(lock, 0, 1)) return 1;
  return 0;
}

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

static void
dt_cache_sleep_ms(uint32_t ms)
{
  struct timeval s;
  s.tv_sec = ms / 1000;
  s.tv_usec = (ms % 1000) * 1000U;
  select(0, NULL, NULL, NULL, &s);
}

#if 0
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
#endif

static dt_cache_bucket_t*
get_start_cacheline_bucket(const dt_cache_t *const cache, dt_cache_bucket_t *const bucket)
{
  return bucket - ((bucket - cache->table) & cache->cache_mask);
}

static void
add_cost(dt_cache_t    *cache,
         const int32_t  cost)
{
  __sync_fetch_and_add(&cache->cost, cost);
}

static void
remove_key(dt_cache_t *cache,
           dt_cache_segment_t *segment,
           dt_cache_bucket_t *const from_bucket,
           dt_cache_bucket_t *const key_bucket,
           dt_cache_bucket_t *const prev_key_bucket,
           const uint32_t hash)
{
  // clean up the user data
  if(cache->cleanup)
  {
    // in case cleanup is requested, assume we should set to NULL again.
    cache->cleanup(cache->cleanup_data, key_bucket->key, key_bucket->data);
    key_bucket->data = DT_CACHE_EMPTY_DATA;
  }
  // else: crucially don't release the data pointer (not for dynamic nor static allocation a good idea)
  // key_bucket->data = DT_CACHE_EMPTY_DATA;
  key_bucket->hash = DT_CACHE_EMPTY_HASH;
  key_bucket->key  = DT_CACHE_EMPTY_KEY;

  // keep track of cost
  add_cost(cache, -key_bucket->cost);

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

// unexposed helpers to increase the read lock count.
// the segment needs to be locked by the caller.
static int
dt_cache_bucket_read_testlock(dt_cache_bucket_t *bucket)
{
  if(bucket->write) return 1;
  assert(bucket->read < 0x7ffe);
  assert(bucket->write == 0);
  bucket->read ++;
  return 0;
}
static void
dt_cache_bucket_read_lock(dt_cache_bucket_t *bucket)
{
  assert(bucket->read < 0x7ffe);
  assert(bucket->write == 0);
  bucket->read ++;
}
static void
dt_cache_bucket_read_release(dt_cache_bucket_t *bucket)
{
  assert(bucket->read > 0);
  assert(bucket->write == 0);
  bucket->read --;
}
static int
dt_cache_bucket_write_testlock(dt_cache_bucket_t *bucket)
{
  if(bucket->read > 1) return 1;
  assert(bucket->read == 1);
  assert(bucket->write < 0x7ffe);
  bucket->write ++;
  return 0;
}
static void
dt_cache_bucket_write_lock(dt_cache_bucket_t *bucket)
{
  assert(bucket->read == 1);
  assert(bucket->write < 0x7ffe);
  bucket->write ++;
}
static void
dt_cache_bucket_write_release(dt_cache_bucket_t *bucket)
{
  assert(bucket->read == 1);
  assert(bucket->write > 0);
  bucket->write --;
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
    // upgrade to a write lock in case the user requests it:
    if(cache->allocate(cache->allocate_data, key, &cost, &free_bucket->data))
      dt_cache_bucket_write_lock(free_bucket);
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
    if(cache->allocate(cache->allocate_data, key, &cost, &free_bucket->data))
      dt_cache_bucket_write_lock(free_bucket);
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
          void *swap_data   = free_bucket->data;
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
          relocate_key->data = swap_data;
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
  // FIXME: if switching this on, lru lists need to move, too! (because they work by bucket and not by key)
  cache->optimize_cacheline = 0;//1;
  // No cache_mask offsetting required when not optimizing for cachelines --RAM
  cache->cache_mask = cache->optimize_cacheline ?
                      cache_line_size / sizeof(dt_cache_bucket_t) - 1 : 0;
  cache->segment_mask = adj_num_threads - 1;
  // cache->segment_shift = calc_div_shift(nearest_power_of_two(num_threads/(float)adj_num_threads)-1);
  // we want a minimum of four entries, as the hopscotch code below proceeds by disregarding the first bucket in the list,
  // so we need to have some space to jump around. not sure if the implementation could be changed to avoid this.
  const uint32_t adj_init_cap = MAX(4, nearest_power_of_two(MAX(adj_num_threads*2, capacity)));
  const uint32_t num_buckets = adj_init_cap;
  cache->bucket_mask = adj_init_cap - 1;
  uint32_t segment_bits = 0;
  while(cache->segment_mask >> segment_bits) segment_bits++;
  uint32_t sh = 0;
  while(cache->bucket_mask >> (sh+segment_bits)) sh ++;
  cache->segment_shift = sh;

  // fprintf(stderr, "[cache init] segment shift %u segment mask %u\n", cache->segment_shift, cache->segment_mask);

  cache->segments = (dt_cache_segment_t *)dt_alloc_align(64, (cache->segment_mask + 1) * sizeof(dt_cache_segment_t));
  cache->table    = (dt_cache_bucket_t  *)dt_alloc_align(64, num_buckets * sizeof(dt_cache_bucket_t));

  cache->cost = 0;
  cache->cost_quota = cost_quota;
  cache->lru_lock = 0;
  cache->allocate = NULL;
  cache->allocate_data = NULL;
  cache->cleanup = NULL;
  cache->cleanup_data = NULL;

  for(uint32_t k=0; k<=cache->segment_mask; k++)
  {
    cache->segments[k].timestamp = 0;
    cache->segments[k].lock = 0;
  }
  for(uint32_t k=0; k<num_buckets; k++)
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
#ifndef DT_UNIT_TEST
  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] after cache initialization\n");
    dt_print_mem_usage();
  }
#endif
}

void
dt_cache_cleanup(dt_cache_t *cache)
{
  // TODO: make sure data* cleanup stuff is called!
  free(cache->table);
  free(cache->segments);
}

void
dt_cache_static_allocation(
  dt_cache_t *cache,
  uint8_t *buf,
  const uint32_t stride)
{
  const int num_buckets = cache->bucket_mask + 1;
  for(int k=0; k<num_buckets; k++)
  {
    cache->table[k].data = (void *)(buf + k*stride);
  }
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
  const uint32_t num = cache->bucket_mask + 1;
  for(uint32_t k=0; k<num; k++)
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
  for(int k=0; k<=cache->bucket_mask; k++)
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

int
dt_cache_for_all(
  dt_cache_t *cache,
  int (*process)(const uint32_t key, const void *data, void *user_data),
  void *user_data)
{
  // this is not thread safe.
  //dt_cache_lock(&cache->lru_lock);
  int32_t curr = cache->mru;
  while(curr >= 0)
  {
    if(cache->table[curr].key != DT_CACHE_EMPTY_KEY)
    {
      const int err = process(cache->table[curr].key, cache->table[curr].data, user_data);
      if(err) return err;
    }
    if(curr == cache->lru) break;
    int32_t next = cache->table[curr].lru;
    assert(cache->table[next].mru == curr);
    curr = next;
  }
  //dt_cache_unlock(&cache->lru_lock);
  return 0;
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

  if(dt_cache_testlock(&segment->lock))
    return NULL;

  dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
  dt_cache_bucket_t *compare_bucket = start_bucket;
  int16_t next_delta = compare_bucket->first_delta;
  while(next_delta != DT_CACHE_NULL_DELTA)
  {
    compare_bucket += next_delta;
    if(hash == compare_bucket->hash && (key == compare_bucket->key))
    {
      void *rc = compare_bucket->data;
      int err = dt_cache_bucket_read_testlock(compare_bucket);
      dt_cache_unlock(&segment->lock);
      if(err) return NULL;
      // move this to the  most recently used slot, too:
      lru_insert_locked(cache, compare_bucket);
      return rc;
    }
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
  assert(key != DT_CACHE_EMPTY_KEY);

  // this is the blocking variant, we might need to allocate stuff.
  // also we have to retry if failed.

  // just to support different keys:
  const uint32_t hash = key;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);
  dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
  dt_cache_bucket_t *last_bucket = NULL;
  dt_cache_bucket_t *compare_bucket = start_bucket;

retry_cache_full:
  while(1)
  {
    // block and try our luck
    dt_cache_lock(&segment->lock);

    last_bucket = NULL;
    compare_bucket = start_bucket;
    int16_t next_delta = compare_bucket->first_delta;
    while(next_delta != DT_CACHE_NULL_DELTA)
    {
      compare_bucket += next_delta;
      if(hash == compare_bucket->hash && (key == compare_bucket->key))
      {
        void *rc = compare_bucket->data;
        int err = dt_cache_bucket_read_testlock(compare_bucket);
        dt_cache_unlock(&segment->lock);
        // actually all good, just we couldn't get a lock on the bucket.
        if(err) goto wait;
        // move this to the  most recently used slot, too:
        lru_insert_locked(cache, compare_bucket);
        // found and locked:
        return rc;
      }
      last_bucket = compare_bucket;
      next_delta = compare_bucket->next_delta;
    }
    // end of the loop, didn't find it. need to alloc (and keep segment locked)
    break;
wait:
    ;
    // try again in 5 milliseconds
    dt_cache_sleep_ms(5);
  }

  // we will be allocing, so first try to clean up.
  // also wait if we can't free more than the requested fill ratio.
  if(cache->cost > 0.8f * cache->cost_quota)
  {
    dt_cache_unlock(&segment->lock);
    // need to roll back all the way to get a consistent lock state:
    dt_cache_gc(cache, 0.8f);
    goto retry_cache_full;
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
        // goes before add_key, because that might call alloc which might want to set
        // a write lock (which can only be an augmented read lock)
        dt_cache_bucket_read_lock(free_bucket);
        add_key_to_beginning_of_list(cache, start_bucket, free_bucket, hash, key);
        void *data = free_bucket->data;
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
      dt_cache_lock(&cache->lru_lock);
      if(free_max_bucket->hash == DT_CACHE_EMPTY_HASH)
      {
        // try that again if it's still empty
        dt_cache_bucket_read_lock(free_max_bucket);
        add_key_to_end_of_list(cache, start_bucket, free_max_bucket, hash, key, last_bucket);
        void *data = free_max_bucket->data;
        dt_cache_unlock(&segment->lock);
        lru_insert(cache, free_max_bucket);
        dt_cache_unlock(&cache->lru_lock);
        return data;
      }
      dt_cache_unlock(&cache->lru_lock);
    }
    // this could walk outside the range where the segment lock is valid.
    // that's why we abuse the lru lock above to shield grabbing a new bucket
    // at this stage.
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
      dt_cache_lock(&cache->lru_lock);
      if(free_min_bucket->hash == DT_CACHE_EMPTY_HASH)
      {
        dt_cache_bucket_read_lock(free_min_bucket);
        add_key_to_end_of_list(cache, start_bucket, free_min_bucket, hash, key, last_bucket);
        void *data = free_min_bucket->data;
        dt_cache_unlock(&segment->lock);
        lru_insert(cache, free_min_bucket);
        dt_cache_unlock(&cache->lru_lock);
        return data;
      }
      dt_cache_unlock(&cache->lru_lock);
    }
    --free_min_bucket;
  }

  fprintf(stderr, "[cache] failed to find a free spot for new data!\n");
  dt_cache_unlock(&segment->lock);
  return NULL;
  // goto wait;
}

int
dt_cache_remove_bucket(dt_cache_t *cache, const uint32_t num)
{
  const uint32_t hash = num;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);
  dt_cache_lock(&segment->lock);

  dt_cache_bucket_t *const curr_bucket = cache->table + (hash & cache->bucket_mask);
  const uint32_t key = curr_bucket->key;
  dt_cache_unlock(&segment->lock);
  // actually remove by key
  if(key != DT_CACHE_EMPTY_KEY)
    return dt_cache_remove(cache, key);
  else
    return 2;
}

int
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
      // fprintf(stderr, "[cache remove] key not found %u!\n", key);
      dt_cache_unlock(&segment->lock);
      return 1;
    }
    curr_bucket += next_delta;

    if(hash == curr_bucket->hash && key == curr_bucket->key)
    {
      if(curr_bucket->read || curr_bucket->write)
      {
        // fprintf(stderr, "[cache remove] key still in use %u!\n", key);
        dt_cache_unlock(&segment->lock);
        return 1;
      }
      remove_key(cache, segment, start_bucket, curr_bucket, last_bucket, hash);
      if(cache->optimize_cacheline)
        optimize_cacheline_use(cache, segment, curr_bucket);
      // put back into unused part of the cache: remove from lru list.
      dt_cache_unlock(&segment->lock);
      lru_remove_locked(cache, curr_bucket);
      // fprintf(stderr, "[cache remove] freeing %d for %u\n", cost, key);
      return 0;
    }
    last_bucket = curr_bucket;
    next_delta = curr_bucket->next_delta;
  }
  dt_cache_unlock(&segment->lock);
  return 1;
}

#if 0
// debug helper functions, in case we want a big fat lock for dt_cache_gc():
static int
dt_cache_remove_no_lru_lock(dt_cache_t *cache, const uint32_t key)
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
      // fprintf(stderr, "[cache remove] key not found %u!\n", key);
      dt_cache_unlock(&segment->lock);
      return 1;
    }
    curr_bucket += next_delta;

    if(hash == curr_bucket->hash && key == curr_bucket->key)
    {
      if(curr_bucket->read || curr_bucket->write)
      {
        // fprintf(stderr, "[cache remove] key still in use %u!\n", key);
        dt_cache_unlock(&segment->lock);
        return 1;
      }
      void *rc = curr_bucket->data;
      const int32_t cost = curr_bucket->cost;
      remove_key(cache, segment, start_bucket, curr_bucket, last_bucket, hash);
      if(cache->optimize_cacheline)
        optimize_cacheline_use(cache, segment, curr_bucket);
      // put back into unused part of the cache: remove from lru list.
      dt_cache_unlock(&segment->lock);
      lru_remove(cache, curr_bucket);
      // clean up the user data
      if(cache->cleanup)
        cache->cleanup(cache->cleanup_data, key, rc);
      // keep track of cost
      add_cost(cache, -cost);
      // fprintf(stderr, "[cache remove] freeing %d for %u\n", cost, key);
      return 0;
    }
    last_bucket = curr_bucket;
    next_delta = curr_bucket->next_delta;
  }
  dt_cache_unlock(&segment->lock);
  return 1;
}

static int
dt_cache_remove_bucket_no_lru_lock(dt_cache_t *cache, const uint32_t num)
{
  // dt_cache_remove works on key, not bucket number, so translate that:
  const uint32_t hash = num;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);
  dt_cache_lock(&segment->lock);

  dt_cache_bucket_t *const curr_bucket = cache->table + (hash & cache->bucket_mask);
  const uint32_t key = curr_bucket->key;
  dt_cache_unlock(&segment->lock);
  // actually remove by key
  if(key != DT_CACHE_EMPTY_KEY)
    return dt_cache_remove_no_lru_lock(cache, key);
  else
    return 2;
}
#endif

int32_t
dt_cache_gc(dt_cache_t *cache, const float fill_ratio)
{
#if 0
  if(1)//cache->bucket_mask <= 4)
  {
    const int fwd = lru_check_consistency(cache);
    const int bwd = lru_check_consistency_reverse(cache);
    const int cnt = dt_cache_size(cache);
    fprintf(stderr, "[cache gc] pre consistency: %d %d %d\n", fwd, bwd, cnt);
    dt_cache_print_locked(cache);
    fprintf(stderr, "[cache gc] current cost: %u/%u\n", cache->cost, cache->cost_quota);
  }
#endif
  // sorry, bfl
  // dt_cache_lock(&cache->lru_lock);
  int32_t curr;
  // get least recently used bucket
  dt_cache_lock(&cache->lru_lock);
  curr = cache->lru;
  dt_cache_unlock(&cache->lru_lock);
  int i = 0;
  // while still too full:
  while(cache->cost > fill_ratio * cache->cost_quota)
  {
    // this i has to be > parallel threads * sane amount to work on for start up times to work
    // we want to allow at least that many entries in the cache before we start cleaning up
    if(curr < 0 || i > (1<<cache->segment_shift))
    {
      // damn, we walked the whole list and not enough free space,
      // can you believe this? yell out:
      if(cache->cost > fill_ratio * cache->cost_quota)
      {
        // dt_cache_unlock(&cache->lru_lock);
        // fprintf(stderr, "[cache gc] failed to free space!\n");
        // dt_cache_print_locked(cache);
        return 1;
      }
      break;
    }
    // fprintf(stderr, "[cache gc] from %u to %u\n", cache->cost, (uint32_t)(0.8*cache->cost_quota));

    // remove it. takes care of lru, cost, user cleanup, and hashtable
    // this could run into keys being concurrently removed, and will not remove these,
    // nor alter the lru list in that case (could be interleaved with the other thread
    // who is currently doing that)
    //
    // in the very unlikely case the bucket in question got just removed,
    // and the lru not cleaned up yet, but another image already occupies that slot...
    // it will be read locked and we go on. very worst case we clean up the wrong image.
    const int err = dt_cache_remove_bucket(cache, curr);
    // =const int err = dt_cache_remove_bucket_no_lru_lock(cache, curr);
    if(err)
    {
      // fprintf(stderr, "[cache gc] remove failed %d\n", err);
      // in case we failed, try next entry
      dt_cache_lock(&cache->lru_lock);
      curr = cache->table[curr].mru;
      dt_cache_unlock(&cache->lru_lock);
    }
    i++;
  }
  // dt_cache_unlock(&cache->lru_lock);
#if 0
  if(cache->bucket_mask <= 4)
  {
    const int fwd = lru_check_consistency(cache);
    const int bwd = lru_check_consistency_reverse(cache);
    const int cnt = dt_cache_size(cache);
    fprintf(stderr, "[cache gc] consistency: %d %d %d\n", fwd, bwd, cnt);
    dt_cache_print(cache);
  }
#endif
  return 0;
}

void
dt_cache_read_release(dt_cache_t *cache, const uint32_t key)
{
  // just to support different keys:
  const uint32_t hash = key;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);

  dt_cache_lock(&segment->lock);

  dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
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
    next_delta = compare_bucket->next_delta;
  }
  dt_cache_unlock(&segment->lock);
  fprintf(stderr, "[cache] read_release: not locked!\n");
  // this should never happen, so bail out in debug mode:
  assert(0);
}

// augments an already acquired read lock to a write lock. blocks until
// all readers have released the image.
void*
dt_cache_write_get(dt_cache_t *cache, const uint32_t key)
{
  // just to support different keys:
  const uint32_t hash = key;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);

  while(1)
  {
    dt_cache_lock(&segment->lock);

    dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
    dt_cache_bucket_t *compare_bucket = start_bucket;
    int16_t next_delta = compare_bucket->first_delta;
    while(next_delta != DT_CACHE_NULL_DELTA)
    {
      compare_bucket += next_delta;
      if(hash == compare_bucket->hash && (key == compare_bucket->key))
      {
        void *rc = compare_bucket->data;
        int err = dt_cache_bucket_write_testlock(compare_bucket);
        dt_cache_unlock(&segment->lock);
        if(err) goto wait;
        return rc;
      }
      next_delta = compare_bucket->next_delta;
    }
    // didn't find any entry :(
    break;
wait:
    ;
    // try again in 5 milliseconds
    dt_cache_sleep_ms(5);
  }
  dt_cache_unlock(&segment->lock);
  // clear user error, he should hold a read lock already, so this has to be there.
  fprintf(stderr, "[cache] write_get: bucket not found!\n");
  return NULL;
}

void
dt_cache_realloc(dt_cache_t *cache, const uint32_t key, const int32_t cost, void *data)
{
  // just to support different keys:
  const uint32_t hash = key;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);

  dt_cache_lock(&segment->lock);

  dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
  dt_cache_bucket_t *compare_bucket = start_bucket;
  int16_t next_delta = compare_bucket->first_delta;
  while(next_delta != DT_CACHE_NULL_DELTA)
  {
    compare_bucket += next_delta;
    if(hash == compare_bucket->hash && (key == compare_bucket->key))
    {
      if(compare_bucket->write != 1 || compare_bucket->read != 1)
        fprintf(stderr, "[cache realloc] key %u not locked!\n", key);
      // need to have the bucket write locked:
      assert(compare_bucket->write == 1);
      assert(compare_bucket->read == 1);
      compare_bucket->data = data;
      const int32_t cost_diff = cost - compare_bucket->cost;
      compare_bucket->cost = cost;
      add_cost(cache, cost_diff);
      dt_cache_unlock(&segment->lock);
      return;
    }
    next_delta = compare_bucket->next_delta;
  }
  dt_cache_unlock(&segment->lock);
  // clear user error, he should hold a write lock already, so this has to be there.
  fprintf(stderr, "[cache] realloc: bucket for key %u not found!\n", key);
  assert(0);
  return;
}

void
dt_cache_write_release(dt_cache_t *cache, const uint32_t key)
{
  // just to support different keys:
  const uint32_t hash = key;
  dt_cache_segment_t *segment = cache->segments + ((hash >> cache->segment_shift) & cache->segment_mask);

  dt_cache_lock(&segment->lock);

  dt_cache_bucket_t *const start_bucket = cache->table + (hash & cache->bucket_mask);
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
    next_delta = compare_bucket->next_delta;
  }
  dt_cache_unlock(&segment->lock);
  fprintf(stderr, "[cache] write_release: bucket not found!\n");
}

void dt_cache_print(dt_cache_t *cache)
{
  fprintf(stderr, "[cache] full entries:\n");
  for(uint32_t k=0; k<=cache->bucket_mask; k++)
  {
    if(cache->table[k].key != DT_CACHE_EMPTY_KEY)
      fprintf(stderr, "[cache] bucket %d holds key %u with locks r %d w %d\n",
              k, (cache->table[k].key & 0x1fffffff)+1, cache->table[k].read, cache->table[k].write);
    else
      fprintf(stderr, "[cache] bucket %d is empty with locks r %d w %d\n",
              k, cache->table[k].read, cache->table[k].write);
  }
  fprintf(stderr, "[cache] lru entries:\n");
  dt_cache_lock(&cache->lru_lock);
  int32_t curr = cache->lru;
  while(curr >= 0)
  {
    if(cache->table[curr].key != DT_CACHE_EMPTY_KEY)
      fprintf(stderr, "[cache] bucket %d holds key %u with locks r %d w %d\n",
              curr, (cache->table[curr].key & 0x1fffffff)+1, cache->table[curr].read, cache->table[curr].write);
    else
    {
      fprintf(stderr, "[cache] bucket %d is empty with locks r %d w %d\n",
              curr, cache->table[curr].read, cache->table[curr].write);
      // this list should only ever contain valid buffers.
      assert(0);
    }
    if(curr == cache->mru) break;
    int32_t next = cache->table[curr].mru;
    assert(cache->table[next].lru == curr);
    curr = next;
  }
  dt_cache_unlock(&cache->lru_lock);
}

void dt_cache_print_locked(dt_cache_t *cache)
{
  fprintf(stderr, "[cache] locked lru entries:\n");
  dt_cache_lock(&cache->lru_lock);
  int32_t curr = cache->lru;
  int32_t i = 0;
  while(curr >= 0)
  {
    if(cache->table[curr].key != DT_CACHE_EMPTY_KEY && (cache->table[curr].read || cache->table[curr].write))
    {
      fprintf(stderr, "[cache] bucket[%d|%d] holds key %u with locks r %d w %d\n",
              i, curr, (cache->table[curr].key & 0x1fffffff)+1, cache->table[curr].read, cache->table[curr].write);
    }
    if(curr == cache->mru) break;
    int32_t next = cache->table[curr].mru;
    assert(cache->table[next].lru == curr);
    curr = next;
    i++;
  }
  dt_cache_unlock(&cache->lru_lock);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
