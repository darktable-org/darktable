/*
    This file is part of darktable,
    copyright (c) 2014 johannes hanika.

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

#include "common/cache.h"

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <assert.h>
#include <sched.h>

// this implements a concurrent LRU cache using
// a concurrent doubly linked list

static void dt_cache_sleep_ms(uint32_t ms)
{
  g_usleep(ms * 1000u);
}

void dt_cache_init(
    dt_cache_t *cache,
    const int32_t capacity,
    const int32_t num_threads,
    size_t cache_line_size,
    size_t cost_quota)
{
  cache->capacity = capacity;
  cache->cost = 0;
  cache->cost_quota = cost_quota;
  dtpthread_mutex_init(&cache->lock, 0);
  cache->allocate = 0;
  cache->allocate_data = 0;
  cache->cleanup = 0;
  cache->cleanup_data = 0;
  cache->hashtable = g_hash_table_new(0, 0);
}

void dt_cache_cleanup(dt_cache_t *cache)
{
  g_hash_table_destroy(cache->hashtable);
  // TODO:
  g_list_free_all(cache->lru);
}

void dt_cache_static_allocation(dt_cache_t *cache, uint8_t *buf, const uint32_t stride)
{
  const int num_buckets = cache->bucket_mask + 1;
  for(int k = 0; k < num_buckets; k++)
  {
    cache->table[k].data = (void *)(buf + k * stride);
  }
}


int32_t dt_cache_contains(const dt_cache_t *const cache, const uint32_t key)
{
  return g_hash_table_contains(cache->hashtable, GINT_TO_POINTER(key));
}


uint32_t dt_cache_size(const dt_cache_t *const cache)
{
  return cache->size;
}

int dt_cache_for_all(
    dt_cache_t *cache,
    int (*process)(const uint32_t key, const void *data, void *user_data),
    void *user_data)
{
  dt_pthread_lock(&cache->lock);
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, cache->hashtable);
  while (g_hash_table_iter_next (&iter, &key, &value))
  {
    // TODO: needs to be cache entry->data ?
    const int err = process(GPOINTER_TO_INT(key), value, user_data);
    if(err)
    {
      dt_pthread_unlock(&cache->lock);
      return err;
    }
  }
  dt_pthread_unlock(&cache->lock);
  return 0;
}

// return read locked bucket, or NULL if it's not already there.
// never attempt to allocate a new slot.
void *dt_cache_read_testget(dt_cache_t *cache, const uint32_t key)
{
  dt_pthread_mutex_lock(&cache->lock);
  gpointer key, value;
  gboolean res = g_hash_table_lookup_extended(
      cache->hash_table, GINT_TO_POINTER(key), &key, &value);
  dt_pthread_mutex_unlock(&cache->lock);
  if(res) return value; // XXX entry->data
  return 0;
}

// if found, the data void* is returned. if not, it is set to be
// the given *data and a new hash table entry is created, which can be
// found using the given key later on.
//
void *dt_cache_read_get(dt_cache_t *cache, const uint32_t key)
{
  dt_pthread_mutex_lock(&cache->lock);
  gpointer orig_key, value;
  gboolean res = g_hash_table_lookup_extended(
      cache->hash_table, GINT_TO_POINTER(key), &orig_key, &value);
  dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
  if(res)
  {
    dt_pthread_mutex_unlock(&cache->lock);
    return value; // XXX entry->data
  }

  // else, not found, need to allocate.

  // first try to clean up.
  // also wait if we can't free more than the requested fill ratio.
  if(cache->cost > 0.8f * cache->cost_quota)
  {
    // need to roll back all the way to get a consistent lock state:
    dt_cache_gc(cache, 0.8f);
  }

  dt_cache_entry_t *entry = (dt_cache_entry_t *)g_malloc(sizeof(dt_cache_entry_t));
  pthread_rwlock_init(&entry->lock);
  entry->data = 0;
  size_t cost = 1;
  int write = 0;
  if(cache->allocate)
    write = cache->allocate(cache->allocate_data, key, &cost, &value->data);
  else // TODO:
    entry->data = dt_alloc_align();
  // write lock in case the caller requests it:
  if(write) pthread_rwlock_wrlock(&entry->lock);
  else      pthread_rwlock_rdlock(&entry->lock);
  cache->cost += cost;

  dt_pthread_mutex_unlock(&cache->lock);
  return entry->data;
}

int dt_cache_remove(dt_cache_t *cache, const uint32_t key)
{
  dt_pthread_mutex_lock(&cache->lock);

  gpointer orig_key, value;
  gboolean res = g_hash_table_lookup_extended(
      cache->hash_table, GINT_TO_POINTER(key), &orig_key, &value);
  dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
  if(!res)
  {
    dt_pthread_mutex_unlock(&cache->lock);
    return 1;
  }
  if(// TODO: pthread trylock

  if(cache->cleanup)
    cache->cleanup(cache->cleanup_data, key, data);

  dt_pthread_mutex_unlock(&cache->lock);
  return 0;
}


int32_t dt_cache_gc(dt_cache_t *cache, const float fill_ratio)
{
// TODO: grab bucket from lru list, remove from hash table and glist, delete it all, call cleanup if need be
  return 0;
}

void dt_cache_read_release(dt_cache_t *cache, const uint32_t key)
{
// TODO: dt_pthread_rwlock_unlock without locking the cache lock! (someone else might be holding it, waiting for us)
}

// augments an already acquired read lock to a write lock. blocks until
// all readers have released the image.
// FIXME: cannot augment without forfeiting the lock!
void *dt_cache_write_get(dt_cache_t *cache, const uint32_t key)
{
  // XXX
}

void dt_cache_realloc(dt_cache_t *cache, const uint32_t key, const size_t cost, void *data)
{
  // XXX whatever, just call remove and re-add?
}

void dt_cache_write_release(dt_cache_t *cache, const uint32_t key)
{
  // XXX
}

void dt_cache_print(dt_cache_t *cache)
{
  fprintf(stderr, "[cache] full entries:\n");
  for(uint32_t k = 0; k <= cache->bucket_mask; k++)
  {
    if(cache->table[k].key != DT_CACHE_EMPTY_KEY)
      fprintf(stderr, "[cache] bucket %d holds key %u with locks r %d w %d\n", k,
              (cache->table[k].key & 0x1fffffff) + 1, cache->table[k].read, cache->table[k].write);
    else
      fprintf(stderr, "[cache] bucket %d is empty with locks r %d w %d\n", k, cache->table[k].read,
              cache->table[k].write);
  }
  fprintf(stderr, "[cache] lru entries:\n");
  dt_cache_lock(&cache->lru_lock);
  int32_t curr = cache->lru;
  while(curr >= 0)
  {
    if(cache->table[curr].key != DT_CACHE_EMPTY_KEY)
      fprintf(stderr, "[cache] bucket %d holds key %u with locks r %d w %d\n", curr,
              (cache->table[curr].key & 0x1fffffff) + 1, cache->table[curr].read, cache->table[curr].write);
    else
    {
      fprintf(stderr, "[cache] bucket %d is empty with locks r %d w %d\n", curr, cache->table[curr].read,
              cache->table[curr].write);
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
      fprintf(stderr, "[cache] bucket[%d|%d] holds key %u with locks r %d w %d\n", i, curr,
              (cache->table[curr].key & 0x1fffffff) + 1, cache->table[curr].read, cache->table[curr].write);
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
