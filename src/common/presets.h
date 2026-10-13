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

// upper bound of a preset's focal length range
#define MAX_FOCAL_LEN 100000

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

/** overwrite the module parameters of the module's preset of that name,
    whatever its version, with the given ones at version op_version */
void dt_presets_update_params(const char *operation,
                              const int op_version,
                              const char *name,
                              const void *op_params,
                              const int op_params_size,
                              const int enabled,
                              const void *blendop_params,
                              const int blendop_params_size,
                              const int blendop_version);

/** a preset's parameters, blobs copied with a zero byte after them: an
    empty or NULL blob is NULL */
typedef struct dt_presets_params_t
{
  int rowid;
  char *name;
  int op_version;
  void *op_params;
  int op_params_size;
  int enabled;
  void *blendop_params;
  int blendop_params_size;
  int blendop_version;
  int writeprotect;
  char *multi_name;
  int multi_name_hand_edited;
  int autoapply;
} dt_presets_params_t;

/** read the module's preset of that name and version into p (name is
    left NULL); FALSE when there is none. Release with
    dt_presets_params_clear() */
gboolean dt_presets_get_params(const char *operation,
                               const int op_version,
                               const char *name,
                               dt_presets_params_t *p);
void dt_presets_params_clear(dt_presets_params_t *p);

/** the name of the module's preset up to |direction| steps after name
    (direction > 0) or before it (direction < 0), in the presets menu
    order; the last one when fewer follow, NULL when none does. An empty
    or NULL name starts from the end of the list. Free with g_free() */
char *dt_presets_adjacent_name(const char *operation,
                               const int op_version,
                               const char *name,
                               const int direction);

/** the preset to auto-apply to the module for the image, among those
    whose filters match it and the workflow preset: user presets before
    built-in ones, the newest first. Read into p (name, op_params and
    blendop_params only); FALSE when there is none */
gboolean dt_presets_find_autoapply(const char *operation,
                                   const int op_version,
                                   const dt_image_t *image,
                                   const gboolean is_display_referred,
                                   const char *workflow_preset,
                                   dt_presets_params_t *p);

typedef struct dt_presets_name_t
{
  char *name;
  gboolean writeprotect;
} dt_presets_name_t;

/** the module's presets, all versions, built-in ones first or last, then
    by name. Free with g_list_free_full(list, dt_presets_name_free) */
GList *dt_presets_list_names(const char *operation,
                             const gboolean writeprotect_first);
void dt_presets_name_free(gpointer data);

/** the names of the module's presets at that version, by name, built-in
    ones first when builtin_first. Free with g_list_free_full(list, g_free) */
GList *dt_presets_version_names(const char *operation,
                                const int op_version,
                                const gboolean builtin_first);

/** the name and module parameters of the module's presets at that
    version, the user's only or all of them.
    Free with g_list_free_full(list, dt_presets_params_free) */
GList *dt_presets_list_params(const char *operation,
                              const int op_version,
                              const gboolean user_only);
void dt_presets_params_free(gpointer data);

/** delete all the module's presets at that version, built-in ones too */
void dt_presets_delete_version(const char *operation, const int op_version);

/** rename the module's preset at that version */
void dt_presets_rename(const char *operation,
                       const int op_version,
                       const char *name,
                       const char *new_name);

/** the name of the auto-applied preset of a utility module matching the
    image: built-in ones first, then by name, descending; NULL when there
    is none. Free with g_free() */
char *dt_presets_find_lib_autoapply(const char *operation,
                                    const int op_version,
                                    const dt_image_t *image);

/** every stored preset of the module, all versions, with rowid, name,
    op_version, op_params, blendop_version and blendop_params.
    Free with g_list_free_full(list, dt_presets_params_free) */
GList *dt_presets_list_stored(const char *operation);

/** the names of the module's presets, built-in ones first, then in the
    order they were stored. Free with g_list_free_full(list, g_free) */
GList *dt_presets_names_by_rowid(const char *operation);

/** set the version of the module's preset of that name, every version */
void dt_presets_set_op_version(const char *operation,
                               const char *name,
                               const int op_version);

/** replace the module parameters, and their version, of the module's
    preset of that name; NULL params for an auto-init preset */
void dt_presets_set_op_params(const char *operation,
                              const char *name,
                              const int op_version,
                              const void *op_params,
                              const int op_params_size);

/** replace the blend parameters, and their version, of the module's
    preset of that name */
void dt_presets_set_blendop_params(const char *operation,
                                   const char *name,
                                   const int blendop_version,
                                   const void *blendop_params,
                                   const int blendop_params_size);

/** delete the module's built-in presets */
void dt_presets_delete_builtin(const char *operation);

/** insert an unfiltered, user preset of a utility module */
void dt_presets_insert_lib_preset(const char *name,
                                  const char *operation,
                                  const int op_version,
                                  const void *op_params,
                                  const int op_params_size);

/** the module parameters of the module's preset of that name, whatever
    its version; FALSE when there is none. Free *op_params with g_free() */
gboolean dt_presets_get_params_any_version(const char *operation,
                                           const char *name,
                                           void **op_params,
                                           int *op_params_size);

/** replace the module parameters of the preset at rowid */
void dt_presets_set_op_params_at(const int rowid,
                                 const void *op_params,
                                 const int op_params_size);

/** delete the preset at rowid, write-protected or not */
void dt_presets_delete_at(const int rowid);

/** built-in presets: dt_gui_presets_add_with_blendop() and the
    dt_gui_presets_update_*() setters of gui/presets.h call these; see there.
    The operation is a module name. */
void dt_presets_update_mml(const char *name,
                           const char *op,
                           const int32_t version,
                           const char *maker,
                           const char *model,
                           const char *lens);

void dt_presets_update_iso(const char *name,
                           const char *op,
                           const int32_t version,
                           const float min,
                           const float max);

void dt_presets_update_av(const char *name,
                          const char *op,
                          const int32_t version,
                          const float min,
                          const float max);

void dt_presets_update_tv(const char *name,
                          const char *op,
                          const int32_t version,
                          const float min,
                          const float max);

void dt_presets_update_fl(const char *name,
                          const char *op,
                          const int32_t version,
                          const float min,
                          const float max);

void dt_presets_update_format(const char *name,
                              const char *op,
                              const int32_t version,
                              const int flag);

void dt_presets_update_autoapply(const char *name,
                                 const char *op,
                                 const int32_t version,
                                 const gboolean autoapply);

void dt_presets_update_filter(const char *name,
                              const char *op,
                              const int32_t version,
                              const int filter);

void dt_presets_add_builtin(const char *name,
                            const char *op,
                            const int32_t version,
                            const void *params,
                            const int32_t params_size,
                            const void *blend_params,
                            const gboolean enabled);

/** delete the built-in presets of every module */
void dt_presets_delete_all_builtin(void);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
