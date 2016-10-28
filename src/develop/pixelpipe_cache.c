/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include <stdlib.h>


// TODO: make cache global (needs to be thread safe then)
// plan:
// - look at mipmap_cache.c, for the full buffer allocs
// - do that, but for `large' and `regular' buffers (full + export/dr mode), so 2 caches
//   (in fact, maybe 3, one for preview pipes?)
// - have at most 3 read locks all the time per pipe, get them at create time
//   ping, pong, and priority buffer (focused plugin)
// - drop read by the time another is requested (with priority, drop that, or alternating ping and pong?)

int dt_dev_pixelpipe_cache_init(dt_dev_pixelpipe_cache_t *cache, int entries, size_t size)
{
  cache->entries = entries;
  cache->data = (void **)calloc(entries, sizeof(void *));
  cache->size = (size_t *)calloc(entries, sizeof(size_t));
  cache->dsc = (dt_iop_buffer_dsc_t *)calloc(entries, sizeof(dt_iop_buffer_dsc_t));
#ifdef _DEBUG
  memset(cache->dsc, 0x2c, sizeof(dt_iop_buffer_dsc_t) * entries);
#endif
  cache->hash = (uint64_t *)calloc(entries, sizeof(uint64_t));
  cache->used = (int32_t *)calloc(entries, sizeof(int32_t));
  for(int k = 0; k < entries; k++)
  {
    cache->size[k] = size;
    if(size)
    { // allow 0 initial buffer size (yet unknown dimensions)
      cache->data[k] = (void *)dt_alloc_align(16, size);
      if(!cache->data[k]) goto alloc_memory_fail;
#ifdef _DEBUG
      memset(cache->data[k], 0x5d, size);
#endif
      ASAN_POISON_MEMORY_REGION(cache->data[k], cache->size[k]);
    }
    else cache->data[k] = 0;
    cache->hash[k] = -1;
    cache->used[k] = 0;
  }
  cache->queries = cache->misses = 0;
  return 1;

alloc_memory_fail:
  dt_dev_pixelpipe_cache_cleanup(cache);
  return 0;
}

void dt_dev_pixelpipe_cache_cleanup(dt_dev_pixelpipe_cache_t *cache)
{
  for(int k = 0; k < cache->entries; k++) dt_free_align(cache->data[k]);
  free(cache->data);
  free(cache->dsc);
  free(cache->hash);
  free(cache->used);
  free(cache->size);
}

uint64_t dt_dev_pixelpipe_cache_hash(int imgid, const dt_iop_roi_t *roi, dt_dev_pixelpipe_t *pipe, int module)
{
  // bernstein hash (djb2)
  uint64_t hash = 5381 + imgid;
  // go through all modules up to module and compute a weird hash using the operation and params.
  GList *pieces = pipe->nodes;
  for(int k = 0; k < module && pieces; k++)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    dt_develop_t *dev = piece->module->dev;
    if(!(dev->gui_module && (dev->gui_module->operation_tags_filter() & piece->module->operation_tags())))
    {
      hash = ((hash << 5) + hash) ^ piece->hash;
      if(piece->module->request_color_pick != DT_REQUEST_COLORPICK_OFF)
      {
        if(darktable.lib->proxy.colorpicker.size)
        {
          const char *str = (const char *)piece->module->color_picker_box;
          for(size_t i = 0; i < sizeof(float) * 4; i++) hash = ((hash << 5) + hash) ^ str[i];
        }
        else
        {
          const char *str = (const char *)piece->module->color_picker_point;
          for(size_t i = 0; i < sizeof(float) * 2; i++) hash = ((hash << 5) + hash) ^ str[i];
        }
      }
    }
    pieces = g_list_next(pieces);
  }
  // also add scale, x and y:
  const char *str = (const char *)roi;
  for(size_t i = 0; i < sizeof(dt_iop_roi_t); i++) hash = ((hash << 5) + hash) ^ str[i];
  return hash;
}

int dt_dev_pixelpipe_cache_available(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash)
{
  // search for hash in cache
  for(int32_t k = 0; k < cache->entries; k++)
    if(cache->hash[k] == hash) return 1;
  return 0;
}

int dt_dev_pixelpipe_cache_get_important(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, const size_t size,
                                         void **data, dt_iop_buffer_dsc_t **dsc)
{
  return dt_dev_pixelpipe_cache_get_weighted(cache, hash, size, data, dsc, -cache->entries);
}

int dt_dev_pixelpipe_cache_get(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, const size_t size,
                               void **data, dt_iop_buffer_dsc_t **dsc)
{
  return dt_dev_pixelpipe_cache_get_weighted(cache, hash, size, data, dsc, 0);
}

int dt_dev_pixelpipe_cache_get_weighted(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, const size_t size,
                                        void **data, dt_iop_buffer_dsc_t **dsc, int weight)
{
  cache->queries++;
  *data = NULL;
  int max_used = -1, max = 0;
  size_t sz = 0;
  for(int k = 0; k < cache->entries; k++)
  {
    // search for hash in cache
    if(cache->used[k] > max_used)
    {
      max_used = cache->used[k];
      max = k;
    }
    cache->used[k]++; // age all entries
    if(cache->hash[k] == hash)
    {
      assert(cache->size[k] >= size);

      *data = cache->data[k];
      *dsc = &cache->dsc[k];
      sz = cache->size[k];
      cache->used[k] = weight; // this is the MRU entry

      ASAN_POISON_MEMORY_REGION(*data, sz);
      ASAN_UNPOISON_MEMORY_REGION(*data, size);
    }
  }

  if(!*data || sz < size)
  {
    // kill LRU entry
    // printf("[pixelpipe_cache_get] hash not found, returning slot %d/%d age %d\n", max, cache->entries,
    // weight);
    if(cache->size[max] < size)
    {
      dt_free_align(cache->data[max]);
      cache->data[max] = (void *)dt_alloc_align(16, size);
      cache->size[max] = size;
    }
    *data = cache->data[max];
    sz = cache->size[max];

    ASAN_POISON_MEMORY_REGION(*data, sz);
    ASAN_UNPOISON_MEMORY_REGION(*data, size);

    // first, update our copy, then update the pointer to point at our copy
    cache->dsc[max] = **dsc;
    *dsc = &cache->dsc[max];

    cache->hash[max] = hash;
    cache->used[max] = weight;
    cache->misses++;
    return 1;
  }
  else
    return 0;
}

void dt_dev_pixelpipe_cache_flush(dt_dev_pixelpipe_cache_t *cache)
{
  for(int k = 0; k < cache->entries; k++)
  {
    cache->hash[k] = -1;
    cache->used[k] = 0;
    ASAN_POISON_MEMORY_REGION(cache->data[k], cache->size[k]);
  }
}

void dt_dev_pixelpipe_cache_reweight(dt_dev_pixelpipe_cache_t *cache, void *data)
{
  for(int k = 0; k < cache->entries; k++)
  {
    if(cache->data[k] == data)
    {
      cache->used[k] = -cache->entries;
    }
  }
}

void dt_dev_pixelpipe_cache_invalidate(dt_dev_pixelpipe_cache_t *cache, void *data)
{
  for(int k = 0; k < cache->entries; k++)
  {
    if(cache->data[k] == data)
    {
      cache->hash[k] = -1;
      ASAN_POISON_MEMORY_REGION(cache->data[k], cache->size[k]);
    }
  }
}

void dt_dev_pixelpipe_cache_print(dt_dev_pixelpipe_cache_t *cache)
{
  for(int k = 0; k < cache->entries; k++)
  {
    printf("pixelpipe cacheline %d ", k);
    printf("used %d by %" PRIu64 "", cache->used[k], cache->hash[k]);
    printf("\n");
  }
  printf("cache hit rate so far: %.3f\n", (cache->queries - cache->misses) / (float)cache->queries);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
