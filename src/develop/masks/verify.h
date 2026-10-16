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

// migration verification: does a real edit render the same mask after
// migration to flexi as before? `darktable --verify-masks harvest.json`, on a
// file --harvest-masks wrote (see harvest.h).
//
// The unit tests check that migration's output is well formed, and never look
// at a pixel: a migration could pass them all and still feather with the wrong
// guide, combine shapes in the wrong order or apply an opacity at the wrong
// level, silently. This compares rendered masks.
//
// It does not compute what the mask should be, which would repeat the beliefs
// of migrate_legacy.c and pass whenever they are wrong. Both renders go
// through dt_develop_blend_process() unchanged, and
// pipe->store_all_raster_masks makes the blend publish its finished mask in
// piece->raster_masks.
//
// The image is the generated probe (probe_image.h), never the user's photo:
// masks are normalized, and parametric ones read whatever pixels are there.
// Two empty masks compare equal however wrong the migration is, so the probe
// has coverage tests of its own, and an edit whose mask is uniform before
// migration is reported as "inert", not as a pass.

#include <glib.h>
#include <stdio.h>

G_BEGIN_DECLS

/** replay every edit in the harvest file at `json_path`, rendering its mask
    before and after migration, and report. With `report_path`, a JSON report
    goes there too, with a row per edit and a "summary" of every figure the
    run prints, so that it stands without the terminal output. TRUE if every
    edit not skipped rendered the same */
gboolean dt_masks_verify_harvest(const char *json_path,
                                 const char *report_path);

/** the same run, writing its report as the members of a JSON object already
    open in `rf` ("source", "edits", "summary", without braces), so that
    several tools can share one document. `rf` may be NULL for no report */
gboolean dt_masks_verify_harvest_section(const char *json_path, FILE *rf);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
