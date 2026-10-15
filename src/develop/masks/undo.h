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

/* --undo-masks: does undoing a mask edit put the mask back?
 *
 * WHY THIS IS NOT ANSWERED BY THE OTHER CHECKS
 *
 * Every other harvest check compares two states that were each *built* --
 * migrated against from-scratch, in-memory against reloaded, authored against
 * stored. Undo is the one operation that has to RECOVER a state it saw
 * earlier, and it recovers it from a copy taken before the edit, not from the
 * edit's inverse. Nothing else here exercises that copy.
 *
 * The copy is dt_history_duplicate() (common/history.c), and the part of it
 * that matters for masks is one line:
 *
 *     if(old->forms) new->forms = dt_masks_dup_forms_deep(old->forms, NULL);
 *
 * Every history item carries its own snapshot of the form tree, and that
 * per-item snapshot is what _dev_write_history_item() writes back to
 * masks_history (develop.c). So an undo is: swap a duplicated history stack
 * into the dev, write it, reload. If the duplicate lost a form, or kept a
 * shallow pointer into the tree the edit then mutated, the write puts the
 * *edited* mask back under the *pre-edit* history and the undo silently does
 * nothing to the mask while appearing to work on everything else.
 *
 * A history item's forms snapshot that holds something other than what its
 * blend_params describe is the failure to look for.
 *
 * THE PROPERTY
 *
 *     render(edit, then undo)  == render(before the edit)
 *     render(edit, undo, redo) == render(after the edit)
 *
 * both to the last bit. Two directions rather than one because an undo that
 * restores by throwing the mask away would pass the first on its own for a
 * whole class of edit -- and because redo re-applies from a snapshot taken
 * *after* the edit, which is a different copy with a different way to be
 * wrong.
 *
 * WHAT IS SIMULATED AND WHAT IS REAL
 *
 * The undo *stack* is not exercised: dt_undo_record() is called from
 * libs/history.c behind a GTK widget and a DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE
 * signal, neither of which exists headless. What is exercised is everything
 * that stack moves around -- dt_history_duplicate(), the swap, the write, the
 * reload -- driven in the same order _pop_undo() drives it. So this check
 * cannot tell you that ctrl-z is wired to the right callback. It can tell you
 * that when it fires, the mask comes back.
 *
 * The reload is a full close-and-reopen rather than
 * dt_dev_reload_history_items() (which needs the GUI). That is strictly
 * stronger and shares its plumbing with --persist-masks.
 *
 * Renders are CPU-only, single-threaded, and compared at 1e-6: both sides run
 * identical code over data that should be identical, so a real match is
 * bit-exact.
 *
 * Writes to the database, so like every other scratch-image check it is only
 * safe against `--library :memory:`.
 */

G_BEGIN_DECLS

/** Run the undo check over one --harvest-masks file. `report_path` may be
    NULL. Returns TRUE if nothing disagreed. */
gboolean dt_masks_undo_harvest(const char *json_path, const char *report_path);

/** The same, writing its JSON into an already-open object for --check-masks. */
gboolean dt_masks_undo_harvest_section(const char *json_path, FILE *rf);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
