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

#pragma once

#include <inttypes.h>
#include <glib.h>
#include "common/dtpthread.h"

struct dt_dev_pixelpipe_t;
struct dt_iop_buffer_dsc_t;
struct dt_iop_roi_t;
struct dt_iop_module_t;

/**
 * Global pixel cache for darktable's processing pipeline.
 *
 * A single cache instance is shared by all pipe types (full, preview, preview2).
 * Cache entries are keyed by hash (computed from pipe identity + module params + ROI).
 * Memory is bounded by max_memory with LRU eviction.
 * Thread-safe via mutex (multiple pipes process concurrently).
 *
 * Export/thumbnail pipes use per-pipe scratch buffers instead of the global cache.
 */

/** A single cached pixel buffer with metadata. */
typedef struct dt_pixel_cache_entry_t
{
  dt_hash_t hash;           // cache key (DT_INVALID_HASH when invalidated)
  void *data;               // pixel buffer (dt_alloc_aligned)
  size_t size;              // buffer size in bytes
  struct dt_iop_buffer_dsc_t dsc;  // buffer descriptor (colorspace, temperature, etc.)
  int64_t last_access;      // g_get_monotonic_time() when last accessed
  int32_t ioporder;         // iop_order of the module that created this entry
  gboolean important;       // TRUE = prefer keeping in cache (e.g., focused module input)
  int pipe_type;            // dt_dev_pixelpipe_type_t base bits of the pipe that created this
} dt_pixel_cache_entry_t;

/** The global pixel cache. One instance in darktable_t. */
typedef struct dt_dev_pixelpipe_cache_t
{
  GHashTable *lookup;       // dt_hash_t* -> dt_pixel_cache_entry_t* (O(1) lookup by hash)
  GList *entries;           // all dt_pixel_cache_entry_t* (for iteration, eviction)
  size_t max_memory;        // memory budget in bytes
  size_t current_memory;    // sum of all entry->size
  dt_pthread_mutex_t lock;  // protects all fields
  // profiling & stats
  uint64_t calls;
  uint64_t tests;
  uint64_t hits;
} dt_dev_pixelpipe_cache_t;

// --- Global cache lifecycle (called once at startup/shutdown) ---

void dt_dev_pixelpipe_cache_init_global(size_t max_memory);
void dt_dev_pixelpipe_cache_cleanup_global(void);

// --- Per-pipe scratch buffer init/cleanup ---

/** Initialize per-pipe scratch buffers. scratch_size > 0 pre-allocates buffers
    (used by export/thumbnail pipes). */
gboolean dt_dev_pixelpipe_cache_init(struct dt_dev_pixelpipe_t *pipe,
                                     const int entries, const size_t size, const size_t limit);
void dt_dev_pixelpipe_cache_cleanup(struct dt_dev_pixelpipe_t *pipe);

// --- Cache operations (all take pipe for context: pipe->type, masking state, etc.) ---

/** creates a hopefully unique hash from the complete module stack up to the module-th, including the roi. */
dt_hash_t dt_dev_pixelpipe_cache_hash(const struct dt_iop_roi_t *roi,
                                     struct dt_dev_pixelpipe_t *pipe, const int position);

/** returns a float data buffer in 'data' for the given hash from the cache, dsc is updated too.
  If the hash does not match any cache line, use an old buffer or allocate a fresh one.
  The size of the buffer in 'data' will be at least of size bytes.
  Returned flag is TRUE for a new buffer
*/
gboolean dt_dev_pixelpipe_cache_get(struct dt_dev_pixelpipe_t *pipe, const dt_hash_t hash,
                               const size_t size, void **data, struct dt_iop_buffer_dsc_t **dsc,
                               const struct dt_iop_module_t *module, const gboolean important);

/** test availability of a cache line without destroying another, if it is not found. */
gboolean dt_dev_pixelpipe_cache_available(struct dt_dev_pixelpipe_t *pipe, const dt_hash_t hash, const size_t size);

/** invalidates all cachelines for this pipe type. */
void dt_dev_pixelpipe_cache_flush(struct dt_dev_pixelpipe_t *pipe);

/** invalidates all cachelines for modules with at least the same iop_order */
void dt_dev_pixelpipe_cache_invalidate_later(struct dt_dev_pixelpipe_t *pipe, const int32_t order);

/** makes this buffer very important after it has been pulled from the cache. */
void dt_dev_pixelpipe_important_cacheline(const struct dt_dev_pixelpipe_t *pipe, const void *data, const size_t size);

/** mark the given cache line as invalid or to be ignored */
void dt_dev_pixelpipe_invalidate_cacheline(const struct dt_dev_pixelpipe_t *pipe, const void *data);

/** print out cache lines/hashes and do a cache cleanup */
void dt_dev_pixelpipe_cache_report(struct dt_dev_pixelpipe_t *pipe);
void dt_dev_pixelpipe_cache_checkmem(struct dt_dev_pixelpipe_t *pipe);

/** pre-computes cumulative global_hash on every pipe piece.
    Must be called after sync and before pipeline processing. */
void dt_dev_pixelpipe_compute_global_hashes(struct dt_dev_pixelpipe_t *pipe);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
