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

// Does a migrated mask survive being saved and loaded again?
//
// Run as `darktable --roundtrip-masks harvest.json`, on a file produced by
// --harvest-masks (see harvest.h).
//
// WHY THIS IS SEPARATE FROM --verify-masks
//
// The verifier (verify.h) replays an edit entirely in memory: it builds a dev,
// migrates, renders, and never touches the database -- it passes history_num
// = -1 precisely so migration takes its non-persisting path. That is the right
// shape for asking "does migration compute the same mask", and it is
// structurally blind to a whole class of failure: state that is correct in
// memory and is then lost, duplicated, or misread on the way to and from disk.
//
// Two of the three migration outcomes only exist after a write:
//
//  - Parametric and raster migrations synthesize *new* forms, which have no
//    masks_history rows of their own. They are written under history_end - 1
//    by dt_masks_finish_flexi_migrations(), and if that write is wrong the
//    form vanishes on the next load, while the in-memory replay passes.
//  - The group markers dt_masks_normalize_flexi_groups() derives are written
//    back with the migrated blend_params, which are then flexi, so the next
//    load takes another path: migration does nothing and the markers must
//    come from storage. This checks that the two paths agree.
//
// WHAT IT COMPARES
//
// Per edit: seed a scratch image with the harvested *classic* history and mask
// forms, read it through the real dt_dev_read_history_ext(), snapshot the
// resulting blend_params and form tree, write it back with the real
// dt_dev_write_history_ext(), read it a second time, and snapshot again. The
// two snapshots must be identical.
//
// Comparing state rather than pixels is deliberate and is not a weaker test:
// --verify-masks already establishes that a given (blend_params, form tree)
// renders the same mask as its classic original, over the whole corpus. What
// is unknown is only whether that tuple survives a save/load, so that is what
// is measured -- and a state diff names the field that broke, where a pixel
// diff would only say "something did".

#include <glib.h>
#include <stdio.h>

G_BEGIN_DECLS

/** Round-trip every edit in the harvest file at `json_path` through a real
    database write and read. If `report_path` is non-NULL a per-edit JSON
    report is written there, carrying a row for every harvested edit (skips
    included, with their reason) and a "summary" object holding every figure
    the run prints.

    Returns TRUE if every edit came back identical. */
gboolean dt_masks_roundtrip_harvest(const char *json_path,
                                    const char *report_path);

/** Same run, writing its report as the body of an already-open JSON object
    (no enclosing braces) so it can be composed into a combined document --
    see dt_masks_check_harvest(). `rf` may be NULL. */
gboolean dt_masks_roundtrip_harvest_section(const char *json_path, FILE *rf);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
