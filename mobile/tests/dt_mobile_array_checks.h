/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_ARRAY_CHECKS_H
#define DT_MOBILE_ARRAY_CHECKS_H

/* Exercise actual public native metadata, original processing/history, and
 * rejection of padding/structural/ill-ordered arrays, not copied IOP structs. */
static int mobile_array_checks(dt_mobile_session *session)
{
  static const struct { const char *module, *path; float value; } edits[] = {
    { "channelmixerrgb", "red[1]", 0.2f },
    { "channelmixer", "red[3]", 0.8f },
    { "colorchecker", "target_L[0]", 35.f },
    { "negadoctor", "Dmin[0]", 0.8f },
    { "colorbalance", "gamma[1]", 1.1f },
    { "levels", "levels[0]", 0.1f },
    { "rgblevels", "levels[0][0]", 0.1f },
    { "borders", "color[0]", 0.4f },
    { "invert", "color[0]", 0.7f },
    { "colorharmonizer", "node_saturation[0]", 0.8f },
    { "atrous", "y[0][2]", 0.7f },
    { "rawdenoise", "y[0][2]", 0.7f },
    { "denoiseprofile", "y[0][2]", 0.7f },
    { "equalizer", "equalizer_y[0][2]", 0.7f },
    { "lowlight", "transition_y[2]", 0.7f },
  };
  char *baseline = NULL, *changed = NULL, *history = NULL, *rejected_history = NULL;
  uint8_t *before = NULL, *after = NULL, *restored = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0, success = 0;
#define ARRAY_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "array controls:%d: check failed: %s\n", __LINE__, #condition); goto finished; } } while(0)
#define ARRAY_OK(call) do { if((call) != DT_MOBILE_OK) { \
  fprintf(stderr, "array controls: %s: %s\n", #call, dt_mobile_last_error(session)); goto finished; } } while(0)
  ARRAY_OK(dt_mobile_modules(session, &baseline));
  ARRAY_CHECK(!module_has(baseline, "mask_manager", "\"operation\""));
  ARRAY_CHECK(module_has(baseline, "tonecurve", "\"resetNodes\":"));
  ARRAY_CHECK(module_has(baseline, "colorzones", "\"resetNodes\":"));
  ARRAY_CHECK(module_has(baseline, "rgblevels", "\"canReset\":true"));
  dt_mobile_free_buffer(baseline); baseline = NULL;

  for(size_t index = 0; index < sizeof(edits) / sizeof(edits[0]); index++)
  {
    ARRAY_OK(dt_mobile_modules(session, &baseline));
    ARRAY_CHECK(module_has(baseline, edits[index].module, edits[index].path));
    ARRAY_OK(dt_mobile_set_parameter(session, edits[index].module, edits[index].path, edits[index].value));
    ARRAY_OK(dt_mobile_modules(session, &changed));
    ARRAY_CHECK(parameter_equals(changed, edits[index].module, edits[index].path, (double)edits[index].value));
    ARRAY_OK(dt_mobile_undo(session));
    dt_mobile_free_buffer(changed); changed = NULL;
    ARRAY_OK(dt_mobile_modules(session, &changed));
    ARRAY_CHECK(!strcmp(changed, baseline));
    dt_mobile_free_buffer(changed); changed = NULL;
    dt_mobile_free_buffer(baseline); baseline = NULL;
  }
  ARRAY_OK(dt_mobile_history(session, &history));
  ARRAY_CHECK(dt_mobile_set_parameter(session, "colorchecker", "source_L[0]", 25.0) == DT_MOBILE_ERROR_UNSUPPORTED);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "colorchecker", "target_L[49]", 25.0) == DT_MOBILE_ERROR_UNSUPPORTED);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "channelmixerrgb", "red[3]", 0.2) == DT_MOBILE_ERROR_UNSUPPORTED);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "rgblevels", "levels[3][0]", 0.2) == DT_MOBILE_ERROR_UNSUPPORTED);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "rgblevels", "levels[0][3]", 0.2) == DT_MOBILE_ERROR_UNSUPPORTED);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "rgblevels", "levels[0][0]", 0.5) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "levels", "levels[1]", 1.0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "atrous", "x[0][2]", 1.0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "atrous", "x[0][0]", 0.1) == DT_MOBILE_ERROR_UNSUPPORTED);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "negadoctor", "Dmin[0]", NAN) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ARRAY_OK(dt_mobile_history(session, &rejected_history));
  ARRAY_CHECK(!strcmp(history, rejected_history));
  dt_mobile_free_buffer(history); history = NULL;
  dt_mobile_free_buffer(rejected_history); rejected_history = NULL;

  /* Reject normalized matrix division by zero without altering history. */
  ARRAY_OK(dt_mobile_set_parameter(session, "channelmixerrgb", "normalize_R", 1));
  ARRAY_OK(dt_mobile_history(session, &history));
  ARRAY_CHECK(dt_mobile_set_parameter(session, "channelmixerrgb", "red[0]", 0.0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ARRAY_OK(dt_mobile_history(session, &rejected_history));
  ARRAY_CHECK(!strcmp(history, rejected_history));
  ARRAY_OK(dt_mobile_undo(session));
  dt_mobile_free_buffer(history); history = NULL;
  dt_mobile_free_buffer(rejected_history); rejected_history = NULL;

  /* Actual RGB-level pixels, exact undo and redo; prime existing first-pass
   * behavior before collecting the baseline as other native regressions do. */
  ARRAY_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  dt_mobile_free_buffer(restored); restored = NULL;
  ARRAY_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  ARRAY_OK(dt_mobile_set_parameter(session, "rgblevels", "levels[0][0]", 0.1));
  ARRAY_OK(dt_mobile_render_preview(session, 96, 96, &after, &other_width, &other_height));
  ARRAY_CHECK(width == other_width && height == other_height && memcmp(before, after, (size_t)width * height * 4));
  ARRAY_OK(dt_mobile_undo(session));
  ARRAY_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  ARRAY_CHECK(width == other_width && height == other_height && !memcmp(before, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ARRAY_OK(dt_mobile_redo(session));
  ARRAY_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  ARRAY_CHECK(width == other_width && height == other_height && !memcmp(after, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  dt_mobile_free_buffer(after); after = NULL;

  /* Reset restores the image-specific parameters in one undoable step. */
  ARRAY_OK(dt_mobile_modules(session, &baseline));
  ARRAY_OK(dt_mobile_set_parameter(session, "rgblevels", "module.reset", 1));
  ARRAY_OK(dt_mobile_modules(session, &changed));
  ARRAY_CHECK(parameter_equals(changed, "rgblevels", "levels[0][0]", 0.0));
  ARRAY_CHECK(module_has(changed, "rgblevels", "\"enabled\":true"));
  ARRAY_OK(dt_mobile_undo(session));
  dt_mobile_free_buffer(changed); changed = NULL;
  ARRAY_OK(dt_mobile_modules(session, &changed));
  ARRAY_CHECK(!strcmp(baseline, changed));
  ARRAY_OK(dt_mobile_undo(session));
  dt_mobile_free_buffer(baseline); baseline = NULL;
  dt_mobile_free_buffer(changed); changed = NULL;

  /* Lab tone-curve and color-zone node edits use the same original spline
   * implementation as desktop, while preserving all untouched channels. */
  const dt_mobile_curve_point tone[] = { {0.f, 0.f}, {0.5f, 0.7f}, {1.f, 1.f} };
  const dt_mobile_curve_point zones[] = { {0.f, 0.7f}, {0.5f, 0.7f}, {1.f, 0.7f} };
  const char *curves[] = { "tonecurve", "colorzones" };
  for(size_t index = 0; index < 2; index++)
  {
    ARRAY_OK(dt_mobile_modules(session, &baseline));
    ARRAY_OK(dt_mobile_set_curve(session, curves[index], 0, 2, index ? zones : tone, 3));
    ARRAY_OK(dt_mobile_render_preview(session, 96, 96, &after, &other_width, &other_height));
    ARRAY_CHECK(width == other_width && height == other_height && memcmp(before, after, (size_t)width * height * 4));
    ARRAY_OK(dt_mobile_undo(session));
    ARRAY_OK(dt_mobile_modules(session, &changed));
    ARRAY_CHECK(!strcmp(baseline, changed));
    ARRAY_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
    ARRAY_CHECK(width == other_width && height == other_height && !memcmp(before, restored, (size_t)width * height * 4));
    dt_mobile_free_buffer(restored); restored = NULL;
    dt_mobile_free_buffer(after); after = NULL;
    dt_mobile_free_buffer(baseline); baseline = NULL;
    dt_mobile_free_buffer(changed); changed = NULL;
  }
  /* The original zone-system stores -1 automatic boundaries. Its mobile
   * controls expose the actual resolved map and pin/automatic operations. */
  ARRAY_OK(dt_mobile_modules(session, &baseline));
  ARRAY_CHECK(parameter_equals(baseline, "zonesystem", "zones.count", 10.0));
  ARRAY_CHECK(parameter_equals(baseline, "zonesystem", "zones.automatic[4]", 1.0));
  ARRAY_CHECK(dt_mobile_set_parameter(session, "zonesystem", "size", 6.0) == DT_MOBILE_ERROR_UNSUPPORTED);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "zonesystem", "zone[4]", 0.5) == DT_MOBILE_ERROR_UNSUPPORTED);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "zonesystem", "zones.count", 3) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "zonesystem", "zones.count", 4.5) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "zonesystem", "zones.position[4]", 0.9) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ARRAY_OK(dt_mobile_set_parameter(session, "zonesystem", "zones.position[4]", 0.5));
  ARRAY_OK(dt_mobile_modules(session, &changed));
  ARRAY_CHECK(parameter_equals(changed, "zonesystem", "zones.position[4]", 0.5));
  ARRAY_CHECK(parameter_equals(changed, "zonesystem", "zones.automatic[4]", 0.0));
  ARRAY_OK(dt_mobile_render_preview(session, 96, 96, &after, &other_width, &other_height));
  ARRAY_CHECK(width == other_width && height == other_height && memcmp(before, after, (size_t)width * height * 4));
  ARRAY_OK(dt_mobile_set_parameter(session, "zonesystem", "zones.count", 6));
  dt_mobile_free_buffer(changed); changed = NULL;
  ARRAY_OK(dt_mobile_modules(session, &changed));
  ARRAY_CHECK(parameter_equals(changed, "zonesystem", "zones.count", 6));
  ARRAY_CHECK(parameter_equals(changed, "zonesystem", "zones.automatic[4]", 1.0));
  ARRAY_CHECK(parameter_equals(changed, "zonesystem", "zones.position[4]", (double)0.8f));
  ARRAY_OK(dt_mobile_undo(session));
  ARRAY_OK(dt_mobile_undo(session));
  dt_mobile_free_buffer(changed); changed = NULL;
  ARRAY_OK(dt_mobile_modules(session, &changed));
  ARRAY_CHECK(!strcmp(baseline, changed));
  ARRAY_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  ARRAY_CHECK(width == other_width && height == other_height && !memcmp(before, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(after); after = NULL;
  dt_mobile_free_buffer(restored); restored = NULL;
  dt_mobile_free_buffer(baseline); baseline = NULL;
  dt_mobile_free_buffer(changed); changed = NULL;

  /* Isolated liquify points use the original native warp records, stamp
   * construction and processing. Their add/delete actions remain undoable. */
  ARRAY_OK(dt_mobile_modules(session, &baseline));
  ARRAY_CHECK(module_has(baseline, "liquify", "\"name\":\"liquify.add\""));
  ARRAY_OK(dt_mobile_set_parameter(session, "liquify", "liquify.add", 1));
  ARRAY_OK(dt_mobile_modules(session, &changed));
  ARRAY_CHECK(parameter_equals(changed, "liquify", "liquify.0.x", 0.5));
  ARRAY_CHECK(parameter_equals(changed, "liquify", "liquify.0.type", 0));
  ARRAY_CHECK(module_has(changed, "liquify", "\"label\":\"radial grow\""));
  ARRAY_OK(dt_mobile_history(session, &history));
  ARRAY_CHECK(dt_mobile_set_parameter(session, "liquify", "liquify.0.radius", 0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "liquify", "liquify.0.hardness1", 0.9) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "liquify", "liquify.0.type", 3) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ARRAY_CHECK(dt_mobile_set_parameter(session, "liquify", "liquify.1.x", 0.5) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ARRAY_OK(dt_mobile_history(session, &rejected_history));
  ARRAY_CHECK(!strcmp(history, rejected_history));
  dt_mobile_free_buffer(history); history = NULL;
  dt_mobile_free_buffer(rejected_history); rejected_history = NULL;
  ARRAY_OK(dt_mobile_render_preview(session, 96, 96, &after, &other_width, &other_height));
  ARRAY_CHECK(width == other_width && height == other_height && memcmp(before, after, (size_t)width * height * 4));
  ARRAY_OK(dt_mobile_undo(session));
  ARRAY_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  ARRAY_CHECK(width == other_width && height == other_height && !memcmp(before, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ARRAY_OK(dt_mobile_redo(session));
  ARRAY_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  ARRAY_CHECK(width == other_width && height == other_height && !memcmp(after, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  dt_mobile_free_buffer(after); after = NULL;
  ARRAY_OK(dt_mobile_set_parameter(session, "liquify", "liquify.0.type", 1));
  ARRAY_OK(dt_mobile_render_preview(session, 96, 96, &after, &other_width, &other_height));
  ARRAY_CHECK(width == other_width && height == other_height && memcmp(before, after, (size_t)width * height * 4));
  ARRAY_OK(dt_mobile_undo(session));
  ARRAY_OK(dt_mobile_set_parameter(session, "liquify", "liquify.add", 1));
  dt_mobile_free_buffer(changed); changed = NULL;
  ARRAY_OK(dt_mobile_modules(session, &changed));
  ARRAY_CHECK(module_has(changed, "liquify", "\"name\":\"liquify.1.x\""));
  ARRAY_OK(dt_mobile_set_parameter(session, "liquify", "liquify.0.delete", 1));
  dt_mobile_free_buffer(changed); changed = NULL;
  ARRAY_OK(dt_mobile_modules(session, &changed));
  ARRAY_CHECK(parameter_equals(changed, "liquify", "liquify.0.x", 0.5));
  ARRAY_CHECK(!module_has(changed, "liquify", "\"name\":\"liquify.1.x\""));
  ARRAY_OK(dt_mobile_undo(session));
  ARRAY_OK(dt_mobile_undo(session));
  ARRAY_OK(dt_mobile_undo(session));
  dt_mobile_free_buffer(changed); changed = NULL;
  ARRAY_OK(dt_mobile_modules(session, &changed));
  ARRAY_CHECK(!strcmp(baseline, changed));
  success = 1;

finished:
  dt_mobile_free_buffer(baseline); dt_mobile_free_buffer(changed);
  dt_mobile_free_buffer(history); dt_mobile_free_buffer(rejected_history);
  dt_mobile_free_buffer(before); dt_mobile_free_buffer(after); dt_mobile_free_buffer(restored);
#undef ARRAY_CHECK
#undef ARRAY_OK
  return success;
}
#endif
