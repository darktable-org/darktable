/*
    This file is part of darktable,
    Copyright (C) 2013-2026 darktable developers.

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

#include "common/debug.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/pixelpipe.h"
#include "develop/pixelpipe_hb.h"

#include <float.h>

/* Another module's raster mask as an element of a group.
 *
 * A raster form (dt_masks_point_raster_t) names an earlier module's mask by
 * operation, instance and id, so it combines with shapes and parametric
 * channels under any operator, as every element does.
 *
 * It renders through dt_dev_get_raster_mask(), which returns the source mask
 * distorted to the requesting module's output roi. The group renders on the
 * CPU in the OpenCL pipe too, so it works the same there.
 *
 * _reconcile_raster_form_users() (imageop.c) registers each element's source,
 * so that the source keeps its mask, at commit_params time, which covers a
 * reload with no GUI. A module can hold several raster elements, each naming
 * another source. The classic blend_params.raster_mask_* fields are not used:
 * nothing writes them for a raster form, so module->raster_mask.sink.source is
 * not this form's source.
 *
 * It has no shape to draw. Hovering its row in the panel while the masks are
 * shown on the canvas tints the area its mask covers, read from the preview
 * pipe's copy in the source's piece. */

static void _raster_set_form_name(dt_masks_form_t *const form, const size_t nb)
{
  // the prefix must match _form_type_prefix and _kind_name in blend_gui.c,
  // which strip it from the name the row shows
  snprintf(form->name, sizeof(form->name), "%s #%d", _("raster mask"), (int)nb);
}

static GSList *_raster_setup_mouse_actions(const dt_masks_form_t *const form)
{
  // no canvas interaction: it is set up in the panel
  return NULL;
}

/* The preview frame as a w x h grid of points carried back to the output of
 * `source`, in the pipe's unscaled distortion space, which is where its mask
 * lives. NULL when the pipe's modules cannot be walked or memory is short */
static float *_preview_grid_in_source(dt_develop_t *const dev,
                                      const dt_iop_module_t *const source,
                                      const int w,
                                      const int h)
{
  float wd, ht;
  dt_masks_get_image_size(&wd, &ht, NULL, NULL);
  const size_t n = (size_t)w * h;
  float *points = dt_alloc_align_float(2 * n);
  if(!points) return NULL;

  DT_OMP_FOR(collapse(2))
  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
    {
      const size_t k = (size_t)y * w + x;
      points[k * 2] = (x + 0.5f) * wd / w;
      points[k * 2 + 1] = (y + 0.5f) * ht / h;
    }
  if(!dt_dev_distort_backtransform_plus(dev, dev->preview_pipe, source->iop_order,
                                        DT_DEV_TRANSFORM_DIR_FORW_EXCL, points, n))
  {
    dt_free_align(points);
    return NULL;
  }
  return points;
}

/* The source's mask resampled into out, a w x h grid over the preview frame,
 * 0 outside the mask. FALSE when the preview pipe has no mask of it to show:
 * it is busy, or the source is off or stores none.
 *
 * The pipe frees and replaces its pieces' buffers under busy_mutex, so the
 * pixels are read under it, with a trylock as dt_preview_data_is_fresh() does
 * (preview_data.c): a busy pipe draws nothing this frame rather than make the
 * gui wait. The grid is carried back first, so that the lock covers the read
 * only and not the transform, which waits on history_mutex */
static gboolean _source_mask_on_preview(const dt_masks_form_t *const form,
                                        const int w,
                                        const int h,
                                        float *const out)
{
  dt_develop_t *dev = darktable.develop;
  dt_dev_pixelpipe_t *pipe = dev->preview_pipe;
  dt_iop_module_t *source = dt_masks_raster_source(form);
  if(!source) return FALSE;

  float *points = _preview_grid_in_source(dev, source, w, h);
  if(!points) return FALSE;

  if(dt_pthread_mutex_trylock(&pipe->busy_mutex))
  {
    dt_print(DT_DEBUG_MASKS, "[masks] raster %d: preview pipe busy, no tint", form->formid);
    dt_free_align(points);
    return FALSE;
  }

  gboolean ok = FALSE;
  const dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(dev, pipe, source);
  const dt_masks_point_raster_t *const ref = form->points->data;
  const float *const mask =
    piece && !dt_masks_raster_is_unresolved(source, piece, form)
    ? g_hash_table_lookup(piece->raster_masks, GINT_TO_POINTER(ref->id)) : NULL;
  if(mask && piece->processed_roi_out.width > 0 && piece->processed_roi_out.height > 0)
  {
    // the mask is at the roi's scale, the grid in unscaled space
    const dt_iop_roi_t *const roi = &piece->processed_roi_out;
    DT_OMP_FOR()
    for(size_t k = 0; k < (size_t)w * h; k++)
    {
      const float x = points[k * 2] * roi->scale - roi->x - 0.5f;
      const float y = points[k * 2 + 1] * roi->scale - roi->y - 0.5f;
      // off the mask by more than the pixel its edge interpolates with reads
      // 0, within it the edge pixel; a NaN fails the test
      out[k] = x > -1.0f && x < roi->width && y > -1.0f && y < roi->height
        ? dt_masks_pixel_bilinear(mask, roi->width, roi->height,
                                  CLAMPF(x, 0.0f, roi->width - 1.0f),
                                  CLAMPF(y, 0.0f, roi->height - 1.0f))
        : 0.0f;
    }
    ok = TRUE;
  }
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
  dt_free_align(points);
  return ok;
}

// the tint of a raster element's mask over the preview, or NULL while there is
// none to show. Kept for the preview it was made for, so a row hovered through
// many redraws costs one build
static cairo_surface_t *_raster_tint(const dt_masks_form_t *const form)
{
  const dt_hash_t hash = darktable.develop->preview_pipe->backbuf_hash;
  char key[DT_DTDATA_ENTRY_LEN];
  g_snprintf(key, sizeof(key), "raster-%d", form->formid);
  cairo_surface_t *kept = dt_masks_pixel_hover_get(key, hash);
  if(kept) return kept;

  float wd, ht;
  dt_masks_get_image_size(&wd, &ht, NULL, NULL);
  const int w = MAX(1, (int)ceilf(wd / DT_MASKS_OVERLAY_STEP));
  const int h = MAX(1, (int)ceilf(ht / DT_MASKS_OVERLAY_STEP));
  const double start = dt_get_debug_wtime();
  float *mask = dt_alloc_align_float((size_t)w * h);
  cairo_surface_t *tint =
    mask && _source_mask_on_preview(form, w, h, mask)
    ? dt_masks_pixel_tint(mask, w, h, TRUE, 0.0f) : NULL;
  dt_free_align(mask);
  if(!tint) return NULL;

  dt_print(DT_DEBUG_PERF, "[masks] raster tint %dx%d: %.2f ms",
           w, h, 1000.0 * (dt_get_debug_wtime() - start));
  return dt_masks_pixel_hover_keep(key, hash, tint);
}

// the group calls this for every member, with the row hovered in the panel
// posing as the selected one and form_selected set, the same pose a canvas
// hover has. A raster form is never picked on the canvas, so that is the only
// time it draws
static void _raster_post_expose(cairo_t *const cr,
                                const float zoom_scale,
                                dt_masks_form_gui_t *const gui,
                                const int index,
                                const int num_points)
{
  if(!gui || gui->group_selected != index || !gui->form_selected) return;
  const dt_masks_form_t *form = dt_masks_pixel_form_at(gui, index);
  if(!form || !(form->type & DT_MASKS_RASTER)) return;

  cairo_surface_t *tint = _raster_tint(form);
  if(tint) dt_masks_pixel_paint_overlay(cr, tint);
}

/* A raster form has no geometry, but the group's event and draw dispatchers
 * (group.c) call some of these entries on every member without a NULL check,
 * so the rest are no-op stubs, as in parametric.c. get_distance never makes it
 * the closest form, so it is never picked on the canvas. */

static void _raster_get_distance(const float x,
                                 const float y,
                                 const float as,
                                 dt_masks_form_gui_t *const gui,
                                 const int index,
                                 const int num_points,
                                 gboolean *const inside,
                                 gboolean *const inside_border,
                                 int *const near,
                                 gboolean *const inside_source,
                                 float *const dist)
{
  // never the closest form to the pointer
  if(inside) *inside = FALSE;
  if(inside_border) *inside_border = FALSE;
  if(near) *near = -1;
  if(inside_source) *inside_source = FALSE;
  if(dist) *dist = FLT_MAX;
}

static int _raster_get_points_border(dt_develop_t *const dev,
                                     dt_masks_form_t *const form,
                                     float **const points,
                                     int *const points_count,
                                     float **const border,
                                     int *const border_count,
                                     const int source,
                                     const dt_iop_module_t *const module)
{
  // no geometric outline
  return 0;
}

static void _raster_duplicate_points(dt_develop_t *const dev,
                                     dt_masks_form_t *const base,
                                     dt_masks_form_t *const dest)
{
  for(GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_point_raster_t *p = calloc(1, sizeof(dt_masks_point_raster_t));
    memcpy(p, pts->data, sizeof(dt_masks_point_raster_t));
    dest->points = g_list_append(dest->points, p);
  }
}

// resolve the form's source (op + instance) to a live module in the pipe
static dt_iop_module_t *_raster_find_source(GList *iop_list,
                                            const dt_masks_point_raster_t *const p)
{
  if(!p->source[0]) return NULL;
  for(GList *iter = iop_list; iter; iter = g_list_next(iter))
  {
    dt_iop_module_t *iop = iter->data;
    if(dt_iop_module_is(iop, p->source) && iop->multi_priority == p->instance) return iop;
  }
  return NULL;
}

static dt_iop_module_t *_raster_resolve_source(const dt_iop_module_t *const module,
                                               const dt_masks_point_raster_t *const p)
{
  if(!module || !module->dev) return NULL;
  return _raster_find_source(module->dev->iop, p);
}

dt_iop_module_t *dt_masks_raster_source(const dt_masks_form_t *form)
{
  if(!form || !(form->type & DT_MASKS_RASTER) || !form->points || !darktable.develop)
    return NULL;
  return _raster_find_source(darktable.develop->iop, form->points->data);
}

/* A raster element that cannot obtain a mask renders all zero and reports
 * success. Do not return 0 here: the group would not count the member, and a
 * group of nothing else would take the "no active mask element" fallback,
 * which fills 1 and applies the module everywhere. Classic's raster branch
 * fills 0 when dt_dev_get_raster_mask() returns NULL, so the module does
 * nothing, whether the source was removed before migration or after. */
static int _raster_unresolved(float *const buffer, const dt_iop_roi_t *const roi)
{
  memset(buffer, 0, (size_t)roi->width * roi->height * sizeof(float));
  return 1;
}

gboolean dt_masks_raster_is_unresolved(const dt_iop_module_t *module,
                                       const dt_dev_pixelpipe_iop_t *piece,
                                       const dt_masks_form_t *form)
{
  if(!form || !(form->type & DT_MASKS_RASTER) || !form->points) return FALSE;

  const dt_iop_module_t *source = _raster_resolve_source(module, form->points->data);
  if(!source) return TRUE;

  // whether the source is on. Inside a pipe its piece decides:
  // module->enabled is not maintained in an export pipe
  gboolean enabled = source->enabled;
  const dt_develop_blend_params_t *sbp = source->blend_params;
  if(piece && piece->pipe)
  {
    const dt_dev_pixelpipe_iop_t *source_piece = NULL;
    for(GList *n = piece->pipe->nodes; n; n = g_list_next(n))
    {
      const dt_dev_pixelpipe_iop_t *cand = n->data;
      if(cand->module == source)
      {
        source_piece = cand;
        break;
      }
    }
    // in this pipe the module does not exist at all
    if(!source_piece) return TRUE;
    enabled = source_piece->enabled;
    // the same goes for its blend params: the module's are rewritten with the
    // defaults while another pipe replays history
    sbp = source_piece->blendop_data;
  }
  if(!enabled) return TRUE;

  // and whether it writes a mask: one with no mask of its own and no
  // IOP_FLAGS_WRITE_RASTER never does, the test dt_dev_get_raster_mask()
  // makes (pixelpipe_hb.c)
  const dt_develop_mask_mode_t mask_mode = sbp ? sbp->mask_mode : DEVELOP_MASK_DISABLED;
  const gboolean writes_masks = (mask_mode > DEVELOP_MASK_ENABLED)
                             || (source->flags() & IOP_FLAGS_WRITE_RASTER);
  return !writes_masks;
}

static int _raster_get_mask_roi(const dt_iop_module_t *const module,
                                const dt_dev_pixelpipe_iop_t *const piece,
                                dt_masks_form_t *const form,
                                const dt_iop_roi_t *const roi,
                                float *const buffer)
{
  if(!form->points) return 0;
  const dt_masks_point_raster_t *const p = form->points->data;

  dt_iop_module_t *source = _raster_resolve_source(module, p);
  if(!source)
  {
    dt_print(DT_DEBUG_MASKS, "[masks] raster form %d: source '%s' not found in pipe",
             form->formid, p->source);
    return _raster_unresolved(buffer, roi);
  }

  // the source mask distorted to the piece's output roi, the group's roi here,
  // or NULL if it is not available. With free_mask set it was allocated for
  // us and must be freed
  gboolean free_mask = FALSE;
  float *raster = dt_dev_get_raster_mask((dt_dev_pixelpipe_iop_t *)piece, source, p->id,
                                         module, &free_mask);
  if(!raster)
  {
    dt_print(DT_DEBUG_MASKS, "[masks] raster form %d: no raster mask from '%s' id=%d",
             form->formid, p->source, p->id);
    return _raster_unresolved(buffer, roi);
  }

  const size_t npix = (size_t)roi->width * roi->height;
  // hand the raw 0..1 mask to the group compositor; opacity and the per-element
  // invert (DT_MASKS_STATE_INVERSE) are applied there like any other element
  memcpy(buffer, raster, npix * sizeof(float));

  if(free_mask) dt_free_align(raster);
  return 1;
}

// the function table for raster forms: most geometric and mouse callbacks are
// unused, as the form only refers to another module's mask
const dt_masks_functions_t dt_masks_functions_raster = {
  .point_struct_size = sizeof(struct dt_masks_point_raster_t),
  .sanitize_config = NULL,
  .setup_mouse_actions = _raster_setup_mouse_actions,
  .set_form_name = _raster_set_form_name,
  .set_hint_message = NULL,
  .modify_property = NULL,
  .duplicate_points = _raster_duplicate_points,
  .initial_source_pos = NULL,
  .get_distance = _raster_get_distance,
  .get_points = NULL,
  .get_points_border = _raster_get_points_border,
  .get_mask = NULL,
  .get_mask_roi = _raster_get_mask_roi,
  .get_area = NULL,
  .get_source_area = NULL,
  .mouse_moved = NULL,
  .mouse_scrolled = NULL,
  .button_pressed = NULL,
  .button_released = NULL,
  .post_expose = _raster_post_expose
};

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
