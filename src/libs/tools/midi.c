/*
    This file is part of darktable,
    copyright (c) 2019-2023 darktable developers.

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

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_NONE;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_TOP_CENTER;
}

typedef struct dt_midi_device_t
{
  dt_input_device_t   id;
  const PmDeviceInfo *info;
  PortMidiStream     *portmidi_in;
  PortMidiStream     *portmidi_out;

  gint8               channel;
  gboolean            syncing;
  gint                encoding;
  gint8               last_known[128];
  gint8               rotor_lights[128];
  guint8              num_keys, num_knobs, first_key, first_knob, first_light;

  int                 last_controller, last_received, last_diff, num_identical;

  gchar               behringer; // (X)-Touch (M)ini/(C)ompact/(E)tended/(O)ne/BC(F/R)2000
} dt_midi_device_t;

static const char *_note_names[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B", NULL };

static gchar *_key_to_string(const guint key,
                             const gboolean display)
{
  // The MIDI note range is from C−1 (note #0) to G9 (note #127).
  return g_strdup_printf(display ? "%s%d (%d)" : "%s%d",
                         _note_names[key % 12], (int)key / 12 - 1, key);
}

static gboolean _string_to_key(const gchar *string,
                               guint *key)
{
  int octave = 0;
  char name[3];

  if(sscanf(string, "%2[ABCDEFG#]%d", name, &octave) == 2)
  {
    for(int note = 0; _note_names[note]; note++)
    {
      if(!strcmp(name, _note_names[note]))
      {
        *key = note + 12 * (octave + 1);
        return TRUE;
      }
    }
  }

  return FALSE;
}

static gchar *_move_to_string(const guint move,
                              const gboolean display)
{
  return g_strdup_printf("CC%u", move);
}

static gboolean _string_to_move(const gchar *string,
                                guint *move)
{
  return sscanf(string, "CC%u", move) == 1;
}

static gboolean _key_to_move(dt_lib_module_t *self,
                             const dt_input_device_t id,
                             const guint key,
                             guint *move)
{
  for(GSList *devices = self->data; devices; devices = devices->next)
  {
    const dt_midi_device_t *midi = devices->data;
    if(midi->id != id) continue;

    if(midi->behringer == 'M')
    {
      if(key < 8)
        *move = key + 1;
      else if(key >= 24 && key < 32)
        *move = key - 13;
      else
        return FALSE;
    }
    else if(midi->behringer == 'C')
    {
      if(key < 16)
        *move = key + 10;
      else if(key >= 40 && key < 49)
        *move = key - 39;
      else if(key >= 55 && key < 71)
        *move = key -18;
      else if(key >= 95 && key < 104)
        *move = key - 67;
      else
        return FALSE;
    }
    else
    {
      *move = key;
    }
  }

  return TRUE;
}

static const dt_input_driver_definition_t _driver_definition
  = { "midi", _key_to_string, _string_to_key, _move_to_string, _string_to_move, _key_to_move };

static void _midi_write(dt_midi_device_t *midi,
                        const gint channel,
                        const gint type,
                        const gint key,
                        const gint velocity)
{
  if(midi->portmidi_out)
  {
    PmMessage message = Pm_Message((type << 4) + channel, key, velocity);
    PmError pmerror = Pm_WriteShort(midi->portmidi_out, 0, message);
    if(pmerror != pmNoError)
    {
      g_print("Portmidi error: %s\n", Pm_GetErrorText(pmerror));
      Pm_Close(midi->portmidi_out);
      midi->portmidi_out = NULL;
    }
  }
}

static gint _calculate_move(dt_midi_device_t *midi,
                            const gint controller,
                            const gint velocity)
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
      const int last = midi->last_known[controller];
      midi->last_known[controller] = velocity;

      int diff = 0;
      if(last != -1)
      {
        if(midi->num_identical)
        {
          if(velocity != midi->last_received && midi->last_received != -1)
          {
            dt_control_log(_("using absolute encoding; reinitialise to switch to relative"));
            midi->num_identical = 0;
          }
          else if(--midi->num_identical)
            dt_control_log(_("%d more identical (down) moves before switching to relative encoding"), midi->num_identical);
          else
          {
            dt_control_log(_("switching encoding to relative (down = %d)"), velocity);
            midi->encoding = velocity;
          }
        }
        else if(velocity == 0)
        {
          if(last == 0)
            diff = -1;
          else
            diff = -1e6; // try to reach min in one step
        }
        else if(velocity == 127)
        {
          if(last == 127)
            diff = 1;
          else
            diff = +1e6; // try to reach max in one step
        }
        else
        {
          diff = velocity - last;

          if(controller == midi->last_controller &&
             diff * midi->last_diff < 0)
          {
            const int diff_received = velocity - midi->last_received;
            if(abs(diff) > abs(diff_received))
              diff = diff_received;
          }
        }
      }

      midi->last_controller = controller;
      midi->last_received   = velocity;
      midi->last_diff       = diff;

      return diff;
    }
    break;
  }
}

static void _midi_write_bcontrol(dt_midi_device_t *midi,
                                 const gchar seq,
                                 gchar *str)
{
  // sysex string contains zeros so can't use standard string handling and Pm_WriteSysEx
  unsigned char sysex[100];
  const int syslen = g_snprintf((gchar *)sysex, sizeof(sysex),
                                "\xF0%c\x20\x32\x7F\x7F\x20%c%c%s\xF7%c%c%c",
                                0, 0, seq, str, 0, 0, 0);
  PmEvent buffer[sizeof(sysex)/4] = {0};
  for(int i = 0; i < syslen / 4; i++)
    buffer[i].message = sysex[i*4] | (sysex[i*4+1] << 8) | (sysex[i*4+2] << 16) | (sysex[i*4+3] << 24);
  PmError pmerror = Pm_Write(midi->portmidi_out, buffer, syslen / 4);
  if(pmerror != pmNoError)
  {
    g_print("Portmidi error while writing light pattern to BCF/R2000: %s\n", Pm_GetErrorText(pmerror));
    Pm_Close(midi->portmidi_out);
    midi->portmidi_out = NULL;
  }
  g_free(str);
}

static void _update_with_move(dt_midi_device_t *midi,
                              const PmTimestamp timestamp,
                              const gint controller,
                              const float move)
{
  float new_position = dt_shortcut_move(midi->id, timestamp, controller, move);

  const int new_pattern =
    DT_ACTION_IS_INVALID(new_position) ? 1
    : fmodf(new_position, DT_VALUE_PATTERN_ACTIVE) == DT_VALUE_PATTERN_SUM ? 2
    : new_position >= DT_VALUE_PATTERN_PERCENTAGE ? 2
    : new_position >= DT_VALUE_PATTERN_PLUS_MINUS ? 3
    : 1;

  static const int light_codes[] = { 1, 1 /* pan */, 2 /* fan */, 4 /* trim */};

  if(midi->behringer == 'M')
  {
    if(midi->first_key == 8
       ? controller <  9 /* layer A */
       : controller > 10 /* layer B */)
    {
      // Light pattern always for 1-8 range, but CC=1-8 (bank A) or 11-18 (bank B).
      _midi_write(midi, 0, 0xB, controller % 10, light_codes[new_pattern]);
    }
  }
  else if(midi->behringer == 'C')
  {
    if(midi->first_key == 16
       ? (controller >= 10 && controller <= 25) /* layer A */
       : (controller >= 37 && controller <= 52) /* layer B */)
    {
      // Light pattern always for 10-25 range, but CC=10-25 (bank A) or 37-52 (bank B).
      _midi_write(midi, 1, 0xB, controller % 27, light_codes[new_pattern]);
    }
  }
  else if(new_pattern != midi->rotor_lights[controller])
  {
    midi->rotor_lights[controller] = new_pattern;

    if((midi->behringer == 'R' || midi->behringer == 'F')
       && controller < 32 && midi->portmidi_out)
    {
      static const gchar *bcontrol_codes[] = { "1dot/off", "12dot", "bar", "pan" };

      _midi_write_bcontrol(midi, 0, g_strdup_printf("$rev %c", midi->behringer));
      _midi_write_bcontrol(midi, 1, g_strdup_printf("$encoder %d", controller + 1));
      _midi_write_bcontrol(midi, 2, g_strdup_printf("  .easypar CC 1 %d 0 127 absolute", controller));
      _midi_write_bcontrol(midi, 3, g_strdup_printf("  .mode %s", bcontrol_codes[new_pattern]));
      _midi_write_bcontrol(midi, 4, g_strdup_printf("  .showvalue on"));
      _midi_write_bcontrol(midi, 5, g_strdup_printf("$end"));
    }
  }

  if(DT_ACTION_IS_INVALID(new_position))
    return;

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
  else
  {
    const int c = - new_position;
    if(c > 1)
    {
      if(midi->behringer == 'M' || midi->behringer == 'C')
        rotor_position = fmodf(c * 10.5f - (c > 13 ? 140.1f : 8.6f), 128);
      else
        rotor_position = fmodf(c * 9.0f - 10.f, 128);
    }
  }

  midi->last_known[controller] = rotor_position;
  _midi_write(midi, midi->channel, 0xB, controller, rotor_position);

  // dt_print(DT_DEBUG_INPUT, "Controller: Channel %d, controller %d, position: %d", midi->channel, controller, rotor_position);
}

static gboolean _poll_devices(gpointer user_data)
{
  dt_lib_module_t *self = user_data;
  for(GSList *devices = self->data; devices; devices = devices->next)
  {
    dt_midi_device_t *midi = devices->data;

    PmEvent event[EVENT_BUFFER_SIZE];
    const int num_events = Pm_Read(midi->portmidi_in, event, EVENT_BUFFER_SIZE);

    for(int i = 0; i < num_events; i++)
    {

      const int event_status = Pm_MessageStatus(event[i].message);
      int event_data1 = Pm_MessageData1(event[i].message);
      int event_data2 = Pm_MessageData2(event[i].message);

      int event_type = event_status >> 4;

      if(event_type == 0x9 && // note on
          event_data2 == 0) // without volume
      {
        event_type = 0x8; // note off
      }

      midi->channel = event_status & 0x0F;

      gboolean layer_B = FALSE;

      switch(event_type)
      {
      case 0x9:  // note on
        dt_print(DT_DEBUG_INPUT, "Note On: Channel %d, Data1 %d", midi->channel, event_data1);

        layer_B = event_data1 > (midi->behringer == 'M' ? 23 : 54);

        const int key_num = event_data1 - midi->first_key + 1;
        if(key_num > midi->num_keys && !midi->behringer)
          midi->num_keys = key_num;

        dt_shortcut_key_press(midi->id, event[i].timestamp, event_data1);
        break;
      case 0x8:  // note off
        dt_print(DT_DEBUG_INPUT, "Note Off: Channel %d, Data1 %d", midi->channel, event_data1);

        layer_B = event_data1 > (midi->behringer == 'M' ? 23 : 54);

        dt_shortcut_key_release(midi->id, event[i].timestamp, event_data1);
        break;
      case 0xb:  // controllers, sustain
        if(midi->behringer == 'C' && event_data1 > 100) // ignore fader touch
        {
          layer_B = event_data1 > 110;
          break;
        }

        layer_B = event_data1 > (midi->behringer == 'M' ? 9 : 27);

        int accum = 0;
        for(int j = i; j < num_events; j++)
        {
          if(Pm_MessageStatus(event[j].message) == event_status &&
             Pm_MessageData1(event[j].message) == event_data1)
          {
            event_data2 = Pm_MessageData2(event[j].message);
            dt_print(DT_DEBUG_INPUT, "Controller: Channel %d, Data1 %d, Data2 %d", midi->channel, event_data1, event_data2);

            accum += _calculate_move(midi, event_data1, event_data2);
            event[j].message = 0; // don't process again later
          }
        }

        const int knob_num = event_data1 - midi->first_knob + 1;
        if(knob_num > midi->num_knobs)
          midi->num_knobs = knob_num;

        _update_with_move(midi, event[i].timestamp, event_data1, accum);

        break;
      default:
        continue; // layer_B has not been set
      }

      if(midi->behringer == 'M' || midi->behringer == 'C')
      {
        guint8 old_first = midi->first_key;
        midi->first_key = midi->behringer == 'M'
                        ? (layer_B ? 32 :  8)
                        : (layer_B ? 71 : 16); // 'C'

        if(midi->first_key != old_first)
          for(int j = 1; j <= midi->num_knobs ; j++) midi->last_known[j] = -1;
      }
    }
  }

  return G_SOURCE_CONTINUE;
}

static void _midi_open_devices(dt_lib_module_t *self)
{
  if(Pm_Initialize())
  {
    dt_print(DT_DEBUG_ALWAYS, "[_midi_open_devices] ERROR initialising PortMidi");
    return;
  }
  else
    dt_print(DT_DEBUG_INPUT, "[_midi_open_devices] PortMidi initialized");

  dt_input_device_t id = dt_register_input_driver(self, &_driver_definition);

  const char *devices_string = dt_conf_get_string_const("plugins/midi/devices");
  gchar **dev_strings = g_strsplit(devices_string, ",", 0);

  int last_dev = -1;

  for(int i = 0; i < Pm_CountDevices(); i++)
  {
    const PmDeviceInfo *info = Pm_GetDeviceInfo(i);
    dt_print(DT_DEBUG_INPUT, "[_midi_open_devices] found midi device '%s' via '%s'", info->name, info->interf);

    if(info->input && !strstr(info->name, "Midi Through Port"))
    {
      int dev = -1, encoding = 0, num_keys = 0;

      gchar **cur_dev = dev_strings;
      gchar **cur_dev_par = NULL;
      for(; cur_dev && *cur_dev; cur_dev++)
      {
        if(**cur_dev == '-')
        {
          if(strstr(info->name, (*cur_dev) + 1))
          {
            dev = 10;
            break;
          }
        }
        else
        {
          dev++;

          if(dev > last_dev) last_dev = dev;

          g_strfreev(cur_dev_par);
          cur_dev_par = g_strsplit(*cur_dev, ":", 3);
          if(*cur_dev_par && strstr(info->name, *cur_dev_par))
          {
            if(*(cur_dev_par+1))
            {
              sscanf(*(cur_dev_par+1), "%d", &encoding);
              if(*(cur_dev_par+2))
                sscanf(*(cur_dev_par+2), "%d", &num_keys);
            }
            break;
          }
        }
      }
      g_strfreev(cur_dev_par);

      if(!cur_dev || !*cur_dev) dev = ++last_dev;

      if(dev >= 10) continue;

      PortMidiStream *stream_in;
      PmError error = Pm_OpenInput(&stream_in, i, NULL, EVENT_BUFFER_SIZE, NULL, NULL);

      if(error < 0)
      {
        dt_print(DT_DEBUG_ALWAYS, "[_midi_open_devices] ERROR opening midi device '%s' via '%s'",
                 info->name, info->interf);
        continue;
      }
      else
      {
        dt_print(DT_DEBUG_INPUT, "[_midi_open_devices] opened midi device '%s' via '%s' as midi%d",
                 info->name, info->interf, dev);
        if(!cur_dev || !*cur_dev)
          dt_control_log(_("%s opened as midi%d"), info->name, dev);
      }

      dt_midi_device_t *midi = g_malloc0(sizeof(dt_midi_device_t));

      midi->id          = id + dev;
      midi->info        = info;
      midi->portmidi_in = stream_in;
      midi->encoding    = encoding;
      midi->num_keys    = num_keys;

      if(strstr(info->name, "X-TOUCH MINI"))
      {
        midi->behringer =  'M';
        midi->num_knobs =   18;
        midi->first_knob =   1;
        midi->num_keys =    16;
        midi->first_key =    8;
        midi->channel =     10;
      }
      else if(strstr(info->name, "X-TOUCH COMPACT"))
       {
        midi->behringer =  'C';
        midi->num_knobs =   52;
        midi->first_knob =   1;
        midi->num_keys =    39;
        midi->first_key =   16;
      }
      else if(strstr(info->name, "BCR2000"))
      {
        midi->behringer =  'R';
        midi->num_knobs =   56;
        midi->num_keys =    26;
        midi->first_key =   32;
        midi->first_light = 32;
      }
      else if(strstr(info->name, "BCF2000"))
      {
        midi->behringer =  'F';
        midi->num_knobs =   40;
        midi->num_keys =    26;
        midi->first_key =   32;
        midi->first_light = 32;
      }

      midi->num_identical = midi->behringer || encoding ? 0 : 5; // countdown "relative down" moves received before switching to relative mode
      midi->last_received = -1;
      for(int j = 0; j < 128; j++) midi->last_known[j] = -1;

      for(int j = 0; j < Pm_CountDevices(); j++)
      {
        const PmDeviceInfo *infoOutput = Pm_GetDeviceInfo(j);

        if(!strcmp(info->name, infoOutput->name) && infoOutput->output && !infoOutput->opened)
        {
          Pm_OpenOutput(&midi->portmidi_out, j, NULL, 1000, NULL, NULL, 0);
        }
      }

      self->data = g_slist_append(self->data, midi);
    }
  }

  g_strfreev(dev_strings);

  if(self->data) g_timeout_add(10, _poll_devices, self);
}

static void _midi_device_free(dt_midi_device_t *midi)
{
  Pm_Close(midi->portmidi_in);

  if(midi->portmidi_out)
  {
    Pm_Close(midi->portmidi_out);
  }

  g_free(midi);
}

static void _midi_close_devices(dt_lib_module_t *self)
{
  g_source_remove_by_user_data(self);

  g_slist_free_full(self->data, (void (*)(void *))_midi_device_free);
  self->data = NULL;

  Pm_Terminate();
}

static gboolean _update_devices(gpointer user_data)
{
  GSList *devices = (GSList *)((dt_lib_module_t *)user_data)->data;
  while(devices)
  {
    dt_midi_device_t *midi = devices->data;

    for(int i = 0; i < midi->num_knobs && midi->portmidi_out; i++)
      _update_with_move(midi, 0, i + midi->first_knob, DT_READ_ACTION_ONLY);

    gint global = midi->behringer == 'M' ? 0
                : midi->behringer == 'C' ? 1
                : midi->channel;
    for(int i = 0; i < midi->num_keys && midi->portmidi_out; i++)
    {
      _midi_write(midi, global, 0x9, i + midi->first_light,
                  dt_shortcut_key_active(midi->id, i + midi->first_key)
                  ? (midi->behringer == 'C' ? 2 : 1) : 0);
    }

    devices = devices->next;
  }

  return G_SOURCE_CONTINUE;
}

void gui_init(dt_lib_module_t *self)
{
  dt_capabilities_add("midi");

  self->data = NULL;

  _midi_open_devices(self);

  g_timeout_add(250, _update_devices, self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_source_remove_by_user_data(self);

  _midi_close_devices(self);
}

#endif // HAVE_PORTMIDI

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
