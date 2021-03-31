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

GtkWidget *dt_shortcuts_prefs(GtkWidget *widget);
GHashTable *dt_shortcut_category_lists(dt_view_type_flags_t v);

void dt_shortcuts_save(const gchar *file_name);

void dt_shortcuts_load(const gchar *file_name);

void dt_shortcuts_reinitialise();

void dt_shortcuts_select_view(dt_view_type_flags_t view);

gboolean dt_shortcut_dispatcher(GtkWidget *w, GdkEvent *event, gpointer user_data);

void dt_action_insert_sorted(dt_action_t *owner, dt_action_t *new_action);

dt_action_t *dt_action_locate(dt_action_t *owner, gchar **path);

void dt_action_define_key_pressed_accel(dt_action_t *action, const gchar *name, GtkAccelKey *key);

void dt_action_define_iop(dt_iop_module_t *self, const gchar *path, gboolean local, guint accel_key, GdkModifierType mods, GtkWidget *widget);

void dt_action_define_preset(dt_action_t *action, const gchar *name);
// delete if new_name == NULL
void dt_action_rename_preset(dt_action_t *action, const gchar *old_name, const gchar *new_name);
void dt_action_rename(dt_action_t *action, const gchar *new_name);

typedef uint8_t dt_input_device_t;

typedef struct dt_input_driver_definition_t
{
  gchar *name;
  gchar *(*key_to_string)(guint key, gboolean display);
  gboolean (*string_to_key)(gchar *string, guint *key);
  gchar *(*move_to_string)(guint move, gboolean display);
  gboolean (*string_to_move)(gchar *string, guint *move);
  dt_lib_module_t *module;
} dt_input_driver_definition_t;

dt_input_device_t dt_register_input_driver(dt_lib_module_t *module, const dt_input_driver_definition_t *callbacks);
void dt_shortcut_key_press(dt_input_device_t id, guint time, guint key, guint mods);
void dt_shortcut_key_release(dt_input_device_t id, guint time, guint key);
float dt_shortcut_move(dt_input_device_t id, guint time, guint move, double size);

typedef enum dt_accel_iop_slider_scale_t
{
  DT_IOP_PRECISION_NORMAL = 0,
  DT_IOP_PRECISION_FINE = 1,
  DT_IOP_PRECISION_COARSE = 2
} dt_accel_iop_slider_scale_t;

// Accelerator registration functions
void dt_accel_register_global(const gchar *path, guint accel_key, GdkModifierType mods);
void dt_accel_register_view(dt_view_t *self, const gchar *path, guint accel_key, GdkModifierType mods);
void dt_accel_register_iop(dt_iop_module_so_t *so, gboolean local, const gchar *path, guint accel_key, GdkModifierType mods);
void dt_accel_register_lib(dt_lib_module_t *self, const gchar *path, guint accel_key, GdkModifierType mods);
//register lib shortcut but make it look like a view shortcut
void dt_accel_register_lib_as_view(gchar *view_name, const gchar *path, guint accel_key, GdkModifierType mods);
void dt_accel_register_common_iop(dt_iop_module_so_t *so);
void dt_accel_register_lua(const gchar *path, guint accel_key, GdkModifierType mods);
void dt_accel_register_manual(const gchar *full_path, dt_view_type_flags_t views, guint accel_key, GdkModifierType mods);

// Accelerator connection functions
void dt_accel_connect_global(const gchar *path, GClosure *closure);
void dt_accel_connect_view(dt_view_t *self, const gchar *path, GClosure *closure);
void dt_accel_connect_iop(dt_iop_module_t *module, const gchar *path, GClosure *closure);
void dt_accel_connect_lib(dt_lib_module_t *module, const gchar *path, GClosure *closure);
//connect lib as a view shortcut
void dt_accel_connect_lib_as_view(dt_lib_module_t *module, gchar *view_name, const gchar *path, GClosure *closure);
//connect lib as a global shortcut
void dt_accel_connect_lib_as_global(dt_lib_module_t *module, const gchar *path, GClosure *closure);
void dt_accel_connect_button_lib_as_global(dt_lib_module_t *module, const gchar *path, GtkWidget *button);
void dt_accel_connect_button_iop(dt_iop_module_t *module, const gchar *path, GtkWidget *button);
void dt_accel_connect_button_lib(dt_lib_module_t *module, const gchar *path, GtkWidget *button);
void dt_accel_connect_instance_iop(dt_iop_module_t *module);
void dt_accel_connect_lua(const gchar *path, GClosure *closure);
void dt_accel_connect_manual(GSList **list_ptr, const gchar *full_path, GClosure *closure);

// Disconnect function
void dt_accel_cleanup_closures_iop(dt_iop_module_t *module); // rename to cleanup instance_list

// Rename/remove functions
void dt_accel_rename_global(const gchar *path, const gchar *new_path);
void dt_accel_rename_lua(const gchar *path, const gchar *new_path);

// UX miscellaneous functions
void dt_accel_widget_toast(GtkWidget *widget);

// Get the scale multiplier for adjusting sliders with shortcuts
float dt_accel_get_slider_scale_multiplier();

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
