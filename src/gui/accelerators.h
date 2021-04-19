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

void dt_shortcuts_save(gboolean backup);

void dt_shortcuts_load(gboolean clear);

void dt_shortcuts_reinitialise();

void dt_shortcuts_select_view(dt_view_type_flags_t view);

gboolean dt_shortcut_dispatcher(GtkWidget *w, GdkEvent *event, gpointer user_data);

void dt_action_insert_sorted(dt_action_t *owner, dt_action_t *new_action);

dt_action_t *dt_action_locate(dt_action_t *owner, gchar **path);

void dt_action_define_key_pressed_accel(dt_action_t *action, const gchar *name, GtkAccelKey *key);

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

typedef enum dt_shortcut_flag_t
{
  DT_SHORTCUT_LONG   = 1 << 0,
  DT_SHORTCUT_DOUBLE = 1 << 1,
  DT_SHORTCUT_TRIPLE = 1 << 2,
  DT_SHORTCUT_LEFT   = 1 << 0,
  DT_SHORTCUT_MIDDLE = 1 << 1,
  DT_SHORTCUT_RIGHT  = 1 << 2,
  DT_SHORTCUT_DOWN   = 1 << 0,
  DT_SHORTCUT_UP     = 1 << 1,
} dt_shortcut_flag_t;

typedef enum dt_shortcut_move_t
{
  DT_SHORTCUT_MOVE_NONE,
  DT_SHORTCUT_MOVE_SCROLL,
  DT_SHORTCUT_MOVE_PAN,
  DT_SHORTCUT_MOVE_HORIZONTAL,
  DT_SHORTCUT_MOVE_VERTICAL,
  DT_SHORTCUT_MOVE_DIAGONAL,
  DT_SHORTCUT_MOVE_SKEW,
  DT_SHORTCUT_MOVE_LEFTRIGHT,
  DT_SHORTCUT_MOVE_UPDOWN,
  DT_SHORTCUT_MOVE_PGUPDOWN,
} dt_shortcut_move_t;

typedef enum dt_action_element_t
{
  DT_ACTION_ELEMENT_DEFAULT = 0,
} dt_action_element_t;

typedef enum dt_action_effect_t
{
  DT_ACTION_EFFECT_DEFAULT_MOVE = -1,
  DT_ACTION_EFFECT_DEFAULT_KEY = 0,
  DT_ACTION_EFFECT_DEFAULT_UP = 1,
  DT_ACTION_EFFECT_DEFAULT_DOWN = 2,

  // Sliders
  DT_ACTION_EFFECT_EDIT = 0,
  DT_ACTION_EFFECT_UP = 1,
  DT_ACTION_EFFECT_DOWN = 2,
  DT_ACTION_EFFECT_RESET = 3,
  DT_ACTION_EFFECT_TOP = 4,
  DT_ACTION_EFFECT_BOTTOM = 5,

  // Combos
  DT_ACTION_EFFECT_POPUP = 0,
  DT_ACTION_EFFECT_PREVIOUS = 1,
  DT_ACTION_EFFECT_NEXT = 2,
//DT_ACTION_EFFECT_RESET = 3,
  DT_ACTION_EFFECT_FIRST = 4,
  DT_ACTION_EFFECT_LAST = 5,

  // Togglebuttons
  DT_ACTION_EFFECT_TOGGLE = 0,
  DT_ACTION_EFFECT_ON = 1,
  DT_ACTION_EFFECT_OFF = 2,
  DT_ACTION_EFFECT_TOGGLE_CTRL = 3,
  DT_ACTION_EFFECT_ON_CTRL = 4,
  DT_ACTION_EFFECT_TOGGLE_RIGHT = 5,
  DT_ACTION_EFFECT_ON_RIGHT = 6,

  // Buttons
  DT_ACTION_EFFECT_ACTIVATE = 0,
  DT_ACTION_EFFECT_ACTIVATE_CTRL = 1,
  DT_ACTION_EFFECT_ACTIVATE_RIGHT = 2,

  // Lib and iop presets
  DT_ACTION_EFFECT_SHOW = 0,
//DT_ACTION_EFFECT_UP = 1,
//DT_ACTION_EFFECT_DOWN = 2,
  DT_ACTION_EFFECT_STORE = 3,
  DT_ACTION_EFFECT_DELETE = 4,
//DT_ACTION_EFFECT_EDIT = 5,
  DT_ACTION_EFFECT_UPDATE = 6,
  DT_ACTION_EFFECT_PREFERENCES = 7,
} dt_action_effect_t;

extern const gchar *dt_action_effect_value[];
extern const gchar *dt_action_effect_selection[];
extern const gchar *dt_action_effect_toggle[];
extern const gchar *dt_action_effect_activate[];
extern const gchar *dt_action_effect_presets[];

typedef struct dt_action_element_def_t
{
  const gchar *name;
  const gchar **effects;
} dt_action_element_def_t;

typedef struct dt_shortcut_fallback_t
{
  guint mods;
  guint press     : 3;
  guint button    : 3;
  guint click     : 3;
  guint direction : 2;
  dt_shortcut_move_t  move;
  dt_action_element_t element;
  dt_action_effect_t  effect;
  float               speed;
} dt_shortcut_fallback_t;

typedef struct dt_action_def_t
{
  const gchar *name;
  float (*process)(gpointer target, dt_action_element_t, dt_action_effect_t, float size);
  dt_action_element_t (*identify)(GtkWidget *w, int x, int y);
  const dt_action_element_def_t *elements;
  const dt_shortcut_fallback_t *fallbacks;
} dt_action_def_t;

extern const dt_action_def_t dt_action_def_toggle;
extern const dt_action_def_t dt_action_def_value;

void dt_action_define_iop(dt_iop_module_t *self, const gchar *section, const gchar *label, GtkWidget *widget, const dt_action_def_t *action_def);

dt_action_t *dt_action_define(dt_action_t *owner, const gchar *section, const gchar *label, GtkWidget *widget, const dt_action_def_t *action_def);

void dt_action_define_fallback(dt_action_type_t type, const dt_action_def_t *action_def);

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
