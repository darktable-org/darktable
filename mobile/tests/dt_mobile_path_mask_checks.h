/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_PATH_MASK_CHECKS_H
#define DT_MOBILE_PATH_MASK_CHECKS_H

static int32_t mobile_test_mask_shape_id(const char *json, const char *operation, const char *shape)
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
      JsonObject *parameter = json_array_get_object_element(parameters, field);
      const char *name = json_object_get_string_member(parameter, "name");
      const char *label = json_object_get_string_member(parameter, "label");
      const size_t length = strlen(shape);
      if(strncmp(label, shape, length) || label[length] != ' ' || strncmp(name, "mask.", 5)) continue;
      char *end;
      const long id = strtol(name + 5, &end, 10);
      if(end > name + 5 && *end == '.' && id > 0 && id <= INT32_MAX) { result = id; goto done; }
    }
  }
done:
  g_object_unref(parser);
  return result;
}

/* Original mask processors consume every node and handle below. Pixel checks
 * compare edits with the same module enabled, and history restores exact form
 * geometry rather than only reproducing the ordinary module parameters. */
static int mobile_path_mask_checks(dt_mobile_session *session)
{
  char *initial = NULL, *baseline = NULL, *created = NULL, *split = NULL, *metadata = NULL;
  char *history = NULL, *after_history = NULL;
  uint8_t *uniform = NULL, *masked = NULL, *moved = NULL, *restored = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0, success = 0;
  char name[128], x[128], insert[128], remove[128];
  ENGINE_OK(dt_mobile_modules(session, &initial));
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 1));
  ENGINE_OK(dt_mobile_modules(session, &baseline));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &uniform, &width, &height));
  dt_mobile_free_buffer(uniform); uniform = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &uniform, &width, &height));
  const char *const shapes[] = { "path", "brush" };
  for(unsigned shape = 0; shape < 2; shape++)
  {
    fprintf(stderr, "path masks: %s geometry and native pixels\n", shapes[shape]);
    snprintf(name, sizeof(name), "mask.add.%s", shapes[shape]);
    CHECK(module_has(baseline, "exposure", name));
    ENGINE_OK(dt_mobile_set_parameter(session, "exposure", name, 1));
    ENGINE_OK(dt_mobile_modules(session, &created));
    const int32_t id = mobile_test_mask_shape_id(created, "exposure", shapes[shape]);
    CHECK(id > 0);
    snprintf(x, sizeof(x), "mask.%d.node.0.x", id);
    snprintf(insert, sizeof(insert), "mask.%d.node.0.insert_after", id);
    snprintf(remove, sizeof(remove), "mask.%d.node.1.delete", id);
    CHECK(parameter_equals(created, "exposure", x, (double).35f));
    snprintf(name, sizeof(name), "mask.%d.node.%d.x", id, shape ? 2 : 3);
    CHECK(module_has(created, "exposure", name));
    ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &masked, &other_width, &other_height));
    CHECK(width == other_width && height == other_height);
    CHECK(memcmp(uniform, masked, (size_t)width * height * 4));
    ENGINE_OK(dt_mobile_history(session, &history));
    CHECK(dt_mobile_set_parameter(session, "exposure", x, 1.1) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    CHECK(dt_mobile_set_parameter(session, "exposure", x, NAN) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    if(!shape) CHECK(dt_mobile_set_parameter(session, "exposure", x, .65) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    snprintf(name, sizeof(name), "mask.%d.node.00.x", id);
    CHECK(dt_mobile_set_parameter(session, "exposure", name, .5) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    snprintf(name, sizeof(name), "mask.%d.node.64.x", id);
    CHECK(dt_mobile_set_parameter(session, "exposure", name, .5) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    snprintf(name, sizeof(name), "mask.%d.node.0.border_in", id);
    CHECK(dt_mobile_set_parameter(session, "exposure", name, 0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    snprintf(name, sizeof(name), "mask.%d.enabled", id);
    CHECK(!module_has(created, "exposure", name));
    CHECK(dt_mobile_set_parameter(session, "exposure", name, 0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    snprintf(name, sizeof(name), "mask.%d.operation", id);
    CHECK(!module_has(created, "exposure", name));
    CHECK(dt_mobile_set_parameter(session, "exposure", name, 32) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    ENGINE_OK(dt_mobile_history(session, &after_history));
    CHECK(!strcmp(history, after_history));
    dt_mobile_free_buffer(history); history = NULL;
    dt_mobile_free_buffer(after_history); after_history = NULL;
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    CHECK(!strcmp(created, metadata));
    dt_mobile_free_buffer(metadata); metadata = NULL;

    /* Preserve the native minimum topology: a closed path needs three nodes,
     * and an open brush needs two. Failure must not acquire a history item. */
    snprintf(name, sizeof(name), "mask.%d.node.%d.delete", id, shape ? 2 : 3);
    ENGINE_OK(dt_mobile_set_parameter(session, "exposure", name, 1));
    ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
    CHECK(restored && width == other_width && height == other_height);
    dt_mobile_free_buffer(restored); restored = NULL;
    ENGINE_OK(dt_mobile_history(session, &history));
    snprintf(name, sizeof(name), "mask.%d.node.0.delete", id);
    CHECK(dt_mobile_set_parameter(session, "exposure", name, 1) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    ENGINE_OK(dt_mobile_history(session, &after_history));
    CHECK(!strcmp(history, after_history));
    dt_mobile_free_buffer(history); history = NULL;
    dt_mobile_free_buffer(after_history); after_history = NULL;
    ENGINE_OK(dt_mobile_undo(session));
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    CHECK(!strcmp(created, metadata));
    dt_mobile_free_buffer(metadata); metadata = NULL;

    ENGINE_OK(dt_mobile_set_parameter(session, "exposure", x, .2));
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

    /* A manual Bezier handle and brush density are independently consumed by
     * the original geometry/rasterizer, beyond merely translating a shape. */
    snprintf(name, sizeof(name), "mask.%d.node.0.%s", id, shape ? "density" : "ctrl2_y");
    ENGINE_OK(dt_mobile_set_parameter(session, "exposure", name, shape ? .2 : .1));
    ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
    CHECK(width == other_width && height == other_height);
    CHECK(memcmp(masked, restored, (size_t)width * height * 4));
    dt_mobile_free_buffer(restored); restored = NULL;
    ENGINE_OK(dt_mobile_undo(session));
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    CHECK(!strcmp(created, metadata));
    dt_mobile_free_buffer(metadata); metadata = NULL;

    ENGINE_OK(dt_mobile_set_parameter(session, "exposure", insert, 1));
    ENGINE_OK(dt_mobile_modules(session, &split));
    snprintf(name, sizeof(name), "mask.%d.node.%d.x", id, shape ? 3 : 4);
    CHECK(module_has(split, "exposure", name));
    snprintf(name, sizeof(name), "mask.%d.node.1.x", id);
    double midpoint;
    CHECK(mobile_color_number(split, "exposure", name, "value", &midpoint));
    CHECK(fabs(midpoint - (shape ? .425 : .5)) < 1e-6);
    ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
    CHECK(restored && width == other_width && height == other_height);
    dt_mobile_free_buffer(restored); restored = NULL;
    ENGINE_OK(dt_mobile_set_parameter(session, "exposure", remove, 1));
    ENGINE_OK(dt_mobile_undo(session));
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    CHECK(!strcmp(split, metadata));
    dt_mobile_free_buffer(metadata); metadata = NULL;
    ENGINE_OK(dt_mobile_redo(session));
    ENGINE_OK(dt_mobile_undo(session));
    ENGINE_OK(dt_mobile_undo(session));
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    CHECK(!strcmp(created, metadata));
    dt_mobile_free_buffer(metadata); metadata = NULL;
    ENGINE_OK(dt_mobile_undo(session));
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    CHECK(!strcmp(baseline, metadata));
    dt_mobile_free_buffer(metadata); metadata = NULL;
    dt_mobile_free_buffer(created); created = NULL;
    dt_mobile_free_buffer(split); split = NULL;
    dt_mobile_free_buffer(masked); masked = NULL;
    dt_mobile_free_buffer(moved); moved = NULL;
  }

  fprintf(stderr, "path masks: original group combinations\n");
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "mask.add.circle", 1));
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "mask.add.path", 1));
  ENGINE_OK(dt_mobile_modules(session, &created));
  const int32_t path_id = mobile_test_mask_shape_id(created, "exposure", "path");
  CHECK(path_id > 0);
  snprintf(name, sizeof(name), "mask.%d.operation", path_id);
  CHECK(parameter_equals(created, "exposure", name, 8));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &masked, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", name, 32));
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
  ENGINE_OK(dt_mobile_history(session, &history));
  CHECK(dt_mobile_set_parameter(session, "exposure", name, 24) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history));
  dt_mobile_free_buffer(history); history = NULL;
  dt_mobile_free_buffer(after_history); after_history = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(initial, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;

  /* Native repair bookkeeping accepts path/brush identities as well as the
   * earlier circles, and unsupported spots brushes must never be advertised. */
  CHECK(!module_has(initial, "spots", "mask.add.brush"));
  const char *const operations[] = { "spots", "retouch", "retouch" };
  const char *const actions[] = { "mask.add.path", "mask.add.blur.brush", "mask.add.fill.path" };
  const char *const kinds[] = { "path", "brush", "path" };
  for(unsigned index = 0; index < 3; index++)
  {
    ENGINE_OK(dt_mobile_set_parameter(session, operations[index], actions[index], 1));
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    CHECK(mobile_test_mask_shape_id(metadata, operations[index], kinds[index]) > 0);
    dt_mobile_free_buffer(metadata); metadata = NULL;
    ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
    CHECK(restored && other_width > 0 && other_height > 0);
    dt_mobile_free_buffer(restored); restored = NULL;
    ENGINE_OK(dt_mobile_undo(session));
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    CHECK(!strcmp(initial, metadata));
    dt_mobile_free_buffer(metadata); metadata = NULL;
  }
  success = 1;
failed:
  dt_mobile_free_buffer(initial); dt_mobile_free_buffer(baseline); dt_mobile_free_buffer(created);
  dt_mobile_free_buffer(split); dt_mobile_free_buffer(metadata); dt_mobile_free_buffer(history);
  dt_mobile_free_buffer(after_history); dt_mobile_free_buffer(uniform); dt_mobile_free_buffer(masked);
  dt_mobile_free_buffer(moved); dt_mobile_free_buffer(restored);
  return success;
}

#endif
