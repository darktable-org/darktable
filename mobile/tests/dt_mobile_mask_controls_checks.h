/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_MASK_CONTROLS_CHECKS_H
#define DT_MOBILE_MASK_CONTROLS_CHECKS_H

#include <json-glib/json-glib.h>

static int32_t mobile_test_mask_id(const char *json, const char *operation)
{
  JsonParser *parser = json_parser_new();
  int32_t result = 0;
  if(!json_parser_load_from_data(parser, json, -1, NULL)) goto done;
  JsonArray *modules = json_node_get_array(json_parser_get_root(parser));
  for(guint index = 0; index < json_array_get_length(modules); index++)
  {
    JsonObject *module = json_array_get_object_element(modules, index);
    if(strcmp(json_object_get_string_member(module, "operation"), operation)) continue;
    JsonArray *parameters = json_object_get_array_member(module, "parameters");
    for(guint field = 0; field < json_array_get_length(parameters); field++)
    {
      const char *name = json_object_get_string_member(json_array_get_object_element(parameters, field), "name");
      if(strncmp(name, "mask.", 5)) continue;
      char *end;
      const long id = strtol(name + 5, &end, 10);
      if(end > name + 5 && *end == '.' && id > 0 && id <= INT32_MAX) { result = (int32_t)id; goto done; }
    }
  }
done:
  g_object_unref(parser);
  return result;
}

static int mobile_mask_controls_checks(dt_mobile_session *session)
{
  char *initial = NULL, *baseline = NULL, *created = NULL, *metadata = NULL, *history = NULL, *after_history = NULL;
  uint8_t *uniform = NULL, *masked = NULL, *moved = NULL, *restored = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0, success = 0;
  char x[96], radius[96], remove[96];
  ENGINE_OK(dt_mobile_modules(session, &initial));
  CHECK(module_has(initial, "exposure", "\"name\":\"mask.add.circle\""));
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 1));
  ENGINE_OK(dt_mobile_modules(session, &baseline));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &uniform, &width, &height));
  dt_mobile_free_buffer(uniform); uniform = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &uniform, &width, &height));
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "mask.add.circle", 1));
  ENGINE_OK(dt_mobile_modules(session, &created));
  const int32_t id = mobile_test_mask_id(created, "exposure");
  CHECK(id > 0);
  snprintf(x, sizeof(x), "mask.%d.x", id);
  snprintf(radius, sizeof(radius), "mask.%d.radius", id);
  snprintf(remove, sizeof(remove), "mask.%d.delete", id);
  CHECK(parameter_equals(created, "exposure", x, .5));
  CHECK(parameter_equals(created, "exposure", radius, (double).1f));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &masked, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(uniform, masked, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_history(session, &history));
  CHECK(dt_mobile_set_parameter(session, "exposure", radius, 0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_parameter(session, "exposure", x, 1.1) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_parameter(session, "exposure", x, NAN) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_parameter(session, "exposure", "mask.add.circle", 0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_parameter(session, "exposure", "mask.2147483647.x", .5) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(created, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", x, .25));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &moved, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(masked, moved, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(created, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(masked, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_redo(session));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(moved, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", remove, 1));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(mobile_test_mask_id(metadata, "exposure") == 0);
  dt_mobile_free_buffer(metadata); metadata = NULL;
  CHECK(dt_mobile_set_parameter(session, "exposure", x, .5) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(created, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(masked, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(initial, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;

  /* The other basic shapes use the same original native form/history path. */
  const char *actions[] = { "mask.add.ellipse", "mask.add.gradient" };
  for(size_t index = 0; index < sizeof(actions) / sizeof(actions[0]); index++)
  {
    ENGINE_OK(dt_mobile_set_parameter(session, "exposure", actions[index], 1));
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    CHECK(mobile_test_mask_id(metadata, "exposure") > 0);
    dt_mobile_free_buffer(metadata); metadata = NULL;
    ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
    CHECK(restored && other_width > 0 && other_height > 0);
    dt_mobile_free_buffer(restored); restored = NULL;
    ENGINE_OK(dt_mobile_undo(session));
  }

  /* A repair form is also registered in the original module's private array;
   * a mask alone would be an inert or unsafe imitation of the repair tool. */
  const char *tools[] = { "spots", "retouch" };
  const char *repair_actions[] = { "mask.add.circle", "mask.add.blur" };
  for(size_t index = 0; index < 2; index++)
  {
    ENGINE_OK(dt_mobile_set_parameter(session, tools[index], repair_actions[index], 1));
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    const int32_t repair_id = mobile_test_mask_id(metadata, tools[index]);
    CHECK(repair_id > 0);
    if(index == 1)
    {
      char algorithm[96]; snprintf(algorithm, sizeof(algorithm), "mask.%d.algorithm", repair_id);
      CHECK(parameter_equals(metadata, tools[index], algorithm, 3));
    }
    dt_mobile_free_buffer(metadata); metadata = NULL;
    ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
    CHECK(restored && other_width > 0 && other_height > 0);
    dt_mobile_free_buffer(restored); restored = NULL;
    ENGINE_OK(dt_mobile_set_parameter(session, tools[index], "module.reset", 1));
    ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
    CHECK(restored && other_width > 0 && other_height > 0);
    dt_mobile_free_buffer(restored); restored = NULL;
    ENGINE_OK(dt_mobile_undo(session));
    ENGINE_OK(dt_mobile_undo(session));
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    CHECK(!strcmp(initial, metadata));
    dt_mobile_free_buffer(metadata); metadata = NULL;
  }
  success = 1;
failed:
  dt_mobile_free_buffer(initial);
  dt_mobile_free_buffer(baseline);
  dt_mobile_free_buffer(created);
  dt_mobile_free_buffer(metadata);
  dt_mobile_free_buffer(history);
  dt_mobile_free_buffer(after_history);
  dt_mobile_free_buffer(uniform);
  dt_mobile_free_buffer(masked);
  dt_mobile_free_buffer(moved);
  dt_mobile_free_buffer(restored);
  return success;
}

#endif
