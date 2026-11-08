/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_WHITEBALANCE_CHECKS_H
#define DT_MOBILE_WHITEBALANCE_CHECKS_H

#include "dt_mobile.h"
#include "common/darktable.h"
#include "develop/imageop.h"
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct wb_test_metadata
{
  size_t count;
  float gains[4], minimum, maximum;
  int setting;
  unsigned settings;
  int has_temperature;
  double kelvin, tint;
} wb_test_metadata;

static int wb_test_read_metadata(const char *json, wb_test_metadata *state)
{
  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  int valid = 0;
  memset(state, 0, sizeof(*state));
  if(!json_parser_load_from_data(parser, json, -1, &error)) goto finished;
  JsonNode *root = json_parser_get_root(parser);
  if(!JSON_NODE_HOLDS_ARRAY(root)) goto finished;
  JsonArray *modules = json_node_get_array(root);
  for(guint i = 0; i < json_array_get_length(modules); i++)
  {
    JsonObject *module = json_array_get_object_element(modules, i);
    if(strcmp(json_object_get_string_member(module, "operation"), "temperature")) continue;
    if(!json_object_has_member(module, "whiteBalance")) break;
    JsonObject *wb = json_object_get_object_member(module, "whiteBalance");
    JsonArray *channels = json_object_get_array_member(wb, "channels");
    state->count = json_array_get_length(channels);
    if(state->count != 3 && state->count != 4) break;
    unsigned indices = 0;
    for(guint channel = 0; channel < state->count; channel++)
    {
      JsonObject *item = json_array_get_object_element(channels, channel);
      const gint64 index = json_object_get_int_member(item, "index");
      const float gain = (float)json_object_get_double_member(item, "value");
      if(index < 0 || (guint64)index >= state->count || (indices & (1u << index))
         || !isnormal(gain) || gain <= 0.0f
         || !json_object_get_string_member(item, "label")[0]) goto finished;
      indices |= 1u << index;
      state->gains[index] = gain;
    }
    state->minimum = (float)json_object_get_double_member(wb, "manualMin");
    state->maximum = (float)json_object_get_double_member(wb, "manualMax");
    state->setting = (int)json_object_get_int_member(wb, "setting");
    if(state->minimum != 0.001f || state->maximum != 8.0f
       || !json_object_get_string_member(wb, "settingLabel")[0]) break;
    JsonArray *settings = json_object_get_array_member(wb, "settings");
    for(guint setting = 0; setting < json_array_get_length(settings); setting++)
    {
      JsonObject *item = json_array_get_object_element(settings, setting);
      const gint64 id = json_object_get_int_member(item, "id");
      if((id != 0 && id != 3 && id != 4) || (state->settings & (1u << id))
         || !json_object_get_string_member(item, "label")[0]) goto finished;
      state->settings |= 1u << id;
    }
    if(json_object_has_member(wb, "temperature"))
    {
      JsonObject *temperature = json_object_get_object_member(wb, "temperature");
      state->kelvin = json_object_get_double_member(temperature, "kelvin");
      state->tint = json_object_get_double_member(temperature, "tint");
      if(!isfinite(state->kelvin) || state->kelvin < 1901.0 || state->kelvin > 25000.0
         || !isfinite(state->tint) || state->tint < 0.135 || state->tint > 2.326
         || json_object_get_double_member(temperature, "minKelvin") != 1901.0
         || json_object_get_double_member(temperature, "maxKelvin") != 25000.0
         || json_object_get_double_member(temperature, "minTint") != 0.135
         || json_object_get_double_member(temperature, "maxTint") != 2.326) goto finished;
      state->has_temperature = 1;
    }
    valid = indices == (1u << state->count) - 1u;
    break;
  }
finished:
  if(error)
  {
    fprintf(stderr, "white-balance metadata: %s\n", error->message);
    g_error_free(error);
  }
  g_object_unref(parser);
  return valid;
}

/* The engine fixture owns one active image session. Obtain its current native
 * history bytes and resolve fields from the original module introspection;
 * there is no copied temperature parameter struct or guessed byte offset. */
static void *wb_test_native_blob(size_t *size)
{
  sqlite3_stmt *statement = NULL;
  void *copy = NULL;
  *size = 0;
  if(sqlite3_prepare_v2(dt_database_get(darktable.db),
      "SELECT h.op_params FROM main.history h JOIN main.images i ON i.id=h.imgid "
      "WHERE h.operation='temperature' AND h.multi_priority=0 AND h.num<i.history_end "
      "ORDER BY h.imgid DESC,h.num DESC LIMIT 1", -1, &statement, NULL) != SQLITE_OK) return NULL;
  if(sqlite3_step(statement) == SQLITE_ROW)
  {
    const int bytes = sqlite3_column_bytes(statement, 0);
    const void *blob = sqlite3_column_blob(statement, 0);
    if(bytes > 0 && blob)
    {
      *size = (size_t)bytes;
      copy = g_malloc(*size);
      memcpy(copy, blob, *size);
    }
  }
  sqlite3_finalize(statement);
  return copy;
}

static const dt_introspection_field_t *wb_test_field(const char *name, size_t size,
                                                    dt_introspection_type_t type, size_t field_size)
{
  dt_iop_module_so_t *module = dt_iop_get_module_so("temperature");
  if(!module || !module->have_introspection || !module->get_f) return NULL;
  const dt_introspection_field_t *field = module->get_f(name);
  return field && field->header.type == type && field->header.size == field_size
    && field->header.offset <= size && field_size <= size - field->header.offset ? field : NULL;
}

static int wb_test_native_preset(int expected)
{
  size_t size;
  void *blob = wb_test_native_blob(&size);
  const dt_introspection_field_t *field = wb_test_field("preset", size, DT_INTROSPECTION_TYPE_INT, sizeof(int));
  int preset = -99;
  if(blob && field) memcpy(&preset, (const char *)blob + field->header.offset, sizeof(preset));
  g_free(blob);
  return preset == expected;
}

static int wb_test_gains_equal(const wb_test_metadata *first, const wb_test_metadata *second)
{
  return first->count == second->count
    && !memcmp(first->gains, second->gains, first->count * sizeof(float));
}

/* Small preview-only round trip, suitable for a separately opened real RAW/GPR
 * session too. It restores the entire original history state with one undo. */
static int wb_test_preview_roundtrip(dt_mobile_session *session, int temperature)
{
  int success = 0, applied = 0;
  char *before = NULL, *after = NULL;
  uint8_t *reference = NULL, *changed = NULL, *restored = NULL;
  int w = 0, h = 0, cw = 0, ch = 0, rw = 0, rh = 0;
  wb_test_metadata state;
  const float gains[4] = { 1.6f, 0.8f, 1.2f, 1.1f };
#define WB_PREVIEW_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "white-balance preview:%d: check failed: %s\n", __LINE__, #condition); goto finished; } } while(0)
#define WB_PREVIEW_OK(call) do { const dt_mobile_status status = (call); if(status != DT_MOBILE_OK) { \
  fprintf(stderr, "white-balance preview: %s: %s\n", #call, dt_mobile_last_error(session)); goto finished; } } while(0)
  WB_PREVIEW_OK(dt_mobile_modules(session, &before));
  WB_PREVIEW_CHECK(wb_test_read_metadata(before, &state));
  WB_PREVIEW_OK(dt_mobile_render_preview(session, 96, 96, &reference, &w, &h));
  if(temperature)
  {
    WB_PREVIEW_CHECK(state.has_temperature);
    WB_PREVIEW_OK(dt_mobile_set_whitebalance_temperature(session, "temperature", 6500.0, 1.2));
  }
  else WB_PREVIEW_OK(dt_mobile_set_whitebalance_gains(session, "temperature", gains, state.count));
  applied = 1;
  WB_PREVIEW_OK(dt_mobile_render_preview(session, 96, 96, &changed, &cw, &ch));
  WB_PREVIEW_CHECK(reference && changed && w == cw && h == ch);
  WB_PREVIEW_CHECK(memcmp(reference, changed, (size_t)w * h * 4));
  WB_PREVIEW_OK(dt_mobile_undo(session));
  applied = 0;
  WB_PREVIEW_OK(dt_mobile_render_preview(session, 96, 96, &restored, &rw, &rh));
  WB_PREVIEW_CHECK(restored && w == rw && h == rh && !memcmp(reference, restored, (size_t)w * h * 4));
  WB_PREVIEW_OK(dt_mobile_modules(session, &after));
  WB_PREVIEW_CHECK(!strcmp(before, after));
  success = 1;
finished:
  if(applied) (void)dt_mobile_undo(session);
  dt_mobile_free_buffer(before); dt_mobile_free_buffer(after);
  dt_mobile_free_buffer(reference); dt_mobile_free_buffer(changed); dt_mobile_free_buffer(restored);
#undef WB_PREVIEW_CHECK
#undef WB_PREVIEW_OK
  return success;
}

static inline int wb_raw_preview_checks(dt_mobile_session *session)
{
  return wb_test_preview_roundtrip(session, 0) && wb_test_preview_roundtrip(session, 1);
}

/* PNG integration coverage. All successful changes, including the optional
 * inactive-channel fixture, are undone before checking original state/pixels. */
static int wb_checks(dt_mobile_session *session)
{
  int success = 0;
  unsigned applied = 0;
  char *original = NULL, *json = NULL, *before_invalid = NULL, *before_history = NULL;
  char *after_history = NULL, *camera_reference = NULL;
  uint8_t *reference = NULL, *restored = NULL;
  int width = 0, height = 0, restored_width = 0, restored_height = 0;
  void *blob = NULL;
  size_t blob_size = 0;
  wb_test_metadata original_state, state, as_shot;
  float gains[4] = { 1.2f, 0.9f, 1.4f, 1.0f };
  uint32_t inactive_bits = 0;
#define WB_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "white-balance:%d: check failed: %s\n", __LINE__, #condition); goto finished; } } while(0)
#define WB_OK(call) do { const dt_mobile_status status = (call); if(status != DT_MOBILE_OK) { \
  fprintf(stderr, "white-balance: %s: %s\n", #call, dt_mobile_last_error(session)); goto finished; } } while(0)
#define WB_APPLY(call) do { WB_OK(call); applied++; } while(0)
#define WB_METADATA(target) do { dt_mobile_free_buffer(json); json = NULL; \
  WB_OK(dt_mobile_modules(session, &json)); WB_CHECK(wb_test_read_metadata(json, &(target))); } while(0)
  WB_OK(dt_mobile_modules(session, &original));
  WB_CHECK(wb_test_read_metadata(original, &original_state));
  WB_CHECK(original_state.count == 3);
  WB_CHECK(original_state.settings == ((1u << 0) | (1u << 3) | (1u << 4)));
  WB_OK(dt_mobile_render_preview(session, 96, 96, &reference, &width, &height));
  WB_CHECK(wb_test_preview_roundtrip(session, 0));
  WB_CHECK(wb_test_preview_roundtrip(session, 1));

  WB_APPLY(dt_mobile_set_whitebalance_gains(session, "temperature", gains, 3));
  WB_METADATA(state);
  WB_CHECK(state.setting == 2 && wb_test_native_preset(2));
  WB_CHECK(!memcmp(state.gains, gains, 3 * sizeof(float)));

  blob = wb_test_native_blob(&blob_size);
  WB_CHECK(blob && sizeof(float) == sizeof(inactive_bits));
  const dt_introspection_field_t *various = wb_test_field("various", blob_size, DT_INTROSPECTION_TYPE_FLOAT, sizeof(float));
  WB_CHECK(various != NULL);
  float fourth;
  memcpy(&fourth, (const char *)blob + various->header.offset, sizeof(fourth));
  if(!isnan(fourth))
  {
    /* A NaN is a supported sentinel for a three-channel input. Keep all active
     * coefficients and the preset valid; no malformed node/count reaches a pipe. */
    fourth = nanf("0x145");
    memcpy((char *)blob + various->header.offset, &fourth, sizeof(fourth));
    WB_APPLY(dt_mobile_set_module_params(session, "temperature", blob, blob_size));
  }
  memcpy(&inactive_bits, &fourth, sizeof(inactive_bits));
  g_free(blob); blob = NULL;
  gains[0] = 1.5f;
  WB_APPLY(dt_mobile_set_whitebalance_gains(session, "temperature", gains, 3));
  blob = wb_test_native_blob(&blob_size);
  various = wb_test_field("various", blob_size, DT_INTROSPECTION_TYPE_FLOAT, sizeof(float));
  WB_CHECK(blob && various);
  uint32_t after_bits;
  memcpy(&after_bits, (const char *)blob + various->header.offset, sizeof(after_bits));
  WB_CHECK(after_bits == inactive_bits);
  g_free(blob); blob = NULL;
  WB_METADATA(state);
  WB_CHECK(state.count == 3 && state.setting == 2 && wb_test_native_preset(2));

  WB_APPLY(dt_mobile_set_whitebalance_temperature(session, "temperature", 6500.0, 1.2));
  WB_METADATA(state);
  WB_CHECK(state.has_temperature && state.setting == 2 && wb_test_native_preset(2));
  /* Original desktop spectral/matrix result, normalized by green. */
  WB_CHECK(fabsf(state.gains[0] - 0.5471105667492383f) < 1e-6f);
  WB_CHECK(state.gains[1] == 1.0f);
  WB_CHECK(fabsf(state.gains[2] - 0.66505783771296367f) < 1e-6f);
  WB_CHECK(fabs(state.kelvin - 6500.0) < 1.0 && fabs(state.tint - 1.2) < 0.001);
  blob = wb_test_native_blob(&blob_size);
  various = wb_test_field("various", blob_size, DT_INTROSPECTION_TYPE_FLOAT, sizeof(float));
  WB_CHECK(blob && various);
  memcpy(&after_bits, (const char *)blob + various->header.offset, sizeof(after_bits));
  WB_CHECK(after_bits == inactive_bits);
  g_free(blob); blob = NULL;

  /* Kelvin-derived coefficients use the desktop result, not the manual
   * editor's [0.001,8] limits. The original 2200 K sRGB blue gain exceeds 8. */
  WB_APPLY(dt_mobile_set_whitebalance_temperature(session, "temperature", 2200.0, 1.0));
  WB_METADATA(state);
  WB_CHECK(state.setting == 2 && state.gains[2] > state.maximum);
  WB_CHECK(fabsf(state.gains[2] - 10.773649092693217f) < 1e-5f);

  gains[0] = state.minimum;
  WB_APPLY(dt_mobile_set_whitebalance_gains(session, "temperature", gains, 3));
  WB_METADATA(state);
  WB_CHECK(state.gains[0] == state.minimum);
  gains[0] = state.maximum;
  WB_APPLY(dt_mobile_set_whitebalance_gains(session, "temperature", gains, 3));
  WB_METADATA(state);
  WB_CHECK(state.gains[0] == state.maximum && wb_test_native_preset(2));

  WB_OK(dt_mobile_modules(session, &before_invalid));
  WB_OK(dt_mobile_history(session, &before_history));
  const float invalid[] = { NAN, INFINITY, -INFINITY, 0.0f, -1.0f,
    nextafterf(0.001f, 0.0f), nextafterf(8.0f, INFINITY) };
  for(size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
  {
    gains[0] = invalid[i];
    WB_CHECK(dt_mobile_set_whitebalance_gains(session, "temperature", gains, 3) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  }
  gains[0] = 1.0f;
  const size_t invalid_counts[] = { 0, 1, 2, 4, 5, SIZE_MAX };
  for(size_t i = 0; i < sizeof(invalid_counts) / sizeof(invalid_counts[0]); i++)
    WB_CHECK(dt_mobile_set_whitebalance_gains(session, "temperature", gains, invalid_counts[i]) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  WB_CHECK(dt_mobile_set_whitebalance_gains(session, "temperature", NULL, 3) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  WB_CHECK(dt_mobile_set_whitebalance_gains(session, "exposure", gains, 3) == DT_MOBILE_ERROR_UNSUPPORTED);
  WB_CHECK(dt_mobile_set_whitebalance_setting(session, "temperature", 1) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  WB_CHECK(dt_mobile_set_whitebalance_setting(session, "temperature", 2) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  WB_CHECK(dt_mobile_set_whitebalance_setting(session, "temperature", -1) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  WB_CHECK(dt_mobile_set_whitebalance_setting(session, "temperature", 5) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  WB_CHECK(dt_mobile_set_whitebalance_setting(session, "exposure", 0) == DT_MOBILE_ERROR_UNSUPPORTED);
  WB_CHECK(dt_mobile_set_whitebalance_temperature(session, "temperature", 1900.0, 1.0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  WB_CHECK(dt_mobile_set_whitebalance_temperature(session, "temperature", 6500.0, 0.134) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  WB_CHECK(dt_mobile_set_whitebalance_temperature(session, "exposure", 6500.0, 1.0) == DT_MOBILE_ERROR_UNSUPPORTED);
  /* Valid UI bounds can still leave the camera gamut: reject before any write. */
  WB_CHECK(dt_mobile_set_whitebalance_temperature(session, "temperature", 6500.0, 0.135) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  WB_METADATA(state);
  WB_OK(dt_mobile_history(session, &after_history));
  WB_CHECK(!strcmp(before_invalid, json) && !strcmp(before_history, after_history));

  WB_APPLY(dt_mobile_set_whitebalance_setting(session, "temperature", 0));
  WB_METADATA(as_shot);
  WB_CHECK(as_shot.setting == 0 && wb_test_native_preset(0));
  WB_APPLY(dt_mobile_set_whitebalance_setting(session, "temperature", 3));
  WB_METADATA(state);
  WB_CHECK(state.setting == 3 && wb_test_native_preset(3));
  WB_OK(dt_mobile_modules(session, &camera_reference));
  WB_APPLY(dt_mobile_set_whitebalance_setting(session, "temperature", 4));
  WB_METADATA(state);
  WB_CHECK(state.setting == 4 && wb_test_native_preset(4) && wb_test_gains_equal(&as_shot, &state));
  WB_OK(dt_mobile_undo(session)); applied--;
  WB_METADATA(state);
  WB_CHECK(state.setting == 3 && wb_test_native_preset(3) && !strcmp(camera_reference, json));
  WB_OK(dt_mobile_redo(session)); applied++;
  WB_METADATA(state);
  WB_CHECK(state.setting == 4 && wb_test_native_preset(4) && wb_test_gains_equal(&as_shot, &state));

  while(applied) { WB_OK(dt_mobile_undo(session)); applied--; }
  WB_METADATA(state);
  WB_CHECK(!strcmp(original, json));
  WB_OK(dt_mobile_render_preview(session, 96, 96, &restored, &restored_width, &restored_height));
  WB_CHECK(restored && width == restored_width && height == restored_height
           && !memcmp(reference, restored, (size_t)width * height * 4));
  success = 1;
finished:
  while(applied)
  {
    if(dt_mobile_undo(session) != DT_MOBILE_OK) break;
    applied--;
  }
  dt_mobile_free_buffer(original); dt_mobile_free_buffer(json); dt_mobile_free_buffer(before_invalid);
  dt_mobile_free_buffer(before_history); dt_mobile_free_buffer(after_history); dt_mobile_free_buffer(camera_reference);
  dt_mobile_free_buffer(reference); dt_mobile_free_buffer(restored);
  g_free(blob);
#undef WB_CHECK
#undef WB_OK
#undef WB_APPLY
#undef WB_METADATA
  return success;
}
#endif
