/*
 * UFRaw - Unidentified Flying Raw converter for digital camera images
 *
 * wb_presets.c - White balance preset values for various cameras
 * Copyright 2004-2013 by Udi Fuchs
 *
 * Thanks goes for all the people who sent in the preset values
 * for their cameras.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>

#include "common/file_location.h"
#include "control/control.h"

#define DT_WB_PRESETS_VERSION 1

/* Column 1 - "make" of the camera.
 * Column 2 - "model" (use the "make" and "model" as provided by DCRaw).
 * Column 3 - WB name.
 * Column 4 - Fine tuning. MUST be in increasing order. 0 for no fine tuning.
 *	      It is enough to give only the extreme values, the other values
 *	      will be interpolated.
 * Column 5 - Channel multipliers.
 *
 * Minolta's ALPHA and MAXXUM models are treated as the Dynax model.
 *
 * WB name is standardized to one of the following: */

// "Sunlight" and other variation should be switched to this:
const char Daylight[] = N_("daylight");
// Probably same as above:
const char DirectSunlight[] = N_("direct sunlight");
const char Cloudy[] = N_("cloudy");
// "Shadows" should be switched to this:
const char Shade[] = N_("shade");
const char Incandescent[] = N_("incandescent");
const char IncandescentWarm[] = N_("incandescent warm");
// Same as "Incandescent":
const char Tungsten[] = N_("tungsten");
const char Fluorescent[] = N_("fluorescent");
// In Canon cameras and some newer Nikon cameras:
const char FluorescentHigh[] = N_("fluorescent high");
const char CoolWhiteFluorescent[] = N_("cool white fluorescent");
const char WarmWhiteFluorescent[] = N_("warm white fluorescent");
const char DaylightFluorescent[] = N_("daylight fluorescent");
const char NeutralFluorescent[] = N_("neutral fluorescent");
const char WhiteFluorescent[] = N_("white fluorescent");
// In some newer Nikon cameras:
const char SodiumVaporFluorescent[] = N_("sodium-vapor fluorescent");
const char DayWhiteFluorescent[] = N_("day white fluorescent");
const char HighTempMercuryVaporFluorescent[] = N_("high temp. mercury-vapor fluorescent");
// found in Nikon Coolpix P1000
const char HTMercury[] = N_("high temp. mercury-vapor");
// On Some Panasonic
const char D55[] = N_("D55");

const char Flash[] = N_("flash");
// For Olympus with no real "Flash" preset:
const char FlashAuto[] = N_("flash (auto mode)");
const char EveningSun[] = N_("evening sun");
const char Underwater[] = N_("underwater");
const char BlackNWhite[] = N_("black & white");

const char uf_spot_wb[] = N_("spot WB");
const char uf_manual_wb[] = N_("manual WB");
const char uf_camera_wb[] = N_("camera WB");
const char uf_auto_wb[] = N_("auto WB");

int wb_presets_size = 10000;
int wb_presets_count = 0;

#define _ERROR(...)     {\
                          dt_print(DT_DEBUG_CONTROL, "[wb_presets] error: " );\
                          dt_print(DT_DEBUG_CONTROL, __VA_ARGS__);\
                          dt_print(DT_DEBUG_CONTROL, "\n");\
                          valid = FALSE; \
                          goto end;\
                        }

dt_wb_data *wb_presets;

int dt_wb_presets_count(void)
{
  return wb_presets_count;
}

dt_wb_data *dt_wb_preset(const int k)
{
  return &wb_presets[k];
}

// extern void dt_wb_presets_w(void);

void dt_wb_presets_init(const char *alternative)
{
  wb_presets = calloc(sizeof(dt_wb_data), wb_presets_size);

  // dt_wb_presets_w();

  GError *error = NULL;
  char filename[PATH_MAX] = { 0 };

  if(alternative == NULL)
  {
    // TODO: shall we look for profiles in the user config dir?
    char datadir[PATH_MAX] = { 0 };
    dt_loc_get_datadir(datadir, sizeof(datadir));
    snprintf(filename, sizeof(filename), "%s/%s", datadir, "wb_presets.json");
  }
  else
    g_strlcpy(filename, alternative, sizeof(filename));

  dt_print(DT_DEBUG_CONTROL, "[wb_presets] loading wb_presets from `%s'\n", filename);
  if(!g_file_test(filename, G_FILE_TEST_EXISTS)) return;

  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_file(parser, filename, &error))
  {
    fprintf(stderr, "[wb_presets] error: parsing json from `%s' failed\n%s\n", filename, error->message);
    g_error_free(error);
    g_object_unref(parser);
    return;
  }

  // read file, store into wb_preset

  JsonReader *reader = NULL;
  gboolean valid = TRUE;

  dt_print(DT_DEBUG_CONTROL, "[wb_presets] loading noiseprofile file\n");

  JsonNode *root = json_parser_get_root(parser);
  if(!root) _ERROR("can't get the root node");

  reader = json_reader_new(root);

  if(!json_reader_read_member(reader, "version"))
    _ERROR("can't find file version.");

  // check the file version
  const int version = json_reader_get_int_value(reader);
  json_reader_end_member(reader);

  if(version > DT_WB_PRESETS_VERSION)
    _ERROR("file version is not what this code understands");

  if(!json_reader_read_member(reader, "wb_presets"))
    _ERROR("can't find `wb_presets' entry.");

  if(!json_reader_is_array(reader))
    _ERROR("`wb_presets' is supposed to be an array");

  const int n_makers = json_reader_count_elements(reader);
  dt_print(DT_DEBUG_CONTROL, "[wb_presets] found %d makers\n", n_makers);

  for(int i = 0; i < n_makers; i++)
  {
    if(!json_reader_read_element(reader, i))
      _ERROR("can't access maker at position %d / %d", i+1, n_makers);

    if(!json_reader_read_member(reader, "maker"))
      _ERROR("missing `maker`");

    const int current_make = wb_presets_count;

    wb_presets[wb_presets_count].make =
      g_strdup(json_reader_get_string_value(reader));
    json_reader_end_member(reader);

    dt_print(DT_DEBUG_CONTROL, "[wb_presets] found maker `%s'\n",
             wb_presets[wb_presets_count].make);
    // go through all models and check those

    if(!json_reader_read_member(reader, "models"))
      _ERROR("missing `models`");

    const int n_models = json_reader_count_elements(reader);
    dt_print(DT_DEBUG_CONTROL, "[wb_presets] found %d models\n", n_models);

    for(int j = 0; j < n_models; j++)
    {
      if(!json_reader_read_element(reader, j))
        _ERROR("can't access model at position %d / %d", j+1, n_models);

      if(!json_reader_read_member(reader, "model"))
        _ERROR("missing `model`");

      const int current_model = wb_presets_count;

      wb_presets[wb_presets_count].model =
        g_strdup(json_reader_get_string_value(reader));

      json_reader_end_member(reader);

      dt_print(DT_DEBUG_CONTROL, "[wb_presets] found %s\n",
               wb_presets[wb_presets_count].model);

      if(!json_reader_read_member(reader, "presets"))
        _ERROR("missing `presets`");

      const int n_presets = json_reader_count_elements(reader);
      dt_print(DT_DEBUG_CONTROL, "[wb_presets] found %d presets\n",
               n_presets);

      for(int k = 0; k < n_presets; k++)
      {
        // we point to the same make/model string, save some memory
        // this is ok as we never deallocate this struct.
        if(wb_presets[wb_presets_count].make == NULL)
          wb_presets[wb_presets_count].make = wb_presets[current_make].make;
        if(wb_presets[wb_presets_count].model == NULL)
          wb_presets[wb_presets_count].model = wb_presets[current_model].model;

        if(!json_reader_read_element(reader, k))
          _ERROR("can't access preset at position %d / %d", k+1, n_presets);

        // name
        json_reader_read_member(reader, "name");
        wb_presets[wb_presets_count].name =
          g_utf8_strdown(json_reader_get_string_value(reader), -1);
        json_reader_end_member(reader);

        // tuning
        json_reader_read_member(reader, "tuning");
        wb_presets[wb_presets_count].tuning =
          json_reader_get_int_value(reader);
        json_reader_end_member(reader);

        // channels
        json_reader_read_member(reader, "channels");

        for(int c = 0; c < 4; c++)
        {
          json_reader_read_element(reader, c);
          wb_presets[wb_presets_count].channels[c] =
            json_reader_get_double_value(reader);
          json_reader_end_element(reader);
        }
        json_reader_end_member(reader);

        wb_presets_count++;

        if(wb_presets_count ==  wb_presets_size)
        {
          // increment for 2000 presets
          wb_presets_size +=2000;
          wb_presets = realloc(wb_presets, sizeof(dt_wb_data) * wb_presets_size);
          memset((void *)&wb_presets[wb_presets_count], 0, sizeof(dt_wb_data) * 2000);
        }

        json_reader_end_element(reader);
      }

      json_reader_end_member(reader);  // presets
      json_reader_end_element(reader); // models
    }

    json_reader_end_member(reader);  // models
    json_reader_end_element(reader); // makers
  }

  dt_print(DT_DEBUG_CONTROL, "[wb_presets] found %d wb presets\n",
           wb_presets_count);

end:
  if(reader) g_object_unref(reader);
  if(!valid) exit(1);
  return;
}

/*
 * interpolate values from p1 and p2 into out.
 */
void dt_wb_preset_interpolate
(const dt_wb_data *const p1, // the smaller tuning
 const dt_wb_data *const p2, // the larger tuning (can't be == p1)
 dt_wb_data *out)            // has tuning initialized
{
  const double t = CLAMP((double)(out->tuning - p1->tuning) / (double)(p2->tuning - p1->tuning), 0.0, 1.0);
  for(int k = 0; k < 3; k++)
  {
    out->channels[k] = 1.0 / (((1.0 - t) / p1->channels[k]) + (t / p2->channels[k]));
  }
}

// vim: tabstop=8 shiftwidth=8 softtabstop=8
// kate: tab-width: 8; replace-tabs off; indent-width 8; tab-indents: on;
// kate: indent-mode c; remove-trailing-spaces modified;
