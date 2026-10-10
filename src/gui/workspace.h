/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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

/* Canonical labels: "" (default), "memory", or a named workspace.
 * Translated UI strings like _("default") are also accepted by apply. */

/* GList of gchar* labels; always starts with "" then "memory", then
 * discovered names sorted. Free with g_list_free_full(list, g_free). */
GList *dt_workspace_list(const char *datadir);

/* Set database + workspace/label conf for label. */
void dt_workspace_apply(const char *label);

/* Create a named workspace (not default/memory). Returns FALSE if reserved
 * or already exists. On TRUE: seeds darktablerc-<label> from the current
 * workspace (sanitized), then applies conf like dt_workspace_apply. */
gboolean dt_workspace_new(const char *datadir, const char *label);

/* Touch one-shot marker so next start skips the workspace picker. */
void dt_workspace_request_relaunch_skip_picker(const char *datadir);

/* Show the full workspace picker (select / create / delete).
 * Returns TRUE if the user chose or created a workspace. */
gboolean dt_workspace_show_dialog(const char *datadir,
                                  const gboolean protect_active);

/* returns TRUE if a workspace is active/created and FALSE otherwise */
gboolean dt_workspace_create(const char *datadir);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
