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

#include "develop/preview_data.h"

#include "common/darktable.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/pixelpipe_hb.h"

void dt_preview_data_alloc(dt_preview_data_t *pd,
                           const dt_iop_module_t *module)
{
  pd->buf = NULL;
  pd->width = 0;
  pd->height = 0;
  pd->components = 1;
  pd->hash = DT_INVALID_HASH;
  pd->module = module;
}

void dt_preview_data_free(dt_preview_data_t *pd)
{
  if(pd->buf)
  {
    dt_free_align(pd->buf);
    pd->buf = NULL;
  }
  pd->width = 0;
  pd->height = 0;
  pd->hash = DT_INVALID_HASH;
  pd->module = NULL;
}

void dt_preview_data_store(dt_preview_data_t *pd,
                           const size_t width,
                           const size_t height,
                           const dt_dev_pixelpipe_iop_t *piece,
                           dt_preview_data_fill_t fill,
                           void *const user_data)
{
  if(!pd || !pd->module || !fill || !piece) return;

  // resize, fill and hash commit all happen under the same GUI lock so
  // that the GUI thread can never observe a resized but not-yet-filled
  // buffer (a window the earlier ensure()/fill/set_hash split exposed).
  dt_iop_gui_enter_critical_section((dt_iop_module_t *)pd->module);

  const size_t nelems = width * height * pd->components;
  gboolean can_fill = TRUE;
  if(pd->width != width || pd->height != height)
  {
    float *const new_buf = dt_alloc_align_float(nelems);
    if(new_buf)
    {
      dt_free_align(pd->buf);
      pd->buf = new_buf;
      pd->width = width;
      pd->height = height;
    }
    else
    {
      pd->hash = DT_INVALID_HASH;
      can_fill = FALSE;
    }
  }

  if(can_fill && pd->buf)
  {
    fill(user_data, pd->buf, nelems);
    pd->hash = dt_dev_pixelpipe_piece_hash((dt_dev_pixelpipe_iop_t *)piece,
                                           &piece->processed_roi_out, TRUE);
  }

  dt_iop_gui_leave_critical_section((dt_iop_module_t *)pd->module);
}

float *dt_preview_data_resize(dt_preview_data_t *pd,
                              const size_t width,
                              const size_t height,
                              dt_preview_data_resize_cb_t resize_cb,
                              void *const user_data)
{
  if(!pd || !pd->module) return NULL;

  // The resize and the caller's invalidation callback run under the same
  // GUI lock so that the module can atomically mark its dependent state
  // stale together with the buffer reallocation.
  dt_iop_gui_enter_critical_section((dt_iop_module_t *)pd->module);

  gboolean ok = TRUE;
  if(pd->width != width || pd->height != height)
  {
    float *const new_buf = dt_alloc_align_float(width * height * pd->components);
    if(new_buf)
    {
      dt_free_align(pd->buf);
      pd->buf = new_buf;
      pd->width = width;
      pd->height = height;
    }
    else
    {
      pd->hash = DT_INVALID_HASH;
      ok = FALSE;
    }
    if(resize_cb) resize_cb(user_data);
  }

  float *const buf = ok ? pd->buf : NULL;
  dt_iop_gui_leave_critical_section((dt_iop_module_t *)pd->module);

  return buf;
}

void dt_preview_data_set_hash(dt_preview_data_t *pd,
                              const dt_dev_pixelpipe_iop_t *piece)
{
  if(!pd || !pd->module) return;

  dt_iop_gui_enter_critical_section((dt_iop_module_t *)pd->module);
  pd->hash = dt_dev_pixelpipe_piece_hash((dt_dev_pixelpipe_iop_t *)piece,
                                         &piece->processed_roi_out, TRUE);
  dt_iop_gui_leave_critical_section((dt_iop_module_t *)pd->module);
}

gboolean dt_preview_data_get(dt_preview_data_t *pd,
                             const size_t x,
                             const size_t y,
                             const size_t comp,
                             float *value)
{
  if(!pd || !pd->module || !value) return FALSE;

  dt_iop_gui_enter_critical_section((dt_iop_module_t *)pd->module);

  gboolean ok = FALSE;
  float v = 0.f;
  // The bounds check and the buffer read must both happen under the
  // GUI lock: the pipe thread may resize pd->buf between the two.
  if(pd->buf && x < pd->width && y < pd->height && comp < pd->components)
  {
    const size_t idx = (((size_t)y * pd->width + (size_t)x) * pd->components) + comp;
    v = pd->buf[idx];
    ok = TRUE;
  }

  dt_iop_gui_leave_critical_section((dt_iop_module_t *)pd->module);

  *value = v;
  return ok;
}

gboolean dt_preview_data_is_fresh(dt_preview_data_t *pd)
{
  if(!pd || !pd->module || !pd->buf) return FALSE;

  const dt_iop_module_t *const module = pd->module;
  dt_iop_gui_enter_critical_section((dt_iop_module_t *)module);

  // No value stored yet, or the stored data has been invalidated.
  const dt_hash_t stored_hash = pd->hash;
  gboolean fresh = (stored_hash != DT_INVALID_HASH);

  if(fresh)
  {
    dt_dev_pixelpipe_t *const pipe = module->dev ? module->dev->preview_pipe : NULL;
    if(!pipe)
      fresh = FALSE;
    // pipe->nodes (and the pieces it points to) are only safe to walk while
    // holding busy_mutex: a concurrent history change can rebuild the whole
    // list -- dt_dev_pixelpipe_cleanup_nodes() frees every piece -- while this
    // runs on the GUI thread, outside that protection, causing a
    // dangling-pointer segfault in the hash walk below. Must be a trylock,
    // not a blocking lock: a module's process() runs under busy_mutex for
    // the whole pipe run (dt_dev_pixelpipe_process) and can itself call
    // dt_preview_data_store(), which takes this same module's gui_lock
    // (already held above) -- the opposite order -- so a blocking lock here
    // would AB-BA deadlock against it. A pipe that's currently busy can't
    // have fresh data for us anyway, so treating "busy" as "not fresh"
    // costs nothing.
    else if(dt_pthread_mutex_trylock(&pipe->busy_mutex))
      fresh = FALSE;
    else
    {
      dt_dev_pixelpipe_iop_t *piece = NULL;
      for(GList *iter = pipe->nodes; iter; iter = g_list_next(iter))
      {
        dt_dev_pixelpipe_iop_t *const p = (dt_dev_pixelpipe_iop_t *)iter->data;
        if(p->module == module)
        {
          piece = p;
          break;
        }
      }
      if(!piece)
        fresh = FALSE;
      else
      {
        const dt_hash_t cur_hash = dt_dev_pixelpipe_piece_hash(piece, &piece->processed_roi_out, TRUE);
        fresh = (cur_hash == stored_hash);
      }
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
    }
  }

  dt_iop_gui_leave_critical_section((dt_iop_module_t *)module);
  return fresh;
}

dt_hash_t dt_preview_data_get_hash(dt_preview_data_t *pd)
{
  if(!pd || !pd->module) return DT_INVALID_HASH;

  dt_iop_gui_enter_critical_section((dt_iop_module_t *)pd->module);
  const dt_hash_t hash = pd->hash;
  dt_iop_gui_leave_critical_section((dt_iop_module_t *)pd->module);

  return hash;
}

void dt_preview_data_invalidate(dt_preview_data_t *pd)
{
  if(!pd || !pd->module) return;

  dt_iop_gui_enter_critical_section((dt_iop_module_t *)pd->module);
  pd->hash = DT_INVALID_HASH;
  dt_iop_gui_leave_critical_section((dt_iop_module_t *)pd->module);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on