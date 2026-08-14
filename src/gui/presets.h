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

#include <sqlite3.h>

// format flags stored into the presets database; the FOR_NOT_
// variants are negated to keep existing presets
typedef enum dt_gui_presets_format_flag_t
{
  FOR_LDR       = 1 << 0,
  FOR_RAW       = 1 << 1,
  FOR_HDR       = 1 << 2,
  FOR_NOT_MONO  = 1 << 3,
  FOR_NOT_COLOR = 1 << 4,
  FOR_MATRIX    = 1 << 5
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
/** the names _have_ to be marked for translation with _("") otherwise use dt_gui_presets_add_with_blendop */
#define dt_gui_presets_add_generic(name, ...) dt_gui_presets_add_generic(N##name, __VA_ARGS__)
void (dt_gui_presets_add_generic)(const char *name,
                                const dt_dev_operation_t op,
                                const int32_t version,
                                const void *params,
                                const int32_t params_size,
                                const gboolean enabled,
                                const dt_develop_blend_colorspace_t blend_cst);
#define BUILTIN_PRESET(name) BUILTIN_PREFIX name
/** same as add_generic but also supply blendop parameters for the presets. */
void dt_gui_presets_add_with_blendop(const char *name,
                                     const dt_dev_operation_t op,
                                     const int32_t version,
                                     const void *params,
                                     const int32_t params_size,
                                     const void *blend_params,
                                     const gboolean enabled);

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
/** update dt_gui_presets_format_flag_t */
void dt_gui_presets_update_format(const char *name,
                                  const dt_dev_operation_t op,
                                  const int32_t version,
                                  const int flag);
/** set auto apply property of preset. */
void dt_gui_presets_update_autoapply(const char *name,
                                     const dt_dev_operation_t op,
                                     const int32_t version,
                                     const gboolean autoapply);
/** set filter mode. if 1, the preset will only show for matching images. */
void dt_gui_presets_update_filter(const char *name,
                                  const dt_dev_operation_t op,
                                  const int32_t version,
                                  const int filter);

/** show the popup menu for the given module, with default behavior. */
GtkMenu *dt_gui_presets_popup_menu_show_for_module(dt_iop_module_t *module);

/** ops for building a preset popup menu (dt_gui_presets_popup_menu_show()).
 *  The SELECT returned by query() must start with the columns
 *  name, op_params, writeprotect, description; extra columns may follow and
 *  are read from the live statement inside the callbacks. */
typedef struct dt_gui_presets_menu_ops_t
{
  gpointer data;                     /* caller context, passed to every callback */
  const gchar *hide_defaults_pref;   /* conf key: which default presets to hide */
  gchar *(*query)(gpointer data);    /* build the SELECT string */
  void (*bind)(sqlite3_stmt *stmt, gpointer data); /* bind the WHERE parameters */
  gboolean (*is_default)(sqlite3_stmt *stmt, gpointer data);   /* dimmed built-in; NULL = none */
  gboolean (*is_disabled)(sqlite3_stmt *stmt, gpointer data);  /* greyed out (wrong op version); NULL = none */
  gboolean (*is_active)(sqlite3_stmt *stmt, gpointer data,
                        gboolean *writeprotect);               /* highlighted current-params match */
  void (*connect_row)(GtkWidget *mi, sqlite3_stmt *stmt, gpointer data); /* wire one preset item */
  int params_size;                 /* 0 disables "store new preset" */
  GCallback manage_cb;               /* "manage presets..." item; NULL = none */
  GCallback edit_cb;                 /* "edit this preset.." */
  GCallback del_cb;                  /* "delete this preset" */
  GCallback store_cb;                /* "store new preset.." */
  GCallback update_cb;               /* "update preset" */
  void (*prefs)(GtkMenu *menu, gpointer data); /* trailing prefs section; NULL = none */
} dt_gui_presets_menu_ops_t;

/** build the shared preset popup menu (the iop darkroom and lib header/
 *  modulegroups presets menus are the same menu).  The caller owns popping
 *  the menu up and any per-menu destroy cleanup; the menu is not popped and
 *  not destroyed here. */
GtkMenu *dt_gui_presets_popup_menu_show(const dt_gui_presets_menu_ops_t *ops);

/** show popupmenu for favorite modules */
void dt_gui_favorite_presets_menu_show(GtkWidget *w);

/** apply a preset to the current module **/
void dt_gui_presets_apply_preset(const gchar* name, dt_iop_module_t *module);

/** apply next or previous preset to the current module **/
void dt_gui_presets_apply_adjacent_preset(dt_iop_module_t *module, const int direction);

/** apply any auto presets that are appropriate for the current module **/
gboolean dt_gui_presets_autoapply_for_module(dt_iop_module_t *module, GtkWidget *widget);

void dt_gui_presets_show_iop_edit_dialog(const char *name_in,
                                         dt_iop_module_t *module,
                                         GCallback final_callback,
                                         gpointer data,
                                         const gboolean allow_name_change,
                                         const gboolean allow_desc_change,
                                         const gboolean allow_remove,
                                         GtkWindow *parent);
void dt_gui_presets_show_edit_dialog(const char *name_in,
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
