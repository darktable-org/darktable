/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_EXTENDED_CONTROLS_CHECKS_H
#define DT_MOBILE_EXTENDED_CONTROLS_CHECKS_H

/* Exercise the production introspection -> public setter -> original develop
 * history path. Iterating the advertised fields also tests complete native
 * channel/band traversal rather than copying the policy table into a test. */
typedef struct mobile_advertised_control
{
  char *operation, *name;
  double value;
  int current_in_domain;
} mobile_advertised_control;

static int mobile_extended_controls_checks(dt_mobile_session *session)
{
  GArray *controls = g_array_new(FALSE, FALSE, sizeof(mobile_advertised_control));
  JsonParser *parser = json_parser_new();
  char *baseline = NULL, *changed = NULL, *history = NULL, *after_history = NULL;
  uint8_t *before = NULL, *after = NULL, *restored = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0, success = 0;
#define EXT_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "extended controls:%d: check failed: %s\n", __LINE__, #condition); goto finished; } } while(0)
#define EXT_OK(call) do { if((call) != DT_MOBILE_OK) { \
  fprintf(stderr, "extended controls: %s: %s\n", #call, dt_mobile_last_error(session)); goto finished; } } while(0)
  EXT_OK(dt_mobile_modules(session, &baseline));
  EXT_CHECK(json_parser_load_from_data(parser, baseline, -1, NULL));
  JsonNode *root = json_parser_get_root(parser);
  EXT_CHECK(JSON_NODE_HOLDS_ARRAY(root));
  JsonArray *modules = json_node_get_array(root);
  /* This integration fixture is a full-color PNG, for which the original
   * engine offers clipping only and disables demosaicing altogether. */
  double unsupported_method;
  EXT_CHECK(!mobile_color_number(baseline, "demosaic", "demosaicing_method", "value", &unsupported_method));
  EXT_CHECK(!mobile_color_number(baseline, "rawprepare", "raw_white_point", "value", &unsupported_method));
  EXT_CHECK(!mobile_color_number(baseline, "rawprepare", "raw_black_level_separate[0]", "value", &unsupported_method));
  int checked_highlights_modes = 0;
  for(guint module_index = 0; module_index < json_array_get_length(modules); module_index++)
  {
    JsonObject *module = json_array_get_object_element(modules, module_index);
    const char *operation = json_object_get_string_member(module, "operation");
    JsonArray *parameters = json_object_get_array_member(module, "parameters");
    for(guint parameter_index = 0; parameter_index < json_array_get_length(parameters); parameter_index++)
    {
      JsonObject *parameter = json_array_get_object_element(parameters, parameter_index);
      if(!strcmp(json_object_get_string_member(parameter, "type"), "action")) continue;
      if(!strcmp(operation, "highlights")
         && !strcmp(json_object_get_string_member(parameter, "name"), "mode"))
      {
        JsonArray *choices = json_object_get_array_member(parameter, "choices");
        EXT_CHECK(json_array_get_length(choices) == 1);
        EXT_CHECK(json_object_get_double_member(json_array_get_object_element(choices, 0), "value") == 0.0);
        checked_highlights_modes = 1;
      }
      mobile_advertised_control control = {
        g_strdup(operation), g_strdup(json_object_get_string_member(parameter, "name")),
        json_object_get_double_member(parameter, "value"), 1
      };
      const double minimum = json_object_get_double_member(parameter, "min");
      const double maximum = json_object_get_double_member(parameter, "max");
      EXT_CHECK(isfinite(minimum) && isfinite(maximum) && minimum <= maximum);
      control.current_in_domain = control.value >= minimum && control.value <= maximum;
      if(json_object_has_member(parameter, "choices"))
      {
        JsonArray *choices = json_object_get_array_member(parameter, "choices");
        int found = 0;
        for(guint choice = 0; choice < json_array_get_length(choices); choice++)
          if(json_object_get_double_member(json_array_get_object_element(choices, choice), "value") == control.value) found = 1;
        control.current_in_domain = control.current_in_domain && found;
      }
      EXT_CHECK(isfinite(control.value));
      g_array_append_val(controls, control);
    }
  }
  /* This fixture loads the complete configured processing graph. Catch an
   * accidental return to the earlier 103-field subset, without freezing a
   * count that changes when a new original module is built. */
  EXT_CHECK(controls->len >= 350);
  EXT_CHECK(checked_highlights_modes);
  fprintf(stderr, "extended controls: native roundtrip for %u advertised parameters\n", controls->len);
  for(guint index = 0; index < controls->len; index++)
  {
    const mobile_advertised_control *control = &g_array_index(controls, mobile_advertised_control, index);
    fprintf(stderr, "extended controls: %s.%s\n", control->operation, control->name);
    if(!control->current_in_domain)
    {
      /* Imported values outside the reviewed domain remain visible. Their
       * presence never grants write permission or a new history operation. */
      EXT_OK(dt_mobile_history(session, &history));
      EXT_CHECK(dt_mobile_set_parameter(session, control->operation, control->name, control->value)
                == DT_MOBILE_ERROR_INVALID_ARGUMENT);
      EXT_OK(dt_mobile_modules(session, &changed));
      EXT_CHECK(!strcmp(baseline, changed));
      EXT_OK(dt_mobile_history(session, &after_history));
      EXT_CHECK(!strcmp(history, after_history));
      dt_mobile_free_buffer(changed); changed = NULL;
      dt_mobile_free_buffer(history); history = NULL;
      dt_mobile_free_buffer(after_history); after_history = NULL;
      continue;
    }
    EXT_OK(dt_mobile_set_parameter(session, control->operation, control->name, control->value));
    EXT_OK(dt_mobile_modules(session, &changed));
    EXT_CHECK(parameter_equals(changed, control->operation, control->name, control->value));
    dt_mobile_free_buffer(changed); changed = NULL;
    EXT_OK(dt_mobile_undo(session));
    EXT_OK(dt_mobile_modules(session, &changed));
    if(strcmp(baseline, changed))
    {
      size_t difference = 0;
      while(baseline[difference] && changed[difference] && baseline[difference] == changed[difference]) difference++;
      const size_t context = difference > 120 ? difference - 120 : 0;
      fprintf(stderr, "extended controls: metadata differs at byte %zu\nbefore: %.300s\nafter: %.300s\n",
              difference, baseline + context, changed + context);
    }
    EXT_CHECK(!strcmp(baseline, changed));
    dt_mobile_free_buffer(changed); changed = NULL;
  }
  dt_mobile_free_buffer(baseline); baseline = NULL;

  /* The source fixture contains distinct colors and luminance levels. Each
   * check compares two parameter settings with the same original module
   * enabled, so merely toggling an otherwise inert module cannot pass. */
  static const struct { const char *operation, *name; double original, edited; } pixels[] = {
    { "basicadj", "brightness", 0., .2 },
    { "filmicrgb", "contrast", 1.2, 1.8 },
    { "agx", "look_slope", 1., 1.2 },
    { "colorequal", "sat_red", 1., 1.7 },
    { "toneequal", "midtones", 0., .5 },
    { "negadoctor", "black", .0755, .15 },
  };
  for(size_t index = 0; index < sizeof(pixels) / sizeof(pixels[0]); index++)
  {
    fprintf(stderr, "extended controls: original pixel processing %s.%s\n", pixels[index].operation, pixels[index].name);
    EXT_OK(dt_mobile_set_parameter(session, pixels[index].operation, pixels[index].name, pixels[index].original));
    /* Prime the unchanged pixelpipe once, then retain the stable baseline. */
    EXT_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
    dt_mobile_free_buffer(before); before = NULL;
    EXT_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
    EXT_OK(dt_mobile_modules(session, &baseline));
    EXT_OK(dt_mobile_set_parameter(session, pixels[index].operation, pixels[index].name, pixels[index].edited));
    EXT_OK(dt_mobile_render_preview(session, 96, 96, &after, &other_width, &other_height));
    EXT_CHECK(width == other_width && height == other_height);
    EXT_CHECK(memcmp(before, after, (size_t)width * height * 4));
    EXT_OK(dt_mobile_undo(session));
    EXT_OK(dt_mobile_modules(session, &changed));
    if(strcmp(baseline, changed))
    {
      /* Keep complete evidence for a failed native undo, including optional
       * capabilities that depend on other modules. CTest runs in its build
       * directory, so these diagnostics stay with the generated test output. */
      if(!g_file_set_contents("dt-mobile-extended-before.json", baseline, -1, NULL)
         || !g_file_set_contents("dt-mobile-extended-after.json", changed, -1, NULL))
        fprintf(stderr, "extended controls: unable to save metadata diagnostics\n");
    }
    EXT_CHECK(!strcmp(baseline, changed));
    dt_mobile_free_buffer(changed); changed = NULL;
    EXT_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
    EXT_CHECK(width == other_width && height == other_height);
    EXT_CHECK(!memcmp(before, restored, (size_t)width * height * 4));
    dt_mobile_free_buffer(before); before = NULL;
    dt_mobile_free_buffer(after); after = NULL;
    dt_mobile_free_buffer(restored); restored = NULL;
    dt_mobile_free_buffer(baseline); baseline = NULL;
    EXT_OK(dt_mobile_undo(session));
  }

  /* Values inside storage domains can still violate desktop tuple semantics.
   * A rejected edit must preserve every parameter and the entire history. */
  static const struct { const char *operation, *name; double value; } invalid[] = {
    { "demosaic", "demosaicing_method", 5. },
    { "highlights", "mode", 5. },
    { "filmicrgb", "white_point_target", 0. },
    { "filmic", "white_point_target", 0. },
    { "agx", "curve_pivot_y_linear_output", 0. },
    { "sharpen", "radius", .01 },
    { "censorize", "radius_1", .01 },
    { "channelmixerrgb", "illuminant", 8. },
    { "channelmixerrgb", "illuminant", 9. },
    { "channelmixerrgb", "illuminant", 11. },
    { "channelmixerrgb", "illuminant", 10. },
    { "channelmixerrgb", "illum_fluo", 12. },
    { "channelmixerrgb", "illum_led", 9. },
    { "channelmixerrgb", "adaptation", 5. },
    { "colorin", "intent", 4. },
    { "colorout", "intent", 4. },
    { "toneequal", "method", 7. },
    { "enlargecanvas", "color", 5. },
    { "rawprepare", "raw_white_point", 0. },
    { "basecurve", "exposure_fusion", 3. },
    { "lens", "target_geom", 0. },
    { "retouch", "algorithm", 0. },
  };
  EXT_OK(dt_mobile_modules(session, &baseline));
  EXT_OK(dt_mobile_history(session, &history));
  for(size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++)
  {
    fprintf(stderr, "extended controls: reject %s.%s\n", invalid[index].operation, invalid[index].name);
    EXT_CHECK(dt_mobile_set_parameter(session, invalid[index].operation, invalid[index].name, invalid[index].value)
              == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  }
  EXT_OK(dt_mobile_modules(session, &changed));
  EXT_CHECK(!strcmp(baseline, changed));
  EXT_OK(dt_mobile_history(session, &after_history));
  EXT_CHECK(!strcmp(history, after_history));
  fprintf(stderr, "extended controls: complete\n");
  success = 1;
finished:
  for(guint index = 0; index < controls->len; index++)
  {
    mobile_advertised_control *control = &g_array_index(controls, mobile_advertised_control, index);
    g_free(control->operation); g_free(control->name);
  }
  g_array_free(controls, TRUE);
  g_object_unref(parser);
  dt_mobile_free_buffer(baseline); dt_mobile_free_buffer(changed);
  dt_mobile_free_buffer(history); dt_mobile_free_buffer(after_history);
  dt_mobile_free_buffer(before); dt_mobile_free_buffer(after); dt_mobile_free_buffer(restored);
  return success;
#undef EXT_CHECK
#undef EXT_OK
}
#endif
