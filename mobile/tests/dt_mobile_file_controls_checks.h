/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_FILE_CONTROLS_CHECKS_H
#define DT_MOBILE_FILE_CONTROLS_CHECKS_H

static int mobile_file_controls_checks(dt_mobile_session *session)
{
  char *initial = NULL, *metadata = NULL, *history = NULL, *after_history = NULL;
  gchar *directory = NULL, *cube = NULL, *raster = NULL, *missing = NULL, *overlay = NULL;
  uint8_t *before = NULL, *after = NULL, *restored = NULL, *inverted = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0, success = 0;
  FILE *file = NULL;
  fprintf(stderr, "file controls: metadata and fixtures\n");
  ENGINE_OK(dt_mobile_modules(session, &initial));
  CHECK(module_has(initial, "rasterfile", "\"name\":\"file.import\""));
  CHECK(module_has(initial, "lut3d", "\"name\":\"file.import\""));
  CHECK(module_has(initial, "overlay", "\"name\":\"file.import\""));
  directory = g_dir_make_tmp("dt-mobile-tool-files-XXXXXX", NULL);
  CHECK(directory);
  cube = g_build_filename(directory, "red.CUBE", NULL);
  raster = g_build_filename(directory, "quarter.pfm", NULL);
  missing = g_build_filename(directory, "missing.cube", NULL);
  overlay = g_build_filename(directory, "overlay.png", NULL);
  CHECK(g_file_set_contents(cube, "TITLE \"mobile original LUT reader\"\nLUT_3D_SIZE 2\n"
    "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n"
    "1 0 0\n1 0 0\n1 0 0\n1 0 0\n1 0 0\n1 0 0\n1 0 0\n1 0 0\n", -1, NULL));
  file = g_fopen(raster, "wb");
  CHECK(file && fputs("PF\n8 8\n-1.0\n", file) >= 0);
  const float quarter = .25f;
  guint32 sample; memcpy(&sample, &quarter, sizeof(sample)); sample = GUINT32_TO_LE(sample);
  for(int index = 0; index < 8 * 8 * 3; index++) CHECK(fwrite(&sample, sizeof(sample), 1, file) == 1);
  const int close_result = fclose(file); file = NULL;
  CHECK(close_result == 0);

  ENGINE_OK(dt_mobile_history(session, &history));
  CHECK(dt_mobile_set_text_parameter(session, "lut3d", "file.import", missing) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_text_parameter(session, "lut3d", "file.import", "relative.cube") == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_text_parameter(session, "rasterfile", "file.import", cube) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_text_parameter(session, "lut3d", "file.import", directory) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_history(session, &after_history));
  CHECK(!strcmp(history, after_history));
  dt_mobile_free_buffer(history); history = NULL;
  dt_mobile_free_buffer(after_history); after_history = NULL;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(initial, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;

  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  dt_mobile_free_buffer(before); before = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  fprintf(stderr, "file controls: import LUT\n");
  ENGINE_OK(dt_mobile_set_text_parameter(session, "lut3d", "file.import", cube));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(module_has(metadata, "lut3d", cube));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &after, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(before, after, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(before, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_redo(session));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(after, restored, (size_t)width * height * 4));
  fprintf(stderr, "file controls: export LUT result as overlay fixture\n");
  ENGINE_OK(dt_mobile_export(session, overlay, "png", 95));
  dt_mobile_free_buffer(before); before = NULL;
  dt_mobile_free_buffer(after); after = NULL;
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_undo(session));

  /* Image overlay import also installs the original image ID; storing only
   * its filename would leave all its displayed transform sliders ineffective. */
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  fprintf(stderr, "file controls: import overlay\n");
  ENGINE_OK(dt_mobile_set_text_parameter(session, "overlay", "file.import", overlay));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &after, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(before, after, (size_t)width * height * 4));
  fprintf(stderr, "file controls: edit overlay and restore history\n");
  ENGINE_OK(dt_mobile_set_parameter(session, "overlay", "opacity", 50));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(after, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(after, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(before, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_redo(session));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(after, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(before); before = NULL;
  dt_mobile_free_buffer(after); after = NULL;
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_undo(session));

  /* Loading a raster file must feed a real consumer, rather than merely store
   * a path on a pipeline module whose normal image output is unchanged. */
  fprintf(stderr, "file controls: import raster mask\n");
  ENGINE_OK(dt_mobile_set_text_parameter(session, "rasterfile", "file.import", raster));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(module_has(metadata, "exposure", "\"name\":\"blend.raster_source.rasterfile.0\""));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 1));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  dt_mobile_free_buffer(before); before = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  fprintf(stderr, "file controls: select raster source\n");
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "blend.raster_source.rasterfile.0", 1));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &after, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(before, after, (size_t)width * height * 4));
  fprintf(stderr, "file controls: invert raster and restore history\n");
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "blend.raster_invert", 1));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &inverted, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(after, inverted, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(after, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(before, restored, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(initial, metadata));
  success = 1;
failed:
  if(file) fclose(file);
  dt_mobile_free_buffer(initial);
  dt_mobile_free_buffer(metadata);
  dt_mobile_free_buffer(history);
  dt_mobile_free_buffer(after_history);
  dt_mobile_free_buffer(before);
  dt_mobile_free_buffer(after);
  dt_mobile_free_buffer(restored);
  dt_mobile_free_buffer(inverted);
  if(cube) g_unlink(cube);
  if(raster) g_unlink(raster);
  if(overlay) g_unlink(overlay);
  if(directory) g_rmdir(directory);
  g_free(cube); g_free(raster); g_free(missing); g_free(overlay); g_free(directory);
  return success;
}

#endif
