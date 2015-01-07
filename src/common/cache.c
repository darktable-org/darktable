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
#include "common/dtpthread.h"
#include "common/darktable.h"

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <assert.h>

// this implements a concurrent LRU cache

void dt_cache_init(
    dt_cache_t *cache,
    size_t entry_size,
    size_t cost_quota)
{
  cache->cost = 0;
  cache->lru = 0;
  cache->entry_size = entry_size;
  cache->cost_quota = cost_quota;
  dt_pthread_mutex_init(&cache->lock, 0);
  cache->allocate = 0;
  cache->allocate_data = 0;
  cache->cleanup = 0;
  cache->cleanup_data = 0;
  cache->hashtable = g_hash_table_new(0, 0);
}

void dt_cache_cleanup(dt_cache_t *cache)
{
  g_hash_table_destroy(cache->hashtable);
  GList *l = cache->lru;
  while(l)
  {
    dt_cache_entry_t *entry = (dt_cache_entry_t *)l->data;
    if(cache->cleanup)
      cache->cleanup(cache->cleanup_data, entry);
    else
      dt_free_align(entry->data);
    dt_pthread_rwlock_destroy(&entry->lock);
    g_slice_free1(sizeof(*entry), entry);
    l = g_list_next(l);
  }
  g_list_free(cache->lru);
  dt_pthread_mutex_destroy(&cache->lock);
}

int32_t dt_cache_contains(dt_cache_t *cache, const uint32_t key)
{
  dt_pthread_mutex_lock(&cache->lock);
  int32_t result = g_hash_table_contains(cache->hashtable, GINT_TO_POINTER(key));
  dt_pthread_mutex_unlock(&cache->lock);
  return result;
}

int dt_cache_for_all(
    dt_cache_t *cache,
    int (*process)(const uint32_t key, const void *data, void *user_data),
    void *user_data)
{
  dt_pthread_mutex_lock(&cache->lock);
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, cache->hashtable);
  while (g_hash_table_iter_next (&iter, &key, &value))
  {
    dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
    const int err = process(GPOINTER_TO_INT(key), entry->data, user_data);
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
  gpointer orig_key, value;
  gboolean res;
  int result;
  double start = dt_get_wtime();
  dt_pthread_mutex_lock(&cache->lock);
  res = g_hash_table_lookup_extended(
      cache->hashtable, GINT_TO_POINTER(key), &orig_key, &value);
  if(res)
  {
    dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
    // lock the cache entry
    if(mode == 'w') result = dt_pthread_rwlock_trywrlock(&entry->lock);
    else            result = dt_pthread_rwlock_tryrdlock(&entry->lock);
    if(result)
    { // need to give up mutex so other threads have a chance to get in between and
      // free the lock we're trying to acquire:
      dt_pthread_mutex_unlock(&cache->lock);
      return 0;
    }
    // bubble up in lru list:
    cache->lru = g_list_remove_link(cache->lru, entry->link);
    cache->lru = g_list_concat(cache->lru, entry->link);
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
  gpointer orig_key, value;
  gboolean res;
  int result;
  double start = dt_get_wtime();
restart:
  dt_pthread_mutex_lock(&cache->lock);
  res = g_hash_table_lookup_extended(
      cache->hashtable, GINT_TO_POINTER(key), &orig_key, &value);
  if(res)
  { // yay, found. read lock and pass on.
    dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
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
    cache->lru = g_list_remove_link(cache->lru, entry->link);
    cache->lru = g_list_concat(cache->lru, entry->link);
    dt_pthread_mutex_unlock(&cache->lock);
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
  entry->cost = 1;
  entry->link = g_list_append(0, entry);
  entry->key = key;
  g_hash_table_insert(cache->hashtable, GINT_TO_POINTER(key), entry);
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
  cache->lru = g_list_concat(cache->lru, entry->link);

  dt_pthread_mutex_unlock(&cache->lock);
  double end = dt_get_wtime();
  if(end - start > 0.1)
    fprintf(stderr, "wait time %.06fs\n", end - start);
  return entry;
}

int dt_cache_remove(dt_cache_t *cache, const uint32_t key)
{
  gpointer orig_key, value;
  gboolean res;
  int result;
  dt_cache_entry_t *entry;
restart:
  dt_pthread_mutex_lock(&cache->lock);

  res = g_hash_table_lookup_extended(
      cache->hashtable, GINT_TO_POINTER(key), &orig_key, &value);
  entry = (dt_cache_entry_t *)value;
  if(!res)
  { // not found in cache, not deleting.
    dt_pthread_mutex_unlock(&cache->lock);
    return 1;
  }
  // need write lock to be able to delete:
  result = dt_pthread_rwlock_trywrlock(&entry->lock);
  if(result)
  {
    dt_pthread_mutex_unlock(&cache->lock);
    g_usleep(5);
    goto restart;
  }

  gboolean removed = g_hash_table_remove(cache->hashtable, GINT_TO_POINTER(key));
  (void)removed; // make non-assert compile happy
  assert(removed);
  cache->lru = g_list_delete_link(cache->lru, entry->link);

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
  GList *l = cache->lru;
  int cnt = 0;
  while(l)
  {
    cnt++;
    dt_cache_entry_t *entry = (dt_cache_entry_t *)l->data;
    assert(entry->link->data == entry);
    l = g_list_next(l); // we might remove this element, so walk to the next one while we still have the pointer..
    if(cache->cost < cache->cost_quota * fill_ratio) break;

    // if still locked by anyone else give up:
    if(dt_pthread_rwlock_trywrlock(&entry->lock)) continue;

    // delete!
    g_hash_table_remove(cache->hashtable, GINT_TO_POINTER(entry->key));
    cache->lru = g_list_delete_link(cache->lru, entry->link);
    cache->cost -= entry->cost;

    if(cache->cleanup)
      cache->cleanup(cache->cleanup_data, entry);
    else
      dt_free_align(entry->data);
    dt_pthread_rwlock_unlock(&entry->lock);
    dt_pthread_rwlock_destroy(&entry->lock);
    g_slice_free1(sizeof(*entry), entry);
  }
}

void dt_cache_release(dt_cache_t *cache, dt_cache_entry_t *entry)
{
  dt_pthread_rwlock_unlock(&entry->lock);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
