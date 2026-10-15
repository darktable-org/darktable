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

#include <float.h>

/* A parametric (blendif) mask as an element of a group.
 *
 * The form carries its own blendif settings (dt_masks_point_parametric_t), so
 * a mask can hold several, combined with any operator, as shapes are. It
 * renders through the blend colorspace's make_mask function, against the
 * module's input and output images, which dt_develop_blend_process() lends the
 * group renderer through the piece's blend_refine_* context (read back from
 * the device in the OpenCL pipe). */

// the localized label of a parametric form: its channel's name, or a generic
// one when its colorspace has no channel table. The panel's rename takes the
// name's prefix from here rather than parsing form->name, so that the prefix
// stays stable
const char *dt_masks_parametric_type_label(const dt_masks_form_t *const form)
{
  const dt_masks_point_parametric_t *p = form->points ? form->points->data : NULL;
  if(p)
  {
    const dt_iop_gui_blendif_channel_t *channels =
      dt_develop_blendif_channels_for_csp((int)p->colorspace);
    int nch = 0;
    if(channels)
      while(channels[nch].label) nch++;
    if(channels && (int)p->channel < nch) return _(channels[p->channel].label);
  }
  return _("parametric");
}

// TRUE when `sel`'s input and output ranges are both full ({0, 0, 1, 1}), so
// it restricts nothing. The group fold does not count such an element and the
// panel badges it, both from this test. Both ranges count: the output range
// refines the mask even while its slider is hidden. An inverted one at its
// full range selects nothing, which is no no-op, whether the member holding it
// is inverted or a range has its polarity bit set (as a migrated channel can):
// neither is flagged. `p->channel` indexes the colorspace's channel table, not
// blendif_parameters (see _blendif_scale in blend_gui.c)
gboolean dt_masks_parametric_is_noop(const dt_masks_form_t *const sel,
                                     const gboolean inverted)
{
  if(inverted || !sel || !(sel->type & DT_MASKS_PARAMETRIC) || !sel->points) return FALSE;
  const dt_masks_point_parametric_t *const p = sel->points->data;
  const dt_iop_gui_blendif_channel_t *const channels =
    dt_develop_blendif_channels_for_csp((int)p->colorspace);
  if(!channels) return FALSE;
  for(int in_out = 0; in_out < 2; in_out++)
  {
    const int ch = channels[p->channel].param_channels[in_out];
    if(p->blendif & (1u << (ch + 16))) return FALSE;
    const float *const r = &p->blendif_parameters[4 * ch];
    if(r[0] != 0.0f || r[1] != 0.0f || r[2] != 1.0f || r[3] != 1.0f) return FALSE;
  }
  return TRUE;
}

gboolean dt_masks_parametric_sanitize(dt_masks_form_t *const form)
{
  if(!form || !(form->type & DT_MASKS_PARAMETRIC) || !form->points) return FALSE;
  dt_masks_point_parametric_t *const p = form->points->data;
  const dt_iop_gui_blendif_channel_t *const channels =
    dt_develop_blendif_channels_for_csp((int)p->colorspace);
  if(!channels) return FALSE;
  uint32_t nch = 0;
  while(channels[nch].label) nch++;
  if(p->channel < nch) return FALSE;
  p->channel = 0;
  return TRUE;
}

static void _parametric_set_form_name(dt_masks_form_t *const form, const size_t nb)
{
  // the name leads with the form's channel
  snprintf(form->name, sizeof(form->name), "%s #%d", dt_masks_parametric_type_label(form),
           (int)nb);
}

static GSList *_parametric_setup_mouse_actions(const dt_masks_form_t *const form)
{
  // no canvas interaction: it is set up in the panel
  return NULL;
}

/* A parametric form has no geometry, but the group's event and draw
 * dispatchers (group.c) call some of these entries on every member without a
 * NULL check, so they are no-op stubs. get_distance never makes it the closest
 * form, so it is never picked on the canvas. */

static void _parametric_post_expose(cairo_t *const cr,
                                    const float zoom_scale,
                                    dt_masks_form_gui_t *const gui,
                                    const int nb,
                                    const int index)
{
  // nothing to draw
}

static void _parametric_get_distance(const float x,
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

static int _parametric_get_points_border(dt_develop_t *const dev,
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

static void _parametric_duplicate_points(dt_develop_t *const dev,
                                         dt_masks_form_t *const base,
                                         dt_masks_form_t *const dest)
{
  for(GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_point_parametric_t *p = calloc(1, sizeof(dt_masks_point_parametric_t));
    memcpy(p, pts->data, sizeof(dt_masks_point_parametric_t));
    dest->points = g_list_append(dest->points, p);
  }
}

static int _parametric_get_mask_roi(const dt_iop_module_t *const module,
                                    const dt_dev_pixelpipe_iop_t *const piece,
                                    dt_masks_form_t *const form,
                                    const dt_iop_roi_t *const roi,
                                    float *const buffer)
{
  if(!form->points) return 0;
  const dt_masks_point_parametric_t *const p = form->points->data;

  const size_t npix = (size_t)roi->width * roi->height;
  // start fully opaque; make_mask multiplies the channel response into it
  for(size_t k = 0; k < npix; k++) buffer[k] = 1.0f;

  // guide images come from the transient blend context (CPU pipe only)
  const float *const a = piece->blend_refine_guide_in;
  const float *const b = piece->blend_refine_guide_out;
  if(!a || !b)
  {
    dt_print(DT_DEBUG_MASKS,
             "[masks] parametric form %d: no guide image available, mask = 1",
             form->formid);
    return 1;
  }

  // evaluate the form's own blendif settings at full opacity (the group applies
  // the form's opacity), on a copy of the piece's params, so that what the
  // form does not define, blend_cst above all, is the module's. The copy goes
  // to make_mask as an argument: do not point piece->blendop_data at it, the
  // piece is shared. The cast only matches make_mask's non-const piece, which
  // blend.c needs; nothing here changes the piece
  dt_dev_pixelpipe_iop_t *const pc = (dt_dev_pixelpipe_iop_t *)piece;
  const dt_develop_blend_params_t *const saved = piece->blendop_data;
  if(!saved) return 1;

  // a form keeps the channel layout of the colorspace it was made in, but is
  // evaluated in the module's, which the pixels are in. Switching it in the
  // panel removes the parametric forms first (_blendif_change_blend_colorspace
  // in blend_gui.c), but an edit stored with DEVELOP_BLEND_CS_NONE takes its
  // colorspace from the workflow preference when loaded
  // (dt_iop_commit_blend_params). This only logs: rendering such an edit
  // differently would change edits on a path not shown to be reachable
  if(p->colorspace != (uint32_t)saved->blend_cst)
    dt_print(DT_DEBUG_MASKS,
             "[masks] parametric form %d: colorspace mismatch (form %u, module %d)"
             " -- channel bits are being read through the wrong channel table",
             form->formid, p->colorspace, (int)saved->blend_cst);

  dt_develop_blend_params_t tmp = *saved;
  tmp.blendif = p->blendif;
  const dt_iop_gui_blendif_channel_t *channels =
    dt_develop_blendif_channels_for_csp((int)p->colorspace);
  if(channels)
  {
    const dt_iop_gui_blendif_channel_t *ch = &channels[p->channel];
    if(p->disabled & 1) tmp.blendif &= ~(1u << ch->param_channels[0]);
    if(p->disabled & 2) tmp.blendif &= ~(1u << ch->param_channels[1]);
  }
  memcpy(tmp.blendif_parameters, p->blendif_parameters, sizeof(tmp.blendif_parameters));
  memcpy(tmp.blendif_boost_factors, p->blendif_boost_factors,
         sizeof(tmp.blendif_boost_factors));
  tmp.opacity = 100.0f;
  // make_mask renders a uniform mask unless mask_mode has a parametric mask,
  // which a flexi mask_mode has not: without the bit, every form would render
  // opaque
  tmp.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;

  const dt_iop_roi_t *const rin =
    piece->blend_refine_roi_in ? piece->blend_refine_roi_in : roi;
  const dt_iop_roi_t *const rout =
    piece->blend_refine_roi_out ? piece->blend_refine_roi_out : roi;

  dt_print(DT_DEBUG_MASKS, "[masks] parametric form %d: make_mask csp=%d blendif=0x%x",
           form->formid, saved->blend_cst, p->blendif);

  switch(saved->blend_cst)
  {
  case DEVELOP_BLEND_CS_LAB:
    dt_develop_blendif_lab_make_mask(pc, &tmp, a, b, rin, rout, buffer);
    break;
  case DEVELOP_BLEND_CS_RGB_DISPLAY:
    dt_develop_blendif_rgb_hsl_make_mask(pc, &tmp, a, b, rin, rout, buffer);
    break;
  case DEVELOP_BLEND_CS_RGB_SCENE:
    dt_develop_blendif_rgb_jzczhz_make_mask(pc, &tmp, a, b, rin, rout, buffer);
    break;
  case DEVELOP_BLEND_CS_RAW:
    dt_develop_blendif_raw_make_mask(pc, &tmp, a, b, rin, rout, buffer);
    break;
  default: break;
  }

  return 1;
}

// the function table for parametric forms: most geometric and mouse callbacks
// are unused, as the form is evaluated from pixel values alone
const dt_masks_functions_t dt_masks_functions_parametric = {
  .point_struct_size = sizeof(struct dt_masks_point_parametric_t),
  .sanitize_config = NULL,
  .setup_mouse_actions = _parametric_setup_mouse_actions,
  .set_form_name = _parametric_set_form_name,
  .set_hint_message = NULL,
  .modify_property = NULL,
  .duplicate_points = _parametric_duplicate_points,
  .initial_source_pos = NULL,
  .get_distance = _parametric_get_distance,
  .get_points = NULL,
  .get_points_border = _parametric_get_points_border,
  .get_mask = NULL,
  .get_mask_roi = _parametric_get_mask_roi,
  .get_area = NULL,
  .get_source_area = NULL,
  .mouse_moved = NULL,
  .mouse_scrolled = NULL,
  .button_pressed = NULL,
  .button_released = NULL,
  .post_expose = _parametric_post_expose
};

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
