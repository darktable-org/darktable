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
    pthread_rwlock_destroy(&entry->lock);
    g_free(entry);
    l = g_list_next(l);
  }
  g_list_free(cache->lru);
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
dt_cache_entry_t *dt_cache_read_testget(dt_cache_t *cache, const uint32_t key, char mode)
{
  dt_pthread_mutex_lock(&cache->lock);
  gpointer orig_key, value;
  gboolean res = g_hash_table_lookup_extended(
      cache->hashtable, GINT_TO_POINTER(key), &orig_key, &value);
  if(res)
  {
    dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
    // lock the cache entry
    if(mode == 'w') pthread_rwlock_wrlock(&entry->lock);
    else            pthread_rwlock_rdlock(&entry->lock);
    // bubble up in lru list:
    cache->lru = g_list_remove_link(cache->lru, entry->link);
    cache->lru = g_list_concat(cache->lru, entry->link);
    dt_pthread_mutex_unlock(&cache->lock);
    return entry;
  }
  dt_pthread_mutex_unlock(&cache->lock);
  return 0;
}

// if found, the data void* is returned. if not, it is set to be
// the given *data and a new hash table entry is created, which can be
// found using the given key later on.
dt_cache_entry_t *dt_cache_get(dt_cache_t *cache, const uint32_t key, char mode)
{
  dt_pthread_mutex_lock(&cache->lock);
  gpointer orig_key, value;
  gboolean res = g_hash_table_lookup_extended(
      cache->hashtable, GINT_TO_POINTER(key), &orig_key, &value);
  if(res)
  { // yay, found. read lock and pass on.
    dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
    pthread_rwlock_rdlock(&entry->lock);
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
  dt_cache_entry_t *entry = (dt_cache_entry_t *)g_malloc(sizeof(dt_cache_entry_t));
  pthread_rwlock_init(&entry->lock, 0);
  entry->data = 0;
  entry->cost = 1;
  entry->link = g_list_append(0, &entry);
  entry->key = key;
  // if allocate callback is given, always return a write lock
  int write = ((mode == 'w') || cache->allocate);
  if(cache->allocate)
    cache->allocate(cache->allocate_data, entry);
  else
    entry->data = dt_alloc_align(16, cache->entry_size);
  // write lock in case the caller requests it:
  if(write) pthread_rwlock_wrlock(&entry->lock);
  else      pthread_rwlock_rdlock(&entry->lock);
  cache->cost += entry->cost;

  // put at end of lru list (most recently used):
  cache->lru = g_list_concat(cache->lru, entry->link);

  dt_pthread_mutex_unlock(&cache->lock);
  return entry;
}

int dt_cache_remove(dt_cache_t *cache, const uint32_t key)
{
  dt_pthread_mutex_lock(&cache->lock);

  gpointer orig_key, value;
  gboolean res = g_hash_table_lookup_extended(
      cache->hashtable, GINT_TO_POINTER(key), &orig_key, &value);
  dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
  if(!res)
  { // not found in cache, not deleting.
    dt_pthread_mutex_unlock(&cache->lock);
    return 1;
  }
  // need write lock to be able to delete:
  pthread_rwlock_wrlock(&entry->lock);
  gboolean removed = g_hash_table_remove(cache->hashtable, GINT_TO_POINTER(key));
  (void)removed; // make non-assert compile happy
  assert(removed);
  cache->lru = g_list_delete_link(cache->lru, entry->link);

  if(cache->cleanup)
    cache->cleanup(cache->cleanup_data, entry);
  else
    dt_free_align(entry->data);
  pthread_rwlock_unlock(&entry->lock);
  pthread_rwlock_destroy(&entry->lock);
  cache->cost -= entry->cost;
  g_free(entry);

  dt_pthread_mutex_unlock(&cache->lock);
  return 0;
}

// best-effort garbage collection. never blocks, never fails. well, sometimes it just doesn't free anything.
void dt_cache_gc(dt_cache_t *cache, const float fill_ratio)
{
  dt_pthread_mutex_lock(&cache->lock);
  GList *l = cache->lru;
  while(l)
  {
    dt_cache_entry_t *entry = (dt_cache_entry_t *)l->data;
    l = g_list_next(l); // we might remove this element, so walk to the next one while we still have the pointer..
    if(cache->cost < cache->cost_quota * fill_ratio) break;

    // if still locked by anyone else give up:
    if(pthread_rwlock_trywrlock(&entry->lock)) continue;

    // delete!
    g_hash_table_remove(cache->hashtable, GINT_TO_POINTER(entry->key));
    cache->lru = g_list_delete_link(cache->lru, entry->link);
    cache->cost -= entry->cost;

    if(cache->cleanup)
      cache->cleanup(cache->cleanup_data, entry);
    else
      dt_free_align(entry->data);
    pthread_rwlock_unlock(&entry->lock);
    pthread_rwlock_destroy(&entry->lock);
    g_free(entry);
  }
  dt_pthread_mutex_unlock(&cache->lock);
}

void dt_cache_release(dt_cache_t *cache, dt_cache_entry_t *entry)
{
  pthread_rwlock_unlock(&entry->lock);
}

void dt_cache_realloc(dt_cache_t *cache, const uint32_t key, const size_t cost, void *data)
{
  // XXX whatever, just call remove and re-add?
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
