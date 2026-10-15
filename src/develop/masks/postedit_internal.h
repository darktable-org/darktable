/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

// The panel's controls, as something a check can apply to a group.
//
// postedit.c owns this: it enumerates every control the masks panel offers and
// applies one to a range of a group's point list, which is what a check needs
// to stand in for a user changing something. --persist-masks and --undo-masks
// both use it; they differ in what they do around the change (a save and
// reload, an undo and redo), not in what the change is.
//
// Sharing it is not just economy. If the checks each kept their own idea of
// what "set the group opacity" means they could drift, and the weaker one
// would then be reporting on a control the panel no longer has.
//
// Nothing outside src/develop/masks/ may use this: it exists for the
// --harvest-masks tooling, not for the GUI or the pipeline.

#include "develop/blend.h"
#include "develop/masks.h"

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  // per group, broadcast across the group's marker and members -- the fold
  // reads all of these from the marker
  POKE_FLEXI_MAXIMUM = 0,
  POKE_FLEXI_SCREEN,
  POKE_FLEXI_MINIMUM,
  POKE_FLEXI_PRODUCT,
  POKE_FLEXI_SUM,
  POKE_FLEXI_DIFFERENCE,
  POKE_FLEXI_EXCLUSION,
  POKE_GROUP_BYPASS,
  POKE_GROUP_INVERT,
  POKE_GROUP_OPACITY,
  POKE_GROUP_REFINE,
  // per element
  POKE_ELEM_DISABLE,
  POKE_ELEM_HIDDEN,
  POKE_ELEM_INVERSE,
  POKE_ELEM_OPACITY,
  POKE_ELEM_REFINE,
  POKE_N
} poke_t;

/* The other half of what the panel can do to a mask: change a SHAPE.

   A poke changes how a member is combined. None of them touches the geometry
   the member refers to, and geometry is not a detail the checks can wave away:
   which shapes overlap is what decides whether an intersection or a difference
   has anything to compute, so a whole class of operator is only ever exercised
   inertly on a corpus where the harvested shapes happen not to meet. Moving one
   shape onto another turns those from "compared, inert" into "compared, live".

   For the storage checks the stake is different and larger: geometry is the one
   part of a mask with a per-type serialized representation (a blob of
   dt_masks_point_<type>_t in masks_history), so a shape that has been edited is
   the only thing that exercises writing a point struct the harvest did not
   supply. path.c's resize even keeps a cached baseline alongside the points --
   state that a save has to either carry or reconstruct.

   These are the panel's own shape controls, driven the way the panel drives
   them: through functions->modify_property() for the sliders, and by moving
   points for the drags, which have no property. */
typedef enum
{
  GEOM_TRANSLATE = 0,   // drag the whole shape across the image
  GEOM_NODE,            // drag one node of a path/brush, deforming it
  GEOM_SIZE,
  GEOM_FEATHER,
  GEOM_HARDNESS,
  GEOM_ROTATION,
  GEOM_CURVATURE,
  GEOM_COMPRESSION,
  GEOM_N
} geom_t;

// ---------------------------------------------------------------------------
// one panel action, addressed to part of a group
// ---------------------------------------------------------------------------

/* A poke and a geometry control say WHAT to change; a step says what and
   WHERE, and adds the two things that are neither -- deleting a member and
   reordering the list.

   Shared by --persist-masks and --undo-masks, for the same reason the poke
   vocabulary above is. */
typedef enum
{
  SCOPE_GROUP = 0,  // the group itself: its marker
  SCOPE_FIRST,      // the first element on its own
  SCOPE_LAST        // the last element on its own
} scope_t;

typedef enum
{
  STEP_POKE = 0,   // change a member's own state (poke_t in `k`)
  STEP_REMOVE,     // delete the member the scope names
  STEP_MOVE_UP,    // swap it with the member below it in the list
  STEP_GEOM,       // edit the SHAPE the member refers to (geom_t in `k`)
} step_kind_t;

/* `k` carries the poke for STEP_POKE and the geom_t for STEP_GEOM: the two
   never appear in the same step, and a second field would have to be spelled
   out in every sequence initializer. STEP_POKE is 0, so that a poke step
   needs only two fields */
typedef struct { poke_t k; scope_t s; step_kind_t kind; } step_t;

#define GEOM_STEP(g, sc) { (poke_t)(g), (sc), STEP_GEOM }

/** The module's own flexi mask group in `dev`, or NULL.

    Read from the module rather than from dev->forms at large: dev->forms is
    per image and every masks_history row is a cumulative snapshot, so it
    routinely carries groups belonging to other modules and groups orphaned by
    earlier edits (see roundtrip.c). */
dt_masks_form_t *dt_masks_postedit_target_group(dt_develop_t *dev,
                                                const dt_develop_blend_params_t *bp);

/** Every group the module renders through: the target group first, then its
    nested groups in a breadth-first walk over member formids in list order.

    The order is deterministic, so two arms of a check resolve the same index
    to the same group. 5.7% of harvested edits carry a nested group. The walk
    is bounded and deduplicated, so a malformed or cyclic tree cannot spin.
    Free the list (not its data) with g_list_free(). */
GList *dt_masks_postedit_groups(dt_develop_t *dev, const dt_develop_blend_params_t *bp);

/** Apply one step to `grp` as it currently stands.

    `dev` resolves member formids to shapes, and is only read by STEP_GEOM.

    A structural step is a no-op on a group with a single member: removing it
    would leave an empty group and reordering it has nothing to swap with, and
    neither is a state the panel can produce either. */
void dt_masks_postedit_apply_step(dt_develop_t *dev, dt_masks_form_t *grp, const step_t *st);

/** Apply one poke to the points [first, last] of `points`: the group's
    marker, index 0, for a group-level poke, and one member for an
    element-level one. */
void dt_masks_postedit_apply_poke(GList *points, const poke_t k, const int first, const int last);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
