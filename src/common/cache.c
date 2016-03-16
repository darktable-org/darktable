/*
    This file is part of darktable,
    copyright (c) 2014 johannes hanika.
    copyright (c) 2015-2016 LebedevRI

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
#include "common/darktable.h" // for dt_get_wtime, dt_free_align, dt_alloc_...
#include <assert.h>           // for assert
#include <ck_ht.h>            // for ck_ht_hash_direct, ck_ht_entry_key_set...
#include <ck_malloc.h>        // for ck_malloc
#include <ck_rwlock.h>        // for ck_rwlock_write_trylock, ck_rwlock_wri...
#include <ck_spinlock.h>      // for ck_spinlock_fas_unlock, ck_spinlock_fa...
#include <glib.h>             // for g_slice_free1, g_usleep, g_slice_alloc
#include <stdio.h>            // for fprintf, stderr
#include <stdlib.h>           // for free, malloc
#include <sys/queue.h>        // for dt_cache_entry_t::(anonymous), TAILQ_R...

struct dt_cache_entry_t
{
  ck_rwlock_t lock;
  void *data;
  size_t cost;
  uint32_t key;
  TAILQ_ENTRY(dt_cache_entry_t) list_entry;
};

struct dt_cache_t
{
  ck_spinlock_fas_t spinlock; // big fat lock. we're only expecting a couple hand full of cpu threads to use
                              // this concurrently.

  size_t entry_size; // cache line allocation
  size_t cost;       // user supplied cost per cache line (bytes?)
  size_t cost_quota; // quota to try and meet. but don't use as hard limit.

  ck_ht_t hashtable; // stores (key, entry) pairs

  // last element is most recently used, first is about to be kicked from cache.
  // NOTE: CK-based implementation would be better, but it is not yet implemented as of 0.5.1
  TAILQ_HEAD(dt_cache_lru, dt_cache_entry_t) lru;

  // callback functions for cache misses/garbage collection
  dt_cache_allocate_callback_t allocate;
  dt_cache_cleanup_callback_t cleanup;
  void *allocate_data;
  void *cleanup_data;
};

// following macros is based on bsd sys/queue.h, almost no other queue.h has it
#ifndef TAILQ_FOREACH_SAFE
#define TAILQ_FOREACH_SAFE(var, head, field, tvar)                                                           \
  for((var) = TAILQ_FIRST((head)); (var) && ((tvar) = TAILQ_NEXT((var), field), 1); (var) = (tvar))
#endif

static void *dt_cache_ht_malloc(size_t size)
{
  return malloc(size);
}

static void dt_cache_ht_free(void *p, size_t size, bool defer)
{
  (void)size;
  (void)defer;
  free(p);
}

static struct ck_malloc dt_cache_ht_allocator = {.malloc = dt_cache_ht_malloc, .free = dt_cache_ht_free };

static void dt_cache_ht_hash(ck_ht_hash_t *h, const void *key, size_t key_length, uint64_t seed)
{
  const uintptr_t *value = key;

  assert((*value > 0) && (*value < UINTPTR_MAX));

  (void)key_length;
  (void)seed;
  h->value = *value;
  return;
}


void dt_cache_entry_set_data(dt_cache_entry_t *entry, void *data)
{
  entry->data = data;
}

void *dt_cache_entry_get_data(dt_cache_entry_t *entry)
{
  return entry->data;
}

void dt_cache_entry_set_cost(dt_cache_entry_t *entry, size_t cost)
{
  entry->cost = cost;
}

size_t dt_cache_entry_get_cost(dt_cache_entry_t *entry)
{
  return entry->cost;
}

uint32_t dt_cache_entry_get_key(dt_cache_entry_t *entry)
{
  return entry->key;
}

bool dt_cache_entry_locked_writer(dt_cache_entry_t *entry)
{
  return (ck_rwlock_locked_writer(&entry->lock) == true);
}

// this implements a concurrent LRU cache

dt_cache_t *dt_cache_init(size_t entry_size, size_t cost_quota)
{
  dt_cache_t *cache = malloc(sizeof(dt_cache_t));

  ck_spinlock_fas_init(&(cache->spinlock));

  cache->cost = 0;
  TAILQ_INIT(&(cache->lru));
  cache->entry_size = entry_size;
  cache->cost_quota = cost_quota;
  cache->allocate = 0;
  cache->allocate_data = 0;
  cache->cleanup = 0;
  cache->cleanup_data = 0;

  ck_ht_init(&(cache->hashtable), CK_HT_MODE_DIRECT, dt_cache_ht_hash, &dt_cache_ht_allocator, 8,
             /* unused */ 0);

  return cache;
}

void dt_cache_cleanup(dt_cache_t *cache)
{
  ck_ht_destroy(&(cache->hashtable));

  dt_cache_entry_t *entry;
  while((entry = TAILQ_FIRST(&(cache->lru))) != NULL)
  {
    TAILQ_REMOVE(&(cache->lru), entry, list_entry);

    if(cache->cleanup)
      cache->cleanup(cache->cleanup_data, entry);
    else
      dt_free_align(entry->data);

    g_slice_free1(sizeof(*entry), entry);
  }

  free(cache);
}

void dt_cache_set_allocate_callback(dt_cache_t *cache, dt_cache_allocate_callback_t allocate,
                                    void *allocate_data)
{
  cache->allocate = allocate;
  cache->allocate_data = allocate_data;
}

void dt_cache_set_cleanup_callback(dt_cache_t *cache, dt_cache_cleanup_callback_t cleanup, void *cleanup_data)
{
  cache->cleanup = cleanup;
  cache->cleanup_data = cleanup_data;
}

dt_cache_cleanup_callback_t dt_cache_get_cleanup_callback(dt_cache_t *cache)
{
  return cache->cleanup;
}

float dt_cache_get_usage_percentage(dt_cache_t *cache)
{
  return 100.0f * (float)cache->cost / (float)cache->cost_quota;
}

size_t dt_cache_get_cost(dt_cache_t *cache)
{
  return cache->cost;
}

size_t dt_cache_get_cost_quota(dt_cache_t *cache)
{
  return cache->cost_quota;
}

static bool dt_cache_ht_remove(ck_ht_t *ht, uintptr_t key)
{
  assert((key > 0) && (key < UINTPTR_MAX));

  ck_ht_entry_t entry;
  ck_ht_hash_t h;

  ck_ht_hash_direct(&h, ht, key);
  ck_ht_entry_key_set_direct(&entry, key);
  return ck_ht_remove_spmc(ht, h, &entry);
}

static void *dt_cache_ht_get(ck_ht_t *ht, uintptr_t key)
{
  assert((key > 0) && (key < UINTPTR_MAX));

  ck_ht_entry_t entry;
  ck_ht_hash_t h;

  ck_ht_hash_direct(&h, ht, key);
  ck_ht_entry_key_set_direct(&entry, key);
  if(ck_ht_get_spmc(ht, h, &entry) == true) return (void *)ck_ht_entry_value_direct(&entry);

  return NULL;
}

static bool dt_cache_ht_insert(ck_ht_t *ht, const uintptr_t key, const void *value)
{
  assert((key > 0) && (key < UINTPTR_MAX));

  ck_ht_entry_t entry;
  ck_ht_hash_t h;

  ck_ht_hash_direct(&h, ht, key);
  ck_ht_entry_set_direct(&entry, h, key, (uintptr_t)value);
  return ck_ht_put_spmc(ht, h, &entry);
}

int32_t dt_cache_contains(dt_cache_t *cache, const uint32_t key)
{
  assert((key > 0) /*&& (key < UINTPTR_MAX)*/);

  ck_spinlock_fas_lock(&(cache->spinlock));
  ck_ht_entry_t entry;
  ck_ht_hash_t h;

  ck_ht_hash_direct(&h, &(cache->hashtable), key);
  ck_ht_entry_key_set_direct(&entry, key);
  const bool found = ck_ht_get_spmc(&(cache->hashtable), h, &entry);
  ck_spinlock_fas_unlock(&(cache->spinlock));
  return found;
}

int dt_cache_for_all(
    dt_cache_t *cache,
    int (*process)(const uint32_t key, const void *data, void *user_data),
    void *user_data)
{
  ck_spinlock_fas_lock(&(cache->spinlock));
  ck_ht_iterator_t iterator = CK_HT_ITERATOR_INITIALIZER;
  ck_ht_entry_t *cursor;
  while(ck_ht_next(&(cache->hashtable), &iterator, &cursor) == true)
  {
    dt_cache_entry_t *entry = (dt_cache_entry_t *)ck_ht_entry_value_direct(cursor);
    const int err = process(entry->key, entry->data, user_data);
    if(err)
    {
      ck_spinlock_fas_unlock(&(cache->spinlock));
      return err;
    }
  }
  ck_spinlock_fas_unlock(&(cache->spinlock));
  return 0;
}

// return read locked bucket, or NULL if it's not already there.
// never attempt to allocate a new slot.
dt_cache_entry_t *dt_cache_testget(dt_cache_t *cache, const uint32_t key, char mode)
{
  assert((key > 0) /*&& (key < UINTPTR_MAX)*/);
  assert((mode == 'w') || (mode == 'r'));

  double start = dt_get_wtime();
  ck_spinlock_fas_lock(&(cache->spinlock));

  void *value = dt_cache_ht_get(&(cache->hashtable), key);

  if(value)
  {
    dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
    // lock the cache entry
    bool result;
    if(mode == 'w')
      result = ck_rwlock_write_trylock(&entry->lock);
    else
      result = ck_rwlock_read_trylock(&entry->lock);
    if(result == false)
    { // need to give up mutex so other threads have a chance to get in between and
      // free the lock we're trying to acquire:
      ck_spinlock_fas_unlock(&(cache->spinlock));
      return 0;
    }

    // bubble up in lru list:
    TAILQ_REMOVE(&(cache->lru), entry, list_entry);
    TAILQ_INSERT_TAIL(&(cache->lru), entry, list_entry);

    ck_spinlock_fas_unlock(&(cache->spinlock));
    double end = dt_get_wtime();
    if(end - start > 0.1)
      fprintf(stderr, "try+ wait time %.06fs mode %c \n", end - start, mode);
    return entry;
  }
  ck_spinlock_fas_unlock(&(cache->spinlock));
  double end = dt_get_wtime();
  if(end - start > 0.1)
    fprintf(stderr, "try- wait time %.06fs\n", end - start);
  return 0;
}

// if found, the data void* is returned. if not, it is set to be
// the given *data and a new hash table entry is created, which can be
// found using the given key later on.
dt_cache_entry_t *dt_cache_get_with_caller(dt_cache_t *cache, const uint32_t key, char mode, const char *file, int line)
{
  assert((key > 0) /*&& (key < UINTPTR_MAX)*/);
  assert((mode == 'w') || (mode == 'r'));

  double start = dt_get_wtime();
restart:
  ck_spinlock_fas_lock(&(cache->spinlock));

  void *value = dt_cache_ht_get(&(cache->hashtable), key);

  if(value)
  { // yay, found. read lock and pass on.
    dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
    bool result;
    if(mode == 'w')
      result = ck_rwlock_write_trylock(&entry->lock);
    else
      result = ck_rwlock_read_trylock(&entry->lock);
    if(result == false)
    { // need to give up mutex so other threads have a chance to get in between and
      // free the lock we're trying to acquire:
      ck_spinlock_fas_unlock(&(cache->spinlock));
      g_usleep(5);
      goto restart;
    }

    // bubble up in lru list:
    TAILQ_REMOVE(&(cache->lru), entry, list_entry);
    TAILQ_INSERT_TAIL(&(cache->lru), entry, list_entry);

    ck_spinlock_fas_unlock(&(cache->spinlock));

    return entry;
  }

  // else, not found, need to allocate.

  // first try to clean up.
  // also wait if we can't free more than the requested fill ratio.
  if(cache->cost > 0.8f * cache->cost_quota)
  {
    // need to roll back all the way to get a consistent lock state:
    dt_cache_gc(cache, 0.8f);
  }

  // here dies your 32-bit system:
  dt_cache_entry_t *entry = (dt_cache_entry_t *)g_slice_alloc(sizeof(dt_cache_entry_t));
  ck_rwlock_init(&entry->lock);
  entry->data = 0;
  entry->cost = cache->entry_size;
  entry->key = key;
  dt_cache_ht_insert(&(cache->hashtable), key, entry);
  // if allocate callback is given, always return a write lock
  int write = ((mode == 'w') || cache->allocate);
  if(cache->allocate)
    cache->allocate(cache->allocate_data, entry);
  else
    entry->data = dt_alloc_align(16, cache->entry_size);
  // write lock in case the caller requests it:
  if(write)
    ck_rwlock_write_lock(&entry->lock);
  else
    ck_rwlock_read_lock(&entry->lock);
  cache->cost += entry->cost;

  // put at end of lru list (most recently used):
  TAILQ_INSERT_TAIL(&(cache->lru), entry, list_entry);

  ck_spinlock_fas_unlock(&(cache->spinlock));
  double end = dt_get_wtime();
  if(end - start > 0.1)
    fprintf(stderr, "wait time %.06fs\n", end - start);
  return entry;
}

int dt_cache_remove(dt_cache_t *cache, const uint32_t key)
{
restart:
  ck_spinlock_fas_lock(&(cache->spinlock));

  void *value = dt_cache_ht_get(&(cache->hashtable), key);

  if(!value)
  { // not found in cache, not deleting.
    ck_spinlock_fas_unlock(&(cache->spinlock));
    return 1;
  }

  // need write lock to be able to delete:
  dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
  if(ck_rwlock_write_trylock(&entry->lock) == false)
  {
    ck_spinlock_fas_unlock(&(cache->spinlock));
    g_usleep(5);
    goto restart;
  }

  gboolean removed = (true == dt_cache_ht_remove(&(cache->hashtable), key));
  (void)removed; // make non-assert compile happy
  assert(removed);

  TAILQ_REMOVE(&(cache->lru), entry, list_entry);

  if(cache->cleanup)
    cache->cleanup(cache->cleanup_data, entry);
  else
    dt_free_align(entry->data);
  ck_rwlock_write_unlock(&entry->lock);
  cache->cost -= entry->cost;
  g_slice_free1(sizeof(*entry), entry);

  ck_spinlock_fas_unlock(&(cache->spinlock));
  return 0;
}

// best-effort garbage collection. never blocks, never fails. well, sometimes it just doesn't free anything.
void dt_cache_gc(dt_cache_t *cache, const float fill_ratio)
{
  int cnt = 0;
  dt_cache_entry_t *entry, *safe;
  TAILQ_FOREACH_SAFE(entry, &(cache->lru), list_entry, safe)
  {
    cnt++;

    if(cache->cost < cache->cost_quota * fill_ratio) break;

    // if still locked by anyone else give up:
    if(ck_rwlock_write_trylock(&entry->lock) == false) continue;

    // delete!
    gboolean removed = (true == dt_cache_ht_remove(&(cache->hashtable), entry->key));
    (void)removed; // make non-assert compile happy
    assert(removed);

    TAILQ_REMOVE(&(cache->lru), entry, list_entry);

    cache->cost -= entry->cost;

    if(cache->cleanup)
      cache->cleanup(cache->cleanup_data, entry);
    else
      dt_free_align(entry->data);
    ck_rwlock_write_unlock(&entry->lock);
    g_slice_free1(sizeof(*entry), entry);
  }

  // ck_ht_gc(&(cache->hashtable), 0, lrand48());
}

void dt_cache_downgrade(dt_cache_t *cache, dt_cache_entry_t *entry)
{
  assert(ck_rwlock_locked_writer(&entry->lock));
  assert(!ck_rwlock_locked_reader(&entry->lock));

  ck_rwlock_write_downgrade(&entry->lock);
}

void dt_cache_release(dt_cache_t *cache, dt_cache_entry_t *entry, char mode)
{
  assert((mode == 'w') || (mode == 'r'));

  if(mode == 'w')
  {
    assert(ck_rwlock_locked_writer(&entry->lock));
    assert(!ck_rwlock_locked_reader(&entry->lock));
    ck_rwlock_write_unlock(&entry->lock);
  }
  else
  {
    assert(ck_rwlock_locked_reader(&entry->lock));
    assert(!ck_rwlock_locked_writer(&entry->lock));
    ck_rwlock_read_unlock(&entry->lock);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
