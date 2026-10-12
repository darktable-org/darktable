/*
    This file is part of darktable,
    Copyright (C) 2019-2026 darktable developers.

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
#include "develop/imageop.h"

/** save preset to file */
void dt_presets_save_to_file(const int rowid,
                             const char *preset_name,
                             const char *filedir);

/** load preset from file */
gboolean dt_presets_import_from_file(const char *preset_path);

/** does the module support autoapplying presets ? */
gboolean dt_presets_module_can_autoapply(const gchar *operation);

/** get the preset filters */
char *dt_presets_get_filter(const dt_image_t *image);

/** do the blend params `bp` of a module match a preset's stored ones, `blob`
    of `size` bytes? A preset carrying the forms of its mask
    (dt_masks_blend_blob_encode()) gives them fresh ids when applied, so its
    mask_id is not compared, nor are the forms */
gboolean dt_presets_blend_params_match(const struct dt_develop_blend_params_t *bp,
                                       const void *blob,
                                       const int size);

/** the same for a module: a stored mask is also compared with the forms the
    module's mask is made of, as editing a shape or a refinement leaves the
    blend params as they were */
gboolean dt_presets_module_blend_match(const struct dt_iop_module_t *module,
                                       const void *blob,
                                       const int size);

/** get preset multi_name for given module params */
char *dt_presets_get_module_label(const char *module_name,
                                  const void *params,
                                  const uint32_t param_size,
                                  const gboolean is_default_params,
                                  const void *blend_params,
                                  const uint32_t blend_params_size);

/* returns the module's multi-name to use given the name of the preset
   and the recorded preset's multi_name. This depends on the preference
   darkroom/ui/auto_module_name_update
*/
char *dt_presets_get_multi_name(const char *name,
                                const char *multi_name,
                                const gboolean localize);

/** get currently active preset name for the module */
gchar *dt_get_active_preset_name(dt_iop_module_t *module,
                                 gboolean *writeprotect);

/** helper for creating menu hierarchy, generates submenus as
 * indicated by vertical bars in name */
void dt_insert_preset_in_menu_hierarchy(const char *name,
                                        const char *action,
                                        GSList **menu_path2,
                                        GMenu *mainmenu2,
                                        GMenu **submenu2,
                                        gchar ***prev_split,
                                        gboolean isdefault,
                                        gboolean writeprotect);

/** TRUE when the module has a preset of that name at that version */
gboolean dt_presets_exists(const char *name,
                           const char *operation,
                           const int op_version);

/** the module and version of the preset at rowid; FALSE, nothing set,
    when there is none. Free *operation with g_free() */
gboolean dt_presets_get_operation(const int rowid,
                                  char **operation,
                                  int *op_version);

/** delete the preset at rowid unless it is write-protected */
void dt_presets_delete_unprotected(const int rowid);

/** a preset's fields as the preset edit dialog shows them */
typedef struct dt_presets_details_t
{
  int rowid;
  char *description;
  char *model;
  char *maker;
  char *lens;
  // the ISO bounds as SQLite prints them: the dialog cuts the decimals and
  // spots FLT_MAX by its "e+38" suffix
  char *iso_min;
  char *iso_max;
  double exposure_min;
  double exposure_max;
  double aperture_min;
  double aperture_max;
  double focal_length_min;
  double focal_length_max;
  int autoapply;
  int filter;
  int format;
  int op_params_size;
} dt_presets_details_t;

/** read the preset of that name, module and version into details; FALSE
    when there is none. Release with dt_presets_details_clear() */
gboolean dt_presets_get_details(const char *name,
                                const char *operation,
                                const int op_version,
                                dt_presets_details_t *details);
void dt_presets_details_clear(dt_presets_details_t *details);

/** a preset as the preset edit dialog saves it */
typedef struct dt_presets_write_t
{
  const char *name;
  const char *description;
  const char *model;
  const char *maker;
  const char *lens;
  double iso_min;
  double iso_max;
  double exposure_min;
  double exposure_max;
  double aperture_min;
  double aperture_max;
  double focal_length_min;
  double focal_length_max;
  int autoapply;
  int filter;
  int format;
  // a processing module's preset: the fields below are written
  gboolean iop;
  const void *op_params;
  int op_params_size;
  int enabled;
  const char *multi_name;
  int multi_name_hand_edited;
  // written for a processing module's preset, or a new one
  const void *blendop_params;
  int blendop_params_size;
  int blendop_version;
  // written for a new preset only
  const char *operation;
  int op_version;
} dt_presets_write_t;

/** update the preset at rowid, or insert a new one when rowid < 0 */
void dt_presets_write(const int rowid, const dt_presets_write_t *p);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
