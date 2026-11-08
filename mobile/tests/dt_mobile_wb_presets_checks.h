/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_WB_PRESETS_CHECKS_H
#define DT_MOBILE_WB_PRESETS_CHECKS_H

#include "dt_mobile_whitebalance_checks.h"
#include "common/wb_presets.h"
#include "develop/develop.h"
#include <limits.h>

/* This fixture supplies camera metadata to the installed original module. It
 * verifies catalog/parameter behavior; it does not claim to decode a RAW file. */
static int mobile_wb_presets_catalog_checks(void)
{
  typedef char *(*metadata_function)(dt_iop_module_t *);
  typedef int (*preset_function)(dt_iop_module_t *, int, int);
  metadata_function metadata = NULL;
  preset_function set_preset = NULL;
  dt_iop_module_t *module = NULL;
  dt_develop_t *develop = NULL;
  void *snapshot = NULL;
  char *json = NULL;
  JsonParser *parser = json_parser_new();
  int success = 0;
  const uint32_t inactive = 0x7fc00145u;
#define WB_CATALOG_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "camera WB catalog:%d: check failed: %s\n", __LINE__, #condition); goto finished; } } while(0)
#define WB_CATALOG_METADATA() do { \
  g_clear_pointer(&json, g_free); json = metadata(module); \
  WB_CATALOG_CHECK(json && json_parser_load_from_data(parser, json, -1, NULL)); \
} while(0)
  dt_iop_module_so_t *plugin = dt_iop_get_module_so("temperature");
  WB_CATALOG_CHECK(plugin && plugin->module && plugin->get_introspection && plugin->get_p);
  const dt_introspection_t *schema = plugin->get_introspection();
  WB_CATALOG_CHECK(schema && schema->size > 0);
  WB_CATALOG_CHECK(g_module_symbol(plugin->module, "dt_iop_mobile_temperature_presets", (gpointer *)&metadata));
  WB_CATALOG_CHECK(g_module_symbol(plugin->module, "dt_iop_mobile_temperature_set_preset", (gpointer *)&set_preset));
  module = g_new0(dt_iop_module_t, 1);
  develop = g_new0(dt_develop_t, 1);
  module->dev = develop;
  module->params_size = schema->size;
  module->params = g_malloc0(schema->size);
  g_strlcpy(module->op, "temperature", sizeof(module->op));
  develop->image_storage.flags = DT_IMAGE_RAW;
  g_strlcpy(develop->image_storage.filename, "catalog-fixture.dng", sizeof(develop->image_storage.filename));
  g_strlcpy(develop->image_storage.camera_maker, "Canon", sizeof(develop->image_storage.camera_maker));
  g_strlcpy(develop->image_storage.camera_model, "PowerShot SX50 HS", sizeof(develop->image_storage.camera_model));
  float *red = plugin->get_p(module->params, "red");
  float *green = plugin->get_p(module->params, "green");
  float *blue = plugin->get_p(module->params, "blue");
  float *fourth = plugin->get_p(module->params, "various");
  int *preset = plugin->get_p(module->params, "preset");
  WB_CATALOG_CHECK(red && green && blue && fourth && preset && sizeof(*fourth) == sizeof(inactive));
  *red = *green = *blue = 1.0f;
  memcpy(fourth, &inactive, sizeof(inactive));
  *preset = 0;
  snapshot = g_malloc(schema->size);
  memcpy(snapshot, module->params, schema->size);
  WB_CATALOG_METADATA();
  WB_CATALOG_CHECK(!memcmp(snapshot, module->params, schema->size));
  JsonObject *catalog = json_node_get_object(json_parser_get_root(parser));
  WB_CATALOG_CHECK(!strcmp(json_object_get_string_member(catalog, "camera"), "Canon PowerShot SX50 HS"));
  WB_CATALOG_CHECK(json_object_get_null_member(catalog, "selectedPreset")
    && json_object_get_null_member(catalog, "selectedTuning"));
  JsonArray *groups = json_object_get_array_member(catalog, "presets");
  WB_CATALOG_CHECK(json_array_get_length(groups) > 0 && json_array_get_length(groups) <= 50);
  JsonObject *first = json_array_get_object_element(groups, 0);
  WB_CATALOG_CHECK(json_object_get_int_member(first, "id") == 6
    && json_object_get_int_member(first, "defaultTuning") == 0);
  JsonArray *tuning = json_object_get_array_member(first, "tuning");
  WB_CATALOG_CHECK(json_array_get_length(tuning) == 19
    && json_array_get_int_element(tuning, 0) == -9
    && json_array_get_int_element(tuning, 18) == 9);

  int exact = -1;
  for(int i = 0; i < dt_wb_presets_count(); i++)
  {
    const dt_wb_data *wb = dt_wb_preset(i);
    if(!strcmp(wb->make, "Canon") && !strcmp(wb->model, "PowerShot SX50 HS")
       && !strcmp(wb->name, "daylight") && wb->tuning == 0) { exact = i; break; }
  }
  WB_CATALOG_CHECK(exact >= 1 && exact + 1 < dt_wb_presets_count());
  WB_CATALOG_CHECK(set_preset(module, 6, 0) == 1 && *preset == 6);
  const dt_wb_data *original = dt_wb_preset(exact);
  WB_CATALOG_CHECK(*red == (float)original->channels[0] && *green == (float)original->channels[1]
    && *blue == (float)original->channels[2] && !memcmp(fourth, &inactive, sizeof(inactive)));
  WB_CATALOG_METADATA();
  catalog = json_node_get_object(json_parser_get_root(parser));
  WB_CATALOG_CHECK(json_object_get_int_member(catalog, "selectedPreset") == 6
    && json_object_get_int_member(catalog, "selectedTuning") == 0);
  dt_wb_data interpolated = { .tuning = 1 };
  dt_wb_preset_interpolate(original, dt_wb_preset(exact + 1), &interpolated);
  WB_CATALOG_CHECK(set_preset(module, 6, 1) == 1 && *preset == 6);
  WB_CATALOG_CHECK(*red == (float)interpolated.channels[0] && *green == (float)interpolated.channels[1]
    && *blue == (float)interpolated.channels[2] && !memcmp(fourth, &inactive, sizeof(inactive)));
  WB_CATALOG_METADATA();
  catalog = json_node_get_object(json_parser_get_root(parser));
  WB_CATALOG_CHECK(json_object_get_int_member(catalog, "selectedPreset") == 6
    && json_object_get_int_member(catalog, "selectedTuning") == 1);
  for(int setting = 0; setting < 5; setting++)
  {
    *preset = setting;
    WB_CATALOG_METADATA();
    catalog = json_node_get_object(json_parser_get_root(parser));
    WB_CATALOG_CHECK(json_object_get_null_member(catalog, "selectedPreset")
      && json_object_get_null_member(catalog, "selectedTuning"));
  }
  *preset = 6; *red += 0.125f;
  WB_CATALOG_METADATA();
  catalog = json_node_get_object(json_parser_get_root(parser));
  WB_CATALOG_CHECK(json_object_get_null_member(catalog, "selectedPreset")
    && json_object_get_null_member(catalog, "selectedTuning"));
  memcpy(snapshot, module->params, schema->size);
  const int invalid[][2] = { { INT_MIN, 0 }, { -1, 0 }, { 0, 0 }, { 5, 0 }, { INT_MAX, 0 },
    { 6, -10 }, { 6, 10 }, { 6, INT_MIN }, { 6, INT_MAX } };
  for(size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    WB_CATALOG_CHECK(set_preset(module, invalid[i][0], invalid[i][1]) == -1
      && !memcmp(snapshot, module->params, schema->size));

  /* No fourth channel is present in this camera's catalog. In particular the
   * RGB-only interpolation must never advertise or write a fourth-channel gap. */
  develop->image_storage.flags |= DT_IMAGE_4BAYER;
  g_clear_pointer(&json, g_free); json = metadata(module);
  WB_CATALOG_CHECK(!json && set_preset(module, 6, 1) == -1
    && !memcmp(snapshot, module->params, schema->size));
  g_strlcpy(develop->image_storage.camera_model, "PowerShot SX160 IS", sizeof(develop->image_storage.camera_model));
  WB_CATALOG_METADATA();
  catalog = json_node_get_object(json_parser_get_root(parser));
  groups = json_object_get_array_member(catalog, "presets");
  first = json_array_get_object_element(groups, 0);
  tuning = json_object_get_array_member(first, "tuning");
  WB_CATALOG_CHECK(json_object_get_int_member(first, "id") == 6
    && json_array_get_length(tuning) == 1 && json_array_get_int_element(tuning, 0) == 0);
  WB_CATALOG_CHECK(set_preset(module, 6, 0) == 1 && *fourth == 1.0f);
  memcpy(snapshot, module->params, schema->size);
  WB_CATALOG_CHECK(set_preset(module, 6, 1) == -1 && !memcmp(snapshot, module->params, schema->size));
  develop->image_storage.flags = DT_IMAGE_RAW | DT_IMAGE_MONOCHROME;
  g_clear_pointer(&json, g_free); json = metadata(module);
  WB_CATALOG_CHECK(!json && set_preset(module, 6, 0) == -1
    && !memcmp(snapshot, module->params, schema->size));
  develop->image_storage.flags = DT_IMAGE_LDR;
  g_clear_pointer(&json, g_free); json = metadata(module);
  WB_CATALOG_CHECK(!json && set_preset(module, 6, 0) == -1
    && !memcmp(snapshot, module->params, schema->size));
  develop->image_storage.flags = DT_IMAGE_RAW;
  g_strlcpy(develop->image_storage.camera_model, "missing catalog camera", sizeof(develop->image_storage.camera_model));
  g_clear_pointer(&json, g_free); json = metadata(module);
  WB_CATALOG_CHECK(!json && set_preset(module, 6, 0) == -1
    && !memcmp(snapshot, module->params, schema->size));

  /* A legitimate camera preset exceeds the manual gain editor's maximum. */
  g_strlcpy(develop->image_storage.camera_maker, "Olympus", sizeof(develop->image_storage.camera_maker));
  g_strlcpy(develop->image_storage.camera_model, "C5050Z", sizeof(develop->image_storage.camera_model));
  int high_id = -1, group_id = 5;
  const char *name = NULL;
  for(int i = 0; i < dt_wb_presets_count(); i++)
  {
    const dt_wb_data *wb = dt_wb_preset(i);
    if(strcmp(wb->make, "Olympus") || strcmp(wb->model, "C5050Z")) continue;
    if(!name || strcmp(name, wb->name)) { group_id++; name = wb->name; }
    if(!strcmp(wb->name, "white fluorescent") && wb->tuning == 7) { high_id = group_id; break; }
  }
  WB_CATALOG_CHECK(high_id >= 6 && set_preset(module, high_id, 7) == 1 && *blue > 8.0f);
  WB_CATALOG_METADATA();
  catalog = json_node_get_object(json_parser_get_root(parser));
  WB_CATALOG_CHECK(json_object_get_int_member(catalog, "selectedPreset") == high_id
    && json_object_get_int_member(catalog, "selectedTuning") == 7);
  success = 1;
finished:
  g_free(json); g_free(snapshot);
  if(module) { g_free(module->params); g_free(module); }
  g_free(develop); g_object_unref(parser);
#undef WB_CATALOG_CHECK
#undef WB_CATALOG_METADATA
  return success;
}

/* The ordinary host fixture is RGB PNG. Catalog writes must fail without a
 * history entry, metadata change or changed pixel, even for known preset IDs. */
static int mobile_wb_presets_checks(dt_mobile_session *session)
{
  char *before = NULL, *after = NULL, *history = NULL, *after_history = NULL;
  uint8_t *pixels = NULL, *restored = NULL;
  int w = 0, h = 0, rw = 0, rh = 0, success = 0;
#define WB_PRESETS_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "camera WB:%d: check failed: %s\n", __LINE__, #condition); goto finished; } } while(0)
  WB_PRESETS_CHECK(mobile_wb_presets_catalog_checks());
  WB_PRESETS_CHECK(dt_mobile_modules(session, &before) == DT_MOBILE_OK && !strstr(before, "\"cameraPresets\""));
  WB_PRESETS_CHECK(dt_mobile_history(session, &history) == DT_MOBILE_OK);
  WB_PRESETS_CHECK(dt_mobile_render_preview(session, 96, 96, &pixels, &w, &h) == DT_MOBILE_OK);
  WB_PRESETS_CHECK(dt_mobile_set_whitebalance_preset(session, "temperature", 6, 0) == DT_MOBILE_ERROR_UNSUPPORTED);
  WB_PRESETS_CHECK(dt_mobile_set_whitebalance_preset(session, "temperature", 6, 1) == DT_MOBILE_ERROR_UNSUPPORTED);
  WB_PRESETS_CHECK(dt_mobile_set_whitebalance_preset(session, "exposure", 6, 0) == DT_MOBILE_ERROR_UNSUPPORTED);
  WB_PRESETS_CHECK(dt_mobile_modules(session, &after) == DT_MOBILE_OK && !strcmp(before, after));
  WB_PRESETS_CHECK(dt_mobile_history(session, &after_history) == DT_MOBILE_OK && !strcmp(history, after_history));
  WB_PRESETS_CHECK(dt_mobile_render_preview(session, 96, 96, &restored, &rw, &rh) == DT_MOBILE_OK
    && rw == w && rh == h && !memcmp(pixels, restored, (size_t)w * h * 4));
  success = 1;
finished:
  dt_mobile_free_buffer(before); dt_mobile_free_buffer(after);
  dt_mobile_free_buffer(history); dt_mobile_free_buffer(after_history);
  dt_mobile_free_buffer(pixels); dt_mobile_free_buffer(restored);
#undef WB_PRESETS_CHECK
  return success;
}
#endif
