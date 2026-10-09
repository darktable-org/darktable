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

// Does closing and reopening the image between two edits change the mask?
//
// Run as `darktable --library :memory: --persist-masks harvest.json`, on a
// --harvest-masks file. It WRITES to the database, so it is only ever safe
// against a scratch library.
//
// WHY THIS CHECK EXISTS
//
// migration writes the group markers dt_masks_normalize_flexi_groups() derives
// back to storage (_sync_forms_to_history() in migrate_legacy.c): a later load
// finds a current-version flexi edit and normalizes nothing, so a marker that
// was not stored renders the mask wrong from then on. --verify-masks and
// --styleapply-masks work in memory and never ask the database; --roundtrip-
// masks compares stored state against a hand-written invariant that covers
// the group boundaries alone.
//
// So the property here is about pixels, and needs no notion of what the mask
// ought to be:
//
//     for every sequence of panel edits e1..en applied to a migrated mask,
//         render(en(...e1(G))) == render(en(...(save/reload)...e1(G)))
//
// A save and a reload between two edits is something the user can do at any
// moment without being told; if it changes the mask, some part of the mask's
// meaning is not stored. A load reads the stored markers rather than deriving
// them again, so this also covers everything the markers carry.
//
// WHAT IS SWEPT
//
// Not all combinations -- a bounded set of short sequences aimed at the seams
// where a save can lose something, listed in _sequences[] with the seam each
// one covers. In outline:
//
//   - a group operator the user changes and then reads back through another
//     group control on the next edit
//   - the base-case repair's disable bits, written by migration and then built
//     on
//   - the group modifiers (bypass, invert-output, group opacity, group
//     refinement) set on one side of a save and read on the other. They live
//     on the group's marker, so a marker the save lost or moved shows up in
//     them
//   - single-step sequences as a floor: if one edit does not survive one save,
//     nothing longer means anything
//
// Both arms address what they edit by resolving the scope (the group, its
// first element, its last) against their OWN current state rather than
// against indices computed once. That is deliberate and makes the check
// strictly more sensitive: if the save lost or moved a point, the reloaded
// arm's next edit lands on a different one -- which is precisely what the
// user would experience, their next click going somewhere else. When the save
// is transparent the two states are identical and the resolution trivially
// agrees.
//
// Every group the module renders through is swept in turn, the mask's own and
// the nested ones (dt_masks_postedit_groups).
//
// WHAT IT CATCHES
//
// A save that loses the migration's write-back (_sync_forms_to_history() in
// migrate_legacy.c skipped) fails it on the migration_failures corpus.
//
// The sequences built on a bypass (single:group-bypass, "modifier:bypass then
// minimum" and "modifier:bypass then opacity") cannot fire on such a loss: a
// bypassed group contributes nothing whatever else it holds. They stay
// because a bypass that did not survive a save at all shows against the
// un-poked baseline, which nothing else here would see.
//
// CPU ONLY: group folding happens on the CPU for the GPU path too, so the
// OpenCL blend consumes a mask the CPU built, and a GPU replay would exercise
// the same fold twice.

#include <glib.h>
#include <stdio.h>

G_BEGIN_DECLS

/** Replay every edit in the harvest file at `json_path`, applying each edit
    sequence twice -- once wholly in memory, once with a save and a reload
    between every step -- and comparing the rendered masks. If `report_path` is
    non-NULL a per-edit JSON report is written there.

    Returns TRUE if every swept edit matched on every sequence. */
gboolean dt_masks_persist_harvest(const char *json_path,
                                  const char *report_path);

/** Same run, writing its report as the *body* of an already-open JSON object
    (`"source"`, `"edits"`, `"summary"` members, no enclosing braces) so it can
    be composed into one document -- see dt_masks_check_harvest(). `rf` may be
    NULL to run without a report. */
gboolean dt_masks_persist_harvest_section(const char *json_path, FILE *rf);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
