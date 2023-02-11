/*
    This file is part of darktable,
    Copyright (C) 2019-2020 darktable developers.

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
  DT_META_OMIT_HIERARCHY = 1 << 18,
  DT_META_CALCULATED = 1 << 19
} dt_metadata_id;

typedef struct dt_export_metadata_t
{
  int32_t flags;
  GList *list;
} dt_export_metadata_t;

uint32_t dt_lib_export_metadata_default_flags(void);
uint32_t dt_lib_export_metadata_get_conf_flags(void);
char *dt_lib_export_metadata_get_conf(void);
void dt_lib_export_metadata_set_conf(const char *metadata_presets);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
