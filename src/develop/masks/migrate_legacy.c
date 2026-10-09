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

/* Conversion of a module's classic mask_mode (DEVELOP_MASK_MASK, _CONDITIONAL,
 * _RASTER and the drawn and parametric combination) into a flexi mask. It is
 * called from dt_develop_blend_legacy_params_ext() (blend.c) after every blend
 * params upgrade, whatever version it started from.
 *
 *  - a drawn mask keeps its group, mask_id and all. The group is converted to
 *    flexi groups (_queue_group_split), since the flexi fold applies one
 *    operator per group where classic applies one per member
 *  - a parametric or raster mask lives in scalar blend params fields, so it
 *    becomes new DT_MASKS_PARAMETRIC or DT_MASKS_RASTER forms
 *  - drawn and parametric becomes a group multiplying the parametric channels
 *    into the existing drawn group
 *  - it is one way: every classic mode becomes flexi, or a uniform blend where
 *    classic renders a constant. It cannot fail
 *
 * New forms go into dev->forms, which style and preset application snapshot,
 * and, on the darkroom-load path (a real history_num), into main.masks_history
 * too: dt_masks_read_masks_history() replaces dev->forms from the database once
 * the history is loaded. Each masks_history row set is a full snapshot that
 * only the current history position is read from, so a new form is written
 * under that position, by dt_masks_finish_flexi_migrations(), not under the row
 * being converted. */

#include "common/darktable.h"
#include "common/debug.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"

// ---------------------------------------------------------------------------
// construction helpers
// ---------------------------------------------------------------------------

/* Repair a group where a member other than the bottom one has no combine
 * operator.
 *
 * In a well-formed classic group only the bottom member has none
 * (dt_masks_group_add_form() gives one to every later member): it seeds the
 * empty accumulator. Classic's fold treats any operator-less member the same
 * way, so one higher up overwrites the accumulator and every member before it
 * is discarded. No UI produces such a group, but stored edits hold some,
 * probably left by the transient groups dt_masks_set_edit_mode_single_form()
 * builds.
 *
 * Conversion reads an operator-less member as a union (dt_masks_eff_group_op),
 * which would bring the discarded members back. So the members before the
 * last operator-less one are dropped: that member is then the bottom one, and
 * both folds render what classic rendered. The forms stay in the list,
 * unreferenced. Hidden and disabled members render nothing, so they take no
 * part on either side.
 *
 * Idempotent: a second pass finds the group well-formed. */
// is `grp` a flexi group, whose list starts with its marker? A classic one
// has none, and only a classic list is repaired here. A flexi group can still
// hold a classic one: migration builds the product group of a drawn and
// parametric mask flexi, around the classic drawn group. So the repairs
// below skip a flexi list's own members but go on into the groups it holds
static gboolean _is_flexi(const dt_masks_form_t *grp)
{
  return grp->points && dt_masks_point_is_marker(grp->points->data);
}

static void _repair_base_case_overwrite(GList *forms,
                                        dt_masks_form_t *grp,
                                        const int depth)
{
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return;
  // a malformed/cyclic tree must not spin here; classic nesting is shallow
  if(depth > DT_MASKS_NESTING_MAX) return;

  // the last live member with no operator: the one whose base case wins
  GList *overwriter = NULL;
  int live = 0;
  for(GList *l = _is_flexi(grp) ? NULL : grp->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(pt->state & (DT_MASKS_STATE_HIDDEN | DT_MASKS_STATE_DISABLE)) continue;
    if(live > 0 && (pt->state & DT_MASKS_STATE_OP_COMBINE) == DT_MASKS_STATE_NONE)
      overwriter = l;
    live++;
  }

  if(overwriter)
  {
    GList *l = grp->points;
    while(l && l != overwriter)
    {
      GList *next = g_list_next(l);
      dt_masks_point_group_t *pt = l->data;
      // hidden and disabled members render nothing either way, so classic
      // discarding them changes nothing: leave them as they are
      if(!(pt->state & (DT_MASKS_STATE_HIDDEN | DT_MASKS_STATE_DISABLE)))
      {
        grp->points = g_list_delete_link(grp->points, l);
        free(pt);
      }
      l = next;
    }
    dt_print(DT_DEBUG_ALWAYS,
             "[masks] group %d: a member with no combine operator sits above"
             " others, which classic renders by discarding them -- dropping"
             " them so the migrated mask keeps rendering the same",
             grp->formid);
  }

  // a member can itself be a group, folded by the same algebra
  for(GList *l = grp->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(dt_masks_point_is_marker(pt)) continue;
    _repair_base_case_overwrite(forms, dt_masks_get_from_id_ext(forms, pt->formid),
                                depth + 1);
  }
}

/* Classic applies every member's operator to the accumulator, the bottom
 * member's included, and the accumulator starts empty. Intersection and
 * difference leave an empty accumulator empty, so a member carrying one below
 * the first member that adds anything contributes nothing at all. The flexi
 * fold copies a group's first member whatever its operator, so migrated as it
 * is that member would suddenly show.
 *
 * Each such member becomes a union at zero opacity: a zero term on an empty
 * accumulator, which is exactly what classic computed. It is not dropped,
 * because a group can consist of nothing else, and classic renders such a
 * group as an empty mask where the flexi fold renders a group with no members
 * as no mask at all. Kept at zero, the group still renders empty, a nested one
 * still combines into its parent as an empty term, and the panel shows the
 * shape at the opacity it really had.
 *
 * Runs after _repair_base_case_overwrite, whose surviving bottom member has no
 * operator and so adds its shape. */
static void _zero_empty_base_members(GList *forms,
                                     dt_masks_form_t *grp,
                                     const int depth)
{
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return;
  if(depth > DT_MASKS_NESTING_MAX) return;

  for(GList *l = _is_flexi(grp) ? NULL : grp->points; l; l = g_list_next(l))
  {
    dt_masks_point_group_t *pt = l->data;
    if(pt->state & (DT_MASKS_STATE_HIDDEN | DT_MASKS_STATE_DISABLE)) continue;
    // a member whose form is gone takes no part in the fold (group.c)
    if(!dt_masks_get_from_id_ext(forms, pt->formid)) continue;
    const int op = pt->state & DT_MASKS_STATE_OP_COMBINE;
    if(op != DT_MASKS_STATE_INTERSECTION && op != DT_MASKS_STATE_DIFFERENCE) break;
    pt->state = (pt->state & ~DT_MASKS_STATE_OP_COMBINE) | DT_MASKS_STATE_UNION;
    pt->opacity = 0.0f;
    dt_print(DT_DEBUG_ALWAYS,
             "[masks] group %d: member %d combines with an empty mask, which"
             " leaves nothing -- keeping it as a zero-opacity union",
             grp->formid, pt->formid);
  }

  for(GList *l = grp->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(dt_masks_point_is_marker(pt)) continue;
    _zero_empty_base_members(forms, dt_masks_get_from_id_ext(forms, pt->formid),
                             depth + 1);
  }
}

/* Does a member composite as max(dest, mask)? A union does, and so does the
 * first visible member whatever its operator, as it seeds the empty
 * accumulator (_zero_empty_base_members has turned those that would not). But
 * only at full opacity, uninverted and unrefined: each of those changes what
 * the member contributes. */
static gboolean _is_union_equivalent(const dt_masks_point_group_t *pt,
                                     const gboolean first)
{
  if(pt->state & DT_MASKS_STATE_INVERSE) return FALSE;
  if(pt->opacity != 1.0f) return FALSE;
  if(pt->refinement.enabled != DT_MASKS_REFINE_OFF) return FALSE;
  return first || (pt->state & DT_MASKS_STATE_UNION);
}

/* Is every live member of `grp` union-equivalent? Then the list renders
 * exactly `max` over its members, in any order, and a member whose value is
 * already in that maximum can go.
 *
 * The whole list has to qualify, not just the member dropped: the first
 * visible member composites as a plain copy whatever its operator, so
 * removing one can promote the next member out of its own operator. In
 * `[a, b:difference]` dropping `a` leaves `b` copied rather than subtracted.
 *
 * A member group is one term of the maximum whatever it holds inside, so this
 * looks at one level only; a group of its own that is not a union list is an
 * opaque term, never descended into by _drop_seen_refs. */
static gboolean _is_union_list(GList *forms, const dt_masks_form_t *grp)
{
  int live = 0;
  for(const GList *l = grp->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(pt->state & (DT_MASKS_STATE_HIDDEN | DT_MASKS_STATE_DISABLE)) continue;
    const dt_masks_form_t *child = dt_masks_get_from_id_ext(forms, pt->formid);
    // a member whose form is gone takes no slot in the composite (group.c)
    if(!child) continue;
    if(child == grp || !_is_union_equivalent(pt, live == 0)) return FALSE;
    live++;
  }
  return TRUE;
}

/* Drop every reference that adds nothing to this region's maximum: a shape
 * already seen, a group form already walked, and a group left empty because
 * everything it held was already there. `seen` and `walked` span the whole
 * region, since the duplicate is usually held both inside a nested group and
 * beside it.
 *
 * A group form is never descended into twice. Both references name one form,
 * so a second descent would find its own leaves already seen and delete them,
 * emptying the form for both references. Dropping the second reference whole
 * is exact for the same reason it would have been empty: everything under it
 * is already in the maximum.
 *
 * A group that is not itself a union list is one opaque term: its shapes are
 * not terms of this maximum, so none of them is seen and none is dropped. A
 * shape held both beside and inside `[a, b:difference]` is no duplicate. */
static void _drop_seen_refs(GList *forms,
                            dt_masks_form_t *grp,
                            GHashTable *seen,
                            GHashTable *walked,
                            const int depth)
{
  if(depth > DT_MASKS_NESTING_MAX) return;
  GList *l = grp->points;
  while(l)
  {
    GList *next = g_list_next(l);
    dt_masks_point_group_t *pt = l->data;
    // a hidden or disabled member renders nothing, so it is nobody's duplicate
    // and keeping it costs the render nothing either
    if(!(pt->state & (DT_MASKS_STATE_HIDDEN | DT_MASKS_STATE_DISABLE)))
    {
      dt_masks_form_t *child = dt_masks_get_from_id_ext(forms, pt->formid);
      gboolean drop = FALSE;
      if(child && child != grp && (child->type & DT_MASKS_GROUP))
      {
        if(!g_hash_table_add(walked, GINT_TO_POINTER(child->formid)))
        {
          // already in the maximum, leaves and all (see above)
          drop = TRUE;
        }
        else if(_is_union_list(forms, child))
        {
          _drop_seen_refs(forms, child, seen, walked, depth + 1);
          // everything it held was already in the maximum: an empty group
          // renders nothing, so the reference to it is noise as well
          drop = child->points == NULL;
        }
      }
      else if(child && !g_hash_table_add(seen, GINT_TO_POINTER(pt->formid)))
        drop = TRUE;

      if(drop)
      {
        grp->points = g_list_delete_link(grp->points, l);
        free(pt);
      }
    }
    l = next;
  }
}

/* Classic lets a mask reach one shape twice, through two groups
 * (dt_masks_group_add_form refuses only cycles). Where everything combining
 * them is a union the repeat renders nothing, max(a, a) = a, so it is dropped,
 * along with a group it leaves empty: the migrated mask then lists each shape
 * once. Anything else a repeat could carry (another operator, opacity,
 * inversion or refinement) gives it a meaning, as in exclusion, so only an
 * all-union list is pruned.
 *
 * A marked list is already flexi, where a shape held twice is a link the user
 * made, each reference with its own row (_masks_row_for_point in
 * blend_gui.c): it is left alone. */
static void _prune_noop_duplicate_refs(GList *forms, dt_masks_form_t *grp)
{
  if(!grp || !(grp->type & DT_MASKS_GROUP) || _is_flexi(grp)) return;
  if(!_is_union_list(forms, grp)) return;

  GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
  // the group forms already descended into: one form can be referenced twice
  GHashTable *walked = g_hash_table_new(g_direct_hash, g_direct_equal);
  g_hash_table_add(walked, GINT_TO_POINTER(grp->formid));
  _drop_seen_refs(forms, grp, seen, walked, 0);
  g_hash_table_destroy(walked);
  g_hash_table_destroy(seen);
}

// every group some module renders as its mask, live or at any position of the
// history. A nested group one of them names is shared, so converting the mask
// that nests it must not move settings onto it (dt_masks_group_mark_classic_runs)
static GHashTable *_module_mask_ids(const dt_develop_t *dev)
{
  GHashTable *ids = g_hash_table_new(NULL, NULL);
  if(!dev) return ids;
  for(const GList *m = dev->iop; m; m = g_list_next(m))
  {
    const dt_develop_blend_params_t *bp = ((const dt_iop_module_t *)m->data)->blend_params;
    if(bp && dt_is_valid_maskid(bp->mask_id))
      g_hash_table_add(ids, GINT_TO_POINTER(bp->mask_id));
  }
  for(const GList *h = dev->history; h; h = g_list_next(h))
  {
    const dt_develop_blend_params_t *bp = ((const dt_dev_history_item_t *)h->data)->blend_params;
    if(bp && dt_is_valid_maskid(bp->mask_id))
      g_hash_table_add(ids, GINT_TO_POINTER(bp->mask_id));
  }
  return ids;
}

static void _mark_classic_runs(const dt_develop_t *dev, GList **forms, dt_masks_form_t *grp)
{
  GHashTable *roots = _module_mask_ids(dev);
  dt_masks_group_mark_classic_runs(forms, grp, roots);
  g_hash_table_destroy(roots);
}

/* Classic applies each member's own operator to the accumulator in turn; a
 * flexi group folds its members in order with one operator. Migration makes a
 * group of each run of members sharing an operator, with what comes before as
 * its first member (dt_masks_group_mark_classic_runs in masks.c), so every
 * member is still applied once, by the same combiner, in the same order.
 * Nested groups need it too: the fold picks flexi or classic by the module's
 * mask_mode, at every depth.
 *
 * The repair runs first, since it decides which members are live, then the
 * duplicate prune, which reads them. A marked list is left as it is, and only
 * the classic groups it holds are repaired: a classic style applied to a
 * flexi edit leaves the edit's own masks alone, and a tree reached twice is
 * converted once. */
static void _normalize_group(const dt_develop_t *dev, GList **forms, dt_masks_form_t *grp)
{
  _repair_base_case_overwrite(*forms, grp, 0);
  _zero_empty_base_members(*forms, grp, 0);
  _prune_noop_duplicate_refs(*forms, grp);
  _mark_classic_runs(dev, forms, grp);
}

/* Classic's whole-mask invert, DEVELOP_COMBINE_MASKS_POS, becomes the mask
 * group's own "invert output" (DT_MASKS_STATE_OP_INVERT on its marker): in
 * flexi the mask is one group, and that group's actions are the only
 * whole-mask controls the panel offers.
 *
 * The two render the same while the group's opacity is 100%, which it always
 * is here: classic has no group opacity and migration starts every group at
 * 1.0 (_new_group_point). MASKS_POS inverts after that opacity, OP_INVERT
 * before it; everything else (the group's refinement before, the module's
 * feathering and details after) sits on the same side of both. The one
 * difference left is a mask where nothing contributes at all (every member
 * hidden): MASKS_POS turns classic's full fallback into an empty mask, while
 * a group with nothing to invert is skipped, OP_INVERT and all.
 *
 * TRUE when `grp` holds a root marker, which is then inverted */
static gboolean _invert_root(dt_masks_form_t *grp)
{
  if(!grp || !(grp->type & DT_MASKS_GROUP) || !grp->points) return FALSE;
  dt_masks_point_group_t *marker = grp->points->data;
  if(!dt_masks_point_is_marker(marker)) return FALSE;
  marker->state ^= DT_MASKS_STATE_OP_INVERT;
  return TRUE;
}

// is group `id` a member of some group in `forms`? Classic lets one module's
// mask hold another's whole group, which flexi cannot express (only shapes are
// linked between masks): inverting such a group's marker would invert it
// inside the other mask as well, so its MASKS_POS stays where it is
static gboolean _group_is_nested(GList *forms, const dt_mask_id_t id)
{
  for(const GList *l = forms; l; l = g_list_next(l))
  {
    const dt_masks_form_t *f = l->data;
    if(!(f->type & DT_MASKS_GROUP)) continue;
    for(const GList *p = f->points; p; p = g_list_next(p))
    {
      const dt_masks_point_group_t *pt = p->data;
      if(!dt_masks_point_is_marker(pt) && pt->formid == id) return TRUE;
    }
  }
  return FALSE;
}

// moves `bp`'s MASKS_POS onto the root marker of the mask it names in
// `forms` (see _invert_root). The bit is cleared only once the marker has
// taken it, so this runs once per params/forms pair however often it is
// called, and a mask whose group is not in `forms` keeps rendering inverted
static void _move_polarity_to_root(GList *forms, dt_develop_blend_params_t *bp)
{
  if(!bp || !(bp->mask_mode & DEVELOP_MASK_FLEXI)) return;
  if(!(bp->mask_combine & DEVELOP_COMBINE_MASKS_POS)) return;
  if(!dt_is_valid_maskid(bp->mask_id) || _group_is_nested(forms, bp->mask_id)) return;
  if(_invert_root(dt_masks_get_from_id_ext(forms, bp->mask_id)))
    bp->mask_combine &= ~(uint32_t)DEVELOP_COMBINE_MASKS_POS;
}

/* Queue a reused classic drawn group for the normalization above, and do it
 * once now.
 *
 * Both halves are needed, for different callers. Doing it now covers the paths
 * that never re-read from the database and instead snapshot dev->forms as it
 * stands (style application, live preset application). Queueing covers the
 * darkroom-load path, where dt_masks_read_masks_history() replaces dev->forms
 * wholesale straight after migration and would discard the in-memory work --
 * see dev->pending_flexi_group_splits.
 *
 * The repair above needs the group's real member list, and on the darkroom-load
 * path there is no point before synthesis where that exists: drawn-only
 * migrates inline while dev->forms still holds the previous image's forms, and
 * drawn+parametric is deferred to dt_masks_finish_flexi_migrations(), which by
 * construction runs *before* dt_masks_read_masks_history(). So the queued pass
 * runs where the forms finally are, in dt_masks_normalize_flexi_groups(). */
static void _queue_group_split(dt_iop_module_t *module, const dt_mask_id_t mask_id)
{
  if(!module->dev || !dt_is_valid_maskid(mask_id)) return;

  _normalize_group(module->dev, &module->dev->forms, dt_masks_get_from_id(module->dev, mask_id));

  const gpointer key = GINT_TO_POINTER(mask_id);
  if(!g_list_find(module->dev->pending_flexi_group_splits, key))
    module->dev->pending_flexi_group_splits =
      g_list_append(module->dev->pending_flexi_group_splits, key);
}

static dt_masks_point_group_t *_new_group_point(const dt_mask_id_t formid,
                                                const int state)
{
  dt_masks_point_group_t *pt = calloc(1, sizeof(dt_masks_point_group_t));
  pt->formid = formid;
  pt->state = state;
  // not the remembered shape opacity: a synthesized member starts opaque, as
  // a parametric or raster element added in the panel does. The member that
  // reuses a drawn group (drawn and parametric) must be 1.0, or it would
  // attenuate the drawn mask where classic did not
  pt->opacity = 1.0f;
  // classic has no group opacity: 1.0 is neutral
  pt->group_opacity = 1.0f;
  return pt;
}

// appends `form` to dev->forms and, when a history position is known, writes
// it into main.masks_history too (see the file header)
static void
_persist_form(dt_iop_module_t *module, dt_masks_form_t *form, const int history_num)
{
  module->dev->forms = g_list_append(module->dev->forms, form);
  if(history_num >= 0)
    dt_masks_write_masks_history_item(module->dev->image_storage.id, history_num, form);
}

// one single-channel DT_MASKS_PARAMETRIC form per channel of `blend_cst` active
// in `blendif`, named as if each channel had been added in the panel. The
// caller applies any DEVELOP_COMBINE_INCL flip to `blendif` first (see
// _channel_polarity_mask), and multiplies the forms together, as classic
// multiplies its channels (`mask *= factor`). An inversion of the whole mask
// goes on the group, not on a channel: invert(a) * b != invert(a * b).
//
// The forms are not added to dev->forms yet
static GList *_build_channel_forms(dt_iop_module_t *module,
                                   const int32_t blend_cst,
                                   const uint32_t blendif,
                                   const float *const blendif_parameters,
                                   const float *const blendif_boost_factors)
{
  const dt_iop_gui_blendif_channel_t *channels =
    dt_develop_blendif_channels_for_csp((int)blend_cst);
  int nch = 0;
  if(channels)
    while(channels[nch].label) nch++;

  int active_ch[DEVELOP_BLENDIF_SIZE];
  gboolean active_out[DEVELOP_BLENDIF_SIZE];
  int n_active = 0;
  for(int ch = 0; ch < nch && n_active < DEVELOP_BLENDIF_SIZE; ch++)
  {
    // param_channels[] holds slot indices, not bits: a slot is active when
    // blendif has 1 << slot set
    const gboolean in_active = (blendif & (1u << channels[ch].param_channels[0])) != 0;
    const gboolean out_active = (blendif & (1u << channels[ch].param_channels[1])) != 0;
    if(in_active || out_active)
    {
      active_ch[n_active] = ch;
      active_out[n_active] = out_active;
      n_active++;
    }
  }

  GList *out = NULL;
  for(int i = 0; i < n_active; i++)
  {
    dt_masks_form_t *form = dt_masks_create(DT_MASKS_PARAMETRIC);
    dt_masks_point_parametric_t *p = calloc(1, sizeof(dt_masks_point_parametric_t));
    // keep only this one channel's own active + polarity bits
    const uint32_t ch_mask = (1u << channels[active_ch[i]].param_channels[0])
                             | (1u << channels[active_ch[i]].param_channels[1]);
    p->blendif = blendif & (ch_mask | (ch_mask << 16));
    memcpy(p->blendif_parameters, blendif_parameters,
           4 * DEVELOP_BLENDIF_SIZE * sizeof(float));
    memcpy(p->blendif_boost_factors, blendif_boost_factors,
           DEVELOP_BLENDIF_SIZE * sizeof(float));
    p->colorspace = (uint32_t)blend_cst;
    p->channel = (uint32_t)active_ch[i];
    p->in_out = active_out[i] ? 1u : 0u;
    form->points = g_list_append(form->points, p);
    dt_masks_assign_unique_name(module->dev, form);
    out = g_list_append(out, form);
  }
  return out;
}

// appends one plain element per form in `forms` to `*points`, for a group
// that folds them by product (_new_product_group): classic multiplies its
// channels together (`mask *= factor` per channel, see _build_channel_forms),
// and into the drawn mask when there is one
static void _append_channel_points(GList *forms, const dt_mask_id_t parentid, GList **points)
{
  GList *added = NULL;
  for(GList *l = forms; l; l = g_list_next(l))
  {
    const dt_masks_form_t *form = l->data;
    dt_masks_point_group_t *pt = _new_group_point(
      form->formid, DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION);
    pt->parentid = parentid;
    added = g_list_append(added, pt);
  }
  *points = g_list_concat(*points, added);
}

// a new group folding its members by product, holding nothing but its marker
static dt_masks_form_t *_new_product_group(GList *forms)
{
  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
  grp->points = g_list_append(NULL,
                              dt_masks_marker_new(forms, grp, DT_MASKS_STATE_FLEXI_PRODUCT));
  return grp;
}

// once the blendif settings are copied into parametric forms, the module's own
// copy must go: dt_develop_blend_process() (blend.c) still runs make_mask()
// with the module's params after the drawn group, and with no
// DEVELOP_MASK_CONDITIONAL its fallback still applies DEVELOP_COMBINE_INV,
// which would invert the mask a second time
static void _clear_toplevel_blendif(dt_develop_blend_params_t *n)
{
  n->mask_combine &= ~(uint32_t)(DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL);
  n->blendif = 0;
  memset(n->blendif_parameters, 0, sizeof(n->blendif_parameters));
  memset(n->blendif_boost_factors, 0, sizeof(n->blendif_boost_factors));
}

// DEVELOP_COMBINE_INCL is not an invert of the result: the make_mask()
// functions that read it (Lab, RGB HSL and JzCzhz; raw ignores it) flip every
// channel's polarity bit with this colorspace mask before selecting. A
// parametric form reproduces it by flipping its copied blendif the same way
static uint32_t _channel_polarity_mask(const int32_t blend_cst)
{
  switch(blend_cst)
  {
  case DEVELOP_BLEND_CS_LAB: return DEVELOP_BLENDIF_Lab_MASK;
  case DEVELOP_BLEND_CS_RGB_DISPLAY:
  case DEVELOP_BLEND_CS_RGB_SCENE: return DEVELOP_BLENDIF_RGB_MASK;
  default: return 0; // raw and anything else: INCL flips no channel there
  }
}

// what make_mask() does with a classic parametric configuration (raw reads
// neither INCL nor canceling channels, so it is always PASSTHROUGH):
//
//  - DT_COND_REAL: a channel is active, and INCL made no inactive channel a
//    canceling one. The per-channel selection runs
//
//  - DT_COND_PASSTHROUGH: no channel is active and none cancels. The incoming
//    mask is multiplied by the opacity, inverted by INV alone, so a drawn mask
//    passes through: "drawn & parametric" with no channel set is the drawn
//    mask alone. It is a constant only when the incoming mask is, and then set
//    by what produced it (INCL for parametric alone, MASKS_POS for drawn and
//    parametric with no shapes), not by INCL and INV as below
//
//  - DT_COND_CONSTANT: INCL made an inactive channel a canceling one.
//    dt_iop_image_fill() replaces the whole buffer, drawn shapes included,
//    with the opacity when INV != INCL and with 0 otherwise
typedef enum
{
  DT_COND_REAL,
  DT_COND_PASSTHROUGH,
  DT_COND_CONSTANT,
} dt_cond_branch_t;

static dt_cond_branch_t _classify_conditional(const int32_t blend_cst,
                                              const uint32_t blendif,
                                              const gboolean incl)
{
  const uint32_t mask = _channel_polarity_mask(blend_cst);
  if(!mask) return DT_COND_PASSTHROUGH; // raw: no channel can cancel

  const uint32_t any_channel_active = blendif & mask;
  const uint32_t flipped = blendif ^ (incl ? (mask << 16) : 0);
  const uint32_t canceling_channel = (flipped >> 16) & ~flipped & mask;

  if(canceling_channel) return DT_COND_CONSTANT;
  if(any_channel_active) return DT_COND_REAL;
  return DT_COND_PASSTHROUGH;
}

// what a classic drawn mask renders, from its form tree alone
typedef enum
{
  DRAWN_MISSING, // mask_id resolves to nothing: blend.c fills 1.0 (0.0 inverted)
  DRAWN_EMPTY,   // a group that renders nothing: an empty mask, 0.0 (1.0 inverted)
  DRAWN_CONTENT, // at least one shape
} _drawn_content_t;

// the member ids of form `id` into `members` when it is a group. FALSE when it
// does not resolve.
//
// While history loads (a real history_num), dev->forms still holds the
// previous image's forms: dt_masks_read_masks_history() only runs once every
// row is converted. So the database is asked instead. Style and preset
// application run with this image's forms loaded
static gboolean _form_members(dt_iop_module_t *module,
                              const dt_mask_id_t id,
                              const int history_num,
                              gboolean *is_group,
                              GArray *members)
{
  if(history_num < 0)
  {
    const dt_masks_form_t *form = dt_masks_get_from_id(module->dev, id);
    if(!form) return FALSE;
    *is_group = (form->type & DT_MASKS_GROUP) != 0;
    if(*is_group)
      for(const GList *l = form->points; l; l = g_list_next(l))
        g_array_append_val(members, ((const dt_masks_point_group_t *)l->data)->formid);
    return TRUE;
  }

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT form, version, points, points_count"
                              " FROM main.masks_history"
                              " WHERE imgid = ?1 AND formid = ?2"
                              " ORDER BY num DESC LIMIT 1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, module->dev->image_storage.id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, id);
  const gboolean found = sqlite3_step(stmt) == SQLITE_ROW;
  if(found)
  {
    *is_group = (sqlite3_column_int(stmt, 0) & DT_MASKS_GROUP) != 0;
    if(*is_group)
    {
      const size_t stride = dt_masks_point_stride(DT_MASKS_GROUP, sqlite3_column_int(stmt, 1),
                                                  sizeof(dt_masks_point_group_t));
      const char *blob = sqlite3_column_blob(stmt, 2);
      const size_t bytes = sqlite3_column_bytes(stmt, 2);
      const int count = sqlite3_column_int(stmt, 3);
      for(int i = 0; blob && i < count && (i + 1) * stride <= bytes; i++)
      {
        dt_mask_id_t member;
        memcpy(&member, blob + i * stride + offsetof(dt_masks_point_group_t, formid),
               sizeof(member));
        g_array_append_val(members, member);
      }
    }
  }
  sqlite3_finalize(stmt);
  return found;
}

// does form `id` render nothing at all: gone, or a group of such members?
static gboolean _renders_nothing(dt_iop_module_t *module,
                                 const dt_mask_id_t id,
                                 const int history_num,
                                 const int depth)
{
  // a cyclic tree: say it renders, which leaves the mask as it is
  if(depth > DT_MASKS_NESTING_MAX) return FALSE;
  GArray *members = g_array_new(FALSE, FALSE, sizeof(dt_mask_id_t));
  gboolean is_group = FALSE;
  gboolean nothing = TRUE;
  if(_form_members(module, id, history_num, &is_group, members))
  {
    nothing = is_group;
    for(guint i = 0; nothing && i < members->len; i++)
      nothing = _renders_nothing(module, g_array_index(members, dt_mask_id_t, i),
                                 history_num, depth + 1);
  }
  g_array_free(members, TRUE);
  return nothing;
}

static _drawn_content_t _drawn_content(dt_iop_module_t *module,
                                       const dt_mask_id_t mask_id,
                                       const int history_num)
{
  if(!dt_is_valid_maskid(mask_id)) return DRAWN_MISSING;
  GArray *members = g_array_new(FALSE, FALSE, sizeof(dt_mask_id_t));
  gboolean is_group = FALSE;
  const gboolean found = _form_members(module, mask_id, history_num, &is_group, members);
  g_array_free(members, TRUE);
  if(!found) return DRAWN_MISSING;
  return _renders_nothing(module, mask_id, history_num, 0) ? DRAWN_EMPTY : DRAWN_CONTENT;
}

// ---------------------------------------------------------------------------
// per-case synthesis
// ---------------------------------------------------------------------------

// DEVELOP_MASK_CONDITIONAL (pure parametric, no drawn shapes): one group
// folding by product one DT_MASKS_PARAMETRIC element per active channel, as
// classic multiplies its channels
static void _migrate_parametric_only(dt_iop_module_t *module,
                                     const dt_develop_blend_params_t *const o,
                                     dt_develop_blend_params_t *n,
                                     const int history_num)
{
  const gboolean incl = (o->mask_combine & DEVELOP_COMBINE_INCL) != 0;
  const gboolean inv = (o->mask_combine & DEVELOP_COMBINE_INV) != 0;
  const dt_cond_branch_t branch = _classify_conditional(o->blend_cst, o->blendif, incl);

  // with no drawn mask, both other branches render a constant: a uniform blend,
  // at zero opacity for an empty mask. Their parities differ: CONSTANT fills
  // the opacity when INV != INCL, while PASSTHROUGH multiplies the fallback
  // dt_develop_blend_process() feeds in (0 with INCL, 1 without), inverted by
  // INV
  gboolean opaque;
  if(branch == DT_COND_CONSTANT)
    opaque = (incl != inv);
  else if(branch == DT_COND_PASSTHROUGH)
    opaque = (incl == inv);
  if(branch != DT_COND_REAL)
  {
    _clear_toplevel_blendif(n);
    n->mask_mode = DEVELOP_MASK_ENABLED;
    n->mask_id = NO_MASKID;
    n->mask_combine &= ~(uint32_t)DEVELOP_COMBINE_MASKS_POS;
    if(!opaque) n->opacity = 0.0f;
    return;
  }

  dt_masks_form_t *grp = _new_product_group(module->dev->forms);

  // INCL flips every channel's polarity (_channel_polarity_mask). With INCL
  // set, only a configuration with every channel active gets here, so no
  // flipped channel is inactive
  const uint32_t flipped_blendif =
    o->blendif ^ (incl ? (_channel_polarity_mask(o->blend_cst) << 16) : 0);

  GList *param_forms =
    _build_channel_forms(module, o->blend_cst, flipped_blendif, o->blendif_parameters,
                         o->blendif_boost_factors);
  _append_channel_points(param_forms, grp->formid, &grp->points);

  for(GList *l = param_forms; l; l = g_list_next(l))
    _persist_form(module, l->data, history_num);
  g_list_free(param_forms);
  // INV inverts the product of the channels, so it goes on the group as its
  // invert output, before the group is persisted: this path never reaches
  // _move_polarity_to_root. INCL inverts it as well, so the two cancel out
  if(incl != inv) _invert_root(grp);
  _persist_form(module, grp, history_num);

  _clear_toplevel_blendif(n);
  n->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI;
  n->mask_id = grp->formid;
  n->mask_combine &= ~(uint32_t)DEVELOP_COMBINE_MASKS_POS;
}

// DEVELOP_MASK_RASTER: one group holding one DT_MASKS_RASTER element,
// referencing the same source the classic raster_mask_* fields already
// describe. Those fields are left untouched and unused: the element carries
// its own source, which _reconcile_raster_form_users() (imageop.c) registers
// and masks/raster.c renders from, while dt_iop_commit_blend_params() only
// reads the fields in classic raster mode, which a flexi mask never is.
static void _migrate_raster(dt_iop_module_t *module,
                            const dt_develop_blend_params_t *const o,
                            dt_develop_blend_params_t *n,
                            const int history_num)
{
  dt_masks_form_t *raster_form = dt_masks_create(DT_MASKS_RASTER);
  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
  dt_masks_point_raster_t *rp = calloc(1, sizeof(dt_masks_point_raster_t));
  dt_strlcpy_fixed_to_fixed(rp->source, sizeof(rp->source),
                            o->raster_mask_source, sizeof(o->raster_mask_source));
  rp->instance = o->raster_mask_instance;
  rp->id = o->raster_mask_id;
  raster_form->points = g_list_append(raster_form->points, rp);

  // classic's raster_mask_invert (1 - raster) and a member's
  // DT_MASKS_STATE_INVERSE (dt_masks_combine_maximum in group.c) are the same
  // formula. But with no source, classic fills 0 without reading the flag,
  // while an inverted unresolved element would render 1 everywhere: the flag
  // only carries over when there is a source
  const gboolean resolvable = o->raster_mask_source[0] != '\0';

  int state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
  if(o->raster_mask_invert && resolvable) state |= DT_MASKS_STATE_INVERSE;

  if(o->raster_mask_invert && !resolvable)
    dt_print(DT_DEBUG_MASKS,
             "[masks] module '%s': raster mask has no source and is inverted --"
             " dropping the inversion, which classic never applies to the"
             " no-source fallback either",
             module->op);

  dt_masks_point_group_t *pt = _new_group_point(raster_form->formid, state);
  pt->parentid = grp->formid;
  grp->points = g_list_append(grp->points, pt);
  _mark_classic_runs(module->dev, &module->dev->forms, grp);

  _persist_form(module, raster_form, history_num);
  _persist_form(module, grp, history_num);

  n->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI;
  n->mask_id = grp->formid;

  /* classic's raster branch in dt_develop_blend_process() reads none of
     mask_combine: the mask is raster * opacity. The flexi group goes through
     the drawn branch, which applies MASKS_POS, INV and INCL, so all three must
     go. Stored edits do pair raster with MASKS_POS */
  n->mask_combine &= ~(uint32_t)(DEVELOP_COMBINE_INV
                                 | DEVELOP_COMBINE_INCL
                                 | DEVELOP_COMBINE_MASKS_POS);
}

// DEVELOP_MASK_MASK_CONDITIONAL: drawn and parametric, multiplied together by
// classic. The parametric channels multiply into the existing drawn group
static void _migrate_drawn_and_parametric(dt_iop_module_t *module,
                                          const dt_develop_blend_params_t *const o,
                                          dt_develop_blend_params_t *n,
                                          const int history_num)
{
  // with no drawn shapes, classic fills 1 (0 with MASKS_POS) and multiplies
  // the parametric mask into that, in the role the INCL fallback plays for
  // parametric alone:
  //
  //  - MASKS_POS == INCL: this is parametric alone
  //  - MASKS_POS != INCL: a constant whatever the channels, opaque when
  //    INCL != INV: a uniform blend, at zero opacity for an empty mask
  const _drawn_content_t drawn = _drawn_content(module, o->mask_id, history_num);
  if(drawn != DRAWN_CONTENT)
  {
    // an empty group renders the opposite of a missing form, so it reads as
    // one with the invert flipped (see _drawn_content_t)
    const gboolean masks_pos = ((o->mask_combine & DEVELOP_COMBINE_MASKS_POS) != 0)
                               != (drawn == DRAWN_EMPTY);
    const gboolean incl = (o->mask_combine & DEVELOP_COMBINE_INCL) != 0;
    if(masks_pos == incl)
    {
      _migrate_parametric_only(module, o, n, history_num);
      return;
    }

    const gboolean inv = (o->mask_combine & DEVELOP_COMBINE_INV) != 0;
    const gboolean opaque = (incl != inv);
    _clear_toplevel_blendif(n);
    n->mask_mode = DEVELOP_MASK_ENABLED;
    n->mask_id = NO_MASKID;
    n->mask_combine &= ~(uint32_t)DEVELOP_COMBINE_MASKS_POS;
    if(!opaque) n->opacity = 0.0f;
    return;
  }

  {
    const gboolean incl = (o->mask_combine & DEVELOP_COMBINE_INCL) != 0;
    const gboolean inv = (o->mask_combine & DEVELOP_COMBINE_INV) != 0;
    const dt_cond_branch_t branch = _classify_conditional(o->blend_cst, o->blendif, incl);

    if(branch == DT_COND_CONSTANT)
    {
      // dt_iop_image_fill() replaces the whole buffer, drawn shapes included,
      // so the shapes change nothing
      const gboolean opaque = (incl != inv);
      _clear_toplevel_blendif(n);
      n->mask_mode = DEVELOP_MASK_ENABLED;
      n->mask_id = NO_MASKID;
      n->mask_combine &= ~(uint32_t)DEVELOP_COMBINE_MASKS_POS;
      if(!opaque) n->opacity = 0.0f;
      return;
    }

    if(branch == DT_COND_PASSTHROUGH)
    {
      // the drawn mask d passes through, inverted by MASKS_POS and then by
      // INV: one inversion by MASKS_POS != INV. So this is a drawn mask alone,
      // migrated as _dispatch() migrates one
      const gboolean masks_pos = (o->mask_combine & DEVELOP_COMBINE_MASKS_POS) != 0;
      _clear_toplevel_blendif(n);
      n->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI;
      _queue_group_split(module, o->mask_id);
      if(masks_pos != inv)
        n->mask_combine |= DEVELOP_COMBINE_MASKS_POS;
      else
        n->mask_combine &= ~(uint32_t)DEVELOP_COMBINE_MASKS_POS;
      return;
    }

    // DT_COND_REAL. With INCL set, which takes every channel active, classic
    // renders 1 - (1 - d) * p without INV and (1 - d) * p with it: the
    // construction below with both of its inversions flipped by INCL, and
    // the channels' polarity flipped as for parametric alone
  }

  dt_masks_form_t *top_grp = _new_product_group(module->dev->forms);

  const gboolean incl = (o->mask_combine & DEVELOP_COMBINE_INCL) != 0;
  const uint32_t flipped_blendif =
    o->blendif ^ (incl ? (_channel_polarity_mask(o->blend_cst) << 16) : 0);

  GList *param_forms =
    _build_channel_forms(module, o->blend_cst, flipped_blendif, o->blendif_parameters,
                         o->blendif_boost_factors);

  // classic inverts twice here, and invert(d) * p != invert(d * p):
  //
  //  - MASKS_POS inverts the drawn mask before the parametric one multiplies
  //    into it. It goes on the member holding the drawn group, which the fold
  //    inverts before the multiply
  //  - INV inverts the product, inside make_mask(). It becomes the module's
  //    MASKS_POS, which dt_develop_blend_process() applies to the whole
  //    rendered group
  //
  // INCL flips both (see above)
  const gboolean invert_drawn =
    ((o->mask_combine & DEVELOP_COMBINE_MASKS_POS) != 0) ^ incl;
  const gboolean invert_composite = ((o->mask_combine & DEVELOP_COMBINE_INV) != 0) ^ incl;
  int drawn_state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION;
  if(invert_drawn) drawn_state |= DT_MASKS_STATE_INVERSE;

  dt_masks_point_group_t *drawn_pt = _new_group_point(o->mask_id, drawn_state);
  drawn_pt->parentid = top_grp->formid;
  top_grp->points = g_list_append(top_grp->points, drawn_pt);

  // the channels multiply into the drawn mask, the group's first member
  _append_channel_points(param_forms, top_grp->formid, &top_grp->points);

  for(GList *l = param_forms; l; l = g_list_next(l))
    _persist_form(module, l->data, history_num);
  g_list_free(param_forms);
  _persist_form(module, top_grp, history_num);

  // the drawn group is folded by the flexi fold too, so it needs converting as
  // for a drawn mask alone. Converting from the top group converts it, and
  // merges it into the top group wherever that renders the same
  _queue_group_split(module, top_grp->formid);

  _clear_toplevel_blendif(n);
  n->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI;
  n->mask_id = top_grp->formid;
  // the original MASKS_POS is on drawn_pt now; the bit now means INV
  n->mask_combine &= ~(uint32_t)DEVELOP_COMBINE_MASKS_POS;
  if(invert_composite) n->mask_combine |= DEVELOP_COMBINE_MASKS_POS;
}

// picks the case in dt_develop_blend_process()'s order: raster wins over any
// drawn or parametric bit also set, as it does there. Runs at once without a
// history position, and from dt_masks_finish_flexi_migrations() with one
static void _dispatch(dt_iop_module_t *module,
                      const dt_develop_blend_params_t *const o,
                      dt_develop_blend_params_t *n,
                      const int history_num)
{
  if(o->mask_mode & DEVELOP_MASK_RASTER)
  {
    if((o->mask_mode & (DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL)))
      dt_print(
        DT_DEBUG_ALWAYS,
        "[masks] module '%s': non-standard mask_mode 0x%x (RASTER combined with "
        "MASK/CONDITIONAL) -- migrating as pure raster, matching how it already renders",
        module->op, o->mask_mode);
    _migrate_raster(module, o, n, history_num);
  }
  else if((o->mask_mode & DEVELOP_MASK_MASK) && (o->mask_mode & DEVELOP_MASK_CONDITIONAL))
  {
    _migrate_drawn_and_parametric(module, o, n, history_num);
  }
  else if(o->mask_mode & DEVELOP_MASK_MASK)
  {
    // drawn only: the group is kept, mask_id and all, and converted to flexi
    // groups (_queue_group_split), since the flexi fold applies one operator
    // per group where classic applies one per member
    if(_drawn_content(module, o->mask_id, history_num) == DRAWN_EMPTY)
    {
      // nothing to draw, which classic renders as an empty mask and flexi
      // would render as no mask at all: a uniform blend at zero opacity is
      // the same empty mask (at full opacity when inverted), as for an
      // always-empty parametric mask (_migrate_parametric_only)
      n->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI;
      n->mask_id = NO_MASKID;
      if(!(o->mask_combine & DEVELOP_COMBINE_MASKS_POS)) n->opacity = 0.0f;
      n->mask_combine &= ~(uint32_t)DEVELOP_COMBINE_MASKS_POS;
    }
    else
    {
      n->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI;
      _queue_group_split(module, o->mask_id);
    }
  }
  else // DEVELOP_MASK_CONDITIONAL alone
  {
    _migrate_parametric_only(module, o, n, history_num);
  }

  // the GUI always sets ENABLED with a mode bit, but foreign data may not. The
  // renderer treats a mode bit alone as ENABLED with it, so this changes
  // nothing it renders
  n->mask_mode |= DEVELOP_MASK_ENABLED;

  // the whole-mask invert onto the mask group, now that the group has its
  // marker. Only where nothing re-reads the forms afterwards: on the
  // darkroom-load path dt_masks_read_masks_history() replaces them, so
  // dt_masks_normalize_flexi_groups() does it there, per history item
  if(history_num < 0) _move_polarity_to_root(module->dev->forms, n);
}

// a migration that creates forms on the darkroom-load path, deferred (see the
// file header and dev->pending_flexi_migrations in develop.h)
typedef struct _pending_flexi_migration_t
{
  dt_iop_module_t *module;
  dt_develop_blend_params_t classic; // bp's original, pre-migration snapshot
  dt_develop_blend_params_t *bp;     // the live params to update once resolved
} _pending_flexi_migration_t;

void dt_masks_migrate_classic_to_flexi(dt_iop_module_t *module,
                                       dt_develop_blend_params_t *bp,
                                       const int history_num)
{
  if(!module) return;

  // no mask mode: disabled, or a uniform blend, which renders as a flexi mask
  // with no group does. The panel relies on every mask_mode being DISABLED or
  // flexi
  if(!(bp->mask_mode
       & (DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL | DEVELOP_MASK_RASTER)))
  {
    if(bp->mask_mode & DEVELOP_MASK_ENABLED)
    {
      bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI;
      bp->mask_id = NO_MASKID;
    }
    return;
  }

  // no image to create forms for: dt_develop_blend_legacy_params_from_so(),
  // converting a built-in preset at module registration. Such presets hold no
  // shapes, so they stay classic
  if(!module->dev) return;

  const dt_develop_blend_params_t o = *bp;

  // a drawn mask alone creates no form (see _dispatch()), so it can migrate
  // at once: the group it keeps is already in every later snapshot
  const gboolean needs_new_form =
    (o.mask_mode & (DEVELOP_MASK_CONDITIONAL | DEVELOP_MASK_RASTER)) != 0;

  if(needs_new_form && history_num >= 0)
  {
    // deferred to dt_masks_finish_flexi_migrations(), which knows the final
    // history_end and runs before dt_masks_read_masks_history() reads it
    _pending_flexi_migration_t *pending = calloc(1, sizeof(_pending_flexi_migration_t));
    pending->module = module;
    pending->classic = o;
    pending->bp = bp;
    module->dev->pending_flexi_migrations =
      g_list_append(module->dev->pending_flexi_migrations, pending);

    // mask_id keeps its classic value until then: nothing reads bp before,
    // within the same dt_dev_read_history_ext() call
    bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI;
    return;
  }

  _dispatch(module, &o, bp, history_num);
}

// the masks_history num dt_masks_read_masks_history() reads as current: the
// highest one below history_end, which is earlier than history_end - 1 when
// the last steps changed no mask. A form written under a num with no other
// rows would make that num the current snapshot, holding nothing else, and
// every other mask would stop resolving
static int _current_masks_history_num(const dt_develop_t *dev)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT MAX(num) FROM main.masks_history"
                              " WHERE imgid = ?1 AND num < ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->image_storage.id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, dev->history_end);
  int num = dev->history_end - 1; // no masks data yet
  if(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
    num = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return num;
}

void dt_masks_finish_flexi_migrations(dt_develop_t *dev)
{
  if(!dev->pending_flexi_migrations) return;

  const int history_num = _current_masks_history_num(dev);

  for(GList *l = dev->pending_flexi_migrations; l; l = g_list_next(l))
  {
    _pending_flexi_migration_t *pending = l->data;

    _dispatch(pending->module, &pending->classic, pending->bp, history_num);
    free(pending);
  }

  g_list_free(dev->pending_flexi_migrations);
  dev->pending_flexi_migrations = NULL;
}

/* Copy the normalized forms onto the history item that owns the current
   snapshot, so the write at the end of dt_dev_read_history_ext() persists them.

   dt_masks_read_masks_history() attaches each stored form to its history item
   and then hands dev->forms a *deep copy* of the newest such set
   (dt_masks_replace_current_forms). Everything above operates on that copy, so
   without this step the markers exist only in memory while the writer, which
   walks each item's own list, stores the group exactly as it found it.

   Only the item the snapshot came from is touched; the older snapshots are
   normalized on their own (_normalize_history_item). */
static void _sync_forms_to_history(dt_develop_t *dev)
{
  // the newest item at or below the current end that carries a snapshot: the
  // same one dt_masks_read_masks_history() copied dev->forms from
  dt_dev_history_item_t *owner = NULL;
  for(GList *l = dev->history; l; l = g_list_next(l))
  {
    dt_dev_history_item_t *h = l->data;
    if(h->forms && h->num < dev->history_end
       && (!owner || h->num >= owner->num))
      owner = h;
  }
  // no item claims the snapshot: nothing to persist it under, and fabricating
  // an owner would put the masks on the wrong history position
  if(!owner) return;

  g_list_free_full(owner->forms, (void (*)(void *))dt_masks_free_form);
  owner->forms = dt_masks_dup_forms_deep(dev->forms, NULL);
}

/* The params a module renders with at history position `limit`: its last
   item below it. NULL when it has none there, and it then renders its defaults */
static dt_develop_blend_params_t *_params_at(GList *history,
                                             const dt_iop_module_t *module,
                                             const int limit)
{
  dt_develop_blend_params_t *bp = NULL;
  int k = 0;
  for(GList *l = history; l && k < limit; l = g_list_next(l), k++)
  {
    dt_dev_history_item_t *h = l->data;
    if(h->module == module && h->blend_params) bp = h->blend_params;
  }
  return bp;
}

// can the whole-mask invert of `bp`, rendered with `forms` at position
// `limit`, move onto its mask group? Not when another module renders the same
// group at that position, nor when some mask nests it (_group_is_nested):
// either way its marker is not this module's alone
static gboolean _polarity_movable(dt_develop_t *dev,
                                  GList *forms,
                                  const dt_develop_blend_params_t *bp,
                                  const int limit)
{
  if(!bp || !(bp->mask_mode & DEVELOP_MASK_FLEXI)) return FALSE;
  if(!dt_is_valid_maskid(bp->mask_id)) return FALSE;
  if(!dt_masks_get_from_id_ext(forms, bp->mask_id)) return FALSE;
  if(_group_is_nested(forms, bp->mask_id)) return FALSE;
  int users = 0;
  for(GList *m = dev->iop; m; m = g_list_next(m))
  {
    const dt_develop_blend_params_t *other = _params_at(dev->history, m->data, limit);
    if(other && (other->mask_mode & DEVELOP_MASK_FLEXI) && other->mask_id == bp->mask_id)
      users++;
  }
  return users == 1;
}

// inverts the mask group `bp` names in `forms`, if `bp` carries MASKS_POS and
// it can move there. `bp` is left alone: _move_history_polarity clears it once
// every tree rendered with it has been inverted
static void _invert_root_for(dt_develop_t *dev,
                             GList *forms,
                             const dt_develop_blend_params_t *bp,
                             const int limit)
{
  if(!bp || !(bp->mask_combine & DEVELOP_COMBINE_MASKS_POS)) return;
  if(_polarity_movable(dev, forms, bp, limit))
    _invert_root(dt_masks_get_from_id_ext(forms, bp->mask_id));
}

/* Move the whole-mask invert of every stored state onto its mask group (see
   _invert_root), on the darkroom-load path.

   A forms snapshot and the params it renders with are not stored together:
   each history item holds its module's params, and only some items hold a
   snapshot, which then serves every position up to the next one. Moved item
   by item, the invert of a module whose last item holds no snapshot would be
   lost. So each tree is inverted by the params that render with it:

   - the live tree, by each module's params at history_end. Export renders it
     straight after this load without popping the history (dt_dev_load_image
     in dt_imageio_export_with_flags), with the pipe taking each module's
     params from its items, so this is the state that has to be exact
   - every stored snapshot, by the params at the end of the span it serves
     (at history_end for the current one), which is where undoing onto it
     lands

   and MASKS_POS then goes from every item rendered with an inverted tree. A
   module that toggled its invert between two snapshots, with no snapshot in
   between, renders one side of that toggle inverted wrongly, as undoing to
   such a position always could. A mask group that is not the module's alone
   keeps MASKS_POS instead (see _polarity_movable). */
static void _move_history_polarity(dt_develop_t *dev)
{
  const int n = g_list_length(dev->history);
  dt_dev_history_item_t **items = g_new0(dt_dev_history_item_t *, MAX(n, 1));
  int k = 0;
  for(GList *l = dev->history; l; l = g_list_next(l)) items[k++] = l->data;

  for(GList *m = dev->iop; m; m = g_list_next(m))
    _invert_root_for(dev, dev->forms, _params_at(dev->history, m->data, dev->history_end),
                     dev->history_end);

  for(int i = 0; i < n; i++)
  {
    if(!items[i]->forms) continue;
    int next = i + 1;
    while(next < n && !items[next]->forms) next++;
    const int limit = i < dev->history_end ? MIN(next, dev->history_end) : next;
    for(GList *m = dev->iop; m; m = g_list_next(m))
      _invert_root_for(dev, items[i]->forms, _params_at(dev->history, m->data, limit), limit);
  }

  // clear the bit where the tree it renders with took it, so that tree was
  // the one inverted; elsewhere it keeps inverting on its own
  const dt_dev_history_item_t *owner = NULL;
  for(int j = 0; j < n; j++)
  {
    if(items[j]->forms) owner = items[j];
    dt_develop_blend_params_t *bp = items[j]->blend_params;
    if(owner && _polarity_movable(dev, owner->forms, bp, j + 1))
      bp->mask_combine &= ~(uint32_t)DEVELOP_COMBINE_MASKS_POS;
  }
  g_free(items);
}

/* Normalize the group a stored history item renders through, inside that
   item's own forms snapshot.

   Every history item is a full cumulative copy of dev->forms as it stood at
   that step, so an item that carries masks carries its own tree, and the group
   its blend_params names is in it.

   dt_dev_read_history_ext() converts the params of every item, so every one
   comes back flexi and is stored that way. Its tree must be converted too:
   the history slider renders an older snapshot
   (dt_dev_pop_history_items_ext), and flexi params over a classic tree
   render a different mask. */
static void _normalize_history_item(const dt_develop_t *dev, dt_dev_history_item_t *h)
{
  if(!h->forms || !h->blend_params) return;
  if(!(h->blend_params->mask_mode & DEVELOP_MASK_FLEXI)) return;
  if(!dt_is_valid_maskid(h->blend_params->mask_id)) return;

  dt_masks_form_t *grp = dt_masks_get_from_id_ext(h->forms, h->blend_params->mask_id);
  if(!grp) return;

  _normalize_group(dev, &h->forms, grp);
}

/* Run-boundary normalization for classic drawn groups reused by a migration.

   Runs after dt_masks_read_masks_history(), unlike
   dt_masks_finish_flexi_migrations(): it changes groups already in the
   database, which the read would replace.

   The result must be written back (see _sync_forms_to_history): opening or
   exporting an image stores the converted params, flexi from then on, and
   migration never runs on it again. Do not persist the params without the
   converted trees: the flexi fold renders a classic tree differently. */
void dt_masks_normalize_flexi_groups(dt_develop_t *dev)
{
  if(!dev->pending_flexi_group_splits) return;

  for(GList *l = dev->pending_flexi_group_splits; l; l = g_list_next(l))
  {
    _normalize_group(dev, &dev->forms, dt_masks_get_from_id(dev, GPOINTER_TO_INT(l->data)));
  }

  // the live tree onto the item that owns it, then every stored snapshot on
  // its own. Normalizing is idempotent, so the owner being done twice costs
  // nothing
  _sync_forms_to_history(dev);
  for(GList *l = dev->history; l; l = g_list_next(l))
    _normalize_history_item(dev, l->data);

  // after the sync above, which gave the owner item its own copy of the tree:
  // the live tree and that copy are inverted separately, once each
  _move_history_polarity(dev);

  // only migration queues anything, so an already flexi edit returned above
  // and its masks are not rewritten on every load
  g_list_free(dev->pending_flexi_group_splits);
  dev->pending_flexi_group_splits = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
