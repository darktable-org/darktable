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

char midi_devices_default[] = "portmidi,alsa,/dev/midi1,/dev/midi2,/dev/midi3,/dev/midi4";

DT_MODULE(1)

#ifdef HAVE_ALSA
  #define _GNU_SOURCE  /* the ALSA headers need this */
  #include <alsa/asoundlib.h>
#endif

#ifdef HAVE_PORTMIDI
  #include <portmidi.h>
#endif

#define D(stmnt) stmnt;

const char *name(dt_lib_module_t *self)
{
  return _("midi");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
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

typedef struct dt_midi_knob_t
{
  gint group;
  gint channel;
  gint key;

  dt_accel_dynamic_t *accelerator;

  gint encoding;
#define MIDI_ABSOLUTE 0
  gboolean locked;
  float acceleration;
} dt_midi_knob_t;

typedef struct dt_midi_note_t
{
  gint group;
  gint channel;
  gint key;

  guint accelerator_key;
  GdkModifierType accelerator_mods;
} dt_midi_note_t;

typedef struct MidiDevice
{
  gchar          *device;
  gchar          *model_name;

#ifdef HAVE_ALSA
  snd_seq_t      *sequencer;
  int             port;
#endif
#ifdef HAVE_PORTMIDI
  PortMidiStream *portmidi;
  PortMidiStream *portmidi_out;
#endif
  guint           source_id;

  GIOChannel     *io;
  guint           io_id;

  gboolean        name_queried;

  /* midi status */
  gboolean        swallow;
  gint            command;
  gint            channel;
  gint            key;
  gint            velocity;
  gint            msb;
  gint            lsb;

  gboolean        config_loaded;
  GSList         *mapping_list;
  GSList         *note_list;

  gint            mapping_channel;
  gint            mapping_key;
  gint            mapping_velocity;

  gint            accum;
  gint            stored_channel;
  gint            stored_key;
  dt_midi_knob_t *stored_knob;

  gboolean        syncing;

  gint            group;
  gint            num_columns;
  gint            group_switch_key;
  gint            group_key_light;
  gint            rating_key_light;
  gint            reset_knob_key;
  gint            first_knob_key;
  gint            num_rotators;
  gint            last_known[128];

  gint            LED_ring_behavior_off;
  gint            LED_ring_behavior_pan;
  gint            LED_ring_behavior_fan;
  gint            LED_ring_behavior_trim;
} MidiDevice;

typedef struct _GMidiSource
{
  GSource         source;
  MidiDevice *device;
} GMidiSource;

static gboolean knob_config_mode = FALSE;
static GtkWidget *mapping_widget = NULL;

void midi_config_save(MidiDevice *midi)
{
  FILE *f = 0;

  gchar datadir[PATH_MAX] = { 0 };
  gchar midipath[PATH_MAX] = { 0 };

  dt_loc_get_user_config_dir(datadir, sizeof(datadir));
  snprintf(midipath, sizeof(midipath), "%s/midirc-%s", datadir, midi->model_name);

  f = g_fopen(midipath, "w");
  if(!f) return;

  g_fprintf(f, "num_columns=%d\n", midi->num_columns);
  g_fprintf(f, "group_switch_key=%d\n", midi->group_switch_key);
  g_fprintf(f, "group_key_light=%d\n", midi->group_key_light);
  g_fprintf(f, "rating_key_light=%d\n", midi->rating_key_light);
  g_fprintf(f, "reset_knob_key=%d\n", midi->reset_knob_key);
  g_fprintf(f, "first_knob_key=%d\n", midi->first_knob_key);
  g_fprintf(f, "num_rotators=%d\n", midi->num_rotators);
  g_fprintf(f, "LED_ring_behavior_off=%d\n", midi->LED_ring_behavior_off);
  g_fprintf(f, "LED_ring_behavior_pan=%d\n", midi->LED_ring_behavior_pan);
  g_fprintf(f, "LED_ring_behavior_fan=%d\n", midi->LED_ring_behavior_fan);
  g_fprintf(f, "LED_ring_behavior_trim=%d\n", midi->LED_ring_behavior_trim);

  g_fprintf(f, "\ngroup,channel,key,path,encoding,accel\n");

  GSList *l = midi->mapping_list;
  while (l)
  {
    dt_midi_knob_t *k = (dt_midi_knob_t *)l->data;

    gchar *spath = g_strndup( k->accelerator->path,strlen(k->accelerator->path)-strlen("/dynamic") );

    g_fprintf(f,"%d,%d,%d,%s,%d,%.4f\n", 
                k->group, k->channel, k->key, spath, k->encoding, k->acceleration);

    g_free(spath);

    l = g_slist_next(l);
  }  

  g_fprintf(f, "\ngroup,channel,key,path\n");

  l = midi->note_list;
  while (l)
  {
    dt_midi_note_t *n = (dt_midi_note_t *)l->data;
    g_fprintf(f,"%d,%d,%d,%s\n", 
                n->group, n->channel, n->key, 
                gtk_accelerator_name(n->accelerator_key,n->accelerator_mods));

    l = g_slist_next(l);
  }  

  fclose(f);
}

void midi_config_load(MidiDevice *midi)
{
  midi->config_loaded = TRUE;

  if (strstr(midi->model_name, "X-TOUCH MINI"))
  {
    midi->group_switch_key = 16;
    midi->group_key_light = 8;
    midi->rating_key_light = 0;
    midi->reset_knob_key = 0;
    midi->first_knob_key = 1;
    midi->num_rotators = 8;
    midi->LED_ring_behavior_off = 0;
    midi->LED_ring_behavior_pan = 1;
    midi->LED_ring_behavior_fan = 2;
    midi->LED_ring_behavior_trim = 4;
  }
  else if (strstr(midi->model_name, "Arturia BeatStep"))
  {
    midi->group_switch_key = 0;
    midi->group_key_light = 0;
    midi->rating_key_light = 8;
  }

  FILE *f = 0;

  int read = 0;

  gchar datadir[PATH_MAX] = { 0 };
  gchar midipath[PATH_MAX] = { 0 };

  dt_loc_get_user_config_dir(datadir, sizeof(datadir));
  snprintf(midipath, sizeof(midipath), "%s/midirc-%s", datadir, midi->model_name);

  f = g_fopen(midipath, "rb");
  
  dt_loc_get_datadir(datadir, sizeof(datadir));
  snprintf(midipath, sizeof(midipath), "%s/midi/midirc-%s", datadir, midi->model_name);

  while (!f && strlen(midipath)>strlen(datadir))
  {
    f = g_fopen(midipath, "rb");
    if (strrchr(midipath,' ') != NULL)
      *(strrchr(midipath,' ')) = 0;
    else
      *midipath = 0;
  }

  if(!f) return;

  char buffer[200];

  while(fgets(buffer, 100, f))
  {
    if (sscanf(buffer, "num_columns=%d\n", &midi->num_columns) == 1) continue;
    if (sscanf(buffer, "group_switch_key=%d\n", &midi->group_switch_key) == 1) continue;
    if (sscanf(buffer, "group_key_light=%d\n", &midi->group_key_light) == 1) continue;
    if (sscanf(buffer, "rating_key_light=%d\n", &midi->rating_key_light) == 1) continue;
    if (sscanf(buffer, "reset_knob_key=%d\n", &midi->reset_knob_key) == 1) continue;
    if (sscanf(buffer, "first_knob_key=%d\n", &midi->first_knob_key) == 1) continue;
    if (sscanf(buffer, "num_rotators=%d\n", &midi->num_rotators) == 1) continue;
    if (sscanf(buffer, "LED_ring_behavior_off=%d\n", &midi->LED_ring_behavior_off) == 1) continue;
    if (sscanf(buffer, "LED_ring_behavior_pan=%d\n", &midi->LED_ring_behavior_pan) == 1) continue;
    if (sscanf(buffer, "LED_ring_behavior_fan=%d\n", &midi->LED_ring_behavior_fan) == 1) continue;
    if (sscanf(buffer, "LED_ring_behavior_trim=%d\n", &midi->LED_ring_behavior_trim) == 1) continue;

    gint group, channel, key, encoding;
    char accelpath[200];
    float acceleration;

    read = sscanf(buffer, "%d,%d,%d,%[^,],%d,%f\n", 
                  &group, &channel, &key, accelpath, &encoding, &acceleration);
    if(read == 6)
    {
      g_strlcat(accelpath,"/dynamic",200);
      GSList *al = darktable.control->dynamic_accelerator_list;
      dt_accel_dynamic_t *da;
      while(al)
      {
        da = (dt_accel_dynamic_t *)al->data;
        if (!g_strcmp0(da->path, accelpath))
          break;

        al = g_slist_next(al);
      }
      if (al)
      {
        dt_midi_knob_t *k = (dt_midi_knob_t *)g_malloc(sizeof(dt_midi_knob_t));

        k->group = group;
        k->channel = channel;
        k->key = key;
        k->accelerator = da;
        k->encoding = encoding;
        k->acceleration = acceleration;

        midi->mapping_list = g_slist_append(midi->mapping_list, k);
      }
      continue;
    }

    read = sscanf(buffer, "%d,%d,%d,%[^\r\n]\r\n",
                  &group, &channel, &key, accelpath);

    if (read == 4)
    {
      guint accelerator_key;
      GdkModifierType accelerator_mods;
      gtk_accelerator_parse(accelpath, &accelerator_key, &accelerator_mods);
      if (accelerator_key)
      {
        dt_midi_note_t *n = (dt_midi_note_t *)g_malloc(sizeof(dt_midi_note_t));

        n->group = group;
        n->channel = channel;
        n->key = key;
        n->accelerator_key = accelerator_key;
        n->accelerator_mods = accelerator_mods;

        midi->note_list = g_slist_append(midi->note_list, n);
      }
      continue;
    }
  }

  fclose(f);
}

void midi_write(MidiDevice *midi, gint channel, gint type, gint key, gint velocity)
{
#ifdef HAVE_ALSA
  if (midi->sequencer)
  {
    snd_seq_event_t event;
    snd_seq_ev_clear(&event);

    switch (type)
    {
    case 0xB:
      event.type = SND_SEQ_EVENT_CONTROLLER;
      event.data.control.channel = channel;
      event.data.control.param = key;
      event.data.control.value = velocity;
      break;
    case 0x9:
      event.type = SND_SEQ_EVENT_NOTEON;
      event.data.note.channel = channel;
      event.data.note.note = key;
      event.data.note.velocity = velocity;
      break;
    default:
      event.type = SND_SEQ_EVENT_SYSTEM;
      break;
    }

    snd_seq_ev_set_subs(&event);  
    snd_seq_ev_set_direct(&event);
    snd_seq_ev_set_source(&event, midi->port);

    int alsa_error = snd_seq_event_output_direct(midi->sequencer, &event);
    if (alsa_error < 0)
    {
      D (g_print ("ALSA output error: %s\n", snd_strerror(alsa_error)));
    }
  } else
#endif /* HAVE_ALSA */
#ifdef HAVE_PORTMIDI
  if (midi->portmidi_out)
  {
    PmMessage message = Pm_Message((type << 4) + channel, key, velocity);
    PmError pmerror = Pm_WriteShort(midi->portmidi_out, 0, message);
    if (pmerror != pmNoError)
    {
      g_print("Portmidi error: %s\n", Pm_GetErrorText(pmerror));
    }
  }
  else
#endif
  if (midi->io)
  {
    gchar buf[3];
    buf[0] = (type << 4) + channel;
    buf[1] = key;
    buf[2] = velocity;

    write(g_io_channel_unix_get_fd (midi->io), buf, 3);
  }
}

void refresh_sliders_to_device(MidiDevice *midi)
{
  if (!midi->config_loaded)
  {
    midi_config_load(midi);
  }

  midi->syncing = FALSE;

  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
  {
    GSList *l = midi->mapping_list;
    while(l)
    {
      dt_midi_knob_t *k = (dt_midi_knob_t *)l->data;

      if (k->encoding == MIDI_ABSOLUTE)
      {
        if (k != midi->stored_knob)
        {
          k->locked = FALSE;
        }

        GtkWidget *w = k->accelerator->widget;
        if (k->group == midi->group && w)
        {
          float min = dt_bauhaus_slider_get_soft_min(w);
          float max = dt_bauhaus_slider_get_soft_max(w);
          float c   = dt_bauhaus_slider_get(w);
          
          int velocity = round((c-min)/(max-min)*127);

          if (velocity != midi->last_known[k->key])
          {
            midi_write(midi, k->channel, 0xB, k->key, velocity);
            midi->last_known[k->key] = velocity;
          }

          // For Behringer; set pattern of rotator lights
          if (k->key < 9)
          {
            if (min == -max)
              midi_write(midi, 0, 0xB, k->key, midi->LED_ring_behavior_trim);
            else if (min == 0 && (max == 1 || max == 100))
              midi_write(midi, 0, 0xB, k->key, midi->LED_ring_behavior_fan);
            else
              midi_write(midi, 0, 0xB, k->key, midi->LED_ring_behavior_pan);
          }
        }
      }
      l = g_slist_next(l);
    }

    int image_id = darktable.develop->image_storage.id;

    if (midi->rating_key_light != -1)
    {
      int on_lights = 0;

      if (image_id != -1)
      {
        if (midi->group == 1)
        {
          int rating = dt_ratings_get(image_id);
          if (rating == 6) // if rating=reject, show x0x0x pattern
          {
            on_lights = 1+4+16;
          }
          else
          {
            on_lights = 31 >> (5-rating);
          }
        }
        else if (midi->group == 2)
        {
          on_lights = dt_colorlabels_get_labels(image_id);
        }
      }

      for (int light = 0; light < midi->num_columns; light++)
      {
        midi_write(midi, 0, 0x9, light + midi->rating_key_light, on_lights & 1);
        on_lights >>= 1;
      }
    }

    midi_write(midi, 0, 0x9, midi->group + midi->group_key_light - 1, 1);
  }
}

void refresh_all_devices(gpointer data)
{
  GSList *l = (GSList *)(((dt_lib_module_t *)data)->data);
  while (l)
  {
    refresh_sliders_to_device( (MidiDevice *)l->data);
    l = g_slist_next(l);
  }
}

static void callback_slider_changed(GtkWidget *w, gpointer data)
{
  if (knob_config_mode)
  {
    mapping_widget = w;

    dt_control_hinter_message
            (darktable.control, (""));

    dt_control_log(_("slowly move midi controller down to connect"));
  }
  else
  {
    refresh_all_devices(data);
  }

  return;
}

static void callback_image_changed(gpointer instance, gpointer data)
{
  refresh_all_devices(data);
}

static void callback_view_changed(gpointer instance, dt_view_t *old_view, dt_view_t *new_view, gpointer data)
{
  if (new_view->view(new_view) == DT_VIEW_DARKROOM)
  { 
    GSList *l = darktable.control->dynamic_accelerator_list;
    while(l)
    {
      dt_accel_dynamic_t *da = (dt_accel_dynamic_t *)l->data;

      if (da->widget)
      {
        g_signal_connect(G_OBJECT(da->widget), "value-changed", G_CALLBACK(callback_slider_changed), data);
      }

      l = g_slist_next(l);
    }
    
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                              G_CALLBACK(callback_image_changed), data);

    dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                              G_CALLBACK(callback_image_changed), data);
  }
  else
  {
    dt_control_signal_disconnect(darktable.signals, 
                                G_CALLBACK(callback_image_changed), data);
  }

  refresh_all_devices(data);
}

gint interpret_move(dt_midi_knob_t *k, gint velocity)
{
    if (k)
    {
      switch(k->encoding)
      {
        case 127: // 2s Complement
          if (velocity < 65)
            return velocity;
          else
            return velocity - 128;
          break;
        case 63: // Offset
          return velocity - 64;
          break;
        case 33: // Sign
          if (velocity < 32)
            return velocity;
          else
            return 32 - velocity;
          break;
        case 15: // Offset 5 bit
          return velocity - 16;
          break;
        case 65: // Sign 6 bit (x-touch mini in MC mode) 
          if (velocity < 64)
            return velocity;
          else
            return 64 - velocity;
          break;
        default:
          return 0;
          break;
      }
    }
    else
    {
      return 0;
    }
}

//  Currently just aggregates one channel/key combination 
//  and sends when changing to different key. This might still
//  cause flooding if multiple knobs are turned simultaneously
//  and alternating codes are received.
//  To deal with this would require maintaining a list of currently
//  changed keys and send all at end. Probably not worth extra complexity,
//  Since it would still mean sending multiple updates in one iteration 
//  which could cause flooding anyway.
void aggregate_and_set_slider(MidiDevice *midi,
                              gint channel, gint key, gint velocity)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view((dt_view_t *)cv) != DT_VIEW_DARKROOM) return;

  if ((channel == midi->stored_channel) && (key == midi->stored_key))
  {
    if (midi->stored_knob == NULL || midi->stored_knob->encoding == MIDI_ABSOLUTE)
        midi->accum = velocity; // override; just use last one
    else
        midi->accum += interpret_move(midi->stored_knob, velocity);
  }
  else
  {
    if (midi->stored_channel != -1)
    {
      if (midi->stored_knob)
      {
        GtkWidget *w = midi->stored_knob->accelerator->widget;

        if (w)
        {
          float v = dt_bauhaus_slider_get(w);
          float s = dt_bauhaus_slider_get_step(w);

          int move = midi->accum;

          if (midi->stored_knob->encoding == MIDI_ABSOLUTE)
          {
            midi->last_known[midi->stored_key] = midi->accum;

            float wmin = dt_bauhaus_slider_get_soft_min(w);
            float wmax = dt_bauhaus_slider_get_soft_max(w);

            int location = round((v-wmin)/(wmax-wmin)*127);
            move -= location;

            // attempt to limit number of steps if acceleration too high to avoid flipping back and forth between ends of range
            int direct_steps = fabsf((v - (wmin + midi->accum * (wmax-wmin)/127))/(s * midi->stored_knob->acceleration))+1;
            move = MIN(direct_steps,MAX(-direct_steps,move));

            if (midi->stored_knob->locked ||
                abs(move) <= 1)
            {
              midi->stored_knob->locked = TRUE;

              if (midi->syncing)
              {
                dt_control_log((">%s/%s<"), 
                                  DT_BAUHAUS_WIDGET(w)->module->name(),
                                  DT_BAUHAUS_WIDGET(w)->label);

                midi->syncing = FALSE;
              }
            }
            else
            {
              if (midi->syncing)
              {
                gchar *left_text  = g_strnfill(MAX(1, move)-1,'<');
                gchar *right_text = g_strnfill(MAX(1, -move)-1,'>');
                
                dt_control_log(("%s %s/%s %s"), 
                                left_text, DT_BAUHAUS_WIDGET(w)->module->name(),
                                DT_BAUHAUS_WIDGET(w)->label, right_text);
                
                g_free(left_text);
                g_free(right_text);
              }

              // if one knob is out of sync, all on same device may need syncing
              refresh_sliders_to_device(midi);
              midi->syncing = TRUE;
              move = 0;
            }
          }
          if (move != 0)
          {
            if (knob_config_mode)
            {
              // configure acceleration setting
              if (move > 0)
              {
                midi->stored_knob->acceleration *= 2;
              }
              else
              {
                midi->stored_knob->acceleration /= 2;
              }

              dt_control_log(_("knob acceleration %.2f"), midi->stored_knob->acceleration);

              midi_config_save(midi);

              knob_config_mode = FALSE;

              channel = -1;
              key = -1;
            }
            else
            {
              dt_bauhaus_slider_set(w, v + s * midi->stored_knob->acceleration * move);
            }
          }
        }
      }
    }

    midi->stored_knob = NULL;

    if (channel != -1)
    {
      if (mapping_widget)
      {
        knob_config_mode = FALSE;

        // link knob to widget and set encoding type

        if (midi->mapping_channel == -1)
        {
          midi->mapping_channel  = channel;
          midi->mapping_key      = key;
          midi->mapping_velocity = velocity;
        }
        else
        {
          if ((velocity != 1) &&
              (channel == midi->mapping_channel) && (key == midi->mapping_key) &&
              ((midi->mapping_velocity == velocity) || (midi->mapping_velocity - velocity == 1)) )
          {
            // store new mapping in table, overriding existing

            GSList *al = darktable.control->dynamic_accelerator_list;
            dt_accel_dynamic_t *da = NULL ;
            while(al)
            {
              da = (dt_accel_dynamic_t *)al->data;
              if (da->widget == mapping_widget)
                break;

              al = g_slist_next(al);
            }

            dt_control_log(_("mapped to %s/%s"), 
                              DT_BAUHAUS_WIDGET(mapping_widget)->module->name(),
                              DT_BAUHAUS_WIDGET(mapping_widget)->label);

            dt_midi_knob_t *new_knob = NULL;

            GSList *l = midi->mapping_list;
            while(l)
            {
              dt_midi_knob_t *d = (dt_midi_knob_t *)l->data;
              if ((d->group > midi->group) |
                  ((d->group == midi->group) && 
                   ((d->channel > channel) |
                    ((d->channel == channel) && (d->key >= key)))))
              {
                if ((d->group == midi->group) && 
                    (d->channel == channel) && 
                    (d->key == key))
                {
                  new_knob = d;
                }
                else
                {
                  new_knob = (dt_midi_knob_t *)g_malloc(sizeof(dt_midi_knob_t));
                  midi->mapping_list = g_slist_insert_before(midi->mapping_list, l, new_knob);
                }
                break;
              }
              l = g_slist_next(l);
            }
            if (!new_knob)
            {
              new_knob = (dt_midi_knob_t *)g_malloc(sizeof(dt_midi_knob_t));
              midi->mapping_list = g_slist_append(midi->mapping_list, new_knob);
            }
            new_knob->group = midi->group;
            new_knob->channel = channel;
            new_knob->key = key;
            new_knob->acceleration = 1;
            new_knob->accelerator = da;
            if (midi->mapping_velocity - velocity == 1)
            {
              new_knob->encoding = MIDI_ABSOLUTE;
              new_knob->locked = FALSE;
            }
            else
            {
              new_knob->encoding = velocity | 1; // force last bit to 1
            }
          
            midi_config_save(midi);
          }

          dt_control_hinter_message (darktable.control, "");

          midi->mapping_channel = -1;
          mapping_widget = NULL;
        }
        
        channel = -1;
        key = -1;
      }
      else
      {
        GSList *l = midi->mapping_list;
        while(l)
        {
          dt_midi_knob_t *d = (dt_midi_knob_t *)l->data;
          if ((d->group > midi->group) |
              ((d->group == midi->group) && 
                ((d->channel > channel) |
                ((d->channel == channel) && (d->key >= key)))))
          {
            if ((d->group == midi->group) &&
                (d->channel == channel) && 
                (d->key == key))
            {
              midi->stored_knob = d;
            }
            break;
          }
          l = g_slist_next(l);
        }

        if (midi->stored_knob == NULL)
        {
          dt_control_log(_("knob %d on channel %d not mapped in group %d"), 
                           key, channel, midi->group);
        }
        else if (midi->stored_knob->encoding == MIDI_ABSOLUTE)
        {
          midi->accum = velocity;
        }
        else
        {
          midi->accum = interpret_move(midi->stored_knob, velocity);
        }
      }     
    }

    midi->stored_channel = channel;
    midi->stored_key = key;
  }
}

void note_on(MidiDevice *midi, gint channel, gint note)
{
  aggregate_and_set_slider(midi, -1, -1, 0);

  if (midi->group_switch_key == -1 || knob_config_mode == TRUE)
  {
    midi->group_switch_key = note;
    
    knob_config_mode = FALSE;
  }

  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);

  if (note >= midi->group_switch_key && note < midi->group_switch_key + midi->num_columns)
  {
    midi_write(midi, 0, 0x9, midi->group + midi->group_key_light - 1, 0); // try to switch off button light

    midi->group = note - midi->group_switch_key + 1;

    // try to initialise rotator lights off
    for (gint knob = midi->first_knob_key; knob <= midi->num_rotators; knob++)
    {
      midi_write(midi,       0, 0xB, knob, midi->LED_ring_behavior_off); // set single pattern on x-touch mini
      midi_write(midi, channel, 0xB, knob, 0);
      midi->last_known[knob] = 0; 
    }

    if(cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
    {    
      refresh_sliders_to_device(midi);

      char *help_text = g_strdup_printf("MIDI key group %d:", midi->group);
      char *tmp;

      char channel_text[30] = "";
      dt_iop_module_t *previous_module = NULL;
      gint remaining_line_items = 0;

      GSList *l = midi->mapping_list;
      gint current_channel = l? ((dt_midi_knob_t *)g_slist_last(l)->data)->channel: 0;
      while(l)
      {
        dt_midi_knob_t *d = (dt_midi_knob_t *)l->data;
        if (d->group > midi->group)
        {
          break;
        }

        if (d->group == midi->group && d->accelerator->widget != NULL)
        {
          if (d->channel != current_channel)
          {
            current_channel = d->channel;
            snprintf(channel_text, sizeof(channel_text), "(Channel %2d) ", current_channel);
            remaining_line_items = 0;
          }

          if (remaining_line_items-- == 0)
          {
            tmp = g_strdup_printf("%s\n%s", help_text, channel_text);
            g_free(help_text);
            help_text = tmp;

            // memset(channel_text, ' ', strlen(channel_text)); // only works with monospaced fonts
            remaining_line_items = midi->num_columns - 1;
          }

          dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(d->accelerator->widget);

          if (w->module == previous_module || strstr(w->module->name(), w->label))
          {
            tmp = g_strdup_printf("%s%2d: %s  ", help_text, d->key, w->label);
          }
          else
          {
            tmp = g_strdup_printf("%s%2d: %s/%s  ", help_text, d->key, w->module->name(), w->label);
            previous_module = w->module;
          }
          g_free(help_text);
          help_text = tmp;
        }
        l = g_slist_next(l);
      }

      dt_control_hinter_message(darktable.control, help_text);
      
      g_free(help_text);
    }
  }
  else if (cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM &&
           midi->reset_knob_key != -1 && 
           note >= midi->reset_knob_key && note < midi->reset_knob_key + midi->num_columns)
  {
    GSList *l = midi->mapping_list;
    while(l)
    {
      dt_midi_knob_t *d = (dt_midi_knob_t *)l->data;
      if ((d->group > midi->group) |
          ((d->group == midi->group) && 
            ((d->channel > channel) |
            ((d->channel == channel) && (d->key >= note - midi->reset_knob_key + midi->first_knob_key)))))
      {
        if ((d->group == midi->group) &&
            (d->channel == channel) && 
            (d->key == note - midi->reset_knob_key + midi->first_knob_key))
        {
          dt_bauhaus_slider_reset(d->accelerator->widget);
        }
        break;
      }
      l = g_slist_next(l);
    }
  }
  else
  {
    GSList *l = midi->note_list;
    while(l)
    {
      dt_midi_note_t *n = (dt_midi_note_t *)l->data;
      if ((n->group > midi->group) |
          ((n->group == midi->group) && 
            ((n->channel > channel) |
            ((n->channel == channel) && (n->key >= note)))))
      {
        if ((n->group == midi->group) &&
            (n->channel == channel) && 
            (n->key == note))
        {
          gtk_accel_groups_activate(G_OBJECT(dt_ui_main_window(darktable.gui->ui)), n->accelerator_key, n->accelerator_mods);
        } 
        break;
      }
      l = g_slist_next(l);
    }
  }
  

}

void note_off(MidiDevice *midi)
{
  refresh_sliders_to_device(midi);

  dt_control_hinter_message(darktable.control, _(""));
}

#ifdef HAVE_ALSA
static gboolean midi_alsa_prepare (GSource *source,
                                   gint    *timeout)
{
  MidiDevice *midi = (MidiDevice*) ((GMidiSource *) source)->device;
  gboolean        ready;

  ready = snd_seq_event_input_pending (midi->sequencer, 1) > 0;
  *timeout = ready ? 1 : 10;

  return ready;
}

static gboolean midi_alsa_check (GSource *source)
{
  MidiDevice *midi = (MidiDevice*) ((GMidiSource *) source)->device;

  return snd_seq_event_input_pending (midi->sequencer, 1) > 0;
}

static gboolean midi_alsa_dispatch (GSource     *source,
                                    GSourceFunc  callback,
                                    gpointer     user_data)
{
  MidiDevice *midi = (MidiDevice*)((GMidiSource *) source)->device;

  snd_seq_event_t       *event;
  snd_seq_client_info_t *client_info;

  gboolean return_value = FALSE;
  while ( snd_seq_event_input_pending (midi->sequencer, 1) > 0 )
  {
    return_value = TRUE;

    snd_seq_event_input (midi->sequencer, &event);

    if (event->type == SND_SEQ_EVENT_NOTEON &&
        event->data.note.velocity == 0)
    {
      event->type = SND_SEQ_EVENT_NOTEOFF;
    }

    switch (event->type)
    {
      case SND_SEQ_EVENT_NOTEON:
        note_on(midi, event->data.note.channel, event->data.note.note);
        break;

      case SND_SEQ_EVENT_NOTEOFF:
        note_off(midi);
        break;

      case SND_SEQ_EVENT_CONTROLLER:
        aggregate_and_set_slider(midi, event->data.control.channel, 
                                       event->data.control.param, 
                                       event->data.control.value);
        break;

      case SND_SEQ_EVENT_PORT_SUBSCRIBED:
        snd_seq_client_info_malloc(&client_info);
        snd_seq_get_any_client_info (midi->sequencer,
                                     event->data.connect.sender.client,
                                     client_info);

        if (g_strcmp0(snd_seq_client_info_get_name (client_info),"darktable"))
        {
          g_free(midi->model_name);
          midi->model_name = g_strdup(snd_seq_client_info_get_name (client_info));
          g_print("Alsa device name: %s\n", midi->model_name);
          midi->config_loaded = FALSE;
        }

        snd_seq_client_info_free(client_info);
        break;

      case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
        break;

      default:
        break;
    }
  }

  aggregate_and_set_slider(midi, -1, -1, -1);

  return return_value;
}

static GSourceFuncs source_funcs_alsa =
{
  midi_alsa_prepare,
  midi_alsa_check,
  midi_alsa_dispatch,
  NULL
};
#endif /* HAVE_ALSA */

#ifdef HAVE_PORTMIDI
static gboolean midi_port_prepare (GSource *source,
                                   gint    *timeout)
{
  MidiDevice *midi = (MidiDevice*) ((GMidiSource *) source)->device;
  gboolean        ready;

  ready = Pm_Poll (midi->portmidi);
  *timeout = ready ? 1 : 10;

  return ready;
}

static gboolean midi_port_check (GSource *source)
{
  MidiDevice *midi = (MidiDevice*) ((GMidiSource *) source)->device;

  return Pm_Poll (midi->portmidi);
}

static gboolean midi_port_dispatch (GSource     *source,
                                    GSourceFunc  callback,
                                    gpointer     user_data)
{
  MidiDevice *midi = (MidiDevice*)((GMidiSource *) source)->device;

  PmEvent       event;

  gboolean return_value = FALSE;
  while ( Pm_Poll (midi->portmidi) > 0 )
  {
    return_value = TRUE;

    Pm_Read (midi->portmidi, &event, 1);

    int eventStatus = Pm_MessageStatus(event.message);
    int eventData1 = Pm_MessageData1(event.message);
    int eventData2 = Pm_MessageData2(event.message);

    int eventType = eventStatus >> 4;
    int eventChannel = eventStatus & 0x0F;

    if (eventType == 0x9 && // note on
        eventData2 == 0)
    {
      eventType = 0x8; // note off
    }

    switch (eventType)
    {
      case 0x9:  // note on
        g_print("Note: Channel %d, Data1 %d\n", eventChannel, eventData1);
        note_on(midi, eventChannel, eventData1);
        break;

      case 0x8:  // note off
        note_off(midi);
        break;

      case 0xb:  // controllers, sustain
        g_print("Controller: Channel %d, Data1 %d, Data2 %d\n", eventChannel, eventData1, eventData2);
        aggregate_and_set_slider(midi, eventChannel, eventData1, eventData2);
        break;

      default:
        break;
    }
  }

  aggregate_and_set_slider(midi, -1, -1, -1);

  return return_value;
}

static GSourceFuncs source_funcs_portmidi =
{
  midi_port_prepare,
  midi_port_check,
  midi_port_dispatch,
  NULL
};
#endif /* HAVE_PORTMIDI */

gboolean midi_read_event (GIOChannel   *io,
                          GIOCondition  cond,
                          gpointer      data)
{
  MidiDevice *midi = (MidiDevice*) data;
  GIOStatus       status;
  GError         *error = NULL;
  guchar          buf[0x3F];
  gsize           size;
  gint            pos = 0;

  status = g_io_channel_read_chars (io,
                                    (gchar *) buf,
                                    sizeof (buf), &size,
                                    &error);

  switch (status)
  {
    case G_IO_STATUS_ERROR:
    case G_IO_STATUS_EOF:
      g_source_remove (midi->io_id);
      midi->io_id = 0;

      g_io_channel_unref (midi->io);
      midi->io = NULL;

      if (error)
      {
        g_clear_error (&error);
      }
      return FALSE;
      break;

    case G_IO_STATUS_AGAIN:
      return TRUE;

    default:
      break;
  }

  if (!midi->name_queried)
  {
      // Send Universal Device Inquiry message 
      char inquiry[6] = "\xF0\x7E\x7F\x06\x01\xF7";
      write(g_io_channel_unix_get_fd (midi->io), inquiry, 6);

      midi->name_queried = TRUE;
      return TRUE; // ignore rest of input
  }

  while (pos < size)
  {
    if (buf[pos] & 0x80)  /* status byte */
    {
      if (buf[pos] >= 0xf8)  /* realtime messages */
      {
        switch (buf[pos])
        {
          case 0xf8:  /* timing clock   */
          case 0xf9:  /* (undefined)    */
          case 0xfa:  /* start          */
          case 0xfb:  /* continue       */
          case 0xfc:  /* stop           */
          case 0xfd:  /* (undefined)    */
          case 0xfe:  /* active sensing */
          case 0xff:  /* system reset   */
            break;
        }
      }
      else
      {
        midi->swallow = FALSE;  /* any status bytes ends swallowing */

        if (buf[pos] >= 0xf0)  /* system messages */
        {
          switch (buf[pos])
          {
            case 0xf0:  /* sysex start */
              midi->swallow = TRUE;

              D (g_print ("MIDI: sysex start\n"));

              // Look for:
              // F0 7E [??] 06 02 Universal Device Reply
              // In response to f0 7e 7f 06 01 f7 Universal Device Inquiry
              // that we sent earlier.
              if ( (buf[pos+1] & 0xFE) == 0x7E &&
                   (buf[pos+3] == 6) )
                pos++;

              if ( buf[pos+2] == 6 && buf[pos+3] == 2 )
              {
                //00 00 0E Alesis Manufacturer ID
                //0E 00    QS Family ID, LSB first
                //0x 00    QS Family Member, LSB first
                //xx xx xx xx Software revision, ASCI (ex. 30 31 30 30 = '0100' = 1.00)
                //F7 End-Of-Exclusive        
                //Arturia Beatstep responds with:
                //7e 00 06 02 00 20 6b 02 00 06 00 03 00 02 01
                //turn this into string 00206B_0002_0006
                g_free(midi->model_name);
                midi->model_name = g_strdup_printf(
                                "%02X%02X%02X_%02X%02X_%02X%02X",
                                buf[pos+4],buf[pos+5],buf[pos+6],
                                buf[pos+8],buf[pos+7],
                                buf[pos+10],buf[pos+9]);
              }
              break;

            case 0xf1:              /* time code   */
              midi->swallow = TRUE; /* type + data */

              D (g_print ("MIDI: time code\n"));
              break;

            case 0xf2:              /* song position */
              midi->swallow = TRUE; /* lsb + msb     */

              D (g_print ("MIDI: song position\n"));
              break;

            case 0xf3:              /* song select */
              midi->swallow = TRUE; /* song number */

              D (g_print ("MIDI: song select\n"));
              break;

            case 0xf4:  /* (undefined) */
            case 0xf5:  /* (undefined) */
              D (g_print ("MIDI: undefined system message\n"));
              break;

            case 0xf6:  /* tune request */
              D (g_print ("MIDI: tune request\n"));
              break;

            case 0xf7:  /* sysex end */
              D (g_print ("MIDI: sysex end\n"));
              break;
          }
        }
        else  /* channel messages */
        {
          midi->command = buf[pos] >> 4;
          midi->channel = buf[pos] & 0xf;

          /* reset running status */
          midi->key      = -1;
          midi->velocity = -1;
          midi->msb      = -1;
          midi->lsb      = -1;
        }
      }

      pos++;  /* status byte consumed */
      continue;
    }

    if (midi->swallow)
    {
      pos++;  /* consume any data byte */
      continue;
    }

    switch (midi->command)
    {
      case 0x8:  /* note off   */
      case 0x9:  /* note on    */
      case 0xa:  /* aftertouch */

        if (midi->key == -1)
        {
          midi->key = buf[pos++];  /* key byte consumed */
          continue;
        }

        if (midi->velocity == -1)
          midi->velocity = buf[pos++];  /* velocity byte consumed */

        /* note on with velocity = 0 means note off */
        if (midi->command == 0x9 && midi->velocity == 0x0)
          midi->command = 0x8;

        if (midi->command == 0x9)
        {
          D (g_print ("MIDI (ch %02d): note on  (%02x vel %02x)\n",
                      midi->channel, midi->key, midi->velocity));

          note_on(midi, midi->channel, midi->key);
        }
        else if (midi->command == 0x8)
        {
          D (g_print ("MIDI (ch %02d): note off (%02x vel %02x)\n",
                      midi->channel, midi->key, midi->velocity));

          note_off(midi);
        }
        else
        {
          D (g_print ("MIDI (ch %02d): polyphonic aftertouch (%02x pressure %02x)\n",
                      midi->channel, midi->key, midi->velocity));
        }

        midi->key      = -1;
        midi->velocity = -1;
        break;

      case 0xb:  /* controllers, sustain */

        if (midi->key == -1)
        {
          midi->key = buf[pos++];
          continue;
        }

        if (midi->velocity == -1)
          midi->velocity = buf[pos++];

        D (g_print ("MIDI (ch %02d): controller %d (value %d)\n",
                    midi->channel, midi->key, midi->velocity));

        aggregate_and_set_slider(midi, midi->channel, midi->key, midi->velocity);

        midi->key      = -1;
        midi->velocity = -1;
        break;

      case 0xc:  /* program change */
        midi->key = buf[pos++];

        D (g_print ("MIDI (ch %02d): program change (%d)\n",
                    midi->channel, midi->key));

        midi->key = -1;
        break;

      case 0xd:  /* channel key pressure */
        midi->velocity = buf[pos++];

        D (g_print ("MIDI (ch %02d): channel aftertouch (%d)\n",
                    midi->channel, midi->velocity));

        midi->velocity = -1;
        break;

      case 0xe:  /* pitch bend */
        if (midi->lsb == -1)
        {
          midi->lsb = buf[pos++];
          continue;
        }

        if (midi->msb == -1)
          midi->msb = buf[pos++];

        midi->velocity = midi->lsb | (midi->msb << 7);

        D (g_print ("MIDI (ch %02d): pitch (%d)\n",
                    midi->channel, midi->velocity));

        midi->msb      = -1;
        midi->lsb      = -1;
        midi->velocity = -1;
        break;
    }
  }

  aggregate_and_set_slider(midi, -1, -1, 0);

  return TRUE;
}

gboolean midi_device_init(MidiDevice *midi, const gchar *device)
{
  if (device && strlen (device))
  {
    midi->device = g_strdup (device);

    midi->model_name = g_strdelimit (g_strdup (device), "/", '_');

    midi->io           = NULL;
    midi->io_id        = 0;
#ifdef HAVE_ALSA
    midi->sequencer    = NULL;
#endif
#ifdef HAVE_PORTMIDI
    midi->portmidi     = NULL;
    midi->portmidi_out = NULL;
#endif
    midi->source_id    = 0;

    midi->name_queried = FALSE;

    midi->swallow      = TRUE; /* get rid of data bytes at start of stream */
    midi->command      = 0x0;
    midi->channel      = 0x0;
    midi->key          = -1;
    midi->velocity     = -1;
    midi->msb          = -1;
    midi->lsb          = -1;

    midi->config_loaded    = FALSE;
    midi->mapping_list     = NULL;
    midi->note_list        = NULL;

    midi->mapping_channel  = -1;
    midi->mapping_key      = -1;
    midi->mapping_velocity = -1;

    midi->accum            =  0;
    midi->stored_channel   = -1;
    midi->stored_key       = -1;
    midi->stored_knob    = NULL;

    midi->group            =  1;
    midi->num_columns      =  8;
    midi->group_switch_key = -1;
    midi->group_key_light  = -100;
    midi->rating_key_light = -1;
    midi->reset_knob_key   = -1;
    midi->first_knob_key   = -1;
    midi->num_rotators     = -100;

    midi->LED_ring_behavior_off = 0;
    midi->LED_ring_behavior_pan = 0;
    midi->LED_ring_behavior_fan = 0;
    midi->LED_ring_behavior_trim = 0; 

#ifdef HAVE_ALSA
    if (! g_ascii_strcasecmp (midi->device, "alsa"))
    {
      GSource *event_source;
      gint     ret;

      ret = snd_seq_open (&midi->sequencer, "default",
                          SND_SEQ_OPEN_DUPLEX, 0);
      if (ret >= 0)
      {
        snd_seq_set_client_name (midi->sequencer, _("darktable"));
        ret = snd_seq_create_simple_port (midi->sequencer,
                                          _("darktable MIDI"),
                                          SND_SEQ_PORT_CAP_WRITE |
                                          SND_SEQ_PORT_CAP_SUBS_WRITE |
                                          SND_SEQ_PORT_CAP_READ |
                                          SND_SEQ_PORT_CAP_SUBS_READ |
                                          SND_SEQ_PORT_CAP_DUPLEX,
                                          SND_SEQ_PORT_TYPE_APPLICATION);
        midi->port = ret;
      }

      if (ret < 0)
      {
        D (g_print(_("Device not available: %s\n"), snd_strerror (ret)));
        if (midi->sequencer)
        {
          snd_seq_close (midi->sequencer);
          midi->sequencer = NULL;
        }

        return FALSE;
      }

      event_source = g_source_new (&source_funcs_alsa,
                                   sizeof (GMidiSource));

      ((GMidiSource *) event_source)->device = midi;

      midi->source_id = g_source_attach (event_source, NULL);
      g_source_unref (event_source);

      return TRUE;
    }
#endif /* HAVE_ALSA */

#ifdef HAVE_PORTMIDI
    if (! g_ascii_strcasecmp (midi->device, "portmidi"))
    {
      PmDeviceID defaultPM = Pm_GetDefaultInputDeviceID();
      if (defaultPM == pmNoDevice) return FALSE;

      const PmDeviceInfo *portmidi_info = Pm_GetDeviceInfo( defaultPM );
      if (portmidi_info == NULL) return FALSE;

      g_print("Portmidi device name: %s\n", (char *)portmidi_info->name);
      
      PmError pmerror = Pm_OpenInput(&midi->portmidi, defaultPM, NULL, 1000, NULL, NULL);
      if (pmerror != pmNoError)
      {
        g_print("Portmidi error: %s\n", Pm_GetErrorText(pmerror));
        return FALSE;
      }

      defaultPM = Pm_GetDefaultOutputDeviceID();
      if (defaultPM == pmNoDevice) return FALSE;

      portmidi_info = Pm_GetDeviceInfo( defaultPM );
      if (portmidi_info == NULL) return FALSE;

      // use output device for midi ID, since input more likely rerouted via key interpreter
      g_print("Portmidi output device name: %s\n", (char *)portmidi_info->name);
      g_free(midi->model_name);
      midi->model_name = g_strdup(portmidi_info->name);
      
      pmerror = Pm_OpenOutput(&midi->portmidi_out, defaultPM, NULL, 1000, NULL, NULL, 0);
      if (pmerror != pmNoError)
      {
        g_print("Portmidi error: %s\n", Pm_GetErrorText(pmerror));
        return FALSE;
      }
      GSource *event_source = g_source_new (&source_funcs_portmidi,
                                   sizeof (GMidiSource));

      ((GMidiSource *) event_source)->device = midi;

      midi->source_id = g_source_attach (event_source, NULL);
      g_source_unref (event_source);

      return TRUE;
    }
#endif /* HAVE_PORTMIDI */

    gint fd;

#ifdef G_OS_WIN32
    fd = g_open (midi->device, O_RDWR, 0);
#else
    fd = g_open (midi->device, O_RDWR | O_NONBLOCK, 0);
#endif

    if (fd >= 0)
    {
      midi->io = g_io_channel_unix_new (fd);
      g_io_channel_set_close_on_unref (midi->io, TRUE);
      g_io_channel_set_encoding (midi->io, NULL, NULL);

      midi->io_id = g_io_add_watch (midi->io,
                                    G_IO_IN  | G_IO_ERR |
                                    G_IO_HUP | G_IO_NVAL,
                                    midi_read_event,
                                    midi);

      return TRUE;
    }
  }

  return FALSE;
}

void midi_device_free(MidiDevice *midi)
{
  if (midi->source_id)
  {
    g_source_remove (midi->source_id);
    midi->source_id = 0;
  }
#ifdef HAVE_ALSA
  if (midi->sequencer)
  {
    snd_seq_close (midi->sequencer);
    midi->sequencer = NULL;
  }
#endif /* HAVE_ALSA */
#ifdef HAVE_PORTMIDI
  if (midi->portmidi)
  {
    Pm_Close(midi->portmidi);

    if (midi->portmidi_out)
    {
      Pm_Close(midi->portmidi_out);
    }
  }
#endif /* HAVE_PORTMIDI */

  if (midi->io)
  {
    g_source_remove (midi->io_id);
    midi->io_id = 0;

    g_io_channel_unref (midi->io);
    midi->io = NULL;
  }

  if (midi->device)
    g_free(midi->device);

  if (midi->model_name)
    g_free(midi->model_name);

  g_slist_free_full (midi->mapping_list, g_free);
  g_slist_free_full (midi->note_list, g_free);
}

void midi_open_devices(dt_lib_module_t *self)
{
#ifdef HAVE_PORTMIDI
  Pm_Initialize();
#endif

  gchar *midi_devices = dt_conf_get_string("plugins/lighttable/midi/devices");
  if (midi_devices == NULL || midi_devices[0] == '\0')
  {
    g_free(midi_devices);
    midi_devices = g_strdup(midi_devices_default);
  }

  gchar **devices = g_strsplit_set(midi_devices,",",-1);
  g_free(midi_devices);
  gchar **cur_device = devices;

  while (*cur_device)
  {
    MidiDevice *midi = (MidiDevice *)g_malloc0(sizeof(MidiDevice));

    if (midi_device_init (midi, *cur_device))
    {
      self->data = g_slist_append(self->data, midi);
    }
    else
    {
      midi_device_free(midi);
    }

    cur_device++;
  }
  g_strfreev(devices);

  if (self->data)
  {
    dt_control_signal_connect(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                              G_CALLBACK(callback_view_changed), self);
  }
}

void midi_close_devices(dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, 
                               G_CALLBACK(callback_view_changed), self);

  g_slist_free_full (self->data, (void (*)(void *))midi_device_free);
  self->data = NULL;

#ifdef HAVE_PORTMIDI
  Pm_Terminate();
#endif
}

void gui_init(dt_lib_module_t *self)
{
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_no_show_all(self->widget, TRUE);

  self->data = NULL;

  midi_open_devices(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  midi_close_devices(self);
}

static gboolean callback_configure_midi(GtkAccelGroup *accel_group,
                                        GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier, gpointer data)
{
  if (!knob_config_mode && !mapping_widget)
  {
    knob_config_mode = TRUE;

    dt_control_hinter_message
            (darktable.control, _("move slider to connect to midi controller or move midi controller to change acceleration."));
  }
  else if(mapping_widget)
  {
    knob_config_mode = FALSE;
    mapping_widget = NULL;
  }
  else
  {
    knob_config_mode = FALSE;

    midi_close_devices(data);

    midi_open_devices(data);

    dt_control_log(_("Reopened all midi devices"));
  }

  return TRUE;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "configure knob"), GDK_KEY_M, GDK_CONTROL_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_accel_connect_lib(self, "configure knob",
                     g_cclosure_new(G_CALLBACK(callback_configure_midi), self, NULL));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
