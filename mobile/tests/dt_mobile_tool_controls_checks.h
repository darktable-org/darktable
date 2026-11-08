/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_TOOL_CONTROLS_CHECKS_H
#define DT_MOBILE_TOOL_CONTROLS_CHECKS_H

#include <float.h>

/* Included after the engine fixture's public metadata helpers. The controls
 * below exercise original modules, not a second implementation of their maths.
 * Explicit expected bounds also catch accidentally exposing upstream sentinel,
 * structural or unbounded parameter domains as ordinary mobile controls. */
static int mobile_tool_parameter_type(const char *json, const char *operation,
                                      const char *parameter, const char *type,
                                      unsigned expected_choices)
{
  JsonParser *parser = json_parser_new();
  int valid = 0;
  if(!json_parser_load_from_data(parser, json, -1, NULL)) goto finished;
  JsonNode *root = json_parser_get_root(parser);
  if(!JSON_NODE_HOLDS_ARRAY(root)) goto finished;
  JsonArray *modules = json_node_get_array(root);
  for(guint i = 0; i < json_array_get_length(modules); i++)
  {
    JsonObject *module = json_array_get_object_element(modules, i);
    if(strcmp(json_object_get_string_member(module, "operation"), operation)) continue;
    JsonArray *parameters = json_object_get_array_member(module, "parameters");
    for(guint j = 0; j < json_array_get_length(parameters); j++)
    {
      JsonObject *field = json_array_get_object_element(parameters, j);
      if(strcmp(json_object_get_string_member(field, "name"), parameter)) continue;
      if(strcmp(json_object_get_string_member(field, "type"), type)) goto finished;
      if(!json_object_has_member(field, "default")) goto finished;
      const double original = json_object_get_double_member(field, "default");
      const double minimum = json_object_get_double_member(field, "min");
      const double maximum = json_object_get_double_member(field, "max");
      if(!isfinite(original) || original < minimum || original > maximum
         || (strcmp(type, "float") && trunc(original) != original)) goto finished;
      if(json_object_has_member(field, "sliderMin") || json_object_has_member(field, "sliderMax"))
      {
        if(!json_object_has_member(field, "sliderMin") || !json_object_has_member(field, "sliderMax")) goto finished;
        const double low = json_object_get_double_member(field, "sliderMin");
        const double high = json_object_get_double_member(field, "sliderMax");
        if(!isfinite(low) || !isfinite(high) || low < minimum || high > maximum || low >= high) goto finished;
      }
      if(expected_choices)
      {
        if(!json_object_has_member(field, "choices")) goto finished;
        JsonArray *choices = json_object_get_array_member(field, "choices");
        if(json_array_get_length(choices) != expected_choices) goto finished;
        for(guint k = 0; k < expected_choices; k++)
        {
          JsonObject *choice = json_array_get_object_element(choices, k);
          if(json_object_get_double_member(choice, "value") != k
             || !json_object_get_string_member(choice, "label")[0]) goto finished;
        }
      }
      valid = 1;
      goto finished;
    }
  }
finished:
  g_object_unref(parser);
  return valid;
}

static int mobile_tool_controls_checks(dt_mobile_session *session)
{
  static const struct {
    const char *operation, *parameter, *type;
    double minimum, maximum, edited;
    unsigned choices;
    int pixel_roundtrip;
  } controls[] = {
    { "tonemap", "contrast", "float", 1, 5, 1.5, 0, 1 },
    { "tonemap", "Fsize", "float", 0, 100, 65, 0, 0 },
    { "highpass", "sharpness", "float", 0, 100, 75, 0, 0 },
    { "highpass", "contrast", "float", 0, 100, 65, 0, 1 },
    { "soften", "size", "float", 0, 100, 65, 0, 0 },
    { "soften", "saturation", "float", 0, 100, 40, 0, 0 },
    { "soften", "brightness", "float", -2, 2, 1, 0, 1 },
    { "soften", "amount", "float", 0, 100, 75, 0, 0 },
    { "lowpass", "radius", "float", .1, 500, 20, 0, 0 },
    { "lowpass", "contrast", "float", -3, 3, .5, 0, 0 },
    { "lowpass", "brightness", "float", -3, 3, 1, 0, 1 },
    { "lowpass", "saturation", "float", -3, 3, .5, 0, 0 },
    { "lowpass", "lowpass_algo", "enum", 0, 1, 1, 2, 0 },
    { "graduatednd", "density", "float", -8, 8, 3, 0, 1 },
    { "graduatednd", "hardness", "float", 0, 100, 75, 0, 0 },
    { "graduatednd", "rotation", "float", -180, 180, 45, 0, 0 },
    { "graduatednd", "offset", "float", 0, 100, 25, 0, 0 },
    { "graduatednd", "hue", "float", 0, 1, .6, 0, 0 },
    { "graduatednd", "saturation", "float", 0, 1, .75, 0, 0 },
    { "monochrome", "a", "float", -128, 128, 40, 0, 0 },
    { "monochrome", "b", "float", -128, 128, -30, 0, 0 },
    { "monochrome", "size", "float", .5, 3, 1.5, 0, 0 },
    { "monochrome", "highlights", "float", 0, 1, .6, 0, 1 },
    { "vignette", "scale", "float", 0, 200, 50, 0, 0 },
    { "vignette", "falloff_scale", "float", 0, 200, 75, 0, 0 },
    { "vignette", "brightness", "float", -1, 1, -.8, 0, 1 },
    { "vignette", "saturation", "float", -1, 1, .25, 0, 0 },
    { "vignette", "center.x", "float", -1, 1, .25, 0, 0 },
    { "vignette", "center.y", "float", -1, 1, -.25, 0, 0 },
    { "vignette", "autoratio", "bool", 0, 1, 1, 0, 0 },
    { "vignette", "whratio", "float", .001, 1.999, .8, 0, 0 },
    { "vignette", "shape", "float", 0, 5, 2, 0, 0 },
    { "vignette", "dithering", "enum", 0, 2, 2, 3, 0 },
    { "splittoning", "shadow_hue", "float", 0, 1, .6, 0, 1 },
    { "splittoning", "shadow_saturation", "float", 0, 1, .8, 0, 0 },
    { "splittoning", "highlight_hue", "float", 0, 1, .8, 0, 0 },
    { "splittoning", "highlight_saturation", "float", 0, 1, .8, 0, 0 },
    { "splittoning", "balance", "float", 0, 1, .3, 0, 0 },
    { "splittoning", "compress", "float", 0, 100, 65, 0, 0 },
    { "bloom", "size", "float", 0, 100, 65, 0, 0 },
    { "bloom", "threshold", "float", 0, 100, 20, 0, 1 },
    { "bloom", "strength", "float", 0, 100, 65, 0, 0 },
    { "grain", "scale", "float", 20.0 / 213.2, 6400.0 / 213.2, 10, 0, 0 },
    { "grain", "strength", "float", 0, 100, 65, 0, 0 },
    { "grain", "midtones_bias", "float", 0, 100, 65, 0, 0 },
    { "colorcorrection", "saturation", "float", -3, 3, .5, 0, 1 },
    { "colorbalance", "mode", "enum", 0, 2, 0, 3, 0 },
    { "colorbalance", "saturation", "float", 0, 2, .5, 0, 1 },
    { "colorbalance", "saturation_out", "float", 0, 2, .5, 0, 0 },
    { "colorbalance", "contrast", "float", .01, 1.99, 1.4, 0, 0 },
    { "colorbalance", "grey", "float", .1, 100, 25, 0, 0 },
    { "globaltonemap", "operator", "enum", 0, 2, 1, 3, 1 },
    { "globaltonemap", "detail", "float", -1, 1, .5, 0, 0 },
    { "globaltonemap", "drago.bias", "float", .5, 1, .6, 0, 0 },
    { "globaltonemap", "drago.max_light", "float", 1, 500, 200, 0, 0 },
    { "colorbalancergb", "global_Y", "float", -1, 1, .2, 0, 1 },
    { "colorbalancergb", "global_C", "float", 0, 1, .2, 0, 0 },
    { "colorbalancergb", "global_H", "float", 0, 360, 90, 0, 0 },
    { "colorbalancergb", "shadows_Y", "float", -1, 1, .2, 0, 0 },
    { "colorbalancergb", "shadows_C", "float", 0, 1, .2, 0, 0 },
    { "colorbalancergb", "shadows_H", "float", 0, 360, 90, 0, 0 },
    { "colorbalancergb", "highlights_Y", "float", -1, 1, .2, 0, 0 },
    { "colorbalancergb", "highlights_C", "float", 0, 1, .2, 0, 0 },
    { "colorbalancergb", "highlights_H", "float", 0, 360, 90, 0, 0 },
    { "colorbalancergb", "shadows_weight", "float", 0, 3, 1.5, 0, 0 },
    { "colorbalancergb", "highlights_weight", "float", 0, 3, 1.5, 0, 0 },
    { "colorbalancergb", "white_fulcrum", "float", -16, 16, 2, 0, 0 },
    { "colorbalancergb", "chroma_shadows", "float", -1, 1, .2, 0, 0 },
    { "colorbalancergb", "chroma_midtones", "float", -1, 1, .2, 0, 0 },
    { "colorbalancergb", "chroma_highlights", "float", -1, 1, .2, 0, 0 },
    { "colorbalancergb", "saturation_shadows", "float", -1, 1, .2, 0, 0 },
    { "colorbalancergb", "saturation_midtones", "float", -1, 1, .2, 0, 0 },
    { "colorbalancergb", "saturation_highlights", "float", -1, 1, .2, 0, 0 },
    { "colorbalancergb", "brilliance_shadows", "float", -1, 1, .2, 0, 0 },
    { "colorbalancergb", "brilliance_midtones", "float", -1, 1, .2, 0, 0 },
    { "colorbalancergb", "brilliance_highlights", "float", -1, 1, .2, 0, 0 },
    { "colorbalancergb", "saturation_formula", "enum", 0, 1, 1, 2, 0 },
  };
  char *baseline = NULL, *changed = NULL, *history = NULL, *history_after = NULL, *instance = NULL;
  uint8_t *before = NULL, *after = NULL, *restored = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0, success = 0;
#define TOOL_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "tool controls:%d: check failed: %s\n", __LINE__, #condition); goto finished; } } while(0)
#define TOOL_OK(call) do { if((call) != DT_MOBILE_OK) { \
  fprintf(stderr, "tool controls: %s: %s\n", #call, dt_mobile_last_error(session)); goto finished; } } while(0)
  /* Retain the separately documented first-PNG-preview observation: warm the
   * unchanged pipeline explicitly, then require exact stable baseline bytes. */
  TOOL_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  TOOL_CHECK(restored && other_width > 0 && other_height > 0);
  dt_mobile_free_buffer(restored); restored = NULL;
  for(size_t index = 0; index < sizeof(controls) / sizeof(controls[0]); index++)
  {
    fprintf(stderr, "tool controls: exercising %s.%s\n", controls[index].operation, controls[index].parameter);
    const int is_float = !strcmp(controls[index].type, "float");
    const double minimum = is_float ? (double)(float)controls[index].minimum : controls[index].minimum;
    const double maximum = is_float ? (double)(float)controls[index].maximum : controls[index].maximum;
    const double edited = is_float ? (double)(float)controls[index].edited : controls[index].edited;
    TOOL_OK(dt_mobile_modules(session, &baseline));
    double number, original_default;
    TOOL_CHECK(mobile_color_number(baseline, controls[index].operation, controls[index].parameter, "min", &number)
               && number == minimum);
    TOOL_CHECK(mobile_color_number(baseline, controls[index].operation, controls[index].parameter, "max", &number)
               && number == maximum);
    TOOL_CHECK(mobile_tool_parameter_type(baseline, controls[index].operation, controls[index].parameter,
                                         controls[index].type, controls[index].choices));
    TOOL_CHECK(mobile_color_number(baseline, controls[index].operation, controls[index].parameter,
                                   "default", &original_default));
    TOOL_CHECK(parameter_equals(baseline, controls[index].operation, controls[index].parameter, original_default));
    TOOL_OK(dt_mobile_history(session, &history));
    const double invalid[] = { NAN, INFINITY, -INFINITY, minimum - 1, maximum + 1 };
    for(size_t k = 0; k < sizeof(invalid) / sizeof(invalid[0]); k++)
      TOOL_CHECK(dt_mobile_set_parameter(session, controls[index].operation, controls[index].parameter, invalid[k])
                  == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    if(is_float)
    {
      /* Adjacent normal floats also test narrowed domains inside an upstream
       * field's broader storage range. The original engine enables subnormal
       * flush-to-zero, so use a normal out-of-range value around a zero bound. */
      float below = nextafterf((float)minimum, -INFINITY);
      float above = nextafterf((float)maximum, INFINITY);
      if(!isnormal(below)) below = -FLT_MIN;
      if(!isnormal(above)) above = FLT_MIN;
      TOOL_CHECK(dt_mobile_set_parameter(session, controls[index].operation, controls[index].parameter,
                                         below)
                  == DT_MOBILE_ERROR_INVALID_ARGUMENT);
      TOOL_CHECK(dt_mobile_set_parameter(session, controls[index].operation, controls[index].parameter,
                                         above)
                  == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    }
    if(!is_float)
      TOOL_CHECK(dt_mobile_set_parameter(session, controls[index].operation, controls[index].parameter, .5)
                  == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    TOOL_OK(dt_mobile_modules(session, &changed));
    TOOL_CHECK(!strcmp(baseline, changed));
    TOOL_OK(dt_mobile_history(session, &history_after));
    TOOL_CHECK(!strcmp(history, history_after));
    dt_mobile_free_buffer(history); history = NULL;
    dt_mobile_free_buffer(history_after); history_after = NULL;
    dt_mobile_free_buffer(changed); changed = NULL;

    /* Every control traverses the setter, independent metadata and undo path.
     * Render endpoints for all first seven tools (including Gaussian radii,
     * nested centers, shape/ratio limits and enums), and for a representative
     * control in each remaining deterministic module. Rebuilding the same
     * large grading LUT for every scalar/default would repeat one pixelpipe
     * check and make the shared fixture unsuitable for Debug CI runs. */
    const double values[] = { minimum, maximum, edited, original_default };
    for(size_t value = 0; value < sizeof(values) / sizeof(values[0]); value++)
    {
      TOOL_OK(dt_mobile_set_parameter(session, controls[index].operation, controls[index].parameter, values[value]));
      TOOL_OK(dt_mobile_modules(session, &changed));
      TOOL_CHECK(parameter_equals(changed, controls[index].operation, controls[index].parameter, values[value]));
      TOOL_CHECK(module_has(changed, controls[index].operation, "\"enabled\":true"));
      for(size_t other = 0; other < sizeof(controls) / sizeof(controls[0]); other++)
      {
        if(other == index) continue;
        double expected;
        TOOL_CHECK(mobile_color_number(baseline, controls[other].operation, controls[other].parameter, "value", &expected));
        TOOL_CHECK(parameter_equals(changed, controls[other].operation, controls[other].parameter, expected));
      }
      if(value < 2 && (index < 33 || controls[index].pixel_roundtrip))
      {
        TOOL_OK(dt_mobile_render_preview(session, 96, 96, &after, &other_width, &other_height));
        TOOL_CHECK(after && other_width > 0 && other_height > 0);
        dt_mobile_free_buffer(after); after = NULL;
      }
      dt_mobile_free_buffer(changed); changed = NULL;
      TOOL_OK(dt_mobile_undo(session));
      TOOL_OK(dt_mobile_modules(session, &changed));
      TOOL_CHECK(!strcmp(baseline, changed));
      dt_mobile_free_buffer(changed); changed = NULL;
    }
    if(controls[index].pixel_roundtrip)
    {
      TOOL_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
      TOOL_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
      TOOL_CHECK(before && restored && width == other_width && height == other_height
                  && !memcmp(before, restored, (size_t)width * height * 4));
      dt_mobile_free_buffer(restored); restored = NULL;
      TOOL_OK(dt_mobile_set_parameter(session, controls[index].operation, controls[index].parameter, edited));
      TOOL_OK(dt_mobile_render_preview(session, 96, 96, &after, &other_width, &other_height));
      TOOL_CHECK(after && width == other_width && height == other_height
                  && memcmp(before, after, (size_t)width * height * 4));
      TOOL_OK(dt_mobile_undo(session));
      TOOL_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
      TOOL_CHECK(restored && width == other_width && height == other_height
                  && !memcmp(before, restored, (size_t)width * height * 4));
      dt_mobile_free_buffer(restored); restored = NULL;
      TOOL_OK(dt_mobile_redo(session));
      TOOL_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
      TOOL_CHECK(restored && width == other_width && height == other_height
                  && !memcmp(after, restored, (size_t)width * height * 4));
      TOOL_OK(dt_mobile_undo(session));
      dt_mobile_free_buffer(before); before = NULL;
      dt_mobile_free_buffer(after); after = NULL;
      dt_mobile_free_buffer(restored); restored = NULL;
    }
    dt_mobile_free_buffer(baseline); baseline = NULL;
  }
  /* Original modules also contain unused flags, array controls and singular
   * coefficients. Opening more desktop controls must not grant writes to them. */
  static const struct { const char *operation, *parameter; double value; } private_fields[] = {
    { "lowpass", "order", 1 }, { "lowpass", "unbound", 0 },
    { "vignette", "unbound", 0 }, { "grain", "channel", 0 },
    { "colorbalancergb", "midtones_C", 1 }, { "colorbalancergb", "midtones_H", 90 },
  };
  TOOL_OK(dt_mobile_modules(session, &baseline));
  TOOL_OK(dt_mobile_history(session, &history));
  for(size_t k = 0; k < sizeof(private_fields) / sizeof(private_fields[0]); k++)
  {
    char fragment[128];
    snprintf(fragment, sizeof(fragment), "\"name\":\"%s\",", private_fields[k].parameter);
    TOOL_CHECK(!module_has(baseline, private_fields[k].operation, fragment));
    TOOL_CHECK(dt_mobile_set_parameter(session, private_fields[k].operation,
                                       private_fields[k].parameter, private_fields[k].value)
                == DT_MOBILE_ERROR_UNSUPPORTED);
  }
  /* These controls are now ported, with narrowed domains excluding their
   * original reciprocal/zero-division singularities. They remain advertised;
   * invalid values must fail before changing parameters or history. */
  static const struct { const char *parameter; double value; } singular_values[] = {
    { "midtones_Y", -1 }, { "mask_grey_fulcrum", 0 }, { "grey_fulcrum", 0 },
  };
  for(size_t k = 0; k < sizeof(singular_values) / sizeof(singular_values[0]); k++)
  {
    char fragment[128];
    snprintf(fragment, sizeof(fragment), "\"name\":\"%s\",", singular_values[k].parameter);
    TOOL_CHECK(module_has(baseline, "colorbalancergb", fragment));
    TOOL_CHECK(dt_mobile_set_parameter(session, "colorbalancergb", singular_values[k].parameter,
                                       singular_values[k].value) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  }
  TOOL_OK(dt_mobile_modules(session, &changed));
  TOOL_CHECK(!strcmp(baseline, changed));
  TOOL_OK(dt_mobile_history(session, &history_after));
  TOOL_CHECK(!strcmp(history, history_after));
  dt_mobile_free_buffer(changed); changed = NULL;
  dt_mobile_free_buffer(history); history = NULL;
  dt_mobile_free_buffer(history_after); history_after = NULL;

  /* The screenshot includes a second tone-mapping instance: its controls must
   * target that exact native instance while leaving the first instance intact. */
  double base_contrast;
  TOOL_CHECK(mobile_color_number(baseline, "tonemap", "contrast", "value", &base_contrast));
  TOOL_OK(dt_mobile_new_instance(session, "tonemap", &instance));
  TOOL_CHECK(instance && strncmp(instance, "tonemap@", strlen("tonemap@")) == 0);
  TOOL_OK(dt_mobile_set_parameter(session, instance, "contrast", 1.75));
  TOOL_OK(dt_mobile_modules(session, &changed));
  TOOL_CHECK(parameter_equals(changed, instance, "contrast", 1.75));
  TOOL_CHECK(parameter_equals(changed, "tonemap", "contrast", base_contrast));
  dt_mobile_free_buffer(changed); changed = NULL;
  TOOL_OK(dt_mobile_undo(session));
  TOOL_OK(dt_mobile_undo(session));
  TOOL_OK(dt_mobile_modules(session, &changed));
  TOOL_CHECK(!strcmp(baseline, changed));
  dt_mobile_free_buffer(changed); changed = NULL;

  /* Reset is an ordinary original scalar write and must obey coupled desktop
   * invariants too. A legal black=1/exposure=-1 state cannot reset exposure to
   * its original zero until black is lowered; metadata must hide that action. */
  TOOL_OK(dt_mobile_set_parameter(session, "exposure", "exposure", -1));
  TOOL_OK(dt_mobile_set_parameter(session, "exposure", "black", 1));
  TOOL_OK(dt_mobile_history(session, &history));
  TOOL_OK(dt_mobile_modules(session, &changed));
  double exposure_default;
  TOOL_CHECK(!mobile_color_number(changed, "exposure", "exposure", "default", &exposure_default));
  TOOL_CHECK(parameter_equals(changed, "exposure", "exposure", -1));
  TOOL_CHECK(parameter_equals(changed, "exposure", "black", 1));
  TOOL_OK(dt_mobile_history(session, &history_after));
  TOOL_CHECK(!strcmp(history, history_after));
  dt_mobile_free_buffer(changed); changed = NULL;
  dt_mobile_free_buffer(history); history = NULL;
  dt_mobile_free_buffer(history_after); history_after = NULL;
  TOOL_OK(dt_mobile_set_parameter(session, "exposure", "black", 0));
  TOOL_OK(dt_mobile_modules(session, &changed));
  TOOL_CHECK(mobile_color_number(changed, "exposure", "exposure", "default", &exposure_default)
               && exposure_default == 0);
  dt_mobile_free_buffer(changed); changed = NULL;
  TOOL_OK(dt_mobile_undo(session));
  TOOL_OK(dt_mobile_undo(session));
  TOOL_OK(dt_mobile_undo(session));
  TOOL_OK(dt_mobile_modules(session, &changed));
  TOOL_CHECK(!strcmp(baseline, changed));
  success = 1;
finished:
  dt_mobile_free_buffer(baseline);
  dt_mobile_free_buffer(changed);
  dt_mobile_free_buffer(history);
  dt_mobile_free_buffer(history_after);
  dt_mobile_free_buffer(instance);
  dt_mobile_free_buffer(before);
  dt_mobile_free_buffer(after);
  dt_mobile_free_buffer(restored);
  return success;
#undef TOOL_OK
#undef TOOL_CHECK
}
#endif
