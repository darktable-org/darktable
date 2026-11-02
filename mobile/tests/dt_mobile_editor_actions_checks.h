/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_EDITOR_ACTIONS_CHECKS_H
#define DT_MOBILE_EDITOR_ACTIONS_CHECKS_H

#include "dt_mobile.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include <gmodule.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*editor_actions_get_function)(dt_iop_module_t *, const char *, double *, double *, double *, double *, int *);
typedef int (*editor_actions_set_function)(dt_iop_module_t *, const char *, double);

static int editor_actions_number(const char *json, const char *operation,
                                 const char *parameter, const char *member, double *number)
{
  char key[160];
  snprintf(key, sizeof(key), "{\"operation\":\"%s\",", operation);
  const char *module = strstr(json, key);
  if(!module) return 0;
  const char *module_end = strstr(module + 1, "{\"operation\":");
  snprintf(key, sizeof(key), "{\"name\":\"%s\",", parameter);
  const char *field = strstr(module, key);
  if(!field || (module_end && field >= module_end)) return 0;
  const char *field_end = strstr(field + 1, "{\"name\":");
  snprintf(key, sizeof(key), ",\"%s\":", member);
  const char *value = strstr(field, key);
  if(!value || (field_end && value >= field_end) || (module_end && value >= module_end)) return 0;
  char *end;
  *number = strtod(value + strlen(key), &end);
  return end != value + strlen(key) && isfinite(*number);
}

static long editor_actions_position(const char *history)
{
  const char *position = strstr(history, "\"position\":");
  return position ? strtol(position + strlen("\"position\":"), NULL, 10) : -1;
}

static int editor_actions_action_metadata(const char *json, const char *operation, const char *name)
{
  char key[160];
  snprintf(key, sizeof(key), "{\"operation\":\"%s\",", operation);
  const char *module = strstr(json, key);
  if(!module) return 0;
  const char *module_end = strstr(module + 1, "{\"operation\":");
  snprintf(key, sizeof(key), "{\"name\":\"%s\",", name);
  const char *field = strstr(module, key);
  if(!field || (module_end && field >= module_end)) return 0;
  const char *duplicate = strstr(field + 1, key);
  if(duplicate && (!module_end || duplicate < module_end)) return 0;
  const char *field_end = strstr(field + 1, "{\"name\":");
  const char *type = strstr(field, "\"type\":\"action\"");
  return type && (!field_end || type < field_end) && (!module_end || type < module_end);
}

static int editor_actions_parameter_count(const char *json, const char *operation)
{
  char key[160];
  snprintf(key, sizeof(key), "{\"operation\":\"%s\",", operation);
  const char *module = strstr(json, key);
  if(!module) return -1;
  const char *module_end = strstr(module + 1, "{\"operation\":");
  int count = 0;
  for(const char *field = strstr(module, "{\"name\":"); field && (!module_end || field < module_end);
      field = strstr(field + 1, "{\"name\":")) count++;
  return count;
}

static float *editor_actions_float(dt_iop_module_t *module, const char *name)
{
  const dt_introspection_field_t *field = module->so->get_f(name);
  return field && field->header.type == DT_INTROSPECTION_TYPE_FLOAT
    ? module->so->get_p(module->params, name) : NULL;
}

static int *editor_actions_integer(dt_iop_module_t *module, const char *name)
{
  const dt_introspection_field_t *field = module->so->get_f(name);
  return field && (field->header.type == DT_INTROSPECTION_TYPE_BOOL
                  || field->header.type == DT_INTROSPECTION_TYPE_ENUM)
    ? module->so->get_p(module->params, name) : NULL;
}

/* Exercise exports from the installed original plugins with introspected
 * original parameter storage. No copied parameter structs or guessed offsets. */
static int editor_actions_native_checks(void)
{
  int success = 0;
  dt_develop_t develop = {0};
  dt_iop_module_t agx = {0}, filmic = {0}, exposure = {0}, other_exposure = {0};
  dt_develop_blend_params_t exposure_blend = {0}, other_exposure_blend = {0};
  void *before = NULL, *defaults = NULL;
  editor_actions_get_function get_agx = NULL, get_filmic = NULL;
  editor_actions_set_function set_agx = NULL, set_filmic = NULL;
#define EDITOR_NATIVE_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "editor actions native:%d: check failed: %s\n", __LINE__, #condition); goto finished; } } while(0)
#define EDITOR_NATIVE_FLOAT(module, name, value) do { \
  float *field = editor_actions_float(&(module), (name)); EDITOR_NATIVE_CHECK(field); *field = (value); \
} while(0)
#define EDITOR_NATIVE_VALUE(module, name) (*editor_actions_float(&(module), (name)))
#define EDITOR_NATIVE_INT(module, name) (*editor_actions_integer(&(module), (name)))
  agx.so = dt_iop_get_module_so("agx");
  filmic.so = dt_iop_get_module_so("filmicrgb");
  EDITOR_NATIVE_CHECK(agx.so && filmic.so && agx.so->module && filmic.so->module
    && agx.so->get_introspection_linear && filmic.so->get_introspection_linear
    && agx.so->get_p && filmic.so->get_p && agx.so->get_f && filmic.so->get_f);
  agx.module = agx.so->module; filmic.module = filmic.so->module;
  agx.dev = filmic.dev = &develop;
  agx.get_f = agx.so->get_f; agx.get_p = agx.so->get_p;
  filmic.get_f = filmic.so->get_f; filmic.get_p = filmic.so->get_p;
  g_strlcpy(agx.op, "agx", sizeof(agx.op));
  g_strlcpy(filmic.op, "filmicrgb", sizeof(filmic.op));
  dt_iop_default_init(&agx); dt_iop_default_init(&filmic);
  EDITOR_NATIVE_CHECK(agx.params && agx.default_params && filmic.params && filmic.default_params);
  memcpy(agx.params, agx.default_params, agx.params_size);
  memcpy(filmic.params, filmic.default_params, filmic.params_size);
  EDITOR_NATIVE_CHECK(g_module_symbol(agx.module, "dt_iop_mobile_editor_get_control", (gpointer *)&get_agx)
    && g_module_symbol(agx.module, "dt_iop_mobile_editor_set_number", (gpointer *)&set_agx)
    && g_module_symbol(filmic.module, "dt_iop_mobile_editor_get_control", (gpointer *)&get_filmic)
    && g_module_symbol(filmic.module, "dt_iop_mobile_editor_set_number", (gpointer *)&set_filmic));

  fprintf(stderr, "editor actions: original AgX primaries helpers and pivot coupling\n");
  const char *actions[] = { "agx.primaries_blender", "agx.primaries_smooth",
    "agx.primaries_unmodified", "agx.copy_pre_to_post" };
  for(size_t k = 0; k < sizeof(actions) / sizeof(actions[0]); k++)
  {
    double value = 0, minimum = 0, maximum = 0, original = 0;
    int kind = -1;
    EDITOR_NATIVE_CHECK(get_agx(&agx, actions[k], &value, &minimum, &maximum, &original, &kind) == 1
      && kind == 3 && value == 1 && minimum == 1 && maximum == 1);
  }
  double value = 0, minimum = 0, maximum = 0, original = 0;
  int kind = -1;
  EDITOR_NATIVE_CHECK(get_agx(&agx, "agx.range_from_exposure", &value, &minimum, &maximum, &original, &kind) == 0);
  before = g_memdup2(agx.params, agx.params_size);
  EDITOR_NATIVE_CHECK(set_agx(&agx, "agx.range_from_exposure", 1) == -1
    && !memcmp(before, agx.params, agx.params_size));
  g_clear_pointer(&before, g_free);
  defaults = g_memdup2(agx.default_params, agx.params_size);
  EDITOR_NATIVE_CHECK(set_agx(&agx, actions[0], 1) == 1);
  EDITOR_NATIVE_CHECK(EDITOR_NATIVE_INT(agx, "base_primaries") == 2
    && !EDITOR_NATIVE_INT(agx, "disable_primaries_adjustments")
    && !EDITOR_NATIVE_INT(agx, "completely_reverse_primaries"));
  const struct { const char *name; float value; } blender[] = {
    { "red_inset", .29462451f }, { "green_inset", .25861925f }, { "blue_inset", .14641371f },
    { "red_rotation", .03540329f }, { "green_rotation", -.02108586f }, { "blue_rotation", -.06305724f },
    { "red_outset", .290776401758f }, { "green_outset", .263155400753f }, { "blue_outset", .045810721815f },
    { "master_outset_ratio", 1 }, { "master_unrotation_ratio", 0 }
  };
  for(size_t k = 0; k < sizeof(blender) / sizeof(blender[0]); k++)
    EDITOR_NATIVE_CHECK(EDITOR_NATIVE_VALUE(agx, blender[k].name) == blender[k].value);
  EDITOR_NATIVE_CHECK(set_agx(&agx, actions[1], 1) == 1);
  EDITOR_NATIVE_CHECK(EDITOR_NATIVE_INT(agx, "base_primaries") == 1
    && EDITOR_NATIVE_VALUE(agx, "red_inset") == .1f
    && EDITOR_NATIVE_VALUE(agx, "green_inset") == .1f
    && EDITOR_NATIVE_VALUE(agx, "blue_inset") == .15f
    && fabsf(EDITOR_NATIVE_VALUE(agx, "red_rotation") - 2.f * 3.14159265358979323846f / 180.f) < 1e-7f
    && EDITOR_NATIVE_VALUE(agx, "master_outset_ratio") == 0
    && EDITOR_NATIVE_VALUE(agx, "master_unrotation_ratio") == 1);
  EDITOR_NATIVE_CHECK(set_agx(&agx, actions[3], 1) == 1);
  EDITOR_NATIVE_CHECK(EDITOR_NATIVE_VALUE(agx, "master_outset_ratio") == 1
    && EDITOR_NATIVE_VALUE(agx, "master_unrotation_ratio") == 1);
  const char *pre[] = { "red_inset", "green_inset", "blue_inset", "red_rotation", "green_rotation", "blue_rotation" };
  const char *post[] = { "red_outset", "green_outset", "blue_outset", "red_unrotation", "green_unrotation", "blue_unrotation" };
  for(size_t k = 0; k < 6; k++)
    EDITOR_NATIVE_CHECK(EDITOR_NATIVE_VALUE(agx, pre[k]) == EDITOR_NATIVE_VALUE(agx, post[k]));
  EDITOR_NATIVE_CHECK(set_agx(&agx, actions[2], 1) == 1);
  for(size_t k = 0; k < 6; k++)
    EDITOR_NATIVE_CHECK(EDITOR_NATIVE_VALUE(agx, pre[k]) == 0 && EDITOR_NATIVE_VALUE(agx, post[k]) == 0);
  EDITOR_NATIVE_CHECK(!memcmp(defaults, agx.default_params, agx.params_size));
  g_clear_pointer(&defaults, g_free);

  EDITOR_NATIVE_FLOAT(agx, "range_black_relative_ev", -10);
  EDITOR_NATIVE_FLOAT(agx, "range_white_relative_ev", 6.5f);
  EDITOR_NATIVE_FLOAT(agx, "curve_pivot_x", .4f);
  const float pivot_ev = -10.f + .4f * 16.5f;
  EDITOR_NATIVE_CHECK(set_agx(&agx, "range_black_relative_ev", -8) == 1);
  EDITOR_NATIVE_CHECK(fabsf(-8.f + EDITOR_NATIVE_VALUE(agx, "curve_pivot_x") * 14.5f - pivot_ev) < 1e-6f);
  EDITOR_NATIVE_CHECK(set_agx(&agx, "range_white_relative_ev", 8) == 1);
  EDITOR_NATIVE_CHECK(fabsf(-8.f + EDITOR_NATIVE_VALUE(agx, "curve_pivot_x") * 16.f - pivot_ev) < 1e-6f);
  EDITOR_NATIVE_FLOAT(agx, "dynamic_range_scaling", 0);
  EDITOR_NATIVE_CHECK(set_agx(&agx, "dynamic_range_scaling", .25) == 1);
  EDITOR_NATIVE_CHECK(EDITOR_NATIVE_VALUE(agx, "range_black_relative_ev") == -10
    && EDITOR_NATIVE_VALUE(agx, "range_white_relative_ev") == 10
    && fabsf(-10.f + EDITOR_NATIVE_VALUE(agx, "curve_pivot_x") * 20.f - pivot_ev) < 1e-6f);
  before = g_memdup2(agx.params, agx.params_size);
  const struct { const char *name; double value; } bad_agx[] = {
    { "agx.primaries_blender", 0 }, { "agx.primaries_smooth", 2 }, { "agx.copy_pre_to_post", NAN },
    { "range_black_relative_ev", 0 }, { "range_white_relative_ev", INFINITY },
    { "dynamic_range_scaling", 2 }, /* would push current endpoints past the original hard limits */
    { "dynamic_range_scaling", -.51 }, { "auto_gamma", .5 }
  };
  for(size_t k = 0; k < sizeof(bad_agx) / sizeof(bad_agx[0]); k++)
    EDITOR_NATIVE_CHECK(set_agx(&agx, bad_agx[k].name, bad_agx[k].value) == -1
      && !memcmp(before, agx.params, agx.params_size));
  EDITOR_NATIVE_CHECK(set_agx(&agx, "not_a_control", 1) == 0);
  g_clear_pointer(&before, g_free);

  before = g_memdup2(agx.params, agx.params_size);
  EDITOR_NATIVE_CHECK(set_agx(&agx, "curve_gamma", 3) == 0
    && !memcmp(before, agx.params, agx.params_size));
  g_clear_pointer(&before, g_free);
  EDITOR_NATIVE_CHECK(set_agx(&agx, "auto_gamma", 1) == 1);
  float auto_gamma = log2f(EDITOR_NATIVE_VALUE(agx, "curve_pivot_y_linear_output"))
    / log2f(EDITOR_NATIVE_VALUE(agx, "curve_pivot_x"));
  EDITOR_NATIVE_CHECK(fabsf(EDITOR_NATIVE_VALUE(agx, "curve_gamma") - auto_gamma) < 1e-6f);
  EDITOR_NATIVE_CHECK(set_agx(&agx, "curve_pivot_x", .6) == 1);
  auto_gamma = log2f(EDITOR_NATIVE_VALUE(agx, "curve_pivot_y_linear_output")) / log2f(.6f);
  EDITOR_NATIVE_CHECK(fabsf(EDITOR_NATIVE_VALUE(agx, "curve_gamma") - auto_gamma) < 1e-6f);
  EDITOR_NATIVE_CHECK(set_agx(&agx, "curve_pivot_y_linear_output", .25) == 1);
  auto_gamma = log2f(.25f) / log2f(.6f);
  EDITOR_NATIVE_CHECK(fabsf(EDITOR_NATIVE_VALUE(agx, "curve_gamma") - auto_gamma) < 1e-6f);
  before = g_memdup2(agx.params, agx.params_size);
  EDITOR_NATIVE_CHECK(set_agx(&agx, "curve_gamma", 3) == -1
    && !memcmp(before, agx.params, agx.params_size));
  g_clear_pointer(&before, g_free);

  /* Resolve only this develop's enabled original exposure instances. Prefer
   * an unmasked instance, then the lowest priority, regardless of list order. */
  exposure.so = other_exposure.so = dt_iop_get_module_so("exposure");
  EDITOR_NATIVE_CHECK(exposure.so && exposure.so->module && exposure.so->get_introspection_linear);
  exposure.module = other_exposure.module = exposure.so->module;
  exposure.dev = other_exposure.dev = &develop;
  g_strlcpy(exposure.op, "exposure", sizeof(exposure.op));
  g_strlcpy(other_exposure.op, "exposure", sizeof(other_exposure.op));
  dt_iop_default_init(&exposure); dt_iop_default_init(&other_exposure);
  EDITOR_NATIVE_CHECK(exposure.params && exposure.default_params && other_exposure.params && other_exposure.default_params);
  memcpy(exposure.params, exposure.default_params, exposure.params_size);
  memcpy(other_exposure.params, other_exposure.default_params, other_exposure.params_size);
  exposure.enabled = other_exposure.enabled = TRUE;
  exposure.multi_priority = 5; other_exposure.multi_priority = 0;
  exposure.blend_params = &exposure_blend; other_exposure.blend_params = &other_exposure_blend;
  exposure_blend.mask_mode = DEVELOP_MASK_ENABLED;
  other_exposure_blend.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK;
  develop.iop = g_list_append(develop.iop, &other_exposure);
  develop.iop = g_list_append(develop.iop, &exposure);
  EDITOR_NATIVE_FLOAT(exposure, "exposure", 1.25f);
  EDITOR_NATIVE_FLOAT(other_exposure, "exposure", 3.f);
  EDITOR_NATIVE_INT(exposure, "compensate_exposure_bias") = TRUE;
  EDITOR_NATIVE_INT(exposure, "compensate_hilite_pres") = TRUE;
  EDITOR_NATIVE_INT(other_exposure, "compensate_exposure_bias") = FALSE;
  EDITOR_NATIVE_INT(other_exposure, "compensate_hilite_pres") = FALSE;
  develop.image_storage.exif_exposure_bias = .75f;
  develop.image_storage.exif_highlight_preservation = .5f;
  EDITOR_NATIVE_CHECK(get_agx(&agx, "agx.range_from_exposure", &value, &minimum, &maximum, &original, &kind) == 1);
  EDITOR_NATIVE_CHECK(set_agx(&agx, "agx.range_from_exposure", 1) == 1);
  EDITOR_NATIVE_CHECK(fabsf(EDITOR_NATIVE_VALUE(agx, "range_black_relative_ev") - (-8.f + .5f) * 1.25f) < 1e-6f
    && fabsf(EDITOR_NATIVE_VALUE(agx, "range_white_relative_ev") - (4.f + .8f) * 1.25f) < 1e-6f);
  other_exposure_blend.mask_mode = DEVELOP_MASK_ENABLED;
  EDITOR_NATIVE_CHECK(set_agx(&agx, "agx.range_from_exposure", 1) == 1);
  EDITOR_NATIVE_CHECK(fabsf(EDITOR_NATIVE_VALUE(agx, "range_black_relative_ev") - (-8.f + 1.5f) * 1.25f) < 1e-6f
    && fabsf(EDITOR_NATIVE_VALUE(agx, "range_white_relative_ev") - (4.f + 2.4f) * 1.25f) < 1e-6f);
  exposure.enabled = other_exposure.enabled = FALSE;
  EDITOR_NATIVE_CHECK(get_agx(&agx, "agx.range_from_exposure", &value, &minimum, &maximum, &original, &kind) == 0);

  fprintf(stderr, "editor actions: original Filmic RGB GUI coupling and hardness math\n");
  EDITOR_NATIVE_FLOAT(filmic, "grey_point_source", 18.45f);
  EDITOR_NATIVE_FLOAT(filmic, "black_point_source", -8);
  EDITOR_NATIVE_FLOAT(filmic, "white_point_source", 4);
  EDITOR_NATIVE_FLOAT(filmic, "grey_point_target", 18.45f);
  EDITOR_NATIVE_FLOAT(filmic, "security_factor", 0);
  EDITOR_NATIVE_CHECK(editor_actions_integer(&filmic, "auto_hardness"));
  EDITOR_NATIVE_INT(filmic, "auto_hardness") = TRUE;
  EDITOR_NATIVE_CHECK(set_filmic(&filmic, "grey_point_source", (double)(18.45f * 2.f)) == 1);
  /* These are the signs in the original gui_changed callback. */
  EDITOR_NATIVE_CHECK(fabsf(EDITOR_NATIVE_VALUE(filmic, "black_point_source") + 7.f) < 1e-6f
    && fabsf(EDITOR_NATIVE_VALUE(filmic, "white_point_source") - 3.f) < 1e-6f);
  float expected = logf(18.45f / 100.f) / logf(7.f / 10.f);
  expected = fminf(10.f, fmaxf(1.f, expected));
  EDITOR_NATIVE_CHECK(fabsf(EDITOR_NATIVE_VALUE(filmic, "output_power") - expected) < 1e-6f);
  EDITOR_NATIVE_CHECK(set_filmic(&filmic, "security_factor", 25) == 1);
  EDITOR_NATIVE_CHECK(EDITOR_NATIVE_VALUE(filmic, "black_point_source") == -8.75f
    && EDITOR_NATIVE_VALUE(filmic, "white_point_source") == 3.75f);
  EDITOR_NATIVE_CHECK(set_filmic(&filmic, "auto_hardness", 0) == 1);
  before = g_memdup2(filmic.params, filmic.params_size);
  EDITOR_NATIVE_CHECK(set_filmic(&filmic, "output_power", 3) == 0
    && !memcmp(before, filmic.params, filmic.params_size));
  g_clear_pointer(&before, g_free);
  const float manual_power = EDITOR_NATIVE_VALUE(filmic, "output_power");
  EDITOR_NATIVE_CHECK(set_filmic(&filmic, "black_point_source", -10) == 1
    && EDITOR_NATIVE_VALUE(filmic, "output_power") == manual_power);
  EDITOR_NATIVE_CHECK(set_filmic(&filmic, "auto_hardness", 1) == 1);
  expected = fminf(10.f, fmaxf(1.f, logf(18.45f / 100.f) / logf(10.f / 13.75f)));
  EDITOR_NATIVE_CHECK(fabsf(EDITOR_NATIVE_VALUE(filmic, "output_power") - expected) < 1e-6f);
  EDITOR_NATIVE_CHECK(get_filmic(&filmic, "security_factor", &value, &minimum, &maximum, &original, &kind) == 1
    && value == 25 && minimum == -50 && maximum == 200 && kind == 0);
  EDITOR_NATIVE_CHECK(get_filmic(&filmic, "auto_hardness", &value, &minimum, &maximum, &original, &kind) == 1
    && value == 1 && minimum == 0 && maximum == 1 && kind == 2);
  before = g_memdup2(filmic.params, filmic.params_size);
  const struct { const char *name; double value; } bad_filmic[] = {
    { "grey_point_source", 0 }, { "grey_point_source", NAN },
    { "black_point_source", -16.1 }, { "white_point_source", 0 },
    { "security_factor", 201 }, { "security_factor", -51 },
    { "security_factor", 200 }, /* coupled range would exceed the hard limits */
    { "output_power", 3 }, /* automatic hardness owns this value */
    { "auto_hardness", .5 }, { "auto_hardness", INFINITY }
  };
  for(size_t k = 0; k < sizeof(bad_filmic) / sizeof(bad_filmic[0]); k++)
    EDITOR_NATIVE_CHECK(set_filmic(&filmic, bad_filmic[k].name, bad_filmic[k].value) == -1
      && !memcmp(before, filmic.params, filmic.params_size));
  EDITOR_NATIVE_CHECK(set_filmic(&filmic, "not_a_control", 1) == 0);
  success = 1;
finished:
  g_list_free(develop.iop);
  g_free(before); g_free(defaults);
  g_free(agx.params); g_free(agx.default_params);
  g_free(filmic.params); g_free(filmic.default_params);
  g_free(exposure.params); g_free(exposure.default_params);
  g_free(other_exposure.params); g_free(other_exposure.default_params);
  return success;
#undef EDITOR_NATIVE_CHECK
#undef EDITOR_NATIVE_FLOAT
#undef EDITOR_NATIVE_VALUE
#undef EDITOR_NATIVE_INT
}

static int editor_actions_roundtrip(dt_mobile_session *session, const char *operation,
                                    const char *parameter, double value)
{
  int success = 0, applied = 0;
  char *before = NULL, *after = NULL, *restored_metadata = NULL;
  char *history = NULL, *after_history = NULL;
  uint8_t *reference = NULL, *changed = NULL, *restored = NULL;
  int width = 0, height = 0, changed_width = 0, changed_height = 0, restored_width = 0, restored_height = 0;
#define EDITOR_ROUNDTRIP_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "editor actions roundtrip %s.%s:%d: check failed: %s\n", operation, parameter, __LINE__, #condition); goto finished; } } while(0)
#define EDITOR_ROUNDTRIP_OK(call) do { if((call) != DT_MOBILE_OK) { \
  fprintf(stderr, "editor actions roundtrip: %s: %s\n", #call, dt_mobile_last_error(session)); goto finished; } } while(0)
  EDITOR_ROUNDTRIP_OK(dt_mobile_modules(session, &before));
  EDITOR_ROUNDTRIP_OK(dt_mobile_history(session, &history));
  /* Match the existing RGB fixture's readiness render before comparing
   * successive original-pixelpipe frames byte for byte. */
  EDITOR_ROUNDTRIP_OK(dt_mobile_render_preview(session, 96, 96, &reference, &width, &height));
  dt_mobile_free_buffer(reference); reference = NULL;
  EDITOR_ROUNDTRIP_OK(dt_mobile_render_preview(session, 96, 96, &reference, &width, &height));
  EDITOR_ROUNDTRIP_OK(dt_mobile_set_parameter(session, operation, parameter, value));
  applied = 1;
  EDITOR_ROUNDTRIP_OK(dt_mobile_modules(session, &after));
  EDITOR_ROUNDTRIP_OK(dt_mobile_history(session, &after_history));
  EDITOR_ROUNDTRIP_CHECK(editor_actions_position(after_history) == editor_actions_position(history) + 1);
  EDITOR_ROUNDTRIP_OK(dt_mobile_render_preview(session, 96, 96, &changed, &changed_width, &changed_height));
  EDITOR_ROUNDTRIP_CHECK(reference && changed && width == changed_width && height == changed_height
    && memcmp(reference, changed, (size_t)width * height * 4));
  EDITOR_ROUNDTRIP_OK(dt_mobile_undo(session)); applied = 0;
  EDITOR_ROUNDTRIP_OK(dt_mobile_modules(session, &restored_metadata));
  EDITOR_ROUNDTRIP_CHECK(!strcmp(before, restored_metadata));
  EDITOR_ROUNDTRIP_OK(dt_mobile_render_preview(session, 96, 96, &restored, &restored_width, &restored_height));
  EDITOR_ROUNDTRIP_CHECK(restored && width == restored_width && height == restored_height
    && !memcmp(reference, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  dt_mobile_free_buffer(restored_metadata); restored_metadata = NULL;
  EDITOR_ROUNDTRIP_OK(dt_mobile_redo(session)); applied = 1;
  EDITOR_ROUNDTRIP_OK(dt_mobile_modules(session, &restored_metadata));
  EDITOR_ROUNDTRIP_CHECK(!strcmp(after, restored_metadata));
  EDITOR_ROUNDTRIP_OK(dt_mobile_render_preview(session, 96, 96, &restored, &restored_width, &restored_height));
  EDITOR_ROUNDTRIP_CHECK(restored && width == restored_width && height == restored_height
    && !memcmp(changed, restored, (size_t)width * height * 4));
  EDITOR_ROUNDTRIP_OK(dt_mobile_undo(session)); applied = 0;
  success = 1;
finished:
  if(applied) dt_mobile_undo(session);
  dt_mobile_free_buffer(before); dt_mobile_free_buffer(after); dt_mobile_free_buffer(restored_metadata);
  dt_mobile_free_buffer(history); dt_mobile_free_buffer(after_history);
  dt_mobile_free_buffer(reference); dt_mobile_free_buffer(changed); dt_mobile_free_buffer(restored);
  return success;
#undef EDITOR_ROUNDTRIP_OK
#undef EDITOR_ROUNDTRIP_CHECK
}

static int editor_actions_mode_checks(dt_mobile_session *session, const char *operation,
                                      const char *automatic_parameter, const char *manual_parameter)
{
  int success = 0;
  char *manual = NULL, *automatic = NULL, *after = NULL, *history = NULL, *after_history = NULL;
  double number;
#define EDITOR_MODE_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "editor actions mode %s:%d: check failed: %s\n", operation, __LINE__, #condition); goto finished; } } while(0)
#define EDITOR_MODE_OK(call) do { if((call) != DT_MOBILE_OK) { \
  fprintf(stderr, "editor actions mode: %s: %s\n", #call, dt_mobile_last_error(session)); goto finished; } } while(0)
  EDITOR_MODE_OK(dt_mobile_set_parameter(session, operation, automatic_parameter, 0));
  EDITOR_MODE_OK(dt_mobile_modules(session, &manual));
  EDITOR_MODE_CHECK(editor_actions_number(manual, operation, manual_parameter, "value", &number));
  const int manual_count = editor_actions_parameter_count(manual, operation);
  EDITOR_MODE_CHECK(manual_count > 1);
  EDITOR_MODE_OK(dt_mobile_set_parameter(session, operation, automatic_parameter, 1));
  EDITOR_MODE_OK(dt_mobile_modules(session, &automatic));
  EDITOR_MODE_CHECK(!editor_actions_number(automatic, operation, manual_parameter, "value", &number)
    && editor_actions_parameter_count(automatic, operation) == manual_count - 1);
  EDITOR_MODE_OK(dt_mobile_history(session, &history));
  EDITOR_MODE_CHECK(dt_mobile_set_parameter(session, operation, manual_parameter, 3)
    == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  EDITOR_MODE_OK(dt_mobile_history(session, &after_history));
  EDITOR_MODE_CHECK(!strcmp(history, after_history));
  EDITOR_MODE_OK(dt_mobile_modules(session, &after));
  EDITOR_MODE_CHECK(!strcmp(automatic, after));
  dt_mobile_free_buffer(after); after = NULL;
  EDITOR_MODE_OK(dt_mobile_set_parameter(session, operation, automatic_parameter, 0));
  EDITOR_MODE_OK(dt_mobile_modules(session, &after));
  EDITOR_MODE_CHECK(editor_actions_number(after, operation, manual_parameter, "value", &number)
    && editor_actions_parameter_count(after, operation) == manual_count);
  success = 1;
finished:
  dt_mobile_free_buffer(manual); dt_mobile_free_buffer(automatic); dt_mobile_free_buffer(after);
  dt_mobile_free_buffer(history); dt_mobile_free_buffer(after_history);
  return success;
#undef EDITOR_MODE_CHECK
#undef EDITOR_MODE_OK
}

/* Public adapter edits use the original pixelpipe and create exactly one
 * undoable history item even when several original fields change together. */
static int mobile_editor_actions_checks(dt_mobile_session *session)
{
  int success = 0;
  long initial_position = -1;
  char *initial = NULL, *metadata = NULL, *history = NULL, *after_history = NULL;
#define EDITOR_ACTIONS_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "editor actions:%d: check failed: %s\n", __LINE__, #condition); goto finished; } } while(0)
#define EDITOR_ACTIONS_OK(call) do { if((call) != DT_MOBILE_OK) { \
  fprintf(stderr, "editor actions: %s: %s\n", #call, dt_mobile_last_error(session)); goto finished; } } while(0)
  EDITOR_ACTIONS_CHECK(editor_actions_native_checks());
  EDITOR_ACTIONS_OK(dt_mobile_modules(session, &initial));
  EDITOR_ACTIONS_OK(dt_mobile_history(session, &history));
  initial_position = editor_actions_position(history);
  EDITOR_ACTIONS_CHECK(initial_position >= 0);
  dt_mobile_free_buffer(history); history = NULL;
  EDITOR_ACTIONS_OK(dt_mobile_set_parameter(session, "agx", "agx.primaries_unmodified", 1));
  EDITOR_ACTIONS_OK(dt_mobile_set_parameter(session, "agx", "range_black_relative_ev", -10));
  EDITOR_ACTIONS_OK(dt_mobile_set_parameter(session, "agx", "range_white_relative_ev", 6.5));
  EDITOR_ACTIONS_OK(dt_mobile_set_parameter(session, "agx", "dynamic_range_scaling", 0));
  EDITOR_ACTIONS_OK(dt_mobile_set_parameter(session, "exposure", "compensate_exposure_bias", 0));
  EDITOR_ACTIONS_OK(dt_mobile_set_parameter(session, "exposure", "compensate_hilite_pres", 0));
  EDITOR_ACTIONS_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 1.25));
  EDITOR_ACTIONS_OK(dt_mobile_modules(session, &metadata));
  const char *actions[] = { "agx.range_from_exposure", "agx.primaries_blender", "agx.primaries_smooth",
    "agx.primaries_unmodified", "agx.copy_pre_to_post" };
  for(size_t k = 0; k < sizeof(actions) / sizeof(actions[0]); k++)
  {
    double number = 0;
    EDITOR_ACTIONS_CHECK(editor_actions_action_metadata(metadata, "agx", actions[k]));
    EDITOR_ACTIONS_CHECK(editor_actions_number(metadata, "agx", actions[k], "value", &number) && number == 1);
    EDITOR_ACTIONS_CHECK(editor_actions_number(metadata, "agx", actions[k], "min", &number) && number == 1);
    EDITOR_ACTIONS_CHECK(editor_actions_number(metadata, "agx", actions[k], "max", &number) && number == 1);
  }
  EDITOR_ACTIONS_OK(dt_mobile_history(session, &history));
  const struct { const char *operation, *parameter; double value; } invalid[] = {
    { "agx", "agx.range_from_exposure", 0 }, { "agx", "agx.primaries_blender", .5 },
    { "agx", "agx.primaries_smooth", INFINITY }, { "agx", "agx.primaries_unmodified", NAN },
    { "agx", "agx.copy_pre_to_post", 2 }, { "agx", "dynamic_range_scaling", -1 },
    { "filmicrgb", "security_factor", 201 }, { "filmicrgb", "auto_hardness", .25 }
  };
  for(size_t k = 0; k < sizeof(invalid) / sizeof(invalid[0]); k++)
    EDITOR_ACTIONS_CHECK(dt_mobile_set_parameter(session, invalid[k].operation, invalid[k].parameter, invalid[k].value)
      == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  EDITOR_ACTIONS_OK(dt_mobile_history(session, &after_history));
  EDITOR_ACTIONS_CHECK(!strcmp(history, after_history));
  dt_mobile_free_buffer(after_history); after_history = NULL;
  EDITOR_ACTIONS_OK(dt_mobile_modules(session, &after_history));
  EDITOR_ACTIONS_CHECK(!strcmp(metadata, after_history));
  dt_mobile_free_buffer(after_history); after_history = NULL;
  dt_mobile_free_buffer(metadata); metadata = NULL;
  dt_mobile_free_buffer(history); history = NULL;

  fprintf(stderr, "editor actions: automatic modes hide and protect manual controls\n");
  EDITOR_ACTIONS_CHECK(editor_actions_mode_checks(session, "agx", "auto_gamma", "curve_gamma"));
  EDITOR_ACTIONS_CHECK(editor_actions_mode_checks(session, "filmicrgb", "auto_hardness", "output_power"));

  fprintf(stderr, "editor actions: AgX and Filmic RGB original pixels, atomic history, undo and redo\n");
  EDITOR_ACTIONS_CHECK(editor_actions_roundtrip(session, "agx", "agx.primaries_smooth", 1));
  EDITOR_ACTIONS_CHECK(editor_actions_roundtrip(session, "agx", "agx.primaries_blender", 1));
  EDITOR_ACTIONS_CHECK(editor_actions_roundtrip(session, "agx", "agx.range_from_exposure", 1));
  EDITOR_ACTIONS_CHECK(editor_actions_roundtrip(session, "agx", "dynamic_range_scaling", .25));
  EDITOR_ACTIONS_OK(dt_mobile_set_parameter(session, "filmicrgb", "black_point_source", -8));
  EDITOR_ACTIONS_OK(dt_mobile_set_parameter(session, "filmicrgb", "white_point_source", 4));
  EDITOR_ACTIONS_OK(dt_mobile_set_parameter(session, "filmicrgb", "auto_hardness", 1));
  EDITOR_ACTIONS_OK(dt_mobile_set_parameter(session, "filmicrgb", "security_factor", 0));
  EDITOR_ACTIONS_CHECK(editor_actions_roundtrip(session, "filmicrgb", "black_point_source", -10));
  EDITOR_ACTIONS_CHECK(editor_actions_roundtrip(session, "filmicrgb", "security_factor", 25));
  success = 1;
finished:
  dt_mobile_free_buffer(history); history = NULL;
  if(initial_position >= 0)
  {
    while(dt_mobile_history(session, &history) == DT_MOBILE_OK)
    {
      const long position = editor_actions_position(history);
      dt_mobile_free_buffer(history); history = NULL;
      if(position <= initial_position) break;
      if(dt_mobile_undo(session) != DT_MOBILE_OK) { success = 0; break; }
    }
    dt_mobile_free_buffer(metadata); metadata = NULL;
    if(success && (dt_mobile_modules(session, &metadata) != DT_MOBILE_OK || strcmp(initial, metadata))) success = 0;
  }
  dt_mobile_free_buffer(initial); dt_mobile_free_buffer(metadata);
  dt_mobile_free_buffer(history); dt_mobile_free_buffer(after_history);
  return success;
#undef EDITOR_ACTIONS_CHECK
#undef EDITOR_ACTIONS_OK
}

#endif
