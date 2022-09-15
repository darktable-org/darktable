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

void dt_shortcuts_save(const gchar *ext, const gboolean backup);

void dt_shortcuts_load(const gchar *ext, const gboolean clear);

void dt_shortcuts_reinitialise(dt_action_t *action);

void dt_shortcuts_select_view(dt_view_type_flags_t view);

gboolean dt_shortcut_dispatcher(GtkWidget *w, GdkEvent *event, gpointer user_data);
gboolean dt_shortcut_tooltip_callback(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                                      GtkTooltip *tooltip, gpointer user_data);

float dt_action_process(const gchar *action, int instance, const gchar *element, const gchar *effect, float size);

void dt_action_insert_sorted(dt_action_t *owner, dt_action_t *new_action);

dt_action_t *dt_action_locate(dt_action_t *owner, gchar **path, gboolean create);

static inline dt_action_t *dt_action_section(dt_action_t *owner, const gchar *section)
{
  return dt_action_locate(owner, (gchar **)(const gchar *[]){section, NULL}, TRUE);
}
static inline dt_view_t *dt_action_view(dt_action_t *action)
{
  while(action && action->type != DT_ACTION_TYPE_VIEW) action = action->owner;
  return (dt_view_t *)action;
}
static inline dt_lib_module_t *dt_action_lib(dt_action_t *action)
{
  while(action && action->type != DT_ACTION_TYPE_LIB) action = action->owner;
  return (dt_lib_module_t *)action;
}

void dt_action_define_preset(dt_action_t *action, const gchar *name);
// rename or remove (new_name == NULL) actions or presets
void dt_action_rename_preset(dt_action_t *action, const gchar *old_name, const gchar *new_name);
void dt_action_rename(dt_action_t *action, const gchar *new_name);

typedef uint8_t dt_input_device_t;

// FIXME this could eventually be refactored into dt_input_module_t
// with its own _api.h and loader
typedef struct dt_input_driver_definition_t
{
  gchar *name;
  gchar *(*key_to_string)(const guint key, const gboolean display);
  gboolean (*string_to_key)(const gchar *string, guint *key);
  gchar *(*move_to_string)(const guint move, const gboolean display);
  gboolean (*string_to_move)(const gchar *string, guint *move);
  gboolean (*key_to_move)(dt_lib_module_t *self, const dt_input_device_t id, const guint key, guint *move);
  dt_lib_module_t *module;
} dt_input_driver_definition_t;

dt_input_device_t dt_register_input_driver(dt_lib_module_t *module, const dt_input_driver_definition_t *callbacks);
void dt_shortcut_key_press(dt_input_device_t id, const guint time, const guint key);
void dt_shortcut_key_release(dt_input_device_t id, const guint time, const guint key);
gboolean dt_shortcut_key_active(dt_input_device_t id, const guint key);
float dt_shortcut_move(dt_input_device_t id, const guint time, const guint move, const double size);

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
  DT_SHORTCUT_MOVE_LEFTRIGHT, // FIXME cursor key pairs will be treated as Moves
  DT_SHORTCUT_MOVE_UPDOWN,    // once per-view key_pressed routines that extensively use them
  DT_SHORTCUT_MOVE_PGUPDOWN,  // are ported to the action framework
} dt_shortcut_move_t;

extern const gchar *dt_action_effect_value[];
extern const gchar *dt_action_effect_selection[];
extern const gchar *dt_action_effect_toggle[];
extern const gchar *dt_action_effect_hold[];
extern const gchar *dt_action_effect_activate[];
extern const gchar *dt_action_effect_presets[];

typedef struct dt_action_element_def_t
{
  const gchar *name;
  const gchar **effects;
} dt_action_element_def_t;

extern const dt_action_element_def_t dt_action_elements_hold[];

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

#define DT_VALUE_PATTERN_PLUS_MINUS 2.f
#define DT_VALUE_PATTERN_PERCENTAGE 4.f
#define DT_VALUE_PATTERN_ACTIVE    -1.f/2
#define DT_VALUE_PATTERN_SUM       -1.f/4

typedef struct dt_action_def_t
{
  const gchar *name;
  float (*process)(gpointer target, dt_action_element_t, dt_action_effect_t, float size);
  const dt_action_element_def_t *elements;
  const dt_shortcut_fallback_t *fallbacks;
  const gboolean no_widget;
} dt_action_def_t;

extern const dt_action_def_t dt_action_def_toggle;
extern const dt_action_def_t dt_action_def_button;
extern const dt_action_def_t dt_action_def_value;

dt_action_t *dt_action_define_iop(dt_iop_module_t *self, const gchar *section, const gchar *label, GtkWidget *widget, const dt_action_def_t *action_def);

dt_action_t *dt_action_define(dt_action_t *owner, const gchar *section, const gchar *label, GtkWidget *widget, const dt_action_def_t *action_def);

void dt_action_define_fallback(dt_action_type_t type, const dt_action_def_t *action_def);

typedef enum dt_accel_iop_slider_scale_t
{
  DT_IOP_PRECISION_NORMAL = 0,
  DT_IOP_PRECISION_FINE = 1,
  DT_IOP_PRECISION_COARSE = 2
} dt_accel_iop_slider_scale_t;

typedef void dt_action_callback_t(dt_action_t *action);
dt_action_t *dt_action_register(dt_action_t *owner, const gchar *label, dt_action_callback_t callback, guint accel_key, GdkModifierType mods);
void dt_shortcut_register(dt_action_t *owner, guint element, guint effect, guint accel_key, GdkModifierType mods);

// Accelerator connection functions
void dt_accel_connect_instance_iop(dt_iop_module_t *module);

// Cleanup function
void dt_action_cleanup_instance_iop(dt_iop_module_t *module);

// UX miscellaneous functions
void dt_action_widget_toast(dt_action_t *action, GtkWidget *widget, const gchar *msg, ...);

// check if widget intentionally hidden (to disable it)
gboolean dt_action_widget_invisible(GtkWidget *w);

// Get the speed multiplier for adjusting sliders and other widgets
float dt_accel_get_speed_multiplier(GtkWidget *widget, guint state);

// create a shortcutable button with ellipsized label and tooltip
GtkWidget *dt_action_button_new(dt_lib_module_t *self, const gchar *label, gpointer callback, gpointer data, const gchar *tooltip, guint accel_key, GdkModifierType mods);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

