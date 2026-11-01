/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Deterministic test double. Its buffers are synthetic API payloads, never
 * evidence of RAW/GPR decoding, pixelpipe correctness, or real export support. */
#include "darktable_mobile_engine.h"
#include "dt_mobile_mock_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dt_mobile_engine { unsigned marker; };
struct dt_mobile_mock_observations dt_mobile_mock;
static enum dt_mobile_mock_call failure_operation = DT_MOCK_CALL_COUNT;
static dt_mobile_status failure_status;

void dt_mobile_mock_fail_next(enum dt_mobile_mock_call operation, dt_mobile_status status)
{ failure_operation = operation; failure_status = status; }

static dt_mobile_status called(enum dt_mobile_mock_call operation, char *error, size_t size)
{
  dt_mobile_mock.calls[operation]++;
  if(failure_operation == operation)
  {
    failure_operation = DT_MOCK_CALL_COUNT;
    snprintf(error, size, "mock failure %d", (int)operation);
    return failure_status;
  }
  if(error && size) error[0] = '\0';
  return DT_MOBILE_OK;
}

static dt_mobile_status json_result(enum dt_mobile_mock_call operation, const char *payload,
                                   char **json, char *error, size_t size)
{
  const dt_mobile_status status = called(operation, error, size);
  if(status != DT_MOBILE_OK) return status;
  *json = malloc(strlen(payload) + 1);
  if(!*json) return DT_MOBILE_ERROR_OUT_OF_MEMORY;
  strcpy(*json, payload);
  return DT_MOBILE_OK;
}

dt_mobile_status dt_mobile_engine_configure_runtime(const char *data_dir, const char *module_dir,
                                                   const char *temp_root, char *error, size_t size)
{
  const dt_mobile_status status = called(DT_MOCK_CONFIGURE, error, size);
  if(status != DT_MOBILE_OK) return status;
  if(dt_mobile_mock.live_engines)
  {
    snprintf(error, size, "mock runtime has an active image session");
    return DT_MOBILE_ERROR_ENGINE;
  }
  snprintf(dt_mobile_mock.data_dir, sizeof(dt_mobile_mock.data_dir), "%s", data_dir);
  snprintf(dt_mobile_mock.module_dir, sizeof(dt_mobile_mock.module_dir), "%s", module_dir);
  snprintf(dt_mobile_mock.temp_root, sizeof(dt_mobile_mock.temp_root), "%s", temp_root);
  return DT_MOBILE_OK;
}

dt_mobile_status dt_mobile_engine_open(const char *path, dt_mobile_engine **engine,
                                      char *error, size_t size)
{
  snprintf(dt_mobile_mock.path, sizeof(dt_mobile_mock.path), "%s", path);
  const dt_mobile_status status = called(DT_MOCK_OPEN, error, size);
  if(status != DT_MOBILE_OK) return status;
  *engine = malloc(sizeof(**engine));
  if(!*engine) return DT_MOBILE_ERROR_OUT_OF_MEMORY;
  (*engine)->marker = 0xface;
  dt_mobile_mock.live_engines++;
  return DT_MOBILE_OK;
}

dt_mobile_status dt_mobile_engine_modules(dt_mobile_engine *engine, char **json,
                                         char *error, size_t size)
{
  (void)engine;
  return json_result(DT_MOCK_MODULES, "{\"modules\":[{\"operation\":\"mock_exposure\"}]}", json, error, size);
}

dt_mobile_status dt_mobile_engine_export_formats(dt_mobile_engine *engine, char **json,
                                                char *error, size_t size)
{
  (void)engine;
  return json_result(DT_MOCK_FORMATS, "{\"formats\":[{\"id\":\"mock_format\"}]}", json, error, size);
}

dt_mobile_status dt_mobile_engine_history(dt_mobile_engine *engine, char **json,
                                         char *error, size_t size)
{
  (void)engine;
  return json_result(DT_MOCK_HISTORY,
    "{\"canUndo\":true,\"canRedo\":false,\"position\":1,\"length\":1,\"limit\":64}", json, error, size);
}

dt_mobile_status dt_mobile_engine_undo(dt_mobile_engine *engine, char *error, size_t size)
{ (void)engine; return called(DT_MOCK_UNDO, error, size); }

dt_mobile_status dt_mobile_engine_redo(dt_mobile_engine *engine, char *error, size_t size)
{ (void)engine; return called(DT_MOCK_REDO, error, size); }

dt_mobile_status dt_mobile_engine_new_instance(dt_mobile_engine *engine, const char *module,
                                              char **identifier, char *error, size_t size)
{
  (void)engine;
  snprintf(dt_mobile_mock.module, sizeof(dt_mobile_mock.module), "%s", module);
  return json_result(DT_MOCK_NEW_INSTANCE, "mock_exposure@1", identifier, error, size);
}

dt_mobile_status dt_mobile_engine_set_parameter(dt_mobile_engine *engine, const char *module,
                                               const char *parameter, double value,
                                               char *error, size_t size)
{
  (void)engine;
  snprintf(dt_mobile_mock.module, sizeof(dt_mobile_mock.module), "%s", module);
  snprintf(dt_mobile_mock.parameter, sizeof(dt_mobile_mock.parameter), "%s", parameter);
  dt_mobile_mock.value = value;
  return called(DT_MOCK_PARAMETER, error, size);
}

dt_mobile_status dt_mobile_engine_set_text_parameter(dt_mobile_engine *engine, const char *module,
                                                     const char *parameter, const char *value,
                                                     char *error, size_t size)
{
  (void)engine;
  (void)module;
  (void)parameter;
  (void)value;
  (void)error;
  (void)size;
  return DT_MOBILE_ERROR_UNSUPPORTED;
}

dt_mobile_status dt_mobile_engine_tone_equalizer_sample(dt_mobile_engine *engine, const char *module,
                                                       double x, double y, double *exposure,
                                                       char *error, size_t size)
{
  (void)engine; (void)module; (void)x; (void)y; (void)exposure; (void)error; (void)size;
  return DT_MOBILE_ERROR_UNSUPPORTED;
}

dt_mobile_status dt_mobile_engine_tone_equalizer_adjust(dt_mobile_engine *engine, const char *module,
                                                       double exposure, double offset,
                                                       char *error, size_t size)
{
  (void)engine; (void)module; (void)exposure; (void)offset; (void)error; (void)size;
  return DT_MOBILE_ERROR_UNSUPPORTED;
}

dt_mobile_status dt_mobile_engine_set_enabled(dt_mobile_engine *engine, const char *module, int enabled,
                                             char *error, size_t size)
{
  (void)engine;
  snprintf(dt_mobile_mock.module, sizeof(dt_mobile_mock.module), "%s", module);
  dt_mobile_mock.enabled = enabled;
  return called(DT_MOCK_ENABLED, error, size);
}

dt_mobile_status dt_mobile_engine_set_curve(dt_mobile_engine *engine, const char *module, int channel,
                                           int interpolation, const dt_mobile_curve_point *points, size_t count,
                                           char *error, size_t size)
{
  (void)engine;
  const dt_mobile_status status = called(DT_MOCK_CURVE, error, size);
  if(status != DT_MOBILE_OK) return status;
  snprintf(dt_mobile_mock.module, sizeof(dt_mobile_mock.module), "%s", module);
  dt_mobile_mock.curve_channel = channel;
  dt_mobile_mock.curve_interpolation = interpolation;
  dt_mobile_mock.curve_count = count;
  if(count > sizeof(dt_mobile_mock.curve_points) / sizeof(dt_mobile_mock.curve_points[0]))
  {
    snprintf(error, size, "invalid curve count reached mock adapter");
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  }
  memcpy(dt_mobile_mock.curve_points, points, count * sizeof(points[0]));
  return DT_MOBILE_OK;
}

dt_mobile_status dt_mobile_engine_set_rgb_curve_options(dt_mobile_engine *engine,
                                                       const char *module, int mode,
                                                       int preserve_colors,
                                                       char *error, size_t size)
{
  (void)engine;
  const dt_mobile_status status = called(DT_MOCK_RGB_CURVE_OPTIONS, error, size);
  if(status != DT_MOBILE_OK) return status;
  snprintf(dt_mobile_mock.module, sizeof(dt_mobile_mock.module), "%s", module);
  dt_mobile_mock.rgb_curve_mode = mode;
  dt_mobile_mock.rgb_curve_preserve_colors = preserve_colors;
  return DT_MOBILE_OK;
}

dt_mobile_status dt_mobile_engine_set_whitebalance_gains(dt_mobile_engine *engine,
                                                       const char *module,
                                                       const float *gains, size_t count,
                                                       char *error, size_t size)
{
  (void)engine;
  const dt_mobile_status status = called(DT_MOCK_WHITEBALANCE_GAINS, error, size);
  if(status != DT_MOBILE_OK) return status;
  snprintf(dt_mobile_mock.module, sizeof(dt_mobile_mock.module), "%s", module);
  dt_mobile_mock.whitebalance_count = count;
  if(count > 4) return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  memcpy(dt_mobile_mock.whitebalance_gains, gains, count * sizeof(float));
  return DT_MOBILE_OK;
}

dt_mobile_status dt_mobile_engine_set_whitebalance_temperature(dt_mobile_engine *engine,
                                                             const char *module,
                                                             double kelvin, double tint,
                                                             char *error, size_t size)
{
  (void)engine;
  const dt_mobile_status status = called(DT_MOCK_WHITEBALANCE_TEMPERATURE, error, size);
  if(status != DT_MOBILE_OK) return status;
  snprintf(dt_mobile_mock.module, sizeof(dt_mobile_mock.module), "%s", module);
  dt_mobile_mock.whitebalance_kelvin = kelvin;
  dt_mobile_mock.whitebalance_tint = tint;
  return DT_MOBILE_OK;
}

dt_mobile_status dt_mobile_engine_set_whitebalance_setting(dt_mobile_engine *engine,
                                                         const char *module, int setting,
                                                         char *error, size_t size)
{
  (void)engine;
  const dt_mobile_status status = called(DT_MOCK_WHITEBALANCE_SETTING, error, size);
  if(status != DT_MOBILE_OK) return status;
  snprintf(dt_mobile_mock.module, sizeof(dt_mobile_mock.module), "%s", module);
  dt_mobile_mock.whitebalance_setting = setting;
  return DT_MOBILE_OK;
}

dt_mobile_status dt_mobile_engine_set_crop(dt_mobile_engine *engine, const char *module,
                                          float left, float top, float right, float bottom,
                                          char *error, size_t size)
{
  (void)engine;
  const dt_mobile_status status = called(DT_MOCK_CROP, error, size);
  if(status != DT_MOBILE_OK) return status;
  snprintf(dt_mobile_mock.module, sizeof(dt_mobile_mock.module), "%s", module);
  const float coordinates[] = { left, top, right, bottom };
  memcpy(dt_mobile_mock.crop_coordinates, coordinates, sizeof(coordinates));
  return DT_MOBILE_OK;
}

dt_mobile_status dt_mobile_engine_set_module_params(dt_mobile_engine *engine, const char *module,
                                                   const void *params, size_t params_size,
                                                   char *error, size_t size)
{
  (void)engine;
  snprintf(dt_mobile_mock.module, sizeof(dt_mobile_mock.module), "%s", module);
  dt_mobile_mock.params_size = params_size;
  memcpy(dt_mobile_mock.params, params, params_size < sizeof(dt_mobile_mock.params) ? params_size : sizeof(dt_mobile_mock.params));
  return called(DT_MOCK_PARAMS, error, size);
}

dt_mobile_status dt_mobile_engine_render(dt_mobile_engine *engine, int max_width, int max_height,
                                        uint8_t **pixels, int *width, int *height,
                                        char *error, size_t size)
{
  (void)engine;
  dt_mobile_mock.max_width = max_width;
  dt_mobile_mock.max_height = max_height;
  const dt_mobile_status status = called(DT_MOCK_RENDER, error, size);
  if(status != DT_MOBILE_OK) return status;
  const uint8_t payload[4] = { 11, 22, 33, 255 };
  *pixels = malloc(sizeof(payload));
  if(!*pixels) return DT_MOBILE_ERROR_OUT_OF_MEMORY;
  memcpy(*pixels, payload, sizeof(payload));
  *width = *height = 1;
  return DT_MOBILE_OK;
}

dt_mobile_status dt_mobile_engine_export(dt_mobile_engine *engine, const char *path,
                                        const char *format, int quality,
                                        char *error, size_t size)
{
  (void)engine;
  snprintf(dt_mobile_mock.path, sizeof(dt_mobile_mock.path), "%s", path);
  snprintf(dt_mobile_mock.format, sizeof(dt_mobile_mock.format), "%s", format);
  dt_mobile_mock.quality = quality;
  /* No output file is written: this verifies delegation, not an encoder. */
  return called(DT_MOCK_EXPORT, error, size);
}

dt_mobile_status dt_mobile_engine_export_with_settings(dt_mobile_engine *engine, const char *path,
                                                      const char *format,
                                                      const dt_mobile_export_settings *settings,
                                                      char *error, size_t size)
{
  (void)engine;
  snprintf(dt_mobile_mock.path, sizeof(dt_mobile_mock.path), "%s", path);
  snprintf(dt_mobile_mock.format, sizeof(dt_mobile_mock.format), "%s", format);
  dt_mobile_mock.export_settings = *settings;
  /* The adapter boundary records settings without simulating format validation
   * or writing an encoded output. */
  return called(DT_MOCK_EXPORT_SETTINGS, error, size);
}

void dt_mobile_engine_cancel(dt_mobile_engine *engine)
{ (void)engine; dt_mobile_mock.calls[DT_MOCK_CANCEL]++; }

void dt_mobile_engine_close(dt_mobile_engine *engine)
{
  dt_mobile_mock.calls[DT_MOCK_CLOSE]++;
  if(engine) { dt_mobile_mock.live_engines--; free(engine); }
}
