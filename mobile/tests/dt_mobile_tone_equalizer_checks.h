/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_TONE_EQUALIZER_CHECKS_H
#define DT_MOBILE_TONE_EQUALIZER_CHECKS_H

static double tone_equalizer_parameter(const char *json, const char *operation, const char *name)
{
  char key[128];
  snprintf(key, sizeof(key), "{\"operation\":\"%s\",", operation);
  const char *module = strstr(json, key);
  if(!module) return NAN;
  const char *module_end = strstr(module + 1, "{\"operation\":");
  snprintf(key, sizeof(key), "{\"name\":\"%s\",", name);
  const char *field = strstr(module, key);
  if(!field || (module_end && field >= module_end)) return NAN;
  const char *field_end = strstr(field + 1, "{\"name\":");
  const char *value = strstr(field, ",\"value\":");
  if(!value || (field_end && value >= field_end) || (module_end && value >= module_end)) return NAN;
  char *end;
  const double result = strtod(value + strlen(",\"value\":"), &end);
  return end == value + strlen(",\"value\":") ? NAN : result;
}

static long tone_equalizer_history_position(const char *json)
{
  const char *position = strstr(json, "\"position\":");
  return position ? strtol(position + strlen("\"position\":"), NULL, 10) : -1;
}

/* Original filtered-mask sampling and the desktop smooth curve update, using
 * the real adapter/pixelpipe. This is an RGB fixture, not Android execution. */
static int mobile_tone_equalizer_checks(dt_mobile_session *session)
{
  char *initial = NULL, *baseline = NULL, *changed = NULL, *metadata = NULL;
  char *history = NULL, *after_history = NULL, *instance = NULL, *target_final = NULL;
  uint8_t *before = NULL, *edited = NULL, *restored = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0;
  int edits = 0, success = 0;
  double exposure = NAN, other_exposure = NAN;
  double output_before = NAN, output_after = NAN, output_restored = NAN;
  const char *gains[] = { "noise", "ultra_deep_blacks", "deep_blacks", "blacks", "shadows",
                         "midtones", "highlights", "whites", "speculars" };
  fprintf(stderr, "tone equalizer: original filtered-mask point sampling and grouped edit\n");
  ENGINE_OK(dt_mobile_modules(session, &initial));
  CHECK(module_has(initial, "toneequal", "\"enabled\":false"));
  CHECK(dt_mobile_tone_equalizer_sample(session, "toneequal", .5, .5, &exposure) != DT_MOBILE_OK);
  CHECK(isnan(exposure));
  CHECK(dt_mobile_tone_equalizer_adjust(session, "toneequal", -4, .25) != DT_MOBILE_OK);
  CHECK(dt_mobile_tone_equalizer_output(session, "toneequal", -4, &other_exposure) != DT_MOBILE_OK);
  CHECK(isnan(other_exposure));
  ENGINE_OK(dt_mobile_set_enabled(session, "toneequal", 1)); edits++;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  dt_mobile_free_buffer(before); before = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  ENGINE_OK(dt_mobile_modules(session, &baseline));
  ENGINE_OK(dt_mobile_history(session, &history));
  ENGINE_OK(dt_mobile_tone_equalizer_sample(session, "toneequal", .5, .5, &exposure));
  CHECK(isfinite(exposure));
  ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", exposure, &output_before));
  CHECK(isfinite(output_before));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history));
  dt_mobile_free_buffer(after_history); after_history = NULL;

  const double invalid_coordinates[][2] = { { -.01, .5 }, { 1, .5 }, { .5, -.01 },
    { .5, 1 }, { NAN, .5 }, { .5, INFINITY } };
  for(size_t k = 0; k < sizeof(invalid_coordinates) / sizeof(invalid_coordinates[0]); k++)
  {
    other_exposure = 0;
    CHECK(dt_mobile_tone_equalizer_sample(session, "toneequal", invalid_coordinates[k][0],
      invalid_coordinates[k][1], &other_exposure) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    CHECK(isnan(other_exposure));
  }
  CHECK(dt_mobile_tone_equalizer_sample(session, "exposure", .5, .5, &other_exposure) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_tone_equalizer_sample(session, "toneequal@999", .5, .5, &other_exposure) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_tone_equalizer_adjust(session, "toneequal", NAN, .25) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_tone_equalizer_adjust(session, "toneequal", exposure, INFINITY) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_tone_equalizer_adjust(session, "toneequal", exposure, -2.01) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_tone_equalizer_adjust(session, "toneequal", exposure, 2.01) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_tone_equalizer_adjust(session, "exposure", exposure, .25) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_tone_equalizer_output(session, "toneequal", NAN, &other_exposure) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(isnan(other_exposure));
  CHECK(dt_mobile_tone_equalizer_output(session, "toneequal", INFINITY, &other_exposure) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(isnan(other_exposure));
  CHECK(dt_mobile_tone_equalizer_output(session, "exposure", exposure, &other_exposure) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(isnan(other_exposure));
  CHECK(dt_mobile_tone_equalizer_output(session, "toneequal@999", exposure, &other_exposure) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(isnan(other_exposure));
  CHECK(dt_mobile_tone_equalizer_output(session, "toneequal", exposure, NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, exposure + .1, 0, &other_exposure)
    == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(isnan(other_exposure));
  CHECK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, NAN, 1, &other_exposure)
    == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(isnan(other_exposure));
  CHECK(dt_mobile_tone_equalizer_set_target(session, "toneequal", NAN, 0, 1, &other_exposure)
    == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(isnan(other_exposure));
  CHECK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, exposure + 2.01, 1, &other_exposure)
    == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(isnan(other_exposure));
  CHECK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, exposure - 2.01, 1, &other_exposure)
    == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(isnan(other_exposure));
  CHECK(dt_mobile_tone_equalizer_set_target(session, "exposure", exposure, exposure + .1, 1, &other_exposure)
    == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(isnan(other_exposure));
  ENGINE_OK(dt_mobile_tone_equalizer_adjust(session, "toneequal", exposure, 0));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history));
  dt_mobile_free_buffer(after_history); after_history = NULL;

  fprintf(stderr, "tone equalizer: exact selected-tone targets and gesture history\n");
  ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", exposure, &output_before));
  ENGINE_OK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, output_before + .2, 11, &output_after)); edits++;
  CHECK(fabs(output_after - (output_before + .2)) < .005);
  ENGINE_OK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, output_before + .4, 11, &output_after));
  CHECK(fabs(output_after - (output_before + .4)) < .005);
  ENGINE_OK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, output_before - .2, 11, &output_after));
  CHECK(fabs(output_after - (output_before - .2)) < .005);
  ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", exposure, &output_restored));
  CHECK(output_after == output_restored);
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(tone_equalizer_history_position(after_history) == tone_equalizer_history_position(history) + 1);
  dt_mobile_free_buffer(after_history); after_history = NULL;
  ENGINE_OK(dt_mobile_modules(session, &target_final));
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", exposure, &output_restored));
  CHECK(output_before == output_restored);
  ENGINE_OK(dt_mobile_redo(session)); edits++;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(target_final, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  dt_mobile_free_buffer(target_final); target_final = NULL;
  ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", exposure, &output_restored));
  CHECK(output_after == output_restored);
  ENGINE_OK(dt_mobile_undo(session)); edits--;

  /* Separate taps are separate undo steps, even at the same image point. */
  ENGINE_OK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, output_before + .2, 21, &output_after)); edits++;
  ENGINE_OK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, output_before + .3, 22, &output_after)); edits++;
  CHECK(fabs(output_after - (output_before + .3)) < .005);
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(tone_equalizer_history_position(after_history) == tone_equalizer_history_position(history) + 2);
  dt_mobile_free_buffer(after_history); after_history = NULL;
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_undo(session)); edits--;

  /* Another native edit must end a gesture before its id can be reused. */
  ENGINE_OK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, output_before + .2, 31, &output_after)); edits++;
  ENGINE_OK(dt_mobile_set_parameter(session, "toneequal", "exposure_boost", 1)); edits++;
  ENGINE_OK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, output_before + .3, 31, &output_after)); edits++;
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(tone_equalizer_history_position(after_history) == tone_equalizer_history_position(history) + 3);
  dt_mobile_free_buffer(after_history); after_history = NULL;
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_undo(session)); edits--;

  /* History navigation also ends coalescing, while reads above do not. */
  ENGINE_OK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, output_before + .2, 41, &output_after)); edits++;
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_redo(session)); edits++;
  ENGINE_OK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, output_before + .3, 41, &output_after)); edits++;
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(tone_equalizer_history_position(after_history) == tone_equalizer_history_position(history) + 2);
  dt_mobile_free_buffer(after_history); after_history = NULL;
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_undo(session)); edits--;

  /* Repeating a reached hard boundary must not create another undo item. */
  ENGINE_OK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, exposure + 2, 51, &output_after)); edits++;
  CHECK(fabs(output_after - (exposure + 2)) < .005);
  ENGINE_OK(dt_mobile_history(session, &after_history));
  ENGINE_OK(dt_mobile_tone_equalizer_set_target(session, "toneequal", exposure, exposure + 2, 52, &output_restored));
  CHECK(fabs(output_after - output_restored) < .002);
  ENGINE_OK(dt_mobile_history(session, &metadata));
  CHECK(!strcmp(after_history, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  dt_mobile_free_buffer(after_history); after_history = NULL;
  ENGINE_OK(dt_mobile_undo(session)); edits--;

  /* The processor uses the endpoint bands for mask values outside [-8,0].
   * Smooth editing must use those same bands instead of fading to a silent
   * no-op when a RAW/mask exposure is far outside the interpolation domain. */
  fprintf(stderr, "tone equalizer: bright and dark out-of-domain tones remain adjustable\n");
  const double outside_exposures[] = { 4, 8, -12 };
  for(size_t k = 0; k < sizeof(outside_exposures) / sizeof(outside_exposures[0]); k++)
  {
    ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", outside_exposures[k], &output_before));
    ENGINE_OK(dt_mobile_tone_equalizer_adjust(session, "toneequal", outside_exposures[k], .1)); edits++;
    ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", outside_exposures[k], &output_after));
    CHECK(isfinite(output_after) && output_after - output_before > .03 && output_after - output_before < .2);
    ENGINE_OK(dt_mobile_history(session, &after_history));
    CHECK(tone_equalizer_history_position(after_history) == tone_equalizer_history_position(history) + 1);
    dt_mobile_free_buffer(after_history); after_history = NULL;
    ENGINE_OK(dt_mobile_undo(session)); edits--;
    ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", outside_exposures[k], &output_restored));
    CHECK(output_before == output_restored);
    ENGINE_OK(dt_mobile_redo(session)); edits++;
    ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", outside_exposures[k], &output_restored));
    CHECK(output_after == output_restored);
    ENGINE_OK(dt_mobile_undo(session)); edits--;
  }

  ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", exposure, &output_before));
  ENGINE_OK(dt_mobile_tone_equalizer_adjust(session, "toneequal", exposure, .25)); edits++;
  ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", exposure, &output_after));
  CHECK(isfinite(output_after) && output_after > output_before + .05);
  ENGINE_OK(dt_mobile_modules(session, &changed));
  int changed_gains = 0;
  for(size_t k = 0; k < sizeof(gains) / sizeof(gains[0]); k++)
  {
    const double old_value = tone_equalizer_parameter(baseline, "toneequal", gains[k]);
    const double new_value = tone_equalizer_parameter(changed, "toneequal", gains[k]);
    CHECK(isfinite(old_value) && isfinite(new_value) && new_value >= -2 && new_value <= 2);
    if(old_value != new_value) changed_gains++;
  }
  CHECK(changed_gains >= 2);
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(tone_equalizer_history_position(after_history) == tone_equalizer_history_position(history) + 1);
  dt_mobile_free_buffer(after_history); after_history = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &edited, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(before, edited, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", exposure, &output_restored));
  CHECK(output_before == output_restored);
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(before, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_redo(session)); edits++;
  ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", exposure, &output_restored));
  CHECK(output_after == output_restored);
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(changed, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(edited, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_tone_equalizer_sample(session, "toneequal", .5, .5, &other_exposure));
  CHECK(exposure == other_exposure); /* Its own nine gains do not modify the mask. */
  ENGINE_OK(dt_mobile_undo(session)); edits--;

  ENGINE_OK(dt_mobile_set_parameter(session, "toneequal", "exposure_boost", 1)); edits++;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_tone_equalizer_sample(session, "toneequal", .5, .5, &other_exposure));
  CHECK(isfinite(other_exposure) && fabs(other_exposure - exposure) > .1);
  ENGINE_OK(dt_mobile_undo(session)); edits--;

  /* Reproduce the user-visible no-effect case with a real mask well above
   * 0 EV, then verify actual preview pixels as well as the output readout. */
  fprintf(stderr, "tone equalizer: boosted bright mask changes actual pixels and output tone\n");
  dt_mobile_free_buffer(before); before = NULL;
  dt_mobile_free_buffer(edited); edited = NULL;
  ENGINE_OK(dt_mobile_set_parameter(session, "toneequal", "exposure_boost", 8)); edits++;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  dt_mobile_free_buffer(before); before = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  ENGINE_OK(dt_mobile_tone_equalizer_sample(session, "toneequal", .5, .5, &other_exposure));
  CHECK(isfinite(other_exposure) && other_exposure > 4);
  ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", other_exposure, &output_before));
  ENGINE_OK(dt_mobile_tone_equalizer_adjust(session, "toneequal", other_exposure, .25)); edits++;
  ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", other_exposure, &output_after));
  CHECK(output_after > output_before + .05);
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &edited, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(before, edited, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_tone_equalizer_output(session, "toneequal", other_exposure, &output_restored));
  CHECK(output_before == output_restored);
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(before, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_undo(session)); edits--;

  /* Crop follows tone equalizer in the original default pipeline. Keep the
   * full-height scale identical so the same original 3x3 mask neighborhood is
   * sampled before and after crop; direct normalized-input lookup must fail. */
  fprintf(stderr, "tone equalizer: oriented image and post-module crop coordinates\n");
  ENGINE_OK(dt_mobile_set_parameter(session, "flip", "orientation", 5)); edits++;
  ENGINE_OK(dt_mobile_set_parameter(session, "toneequal", "details", 0)); edits++;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  dt_mobile_free_buffer(restored); restored = NULL;
  double crop_reference = NAN, crop_wrong = NAN;
  ENGINE_OK(dt_mobile_tone_equalizer_sample(session, "toneequal", .375, .5, &crop_reference));
  ENGINE_OK(dt_mobile_tone_equalizer_sample(session, "toneequal", .25, .5, &crop_wrong));
  CHECK(isfinite(crop_reference) && isfinite(crop_wrong) && fabs(crop_reference - crop_wrong) > .1);
  ENGINE_OK(dt_mobile_set_crop(session, "crop", .25f, 0, .75f, 1)); edits++;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(other_width * 2 == width && other_height == height);
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_tone_equalizer_sample(session, "toneequal", .25, .5, &other_exposure));
  CHECK(isfinite(other_exposure) && fabs(crop_reference - other_exposure) < .0001);
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_undo(session)); edits--;

  ENGINE_OK(dt_mobile_new_instance(session, "toneequal", &instance)); edits++;
  CHECK(instance && strcmp(instance, "toneequal"));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_tone_equalizer_sample(session, instance, .5, .5, &other_exposure));
  CHECK(isfinite(other_exposure));
  ENGINE_OK(dt_mobile_tone_equalizer_adjust(session, instance, other_exposure, .25)); edits++;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(tone_equalizer_parameter(metadata, instance, "midtones") != 0);
  for(size_t k = 0; k < sizeof(gains) / sizeof(gains[0]); k++)
    CHECK(tone_equalizer_parameter(metadata, "toneequal", gains[k]) == tone_equalizer_parameter(baseline, "toneequal", gains[k]));
  success = 1;
failed:
  while(edits > 0)
  {
    if(dt_mobile_undo(session) != DT_MOBILE_OK) { success = 0; break; }
    edits--;
  }
  dt_mobile_free_buffer(initial); dt_mobile_free_buffer(baseline);
  dt_mobile_free_buffer(changed); dt_mobile_free_buffer(metadata);
  dt_mobile_free_buffer(history); dt_mobile_free_buffer(after_history);
  dt_mobile_free_buffer(target_final);
  dt_mobile_free_buffer(instance);
  dt_mobile_free_buffer(before); dt_mobile_free_buffer(edited); dt_mobile_free_buffer(restored);
  return success;
}

#endif
