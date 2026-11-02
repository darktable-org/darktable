/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Public API checks for a build without a processing engine. Opaque fixture
 * bytes exercise file validation only; they are not a RAW decode test. */
#include "dt_mobile.h"
#include "dt_mobile_export.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
  exit(EXIT_FAILURE); } } while(0)

static void test_arguments(void)
{
  CHECK(dt_mobile_configure_runtime(NULL, "/modules", "/tmp") == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_configure_runtime("", "/modules", "/tmp") == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_configure_runtime("/data", NULL, "/tmp") == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_configure_runtime("/data", "", "/tmp") == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_configure_runtime("/data", "/modules", NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_configure_runtime("/data", "/modules", "") == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(strlen(dt_mobile_runtime_error()) > 0);
  CHECK(dt_mobile_configure_runtime("/data", "/modules", "/tmp") == DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(strstr(dt_mobile_runtime_error(), "engine") != NULL);
  dt_mobile_session *session = (dt_mobile_session *)1;
  CHECK(dt_mobile_open(NULL, &session) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(session == NULL);
  CHECK(strlen(dt_mobile_open_error()) > 0);
  session = (dt_mobile_session *)1;
  CHECK(dt_mobile_open("", &session) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(session == NULL);
  CHECK(dt_mobile_open("unused", NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_open("dt-mobile-no-such-input.gpr", &session) == DT_MOBILE_ERROR_IO);
  CHECK(session == NULL);
  CHECK(strlen(dt_mobile_open_error()) > 0);

  char *json = (char *)1;
  CHECK(dt_mobile_modules(NULL, &json) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(json == NULL);
  json = (char *)1;
  CHECK(dt_mobile_export_formats(NULL, &json) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(json == NULL);
  CHECK(dt_mobile_modules(NULL, NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  json = (char *)1;
  CHECK(dt_mobile_history(NULL, &json) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(json == NULL);
  CHECK(dt_mobile_history(NULL, NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_undo(NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_redo(NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  json = (char *)1;
  CHECK(dt_mobile_new_instance(NULL, "exposure", &json) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(json == NULL);
  CHECK(dt_mobile_new_instance(NULL, "exposure", NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_export_formats(NULL, NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_parameter(NULL, "exposure", "exposure", 1.0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_parameter(NULL, "exposure", "exposure", NAN) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  double sample = 0;
  CHECK(dt_mobile_tone_equalizer_sample(NULL, "toneequal", .5, .5, &sample) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(isnan(sample));
  CHECK(dt_mobile_tone_equalizer_sample(NULL, "toneequal", .5, .5, NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_tone_equalizer_adjust(NULL, "toneequal", -4, .25) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  sample = 0;
  CHECK(dt_mobile_tone_equalizer_output(NULL, "toneequal", -4, &sample) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(isnan(sample));
  CHECK(dt_mobile_tone_equalizer_output(NULL, "toneequal", -4, NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  sample = 0;
  CHECK(dt_mobile_tone_equalizer_set_target(NULL, "toneequal", -4, -3.9, 1, &sample) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(isnan(sample));
  CHECK(dt_mobile_tone_equalizer_set_target(NULL, "toneequal", -4, -3.9, 1, NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_enabled(NULL, "exposure", 1) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  const dt_mobile_curve_point curve[] = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
  CHECK(dt_mobile_set_curve(NULL, "basecurve", 0, 0, curve, 2) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_rgb_curve_options(NULL, "rgbcurve", 0, 1) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  const float whitebalance[] = { 1.0f, 1.0f, 1.0f };
  CHECK(dt_mobile_set_whitebalance_gains(NULL, "temperature", whitebalance, 3) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_whitebalance_temperature(NULL, "temperature", 6500.0, 1.0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_whitebalance_setting(NULL, "temperature", 0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_whitebalance_preset(NULL, "temperature", 6, 0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  json = (char *)1;
  CHECK(dt_mobile_mask_photo_points(NULL, "exposure", &json) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(json == NULL);
  CHECK(dt_mobile_mask_photo_points(NULL, "exposure", NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_mask_place_photo(NULL, "exposure", 1, 0, .5, .5) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_crop(NULL, "crop", 0.0f, 0.0f, 1.0f, 1.0f) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  const int params = 1;
  CHECK(dt_mobile_set_module_params(NULL, "exposure", &params, sizeof(params)) == DT_MOBILE_ERROR_INVALID_ARGUMENT);

  uint8_t *pixels = (uint8_t *)1;
  int width = 7, height = 8;
  CHECK(dt_mobile_render_preview(NULL, 10, 10, &pixels, &width, &height) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(pixels == NULL && width == 0 && height == 0);
  CHECK(dt_mobile_render_preview(NULL, 10, 10, NULL, NULL, NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_export(NULL, "unused.jpg", "jpeg", 90) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  dt_mobile_export_settings settings = DT_MOBILE_EXPORT_SETTINGS_INIT;
  CHECK(settings.struct_size == sizeof(settings));
  CHECK(dt_mobile_export_with_settings(NULL, "unused.jpg", "jpeg", &settings) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_export_with_settings(NULL, NULL, NULL, NULL) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  settings.struct_size = 0;
  CHECK(dt_mobile_export_with_settings(NULL, "unused.jpg", "jpeg", &settings) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(strcmp(dt_mobile_last_error(NULL), "invalid session") == 0);
  dt_mobile_cancel(NULL);
  dt_mobile_close(NULL);
  dt_mobile_free_buffer(NULL);
}

static void test_file_validation(void)
{
  const char *path = "dt-mobile-noengine-opaque-input.bin";
  FILE *input = fopen(path, "wb");
  CHECK(input != NULL);
  CHECK(fclose(input) == 0);
  dt_mobile_session *session = (dt_mobile_session *)1;
  CHECK(dt_mobile_open(path, &session) == DT_MOBILE_ERROR_IO);
  CHECK(session == NULL);
  CHECK(strstr(dt_mobile_open_error(), "truncated") != NULL);

  input = fopen(path, "wb");
  CHECK(input != NULL);
  const unsigned char opaque_bytes[16] = { 0 };
  CHECK(fwrite(opaque_bytes, 1, sizeof(opaque_bytes), input) == sizeof(opaque_bytes));
  CHECK(fclose(input) == 0);
  session = (dt_mobile_session *)1;
  CHECK(dt_mobile_open(path, &session) == DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(session == NULL);
  CHECK(strstr(dt_mobile_open_error(), "engine") != NULL);
  CHECK(remove(path) == 0);
}

int main(void)
{
  CHECK(dt_mobile_capabilities() == 0);
  test_arguments();
  test_file_validation();
  return EXIT_SUCCESS;
}
