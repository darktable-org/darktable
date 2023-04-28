/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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

#include "gui/accelerators.h"
#include "common/action.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/utility.h"
#include "control/control.h"
#include "develop/blend.h"
#include "gui/presets.h"
#include "dtgtk/expander.h"
#include "bauhaus/bauhaus.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <math.h>
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

typedef struct dt_shortcut_t
{
  dt_view_type_flags_t views;

  dt_input_device_t key_device;
  guint key;
  guint mods;
  guint press     : 3;
  guint button    : 3;
  guint click     : 3;
  guint direction : 2;
  dt_input_device_t move_device;
  dt_shortcut_move_t move;

  dt_action_t *action;

  dt_action_element_t element;
  dt_action_effect_t effect;
  float speed;
  int instance; // 0 is from prefs, >0 counting from first, <0 counting from last
} dt_shortcut_t;

const gchar *shortcut_category_label[]
  = { N_("active view"),
      N_("other views"),
      N_("fallbacks"),
      N_("speed") };
#define NUM_CATEGORIES G_N_ELEMENTS(shortcut_category_label)

typedef struct dt_device_key_t
{
  dt_input_device_t key_device;
  guint key;
  const dt_action_def_t *hold_def;
  dt_action_element_t hold_element;
} dt_device_key_t;

#define DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE 0
#define DT_SHORTCUT_DEVICE_TABLET 1

const char *move_string[]
  = { "",
      N_("scroll"),
      N_("pan"),
      N_("horizontal"),
      N_("vertical"),
      N_("diagonal"),
      N_("skew"),
      N_("leftright"),
      N_("updown"),
      N_("pgupdown"),
      NULL };

const struct _modifier_name
{
  GdkModifierType modifier;
  char           *name;
} modifier_string[]
  = { { GDK_SHIFT_MASK  , N_("shift") },
      { GDK_CONTROL_MASK, N_("ctrl" ) },
      { GDK_MOD1_MASK   , N_("alt"  ) },
      { GDK_MOD2_MASK   , N_("cmd"  ) },
      { GDK_MOD5_MASK   , N_("altgr") },
      { 0, NULL } };

static dt_shortcut_t _sc = { 0 };  //  shortcut under construction
static guint _previous_move = DT_SHORTCUT_MOVE_NONE;
static dt_action_t *_selected_action = NULL;
static dt_action_t *_highlighted_action = NULL;

const gchar *dt_action_effect_value[]
  = { N_("edit"),
      N_("up"),
      N_("down"),
      N_("reset"),
      N_("top"),
      N_("bottom"),
      N_("set"),
      NULL };

const gchar *dt_action_effect_selection[]
  = { N_("popup"),
      N_("next"),
      N_("previous"),
      N_("reset"),
      N_("last"),
      N_("first"),
      NULL };

const gchar *dt_action_effect_toggle[]
  = { N_("toggle"),
      N_("on"),
      N_("off"),
      N_("ctrl-toggle"),
      N_("ctrl-on"),
      N_("right-toggle"),
      N_("right-on"),
      NULL };

const gchar *dt_action_effect_hold[]
  = { N_("hold"),
      N_("on"),
      N_("off"),
      N_("toggle"),
      NULL };

const gchar *dt_action_effect_activate[]
  = { N_("activate"),
      N_("ctrl-activate"),
      N_("right-activate"),
      NULL };

const gchar *dt_action_effect_presets[]
  = { N_("show"),
      N_("previous"),
      N_("next"),
      N_("store"),
      N_("delete"),
      N_("edit"),
      N_("update"),
      N_("preferences"),
      NULL };

const gchar *dt_action_effect_preset_iop[]
  = { N_("apply"),
      N_("apply on new instance"),
      NULL };

const gchar *dt_action_effect_entry[]
  = { N_("focus"),
      N_("start"),
      N_("end"),
      N_("clear"),
      NULL };

const dt_action_element_def_t dt_action_elements_hold[]
  = { { NULL, dt_action_effect_hold } };

const dt_action_element_def_t _action_elements_toggle[]
  = { { NULL, dt_action_effect_toggle } };

const dt_action_element_def_t _action_elements_button[]
  = { { NULL, dt_action_effect_activate } };

const dt_action_element_def_t _action_elements_entry[]
  = { { NULL, dt_action_effect_entry } };

const dt_action_element_def_t _action_elements_value_fallback[]
  = { { NULL, dt_action_effect_value } };

static float _action_process_toggle(gpointer target,
                                    dt_action_element_t element,
                                    dt_action_effect_t effect,
                                    float move_size)
{
  float value = gtk_toggle_button_get_active(target);

  if(DT_PERFORM_ACTION(move_size) &&
     !((effect == DT_ACTION_EFFECT_ON
        || effect == DT_ACTION_EFFECT_ON_CTRL
        || effect == DT_ACTION_EFFECT_ON_RIGHT) && value)
     && (effect != DT_ACTION_EFFECT_OFF
         || value))
  {
    GdkEvent *event = gdk_event_new(GDK_BUTTON_PRESS);
    event->button.state = (effect == DT_ACTION_EFFECT_TOGGLE_CTRL
                           || effect == DT_ACTION_EFFECT_ON_CTRL)
                        ? GDK_CONTROL_MASK : 0;
    event->button.button = (effect == DT_ACTION_EFFECT_TOGGLE_RIGHT
                            || effect == DT_ACTION_EFFECT_ON_RIGHT)
                         ? GDK_BUTTON_SECONDARY : GDK_BUTTON_PRIMARY;

    if(!gtk_widget_get_realized(target)) gtk_widget_realize(target);
    event->button.window = gtk_widget_get_window(target);
    g_object_ref(event->button.window);

    // some togglebuttons connect to the clicked signal, others to toggled or button-press-event
    // gtk_widget_event does not work when widgets are hidden in event boxes or some other conditions
    gboolean handled;
    g_signal_emit_by_name(G_OBJECT(target), "button-press-event", event, &handled);
    if(!handled) gtk_button_clicked(GTK_BUTTON(target));
    event->type = GDK_BUTTON_RELEASE;
    g_signal_emit_by_name(G_OBJECT(target), "button-release-event", event, &handled);

    gdk_event_free(event);

    value = gtk_toggle_button_get_active(target);

    if(!gtk_widget_is_visible(target))
      dt_action_widget_toast(NULL, target, value ? _("on") : _("off"));
  }

  return value;
}

static float _action_process_button(gpointer target,
                                    dt_action_element_t element,
                                    dt_action_effect_t effect,
                                    float move_size)
{
  if(!gtk_widget_get_realized(target)) gtk_widget_realize(target);

  if(DT_PERFORM_ACTION(move_size) && gtk_widget_is_sensitive(target))
  {
    if(effect != DT_ACTION_EFFECT_ACTIVATE
      || !g_signal_handler_find(target, G_SIGNAL_MATCH_ID,
                                g_signal_lookup("clicked", gtk_button_get_type()),
                                0, NULL, NULL, NULL)
      || !gtk_widget_activate(GTK_WIDGET(target)))
    {
      GdkEvent *event = gdk_event_new(GDK_BUTTON_PRESS);
      event->button.state = effect == DT_ACTION_EFFECT_ACTIVATE_CTRL
                          ? GDK_CONTROL_MASK : 0;
      event->button.button = effect == DT_ACTION_EFFECT_ACTIVATE_RIGHT
                          ? GDK_BUTTON_SECONDARY : GDK_BUTTON_PRIMARY;

      event->button.window = gtk_widget_get_window(target);
      g_object_ref(event->button.window);

      gtk_widget_event(target, event);
      event->type = GDK_BUTTON_RELEASE;
      gtk_widget_event(target, event);

      gdk_event_free(event);
    }
  }

  return DT_ACTION_NOT_VALID;
}

static const gchar *_entry_set_element = NULL;

static float _action_process_entry(gpointer target,
                                   dt_action_element_t element,
                                   dt_action_effect_t effect,
                                   float move_size)
{
  if(DT_PERFORM_ACTION(move_size))
  {
    switch(effect)
    {
    case DT_ACTION_EFFECT_FOCUS:
      gtk_widget_grab_focus(target);
      break;
    case DT_ACTION_EFFECT_START:
      gtk_widget_grab_focus(target);
      gtk_editable_set_position(target, 0);
      break;
    case DT_ACTION_EFFECT_END:
      gtk_widget_grab_focus(target);
      gtk_editable_set_position(target, -1);
      break;
    case DT_ACTION_EFFECT_CLEAR:
      gtk_editable_delete_text(target, 0, -1);
      break;
    case DT_ACTION_EFFECT_SET:;
      gint position = move_size;
      gtk_editable_insert_text(target, _entry_set_element, -1, &position);
      break;
    }
  }
  else if(effect == DT_ACTION_EFFECT_SET)
    gtk_entry_set_text(target, _entry_set_element);

  return DT_ACTION_NOT_VALID;
}

static const dt_shortcut_fallback_t _action_fallbacks_toggle[]
  = { { .mods = GDK_CONTROL_MASK    , .effect = DT_ACTION_EFFECT_TOGGLE_CTRL  },
      { .button = DT_SHORTCUT_RIGHT , .effect = DT_ACTION_EFFECT_TOGGLE_RIGHT },
      { .press = DT_SHORTCUT_LONG   , .effect = DT_ACTION_EFFECT_TOGGLE_RIGHT },
      { } };

const dt_action_def_t dt_action_def_toggle
  = { N_("toggle"),
      _action_process_toggle,
      _action_elements_toggle,
      _action_fallbacks_toggle };

static const dt_shortcut_fallback_t _action_fallbacks_button[]
  = { { .mods = GDK_CONTROL_MASK    , .effect = DT_ACTION_EFFECT_ACTIVATE_CTRL  },
      { .button = DT_SHORTCUT_RIGHT , .effect = DT_ACTION_EFFECT_ACTIVATE_RIGHT },
      { .press = DT_SHORTCUT_LONG   , .effect = DT_ACTION_EFFECT_ACTIVATE_RIGHT },
      { } };

const dt_action_def_t dt_action_def_button
  = { N_("button"),
      _action_process_button,
      _action_elements_button,
      _action_fallbacks_button };

const dt_action_def_t dt_action_def_entry
  = { N_("entry"),
      _action_process_entry,
      _action_elements_entry };

static const dt_shortcut_fallback_t _action_fallbacks_value[]
  = { { .mods = GDK_CONTROL_MASK                  , .effect = -1, .speed = 0.1 },
      { .mods = GDK_SHIFT_MASK                    , .effect = -1, .speed = 10. },
      { .mods = GDK_CONTROL_MASK | GDK_SHIFT_MASK , .effect = -1, .speed = 10. },
      { .move = DT_SHORTCUT_MOVE_HORIZONTAL       , .effect = -1, .speed = 0.1 },
      { .move = DT_SHORTCUT_MOVE_VERTICAL         , .effect = -1, .speed = 10. },
      { .effect = DT_ACTION_EFFECT_RESET  , .button = DT_SHORTCUT_LEFT, .click = DT_SHORTCUT_DOUBLE },
      { .effect = DT_ACTION_EFFECT_TOP    , .button = DT_SHORTCUT_LEFT, .click = DT_SHORTCUT_DOUBLE, .move = DT_SHORTCUT_MOVE_VERTICAL, .direction = DT_SHORTCUT_UP },
      { .effect = DT_ACTION_EFFECT_BOTTOM , .button = DT_SHORTCUT_LEFT, .click = DT_SHORTCUT_DOUBLE, .move = DT_SHORTCUT_MOVE_VERTICAL, .direction = DT_SHORTCUT_DOWN },
      { } };

const dt_action_def_t dt_action_def_value
  = { N_("value"),
      NULL,
      _action_elements_value_fallback,
      _action_fallbacks_value };

const dt_action_def_t _action_def_dummy
  = { };

static const dt_action_def_t *_action_find_definition(dt_action_t *action)
{
  if(!action) return NULL;

  dt_action_type_t type = action->type != DT_ACTION_TYPE_FALLBACK
                        ? action->type : GPOINTER_TO_INT(action->target);
  const int index = type - DT_ACTION_TYPE_WIDGET - 1;

  if(index >= 0 && index < darktable.control->widget_definitions->len)
    return darktable.control->widget_definitions->pdata[index];
  else if(type == DT_ACTION_TYPE_IOP)
    return &dt_action_def_iop;
  else if(type == DT_ACTION_TYPE_LIB)
    return &dt_action_def_lib;
  else if(type == DT_ACTION_TYPE_VALUE_FALLBACK)
    return &dt_action_def_value;
  else
    return NULL;
}

static const dt_action_element_def_t *_action_find_elements(dt_action_t *action)
{
  const dt_action_def_t *definition = _action_find_definition(action);

  if(!definition)
    return NULL;
  else
    return definition->elements;
}

static const gchar *_action_find_effect_combo(dt_action_t *ac,
                                              const dt_action_element_def_t *el,
                                              dt_action_effect_t ef)
{
  if(el->effects == dt_action_effect_selection && ef > DT_ACTION_EFFECT_COMBO_SEPARATOR)
  {
    dt_introspection_type_enum_tuple_t *values
      = g_hash_table_lookup(darktable.bauhaus->combo_introspection, ac);
    if(values)
    {
      values += ef - DT_ACTION_EFFECT_COMBO_SEPARATOR - 1;
      if(values->description)
        return values->description;
      else
        return values->name; // if not set up by introspection but for example in blend_gui
    }
    else
    {
      gchar **strings
        = g_hash_table_lookup(darktable.bauhaus->combo_list, ac);
      if(strings)
        return strings[ef - DT_ACTION_EFFECT_COMBO_SEPARATOR - 1];
      else
        return _("combo effect not found");
    }
  }

  return NULL;
}

dt_action_t *dt_action_widget(GtkWidget *widget)
{
  return g_hash_table_lookup(darktable.control->widgets, widget);
}

static gboolean _is_kp_key(guint keycode)
{
  return keycode >= GDK_KEY_KP_Space && keycode <= GDK_KEY_KP_Equal;
}

static gboolean _shortcut_is_speed(const dt_shortcut_t *s)
{
  return (!s->key_device && !s->key && !s->press && !s->move_device && !s->move
          && !s->button && !s->click && !s->mods);
}

static gint _shortcut_compare_func(gconstpointer shortcut_a,
                                   gconstpointer shortcut_b,
                                   gpointer user_data)
{
  const dt_shortcut_t *a = (const dt_shortcut_t *)shortcut_a;
  const dt_shortcut_t *b = (const dt_shortcut_t *)shortcut_b;

  const gboolean a_is_speed = _shortcut_is_speed(a);
  const gboolean b_is_speed = _shortcut_is_speed(b);

  dt_view_type_flags_t active_view = GPOINTER_TO_INT(user_data);
  const int a_category = a_is_speed ? -1 : a->views ? a->views & active_view : -2; // put fallbacks last
  const int b_category = b_is_speed ? -1 : b->views ? b->views & active_view : -2; // put fallbacks last

  if(a_category != b_category)
    // reverse order; in current view first, fallbacks and speed last
    return b_category - a_category;
  if(a_is_speed && a->action != b->action)
    //FIXME order by (full) name, but avoid slow full path generation and comparison
    return GPOINTER_TO_INT(a->action) - GPOINTER_TO_INT(b->action);
  if(!a->views && a->action && b->action && a->action->target != b->action->target)
    // order fallbacks by referred type
    return GPOINTER_TO_INT(a->action->target) - GPOINTER_TO_INT(b->action->target);
  if(a->key_device != b->key_device)
    return a->key_device - b->key_device;
  if(a->key != b->key)
    return a->key - b->key;
  if(a->press != b->press)
    return a->press - b->press;
  if(a->button != b->button)
    return a->button - b->button;
  if(a->click != b->click)
    return a->click - b->click;
  if(a->move_device != b->move_device)
    return a->move_device - b->move_device;
  if(a->move != b->move)
    return a->move - b->move;
  if(a->mods != b->mods)
    return a->mods - b->mods;
  if((a->direction | b->direction) == (DT_SHORTCUT_UP | DT_SHORTCUT_DOWN))
    return a->direction - b->direction;
  return 0;
};

static gchar *_action_full_id(dt_action_t *action)
{
  if(action->owner)
  {
    gchar *owner_id = _action_full_id(action->owner);
    gchar *full_label = g_strdup_printf("%s/%s", owner_id, action->id);
    g_free(owner_id);
    return full_label;
  }
  else
    return g_strdup(action->id);
}

static gchar *_action_full_label(dt_action_t *action)
{
  if(action->owner)
  {
    gchar *owner_label = _action_full_label(action->owner);
    gchar *full_label = g_strdup_printf("%s/%s", owner_label, action->label);
    g_free(owner_label);
    return full_label;
  }
  else
    return g_strdup(action->label);
}

static void _action_distinct_label(gchar **label, dt_action_t *action, gchar *instance)
{
  if(!action || action->type <= DT_ACTION_TYPE_GLOBAL)
    return;

  gchar *instance_label = action->type == DT_ACTION_TYPE_IOP && *instance
                        ? g_strdup_printf("%s %s", action->label, instance)
                        : g_strdup(action->label);

  if(*label)
  {
    if(!strstr(action->label, *label) || *instance)
    {
      gchar *distinct_label = g_strdup_printf("%s / %s", instance_label, *label);
      g_free(*label);
      *label = distinct_label;
    }

    g_free(instance_label);
  }
  else
    *label = instance_label;

  _action_distinct_label(label, action->owner, instance);
}

static void _dump_actions(FILE *f, dt_action_t *action)
{
  while(action)
  {
    gchar *label = _action_full_id(action);
    fprintf(f, "%s %s %d\n", label, !action->target ? "*" : "", action->type);
    g_free(label);
    if(action->type <= DT_ACTION_TYPE_SECTION)
      _dump_actions(f, action->target);
    action = action->next;
  }
}

dt_input_device_t dt_register_input_driver(dt_lib_module_t *module,
                                           const dt_input_driver_definition_t *callbacks)
{
  dt_input_device_t id = 10;

  for(GSList *d = darktable.control->input_drivers; d; d = d->next, id += 10)
    if(((dt_input_driver_definition_t *)d->data)->module == module) return id;

  dt_input_driver_definition_t *new_driver = calloc(1, sizeof(dt_input_driver_definition_t));
  *new_driver = *callbacks;
  new_driver->module = module;
  darktable.control->input_drivers
    = g_slist_append(darktable.control->input_drivers, (gpointer)new_driver);

  return id;
}

#define DT_MOVE_NAME -1
static gchar *_shortcut_key_move_name(dt_input_device_t id,
                                      guint key_or_move,
                                      guint mods,
                                      gboolean display)
{
  gchar *name = NULL, *post_name = NULL;
  if(id == DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE)
  {
    if(mods == DT_MOVE_NAME)
      return g_strdup(display && key_or_move != 0
                      ? _(move_string[key_or_move])
                      : move_string[key_or_move]);
    else
    {
      if(display)
      {
        gchar *key_name = gtk_accelerator_get_label(key_or_move, 0);
        post_name = g_utf8_strdown(key_name, -1);
        if(strlen(post_name) == 1 && _is_kp_key(key_or_move))
          post_name = dt_util_dstrcat(post_name, " %s", _("(keypad)"));
        g_free(key_name);
      }
      else
        name = key_or_move ? gtk_accelerator_name(key_or_move, 0) : g_strdup("None");
    }
  }
  else if(id == DT_SHORTCUT_DEVICE_TABLET)
  {
    return g_strdup_printf("%s %u", display ? _("tablet button") : "tablet button", key_or_move);
  }
  else
  {
    GSList *driver = darktable.control->input_drivers;
    while(driver && (id -= 10) >= 10)
      driver = driver->next;

    if(!driver)
      name = g_strdup(_("unknown driver"));
    else
    {
      dt_input_driver_definition_t *callbacks = driver->data;
      gchar *without_device
        = mods == DT_MOVE_NAME
        ? callbacks->move_to_string(key_or_move, display)
        : callbacks->key_to_string(key_or_move, display);

      if(display && id == 0)
        post_name = without_device;
      else
      {
        char id_str[2] = "\0\0";
        if(id) id_str[0] = '0' + id;

        name = g_strdup_printf("%s%s:%s", display ? "" : callbacks->name, id_str, without_device);
        g_free(without_device);
      }
    }
  }
  if(mods != DT_MOVE_NAME)
  {
    for(const struct _modifier_name *mod_str = modifier_string;
        mod_str->modifier;
        mod_str++)
    {
      if(mods & mod_str->modifier)
      {
        gchar *save_name = name;
        name = display
             ? g_strdup_printf("%s%s+", name ? name : "", _(mod_str->name))
             : g_strdup_printf("%s;%s", name ? name : "",   mod_str->name);
        g_free(save_name);
      }
    }
  }

  if(post_name)
  {
    gchar *save_name = name;
    name = g_strdup_printf("%s%s", name ? name : "", post_name);
    g_free(save_name);
    g_free(post_name);
  }

  return name;
}

static gboolean _shortcut_is_move(dt_shortcut_t *s)
{
  return (s->move_device != DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE ||
          s->move != DT_SHORTCUT_MOVE_NONE) && !s->direction;
}

static gchar *_shortcut_description(dt_shortcut_t *s)
{
  static gchar hint[1024];
  int length = 0;

#define add_hint(format, ...) length += length >= sizeof(hint) ? 0 : snprintf(hint + length, sizeof(hint) - length, format, ##__VA_ARGS__)

  gchar *key_name = _shortcut_key_move_name(s->key_device, s->key, s->mods, TRUE);
  gchar *move_name = _shortcut_key_move_name(s->move_device, s->move, DT_MOVE_NAME, TRUE);

  add_hint("%s%s", key_name, s->key_device || s->key ? "" : move_name);

  if(s->press & DT_SHORTCUT_LONG  ) add_hint(" %s", _("long"));
  if(s->press & DT_SHORTCUT_DOUBLE) add_hint(" %s", _("double-press")); else
  if(s->press & DT_SHORTCUT_TRIPLE) add_hint(" %s", _("triple-press")); else
  if(s->press) add_hint(" %s", _("press"));
  if(s->button)
  {
    if(*key_name || *move_name) add_hint(",");
    if(s->button & DT_SHORTCUT_LEFT  ) add_hint(" %s", C_("accel", "left"));
    if(s->button & DT_SHORTCUT_RIGHT ) add_hint(" %s", C_("accel", "right"));
    if(s->button & DT_SHORTCUT_MIDDLE) add_hint(" %s", C_("accel", "middle"));
    if(s->click  & DT_SHORTCUT_LONG  ) add_hint(" %s", C_("accel", "long"));
    if(s->click  & DT_SHORTCUT_DOUBLE) add_hint(" %s", C_("accel", "double-click")); else
    if(s->click  & DT_SHORTCUT_TRIPLE) add_hint(" %s", C_("accel", "triple-click")); else
      add_hint(" %s", _("click"));
  }

  if(*move_name && (s->key_device || s->key))
    add_hint(", %s", move_name);
  if(s->direction)
    add_hint(", %s", s->direction == DT_SHORTCUT_UP ? _("up") : _("down"));

  g_free(key_name);
  g_free(move_name);

  return hint + (hint[0] == ' ' ? 1 : 0);
}

static gchar *_action_description(dt_shortcut_t *s, int components)
{
  static gchar hint[1024];
  int length = 0;
  hint[0] = 0;

  if(components == 2)
  {
    gchar *action_label = _action_full_label(s->action);
    add_hint("%s", action_label);
    g_free(action_label);
  }

  if(s->instance == 1)
    add_hint(", %s", _("first instance"));
  else if(s->instance == -1)
    add_hint(", %s", _("last instance"));
  else if(s->instance != 0)
    add_hint(", %s %+d", _("relative instance"), s->instance);

  const dt_action_def_t *def = _action_find_definition(s->action);

  if(def && def->elements)
  {
    if(components && (s->element || (!def->fallbacks && def->elements->name)))
      add_hint(", %s", _(def->elements[s->element].name));
    const gchar *cef = _action_find_effect_combo(s->action, &def->elements[s->element], s->effect);
    if(cef || s->effect > 0)
      add_hint(", %s", Q_(cef ? cef : def->elements[s->element].effects[s->effect]));
  }

  if(s->speed != 1.0)
    add_hint("%s%s *%g", length ? ", ": "", _("speed"), s->speed);

#undef add_hint

  return hint;
}

static void _insert_shortcut_in_list(GHashTable *ht,
                                     char *shortcut,
                                     dt_action_t *ac,
                                     char *label)
{
  if(ac->owner && ac->owner->owner)
    _insert_shortcut_in_list(ht, shortcut, ac->owner,
                             g_strdup_printf("%s/%s", ac->owner->label, label));
  {
    GtkListStore *list_store = g_hash_table_lookup(ht, ac->owner);
    if(!list_store)
    {
      list_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
      g_hash_table_insert(ht, ac->owner, list_store);
    }

    gtk_list_store_insert_with_values(list_store, NULL, -1, 0, shortcut, 1, label, -1);
  }

  g_free(label);
}

GHashTable *dt_shortcut_category_lists(dt_view_type_flags_t v)
{
  GHashTable *ht = g_hash_table_new(NULL, NULL);

  for(GSequenceIter *iter = g_sequence_get_begin_iter(darktable.control->shortcuts);
      !g_sequence_iter_is_end(iter);
      iter = g_sequence_iter_next(iter))
  {
    dt_shortcut_t *s = g_sequence_get(iter);
    if(s && s->views & v)
      _insert_shortcut_in_list(ht, _shortcut_description(s), s->action, g_strdup_printf("%s%s", s->action->label, _action_description(s, 1)));
  }

  return ht;
}

static gboolean _find_relative_instance(dt_action_t *action,
                                        GtkWidget *widget,
                                        int *instance)
{
  dt_action_t *owner = action;
  while(owner && owner->type != DT_ACTION_TYPE_IOP) owner = owner->owner;

  dt_iop_module_so_t *module = (dt_iop_module_so_t *)owner;
  if(!owner || owner == &darktable.control->actions_focus
     || (module->flags() & IOP_FLAGS_ONE_INSTANCE))
    return FALSE;

  if(!widget || action->target == widget) return TRUE;

  GtkWidget *expander = gtk_widget_get_ancestor(widget, DTGTK_TYPE_EXPANDER);

  dt_iop_module_t *preferred = dt_iop_get_module_preferred_instance(module);

  if(preferred && expander != preferred->expander)
  {
    int current_instance = 0;
    for(GList *iop_mods = darktable.develop->iop;
        iop_mods;
        iop_mods = g_list_next(iop_mods))
    {
      const dt_iop_module_t *mod = (dt_iop_module_t *)iop_mods->data;

      if(mod->so == module && mod->iop_order != INT_MAX)
      {
        current_instance++;

        if(mod->expander == expander)
          *instance = current_instance; // and continue counting
      }
    }

    if(current_instance + 1 - *instance < *instance) *instance -= current_instance + 1;
  }

  return TRUE;
}

static gchar *_shortcut_lua_command(GtkWidget *widget,
                                    dt_shortcut_t *s,
                                    gchar *preset_name)
{
  const dt_action_element_def_t *elements = _action_find_elements(s->action);

  if(!s->action || s->action->owner == &darktable.control->actions_fallbacks
     || !(elements || s->action->type == DT_ACTION_TYPE_COMMAND
          || s->action->type == DT_ACTION_TYPE_PRESET))
    return NULL;

  gchar instance_string[5] = ""; // longest is ", -9"
  if(_find_relative_instance(s->action, widget, &s->instance))
    g_snprintf(instance_string, sizeof(instance_string), ", %d", s->instance);

  int elem = 0;
  while(elements && elements[0].name && elem < s->element && elements[elem + 1].name) elem++;

  if(DT_IS_BAUHAUS_WIDGET(widget) && s->element == DT_ACTION_ELEMENT_DEFAULT)
  {
    if(DT_BAUHAUS_WIDGET(widget)->type == DT_BAUHAUS_COMBOBOX)
    {
      int value = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(widget));
      dt_introspection_type_enum_tuple_t *values
        = g_hash_table_lookup(darktable.bauhaus->combo_introspection, s->action);
      for(int i = 0; values && values->name; values++, i++)
      {
        if(values->value == value)
        {
          value = i;
          break;
        }
      }
      s->effect = DT_ACTION_EFFECT_COMBO_SEPARATOR + 1 + value;
    }
    else
    {
      s->effect = DT_ACTION_EFFECT_SET;
      s->speed = dt_bauhaus_slider_get(widget);
    }
  }

  const gchar *cef = elements ? _action_find_effect_combo(s->action, &elements[elem], s->effect) : NULL;
  const gchar *el = elements ? elements[elem].name : NULL;
  const gchar **ef = elements && s->effect >= 0 ? elements[elem].effects : NULL;
  const gchar *quo = elements ? "\", \"" : "";

  return g_strdup_printf("dt.gui.action(\"%s%s%s%s%s%s\", %.3f%s)\n",
                         _action_full_id(s->action), quo, el ? el : "", quo,
                         cef ? "item:" : "", cef ? NQ_(cef) : ef ? NQ_(ef[s->effect]) : "",
                         s->speed, instance_string);
}

void _shortcut_copy_lua(GtkWidget *widget, dt_shortcut_t *shortcut, gchar *preset_name)
{
  gchar *lua_command = _shortcut_lua_command(widget, shortcut, preset_name);
  if(!lua_command) return;
  gtk_clipboard_set_text(gtk_clipboard_get_default(gdk_display_get_default()), lua_command, -1);
  dt_control_log(_("Lua script command copied to clipboard:\n\n<tt>%s</tt>"), lua_command);
  g_free(lua_command);
}

void dt_shortcut_copy_lua(dt_action_t *action, gchar *preset_name)
{
  GtkWidget *widget = NULL;
  dt_shortcut_t shortcut = { .speed = 1.0 };

  if(!action)
  {
    if(preset_name)
      shortcut.action = dt_action_locate(&darktable.control->actions_global,
                                         (gchar *[]){"styles", (gchar *)preset_name, NULL}, FALSE);
    else
    {
      widget = darktable.control->mapping_widget;
      shortcut.action = dt_action_widget(widget);
      shortcut.element = darktable.control->element;
    }
  }
  else
  {
    if(action->type == DT_ACTION_TYPE_IOP_INSTANCE)
      action = &((dt_iop_module_t*)action)->so->actions;
    shortcut.action = dt_action_locate(action, (gchar *[]){"preset", preset_name, NULL}, FALSE);
  }

  _shortcut_copy_lua(widget, &shortcut, preset_name);
}

static void _tooltip_reposition(GtkWidget *widget,
                                GdkRectangle *allocation,
                                gpointer user_data)
{
  GdkWindow *window = gtk_widget_get_window(gtk_widget_get_toplevel(widget));
  if(!window) return;

  gint wx, wy, width = gdk_window_get_width(window);
  gdk_window_get_origin(window, &wx, &wy);

  GdkRectangle workarea;
  gdk_monitor_get_workarea(gdk_display_get_monitor_at_window(gdk_window_get_display(window),
                                                             window),
                           &workarea);

  wx = CLAMP(wx, workarea.x, workarea.x + workarea.width - width);

  gdk_window_move(window, wx, wy);
}

gboolean dt_shortcut_tooltip_callback(GtkWidget *widget,
                                      gint x,
                                      gint y,
                                      gboolean keyboard_mode,
                                      GtkTooltip *tooltip,
                                      GtkWidget *vbox)
{
  GtkWindow *top = GTK_WINDOW(gtk_widget_get_toplevel(widget));
  if(!gtk_window_is_active(top) && gtk_window_get_window_type(top) != GTK_WINDOW_POPUP)
    return FALSE;

  if(dt_key_modifier_state() & (GDK_BUTTON1_MASK|GDK_BUTTON2_MASK|GDK_BUTTON3_MASK))
    return FALSE;

  gchar *markup_text = NULL;
  gchar *description = NULL;
  dt_action_t *action = NULL;
  dt_action_def_t const *def = NULL;
  int show_element = 0;
  dt_shortcut_t lua_shortcut = { .speed = 1.0 };

  gchar *original_markup = gtk_widget_get_tooltip_markup(widget);
  gchar *preset_name = g_object_get_data(G_OBJECT(widget), "dt-preset-name");
  const gchar *widget_name = gtk_widget_get_name(widget);

  if(!strcmp(widget_name, "actions_view") || !strcmp(widget_name, "shortcuts_view"))
  {
    if(!gtk_widget_is_sensitive(widget)) return FALSE;

    show_element = 1;

    GtkTreePath *path = NULL;
    GtkTreeModel *model;
    GtkTreeIter iter;
    if(!gtk_tree_view_get_tooltip_context(GTK_TREE_VIEW(widget), &x, &y,
                                          keyboard_mode, &model, &path, &iter))
      return FALSE;

    gtk_tree_view_set_tooltip_row(GTK_TREE_VIEW(widget), tooltip, path);
    gtk_tree_path_free(path);


    if(!strcmp(widget_name, "shortcuts_view"))
    {
      GSequenceIter  *shortcut_iter = NULL;
      gtk_tree_model_get(model, &iter, 0, &shortcut_iter, -1);
      markup_text = g_markup_printf_escaped("%s%s%s",
                                            _("start typing for incremental search"),
                                            _highlighted_action ? _("\npress Delete to delete selected shortcut") : "",
                                            (GPOINTER_TO_UINT(shortcut_iter) < NUM_CATEGORIES) ? "" :
                                            _("\ndouble-click to add new shortcut"));

      if(GPOINTER_TO_UINT(shortcut_iter) >= NUM_CATEGORIES)
        lua_shortcut = *(dt_shortcut_t*)g_sequence_get(shortcut_iter);
    }
    else
    {
      gtk_tree_model_get(model, &iter, 0, &action, -1);
      def = _action_find_definition(action);
      markup_text = g_markup_printf_escaped("%s\n%s%s%s%s%s",
                                            _("start typing for incremental search"),
                                            _("click to filter shortcuts list"),
                                            _highlighted_action ?
                                            _("\nright click to show action of selected shortcut")
                                            : "",
                                            def || action->type > DT_ACTION_TYPE_SECTION ?
                                            _("\ndouble-click to define new shortcut")
                                            : "",
                                            def ?
                                            "\n\nmultiple shortcuts can be defined for the same action;"
                                            "\na different element, effect, speed or instance can be set for each in the shortcuts list."
                                            : "",
                                            def && def->fallbacks && action->type != DT_ACTION_TYPE_FALLBACK ?
                                            "\n\nwith fallbacks enabled, the same shortcut can be used with additional modifiers"
                                            "\nor mouse scroll/clicks/moves to affect a different element or change the effect or speed."
                                            : "");
    }
  }
  else if(preset_name)
  {
    dt_action_t *module = g_object_get_data(G_OBJECT(widget), "dt-preset-module");
    if(!module)
    {
      action = dt_action_locate(&darktable.control->actions_global,
                                (gchar *[]){"styles", (gchar *)preset_name, NULL}, FALSE);
    }
    else
    {
      if(module->type == DT_ACTION_TYPE_IOP_INSTANCE)
        module = &((dt_iop_module_t*)module)->so->actions;
      action = dt_action_locate(module, (gchar *[]){"preset", preset_name, NULL}, FALSE);
    }
  }
  else
  {
    if(g_object_get_data(G_OBJECT(widget), "scroll-resize-tooltip"))
      original_markup = dt_util_dstrcat(original_markup, "%s%s",
                                        original_markup ? "\n" : "", _("shift+alt+scroll to change height"));
    action = dt_action_widget(widget);
    if(!action)
    {
      widget = gtk_widget_get_parent(widget);
      action = dt_action_widget(widget);
      show_element = -1; // for notebook tabs
    }

    if(darktable.control->element > 0)
      lua_shortcut.element = darktable.control->element;

    if(darktable.control->mapping_widget == widget)
    {

      const int add_remove_qap = darktable.develop
        ? dt_dev_modulegroups_basics_module_toggle(darktable.develop, widget, FALSE)
        : 0;

      markup_text = g_markup_printf_escaped("%s\n%s\n%s%s\n%s",
                                            _("press keys with mouse click and scroll or move combinations to create a shortcut"),
                                            _("click to open shortcut configuration"),
                                            add_remove_qap > 0 ? _("ctrl+click to add to quick access panel\n") :
                                            add_remove_qap < 0 ? _("ctrl+click to remove from quick access panel\n")  : "",
                                            _("scroll to change default speed"),
                                            _("right click to exit mapping mode"));
    }
  }

  if(!def) def = _action_find_definition(action);
  const gboolean has_fallbacks = def && def->fallbacks;

  const gchar *element_name = NULL;
  if(def)
  {
    for(int i = 0; i <= lua_shortcut.element; i++)
    {
      element_name = def->elements[i].name;
      if(!element_name) break;
    }
    if(element_name
       && (lua_shortcut.element || !has_fallbacks)
       && show_element == 0
       && darktable.control->element != -1)
      description = g_markup_escape_text(_(element_name), -1);
  }

  int num_shortcuts = 0;
  for(GSequenceIter *iter = g_sequence_get_begin_iter(darktable.control->shortcuts);
      !g_sequence_iter_is_end(iter);
      iter = g_sequence_iter_next(iter))
  {
    dt_shortcut_t *s = g_sequence_get(iter);
    if(s->action == action &&
       (!def || darktable.control->element == -1 ||
        s->element == darktable.control->element ||
        (s->element == DT_ACTION_ELEMENT_DEFAULT && has_fallbacks)))
    {
      num_shortcuts++;
      gchar *sc_escaped = g_markup_escape_text(_shortcut_description(s), -1);
      const int components = (show_element > 0 || s->element != darktable.control->element) ? 1 : 0;
      gchar *ac_escaped = g_markup_escape_text(_action_description(s, components), -1);
      description = dt_util_dstrcat(description, "%s<b><big>%s</big></b><i>%s</i>",
                                                 description ? "\n" : "",
                                                 sc_escaped, ac_escaped);
      g_free(sc_escaped);
      g_free(ac_escaped);
    }
  }

  if(!num_shortcuts && original_markup && darktable.control->mapping_widget != widget)
    g_clear_pointer(&description, g_free);

#ifdef USE_LUA
  if(markup_text)
  {
    if(action) lua_shortcut.action = action;
    gchar *lua_command = _shortcut_lua_command(widget, &lua_shortcut, preset_name);
    if(lua_command)
    {
      gchar *lua_escaped = g_markup_printf_escaped("\n\nLua: <tt>%s</tt>%s %s", lua_command,
                                    show_element == 1 ? _("ctrl+v") : _("right long click") , _("to copy Lua command"));
      markup_text = dt_util_dstrcat(markup_text, "%s", lua_escaped);
      g_free(lua_escaped);
      g_free(lua_command);
    }
  }
#endif

  if(description || original_markup || markup_text)
  {
    if(original_markup) markup_text = dt_util_dstrcat(markup_text, markup_text ? "\n\n%s" : "%s", original_markup);
    if(description    ) markup_text = dt_util_dstrcat(markup_text, markup_text ? "\n\n%s" : "%s", description);

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), markup_text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 70);
    gtk_widget_set_halign(label, GTK_ALIGN_START);

    g_free(markup_text);
    g_free(original_markup);
    g_free(description);

    if(vbox)
      gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
    else
      vbox = label;
  }

  if(!vbox) return FALSE;

  gtk_widget_show_all(vbox);
  gtk_tooltip_set_custom(tooltip, vbox);
  g_signal_connect(G_OBJECT(vbox), "size-allocate", G_CALLBACK(_tooltip_reposition), widget);

  return TRUE;
}

static dt_view_type_flags_t _find_views(dt_action_t *action)
{
  dt_view_type_flags_t vws = 0;

  dt_action_t *owner = action;
  while(owner && owner->type >= DT_ACTION_TYPE_SECTION) owner = owner->owner;

  if(owner)

  switch(owner->type)
  {
  case DT_ACTION_TYPE_IOP:
    vws = DT_VIEW_DARKROOM;
    break;
  case DT_ACTION_TYPE_VIEW:;
    dt_view_t *view = (dt_view_t *)owner;
    vws = view->view(view);
    break;
  case DT_ACTION_TYPE_LIB:;
    dt_lib_module_t *lib = (dt_lib_module_t *)owner;
    vws = lib->views(lib);
    break;
  case DT_ACTION_TYPE_BLEND:
    vws = DT_VIEW_DARKROOM;
    break;
  case DT_ACTION_TYPE_CATEGORY:
    if(owner == &darktable.control->actions_fallbacks)
      vws = 0;
    else if(owner == &darktable.control->actions_lua)
      vws = DT_VIEW_ALL;
    else if(owner == &darktable.control->actions_thumb)
    {
      vws = DT_VIEW_DARKROOM | DT_VIEW_MAP | DT_VIEW_TETHERING | DT_VIEW_PRINT;
      if(!g_ascii_strcasecmp(action->id,"rating") || !g_ascii_strcasecmp(action->id,"color label"))
        vws |= DT_VIEW_LIGHTTABLE; // lighttable has copy/paste history shortcuts in separate lib
    }
    else
      dt_print(DT_DEBUG_ALWAYS, "[find_views] views for category '%s' unknown\n", owner->id);
    break;
  case DT_ACTION_TYPE_GLOBAL:
    vws = DT_VIEW_ALL;
    break;
  default:
    break;
  }

  return vws;
}

static GtkTreeStore *_shortcuts_store = NULL;
static GtkTreeStore *_actions_store = NULL;
static GtkWidget *_grab_widget = NULL, *_grab_window = NULL;

static void _shortcuts_store_category(GtkTreeIter *category,
                                      dt_shortcut_t *s,
                                      dt_view_type_flags_t view)
{
  gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(_shortcuts_store), category, NULL,
                                _shortcut_is_speed(s) ? 3 : s && s->views ? s->views & view ? 0 : 1 : 2);
}

static gboolean _remove_shortcut_from_store(GtkTreeModel *model,
                                            GtkTreePath *path,
                                            GtkTreeIter *iter,
                                            gpointer data)
{
  gpointer iter_data;
  gtk_tree_model_get(model, iter, 0, &iter_data, -1);
  if(iter_data == data)
  {
    gtk_tree_store_remove(GTK_TREE_STORE(model), iter);
    return TRUE;
  }

  return FALSE;
}

static void _remove_shortcut(GSequenceIter *shortcut)
{
  if(_shortcuts_store)
    gtk_tree_model_foreach(GTK_TREE_MODEL(_shortcuts_store),
                           _remove_shortcut_from_store, shortcut);

  dt_shortcut_t *s = g_sequence_get(shortcut);
  if(s && s->direction) // was this a split move?
  {
    // unsplit the other half of the move
    s->direction = 0;
    dt_shortcut_t *o = g_sequence_get(g_sequence_iter_prev(shortcut));
    if(g_sequence_iter_is_begin(shortcut)
       || _shortcut_compare_func(s, o, GINT_TO_POINTER(s->views)))
      o = g_sequence_get(g_sequence_iter_next(shortcut));
    o->direction = 0;
  }

  g_sequence_remove(shortcut);
}

static void _add_shortcut(dt_shortcut_t *shortcut, dt_view_type_flags_t view)
{
  GSequenceIter *new_shortcut
    = g_sequence_insert_sorted(darktable.control->shortcuts, shortcut,
                               _shortcut_compare_func, GINT_TO_POINTER(view));

  GtkTreeModel *model = GTK_TREE_MODEL(_shortcuts_store);
  if(model)
  {
    GSequenceIter *prev_shortcut = g_sequence_iter_prev(new_shortcut);
    GSequenceIter *seq_iter = NULL;
    GtkTreeIter category, child;
    _shortcuts_store_category(&category, shortcut, view);

    gint position = 1, found = 0;
    if(gtk_tree_model_iter_children(model, &child, &category))
    do
    {
      gtk_tree_model_get(model, &child, 0, &seq_iter, -1);
      if(seq_iter == prev_shortcut)
      {
        found = position;
        break;
      }
      position++;
    } while(gtk_tree_model_iter_next(model, &child));

    gtk_tree_store_insert_with_values(_shortcuts_store, NULL, &category,
                                      found, 0, new_shortcut, -1);
  }
}

static void _shortcut_row_inserted(GtkTreeModel *tree_model,
                                   GtkTreePath *path,
                                   GtkTreeIter *iter,
                                   gpointer view)
{
  // connect to original store, not filtered one, because otherwise view not sufficiently updated to expand

  GtkTreePath *filter_path
    = gtk_tree_model_filter_convert_child_path_to_path(GTK_TREE_MODEL_FILTER(gtk_tree_view_get_model(view)),
                                                       path);
  if(!filter_path) return;

  gtk_tree_view_expand_to_path(view, filter_path);
  gtk_tree_view_scroll_to_cell(view, filter_path, NULL, TRUE, 0.5, 0);
  gtk_tree_view_set_cursor(view, filter_path, NULL, FALSE);
  gtk_tree_path_free(filter_path);
}

static gboolean _insert_shortcut(dt_shortcut_t *shortcut, gboolean confirm)
{
  if(!shortcut->speed && shortcut->effect != DT_ACTION_EFFECT_SET)
    return FALSE;

  dt_shortcut_t *s = calloc(sizeof(dt_shortcut_t), 1);
  *s = *shortcut;
  s->views = _find_views(s->action);
  dt_view_type_flags_t real_views = s->views;

  const dt_view_t *vw = NULL;
  if(darktable.view_manager)
    vw = dt_view_manager_get_current_view(darktable.view_manager);

  dt_view_type_flags_t view = vw && vw->view ? vw->view(vw) : DT_VIEW_LIGHTTABLE;

  // check (and remove if confirmed) clashes in current and other views
  gboolean remove_existing = !confirm;
  do
  {
    gchar *existing_labels = NULL;
    int active_view = 1;
    do
    {
      GSequenceIter *existing
        = g_sequence_lookup(darktable.control->shortcuts, s, _shortcut_compare_func, GINT_TO_POINTER(view));
      if(existing) // at least one found
      {
        // go to first one that has same shortcut
        while(!g_sequence_iter_is_begin(existing)
              && !_shortcut_compare_func(s, g_sequence_get(g_sequence_iter_prev(existing)),
                                         GINT_TO_POINTER(view)))
          existing = g_sequence_iter_prev(existing);

        do
        {
          GSequenceIter *saved_next = g_sequence_iter_next(existing);

          dt_shortcut_t *e = g_sequence_get(existing);

          if(e->action == s->action)
          {
            if(_shortcut_is_move(e) && e->effect != DT_ACTION_EFFECT_DEFAULT_MOVE)
            {
              if(!confirm ||
                 dt_gui_show_yes_no_dialog(_("shortcut for move exists with single effect"),
                                           _("create separate shortcuts for up and down move?")))
              {
                e->direction = (DT_SHORTCUT_UP | DT_SHORTCUT_DOWN) ^ s->direction;
                if(s->effect == DT_ACTION_EFFECT_DEFAULT_MOVE)
                  s->effect = DT_ACTION_EFFECT_DEFAULT_KEY;
                _add_shortcut(s, view);
                return TRUE;
              }
            }
            else if(_shortcut_is_speed(e))
            {
              // adjust if ui action, overwrite on import
              if(confirm)
                shortcut->speed = s->speed = roundf(s->speed * e->speed * 1000.) / 1000.;
              if(fabsf(s->speed) >= .001 && fabsf(s->speed) <= 1000.)
              {
                _remove_shortcut(existing);
                if(s->speed != 1.0)
                {
                  _add_shortcut(s, view);
                  return TRUE;
                }
                else
                  dt_control_log(_("%s, speed reset"), _action_description(s, 2));
              }
            }
            else if(e->element  != s->element ||
                    e->effect   != s->effect  ||
                    e->speed    != s->speed   ||
                    e->instance != s->instance )
            {
              if(!confirm ||
                 dt_gui_show_yes_no_dialog(_("shortcut exists with different settings"),
                                           _("reset the settings of the shortcut?")))
              {
                _remove_shortcut(existing);
                _add_shortcut(s, view);
                return TRUE;
              }
            }
            else
            {
              // there should be no other clashes because same mapping already existed
              if(confirm &&
                 dt_gui_show_yes_no_dialog(_("shortcut already exists"),
                                           _("remove the shortcut?")))
              {
                _remove_shortcut(existing);
              }
            }
            g_free(s);
            return FALSE;
          }

          if(e->views & real_views) // overlap
          {
            if(remove_existing)
              _remove_shortcut(existing);
            else
            {
              gchar *old_labels = existing_labels;
              existing_labels = g_strdup_printf("%s\n%s",
                                                existing_labels ? existing_labels : "",
                                                _action_description(e, 2));
              g_free(old_labels);
            }
          }

          existing = saved_next;
        } while(!g_sequence_iter_is_end(existing)
                && !_shortcut_compare_func(s, g_sequence_get(existing), GINT_TO_POINTER(view)));
      }

      s->views ^= view; // look in the opposite selection
    } while(active_view--);

    if(existing_labels)
    {
      remove_existing = dt_gui_show_yes_no_dialog(_("clashing shortcuts exist"), "%s\n%s",
                                                  _("remove these existing shortcuts?"),
                                                  existing_labels);
      g_free(existing_labels);

      if(!remove_existing)
      {
        g_free(s);
        return FALSE;
      }
    }
    else
    {
      remove_existing = FALSE;
    }

  } while(remove_existing);

  s->direction = shortcut->direction = 0;
  _add_shortcut(s, view);

  return TRUE;
}

typedef enum
{
  SHORTCUT_VIEW_DESCRIPTION,
  SHORTCUT_VIEW_ACTION,
  SHORTCUT_VIEW_ELEMENT,
  SHORTCUT_VIEW_EFFECT,
  SHORTCUT_VIEW_SPEED,
  SHORTCUT_VIEW_INSTANCE,
  SHORTCUT_VIEW_COLUMNS
} field_id;

#define NUM_INSTANCES 5 // or 3, but change char relative[] = "-2" to "-1"
const gchar *instance_label[/*NUM_INSTANCES*/]
  = { N_("preferred"),
      N_("first"),
      N_("last"),
      N_("second"),
      N_("last but one") };

static void _fill_shortcut_fields(GtkTreeViewColumn *column,
                                  GtkCellRenderer *cell,
                                  GtkTreeModel *model,
                                  GtkTreeIter *iter,
                                  gpointer data)
{
  void *data_ptr = NULL;
  gtk_tree_model_get(model, iter, 0, &data_ptr, -1);
  const field_id field = GPOINTER_TO_INT(data);
  gchar *field_text = NULL;
  gboolean editable = FALSE;
  PangoUnderline underline = PANGO_UNDERLINE_NONE;
  int weight = PANGO_WEIGHT_NORMAL;

  if(GPOINTER_TO_UINT(data_ptr) < NUM_CATEGORIES)
  {
    if(field == SHORTCUT_VIEW_DESCRIPTION)
      field_text = g_strdup(_(shortcut_category_label[GPOINTER_TO_INT(data_ptr)]));
  }
  else
  {
    const dt_action_element_def_t *elements = NULL;
    dt_shortcut_t *s = g_sequence_get(data_ptr);

    dt_action_t *owner = s->action;
    while(owner && owner->type >= DT_ACTION_TYPE_SECTION) owner = owner->owner;

    switch(field)
    {
    case SHORTCUT_VIEW_DESCRIPTION:
      field_text = g_strdup(_shortcut_description(s));
      break;
    case SHORTCUT_VIEW_ACTION:
      if(s->action)
        field_text = _action_full_label(s->action);
      break;
    case SHORTCUT_VIEW_ELEMENT:
      if(owner == &darktable.control->actions_lua || _shortcut_is_speed(s)) break;
      elements = _action_find_elements(s->action);
      if(elements && elements->name)
      {
        if(s->element || s->action->type != DT_ACTION_TYPE_FALLBACK)
          field_text = g_strdup(_(elements[s->element].name));
        if(s->element == 0) weight = PANGO_WEIGHT_LIGHT;
        editable = TRUE;
      }
      break;
    case SHORTCUT_VIEW_EFFECT:
      if(owner == &darktable.control->actions_lua || _shortcut_is_speed(s)) break;
      elements = _action_find_elements(s->action);
      if(elements)
      {
        const gchar *cef = _action_find_effect_combo(s->action, &elements[s->element], s->effect);
        if(cef || s->effect > 0 || (s->effect == 0 && s->action->type != DT_ACTION_TYPE_FALLBACK))
          field_text = g_strdup(Q_(cef ? cef : elements[s->element].effects[s->effect]));
        if(s->effect == 0) weight = PANGO_WEIGHT_LIGHT;
        editable = TRUE;
      }
      break;
    case SHORTCUT_VIEW_SPEED:
      elements = _action_find_elements(s->action);
      if(s->speed != 1.0
         || (elements && elements[s->element].effects == dt_action_effect_value
             && (s->effect == DT_ACTION_EFFECT_DEFAULT_MOVE
                 || s->effect == DT_ACTION_EFFECT_DEFAULT_KEY
                 || s->effect == DT_ACTION_EFFECT_DEFAULT_UP
                 || s->effect == DT_ACTION_EFFECT_DEFAULT_DOWN
                 || s->effect == DT_ACTION_EFFECT_SET
                 || (!s->effect && s->action->type == DT_ACTION_TYPE_FALLBACK))))
      {
        field_text = g_strdup_printf("%.3f", s->speed);
        if(s->speed == 1.0) weight = PANGO_WEIGHT_LIGHT;
      }
      editable = TRUE;
      break;
    case SHORTCUT_VIEW_INSTANCE:
      if(_shortcut_is_speed(s)) break;
      for(; owner; owner = owner->owner)
      {
        if(owner->type == DT_ACTION_TYPE_IOP)
        {
          dt_iop_module_so_t *iop = (dt_iop_module_so_t *)owner;

          if(owner != &darktable.control->actions_focus && !(iop->flags() & IOP_FLAGS_ONE_INSTANCE))
          {
            field_text = abs(s->instance) <= (NUM_INSTANCES - 1) /2
                       ? g_strdup(_(instance_label[abs(s->instance)*2 - (s->instance > 0)]))
                       : g_strdup_printf("%+d", s->instance);
            if(s->instance == 0) weight = PANGO_WEIGHT_LIGHT;
            editable = TRUE;
          }
          break;
        }
      }
      break;
    default:
      break;
    }
  }
  g_object_set(cell, "text", field_text, "editable", editable, "underline", underline, "weight", weight, NULL);
  g_free(field_text);
}

static void _add_prefs_column(GtkTreeView *tree,
                              GtkCellRenderer *renderer,
                              char *name,
                              int position)
{
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(name, renderer, NULL);
  gtk_tree_view_column_set_cell_data_func(column, renderer,
                                          _fill_shortcut_fields, GINT_TO_POINTER(position), NULL);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_append_column(tree, column);
}

static dt_shortcut_t *_find_edited_shortcut(GtkTreeModel *model, const gchar *path_string)
{
  GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
  GtkTreeIter iter;
  gtk_tree_model_get_iter(model, &iter, path);
  gtk_tree_path_free(path);

  void *data_ptr = NULL;
  gtk_tree_model_get(model, &iter, 0, &data_ptr, -1);

  return g_sequence_get(data_ptr);
}

static void _element_editing_started(GtkCellRenderer *renderer,
                                     GtkCellEditable *editable,
                                     char *path,
                                     gpointer data)
{
  dt_shortcut_t *s = _find_edited_shortcut(data, path);

  GtkComboBox *combo_box = GTK_COMBO_BOX(editable);
  GtkListStore *store = GTK_LIST_STORE(gtk_combo_box_get_model(combo_box));
  gtk_list_store_clear(store);

  int show_all = s->action->type != DT_ACTION_TYPE_FALLBACK;
  for(const dt_action_element_def_t *element = _action_find_elements(s->action);
      element && element->name ;
      element++)
    gtk_list_store_insert_with_values(store, NULL, -1, 0,
                                      show_all++ ? _(element->name) : _("(unchanged)"), -1);

  gtk_combo_box_set_active(combo_box, s->element);
}

static void _element_changed(GtkCellRendererCombo *combo,
                             char *path_string,
                             GtkTreeIter *new_iter,
                             gpointer data)
{
  dt_shortcut_t *s = _find_edited_shortcut(data, path_string);

  GtkTreeModel *model = NULL;
  g_object_get(combo, "model", &model, NULL);
  GtkTreePath *path = gtk_tree_model_get_path(model, new_iter);
  const gint new_index = gtk_tree_path_get_indices(path)[0];
  gtk_tree_path_free(path);

  const dt_action_element_def_t *elements = _action_find_elements(s->action);
  if(elements[s->element].effects != elements[new_index].effects)
  {
    s->effect = _shortcut_is_move(s) ? DT_ACTION_EFFECT_DEFAULT_MOVE : DT_ACTION_EFFECT_DEFAULT_KEY;
  }
  s->element = new_index;

  dt_shortcuts_save(NULL, FALSE);
}

enum
{
  DT_ACTION_EFFECT_COLUMN_NAME,
  DT_ACTION_EFFECT_COLUMN_SEPARATOR,
  DT_ACTION_EFFECT_COLUMN_WEIGHT,
};

static gboolean _effects_separator_func(GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
  gboolean is_separator;
  gtk_tree_model_get(model, iter, DT_ACTION_EFFECT_COLUMN_SEPARATOR, &is_separator, -1);
  return is_separator;
}

static void _effect_editing_started(GtkCellRenderer *renderer,
                                    GtkCellEditable *editable,
                                    char *path,
                                    gpointer data)
{
  dt_shortcut_t *s = _find_edited_shortcut(data, path);

  GtkComboBox *combo_box = GTK_COMBO_BOX(editable);
  GtkListStore *store = GTK_LIST_STORE(gtk_combo_box_get_model(combo_box));
  gtk_list_store_clear(store);

  const dt_action_element_def_t *elements = _action_find_elements(s->action);
  int show_all = s->action->type != DT_ACTION_TYPE_FALLBACK;
  int bold_move = _shortcut_is_move(s) ? DT_ACTION_EFFECT_DEFAULT_KEY : DT_ACTION_EFFECT_DEFAULT_DOWN + 1;
  if(elements)
    for(const gchar **effect = elements[s->element].effects; *effect ; effect++, bold_move++)
    {
      gtk_list_store_insert_with_values(store, NULL, -1,
                                        DT_ACTION_EFFECT_COLUMN_NAME, show_all++ ? Q_(*effect) : _("(unchanged)"),
                                        DT_ACTION_EFFECT_COLUMN_WEIGHT, bold_move >  DT_ACTION_EFFECT_DEFAULT_KEY
                                                                     && bold_move <= DT_ACTION_EFFECT_DEFAULT_DOWN
                                                                      ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                                        -1);
    }

  GList *cell = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(combo_box));
  gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(combo_box), cell->data,
                                "weight", DT_ACTION_EFFECT_COLUMN_WEIGHT);
  g_list_free(cell);

  if(elements && elements[s->element].effects == dt_action_effect_selection)
  {
    gtk_combo_box_set_row_separator_func(combo_box, _effects_separator_func, NULL, NULL);

    dt_introspection_type_enum_tuple_t *values
      = g_hash_table_lookup(darktable.bauhaus->combo_introspection, s->action);
    if(values)
    {
      // insert empty/separator row
      gtk_list_store_insert_with_values(store, NULL, -1, DT_ACTION_EFFECT_COLUMN_SEPARATOR, TRUE, -1);

      for(; values->name; values++)
      {
        gtk_list_store_insert_with_values(store, NULL, -1,
                                          DT_ACTION_EFFECT_COLUMN_NAME, Q_(values->description ? values->description : values->name),
                                          DT_ACTION_EFFECT_COLUMN_WEIGHT, PANGO_WEIGHT_NORMAL,
                                          -1);
      }
    }
    else
    {
      gchar **strings
        = g_hash_table_lookup(darktable.bauhaus->combo_list, s->action);
      if(strings)
      {
        // insert empty/separator row
        gtk_list_store_insert_with_values(store, NULL, -1, DT_ACTION_EFFECT_COLUMN_SEPARATOR, TRUE, -1);

        while(*strings)
        {
          gtk_list_store_insert_with_values(store, NULL, -1,
                                            DT_ACTION_EFFECT_COLUMN_NAME, Q_(*(strings++)),
                                            DT_ACTION_EFFECT_COLUMN_WEIGHT, PANGO_WEIGHT_NORMAL,
                                            -1);
        }
      }
    }
  }

  gtk_combo_box_set_active(combo_box, s->effect == -1 ? 1 : s->effect);
}

static void _effect_changed(GtkCellRendererCombo *combo,
                            char *path_string,
                            GtkTreeIter *new_iter,
                            gpointer data)
{
  dt_shortcut_t *s = _find_edited_shortcut(data, path_string);

  GtkTreeModel *model = NULL;
  g_object_get(combo, "model", &model, NULL);
  GtkTreePath *path = gtk_tree_model_get_path(model, new_iter);
  const gint new_index = s->effect = gtk_tree_path_get_indices(path)[0];
  gtk_tree_path_free(path);

  if(_shortcut_is_move(s) &&
     (new_index == DT_ACTION_EFFECT_DEFAULT_UP || new_index == DT_ACTION_EFFECT_DEFAULT_DOWN))
    s->effect = DT_ACTION_EFFECT_DEFAULT_MOVE;
  else
    s->effect = new_index;

  dt_shortcuts_save(NULL, FALSE);
}

static void _speed_edited(GtkCellRendererText *cell,
                          const gchar *path_string,
                          const gchar *new_text,
                          gpointer data)
{
  _find_edited_shortcut(data, path_string)->speed = atof(new_text);

  dt_shortcuts_save(NULL, FALSE);
}

static void _instance_edited(GtkCellRendererText *cell,
                             const gchar *path_string,
                             const gchar *new_text,
                             gpointer data)
{
  dt_shortcut_t *s = _find_edited_shortcut(data, path_string);

  if(!(s->instance = atoi(new_text)))
    for(int i = 0; i < NUM_INSTANCES; i++)
      if(!strcmp(instance_label[i], new_text))
        s->instance = (i + 1) / 2 * (i % 2 ? 1 : -1);

  dt_shortcuts_save(NULL, FALSE);
}

static void _grab_in_tree_view(GtkTreeView *tree_view)
{
  g_set_weak_pointer(&_grab_widget, gtk_widget_get_parent(gtk_widget_get_parent(GTK_WIDGET(tree_view)))); // static
  gtk_widget_set_sensitive(_grab_widget, FALSE);
  gtk_widget_set_tooltip_text(_grab_widget,
                              _("define a shortcut by pressing a key, optionally combined with modifier keys (ctrl/shift/alt)\n"
                                "a key can be double or triple pressed, with a long last press\n"
                                "while the key is held, a combination of mouse buttons can be (double/triple/long) clicked\n"
                                "still holding the key (and modifiers and/or buttons) a scroll or mouse move can be added\n"
                                "connected devices can send keys or moves using their physical controllers\n\n"
                                "right-click to cancel"));
  g_set_weak_pointer(&_grab_window, gtk_widget_get_toplevel(_grab_widget));
  if(_sc.action && _sc.action->type == DT_ACTION_TYPE_FALLBACK)
    dt_shortcut_key_press(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, 0, 0);
  g_signal_connect(_grab_window, "event", G_CALLBACK(dt_shortcut_dispatcher), NULL);
}

static void _shortcut_row_activated(GtkTreeView *tree_view,
                                    GtkTreePath *path,
                                    GtkTreeViewColumn *column,
                                    gpointer user_data)
{
  GtkTreeIter iter;
  gtk_tree_model_get_iter(GTK_TREE_MODEL(user_data), &iter, path);

  GSequenceIter  *shortcut_iter = NULL;
  gtk_tree_model_get(GTK_TREE_MODEL(user_data), &iter, 0, &shortcut_iter, -1);

  if(GPOINTER_TO_UINT(shortcut_iter) < NUM_CATEGORIES) return;

  dt_shortcut_t *s = g_sequence_get(shortcut_iter);
  _sc.action = s->action;
  _sc.element = s->element;
  _sc.instance = s->instance;

  _grab_in_tree_view(tree_view);
}

static gboolean _view_key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  GtkTreeView *view = GTK_TREE_VIEW(widget);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(view);

  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    if(!strcmp(gtk_widget_get_name(widget), "actions_view"))
    {
      // if control key pressed, copy lua command to clipboard (CTRL+C will work)
      if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
      {
        dt_shortcut_t shortcut = { .speed = 1.0 };
        gtk_tree_model_get(model, &iter, 0, &shortcut.action, -1);

        _shortcut_copy_lua(NULL, &shortcut, NULL);
      }
    }
    else
    {
      GSequenceIter  *shortcut_iter = NULL;
      gtk_tree_model_get(model, &iter, 0, &shortcut_iter, -1);

      if(GPOINTER_TO_UINT(shortcut_iter) >= NUM_CATEGORIES)
      {
        dt_shortcut_t *s = g_sequence_get(shortcut_iter);

        // if control key pressed, copy lua command to clipboard (CTRL+C will work)
        if(dt_modifier_is(event->state, GDK_CONTROL_MASK) && s->views)
        {
          _shortcut_copy_lua(NULL, s, NULL);
        }

        // GDK_KEY_BackSpace moves to parent in tree
        if(event->keyval == GDK_KEY_Delete || event->keyval == GDK_KEY_KP_Delete)
        {
          if(dt_gui_show_yes_no_dialog(_("removing shortcut"),
                                       _("remove the selected shortcut?")))
          {
            _remove_shortcut(shortcut_iter);

            dt_shortcuts_save(NULL, FALSE);
          }

          return TRUE;
        }
      }
    }
  }

  return dt_gui_search_start(widget, event, user_data);
}

static void _add_shortcuts_to_tree()
{
  const dt_view_t *vw = dt_view_manager_get_current_view(darktable.view_manager);
  dt_view_type_flags_t view = vw && vw->view ? vw->view(vw) : DT_VIEW_LIGHTTABLE;

  for(gint i = 0; i < NUM_CATEGORIES; i++)
    gtk_tree_store_insert_with_values(_shortcuts_store, NULL, NULL, -1, 0, GINT_TO_POINTER(i), -1);

  for(GSequenceIter *iter = g_sequence_get_begin_iter(darktable.control->shortcuts);
      !g_sequence_iter_is_end(iter);
      iter = g_sequence_iter_next(iter))
  {
    dt_shortcut_t *s = g_sequence_get(iter);
    GtkTreeIter category;
    _shortcuts_store_category(&category, s, view);

    gtk_tree_store_insert_with_values(_shortcuts_store, NULL, &category, -1, 0, iter, -1);
  }
}

static gboolean _add_actions_to_tree(GtkTreeIter *parent,
                                     dt_action_t *action,
                                     dt_action_t *find,
                                     GtkTreeIter *found)
{
  gboolean any_leaves = FALSE;

  GtkTreeIter iter;
  for(; action; action = action->next)
  {
    if(action->type == DT_ACTION_TYPE_IOP)
    {
      const dt_iop_module_so_t *module = (dt_iop_module_so_t *)action;
      if(action != &darktable.control->actions_focus
         && module->flags() & (IOP_FLAGS_HIDDEN | IOP_FLAGS_DEPRECATED))
        continue;
    }

    gboolean module_is_needed = FALSE;
    if(action->type == DT_ACTION_TYPE_LIB)
    {
      dt_lib_module_t *module = (dt_lib_module_t *)action;
      module_is_needed = module->gui_reset || module->get_params || module->expandable(module);
    }

    gtk_tree_store_insert_with_values(_actions_store, &iter, parent, -1, 0, action, -1);

    if(action->type <= DT_ACTION_TYPE_SECTION &&
       !_add_actions_to_tree(&iter, action->target, find, found) &&
       !module_is_needed)
      gtk_tree_store_remove(_actions_store, &iter);
    else
    {
      any_leaves = TRUE;
      if(action == find) *found = iter;
    }
  }

  return any_leaves;
}

static void _fill_action_fields(GtkTreeViewColumn *column,
                                GtkCellRenderer *cell,
                                GtkTreeModel *model,
                                GtkTreeIter *iter,
                                gpointer data)
{
  dt_action_t *action = NULL;
  gtk_tree_model_get(model, iter, 0, &action, -1);
  gchar const *text = action->label;
  if(!data)
  {
    const dt_action_def_t *def = _action_find_definition(action);
    text = def ? _(def->name) : "";
  }

  int weight = PANGO_WEIGHT_NORMAL;

  for(dt_action_t *ac = _highlighted_action; ac; ac = ac->owner)
  {
    if(ac == action)
    {
      weight = PANGO_WEIGHT_BOLD;
      break;
    }
  }

  g_object_set(cell, "text", text, "weight", weight, NULL);
}

static void _action_row_activated(GtkTreeView *tree_view,
                                  GtkTreePath *path,
                                  GtkTreeViewColumn *column,
                                  gpointer user_data)
{
  GtkTreeIter iter;
  gtk_tree_model_get_iter(GTK_TREE_MODEL(user_data), &iter, path);

  gtk_tree_model_get(GTK_TREE_MODEL(user_data), &iter, 0, &_sc.action, -1);
  _sc.element = DT_ACTION_ELEMENT_DEFAULT;
  _sc.instance = 0;

  if(_sc.action->type > DT_ACTION_TYPE_SECTION || _action_find_definition(_sc.action))
    _grab_in_tree_view(tree_view);
  else
    _sc.action = NULL;
}

static gboolean _shortcut_selection_function(GtkTreeSelection *selection,
                                             GtkTreeModel *model,
                                             GtkTreePath *path,
                                             gboolean path_currently_selected,
                                             gpointer data)
{
  GtkTreeIter iter;
  gtk_tree_model_get_iter(model, &iter, path);

  void *data_ptr = NULL;
  gtk_tree_model_get(model, &iter, 0, &data_ptr, -1);

  if(GPOINTER_TO_UINT(data_ptr) < NUM_CATEGORIES)
  {
    GtkTreeView *view = gtk_tree_selection_get_tree_view(selection);
    if(gtk_tree_view_row_expanded(view, path))
      gtk_tree_view_collapse_row(view, path);
    else
      gtk_tree_view_expand_row(view, path, FALSE);

    return FALSE;
  }
  else
    return TRUE;
}

static void _shortcut_selection_changed(GtkTreeSelection *selection, gpointer data)
{
  GtkTreeModel *model = NULL;
  GtkTreeIter iter;

  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    void *data_ptr = NULL;
    gtk_tree_model_get(model, &iter, 0, &data_ptr, -1);
    dt_shortcut_t *selected_shortcut = g_sequence_get(data_ptr);
    _highlighted_action = selected_shortcut->action;
  }
  else
    _highlighted_action = NULL;

  gtk_widget_queue_draw(GTK_WIDGET(data));
}

static gboolean _action_find_and_expand(GtkTreeModel *model,
                                        GtkTreeIter *iter,
                                        GtkTreeView *view)
{
  do
  {
    dt_action_t *current_action = NULL;
    gtk_tree_model_get(model, iter, 0, &current_action, -1);

    if(current_action == _highlighted_action)
    {
      GtkTreePath *path = gtk_tree_model_get_path(model, iter);
      gtk_tree_view_expand_to_path(view, path);
      gtk_tree_view_scroll_to_cell(view, path, NULL, TRUE, 0.5, 0);
      gtk_tree_path_free(path);

      return TRUE;
    }

    GtkTreeIter child;
    if(gtk_tree_model_iter_children(model, &child, iter)
       && _action_find_and_expand(model, &child, view))
    {
      return TRUE;
    }
  } while(gtk_tree_model_iter_next(model, iter));

  return FALSE;
}

static gboolean _action_view_click(GtkWidget *widget,
                                   GdkEventButton *event,
                                   gpointer data)
{
  GtkTreeView *view = GTK_TREE_VIEW(widget);
  GtkTreeModel *model = gtk_tree_view_get_model(view);

  if(event->button == GDK_BUTTON_PRIMARY)
  {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(view);

    GtkTreePath *path = NULL;
    if(gtk_tree_view_get_path_at_pos(view, (gint)event->x, (gint)event->y,
                                     &path, NULL, NULL, NULL))
    {
      if(event->type == GDK_DOUBLE_BUTTON_PRESS)
      {
        gtk_tree_selection_select_path(selection, path);
        _action_row_activated(view, path, NULL, model);
      }
      else if(gtk_tree_selection_path_is_selected(selection, path))
      {
        gtk_tree_selection_unselect_path(selection, path);
        gtk_tree_view_collapse_row(view, path);
      }
      else
      {
        gtk_tree_selection_select_path(selection, path);
        gtk_tree_view_set_cursor(view, path, NULL, FALSE);
      }

      gtk_widget_grab_focus(widget);
    }
    else
      gtk_tree_selection_unselect_all(selection);
  }
  else if(event->button == GDK_BUTTON_SECONDARY)
  {
    GtkTreeIter iter;
    gtk_tree_model_get_iter_first(model, &iter);

    _action_find_and_expand(model, &iter, view);
  }

  return TRUE;
}

static gboolean _action_view_map(GtkTreeView *view, GdkEvent *event, gpointer found_iter)
{
  GtkTreePath *path = gtk_tree_model_get_path(gtk_tree_view_get_model(view), found_iter);
  gtk_tree_view_expand_to_path(view, path);
  gtk_tree_view_scroll_to_cell(view, path, NULL, TRUE, 0.5, 0);
  gtk_tree_view_set_cursor(view, path, NULL, FALSE);
  gtk_tree_path_free(path);

  gtk_tree_selection_select_iter(gtk_tree_view_get_selection(view), found_iter);

  return FALSE;
}

static void _action_selection_changed(GtkTreeSelection *selection, gpointer data)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;

  if(!gtk_tree_selection_get_selected(selection, &model, &iter))
    _selected_action = NULL;
  else
  {
    gtk_tree_model_get(model, &iter, 0, &_selected_action, -1);

    GtkTreeView *view = gtk_tree_selection_get_tree_view(selection);
    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
    gtk_tree_view_expand_row(view, path, FALSE);
    gtk_tree_path_free(path);
  }

  GtkTreeView *shortcuts_view = GTK_TREE_VIEW(data);
  gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(gtk_tree_view_get_model(shortcuts_view)));
  gtk_tree_view_expand_all(shortcuts_view);
}

static gboolean _search_func(GtkTreeModel *model,
                             gint column,
                             const gchar *key,
                             GtkTreeIter *iter,
                             gpointer search_data)
{
  gchar *key_case = g_utf8_casefold(key, -1), *label_case = NULL;
  if(column == 1)
  {
    dt_action_t *action = NULL;
    gtk_tree_model_get(model, iter, 0, &action, -1);
    label_case = g_utf8_casefold(action->label, -1);
  }
  else
  {
    GSequenceIter *seq_iter = NULL;
    gtk_tree_model_get(model, iter, 0, &seq_iter, -1);
    if(GPOINTER_TO_UINT(seq_iter) >= NUM_CATEGORIES)
    {
      dt_shortcut_t *s = g_sequence_get(seq_iter);
      if(s->action)
      {
        gchar *label = _action_full_label(s->action);
        label_case = g_utf8_casefold(label, -1);
        g_free(label);
      }
    }
  }
  const gboolean different = label_case ? !strstr(label_case, key_case) : TRUE;
  g_free(key_case);
  g_free(label_case);
  if(!different)
  {
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gtk_tree_view_expand_to_path(GTK_TREE_VIEW(search_data), path);
    gtk_tree_path_free(path);

    return FALSE;
  }

  GtkTreeIter child;
  if(gtk_tree_model_iter_children(model, &child, iter))
  {
    do
    {
      _search_func(model, column, key, &child, search_data);
    }
    while(gtk_tree_model_iter_next(model, &child));
  }

  return TRUE;
}

static gboolean _fallback_type_is_relevant(dt_action_t *ac, dt_action_type_t type)
{
  if(!ac) return FALSE;

  if(ac->type == type) return TRUE;

  if(ac->type >= DT_ACTION_TYPE_WIDGET)
  {
    if(type == DT_ACTION_TYPE_VALUE_FALLBACK)
    {
      const dt_action_def_t *def = _action_find_definition(ac);
      if(def && def->elements)
      {
        const dt_action_element_def_t *el = def->elements;
        do
        {
          if(el->effects == dt_action_effect_value) return TRUE;
          el++;
        } while(el->name);
      }
    }
  }
  else if(ac->type <= DT_ACTION_TYPE_SECTION)
    for(ac = ac->target; ac; ac = ac->next)
      if(_fallback_type_is_relevant(ac, type)) return TRUE;

  return FALSE;
}

static gboolean _visible_shortcuts(GtkTreeModel *model, GtkTreeIter  *iter, gpointer data)
{
  void *data_ptr = NULL;
  gtk_tree_model_get(model, iter, 0, &data_ptr, -1);

  if(GPOINTER_TO_UINT(data_ptr) < NUM_CATEGORIES) return TRUE;

  dt_shortcut_t *s = g_sequence_get(data_ptr);

  if(!darktable.control->enable_fallbacks && s->action->type == DT_ACTION_TYPE_FALLBACK
     && (GPOINTER_TO_INT(s->action->target) != DT_ACTION_TYPE_VALUE_FALLBACK
         || s->key_device || s->key || s->press || s->move_device || s->move || s->button))
    return FALSE;

  if(!_selected_action) return TRUE;

  if(_selected_action->type == DT_ACTION_TYPE_FALLBACK &&
     s->action->type == GPOINTER_TO_INT(_selected_action->target))
    return TRUE;

  for(dt_action_t *ac = s->action; ac; ac = ac->owner)
    if(ac == _selected_action)
      return TRUE;

  if(s->action->type == DT_ACTION_TYPE_FALLBACK)
    return _fallback_type_is_relevant(_selected_action, GPOINTER_TO_INT(s->action->target));

  return FALSE;
}

static void _resize_shortcuts_view(GtkWidget *view, GdkRectangle *allocation, gpointer data)
{
  dt_conf_set_int("shortcuts/window_split", gtk_paned_get_position(GTK_PANED(data)));
}

const dt_input_device_t DT_ALL_DEVICES = UINT8_MAX;
static void _shortcuts_save(const gchar *shortcuts_file, const dt_input_device_t device);
static void _shortcuts_load(const gchar *shortcuts_file,
                            const dt_input_device_t file_dev,
                            const dt_input_device_t load_dev,
                            const gboolean clear);

static void _fallbacks_toggled(GtkToggleButton *button, gpointer data)
{
  dt_conf_set_bool("accel/enable_fallbacks",
                   (darktable.control->enable_fallbacks = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button))));

  GtkTreeView *shortcuts_view = GTK_TREE_VIEW(data);
  gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(gtk_tree_view_get_model(shortcuts_view)));
}

static void _restore_clicked(GtkButton *button, gpointer user_data)
{
  enum
  {
    _DEFAULTS = 1,
    _STARTUP,
    _EDITS,
  };

  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("restore shortcuts"),
                                                  GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button))),
                                                  GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  _("_cancel"), GTK_RESPONSE_REJECT,
                                                  _("_defaults"), _DEFAULTS,
                                                  _("_startup"), _STARTUP,
                                                  _("_edits"), _EDITS,
                                                  NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_REJECT);

  GtkContainer *content_area = GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG (dialog)));
  GtkWidget *label = gtk_label_new(_("restore shortcuts from one of these states:\n"
                                     "  - default\n"
                                     "  - as at startup\n"
                                     "  - as when opening this dialog\n"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_container_add(content_area, label);
  GtkWidget *clear = gtk_check_button_new_with_label(_("clear all newer shortcuts\n"
                                                       "(instead of just restoring changed ones)"));
  gtk_container_add(content_area, clear);

  gtk_widget_show_all(GTK_WIDGET(content_area));

  const int resp = gtk_dialog_run(GTK_DIALOG(dialog));
  const gboolean wipe = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(clear));

  gtk_widget_destroy(dialog);

  switch(resp)
  {
  case _DEFAULTS:
    dt_shortcuts_load(".defaults", wipe);
    break;
  case _STARTUP:
    dt_shortcuts_load(".backup", wipe);
    break;
  case _EDITS:
    dt_shortcuts_load(".edit", wipe);
    break;
  }

  dt_shortcuts_save(NULL, FALSE);
}

static void _import_export_dev_changed(GtkComboBox *widget, gpointer user_data)
{
  gint dev = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
  g_object_set_data(G_OBJECT(user_data), "device", GINT_TO_POINTER(dev));
  gtk_combo_box_set_active(GTK_COMBO_BOX(user_data), 1); // make sure changed triggered
  gtk_combo_box_set_active(GTK_COMBO_BOX(user_data), dev > 1 ? 0 : -1);
  gtk_widget_set_visible(gtk_widget_get_parent(GTK_WIDGET(user_data)), dev > 1);
}

static void _export_id_changed(GtkComboBox *widget, gpointer user_data)
{
  gint dev = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "device"));
  gint id = dev <= 1 ? 0 :
            gtk_combo_box_get_active(GTK_COMBO_BOX(widget)) + (dev-1) * 10;

  gint count = 0;

  for(GSequenceIter *iter = g_sequence_get_begin_iter(darktable.control->shortcuts);
      !g_sequence_iter_is_end(iter);
      iter = g_sequence_iter_next(iter))
  {
    const dt_shortcut_t *s = g_sequence_get(iter);
    if(dev == 0 ||
       (id == 0 &&  s->key_device == id && s->move_device == id) ||
       (id != 0 && (s->key_device == id || s->move_device == id)))
      count++;
  }

  gchar *text = g_strdup_printf("%d %s", count, _("shortcuts"));
  gtk_label_set_text(GTK_LABEL(user_data), text);
  g_free(text);
}

static void _export_clicked(GtkButton *button, gpointer user_data)
{
  GtkWindow *win = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));

  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("export shortcuts"),
                                                  win, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  _("_cancel"), GTK_RESPONSE_REJECT,
                                                  _("_ok"), GTK_RESPONSE_OK,
                                                  NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_REJECT);

  GtkContainer *content_area = GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG (dialog)));
  GtkWidget *label = gtk_label_new(_("export all shortcuts to a file\n"
                                     "or just for one selected device\n"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_container_add(content_area, label);

  GtkWidget *combo_dev = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dev), _("all"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dev), _("keyboard"));
  for(GSList *driver = darktable.control->input_drivers; driver; driver = driver->next)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dev),
                                   ((dt_input_driver_definition_t *)driver->data)->name);
  gtk_container_add(content_area, combo_dev);

  GtkWidget *device_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  GtkWidget *combo_id = gtk_combo_box_text_new();
  for(gchar num[] = "0"; *num <= '9'; (*num)++)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_id), num);
  gtk_container_add(GTK_CONTAINER(device_box), combo_id);
  gtk_container_add(GTK_CONTAINER(device_box), dt_ui_label_new(_("device id")));

  gtk_container_add(content_area, device_box);

  GtkWidget *count = gtk_label_new("");
  gtk_container_add(content_area, count);

  g_signal_connect(combo_dev, "changed", G_CALLBACK(_import_export_dev_changed), combo_id);
  g_signal_connect(combo_id, "changed", G_CALLBACK(_export_id_changed), count);

  gtk_widget_show_all(GTK_WIDGET(content_area));

  gtk_combo_box_set_active(GTK_COMBO_BOX(combo_dev), 0);

  const int resp = gtk_dialog_run(GTK_DIALOG(dialog));

  const gint dev = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_dev));
  const gint id = dev == 0 ? DT_ALL_DEVICES :
                  dev == 1 ? 0 :
                  gtk_combo_box_get_active(GTK_COMBO_BOX(combo_id)) + (dev-1) * 10;

  gtk_widget_destroy(dialog);

  if(resp != GTK_RESPONSE_OK) return;

  GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        _("select file to export"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SAVE,
        _("_export"), _("_cancel"));

  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser), TRUE);
  dt_conf_get_folder_to_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(chooser));
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), "shortcutsrc");
  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));

    _shortcuts_save(filename, id);
    g_free(filename);
    dt_conf_set_folder_from_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(chooser));
  }
  g_object_unref(chooser);
}

static void _import_id_changed(GtkComboBox *widget, gpointer user_data)
{
  gint id = gtk_combo_box_get_active(widget);
  gtk_combo_box_set_active(GTK_COMBO_BOX(user_data), id);
}

static void _import_clicked(GtkButton *button, gpointer user_data)
{
  GtkWindow *win = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));

  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("import shortcuts"),
                                                  win, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  _("_cancel"), GTK_RESPONSE_REJECT,
                                                  _("_ok"), GTK_RESPONSE_OK,
                                                  NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_REJECT);

  GtkContainer *content_area = GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG (dialog)));
  GtkWidget *label = gtk_label_new(_("import all shortcuts from a file\n"
                                     "or just for one selected device\n"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_container_add(content_area, label);

  GtkWidget *combo_dev = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dev), _("all"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dev), _("keyboard"));
  for(GSList *driver = darktable.control->input_drivers; driver; driver = driver->next)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_dev),
                                   ((dt_input_driver_definition_t *)driver->data)->name);
  gtk_container_add(content_area, combo_dev);

  GtkWidget *device_grid = gtk_grid_new();

  GtkWidget *combo_from_id = gtk_combo_box_text_new();
  for(gchar num[] = "0"; *num <= '9'; (*num)++)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_from_id), num);
  gtk_grid_attach(GTK_GRID(device_grid), combo_from_id, 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(device_grid), dt_ui_label_new(_("id in file")), 1, 0, 1, 1);

  GtkWidget *combo_to_id = gtk_combo_box_text_new();
  for(gchar num[] = "0"; *num <= '9'; (*num)++)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_to_id), num);
  gtk_grid_attach(GTK_GRID(device_grid), combo_to_id, 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(device_grid), dt_ui_label_new(_("id when loaded")), 1, 1, 1, 1);

  gtk_container_add(content_area, device_grid);

  GtkWidget *clear = gtk_check_button_new_with_label(_("clear device first"));
  gtk_container_add(content_area, clear);

  g_signal_connect(combo_dev, "changed", G_CALLBACK(_import_export_dev_changed), combo_from_id);
  g_signal_connect(combo_from_id, "changed", G_CALLBACK(_import_id_changed), combo_to_id);

  gtk_widget_show_all(GTK_WIDGET(content_area));

  gtk_combo_box_set_active(GTK_COMBO_BOX(combo_dev), 0);

  const int resp = gtk_dialog_run(GTK_DIALOG(dialog));
  const gint dev = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_dev));
  const gint from_id = dev == 0 ? DT_ALL_DEVICES :
                       dev == 1 ? 0 :
                       gtk_combo_box_get_active(GTK_COMBO_BOX(combo_from_id)) + (dev-1) * 10;
  const gint to_id = dev == 1 ? 0 :
                     gtk_combo_box_get_active(GTK_COMBO_BOX(combo_to_id)) + (dev-1) * 10;
  const gboolean wipe = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(clear));

  gtk_widget_destroy(dialog);

  if(resp != GTK_RESPONSE_OK) return;

  GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        _("select file to import"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
        _("_import"), _("_cancel"));
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(chooser), FALSE);

  dt_conf_get_folder_to_file_chooser("ui_last/import_path", GTK_FILE_CHOOSER(chooser));
  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));

    if(wipe && from_id != DT_ALL_DEVICES)
    {
      GtkTreeModel *model = GTK_TREE_MODEL(_shortcuts_store);
      GtkTreeIter category;
      gboolean valid_category = gtk_tree_model_get_iter_first(model, &category);
      while(valid_category)
      {
        GtkTreeIter child;
        gboolean valid_child = gtk_tree_model_iter_children(model, &child, &category);
        while(valid_child)
        {
          gpointer child_data;
          gtk_tree_model_get(model, &child, 0, &child_data, -1);

          dt_shortcut_t *s = g_sequence_get(child_data);
          if((to_id == 0 &&  s->key_device == to_id && s->move_device == to_id) ||
             (to_id != 0 && (s->key_device == to_id || s->move_device == to_id)))
          {
            g_sequence_remove(child_data);
            valid_child = gtk_tree_store_remove(GTK_TREE_STORE(model), &child);
          }
          else
            valid_child = gtk_tree_model_iter_next(model, &child);
        }
        valid_category = gtk_tree_model_iter_next(model, &category);
      };
    }

    _shortcuts_load(filename, from_id, to_id, wipe && from_id == DT_ALL_DEVICES);

    g_free(filename);
    dt_conf_set_folder_from_file_chooser("ui_last/import_path", GTK_FILE_CHOOSER(chooser));
  }
  g_object_unref(chooser);

  dt_shortcuts_save(NULL, FALSE);
}

GtkWidget *dt_shortcuts_prefs(GtkWidget *widget)
{
  // Save the shortcuts before editing
  dt_shortcuts_save(".edit", FALSE);

  GtkWidget *widget_or_parent = widget;
  while(!(_selected_action = dt_action_widget(widget_or_parent)) && widget_or_parent)
    widget_or_parent = gtk_widget_get_parent(widget_or_parent);
  darktable.control->element = -1;

  GtkWidget *container = gtk_paned_new(GTK_ORIENTATION_VERTICAL);

  // Building the shortcut treeview
  g_set_weak_pointer(&_shortcuts_store, gtk_tree_store_new(1, G_TYPE_POINTER)); // static

  _add_shortcuts_to_tree();

  GtkTreeModel *filtered_shortcuts = gtk_tree_model_filter_new(GTK_TREE_MODEL(_shortcuts_store), NULL);
  g_object_unref(G_OBJECT(_shortcuts_store));

  gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(filtered_shortcuts), _visible_shortcuts, NULL, NULL);

  GtkTreeView *shortcuts_view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(filtered_shortcuts));
  g_object_unref(G_OBJECT(filtered_shortcuts));
  gtk_tree_view_set_search_column(shortcuts_view, 0); // fake column for _search_func
  gtk_tree_view_set_search_equal_func(shortcuts_view, _search_func, shortcuts_view, NULL);
  GtkWidget *search_shortcuts = gtk_search_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(search_shortcuts), _("search shortcuts list"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(search_shortcuts), _("incrementally search the list of shortcuts\npress up or down keys to cycle through matches"));
  g_signal_connect(G_OBJECT(search_shortcuts), "activate", G_CALLBACK(dt_gui_search_stop), shortcuts_view);
  g_signal_connect(G_OBJECT(search_shortcuts), "stop-search", G_CALLBACK(dt_gui_search_stop), shortcuts_view);
  gtk_tree_view_set_search_entry(shortcuts_view, GTK_ENTRY(search_shortcuts));

  gtk_tree_selection_set_select_function(gtk_tree_view_get_selection(shortcuts_view),
                                         _shortcut_selection_function, NULL, NULL);
  g_object_set(shortcuts_view, "has-tooltip", TRUE, NULL);
  gtk_widget_set_name(GTK_WIDGET(shortcuts_view), "shortcuts_view");
  g_signal_connect(G_OBJECT(shortcuts_view), "row-activated", G_CALLBACK(_shortcut_row_activated), filtered_shortcuts);
  g_signal_connect(G_OBJECT(shortcuts_view), "key-press-event", G_CALLBACK(_view_key_pressed), search_shortcuts);
  g_signal_connect(G_OBJECT(_shortcuts_store), "row-inserted", G_CALLBACK(_shortcut_row_inserted), shortcuts_view);

  // Setting up the cell renderers
  _add_prefs_column(shortcuts_view, gtk_cell_renderer_text_new(), _("shortcut"), SHORTCUT_VIEW_DESCRIPTION);

  _add_prefs_column(shortcuts_view, gtk_cell_renderer_text_new(), _("action"), SHORTCUT_VIEW_ACTION);

  GtkCellRenderer *renderer = NULL;

  renderer = gtk_cell_renderer_combo_new();
  GtkListStore *elements = gtk_list_store_new(1, G_TYPE_STRING);
  g_object_set(renderer, "model", elements, "text-column", 0, "has-entry", FALSE, NULL);
  g_signal_connect(renderer, "editing-started" , G_CALLBACK(_element_editing_started), filtered_shortcuts);
  g_signal_connect(renderer, "changed", G_CALLBACK(_element_changed), filtered_shortcuts);
  _add_prefs_column(shortcuts_view, renderer, _("element"), SHORTCUT_VIEW_ELEMENT);

  renderer = gtk_cell_renderer_combo_new();
  GtkListStore *effects = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_INT);
  g_object_set(renderer, "model", effects, "text-column", 0, "has-entry", FALSE, NULL);
  g_signal_connect(renderer, "editing-started" , G_CALLBACK(_effect_editing_started), filtered_shortcuts);
  g_signal_connect(renderer, "changed", G_CALLBACK(_effect_changed), filtered_shortcuts);
  _add_prefs_column(shortcuts_view, renderer, _("effect"), SHORTCUT_VIEW_EFFECT);

  renderer = gtk_cell_renderer_spin_new();
  g_object_set(renderer, "adjustment", gtk_adjustment_new(1, -1000, 1000, .01, 1, 10),
                         "digits", 3, "xalign", 1.0, NULL);
  g_signal_connect(renderer, "edited", G_CALLBACK(_speed_edited), filtered_shortcuts);
  _add_prefs_column(shortcuts_view, renderer, _("speed"), SHORTCUT_VIEW_SPEED);

  renderer = gtk_cell_renderer_combo_new();
  GtkListStore *instances = gtk_list_store_new(1, G_TYPE_STRING);
  for(int i = 0; i < NUM_INSTANCES; i++)
    gtk_list_store_insert_with_values(instances, NULL, -1, 0, _(instance_label[i]), -1);
  for(char relative[] = "-2"; (relative[0] ^= '+' ^ '-') == '-' || ++relative[1] <= '9'; )
    gtk_list_store_insert_with_values(instances, NULL, -1, 0, relative, -1);
  g_object_set(renderer, "model", instances, "text-column", 0, "has-entry", FALSE, NULL);
  g_signal_connect(renderer, "edited", G_CALLBACK(_instance_edited), filtered_shortcuts);
  _add_prefs_column(shortcuts_view, renderer, _("instance"), SHORTCUT_VIEW_INSTANCE);

  // Adding the shortcuts treeview to its containers
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(scroll, -1, 100);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(shortcuts_view));
  gtk_paned_pack2(GTK_PANED(container), scroll, TRUE, FALSE);

  // Creating the action selection treeview
  g_set_weak_pointer(&_actions_store, gtk_tree_store_new(1, G_TYPE_POINTER)); // static
  GtkTreeIter found_iter = {};
  if(widget && !_selected_action)
  {
    const dt_view_t *active_view = dt_view_manager_get_current_view(darktable.view_manager);
    if(gtk_widget_is_ancestor(widget, dt_ui_center_base(darktable.gui->ui)) ||
       dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP, widget) ||
       dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM, widget) ||
       gtk_widget_is_ancestor(widget, GTK_WIDGET(dt_ui_get_container(darktable.gui->ui,
                                               DT_UI_CONTAINER_PANEL_LEFT_TOP))) ||
       gtk_widget_is_ancestor(widget, GTK_WIDGET(dt_ui_get_container(darktable.gui->ui,
                                               DT_UI_CONTAINER_PANEL_RIGHT_TOP))))
      _selected_action = (dt_action_t*)active_view;
    else if(dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_BOTTOM, widget))
      _selected_action = &darktable.control->actions_thumb;
    else if(dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_RIGHT, widget))
      _selected_action = active_view->view(active_view) == DT_VIEW_DARKROOM
                       ? &darktable.control->actions_iops
                       : &darktable.control->actions_libs;
    else if(dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_LEFT, widget))
      _selected_action = &darktable.control->actions_libs;
    else
      _selected_action = &darktable.control->actions_global;
  }
  _add_actions_to_tree(NULL, darktable.control->actions, _selected_action, &found_iter);

  GtkTreeView *actions_view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(_actions_store)));
  g_object_unref(_actions_store);
  gtk_tree_view_set_search_column(actions_view, 1); // fake column for _search_func
  gtk_tree_view_set_search_equal_func(actions_view, _search_func, actions_view, NULL);
  GtkWidget *search_actions = gtk_search_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(search_actions), _("search actions list"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(search_actions), _("incrementally search the list of actions\npress up or down keys to cycle through matches"));
  g_signal_connect(G_OBJECT(search_actions), "activate", G_CALLBACK(dt_gui_search_stop), actions_view);
  g_signal_connect(G_OBJECT(search_actions), "stop-search", G_CALLBACK(dt_gui_search_stop), actions_view);
  gtk_tree_view_set_search_entry(actions_view, GTK_ENTRY(search_actions));

  g_object_set(actions_view, "has-tooltip", TRUE, NULL);
  gtk_widget_set_name(GTK_WIDGET(actions_view), "actions_view");
  g_signal_connect(G_OBJECT(actions_view), "row-activated", G_CALLBACK(_action_row_activated), _actions_store);
  g_signal_connect(G_OBJECT(actions_view), "button-press-event", G_CALLBACK(_action_view_click), _actions_store);
  g_signal_connect(G_OBJECT(actions_view), "key-press-event", G_CALLBACK(_view_key_pressed), search_actions);

  g_signal_connect(G_OBJECT(gtk_tree_view_get_selection(actions_view)), "changed",
                   G_CALLBACK(_action_selection_changed), shortcuts_view);
  g_signal_connect(G_OBJECT(gtk_tree_view_get_selection(shortcuts_view)), "changed",
                   G_CALLBACK(_shortcut_selection_changed), actions_view);

  renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(_("action"), renderer, NULL);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_column_set_cell_data_func(column, renderer, _fill_action_fields, GINT_TO_POINTER(TRUE), NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(actions_view), column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("type"), renderer, NULL);
  gtk_tree_view_column_set_alignment(column, 1.0);
  gtk_cell_renderer_set_alignment(renderer, 1.0, 0.0);
  gtk_tree_view_column_set_cell_data_func(column, renderer, _fill_action_fields, NULL, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(actions_view), column);

  // Adding the action treeview to its containers
  scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(scroll, -1, 100);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(actions_view));
  gtk_paned_pack1(GTK_PANED(container), scroll, TRUE, FALSE);

  if(found_iter.user_data)
  {
    GtkTreeIter *send_iter = calloc(1, sizeof(GtkTreeIter));
    *send_iter = found_iter;
    gtk_widget_add_events(GTK_WIDGET(actions_view), GDK_STRUCTURE_MASK);
    g_signal_connect_data(G_OBJECT(actions_view), "map-event", G_CALLBACK(_action_view_map),
                          send_iter, (GClosureNotify)g_free, G_CONNECT_AFTER);
  }

  GtkTreePath *path = gtk_tree_path_new_first();
  gtk_tree_view_set_cursor(shortcuts_view, path, NULL, FALSE);
  gtk_tree_path_free(path);

  const int split_position = dt_conf_get_int("shortcuts/window_split");
  if(split_position) gtk_paned_set_position(GTK_PANED(container), split_position);
  g_signal_connect(G_OBJECT(shortcuts_view), "size-allocate", G_CALLBACK(_resize_shortcuts_view), container);

  GtkWidget *button_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), *button = NULL;
  gtk_widget_set_name(button_bar, "shortcut-controls");
  gtk_box_pack_start(GTK_BOX(button_bar), search_actions, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(button_bar), search_shortcuts, FALSE, FALSE, 0);

  button = gtk_check_button_new_with_label(_("enable fallbacks"));
  gtk_widget_set_tooltip_text(button, _("enables default meanings for additional buttons, modifiers or moves\n"
                                        "when used in combination with a base shortcut"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), darktable.control->enable_fallbacks);
  g_signal_connect(button, "toggled", G_CALLBACK(_fallbacks_toggled), shortcuts_view);
  gtk_box_pack_start(GTK_BOX(button_bar), button, TRUE, FALSE, 0);

  button = gtk_button_new_with_label(_("help"));
  gtk_widget_set_tooltip_text(button, _("open help page for shortcuts"));
  dt_gui_add_help_link(button, "shortcuts");
  g_signal_connect(button, "clicked", G_CALLBACK(dt_gui_show_help), NULL);
  gtk_box_pack_end(GTK_BOX(button_bar), button, FALSE, FALSE, 0);

  button = gtk_button_new_with_label(_("restore..."));
  gtk_widget_set_tooltip_text(button, _("restore default shortcuts or previous state"));
  g_signal_connect(button, "clicked", G_CALLBACK(_restore_clicked), NULL);
  gtk_box_pack_end(GTK_BOX(button_bar), button, FALSE, FALSE, 0);

  button = gtk_button_new_with_label(_("import..."));
  gtk_widget_set_tooltip_text(button, _("fully or partially import shortcuts from file"));
  g_signal_connect(button, "clicked", G_CALLBACK(_import_clicked), NULL);
  gtk_box_pack_end(GTK_BOX(button_bar), button, FALSE, FALSE, 0);

  button = gtk_button_new_with_label(_("export..."));
  gtk_widget_set_tooltip_text(button, _("fully or partially export shortcuts to file"));
  g_signal_connect(button, "clicked", G_CALLBACK(_export_clicked), NULL);
  gtk_box_pack_end(GTK_BOX(button_bar), button, FALSE, FALSE, 0);

  GtkWidget *top_level = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(top_level), container, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(top_level), button_bar, FALSE, FALSE, 0);

  return top_level;
}

static void _shortcuts_save(const gchar *shortcuts_file, const dt_input_device_t device)
{
  FILE *f = g_fopen(shortcuts_file, "wb");
  if(f)
  {
    for(GSequenceIter *i = g_sequence_get_begin_iter(darktable.control->shortcuts);
        !g_sequence_iter_is_end(i);
        i = g_sequence_iter_next(i))
    {
      dt_shortcut_t *s = g_sequence_get(i);

      if(device != DT_ALL_DEVICES &&
         (device != 0 ||  s->key_device != device || s->move_device != device) &&
         (device == 0 || (s->key_device != device && s->move_device != device)))
        continue;

      gchar *key_name = _shortcut_key_move_name(s->key_device, s->key, s->mods, FALSE);
      fprintf(f, "%s", key_name);
      g_free(key_name);

      if(s->move_device || s->move)
      {
        gchar *move_name = _shortcut_key_move_name(s->move_device, s->move, DT_MOVE_NAME, FALSE);
        fprintf(f, ";%s", move_name);
        g_free(move_name);
        if(s->direction)
          fprintf(f, ";%s", s->direction & DT_SHORTCUT_UP ? "up" : "down");
      }

      if(s->press  & DT_SHORTCUT_DOUBLE ) fprintf(f, ";%s", "double");
      if(s->press  & DT_SHORTCUT_TRIPLE ) fprintf(f, ";%s", "triple");
      if(s->press  & DT_SHORTCUT_LONG   ) fprintf(f, ";%s", "long");
      if(s->button & DT_SHORTCUT_LEFT   ) fprintf(f, ";%s", "left");
      if(s->button & DT_SHORTCUT_MIDDLE ) fprintf(f, ";%s", "middle");
      if(s->button & DT_SHORTCUT_RIGHT  ) fprintf(f, ";%s", "right");
      if(s->click  & DT_SHORTCUT_DOUBLE ) fprintf(f, ";%s", "double");
      if(s->click  & DT_SHORTCUT_TRIPLE ) fprintf(f, ";%s", "triple");
      if(s->click  & DT_SHORTCUT_LONG   ) fprintf(f, ";%s", "long");

      fprintf(f, "=");

      gchar *action_label = _action_full_id(s->action);
      fprintf(f, "%s", action_label);
      g_free(action_label);

      const dt_action_element_def_t *elements = _action_find_elements(s->action);
      if(s->element)
        fprintf(f, ";%s", NQ_(elements[s->element].name));
      if(s->effect > (_shortcut_is_move(s) ? DT_ACTION_EFFECT_DEFAULT_MOVE
                                           : DT_ACTION_EFFECT_DEFAULT_KEY))
      {
        const gchar *cef = _action_find_effect_combo(s->action, &elements[s->element], s->effect);
        if(cef)
          fprintf(f, ";item:%s", NQ_(cef));
        else
          fprintf(f, ";%s", NQ_(elements[s->element].effects[s->effect]));
     }

      if(s->instance == -1) fprintf(f, ";last");
      if(s->instance == +1) fprintf(f, ";first");
      if(abs(s->instance) > 1) fprintf(f, ";%+d", s->instance);
      if(s->speed != 1.0) fprintf(f, ";*%g", s->speed);

      fprintf(f, "\n");
    }

    fclose(f);
  }
}

void dt_shortcuts_save(const gchar *ext, const gboolean backup)
{
  char shortcuts_file[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(shortcuts_file, sizeof(shortcuts_file));
  g_strlcat(shortcuts_file, "/shortcutsrc", PATH_MAX);
  if(ext) g_strlcat(shortcuts_file, ext, PATH_MAX);
  if(backup)
  {
    gchar *backup_file = g_strdup_printf("%s.backup", shortcuts_file);
    g_rename(shortcuts_file, backup_file);
    g_free(backup_file);
  }

  _shortcuts_save(shortcuts_file, DT_ALL_DEVICES);
}

static gboolean _find_combo_effect(const gchar **effects, const gchar *token, dt_action_t *ac, gint *ef)
{
  if(effects == dt_action_effect_selection && g_strstr_len(token, 5, "item:"))
  {
    int effect = -1;
    const char *entry = NULL;

    dt_introspection_type_enum_tuple_t *values
      = g_hash_table_lookup(darktable.bauhaus->combo_introspection, ac);
    if(values)
    {
      while((entry = (values[++effect].description ? values[effect].description : values[effect].name)))
        if(!g_ascii_strcasecmp(token + 5, NQ_(entry))) break;
    }
    else
    {
      gchar **strings
        = g_hash_table_lookup(darktable.bauhaus->combo_list, ac);
      if(strings)
      {
        while((entry = strings[++effect]))
          if(!g_ascii_strcasecmp(token + 5, NQ_(entry))) break;
      }
    }
    if(entry)
    {
      *ef = effect + DT_ACTION_EFFECT_COMBO_SEPARATOR + 1;
      return TRUE;
    }
  }

  return FALSE;
}

static void _shortcuts_load(const gchar *shortcuts_file, dt_input_device_t file_dev, const dt_input_device_t load_dev, const gboolean clear)
{
  // start with an empty shortcuts collection
  if(clear && darktable.control->shortcuts)
  {
    if(_shortcuts_store) gtk_tree_store_clear(_shortcuts_store);

    g_sequence_free(darktable.control->shortcuts);
    darktable.control->shortcuts = g_sequence_new(g_free);

    if(_shortcuts_store) _add_shortcuts_to_tree();
  }

  FILE *f = g_fopen(shortcuts_file, "rb");
  if(f)
  {
    while(!feof(f))
    {
      char line[1024];
      char *read = fgets(line, sizeof(line), f);
      if(read > 0)
      {
        line[strcspn(line, "\r\n")] = '\0';

        char *act_start = strchr(line, '=');
        if(!act_start)
        {
          dt_print(DT_DEBUG_ALWAYS,
                   "[dt_shortcuts_load] line '%s' is not an assignment\n",
                   line);
          continue;
        }

        dt_shortcut_t s = { .speed = 1 };

        char *token = strtok(line, "=;");
        if(g_ascii_strcasecmp(token, "None"))
        {
          char *colon = strchr(token, ':');
          if(!colon)
          {
            gtk_accelerator_parse(token, &s.key, &s.mods);
            if(s.mods)
              dt_print(DT_DEBUG_ALWAYS,
                       "[dt_shortcuts_load] unexpected modifiers found in %s\n",
                       token);
            if(!s.key && sscanf(token, "tablet button %u", &s.key))
              s.key_device = DT_SHORTCUT_DEVICE_TABLET;
            if(!s.key)
              dt_print(DT_DEBUG_ALWAYS,
                       "[dt_shortcuts_load] no key name found in %s\n",
                       token);
          }
          else
          {
            char *key_start = colon + 1;
            *colon-- = 0;
            if(colon == token)
            {
              dt_print(DT_DEBUG_ALWAYS,
                       "[dt_shortcuts_load] missing driver name in %s\n", token);
              continue;
            }
            dt_input_device_t id = *colon - '0';
            if(id > 9 )
              id = 0;
            else
              *colon-- = 0;

            GSList *driver = darktable.control->input_drivers;
            while(driver)
            {
              id += 10;
              dt_input_driver_definition_t *callbacks = driver->data;
              if(!g_ascii_strcasecmp(token, callbacks->name))
              {
                if(!callbacks->string_to_key(key_start, &s.key))
                  dt_print(DT_DEBUG_ALWAYS,
                           "[dt_shortcuts_load] key not recognised in %s\n", key_start);

                s.key_device = id;
                break;
              }
              driver = driver->next;
            }
            if(!driver)
            {
              dt_print(DT_DEBUG_ALWAYS,
                       "[dt_shortcuts_load] '%s' is not a valid driver\n", token);
              continue;
            }
          }
        }

        while((token = strtok(NULL, "=;")) && token < act_start)
        {
          char *colon = strchr(token, ':');
          if(!colon)
          {
            int mod = -1;
            while(modifier_string[++mod].modifier)
              if(!g_ascii_strcasecmp(token, modifier_string[mod].name)) break;
            if(modifier_string[mod].modifier)
            {
              s.mods |= modifier_string[mod].modifier;
              continue;
            }

            if(!g_ascii_strcasecmp(token, "left"  )) { s.button |= DT_SHORTCUT_LEFT  ; continue; }
            if(!g_ascii_strcasecmp(token, "middle")) { s.button |= DT_SHORTCUT_MIDDLE; continue; }
            if(!g_ascii_strcasecmp(token, "right" )) { s.button |= DT_SHORTCUT_RIGHT ; continue; }

            if(s.button)
            {
              if(!g_ascii_strcasecmp(token, "double")) { s.click |= DT_SHORTCUT_DOUBLE; continue; }
              if(!g_ascii_strcasecmp(token, "triple")) { s.click |= DT_SHORTCUT_TRIPLE; continue; }
              if(!g_ascii_strcasecmp(token, "long"  )) { s.click |= DT_SHORTCUT_LONG  ; continue; }
            }
            else
            {
              if(!g_ascii_strcasecmp(token, "double")) { s.press |= DT_SHORTCUT_DOUBLE; continue; }
              if(!g_ascii_strcasecmp(token, "triple")) { s.press |= DT_SHORTCUT_TRIPLE; continue; }
              if(!g_ascii_strcasecmp(token, "long"  )) { s.press |= DT_SHORTCUT_LONG  ; continue; }
            }

            int move = 0;
            while(move_string[++move])
              if(!g_ascii_strcasecmp(token, move_string[move])) break;
            if(move_string[move])
            {
              s.move = move;
              continue;
            }

            if(!g_ascii_strcasecmp(token, "up"  )) { s.direction = DT_SHORTCUT_UP  ; continue; }
            if(!g_ascii_strcasecmp(token, "down")) { s.direction= DT_SHORTCUT_DOWN; continue; }

            dt_print(DT_DEBUG_ALWAYS,
                     "[dt_shortcuts_load] token '%s' not recognised\n", token);
          }
          else
          {
            char *move_start = colon + 1;
            *colon-- = 0;
            if(colon == token)
            {
              dt_print(DT_DEBUG_ALWAYS,
                       "[dt_shortcuts_load] missing driver name in %s\n", token);
              continue;
            }
            dt_input_device_t id = *colon - '0';
            if(id > 9 )
              id = 0;
            else
              *colon-- = 0;

            GSList *driver = darktable.control->input_drivers;
            while(driver)
            {
              id += 10;
              const dt_input_driver_definition_t *callbacks = driver->data;
              if(!g_ascii_strcasecmp(token, callbacks->name))
              {
                if(!callbacks->string_to_move(move_start, &s.move))
                  dt_print(DT_DEBUG_ALWAYS,
                           "[dt_shortcuts_load] move not recognised in %s\n", move_start);

                s.move_device = id;
                break;
              }
              driver = driver->next;
            }
            if(!driver)
            {
              dt_print(DT_DEBUG_ALWAYS,
                       "[dt_shortcuts_load] '%s' is not a valid driver\n", token);
              continue;
            }
          }
        }

        // find action
        gchar **path = g_strsplit(token, "/", 0);
        s.action = dt_action_locate(NULL, path, FALSE);
        g_strfreev(path);

        if(!s.action)
        {
          dt_print(DT_DEBUG_ALWAYS,
                   "[dt_shortcuts_load] action path '%s' not found\n", token);
          continue;
        }

        const dt_action_element_def_t *elements = _action_find_elements(s.action);
        const gchar **effects = NULL;
        const gint default_effect = s.effect = _shortcut_is_move(&s)
                                             ? DT_ACTION_EFFECT_DEFAULT_MOVE
                                             : DT_ACTION_EFFECT_DEFAULT_KEY;

        while((token = strtok(NULL, ";")))
        {
          if(elements)
          {
            int element = -1;
            while(elements[++element].name)
              if(!g_ascii_strcasecmp(token, NQ_(elements[element].name))) break;
            if(elements[element].name)
            {
              s.element = element;
              s.effect = default_effect; // reset if an effect for a different element was found first
              continue;
            }

            effects = elements[s.element].effects;

            if(_find_combo_effect(effects, token, s.action, &s.effect)) continue;

            int effect = -1;
            while(effects[++effect])
              if(!g_ascii_strcasecmp(token, NQ_(effects[effect]))) break;
            if(effects[effect])
            {
              s.effect = effect;
              continue;
            }
          }

          if(!g_ascii_strcasecmp(token, "first")) s.instance =  1; else
          if(!g_ascii_strcasecmp(token, "last" )) s.instance = -1; else
          if(*token == '+' || *token == '-') sscanf(token, "%d", &s.instance); else
          if(*token == '*') sscanf(token, "*%g", &s.speed); else
          dt_print(DT_DEBUG_ALWAYS,
                   "[dt_shortcuts_load] token '%s' not recognised\n", token);
        }

        if(file_dev == DT_ALL_DEVICES ||
           (file_dev == 0 &&  s.key_device == file_dev && s.move_device == file_dev) ||
           (file_dev != 0 && (s.key_device == file_dev || s.move_device == file_dev)))
        {
          if(file_dev != 0)
          {
            if(s.key_device  == file_dev) s.key_device  = load_dev;
            if(s.move_device == file_dev) s.move_device = load_dev;
          }

          _insert_shortcut(&s, FALSE);
        }
      }
    }
    fclose(f);
  }
}

void dt_shortcuts_load(const gchar *ext, const gboolean clear)
{
  char shortcuts_file[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(shortcuts_file, sizeof(shortcuts_file));
  g_strlcat(shortcuts_file, "/shortcutsrc", PATH_MAX);
  if(ext) g_strlcat(shortcuts_file, ext, PATH_MAX);
  if(!g_file_test(shortcuts_file, G_FILE_TEST_EXISTS))
    return;

  _shortcuts_load(shortcuts_file, DT_ALL_DEVICES, DT_ALL_DEVICES, clear);
}

void dt_shortcuts_reinitialise(dt_action_t *action)
{
  for(GSList *d = darktable.control->input_drivers; d; d = d->next)
  {
    const dt_input_driver_definition_t *driver = d->data;
    driver->module->gui_cleanup(driver->module);
    driver->module->gui_init(driver->module);
  }

  // reload shortcuts
  dt_shortcuts_load(NULL, TRUE);

  char actions_file[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(actions_file, sizeof(actions_file));
  g_strlcat(actions_file, "/all_actions", PATH_MAX);
  FILE *f = g_fopen(actions_file, "wb");
  _dump_actions(f, darktable.control->actions);
  fclose(f);

  dt_control_log(_("input devices reinitialised"));
}

void dt_shortcuts_select_view(dt_view_type_flags_t view)
{
  g_sequence_sort(darktable.control->shortcuts, _shortcut_compare_func, GINT_TO_POINTER(view));
}

static GSList *_pressed_keys = NULL, *_hold_keys = NULL; // lists of currently pressed and held keys
static guint _pressed_button = 0;
static guint _last_time = 0; // time of key or button press
                             // used to determine if release should trigger action
                             // set to 0 by any intermediate move (so no action on release)
static guint  _last_mapping_time = 0;
static guint _timeout_source = 0;
static guint _focus_loss_key = 0;
static guint _focus_loss_press = 0;

static dt_action_t _value_action = { .type = DT_ACTION_TYPE_FALLBACK,
                                     .target = GINT_TO_POINTER(DT_ACTION_TYPE_VALUE_FALLBACK) };

static void _lookup_mapping_widget()
{
  if(_sc.action) return;
  _sc.action = dt_action_widget(darktable.control->mapping_widget);
  if(!_sc.action) return;

  _sc.instance = 0;
  if(dt_conf_get_bool("accel/assign_instance"))
    _find_relative_instance(_sc.action, darktable.control->mapping_widget, &_sc.instance);

  _sc.element = 0;
  const dt_action_def_t *def = _action_find_definition(_sc.action);
  if(def && def->elements && def->elements[0].name && darktable.control->element > 0)
    _sc.element = darktable.control->element;
}

gboolean dt_action_widget_invisible(GtkWidget *w)
{
  GtkWidget *p = gtk_widget_get_parent(w);
  return (!GTK_IS_WIDGET(w) || !gtk_widget_get_visible(w) || (!gtk_widget_get_visible(p)
          && strcmp(gtk_widget_get_name(p), "collapsible")
          && !gtk_style_context_has_class(gtk_widget_get_style_context(p), "dt_plugin_ui_main")));
}

gboolean _shortcut_closest_match(GSequenceIter **current,
                                 dt_shortcut_t *s,
                                 gboolean *fully_matched,
                                 const dt_action_def_t *def,
                                 char **fb_log)
{
  *current = g_sequence_iter_prev(*current);
  dt_shortcut_t *c = g_sequence_get(*current);
//dt_print(DT_DEBUG_INPUT, "  [_shortcut_closest_match] shortcut considered: %s\n", _shortcut_description(c));

  gboolean applicable;
  while((applicable =
           (c->key_device == s->key_device && c->key == s->key && c->press >= (s->press & ~DT_SHORTCUT_LONG) &&
           ((!c->move_device && !c->move) ||
             (c->move_device == s->move_device && c->move == s->move)) &&
           (!s->action || s->action->type != DT_ACTION_TYPE_FALLBACK ||
            s->action->target == c->action->target))) &&
        !g_sequence_iter_is_begin(*current) &&
        (((c->button || c->click) && (c->button != s->button || c->click != s->click)) ||
         (c->mods       && c->mods != s->mods ) ||
         (c->direction  & ~s->direction       ) ||
         (c->element    && s->element         ) ||
         (c->effect > 0 && s->effect > 0      ) ||
         (c->instance   && s->instance        ) ||
         (c->element    && s->effect > 0 && def &&
          def->elements[c->element].effects != def->elements[s->element].effects ) ))
  {
    *current = g_sequence_iter_prev(*current);
    c = g_sequence_get(*current);
//dt_print(DT_DEBUG_INPUT, "  [_shortcut_closest_match] shortcut considered: %s\n", _shortcut_description(c));
  }

  if(applicable)
  {
    s->key_device   =  0;
    s->key          =  0;
    s->mods        &= ~c->mods;
    s->press       -=  c->press;
    s->button      &= ~c->button;
    s->click       -=  c->click;
    s->direction   &= ~c->direction;
    s->move_device -=  c->move_device;
    s->move        -=  c->move;

    if(c->element) s->element = c->element;
    if(c->effect > DT_ACTION_EFFECT_DEFAULT_KEY) s->effect = c->effect;
    if(c->instance) s->instance = c->instance;

    s->speed *= c->speed;
    s->action = c->action;

    *fully_matched = !(s->mods || s->press || s->button || s->click || s->move_device || s->move);

    if(*fb_log)
      *fb_log = dt_util_dstrcat(*fb_log, "\n%s \u2192 %s", _shortcut_description(c), _action_description(c, 2));

    return TRUE;
  }
  else
  {
    *fully_matched = FALSE;
    return FALSE;
  }
}

static gboolean _shortcut_match(dt_shortcut_t *f, gchar **fb_log)
{
  if(!darktable.view_manager->current_view) return FALSE;

  f->views = darktable.view_manager->current_view->view(darktable.view_manager->current_view);
  gpointer v = GINT_TO_POINTER(f->views);

  GSequenceIter *existing = g_sequence_search(darktable.control->shortcuts, f, _shortcut_compare_func, v);

  gboolean matched = FALSE;

  if(!_shortcut_closest_match(&existing, f, &matched, NULL, fb_log))
  {
    // see if there is a fallback from midi knob press to knob turn
    if(!f->key_device || f->move_device || f->move)
      return FALSE;

    dt_input_device_t id = f->key_device;
    GSList *driver = darktable.control->input_drivers;
    while(driver && (id -= 10) >= 10)
      driver = driver->next;

    if(!driver)
      return FALSE;
    else
    {
      dt_input_driver_definition_t *callbacks = driver->data;

      if(callbacks->key_to_move &&
         callbacks->key_to_move(callbacks->module, f->key_device, f->key, &f->move))
      {
        f->move_device = f->key_device;
        f->key_device = 0;
        f->key = 0;

        existing = g_sequence_search(darktable.control->shortcuts, f, _shortcut_compare_func, v);
        if(!_shortcut_closest_match(&existing, f, &matched, NULL, fb_log) && !f->action)
          return FALSE;
        else
        {
          const dt_action_def_t *def = _action_find_definition(f->action);

          if(def && def->elements
             && (def->elements[f->element].effects == dt_action_effect_value
                 || def->elements[f->element].effects == dt_action_effect_selection))
            f->effect = DT_ACTION_EFFECT_RESET;
        }
      }
    }
  }

  if(!matched && f->action && darktable.control->enable_fallbacks)
  {
    // try to add fallbacks
    f->views = 0;

    dt_action_t *matched_action = f->action;
    dt_action_t fallback_action = { .type = DT_ACTION_TYPE_FALLBACK,
                                    .target = GINT_TO_POINTER(matched_action->type) };
    f->action = &fallback_action;

    const dt_action_def_t *def = _action_find_definition(matched_action);

    existing = g_sequence_search(darktable.control->shortcuts, f, _shortcut_compare_func, v);
    while(_shortcut_closest_match(&existing, f, &matched, def, fb_log) && !matched) {};

    if(!matched && def && def->elements[f->element].effects == dt_action_effect_value)
    {
      f->action = &_value_action;
      existing = g_sequence_search(darktable.control->shortcuts, f, _shortcut_compare_func, v);
      while(_shortcut_closest_match(&existing, f, &matched, def, fb_log) && !matched) {};
    }

    if(f->move && !f->move_device && !(f->mods || f->press || f->button || f->click))
    {
      if(*fb_log)
        *fb_log = dt_util_dstrcat(*fb_log, "\n%s \u2192 %s", _shortcut_description(f), _("fallback to move"));

      f->effect = DT_ACTION_EFFECT_DEFAULT_MOVE;
      f->move = 0;
    }

    f->action = matched_action;
  }

  return f->action != NULL && !f->move;
}


static float _process_action(dt_action_t *action,
                             int instance,
                             dt_action_element_t element,
                             dt_action_effect_t effect,
                             float move_size,
                             gchar **fb_log)
{
  float return_value = DT_ACTION_NOT_VALID;

  dt_action_t *owner = action;
  while(owner && owner->type >= DT_ACTION_TYPE_SECTION) owner = owner->owner;

  gpointer action_target = action->type == DT_ACTION_TYPE_LIB ? action : action->target;

  if(owner && owner->type == DT_ACTION_TYPE_IOP)
  {
    // find module instance
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)owner;

    if(owner == &darktable.control->actions_focus)
    {
      action_target = darktable.develop->gui_module;
      if(!action_target)
        return return_value;
    }
    else if(instance)
    {
      int current_instance = abs(instance);

      dt_iop_module_t *mod = NULL;

      for(GList *iop_mods = instance >= 0
                          ? darktable.develop->iop
                          : g_list_last(darktable.develop->iop);
          iop_mods;
          iop_mods = instance >= 0
                  ? g_list_next(iop_mods)
                  : g_list_previous(iop_mods))
      {
        mod = (dt_iop_module_t *)iop_mods->data;

        if(mod->so == module
            && mod->iop_order != INT_MAX
            && !--current_instance)
          break;
      }

      // find module instance widget
      if(mod && action->type >= DT_ACTION_TYPE_PER_INSTANCE)
      {
        for(GSList *w = mod->widget_list; w; w = w->next)
        {
          const dt_action_target_t *referral = w->data;
          if(referral->action == action)
          {
            action_target = referral->target;
            break;
          }
        }
      }
      else
        action_target = mod;
    }
    else if(action->type == DT_ACTION_TYPE_IOP
            || action->type == DT_ACTION_TYPE_PRESET)
    {
      action_target = dt_iop_get_module_preferred_instance((dt_iop_module_so_t *)owner);
    }
  }

  if(action->type == DT_ACTION_TYPE_COMMAND && action->target && DT_PERFORM_ACTION(move_size))
  {
    ((dt_action_callback_t*)action->target)(action);
  }
  else if(action->type == DT_ACTION_TYPE_PRESET && owner && DT_PERFORM_ACTION(move_size))
  {
    if(owner->type == DT_ACTION_TYPE_LIB)
    {
      const dt_lib_module_t *lib = (dt_lib_module_t *)owner;
      dt_lib_presets_apply(action->label, lib->plugin_name, lib->version());
    }
    else if(owner->type == DT_ACTION_TYPE_IOP)
    {
      dt_action_widget_toast(action_target, NULL, "\napplying preset '%s'", action->label);

      dt_gui_presets_apply_preset(action->label, action_target);
    }
    else
      dt_print(DT_DEBUG_ALWAYS,
               "[process_action] preset '%s' has unsupported type\n", action->label);
  }
  else
  {
    const dt_action_def_t *definition = _action_find_definition(action);

    if(definition && definition->process
        && (action->type < DT_ACTION_TYPE_WIDGET
            || definition->no_widget
            || (action_target && !dt_action_widget_invisible(action_target))))
    {
      if(DT_PERFORM_ACTION(move_size) &&
         (definition->elements[element].effects != dt_action_effect_value
          || effect != DT_ACTION_EFFECT_SET))
      {
        dt_shortcut_t s = { .action = action };
        GSequenceIter *speed_adjustment
          = g_sequence_lookup(darktable.control->shortcuts, &s, _shortcut_compare_func, NULL);
        if(speed_adjustment)
        {
          dt_shortcut_t *f = g_sequence_get(speed_adjustment);

          move_size *= f->speed;

          if(*fb_log)
            *fb_log = dt_util_dstrcat(*fb_log, "\n%s \u2192 %s = %g",
                                      _action_description(f, 2), _("speed"), move_size);
        }
      }
      return_value = definition->process(action_target, element, effect, move_size);
    }
#ifdef USE_LUA
    else if(owner == &darktable.control->actions_lua && definition)
    {
      dt_lua_lock();

      lua_State* L= darktable.lua_state.state;

      lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_mimic_list");
      int stacknum = 1;
      if(lua_isnil(L, -1)) goto lua_end;

      lua_getfield(L, -1, action->id);
      ++stacknum;
      if(lua_isnil(L, -1)) goto lua_end;

      if(!DT_PERFORM_ACTION(move_size))
        move_size = NAN;

      lua_pushstring(L, action->label);
      lua_pushstring(L, definition->elements[element].name);
      lua_pushstring(L, definition->elements[element].effects[effect]);
      lua_pushnumber(L, move_size);

      lua_pcall(L, 4, 1, 0);

      return_value = lua_tonumber(L, -1);

      if(dt_isnan(return_value))
        return_value = DT_ACTION_NOT_VALID;

lua_end:
      lua_pop(L, stacknum);
      dt_lua_unlock();
    }
#endif
    else if(DT_PERFORM_ACTION(move_size))
      dt_action_widget_toast(action, action_target, "not active");
  }

  return return_value;
}

static void _ungrab_grab_widget()
{
  gdk_seat_ungrab(gdk_display_get_default_seat(gdk_display_get_default()));

  g_slist_free_full(_pressed_keys, g_free);
  _pressed_keys = NULL;

  if(_grab_widget)
  {
    gtk_widget_set_sensitive(_grab_widget, TRUE);
    gtk_widget_set_tooltip_text(_grab_widget, NULL);
    g_signal_handlers_disconnect_by_func(gtk_widget_get_toplevel(_grab_widget), G_CALLBACK(dt_shortcut_dispatcher), NULL);
    _grab_widget = NULL;
  }
}

static void _ungrab_at_focus_loss()
{
  _grab_window = NULL;
  _focus_loss_key = _sc.key;
  _focus_loss_press = _sc.press;
  _ungrab_grab_widget();
  _sc = (dt_shortcut_t) { 0 };
}

static float _process_shortcut(float move_size)
{
  float return_value = DT_ACTION_NOT_VALID;

  dt_print(DT_DEBUG_INPUT | DT_DEBUG_VERBOSE,
            "  [_process_shortcut] processing shortcut: %s\n",
            _shortcut_description(&_sc));

  dt_shortcut_t fsc = _sc;
  fsc.action = NULL;
  fsc.element  = 0;

  gchar *fb_log = darktable.control->mapping_widget && DT_PERFORM_ACTION(move_size)
                ? g_strdup_printf("[ %s ]", _shortcut_description(&fsc))
                : NULL;

  if(_shortcut_match(&fsc, &fb_log))
  {
    if(DT_PERFORM_ACTION(move_size))
      move_size *= fsc.speed;

    if(fsc.effect == DT_ACTION_EFFECT_DEFAULT_MOVE)
    {
      if(DT_PERFORM_ACTION(move_size) && move_size < .0f)
      {
        fsc.effect = DT_ACTION_EFFECT_DEFAULT_DOWN;
        move_size *= -1;
      }
      else
        fsc.effect = DT_ACTION_EFFECT_DEFAULT_UP;
    }

    return_value =  _process_action(fsc.action, fsc.instance, fsc.element, fsc.effect, move_size, &fb_log);
  }
  else if(DT_PERFORM_ACTION(move_size) && !fsc.action)
  {
    dt_toast_log(_("%s not assigned"), _shortcut_description(&_sc));
  }

  if(fb_log)
  {
    dt_control_log("%s", fb_log);
    g_free(fb_log);
  }

  return return_value;
}

float dt_action_process(const gchar *action,
                        int instance,
                        const gchar *element,
                        const gchar *effect,
                        float move_size)
{
  gchar **path = g_strsplit(action, "/", 0);
  dt_action_t *ac = dt_action_locate(NULL, path, FALSE);
  g_strfreev(path);

  if(!ac)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dt_action_process] action path '%s' not found\n", action);
    return DT_ACTION_NOT_VALID;
  }

  if(ac->owner == &darktable.control->actions_lua)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_action_process] lua action '%s' triggered from lua\n", action);
    return DT_ACTION_NOT_VALID;
  }

  const dt_view_type_flags_t vws = _find_views(ac);
  if(!(vws & darktable.view_manager->current_view->view(darktable.view_manager->current_view)))
  {
    if(DT_PERFORM_ACTION(move_size))
      dt_print(DT_DEBUG_ALWAYS,
              "[dt_action_process] action '%s' not valid for current view\n", action);
    return DT_ACTION_NOT_VALID;
  }

  dt_action_element_t el = DT_ACTION_ELEMENT_DEFAULT;
  dt_action_effect_t ef = DT_ACTION_EFFECT_DEFAULT_KEY;
  if((element && *element) || (effect && *effect))
  {
    const dt_action_element_def_t *elements = _action_find_elements(ac);
    if(elements)
    {
      if(elements == _action_elements_entry && (_entry_set_element = element)
         && !g_ascii_strcasecmp("set", effect))
        return _process_action(ac, instance, 0, DT_ACTION_EFFECT_SET, move_size, NULL);

      if(element && *element)
      {
        while(elements[el].name && g_ascii_strcasecmp(elements[el].name, element)) el++;

        if(!elements[el].name)
        {
          dt_print(DT_DEBUG_ALWAYS,
                   "[dt_action_process] element '%s' not valid for action '%s'\n",
                   element, action);
          return DT_ACTION_NOT_VALID;
        }
      }

      const gchar **effects = elements[el].effects;
      if(effect && *effect && !_find_combo_effect(effects, effect, ac, &ef))
      {
        while(effects[ef] && g_ascii_strcasecmp(effects[ef], effect)) ef++;

        if(!effects[ef])
        {
          dt_print(DT_DEBUG_ALWAYS,
                   "[dt_action_process] effect '%s' not valid for action '%s'\n",
                   effect, action);
          return DT_ACTION_NOT_VALID;
        }
      }
    }
  }

  return _process_action(ac, instance, el, ef, move_size, NULL);
}

static gint _cmp_key(const gconstpointer a, const gconstpointer b)
{
  const dt_device_key_t *key_a = a;
  const dt_device_key_t *key_b = b;
  return key_a->key_device != key_b->key_device || key_a->key != key_b->key;
}

static inline void _interrupt_delayed_release(gboolean trigger)
{
  if(_timeout_source)
  {
    g_source_remove(_timeout_source);
    _timeout_source = 0;

    if(trigger)
      dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, 0, DT_SHORTCUT_MOVE_NONE, 1);

    _sc.button = _pressed_button;
    _sc.click = 0;
  }
}

static guint _key_modifiers_clean(guint mods)
{
  GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());
  mods &= GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_MOD5_MASK |
          gdk_keymap_get_modifier_mask(keymap, GDK_MODIFIER_INTENT_PRIMARY_ACCELERATOR);
  return mods | dt_modifier_shortcuts;
}

float dt_shortcut_move(dt_input_device_t id, guint time, guint move, float move_size)
{
  if(DT_PERFORM_ACTION(move_size))
    _interrupt_delayed_release(TRUE); // reenters dt_shortcut_move

  _sc.move_device = id;
  _sc.move = move;
  _sc.speed = 1.0;
  _sc.direction = 0;

  if(_shortcut_is_move(&_sc))
  {
    _sc.effect =  DT_ACTION_EFFECT_DEFAULT_MOVE;
    _sc.direction = move_size > 0 ? DT_SHORTCUT_UP : DT_SHORTCUT_DOWN;
  }
  else
    _sc.effect = DT_ACTION_EFFECT_DEFAULT_KEY;

  if(id != DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE)
    _sc.mods = _key_modifiers_clean(dt_key_modifier_state());

  float return_value = 0;
  if(!DT_PERFORM_ACTION(move_size))
    return_value = _process_shortcut(move_size);
  else
  {
    gboolean key_or_button_released = (id == DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE
                                      && move == DT_SHORTCUT_MOVE_NONE);
    _previous_move = move;

    if(!key_or_button_released)
      _last_time = 0;

    if(_grab_widget) // in mapping mode end grab immediately after first shortcut
      _ungrab_grab_widget();

    dt_print(DT_DEBUG_INPUT,
             "  [dt_shortcut_move] shortcut received: %s\n",
             _shortcut_description(&_sc));

    _lookup_mapping_widget();

    if(_sc.action)
    {
      if(!time
         || time > _last_mapping_time + 1000
         || time < _last_mapping_time)
      {
        _last_mapping_time = time;

        // mapping_widget gets cleared by confirmation dialog focus loss
        GtkWidget *mapped_widget = darktable.control->mapping_widget;

        dt_shortcut_t s = _sc;
        if(_insert_shortcut(&s, darktable.control->confirm_mapping))
        {
          dt_control_log(_("%s assigned to %s"),
                         _shortcut_description(&s), _action_description(&s, 2));

          if(mapped_widget)
            gtk_widget_trigger_tooltip_query(mapped_widget);
        }

        dt_shortcuts_save(NULL, FALSE);
      }

      _sc.action = NULL;
      _sc.instance = 0;
    }
    else
    {
      if(!_pressed_keys || (key_or_button_released && !_sc.button))
        return_value = _process_shortcut(move_size);
      else
      {
        // pressed_keys can be emptied if losing grab during processing
        for(GSList *k = _pressed_keys; k; k = _pressed_keys ? k->next : NULL)
        {
          const dt_device_key_t *device_key = k->data;
          _sc.key_device = device_key->key_device;
          _sc.key = device_key->key;

          return_value = _process_shortcut(move_size);
        }
      }
    }
  }

  _sc.move_device = 0;
  _sc.move = DT_SHORTCUT_MOVE_NONE;
  _sc.direction = 0;

  return return_value;
}

static gboolean _key_release_delayed(gpointer timed_out)
{
  _timeout_source = 0;

  if(!_pressed_keys)
    _ungrab_grab_widget();

  if(!timed_out)
    dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, 0, DT_SHORTCUT_MOVE_NONE, 1);

  if(!_pressed_keys)
    _sc = (dt_shortcut_t) { 0 };

  return G_SOURCE_REMOVE;
}

static gboolean _button_release_delayed(gpointer timed_out)
{
  _timeout_source = 0;

  if(!timed_out)
    dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, 0, DT_SHORTCUT_MOVE_NONE, 1);

  _sc.button = _pressed_button;
  _sc.click = 0;

  return G_SOURCE_REMOVE;
}

gboolean break_stuck = FALSE;

void dt_shortcut_key_press(dt_input_device_t id, guint time, guint key)
{
  dt_device_key_t this_key = { id, key };

  if(g_slist_find_custom(_pressed_keys, &this_key, _cmp_key))
  {
    // if key is still repeating (after return from popup menu) then restore double/triple press state
    if(id == DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE && key == _focus_loss_key && time < _last_time + 50)
      _sc.press = _focus_loss_press;
    _focus_loss_key = 0;
  }
  else if(g_slist_find_custom(_hold_keys, &this_key, _cmp_key))
  {} // ignore repeating hold key
  else
  {
    if(id) _sc.mods = _key_modifiers_clean(dt_key_modifier_state());

    dt_shortcut_t just_key
      = { .key_device = id,
          .key = key,
          .mods = _sc.mods,
          .views = darktable.view_manager->current_view->view(darktable.view_manager->current_view) };

    dt_shortcut_t *s = NULL;
    GSequenceIter *existing
      = g_sequence_lookup(darktable.control->shortcuts, &just_key,
                          _shortcut_compare_func, GINT_TO_POINTER(just_key.views));
    if(existing)
      s = g_sequence_get(existing);
    else
    {
      just_key.mods = 0; // fall back to key without modifiers (for multiple emulated modifiers)
      existing = g_sequence_lookup(darktable.control->shortcuts, &just_key,
                                   _shortcut_compare_func, GINT_TO_POINTER(just_key.views));
      if(existing && (s = g_sequence_get(existing)) &&
         (s->action != darktable.control->actions_modifiers || s->effect != DT_ACTION_EFFECT_HOLD))
        s = NULL;
    }
    if(s
       && !_sc.action
       && s->effect == DT_ACTION_EFFECT_HOLD
       && s->action
       && s->action->type >= DT_ACTION_TYPE_WIDGET
       && !dt_action_widget(darktable.control->mapping_widget))
    {
      const dt_action_def_t *definition = _action_find_definition(s->action);
      if(definition && definition->process
         && definition->elements[s->element].effects == dt_action_effect_hold)
      {
        if(darktable.control->mapping_widget)
        {
          dt_control_log("[ %s ]\n%s \u2192 %s", _shortcut_description(&_sc),
                         _shortcut_description(s), _action_description(s, 2));
        }

        definition->process(NULL, s->element, DT_ACTION_EFFECT_ON, 1);

        this_key.hold_def = definition;
        this_key.hold_element = s->element;

        dt_device_key_t *new_key = calloc(1, sizeof(dt_device_key_t));
        *new_key = this_key;
        _hold_keys = g_slist_prepend(_hold_keys, new_key);

        return;
      }
    }

    gboolean double_press = !dt_gui_long_click(time, _last_time);

    if((id || key)
        && id == _sc.key_device
        && key == _sc.key
        && double_press
        && !(_sc.press & DT_SHORTCUT_TRIPLE))
    {
      _interrupt_delayed_release(FALSE);
      _sc.press += DT_SHORTCUT_DOUBLE;
    }
    else
      _interrupt_delayed_release(TRUE);

    if(!_pressed_keys)
    {
      _lookup_mapping_widget();

      gdk_seat_grab(gdk_display_get_default_seat(gdk_display_get_default()),
                    gtk_widget_get_window(_grab_window ? _grab_window
                                                       : dt_ui_main_window(darktable.gui->ui)),
                    GDK_SEAT_CAPABILITY_ALL, FALSE, NULL, NULL, NULL, NULL);
    }
    else
    {
      if(_sc.action)
      {
        // only one key press allowed while defining shortcut
        _ungrab_grab_widget();
        _sc = (dt_shortcut_t) { 0 };
        return;
      }
    }

    // short press after 2 seconds will clear all keys
    break_stuck = _pressed_keys && time > _last_time + 2000;

    // allow extra time when pressing multiple keys "at same time"
    if(!_pressed_keys || double_press || break_stuck)
      _last_time = time;

    _sc.key_device = id;
    _sc.key = key;
    _sc.button = _pressed_button = 0;
    _sc.click = 0;
    _sc.direction = 0;

    dt_device_key_t *new_key = calloc(1, sizeof(dt_device_key_t));
    *new_key = this_key;
    _pressed_keys = g_slist_prepend(_pressed_keys, new_key);

    // FIXME: make arrow keys repeat; eventually treat up/down and left/right as move
    if(key == GDK_KEY_Left
       || key == GDK_KEY_Right
       || key == GDK_KEY_Up
       || key == GDK_KEY_Down)
    {
      dt_shortcut_key_release(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, time, key);
    }
  }
}

static void _delay_for_double_triple(guint time, guint is_key)
{
  int delay = 0;
  g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &delay, NULL);

  guint passed_time = time - _last_time;

  if(passed_time > delay)
  {
    _sc.press |= DT_SHORTCUT_LONG & is_key;
    _sc.click |= DT_SHORTCUT_LONG & ~is_key;
  }
  else if(break_stuck && !_sc.button)
  {
    _ungrab_grab_widget();
    dt_control_log("short key press resets stuck keys");
    return;
  }
  else if((is_key ? _sc.press : _sc.click) & DT_SHORTCUT_TRIPLE)
    passed_time += delay;
  else if(!_sc.action) // in mapping mode always wait for double/triple
  {
    // detect if any double or triple press shortcuts exist for this key; otherwise skip delay
    _sc.press += DT_SHORTCUT_DOUBLE & is_key;
    _sc.click += DT_SHORTCUT_DOUBLE & ~is_key;

    _sc.views = darktable.view_manager->current_view->view(darktable.view_manager->current_view);
    GSequenceIter *multi = g_sequence_search(darktable.control->shortcuts, &_sc, _shortcut_compare_func,
                                             GINT_TO_POINTER(_sc.views));

    for(int checks = 2; multi; multi = --checks ? g_sequence_iter_prev(multi) : NULL)
    {
      dt_shortcut_t *m = g_sequence_get(multi);
      if(m && m->key_device == _sc.key_device && m->key == _sc.key &&
         (is_key ? m->press >= _sc.press :
                   m->press == _sc.press && m->button == _sc.button && m->click >= _sc.click))
        break;

      if(_sc.click && darktable.control->enable_fallbacks)
      {
        const dt_action_def_t *def = _action_find_definition(m->action);
        if(def && def->fallbacks)
          break;
      }
    }
    if(!multi) passed_time += delay;

    _sc.press -= DT_SHORTCUT_DOUBLE & is_key;
    _sc.click -= DT_SHORTCUT_DOUBLE & ~is_key;
  }

  GSourceFunc delay_func = is_key ? _key_release_delayed
                                  : _button_release_delayed;

  if(passed_time < delay)
    _timeout_source = g_timeout_add(delay - passed_time, delay_func, NULL);
  else
    delay_func(GINT_TO_POINTER(passed_time > 2 * delay)); // call immediately
}

void dt_shortcut_key_release(dt_input_device_t id, guint time, guint key)
{
  dt_device_key_t this_key = { id, key };

  GSList *held_key = g_slist_find_custom(_hold_keys, &this_key, _cmp_key);
  if(held_key)
  {
    dt_device_key_t *held_data = held_key->data;
    held_data->hold_def->process(NULL, held_data->hold_element, DT_ACTION_EFFECT_OFF, 1);
    g_free(held_data);
    _hold_keys = g_slist_delete_link(_hold_keys, held_key);
    return;
  }

  GSList *stored_key = g_slist_find_custom(_pressed_keys, &this_key, _cmp_key);
  if(stored_key)
  {
    _interrupt_delayed_release(TRUE);

    g_free(stored_key->data);
    _pressed_keys = g_slist_delete_link(_pressed_keys, stored_key);

    if(_sc.key_device != id || _sc.key != key)
      break_stuck = FALSE;

    _sc.key_device = id;
    _sc.key = key;

    _delay_for_double_triple(time, -1);
  }
}

gboolean dt_shortcut_key_active(dt_input_device_t id, guint key)
{
  dt_shortcut_t saved_sc = _sc;
  _sc = (dt_shortcut_t) {.key_device = id, .key = key};

  float value = dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, 0, DT_SHORTCUT_MOVE_NONE, DT_READ_ACTION_ONLY);

  _sc = saved_sc;

  return fmodf(value, 1) <= DT_VALUE_PATTERN_ACTIVE || fmodf(value, 2) > .5;
}

static guint _fix_keyval(GdkEvent *event)
{
  guint keyval = 0;
  GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());
  gdk_keymap_translate_keyboard_state(keymap, event->key.hardware_keycode, 0, 0,
                                      &keyval, NULL, NULL, NULL);
  return keyval;
}

gboolean dt_shortcut_dispatcher(GtkWidget *w, GdkEvent *event, gpointer user_data)
{
  if((event->type ==  GDK_BUTTON_PRESS || event->type ==  GDK_BUTTON_RELEASE ||
      event->type ==  GDK_DOUBLE_BUTTON_PRESS || event->type ==  GDK_TRIPLE_BUTTON_PRESS)
     && event->button.button > 7)
  {
    if(event->type == GDK_BUTTON_RELEASE)
      dt_shortcut_key_release(DT_SHORTCUT_DEVICE_TABLET, event->button.time, event->button.button - 7);
    else
      dt_shortcut_key_press  (DT_SHORTCUT_DEVICE_TABLET, event->button.time, event->button.button - 7);

    return TRUE;
  }

  if(_pressed_keys == NULL)
  {
    dt_shortcut_t s = { .action = _sc.action };
    gboolean middle_click = event->type == GDK_BUTTON_PRESS && event->button.button == GDK_BUTTON_MIDDLE;
    if((middle_click || event->type == GDK_SCROLL) &&
       (s.action || (s.action = dt_action_widget(darktable.control->mapping_widget))))
    {
      int delta;
      if(middle_click || dt_gui_get_scroll_unit_delta(&event->scroll, &delta))
      {
        s.speed = middle_click ? -1 : powf(10.0f, delta);

        if(_insert_shortcut(&s, TRUE))
          dt_control_log("%s", _action_description(&s, 2));

        dt_shortcuts_save(NULL, FALSE);
      }

      return TRUE;
    }

    if(_grab_widget && event->type == GDK_BUTTON_PRESS)
    {
      _ungrab_grab_widget();
      _sc = (dt_shortcut_t) { 0 };
      return TRUE;
    }

    if(event->type != GDK_KEY_PRESS && event->type != GDK_KEY_RELEASE &&
       event->type != GDK_FOCUS_CHANGE)
      return FALSE;

    if(GTK_IS_WINDOW(w) &&
       (event->type == GDK_KEY_PRESS || event->type == GDK_KEY_RELEASE))
    {
      GtkWidget *focused_widget = gtk_window_get_focus(GTK_WINDOW(w));
      if(focused_widget)
      {
        if(gtk_widget_event(focused_widget, event))
          return TRUE;

        if((GTK_IS_ENTRY(focused_widget) || GTK_IS_TREE_VIEW(focused_widget)) &&
           (event->key.keyval == GDK_KEY_Tab ||
            event->key.keyval == GDK_KEY_KP_Tab ||
            event->key.keyval == GDK_KEY_ISO_Left_Tab))
          return FALSE;
      }
    }
  }

  switch(event->type)
  {
  case GDK_KEY_PRESS:
    if(event->key.is_modifier
       || event->key.keyval == GDK_KEY_VoidSymbol
       || event->key.keyval == GDK_KEY_Meta_L
       || event->key.keyval == GDK_KEY_Meta_R
       || event->key.keyval == GDK_KEY_ISO_Level3_Shift)
      return FALSE;

    _sc.mods = _key_modifiers_clean(event->key.state);

    // FIXME: for vimkeys and game. Needs generalising for non-bauhaus/non-darkroom
    if(!_grab_widget && !darktable.control->mapping_widget &&
       dt_control_key_pressed_override(event->key.keyval, dt_gui_translated_key_state(&event->key)))
      return TRUE;

    dt_shortcut_key_press(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, event->key.time, _fix_keyval(event));
    break;
  case GDK_KEY_RELEASE:
    if(event->key.is_modifier || event->key.keyval == GDK_KEY_ISO_Level3_Shift)
    {
      // are we defining shortcuts for fallbacks? just modifiers can be used.
      if(_sc.action && _sc.action->type == DT_ACTION_TYPE_FALLBACK)
      {
        _sc.mods = _key_modifiers_clean(event->key.state);
        dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, 0, DT_SHORTCUT_MOVE_NONE, 1);
      }
      return FALSE;
    }

    dt_shortcut_key_release(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, event->key.time, _fix_keyval(event));
    break;
  case GDK_GRAB_BROKEN:
    if(!event->grab_broken.implicit)
      _ungrab_at_focus_loss();
    return FALSE;
  case GDK_WINDOW_STATE:
    if(!(event->window_state.new_window_state & GDK_WINDOW_STATE_FOCUSED))
      _ungrab_at_focus_loss();
    return FALSE;
  case GDK_FOCUS_CHANGE: // dialog boxes and switch to other app release grab
    if(event->focus_change.in)
      g_set_weak_pointer(&_grab_window, w);
    else
      _ungrab_at_focus_loss();
    return FALSE;
  case GDK_SCROLL:
    _sc.mods = _key_modifiers_clean(event->scroll.state);

    int delta_x, delta_y;
    if(dt_gui_get_scroll_unit_deltas(&event->scroll, &delta_x, &delta_y))
    {
      if(delta_x)
        dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, event->scroll.time,
                         DT_SHORTCUT_MOVE_PAN, -delta_x);
      if(delta_y)
        dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, event->scroll.time,
                         DT_SHORTCUT_MOVE_SCROLL, -delta_y);
    }
    break;
  case GDK_MOTION_NOTIFY:
    ;
    static gdouble move_start_x = 0, move_start_y = 0, last_distance = 0;

    const gdouble x_move = event->motion.x - move_start_x;
    const gdouble y_move = event->motion.y - move_start_y;
    const gdouble new_distance = x_move * x_move + y_move * y_move;

    static int move_last_time = 0;
    if(move_last_time != _last_time || new_distance < last_distance)
    {
      move_start_x = event->motion.x;
      move_start_y = event->motion.y;
      move_last_time = _last_time;
      last_distance = 0;
      break;
    }

    // might just be an accidental move during a key press or button click
    // possibly different time sources from midi or other devices
    if(event->motion.time > _last_time && !dt_gui_long_click(event->motion.time, _last_time))
      break;

    _sc.mods = _key_modifiers_clean(event->motion.state);

    const gdouble step_size = 10;

    const gdouble angle = x_move / (0.001 + y_move);
    gdouble size = trunc(x_move / step_size);
    gdouble y_size = - trunc(y_move / step_size);

    if(size != 0 || y_size != 0)
    {
      guint move = DT_SHORTCUT_MOVE_HORIZONTAL;
      if(fabs(angle) >= 2)
      {
        move_start_x += size * step_size;
        move_start_y = event->motion.y;
      }
      else
      {
        size = y_size;
        move_start_y -= size * step_size;
        if(fabs(angle) < .5)
        {
          move_start_x = event->motion.x;
          move = DT_SHORTCUT_MOVE_VERTICAL;
        }
        else
        {
          move_start_x -= size * step_size * angle;
          move = angle < 0 ? DT_SHORTCUT_MOVE_SKEW : DT_SHORTCUT_MOVE_DIAGONAL;
        }
      }

      if(_previous_move == move || _previous_move == DT_SHORTCUT_MOVE_NONE)
        dt_shortcut_move(DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE, event->motion.time, move, size);
      else
        _previous_move = move;
    }
    break;
  case GDK_BUTTON_PRESS:
    _sc.mods = _key_modifiers_clean(event->button.state);

    _pressed_button |= 1 << (event->button.button - 1);
    _interrupt_delayed_release(_sc.button != _pressed_button);
    _sc.button = _pressed_button;
    _sc.click = 0;
    _last_time = event->button.time;
    break;
  case GDK_DOUBLE_BUTTON_PRESS:
    _sc.click |= DT_SHORTCUT_DOUBLE;
    break;
  case GDK_TRIPLE_BUTTON_PRESS:
    _sc.click |= DT_SHORTCUT_TRIPLE;
    break;
  case GDK_BUTTON_RELEASE:
    _pressed_button &= ~(1 << (event->button.button - 1));

    _interrupt_delayed_release(FALSE);

    _delay_for_double_triple(event->button.time, 0);

    _last_time = 0; // important; otherwise releasing two buttons will trigger two actions
                    // also, we seem to be receiving presses and releases twice!?! FIXME
    break;
  default:
    return FALSE;
  }

  return TRUE;
}

static void _remove_widget_from_hashtable(GtkWidget *widget, gpointer user_data)
{
  dt_action_t *action = dt_action_widget(widget);
  if(action)
  {
    if(action->target == widget) action->target = NULL;

    g_hash_table_remove(darktable.control->widgets, widget);
  }
}

static inline gchar *path_without_symbols(const gchar *path)
{
  return g_strdelimit(g_strndup(path, strlen(path) - (g_str_has_suffix(path, "...")?3:0)), "=,/.;", '-');
}

void dt_action_insert_sorted(dt_action_t *owner, dt_action_t *new_action)
{
  new_action->owner = owner;

  dt_action_t **insertion_point = (dt_action_t **)&owner->target;

  while(*insertion_point
        && g_ascii_strcasecmp(new_action->id, "preset")
        && (!g_ascii_strcasecmp((*insertion_point)->id, "preset")
            || g_utf8_collate((*insertion_point)->label, new_action->label) <
                 ((*((*insertion_point)->label) == '<' ? 1000 : 0) -
                  (*(        new_action->label) == '<' ? 1000 : 0))))
  {
    insertion_point = &(*insertion_point)->next;
  }
  new_action->next = *insertion_point;
  *insertion_point = new_action;
}

dt_action_t *dt_action_locate(dt_action_t *owner, gchar **path, gboolean create)
{
  gchar *clean_path = NULL;

  dt_action_t *action = owner ? owner->target : darktable.control->actions;
  while(*path)
  {
    if(owner == &darktable.control->actions_lua) create = TRUE;

    const gboolean needs_translation =
      !owner
      || owner->type != DT_ACTION_TYPE_SECTION
      || (g_ascii_strcasecmp(owner->id, "styles") && g_ascii_strcasecmp(owner->id, "preset"));

    const gchar *id_start = needs_translation ? NQ_(*path) : *path;

    if(!clean_path) clean_path = path_without_symbols(id_start);

    if(!action)
    {
      if(!owner || !create)
      {
        dt_print(DT_DEBUG_ALWAYS, "[dt_action_locate] action '%s' %s\n", *path,
                !owner ? "not valid base node" : "doesn't exist");
        g_free(clean_path);
        return NULL;
      }

      dt_action_t *new_action = calloc(1, sizeof(dt_action_t));
      new_action->id = clean_path;
      new_action->label = g_strdup(needs_translation ? Q_(*path) : *path);
      new_action->type = DT_ACTION_TYPE_SECTION;

      dt_action_insert_sorted(owner, new_action);

      owner = new_action;
      action = NULL;
    }
    else if(!g_ascii_strcasecmp(action->id, clean_path))
    {
      g_free(clean_path);
      owner = action;
      action = action->target;
    }
    else
    {
      action = action->next;
      continue;
    }
    clean_path = NULL; // now owned by action or freed
    path++;
  }

  if(owner)
  {
    if(owner->type <= DT_ACTION_TYPE_VIEW)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_action_locate] found action '%s' internal node\n", owner->id);
      return NULL;
    }
  }

  return owner;
}

static gboolean _reset_element_on_leave(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  darktable.control->element = -1;
  return FALSE;
}

dt_action_t *dt_action_define(dt_action_t *owner,
                              const gchar *section,
                              const gchar *label,
                              GtkWidget *widget,
                              const dt_action_def_t *action_def)
{
  if(owner->type == DT_ACTION_TYPE_IOP_INSTANCE)
  {
    return dt_action_define_iop((dt_iop_module_t *)owner, section, label, widget, action_def);
  }

  dt_action_t *ac = owner;

  if(label)
  {
    const gchar *path[] = { section, label, NULL };
    ac = dt_action_locate(owner, (gchar**)&path[section ? 0 : 1], TRUE);
  }

  if(ac)
  {
    if(label)
    {
      guint index = 0;
      if(g_ptr_array_find(darktable.control->widget_definitions, action_def, &index))
        ac->type = DT_ACTION_TYPE_WIDGET + index + 1;
      else if(action_def == &_action_def_dummy)
        ac->type = DT_ACTION_TYPE_WIDGET;
      else if(action_def)
      {
        ac->type = DT_ACTION_TYPE_WIDGET + darktable.control->widget_definitions->len + 1;
        g_ptr_array_add(darktable.control->widget_definitions, (gpointer)action_def);

        dt_action_define_fallback(ac->type, action_def);
      }
    }

    if(action_def && action_def->no_widget)
    {
      ac->target = widget;
    }
    else if(!darktable.control->accel_initialising && widget)
    {
      if(label && action_def && !ac->target) ac->target = widget;
      g_hash_table_insert(darktable.control->widgets, widget, ac);

      gtk_widget_set_has_tooltip(widget, TRUE);
      g_signal_connect(G_OBJECT(widget), "leave-notify-event", G_CALLBACK(_reset_element_on_leave), NULL);
      g_signal_connect(G_OBJECT(widget), "destroy", G_CALLBACK(_remove_widget_from_hashtable), NULL);
    }
  }

  return ac;
}

dt_action_t *dt_action_define_iop(dt_iop_module_t *self,
                                  const gchar *section,
                                  const gchar *label,
                                  GtkWidget *widget,
                                  const dt_action_def_t *action_def)
{
  // add to module_so or blending actions list
  dt_action_t *ac = NULL;
  if(section && g_str_has_prefix(section, "blend"))
  {
    const char *subsection = section[strlen("blend")] ? section + strlen("blend") + 1 : NULL;
    ac = dt_action_define(&darktable.control->actions_blend, subsection, label, widget, action_def);
  }
  else
  {
    ac = dt_action_define(&self->so->actions, section, label, widget,
                          action_def ? action_def : &_action_def_dummy);
  }

  // to support multi-instance, also save in per instance widget list
  dt_action_target_t *referral = g_malloc0(sizeof(dt_action_target_t));
  referral->action = ac;
  referral->target = widget;
  self->widget_list = g_slist_prepend(self->widget_list, referral);

  return ac;
}

static GdkModifierType _mods_fix_primary(GdkModifierType mods)
{
  // FIXME move to darktable.h (?) and use there too in dt_modifier_is and dt_modifiers_include
  // use global variable?
  GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());
  if(mods & GDK_CONTROL_MASK)
    return (mods & ~GDK_CONTROL_MASK)
           | gdk_keymap_get_modifier_mask(keymap, GDK_MODIFIER_INTENT_PRIMARY_ACCELERATOR);
  else
    return mods;
}

void dt_action_define_fallback(dt_action_type_t type, const dt_action_def_t *action_def)
{
  const dt_shortcut_fallback_t *f = action_def->fallbacks;
  if(f)
  {
    const gchar *fallback_path[] = { action_def->name, NULL };
    dt_action_t *fb = dt_action_locate(&darktable.control->actions_fallbacks, (gchar**)fallback_path, TRUE);
    fb->type = DT_ACTION_TYPE_FALLBACK;
    fb->target = GINT_TO_POINTER(type);

    while(f->mods || f->press || f->button || f->click || f->direction || f->move)
    {
      dt_shortcut_t s = { .mods = _mods_fix_primary(f->mods),
                          .press = f->press,
                          .button = f->button,
                          .click = f->click,
                          .direction = f->direction,
                          .move = f->move,
                          .element = f->element,
                          .effect = f->effect,
                          .action = fb,
                          .speed = f->speed ? f->speed : 1.0 };

      _insert_shortcut(&s, FALSE);

      f++;
    }
  }
}

void dt_shortcut_register(dt_action_t *owner,
                          guint element,
                          guint effect,
                          guint accel_key,
                          GdkModifierType mods)
{
  if(accel_key != 0)
  {
    GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());

    GdkKeymapKey *keys;
    gint n_keys, i = 0;

    if(!gdk_keymap_get_entries_for_keyval(keymap, accel_key, &keys, &n_keys)) return;

    for(int j = 0; j < n_keys; j++)
    {
      gdk_keymap_translate_keyboard_state(keymap, keys[j].keycode, 0, 0, &keys[j].keycode, NULL, NULL, NULL);

      if(_is_kp_key(keys[j].keycode))
        keys[j].group = 10;

      if(keys[j].group < keys[i].group || (keys[j].group == keys[i].group && keys[j].level < keys[i].level))
        i = j;
    }

    if(keys[i].level & 1) mods |= GDK_SHIFT_MASK;
    if(keys[i].level & 2) mods |= GDK_MOD5_MASK;

    mods = _mods_fix_primary(mods);

    dt_shortcut_t s = { .key_device = DT_SHORTCUT_DEVICE_KEYBOARD_MOUSE,
                        .key = keys[i].keycode,
                        .mods = mods,
                        .speed = 1.0,
                        .action = owner,
                        .element = element,
                        .effect = effect };

    _insert_shortcut(&s, FALSE);

    g_free(keys);
  }
}

void dt_action_define_preset(dt_action_t *action, const gchar *name)
{
  gchar *path[3] = { "preset", (gchar *)name, NULL };
  dt_action_t *const p = dt_action_locate(action, path, TRUE);
  if(p)
  {
    p->type = DT_ACTION_TYPE_PRESET;
    p->target = (gpointer)TRUE;
  }
}

void dt_action_rename(dt_action_t *action, const gchar *new_name)
{
  if(!action) return;

  g_free((char*)action->id);
  g_free((char*)action->label);

  dt_action_t **previous = (dt_action_t **)&action->owner->target;
  while(*previous)
  {
    if(*previous == action)
    {
      *previous = action->next;
      break;
    }
    previous = &(*previous)->next;
  }

  if(new_name)
  {
    action->id = path_without_symbols(new_name);
    action->label = g_strdup(_(new_name));

    dt_action_insert_sorted(action->owner, action);
  }
  else
  {
    GSequenceIter *iter = g_sequence_get_begin_iter(darktable.control->shortcuts);
    while(!g_sequence_iter_is_end(iter))
    {
      GSequenceIter *const current = iter;
      iter = g_sequence_iter_next(iter); // remove will invalidate

      dt_shortcut_t *s = g_sequence_get(current);
      if(s->action == action)
        _remove_shortcut(current);
    }

    g_free(action);
  }

  dt_shortcuts_save(NULL, FALSE);
}

void dt_action_rename_preset(dt_action_t *action,
                             const gchar *old_name,
                             const gchar *new_name)
{
  gchar *path[3] = { "preset", (gchar *)old_name, NULL };
  dt_action_t *p = dt_action_locate(action, path, FALSE);
  if(p)
  {
    if(!new_name)
    {
      if(_actions_store)
        gtk_tree_model_foreach(GTK_TREE_MODEL(_actions_store), _remove_shortcut_from_store, p);
    }

    dt_action_rename(p, new_name);
  }
}

void dt_action_widget_toast(dt_action_t *action,
                            GtkWidget *widget,
                            const gchar *msg,
                            ...)
{
  if(!darktable.gui->reset)
  {
    va_list ap;
    va_start(ap, msg);
    char *text = g_strdup_vprintf(msg, ap);

    if(!action)
      action = dt_action_widget(widget);
    if(action)
    {
      gchar *instance_name = "";
      gchar *label = NULL;

      if(action->type == DT_ACTION_TYPE_IOP_INSTANCE)
      {
        dt_iop_module_t *module = (dt_iop_module_t *)action;

        action = DT_ACTION(module->so);
        instance_name = module->multi_name;

        for(GSList *w = module->widget_list; w; w = w->next)
        {
          dt_action_target_t *referral = w->data;
          if(referral->target == widget)
          {
            if(referral->action->owner == &darktable.control->actions_blend)
            {
              _action_distinct_label(&label, referral->action, NULL);
            }
            else
              action = referral->action;
            break;
          }
        }
      }

      _action_distinct_label(&label, action, instance_name);
      dt_toast_log("%s : %s", label, text);
      g_free(label);
    }
    else
      dt_toast_log("%s", text);

    g_free(text);
    va_end(ap);
  }
}

float dt_accel_get_speed_multiplier(GtkWidget *widget, guint state)
{
  const int slider_precision = dt_conf_get_int("accel/slider_precision");
  float multiplier
    = dt_conf_get_float(slider_precision == DT_IOP_PRECISION_FINE   ? "darkroom/ui/scale_precise_step_multiplier" :
                        slider_precision == DT_IOP_PRECISION_COARSE ? "darkroom/ui/scale_rough_step_multiplier" :
                                                                      "darkroom/ui/scale_step_multiplier");

  if(state != GDK_MODIFIER_MASK)
  {
    dt_shortcut_t s = { .action = &_value_action, .mods = _key_modifiers_clean(state) };
    dt_action_t *wac = dt_action_widget(widget);
    while(s.action)
    {
      GSequenceIter *speed_adjustment
        = g_sequence_lookup(darktable.control->shortcuts, &s, _shortcut_compare_func, NULL);
      if(speed_adjustment)
      {
        const dt_shortcut_t *const f = g_sequence_get(speed_adjustment);

        multiplier *= f->speed;
      }
      s.action = wac;
      s.mods = 0;
      wac = NULL;
    }
  }

  return multiplier;
}

// FIXME possibly just find correct widget for each shortcut execution, rather than updating for each focus change etc
void dt_accel_connect_instance_iop(dt_iop_module_t *module)
{
  const gboolean focused = darktable.develop->gui_module
                           && darktable.develop->gui_module->so == module->so;
  const dt_action_t *const blend = &darktable.control->actions_blend;
  for(GSList *w = module->widget_list; w; w = w->next)
  {
    const dt_action_target_t *const referral = w->data;
    dt_action_t *const ac = referral->action;
    if(focused || (ac->owner != blend && ac->owner->owner != blend))
      ac->target = referral->target;
  }
}

void dt_action_cleanup_instance_iop(dt_iop_module_t *module)
{
  g_slist_free_full(module->widget_list, g_free);
}

GtkWidget *dt_action_button_new(dt_lib_module_t *self,
                                const gchar *label,
                                gpointer callback,
                                gpointer data,
                                const gchar *tooltip,
                                guint accel_key,
                                GdkModifierType mods)
{
  GtkWidget *button = gtk_button_new_with_label(Q_(label));
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), PANGO_ELLIPSIZE_END);
  if(tooltip) gtk_widget_set_tooltip_text(button, tooltip);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(callback), data);

  if(self)
  {
    dt_action_t *ac = dt_action_define(DT_ACTION(self), NULL, label, button, &dt_action_def_button);
    if(accel_key && (self->actions.type != DT_ACTION_TYPE_IOP_INSTANCE
                     || darktable.control->accel_initialising))
      dt_shortcut_register(ac, 0, 0, accel_key, mods);
  }

  return button;
}

GtkWidget *dt_action_entry_new(dt_action_t *ac,
                               const gchar *label,
                               gpointer callback,
                               gpointer data,
                               const gchar *tooltip,
                               const gchar *text)
{
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(entry), 5);
  if(text)
    gtk_entry_set_text (GTK_ENTRY(entry), text);
  if(tooltip)
    gtk_widget_set_tooltip_text(entry, tooltip);
  g_signal_connect(G_OBJECT(entry), "changed", G_CALLBACK(callback), data);

  dt_action_define(ac, NULL, label, entry, &dt_action_def_entry);

  return entry;
}

dt_action_t *dt_action_register(dt_action_t *owner,
                                const gchar *label,
                                dt_action_callback_t callback,
                                guint accel_key,
                                GdkModifierType mods)
{
  dt_action_t *ac = dt_action_section(owner, label);
  if(ac->type == DT_ACTION_TYPE_SECTION)
  {
    ac->type = DT_ACTION_TYPE_COMMAND;
    ac->target = callback;
    dt_shortcut_register(ac, 0, 0, accel_key, mods);
  }

  return ac;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
