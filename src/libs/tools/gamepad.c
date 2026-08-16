/*
    This file is part of darktable,
    copyright (c) 2019-2023 Diederik ter Rahe.

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

#include "common/darktable.h"
#include "common/ratings.h"
#include "common/colorlabels.h"
#include "bauhaus/bauhaus.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "common/file_location.h"
#include <fcntl.h>

DT_MODULE(1)

#ifdef HAVE_SDL

#include <SDL3/SDL.h>

const char *name(dt_lib_module_t *self)
{
  return _("gamepad");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_NONE;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_TOP_CENTER;
}

typedef struct dt_gamepad_device_t
{
  dt_input_device_t id;
  SDL_Gamepad *controller;
  Uint32 timestamp;
  int value[SDL_GAMEPAD_AXIS_COUNT];
  int location[SDL_GAMEPAD_AXIS_COUNT];
} dt_gamepad_device_t;

// SDL3 face buttons are south/east/west/north but share indices 0–3 with former a/b/x/y
static const char *_button_names[]
  = { N_("button a"), N_("button b"), N_("button x"), N_("button y"),
      N_("button back"), N_("button guide"), N_("button start"),
      N_("left stick"), N_("right stick"), N_("left shoulder"), N_("right shoulder"),
      N_("dpad up"), N_("dpad down"), N_("dpad left"), N_("dpad right"),
      N_("button misc1"), N_("paddle1"), N_("paddle2"), N_("paddle3"), N_("paddle4"), N_("touchpad"),
      N_("left trigger"), N_("right trigger"),
      NULL };

static const struct { const char *alias; guint key; } _button_aliases[]
  = { { N_("button south"), 0 }, { N_("button east"), 1 }, { N_("button west"), 2 }, { N_("button north"), 3 },
      { NULL, 0 } };

static gchar *_key_to_string(const guint key,
                             const gboolean display)
{
  const gchar *name = key < SDL_GAMEPAD_BUTTON_COUNT + 2 ? _button_names[key] : N_("invalid gamepad button");
  return g_strdup(display ? _(name) : name);
}

static gboolean _string_to_key(const gchar *string,
                               guint *key)
{
  *key = 0;
  while(_button_names[*key])
    if(!strcmp(_button_names[*key], string))
      return TRUE;
    else
      (*key)++;

  for(int i = 0; _button_aliases[i].alias; i++)
    if(!strcmp(_button_aliases[i].alias, string))
    {
      *key = _button_aliases[i].key;
      return TRUE;
    }

  return FALSE;
}

static const char *_move_names[]
  = { N_("left x"), N_("left y"), N_("right x"), N_("right y"),
      N_("left diagonal"), N_("left skew"), N_("right diagonal"), N_("right skew"),
      NULL };

static gchar *_move_to_string(const guint move,
                              const gboolean display)
{
  const gchar *name = move < SDL_GAMEPAD_AXIS_COUNT + 4 /* diagonals */ ? _move_names[move] : N_("invalid gamepad axis");
  return g_strdup(display ? _(name) : name);
}

static gboolean _string_to_move(const gchar *string,
                                guint *move)
{
  *move = 0;
  while(_move_names[*move])
    if(!strcmp(_move_names[*move], string))
      return TRUE;
    else
      (*move)++;

  return FALSE;
}

static const dt_input_driver_definition_t _driver_definition
  = { "game", _key_to_string, _string_to_key, _move_to_string, _string_to_move };

static gboolean _pump_events(gpointer user_data)
{
  SDL_PumpEvents();

  return G_SOURCE_CONTINUE;
}

static void _process_axis_timestep(dt_gamepad_device_t *gamepad,
                                   const Uint32 timestamp)
{
  if(timestamp > gamepad->timestamp)
  {
    Uint32 time = timestamp - gamepad->timestamp;
    for(SDL_GamepadAxis axis = SDL_GAMEPAD_AXIS_LEFTX; axis <= SDL_GAMEPAD_AXIS_RIGHTY; axis++)
    {
      if(abs(gamepad->value[axis]) > 4000)
        gamepad->location[axis] += time * gamepad->value[axis];
    }
  }

  gamepad->timestamp = timestamp;
}

static void _process_axis_and_send(dt_gamepad_device_t *gamepad,
                                   const Uint32 timestamp)
{
  _process_axis_timestep(gamepad, timestamp);

  const gdouble step_size = 32768 * 1000 / 5; // FIXME configurable, x & y separately

  for(int side = 0; side < 2; side++)
  {
    int stick = SDL_GAMEPAD_AXIS_LEFTX + 2 * side;

    gdouble angle = gamepad->location[stick] / (0.001 + gamepad->location[stick + 1]);

    gdouble size = trunc(gamepad->location[stick] / step_size);

    if(size != 0 && fabs(angle) >= 2)
    {
      gamepad->location[stick] -= size * step_size;
      gamepad->location[stick + 1] = 0;
      dt_shortcut_move(gamepad->id, timestamp, stick, size);
    }
    else
    {
      size = - trunc(gamepad->location[stick + 1] / step_size);
      if(size != 0)
      {
        gamepad->location[stick + 1] += size * step_size;
        if(fabs(angle) < .5)
        {
          gamepad->location[stick] = 0;
          dt_shortcut_move(gamepad->id, timestamp, stick + 1, size);
        }
        else
        {
          gamepad->location[stick] += size * step_size * angle;
          dt_shortcut_move(gamepad->id, timestamp, stick + ((angle < 0) ? 5 : 4), size);
        }
      }
    }
  }
}

static gboolean _poll_devices(gpointer user_data)
{
  dt_lib_module_t *self = user_data;

  SDL_Event event;
  int num_events = 0;

  dt_gamepad_device_t *gamepad = NULL;
  SDL_JoystickID prev_which = -1;

  while(SDL_PollEvent(&event))
  {
    num_events++;

    if(event.gbutton.which != prev_which)
    {
      prev_which = event.gbutton.which;
      SDL_Gamepad *controller = SDL_GetGamepadFromID(prev_which);
      gamepad = NULL;
      for(GSList *gamepads = self->data; gamepads; gamepads = gamepads->next)
        if(((dt_gamepad_device_t *)(gamepads->data))->controller == controller)
        {
          gamepad = gamepads->data;
          break;
        }
      if(!gamepad) return G_SOURCE_REMOVE;
    }

    switch(event.type)
    {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      dt_print(DT_DEBUG_INPUT, "SDL button down event time %u id %u button %hhd down %hhd", (guint)SDL_NS_TO_MS(event.gbutton.timestamp), (guint)event.gbutton.which, event.gbutton.button, event.gbutton.down);
      _process_axis_and_send(gamepad, SDL_NS_TO_MS(event.gbutton.timestamp));
      dt_shortcut_key_press(gamepad->id, SDL_NS_TO_MS(event.gbutton.timestamp), event.gbutton.button);

      break;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      dt_print(DT_DEBUG_INPUT, "SDL button up event time %u id %u button %hhd down %hhd", (guint)SDL_NS_TO_MS(event.gbutton.timestamp), (guint)event.gbutton.which, event.gbutton.button, event.gbutton.down);
      _process_axis_and_send(gamepad, SDL_NS_TO_MS(event.gbutton.timestamp));
      dt_shortcut_key_release(gamepad->id, SDL_NS_TO_MS(event.gbutton.timestamp), event.gbutton.button);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      dt_print(DT_DEBUG_INPUT, "SDL axis event type %u time %u id %u axis %hhd value %hd", event.gaxis.type, (guint)SDL_NS_TO_MS(event.gaxis.timestamp), (guint)event.gaxis.which, event.gaxis.axis, event.gaxis.value);

      if(event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
      {
        int trigger = event.gaxis.axis - SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
        if(event.gaxis.value / 10500 > gamepad->value[event.gaxis.axis])
        {
          dt_shortcut_key_release(gamepad->id, SDL_NS_TO_MS(event.gbutton.timestamp), SDL_GAMEPAD_BUTTON_COUNT + trigger);
          dt_shortcut_key_press(gamepad->id, SDL_NS_TO_MS(event.gbutton.timestamp), SDL_GAMEPAD_BUTTON_COUNT + trigger);
          gamepad->value[event.gaxis.axis] = event.gaxis.value / 10500;
        }
        else if(event.gaxis.value / 9500 < gamepad->value[event.gaxis.axis])
        {
          dt_shortcut_key_release(gamepad->id, SDL_NS_TO_MS(event.gbutton.timestamp), SDL_GAMEPAD_BUTTON_COUNT + trigger);
          gamepad->value[event.gaxis.axis] = event.gaxis.value / 9500;
        }
      }
      else
      {
        _process_axis_timestep(gamepad, SDL_NS_TO_MS(event.gaxis.timestamp));
        gamepad->value[event.gaxis.axis] = event.gaxis.value;
      }
      break;
    case SDL_EVENT_GAMEPAD_ADDED:
      break;
    }
  }

  for(GSList *gamepads = self->data; gamepads; gamepads = gamepads->next) _process_axis_and_send(gamepads->data, SDL_GetTicks());

  if(num_events) dt_print(DT_DEBUG_INPUT, "sdl num_events: %d time: %u", num_events, (guint)SDL_GetTicks());
  return G_SOURCE_CONTINUE;
}

static void _gamepad_open_devices(dt_lib_module_t *self)
{
  if(!SDL_Init(SDL_INIT_GAMEPAD))
  {
    dt_print(DT_DEBUG_ALWAYS, "[_gamepad_open_devices] ERROR initialising SDL");
    return;
  }
  else
    dt_print(DT_DEBUG_INPUT, "[_gamepad_open_devices] SDL initialized");

  dt_input_device_t id = dt_register_input_driver(self, &_driver_definition);

  int count = 0;
  SDL_JoystickID *ids = SDL_GetGamepads(&count);
  if(ids)
  {
    for(int i = 0; i < count && i < 10; i++)
    {
      SDL_Gamepad *controller = SDL_OpenGamepad(ids[i]);

      if(!controller)
      {
        dt_print(DT_DEBUG_ALWAYS, "[_gamepad_open_devices] ERROR opening game controller '%s'",
                 SDL_GetGamepadNameForID(ids[i]));
        continue;
      }
      else
      {
        dt_print(DT_DEBUG_ALWAYS, "[_gamepad_open_devices] opened game controller '%s'",
                 SDL_GetGamepadNameForID(ids[i]));
      }

      dt_gamepad_device_t *gamepad = g_malloc0(sizeof(dt_gamepad_device_t));

      gamepad->controller = controller;
      gamepad->id = id++;

      self->data = g_slist_append(self->data, gamepad);
    }
    SDL_free(ids);
  }
  if(self->data)
  {
    g_timeout_add(10, _poll_devices, self);
    g_timeout_add_full(G_PRIORITY_HIGH, 5, _pump_events, self, NULL);
  }
}

static void _gamepad_device_free(dt_gamepad_device_t *gamepad)
{
  SDL_CloseGamepad(gamepad->controller);

  g_free(gamepad);
}

static void _gamepad_close_devices(dt_lib_module_t *self)
{
  g_source_remove_by_user_data(self); // _poll_devices
  g_source_remove_by_user_data(self); // _pump_events

  g_slist_free_full(self->data, (void (*)(void *))_gamepad_device_free);
  self->data = NULL;

  // Don't call SDL_Quit here because reinitialising using SDL_Init won't work
}

void gui_init(dt_lib_module_t *self)
{
  self->data = NULL;

  _gamepad_open_devices(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  _gamepad_close_devices(self);
}

#endif // HAVE_SDL

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
