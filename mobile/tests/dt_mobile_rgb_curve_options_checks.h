/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_RGB_CURVE_OPTIONS_CHECKS_H
#define DT_MOBILE_RGB_CURVE_OPTIONS_CHECKS_H

#include "dt_mobile.h"
#include <glib.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These helpers inspect the public metadata, rather than copying the upstream
 * parameter struct. Skip quoted strings when finding nested object boundaries. */
static const char *rgb_options_json_end(const char *start)
{
  if(!start || (*start != '{' && *start != '[')) return NULL;
  unsigned depth = 0;
  int quoted = 0, escaped = 0;
  for(const char *p = start; *p; p++)
  {
    if(quoted)
    {
      if(escaped) escaped = 0;
      else if(*p == '\\') escaped = 1;
      else if(*p == '"') quoted = 0;
    }
    else if(*p == '"') quoted = 1;
    else if(*p == '{' || *p == '[') depth++;
    else if(*p == '}' || *p == ']')
    {
      if(!depth) return NULL;
      if(!--depth) return p + 1;
    }
  }
  return NULL;
}

static const char *rgb_options_find(const char *start, const char *end, const char *key)
{
  const char *found = strstr(start, key);
  return found && found < end ? found + strlen(key) : NULL;
}

static int rgb_options_integer(const char *start, const char *end, const char *key, int *value)
{
  const char *field = rgb_options_find(start, end, key);
  if(!field) return 0;
  char *after = NULL;
  const long parsed = strtol(field, &after, 10);
  if(after == field || after > end || parsed < INT_MIN || parsed > INT_MAX) return 0;
  *value = (int)parsed;
  return 1;
}

typedef struct rgb_options_metadata
{
  int mode, preserve, channels;
  int interpolation[3];
  gchar *nodes[3];
} rgb_options_metadata;

static void rgb_options_metadata_clear(rgb_options_metadata *metadata)
{
  for(int channel = 0; channel < 3; channel++) g_clear_pointer(&metadata->nodes[channel], g_free);
  memset(metadata, 0, sizeof(*metadata));
}

static int rgb_options_metadata_read(const char *json, rgb_options_metadata *metadata)
{
  const char *module = strstr(json, "{\"operation\":\"rgbcurve\",");
  const char *end = rgb_options_json_end(module);
  if(!module || !end) return 0;
  const char *options = rgb_options_find(module, end, "\"rgbCurveOptions\":");
  const char *options_end = rgb_options_json_end(options);
  if(!options_end || options_end > end
     || !rgb_options_integer(options, options_end, "\"mode\":", &metadata->mode)
     || !rgb_options_integer(options, options_end, "\"preserveColors\":", &metadata->preserve)) return 0;
  const char *curves = rgb_options_find(module, end, "\"curves\":");
  const char *curves_end = rgb_options_json_end(curves);
  if(!curves_end || curves_end > end) return 0;
  for(const char *curve = curves + 1; curve < curves_end && *curve == '{';)
  {
    const char *curve_end = rgb_options_json_end(curve);
    int channel;
    if(!curve_end || curve_end > curves_end
       || !rgb_options_integer(curve, curve_end, "\"channel\":", &channel)
       || channel < 0 || channel > 2 || metadata->nodes[channel]
       || !rgb_options_integer(curve, curve_end, "\"interpolation\":", &metadata->interpolation[channel])) return 0;
    const char *nodes = rgb_options_find(curve, curve_end, "\"nodes\":");
    const char *nodes_end = rgb_options_json_end(nodes);
    if(!nodes_end || nodes_end > curve_end) return 0;
    metadata->nodes[channel] = g_strndup(nodes, (gsize)(nodes_end - nodes));
    metadata->channels++;
    curve = curve_end;
    if(*curve == ',') curve++;
  }
  return metadata->channels == (metadata->mode ? 3 : 1);
}

static int rgb_options_same_curve(const rgb_options_metadata *first, int a,
                                 const rgb_options_metadata *second, int b)
{
  return first->nodes[a] && second->nodes[b]
    && first->interpolation[a] == second->interpolation[b]
    && !strcmp(first->nodes[a], second->nodes[b]);
}

/* Call before the existing default-linked RGB-curve checks. The fixture leaves
 * RGB curve disabled, linked, luminance preservation and all channels identity
 * with monotone interpolation, so the following checks retain their baseline.
 * Every preview still comes from the original processing pixelpipe. */
static int rgb_curve_options_checks(dt_mobile_session *session)
{
  int success = 0;
  char *json = NULL, *before_json = NULL, *before_history = NULL, *after_history = NULL;
  uint8_t *reference = NULL, *changed = NULL, *restored = NULL;
  int width = 0, height = 0, changed_width = 0, changed_height = 0, restored_width = 0, restored_height = 0;
  rgb_options_metadata before = {0}, after = {0}, identity_state = {0};
  const dt_mobile_curve_point identity[] = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
  const dt_mobile_curve_point dark[] = { { 0.0f, 0.0f }, { 0.5f, 0.2f }, { 1.0f, 1.0f } };
  const dt_mobile_curve_point bright[] = { { 0.0f, 0.0f }, { 0.5f, 0.8f }, { 1.0f, 1.0f } };
#define RGB_OPTIONS_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "RGB curve options:%d: check failed: %s\n", __LINE__, #condition); goto finished; } } while(0)
#define RGB_OPTIONS_OK(call) do { const dt_mobile_status result = (call); if(result != DT_MOBILE_OK) { \
  fprintf(stderr, "RGB curve options: %s: %s\n", #call, dt_mobile_last_error(session)); goto finished; } } while(0)
#define RGB_OPTIONS_METADATA(target) do { \
  dt_mobile_free_buffer(json); json = NULL; rgb_options_metadata_clear(&(target)); \
  RGB_OPTIONS_OK(dt_mobile_modules(session, &json)); RGB_OPTIONS_CHECK(rgb_options_metadata_read(json, &(target))); \
} while(0)

  RGB_OPTIONS_OK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 1, 1));
  for(int channel = 0; channel < 3; channel++)
    RGB_OPTIONS_OK(dt_mobile_set_curve(session, "rgbcurve", channel, 2, identity, 2));
  RGB_OPTIONS_METADATA(identity_state);
  RGB_OPTIONS_CHECK(identity_state.mode == 1 && identity_state.preserve == 1);

  /* A new independent mode inherits the linked curve only when both stored
   * G/B curves are untouched identity curves, matching the desktop widget. */
  RGB_OPTIONS_OK(dt_mobile_set_curve(session, "rgbcurve", 0, 1, dark, 3));
  RGB_OPTIONS_OK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 0, 1));
  RGB_OPTIONS_OK(dt_mobile_modules(session, &before_json));
  RGB_OPTIONS_CHECK(rgb_options_metadata_read(before_json, &before));
  RGB_OPTIONS_CHECK(before.mode == 0 && before.channels == 1);
  RGB_OPTIONS_OK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 1, 1));
  RGB_OPTIONS_METADATA(after);
  RGB_OPTIONS_CHECK(after.mode == 1 && after.channels == 3);
  for(int channel = 0; channel < 3; channel++)
    RGB_OPTIONS_CHECK(rgb_options_same_curve(&before, 0, &after, channel));

  RGB_OPTIONS_OK(dt_mobile_undo(session));
  dt_mobile_free_buffer(json); json = NULL;
  RGB_OPTIONS_OK(dt_mobile_modules(session, &json));
  RGB_OPTIONS_CHECK(!strcmp(before_json, json));
  dt_mobile_free_buffer(json); json = NULL;
  dt_mobile_free_buffer(before_json); before_json = NULL;
  /* Replacing R with identity before another mode switch makes hidden G/B
   * contents observable: undo must also have restored their original identity. */
  RGB_OPTIONS_OK(dt_mobile_set_curve(session, "rgbcurve", 0, 2, identity, 2));
  RGB_OPTIONS_OK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 1, 1));
  RGB_OPTIONS_METADATA(after);
  for(int channel = 0; channel < 3; channel++)
    RGB_OPTIONS_CHECK(rgb_options_same_curve(&identity_state, channel, &after, channel));

  /* Either edited side channel prevents the copy. Preserve BOTH side channels,
   * including the other channel that remains identity. */
  for(int edited = 1; edited <= 2; edited++)
  {
    for(int channel = 0; channel < 3; channel++)
      RGB_OPTIONS_OK(dt_mobile_set_curve(session, "rgbcurve", channel, 2, identity, 2));
    RGB_OPTIONS_OK(dt_mobile_set_curve(session, "rgbcurve", 0, 1, dark, 3));
    RGB_OPTIONS_OK(dt_mobile_set_curve(session, "rgbcurve", edited, 0, bright, 3));
    RGB_OPTIONS_METADATA(before);
    RGB_OPTIONS_OK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 0, 1));
    RGB_OPTIONS_OK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 1, 1));
    RGB_OPTIONS_METADATA(after);
    for(int channel = 0; channel < 3; channel++)
      RGB_OPTIONS_CHECK(rgb_options_same_curve(&before, channel, &after, channel));
  }

  for(int channel = 1; channel < 3; channel++)
    RGB_OPTIONS_OK(dt_mobile_set_curve(session, "rgbcurve", channel, 2, identity, 2));
  RGB_OPTIONS_METADATA(before);
  RGB_OPTIONS_OK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 1, 3));
  RGB_OPTIONS_METADATA(after);
  RGB_OPTIONS_CHECK(after.mode == 1 && after.preserve == 3);
  for(int channel = 0; channel < 3; channel++)
    RGB_OPTIONS_CHECK(rgb_options_same_curve(&before, channel, &after, channel));

  /* Rejected requests must change neither advertised state nor session history. */
  RGB_OPTIONS_OK(dt_mobile_modules(session, &before_json));
  RGB_OPTIONS_OK(dt_mobile_history(session, &before_history));
  RGB_OPTIONS_CHECK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", -1, 1) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  RGB_OPTIONS_CHECK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 2, 1) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  RGB_OPTIONS_CHECK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 0, -1) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  RGB_OPTIONS_CHECK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 0, 7) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  RGB_OPTIONS_CHECK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 0, INT_MAX) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  RGB_OPTIONS_CHECK(dt_mobile_set_rgb_curve_options(session, "basecurve", 0, 1) == DT_MOBILE_ERROR_UNSUPPORTED);
  RGB_OPTIONS_CHECK(dt_mobile_set_rgb_curve_options(session, "exposure", 0, 1) == DT_MOBILE_ERROR_UNSUPPORTED);
  RGB_OPTIONS_CHECK(dt_mobile_set_rgb_curve_options(session, "rgbcurve@999", 0, 1) == DT_MOBILE_ERROR_UNSUPPORTED);
  RGB_OPTIONS_CHECK(dt_mobile_set_parameter(session, "rgbcurve", "curve_autoscale", 1) == DT_MOBILE_ERROR_UNSUPPORTED);
  RGB_OPTIONS_CHECK(dt_mobile_set_parameter(session, "rgbcurve", "preserve_colors", 2) == DT_MOBILE_ERROR_UNSUPPORTED);
  dt_mobile_free_buffer(json); json = NULL;
  RGB_OPTIONS_OK(dt_mobile_modules(session, &json));
  RGB_OPTIONS_OK(dt_mobile_history(session, &after_history));
  RGB_OPTIONS_CHECK(!strcmp(before_json, json) && !strcmp(before_history, after_history));
  dt_mobile_free_buffer(json); json = NULL;
  dt_mobile_free_buffer(before_json); before_json = NULL;

  RGB_OPTIONS_OK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 1, 1));
  for(int channel = 0; channel < 3; channel++)
    RGB_OPTIONS_OK(dt_mobile_set_curve(session, "rgbcurve", channel, 2, identity, 2));
  RGB_OPTIONS_OK(dt_mobile_render_preview(session, 96, 96, &reference, &width, &height));
  RGB_OPTIONS_OK(dt_mobile_set_curve(session, "rgbcurve", 1, 2, bright, 3));
  RGB_OPTIONS_OK(dt_mobile_render_preview(session, 96, 96, &changed, &changed_width, &changed_height));
  RGB_OPTIONS_CHECK(reference && changed && width == changed_width && height == changed_height);
  RGB_OPTIONS_CHECK(memcmp(reference, changed, (size_t)width * height * 4));
  RGB_OPTIONS_OK(dt_mobile_undo(session));
  RGB_OPTIONS_OK(dt_mobile_render_preview(session, 96, 96, &restored, &restored_width, &restored_height));
  RGB_OPTIONS_CHECK(restored && width == restored_width && height == restored_height);
  RGB_OPTIONS_CHECK(!memcmp(reference, restored, (size_t)width * height * 4));

  RGB_OPTIONS_OK(dt_mobile_set_rgb_curve_options(session, "rgbcurve", 0, 1));
  RGB_OPTIONS_OK(dt_mobile_set_enabled(session, "rgbcurve", 0));
  success = 1;
finished:
  dt_mobile_free_buffer(json); dt_mobile_free_buffer(before_json);
  dt_mobile_free_buffer(before_history); dt_mobile_free_buffer(after_history);
  dt_mobile_free_buffer(reference); dt_mobile_free_buffer(changed); dt_mobile_free_buffer(restored);
  rgb_options_metadata_clear(&before); rgb_options_metadata_clear(&after); rgb_options_metadata_clear(&identity_state);
#undef RGB_OPTIONS_CHECK
#undef RGB_OPTIONS_OK
#undef RGB_OPTIONS_METADATA
  return success;
}
#endif
