/*
    This file is part of darktable,
    copyright (c) 2019--2020 Diederik ter Rahe.

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

char midi_devices_default[] = "*";

DT_MODULE(1)

#ifdef HAVE_PORTMIDI

#include <portmidi.h>

#define EVENT_BUFFER_SIZE 100

const char *name(dt_lib_module_t *self)
{
  return _("midi");
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

int position()
{
  return 1;
}

typedef struct midi_device
{
  dt_input_device_t   id;
  const PmDeviceInfo *info;
  PortMidiStream     *portmidi_in;
  PortMidiStream     *portmidi_out;

  gint8               channel;
  gboolean            syncing;
  gint                encoding;
  gint8               last_known[128];
  gint8               last_type, last_data1, num_identical;
} midi_device;

const char *note_names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B", NULL };

gchar *key_to_string(guint key, gboolean display)
{
  return g_strdup_printf("%s%d", note_names[key % 12], key / 12 - 1);
}

gboolean string_to_key(gchar *string, guint *key)
{
  int octave = 0;
  char name[3];

  if(sscanf(string, "%2[ABCDEFG#]%d", name, &octave) == 2)
  {
    for(int note = 0; note_names[note]; note++)
    {
      if(!strcmp(name, note_names[note]))
      {
        *key = note + 12 * (octave + 1);
        return TRUE;
      }
    }
  }

  return FALSE;
}

gchar *move_to_string(guint move, gboolean display)
{
  return g_strdup_printf("CC%u", move);
}

gboolean string_to_move(gchar *string, guint *move)
{
  return sscanf(string, "CC%u", move) == 1;
}

const dt_input_driver_definition_t driver_definition
  = { "midi", key_to_string, string_to_key, move_to_string, string_to_move };

void midi_write(midi_device *midi, gint channel, gint type, gint key, gint velocity)
{
  if (midi->portmidi_out)
  {
    PmMessage message = Pm_Message((type << 4) + channel, key, velocity);
    PmError pmerror = Pm_WriteShort(midi->portmidi_out, 0, message);
    if (pmerror != pmNoError)
    {
      g_print("Portmidi error: %s\n", Pm_GetErrorText(pmerror));
    }
  }
}

gint calculate_move(midi_device *midi, gint controller, gint velocity)
{
  switch(midi->encoding)
  {
  case 127: // 2s Complement
    if(velocity < 65)
      return velocity;
    else
      return velocity - 128;
    break;
  case 63: // Offset
    return velocity - 64;
    break;
  case 33: // Sign
    if(velocity < 32)
      return velocity;
    else
      return 32 - velocity;
    break;
  case 15: // Offset 5 bit
    return velocity - 16;
    break;
  case 65: // Sign 6 bit (x-touch mini in MC mode)
    if(velocity < 64)
      return velocity;
    else
      return 64 - velocity;
    break;
  default: // absolute
    {
      int diff = velocity - midi->last_known[controller];
      midi->last_known[controller] = velocity;
/*
      if(diff == 0)
      {
        // detecting relative encoding?
        if(++midi->num_identical == 5)
        {
          dt_print(DT_DEBUG_INPUT, "Controller: switching encoding %d\n", velocity);
          midi->encoding = velocity;
        }
        return 0;
      }
      else
        midi->num_identical = 0;
*/
      if(velocity == 0) return -1e6; // try to reach min in one step
      if(velocity == 127) return +1e6; // try to reach max in one step
      return diff;
    }
    break;
  }
}

void update_with_move(midi_device *midi, PmTimestamp timestamp, gint controller, float move)
{
  float new_position = dt_shortcut_move(midi->id, timestamp, controller, move);

  if(strstr(midi->info->name, "X-TOUCH MINI") && controller >= 1 && controller <= 18)
  {
    // Light pattern always for 1-8 range, but CC=1-8 (bank A) or 11-18 (bank B).
    // Can't determine which with certainty. Guess from last received command.
    if( (controller <=  8 && (midi->last_type == 0xb ? midi->last_data1 <=  9 : midi->last_data1 <= 23)) ||
        (controller >= 11 && (midi->last_type == 0xb ? midi->last_data1 >= 10 : midi->last_data1 >= 24)) )
    {
      if(isnan(new_position))
        midi_write(midi, 0, 0xB, controller % 10, 0); // off
      else if(new_position >= 4)
        midi_write(midi, 0, 0xB, controller % 10, 2); // fan
      else if(new_position >= 2)
        midi_write(midi, 0, 0xB, controller % 10, 4); // trim
      else
        midi_write(midi, 0, 0xB, controller % 10, 1); // pan
    }
  }

  int rotor_position = 0;
  if(new_position >= 0)
  {
    new_position = fmodf(new_position, 2.0);
    if(new_position != 0.0)
    {
      if(new_position == 1.0)
        rotor_position = 127;
      else
      {
        rotor_position = 2.0 + new_position * 124.0; // 2-125
      }
    }
  }
  else if(!isnan(new_position))
  {
    int c = - new_position - 1;
    rotor_position = c > 11 ? c+107 : c*127./12.+1.25;
  }
  else
  {
    if(midi->last_known[controller] == 0) return;
  }

  midi->last_known[controller] = rotor_position;
  midi_write(midi, midi->channel, 0xB, controller, rotor_position);
  dt_print(DT_DEBUG_INPUT, "Controller: Channel %d, controller %d, position: %d\n", midi->channel, controller, rotor_position);
}

static gboolean poll_midi_devices(gpointer user_data)
{
  GSList *devices = (GSList *)((dt_lib_module_t *)user_data)->data;
  while(devices)
  {
    midi_device *midi = devices->data;

    PmEvent event[EVENT_BUFFER_SIZE];
    int num_events = Pm_Read(midi->portmidi_in, event, EVENT_BUFFER_SIZE);

    for(int i = 0; i < num_events; i++)
    {

      int event_status = Pm_MessageStatus(event[i].message);
      int event_data1 = Pm_MessageData1(event[i].message);
      int event_data2 = Pm_MessageData2(event[i].message);

      int event_type = event_status >> 4;

      if (event_type == 0x9 && // note on
          event_data2 == 0) // without volume
      {
        event_type = 0x8; // note off
      }

      midi->last_type = event_type;
      midi->channel = event_status & 0x0F;
      midi->last_data1 = event_data1;

      switch (event_type)
      {
      case 0x9:  // note on
        dt_print(DT_DEBUG_INPUT, "Note On: Channel %d, Data1 %d\n", midi->channel, event_data1);

        dt_shortcut_key_press(midi->id, event[i].timestamp, event_data1, dt_key_modifier_state());
        break;
      case 0x8:  // note off
        dt_print(DT_DEBUG_INPUT, "Note Off: Channel %d, Data1 %d\n", midi->channel, event_data1);

        dt_shortcut_key_release(midi->id, event[i].timestamp, event_data1);
        break;
      case 0xb:  // controllers, sustain
        dt_print(DT_DEBUG_INPUT, "Controller: Channel %d, Data1 %d\n", midi->channel, event_data1);

        int accum = 0;
        for(int j = i; j < num_events; j++)
          if(Pm_MessageStatus(event[j].message) == event_status &&
             Pm_MessageData1(event[j].message) == event_data1)
          {
            event_data2 = Pm_MessageData2(event[j].message);
            dt_print(DT_DEBUG_INPUT, "Controller: Channel %d, Data1 %d, Data2 %d\n", midi->channel, event_data1, event_data2);

            accum += calculate_move(midi, event_data1, event_data2);
            event[j].message = 0; // don't process again later
          }

        update_with_move(midi, event[i].timestamp, event_data1, accum);

        break;
      default:
        break;
      }
    }

    devices = devices->next;
  }

  return G_SOURCE_CONTINUE;
}

void midi_open_devices(dt_lib_module_t *self)
{
  if(Pm_Initialize())
  {
    fprintf(stderr, "[midi_open_devices] ERROR initialising PortMidi\n");
    return;
  }
  else
    dt_print(DT_DEBUG_INPUT, "[midi_open_devices] PortMidi initialized\n");

  dt_input_device_t id = dt_register_input_driver(self, &driver_definition);

  for(int i = 0; i < Pm_CountDevices() && i < 10; i++)
  {
    const PmDeviceInfo *info = Pm_GetDeviceInfo(i);
    dt_print(DT_DEBUG_INPUT, "[midi_open_devices] found midi device '%s' via '%s'\n", info->name, info->interf);


    if(info->input && !strstr(info->name, "Midi Through Port"))
    {
      PortMidiStream *stream_in;
      PmError error = Pm_OpenInput(&stream_in, i, NULL, EVENT_BUFFER_SIZE, NULL, NULL);

      if(error < 0)
      {
        fprintf(stderr, "[midi_open_devices] ERROR opening midi device '%s' via '%s'\n", info->name, info->interf);
        continue;
      }
      else
      {
        fprintf(stderr, "[midi_open_devices] opened midi device '%s' via '%s'\n", info->name, info->interf);
      }

      midi_device *midi = (midi_device *)g_malloc0(sizeof(midi_device));

      midi->id = id++;
      midi->info = info;
      midi->portmidi_in = stream_in;

      for(int j = 0; j < Pm_CountDevices(); j++)
      {
        const PmDeviceInfo *infoOutput = Pm_GetDeviceInfo(j);

        if(infoOutput->output && !strcmp(info->name, infoOutput->name))
        {
          Pm_OpenOutput(&midi->portmidi_out, j, NULL, 1000, NULL, NULL, 0);
        }
      }

      self->data = g_slist_append(self->data, midi);
    }
  }

  if(self->data) g_timeout_add(10, poll_midi_devices, self);
}

void midi_device_free(midi_device *midi)
{
  Pm_Close(midi->portmidi_in);

  if (midi->portmidi_out)
  {
    Pm_Close(midi->portmidi_out);
  }

  g_free(midi);
}

void midi_close_devices(dt_lib_module_t *self)
{
  g_source_remove_by_user_data(self);

  g_slist_free_full(self->data, (void (*)(void *))midi_device_free);
  self->data = NULL;

  Pm_Terminate();
}

static void callback_image_changed(gpointer instance, gpointer user_data)
{
  GSList *devices = (GSList *)((dt_lib_module_t *)user_data)->data;
  while(devices)
  {
    midi_device *midi = devices->data;

    for(int i = 0; i < 128; i++) update_with_move(midi, 0, i, 0);
    // FIXME this needs to happen with lower priority to avoid flooding
    // could be priority of slider update that is too high
    // also; reduce range if it can be determined reliably

    devices = devices->next;
  }
}

static void callback_view_changed(gpointer instance, dt_view_t *old_view, dt_view_t *new_view, gpointer data)
{
}

void gui_init(dt_lib_module_t *self)
{
  if(!self->widget)
  {
    self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_no_show_all(self->widget, TRUE);
  }
  self->data = NULL;

  midi_open_devices(self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                            G_CALLBACK(callback_view_changed), self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                            G_CALLBACK(callback_image_changed), self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                            G_CALLBACK(callback_image_changed), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
 dt_control_signal_disconnect(darktable.signals,
                               G_CALLBACK(callback_view_changed), self);

  dt_control_signal_disconnect(darktable.signals,
                               G_CALLBACK(callback_image_changed), self);

  dt_control_signal_disconnect(darktable.signals,
                               G_CALLBACK(callback_image_changed), self);

  midi_close_devices(self);
}

#endif // HAVE_PORTMIDI

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
