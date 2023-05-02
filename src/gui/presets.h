/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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

#include "develop/blend.h"

// format flags stored into the presets database; the FOR_NOT_
// variants are negated to keep existing presets
typedef enum dt_gui_presets_format_flag_t
{
  FOR_LDR       = 1 << 0,
  FOR_RAW       = 1 << 1,
  FOR_HDR       = 1 << 2,
  FOR_NOT_MONO  = 1 << 3,
  FOR_NOT_COLOR = 1 << 4
} dt_gui_presets_format_flag_t;

enum // Lib and iop presets
{
  DT_ACTION_EFFECT_SHOW = DT_ACTION_EFFECT_DEFAULT_KEY,
//DT_ACTION_EFFECT_UP,
//DT_ACTION_EFFECT_DOWN,
  DT_ACTION_EFFECT_STORE = 3,
  DT_ACTION_EFFECT_DELETE = 4,
  DT_ACTION_EFFECT_EDIT = 5,
  DT_ACTION_EFFECT_UPDATE = 6,
  DT_ACTION_EFFECT_PREFERENCES = 7,
};
typedef struct dt_gui_presets_edit_dialog_t
{
  GtkWindow *parent;

  dt_iop_module_t *iop;
  gchar *module_name;
  gchar *operation;
  int op_version;

  GtkEntry *name, *description;
  GtkCheckButton *autoinit, *autoapply, *filter;
  GtkWidget *details;
  GtkWidget *model, *maker, *lens;
  GtkWidget *iso_min, *iso_max;
  GtkWidget *exposure_min, *exposure_max;
  GtkWidget *aperture_min, *aperture_max;
  GtkWidget *focal_length_min, *focal_length_max;
  GtkWidget *dialog, *and_label;
  gchar *original_name;
  gint old_id;
  GtkWidget *format_btn[5];

  GCallback callback;
  gpointer data;
} dt_gui_presets_edit_dialog_t;

#define DT_PRESETS_FOR_NOT (FOR_NOT_MONO | FOR_NOT_COLOR);

/** create a db table with presets for all operations. */
void dt_gui_presets_init();

/** add or replace a generic (i.e. non-exif specific) preset for this operation. */
void dt_gui_presets_add_generic(const char *name,
                                const dt_dev_operation_t op,
                                const int32_t version,
                                const void *params,
                                const int32_t params_size,
                                const int32_t enabled,
                                const dt_develop_blend_colorspace_t blend_cst);

/** same as add_generic but also supply blendop parameters for the presets. */
void dt_gui_presets_add_with_blendop(const char *name,
                                     const dt_dev_operation_t op,
                                     const int32_t version,
                                     const void *params,
                                     const int32_t params_size,
                                     const void *blend_params,
                                     const int32_t enabled);

/** update match strings for maker, model, lens. */
void dt_gui_presets_update_mml(const char *name,
                               const dt_dev_operation_t op,
                               const int32_t version,
                               const char *maker,
                               const char *model,
                               const char *lens);
/** update ranges for iso, aperture, exposure, and focal length, respectively. */
void dt_gui_presets_update_iso(const char *name,
                               const dt_dev_operation_t op,
                               const int32_t version,
                               const float min,
                               const float max);
void dt_gui_presets_update_av(const char *name,
                              const dt_dev_operation_t op,
                              const int32_t version,
                              const float min,
                              const float max);
void dt_gui_presets_update_tv(const char *name,
                              const dt_dev_operation_t op,
                              const int32_t version,
                              const float min,
                              const float max);
void dt_gui_presets_update_fl(const char *name,
                              const dt_dev_operation_t op,
                              const int32_t version,
                              const float min,
                              const float max);
/** update ldr flag: 0-don't care, 1-low dynamic range, 2-raw */
void dt_gui_presets_update_ldr(const char *name,
                               const dt_dev_operation_t op,
                               const int32_t version,
                               const int ldrflag);
/** set auto apply property of preset. */
void dt_gui_presets_update_autoapply(const char *name,
                                     const dt_dev_operation_t op,
                                     const int32_t version,
                                     const int autoapply);
/** set filter mode. if 1, the preset will only show for matching images. */
void dt_gui_presets_update_filter(const char *name,
                                  const dt_dev_operation_t op,
                                  const int32_t version,
                                  const int filter);

/** show a popup menu without initialized module. */
void dt_gui_presets_popup_menu_show_for_params(const dt_dev_operation_t op,
                                               const int32_t version,
                                               void *params,
                                               const int32_t params_size,
                                               void *blendop_params,
                                               const dt_image_t *image,
                                               void (*pick_callback)(GtkMenuItem *, void *),
                                               void *callback_data);

/** show the popup menu for the given module, with default behavior. */
void dt_gui_presets_popup_menu_show_for_module(dt_iop_module_t *module);

/** show popupmenu for favorite modules */
void dt_gui_favorite_presets_menu_show();

/** apply a preset to the current module **/
void dt_gui_presets_apply_preset(const gchar* name, dt_iop_module_t *module);

/** apply next or previous preset to the current module **/
void dt_gui_presets_apply_adjacent_preset(dt_iop_module_t *module, const int direction);

/** apply any auto presets that are appropriate for the current module **/
gboolean dt_gui_presets_autoapply_for_module(dt_iop_module_t *module);

void dt_gui_presets_show_iop_edit_dialog(const char *name_in,
                                         dt_iop_module_t *module,
                                         GCallback final_callback,
                                         gpointer data,
                                         const gboolean allow_name_change,
                                         const gboolean allow_desc_change,
                                         const gboolean allow_remove,
                                         GtkWindow *parent);
void dt_gui_presets_show_edit_dialog(const char *name_in,
                                     const char *module_name,
                                     const int rowid,
                                     GCallback final_callback,
                                     gpointer data,
                                     const gboolean allow_name_change,
                                     const gboolean allow_desc_change,
                                     const gboolean allow_remove,
                                     GtkWindow *parent);

gboolean dt_gui_presets_confirm_and_delete(const char *name,
                                           const char *module_name,
                                           const int rowid);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
