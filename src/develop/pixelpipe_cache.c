/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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
#define INVALID_CACHEHASH ULLONG_MAX
#define UNUSED_CACHEHASH (INVALID_CACHEHASH -1)
static inline int _to_mb(size_t m)
{
  return (int)((m + 0x80000lu) / 0x400lu / 0x400lu);
}

gboolean dt_dev_pixelpipe_cache_init(
           struct dt_dev_pixelpipe_t *pipe,
           const int entries,
           const size_t size,
           const size_t limit)
{
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);

  cache->entries = entries;
  cache->allmem = cache->hits = cache->calls = cache->tests = 0;
  cache->memlimit = limit;
  cache->data = (void **)calloc(entries, sizeof(void *));
  cache->size = (size_t *)calloc(entries, sizeof(size_t));
  cache->dsc = (dt_iop_buffer_dsc_t *)calloc(entries, sizeof(dt_iop_buffer_dsc_t));
#ifdef _DEBUG
  memset(cache->dsc, 0x2c, sizeof(dt_iop_buffer_dsc_t) * entries);
#endif
  cache->hash = (uint64_t *)calloc(entries, sizeof(uint64_t));
  cache->used = (int32_t *)calloc(entries, sizeof(int32_t));
  cache->ioporder = (int32_t *)calloc(entries, sizeof(int32_t));

  for(int k = 0; k < entries; k++)
  {
    cache->size[k] = 0;
    cache->data[k] = NULL;
    cache->hash[k] = UNUSED_CACHEHASH;
    cache->used[k] = 1;
    cache->ioporder[k] = 0;
  }
  if(!size) return TRUE;

  // some pixelpipes use preallocated cachelines, following code is special for those
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
  // Make sure all cachelines are cleared.
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

void dt_dev_pixelpipe_cache_cleanup(struct dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);

  if(pipe->type & DT_DEV_PIXELPIPE_FULL)
  {
    dt_print(DT_DEBUG_PIPE, "Session fullpipe cache report. hits/run=%.2f, hits/test=%.3f\n",
    (double)(cache->hits) / fmax(1.0, pipe->runs),
    (double)(cache->hits) / fmax(1.0, cache->tests));
  }

  for(int k = 0; k < cache->entries; k++)
  {
    dt_free_align(cache->data[k]);
    cache->data[k] = NULL;
  }
  free(cache->data);
  cache->data = NULL;
  free(cache->dsc);
  cache->dsc = NULL;
  free(cache->hash);
  cache->hash = NULL;
  free(cache->used);
  cache->used = NULL;
  free(cache->size);
  cache->size = NULL;
  free(cache->ioporder);
  cache->ioporder = NULL;
}

static uint64_t _dev_pixelpipe_cache_basichash(
           const dt_imgid_t imgid,
           struct dt_dev_pixelpipe_t *pipe,
           const int position)
{
  // bernstein hash (djb2)
  uint64_t hash = 5381;

  /* What do we use for the basic hash
       1) imgid as all structures using the hash might possibly contain data from other images
       2) pipe->type for the cache it's important to keep status of fast mode included here
           also, we might use the hash also for different pipe.
       3) pipe->want_detail_mask make sure old cachelines from before activating details are
          not valid any more.
          Do we have to keep the roi of details mask? No as that is always defined by roi_in
          of the mask writing module (rawprepare or demosaic)
  */ 
  const uint32_t hashing_pipemode[3] = {(uint32_t)imgid,
                                        (uint32_t)pipe->type,
                                        (uint32_t)pipe->want_detail_mask };

  char *pstr = (char *)hashing_pipemode;
  for(size_t ip = 0; ip < sizeof(hashing_pipemode); ip++)
    hash = ((hash << 5) + hash) ^ pstr[ip];

  // go through all modules up to position and compute a hash using the operation and params.
  GList *pieces = pipe->nodes;
  for(int k = 0; k < position && pieces; k++)
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

uint64_t dt_dev_pixelpipe_cache_hash(
           const dt_imgid_t imgid,
           const dt_iop_roi_t *roi,
           struct dt_dev_pixelpipe_t *pipe,
           const int position)
{
  uint64_t hash = _dev_pixelpipe_cache_basichash(imgid, pipe, position);
  // also include roi data
  char *str = (char *)roi;
  for(size_t i = 0; i < sizeof(dt_iop_roi_t); i++)
    hash = ((hash << 5) + hash) ^ str[i];

  str = (char *)&pipe->details.hash;
  for(size_t i = 0; i < sizeof(uint64_t); i++)
    hash = ((hash << 5) + hash) ^ str[i];

  return hash;
}

gboolean dt_dev_pixelpipe_cache_available(
           dt_dev_pixelpipe_t *pipe,
           const uint64_t hash,
           const size_t size)
{
  if(pipe->mask_display || pipe->nocache)
    return FALSE;

  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);
  cache->tests++;
  // search for hash in cache and make the sizes are identical
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if((cache->size[k] == size)
        && (cache->hash[k] == hash)
        && (cache->hash[k] != INVALID_CACHEHASH))
    {
      cache->hits++;
      return TRUE;
    }
  }
  return FALSE;
}

// While looking for the oldest cacheline we always ignore the first two lines as they are used
// for swapping buffers while in entries==DT_PIPECACHE_MIN or masking mode
static int _get_oldest_cacheline(dt_dev_pixelpipe_cache_t *cache)
{
  // we never want the latest used cacheline! It was <= 0 and the weight has increased just now
  int age = 1;
  int id = 0;
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if(cache->used[k] > age)
    {
      age = cache->used[k];
      id = k;
    }
  }
  return id;
}

static int _get_oldest_used_cacheline(dt_dev_pixelpipe_cache_t *cache)
{
  int age = 1;
  int id = 0;
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if((cache->used[k] > age) && (cache->data[k] != NULL))
    {
      age = cache->used[k];
      id = k;
    }
  }
  return id;
}

static int _get_oldest_free_cacheline(dt_dev_pixelpipe_cache_t *cache)
{
  // we never want the latest used cacheline!
  int age = 1;
  int id = 0;
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if((cache->used[k] > age) && (cache->data[k] == NULL))
    {
      age = cache->used[k];
      id = k;
    }
  }
  return id;
}

static int _get_cacheline(struct dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);
  // Simplest case is some pipes having only two cachelines or we are in masking mode so we
  // can just toggle between them.
  if((cache->entries == DT_PIPECACHE_MIN) || pipe->mask_display || pipe->nocache)
    return cache->calls & 1;

  int oldest = _get_oldest_free_cacheline(cache);
  if(oldest > 0) return oldest;

  oldest = _get_oldest_used_cacheline(cache);
  if(oldest > 0) return oldest;

  oldest = _get_oldest_cacheline(cache);
  return (oldest == 0) ? cache->calls & 1 : oldest; 
}

// return TRUE in case of a hit
static gboolean _get_by_hash(
          struct dt_dev_pixelpipe_t *pipe,
          const uint64_t hash,
          const size_t size,
          void **data,
          dt_iop_buffer_dsc_t **dsc)
{
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if((cache->hash[k] == hash) && (cache->hash[k] != INVALID_CACHEHASH))
    {
      /* We check for situation with a hash identity but buffer sizes don't match.
           This could happen because of "hash overlaps" or other situations where the hash
           doesn't reflect the complete status. (or we have another bug in dt)
         Also we don't use cached data while bypassing modules because of mask visualizing
           or we have pipe->nocache set to TRUE
         In these cases we don't want to simply realloc or alike as these data could possibly
         still be used in the pipe.
         Instead we make sure the cleanup after running the pixelpipe can free it but it
         won't be taken in this pixelpipe process.
      */
      if((cache->size[k] != size) || pipe->mask_display || pipe->nocache)
      {
        cache->hash[k] = INVALID_CACHEHASH;
        cache->used[k] = VERY_OLD_CACHE_WEIGHT;
      }
      else
      {
        // we have a proper hit
        *data = cache->data[k];
        *dsc = &cache->dsc[k];
        ASAN_POISON_MEMORY_REGION(*data, cache->size[k]);
        ASAN_UNPOISON_MEMORY_REGION(*data, size);

        // in case of a hit it's always good to further keep the cacheline as important
        cache->used[k] = -cache->entries;
        return TRUE;
      }
    }
  }

  return FALSE;
}

gboolean dt_dev_pixelpipe_cache_get(
           struct dt_dev_pixelpipe_t *pipe,
           const uint64_t hash,
           const size_t size,
           void **data,
           dt_iop_buffer_dsc_t **dsc,
           struct dt_iop_module_t *module,
           const gboolean important)
{
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);
  cache->calls++;
  for(int k = 0; k < cache->entries; k++)
    cache->used[k]++; // age all entries

  // cache keeps history and we have a cache hit, so no new buffer
  if(cache->entries > DT_PIPECACHE_MIN && _get_by_hash(pipe, hash, size, data, dsc))
  {
    const dt_iop_buffer_dsc_t *cdsc = *dsc;
    dt_print_pipe(DT_DEBUG_PIPE, "cache HIT",
          pipe, module, NULL, NULL,
          "%s, hash=%" PRIx64 "\n",
          dt_iop_colorspace_to_name(cdsc->cst), hash); 
    return FALSE;
  }
  // We need a fresh buffer as there was no hit.
  //
  // Pipes with two cache lines have pre-allocated memory, but we must
  // grow storage if a later iop requires a larger buffer.
  //
  // Otherwise, get an old/free cacheline and allocate required size.
  // Check both for free and non-matching (and grow or shrink buffer).

  // Can the module having used this cacheline before might still use the data with other dsc?
  const int cline = _get_cacheline(pipe);
  gboolean newdata = FALSE;
  if(((cache->entries == DT_PIPECACHE_MIN) && (cache->size[cline] < size))
     || ((cache->entries > DT_PIPECACHE_MIN) && (cache->size[cline] != size)))
  {
    newdata = TRUE;
    dt_free_align(cache->data[cline]);
    cache->allmem -= cache->size[cline];
    cache->data[cline] = (void *)dt_alloc_align(64, size);
    if(cache->data[cline])
    {
      cache->size[cline] = size;
      cache->allmem += size;
    }
    else
    {
      cache->size[cline] = 0;
    }
  }

  *data = cache->data[cline];
  ASAN_UNPOISON_MEMORY_REGION(*data, size);

  // first, update our copy, then update the pointer to point at our copy
  cache->dsc[cline] = **dsc;
  *dsc = &cache->dsc[cline];
  const gboolean masking = pipe->mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE;

  cache->hash[cline]      = masking ? UNUSED_CACHEHASH : hash;
  cache->used[cline]      = masking ? VERY_OLD_CACHE_WEIGHT
                                    : (important ? -cache->entries : 0);
  cache->ioporder[cline]  = module  ? module->iop_order : 0;

  const dt_iop_buffer_dsc_t *cdsc = *dsc;
  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE, "pixelpipe_cache_get",
    pipe, module, NULL, NULL,
    "%s %s %s line%3i at %p. hash=%" PRIx64 "\n",
     dt_iop_colorspace_to_name(cdsc->cst),
     newdata ? "new" : "   ",
     important ? "important" : (masking ? "masking  " : "         "),
     cline, cache->data[cline], cache->hash[cline]); 

  return TRUE;
}

/* if invalid is FALSE the buffer hash marks it as usable, otherwise the cacheline
   will be usable again after the next dt_dev_pixelpipe_cache_checkmem()
*/
static void _mark_invalid_cacheline(const dt_dev_pixelpipe_cache_t *cache,
                                    const int k,
                                    const gboolean invalid)
{
  cache->hash[k] = invalid ? INVALID_CACHEHASH : UNUSED_CACHEHASH;
  cache->used[k] += VERY_OLD_CACHE_WEIGHT;
  cache->ioporder[k] = 0;
}

void dt_dev_pixelpipe_cache_flush(const struct dt_dev_pixelpipe_t *pipe)
{
  const dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
    _mark_invalid_cacheline(cache, k, FALSE);
}

void dt_dev_pixelpipe_cache_invalidate_from(
        const struct dt_dev_pixelpipe_t *pipe,
        const struct dt_iop_module_t *module,
        const gboolean invalid)
{
  const dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);
  const int32_t order = module ? module->iop_order : 0;
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if(cache->ioporder[k] >= order)
      _mark_invalid_cacheline(cache, k, invalid);
  }
}

void dt_dev_pixelpipe_important_cacheline(
       const struct dt_dev_pixelpipe_t *pipe,
       const void *data,
       const size_t size)
{
  const dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if((cache->data[k] == data)
        && (size == cache->size[k])
        && (cache->used[k] < VERY_OLD_CACHE_WEIGHT)
        && (cache->hash[k] != INVALID_CACHEHASH))
      cache->used[k] = -cache->entries;
  }
}

void dt_dev_pixelpipe_invalidate_cacheline(const struct dt_dev_pixelpipe_t *pipe,
                                           const void *data)
{
  const dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if(cache->data[k] == data)
      _mark_invalid_cacheline(cache, k, FALSE);
  }
}

static size_t _free_cacheline(dt_dev_pixelpipe_cache_t *cache, const int k)
{
  const size_t removed = cache->size[k];

  dt_free_align(cache->data[k]);
  cache->allmem -= removed;
  cache->size[k] = 0;
  cache->data[k] = NULL;
  _mark_invalid_cacheline(cache, k, FALSE);
  return removed;
}

static int _important_lines(const dt_dev_pixelpipe_cache_t *cache)
{
  int important = 0;
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
    if(cache->used[k] < 0) important++;
  return important;
}

static int _used_lines(const dt_dev_pixelpipe_cache_t *cache)
{
  int in_use = 0;
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
    if(cache->data[k]) in_use++;
  return in_use;
}

static int _invalid_lines(const dt_dev_pixelpipe_cache_t *cache)
{
  int invalid = 0;
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
    if(cache->data[k] && (cache->hash[k] == INVALID_CACHEHASH))
      invalid++;
  return invalid;
}

void dt_dev_pixelpipe_cache_checkmem(struct dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);

  // we have pixelpipes like export & thumbnail that just use
  // alternating buffers so no cleanup
  if(cache->entries == DT_PIPECACHE_MIN) return;

  size_t freed = 0;

  int invalid = 0;
  // We should always free cachelines maked as not valid
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if((cache->data[k] != NULL) && (cache->hash[k] == UNUSED_CACHEHASH))
      freed += _free_cacheline(cache, k);
    if((cache->data[k] != NULL) && (cache->hash[k] == INVALID_CACHEHASH))
    {
      freed += _free_cacheline(cache, k);
      invalid++;
    }
  }

  int oldlines = 0;
  while(cache->memlimit && (cache->memlimit < cache->allmem))
  {
    const int k = _get_oldest_used_cacheline(cache);
    if(k == 0) break;
    freed += _free_cacheline(cache, k);
    oldlines++;
  }

  dt_print_pipe(DT_DEBUG_PIPE, "pixelpipe_cache_checkmem", pipe, NULL, NULL, NULL,
    "%i lines (removed invalid=%i, oldlines=%i). Now important=%i, used=%i."
    " Cache: freed=%iMB. Now using %iMB, limit=%iMB\n",
    cache->entries,
    invalid, oldlines,
    _important_lines(cache), _used_lines(cache),
    _to_mb(freed), _to_mb(cache->allmem), _to_mb(cache->memlimit));
}

void dt_dev_pixelpipe_cache_report(struct dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);
  dt_print_pipe(DT_DEBUG_PIPE, "cache report", pipe, NULL, NULL, NULL,
    "%i lines (important=%i, used=%i, invalid=%i). Used %iMB, limit=%iMB. hits/run=%.2f, hits/test=%.3f\n",
    cache->entries, _important_lines(cache), _used_lines(cache), _invalid_lines(cache),
    _to_mb(cache->allmem), _to_mb(cache->memlimit),
    (double)(cache->hits) / fmax(1.0, pipe->runs),
    (double)(cache->hits) / fmax(1.0, cache->tests));
}

#undef VERY_OLD_CACHE_WEIGHT
#undef INVALID_CACHEHASH
#undef UNUSED_CACHEHASH
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

