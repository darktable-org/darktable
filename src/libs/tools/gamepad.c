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

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_TOP_CENTER;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 1;
}

typedef struct gamepad_device
{
  dt_input_device_t id;
  SDL_GameController *controller;
  Uint32 timestamp;
  int value[SDL_CONTROLLER_AXIS_MAX];
  int location[SDL_CONTROLLER_AXIS_MAX];
} gamepad_device;

const char *button_names[]
  = { N_("button a"), N_("button b"), N_("button x"), N_("button y"),
      N_("button back"), N_("button guide"), N_("button start"),
      N_("left stick"), N_("right stick"), N_("left shoulder"), N_("right shoulder"),
      N_("dpad up"), N_("dpad down"), N_("dpad left"), N_("dpad right"),
      N_("button misc1"), N_("paddle1"), N_("paddle2"), N_("paddle3"), N_("paddle4"), N_("touchpad"),
      N_("left trigger"), N_("right trigger"),
      NULL };

gchar *key_to_string(const guint key, const gboolean display)
{
  const gchar *name = key < SDL_CONTROLLER_BUTTON_MAX + 2 ? button_names[key] : N_("invalid gamepad button");
  return g_strdup(display ? _(name) : name);
}

gboolean string_to_key(const gchar *string, guint *key)
{
  *key = 0;
  while(button_names[*key])
    if(!strcmp(button_names[*key], string))
      return TRUE;
    else
      (*key)++;

  return FALSE;
}

const char *move_names[]
  = { N_("left x"), N_("left y"), N_("right x"), N_("right y"),
      N_("left diagonal"), N_("left skew"), N_("right diagonal"), N_("right skew"),
      NULL };

gchar *move_to_string(const guint move, const gboolean display)
{
  const gchar *name = move < SDL_CONTROLLER_AXIS_MAX + 4 /* diagonals */ ? move_names[move] : N_("invalid gamepad axis");
  return g_strdup(display ? _(name) : name);
}

gboolean string_to_move(const gchar *string, guint *move)
{
  *move = 0;
  while(move_names[*move])
    if(!strcmp(move_names[*move], string))
      return TRUE;
    else
      (*move)++;

  return FALSE;
}

const dt_input_driver_definition_t driver_definition
  = { "game", key_to_string, string_to_key, move_to_string, string_to_move };

static gboolean pump_events(gpointer user_data)
{
  SDL_PumpEvents();

  return G_SOURCE_CONTINUE;
}

void process_axis_timestep(gamepad_device *gamepad, Uint32 timestamp)
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

void process_axis_and_send(gamepad_device *gamepad, Uint32 timestamp)
{
  process_axis_timestep(gamepad, timestamp);

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

static gboolean poll_gamepad_devices(gpointer user_data)
{
//  dt_input_device_t id = GPOINTER_TO_INT(user_data);
  dt_lib_module_t *self = user_data;

  SDL_Event event;
  int num_events = 0;

  gamepad_device *gamepad = NULL;
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
        if(((gamepad_device *)(gamepads->data))->controller == controller)
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
      process_axis_and_send(gamepad, event.cbutton.timestamp);
      dt_shortcut_key_press(gamepad->id, event.cbutton.timestamp, event.cbutton.button);

      break;
    case SDL_CONTROLLERBUTTONUP:
      dt_print(DT_DEBUG_INPUT, "SDL button up event time %d id %d button %hhd state %hhd\n", event.cbutton.timestamp, event.cbutton.which, event.cbutton.button, event.cbutton.state);
      process_axis_and_send(gamepad, event.cbutton.timestamp);
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
        process_axis_timestep(gamepad, event.caxis.timestamp);
        gamepad->value[event.caxis.axis] = event.caxis.value;
      }
      break;
    case SDL_CONTROLLERDEVICEADDED:
      break;
    }
  }

  for(GSList *gamepads = self->data; gamepads; gamepads = gamepads->next) process_axis_and_send(gamepads->data, SDL_GetTicks());

  if(num_events) dt_print(DT_DEBUG_INPUT, "sdl num_events: %d time: %u\n", num_events, SDL_GetTicks());
  return G_SOURCE_CONTINUE;
}

void gamepad_open_devices(dt_lib_module_t *self)
{
  if(SDL_Init(SDL_INIT_GAMECONTROLLER))
  {
    dt_print(DT_DEBUG_ALWAYS, "[gamepad_open_devices] ERROR initialising SDL\n");
    return;
  }
  else
    dt_print(DT_DEBUG_INPUT, "[gamepad_open_devices] SDL initialized\n");

  dt_input_device_t id = dt_register_input_driver(self, &driver_definition);

  for(int i = 0; i < SDL_NumJoysticks() && i < 10; i++)
  {
    if(SDL_IsGameController(i))
    {
      SDL_GameController *controller = SDL_GameControllerOpen(i);

      if(!controller)
      {
        dt_print(DT_DEBUG_ALWAYS, "[gamepad_open_devices] ERROR opening game controller '%s'\n",
                 SDL_GameControllerNameForIndex(i));
        continue;
      }
      else
      {
        dt_print(DT_DEBUG_ALWAYS, "[gamepad_open_devices] opened game controller '%s'\n",
                 SDL_GameControllerNameForIndex(i));
      }

      gamepad_device *gamepad = (gamepad_device *)g_malloc0(sizeof(gamepad_device));

      gamepad->controller = controller;
      gamepad->id = id++;

      self->data = g_slist_append(self->data, gamepad);
    }
  }
  if(self->data)
  {
    g_timeout_add(10, poll_gamepad_devices, self);
    g_timeout_add_full(G_PRIORITY_HIGH, 5, pump_events, self, NULL);
  }
}

void gamepad_device_free(gamepad_device *gamepad)
{
  SDL_GameControllerClose(gamepad->controller);

  g_free(gamepad);
}

void gamepad_close_devices(dt_lib_module_t *self)
{
  g_source_remove_by_user_data(self); // poll_gamepad_devices
  g_source_remove_by_user_data(self); // pump_events

  g_slist_free_full(self->data, (void (*)(void *))gamepad_device_free);
  self->data = NULL;

  // Don't call SDL_Quit here because reinitialising using SDL_Init won't work
}

void gui_init(dt_lib_module_t *self)
{
  if(!self->widget)
  {
    self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_no_show_all(self->widget, TRUE);
  }
  self->data = NULL;

  gamepad_open_devices(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  gamepad_close_devices(self);
}

#endif // HAVE_SDL

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
