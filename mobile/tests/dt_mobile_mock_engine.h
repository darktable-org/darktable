/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Test-only observations for the public API/engine adapter boundary. */
#ifndef DT_MOBILE_MOCK_ENGINE_H
#define DT_MOBILE_MOCK_ENGINE_H
#include "dt_mobile.h"
#include "dt_mobile_export.h"

enum dt_mobile_mock_call {
  DT_MOCK_OPEN, DT_MOCK_MODULES, DT_MOCK_HISTORY, DT_MOCK_UNDO, DT_MOCK_REDO, DT_MOCK_NEW_INSTANCE,
  DT_MOCK_PARAMS, DT_MOCK_PARAMETER,
  DT_MOCK_ENABLED, DT_MOCK_CURVE, DT_MOCK_RGB_CURVE_OPTIONS, DT_MOCK_WHITEBALANCE_GAINS,
  DT_MOCK_WHITEBALANCE_TEMPERATURE, DT_MOCK_WHITEBALANCE_SETTING, DT_MOCK_CROP, DT_MOCK_RENDER, DT_MOCK_FORMATS,
  DT_MOCK_EXPORT_SETTINGS, DT_MOCK_EXPORT,
  DT_MOCK_CANCEL, DT_MOCK_CLOSE, DT_MOCK_CONFIGURE, DT_MOCK_CALL_COUNT
};
struct dt_mobile_mock_observations {
  unsigned calls[DT_MOCK_CALL_COUNT];
  unsigned live_engines;
  char path[256];
  char data_dir[256], module_dir[256], temp_root[256];
  char module[128];
  char parameter[128];
  char format[64];
  unsigned char params[32];
  size_t params_size;
  double value;
  int enabled;
  int curve_channel, curve_interpolation;
  int rgb_curve_mode, rgb_curve_preserve_colors;
  float whitebalance_gains[4];
  size_t whitebalance_count;
  int whitebalance_setting;
  double whitebalance_kelvin, whitebalance_tint;
  dt_mobile_curve_point curve_points[20];
  size_t curve_count;
  float crop_coordinates[4];
  int max_width, max_height, quality;
  dt_mobile_export_settings export_settings;
};
extern struct dt_mobile_mock_observations dt_mobile_mock;
void dt_mobile_mock_fail_next(enum dt_mobile_mock_call operation, dt_mobile_status status);
#endif
