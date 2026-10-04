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

#include <glib.h>
#include <stdio.h>

/* --styleapply-masks: does a *classic* style survive being applied to an image
 * that already has flexi masks of its own?
 *
 * Why this is not covered by --verify-masks or --roundtrip-masks.
 *
 * Both of those enter migration through dt_dev_read_history_ext(), which is
 * the history-stack path: a real history_num, deferred form synthesis via
 * dt_masks_finish_flexi_migrations(). Styles enter through a different door.
 * dt_styles_apply_style_item() (src/common/styles.c) calls
 * dt_develop_blend_legacy_params(), which delegates to the _ext variant with
 * history_num = -1 -- the *immediate*, non-deferred branch of
 * dt_masks_migrate_classic_to_flexi(). Two things follow from that -1 that no
 * other test exercises:
 *
 *   - _persist_form() writes the synthesized form to dev->forms ONLY; it does
 *     not touch main.masks_history (see its guard on history_num >= 0). The
 *     form's survival therefore depends entirely on whoever writes history
 *     next picking it up out of dev->forms.
 *
 *   - the migration runs against a dev that is already fully loaded and
 *     already owns another module's masks, rather than one being built up
 *     row by row.
 *
 * So the question this asks is narrow and specific: after applying a classic
 * style to an image that already carries flexi masks, does the style's own
 * migrated mask actually resolve on the next load, and is the image's
 * pre-existing mask still intact?
 *
 * The corpus is the same harvest JSON the other two tools take. Each edit is
 * used as the *incoming style*; the *host* edit (the mask already on the
 * image) is one fixed drawn-mask edit picked from the same corpus, so that
 * what varies between iterations is only the thing under test.
 *
 * The report carries a row for every harvested edit (skips included, with their
 * reason) and a "summary" object holding every figure the run prints, so it can
 * be read without the terminal output that went with it.
 *
 * Returns TRUE if every edit passed. */
gboolean dt_masks_styleapply_harvest(const char *json_path, const char *report_path);

/* Same run, writing its report as the body of an already-open JSON object (no
 * enclosing braces) so it can be composed into a combined document -- see
 * dt_masks_check_harvest(). `rf` may be NULL.
 *
 * `ran` (may be NULL) reports whether the check was applicable at all: it needs
 * a drawn-mask edit from the corpus to act as the host, and a library holding
 * none leaves nothing to apply a style onto. That is not a failure, so the
 * return value is still TRUE -- callers that distinguish "passed" from "did not
 * apply" should read this. */
gboolean dt_masks_styleapply_harvest_section(const char *json_path,
                                             FILE *rf,
                                             gboolean *ran);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
