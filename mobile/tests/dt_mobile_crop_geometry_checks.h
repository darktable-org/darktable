/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_CROP_GEOMETRY_CHECKS_H
#define DT_MOBILE_CROP_GEOMETRY_CHECKS_H

static int mobile_test_crop_input(const char *metadata, int *width, int *height)
{
  JsonParser *parser = json_parser_new();
  int result = 0;
  if(!json_parser_load_from_data(parser, metadata, -1, NULL)) goto done;
  JsonArray *modules = json_node_get_array(json_parser_get_root(parser));
  for(guint index = 0; index < json_array_get_length(modules); index++)
  {
    JsonObject *module = json_array_get_object_element(modules, index);
    if(strcmp(json_object_get_string_member(module, "operation"), "crop")) continue;
    JsonObject *crop = json_object_get_object_member(module, "crop");
    if(!crop || !json_object_has_member(crop, "inputWidth") || !json_object_has_member(crop, "inputHeight")) break;
    *width = json_object_get_int_member(crop, "inputWidth");
    *height = json_object_get_int_member(crop, "inputHeight");
    result = *width > 0 && *height > 0;
    break;
  }
done:
  g_object_unref(parser);
  return result;
}

static int mobile_crop_geometry_checks(dt_mobile_session *session, const char *export_path)
{
  char *baseline = NULL, *scaled = NULL, *metadata = NULL;
  int width = 0, height = 0, input_width = 0, input_height = 0, success = 0;
  uint32_t output_width = 0, output_height = 0;
  ENGINE_OK(dt_mobile_modules(session, &baseline));
  CHECK(mobile_test_crop_input(baseline, &width, &height));
  /* This native geometry operation precedes crop. The new ratio editor must
   * use its actual changed ROI, rather than the original square fixture. */
  ENGINE_OK(dt_mobile_set_parameter(session, "scalepixels", "pixel_aspect_ratio", 2));
  ENGINE_OK(dt_mobile_modules(session, &scaled));
  CHECK(mobile_test_crop_input(scaled, &input_width, &input_height));
  CHECK(input_width == width * 2 && input_height == height);
  ENGINE_OK(dt_mobile_set_parameter(session, "flip", "orientation", 5));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(mobile_test_crop_input(metadata, &input_width, &input_height));
  CHECK(input_width == height && input_height == width * 2);
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(scaled, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_set_crop(session, "crop", .25f, 0, .75f, 1));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(mobile_test_crop_input(metadata, &input_width, &input_height));
  CHECK(input_width == width * 2 && input_height == height);
  /* The processed output differs from that input, including with the module
   * itself enabled. A cropped preview cannot supply the ratio dimensions. */
  ENGINE_OK(dt_mobile_export(session, export_path, "png", 90));
  CHECK(png_dimensions(export_path, &output_width, &output_height));
  CHECK(output_width == (uint32_t)width && output_height == (uint32_t)height);
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  success = 1;
failed:
  dt_mobile_free_buffer(baseline);
  dt_mobile_free_buffer(scaled);
  dt_mobile_free_buffer(metadata);
  return success;
}
#endif
