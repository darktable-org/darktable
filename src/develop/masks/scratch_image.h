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

#include "common/image.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/masks/harvest_read.h"

#include <glib.h>

/* Putting a harvested edit into a throwaway database as *classic* history, so
 * that reading it back runs the real migration.
 *
 * Shared by the --*-masks checks (roundtrip, styleapply, persist, undo, the
 * lock check), which all need an image that exists only for the duration of
 * one comparison. Every one of these
 * writes to the database, so they are only ever safe against a scratch library
 * (`--library :memory:`), never a real catalog.
 */

/** Open the scratch image `imgid` into `dev` the way darktable opens an
    image, through the real history reader. Caller cleans up with
    dt_dev_cleanup().

    The read passes no_image = TRUE: there is no raw file behind the scratch
    row, and the default-module machinery that flag skips would add
    auto-applied modules that have nothing to do with what is being measured.
    It also skips loading the image, so dev->image_storage would keep the
    invalid id dt_dev_init() leaves. Migration's _mask_id_has_content()
    (migrate_legacy.c), given a real history num, decides whether a classic
    drawn group has content by querying

        SELECT points_count FROM main.masks_history
         WHERE imgid = module->dev->image_storage.id AND formid = ?

    With an unset id that finds nothing, and migration takes the no-content
    branch: mask_mode drops to DEVELOP_MASK_ENABLED and mask_id to NO_MASKID.
    A drawn+parametric edit with an inert parametric side would then arrive
    with no mask at all, which round-trips, renders and compares equal to
    itself all the same. So this sets the identity between dt_dev_init() and
    the read. */
void dt_masks_scratch_open(dt_develop_t *dev, const dt_imgid_t imgid);

/** Open the scratch image and copy out the mask of its last history item:
    `bp_out` gets the blend_params and `forms_out` a deep copy of the form
    tree, both owned by the caller (either may be NULL). Stands in for the
    close-and-reopen of dt_dev_reload_history_items(), which needs the GUI.
    Returns FALSE if nothing masked came back. */
gboolean dt_masks_scratch_read_last(const dt_imgid_t imgid,
                                    dt_develop_blend_params_t *bp_out,
                                    GList **forms_out);

/** put the scratch image back to its just-migrated state: seed the classic
    history row of `edit` again and open it once, which runs migration and stores the
    result.

    `bp_out` and `forms_out` (either may be NULL) receive the migrated state as
    it stands in memory, before anything is read back. Whether the stored
    state is enough to reconstruct it is what --persist-masks asks, so an arm
    that wants "the first open" takes it from here, not from a re-read: with
    both arms past a lossy save, they would agree about the wrong mask.

    Returns FALSE if the row could not be seeded, or if a requested output
    found no mask to copy. */
gboolean dt_masks_scratch_reset_to_migrated(const dt_imgid_t imgid,
                                            const dt_masks_harvest_edit_t *edit,
                                            dt_develop_blend_params_t *bp_out,
                                            GList **forms_out);

/** Wipe history, masks_history and module_order for `imgid`. */
void dt_masks_scratch_wipe_history(const dt_imgid_t imgid);

/** Create (or replace) the scratch image row. There are no NOT NULL
    constraints on main.images, so only the few columns the history reader
    actually consults are filled. history_end is 1: callers seed a single
    history row, at num 0. */
void dt_masks_scratch_seed_image(const dt_imgid_t imgid,
                                 const int width,
                                 const int height);

/** Write one history row at `num`, plus its forms under the same num.

    `op_params` are the module's *defaults*, not the user's: the harvest
    deliberately records no module parameters (they are user data and say
    nothing about masks). That substitution is sound because nothing under test
    reads them -- migration and the mask code work on blend_params and the form
    tree -- but they cannot simply be omitted, since the history reader runs
    each module's own legacy_params on them and drops the row outright if the
    blob is the wrong size.

    `blendop_version` is the harvested one, so the row genuinely arrives as
    classic and the real migration runs on read.

    `multi_priority` is written as given. A second instance only survives the
    read if the image also has an iop-order entry for it -- see
    dt_masks_scratch_seed_iop_order(), which callers must invoke first for any
    non-zero multi_priority.

    Returns FALSE if the module is unknown or the insert failed. */
gboolean dt_masks_scratch_seed_history(const dt_imgid_t imgid,
                                       const int num,
                                       const char *operation,
                                       const int multi_priority,
                                       const int blendop_version,
                                       const dt_develop_blend_params_t *bp,
                                       GList *forms);

/** Give `imgid` an iop-order list that contains (operation, multi_priority).

    Only needed for a second or later instance. A scratch image otherwise gets
    the *default* iop order, which has one entry per module at instance 0 only;
    dt_ioppr_get_iop_order() returns INT_MAX for any other instance ("cannot get
    iop-order for X instance N"), and dt_dev_read_history_ext() then drops the
    row without a word, leaving a dev with no modules, and comparisons between
    two empty module lists pass whatever migration did: without it a
    multi-instance edit is skipped, not tested.

    Mirrors dt_ioppr_insert_module_instance(): the new entry goes immediately
    before the highest-instance entry already present for that operation, which
    is where darktable itself puts a duplicated module. A no-op for
    multi_priority 0. */
void dt_masks_scratch_seed_iop_order(const dt_imgid_t imgid,
                                     const char *operation,
                                     const int multi_priority);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
