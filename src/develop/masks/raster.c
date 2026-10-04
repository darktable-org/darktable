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
 * not this form's source. */

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

/* A raster form has no geometry, but the group's event and draw dispatchers
 * (group.c) call some of these entries on every member without a NULL check,
 * so they are no-op stubs, as in parametric.c. get_distance never makes it the
 * closest form, so it is never picked on the canvas. */

static void _raster_post_expose(cairo_t *const cr,
                                const float zoom_scale,
                                dt_masks_form_gui_t *const gui,
                                const int nb,
                                const int index)
{
  // nothing to draw
}

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
