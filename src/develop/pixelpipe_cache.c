/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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

#define INVALID_CACHEHASH 0

static inline int _to_mb(size_t m)
{
  return (int)((m + 0x80000lu) / 0x400lu / 0x400lu);
}

gboolean dt_dev_pixelpipe_cache_init(dt_dev_pixelpipe_t *pipe,
                                     const int entries,
                                     const size_t size,
                                     const size_t limit)
{
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);

  cache->entries = entries;
  cache->allmem = cache->hits = cache->calls = cache->tests = 0;
  cache->memlimit = limit;

  const size_t csize = sizeof(void *) + sizeof(size_t) + sizeof(dt_iop_buffer_dsc_t) + 2*sizeof(int32_t) + sizeof(uint64_t);
  cache->data = (void **) calloc(entries, csize);
  cache->size = (size_t *)((void *)cache->data + entries * sizeof(void *));
  cache->dsc = (dt_iop_buffer_dsc_t *)((void *)cache->size + entries * sizeof(size_t));
  cache->hash = (dt_hash_t *)((void *)cache->dsc + entries * sizeof(dt_iop_buffer_dsc_t));
  cache->used = (int32_t *)((void *)cache->hash + entries * sizeof(dt_hash_t));
  cache->ioporder = (int32_t *)((void *)cache->used + entries * sizeof(int32_t));

  for(int k = 0; k < entries; k++)
  {
    cache->hash[k] = INVALID_CACHEHASH;
    cache->used[k] = 64 + k;
  }
  if(!size) return TRUE;

  // some pixelpipes use preallocated cachelines, following code is special for those
  for(int k = 0; k < entries; k++)
  {
    cache->size[k] = size;
    cache->data[k] = (void *)dt_alloc_aligned(size);
    if(!cache->data[k])
      goto alloc_memory_fail;

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

void dt_dev_pixelpipe_cache_cleanup(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_t *cache = &pipe->cache;

  if(pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    dt_print(DT_DEBUG_PIPE, "Session fullpipe cache report. hits/run=%.2f, hits/test=%.3f",
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
}

static dt_hash_t _dev_pixelpipe_cache_basichash(const dt_imgid_t imgid,
                                                dt_dev_pixelpipe_t *pipe,
                                                const int order)
{
  /* What do we use for the basic hash
       1) imgid as all structures using the hash might possibly contain data from other images
       2) pipe->type as we want to keep status of fast mode included
       3) pipe->want_detail_mask makes sure old cachelines from before activating details are
          not valid any more.
          Do we have to keep the roi of details mask? No, as that is always defined by roi_in
          of the mask writing module (rawprepare or demosaic)
       4) The piece->hash of enabled modules within the given limit excluding the skipped
  */
  const uint32_t hashing_pipemode[3] = {(uint32_t)imgid,
                                        (uint32_t)pipe->type,
                                        (uint32_t)pipe->want_detail_mask };
  dt_hash_t hash = dt_hash(DT_INITHASH, &hashing_pipemode, sizeof(hashing_pipemode));

  // go through all modules up to iop_order and compute a hash using the operation and params.
  GList *pieces = pipe->nodes;
  while(pieces)
  {
    const dt_dev_pixelpipe_iop_t *piece = pieces->data;
    const dt_iop_module_t *mod = piece->module;

    // don't take skipped modules into account
    const gboolean skipped = dt_iop_module_is_skipped(mod->dev, mod)
                          && (pipe->type & DT_DEV_PIXELPIPE_BASIC);

    const gboolean relevant = mod->iop_order > 0
                           && mod->iop_order <= order
                           && piece->enabled;

    if(!skipped && relevant)
    {
      hash = dt_hash(hash, &piece->hash, sizeof(piece->hash));
      if(mod->request_color_pick != DT_REQUEST_COLORPICK_OFF)
      {
        if(darktable.lib->proxy.colorpicker.primary_sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
        {
          hash = dt_hash(hash, darktable.lib->proxy.colorpicker.primary_sample->box, sizeof(dt_pickerbox_t));
        }
        else if(darktable.lib->proxy.colorpicker.primary_sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
        {
          hash = dt_hash(hash, darktable.lib->proxy.colorpicker.primary_sample->point, 2 * sizeof(float));
        }
      }
    }
    pieces = g_list_next(pieces);
  }
  return hash;
}

dt_hash_t dt_dev_pixelpipe_cache_hash(const dt_imgid_t imgid,
                                      const dt_iop_roi_t *roi,
                                      dt_dev_pixelpipe_t *pipe,
                                      const int order)
{
  dt_hash_t hash = _dev_pixelpipe_cache_basichash(imgid, pipe, order);
  // also include roi data
  // FIXME include full roi data in cachelines
  hash = dt_hash(hash, roi, sizeof(dt_iop_roi_t));
  return dt_hash(hash, &pipe->scharr.hash, sizeof(pipe->scharr.hash));
}

gboolean dt_dev_pixelpipe_cache_available(dt_dev_pixelpipe_t *pipe,
                                          const dt_hash_t hash,
                                          const size_t size)
{
  if(pipe->mask_display
     || pipe->nocache
     || (hash == INVALID_CACHEHASH))
    return FALSE;

  dt_dev_pixelpipe_cache_t *cache = &pipe->cache;
  cache->tests++;
  // search for hash in cache and make the sizes are identical
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if((cache->size[k] == size) && (cache->hash[k] == hash))
    {
      cache->hits++;
      return TRUE;
    }
  }
  return FALSE;
}

// While looking for the oldest cacheline we always ignore the first two lines as they are used
// for swapping buffers while in entries==DT_PIPECACHE_MIN or masking mode
static int _get_oldest_cacheline(dt_dev_pixelpipe_cache_t *cache,
                                 const dt_dev_pixelpipe_cache_test_t mode)
{
  // we never want the latest used cacheline! It was <= 0 and the weight has increased just now
  int age = 1;
  int id = 0;
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    gboolean older = (cache->used[k] > age) && (k != cache->lastline);
    if(older)
    {
      if(mode == DT_CACHETEST_USED)         older = cache->data[k] != NULL;
      else if(mode == DT_CACHETEST_FREE)    older = cache->data[k] == NULL;
      else if(mode == DT_CACHETEST_INVALID) older = cache->hash[k] == INVALID_CACHEHASH;
      if(older)
      {
        age = cache->used[k];
        id = k;
      }
    }
  }
  return id;
}

static int __get_cacheline(dt_dev_pixelpipe_cache_t *cache)
{
  int oldest = _get_oldest_cacheline(cache, DT_CACHETEST_INVALID);
  if(oldest > 0) return oldest;

  oldest = _get_oldest_cacheline(cache, DT_CACHETEST_FREE);
  if(oldest > 0) return oldest;

  oldest = _get_oldest_cacheline(cache, DT_CACHETEST_PLAIN);
  return (oldest == 0) ? cache->calls & 1 : oldest;
}

static int _get_cacheline(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_t *cache = &pipe->cache;
  // If pipe has only two cachelines or we are in masking or nocache mode
  // we just toggle between the first two cachelines.
  // These are also taken if there is no valid cacheline returned
  if((cache->entries == DT_PIPECACHE_MIN) || pipe->mask_display || pipe->nocache)
    return cache->calls & 1;

  cache->lastline = __get_cacheline(cache);
  return cache->lastline;
}

// return TRUE in case of a hit
static gboolean _get_by_hash(dt_dev_pixelpipe_t *pipe,
                             const dt_iop_module_t *module,
                             const dt_hash_t hash,
                             const size_t size,
                             void **data,
                             dt_iop_buffer_dsc_t **dsc)
{
  dt_dev_pixelpipe_cache_t *cache = &pipe->cache;
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if(cache->hash[k] == hash)
    {
      if(cache->size[k] != size)
      {
        /* We check for situation with a hash identity but buffer sizes don't match.
           This could happen because of "hash overlaps" or other situations where the hash
           doesn't reflect the complete status.
           Anyway this has to be accepted as a dt bug so we always report
        */
        cache->hash[k] = INVALID_CACHEHASH;
        dt_print_pipe(DT_DEBUG_ALWAYS, "CACHELINE_SIZE ERROR",
          pipe, module, DT_DEVICE_NONE, NULL, NULL);
      }
      else if(pipe->mask_display || pipe->nocache)
      {
        // this should not happen but we make sure
        cache->hash[k] = INVALID_CACHEHASH;
      }
      else
      {
        // we have a proper hit
        *data = cache->data[k];
        *dsc = &cache->dsc[k];
        // in case of a hit it's always good to further keep the cacheline as important
        cache->used[k] = -cache->entries;
        return TRUE;
      }
    }
  }
  return FALSE;
}

gboolean dt_dev_pixelpipe_cache_get(dt_dev_pixelpipe_t *pipe,
                                    const dt_hash_t hash,
                                    const size_t size,
                                    void **data,
                                    dt_iop_buffer_dsc_t **dsc,
                                    dt_iop_module_t *module,
                                    const gboolean important)
{
  dt_dev_pixelpipe_cache_t *cache = &pipe->cache;
  cache->calls++;
  for(int k = 0; k < cache->entries; k++)
    cache->used[k]++; // age all entries

  // cache keeps history and we have a cache hit, so no new buffer
  if(cache->entries > DT_PIPECACHE_MIN
     && (hash != INVALID_CACHEHASH)
     && _get_by_hash(pipe, module, hash, size, data, dsc))
  {
    const dt_iop_buffer_dsc_t *cdsc = *dsc;
    dt_print_pipe(DT_DEBUG_PIPE, "cache HIT",
          pipe, module, DT_DEVICE_NONE, NULL, NULL,
          "%s, hash=%" PRIx64,
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
  const int cline = _get_cacheline(pipe);

  if(((cache->entries == DT_PIPECACHE_MIN) && (cache->size[cline] < size))
     || ((cache->entries > DT_PIPECACHE_MIN) && (cache->size[cline] != size)))
  {
    dt_free_align(cache->data[cline]);
    cache->allmem -= cache->size[cline];
    cache->data[cline] = (void *)dt_alloc_aligned(size);
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

  // first, update our copy, then update the pointer to point at our copy
  cache->dsc[cline] = **dsc;
  *dsc = &cache->dsc[cline];

  const gboolean masking = pipe->mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE;
  cache->hash[cline]      = masking ? INVALID_CACHEHASH : hash;

  const dt_iop_buffer_dsc_t *cdsc = *dsc;
  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE, "pipe cache get",
    pipe, module, DT_DEVICE_NONE, NULL, NULL,
    "%s %sline%3i(%2i) at %p. hash=%" PRIx64 "%s",
     dt_iop_colorspace_to_name(cdsc->cst),
     important ? "important " : "",
     cline, cache->used[cline], cache->data[cline], cache->hash[cline],
     masking ? ". masking." : "");

  cache->used[cline]      = !masking && important ? -cache->entries : 0;
  cache->ioporder[cline]  = module ? module->iop_order : 0;

  return TRUE;
}

static void _mark_invalid_cacheline(const dt_dev_pixelpipe_cache_t *cache, const int k)
{
  cache->hash[k] = INVALID_CACHEHASH;
  cache->ioporder[k] = 0;
}

void dt_dev_pixelpipe_cache_invalidate_later(const dt_dev_pixelpipe_t *pipe,
                                             const int32_t order)
{
  const dt_dev_pixelpipe_cache_t *cache = &pipe->cache;
  int invalidated = 0;
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if((cache->ioporder[k] >= order) && (cache->hash[k] != INVALID_CACHEHASH))
    {
      _mark_invalid_cacheline(cache, k);
      invalidated++;
    }
  }
  if(invalidated)
    dt_print_pipe(DT_DEBUG_PIPE,
    order ? "pipecache invalidate" : "pipecache flush",
    pipe, NULL, DT_DEVICE_NONE, NULL, NULL,
    "%i cachelines after ioporder=%i", invalidated, order);
}

void dt_dev_pixelpipe_cache_flush(const dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_invalidate_later(pipe, 0);
}

void dt_dev_pixelpipe_important_cacheline(const dt_dev_pixelpipe_t *pipe,
                                          const void *data,
                                          const size_t size)
{
  const dt_dev_pixelpipe_cache_t *cache = &pipe->cache;
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if((cache->data[k] == data)
        && (size == cache->size[k])
        && (cache->hash[k] != INVALID_CACHEHASH))
      cache->used[k] = -cache->entries;
  }
}

void dt_dev_pixelpipe_invalidate_cacheline(const dt_dev_pixelpipe_t *pipe,
                                           const void *data)
{
  const dt_dev_pixelpipe_cache_t *cache = &pipe->cache;
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if(cache->data[k] == data) _mark_invalid_cacheline(cache, k);
  }
}

static size_t _free_cacheline(dt_dev_pixelpipe_cache_t *cache, const int k)
{
  const size_t removed = cache->size[k];

  dt_free_align(cache->data[k]);
  cache->allmem -= removed;
  cache->size[k] = 0;
  cache->data[k] = NULL;
  _mark_invalid_cacheline(cache, k);
  return removed;
}

static void _cline_stats(dt_dev_pixelpipe_cache_t *cache)
{
  cache->lused = cache->linvalid = cache->limportant = 0;
  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if(cache->data[k]) cache->lused++;
    if(cache->data[k] && (cache->hash[k] == INVALID_CACHEHASH)) cache->linvalid++;
    if(cache->used[k] < 0) cache->limportant++;
  }
}

void dt_dev_pixelpipe_cache_checkmem(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);

  // we have pixelpipes like export & thumbnail that just use
  // alternating buffers so no cleanup
  if(cache->entries == DT_PIPECACHE_MIN) return;

  // We always free cachelines marked as not valid
  size_t freed = 0;

  for(int k = DT_PIPECACHE_MIN; k < cache->entries; k++)
  {
    if((cache->hash[k] == INVALID_CACHEHASH) && cache->data)
      freed += _free_cacheline(cache, k);
  }

  while(cache->memlimit && (cache->memlimit < cache->allmem))
  {
    const int k = _get_oldest_cacheline(cache, DT_CACHETEST_USED);
    if(k == 0) break;

    freed += _free_cacheline(cache, k);
  }

  _cline_stats(cache);
  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_MEMORY, "pipe cache check", pipe, NULL, DT_DEVICE_NONE, NULL, NULL,
    "%i lines (important=%i, used=%i). Freed %iMB. Using using %iMB, limit=%iMB",
    cache->entries, cache->limportant, cache->lused,
    _to_mb(freed), _to_mb(cache->allmem), _to_mb(cache->memlimit));
}

void dt_dev_pixelpipe_cache_report(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_t *cache = &(pipe->cache);

  _cline_stats(cache);
  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_MEMORY, "cache report", pipe, NULL, DT_DEVICE_NONE, NULL, NULL,
    "%i lines (important=%i, used=%i, invalid=%i). Using %iMB, limit=%iMB. Hits/run=%.2f. Hits/test=%.3f",
    cache->entries, cache->limportant, cache->lused, cache->linvalid,
    _to_mb(cache->allmem), _to_mb(cache->memlimit),
    (double)(cache->hits) / fmax(1.0, pipe->runs),
    (double)(cache->hits) / fmax(1.0, cache->tests));
}

#undef INVALID_CACHEHASH
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

