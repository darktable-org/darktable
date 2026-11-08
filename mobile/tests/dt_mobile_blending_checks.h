/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_BLENDING_CHECKS_H
#define DT_MOBILE_BLENDING_CHECKS_H

/* Runs against the real original pixelpipe, not a replacement blending math.
 * Invalid requests must preserve both metadata and the native history window;
 * a changed opacity must affect pixels and survive exact undo/redo. */
static int mobile_blending_checks(dt_mobile_session *session)
{
  char *initial = NULL, *baseline = NULL, *metadata = NULL, *history = NULL, *after_history = NULL;
  uint8_t *before = NULL, *changed = NULL, *restored = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0;
  int success = 0;
  ENGINE_OK(dt_mobile_modules(session, &initial));
  CHECK(module_has(initial, "exposure", "\"name\":\"blend.opacity\""));
  CHECK(module_has(initial, "exposure", "\"name\":\"blend.mode\""));
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 1));
  ENGINE_OK(dt_mobile_modules(session, &baseline));
  ENGINE_OK(dt_mobile_history(session, &history));
  const struct { const char *name; double value; } invalid[] = {
    { "blend.opacity", -1 }, { "blend.opacity", 101 }, { "blend.opacity", NAN },
    { "blend.opacity", INFINITY }, { "blend.mode", 255 }, { "blend.mode", 1.5 },
    { "blend.reverse", 2 }, { "blend.enabled", .5 }, { "blend.unknown", 0 },
  };
  for(size_t k = 0; k < sizeof(invalid) / sizeof(invalid[0]); k++)
    CHECK(dt_mobile_set_parameter(session, "exposure", invalid[k].name, invalid[k].value)
            == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history));
  dt_mobile_free_buffer(after_history); after_history = NULL;
  dt_mobile_free_buffer(history); history = NULL;
  /* Prime the independently documented first-render path before comparing. */
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  dt_mobile_free_buffer(before); before = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "blend.opacity", 0));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(parameter_equals(metadata, "exposure", "blend.enabled", 1));
  CHECK(parameter_equals(metadata, "exposure", "blend.opacity", 0));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &changed, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(before, changed, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(before, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_redo(session));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(changed, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(initial, metadata));
  success = 1;
failed:
  dt_mobile_free_buffer(initial);
  dt_mobile_free_buffer(baseline);
  dt_mobile_free_buffer(metadata);
  dt_mobile_free_buffer(history);
  dt_mobile_free_buffer(after_history);
  dt_mobile_free_buffer(before);
  dt_mobile_free_buffer(changed);
  dt_mobile_free_buffer(restored);
  return success;
}

#endif
