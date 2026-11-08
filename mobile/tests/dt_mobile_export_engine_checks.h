/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DT_MOBILE_EXPORT_ENGINE_CHECKS_H
#define DT_MOBILE_EXPORT_ENGINE_CHECKS_H

#include "dt_mobile.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "imageio/imageio_module.h"
#include <glib/gstdio.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <signal.h>
#include <sys/resource.h>
#endif

typedef struct mobile_export_conf_snapshot {
  const char *key;
  gchar *value;
} mobile_export_conf_snapshot;

static const char *const mobile_export_conf_keys[] = {
  "plugins/imageio/format/jpeg/quality", "plugins/imageio/format/jpeg/subsample",
  "plugins/imageio/format/png/bpp", "plugins/imageio/format/png/compression",
  "plugins/imageio/format/tiff/bpp", "plugins/imageio/format/tiff/pixelformat",
  "plugins/imageio/format/tiff/compress", "plugins/imageio/format/tiff/compresslevel",
  "plugins/imageio/format/xcf/bpp", "metadata/resolution"
};
enum { MOBILE_EXPORT_CONF_COUNT = sizeof(mobile_export_conf_keys) / sizeof(mobile_export_conf_keys[0]) };

static void mobile_export_conf_capture(mobile_export_conf_snapshot *snapshot)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  for(size_t i = 0; i < MOBILE_EXPORT_CONF_COUNT; i++)
  {
    snapshot[i].key = mobile_export_conf_keys[i];
    snapshot[i].value = g_strdup(g_hash_table_lookup(darktable.conf->table, snapshot[i].key));
  }
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static int mobile_export_conf_matches(const mobile_export_conf_snapshot *snapshot)
{
  int same = 1;
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  for(size_t i = 0; i < MOBILE_EXPORT_CONF_COUNT; i++)
    if(g_strcmp0(snapshot[i].value, g_hash_table_lookup(darktable.conf->table, snapshot[i].key)))
    {
      fprintf(stderr, "export changed raw configuration: %s\n", snapshot[i].key);
      same = 0;
    }
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return same;
}

static void mobile_export_conf_restore(mobile_export_conf_snapshot *snapshot)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  for(size_t i = 0; i < MOBILE_EXPORT_CONF_COUNT; i++)
    if(snapshot[i].value)
      g_hash_table_replace(darktable.conf->table, g_strdup(snapshot[i].key), g_strdup(snapshot[i].value));
    else g_hash_table_remove(darktable.conf->table, snapshot[i].key);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static void mobile_export_conf_free(mobile_export_conf_snapshot *snapshot)
{
  for(size_t i = 0; i < MOBILE_EXPORT_CONF_COUNT; i++) g_free(snapshot[i].value);
}

static uint64_t mobile_export_integer(const unsigned char *bytes, size_t count, int little)
{
  uint64_t value = 0;
  for(size_t i = 0; i < count; i++) value = (value << 8) | bytes[little ? count - 1 - i : i];
  return value;
}

static int mobile_export_png_header(const char *path, uint32_t width, uint32_t height, unsigned int depth)
{
  unsigned char data[29];
  FILE *file = g_fopen(path, "rb");
  if(!file) return 0;
  const size_t count = fread(data, 1, sizeof(data), file);
  fclose(file);
  return count == sizeof(data) && !memcmp(data, "\211PNG\r\n\032\n", 8)
    && !memcmp(data + 12, "IHDR", 4)
    && mobile_export_integer(data + 16, 4, 0) == width
    && mobile_export_integer(data + 20, 4, 0) == height && data[24] == depth;
}

static gchar *mobile_export_png_profile_hash(const char *path)
{
  gchar *contents = NULL, *hash = NULL;
  gsize size = 0;
  if(!g_file_get_contents(path, &contents, &size, NULL)) return NULL;
  const unsigned char *data = (const unsigned char *)contents;
  for(size_t offset = 8; offset <= size && size - offset >= 12;)
  {
    const size_t length = mobile_export_integer(data + offset, 4, 0);
    if(length > size - offset - 12) break;
    if(!memcmp(data + offset + 4, "iCCP", 4) && length > 2)
    {
      const unsigned char *payload = data + offset + 8;
      const unsigned char *name_end = memchr(payload, 0, length);
      if(name_end && name_end > payload && name_end + 2 < payload + length && name_end[1] == 0)
        hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, payload, length);
      break;
    }
    offset += length + 12;
  }
  g_free(contents);
  return hash;
}

/* Read necessary IFD values only; encoding/decoding remains with the original
 * TIFF plugin. Both byte orders and classic/BigTIFF offset widths are accepted. */
static int mobile_export_tiff_tag(const unsigned char *data, size_t size, unsigned int wanted, uint64_t *value)
{
  if(size < 8 || !((data[0] == 'I' && data[1] == 'I') || (data[0] == 'M' && data[1] == 'M'))) return 0;
  const int little = data[0] == 'I';
  const uint64_t magic = mobile_export_integer(data + 2, 2, little);
  const int big = magic == 43;
  if(magic != 42 && !big) return 0;
  if(big && (size < 16 || mobile_export_integer(data + 4, 2, little) != 8
             || mobile_export_integer(data + 6, 2, little))) return 0;
  const size_t offset_size = big ? 8 : 4, count_size = big ? 8 : 2, entry_size = big ? 20 : 12;
  const uint64_t ifd = mobile_export_integer(data + (big ? 8 : 4), offset_size, little);
  if(ifd > size || count_size > size - (size_t)ifd) return 0;
  const uint64_t entries = mobile_export_integer(data + ifd, count_size, little);
  if(entries > (size - (size_t)ifd - count_size) / entry_size) return 0;
  for(uint64_t i = 0; i < entries; i++)
  {
    const unsigned char *entry = data + ifd + count_size + i * entry_size;
    if(mobile_export_integer(entry, 2, little) != wanted) continue;
    const uint64_t type = mobile_export_integer(entry + 2, 2, little);
    const size_t width = type == 3 ? 2 : type == 4 ? 4 : type == 16 ? 8 : 0;
    const uint64_t count = mobile_export_integer(entry + 4, big ? 8 : 4, little);
    if(!width || !count || count > SIZE_MAX / width) return 0;
    const unsigned char *payload = entry + (big ? 12 : 8);
    if(count * width > offset_size)
    {
      const uint64_t offset = mobile_export_integer(payload, offset_size, little);
      if(offset > size || count * width > size - (size_t)offset) return 0;
      payload = data + offset;
    }
    *value = mobile_export_integer(payload, width, little);
    return 1;
  }
  return 0;
}

static int mobile_export_tiff_header(const char *path, uint32_t width, uint32_t height,
                                     unsigned int depth, unsigned int sample_format,
                                     unsigned int compression, unsigned int predictor)
{
  gchar *contents = NULL;
  gsize size = 0;
  if(!g_file_get_contents(path, &contents, &size, NULL)) return 0;
  const unsigned char *data = (const unsigned char *)contents;
  const unsigned int tags[] = { 256, 257, 258, 339, 259, 317 };
  const uint64_t expected[] = { width, height, depth, sample_format, compression, predictor };
  int valid = 1;
  for(size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
  {
    uint64_t actual = 0;
    const int found = mobile_export_tiff_tag(data, size, tags[i], &actual);
    /* TIFF permits omitted default UINT samples, uncompressed data and NONE
     * predictor. Explicit float/deflate/predictor choices must be present. */
    if(!found && expected[i] == 1 && (i == 3 || i == 4 || i == 5)) continue;
    if(!found || actual != expected[i])
    {
      fprintf(stderr, "TIFF tag %u: found=%d value=%llu expected=%llu\n", tags[i], found,
              (unsigned long long)actual, (unsigned long long)expected[i]);
      valid = 0;
    }
  }
  g_free(contents);
  return valid;
}

static int mobile_export_jpeg_header(const char *path, unsigned char sampling, unsigned int dpi)
{
  gchar *contents = NULL;
  gsize size = 0;
  if(!g_file_get_contents(path, &contents, &size, NULL)) return 0;
  const unsigned char *data = (const unsigned char *)contents;
  int found_sof = 0, found_density = 0;
  if(size < 2 || data[0] != 0xff || data[1] != 0xd8) goto finished;
  for(size_t offset = 2; offset + 4 <= size;)
  {
    if(data[offset++] != 0xff) break;
    while(offset < size && data[offset] == 0xff) offset++;
    if(offset >= size) break;
    const unsigned int marker = data[offset++];
    if(marker == 0xda || marker == 0xd9) break;
    if(marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    if(offset + 2 > size) break;
    const size_t length = mobile_export_integer(data + offset, 2, 0);
    if(length < 2 || length > size - offset) break;
    const unsigned char *payload = data + offset + 2;
    if(marker == 0xe0 && length >= 16 && !memcmp(payload, "JFIF\0", 5))
      found_density = payload[7] == 1 && mobile_export_integer(payload + 8, 2, 0) == dpi
        && mobile_export_integer(payload + 10, 2, 0) == dpi;
    if((marker == 0xc0 || marker == 0xc1 || marker == 0xc2) && length >= 17)
      found_sof = payload[0] == 8 && payload[5] == 3 && payload[7] == sampling
        && payload[10] == 0x11 && payload[13] == 0x11;
    offset += length;
  }
finished:
  g_free(contents);
  return found_sof && found_density;
}

/* Check the real writer's image header. This is a storage/precision assertion,
 * not an independent pixel decoder or proof of GIMP import compatibility. */
static int mobile_export_xcf_header(const char *path, uint32_t width, uint32_t height,
                                    unsigned int precision)
{
  unsigned char data[30];
  FILE *file = g_fopen(path, "rb");
  if(!file) return 0;
  const size_t count = fread(data, 1, sizeof(data), file);
  fclose(file);
  return count == sizeof(data) && !memcmp(data, "gimp xcf v012\0", 14)
    && mobile_export_integer(data + 14, 4, 0) == width
    && mobile_export_integer(data + 18, 4, 0) == height
    && mobile_export_integer(data + 22, 4, 0) == 0 /* RGB */
    && mobile_export_integer(data + 26, 4, 0) == precision;
}

static int export_settings_checks(dt_mobile_session *session, const char *image_path,
                                  const char *output_path, uint32_t source_width, uint32_t source_height)
{
  int success = 0, snapshots_ready = 0, override_changed = 0;
  char *formats = NULL, *modules_before = NULL, *modules_after = NULL;
  gchar *source_before = NULL, *source_after = NULL, *previous_override = NULL;
  gchar *srgb_profile_hash = NULL, *adobe_profile_hash = NULL;
  gsize source_size = 0, after_size = 0;
  gchar *png = g_strconcat(output_path, ".settings.png", NULL);
  gchar *tiff = g_strconcat(output_path, ".settings.tiff", NULL);
  gchar *jpeg = g_strconcat(output_path, ".settings.jpg", NULL);
  gchar *xcf = g_strconcat(output_path, ".settings.xcf", NULL);
  gchar *missing = g_strconcat(output_path, ".missing-directory/output.png", NULL);
  gchar *alias = g_strconcat(output_path, ".source-alias", NULL);
  mobile_export_conf_snapshot original[MOBILE_EXPORT_CONF_COUNT] = {{0}}, expected[MOBILE_EXPORT_CONF_COUNT] = {{0}};
  dt_mobile_export_settings settings = DT_MOBILE_EXPORT_SETTINGS_INIT;
#define EXPORT_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "export settings:%d: check failed: %s\n", __LINE__, #condition); goto finished; } } while(0)
#define EXPORT_OK(call) do { const dt_mobile_status result = (call); if(result != DT_MOBILE_OK) { \
  fprintf(stderr, "export settings: %s: %s\n", #call, dt_mobile_last_error(session)); goto finished; } } while(0)
  EXPORT_OK(dt_mobile_export_formats(session, &formats));
  EXPORT_OK(dt_mobile_modules(session, &modules_before));
  EXPORT_CHECK(formats && strstr(formats, "\"options\":{") && source_width >= 8 && source_height >= 8);
  EXPORT_CHECK(g_file_get_contents(image_path, &source_before, &source_size, NULL));
  mobile_export_conf_capture(original);
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  /* Exercise exact string restoration and absence, not just effective ints. */
  g_hash_table_replace(darktable.conf->table, g_strdup(mobile_export_conf_keys[0]), g_strdup("095"));
  g_hash_table_remove(darktable.conf->table, "plugins/imageio/format/png/compression");
  g_hash_table_replace(darktable.conf->table, g_strdup("plugins/imageio/format/xcf/bpp"), g_strdup("016"));
  previous_override = g_strdup(g_hash_table_lookup(darktable.conf->override_entries, mobile_export_conf_keys[0]));
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  mobile_export_conf_capture(expected);
  snapshots_ready = 1;
  /* The supplied source fixture is square. A real upstream crop makes the
   * output rectangular, so preserving aspect cannot pass by coincidence. */
  EXPORT_OK(dt_mobile_set_crop(session, "crop", 0.25f, 0.0f, 0.75f, 1.0f));
  const uint32_t cropped_width = source_width / 2;
  settings.max_width = source_width / 4;
  settings.max_height = 0;
  settings.pixel_format = DT_MOBILE_EXPORT_UINT8;
  settings.png_compression = 9;
  settings.output_profile = DT_MOBILE_EXPORT_PROFILE_SRGB;
  settings.rendering_intent = DT_MOBILE_EXPORT_INTENT_RELATIVE;
  settings.metadata_policy = DT_MOBILE_EXPORT_METADATA_SOURCE_NONE;
  EXPORT_OK(dt_mobile_export_with_settings(session, png, "png", &settings));
  const uint32_t half_height = (uint32_t)((uint64_t)source_height * settings.max_width / cropped_width);
  EXPORT_CHECK(mobile_export_png_header(png, settings.max_width, half_height, 8));
  srgb_profile_hash = mobile_export_png_profile_hash(png);
  EXPORT_CHECK(srgb_profile_hash != NULL);
  EXPORT_CHECK(mobile_export_conf_matches(expected));
  if(strstr(formats, "\"value\":2,\"label\":\"Adobe RGB\""))
  {
    settings.output_profile = DT_MOBILE_EXPORT_PROFILE_ADOBE_RGB;
    EXPORT_OK(dt_mobile_export_with_settings(session, png, "png", &settings));
    adobe_profile_hash = mobile_export_png_profile_hash(png);
    EXPORT_CHECK(adobe_profile_hash && strcmp(srgb_profile_hash, adobe_profile_hash));
    EXPORT_CHECK(mobile_export_conf_matches(expected));
    settings.output_profile = DT_MOBILE_EXPORT_PROFILE_SRGB;
  }
  settings.max_width = source_width * 2;
  settings.max_height = source_height * 2;
  settings.pixel_format = DT_MOBILE_EXPORT_UINT16;
  settings.png_compression = 0;
  EXPORT_OK(dt_mobile_export_with_settings(session, png, "png", &settings));
  EXPORT_CHECK(mobile_export_png_header(png, cropped_width, source_height, 16));
  EXPORT_CHECK(mobile_export_conf_matches(expected));
  /* With both bounds set, height is the limiting edge of the 1:2 crop. */
  settings.max_width = source_width / 2;
  settings.max_height = source_height / 4;
  EXPORT_OK(dt_mobile_export_with_settings(session, png, "png", &settings));
  EXPORT_CHECK(mobile_export_png_header(png, source_width / 8, source_height / 4, 16));
  EXPORT_OK(dt_mobile_set_crop(session, "crop", 0.0f, 0.0f, 1.0f, 1.0f));
  EXPORT_CHECK(mobile_export_conf_matches(expected));
  EXPORT_CHECK(dt_mobile_export_with_settings(session, missing, "png", &settings) == DT_MOBILE_ERROR_IO);
  EXPORT_CHECK(mobile_export_conf_matches(expected));
  EXPORT_CHECK(dt_mobile_export_with_settings(session, image_path, "png", &settings) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  EXPORT_CHECK(link(image_path, alias) == 0);
  EXPORT_CHECK(dt_mobile_export_with_settings(session, alias, "png", &settings) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  settings.pixel_format = DT_MOBILE_EXPORT_FLOAT32;
  EXPORT_CHECK(dt_mobile_export_with_settings(session, png, "png", &settings) == DT_MOBILE_ERROR_UNSUPPORTED);
  settings.pixel_format = DT_MOBILE_EXPORT_UINT8;
  settings.max_width = UINT32_MAX;
  EXPORT_CHECK(dt_mobile_export_with_settings(session, png, "png", &settings) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  settings.max_width = 0;
  settings.struct_size = 0;
  EXPORT_CHECK(dt_mobile_export_with_settings(session, png, "png", &settings) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  EXPORT_CHECK(mobile_export_conf_matches(expected));
  if(strstr(formats, "\"id\":\"tiff\""))
  {
    const int pixel_formats[] = { DT_MOBILE_EXPORT_UINT8, DT_MOBILE_EXPORT_UINT16, DT_MOBILE_EXPORT_FLOAT32 };
    const unsigned int depths[] = { 8, 16, 32 };
    for(size_t i = 0; i < 3; i++)
    {
      settings = (dt_mobile_export_settings)DT_MOBILE_EXPORT_SETTINGS_INIT;
      settings.pixel_format = pixel_formats[i];
      settings.tiff_compression = i ? 2 : 0;
      settings.tiff_compression_level = 3;
      settings.dpi = 240;
      settings.metadata_policy = DT_MOBILE_EXPORT_METADATA_SOURCE_NONE;
      EXPORT_OK(dt_mobile_export_with_settings(session, tiff, "tiff", &settings));
      EXPORT_CHECK(mobile_export_tiff_header(tiff, source_width, source_height, depths[i], i == 2 ? 3 : 1,
                                             i ? 8 : 1, i ? (i == 2 ? 3 : 2) : 1));
      EXPORT_CHECK(mobile_export_conf_matches(expected));
    }
  }
  const char *xcf_options = strstr(formats, "\"id\":\"xcf\"");
#ifdef DT_MOBILE_TEST_REQUIRE_XCF
  EXPORT_CHECK(xcf_options);
#endif
  if(xcf_options)
  {
    const char *next_format = strstr(xcf_options + 1, "{\"id\":");
    gchar *options = next_format ? g_strndup(xcf_options, next_format - xcf_options) : g_strdup(xcf_options);
    const int advertised = strstr(options, "\"mimeType\":\"image/x-xcf\"")
      && strstr(options, "{\"value\":1,\"label\":\"8 bit unsigned\"}")
      && strstr(options, "{\"value\":2,\"label\":\"16 bit unsigned\"}")
      && strstr(options, "{\"value\":3,\"label\":\"32 bit float\"}")
      && !strstr(options, "\"pngCompression\"") && !strstr(options, "\"tiffCompression\"")
      && !strstr(options, "\"jpegSubsample\"") && !strstr(options, "\"dpi\"");
    g_free(options);
    EXPORT_CHECK(advertised);
    const int pixel_formats[] = { DT_MOBILE_EXPORT_UINT8, DT_MOBILE_EXPORT_UINT16, DT_MOBILE_EXPORT_FLOAT32 };
    /* XCF stores the transfer function in its precision identifier. sRGB
     * must use gamma encodings, whereas linear Rec709 must use linear. */
    const unsigned int precisions[] = { 150, 250, 650 };
    for(size_t i = 0; i < 3; i++)
    {
      settings = (dt_mobile_export_settings)DT_MOBILE_EXPORT_SETTINGS_INIT;
      settings.pixel_format = pixel_formats[i];
      settings.output_profile = DT_MOBILE_EXPORT_PROFILE_SRGB;
      settings.metadata_policy = DT_MOBILE_EXPORT_METADATA_SOURCE_NONE;
      EXPORT_OK(dt_mobile_export_with_settings(session, xcf, "xcf", &settings));
      EXPORT_CHECK(mobile_export_xcf_header(xcf, source_width, source_height, precisions[i]));
      EXPORT_CHECK(mobile_export_conf_matches(expected));
    }
    settings.output_profile = DT_MOBILE_EXPORT_PROFILE_LINEAR_REC709;
    EXPORT_OK(dt_mobile_export_with_settings(session, xcf, "xcf", &settings));
    EXPORT_CHECK(mobile_export_xcf_header(xcf, source_width, source_height, 600));
    EXPORT_CHECK(mobile_export_conf_matches(expected));
    /* DEFAULT must still honor the existing original-module setting, not
     * the last export's temporary 32-bit precision. */
    settings.pixel_format = DT_MOBILE_EXPORT_PIXEL_DEFAULT;
    EXPORT_OK(dt_mobile_export_with_settings(session, xcf, "xcf", &settings));
    EXPORT_CHECK(mobile_export_xcf_header(xcf, source_width, source_height, 200));
    EXPORT_CHECK(mobile_export_conf_matches(expected));
#ifdef __linux__
    /* Force a real buffered writer/close failure. Only this serialized export
     * runs under the temporary file-size limit; the original destination must
     * survive and the per-export configuration must still be restored. */
    EXPORT_CHECK(g_file_set_contents(xcf, "preserve-existing-destination", -1, NULL));
    struct rlimit old_limit;
    struct sigaction old_signal, ignored_signal = {0};
    EXPORT_CHECK(getrlimit(RLIMIT_FSIZE, &old_limit) == 0);
    ignored_signal.sa_handler = SIG_IGN;
    sigemptyset(&ignored_signal.sa_mask);
    EXPORT_CHECK(sigaction(SIGXFSZ, &ignored_signal, &old_signal) == 0);
    struct rlimit short_limit = old_limit;
    short_limit.rlim_cur = 1;
    const int limit_set = setrlimit(RLIMIT_FSIZE, &short_limit);
    dt_mobile_status failed_write = DT_MOBILE_OK;
    if(limit_set == 0)
    {
      settings.pixel_format = DT_MOBILE_EXPORT_FLOAT32;
      failed_write = dt_mobile_export_with_settings(session, xcf, "xcf", &settings);
    }
    const int limit_restored = setrlimit(RLIMIT_FSIZE, &old_limit);
    const int signal_restored = sigaction(SIGXFSZ, &old_signal, NULL);
    EXPORT_CHECK(limit_set == 0 && limit_restored == 0 && signal_restored == 0);
    EXPORT_CHECK(failed_write == DT_MOBILE_ERROR_PROCESSING);
    gchar *preserved = NULL;
    gsize preserved_size = 0;
    const int read_preserved = g_file_get_contents(xcf, &preserved, &preserved_size, NULL);
    const int destination_unchanged = read_preserved
      && preserved_size == strlen("preserve-existing-destination")
      && !memcmp(preserved, "preserve-existing-destination", preserved_size);
    g_free(preserved);
    EXPORT_CHECK(destination_unchanged);
    EXPORT_CHECK(mobile_export_conf_matches(expected));
#endif
    settings.pixel_format = DT_MOBILE_EXPORT_UINT8;
    EXPORT_CHECK(dt_mobile_export_with_settings(session, missing, "xcf", &settings) == DT_MOBILE_ERROR_IO);
    EXPORT_CHECK(mobile_export_conf_matches(expected));
    EXPORT_CHECK(dt_mobile_export_with_settings(session, image_path, "xcf", &settings)
                 == DT_MOBILE_ERROR_INVALID_ARGUMENT);
    settings.png_compression = 1;
    EXPORT_CHECK(dt_mobile_export_with_settings(session, xcf, "xcf", &settings) == DT_MOBILE_ERROR_UNSUPPORTED);
    settings.png_compression = -1;
    settings.tiff_compression = 1;
    EXPORT_CHECK(dt_mobile_export_with_settings(session, xcf, "xcf", &settings) == DT_MOBILE_ERROR_UNSUPPORTED);
    settings.tiff_compression = -1;
    settings.jpeg_subsample = 1;
    EXPORT_CHECK(dt_mobile_export_with_settings(session, xcf, "xcf", &settings) == DT_MOBILE_ERROR_UNSUPPORTED);
    settings.jpeg_subsample = -1;
    settings.dpi = 240;
    EXPORT_CHECK(dt_mobile_export_with_settings(session, xcf, "xcf", &settings) == DT_MOBILE_ERROR_UNSUPPORTED);
    EXPORT_CHECK(mobile_export_conf_matches(expected));
  }
  if(strstr(formats, "\"id\":\"jpeg\""))
  {
    settings = (dt_mobile_export_settings)DT_MOBILE_EXPORT_SETTINGS_INIT;
    settings.quality = 87;
    settings.jpeg_subsample = 4;
    settings.dpi = 240;
    settings.metadata_policy = DT_MOBILE_EXPORT_METADATA_SOURCE_NONE;
    EXPORT_OK(dt_mobile_export_with_settings(session, jpeg, "jpeg", &settings));
    EXPORT_CHECK(mobile_export_jpeg_header(jpeg, 0x22, 240));
    EXPORT_CHECK(mobile_export_conf_matches(expected));
    EXPORT_OK(dt_mobile_export(session, jpeg, "jpeg", 0));
    EXPORT_CHECK(g_file_test(jpeg, G_FILE_TEST_IS_REGULAR));
    EXPORT_CHECK(mobile_export_conf_matches(expected));
    dt_pthread_mutex_lock(&darktable.conf->mutex);
    g_hash_table_replace(darktable.conf->table, g_strdup(mobile_export_conf_keys[0]), g_strdup("95"));
    g_hash_table_replace(darktable.conf->override_entries, g_strdup(mobile_export_conf_keys[0]), g_strdup("95"));
    dt_pthread_mutex_unlock(&darktable.conf->mutex);
    override_changed = 1;
    settings.quality = 90;
    EXPORT_CHECK(dt_mobile_export_with_settings(session, jpeg, "jpeg", &settings) == DT_MOBILE_ERROR_UNSUPPORTED);
    dt_pthread_mutex_lock(&darktable.conf->mutex);
    const int restored = !g_strcmp0(g_hash_table_lookup(darktable.conf->table, mobile_export_conf_keys[0]), "95")
      && !g_strcmp0(g_hash_table_lookup(darktable.conf->override_entries, mobile_export_conf_keys[0]), "95");
    dt_pthread_mutex_unlock(&darktable.conf->mutex);
    EXPORT_CHECK(restored);
  }
  EXPORT_CHECK(g_file_get_contents(image_path, &source_after, &after_size, NULL));
  EXPORT_CHECK(source_size == after_size && !memcmp(source_before, source_after, source_size));
  EXPORT_OK(dt_mobile_modules(session, &modules_after));
  EXPORT_CHECK(!strcmp(modules_before, modules_after));
  success = 1;
finished:
  if(override_changed)
  {
    dt_pthread_mutex_lock(&darktable.conf->mutex);
    if(previous_override)
      g_hash_table_replace(darktable.conf->override_entries, g_strdup(mobile_export_conf_keys[0]), g_strdup(previous_override));
    else g_hash_table_remove(darktable.conf->override_entries, mobile_export_conf_keys[0]);
    dt_pthread_mutex_unlock(&darktable.conf->mutex);
  }
  if(snapshots_ready) mobile_export_conf_restore(original);
  mobile_export_conf_free(original);
  mobile_export_conf_free(expected);
  dt_mobile_free_buffer(formats);
  dt_mobile_free_buffer(modules_before); dt_mobile_free_buffer(modules_after);
  g_free(source_before); g_free(source_after); g_free(previous_override);
  g_free(srgb_profile_hash); g_free(adobe_profile_hash);
  g_unlink(png); g_unlink(tiff); g_unlink(jpeg); g_unlink(xcf); g_unlink(alias);
  g_free(png); g_free(tiff); g_free(jpeg); g_free(xcf); g_free(missing); g_free(alias);
#undef EXPORT_CHECK
#undef EXPORT_OK
  return success;
}

typedef int (*mobile_export_writer_fn)(dt_imageio_module_data_t *, const char *, const void *,
  dt_colorspaces_color_profile_type_t, const char *, void *, int, dt_imgid_t, int, int,
  struct dt_dev_pixelpipe_t *, const gboolean);

typedef struct mobile_export_cancel_probe {
  dt_mobile_session *session;
  mobile_export_writer_fn original;
  gchar *temporary_path;
  int calls, encoded, overrides_active;
} mobile_export_cancel_probe;

/* This fixture runs one serialized export in the test process. The callback
 * retains the real encoder and changes only when cancellation is requested. */
static mobile_export_cancel_probe mobile_export_cancel_state;

static int mobile_export_cancel_after_write(dt_imageio_module_data_t *params,
  const char *filename, const void *input, dt_colorspaces_color_profile_type_t profile,
  const char *profile_filename, void *exif, int exif_length, dt_imgid_t image_id,
  int number, int total, struct dt_dev_pixelpipe_t *pipe, const gboolean export_masks)
{
  mobile_export_cancel_probe *probe = &mobile_export_cancel_state;
  probe->calls++;
  probe->overrides_active = dt_conf_get_int("plugins/imageio/format/png/bpp") == 16
    && dt_conf_get_int("plugins/imageio/format/png/compression") == 9;
  g_free(probe->temporary_path);
  probe->temporary_path = g_strdup(filename);
  const int result = probe->original(params, filename, input, profile, profile_filename,
                                    exif, exif_length, image_id, number, total, pipe, export_masks);
  probe->encoded = !result && params->width > 0 && params->height > 0
    && mobile_export_png_header(filename, (uint32_t)params->width, (uint32_t)params->height, 16);
  /* write_image runs outside the cancellation-token mutex. The public cancel
   * path takes neither the operation nor engine-work mutex, so requesting it
   * here is synchronous and cannot deadlock the enclosing export. */
  dt_mobile_cancel(probe->session);
  return result;
}

static int export_midstream_cancel_checks(dt_mobile_session *session, const char *output_path)
{
  int success = 0, saved_configuration = 0, installed_callback = 0;
  dt_imageio_module_format_t *format = dt_imageio_get_format_by_name("png");
  mobile_export_writer_fn original = format ? format->write_image : NULL;
  gchar *path = g_strconcat(output_path, ".midstream-settings.png", NULL);
  gchar *temporary_prefix = g_strconcat(path, ".dt-mobile-", NULL);
  mobile_export_conf_snapshot original_conf[MOBILE_EXPORT_CONF_COUNT] = {{0}};
  mobile_export_conf_snapshot expected_conf[MOBILE_EXPORT_CONF_COUNT] = {{0}};
  dt_mobile_export_settings settings = DT_MOBILE_EXPORT_SETTINGS_INIT;
#define MIDSTREAM_CHECK(condition) do { if(!(condition)) { \
  fprintf(stderr, "midstream export cancellation:%d: check failed: %s\n", __LINE__, #condition); \
  goto finished; } } while(0)
  MIDSTREAM_CHECK(original != NULL);
  MIDSTREAM_CHECK(!g_file_test(path, G_FILE_TEST_EXISTS));
  mobile_export_conf_capture(original_conf);
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  g_hash_table_replace(darktable.conf->table, g_strdup("plugins/imageio/format/png/bpp"), g_strdup("08"));
  g_hash_table_remove(darktable.conf->table, "plugins/imageio/format/png/compression");
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  saved_configuration = 1;
  mobile_export_conf_capture(expected_conf);
  settings.pixel_format = DT_MOBILE_EXPORT_UINT16;
  settings.png_compression = 9;
  settings.max_width = settings.max_height = 32;
  settings.metadata_policy = DT_MOBILE_EXPORT_METADATA_SOURCE_NONE;
  mobile_export_cancel_state = (mobile_export_cancel_probe){ .session = session, .original = original };
  format->write_image = mobile_export_cancel_after_write;
  installed_callback = 1;
  const dt_mobile_status result = dt_mobile_export_with_settings(session, path, "png", &settings);
  format->write_image = original;
  installed_callback = 0;
  MIDSTREAM_CHECK(result == DT_MOBILE_ERROR_CANCELLED);
  MIDSTREAM_CHECK(mobile_export_cancel_state.calls == 1 && mobile_export_cancel_state.encoded
                  && mobile_export_cancel_state.overrides_active);
  MIDSTREAM_CHECK(mobile_export_cancel_state.temporary_path
                  && g_str_has_prefix(mobile_export_cancel_state.temporary_path, temporary_prefix));
  MIDSTREAM_CHECK(!g_file_test(path, G_FILE_TEST_EXISTS));
  MIDSTREAM_CHECK(!g_file_test(mobile_export_cancel_state.temporary_path, G_FILE_TEST_EXISTS));
  MIDSTREAM_CHECK(mobile_export_conf_matches(expected_conf));
  success = 1;
finished:
  if(installed_callback) format->write_image = original;
  if(saved_configuration) mobile_export_conf_restore(original_conf);
  mobile_export_conf_free(original_conf);
  mobile_export_conf_free(expected_conf);
  if(mobile_export_cancel_state.temporary_path) g_unlink(mobile_export_cancel_state.temporary_path);
  g_free(mobile_export_cancel_state.temporary_path);
  mobile_export_cancel_state = (mobile_export_cancel_probe){0};
  g_unlink(path);
  g_free(path);
  g_free(temporary_prefix);
#undef MIDSTREAM_CHECK
  return success;
}

static int export_cancelled_settings_checks(dt_mobile_session *session, const char *output_path)
{
  dt_mobile_export_settings settings = DT_MOBILE_EXPORT_SETTINGS_INIT;
  settings.pixel_format = DT_MOBILE_EXPORT_UINT16;
  settings.png_compression = 9;
  gchar *path = g_strconcat(output_path, ".cancelled-settings.png", NULL);
  mobile_export_conf_snapshot before[MOBILE_EXPORT_CONF_COUNT] = {{0}};
  mobile_export_conf_capture(before);
  const int valid = dt_mobile_export_with_settings(session, path, "png", &settings) == DT_MOBILE_ERROR_CANCELLED
    && !g_file_test(path, G_FILE_TEST_EXISTS) && mobile_export_conf_matches(before);
  mobile_export_conf_free(before);
  g_unlink(path);
  g_free(path);
  return valid;
}
#endif
