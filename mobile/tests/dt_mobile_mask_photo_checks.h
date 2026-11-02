/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_MASK_PHOTO_CHECKS_H
#define DT_MOBILE_MASK_PHOTO_CHECKS_H

#include <json-glib/json-glib.h>

static int mobile_mask_photo_test_handle(const char *json, int32_t id, int source,
    double *x, double *y, int *visible)
{
  JsonParser *parser = json_parser_new();
  int found = 0;
  if(!json_parser_load_from_data(parser, json, -1, NULL)) goto done;
  JsonObject *root = json_node_get_object(json_parser_get_root(parser));
  JsonArray *forms = json_object_get_array_member(root, "forms");
  for(guint index = 0; index < json_array_get_length(forms); index++)
  {
    JsonObject *form = json_array_get_object_element(forms, index);
    if(json_object_get_int_member(form, "id") != id) continue;
    const char *name = source ? "source" : "center";
    if(!json_object_has_member(form, name)) break;
    JsonObject *point = json_object_get_object_member(form, name);
    *x = JSON_NODE_HOLDS_NULL(json_object_get_member(point, "x")) ? NAN : json_object_get_double_member(point, "x");
    *y = JSON_NODE_HOLDS_NULL(json_object_get_member(point, "y")) ? NAN : json_object_get_double_member(point, "y");
    *visible = json_object_get_boolean_member(point, "visible");
    found = 1;
    break;
  }
done:
  g_object_unref(parser);
  return found;
}

static double mobile_mask_photo_test_value(const char *json, const char *operation, const char *name)
{
  JsonParser *parser = json_parser_new();
  double result = NAN;
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
      if(strcmp(json_object_get_string_member(parameter, "name"), name)) continue;
      result = json_object_get_double_member(parameter, "value");
      goto done;
    }
  }
done:
  g_object_unref(parser);
  return result;
}

static long mobile_mask_photo_test_position(const char *json)
{
  const char *position = strstr(json, "\"position\":");
  return position ? strtol(position + strlen("\"position\":"), NULL, 10) : -1;
}

/* These checks use the real engine and original native masks. Independent crop,
 * aspect and orientation arithmetic checks the image-space contract; render and
 * history checks make a two-request x/y implementation observably incorrect. */
static int mobile_mask_photo_checks(dt_mobile_session *session)
{
  char *initial = NULL, *before = NULL, *moved = NULL, *metadata = NULL;
  char *points = NULL, *history = NULL, *after_history = NULL;
  uint8_t *original_pixels = NULL, *moved_pixels = NULL, *restored = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0;
  int original_width = 0, original_height = 0, visible = 0, edits = 0, success = 0;
  double px = NAN, py = NAN;
  char center_x[96], center_y[96], source_x[96], source_y[96];
  fprintf(stderr, "mask photo placement: original geometry, atomic XY and native history\n");
  ENGINE_OK(dt_mobile_modules(session, &initial));
  CHECK(mobile_test_crop_input(initial, &original_width, &original_height));
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 1)); edits++;
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "mask.add.circle", 1)); edits++;
  ENGINE_OK(dt_mobile_modules(session, &before));
  const int32_t id = mobile_test_mask_id(before, "exposure");
  CHECK(id > 0);
  snprintf(center_x, sizeof(center_x), "mask.%d.x", id);
  snprintf(center_y, sizeof(center_y), "mask.%d.y", id);
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &original_pixels, &width, &height));
  ENGINE_OK(dt_mobile_history(session, &history));
  ENGINE_OK(dt_mobile_mask_photo_points(session, "exposure", &points));
  CHECK(mobile_mask_photo_test_handle(points, id, 0, &px, &py, &visible));
  CHECK(visible && fabs(px - .5) < 1e-6 && fabs(py - .5) < 1e-6);
  CHECK(!mobile_mask_photo_test_handle(points, id, 1, &px, &py, &visible));
  dt_mobile_free_buffer(points); points = NULL;
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history)); /* Listing handles is read-only. */
  dt_mobile_free_buffer(after_history); after_history = NULL;
  ENGINE_OK(dt_mobile_mask_place_photo(session, "exposure", id, 0, .25, .625)); edits++;
  ENGINE_OK(dt_mobile_modules(session, &moved));
  CHECK(parameter_equals(moved, "exposure", center_x, .25));
  CHECK(parameter_equals(moved, "exposure", center_y, .625));
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(mobile_mask_photo_test_position(after_history) == mobile_mask_photo_test_position(history) + 1);
  dt_mobile_free_buffer(history); history = NULL;
  dt_mobile_free_buffer(after_history); after_history = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &moved_pixels, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(original_pixels, moved_pixels, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(before, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(original_pixels, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_redo(session)); edits++;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(moved_pixels, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;

  ENGINE_OK(dt_mobile_history(session, &history));
  const double invalid[][2] = { { -.01, .5 }, { 1, .5 }, { .5, -.01 }, { .5, 1 }, { NAN, .5 }, { .5, INFINITY } };
  for(size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++)
    CHECK(dt_mobile_mask_place_photo(session, "exposure", id, 0, invalid[index][0], invalid[index][1])
          == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_mask_place_photo(session, "exposure", id, 1, .5, .5) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_mask_place_photo(session, "exposure", INT32_MAX, 0, .5, .5) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_mask_place_photo(session, "spots", id, 0, .5, .5) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history));
  dt_mobile_free_buffer(history); history = NULL;
  dt_mobile_free_buffer(after_history); after_history = NULL;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(moved, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_set_enabled(session, "exposure", 0)); edits++;
  CHECK(dt_mobile_mask_place_photo(session, "exposure", id, 0, .5, .5) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_undo(session)); edits--;

  /* Exposure is before crop. A cropped-out stored center stays hidden, and a
   * tap maps through crop using actual floored preview dimensions. */
  ENGINE_OK(dt_mobile_set_crop(session, "crop", .375f, .125f, .875f, .875f)); edits++;
  ENGINE_OK(dt_mobile_render_preview(session, 95, 73, &restored, &other_width, &other_height));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_mask_photo_points(session, "exposure", &points));
  CHECK(mobile_mask_photo_test_handle(points, id, 0, &px, &py, &visible) && !visible);
  dt_mobile_free_buffer(points); points = NULL;
  ENGINE_OK(dt_mobile_mask_place_photo(session, "exposure", id, 0, .25, .5)); edits++;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  const double crop_scale = fmin(1., fmin(95. / (original_width * .5), 73. / (original_height * .75)));
  CHECK(fabs(mobile_mask_photo_test_value(metadata, "exposure", center_x)
    - (.375 + .25 * other_width / crop_scale / original_width)) < 1e-6);
  CHECK(fabs(mobile_mask_photo_test_value(metadata, "exposure", center_y)
    - (.125 + .5 * other_height / crop_scale / original_height)) < 1e-6);
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_undo(session)); edits--;

  ENGINE_OK(dt_mobile_set_parameter(session, "scalepixels", "pixel_aspect_ratio", 2)); edits++;
  ENGINE_OK(dt_mobile_render_preview(session, 95, 73, &restored, &other_width, &other_height));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_mask_place_photo(session, "exposure", id, 0, .25, .625)); edits++;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  const double aspect_scale = fmin(1., fmin(95. / (original_width * 2), 73. / original_height));
  CHECK(fabs(mobile_mask_photo_test_value(metadata, "exposure", center_x)
    - .25 * other_width / aspect_scale / (original_width * 2)) < 1e-6);
  CHECK(fabs(mobile_mask_photo_test_value(metadata, "exposure", center_y)
    - .625 * other_height / aspect_scale / original_height) < 1e-6);
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_set_parameter(session, "flip", "orientation", 5)); edits++;
  ENGINE_OK(dt_mobile_render_preview(session, 95, 73, &restored, &other_width, &other_height));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_mask_place_photo(session, "exposure", id, 0, .375, .625)); edits++;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  const double flip_scale = fmin(1., fmin(95. / original_height, 73. / (original_width * 2)));
  CHECK(fabs(mobile_mask_photo_test_value(metadata, "exposure", center_x)
    - .625 * other_height / flip_scale / (original_width * 2)) < 1e-6);
  CHECK(fabs(mobile_mask_photo_test_value(metadata, "exposure", center_y)
    - (1. - .375 * other_width / flip_scale / original_height)) < 1e-6);
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_mask_photo_points(session, "exposure", &points));
  CHECK(mobile_mask_photo_test_handle(points, id, 0, &px, &py, &visible));
  CHECK(visible && fabs(px - .375) < 1e-6 && fabs(py - .625) < 1e-6);
  dt_mobile_free_buffer(points); points = NULL;
  for(int index = 0; index < 4; index++) { ENGINE_OK(dt_mobile_undo(session)); edits--; }

  /* The other aspect axis has a fractional output size before native ROI
   * rounding. Its callbacks must use those committed dimensions, just like
   * native pixel processing, rather than resampling-buffer scale factors. */
  ENGINE_OK(dt_mobile_set_parameter(session, "scalepixels", "pixel_aspect_ratio", .625)); edits++;
  ENGINE_OK(dt_mobile_render_preview(session, 95, 73, &restored, &other_width, &other_height));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_mask_place_photo(session, "exposure", id, 0, .3, .7)); edits++;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  int fractional_width = 0, fractional_height = 0;
  CHECK(mobile_test_crop_input(metadata, &fractional_width, &fractional_height));
  CHECK(fractional_width == original_width && fractional_height == (int)ceil(original_height / .625));
  const double fractional_scale = fmin(1., fmin(95. / fractional_width, 73. / fractional_height));
  CHECK(fabs(mobile_mask_photo_test_value(metadata, "exposure", center_x)
    - .3 * other_width / fractional_scale / fractional_width) < 1e-6);
  CHECK(fabs(mobile_mask_photo_test_value(metadata, "exposure", center_y)
    - .7 * other_height / fractional_scale / fractional_height) < 1e-6);
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_mask_photo_points(session, "exposure", &points));
  CHECK(mobile_mask_photo_test_handle(points, id, 0, &px, &py, &visible));
  CHECK(visible && fabs(px - .3) < 1e-6 && fabs(py - .7) < 1e-6);
  dt_mobile_free_buffer(points); points = NULL;
  for(int index = 0; index < 2; index++) { ENGINE_OK(dt_mobile_undo(session)); edits--; }

  /* Native lens callbacks return FALSE for the identity geometry of manual
   * vignette mode. The original traversal still supports every photo point. */
  ENGINE_OK(dt_mobile_set_parameter(session, "lens", "method", 2)); edits++;
  ENGINE_OK(dt_mobile_set_parameter(session, "lens", "v_strength", .8)); edits++;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_mask_place_photo(session, "exposure", id, 0, .35, .65)); edits++;
  ENGINE_OK(dt_mobile_mask_photo_points(session, "exposure", &points));
  CHECK(mobile_mask_photo_test_handle(points, id, 0, &px, &py, &visible));
  CHECK(visible && fabs(px - .35) < 1e-6 && fabs(py - .65) < 1e-6);
  dt_mobile_free_buffer(points); points = NULL;
  for(int index = 0; index < 3; index++) { ENGINE_OK(dt_mobile_undo(session)); edits--; }

  /* The perspective bounding box contains black corners. Those taps must not
   * alter masks/history; an interior point still round-trips to its crosshair. */
  ENGINE_OK(dt_mobile_set_parameter(session, "ashift", "rotation", 10)); edits++;
  ENGINE_OK(dt_mobile_set_parameter(session, "ashift", "lensshift_v", .2)); edits++;
  ENGINE_OK(dt_mobile_render_preview(session, 95, 73, &restored, &other_width, &other_height));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_history(session, &history));
  CHECK(dt_mobile_mask_place_photo(session, "exposure", id, 0, .001, .001) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history));
  dt_mobile_free_buffer(history); history = NULL;
  dt_mobile_free_buffer(after_history); after_history = NULL;
  ENGINE_OK(dt_mobile_mask_place_photo(session, "exposure", id, 0, .45, .55)); edits++;
  ENGINE_OK(dt_mobile_mask_photo_points(session, "exposure", &points));
  CHECK(mobile_mask_photo_test_handle(points, id, 0, &px, &py, &visible));
  CHECK(visible && fabs(px - .45) < 1e-5 && fabs(py - .55) < 1e-5);
  dt_mobile_free_buffer(points); points = NULL;
  for(int index = 0; index < 3; index++) { ENGINE_OK(dt_mobile_undo(session)); edits--; }

  ENGINE_OK(dt_mobile_set_parameter(session, "spots", "mask.add.circle", 1)); edits++;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  const int32_t clone_id = mobile_test_mask_id(metadata, "spots");
  CHECK(clone_id > 0);
  snprintf(source_x, sizeof(source_x), "mask.%d.source_x", clone_id);
  snprintf(source_y, sizeof(source_y), "mask.%d.source_y", clone_id);
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_mask_place_photo(session, "spots", clone_id, 1, .625, .375)); edits++;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(parameter_equals(metadata, "spots", source_x, .625));
  CHECK(parameter_equals(metadata, "spots", source_y, .375));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_mask_photo_points(session, "spots", &points));
  CHECK(mobile_mask_photo_test_handle(points, clone_id, 1, &px, &py, &visible));
  CHECK(visible && fabs(px - .625) < 1e-6 && fabs(py - .375) < 1e-6);
  dt_mobile_free_buffer(points); points = NULL;
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_undo(session)); edits--;

  /* An unrotated path has a precise native first-node source anchor. Rotation
   * moves that node around a centroid, so its stored anchor is no longer an
   * accurate marker and must disappear without changing the numeric controls. */
  ENGINE_OK(dt_mobile_set_parameter(session, "retouch", "mask.add.clone.path", 1)); edits++;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  const int32_t path_id = mobile_test_mask_id(metadata, "retouch");
  CHECK(path_id > 0);
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_mask_photo_points(session, "retouch", &points));
  CHECK(!mobile_mask_photo_test_handle(points, path_id, 0, &px, &py, &visible));
  CHECK(mobile_mask_photo_test_handle(points, path_id, 1, &px, &py, &visible));
  dt_mobile_free_buffer(points); points = NULL;
  ENGINE_OK(dt_mobile_mask_place_photo(session, "retouch", path_id, 1, .6, .4)); edits++;
  char rotation[96]; snprintf(rotation, sizeof(rotation), "mask.%d.source_rotation", path_id);
  ENGINE_OK(dt_mobile_set_parameter(session, "retouch", rotation, 45)); edits++;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  ENGINE_OK(dt_mobile_history(session, &history));
  ENGINE_OK(dt_mobile_mask_photo_points(session, "retouch", &points));
  CHECK(!mobile_mask_photo_test_handle(points, path_id, 1, &px, &py, &visible));
  dt_mobile_free_buffer(points); points = NULL;
  CHECK(dt_mobile_mask_place_photo(session, "retouch", path_id, 1, .2, .8) == DT_MOBILE_ERROR_UNSUPPORTED);
  ENGINE_OK(dt_mobile_modules(session, &points));
  CHECK(!strcmp(metadata, points));
  dt_mobile_free_buffer(points); points = NULL;
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history));
  dt_mobile_free_buffer(history); history = NULL;
  dt_mobile_free_buffer(after_history); after_history = NULL;
  for(int index = 0; index < 3; index++) { ENGINE_OK(dt_mobile_undo(session)); edits--; }

  const char *shapes[] = { "mask.add.ellipse", "mask.add.gradient" };
  for(size_t index = 0; index < sizeof(shapes) / sizeof(shapes[0]); index++)
  {
    ENGINE_OK(dt_mobile_set_parameter(session, "exposure", shapes[index], 1)); edits++;
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    /* Existing circle is first; use the new form's ID from photo metadata. */
    dt_mobile_free_buffer(metadata); metadata = NULL;
    ENGINE_OK(dt_mobile_mask_photo_points(session, "exposure", &points));
    JsonParser *parser = json_parser_new();
    CHECK(json_parser_load_from_data(parser, points, -1, NULL));
    JsonArray *forms = json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)), "forms");
    const int32_t shape_id = json_object_get_int_member(json_array_get_object_element(forms,
      json_array_get_length(forms) - 1), "id");
    g_object_unref(parser);
    dt_mobile_free_buffer(points); points = NULL;
    ENGINE_OK(dt_mobile_mask_place_photo(session, "exposure", shape_id, 0, .3, .7)); edits++;
    ENGINE_OK(dt_mobile_mask_photo_points(session, "exposure", &points));
    CHECK(mobile_mask_photo_test_handle(points, shape_id, 0, &px, &py, &visible));
    CHECK(visible && fabs(px - .3) < 1e-6 && fabs(py - .7) < 1e-6);
    dt_mobile_free_buffer(points); points = NULL;
    ENGINE_OK(dt_mobile_undo(session)); edits--;
    ENGINE_OK(dt_mobile_undo(session)); edits--;
  }
  while(edits > 0) { ENGINE_OK(dt_mobile_undo(session)); edits--; }
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(initial, metadata));
  success = 1;
failed:
  dt_mobile_free_buffer(initial);
  dt_mobile_free_buffer(before);
  dt_mobile_free_buffer(moved);
  dt_mobile_free_buffer(metadata);
  dt_mobile_free_buffer(points);
  dt_mobile_free_buffer(history);
  dt_mobile_free_buffer(after_history);
  dt_mobile_free_buffer(original_pixels);
  dt_mobile_free_buffer(moved_pixels);
  dt_mobile_free_buffer(restored);
  return success;
}

#endif
