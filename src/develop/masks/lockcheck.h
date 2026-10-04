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

/* --lock-masks: does a locked mask survive paste and styles?
 *
 * The lock (dt_develop_blend_params_t's mask_lock) is honored in the history
 * merge, and restored around an overwrite paste, which deletes the history
 * before anything can check it (see _locked_masks_restore in
 * common/history.c). Both run on the database, through a history read back
 * from it, so a unit test on the params alone cannot see them: this drives
 * the real dt_history_copy_and_paste_on_image() and dt_styles_apply_to_image()
 * against two scratch images, and reads the result back the same way.
 *
 * Writes to the database: run it with `--library :memory:` only, never on a
 * real catalog. Prints one line per case and returns TRUE if all passed.
 */
gboolean dt_masks_lock_check(void);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
