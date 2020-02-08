/*
    This file is part of darktable,
    copyright (c) 2010 tobias ellinghaus.

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

#include "common/darktable.h"
#include "gui/gtk.h"
#include "metadata_gen.h"

/** Set metadata for a specific image, or all selected for id == -1. */
void dt_metadata_set(int id, const char *key, const char *value, const gboolean undo_on, const gboolean group_on); // exif.cc, ligthroom.c, duplicate.c, lua/image.c

/** Set metadata (named keys) for a specific image, or all selected for id == -1. */
/** list is a set of key, value */
void dt_metadata_set_list(int id, GList *key_value, const gboolean undo_on, const gboolean group_on); // libs/metadata.c

/** Set metadata (id keys) for a specific image, or all selected for id == -1.
    list is a set of keyid, value
    if clear_on TRUE the image metadata are cleared before attaching the new ones*/
void dt_metadata_set_list_id(const int id, GList *key_value, const gboolean clear_on, const gboolean undo_on, const gboolean group_on); // libs/image.c

/** Get metadata (named keys) for a specific image, or all selected for id == -1.
    For keys which return a string, the caller has to make sure that it
    is freed after usage. */
GList *dt_metadata_get(int id, const char *key, uint32_t *count); // exif.cc, variables.c, facebook.c, flicker.c, gallery.c, googlephoto.c, latex.c, piwigo.c, watermark.c, metadata_view.c, libs/metadata.c, print_settings.c, lua/image.c

/** Get metadata (id keys) for a specific image. The caller has to free the list after usage. */
GList *dt_metadata_get_list_id(int id); // libs/image.c

/** Remove metadata from specific images, or all selected for id == -1. */
void dt_metadata_clear(int id, const gboolean undo_on, const gboolean group_on); // libs/metadata.c

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
