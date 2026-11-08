/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_LENS_CHECKS_H
#define DT_MOBILE_LENS_CHECKS_H

/* Use actual installed Lensfun calibration and the original pixelpipe. The
 * camera/lens names below belong to the pinned database, not invented image
 * formulas. Unknown-body coverage guards the missing crop-factor commit. */
static int mobile_lens_checks(dt_mobile_session *session)
{
  char *initial = NULL, *baseline = NULL, *metadata = NULL, *history = NULL, *after_history = NULL;
  uint8_t *before = NULL, *changed = NULL, *restored = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0, edits = 0, success = 0;
#define LENS_SET(name, value) do { ENGINE_OK(dt_mobile_set_parameter(session, "lens", name, value)); edits++; } while(0)
#define LENS_TEXT(name, value) do { ENGINE_OK(dt_mobile_set_text_parameter(session, "lens", name, value)); edits++; } while(0)
  /* Exercise the production capability classifier with explicit opcode-field
   * fixtures. These establish radial/CA interpretation, not HERO12 RAW output. */
  typedef int (*dng_capability_function)(const dt_image_correction_data_t *);
  dng_capability_function capabilities = NULL;
  dt_iop_module_so_t *plugin = dt_iop_get_module_so("lens");
  CHECK(plugin && g_module_symbol(plugin->module, "dt_iop_mobile_lens_dng_capabilities", (gpointer *)&capabilities));
  dt_image_correction_data_t opcode = { 0 };
  CHECK(capabilities(NULL) == 0 && capabilities(&opcode) == 0);
  opcode.dng.has_warp = TRUE; opcode.dng.planes = 3;
  for(int plane = 0; plane < 3; plane++) opcode.dng.cwarp[plane][0] = 1;
  CHECK(capabilities(&opcode) == 0);
  opcode.dng.cwarp[0][0] = 1.001f; opcode.dng.cwarp[2][0] = .999f;
  CHECK(capabilities(&opcode) == 1); /* TCA only; green has no geometry warp. */
  opcode.dng.cwarp[1][1] = .1f;
  CHECK(capabilities(&opcode) == 5);
  for(int plane = 0; plane < 3; plane++)
  { opcode.dng.cwarp[plane][0] = 1; opcode.dng.cwarp[plane][1] = .1f; }
  CHECK(capabilities(&opcode) == 4);
  opcode.dng.has_vignette = TRUE;
  CHECK(capabilities(&opcode) == 4); /* An identity vignette tag is not a correction. */
  opcode.dng.cvig[0] = .2f;
  CHECK(capabilities(&opcode) == 6);
  opcode.dng.planes = 2;
  CHECK(capabilities(&opcode) == 2);
  opcode.dng.planes = 1; opcode.dng.cwarp[0][1] = 0;
  opcode.dng.cwarp[0][4] = .5f;
  CHECK(capabilities(&opcode) == 2); /* Tangential-only terms are not consumed. */
  opcode.dng.cwarp[0][0] = NAN; opcode.dng.cvig[2] = INFINITY;
  CHECK(capabilities(&opcode) == 0);
  fprintf(stderr, "lens controls: image capability and manual vignette\n");
  ENGINE_OK(dt_mobile_modules(session, &initial));
  CHECK(module_has(initial, "lens", "\"embeddedAvailable\":false"));
  CHECK(module_has(initial, "lens", "\"databaseAvailable\":true"));
  CHECK(!module_has(initial, "lens", "\"name\":\"cor_dist_ft\""));
  CHECK(!module_has(initial, "lens", "\"name\":\"tca_r\""));
  ENGINE_OK(dt_mobile_history(session, &history));
  CHECK(dt_mobile_set_parameter(session, "lens", "method", 0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_parameter(session, "lens", "cor_dist_ft", .2) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(initial, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history));
  dt_mobile_free_buffer(history); history = NULL;
  dt_mobile_free_buffer(after_history); after_history = NULL;

  LENS_SET("method", 2);
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  dt_mobile_free_buffer(before); before = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  ENGINE_OK(dt_mobile_modules(session, &baseline));
  LENS_SET("v_strength", .8);
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
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  dt_mobile_free_buffer(before); before = NULL;
  dt_mobile_free_buffer(changed); changed = NULL;
  dt_mobile_free_buffer(baseline); baseline = NULL;

  fprintf(stderr, "lens controls: original Lensfun distortion, vignette and TCA\n");
  LENS_SET("method", 1);
  /* The updated official database must be usable through the same profile
   * actions as the Android picker, not merely present as packaged XML. */
  const char *gopro_cameras[] = { "Hero10 black", "Hero11 Black" };
  const char *gopro_lenses[] = { "HERO10 Black & compatibles", "HERO11 Black & compatibles" };
  for(size_t index = 0; index < sizeof(gopro_cameras) / sizeof(gopro_cameras[0]); index++)
  {
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    const int available = module_has(metadata, "lens", gopro_cameras[index]);
    dt_mobile_free_buffer(metadata); metadata = NULL;
    /* Desktop adapter CI may use the distribution's older calibration data.
     * The pinned-mobile-database validation explicitly requires both pairs. */
    CHECK(available || !getenv("DT_MOBILE_TEST_REQUIRE_UPDATED_LENSFUN"));
    if(!available)
    {
      fprintf(stderr, "lens controls: %s absent from host database\n", gopro_cameras[index]);
      continue;
    }
    fprintf(stderr, "lens controls: updated database %s\n", gopro_cameras[index]);
    LENS_TEXT("lens.camera_profile", gopro_cameras[index]);
    LENS_TEXT("lens.profile", gopro_lenses[index]);
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    CHECK(module_has(metadata, "lens", "\"cameraMatched\":true"));
    CHECK(module_has(metadata, "lens", "\"lensMatched\":true"));
    CHECK(module_has(metadata, "lens", "\"status\":\"ready\""));
    CHECK(module_has(metadata, "lens", "\"supportedFlags\":4"));
    CHECK(parameter_equals(metadata, "lens", "focal", 2.7f));
    dt_mobile_free_buffer(metadata); metadata = NULL;
    ENGINE_OK(dt_mobile_undo(session)); edits--;
    ENGINE_OK(dt_mobile_undo(session)); edits--;
  }
  /* Compatible fixed-lens profiles need not share the body's model name.
   * This relation is explicitly present in the pinned database's mounts. */
  LENS_TEXT("lens.camera_profile", "HERO5 Black");
  LENS_TEXT("lens.profile", "HERO4 black");
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(module_has(metadata, "lens", "\"cameraMatched\":true"));
  CHECK(module_has(metadata, "lens", "\"camera\":\"HERO5 Black\""));
  CHECK(module_has(metadata, "lens", "\"lens\":\"HERO4 black\""));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  LENS_TEXT("lens.camera_profile", "Canon EOS 5D Mark II");
  LENS_TEXT("lens.profile", "Canon EF 24-105mm f/4L IS USM");
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(module_has(metadata, "lens", "\"cameraMatched\":true"));
  CHECK(module_has(metadata, "lens", "\"lensMatched\":true"));
  CHECK(module_has(metadata, "lens", "\"supportedFlags\":7"));
  CHECK(parameter_equals(metadata, "lens", "focal", 24));
  CHECK(parameter_equals(metadata, "lens", "aperture", 4));
  CHECK(!module_has(metadata, "lens", "\"name\":\"crop\""));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  LENS_SET("modify_flags", 0);
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  dt_mobile_free_buffer(before); before = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  ENGINE_OK(dt_mobile_modules(session, &baseline));
  const int corrections[] = { 4, 2, 1 };
  for(size_t index = 0; index < sizeof(corrections) / sizeof(corrections[0]); index++)
  {
    LENS_SET("modify_flags", corrections[index]);
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
    dt_mobile_free_buffer(changed); changed = NULL;
  }
  dt_mobile_free_buffer(baseline); baseline = NULL;

  fprintf(stderr, "lens controls: unknown body retains crop and real correction\n");
  LENS_TEXT("camera", "HERO12 Black");
  ENGINE_OK(dt_mobile_modules(session, &baseline));
  CHECK(module_has(baseline, "lens", "\"cameraMatched\":false"));
  CHECK(module_has(baseline, "lens", "\"lensMatched\":true"));
  CHECK(module_has(baseline, "lens", "\"status\":\"unverified_camera\""));
  CHECK(module_has(baseline, "lens", "\"camera\":\"\""));
  CHECK(module_has(baseline, "lens", "\"requestedCamera\":\"HERO12 Black\""));
  CHECK(module_has(baseline, "lens", "\"lenses\":[]"));
  CHECK(parameter_equals(baseline, "lens", "crop", 1));
  LENS_SET("modify_flags", 4);
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &changed, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(before, changed, (size_t)width * height * 4));
  dt_mobile_free_buffer(changed); changed = NULL;
  ENGINE_OK(dt_mobile_undo(session)); edits--;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_history(session, &history));
  CHECK(dt_mobile_set_text_parameter(session, "lens", "lens.profile", "Canon EF 24-105mm f/4L IS USM")
    == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_text_parameter(session, "lens", "lens.profile", "Not a real calibrated lens")
    == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_text_parameter(session, "lens", "lens.camera_profile", "Not a real camera")
    == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_parameter(session, "lens", "crop", 0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_parameter(session, "lens", "focal", 0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_parameter(session, "lens", "target_geom", 0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_parameter(session, "lens", "v_strength", NAN) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history));
  while(edits > 0) { ENGINE_OK(dt_mobile_undo(session)); edits--; }
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(initial, metadata));
  success = 1;
failed:
  dt_mobile_free_buffer(initial); dt_mobile_free_buffer(baseline); dt_mobile_free_buffer(metadata);
  dt_mobile_free_buffer(history); dt_mobile_free_buffer(after_history);
  dt_mobile_free_buffer(before); dt_mobile_free_buffer(changed); dt_mobile_free_buffer(restored);
#undef LENS_SET
#undef LENS_TEXT
  return success;
}
#endif
