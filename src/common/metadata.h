/*
    This file is part of darktable,
    Copyright (C) 2010-2025 darktable developers.

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

G_BEGIN_DECLS

typedef struct dt_metadata_t
{
  uint32_t key;
  gchar *tagname;
  gchar *name;
  uint32_t internal;
  gboolean visible;
  gboolean priv;
  uint32_t display_order;
} dt_metadata_t;

// for compatibility we need to keep the number of metadata fields we had
// before PR #18036
#define DT_METADATA_LEGACY_NUMBER 9

typedef enum dt_metadata_signal_t
{
  DT_METADATA_SIGNAL_NEW_VALUE,     // metadata value changed
  DT_METADATA_SIGNAL_PREF_CHANGED   // metadata preferences changed
}
dt_metadata_signal_t;

// a metadata value and the number of images holding it
typedef struct dt_metadata_count_t
{
  uint32_t key;
  gchar *value;
  uint32_t count;
} dt_metadata_count_t;

// a version of an image and its version name
typedef struct dt_metadata_version_t
{
  int version;
  dt_imgid_t imgid;
  gchar *name;
} dt_metadata_version_t;

typedef enum dt_metadata_flag_t
{
  DT_METADATA_FLAG_IMPORTED = 1 << 2    // metadata import
}
dt_metadata_flag_t;

/** return the list of metadata items */
GList *dt_metadata_get_list();

/** sort the list by display_order */
void dt_metadata_sort();

/** add a metadata entry */
gboolean dt_metadata_add_metadata(dt_metadata_t *metadata);

/** update name, visibility, privacy and display order of a metadata entry,
    in the database only: the caller updates the cached entry
    (dt_metadata_get_metadata_by_keyid()) */
void dt_metadata_update_metadata(const dt_metadata_t *metadata);

/** number of image assignments for the metadata keys in the list */
uint32_t dt_metadata_count_assigned(const GList *keys);

/** remove the metadata entries in the list and their image assignments,
    in the database only: the caller refreshes the cached list with
    dt_metadata_init() */
void dt_metadata_remove_keys(const GList *keys);

/** return the metadata by keyid */
dt_metadata_t *dt_metadata_get_metadata_by_keyid(const uint32_t keyid);

/** return the metadata by tagname */
dt_metadata_t *dt_metadata_get_metadata_by_tagname(const char *tagname);

/** return the keyid of the metadata key */
uint32_t dt_metadata_get_keyid(const char* key);

/** return the key of the metadata keyid */
const char *dt_metadata_get_key(const uint32_t keyid);

/** return the key of the metadata subkey */
const char *dt_metadata_get_key_by_subkey(const char *subkey);

/** return the last part of the tagname */
const char *dt_metadata_get_tag_subkey(const char *tagname);

/** init metadata flags */
void dt_metadata_init();

/** Set metadata for a specific image, or all selected for an invalid id */
void dt_metadata_set(const dt_imgid_t imgid, const char *key, const char *value, const gboolean undo_on); // duplicate.c, lua/image.c

/** Set imported metadata for a specific image (with mutex lock) */
void dt_metadata_set_import_lock(const dt_imgid_t imgid, const char *key, const char *value);

/** Set imported metadata for a specific image */
void dt_metadata_set_import(const dt_imgid_t imgid, const char *key, const char *value);

/** Set metadata (named keys) for a specific image, or all selected for id == -1. */
/** list is a set of key, value */
void dt_metadata_set_list(const GList *imgs, GList *key_value, const gboolean undo_on); // libs/metadata.c

/** Set metadata (id keys) for a list of images.
    list is a set of keyid, value
    if clear_on TRUE the image metadata are cleared before attaching the new ones*/
void dt_metadata_set_list_id(const GList *img, const GList *metadata, const gboolean clear_on,
                             const gboolean undo_on);
/** Get metadata (named keys) for a specific image, or all selected for an invalid imgid
    For keys which return a string, the caller has to make sure that it
    is freed after usage. With mutex lock. */
GList *dt_metadata_get_lock(const dt_imgid_t imgid, const char *key, uint32_t *count);
/** Get metadata (named keys) for a specific image, or all selected for an invalid imgid
    For keys which return a string, the caller has to make sure that it
    is freed after usage. */
GList *dt_metadata_get(const dt_imgid_t imgid, const char *key, uint32_t *count);

/** Get metadata (id keys) for a specific image. The caller has to free the list after usage. */
GList *dt_metadata_get_list_id(const dt_imgid_t imgid); // libs/image.c

/** Get the non-empty values set on the images of a comma-separated id list,
    as dt_metadata_count_t, each with the number of images holding it.
    Free with g_list_free_full(list, dt_metadata_count_free). */
GList *dt_metadata_get_counts(const char *images); // libs/metadata.c
void dt_metadata_count_free(gpointer data);

/** Get all versions of an image, by version, as dt_metadata_version_t.
    Free with g_list_free_full(list, dt_metadata_version_free). */
GList *dt_metadata_get_versions(const dt_filmid_t film_id, const char *filename); // libs/duplicate.c
void dt_metadata_version_free(gpointer data);

/** Remove metadata from images in list */
void dt_metadata_clear(const GList *imgs, const gboolean undo_on); // libs/metadata.c

/** check if the "Xmp.darktable.image_id" already exists */
gboolean dt_metadata_already_imported(const char *filename, const char *datetime);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

