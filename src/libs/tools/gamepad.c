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

#include <SDL.h>

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
  SDL_GameController *controller;
  Uint32 timestamp;
  int value[SDL_CONTROLLER_AXIS_MAX];
  int location[SDL_CONTROLLER_AXIS_MAX];
} dt_gamepad_device_t;

static const char *_button_names[]
  = { N_("button a"), N_("button b"), N_("button x"), N_("button y"),
      N_("button back"), N_("button guide"), N_("button start"),
      N_("left stick"), N_("right stick"), N_("left shoulder"), N_("right shoulder"),
      N_("dpad up"), N_("dpad down"), N_("dpad left"), N_("dpad right"),
      N_("button misc1"), N_("paddle1"), N_("paddle2"), N_("paddle3"), N_("paddle4"), N_("touchpad"),
      N_("left trigger"), N_("right trigger"),
      NULL };

static gchar *_key_to_string(const guint key,
                             const gboolean display)
{
  const gchar *name = key < SDL_CONTROLLER_BUTTON_MAX + 2 ? _button_names[key] : N_("invalid gamepad button");
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

  return FALSE;
}

static const char *_move_names[]
  = { N_("left x"), N_("left y"), N_("right x"), N_("right y"),
      N_("left diagonal"), N_("left skew"), N_("right diagonal"), N_("right skew"),
      NULL };

static gchar *_move_to_string(const guint move,
                              const gboolean display)
{
  const gchar *name = move < SDL_CONTROLLER_AXIS_MAX + 4 /* diagonals */ ? _move_names[move] : N_("invalid gamepad axis");
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
    for(SDL_GameControllerAxis axis = SDL_CONTROLLER_AXIS_LEFTX; axis <= SDL_CONTROLLER_AXIS_RIGHTY; axis++)
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
    int stick = SDL_CONTROLLER_AXIS_LEFTX + 2 * side;

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
          gamepad->location[stick]  += size * step_size * angle;
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

  while(SDL_PollEvent(&event) > 0 )
  {
    num_events++;

    if(event.cbutton.which != prev_which)
    {
      prev_which = event.cbutton.which;
      SDL_GameController *controller = SDL_GameControllerFromInstanceID(prev_which);
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
    case SDL_CONTROLLERBUTTONDOWN:
      dt_print(DT_DEBUG_INPUT, "SDL button down event time %d id %d button %hhd state %hhd\n", event.cbutton.timestamp, event.cbutton.which, event.cbutton.button, event.cbutton.state);
      _process_axis_and_send(gamepad, event.cbutton.timestamp);
      dt_shortcut_key_press(gamepad->id, event.cbutton.timestamp, event.cbutton.button);

      break;
    case SDL_CONTROLLERBUTTONUP:
      dt_print(DT_DEBUG_INPUT, "SDL button up event time %d id %d button %hhd state %hhd\n", event.cbutton.timestamp, event.cbutton.which, event.cbutton.button, event.cbutton.state);
      _process_axis_and_send(gamepad, event.cbutton.timestamp);
      dt_shortcut_key_release(gamepad->id, event.cbutton.timestamp, event.cbutton.button);
      break;
    case SDL_CONTROLLERAXISMOTION:
      dt_print(DT_DEBUG_INPUT, "SDL axis event type %d time %d id %d axis %hhd value %hd\n", event.caxis.type, event.caxis.timestamp, event.caxis.which, event.caxis.axis, event.caxis.value);

      if(event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
      {
        int trigger = event.caxis.axis - SDL_CONTROLLER_AXIS_TRIGGERLEFT;
        if(event.caxis.value / 10500 > gamepad->value[event.caxis.axis])
        {
          dt_shortcut_key_release(gamepad->id, event.cbutton.timestamp, SDL_CONTROLLER_BUTTON_MAX + trigger);
          dt_shortcut_key_press(gamepad->id, event.cbutton.timestamp, SDL_CONTROLLER_BUTTON_MAX + trigger);
          gamepad->value[event.caxis.axis] = event.caxis.value / 10500;
        }
        else if(event.caxis.value / 9500 < gamepad->value[event.caxis.axis])
        {
          dt_shortcut_key_release(gamepad->id, event.cbutton.timestamp, SDL_CONTROLLER_BUTTON_MAX + trigger);
          gamepad->value[event.caxis.axis] = event.caxis.value / 9500;
        }
      }
      else
      {
        _process_axis_timestep(gamepad, event.caxis.timestamp);
        gamepad->value[event.caxis.axis] = event.caxis.value;
      }
      break;
    case SDL_CONTROLLERDEVICEADDED:
      break;
    }
  }

  for(GSList *gamepads = self->data; gamepads; gamepads = gamepads->next) _process_axis_and_send(gamepads->data, SDL_GetTicks());

  if(num_events) dt_print(DT_DEBUG_INPUT, "sdl num_events: %d time: %u\n", num_events, SDL_GetTicks());
  return G_SOURCE_CONTINUE;
}

static void _gamepad_open_devices(dt_lib_module_t *self)
{
  if(SDL_Init(SDL_INIT_GAMECONTROLLER))
  {
    dt_print(DT_DEBUG_ALWAYS, "[_gamepad_open_devices] ERROR initialising SDL\n");
    return;
  }
  else
    dt_print(DT_DEBUG_INPUT, "[_gamepad_open_devices] SDL initialized\n");

  dt_input_device_t id = dt_register_input_driver(self, &_driver_definition);

  for(int i = 0; i < SDL_NumJoysticks() && i < 10; i++)
  {
    if(SDL_IsGameController(i))
    {
      SDL_GameController *controller = SDL_GameControllerOpen(i);

      if(!controller)
      {
        dt_print(DT_DEBUG_ALWAYS, "[_gamepad_open_devices] ERROR opening game controller '%s'\n",
                 SDL_GameControllerNameForIndex(i));
        continue;
      }
      else
      {
        dt_print(DT_DEBUG_ALWAYS, "[_gamepad_open_devices] opened game controller '%s'\n",
                 SDL_GameControllerNameForIndex(i));
      }

      dt_gamepad_device_t *gamepad = (dt_gamepad_device_t *)g_malloc0(sizeof(dt_gamepad_device_t));

      gamepad->controller = controller;
      gamepad->id = id++;

      self->data = g_slist_append(self->data, gamepad);
    }
  }
  if(self->data)
  {
    g_timeout_add(10, _poll_devices, self);
    g_timeout_add_full(G_PRIORITY_HIGH, 5, _pump_events, self, NULL);
  }
}

static void _gamepad_device_free(dt_gamepad_device_t *gamepad)
{
  SDL_GameControllerClose(gamepad->controller);

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
