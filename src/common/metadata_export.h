/*
    This file is part of darktable,
    copyright (c) 2019 philippe weyland.

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

typedef enum dt_metadata_id
{
  DT_META_NONE = 0,
  DT_META_EXIF = 1 << 0,
  DT_META_METADATA = 1 << 1,
  DT_META_GEOTAG = 1 << 2,
  DT_META_TAG = 1 << 3,
  DT_META_HIERARCHICAL_TAG = 1 << 4,
  DT_META_DT_HISTORY= 1 << 5,
  DT_META_PRIVATE_TAG = 1 << 16,
  DT_META_SYNONYMS_TAG = 1 << 17,
} dt_metadata_id;

typedef struct dt_export_metadata_t
{
  int32_t flags;
  GList *list;
} dt_export_metadata_t;

/** open dialog to configure metadata exportation */
// char *dt_lib_export_metadata_configuration_dialog(const char *name);

/** get list of metadata which can be calculated or removed from exported file */
const char **dt_lib_export_metadata_get_export_keys(guint *dt_export_xmp_keys_n);
/** get  metadata presets */
GList *dt_lib_export_metadata_get_presets(const char *name, int32_t *flags);
/** delete metadata presets */
void dt_lib_export_metadata_delete_presets(const char *name);
/** get the list of export metadata presets */
GList *dt_lib_export_metadata_get_presets_list();
// only difference with dt_lib_presets_add is writeprotect = 0
void dt_lib_export_metadata_presets_add(const char *name, const char *plugin_name, const int32_t version, const void *params,
                        const int32_t params_size);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
