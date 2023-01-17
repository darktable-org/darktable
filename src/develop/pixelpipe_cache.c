/*
    This file is part of darktable,
    Copyright (C) 2009-2022 darktable developers.

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

#include "develop/pixelpipe_cache.h"
#include "develop/format.h"
#include "develop/pixelpipe_hb.h"
#include "libs/lib.h"
#include "libs/colorpicker.h"
#include <stdlib.h>

#define VERY_OLD_CACHE_WEIGHT 1000
// TODO: make cache global (needs to be thread safe then)

gboolean dt_dev_pixelpipe_cache_init(dt_dev_pixelpipe_cache_t *cache, int entries, size_t size, size_t limit)
{
  cache->entries = entries;
  cache->allmem = cache->queries = cache->misses = 0;
  cache->memlimit = limit;
  cache->data = (void **)calloc(entries, sizeof(void *));
  cache->size = (size_t *)calloc(entries, sizeof(size_t));
  cache->dsc = (dt_iop_buffer_dsc_t *)calloc(entries, sizeof(dt_iop_buffer_dsc_t));
#ifdef _DEBUG
  memset(cache->dsc, 0x2c, sizeof(dt_iop_buffer_dsc_t) * entries);
#endif
  cache->basichash = (uint64_t *)calloc(entries, sizeof(uint64_t));
  cache->hash = (uint64_t *)calloc(entries, sizeof(uint64_t));
  cache->used = (int32_t *)calloc(entries, sizeof(int32_t));
  cache->modname = (char **)calloc(entries, sizeof(char *));

  for(int k = 0; k < entries; k++)
  {
    cache->size[k] = 0;
    cache->data[k] = NULL;
    cache->basichash[k] = -1;
    cache->hash[k] = -1;
    cache->used[k] = 1;
    cache->modname[k] = NULL;
  }
  if(!size) return TRUE;

  for(int k = 0; k < entries; k++)
  {
    cache->size[k] = size;
    cache->data[k] = (void *)dt_alloc_align(64, size);
    if(!cache->data[k])
      goto alloc_memory_fail;
#ifdef _DEBUG
    memset(cache->data[k], 0x5d, size);
#endif
    ASAN_POISON_MEMORY_REGION(cache->data[k], cache->size[k]);
    cache->allmem += size;
  }
  return TRUE;

alloc_memory_fail:
  //  dt_dev_pixelpipe_cache_cleanup(cache);
  // The above code seems to be not correct as failing to allocate the cache->data buffers
  // should not cleanup the whole pixelpipe cache but only reset the buffers to null.
  // A warning about low memory will appear but the pipeline still has valid data so dt won't crash
  // but will only fail to generate thumbnails for example.
  for(int k = 0; k < cache->entries; k++)
  {
    dt_free_align(cache->data[k]);
    cache->size[k] = 0;
    cache->data[k] = NULL;
  }
  cache->allmem = 0;
  return FALSE;
}

void dt_dev_pixelpipe_cache_cleanup(dt_dev_pixelpipe_cache_t *cache)
{
  for(int k = 0; k < cache->entries; k++)
  {
    dt_free_align(cache->data[k]);
    cache->data[k] = NULL;
  }
  free(cache->data);
  cache->data = NULL;
  free(cache->dsc);
  cache->dsc = NULL;
  free(cache->basichash);
  cache->basichash = NULL;
  free(cache->hash);
  cache->hash = NULL;
  free(cache->used);
  cache->used = NULL;
  free(cache->size);
  cache->size = NULL;
  free(cache->modname);
  cache->modname = NULL;
}

uint64_t dt_dev_pixelpipe_cache_basichash(int imgid, struct dt_dev_pixelpipe_t *pipe, int module)
{
  // bernstein hash (djb2)
  uint64_t hash = 5381;
  // we use the the imgid and both actual pipe type and mask_display for the hash
  const int hashing_pipemode[3] = {(int)imgid, (int)pipe->type, (int)pipe->mask_display};
  const char *pstr = (const char *)hashing_pipemode;
  for(size_t ip = 0; ip < sizeof(hashing_pipemode); ip++)
    hash = ((hash << 5) + hash) ^ pstr[ip];
  // go through all modules up to module and compute a weird hash using the operation and params.
  GList *pieces = pipe->nodes;
  for(int k = 0; k < module && pieces; k++)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    dt_develop_t *dev = piece->module->dev;
    if(!(dev->gui_module && dev->gui_module != piece->module
         && (dev->gui_module->operation_tags_filter() & piece->module->operation_tags())))
    {
      hash = ((hash << 5) + hash) ^ piece->hash;
      if(piece->module->request_color_pick != DT_REQUEST_COLORPICK_OFF)
      {
        if(darktable.lib->proxy.colorpicker.primary_sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
        {
          const char *str = (const char *)darktable.lib->proxy.colorpicker.primary_sample->box;
          for(size_t i = 0; i < sizeof(float) * 4; i++)
            hash = ((hash << 5) + hash) ^ str[i];
        }
        else if(darktable.lib->proxy.colorpicker.primary_sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
        {
          const char *str = (const char *)darktable.lib->proxy.colorpicker.primary_sample->point;
          for(size_t i = 0; i < sizeof(float) * 2; i++)
            hash = ((hash << 5) + hash) ^ str[i];
        }
      }
    }
    pieces = g_list_next(pieces);
  }
  return hash;
}

uint64_t dt_dev_pixelpipe_cache_basichash_prior(int imgid, struct dt_dev_pixelpipe_t *pipe,
                                                const dt_iop_module_t *const module)
{
  // find the last enabled module prior to the specified one, then get its hash
  GList *pieces = pipe->nodes;
  GList *modules = pipe->iop;
  int last = -1;
  for(int k = 1; modules && pieces; k++)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    if(module == (dt_iop_module_t *)modules->data)
      break;		// we've found the given module, so 'last' now contains the index of the prior active module
    dt_develop_t *dev = piece->module->dev;
    if(piece->enabled
       && !(dev->gui_module && dev->gui_module != piece->module
            && (dev->gui_module->operation_tags_filter() & piece->module->operation_tags())))
      last = k;
    pieces = g_list_next(pieces);
    modules = g_list_next(modules);
  }
  return last>=0 ? dt_dev_pixelpipe_cache_basichash(imgid, pipe, last) : -1;
}

void dt_dev_pixelpipe_cache_fullhash(int imgid, const dt_iop_roi_t *roi, struct dt_dev_pixelpipe_t *pipe, int module,
                                     uint64_t *basichash, uint64_t *fullhash)
{
  uint64_t hash = *basichash = dt_dev_pixelpipe_cache_basichash(imgid, pipe, module);
  // also add scale, x and y:
  const char *str = (const char *)roi;
  for(size_t i = 0; i < sizeof(dt_iop_roi_t); i++)
    hash = ((hash << 5) + hash) ^ str[i];
  *fullhash = hash;
}

uint64_t dt_dev_pixelpipe_cache_hash(int imgid, const dt_iop_roi_t *roi, dt_dev_pixelpipe_t *pipe, int module)
{
  uint64_t basichash, hash;
  dt_dev_pixelpipe_cache_fullhash(imgid, roi, pipe, module, &basichash, &hash);
  return hash;
}

gboolean dt_dev_pixelpipe_cache_available(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, const size_t size)
{
  // search for hash in cache
  for(int k = 0; k < cache->entries; k++)
    if((cache->hash[k] == hash) && (cache->size[k] == size))
      return TRUE;
  return FALSE;
}

static int _get_oldest_cacheline(dt_dev_pixelpipe_cache_t *cache)
{
  // we never want the latest used cacheline! It was <= 0 and the weight has increased just now
  int weight = 1;
  int id = 0;
  for(int k = 0; k < cache->entries; k++)
  {
    if(cache->used[k] > weight)
    {
      weight = cache->used[k];
      id = k;
    }
  }
  return id;
}

static int _get_oldest_used_cacheline(dt_dev_pixelpipe_cache_t *cache, const int age, const size_t minsize)
{
  int weight = 0;
  int id = -1;
  for(int k = 0; k < cache->entries; k++)
  {
    if((cache->used[k] > weight) && (cache->size[k] >= minsize) && (cache->used[k] > age))
    {
      weight = cache->used[k];
      id = k;
    }
  }
  return id;
}

static int _get_oldest_free_cacheline(dt_dev_pixelpipe_cache_t *cache)
{
  // we never want the latest used cacheline! It was <= 0 and the weight has increased just now
  int weight = 1;
  int id = -1;
  for(int k = 0; k < cache->entries; k++)
  {
    if((cache->used[k] > weight) && (cache->data[k] == NULL))
    {
      weight = cache->used[k];
      id = k;
    }
  }
  return id;
}

static int _get_free_cacheline(dt_dev_pixelpipe_cache_t *cache, size_t size)
{
  const int oldest   = _get_oldest_cacheline(cache);
  const int old_free = _get_oldest_free_cacheline(cache);
  const int old_used = _get_oldest_used_cacheline(cache, 2, size);

  // No memory restrictions
  if((cache->memlimit == 0) || (cache->memlimit > cache->allmem))
  {
    if(old_free >= 0) return old_free;
    if(old_used >= 0) return old_used;
    return oldest;
   }

  if(old_used >= 0) return old_used;
  
  return oldest;
}

gboolean dt_dev_pixelpipe_cache_get(struct dt_dev_pixelpipe_t *pipe, const uint64_t basichash, const uint64_t hash,
                                        const size_t size, void **data, dt_iop_buffer_dsc_t **dsc, char *name, const gboolean important)
{
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);
  const int weight = important ? -cache->entries : 0;
  cache->queries++;
  for(int k = 0; k < cache->entries; k++)
    cache->used[k]++; // age all entries

  for(int k = 0; k < cache->entries; k++)
  {
    if(cache->hash[k] == hash)
    {
      if(cache->size[k] != size)
      {
        /* In rare sitations we might find an identical hash but the buffer size does not meet the requirements.
           This can happen if the pixelpipe roi is expanded like in rotate&crop without a proper hash.
           In this case we can't simply realloc or alike as there might be data in the pipeline just making use
           of that buffer so we disable it and make the cleanup will free it.
        */ 
        dt_print(DT_DEBUG_DEV | DT_DEBUG_ROI, "[pixelpipe_cache_get] %12s %16s HIT age %4i, line%3i, SIZE ERROR at %p, cache size %ikB, requested %ikB\n",
          dt_dev_pixelpipe_type_to_str(pipe->type), name, cache->used[k], k, cache->data[k], (int)cache->size[k] / 1024, (int)size / 1024);
        cache->hash[k] = cache->basichash[k] = -1;
        cache->used[k] = VERY_OLD_CACHE_WEIGHT;
      }
      else
      {
        // we have a proper hit
        *data = cache->data[k];
        *dsc = &cache->dsc[k];
        ASAN_POISON_MEMORY_REGION(*data, cache->size[k]);
        ASAN_UNPOISON_MEMORY_REGION(*data, size);
        dt_print(DT_DEBUG_DEV | DT_DEBUG_ROI, "[pixelpipe_cache_get] %12s %16s HIT age %4i, line%3i, at %p, hash%22" PRIu64 ", basic%22" PRIu64 "\n",
          dt_dev_pixelpipe_type_to_str(pipe->type), name, cache->used[k], k, cache->data[k],
          cache->hash[k], cache->basichash[k]); 
        // in case of a hit its always good to keep the cacheline as important
        cache->used[k] = -cache->entries;
        return FALSE;
      }
    }
  }

  // We need a fresh buffer as there was no hit.
  // Either we just toggle cachelines 0/1 in case of cache->entries == 2
  // or we get an old/free cacheline. As that might have no or not enough memory allocated we have to make sure.
  // A problem? The module having used this cacheline before might still use the data with other dsc?
  const int cline = (cache->entries == 2) ? cache->queries & 1 : _get_free_cacheline(&(pipe->cache), size);

  const gboolean new_cline = (cache->size[cline] == 0);
  const gboolean not_enough = (cache->size[cline] < size);

  if(not_enough)
  {
    if(not_enough && !new_cline)
    {
      dt_free_align(cache->data[cline]);
      cache->allmem -= cache->size[cline];
      dt_print(DT_DEBUG_DEV | DT_DEBUG_ROI, "[pixelpipe_cache_get] %12s %16s CHG age %4i, line%3i, was %s, %lu->%luMB\n",
        dt_dev_pixelpipe_type_to_str(pipe->type), name, cache->used[cline], cline, cache->modname[cline],
        cache->size[cline] / 1024lu / 1024lu, size / 1024lu / 1024lu); 
    }
    cache->data[cline] = (void *)dt_alloc_align(64, size);
    cache->size[cline] = size;
    cache->allmem += cache->size[cline];
  }

  *data = cache->data[cline];

  ASAN_UNPOISON_MEMORY_REGION(*data, size);

  // first, update our copy, then update the pointer to point at our copy
  cache->dsc[cline] = **dsc;
  *dsc = &cache->dsc[cline];

  cache->basichash[cline] = basichash;
  cache->hash[cline] = hash;
  const int avoiding = pipe->mask_display & (DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU | DT_DEV_PIXELPIPE_DISPLAY_ANY);
  cache->used[cline] = (avoiding) ? VERY_OLD_CACHE_WEIGHT : weight;
  cache->modname[cline] = name;
  cache->misses++;
  dt_print(DT_DEBUG_DEV | DT_DEBUG_ROI, "[pixelpipe_cache_get] %12s %16s %s age %4i, line%3i, at %p, hash%22" PRIu64 ", basic%22" PRIu64 "\n",
    dt_dev_pixelpipe_type_to_str(pipe->type), name, new_cline ? "NEW" : "   ", cache->used[cline], cline, cache->data[cline],
    cache->hash[cline], cache->basichash[cline]); 
  return TRUE;
}

void dt_dev_pixelpipe_cache_flush(dt_dev_pixelpipe_cache_t *cache)
{
  cache->queries = cache->misses = cache->queries & 1; // we don't use zero here for "swapping pipelines" having only two lines
  for(int k = 0; k < cache->entries; k++)
  {
    cache->basichash[k] = -1;
    cache->hash[k] = -1;
    cache->used[k] = VERY_OLD_CACHE_WEIGHT;
    ASAN_POISON_MEMORY_REGION(cache->data[k], cache->size[k]);
  }
}

void dt_dev_pixelpipe_cache_flush_all_but(dt_dev_pixelpipe_cache_t *cache, uint64_t basichash)
{
  for(int k = 0; k < cache->entries; k++)
  {
    if(cache->basichash[k] == basichash)
      continue;
    cache->basichash[k] = -1;
    cache->hash[k] = -1;
    cache->used[k] = VERY_OLD_CACHE_WEIGHT;
    ASAN_POISON_MEMORY_REGION(cache->data[k], cache->size[k]);
  }
}

void dt_dev_pixelpipe_cache_reweight(struct dt_dev_pixelpipe_t *pipe, void *data, const size_t size)
{
  const int avoiding = pipe->mask_display & (DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU | DT_DEV_PIXELPIPE_DISPLAY_ANY);
  if(avoiding)
    return;
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);
  for(int k = 0; k < cache->entries; k++)
  {
    if((cache->data[k] == data) && (size == cache->size[k]))
    {
      cache->used[k] = -cache->entries;
      dt_print(DT_DEBUG_DEV | DT_DEBUG_ROI, "[pipecache_reweight] %13s %16s     age %4i, line%3i, at %p, hash%22" PRIu64 ", basic%22" PRIu64 "\n",
        dt_dev_pixelpipe_type_to_str(pipe->type), cache->modname[k], cache->used[k], k, cache->data[k], cache->hash[k], cache->basichash[k]);
    }
  }
}

void dt_dev_pixelpipe_cache_invalidate(dt_dev_pixelpipe_cache_t *cache, void *data)
{
  for(int k = 0; k < cache->entries; k++)
  {
    if(cache->data[k] == data)
    {
      cache->basichash[k] = -1;
      cache->hash[k] = -1;
      cache->used[k] = VERY_OLD_CACHE_WEIGHT;
      ASAN_POISON_MEMORY_REGION(cache->data[k], cache->size[k]);
    }
  }
}

static size_t _free_cacheline(dt_dev_pixelpipe_cache_t *cache, const int k, const int pipetype)
{
  const size_t removed = cache->size[k];
  dt_print(DT_DEBUG_DEV | DT_DEBUG_VERBOSE , "  [free cacheline] %s %16s, age %4i, line%3i, size=%luMB at %p\n",
    dt_dev_pixelpipe_type_to_str(pipetype), cache->modname[k], cache->used[k], k, removed / 1024lu / 1024lu, cache->data[k]);

  dt_free_align(cache->data[k]);
  cache->allmem -= removed;
  cache->size[k] = 0;
  cache->data[k] = NULL;
  cache->hash[k] = -1;
  cache->basichash[k] = -1;
  cache->modname[k] = NULL;
  cache->used[k] = VERY_OLD_CACHE_WEIGHT;
  return removed;
}

void dt_dev_pixelpipe_cache_checkmem(struct dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);
  if((cache->memlimit == 0) || !(pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    dt_print(DT_DEBUG_DEV, "[pixelpipe_process] %s using device %d. Cache: used=%luMB\n",
    dt_dev_pixelpipe_type_to_str(pipe->type), pipe->devid, cache->allmem / 1024lu / 1024lu);
    return;
  }
  size_t freed = 0;
  int low_grp = 0;
  int high_grp = 0;

  // release unimportant lines first
  const int old_limit = MAX(2, cache->entries / 8);
  int oldest = _get_oldest_used_cacheline(cache, old_limit, 1);
  while((cache->memlimit < cache->allmem * 2) && (oldest >= 0))
  {
    low_grp += 1;
    freed += _free_cacheline(cache, oldest, pipe->type);
    oldest = _get_oldest_used_cacheline(cache, old_limit, 1);
  }

  oldest = -1;
  while((cache->memlimit < cache->allmem * 2) && (oldest > -(cache->entries / 2)))
  {
    for(int k = 0; k < cache->entries; k++)
    {
      if((cache->size[k] != 0) && (cache->used[k] == oldest))
      {
        high_grp += 1;
        freed += _free_cacheline(cache, k, pipe->type);   
      }
    }
    oldest -= 1;
  }
  dt_print(DT_DEBUG_DEV, "[pixelpipe_process] %s using device %d. Cache: freed=%luMB (%i/%i), used=%luMB, limit=%luMB\n",
    dt_dev_pixelpipe_type_to_str(pipe->type), pipe->devid, freed / 1024lu / 1024lu, low_grp, high_grp,
    cache->allmem / 1024lu / 1024lu, cache->memlimit / 1024lu / 1024lu);
}

void dt_dev_pixelpipe_cache_report(struct dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);
  dt_print(DT_DEBUG_DEV, "[pixelpipe_process] %s done. used=%luMB, limit=%luMB, hitrate=%.2f\n",
    dt_dev_pixelpipe_type_to_str(pipe->type),
    cache->allmem / 1024lu / 1024lu, cache->memlimit / 1024lu / 1024lu, 
    (cache->queries - cache->misses) / (float)cache->queries);
}

#undef VERY_OLD_CACHE_WEIGHT

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

