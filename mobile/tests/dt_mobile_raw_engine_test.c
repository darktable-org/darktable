/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dt_mobile.h"
#include <stdbool.h>
#include "gpr.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This test starts at the public mobile API with an actual VC-5 GPR file.
 * The SDK is used only to read the expected sensor dimensions, independently
 * of darktable's rendered/exported image. Checks stay active in Release.
 * Optional CLI: INPUT.GPR OUTPUT.PNG --whitebalance also exercises original
 * three-channel RAW white balance, Kelvin/tint and pixel-exact session undo/redo. */
#define CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
  goto failed; } } while(0)
#define ENGINE_OK(call) do { dt_mobile_status status = (call); \
  if(status != DT_MOBILE_OK) { fprintf(stderr, "%s: %s\n", #call, \
    session ? dt_mobile_last_error(session) : dt_mobile_open_error()); goto failed; } } while(0)

static uint32_t read_be32(const unsigned char *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int png_dimensions(const char *path, uint32_t *width, uint32_t *height)
{
  unsigned char header[24];
  FILE *file = fopen(path, "rb");
  if(!file) return 0;
  const size_t count = fread(header, 1, sizeof(header), file);
  fclose(file);
  if(count != sizeof(header) || memcmp(header, "\211PNG\r\n\032\n", 8)
     || memcmp(header + 12, "IHDR", 4)) return 0;
  *width = read_be32(header + 16);
  *height = read_be32(header + 20);
  return 1;
}

static int whitebalance_metadata(const char *json, int setting)
{
  const char *module = strstr(json, "{\"operation\":\"temperature\",");
  if(!module) return 0;
  const char *module_end = strstr(module + 1, "{\"operation\":");
  const char *balance = strstr(module, "\"whiteBalance\":{");
  if(!balance || (module_end && balance >= module_end)) return 0;
  const char *channels = strstr(balance, "\"channels\":[");
  const char *channels_end = channels ? strchr(channels, ']') : NULL;
  if(!channels_end || (module_end && channels_end >= module_end)) return 0;
  size_t count = 0;
  for(const char *entry = channels; (entry = strstr(entry, "\"index\":")) && entry < channels_end; entry++)
    count++;
  if(count != 3) return 0;
  for(int index = 0; index < 3; index++)
  {
    char key[32];
    snprintf(key, sizeof(key), "\"index\":%d,", index);
    const char *entry = strstr(channels, key);
    if(!entry || entry >= channels_end) return 0;
  }
  char key[32];
  snprintf(key, sizeof(key), "\"setting\":%d,", setting);
  const char *entry = strstr(balance, key);
  return entry && (!module_end || entry < module_end);
}

static int whitebalance_checks(dt_mobile_session *session)
{
  uint8_t *neutral = NULL, *changed = NULL, *restored = NULL, *redone = NULL, *temperature = NULL;
  char *metadata = NULL;
  int width = 0, height = 0, other_width = 0, other_height = 0;
  int success = 0;
  const float neutral_gains[] = { 1.0f, 1.0f, 1.0f };
  const float red_gains[] = { 1.25f, 1.0f, 1.0f };

  /* Both settings use this RAW image's original as-shot basis. The late
   * adaptation mode must travel with those gains, not just change a label. */
  ENGINE_OK(dt_mobile_set_whitebalance_setting(session, "temperature", 0));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(whitebalance_metadata(metadata, 0));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_set_whitebalance_setting(session, "temperature", 4));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(whitebalance_metadata(metadata, 4));
  dt_mobile_free_buffer(metadata); metadata = NULL;

  ENGINE_OK(dt_mobile_set_whitebalance_gains(session, "temperature", neutral_gains, 3));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(whitebalance_metadata(metadata, 2));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 192, 192, &neutral, &width, &height));
  CHECK(neutral && width > 0 && height > 0 && width <= 192 && height <= 192);
  ENGINE_OK(dt_mobile_set_whitebalance_gains(session, "temperature", red_gains, 3));
  ENGINE_OK(dt_mobile_render_preview(session, 192, 192, &changed, &other_width, &other_height));
  CHECK(changed && width == other_width && height == other_height);
  CHECK(memcmp(neutral, changed, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_undo(session));
  ENGINE_OK(dt_mobile_render_preview(session, 192, 192, &restored, &other_width, &other_height));
  CHECK(restored && width == other_width && height == other_height);
  CHECK(!memcmp(neutral, restored, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_redo(session));
  ENGINE_OK(dt_mobile_render_preview(session, 192, 192, &redone, &other_width, &other_height));
  CHECK(redone && width == other_width && height == other_height);
  CHECK(!memcmp(changed, redone, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_set_whitebalance_temperature(session, "temperature", 6500.0, 1.1));
  ENGINE_OK(dt_mobile_modules(session, &metadata));
  CHECK(whitebalance_metadata(metadata, 2));
  const char *balance = strstr(metadata, "\"whiteBalance\":{");
  CHECK(balance && strstr(balance, "\"temperature\":{\"kelvin\":"));
  dt_mobile_free_buffer(metadata); metadata = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 192, 192, &temperature, &other_width, &other_height));
  CHECK(temperature && width == other_width && height == other_height);
  CHECK(memcmp(changed, temperature, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_undo(session));
  dt_mobile_free_buffer(restored); restored = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 192, 192, &restored, &other_width, &other_height));
  CHECK(restored && width == other_width && height == other_height);
  CHECK(!memcmp(changed, restored, (size_t)width * height * 4));
  ENGINE_OK(dt_mobile_redo(session));
  dt_mobile_free_buffer(redone); redone = NULL;
  ENGINE_OK(dt_mobile_render_preview(session, 192, 192, &redone, &other_width, &other_height));
  CHECK(redone && width == other_width && height == other_height);
  CHECK(!memcmp(temperature, redone, (size_t)width * height * 4));
  /* Keep the Kelvin/tint edit active for the normal full-resolution export below. */
  success = 1;
failed:
  dt_mobile_free_buffer(neutral); dt_mobile_free_buffer(changed);
  dt_mobile_free_buffer(restored); dt_mobile_free_buffer(redone);
  dt_mobile_free_buffer(metadata); dt_mobile_free_buffer(temperature);
  return success;
}

int main(int argc, char **argv)
{
  dt_mobile_session *session = NULL, *exported = NULL;
  uint8_t *before = NULL, *after = NULL, *export_preview = NULL;
  char *modules = NULL;
  int width = 0, height = 0, edited_width = 0, edited_height = 0;
  int export_preview_width = 0, export_preview_height = 0;
  int output_argument_valid = 0;
  uint32_t output_width = 0, output_height = 0;
  const gpr_allocator allocator = { malloc, free };
  gpr_buffer original = { 0 }, unchanged = { 0 };
  gpr_parameters source_info;
  gpr_parameters_set_defaults(&source_info);
  if(argc != 3 && !(argc == 4 && !strcmp(argv[3], "--whitebalance")))
  {
    fprintf(stderr, "usage: %s INPUT.GPR OUTPUT.PNG [--whitebalance]\n", argv[0]);
    goto failed;
  }
  output_argument_valid = 1;
  CHECK((dt_mobile_capabilities() & (DT_MOBILE_CAP_ENGINE | DT_MOBILE_CAP_GPR))
        == (DT_MOBILE_CAP_ENGINE | DT_MOBILE_CAP_GPR));
  CHECK(read_from_file(&original, argv[1], allocator.Alloc, allocator.Free) == 0);
  CHECK(gpr_parse_metadata(&allocator, &original, &source_info));
  CHECK(source_info.input_width > 192 && source_info.input_height > 192);

  ENGINE_OK(dt_mobile_open(argv[1], &session));
  ENGINE_OK(dt_mobile_modules(session, &modules));
  CHECK(modules && strstr(modules, "\"operation\":\"exposure\""));
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 0.0));
  ENGINE_OK(dt_mobile_render_preview(session, 192, 192, &before, &width, &height));
  CHECK(before && width > 0 && height > 0 && width <= 192 && height <= 192);
  ENGINE_OK(dt_mobile_set_parameter(session, "exposure", "exposure", 1.0));
  ENGINE_OK(dt_mobile_render_preview(session, 192, 192, &after, &edited_width, &edited_height));
  CHECK(after && width == edited_width && height == edited_height);
  CHECK(memcmp(before, after, (size_t)width * height * 4) != 0);

  if(argc == 4) CHECK(whitebalance_checks(session));

  ENGINE_OK(dt_mobile_export(session, argv[2], "png", 90));
  CHECK(png_dimensions(argv[2], &output_width, &output_height));
  CHECK(output_width == source_info.input_width && output_height == source_info.input_height);
  CHECK(output_width > (uint32_t)width && output_height > (uint32_t)height);
  /* Decode the exported file as well as checking its header, so a file with a
   * plausible IHDR but invalid image data cannot pass the integration test. */
  CHECK(dt_mobile_open(argv[2], &exported) == DT_MOBILE_OK);
  CHECK(dt_mobile_render_preview(exported, 192, 192, &export_preview,
                                 &export_preview_width, &export_preview_height) == DT_MOBILE_OK);
  CHECK(export_preview && export_preview_width > 0 && export_preview_height > 0);
  CHECK(read_from_file(&unchanged, argv[1], allocator.Alloc, allocator.Free) == 0);
  CHECK(unchanged.size == original.size && !memcmp(unchanged.buffer, original.buffer, original.size));
  printf("%s: VC-5 GPR -> edited native preview -> %u x %u PNG -> decoded PNG; original unchanged\n",
         argv[1], output_width, output_height);
  if(argc == 4) printf("Native RAW white balance, pixel-exact undo/redo and edited full-resolution export verified\n");
  dt_mobile_free_buffer(before);
  dt_mobile_free_buffer(after);
  dt_mobile_free_buffer(export_preview);
  dt_mobile_free_buffer(modules);
  dt_mobile_close(exported);
  dt_mobile_close(session);
  gpr_parameters_destroy(&source_info, free);
  free(original.buffer);
  free(unchanged.buffer);
  remove(argv[2]);
  return EXIT_SUCCESS;

failed:
  dt_mobile_free_buffer(before);
  dt_mobile_free_buffer(after);
  dt_mobile_free_buffer(export_preview);
  dt_mobile_free_buffer(modules);
  dt_mobile_close(exported);
  dt_mobile_close(session);
  gpr_parameters_destroy(&source_info, free);
  free(original.buffer);
  free(unchanged.buffer);
  if(output_argument_valid) remove(argv[2]);
  return EXIT_FAILURE;
}
