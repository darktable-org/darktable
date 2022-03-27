/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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

typedef enum dt_metadata_t
{
  // do change the order. Must match with dt_metadata_def[] in metadata.c.
  // just add new metadata before DT_METADATA_NUMBER when needed
  // and this must also be synchronized with the collect.c module (legacy_presets).
  DT_METADATA_XMP_DC_CREATOR,
  DT_METADATA_XMP_DC_PUBLISHER,
  DT_METADATA_XMP_DC_TITLE,
  DT_METADATA_XMP_DC_DESCRIPTION,
  DT_METADATA_XMP_DC_RIGHTS,
  DT_METADATA_XMP_ACDSEE_NOTES,
  DT_METADATA_XMP_VERSION_NAME,
  DT_METADATA_XMP_IMAGE_ID,
  DT_METADATA_NUMBER
}
dt_metadata_t;

typedef enum dt_metadata_type_t
{
  DT_METADATA_TYPE_USER,     // metadata for users
  DT_METADATA_TYPE_OPTIONAL, // metadata hidden by default
  DT_METADATA_TYPE_INTERNAL  // metadata for dt internal usage - the user cannot see it
}
dt_metadata_type_t;

typedef enum dt_metadata_signal_t
{
  DT_METADATA_SIGNAL_SHOWN,       // metadata set as shown
  DT_METADATA_SIGNAL_HIDDEN,      // metadata set as hidden
  DT_METADATA_SIGNAL_NEW_VALUE    // metadata value changed
}
dt_metadata_signal_t;

typedef enum dt_metadata_flag_t
{
  DT_METADATA_FLAG_HIDDEN = 1 << 0,     // metadata set as shown
  DT_METADATA_FLAG_PRIVATE = 1 << 1,    // metadata set as hidden
  DT_METADATA_FLAG_IMPORTED = 1 << 2    // metadata value changed
}
dt_metadata_flag_t;

/** return the number of user metadata (!= DT_METADATA_TYPE_INTERNAL) */
unsigned int dt_metadata_get_nb_user_metadata();

/** return the metadata key by display order */
const char *dt_metadata_get_name_by_display_order(const uint32_t order);

/** return the metadata keyid by display order */
dt_metadata_t dt_metadata_get_keyid_by_display_order(const uint32_t order);

/** return the metadata keyid by name */
dt_metadata_t dt_metadata_get_keyid_by_name(const char* name);

/** return the metadata type by display order */
int dt_metadata_get_type_by_display_order(const uint32_t order);

/** return the metadata name of the metadata keyid */
const char *dt_metadata_get_name(const uint32_t keyid);

/** return the keyid of the metadata key */
dt_metadata_t dt_metadata_get_keyid(const char* key);

/** return the key of the metadata keyid */
const char *dt_metadata_get_key(const uint32_t keyid);

/** return the metadata subeky of the metadata keyid */
const char *dt_metadata_get_subkey(const uint32_t keyid);

/** return the key of the metadata subkey */
const char *dt_metadata_get_key_by_subkey(const char *subkey);

/** return the type of the metadata keyid */
int dt_metadata_get_type(const uint32_t keyid);

/** init metadata flags */
void dt_metadata_init();

/** Set metadata for a specific image, or all selected for id == -1. */
void dt_metadata_set(int id, const char *key, const char *value, const gboolean undo_on); // duplicate.c, lua/image.c

/** Set imported metadata for a specific image */
void dt_metadata_set_import(int id, const char *key, const char *value); // exif.cc, ligthroom.c

/** Set metadata (named keys) for a specific image, or all selected for id == -1. */
/** list is a set of key, value */
void dt_metadata_set_list(const GList *imgs, GList *key_value, const gboolean undo_on); // libs/metadata.c

/** Set metadata (id keys) for a list of images.
    list is a set of keyid, value
    if clear_on TRUE the image metadata are cleared before attaching the new ones*/
void dt_metadata_set_list_id(const GList *img, const GList *metadata, const gboolean clear_on,
                             const gboolean undo_on);
/** Get metadata (named keys) for a specific image, or all selected for id == -1.
    For keys which return a string, the caller has to make sure that it
    is freed after usage. */
GList *dt_metadata_get(int id, const char *key, uint32_t *count); // exif.cc, variables.c, facebook.c, flicker.c, gallery.c, googlephoto.c, latex.c, piwigo.c, watermark.c, metadata_view.c, libs/metadata.c, print_settings.c, lua/image.c

/** Get metadata (id keys) for a specific image. The caller has to free the list after usage. */
GList *dt_metadata_get_list_id(int id); // libs/image.c

/** Remove metadata from specific images, or all selected for id == -1. */
void dt_metadata_clear(const GList *imgs, const gboolean undo_on); // libs/metadata.c

/** check if the "Xmp.darktable.image_id" already exists */
gboolean dt_metadata_already_imported(const char *filename, const char *datetime);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

