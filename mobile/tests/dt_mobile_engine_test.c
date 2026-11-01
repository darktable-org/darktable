/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "dt_mobile.h"
#include "dt_mobile_headless_masks_fixture.h"
#include "dt_mobile_export_engine_checks.h"
#include "dt_mobile_rgb_curve_options_checks.h"
#include "dt_mobile_whitebalance_checks.h"
#include "develop/imageop.h"
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Keep checks active in Release builds as well as Debug builds. */
#define CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
  goto failed; } } while(0)
#define ENGINE_OK(call) do { dt_mobile_status status = (call); \
  if(status != DT_MOBILE_OK) { fprintf(stderr, "%s: %s\n", #call, \
    session ? dt_mobile_last_error(session) : dt_mobile_open_error()); goto failed; } } while(0)

#ifdef DT_MOBILE_TEST_INSTANCE_RECLAMATION
static size_t reclaimed_instances;
void __real_dt_iop_cleanup_module(dt_iop_module_t *module);
void __wrap_dt_iop_cleanup_module(dt_iop_module_t *module)
{
  reclaimed_instances++;
  __real_dt_iop_cleanup_module(module);
}
#endif

static uint32_t read_be32(const unsigned char *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int png_dimensions(const char *path, uint32_t *width, uint32_t *height)
{
  unsigned char header[24];
  FILE *file = g_fopen(path, "rb");
  if(!file) return 0;
  const size_t count = fread(header, 1, sizeof(header), file);
  fclose(file);
  if(count != sizeof(header) || memcmp(header, "\211PNG\r\n\032\n", 8)
     || memcmp(header + 12, "IHDR", 4)) return 0;
  *width = read_be32(header + 16);
  *height = read_be32(header + 20);
  return 1;
}

static int runtime_children(const char *directory)
{
  DIR *dir = opendir(directory);
  if(!dir) return -1;
  int count = 0;
  struct dirent *entry;
  while((entry = readdir(dir)))
    if(strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) count++;
  closedir(dir);
  return count;
}

static int module_has(const char *json, const char *operation, const char *fragment)
{
  char key[128];
  snprintf(key, sizeof(key), "{\"operation\":\"%s\",", operation);
  const char *module = strstr(json, key);
  if(!module) return 0;
  const char *end = strstr(module + 1, "{\"operation\":");
  const char *match = strstr(module, fragment);
  return match && (!end || match < end);
}

static int parameter_equals(const char *json, const char *operation, const char *parameter, double expected)
{
  char key[128];
  snprintf(key, sizeof(key), "{\"operation\":\"%s\",", operation);
  const char *module = strstr(json, key);
  if(!module) return 0;
  const char *module_end = strstr(module + 1, "{\"operation\":");
  snprintf(key, sizeof(key), "{\"name\":\"%s\",", parameter);
  const char *field = strstr(module, key);
  if(!field || (module_end && field >= module_end)) return 0;
  const char *field_end = strstr(field + 1, "{\"name\":");
  const char *value = strstr(field, ",\"value\":");
  if(!value || (field_end && value >= field_end) || (module_end && value >= module_end)) return 0;
  char *end;
  const double actual = strtod(value + strlen(",\"value\":"), &end);
  return end != value + strlen(",\"value\":") && isfinite(actual) && actual == expected;
}

#include "dt_mobile_color_controls_checks.h"
#include "dt_mobile_tool_controls_checks.h"
#include "dt_mobile_blending_checks.h"
#include "dt_mobile_text_controls_checks.h"
#include "dt_mobile_mask_controls_checks.h"
#include "dt_mobile_lens_checks.h"
#include "dt_mobile_lens_auto_checks.h"
#include "dt_mobile_tone_equalizer_checks.h"
#include "dt_mobile_path_mask_checks.h"
#include "dt_mobile_parametric_checks.h"
#include "dt_mobile_crop_geometry_checks.h"
#include "dt_mobile_file_controls_checks.h"
#include "dt_mobile_array_checks.h"
#include "dt_mobile_extended_controls_checks.h"

static int instance_checks(dt_mobile_session *session)
{
  char *baseline = NULL, *metadata = NULL, *history = NULL, *after_failure = NULL;
  char *first = NULL, *second = NULL, *branch = NULL, *previous = NULL, *next = NULL;
  uint8_t *original = NULL, *created = NULL, *edited = NULL, *restored = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0;
  int success = 0;
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 0.25));
  ENGINE_OK(dt_mobile_modules(session, &baseline));
  CHECK(module_has(baseline, "exposure", "\"canCreateInstance\":true"));
  CHECK(module_has(baseline, "temperature", "\"canCreateInstance\":false"));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &original, &width, &height));
  ENGINE_OK(dt_mobile_history(session, &history));
  first = (char *)1;
  CHECK(dt_mobile_new_instance(session, "temperature", &first) == DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(first == NULL);
  first = (char *)1;
  CHECK(dt_mobile_new_instance(session, "exposure@2147483647", &first) == DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(first == NULL);
  ENGINE_OK(dt_mobile_history(session, &after_failure));
  CHECK(!strcmp(history, after_failure));
  dt_mobile_free_buffer(after_failure); after_failure = NULL;
  dt_mobile_free_buffer(history); history = NULL;
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;

#ifdef DT_MOBILE_TEST_INSTANCE_RECLAMATION
  const size_t retained_before = reclaimed_instances;
#endif
  ENGINE_OK(dt_mobile_new_instance(session, "exposure", &first));
  CHECK(first && !strcmp(first, "exposure@1"));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(module_has(metadata, first, "\"enabled\":true"));
  CHECK(parameter_equals(metadata, first, "exposure", 0.0));
  CHECK(parameter_equals(metadata, "exposure", "exposure", 0.25));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &created, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  ENGINE_OK(dt_mobile_set_parameter(session, first, "exposure", 1.0));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &edited, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(created, edited, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(created, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!strcmp(baseline, metadata));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(original, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_redo(session));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(created, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_redo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(parameter_equals(metadata, first, "exposure", 1.0));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &restored, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(edited, restored, (size_t)width * height * 4));
  dt_mobile_free_buffer(restored); restored = NULL;
#ifdef DT_MOBILE_TEST_INSTANCE_RECLAMATION
  CHECK(reclaimed_instances == retained_before); /* undo/redo owns the object */
#endif

  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_new_instance(session, "exposure", &branch));
  CHECK(strcmp(first, branch));
#ifdef DT_MOBILE_TEST_INSTANCE_RECLAMATION
  CHECK(reclaimed_instances == retained_before + 1); /* discarded first branch */
#endif
  ENGINE_OK(dt_mobile_history(session, &history));
  CHECK(dt_mobile_set_parameter(session, first, "exposure", 1.0) == DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_enabled(session, first, 1) == DT_MOBILE_ERROR_UNSUPPORTED);
  next = (char *)1;
  CHECK(dt_mobile_new_instance(session, first, &next) == DT_MOBILE_ERROR_UNSUPPORTED && next == NULL);
  ENGINE_OK(dt_mobile_history(session, &after_failure));
  CHECK(!strcmp(history, after_failure));
  dt_mobile_free_buffer(after_failure); after_failure = NULL;
  dt_mobile_free_buffer(history); history = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  previous = branch; branch = NULL;
  /* Repeated redo-branch replacement checks stale identifiers and the complete
   * visible topology. On Linux the linker wrapper also counts original module
   * cleanup calls, so visibility alone cannot conceal accumulated retirements. */
#ifdef DT_MOBILE_TEST_INSTANCE_RECLAMATION
  const size_t reclaimed_before = reclaimed_instances;
#endif
  for(int iteration = 0; iteration < 80; iteration++)
  {
    ENGINE_OK(dt_mobile_new_instance(session, "exposure", &next));
    CHECK(next && previous && strchr(next, '@') && strchr(previous, '@'));
    CHECK(strtol(strchr(next, '@') + 1, NULL, 10) > strtol(strchr(previous, '@') + 1, NULL, 10));
    CHECK(dt_mobile_set_enabled(session, previous, 1) == DT_MOBILE_ERROR_UNSUPPORTED);
    ENGINE_OK(dt_mobile_undo(session));
    ENGINE_OK(dt_mobile_modules(session, &metadata));
    CHECK(!strcmp(baseline, metadata));
    dt_mobile_free_buffer(metadata); metadata = NULL;
    dt_mobile_free_buffer(previous); previous = next; next = NULL;
  }
#ifdef DT_MOBILE_TEST_INSTANCE_RECLAMATION
  CHECK(reclaimed_instances - reclaimed_before == 80);
#endif
  for(int iteration = 0; iteration < 70; iteration++)
    ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", iteration % 2 ? 0.5 : 0.25));
  ENGINE_OK(dt_mobile_history(session, &history));
  CHECK(strstr(history, "\"position\":64,\"length\":64,\"limit\":64"));
  dt_mobile_free_buffer(history); history = NULL;
  for(int iteration = 0; iteration < 64; iteration++) ENGINE_OK(dt_mobile_undo(session));
  CHECK(dt_mobile_undo(session) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  for(int iteration = 0; iteration < 64; iteration++) ENGINE_OK(dt_mobile_redo(session));
  CHECK(dt_mobile_set_enabled(session, previous, 1) == DT_MOBILE_ERROR_UNSUPPORTED);
  ENGINE_OK(dt_mobile_new_instance(session, "exposure", &branch));
  ENGINE_OK(dt_mobile_set_parameter(session, branch, "exposure", 0.75));
  ENGINE_OK(dt_mobile_new_instance(session, branch, &second));
  CHECK(strcmp(branch, second));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(module_has(metadata, branch, "\"enabled\":true"));
  CHECK(module_has(metadata, second, "\"enabled\":true"));
  CHECK(parameter_equals(metadata, branch, "exposure", 0.75));
  CHECK(parameter_equals(metadata, "exposure", "exposure", 0.5));
  CHECK(parameter_equals(metadata, second, "exposure", 0.0));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(!module_has(metadata, branch, "\"enabled\":"));
  CHECK(!module_has(metadata, second, "\"enabled\":"));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_redo(session));
  ENGINE_OK(dt_mobile_redo(session));
  ENGINE_OK(dt_mobile_redo(session));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(module_has(metadata, branch, "\"enabled\":true"));
  CHECK(module_has(metadata, second, "\"enabled\":true"));
  success = 1;
failed:
  dt_mobile_free_buffer(baseline); dt_mobile_free_buffer(metadata);
  dt_mobile_free_buffer(history); dt_mobile_free_buffer(after_failure);
  if(first != (char *)1) dt_mobile_free_buffer(first);
  dt_mobile_free_buffer(second); dt_mobile_free_buffer(branch);
  dt_mobile_free_buffer(previous);
  if(next != (char *)1) dt_mobile_free_buffer(next);
  dt_mobile_free_buffer(original); dt_mobile_free_buffer(created);
  dt_mobile_free_buffer(edited); dt_mobile_free_buffer(restored);
  return success;
}

static int instance_priority_range_fixture(void)
{
  /* Check the original helper's overflow/rejection path before module loading
   * or any topology/history allocation. No synthetic image processing occurs. */
  dt_develop_t dev;
  dt_iop_module_t base = { 0 };
  int success = 0;
  dt_dev_init(&dev, FALSE);
  base.dev = &dev;
  base.instance = 17;
  base.multi_priority = INT_MAX;
  dev.iop = g_list_append(NULL, &base);
  GList *const original = dev.iop;
  CHECK(dt_dev_module_duplicate_at_priority(&dev, &base, TRUE, 0) == NULL);
  CHECK(dev.iop == original && dev.iop->data == &base && !dev.iop->next);
  CHECK(!dev.history && !dev.iop_order_list && !dev.alliop);
  base.multi_priority = 0;
  CHECK(dt_dev_module_duplicate_at_priority(&dev, &base, TRUE, -1) == NULL);
  CHECK(dev.iop == original && !dev.history && !dev.iop_order_list && !dev.alliop);
  success = 1;
failed:
  g_list_free(dev.iop); dev.iop = NULL; /* stack-owned guard fixture */
  dt_dev_cleanup(&dev);
  return success;
}

static int history_checks(dt_mobile_session *session, const char *output,
                          uint32_t source_width, uint32_t source_height)
{
  uint8_t *reference = NULL, *changed = NULL;
  char *json = NULL, *state_before = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0;
  uint32_t output_width = 0, output_height = 0;
  int success = 0;
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 0.5));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &reference, &width, &height));
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 1.5));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &changed, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(memcmp(reference, changed, (size_t)width * height * 4));
  dt_mobile_free_buffer(changed); changed = NULL;
  /* The desktop merges adjacent edits to the same module into one history
   * entry. Session undo must still recover each explicit mobile Apply. */
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_modules(session, &json));
  CHECK(parameter_equals(json, "exposure", "exposure", 0.5));
  dt_mobile_free_buffer(json); json = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &changed, &other_width, &other_height));
  CHECK(width == other_width && height == other_height);
  CHECK(!memcmp(reference, changed, (size_t)width * height * 4));
  dt_mobile_free_buffer(changed); changed = NULL;
  ENGINE_OK(dt_mobile_redo(session));
  ENGINE_OK(dt_mobile_modules(session, &json));
  CHECK(parameter_equals(json, "exposure", "exposure", 1.5));
  dt_mobile_free_buffer(json); json = NULL;
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 0.25));
  ENGINE_OK(dt_mobile_history(session, &state_before));
  CHECK(strstr(state_before, "\"canUndo\":true,\"canRedo\":false"));
  CHECK(dt_mobile_redo(session) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_parameter(session, "exposure", "exposure", NAN) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_history(session, &json));
  CHECK(!strcmp(state_before, json));
  dt_mobile_free_buffer(json); json = NULL;
  dt_mobile_free_buffer(state_before); state_before = NULL;
  ENGINE_OK(dt_mobile_set_crop(session, "crop", 0.25f, 0.0f, 0.75f, 1.0f));
  ENGINE_OK(dt_mobile_export(session, output, "png", 90));
  CHECK(png_dimensions(output, &output_width, &output_height));
  CHECK(output_width == source_width / 2 && output_height == source_height);
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_export(session, output, "png", 90));
  CHECK(png_dimensions(output, &output_width, &output_height));
  CHECK(output_width == source_width && output_height == source_height);
  ENGINE_OK(dt_mobile_redo(session));
  ENGINE_OK(dt_mobile_export(session, output, "png", 90));
  CHECK(png_dimensions(output, &output_width, &output_height));
  CHECK(output_width == source_width / 2 && output_height == source_height);
  ENGINE_OK(dt_mobile_set_crop(session, "crop", 0.0f, 0.0f, 1.0f, 1.0f));
  /* The bounded window must release both old snapshots and a discarded redo
   * branch without exposing the automatic startup history as another step. */
  for(int step = 0; step < 65; step++)
    ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", step % 2 ? 1.5 : 0.5));
  ENGINE_OK(dt_mobile_history(session, &json));
  CHECK(strstr(json, "\"position\":64,\"length\":64,\"limit\":64"));
  dt_mobile_free_buffer(json); json = NULL;
  for(int step = 0; step < 64; step++) ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_history(session, &state_before));
  CHECK(strstr(state_before, "\"canUndo\":false,\"canRedo\":true,\"position\":0"));
  CHECK(dt_mobile_undo(session) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_history(session, &json));
  CHECK(!strcmp(state_before, json));
  success = 1;
failed:
  dt_mobile_free_buffer(reference);
  dt_mobile_free_buffer(changed);
  dt_mobile_free_buffer(json);
  dt_mobile_free_buffer(state_before);
  return success;
}

int main(int argc, char **argv)
{
  dt_mobile_session *session = NULL;
  uint8_t *before = NULL, *after = NULL;
  char *modules = NULL, *formats = NULL, *curve_json = NULL;
  char *scalar_json = NULL;
  char *temporary_root = NULL, *data_copy = NULL, *module_copy = NULL;
  char *data_directory = NULL, *module_directory = NULL;
  int width = 0, height = 0, edited_width = 0, edited_height = 0;
  uint32_t source_width = 0, source_height = 0, output_width = 0, output_height = 0;
  CHECK(argc == 3);
  CHECK(png_dimensions(argv[1], &source_width, &source_height));
  const char *data_environment = getenv("DT_MOBILE_DATADIR");
  const char *module_environment = getenv("DT_MOBILE_MODULEDIR");
  const char *temporary_directory = getenv("TMPDIR");
  if(!temporary_directory || !temporary_directory[0]) temporary_directory = "/tmp";
  CHECK(data_environment && module_environment);
  data_directory = strdup(data_environment);
  module_directory = strdup(module_environment);
  CHECK(data_directory && module_directory);
  temporary_root = malloc(strlen(temporary_directory) + 40);
  CHECK(temporary_root);
  sprintf(temporary_root, "%s/dt-mobile-runtime-test-XXXXXX", temporary_directory);
  CHECK(mkdtemp(temporary_root));
  CHECK(dt_mobile_configure_runtime("relative/data", module_directory, temporary_root)
        == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  /* An existing but incomplete data directory must never be treated as a
   * successfully prepared runtime, including on the native entry point. */
  CHECK(dt_mobile_configure_runtime(temporary_root, module_directory, temporary_root)
        == DT_MOBILE_ERROR_IO);
  CHECK(strstr(dt_mobile_runtime_error(), "darktablerc"));
  data_copy = strdup(data_directory);
  module_copy = strdup(module_directory);
  CHECK(data_copy && module_copy);
  CHECK(dt_mobile_configure_runtime(data_copy, module_copy, temporary_root) == DT_MOBILE_OK);
  /* JNI releases its UTF buffers when configure returns. The engine must own
   * independent path strings, including when its next open happens later. */
  memset(data_copy, 'x', strlen(data_copy));
  memset(module_copy, 'x', strlen(module_copy));
  free(data_copy); data_copy = NULL;
  free(module_copy); module_copy = NULL;
  /* Android uses the configuration API without these host-only variables.
   * Opening and reopening must therefore work after both variables are gone. */
  CHECK(unsetenv("DT_MOBILE_DATADIR") == 0);
  CHECK(unsetenv("DT_MOBILE_MODULEDIR") == 0);
  /* Rejected configuration must leave the successful paths intact. */
  CHECK(dt_mobile_configure_runtime(temporary_root, module_directory, temporary_root)
        == DT_MOBILE_ERROR_IO);
  ENGINE_OK(dt_mobile_open(argv[1], &session));
  CHECK(mobile_lens_auto_checks());
  CHECK(dt_mobile_headless_masks_fixture());
  CHECK(instance_priority_range_fixture());
  ENGINE_OK(dt_mobile_history(session, &curve_json));
  CHECK(strstr(curve_json, "\"canUndo\":false,\"canRedo\":false,\"position\":0,\"length\":0"));
  dt_mobile_free_buffer(curve_json); curve_json = NULL;
  CHECK(dt_mobile_undo(session) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_redo(session) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(runtime_children(temporary_root) == 1);
  CHECK(dt_mobile_configure_runtime(data_directory, module_directory, temporary_root)
        == DT_MOBILE_ERROR_ENGINE);
  CHECK(strstr(dt_mobile_runtime_error(), "active"));
  ENGINE_OK(dt_mobile_modules(session, &modules));
  CHECK(modules && strstr(modules, "exposure"));
  /* Desktop introspection includes internal structural fields. The scalar
   * bridge must neither advertise them nor accept writes to them: rgbcurve's
   * count has the full int range, and basecurve declares zero as an allowed
   * count although processing indexes count-1. Rendering those values is unsafe. */
  CHECK(strstr(modules, "\"name\":\"exposure\",\"label\":"));
  CHECK(!strstr(modules, "\"name\":\"curve_num_nodes[0]\""));
  CHECK(!strstr(modules, "\"name\":\"basecurve_nodes[0]\""));
  CHECK(module_has(modules, "basecurve", "\"name\":\"exposure_fusion\""));
  static const struct {
    const char *operation, *parameter;
    double minimum, maximum, edited;
  } scalar_controls[] = {
    { "sigmoid", "middle_grey_contrast", 0.1f, 10.0f, 2.2f },
    { "sigmoid", "contrast_skewness", -1.0f, 1.0f, 0.4f },
    { "sigmoid", "hue_preservation", 0.0f, 100.0f, 80.0f },
    { "sharpen", "amount", 0.0f, 2.0f, 1.3f },
    { "sharpen", "threshold", 0.0f, 100.0f, 0.0f },
  };
  for(size_t k = 0; k < sizeof(scalar_controls) / sizeof(scalar_controls[0]); k++)
  {
    char field[128];
    snprintf(field, sizeof(field), "\"name\":\"%s\",\"label\":", scalar_controls[k].parameter);
    CHECK(module_has(modules, scalar_controls[k].operation, field));
    ENGINE_OK(dt_mobile_set_parameter(session, scalar_controls[k].operation,
                                     scalar_controls[k].parameter, scalar_controls[k].minimum));
    ENGINE_OK(dt_mobile_set_parameter(session, scalar_controls[k].operation,
                                     scalar_controls[k].parameter, scalar_controls[k].maximum));
    ENGINE_OK(dt_mobile_set_parameter(session, scalar_controls[k].operation,
                                     scalar_controls[k].parameter, scalar_controls[k].edited));
    ENGINE_OK(dt_mobile_modules(session, &curve_json));
    CHECK(parameter_equals(curve_json, scalar_controls[k].operation,
                           scalar_controls[k].parameter, scalar_controls[k].edited));
    dt_mobile_free_buffer(curve_json); curve_json = NULL;
  }
  /* JNI supplies a double parsed from the user's decimal input. Its float
   * canonical value equals the native lower bound even though 0.1 < 0.1f. */
  ENGINE_OK(dt_mobile_set_parameter(session, "sigmoid", "middle_grey_contrast", 0.1));
  ENGINE_OK(dt_mobile_modules(session, &curve_json));
  CHECK(parameter_equals(curve_json, "sigmoid", "middle_grey_contrast", (double)0.1f));
  dt_mobile_free_buffer(curve_json); curve_json = NULL;
  ENGINE_OK(dt_mobile_set_parameter(session, "sigmoid", "middle_grey_contrast", 2.2f));
  CHECK(module_has(modules, "sharpen", "\"name\":\"radius\""));
  CHECK(module_has(modules, "sigmoid", "\"name\":\"red_inset\""));
  CHECK(dt_mobile_set_parameter(session, "sharpen", "radius", 0.01)
        == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_modules(session, &scalar_json));
  for(size_t k = 0; k < sizeof(scalar_controls) / sizeof(scalar_controls[0]); k++)
  {
    CHECK(dt_mobile_set_parameter(session, scalar_controls[k].operation, scalar_controls[k].parameter,
                                  scalar_controls[k].minimum - 1.0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    CHECK(dt_mobile_set_parameter(session, scalar_controls[k].operation, scalar_controls[k].parameter,
                                  scalar_controls[k].maximum + 1.0) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    CHECK(dt_mobile_set_parameter(session, scalar_controls[k].operation, scalar_controls[k].parameter,
                                  NAN) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    CHECK(dt_mobile_set_parameter(session, scalar_controls[k].operation, scalar_controls[k].parameter,
                                  1e300) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  }
  ENGINE_OK(dt_mobile_modules(session, &curve_json));
  CHECK(!strcmp(scalar_json, curve_json)); /* Every rejected scalar leaves history/params untouched. */
  dt_mobile_free_buffer(scalar_json); scalar_json = NULL;
  dt_mobile_free_buffer(curve_json); curve_json = NULL;
  ENGINE_OK(dt_mobile_set_enabled(session, "sigmoid", 0));
  ENGINE_OK(dt_mobile_set_enabled(session, "sharpen", 0));
  CHECK(dt_mobile_set_parameter(session, "rgbcurve", "curve_num_nodes[0]", 10000.0)
        == DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_parameter(session, "basecurve", "basecurve_nodes[0]", 0.0)
        == DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_parameter(session, "basecurve", "exposure_fusion", 10000.0)
        == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  /* The desktop exposure widget keeps black below its exposure-derived white
   * point. Numeric updates must preserve that relation before saving history. */
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "black", 0.0));
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 0.0));
  CHECK(dt_mobile_set_parameter(session, "exposure", "black", 1.0)
        == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "black", 0.5));
  CHECK(dt_mobile_set_parameter(session, "exposure", "exposure", 1.0)
        == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "black", 0.0));
  ENGINE_OK(dt_mobile_export_formats(session, &formats));
  CHECK(formats && strstr(formats, "png") && strstr(formats, "jpeg"));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  CHECK(before && width > 0 && height > 0 && width <= 96 && height <= 96);

  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 1.0));
  ENGINE_OK(dt_mobile_set_enabled(session, "exposure", 1));
  CHECK(dt_mobile_set_parameter(session, "exposure", "exposure", 1000.0)
        == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_parameter(session, "exposure", "exposure", NAN)
        == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &after, &edited_width, &edited_height));
  CHECK(after && width == edited_width && height == edited_height);
  CHECK(memcmp(before, after, (size_t)width * height * 4));

  /* These controls must alter the actual desktop pixelpipe, rather than only
   * round-trip through metadata. Keep the other new module disabled so the
   * comparison isolates each processing tool. */
  dt_mobile_free_buffer(before); before = NULL;
  dt_mobile_free_buffer(after); after = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  ENGINE_OK(dt_mobile_set_parameter(session, "sigmoid", "middle_grey_contrast", 2.2f));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &after, &edited_width, &edited_height));
  CHECK(before && after && width == edited_width && height == edited_height);
  CHECK(memcmp(before, after, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_set_enabled(session, "sigmoid", 0));
  dt_mobile_free_buffer(before); before = NULL;
  dt_mobile_free_buffer(after); after = NULL;
  ENGINE_OK(dt_mobile_set_parameter(session, "sharpen", "amount", 0.0));
  ENGINE_OK(dt_mobile_set_parameter(session, "sharpen", "threshold", 0.0));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  ENGINE_OK(dt_mobile_set_parameter(session, "sharpen", "amount", 2.0));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &after, &edited_width, &edited_height));
  CHECK(before && after && width == edited_width && height == edited_height);
  CHECK(memcmp(before, after, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_set_enabled(session, "sharpen", 0));

  /* The dedicated batch editor can change a real curve without exposing its
   * node count as an unsafe independent scalar. Metadata samples and processing
   * both use the upstream spline implementation. */
  CHECK(rgb_curve_options_checks(session));
  CHECK(wb_checks(session));
  CHECK(module_has(modules, "basecurve", "\"curves\":[{\"channel\":0"));
  CHECK(module_has(modules, "rgbcurve", "\"curves\":[{\"channel\":0"));
  CHECK(!module_has(modules, "basecurve", "\"channel\":1"));
  CHECK(!module_has(modules, "rgbcurve", "\"channel\":1")); /* Default linked mode. */
  dt_mobile_free_buffer(modules); modules = NULL;
  const dt_mobile_curve_point identity[] = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
  const dt_mobile_curve_point curved[] = { { 0.0f, 0.0f }, { 0.5f, 0.2f }, { 1.0f, 1.0f } };
  for(int interpolation = 0; interpolation <= 2; interpolation++)
  {
    ENGINE_OK(dt_mobile_set_curve(session, "basecurve", 0, interpolation, identity, 2));
    ENGINE_OK(dt_mobile_modules(session, &curve_json));
    char choice[40]; snprintf(choice, sizeof(choice), "\"interpolation\":%d", interpolation);
    CHECK(module_has(curve_json, "basecurve", choice));
    CHECK(module_has(curve_json, "basecurve", "\"samples\":[{\"x\":0"));
    dt_mobile_free_buffer(curve_json); curve_json = NULL;
  }
  ENGINE_OK(dt_mobile_set_curve(session, "rgbcurve", 0, 2, identity, 2));
  ENGINE_OK(dt_mobile_modules(session, &modules));
  const dt_mobile_curve_point reversed[] = { { 0.0f, 0.0f }, { 0.8f, 0.2f }, { 0.5f, 1.0f } };
  const dt_mobile_curve_point nonfinite[] = { { 0.0f, 0.0f }, { 1.0f, NAN } };
  CHECK(dt_mobile_set_curve(session, "basecurve", 0, 2, reversed, 3) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_curve(session, "basecurve", 0, 2, nonfinite, 2) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_curve(session, "basecurve", 1, 2, identity, 2) == DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_curve(session, "rgbcurve", 1, 2, identity, 2) == DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_curve(session, "exposure", 0, 2, identity, 2) == DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_curve(session, "basecurve@999", 0, 2, identity, 2) == DT_MOBILE_ERROR_UNSUPPORTED);
  ENGINE_OK(dt_mobile_modules(session, &curve_json));
  CHECK(!strcmp(modules, curve_json)); /* Rejected batches never change state. */
  dt_mobile_free_buffer(curve_json); curve_json = NULL;
  dt_mobile_free_buffer(before); before = NULL;
  dt_mobile_free_buffer(after); after = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  ENGINE_OK(dt_mobile_set_curve(session, "basecurve", 0, 2, curved, 3));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &after, &edited_width, &edited_height));
  CHECK(after && before && width == edited_width && height == edited_height);
  CHECK(memcmp(before, after, (size_t)width * height * 4));

  ENGINE_OK(dt_mobile_export(session, argv[2], "png", 90));
  CHECK(png_dimensions(argv[2], &output_width, &output_height));
  CHECK(output_width == source_width && output_height == source_height);
  CHECK(output_width > (uint32_t)width && output_height > (uint32_t)height);

  /* Cropping writes the rectangle and freehand mode atomically, then delegates
   * the actual dimensions and pixels to the desktop crop operation. Structural
   * coordinates/aspect fields remain unavailable to the generic scalar API. */
  dt_mobile_free_buffer(modules); modules = NULL;
  ENGINE_OK(dt_mobile_modules(session, &modules));
  CHECK(module_has(modules, "crop", "\"crop\":{\"left\":"));
  CHECK(!module_has(modules, "crop", "\"name\":\"cx\""));
  CHECK(!module_has(modules, "crop", "\"name\":\"ratio_n\""));
  CHECK(dt_mobile_set_parameter(session, "crop", "cx", 0.25) == DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_crop(session, "crop", 0.75f, 0.0f, 0.25f, 1.0f)
        == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_crop(session, "crop", 0.5f, 0.0f, nextafterf(0.51f, 0.0f), 1.0f)
        == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_crop(session, "crop", 0.0f, NAN, 1.0f, 1.0f)
        == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  CHECK(dt_mobile_set_crop(session, "exposure", 0.0f, 0.0f, 1.0f, 1.0f)
        == DT_MOBILE_ERROR_UNSUPPORTED);
  CHECK(dt_mobile_set_crop(session, "crop@999", 0.0f, 0.0f, 1.0f, 1.0f)
        == DT_MOBILE_ERROR_UNSUPPORTED);
  ENGINE_OK(dt_mobile_modules(session, &curve_json));
  CHECK(!strcmp(modules, curve_json)); /* Rejection must not enable the module or alter history. */
  dt_mobile_free_buffer(curve_json); curve_json = NULL;

  /* Desktop sliders add minimum widths in float space. The exact difference
   * of 0.51f and 0.5f is slightly less than 0.01f; the legal edge still passes. */
  ENGINE_OK(dt_mobile_set_crop(session, "crop", 0.5f, 0.5f, 0.51f, 0.51f));
  ENGINE_OK(dt_mobile_export(session, argv[2], "png", 90));
  CHECK(png_dimensions(argv[2], &output_width, &output_height));
  CHECK(output_width == 4 && output_height == 4); /* Upstream minimum ROI for this 256px fixture. */

  ENGINE_OK(dt_mobile_set_crop(session, "crop", 0.25f, 0.0f, 0.75f, 1.0f));
  ENGINE_OK(dt_mobile_modules(session, &curve_json));
  CHECK(module_has(curve_json, "crop", "\"crop\":{\"left\":0.25,\"top\":0,\"right\":0.75,\"bottom\":1,"));
  CHECK(module_has(curve_json, "crop", "\"freehand\":true"));
  dt_mobile_free_buffer(curve_json); curve_json = NULL;
  dt_mobile_free_buffer(after); after = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &after, &edited_width, &edited_height));
  CHECK(after && edited_width > 0 && edited_height > 0);
  CHECK(width * edited_height != edited_width * height); /* Cropped aspect, not merely a metadata edit. */
  ENGINE_OK(dt_mobile_export(session, argv[2], "png", 90));
  CHECK(png_dimensions(argv[2], &output_width, &output_height));
  CHECK(output_width == source_width / 2 && output_height == source_height);

  ENGINE_OK(dt_mobile_set_crop(session, "crop", 0.0f, 0.0f, 1.0f, 1.0f));
  dt_mobile_free_buffer(after); after = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &after, &edited_width, &edited_height));
  CHECK(after && edited_width == width && edited_height == height);
  ENGINE_OK(dt_mobile_export(session, argv[2], "png", 90));
  CHECK(png_dimensions(argv[2], &output_width, &output_height));
  CHECK(output_width == source_width && output_height == source_height);
  CHECK(export_settings_checks(session, argv[1], argv[2], source_width, source_height));
  CHECK(history_checks(session, argv[2], source_width, source_height));
  CHECK(instance_checks(session));
  CHECK(export_midstream_cancel_checks(session, argv[2]));
  dt_mobile_cancel(session);
  CHECK(export_cancelled_settings_checks(session, argv[2]));
  CHECK(dt_mobile_undo(session) == DT_MOBILE_ERROR_CANCELLED);
  CHECK(dt_mobile_redo(session) == DT_MOBILE_ERROR_CANCELLED);
  curve_json = (char *)1;
  CHECK(dt_mobile_history(session, &curve_json) == DT_MOBILE_ERROR_CANCELLED);
  CHECK(curve_json == NULL);
  CHECK(dt_mobile_set_curve(session, "basecurve", 0, 2, identity, 2) == DT_MOBILE_ERROR_CANCELLED);
  CHECK(dt_mobile_set_crop(session, "crop", 0.0f, 0.0f, 1.0f, 1.0f) == DT_MOBILE_ERROR_CANCELLED);
  dt_mobile_free_buffer(before); before = NULL;
  dt_mobile_free_buffer(after); after = NULL;
  dt_mobile_free_buffer(modules); modules = NULL;
  dt_mobile_free_buffer(formats); formats = NULL;
  dt_mobile_close(session); session = NULL;
  CHECK(runtime_children(temporary_root) == 0);
  /* Android closes its previous document before opening the next one. Verify
   * that dropping the last runtime user does not break the following open. */
  ENGINE_OK(dt_mobile_open(argv[1], &session));
  ENGINE_OK(dt_mobile_history(session, &curve_json));
  CHECK(strstr(curve_json, "\"position\":0,\"length\":0"));
  dt_mobile_free_buffer(curve_json); curve_json = NULL;
  /* Default-profile export must resolve colorin/colorout from the new runtime;
   * cached module pointers from the previous document have been unloaded. */
  ENGINE_OK(dt_mobile_export(session, argv[2], "png", 90));
  CHECK(png_dimensions(argv[2], &output_width, &output_height));
  CHECK(output_width == source_width && output_height == source_height);
  CHECK(mobile_color_controls_checks(session));
  CHECK(mobile_tool_controls_checks(session));
  CHECK(mobile_blending_checks(session));
  CHECK(mobile_parametric_checks(session));
  CHECK(mobile_crop_geometry_checks(session, argv[2]));
  CHECK(mobile_text_controls_checks(session));
  CHECK(mobile_mask_controls_checks(session));
  CHECK(mobile_lens_checks(session));
  CHECK(mobile_tone_equalizer_checks(session));
  CHECK(mobile_path_mask_checks(session));
  CHECK(mobile_array_checks(session));
  CHECK(mobile_extended_controls_checks(session));
  CHECK(mobile_file_controls_checks(session));
  ENGINE_OK(dt_mobile_render_preview(session, 96, 96, &before, &width, &height));
  CHECK(before && width > 0 && height > 0);
  dt_mobile_free_buffer(before);
  before = NULL;
  dt_mobile_close(session); session = NULL;
  CHECK(runtime_children(temporary_root) == 0);
  CHECK(rmdir(temporary_root) == 0);
  free(temporary_root);
  free(data_directory);
  free(module_directory);
  remove(argv[2]);
  return EXIT_SUCCESS;

failed:
  dt_mobile_free_buffer(before);
  dt_mobile_free_buffer(after);
  dt_mobile_free_buffer(modules);
  dt_mobile_free_buffer(formats);
  dt_mobile_free_buffer(curve_json);
  dt_mobile_free_buffer(scalar_json);
  dt_mobile_close(session);
  free(data_copy);
  free(module_copy);
  free(data_directory);
  free(module_directory);
  if(temporary_root) { rmdir(temporary_root); free(temporary_root); }
  if(argc == 3) remove(argv[2]);
  return EXIT_FAILURE;
}
