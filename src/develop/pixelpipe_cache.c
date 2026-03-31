/*
    This file is part of darktable,
    Copyright (C) 2009-2026 darktable developers.

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
#include "develop/pixelpipe.h"
#include "libs/lib.h"
#include "libs/colorpicker.h"
#include <stdlib.h>

// ---- hash table helpers for dt_hash_t keys ----

static guint _hash_func(gconstpointer key)
{
  const dt_hash_t h = *(const dt_hash_t *)key;
  // mix upper and lower 32 bits for a good 32-bit hash
  return (guint)(h ^ (h >> 32));
}

static gboolean _equal_func(gconstpointer a, gconstpointer b)
{
  return *(const dt_hash_t *)a == *(const dt_hash_t *)b;
}

static inline int _to_mb(size_t m)
{
  return (int)((m + 0x80000lu) / 0x400lu / 0x400lu);
}

// mask for the base pipe type bits (excluding FAST, IMAGE, etc.)
#define DT_PIPETYPE_BASE_MASK (DT_DEV_PIXELPIPE_ANY)

static inline int _pipe_base_type(const dt_dev_pixelpipe_t *pipe)
{
  return pipe->type & DT_PIPETYPE_BASE_MASK;
}

// ---- global cache lifecycle ----

void dt_dev_pixelpipe_cache_init_global(size_t max_memory)
{
  darktable.pipeline_cache = calloc(1, sizeof(dt_dev_pixelpipe_cache_t));
  dt_dev_pixelpipe_cache_t *cache = darktable.pipeline_cache;
  cache->lookup = g_hash_table_new(_hash_func, _equal_func);
  cache->entries = NULL;
  cache->max_memory = max_memory;
  cache->current_memory = 0;
  dt_pthread_mutex_init(&cache->lock, NULL);
  cache->calls = cache->tests = cache->hits = 0;

  dt_print(DT_DEBUG_PIPE | DT_DEBUG_MEMORY,
           "[pipeline cache] initialized global cache with %iMB budget",
           _to_mb(max_memory));
}

void dt_dev_pixelpipe_cache_cleanup_global(void)
{
  dt_dev_pixelpipe_cache_t *cache = darktable.pipeline_cache;
  dt_pthread_mutex_lock(&cache->lock);

  // free all entries
  for(GList *l = cache->entries; l; l = g_list_next(l))
  {
    dt_pixel_cache_entry_t *entry = l->data;
    dt_free_align(entry->data);
    free(entry);
  }
  g_list_free(cache->entries);
  cache->entries = NULL;
  g_hash_table_destroy(cache->lookup);
  cache->lookup = NULL;
  cache->current_memory = 0;

  dt_pthread_mutex_unlock(&cache->lock);
  dt_pthread_mutex_destroy(&cache->lock);

  free(darktable.pipeline_cache);
  darktable.pipeline_cache = NULL;
}

// ---- per-pipe scratch buffer init/cleanup ----

gboolean dt_dev_pixelpipe_cache_init(dt_dev_pixelpipe_t *pipe,
                                     const int entries,
                                     const size_t size,
                                     const size_t limit)
{
  // Initialize scratch buffers
  pipe->scratch_calls = 0;
  for(int k = 0; k < 2; k++)
  {
    pipe->scratch_data[k] = NULL;
    pipe->scratch_size[k] = 0;
    memset(&pipe->scratch_dsc[k], 0, sizeof(dt_iop_buffer_dsc_t));
  }

  // Determine if this pipe uses the global cache.
  // Export, thumbnail, and dummy pipes (entries <= DT_PIPECACHE_MIN) use scratch buffers only.
  pipe->use_cache = darktable.pipe_cache && (entries > DT_PIPECACHE_MIN);

  if(!size) return TRUE;

  // Pre-allocate scratch buffers for export/thumbnail pipes
  for(int k = 0; k < 2; k++)
  {
    pipe->scratch_data[k] = (void *)dt_alloc_aligned(size);
    if(!pipe->scratch_data[k])
    {
      // cleanup on failure
      for(int j = 0; j < 2; j++)
      {
        dt_free_align(pipe->scratch_data[j]);
        pipe->scratch_data[j] = NULL;
        pipe->scratch_size[j] = 0;
      }
      return FALSE;
    }
    pipe->scratch_size[k] = size;
  }
  return TRUE;
}

void dt_dev_pixelpipe_cache_cleanup(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_t *cache = darktable.pipeline_cache;

  if(cache && pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    dt_print(DT_DEBUG_PIPE, "Session fullpipe cache report. hits/run=%.2f, hits/test=%.3f",
    (double)(cache->hits) / fmax(1.0, pipe->runs),
    (double)(cache->hits) / fmax(1.0, cache->tests));
  }

  // Flush this pipe's entries from the global cache
  if(cache) dt_dev_pixelpipe_cache_flush(pipe);

  // Free scratch buffers
  for(int k = 0; k < 2; k++)
  {
    dt_free_align(pipe->scratch_data[k]);
    pipe->scratch_data[k] = NULL;
    pipe->scratch_size[k] = 0;
  }
}

// ---- global_hash computation (WP1) ----

void dt_dev_pixelpipe_compute_global_hashes(dt_dev_pixelpipe_t *pipe)
{
  /* Pre-compute cumulative hashes for every piece in the pipe.
     Each piece->global_hash represents the hash of:
       - pipe identity (imgid, type, want_detail_mask)
       - color profiles
       - all upstream piece->hash values (including this piece)
     This allows O(1) cache lookups instead of O(N) chain walks
     when no color picker is active.
     Color picker state is NOT included because it can change
     between sync and processing.
  */
  const uint32_t hashing_pipemode[3] = {(uint32_t)pipe->image.id,
                                         (uint32_t)pipe->type,
                                         (uint32_t)pipe->want_detail_mask };
  dt_hash_t cumulative = dt_hash(DT_INITHASH, &hashing_pipemode, sizeof(uint32_t) * 3);
  cumulative = dt_hash(cumulative, &pipe->input_profile_info, sizeof(pipe->input_profile_info));
  cumulative = dt_hash(cumulative, &pipe->work_profile_info, sizeof(pipe->work_profile_info));
  cumulative = dt_hash(cumulative, &pipe->output_profile_info, sizeof(pipe->output_profile_info));

  for(GList *nodes = pipe->nodes; nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = nodes->data;
    const gboolean included = piece->module->enabled || piece->enabled;
    const gboolean skipped = dt_iop_module_is_skipped(piece->module->dev, piece->module)
      && (pipe->type & DT_DEV_PIXELPIPE_BASIC);
    if(!skipped && included)
    {
      cumulative = dt_hash(cumulative, &piece->hash, sizeof(piece->hash));
    }
    piece->global_hash = cumulative;
  }
}

// ---- hash computation ----

static dt_hash_t _dev_pixelpipe_cache_basichash(dt_dev_pixelpipe_t *pipe,
                                                const int position,
                                                const dt_iop_roi_t *roi)
{
  /* What do we use for the basic hash
       1) imgid as all structures using the hash might possibly contain data from other images
       2) pipe->type for the cache it's important to keep status of fast mode included here
           also, we might use the hash also for different pipe.
       3) pipe->want_detail_mask make sure old cachelines from before activating details are
          not valid any more.
          Do we have to keep the roi of details mask? No as that is always defined by roi_in
          of the mask writing module (rawprepare or demosaic)
       4) If we change any color profile they are committed for every history entry so we can't check
          for them in the piece->hash but must use the final profiles available via pipe->xxx_profile_info
       5) Please note that position is not the iop_order but the position in the pipe
       6) Please note that pipe->type, want_details and request_color_pick are only used if a roi is provided
          for better support of dt_dev_pixelpipe_piece_hash()
  */

  // Fast path: use precomputed global_hash when available and roi is provided.
  // The global_hash includes pipe identity (imgid + type + want_detail_mask) and profiles,
  // which matches the roi!=NULL case. We can only use it if no color picker is active
  // in any upstream module, since picker state is not baked into global_hash.
  if(roi && position > 0)
  {
    // find the piece at position-1 (0-based index)
    GList *node = g_list_nth(pipe->nodes, position - 1);
    if(node)
    {
      dt_dev_pixelpipe_iop_t *piece = node->data;
      if(piece->global_hash != DT_INVALID_HASH)
      {
        // check if any upstream module has an active color picker
        gboolean has_picker = FALSE;
        GList *check = pipe->nodes;
        for(int k = 0; k < position && check; k++)
        {
          dt_dev_pixelpipe_iop_t *p = check->data;
          if(p->module->request_color_pick != DT_REQUEST_COLORPICK_OFF)
          {
            has_picker = TRUE;
            break;
          }
          check = g_list_next(check);
        }

        if(!has_picker)
          return piece->global_hash;
      }
    }
  }

  // Slow path: walk the chain (used when roi==NULL, global_hash not computed, or picker active)
  const uint32_t hashing_pipemode[3] = {(uint32_t)pipe->image.id,
                                         (uint32_t)pipe->type,
                                         (uint32_t)pipe->want_detail_mask };
  dt_hash_t hash = dt_hash(DT_INITHASH, &hashing_pipemode, sizeof(uint32_t) * (roi ? 3 : 1));
  hash = dt_hash(hash, &pipe->input_profile_info, sizeof(pipe->input_profile_info));
  hash = dt_hash(hash, &pipe->work_profile_info, sizeof(pipe->work_profile_info));
  hash = dt_hash(hash, &pipe->output_profile_info, sizeof(pipe->output_profile_info));

  // go through all modules up to position and compute a hash using the operation and params.
  GList *pieces = pipe->nodes;
  for(int k = 0; k < position && pieces; k++)
  {
    dt_dev_pixelpipe_iop_t *piece = pieces->data;
    // As this runs through all pipe nodes - also the ones not commited -
    // we can safely avoid disabled modules/pieces
    const gboolean included = piece->module->enabled || piece->enabled;
    // don't take skipped modules into account
    const gboolean skipped = dt_iop_module_is_skipped(piece->module->dev, piece->module)
      && (pipe->type & DT_DEV_PIXELPIPE_BASIC);
    if(!skipped && included)
    {
      hash = dt_hash(hash, &piece->hash, sizeof(piece->hash));
      if(piece->module->request_color_pick != DT_REQUEST_COLORPICK_OFF && roi)
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

/* If we don't provide a roi this reflects the parameters including blending of all used pieces
   in the pipe until the provided postion.
*/
dt_hash_t dt_dev_pixelpipe_cache_hash(const dt_iop_roi_t *roi,
                                      dt_dev_pixelpipe_t *pipe,
                                      const int position)
{
  dt_hash_t hash = _dev_pixelpipe_cache_basichash(pipe, position, roi);
  // also include roi data if provided
  if(roi)
  {
    hash = dt_hash(hash, roi, sizeof(dt_iop_roi_t));
    hash = dt_hash(hash, &pipe->scharr.hash, sizeof(pipe->scharr.hash));
  }
  return hash;
}

// ---- internal helpers ----

// Find an entry by data pointer in the entries list. Returns entry or NULL.
// Caller must hold cache->lock.
static dt_pixel_cache_entry_t *_find_entry_by_data(dt_dev_pixelpipe_cache_t *cache,
                                                    const void *data)
{
  for(GList *l = cache->entries; l; l = g_list_next(l))
  {
    dt_pixel_cache_entry_t *entry = l->data;
    if(entry->data == data) return entry;
  }
  return NULL;
}

// ---- public API ----

gboolean dt_dev_pixelpipe_cache_available(dt_dev_pixelpipe_t *pipe,
                                          const dt_hash_t hash,
                                          const size_t size)
{
  if(pipe->mask_display
     || pipe->nocache
     || !pipe->use_cache
     || (hash == DT_INVALID_HASH))
    return FALSE;

  dt_dev_pixelpipe_cache_t *cache = darktable.pipeline_cache;
  dt_pthread_mutex_lock(&cache->lock);
  cache->tests++;

  dt_pixel_cache_entry_t *entry = g_hash_table_lookup(cache->lookup, &hash);
  const gboolean found = entry
                          && entry->hash != DT_INVALID_HASH
                          && entry->size == size;
  if(found)
    cache->hits++;

  dt_pthread_mutex_unlock(&cache->lock);
  return found;
}

gboolean dt_dev_pixelpipe_cache_get(dt_dev_pixelpipe_t *pipe,
                                    const dt_hash_t hash,
                                    const size_t size,
                                    void **data,
                                    dt_iop_buffer_dsc_t **dsc,
                                    const dt_iop_module_t *module,
                                    const gboolean important)
{
  // Scratch buffer path: used for export/thumbnail pipes, masking mode, nocache mode
  const gboolean use_scratch = !pipe->use_cache
    || pipe->mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE
    || pipe->nocache;

  if(use_scratch)
  {
    const int idx = pipe->scratch_calls++ & 1;

    // Grow scratch buffer if needed (never shrink)
    if(pipe->scratch_size[idx] < size)
    {
      dt_free_align(pipe->scratch_data[idx]);
      pipe->scratch_data[idx] = (void *)dt_alloc_aligned(size);
      pipe->scratch_size[idx] = pipe->scratch_data[idx] ? size : 0;
    }

    *data = pipe->scratch_data[idx];

    // Copy caller's dsc into scratch, redirect caller to scratch copy
    pipe->scratch_dsc[idx] = **dsc;
    *dsc = &pipe->scratch_dsc[idx];
    return TRUE; // always "new" (no cache hit possible)
  }

  // Global cache path
  dt_dev_pixelpipe_cache_t *cache = darktable.pipeline_cache;
  dt_pthread_mutex_lock(&cache->lock);
  cache->calls++;

  // Check for cache hit
  dt_pixel_cache_entry_t *entry = g_hash_table_lookup(cache->lookup, &hash);
  if(entry && entry->hash != DT_INVALID_HASH && entry->size == size)
  {
    // HIT
    entry->last_access = g_get_monotonic_time();
    if(important) entry->important = TRUE;
    *data = entry->data;
    *dsc = &entry->dsc;
    cache->hits++;

    const dt_iop_buffer_dsc_t *cdsc = *dsc;
    dt_print_pipe(DT_DEBUG_PIPE, "cache HIT",
          pipe, module, DT_DEVICE_NONE, NULL, NULL,
          "%s %.3f %.3f %.3f, hash=%" PRIx64,
          dt_iop_colorspace_to_name(cdsc->cst), cdsc->temperature.coeffs[0], cdsc->temperature.coeffs[1], cdsc->temperature.coeffs[2],
          hash);

    dt_pthread_mutex_unlock(&cache->lock);
    return FALSE; // not new
  }

  // Handle size mismatch (hash collision or bug)
  if(entry && entry->hash != DT_INVALID_HASH && entry->size != size)
  {
    dt_print_pipe(DT_DEBUG_ALWAYS, "CACHELINE_SIZE ERROR",
      pipe, module, DT_DEVICE_NONE, NULL, NULL);
    // Remove the bad entry
    g_hash_table_steal(cache->lookup, &entry->hash);
    cache->entries = g_list_remove(cache->entries, entry);
    cache->current_memory -= entry->size;
    dt_free_align(entry->data);
    free(entry);
    entry = NULL;
  }

  // MISS: allocate new entry
  dt_pixel_cache_entry_t *new_entry = calloc(1, sizeof(dt_pixel_cache_entry_t));
  new_entry->hash = hash;
  new_entry->data = (void *)dt_alloc_aligned(size);
  new_entry->size = new_entry->data ? size : 0;
  new_entry->dsc = **dsc;  // copy caller's dsc in
  new_entry->last_access = g_get_monotonic_time();
  new_entry->ioporder = module ? module->iop_order : 0;
  new_entry->important = important;
  new_entry->pipe_type = _pipe_base_type(pipe);

  const gboolean masking = pipe->mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE;

  // Don't insert into lookup table if masking (hash would be unreliable)
  if(!masking && hash != DT_INVALID_HASH)
    g_hash_table_insert(cache->lookup, &new_entry->hash, new_entry);

  cache->entries = g_list_prepend(cache->entries, new_entry);
  cache->current_memory += new_entry->size;

  *data = new_entry->data;

  // Redirect caller's dsc to the entry's copy
  *dsc = &new_entry->dsc;

  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE, "pipe cache get",
    pipe, module, DT_DEVICE_NONE, NULL, NULL,
    "%s %sat %p. hash=%" PRIx64 "%s",
     dt_iop_colorspace_to_name(new_entry->dsc.cst),
     important ? "important " : "",
     new_entry->data, new_entry->hash,
     masking ? ". masking." : "");

  dt_pthread_mutex_unlock(&cache->lock);
  return TRUE; // new buffer
}

void dt_dev_pixelpipe_cache_invalidate_later(dt_dev_pixelpipe_t *pipe,
                                             const int32_t order)
{
  dt_dev_pixelpipe_cache_t *cache = darktable.pipeline_cache;
  dt_pthread_mutex_lock(&cache->lock);

  const int base_type = _pipe_base_type(pipe);
  int invalidated = 0;

  for(GList *l = cache->entries; l; l = g_list_next(l))
  {
    dt_pixel_cache_entry_t *entry = l->data;
    if(entry->pipe_type == base_type
       && entry->ioporder >= order
       && entry->hash != DT_INVALID_HASH)
    {
      // Remove from lookup table, mark as invalid
      g_hash_table_steal(cache->lookup, &entry->hash);
      entry->hash = DT_INVALID_HASH;
      entry->ioporder = 0;
      invalidated++;
    }
  }

  dt_pthread_mutex_unlock(&cache->lock);

  // bcache is per-pipe, handle outside the lock
  const gboolean bcache = pipe->bcache_data != NULL && pipe->bcache_hash != DT_INVALID_HASH;
  pipe->bcache_hash = DT_INVALID_HASH;

  if(invalidated || bcache)
    dt_print_pipe(DT_DEBUG_PIPE,
    order ? "pipecache invalidate" : "pipecache flush",
    pipe, NULL, DT_DEVICE_NONE, NULL, NULL,
    "%i cachelines after ioporder=%i%s",
    invalidated, order, bcache ? ", blend cache" : "");
}

void dt_dev_pixelpipe_cache_flush(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_invalidate_later(pipe, 0);
}

void dt_dev_pixelpipe_important_cacheline(const dt_dev_pixelpipe_t *pipe,
                                          const void *data,
                                          const size_t size)
{
  dt_dev_pixelpipe_cache_t *cache = darktable.pipeline_cache;
  dt_pthread_mutex_lock(&cache->lock);

  dt_pixel_cache_entry_t *entry = _find_entry_by_data(cache, data);
  if(entry && entry->size == size && entry->hash != DT_INVALID_HASH)
    entry->important = TRUE;

  dt_pthread_mutex_unlock(&cache->lock);
}

void dt_dev_pixelpipe_invalidate_cacheline(const dt_dev_pixelpipe_t *pipe,
                                           const void *data)
{
  dt_dev_pixelpipe_cache_t *cache = darktable.pipeline_cache;
  dt_pthread_mutex_lock(&cache->lock);

  dt_pixel_cache_entry_t *entry = _find_entry_by_data(cache, data);
  if(entry)
  {
    if(entry->hash != DT_INVALID_HASH)
      g_hash_table_steal(cache->lookup, &entry->hash);
    entry->hash = DT_INVALID_HASH;
    entry->ioporder = 0;
  }

  dt_pthread_mutex_unlock(&cache->lock);
}

void dt_dev_pixelpipe_cache_checkmem(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_t *cache = darktable.pipeline_cache;

  // Export/thumbnail pipes have no cache interaction
  if(!pipe->use_cache) return;

  dt_pthread_mutex_lock(&cache->lock);

  const int base_type = _pipe_base_type(pipe);
  size_t freed_invalid = 0;

  // First pass: free all invalid entries for this pipe type
  GList *l = cache->entries;
  while(l)
  {
    GList *next = g_list_next(l);
    dt_pixel_cache_entry_t *entry = l->data;
    if(entry->hash == DT_INVALID_HASH && entry->pipe_type == base_type)
    {
      freed_invalid += entry->size;
      cache->current_memory -= entry->size;
      cache->entries = g_list_delete_link(cache->entries, l);
      dt_free_align(entry->data);
      free(entry);
    }
    l = next;
  }

  // Second pass: evict LRU if still over budget.
  // IMPORTANT: only evict entries from THIS pipe's type. Other pipes may be
  // actively processing with pointers into their cache entries. Freeing those
  // entries would cause use-after-free (the caller holds raw pointers to
  // entry->data and entry->dsc during the entire recursive processing chain).
  size_t freed_lru = 0;
  while(cache->max_memory && cache->current_memory > cache->max_memory)
  {
    dt_pixel_cache_entry_t *victim = NULL;
    int64_t oldest = G_MAXINT64;
    for(GList *m = cache->entries; m; m = g_list_next(m))
    {
      dt_pixel_cache_entry_t *e = m->data;
      if(e->pipe_type == base_type && !e->important && e->last_access < oldest)
      {
        oldest = e->last_access;
        victim = e;
      }
    }
    if(!victim)
    {
      // All entries of this type are important — evict oldest important as last resort
      for(GList *m = cache->entries; m; m = g_list_next(m))
      {
        dt_pixel_cache_entry_t *e = m->data;
        if(e->pipe_type == base_type && e->last_access < oldest)
        {
          oldest = e->last_access;
          victim = e;
        }
      }
    }
    if(!victim) break;

    if(victim->hash != DT_INVALID_HASH)
      g_hash_table_steal(cache->lookup, &victim->hash);

    freed_lru += victim->size;
    cache->current_memory -= victim->size;
    cache->entries = g_list_remove(cache->entries, victim);
    dt_free_align(victim->data);
    free(victim);
  }

  // Compute stats for reporting
  uint32_t lused = 0, linvalid = 0, limportant = 0;
  for(GList *m = cache->entries; m; m = g_list_next(m))
  {
    dt_pixel_cache_entry_t *e = m->data;
    if(e->data) lused++;
    if(e->hash == DT_INVALID_HASH) linvalid++;
    if(e->important) limportant++;
  }

  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_MEMORY, "pipe cache check", pipe, NULL, DT_DEVICE_NONE, NULL, NULL,
    "%u lines (important=%u, used=%u, invalid=%u). Freed: invalid %iMB lru %iMB. Using %iMB, limit=%iMB",
    g_list_length(cache->entries), limportant, lused, linvalid,
    _to_mb(freed_invalid), _to_mb(freed_lru), _to_mb(cache->current_memory), _to_mb(cache->max_memory));

  dt_pthread_mutex_unlock(&cache->lock);
}

void dt_dev_pixelpipe_cache_report(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_t *cache = darktable.pipeline_cache;
  dt_pthread_mutex_lock(&cache->lock);

  uint32_t lused = 0, linvalid = 0, limportant = 0;
  for(GList *l = cache->entries; l; l = g_list_next(l))
  {
    dt_pixel_cache_entry_t *e = l->data;
    if(e->data) lused++;
    if(e->hash == DT_INVALID_HASH) linvalid++;
    if(e->important) limportant++;
  }

  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_MEMORY, "cache report", pipe, NULL, DT_DEVICE_NONE, NULL, NULL,
    "%u lines (important=%u, used=%u, invalid=%u). Using %iMB, limit=%iMB. Hits/run=%.2f. Hits/test=%.3f",
    g_list_length(cache->entries), limportant, lused, linvalid,
    _to_mb(cache->current_memory), _to_mb(cache->max_memory),
    (double)(cache->hits) / fmax(1.0, pipe->runs),
    (double)(cache->hits) / fmax(1.0, cache->tests));

  dt_pthread_mutex_unlock(&cache->lock);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
