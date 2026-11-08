/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_PARAMETRIC_CHECKS_H
#define DT_MOBILE_PARAMETRIC_CHECKS_H

#include "develop/blend.h"

/* Runs the original RGB/Lab blend-if processor. It verifies activation, actual
 * threshold pixels, neutral union channels, exact history and atomic rejection
 * of invalid handles/refinement modes rather than duplicating mask formulas. */
static int mobile_parametric_checks(dt_mobile_session *session)
{
  char *initial = NULL, *baseline = NULL, *metadata = NULL, *history = NULL, *after_history = NULL;
  uint8_t *before = NULL, *changed = NULL, *restored = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0, edits = 0, success = 0;
#define PM_SET(operation, name, value) do { ENGINE_OK(dt_mobile_set_parameter(session, operation, name, value)); edits++; } while(0)
  ENGINE_OK(dt_mobile_modules(session, &initial));
  CHECK(module_has(initial, "exposure", "\"name\":\"blend.parametric.enabled\""));
  CHECK(!module_has(initial, "exposure", "\"name\":\"blend.parametric.0.p0\""));
  PM_SET("exposure", "exposure", 1);
  PM_SET("exposure", "blend.parametric.enabled", 1);
  ENGINE_OK(dt_mobile_modules(session, &baseline));
  CHECK(parameter_equals(baseline, "exposure", "blend.parametric.enabled", 1));
  CHECK(module_has(baseline, "exposure", "\"name\":\"blend.parametric.0.p0\""));
  CHECK(module_has(baseline, "exposure", "\"name\":\"blend.parametric.4.p0\""));
  CHECK(module_has(baseline, "exposure", "\"name\":\"blend.parametric.14.p3\""));
  CHECK(module_has(baseline, "exposure", "\"name\":\"blend.mask.feather_guide\""));
  CHECK(!module_has(baseline, "exposure", "\"name\":\"blend.mask.details\""));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  dt_mobile_free_buffer(before); before = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  PM_SET("exposure", "blend.parametric.0.p1", .4);
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(parameter_equals(metadata, "exposure", "blend.parametric.0.enabled", 1));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &changed, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(before, changed, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(before, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_redo(session)); edits++;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(changed, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  dt_mobile_free_buffer(baseline); baseline = NULL;
  ENGINE_OK(dt_mobile_modules(session, &baseline));
  ENGINE_OK(dt_mobile_history(session, &history));
  const struct { const char *name; double value; } invalid[] = {
    { "blend.parametric.enabled", .5 }, { "blend.parametric.0.p0", .5 },
    { "blend.parametric.0.p1", -.1 }, { "blend.parametric.0.p2", .3 },
    { "blend.parametric.0.p3", .2 }, { "blend.parametric.0.p4", .5 },
    { "blend.parametric.0.p1", NAN }, { "blend.parametric.00.p1", .4 },
    { "blend.parametric.11.p1", .4 }, { "blend.parametric.15.enabled", 1 },
    { "blend.parametric.combine", 4 }, { "blend.parametric.combine", 1.5 },
    { "blend.parametric.0.boost_ev", 19 }, { "blend.parametric.10.boost_ev", 1 },
    { "blend.parametric.0.inverted", 2 }, { "blend.parametric.0.reset", 0 },
    { "blend.mask.feather_guide", 3 }, { "blend.mask.feather_radius", 251 },
    { "blend.mask.blur_radius", -1 }, { "blend.mask.brightness", -1.01 },
    { "blend.mask.contrast", 1.01 }, { "blend.mask.details", .5 }
  };
  for(size_t k = 0; k < sizeof(invalid) / sizeof(invalid[0]); k++)
    CHECK(dt_mobile_set_parameter(session, "exposure", invalid[k].name, invalid[k].value)
      == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history));

  /* An inclusive mask with one active channel must not be canceled by any
   * unused full-span channel. Both modes select the same single channel. */
  PM_SET("exposure", "blend.parametric.combine", DEVELOP_COMBINE_INCL);
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(changed, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  PM_SET("exposure", "blend.parametric.combine", DEVELOP_COMBINE_INV_INCL);
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(changed, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  PM_SET("exposure", "blend.parametric.combine", DEVELOP_COMBINE_NORM_EXCL);

  /* Boost changes the normalized handles while retaining their physical
   * cutoff, following the original desktop slider callback. */
  PM_SET("exposure", "blend.parametric.0.boost_ev", 1);
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(parameter_equals(metadata, "exposure", "blend.parametric.0.p1", (double).2f));
  CHECK(parameter_equals(metadata, "exposure", "blend.parametric.0.boost_ev", 1));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(changed, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  PM_SET("exposure", "blend.parametric.0.reset", 1);
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(parameter_equals(metadata, "exposure", "blend.parametric.0.p1", 0));
  CHECK(parameter_equals(metadata, "exposure", "blend.parametric.0.enabled", 0));
  CHECK(parameter_equals(metadata, "exposure", "blend.parametric.0.boost_ev", 0));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  PM_SET("exposure", "blend.parametric.4.p2", .35);
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(before, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  PM_SET("exposure", "blend.mask.feather_radius", 10);
  PM_SET("exposure", "blend.mask.feather_guide", DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR);
  PM_SET("exposure", "blend.mask.blur_radius", 3);
  PM_SET("exposure", "blend.mask.brightness", .3);
  PM_SET("exposure", "blend.mask.contrast", .2);
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  dt_mobile_free_buffer(restored); restored = NULL;

  /* The Lab editor must use its own channel membership and hue has no boost. */
  PM_SET("colisa", "blend.parametric.enabled", 1);
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(module_has(metadata, "colisa", "\"name\":\"blend.parametric.2.p0\""));
  CHECK(module_has(metadata, "colisa", "\"name\":\"blend.parametric.13.p3\""));
  CHECK(!module_has(metadata, "colisa", "\"name\":\"blend.parametric.3.p0\""));
  CHECK(!module_has(metadata, "colisa", "\"name\":\"blend.parametric.10.p0\""));
  CHECK(!module_has(metadata, "colisa", "\"name\":\"blend.parametric.9.boost_ev\""));
  CHECK(dt_mobile_set_parameter(session, "colisa", "blend.parametric.3.p0", .2) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  dt_mobile_free_buffer(metadata); metadata = NULL;
  while(edits > 0) { ENGINE_OK(dt_mobile_undo(session)); edits--; }
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(initial, metadata));
  success = 1;
failed:
  dt_mobile_free_buffer(initial); dt_mobile_free_buffer(baseline); dt_mobile_free_buffer(metadata);
  dt_mobile_free_buffer(history); dt_mobile_free_buffer(after_history);
  dt_mobile_free_buffer(before); dt_mobile_free_buffer(changed); dt_mobile_free_buffer(restored);
#undef PM_SET
  return success;
}

#endif
