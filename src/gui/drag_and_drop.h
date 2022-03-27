/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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

#define _BYTE 8
#define _WORD 16
#define _DWORD 32

/* common for all the drag&drop sources/destinations */
enum
{
  DND_TARGET_IMGID,
  DND_TARGET_URI,
  DND_TARGET_TAG
};

/* drag & drop for internal image ids */
static const GtkTargetEntry target_list_internal[] = { { "image-id", GTK_TARGET_SAME_APP, DND_TARGET_IMGID } };
static const guint n_targets_internal = G_N_ELEMENTS(target_list_internal);

/* drag & drop for global uris */
static const GtkTargetEntry target_list_external[] = { { "text/uri-list", GTK_TARGET_OTHER_APP, DND_TARGET_URI } };
static const guint n_targets_external = G_N_ELEMENTS(target_list_external);

/* drag & drop for both internal image ids and global uris */
static const GtkTargetEntry target_list_all[]
    = { { "image-id", GTK_TARGET_SAME_APP, DND_TARGET_IMGID }, { "text/uri-list", GTK_TARGET_OTHER_APP, DND_TARGET_URI } };
static const guint n_targets_all = G_N_ELEMENTS(target_list_all);

static const GtkTargetEntry target_list_tags[] = { { "tags-dnd", GTK_TARGET_SAME_WIDGET, DND_TARGET_TAG } };
static const guint n_targets_tags = G_N_ELEMENTS(target_list_tags);

static const GtkTargetEntry target_list_tags_dest[]
    = { { "image-id", GTK_TARGET_SAME_APP, DND_TARGET_IMGID }, { "tags-dnd", GTK_TARGET_SAME_WIDGET, DND_TARGET_TAG } };
static const guint n_targets_tags_dest = G_N_ELEMENTS(target_list_tags_dest);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

