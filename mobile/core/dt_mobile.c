/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dt_mobile.h"
#include "dt_mobile_crop_validation.h"
#include <stdatomic.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
#include "darktable_mobile_engine.h"
#endif
#ifdef DT_MOBILE_HAS_GPR
#include "dt_mobile_gpr.h"
#endif
struct dt_mobile_session {
  char *path;
#ifdef DT_MOBILE_HAS_GPR
  char *prepared_path;
#endif
  atomic_bool cancelled;
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  dt_mobile_engine *engine;
#endif
  char error[256];
};
static _Thread_local char open_error[256];
static _Thread_local char runtime_error[256];
#ifdef DT_MOBILE_HAS_ANDROID_DEPENDENCIES
extern unsigned long dt_mobile_dependency_probe(void);
#endif
static dt_mobile_status fail(dt_mobile_session *s, dt_mobile_status status, const char *message)
{ if(s) snprintf(s->error, sizeof(s->error), "%s", message); return status; }
uint32_t dt_mobile_capabilities(void)
{
  uint32_t capabilities = 0;
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  capabilities |= DT_MOBILE_CAP_ENGINE;
#if defined(DT_MOBILE_HAS_GPR) || defined(DT_MOBILE_HAS_RAWSPEED_GPR)
  capabilities |= DT_MOBILE_CAP_GPR;
#endif
#endif
  return capabilities;
}
dt_mobile_status dt_mobile_configure_runtime(const char *data_dir, const char *module_dir,
                                            const char *temp_root)
{
  runtime_error[0] = '\0';
  if(!data_dir || !data_dir[0] || !module_dir || !module_dir[0] || !temp_root || !temp_root[0])
  {
    snprintf(runtime_error, sizeof(runtime_error), "data, module and temporary runtime directories are required");
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  }
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_configure_runtime(data_dir, module_dir, temp_root,
                                             runtime_error, sizeof(runtime_error));
#else
  snprintf(runtime_error, sizeof(runtime_error), "this build does not contain the darktable processing engine");
  return DT_MOBILE_ERROR_UNSUPPORTED;
#endif
}
const char *dt_mobile_runtime_error(void)
{ return runtime_error[0] ? runtime_error : "native runtime configuration failed"; }
dt_mobile_status dt_mobile_open(const char *path, dt_mobile_session **out)
{
#ifdef DT_MOBILE_HAS_ANDROID_DEPENDENCIES
  (void)dt_mobile_dependency_probe();
#endif
  open_error[0] = '\0';
  if(out) *out = NULL;
  if(!path || !path[0] || !out) { snprintf(open_error, sizeof(open_error), "a non-empty source path and output session are required"); return DT_MOBILE_ERROR_INVALID_ARGUMENT; }
  FILE *input = fopen(path, "rb");
  if(!input) { snprintf(open_error, sizeof(open_error), "unable to open cached source: %s", path); return DT_MOBILE_ERROR_IO; }
  if(fseek(input, 0, SEEK_END) != 0 || ftell(input) < 16) { fclose(input); snprintf(open_error, sizeof(open_error), "source is empty or truncated"); return DT_MOBILE_ERROR_IO; }
  fclose(input);
#ifndef DT_MOBILE_HAS_DARKTABLE_ENGINE
  snprintf(open_error, sizeof(open_error), "this build does not contain the darktable processing engine");
  return DT_MOBILE_ERROR_UNSUPPORTED;
#else
  dt_mobile_session *s = calloc(1, sizeof(*s));
  if(!s) { snprintf(open_error, sizeof(open_error), "unable to allocate native session"); return DT_MOBILE_ERROR_OUT_OF_MEMORY; }
  s->path = malloc(strlen(path) + 1);
  if(!s->path) { free(s); snprintf(open_error, sizeof(open_error), "unable to allocate source path"); return DT_MOBILE_ERROR_OUT_OF_MEMORY; }
  strcpy(s->path, path); atomic_init(&s->cancelled, 0);
  const char *engine_path = path;
#if defined(DT_MOBILE_HAS_GPR) && !defined(DT_MOBILE_HAS_RAWSPEED_GPR)
  const char *extension = strrchr(path, '.');
  if(extension && !strcasecmp(extension, ".gpr"))
  {
    char *cache_directory = NULL;
#ifdef __ANDROID__
    /* Android sources have already been copied into the application's cache. */
    const char *slash = strrchr(path, '/');
    if(slash)
    {
      const size_t length = (size_t)(slash - path);
      cache_directory = malloc(length + 2);
      if(!cache_directory)
      { snprintf(open_error, sizeof(open_error), "unable to allocate GPR cache path"); free(s->path); free(s); return DT_MOBILE_ERROR_OUT_OF_MEMORY; }
      if(length) memcpy(cache_directory, path, length);
      else cache_directory[0] = '/';
      cache_directory[length ? length : 1] = '\0';
    }
#endif
    const dt_mobile_status prepared = dt_mobile_gpr_prepare(path, cache_directory, &s->prepared_path,
                                                            s->error, sizeof(s->error));
    free(cache_directory);
    if(prepared != DT_MOBILE_OK)
    {
      snprintf(open_error, sizeof(open_error), "%s", s->error);
      dt_mobile_gpr_cleanup(s->prepared_path);
      free(s->path); free(s); return prepared;
    }
    engine_path = s->prepared_path;
  }
#endif
  const dt_mobile_status status = dt_mobile_engine_open(engine_path, &s->engine, s->error, sizeof(s->error));
  if(status != DT_MOBILE_OK)
  {
    snprintf(open_error, sizeof(open_error), "%s", s->error);
#ifdef DT_MOBILE_HAS_GPR
    dt_mobile_gpr_cleanup(s->prepared_path);
#endif
    free(s->path); free(s); return status;
  }
  *out = s; return DT_MOBILE_OK;
#endif
}
const char *dt_mobile_open_error(void) { return open_error[0] ? open_error : "native open failed"; }
dt_mobile_status dt_mobile_modules(dt_mobile_session *s, char **json)
{
  if(json) *json = NULL;
  if(!s || !json) return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_modules(s->engine, json, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_set_parameter(dt_mobile_session *s, const char *module,
                                          const char *parameter, double value)
{
  if(!s || !module || !module[0] || !parameter || !parameter[0] || !isfinite(value))
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_set_parameter(s->engine, module, parameter, value, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_set_text_parameter(dt_mobile_session *s, const char *module,
                                               const char *parameter, const char *value)
{
  if(!s || !module || !module[0] || !parameter || !parameter[0] || !value)
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_set_text_parameter(s->engine, module, parameter, value, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_tone_equalizer_sample(dt_mobile_session *s, const char *module,
                                                 double x, double y, double *exposure)
{
  if(exposure) *exposure = NAN;
  if(!s || !module || !module[0] || !exposure || !isfinite(x) || !isfinite(y)
     || x < 0 || x >= 1 || y < 0 || y >= 1)
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_tone_equalizer_sample(s->engine, module, x, y, exposure,
                                                s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_tone_equalizer_adjust(dt_mobile_session *s, const char *module,
                                                 double exposure, double offset)
{
  if(!s || !module || !module[0] || !isfinite(exposure) || !isfinite(offset)
     || offset < -2 || offset > 2)
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_tone_equalizer_adjust(s->engine, module, exposure, offset,
                                                s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_tone_equalizer_output(dt_mobile_session *s, const char *module,
                                                 double exposure, double *output_exposure)
{
  if(output_exposure) *output_exposure = NAN;
  if(!s || !module || !module[0] || !output_exposure || !isfinite(exposure))
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_tone_equalizer_output(s->engine, module, exposure, output_exposure,
                                                s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_tone_equalizer_set_target(dt_mobile_session *s, const char *module,
                                                     double exposure, double target, uint64_t gesture,
                                                     double *output_exposure)
{
  if(output_exposure) *output_exposure = NAN;
  if(!s || !module || !module[0] || !output_exposure || !gesture || !isfinite(exposure)
     || !isfinite(target) || target < exposure - 2 || target > exposure + 2)
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_tone_equalizer_set_target(s->engine, module, exposure, target, gesture,
                                                    output_exposure, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_set_whitebalance_preset(dt_mobile_session *s, const char *module, int preset, int tuning)
{
  if(!s || !module || !module[0] || preset < 6) return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_set_whitebalance_preset(s->engine, module, preset, tuning, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_mask_photo_points(dt_mobile_session *s, const char *module, char **json)
{
  if(json) *json = NULL;
  if(!s || !module || !module[0] || !json) return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_mask_photo_points(s->engine, module, json, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_mask_place_photo(dt_mobile_session *s, const char *module,
                                            int form_id, int source, double x, double y)
{
  if(!s || !module || !module[0] || form_id <= 0 || (source != 0 && source != 1)
     || !isfinite(x) || !isfinite(y) || x < 0 || x >= 1 || y < 0 || y >= 1)
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_mask_place_photo(s->engine, module, form_id, source, x, y, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_new_instance(dt_mobile_session *s, const char *module, char **new_identifier)
{
  if(new_identifier) *new_identifier = NULL;
  if(!s || !module || !module[0] || !new_identifier) return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_new_instance(s->engine, module, new_identifier, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_history(dt_mobile_session *s, char **json)
{
  if(json) *json = NULL;
  if(!s || !json) return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_history(s->engine, json, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_undo(dt_mobile_session *s)
{
  if(!s) return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_undo(s->engine, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_redo(dt_mobile_session *s)
{
  if(!s) return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_redo(s->engine, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_set_enabled(dt_mobile_session *s, const char *module, int enabled)
{
  if(!s || !module || !module[0] || (enabled != 0 && enabled != 1)) return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_set_enabled(s->engine, module, enabled, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_set_curve(dt_mobile_session *s, const char *module, int channel,
                                     int interpolation, const dt_mobile_curve_point *points, size_t count)
{
  if(!s || !module || !module[0] || !points || count < 2 || count > 20
     || channel < 0 || channel > 2 || interpolation < 0 || interpolation > 2)
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  for(size_t k = 0; k < count; k++)
    if(!isfinite(points[k].x) || !isfinite(points[k].y)
       || points[k].x < 0.0f || points[k].x > 1.0f || points[k].y < 0.0f || points[k].y > 1.0f
       || (k && (double)points[k].x - (double)points[k - 1].x < (double)1e-6f))
      return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_set_curve(s->engine, module, channel, interpolation, points, count,
                                     s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_set_rgb_curve_options(dt_mobile_session *s, const char *module,
                                                 int mode, int preserve_colors)
{
  if(!s || !module || !module[0] || (mode != 0 && mode != 1))
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  /* Explicit reviewed protocol choices from common/rgb_norms.h. The engine
   * also checks actual introspection membership before touching parameters. */
  switch(preserve_colors)
  {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: break;
    default: return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  }
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_set_rgb_curve_options(s->engine, module, mode, preserve_colors,
                                                 s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_set_whitebalance_gains(dt_mobile_session *s, const char *module,
                                                 const float *gains, size_t count)
{
  if(!s || !module || !module[0] || !gains || (count != 3 && count != 4))
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  for(size_t k = 0; k < count; k++)
    if(!isfinite(gains[k]) || gains[k] < 0.001f || gains[k] > 8.0f)
      return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_set_whitebalance_gains(s->engine, module, gains, count,
                                                 s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_set_whitebalance_temperature(dt_mobile_session *s, const char *module,
                                                       double kelvin, double tint)
{
  if(!s || !module || !module[0] || !isfinite(kelvin) || kelvin < 1901.0 || kelvin > 25000.0
     || !isfinite(tint) || tint < 0.135 || tint > 2.326)
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_set_whitebalance_temperature(s->engine, module, kelvin, tint,
                                                       s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_set_whitebalance_setting(dt_mobile_session *s, const char *module,
                                                   int setting)
{
  if(!s || !module || !module[0] || (setting != 0 && setting != 3 && setting != 4))
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_set_whitebalance_setting(s->engine, module, setting,
                                                   s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_set_crop(dt_mobile_session *s, const char *module,
                                    float left, float top, float right, float bottom)
{
  if(!s || !module || !module[0] || !dt_mobile_crop_valid(left, top, right, bottom))
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_set_crop(s->engine, module, left, top, right, bottom,
                                    s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_export_formats(dt_mobile_session *s, char **json)
{
  if(json) *json = NULL;
  if(!s || !json) return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_export_formats(s->engine, json, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_set_module_params(dt_mobile_session *s, const char *module,
                                               const void *params, size_t size)
{
  if(!s || !module || !module[0] || !params || !size) return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_set_module_params(s->engine, module, params, size, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_render_preview(dt_mobile_session *s, int mw, int mh,
                                            uint8_t **rgba, int *w, int *h)
{
  if(rgba) *rgba = NULL;
  if(w) *w = 0;
  if(h) *h = 0;
  if(!s || mw <= 0 || mh <= 0 || !rgba || !w || !h) return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_render(s->engine, mw, mh, rgba, w, h, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
static dt_mobile_status export_destination_status(dt_mobile_session *s, const char *path)
{
  struct stat source_info, output_info;
  if(!strcmp(s->path, path)
     || (stat(s->path, &source_info) == 0 && stat(path, &output_info) == 0
         && source_info.st_dev == output_info.st_dev && source_info.st_ino == output_info.st_ino))
    return fail(s, DT_MOBILE_ERROR_INVALID_ARGUMENT, "export must not overwrite the original source");
  return DT_MOBILE_OK;
}
dt_mobile_status dt_mobile_export(dt_mobile_session *s, const char *path, const char *format, int quality)
{
  if(!s || !path || !path[0] || !format || !format[0] || quality < 0 || quality > 100) return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
  const dt_mobile_status destination = export_destination_status(s, path);
  if(destination != DT_MOBILE_OK) return destination;
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_export(s->engine, path, format, quality, s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
dt_mobile_status dt_mobile_export_with_settings(dt_mobile_session *s, const char *path,
                                                const char *format,
                                                const dt_mobile_export_settings *settings)
{
  if(!s || !path || !path[0] || !format || !format[0] || !settings
     || settings->struct_size != sizeof(*settings)
     || settings->max_width > INT_MAX || settings->max_height > INT_MAX)
    return DT_MOBILE_ERROR_INVALID_ARGUMENT;
  if(atomic_load(&s->cancelled)) return fail(s, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
  const dt_mobile_status destination = export_destination_status(s, path);
  if(destination != DT_MOBILE_OK) return destination;
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  return dt_mobile_engine_export_with_settings(s->engine, path, format, settings,
                                                s->error, sizeof(s->error));
#else
  return fail(s, DT_MOBILE_ERROR_UNSUPPORTED, "this build does not contain the darktable engine");
#endif
}
void dt_mobile_free_buffer(void *buffer) { free(buffer); }
void dt_mobile_cancel(dt_mobile_session *s) { if(s) { atomic_store(&s->cancelled, 1);
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  dt_mobile_engine_cancel(s->engine);
#endif
} }
const char *dt_mobile_last_error(const dt_mobile_session *s) { return s ? s->error : "invalid session"; }
void dt_mobile_close(dt_mobile_session *s) { if(s) {
#ifdef DT_MOBILE_HAS_DARKTABLE_ENGINE
  dt_mobile_engine_close(s->engine);
#endif
#ifdef DT_MOBILE_HAS_GPR
  dt_mobile_gpr_cleanup(s->prepared_path);
#endif
  free(s->path); free(s); } }
