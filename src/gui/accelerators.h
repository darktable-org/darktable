/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

#include <gtk/gtk.h>

#include "develop/imageop.h"
#include "libs/lib.h"
#include "views/view.h"

typedef struct dt_accel_t
{

  gchar path[256];
  gchar translated_path[256];
  gchar module[256];
  gboolean local;
  dt_view_type_flags_t views;
  GClosure *closure;

} dt_accel_t;

typedef enum dt_accel_iop_slider_scale_t
{
  DT_IOP_PRECISION_NORMAL = 0,
  DT_IOP_PRECISION_FINE = 1,
  DT_IOP_PRECISION_COARSE = 2
} dt_accel_iop_slider_scale_t;

// Accel path string building functions
void dt_accel_path_global(char *s, size_t n, const char *path);
void dt_accel_path_view(char *s, size_t n, char *module, const char *path);
void dt_accel_path_iop(char *s, size_t n, char *module, const char *path);
void dt_accel_path_lib(char *s, size_t n, char *module, const char *path);
void dt_accel_path_lua(char *s, size_t n, const char *path);
void dt_accel_path_manual(char *s, size_t n, const char *full_path);

// Accelerator search functions
dt_accel_t *dt_accel_find_by_path(const gchar *path);

// Accelerator registration functions
void dt_accel_register_global(const gchar *path, guint accel_key, GdkModifierType mods);
void dt_accel_register_view(dt_view_t *self, const gchar *path, guint accel_key, GdkModifierType mods);
void dt_accel_register_iop(dt_iop_module_so_t *so, gboolean local, const gchar *path, guint accel_key,
                           GdkModifierType mods);
void dt_accel_register_lib(dt_lib_module_t *self, const gchar *path, guint accel_key, GdkModifierType mods);
void dt_accel_register_lib_for_views(dt_lib_module_t *self, dt_view_type_flags_t views, const gchar *path,
                                     guint accel_key, GdkModifierType mods);
//register lib shortcut but make it look like a view shortcut
void dt_accel_register_lib_as_view(gchar *view_name, const gchar *path, guint accel_key, GdkModifierType mods);
void dt_accel_register_common_iop(dt_iop_module_so_t *so);
void dt_accel_register_slider_iop(dt_iop_module_so_t *so, gboolean local, const gchar *path);
void dt_accel_register_combobox_iop(dt_iop_module_so_t *so, gboolean local, const gchar *path);
void dt_accel_register_lua(const gchar *path, guint accel_key, GdkModifierType mods);
void dt_accel_register_manual(const gchar *full_path, dt_view_type_flags_t views, guint accel_key,
                              GdkModifierType mods);

// Accelerator connection functions
void dt_accel_connect_global(const gchar *path, GClosure *closure);
void dt_accel_connect_view(dt_view_t *self, const gchar *path, GClosure *closure);
dt_accel_t *dt_accel_connect_iop(dt_iop_module_t *module, const gchar *path, GClosure *closure);
dt_accel_t *dt_accel_connect_lib(dt_lib_module_t *module, const gchar *path, GClosure *closure);
//connect lib as a view shortcut
dt_accel_t *dt_accel_connect_lib_as_view(dt_lib_module_t *module, gchar *view_name, const gchar *path, GClosure *closure);
//connect lib as a global shortcut
dt_accel_t *dt_accel_connect_lib_as_global(dt_lib_module_t *module, const gchar *path, GClosure *closure);
void dt_accel_connect_button_lib_as_global(dt_lib_module_t *module, const gchar *path, GtkWidget *button);
void dt_accel_connect_button_iop(dt_iop_module_t *module, const gchar *path, GtkWidget *button);
void dt_accel_connect_button_lib(dt_lib_module_t *module, const gchar *path, GtkWidget *button);
void dt_accel_connect_slider_iop(dt_iop_module_t *module, const gchar *path, GtkWidget *slider);
void dt_accel_connect_combobox_iop(dt_iop_module_t *module, const gchar *path, GtkWidget *combobox);
void dt_accel_connect_instance_iop(dt_iop_module_t *module);
void dt_accel_connect_locals_iop(dt_iop_module_t *module);
void dt_accel_connect_preset_iop(dt_iop_module_t *so, const gchar *path);
void dt_accel_connect_preset_lib(dt_lib_module_t *so, const gchar *path);
void dt_accel_connect_lua(const gchar *path, GClosure *closure);
void dt_accel_connect_manual(GSList **list_ptr, const gchar *full_path, GClosure *closure);

// Disconnect function
void dt_accel_disconnect_list(GSList **accels_ptr);
void dt_accel_disconnect_locals_iop(dt_iop_module_t *module);
void dt_accel_cleanup_closures_iop(dt_iop_module_t *module);

// Deregister functions
void dt_accel_deregister_iop(dt_iop_module_t *module, const gchar *path);
void dt_accel_deregister_lib(dt_lib_module_t *module, const gchar *path);
void dt_accel_deregister_global(const gchar *path);
void dt_accel_deregister_lua(const gchar *path);
void dt_accel_deregister_manual(GSList *list, const gchar *full_path);
// Rename functions
void dt_accel_rename_preset_iop(dt_iop_module_t *module, const gchar *path, const gchar *new_path);
void dt_accel_rename_preset_lib(dt_lib_module_t *module, const gchar *path, const gchar *new_path);
void dt_accel_rename_global(const gchar *path, const gchar *new_path);
void dt_accel_rename_lua(const gchar *path, const gchar *new_path);

// UX miscellaneous functions
void dt_accel_widget_toast(GtkWidget *widget);

// Get the scale multiplier for adjusting sliders with shortcuts
float dt_accel_get_slider_scale_multiplier();

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
