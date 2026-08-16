/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
  Generic support for reading a per-pixel scalar value produced on the
  preview pipe while flying over the image in the darkroom.

  iop modules (tone equalizer, color equalizer, ...) that want to
  display a value under the mouse cursor (exposure, hue, ...) previously
  had to duplicate buffer management, hashing and locking in each module.
  This service factorizes that common plumbing:

    - allocation/resizing of the per-pixel scalar buffer,
    - recording and checking the cumulative pipe hash,
    - thread-safe write (pipe thread) and read (GUI thread),
    - invalidation helpers.

  The service only deals with *data*.  How the per-pixel value is
  computed (the module's own process(), CPU or OpenCL) and how it is
  rendered on screen (the module's own gui_post_expose() and cursor
  handling) stay fully module specific.  The mapping between normalized
  cursor coordinates and buffer pixels is also left to the module:
  it depends on the module position in the pipe (geometry transforms
  located after the module, such as crop, must be inverted back to reach
  the module buffer space) and is therefore not part of this service.
*/

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include "common/darktable.h"
#include "develop/develop.h"
#include "develop/pixelpipe_hb.h"

typedef struct dt_preview_data_t
{
  float *buf;              // one float per pixel of the preview pipe
  size_t width;            // buffer width in pixels
  size_t height;           // buffer height in pixels
  dt_hash_t hash;          // cumulative pipe hash when the buffer was last filled
  const dt_iop_module_t *module;  // owning module
} dt_preview_data_t;

/** create/allocate the structure. */
void dt_preview_data_alloc(dt_preview_data_t *pd,
                           const dt_iop_module_t *module);

/** free allocated data. */
void dt_preview_data_free(dt_preview_data_t *pd);

/** fill the buffer with the per-pixel values; called under the module GUI lock. */
typedef void (*dt_preview_data_fill_t)(void *const user_data,
                                       float *const buf,
                                       const size_t npixels);

/** called under the module GUI lock after the buffer has been resized. */
typedef void (*dt_preview_data_resize_cb_t)(void *const user_data);

/**
 * Make sure the buffer has the given dimensions, call fill() while
 * holding the module GUI lock, then commit the cumulative pipe hash of
 * piece.  resize, fill and hash commit all happen atomically under a
 * single critical section, so that the GUI can never observe a resized
 * but not-yet-filled buffer.  Thread-safe.
 *
 * On allocation failure the previously stored data is invalidated and
 * the old buffer is kept (no fill, no new hash).
 *
 * Intended for cheap per-pixel copies (e.g. a hue buffer filled from
 * process() output).  Expensive fills that must not hold the GUI lock
 * should use dt_preview_data_resize() and commit afterwards with
 * dt_preview_data_set_hash().
 */
void dt_preview_data_store(dt_preview_data_t *pd,
                           const size_t width,
                           const size_t height,
                           const dt_dev_pixelpipe_iop_t *piece,
                           dt_preview_data_fill_t fill,
                           void *const user_data);

/**
 * Make sure the buffer has the given dimensions and return a pointer to
 * it (owned by the service, valid until the next resize()/free() call).
 * Returns NULL on allocation failure (the stored data is invalidated).
 *
 * If the buffer had to be resized, resize_cb() is called while still
 * holding the module GUI lock so that the module can atomically
 * invalidate its own dependent state (e.g. its "buffer valid" flag)
 * together with the resize.  Thread-safe.
 */
float *dt_preview_data_resize(dt_preview_data_t *pd,
                              const size_t width,
                              const size_t height,
                              dt_preview_data_resize_cb_t resize_cb,
                              void *const user_data);

/**
 * Record the cumulative hash of the module's piece in the preview
 * pipe.  Must be called after the buffer has been (re)filled in memory
 * returned by dt_preview_data_resize() so that is_fresh() can later
 * validate the data.  Thread-safe.
 */
void dt_preview_data_set_hash(dt_preview_data_t *pd,
                              const dt_dev_pixelpipe_iop_t *piece);

/**
 * Read the scalar value at buffer pixel (x, y).
 *
 * Thread-safe: takes the module GUI lock around the read.
 * Returns TRUE and fills *value on success, FALSE if there is no data
 * stored yet or if (x, y) is outside the buffer.
 *
 * @param x, y: coordinates in buffer pixels.  The mapping from the
 *              cursor position to buffer pixels is module specific and
 *              must be done by the caller (see comment at top of file).
 */
gboolean dt_preview_data_get(dt_preview_data_t *pd,
                             const size_t x,
                             const size_t y,
                             float *value);

/**
 * Check whether the stored data is still fresh with respect to the
 * current preview pipe: TRUE when a value is stored and the cumulative
 * hash of the module's piece in the preview pipe still matches the hash
 * recorded at store time.  FALSE when nothing is stored yet or when the
 * upstream pipe state has changed (the module needs a reprocess).
 *
 * Thread-safe.
 */
gboolean dt_preview_data_is_fresh(dt_preview_data_t *pd);

/**
 * Return the cumulative pipe hash recorded at the last store, so the
 * module can decide whether its per-pixel value needs to be recomputed
 * (compared against the hash of the current pipe state).  Thread-safe.
 */
dt_hash_t dt_preview_data_get_hash(dt_preview_data_t *pd);

/**
 * Invalidate the stored data (mark the recorded hash as stale).
 * The buffer itself is kept and may be read but is_fresh() will return
 * FALSE until a new set_hash() happens.  Thread-safe.
 */
void dt_preview_data_invalidate(dt_preview_data_t *pd);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on