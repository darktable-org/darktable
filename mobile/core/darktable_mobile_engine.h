/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "dt_mobile.h"
typedef struct dt_mobile_engine dt_mobile_engine;
dt_mobile_status dt_mobile_engine_configure_runtime(const char *data_dir, const char *module_dir,
                                                   const char *temp_root, char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_open(const char *path, dt_mobile_engine **engine,
                                      char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_modules(dt_mobile_engine *engine, char **json,
                                         char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_history(dt_mobile_engine *engine, char **json,
                                         char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_undo(dt_mobile_engine *engine, char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_redo(dt_mobile_engine *engine, char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_new_instance(dt_mobile_engine *engine, const char *module,
                                               char **new_identifier, char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_set_module_params(dt_mobile_engine *engine, const char *module,
                                                   const void *params, size_t size,
                                                   char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_set_parameter(dt_mobile_engine *engine, const char *module,
                                               const char *parameter, double value,
                                               char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_set_text_parameter(dt_mobile_engine *engine, const char *module,
                                                     const char *parameter, const char *value,
                                                     char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_set_curve(dt_mobile_engine *engine, const char *module,
                                           int channel, int interpolation,
                                           const dt_mobile_curve_point *points, size_t count,
                                           char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_set_rgb_curve_options(dt_mobile_engine *engine,
                                                       const char *module, int mode,
                                                       int preserve_colors,
                                                       char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_set_whitebalance_gains(dt_mobile_engine *engine,
                                                       const char *module,
                                                       const float *gains, size_t count,
                                                       char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_set_whitebalance_temperature(dt_mobile_engine *engine,
                                                             const char *module,
                                                             double kelvin, double tint,
                                                             char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_set_whitebalance_setting(dt_mobile_engine *engine,
                                                         const char *module, int setting,
                                                         char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_set_crop(dt_mobile_engine *engine, const char *module,
                                          float left, float top, float right, float bottom,
                                          char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_set_enabled(dt_mobile_engine *engine, const char *module, int enabled,
                                             char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_tone_equalizer_sample(dt_mobile_engine *engine, const char *module,
                                                        double x, double y, double *exposure,
                                                        char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_tone_equalizer_adjust(dt_mobile_engine *engine, const char *module,
                                                        double exposure, double offset,
                                                        char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_render(dt_mobile_engine *engine, int max_width, int max_height,
                                        uint8_t **pixels, int *width, int *height,
                                        char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_export_formats(dt_mobile_engine *engine, char **json,
                                                char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_export(dt_mobile_engine *engine, const char *path,
                                        const char *format, int quality,
                                        char *error, size_t error_size);
dt_mobile_status dt_mobile_engine_export_with_settings(dt_mobile_engine *engine,
                                                      const char *path,
                                                      const char *format,
                                                      const dt_mobile_export_settings *settings,
                                                      char *error, size_t error_size);
void dt_mobile_engine_cancel(dt_mobile_engine *engine);
void dt_mobile_engine_close(dt_mobile_engine *engine);
