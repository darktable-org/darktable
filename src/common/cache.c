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
#include "common/dtpthread.h"
#include "common/darktable.h"

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <assert.h>

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


// this implements a concurrent LRU cache

void dt_cache_init(
    dt_cache_t *cache,
    size_t entry_size,
    size_t cost_quota)
{
  cache->cost = 0;
  CK_STAILQ_INIT(&(cache->lru));
  cache->entry_size = entry_size;
  cache->cost_quota = cost_quota;
  dt_pthread_mutex_init(&cache->lock, 0);
  cache->allocate = 0;
  cache->allocate_data = 0;
  cache->cleanup = 0;
  cache->cleanup_data = 0;

  ck_ht_init(&(cache->hashtable), CK_HT_MODE_DIRECT, dt_cache_ht_hash, &dt_cache_ht_allocator, 8,
             /* unused */ 0);
}

void dt_cache_cleanup(dt_cache_t *cache)
{
  ck_ht_destroy(&(cache->hashtable));

  struct dt_cache_entry *entry;
  while((entry = CK_STAILQ_FIRST(&(cache->lru))) != NULL)
  {
    CK_STAILQ_REMOVE_HEAD(&(cache->lru), list_entry);

    if(cache->cleanup)
      cache->cleanup(cache->cleanup_data, entry);
    else
      dt_free_align(entry->data);
    dt_pthread_rwlock_destroy(&entry->lock);
    g_slice_free1(sizeof(*entry), entry);
  }

  dt_pthread_mutex_destroy(&cache->lock);
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

  dt_pthread_mutex_lock(&cache->lock);
  ck_ht_entry_t entry;
  ck_ht_hash_t h;

  ck_ht_hash_direct(&h, &(cache->hashtable), key);
  ck_ht_entry_key_set_direct(&entry, key);
  const bool found = ck_ht_get_spmc(&(cache->hashtable), h, &entry);
  dt_pthread_mutex_unlock(&cache->lock);
  return found;
}

int dt_cache_for_all(
    dt_cache_t *cache,
    int (*process)(const uint32_t key, const void *data, void *user_data),
    void *user_data)
{
  dt_pthread_mutex_lock(&cache->lock);
  ck_ht_iterator_t iterator = CK_HT_ITERATOR_INITIALIZER;
  ck_ht_entry_t *cursor;
  while(ck_ht_next(&(cache->hashtable), &iterator, &cursor) == true)
  {
    dt_cache_entry_t *entry = (dt_cache_entry_t *)ck_ht_entry_value_direct(cursor);
    const int err = process(entry->key, entry->data, user_data);
    if(err)
    {
      dt_pthread_mutex_unlock(&cache->lock);
      return err;
    }
  }
  dt_pthread_mutex_unlock(&cache->lock);
  return 0;
}

// return read locked bucket, or NULL if it's not already there.
// never attempt to allocate a new slot.
dt_cache_entry_t *dt_cache_testget(dt_cache_t *cache, const uint32_t key, char mode)
{
  assert((key > 0) /*&& (key < UINTPTR_MAX)*/);

  double start = dt_get_wtime();
  dt_pthread_mutex_lock(&cache->lock);

  void *value = dt_cache_ht_get(&(cache->hashtable), key);

  if(value)
  {
    dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
    // lock the cache entry
    int result;
    if(mode == 'w') result = dt_pthread_rwlock_trywrlock(&entry->lock);
    else            result = dt_pthread_rwlock_tryrdlock(&entry->lock);
    if(result)
    { // need to give up mutex so other threads have a chance to get in between and
      // free the lock we're trying to acquire:
      dt_pthread_mutex_unlock(&cache->lock);
      return 0;
    }

    // bubble up in lru list:
    CK_STAILQ_REMOVE(&(cache->lru), entry, dt_cache_entry, list_entry);
    CK_STAILQ_INSERT_TAIL(&(cache->lru), entry, list_entry);

    dt_pthread_mutex_unlock(&cache->lock);
    double end = dt_get_wtime();
    if(end - start > 0.1)
      fprintf(stderr, "try+ wait time %.06fs mode %c \n", end - start, mode);
    return entry;
  }
  dt_pthread_mutex_unlock(&cache->lock);
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

  double start = dt_get_wtime();
restart:
  dt_pthread_mutex_lock(&cache->lock);

  void *value = dt_cache_ht_get(&(cache->hashtable), key);

  if(value)
  { // yay, found. read lock and pass on.
    dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
    int result;
    if(mode == 'w') result = dt_pthread_rwlock_trywrlock_with_caller(&entry->lock, file, line);
    else            result = dt_pthread_rwlock_tryrdlock_with_caller(&entry->lock, file, line);
    if(result)
    { // need to give up mutex so other threads have a chance to get in between and
      // free the lock we're trying to acquire:
      dt_pthread_mutex_unlock(&cache->lock);
      g_usleep(5);
      goto restart;
    }

    // bubble up in lru list:
    CK_STAILQ_REMOVE(&(cache->lru), entry, dt_cache_entry, list_entry);
    CK_STAILQ_INSERT_TAIL(&(cache->lru), entry, list_entry);

    dt_pthread_mutex_unlock(&cache->lock);

#ifdef _DEBUG
    const pthread_t writer = dt_pthread_rwlock_get_writer(&entry->lock);
    if(mode == 'w')
    {
      assert(pthread_equal(writer, pthread_self()));
    }
    else
    {
      assert(!pthread_equal(writer, pthread_self()));
    }
#endif

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
  int ret = dt_pthread_rwlock_init(&entry->lock, 0);
  if(ret) fprintf(stderr, "rwlock init: %d\n", ret);
  entry->data = 0;
  entry->cost = cache->entry_size;
  entry->key = key;
  entry->_lock_demoting = 0;
  dt_cache_ht_insert(&(cache->hashtable), key, entry);
  // if allocate callback is given, always return a write lock
  int write = ((mode == 'w') || cache->allocate);
  if(cache->allocate)
    cache->allocate(cache->allocate_data, entry);
  else
    entry->data = dt_alloc_align(16, cache->entry_size);
  // write lock in case the caller requests it:
  if(write) dt_pthread_rwlock_wrlock_with_caller(&entry->lock, file, line);
  else      dt_pthread_rwlock_rdlock_with_caller(&entry->lock, file, line);
  cache->cost += entry->cost;

  // put at end of lru list (most recently used):
  CK_STAILQ_INSERT_TAIL(&(cache->lru), entry, list_entry);

  dt_pthread_mutex_unlock(&cache->lock);
  double end = dt_get_wtime();
  if(end - start > 0.1)
    fprintf(stderr, "wait time %.06fs\n", end - start);
  return entry;
}

int dt_cache_remove(dt_cache_t *cache, const uint32_t key)
{
restart:
  dt_pthread_mutex_lock(&cache->lock);

  void *value = dt_cache_ht_get(&(cache->hashtable), key);

  if(!value)
  { // not found in cache, not deleting.
    dt_pthread_mutex_unlock(&cache->lock);
    return 1;
  }

  // need write lock to be able to delete:
  dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
  int result = dt_pthread_rwlock_trywrlock(&entry->lock);
  if(result)
  {
    dt_pthread_mutex_unlock(&cache->lock);
    g_usleep(5);
    goto restart;
  }

  if(entry->_lock_demoting)
  {
    // oops, we are currently demoting (rw -> r) lock to this entry in some thread. do not touch!
    dt_pthread_rwlock_unlock(&entry->lock);
    dt_pthread_mutex_unlock(&cache->lock);
    g_usleep(5);
    goto restart;
  }

  gboolean removed = (true == dt_cache_ht_remove(&(cache->hashtable), key));
  (void)removed; // make non-assert compile happy
  assert(removed);

  CK_STAILQ_REMOVE(&(cache->lru), entry, dt_cache_entry, list_entry);

  if(cache->cleanup)
    cache->cleanup(cache->cleanup_data, entry);
  else
    dt_free_align(entry->data);
  dt_pthread_rwlock_unlock(&entry->lock);
  dt_pthread_rwlock_destroy(&entry->lock);
  cache->cost -= entry->cost;
  g_slice_free1(sizeof(*entry), entry);

  dt_pthread_mutex_unlock(&cache->lock);
  return 0;
}

// best-effort garbage collection. never blocks, never fails. well, sometimes it just doesn't free anything.
void dt_cache_gc(dt_cache_t *cache, const float fill_ratio)
{
  int cnt = 0;
  struct dt_cache_entry *entry, *safe;
  CK_STAILQ_FOREACH_SAFE(entry, &(cache->lru), list_entry, safe)
  {
    cnt++;

    if(cache->cost < cache->cost_quota * fill_ratio) break;

    // if still locked by anyone else give up:
    if(dt_pthread_rwlock_trywrlock(&entry->lock)) continue;

    if(entry->_lock_demoting)
    {
      // oops, we are currently demoting (rw -> r) lock to this entry in some thread. do not touch!
      dt_pthread_rwlock_unlock(&entry->lock);
      continue;
    }

    // delete!
    gboolean removed = (true == dt_cache_ht_remove(&(cache->hashtable), entry->key));
    (void)removed; // make non-assert compile happy
    assert(removed);

    CK_STAILQ_REMOVE(&(cache->lru), entry, dt_cache_entry, list_entry);

    cache->cost -= entry->cost;

    if(cache->cleanup)
      cache->cleanup(cache->cleanup_data, entry);
    else
      dt_free_align(entry->data);
    dt_pthread_rwlock_unlock(&entry->lock);
    dt_pthread_rwlock_destroy(&entry->lock);
    g_slice_free1(sizeof(*entry), entry);
  }

  // ck_ht_gc(&(cache->hashtable), 0, lrand48());
}

void dt_cache_release(dt_cache_t *cache, dt_cache_entry_t *entry)
{
  dt_pthread_rwlock_unlock(&entry->lock);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
