/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_COLOR_CONTROLS_CHECKS_H
#define DT_MOBILE_COLOR_CONTROLS_CHECKS_H

/* Included after the engine fixture's exact module/parameter JSON helpers. */
static int mobile_color_number(const char *json, const char *operation,
                               const char *parameter, const char *name, double *number)
{
  char key[128];
  snprintf(key, sizeof(key), "{\"operation\":\"%s\",", operation);
  const char *module = strstr(json, key);
  if(!module) return 0;
  const char *module_end = strstr(module + 1, "{\"operation\":");
  snprintf(key, sizeof(key), "{\"name\":\"%s\",", parameter);
  const char *field = strstr(module, key);
  if(!field || (module_end && field >= module_end)) return 0;
  const char *field_end = strstr(field + 1, "{\"name\":");
  snprintf(key, sizeof(key), ",\"%s\":", name);
  const char *value = strstr(field, key);
  if(!value || (field_end && value >= field_end) || (module_end && value >= module_end)) return 0;
  char *end;
  *number = strtod(value + strlen(key), &end);
  return end != value + strlen(key) && isfinite(*number);
}

static int mobile_color_controls_checks(dt_mobile_session *session)
{
  static const struct {
    const char *operation, *parameter;
    double minimum, maximum, edited;
  } controls[] = {
    { "colisa", "contrast", -1.0, 1.0, 0.35 },
    { "colisa", "brightness", -1.0, 1.0, 0.2 },
    { "colisa", "saturation", -1.0, 1.0, -0.75 },
    { "colorize", "hue", 0.0, 1.0, 0.65 },
    { "colorize", "saturation", 0.0, 1.0, 0.8 },
    { "colorize", "source_lightness_mix", 0.0, 100.0, 80.0 },
    { "colorize", "lightness", 0.0, 100.0, 65.0 },
  };
  char *baseline = NULL, *changed = NULL, *history = NULL, *history_after = NULL;
  uint8_t *before = NULL, *after = NULL, *restored = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0, success = 0;
#define COLOR_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "color controls:%d: check failed: %s\n", __LINE__, #condition); goto finished; } } while(0)
#define COLOR_OK(call) do { if((call) != DT_MOBILE_OK) { \
  fprintf(stderr, "color controls: %s: %s\n", #call, dt_mobile_last_error(session)); goto finished; } } while(0)
  /* Keep first-preview behavior separate from this edit regression. The
   * original PNG fixture's first unedited pass can differ from subsequent
   * passes; a work-only ten-frame probe records that pre-existing anomaly.
   * After this explicit readiness render, require two byte-identical baseline
   * frames and exact edit undo/redo below. This is not a tolerance comparison. */
  COLOR_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  COLOR_CHECK(restored && other_width > 0 && other_height > 0);
  dt_mobile_free_buffer(restored); restored = NULL;
  for(size_t index = 0; index < sizeof(controls) / sizeof(controls[0]); index++)
  {
    fprintf(stderr, "color controls: exercising %s.%s\n", controls[index].operation, controls[index].parameter);
    COLOR_OK(dt_mobile_modules(session, &baseline));
    double number;
    COLOR_CHECK(mobile_color_number(baseline, controls[index].operation, controls[index].parameter, "min", &number)
                && number == controls[index].minimum);
    COLOR_CHECK(mobile_color_number(baseline, controls[index].operation, controls[index].parameter, "max", &number)
                && number == controls[index].maximum);
    COLOR_CHECK(!module_has(baseline, "colorize", "\"name\":\"version\""));
    COLOR_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
    COLOR_CHECK(before && width > 0 && height > 0);
    COLOR_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
    COLOR_CHECK(restored && width == other_width && height == other_height
                && !memcmp(before, restored, (size_t)width * height * 4));
    dt_mobile_free_buffer(restored); restored = NULL;
    COLOR_OK(dt_mobile_history(session, &history));
    const double invalid[] = { NAN, INFINITY, -INFINITY, controls[index].minimum - 1.0, controls[index].maximum + 1.0 };
    for(size_t k = 0; k < sizeof(invalid) / sizeof(invalid[0]); k++)
      COLOR_CHECK(dt_mobile_set_parameter(session, controls[index].operation, controls[index].parameter, invalid[k])
                  == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    COLOR_CHECK(dt_mobile_set_parameter(session, "colorize", "version", 2.0) == DT_MOBILE_ERROR_UNSUPPORTED);
    COLOR_OK(dt_mobile_history(session, &history_after));
    COLOR_CHECK(!strcmp(history, history_after));
    COLOR_OK(dt_mobile_modules(session, &changed));
    COLOR_CHECK(!strcmp(baseline, changed));
    dt_mobile_free_buffer(history); history = NULL;
    dt_mobile_free_buffer(history_after); history_after = NULL;
    dt_mobile_free_buffer(changed); changed = NULL;

    /* Exercise both original declared endpoints through the actual pixelpipe,
     * then restore the starting settings before checking an interior edit. */
    const double endpoints[] = { controls[index].minimum, controls[index].maximum };
    for(size_t endpoint = 0; endpoint < 2; endpoint++)
    {
      COLOR_OK(dt_mobile_set_parameter(session, controls[index].operation, controls[index].parameter, endpoints[endpoint]));
      COLOR_OK(dt_mobile_modules(session, &changed));
      COLOR_CHECK(parameter_equals(changed, controls[index].operation, controls[index].parameter, endpoints[endpoint]));
      COLOR_CHECK(module_has(changed, controls[index].operation, "\"enabled\":true"));
      COLOR_OK(dt_mobile_render_preview(session, 96, 96, &after, &other_width, &other_height));
      COLOR_CHECK(after && width == other_width && height == other_height);
      dt_mobile_free_buffer(after); after = NULL;
      dt_mobile_free_buffer(changed); changed = NULL;
      COLOR_OK(dt_mobile_undo(session));
    }
    COLOR_OK(dt_mobile_set_parameter(session, controls[index].operation, controls[index].parameter, controls[index].edited));
    COLOR_OK(dt_mobile_modules(session, &changed));
    for(size_t other = 0; other < sizeof(controls) / sizeof(controls[0]); other++)
    {
      double expected;
      COLOR_CHECK(mobile_color_number(baseline, controls[other].operation, controls[other].parameter, "value", &expected));
      if(other == index) expected = (double)(float)controls[index].edited;
      COLOR_CHECK(parameter_equals(changed, controls[other].operation, controls[other].parameter, expected));
    }
    COLOR_OK(dt_mobile_render_preview(session, 96, 96, &after, &other_width, &other_height));
    COLOR_CHECK(after && width == other_width && height == other_height);
    COLOR_CHECK(memcmp(before, after, (size_t)width * height * 4));
    COLOR_OK(dt_mobile_undo(session));
    dt_mobile_free_buffer(changed); changed = NULL;
    COLOR_OK(dt_mobile_modules(session, &changed));
    COLOR_CHECK(!strcmp(baseline, changed));
    COLOR_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
    if(restored && width == other_width && height == other_height)
      for(size_t k = 0; k < (size_t)width * height * 4; k++)
        if(before[k] != restored[k]) { fprintf(stderr, "color controls: undo first diff byte%zu %u -> %u\n", k, before[k], restored[k]); break; }
    COLOR_CHECK(restored && width == other_width && height == other_height
                && !memcmp(before, restored, (size_t)width * height * 4));
    dt_mobile_free_buffer(restored); restored = NULL;
    COLOR_OK(dt_mobile_redo(session));
    COLOR_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
    COLOR_CHECK(restored && width == other_width && height == other_height
                && !memcmp(after, restored, (size_t)width * height * 4));
    COLOR_OK(dt_mobile_undo(session));
    dt_mobile_free_buffer(changed); changed = NULL;
    COLOR_OK(dt_mobile_modules(session, &changed));
    COLOR_CHECK(!strcmp(baseline, changed));
    dt_mobile_free_buffer(changed); changed = NULL;
    dt_mobile_free_buffer(baseline); baseline = NULL;
    dt_mobile_free_buffer(before); before = NULL;
    dt_mobile_free_buffer(after); after = NULL;
    dt_mobile_free_buffer(restored); restored = NULL;
  }
  success = 1;
finished:
  dt_mobile_free_buffer(baseline);
  dt_mobile_free_buffer(changed);
  dt_mobile_free_buffer(history);
  dt_mobile_free_buffer(history_after);
  dt_mobile_free_buffer(before);
  dt_mobile_free_buffer(after);
  dt_mobile_free_buffer(restored);
  return success;
#undef COLOR_OK
#undef COLOR_CHECK
}
#endif
