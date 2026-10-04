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

// one command for the whole migration check.
//
// --roundtrip-masks, --verify-masks, --styleapply-masks, --persist-masks and
// --undo-masks each answer a different question about the same harvest file
// (see their own headers). `--check-masks harvest.json` runs them all
// against one file and writes a single self-contained FILE.check.json, so
// that a contributor's run can be acted on without its terminal output:
//
//   { "source": ..., "darktable_version": ...,
//     "roundtrip":  { "edits": [...], "summary": {...} },
//     "verify":     { "edits": [...], "summary": {...} },
//     "styleapply": { "edits": [...], "summary": {...} },
//     "persist":    { "edits": [...], "summary": {...} },
//     "undo":       { "edits": [...], "summary": {...} },
//     "summary":    { "passed": bool, per-tool flags } }
//
// Cheapest first: roundtrip names the field that broke, the others render.
// All always run: an early failure must not hide what the others would find,
// since a contributor's file may not come back a second time.
//
// It drives the real history writer against a scratch image id, so it needs
// `--library :memory:` and must never be pointed at a real catalog.

#include <glib.h>

G_BEGIN_DECLS

/** run all five checks over the harvest file at `json_path`, writing one
    combined report to `report_path` (may be NULL).

    Returns TRUE only if all passed. */
gboolean dt_masks_check_harvest(const char *json_path, const char *report_path);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
