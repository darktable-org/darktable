/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Public API delegation/ownership tests using an explicit test double.
 * Real decoding and export are covered separately by the engine integration test. */
#define _POSIX_C_SOURCE 200809L
#include "dt_mobile.h"
#include "dt_mobile_mock_engine.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
  exit(EXIT_FAILURE); } } while(0)
#define INVALID(call) CHECK((call) == DT_MOBILE_ERROR_INVALID_ARGUMENT)

static void test_invalid_arguments(dt_mobile_session *session)
{
  unsigned before[DT_MOCK_CALL_COUNT];
  memcpy(before, dt_mobile_mock.calls, sizeof(before));
  char *json = (char *)1;
  INVALID(dt_mobile_modules(NULL, &json)); CHECK(json == NULL);
  INVALID(dt_mobile_modules(session, NULL));
  json = (char *)1;
  INVALID(dt_mobile_history(NULL, &json)); CHECK(json == NULL);
  INVALID(dt_mobile_history(session, NULL));
  INVALID(dt_mobile_undo(NULL));
  INVALID(dt_mobile_redo(NULL));
  json = (char *)1;
  INVALID(dt_mobile_new_instance(NULL, "exposure", &json)); CHECK(json == NULL);
  json = (char *)1;
  INVALID(dt_mobile_new_instance(session, NULL, &json)); CHECK(json == NULL);
  json = (char *)1;
  INVALID(dt_mobile_new_instance(session, "", &json)); CHECK(json == NULL);
  INVALID(dt_mobile_new_instance(session, "exposure", NULL));
  json = (char *)1;
  INVALID(dt_mobile_export_formats(NULL, &json)); CHECK(json == NULL);
  INVALID(dt_mobile_export_formats(session, NULL));
  INVALID(dt_mobile_set_parameter(NULL, "exposure", "exposure", 1));
  INVALID(dt_mobile_set_parameter(session, NULL, "exposure", 1));
  INVALID(dt_mobile_set_parameter(session, "", "exposure", 1));
  INVALID(dt_mobile_set_parameter(session, "exposure", NULL, 1));
  INVALID(dt_mobile_set_parameter(session, "exposure", "", 1));
  INVALID(dt_mobile_set_parameter(session, "exposure", "exposure", NAN));
  INVALID(dt_mobile_set_parameter(session, "exposure", "exposure", INFINITY));
  INVALID(dt_mobile_set_parameter(session, "exposure", "exposure", -INFINITY));
  INVALID(dt_mobile_set_enabled(NULL, "exposure", 1));
  INVALID(dt_mobile_set_enabled(session, NULL, 1));
  INVALID(dt_mobile_set_enabled(session, "", 1));
  INVALID(dt_mobile_set_enabled(session, "exposure", -1));
  INVALID(dt_mobile_set_enabled(session, "exposure", 2));
  INVALID(dt_mobile_set_rgb_curve_options(NULL, "rgbcurve", 0, 1));
  INVALID(dt_mobile_set_rgb_curve_options(session, NULL, 0, 1));
  INVALID(dt_mobile_set_rgb_curve_options(session, "", 0, 1));
  INVALID(dt_mobile_set_rgb_curve_options(session, "rgbcurve", -1, 1));
  INVALID(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 2, 1));
  INVALID(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 0, -1));
  INVALID(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 0, 7));
  INVALID(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 0, INT_MAX));
  float gains[] = { 1.0f, 1.0f, 1.0f, 1.0f };
  INVALID(dt_mobile_set_whitebalance_gains(NULL, "temperature", gains, 3));
  INVALID(dt_mobile_set_whitebalance_gains(session, NULL, gains, 3));
  INVALID(dt_mobile_set_whitebalance_gains(session, "", gains, 3));
  INVALID(dt_mobile_set_whitebalance_gains(session, "temperature", NULL, 3));
  INVALID(dt_mobile_set_whitebalance_gains(session, "temperature", gains, 0));
  INVALID(dt_mobile_set_whitebalance_gains(session, "temperature", gains, 2));
  INVALID(dt_mobile_set_whitebalance_gains(session, "temperature", gains, 5));
  INVALID(dt_mobile_set_whitebalance_gains(session, "temperature", gains, SIZE_MAX));
  const float invalid_gains[] = { NAN, INFINITY, -INFINITY, -1.0f, 0.0f, 0.0009f, 8.01f };
  for(size_t k = 0; k < sizeof(invalid_gains) / sizeof(invalid_gains[0]); k++)
  {
    gains[2] = invalid_gains[k];
    INVALID(dt_mobile_set_whitebalance_gains(session, "temperature", gains, 3));
  }
  gains[2] = nextafterf(0.001f, 0.0f);
  INVALID(dt_mobile_set_whitebalance_gains(session, "temperature", gains, 3));
  gains[2] = 1.0f;
  INVALID(dt_mobile_set_whitebalance_setting(NULL, "temperature", 0));
  INVALID(dt_mobile_set_whitebalance_setting(session, NULL, 0));
  INVALID(dt_mobile_set_whitebalance_setting(session, "", 0));
  const int invalid_settings[] = { -1, 1, 2, 5, INT_MAX };
  for(size_t k = 0; k < sizeof(invalid_settings) / sizeof(invalid_settings[0]); k++)
    INVALID(dt_mobile_set_whitebalance_setting(session, "temperature", invalid_settings[k]));
  INVALID(dt_mobile_set_whitebalance_temperature(NULL, "temperature", 6500.0, 1.0));
  INVALID(dt_mobile_set_whitebalance_temperature(session, NULL, 6500.0, 1.0));
  INVALID(dt_mobile_set_whitebalance_temperature(session, "", 6500.0, 1.0));
  const double invalid_kelvin[] = { NAN, INFINITY, -INFINITY, 1900.0, 25001.0,
                                    nextafter(1901.0, 0.0), nextafter(25000.0, INFINITY) };
  for(size_t k = 0; k < sizeof(invalid_kelvin) / sizeof(invalid_kelvin[0]); k++)
    INVALID(dt_mobile_set_whitebalance_temperature(session, "temperature", invalid_kelvin[k], 1.0));
  const double invalid_tint[] = { NAN, INFINITY, -INFINITY, 0.0,
                                  nextafter(0.135, 0.0), nextafter(2.326, INFINITY) };
  for(size_t k = 0; k < sizeof(invalid_tint) / sizeof(invalid_tint[0]); k++)
    INVALID(dt_mobile_set_whitebalance_temperature(session, "temperature", 6500.0, invalid_tint[k]));
  dt_mobile_curve_point curve[] = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
  INVALID(dt_mobile_set_curve(NULL, "basecurve", 0, 0, curve, 2));
  INVALID(dt_mobile_set_curve(session, NULL, 0, 0, curve, 2));
  INVALID(dt_mobile_set_curve(session, "", 0, 0, curve, 2));
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 0, NULL, 2));
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 0, curve, 0));
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 0, curve, 1));
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 0, curve, 21));
  INVALID(dt_mobile_set_curve(session, "basecurve", -1, 0, curve, 2));
  INVALID(dt_mobile_set_curve(session, "basecurve", 3, 0, curve, 2));
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, -1, curve, 2));
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 3, curve, 2));
  curve[0].x = -0.01f;
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 0, curve, 2));
  curve[0].x = 0.0f; curve[1].x = 1.01f;
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 0, curve, 2));
  curve[1].x = 1.0f; curve[0].y = -0.01f;
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 0, curve, 2));
  curve[0].y = 0.0f; curve[1].y = 1.01f;
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 0, curve, 2));
  curve[1].y = 1.0f; curve[0].x = NAN;
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 0, curve, 2));
  curve[0].x = 0.0f; curve[1].y = INFINITY;
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 0, curve, 2));
  curve[1].y = 1.0f; curve[1].x = 0.0f;
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 0, curve, 2));
  curve[0].x = 1.0f; curve[1].x = 0.5f;
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 0, curve, 2));
  curve[0].x = 0.0f; curve[1].x = 0.5e-6f;
  INVALID(dt_mobile_set_curve(session, "basecurve", 0, 0, curve, 2));
  INVALID(dt_mobile_set_crop(NULL, "crop", 0.0f, 0.0f, 1.0f, 1.0f));
  INVALID(dt_mobile_set_crop(session, NULL, 0.0f, 0.0f, 1.0f, 1.0f));
  INVALID(dt_mobile_set_crop(session, "", 0.0f, 0.0f, 1.0f, 1.0f));
  static const float invalid_crops[][4] = {
    { -0.1f, 0.0f, 1.0f, 1.0f }, { 0.0f, -0.1f, 1.0f, 1.0f },
    { 0.0f, 0.0f, 1.1f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.1f },
    { 1.1f, 0.0f, 1.0f, 1.0f }, { 0.0f, 1.1f, 1.0f, 1.0f },
    { 0.0f, 0.0f, -0.1f, 1.0f }, { 0.0f, 0.0f, 1.0f, -0.1f },
    { NAN, 0.0f, 1.0f, 1.0f }, { 0.0f, NAN, 1.0f, 1.0f },
    { 0.0f, 0.0f, NAN, 1.0f }, { 0.0f, 0.0f, 1.0f, NAN },
    { INFINITY, 0.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, -INFINITY, 1.0f },
    { 0.5f, 0.5f, 0.5f, 1.0f }, { 0.5f, 0.5f, 1.0f, 0.5f },
    { 0.7f, 0.0f, 0.6f, 1.0f }, { 0.0f, 0.7f, 1.0f, 0.6f },
    /* The preceding float below 0.51f is smaller than the desktop minimum edge. */
    { 0.5f, 0.0f, 0x1.051eb6p-1f, 1.0f }, { 0.0f, 0.5f, 1.0f, 0x1.051eb6p-1f },
  };
  for(size_t k = 0; k < sizeof(invalid_crops) / sizeof(invalid_crops[0]); k++)
    INVALID(dt_mobile_set_crop(session, "crop", invalid_crops[k][0], invalid_crops[k][1],
                               invalid_crops[k][2], invalid_crops[k][3]));
  const unsigned char params[] = { 0, 1, 2, 255 };
  INVALID(dt_mobile_set_module_params(NULL, "exposure", params, sizeof(params)));
  INVALID(dt_mobile_set_module_params(session, NULL, params, sizeof(params)));
  INVALID(dt_mobile_set_module_params(session, "", params, sizeof(params)));
  INVALID(dt_mobile_set_module_params(session, "exposure", NULL, sizeof(params)));
  INVALID(dt_mobile_set_module_params(session, "exposure", params, 0));

  uint8_t *pixels = (uint8_t *)1;
  int width = 7, height = 8;
  INVALID(dt_mobile_render_preview(NULL, 10, 10, &pixels, &width, &height));
  CHECK(pixels == NULL && width == 0 && height == 0);
  pixels = (uint8_t *)1; width = 7; height = 8;
  INVALID(dt_mobile_render_preview(session, 0, 10, &pixels, &width, &height));
  CHECK(pixels == NULL && width == 0 && height == 0);
  INVALID(dt_mobile_render_preview(session, 10, -1, &pixels, &width, &height));
  width = 7; height = 8;
  INVALID(dt_mobile_render_preview(session, 10, 10, NULL, &width, &height));
  CHECK(width == 0 && height == 0);
  pixels = (uint8_t *)1; height = 8;
  INVALID(dt_mobile_render_preview(session, 10, 10, &pixels, NULL, &height));
  CHECK(pixels == NULL && height == 0);
  pixels = (uint8_t *)1; width = 7;
  INVALID(dt_mobile_render_preview(session, 10, 10, &pixels, &width, NULL));
  CHECK(pixels == NULL && width == 0);
  INVALID(dt_mobile_export(NULL, "out", "mock_format", 50));
  INVALID(dt_mobile_export(session, NULL, "mock_format", 50));
  INVALID(dt_mobile_export(session, "", "mock_format", 50));
  INVALID(dt_mobile_export(session, "out", NULL, 50));
  INVALID(dt_mobile_export(session, "out", "", 50));
  INVALID(dt_mobile_export(session, "out", "mock_format", -1));
  INVALID(dt_mobile_export(session, "out", "mock_format", 101));
  dt_mobile_export_settings settings = DT_MOBILE_EXPORT_SETTINGS_INIT;
  INVALID(dt_mobile_export_with_settings(NULL, "out", "mock_format", &settings));
  INVALID(dt_mobile_export_with_settings(session, NULL, "mock_format", &settings));
  INVALID(dt_mobile_export_with_settings(session, "", "mock_format", &settings));
  INVALID(dt_mobile_export_with_settings(session, "out", NULL, &settings));
  INVALID(dt_mobile_export_with_settings(session, "out", "", &settings));
  INVALID(dt_mobile_export_with_settings(session, "out", "mock_format", NULL));
  const size_t invalid_sizes[] = { 0, sizeof(settings) - 1, sizeof(settings) + 1, SIZE_MAX };
  for(size_t k = 0; k < sizeof(invalid_sizes) / sizeof(invalid_sizes[0]); k++)
  {
    settings.struct_size = invalid_sizes[k];
    INVALID(dt_mobile_export_with_settings(session, "out", "mock_format", &settings));
  }
  settings.struct_size = sizeof(settings);
  settings.max_width = (uint32_t)INT_MAX + 1u;
  INVALID(dt_mobile_export_with_settings(session, "out", "mock_format", &settings));
  settings.max_width = 0; settings.max_height = (uint32_t)INT_MAX + 1u;
  INVALID(dt_mobile_export_with_settings(session, "out", "mock_format", &settings));
  settings.max_width = settings.max_height = UINT32_MAX;
  INVALID(dt_mobile_export_with_settings(session, "out", "mock_format", &settings));
  CHECK(memcmp(before, dt_mobile_mock.calls, sizeof(before)) == 0);
}

static void test_source_protection(dt_mobile_session *session, const char *source)
{
  const unsigned before = dt_mobile_mock.calls[DT_MOCK_EXPORT];
  const unsigned settings_before = dt_mobile_mock.calls[DT_MOCK_EXPORT_SETTINGS];
  const dt_mobile_export_settings settings = DT_MOBILE_EXPORT_SETTINGS_INIT;
  INVALID(dt_mobile_export(session, source, "mock_format", 50));
  CHECK(strstr(dt_mobile_last_error(session), "original source") != NULL);
  INVALID(dt_mobile_export_with_settings(session, source, "mock_format", &settings));
  CHECK(strstr(dt_mobile_last_error(session), "original source") != NULL);
  const char *hardlink_path = "dt-mobile-mock-source-hardlink.bin";
  const char *symlink_path = "dt-mobile-mock-source-symlink.bin";
  CHECK(link(source, hardlink_path) == 0);
  INVALID(dt_mobile_export(session, hardlink_path, "mock_format", 50));
  CHECK(strstr(dt_mobile_last_error(session), "original source") != NULL);
  INVALID(dt_mobile_export_with_settings(session, hardlink_path, "mock_format", &settings));
  CHECK(strstr(dt_mobile_last_error(session), "original source") != NULL);
  CHECK(symlink(source, symlink_path) == 0);
  INVALID(dt_mobile_export(session, symlink_path, "mock_format", 50));
  CHECK(strstr(dt_mobile_last_error(session), "original source") != NULL);
  INVALID(dt_mobile_export_with_settings(session, symlink_path, "mock_format", &settings));
  CHECK(strstr(dt_mobile_last_error(session), "original source") != NULL);
  CHECK(dt_mobile_mock.calls[DT_MOCK_EXPORT] == before);
  CHECK(dt_mobile_mock.calls[DT_MOCK_EXPORT_SETTINGS] == settings_before);
  FILE *input = fopen(source, "rb"); CHECK(input != NULL);
  unsigned char contents[17];
  CHECK(fread(contents, 1, sizeof(contents), input) == 16);
  for(size_t i = 0; i < 16; i++) CHECK(contents[i] == 0);
  CHECK(fclose(input) == 0);
  CHECK(remove(hardlink_path) == 0 && remove(symlink_path) == 0);
}

static void check_export_settings(const dt_mobile_export_settings *expected)
{
  const dt_mobile_export_settings *actual = &dt_mobile_mock.export_settings;
  CHECK(actual->struct_size == expected->struct_size);
  CHECK(actual->max_width == expected->max_width && actual->max_height == expected->max_height);
  CHECK(actual->quality == expected->quality && actual->pixel_format == expected->pixel_format);
  CHECK(actual->png_compression == expected->png_compression);
  CHECK(actual->tiff_compression == expected->tiff_compression);
  CHECK(actual->tiff_compression_level == expected->tiff_compression_level);
  CHECK(actual->jpeg_subsample == expected->jpeg_subsample && actual->dpi == expected->dpi);
  CHECK(actual->output_profile == expected->output_profile);
  CHECK(actual->rendering_intent == expected->rendering_intent);
  CHECK(actual->metadata_policy == expected->metadata_policy);
}

static void test_export_settings_delegation(dt_mobile_session *session)
{
  const unsigned legacy_before = dt_mobile_mock.calls[DT_MOCK_EXPORT];
  const unsigned before = dt_mobile_mock.calls[DT_MOCK_EXPORT_SETTINGS];
  dt_mobile_export_settings settings = DT_MOBILE_EXPORT_SETTINGS_INIT;
  /* Zero bounds and each format-default sentinel must reach the real adapter. */
  CHECK(dt_mobile_export_with_settings(session, "mock-settings-never-written", "mock_format", &settings)
        == DT_MOBILE_OK);
  CHECK(strcmp(dt_mobile_mock.path, "mock-settings-never-written") == 0);
  CHECK(strcmp(dt_mobile_mock.format, "mock_format") == 0);
  check_export_settings(&settings);
  settings.max_width = 2048; settings.max_height = 1536;
  settings.quality = 87; settings.pixel_format = DT_MOBILE_EXPORT_UINT16;
  settings.png_compression = 7; settings.tiff_compression = 2;
  settings.tiff_compression_level = 6; settings.jpeg_subsample = 3;
  settings.dpi = 300; settings.output_profile = DT_MOBILE_EXPORT_PROFILE_DISPLAY_P3;
  settings.rendering_intent = DT_MOBILE_EXPORT_INTENT_RELATIVE;
  settings.metadata_policy = DT_MOBILE_EXPORT_METADATA_SOURCE_NONE;
  CHECK(dt_mobile_export_with_settings(session, "mock-settings-never-written", "mock_format", &settings)
        == DT_MOBILE_OK);
  check_export_settings(&settings);
  settings.quality = 91;
  CHECK(dt_mobile_mock.export_settings.quality == 87);
  /* Preserve the signed adapter boundary and independent unrestricted axes. */
  settings.max_width = INT_MAX; settings.max_height = 0;
  CHECK(dt_mobile_export_with_settings(session, "mock-settings-never-written", "mock_format", &settings)
        == DT_MOBILE_OK);
  check_export_settings(&settings);
  settings.max_width = 0; settings.max_height = INT_MAX;
  CHECK(dt_mobile_export_with_settings(session, "mock-settings-never-written", "mock_format", &settings)
        == DT_MOBILE_OK);
  check_export_settings(&settings);
  settings.max_width = INT_MAX;
  CHECK(dt_mobile_export_with_settings(session, "mock-settings-never-written", "mock_format", &settings)
        == DT_MOBILE_OK);
  check_export_settings(&settings);
  CHECK(dt_mobile_mock.calls[DT_MOCK_EXPORT_SETTINGS] == before + 5);
  CHECK(dt_mobile_mock.calls[DT_MOCK_EXPORT] == legacy_before);
}

static void test_delegation_and_buffers(dt_mobile_session *session)
{
  char *first = NULL, *second = NULL, *formats = NULL;
  CHECK(dt_mobile_modules(session, &first) == DT_MOBILE_OK);
  CHECK(strstr(first, "mock_exposure") != NULL);
  CHECK(dt_mobile_modules(session, &second) == DT_MOBILE_OK);
  CHECK(first != second && strcmp(first, second) == 0);
  first[0] = '['; CHECK(second[0] == '{');
  CHECK(dt_mobile_export_formats(session, &formats) == DT_MOBILE_OK);
  CHECK(strstr(formats, "mock_format") != NULL && second[0] == '{');
  dt_mobile_free_buffer(first); dt_mobile_free_buffer(second); dt_mobile_free_buffer(formats);
  char *history = NULL;
  CHECK(dt_mobile_history(session, &history) == DT_MOBILE_OK);
  CHECK(history && strstr(history, "\"position\":1"));
  dt_mobile_free_buffer(history);
  CHECK(dt_mobile_undo(session) == DT_MOBILE_OK);
  CHECK(dt_mobile_redo(session) == DT_MOBILE_OK);
  CHECK(dt_mobile_mock.calls[DT_MOCK_UNDO] == 1 && dt_mobile_mock.calls[DT_MOCK_REDO] == 1);
  char operation[] = "exposure@2";
  char *instance = (char *)1, *other_instance = NULL;
  CHECK(dt_mobile_new_instance(session, operation, &instance) == DT_MOBILE_OK);
  CHECK(!strcmp(dt_mobile_mock.module, "exposure@2"));
  CHECK(instance && !strcmp(instance, "mock_exposure@1"));
  operation[0] = 'x';
  CHECK(!strcmp(dt_mobile_mock.module, "exposure@2"));
  CHECK(dt_mobile_new_instance(session, "exposure", &other_instance) == DT_MOBILE_OK);
  CHECK(instance != other_instance && !strcmp(instance, other_instance));
  instance[0] = 'x';
  CHECK(!strcmp(other_instance, "mock_exposure@1"));
  dt_mobile_free_buffer(instance); dt_mobile_free_buffer(other_instance);
  CHECK(dt_mobile_set_parameter(session, "exposure", "exposure", -2.75) == DT_MOBILE_OK);
  CHECK(strcmp(dt_mobile_mock.module, "exposure") == 0);
  CHECK(strcmp(dt_mobile_mock.parameter, "exposure") == 0 && dt_mobile_mock.value == -2.75);
  CHECK(dt_mobile_set_enabled(session, "filmicrgb", 0) == DT_MOBILE_OK);
  CHECK(strcmp(dt_mobile_mock.module, "filmicrgb") == 0 && dt_mobile_mock.enabled == 0);
  CHECK(dt_mobile_set_enabled(session, "filmicrgb", 1) == DT_MOBILE_OK);
  CHECK(dt_mobile_mock.enabled == 1);
  float gains[] = { 0.001f, 1.0f, 8.0f, 2.0f };
  CHECK(dt_mobile_set_whitebalance_gains(session, "temperature", gains, 3) == DT_MOBILE_OK);
  CHECK(!strcmp(dt_mobile_mock.module, "temperature") && dt_mobile_mock.whitebalance_count == 3);
  CHECK(!memcmp(dt_mobile_mock.whitebalance_gains, gains, 3 * sizeof(float)));
  CHECK(dt_mobile_set_whitebalance_gains(session, "temperature", gains, 4) == DT_MOBILE_OK);
  CHECK(dt_mobile_mock.whitebalance_count == 4 && !memcmp(dt_mobile_mock.whitebalance_gains, gains, sizeof(gains)));
  gains[0] = 1.0f;
  CHECK(dt_mobile_mock.whitebalance_gains[0] == 0.001f);
  CHECK(dt_mobile_set_whitebalance_temperature(session, "temperature@2", 6504.5, 1.234) == DT_MOBILE_OK);
  CHECK(!strcmp(dt_mobile_mock.module, "temperature@2"));
  CHECK(dt_mobile_mock.whitebalance_kelvin == 6504.5 && dt_mobile_mock.whitebalance_tint == 1.234);
  CHECK(dt_mobile_set_whitebalance_temperature(session, "temperature", 1901.0, 0.135) == DT_MOBILE_OK);
  CHECK(dt_mobile_set_whitebalance_temperature(session, "temperature", 25000.0, 2.326) == DT_MOBILE_OK);
  const int settings[] = { 0, 3, 4 };
  for(size_t k = 0; k < sizeof(settings) / sizeof(settings[0]); k++)
  {
    CHECK(dt_mobile_set_whitebalance_setting(session, "temperature", settings[k]) == DT_MOBILE_OK);
    CHECK(dt_mobile_mock.whitebalance_setting == settings[k]);
  }
  for(int preserve = 0; preserve <= 6; preserve++)
  {
    CHECK(dt_mobile_set_rgb_curve_options(session, "rgbcurve@2", preserve % 2, preserve) == DT_MOBILE_OK);
    CHECK(strcmp(dt_mobile_mock.module, "rgbcurve@2") == 0);
    CHECK(dt_mobile_mock.rgb_curve_mode == preserve % 2 && dt_mobile_mock.rgb_curve_preserve_colors == preserve);
  }
  dt_mobile_curve_point curve[] = { { 0.0f, 0.0f }, { 0.4f, 0.6f }, { 1.0f, 1.0f } };
  CHECK(dt_mobile_set_curve(session, "rgbcurve@2", 2, 1, curve, 3) == DT_MOBILE_OK);
  CHECK(strcmp(dt_mobile_mock.module, "rgbcurve@2") == 0);
  CHECK(dt_mobile_mock.curve_channel == 2 && dt_mobile_mock.curve_interpolation == 1);
  CHECK(dt_mobile_mock.curve_count == 3);
  CHECK(memcmp(dt_mobile_mock.curve_points, curve, sizeof(curve)) == 0);
  curve[1].y = 0.2f;
  CHECK(dt_mobile_mock.curve_points[1].y == 0.6f);
  /* Exercise both public count limits. The test double only checks forwarding;
   * interpolation and module/channel validity require real engine tests. */
  const dt_mobile_curve_point minimum[] = { { 0.0f, 0.0f }, { 1e-6f, 1.0f } };
  CHECK(dt_mobile_set_curve(session, "basecurve", 0, 0, minimum, 2) == DT_MOBILE_OK);
  CHECK(dt_mobile_mock.curve_count == 2 && dt_mobile_mock.curve_interpolation == 0);
  dt_mobile_curve_point maximum[20];
  for(size_t k = 0; k < 20; k++) maximum[k].x = maximum[k].y = (float)k / 19.0f;
  CHECK(dt_mobile_set_curve(session, "rgbcurve", 1, 2, maximum, 20) == DT_MOBILE_OK);
  CHECK(dt_mobile_mock.curve_count == 20 && dt_mobile_mock.curve_interpolation == 2);
  CHECK(memcmp(dt_mobile_mock.curve_points, maximum, sizeof(maximum)) == 0);
  CHECK(dt_mobile_set_crop(session, "crop@2", 0.1f, 0.2f, 0.8f, 0.9f) == DT_MOBILE_OK);
  CHECK(strcmp(dt_mobile_mock.module, "crop@2") == 0);
  const float crop[] = { 0.1f, 0.2f, 0.8f, 0.9f };
  CHECK(memcmp(dt_mobile_mock.crop_coordinates, crop, sizeof(crop)) == 0);
  /* Minimum edges match float addition used by the desktop widgets. */
  CHECK(dt_mobile_set_crop(session, "crop", 0.5f, 0.5f, 0.51f, 0.51f) == DT_MOBILE_OK);
  CHECK(dt_mobile_set_crop(session, "crop", 0.0f, 0.0f, 1.0f, 1.0f) == DT_MOBILE_OK);
  const unsigned char params[] = { 0, 1, 2, 255 };
  CHECK(dt_mobile_set_module_params(session, "colorbalancergb", params, sizeof(params)) == DT_MOBILE_OK);
  CHECK(strcmp(dt_mobile_mock.module, "colorbalancergb") == 0);
  CHECK(dt_mobile_mock.params_size == sizeof(params));
  CHECK(memcmp(dt_mobile_mock.params, params, sizeof(params)) == 0);
  uint8_t *pixels = NULL;
  int width = 0, height = 0;
  CHECK(dt_mobile_render_preview(session, 640, 480, &pixels, &width, &height) == DT_MOBILE_OK);
  CHECK(dt_mobile_mock.max_width == 640 && dt_mobile_mock.max_height == 480);
  CHECK(width == 1 && height == 1 && pixels != NULL);
  CHECK(pixels[0] == 11 && pixels[1] == 22 && pixels[2] == 33 && pixels[3] == 255);
  dt_mobile_free_buffer(pixels);
  CHECK(dt_mobile_export(session, "mock-output-never-written", "mock_format", 0) == DT_MOBILE_OK);
  CHECK(strcmp(dt_mobile_mock.path, "mock-output-never-written") == 0);
  CHECK(strcmp(dt_mobile_mock.format, "mock_format") == 0 && dt_mobile_mock.quality == 0);
  CHECK(dt_mobile_export(session, "mock-output-never-written", "mock_format", 100) == DT_MOBILE_OK);
  CHECK(dt_mobile_mock.quality == 100);
  test_export_settings_delegation(session);
  for(int op = DT_MOCK_MODULES; op <= DT_MOCK_EXPORT; op++) CHECK(dt_mobile_mock.calls[op] > 0);
}

static void check_error(dt_mobile_session *session, enum dt_mobile_mock_call operation)
{
  char expected[64];
  snprintf(expected, sizeof(expected), "mock failure %d", (int)operation);
  CHECK(strcmp(dt_mobile_last_error(session), expected) == 0);
}

static void test_engine_errors(dt_mobile_session *session)
{
  char *json = (char *)1;
  dt_mobile_mock_fail_next(DT_MOCK_MODULES, DT_MOBILE_ERROR_ENGINE);
  CHECK(dt_mobile_modules(session, &json) == DT_MOBILE_ERROR_ENGINE);
  CHECK(json == NULL); check_error(session, DT_MOCK_MODULES);
  json = (char *)1;
  dt_mobile_mock_fail_next(DT_MOCK_HISTORY, DT_MOBILE_ERROR_ENGINE);
  CHECK(dt_mobile_history(session, &json) == DT_MOBILE_ERROR_ENGINE);
  CHECK(json == NULL); check_error(session, DT_MOCK_HISTORY);
  dt_mobile_mock_fail_next(DT_MOCK_UNDO, DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_undo(session) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  check_error(session, DT_MOCK_UNDO);
  dt_mobile_mock_fail_next(DT_MOCK_REDO, DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_redo(session) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  check_error(session, DT_MOCK_REDO);
  const dt_mobile_status instance_errors[] = {
    DT_MOBILE_ERROR_INVALID_ARGUMENT, DT_MOBILE_ERROR_UNSUPPORTED, DT_MOBILE_ERROR_ENGINE,
    DT_MOBILE_ERROR_OUT_OF_MEMORY, DT_MOBILE_ERROR_CANCELLED
  };
  for(size_t k = 0; k < sizeof(instance_errors) / sizeof(instance_errors[0]); k++)
  {
    json = (char *)1;
    dt_mobile_mock_fail_next(DT_MOCK_NEW_INSTANCE, instance_errors[k]);
    CHECK(dt_mobile_new_instance(session, "exposure@2", &json) == instance_errors[k]);
    CHECK(json == NULL); check_error(session, DT_MOCK_NEW_INSTANCE);
  }
  json = (char *)1;
  dt_mobile_mock_fail_next(DT_MOCK_FORMATS, DT_MOBILE_ERROR_OUT_OF_MEMORY);
  CHECK(dt_mobile_export_formats(session, &json) == DT_MOBILE_ERROR_OUT_OF_MEMORY);
  CHECK(json == NULL); check_error(session, DT_MOCK_FORMATS);
  dt_mobile_mock_fail_next(DT_MOCK_PARAMETER, DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_parameter(session, "unsupported", "value", 2) == DT_MOBILE_ERROR_UNSUPPORTED);
  check_error(session, DT_MOCK_PARAMETER);
  dt_mobile_mock_fail_next(DT_MOCK_ENABLED, DT_MOBILE_ERROR_PROCESSING);
  CHECK(dt_mobile_set_enabled(session, "exposure", 1) == DT_MOBILE_ERROR_PROCESSING);
  check_error(session, DT_MOCK_ENABLED);
  const dt_mobile_curve_point curve[] = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
  dt_mobile_mock_fail_next(DT_MOCK_CURVE, DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_curve(session, "unknown@2", 0, 0, curve, 2) == DT_MOBILE_ERROR_UNSUPPORTED);
  check_error(session, DT_MOCK_CURVE);
  dt_mobile_mock_fail_next(DT_MOCK_RGB_CURVE_OPTIONS, DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_rgb_curve_options(session, "unknown@2", 0, 1) == DT_MOBILE_ERROR_UNSUPPORTED);
  check_error(session, DT_MOCK_RGB_CURVE_OPTIONS);
  const float gains[] = { 1.0f, 1.0f, 1.0f };
  dt_mobile_mock_fail_next(DT_MOCK_WHITEBALANCE_GAINS, DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_whitebalance_gains(session, "unknown@2", gains, 3) == DT_MOBILE_ERROR_UNSUPPORTED);
  check_error(session, DT_MOCK_WHITEBALANCE_GAINS);
  dt_mobile_mock_fail_next(DT_MOCK_WHITEBALANCE_TEMPERATURE, DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_whitebalance_temperature(session, "temperature", 6500.0, 1.0) == DT_MOBILE_ERROR_UNSUPPORTED);
  check_error(session, DT_MOCK_WHITEBALANCE_TEMPERATURE);
  dt_mobile_mock_fail_next(DT_MOCK_WHITEBALANCE_SETTING, DT_MOBILE_ERROR_PROCESSING);
  CHECK(dt_mobile_set_whitebalance_setting(session, "temperature", 3) == DT_MOBILE_ERROR_PROCESSING);
  check_error(session, DT_MOCK_WHITEBALANCE_SETTING);
  dt_mobile_mock_fail_next(DT_MOCK_CROP, DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_crop(session, "unknown@2", 0.0f, 0.0f, 1.0f, 1.0f) == DT_MOBILE_ERROR_UNSUPPORTED);
  check_error(session, DT_MOCK_CROP);
  const int params = 123;
  dt_mobile_mock_fail_next(DT_MOCK_PARAMS, DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_module_params(session, "exposure", &params, sizeof(params)) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  check_error(session, DT_MOCK_PARAMS);
  uint8_t *pixels = (uint8_t *)1;
  int width = 7, height = 8;
  dt_mobile_mock_fail_next(DT_MOCK_RENDER, DT_MOBILE_ERROR_DECODE);
  CHECK(dt_mobile_render_preview(session, 10, 10, &pixels, &width, &height) == DT_MOBILE_ERROR_DECODE);
  CHECK(pixels == NULL && width == 0 && height == 0); check_error(session, DT_MOCK_RENDER);
  dt_mobile_mock_fail_next(DT_MOCK_EXPORT, DT_MOBILE_ERROR_IO);
  CHECK(dt_mobile_export(session, "unused", "mock_format", 50) == DT_MOBILE_ERROR_IO);
  check_error(session, DT_MOCK_EXPORT);
  const dt_mobile_export_settings settings = DT_MOBILE_EXPORT_SETTINGS_INIT;
  const dt_mobile_status export_errors[] = {
    DT_MOBILE_ERROR_INVALID_ARGUMENT, DT_MOBILE_ERROR_UNSUPPORTED, DT_MOBILE_ERROR_IO,
    DT_MOBILE_ERROR_PROCESSING, DT_MOBILE_ERROR_OUT_OF_MEMORY, DT_MOBILE_ERROR_CANCELLED
  };
  for(size_t k = 0; k < sizeof(export_errors) / sizeof(export_errors[0]); k++)
  {
    dt_mobile_mock_fail_next(DT_MOCK_EXPORT_SETTINGS, export_errors[k]);
    CHECK(dt_mobile_export_with_settings(session, "unused", "mock_format", &settings) == export_errors[k]);
    check_error(session, DT_MOCK_EXPORT_SETTINGS);
  }
  CHECK(dt_mobile_export_with_settings(session, "unused", "mock_format", &settings) == DT_MOBILE_OK);
  CHECK(dt_mobile_last_error(session)[0] == '\0');
  CHECK(dt_mobile_set_parameter(session, "exposure", "exposure", 0) == DT_MOBILE_OK);
}

static void test_cancel(dt_mobile_session *session)
{
  unsigned before[DT_MOCK_CALL_COUNT];
  memcpy(before, dt_mobile_mock.calls, sizeof(before));
  dt_mobile_cancel(session);
  CHECK(dt_mobile_mock.calls[DT_MOCK_CANCEL] == before[DT_MOCK_CANCEL] + 1);
  char *json = (char *)1;
  CHECK(dt_mobile_modules(session, &json) == DT_MOBILE_ERROR_CANCELLED); CHECK(json == NULL);
  json = (char *)1;
  CHECK(dt_mobile_history(session, &json) == DT_MOBILE_ERROR_CANCELLED); CHECK(json == NULL);
  CHECK(dt_mobile_undo(session) == DT_MOBILE_ERROR_CANCELLED);
  CHECK(dt_mobile_redo(session) == DT_MOBILE_ERROR_CANCELLED);
  json = (char *)1;
  CHECK(dt_mobile_new_instance(session, "exposure", &json) == DT_MOBILE_ERROR_CANCELLED);
  CHECK(json == NULL);
  json = (char *)1;
  CHECK(dt_mobile_export_formats(session, &json) == DT_MOBILE_ERROR_CANCELLED); CHECK(json == NULL);
  CHECK(dt_mobile_set_parameter(session, "exposure", "exposure", 0) == DT_MOBILE_ERROR_CANCELLED);
  CHECK(dt_mobile_set_enabled(session, "exposure", 1) == DT_MOBILE_ERROR_CANCELLED);
  const dt_mobile_curve_point curve[] = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
  CHECK(dt_mobile_set_curve(session, "basecurve", 0, 0, curve, 2) == DT_MOBILE_ERROR_CANCELLED);
  CHECK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 1, 1) == DT_MOBILE_ERROR_CANCELLED);
  const float gains[] = { 1.0f, 1.0f, 1.0f };
  CHECK(dt_mobile_set_whitebalance_gains(session, "temperature", gains, 3) == DT_MOBILE_ERROR_CANCELLED);
  CHECK(dt_mobile_set_whitebalance_setting(session, "temperature", 4) == DT_MOBILE_ERROR_CANCELLED);
  CHECK(dt_mobile_set_whitebalance_temperature(session, "temperature", 6500.0, 1.0) == DT_MOBILE_ERROR_CANCELLED);
  CHECK(dt_mobile_set_crop(session, "crop", 0.0f, 0.0f, 1.0f, 1.0f) == DT_MOBILE_ERROR_CANCELLED);
  const int params = 123;
  CHECK(dt_mobile_set_module_params(session, "exposure", &params, sizeof(params)) == DT_MOBILE_ERROR_CANCELLED);
  uint8_t *pixels = (uint8_t *)1;
  int width = 7, height = 8;
  CHECK(dt_mobile_render_preview(session, 10, 10, &pixels, &width, &height) == DT_MOBILE_ERROR_CANCELLED);
  CHECK(pixels == NULL && width == 0 && height == 0);
  CHECK(dt_mobile_export(session, "unused", "mock_format", 50) == DT_MOBILE_ERROR_CANCELLED);
  const dt_mobile_export_settings settings = DT_MOBILE_EXPORT_SETTINGS_INIT;
  CHECK(dt_mobile_export_with_settings(session, "unused", "mock_format", &settings) == DT_MOBILE_ERROR_CANCELLED);
  CHECK(strstr(dt_mobile_last_error(session), "cancelled") != NULL);
  for(int op = DT_MOCK_MODULES; op <= DT_MOCK_EXPORT; op++) CHECK(dt_mobile_mock.calls[op] == before[op]);
}

int main(void)
{
  CHECK(dt_mobile_capabilities() == DT_MOBILE_CAP_ENGINE);
  const unsigned initial_configures = dt_mobile_mock.calls[DT_MOCK_CONFIGURE];
  INVALID(dt_mobile_configure_runtime(NULL, "/modules", "/temporary"));
  INVALID(dt_mobile_configure_runtime("/data", "", "/temporary"));
  INVALID(dt_mobile_configure_runtime("/data", "/modules", NULL));
  CHECK(dt_mobile_mock.calls[DT_MOCK_CONFIGURE] == initial_configures);
  dt_mobile_mock_fail_next(DT_MOCK_CONFIGURE, DT_MOBILE_ERROR_IO);
  CHECK(dt_mobile_configure_runtime("/data", "/modules", "/temporary") == DT_MOBILE_ERROR_IO);
  CHECK(strstr(dt_mobile_runtime_error(), "mock failure") != NULL);
  CHECK(dt_mobile_configure_runtime("/data", "/modules", "/temporary") == DT_MOBILE_OK);
  CHECK(strcmp(dt_mobile_mock.data_dir, "/data") == 0);
  CHECK(strcmp(dt_mobile_mock.module_dir, "/modules") == 0);
  CHECK(strcmp(dt_mobile_mock.temp_root, "/temporary") == 0);
  CHECK(strstr(dt_mobile_runtime_error(), "mock failure") == NULL);
  const char *path = "dt-mobile-mock-opaque-input.bin";
  FILE *input = fopen(path, "wb"); CHECK(input != NULL);
  const unsigned char opaque_bytes[16] = { 0 };
  CHECK(fwrite(opaque_bytes, 1, sizeof(opaque_bytes), input) == sizeof(opaque_bytes));
  CHECK(fclose(input) == 0);
  dt_mobile_session *session = (dt_mobile_session *)1;
  dt_mobile_mock_fail_next(DT_MOCK_OPEN, DT_MOBILE_ERROR_DECODE);
  CHECK(dt_mobile_open(path, &session) == DT_MOBILE_ERROR_DECODE);
  CHECK(session == NULL && dt_mobile_mock.live_engines == 0);
  CHECK(strstr(dt_mobile_open_error(), "mock failure") != NULL);
  CHECK(dt_mobile_open(path, &session) == DT_MOBILE_OK);
  CHECK(session != NULL && dt_mobile_mock.live_engines == 1);
  CHECK(dt_mobile_configure_runtime("/new-data", "/new-modules", "/temporary") == DT_MOBILE_ERROR_ENGINE);
  CHECK(strstr(dt_mobile_runtime_error(), "active") != NULL);
  CHECK(strcmp(dt_mobile_mock.data_dir, "/data") == 0);
  CHECK(strcmp(dt_mobile_mock.path, path) == 0);
  CHECK(strstr(dt_mobile_open_error(), "mock failure") == NULL);
  test_invalid_arguments(session);
  test_source_protection(session, path);
  test_delegation_and_buffers(session);
  test_engine_errors(session);
  char *retained_json = NULL;
  CHECK(dt_mobile_modules(session, &retained_json) == DT_MOBILE_OK);
  char *retained_formats = NULL;
  CHECK(dt_mobile_export_formats(session, &retained_formats) == DT_MOBILE_OK);
  char *retained_instance = NULL;
  CHECK(dt_mobile_new_instance(session, "exposure", &retained_instance) == DT_MOBILE_OK);
  uint8_t *retained_pixels = NULL;
  int width = 0, height = 0;
  CHECK(dt_mobile_render_preview(session, 1, 1, &retained_pixels, &width, &height) == DT_MOBILE_OK);
  test_cancel(session);
  dt_mobile_close(session);
  CHECK(dt_mobile_mock.calls[DT_MOCK_CLOSE] == 1 && dt_mobile_mock.live_engines == 0);
  CHECK(strstr(retained_json, "mock_exposure") != NULL);
  CHECK(strstr(retained_formats, "mock_format") != NULL);
  CHECK(retained_instance && !strcmp(retained_instance, "mock_exposure@1"));
  CHECK(retained_pixels[0] == 11 && retained_pixels[3] == 255);
  dt_mobile_free_buffer(retained_json);
  dt_mobile_free_buffer(retained_formats);
  dt_mobile_free_buffer(retained_instance);
  dt_mobile_free_buffer(retained_pixels);
  /* Cancellation belongs to the closed session, not the next one. */
  CHECK(dt_mobile_open(path, &session) == DT_MOBILE_OK);
  CHECK(dt_mobile_set_enabled(session, "exposure", 1) == DT_MOBILE_OK);
  const dt_mobile_export_settings settings = DT_MOBILE_EXPORT_SETTINGS_INIT;
  CHECK(dt_mobile_export_with_settings(session, "unused", "mock_format", &settings) == DT_MOBILE_OK);
  dt_mobile_close(session);
  CHECK(dt_mobile_mock.calls[DT_MOCK_CLOSE] == 2 && dt_mobile_mock.live_engines == 0);
  CHECK(dt_mobile_configure_runtime("/new-data", "/new-modules", "/temporary") == DT_MOBILE_OK);
  CHECK(strcmp(dt_mobile_mock.data_dir, "/new-data") == 0);
  dt_mobile_cancel(NULL); dt_mobile_close(NULL); dt_mobile_free_buffer(NULL);
  CHECK(remove(path) == 0);
  return EXIT_SUCCESS;
}
