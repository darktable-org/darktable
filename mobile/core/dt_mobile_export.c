/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dt_mobile_export.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/image.h"
#include "control/conf.h"
#include "imageio/imageio_module.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct export_profile {
  int id;
  dt_colorspaces_color_profile_type_t type;
  const char *label;
} export_profile;
static const export_profile profiles[] = {
  { DT_MOBILE_EXPORT_PROFILE_IMAGE, DT_COLORSPACE_NONE, "image settings" },
  { DT_MOBILE_EXPORT_PROFILE_SRGB, DT_COLORSPACE_SRGB, "sRGB" },
  { DT_MOBILE_EXPORT_PROFILE_ADOBE_RGB, DT_COLORSPACE_ADOBERGB, "Adobe RGB" },
  { DT_MOBILE_EXPORT_PROFILE_LINEAR_REC709, DT_COLORSPACE_LIN_REC709, "linear Rec709" },
  { DT_MOBILE_EXPORT_PROFILE_LINEAR_REC2020, DT_COLORSPACE_LIN_REC2020, "linear Rec2020" },
  { DT_MOBILE_EXPORT_PROFILE_REC709, DT_COLORSPACE_REC709, "Rec709" },
  { DT_MOBILE_EXPORT_PROFILE_PROPHOTO_RGB, DT_COLORSPACE_PROPHOTO_RGB, "ProPhoto RGB" },
  { DT_MOBILE_EXPORT_PROFILE_DISPLAY_P3, DT_COLORSPACE_DISPLAY_P3, "Display P3" },
};

static dt_mobile_status export_fail(char *error, size_t size, dt_mobile_status status,
                                    const char *message)
{
  if(error && size) snprintf(error, size, "%s", message);
  return status;
}

static const char *canonical_format(const char *format)
{
  if(format && (!g_ascii_strcasecmp(format, "jpg") || !g_ascii_strcasecmp(format, "jpeg"))) return "jpeg";
  if(format && (!g_ascii_strcasecmp(format, "tif") || !g_ascii_strcasecmp(format, "tiff"))) return "tiff";
  if(format && !g_ascii_strcasecmp(format, "png")) return "png";
  if(format && !g_ascii_strcasecmp(format, "xcf")) return "xcf";
  return format;
}

static const export_profile *find_profile(int id)
{
  for(size_t k = 0; k < sizeof(profiles) / sizeof(profiles[0]); k++)
    if(profiles[k].id == id) return &profiles[k];
  return NULL;
}

static gboolean profile_available(const export_profile *profile)
{
  if(profile->type == DT_COLORSPACE_NONE) return TRUE;
  if(!darktable.color_profiles) return FALSE;
  const dt_colorspaces_color_profile_t *native =
    dt_colorspaces_get_profile(profile->type, "", DT_PROFILE_DIRECTION_OUT);
  return native && native->out_pos >= 0 && native->profile
    && cmsGetColorSpace(native->profile) == cmsSigRgbData;
}

dt_mobile_status dt_mobile_export_validate(const char *format_name,
                                           const dt_mobile_export_settings *s,
                                           char *error, size_t size)
{
  if(!format_name || !format_name[0] || !s || s->struct_size != sizeof(*s)
     || s->max_width > INT_MAX || s->max_height > INT_MAX
     || s->quality < 5 || s->quality > 100
     || s->pixel_format < DT_MOBILE_EXPORT_PIXEL_DEFAULT || s->pixel_format > DT_MOBILE_EXPORT_FLOAT32
     || s->png_compression < -1 || s->png_compression > 9
     || s->tiff_compression < -1 || s->tiff_compression > 2
     || (s->tiff_compression_level != -1 && (s->tiff_compression_level < 1 || s->tiff_compression_level > 9))
     || s->jpeg_subsample < -1 || s->jpeg_subsample > 4
     || (s->dpi && (s->dpi < 72 || s->dpi > 9600))
     || !find_profile(s->output_profile)
     || s->rendering_intent < DT_MOBILE_EXPORT_INTENT_IMAGE || s->rendering_intent > DT_MOBILE_EXPORT_INTENT_ABSOLUTE
     || s->metadata_policy < DT_MOBILE_EXPORT_METADATA_DEFAULT || s->metadata_policy > DT_MOBILE_EXPORT_METADATA_SOURCE_NONE)
    return export_fail(error, size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "invalid export settings");
  const char *name = canonical_format(format_name);
  const gboolean png = !strcmp(name, "png"), tiff = !strcmp(name, "tiff"), jpeg = !strcmp(name, "jpeg"),
                 xcf = !strcmp(name, "xcf");
  if((s->png_compression != -1 && !png)
     || ((s->tiff_compression != -1 || s->tiff_compression_level != -1) && !tiff)
     || (s->jpeg_subsample != -1 && !jpeg) || (s->dpi && !jpeg && !tiff)
     || (s->pixel_format != DT_MOBILE_EXPORT_PIXEL_DEFAULT
         && !(tiff || xcf || (png && s->pixel_format != DT_MOBILE_EXPORT_FLOAT32)
              || (jpeg && s->pixel_format == DT_MOBILE_EXPORT_UINT8))))
    return export_fail(error, size, DT_MOBILE_ERROR_UNSUPPORTED, "these settings are not supported by the selected format");
  if(!profile_available(find_profile(s->output_profile)))
    return export_fail(error, size, DT_MOBILE_ERROR_UNSUPPORTED, "the requested output profile is unavailable");
  return DT_MOBILE_OK;
}

typedef struct config_override { const char *key; gchar *previous; gboolean existed; } config_override;
static gboolean override_config(config_override *saved, const char *key, int value, gboolean boolean)
{
  saved->key = key;
  /* key_exists() includes generated defaults; preserve the actual table entry. */
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const char *previous = g_hash_table_lookup(darktable.conf->table, key);
  saved->existed = previous != NULL;
  saved->previous = g_strdup(previous);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  if(boolean) dt_conf_set_bool(key, value);
  else dt_conf_set_int(key, value);
  /* Command-line overrides are authoritative. Do not silently encode a
   * different setting when an override prevents this per-export choice. */
  return (boolean ? dt_conf_get_bool(key) : dt_conf_get_int(key)) == value;
}

static void restore_config(config_override *saved, size_t count)
{
  while(count)
  {
    config_override *entry = &saved[--count];
    /* The public setter can skip a value equal to a command-line override,
     * even when the underlying table currently contains our temporary value. */
    dt_pthread_mutex_lock(&darktable.conf->mutex);
    if(entry->existed)
    {
      g_hash_table_replace(darktable.conf->table, g_strdup(entry->key), entry->previous);
      entry->previous = NULL;
    }
    else g_hash_table_remove(darktable.conf->table, entry->key);
    dt_pthread_mutex_unlock(&darktable.conf->mutex);
    g_free(entry->previous);
  }
}

static gboolean is_input_path(dt_imgid_t image_id, const char *path)
{
  char source[PATH_MAX] = {0};
  gboolean from_cache = TRUE;
  dt_image_full_path(image_id, source, sizeof(source), &from_cache);
  gboolean same = !strcmp(source, path);
  struct stat input_info, output_info;
  if(!same && !stat(source, &input_info) && !stat(path, &output_info))
    same = input_info.st_dev == output_info.st_dev && input_info.st_ino == output_info.st_ino;
  return same;
}

dt_mobile_status dt_mobile_export_write(dt_imgid_t image_id, const char *path,
                                        const char *format_name,
                                        const dt_mobile_export_settings *s,
                                        dt_imageio_preview_cancel_t *cancel,
                                        const atomic_bool *cancelled,
                                        char *error, size_t size)
{
  if(!path || !path[0] || !dt_is_valid_imgid(image_id))
    return export_fail(error, size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "an image and export path are required");
  dt_mobile_status status = dt_mobile_export_validate(format_name, s, error, size);
  if(status != DT_MOBILE_OK) return status;
  if(cancelled && atomic_load(cancelled))
    return export_fail(error, size, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
  if(!darktable.imageio || !darktable.conf)
    return export_fail(error, size, DT_MOBILE_ERROR_ENGINE, "the processing engine is not initialized");
  const char *name = canonical_format(format_name);
  dt_imageio_module_format_t *format = strcmp(name, "copy") ? dt_imageio_get_format_by_name(name) : NULL;
  if(!format || !strcmp(format->mime(NULL), "x-copy"))
    return export_fail(error, size, DT_MOBILE_ERROR_UNSUPPORTED, "export format is not installed");
  if(is_input_path(image_id, path))
    return export_fail(error, size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "export cannot replace its input");

  config_override saved[10] = {{0}};
  size_t saved_count = 0;
  dt_imageio_module_data_t *params = NULL;
  gchar *temporary = NULL;
#define OVERRIDE(key, value, boolean) do { \
    if(!override_config(&saved[saved_count++], key, value, boolean)) { \
      status = export_fail(error, size, DT_MOBILE_ERROR_UNSUPPORTED, "export configuration is overridden"); \
      goto cleanup; \
    } \
  } while(0)
  if(!strcmp(name, "jpeg"))
  {
    OVERRIDE("plugins/imageio/format/jpeg/quality", s->quality, FALSE);
    if(s->jpeg_subsample != -1) OVERRIDE("plugins/imageio/format/jpeg/subsample", s->jpeg_subsample, FALSE);
  }
  if(s->pixel_format != DT_MOBILE_EXPORT_PIXEL_DEFAULT && strcmp(name, "jpeg"))
  {
    const int bits = s->pixel_format == DT_MOBILE_EXPORT_UINT8 ? 8 : s->pixel_format == DT_MOBILE_EXPORT_UINT16 ? 16 : 32;
    const char *key = !strcmp(name, "png") ? "plugins/imageio/format/png/bpp"
      : !strcmp(name, "xcf") ? "plugins/imageio/format/xcf/bpp" : "plugins/imageio/format/tiff/bpp";
    OVERRIDE(key, bits, FALSE);
    if(!strcmp(name, "tiff")) OVERRIDE("plugins/imageio/format/tiff/pixelformat", FALSE, TRUE);
  }
  if(s->png_compression != -1) OVERRIDE("plugins/imageio/format/png/compression", s->png_compression, FALSE);
  if(s->tiff_compression != -1) OVERRIDE("plugins/imageio/format/tiff/compress", s->tiff_compression, FALSE);
  if(s->tiff_compression_level != -1) OVERRIDE("plugins/imageio/format/tiff/compresslevel", s->tiff_compression_level, FALSE);
  if(s->dpi) OVERRIDE("metadata/resolution", s->dpi, FALSE);
#undef OVERRIDE
  params = format->get_params(format);
  if(!params)
  {
    status = export_fail(error, size, DT_MOBILE_ERROR_OUT_OF_MEMORY, "unable to allocate export parameters");
    goto cleanup;
  }
  if(s->pixel_format != DT_MOBILE_EXPORT_PIXEL_DEFAULT)
  {
    const int expected = s->pixel_format == DT_MOBILE_EXPORT_UINT8 ? 8 : s->pixel_format == DT_MOBILE_EXPORT_UINT16 ? 16 : 32;
    if(format->bpp(params) != expected)
    {
      status = export_fail(error, size, DT_MOBILE_ERROR_UNSUPPORTED, "the requested output precision is unavailable");
      goto cleanup;
    }
  }
  params->max_width = (int)s->max_width;
  params->max_height = (int)s->max_height;
  params->style[0] = '\0';
  params->style_append = TRUE;
  temporary = g_strdup_printf("%s.dt-mobile-XXXXXX", path);
  const int descriptor = g_mkstemp(temporary);
  if(descriptor < 0)
  {
    status = export_fail(error, size, DT_MOBILE_ERROR_IO, "unable to create export destination");
    goto cleanup;
  }
  close(descriptor);
  const gboolean source_metadata = s->metadata_policy == DT_MOBILE_EXPORT_METADATA_DEFAULT;
  dt_export_metadata_t metadata = { dt_lib_export_metadata_default_flags(), NULL };
  const export_profile *profile = find_profile(s->output_profile);
  const dt_iop_color_intent_t intent = s->rendering_intent == DT_MOBILE_EXPORT_INTENT_IMAGE
    ? DT_INTENT_LAST : (dt_iop_color_intent_t)(s->rendering_intent - 1);
  if(dt_imageio_export_to_file_with_options(image_id, temporary, format, params,
       !source_metadata, source_metadata, profile->type, NULL, intent, &metadata, cancel))
    status = export_fail(error, size, DT_MOBILE_ERROR_PROCESSING, "darktable export pixelpipe failed");
  if(cancelled && atomic_load(cancelled))
    status = export_fail(error, size, DT_MOBILE_ERROR_CANCELLED, "operation cancelled");
  if(status == DT_MOBILE_OK && rename(temporary, path))
    status = export_fail(error, size, DT_MOBILE_ERROR_IO, "unable to commit exported image");
cleanup:
  if(temporary && status != DT_MOBILE_OK) remove(temporary);
  g_free(temporary);
  if(params) format->free_params(format, params);
  /* JPEG subsampling and DPI are read by writers, so restore only after the
   * complete pixelpipe/encoder/metadata operation has returned. */
  restore_config(saved, saved_count);
  return status;
}

void dt_mobile_export_append_options(GString *json, const char *format_name)
{
  const char *name = canonical_format(format_name);
  const gboolean png = name && !strcmp(name, "png"), tiff = name && !strcmp(name, "tiff"),
                 jpeg = name && !strcmp(name, "jpeg"), xcf = name && !strcmp(name, "xcf");
  g_string_append_printf(json, "{\"maxWidth\":{\"default\":0,\"min\":0,\"max\":%d},"
    "\"maxHeight\":{\"default\":0,\"min\":0,\"max\":%d},\"upscale\":false,\"pixelFormats\":["
    "{\"value\":0,\"label\":\"format settings\"}", INT_MAX, INT_MAX);
  if(png || tiff || jpeg || xcf) g_string_append(json, ",{\"value\":1,\"label\":\"8 bit unsigned\"}");
  if(png || tiff || xcf) g_string_append(json, ",{\"value\":2,\"label\":\"16 bit unsigned\"}");
  if(tiff || xcf) g_string_append(json, ",{\"value\":3,\"label\":\"32 bit float\"}");
  g_string_append(json, "],\"pixelFormatDefault\":0");
  if(jpeg) g_string_append_printf(json, ",\"quality\":{\"default\":95,\"min\":5,\"max\":100},"
    "\"jpegSubsample\":{\"default\":-1,\"inherited\":%d,\"values\":[-1,0,1,2,3,4]}",
    dt_conf_get_int("plugins/imageio/format/jpeg/subsample"));
  if(png) g_string_append_printf(json, ",\"pngCompression\":{\"default\":-1,\"inherited\":%d,\"min\":0,\"max\":9}", dt_conf_get_int("plugins/imageio/format/png/compression"));
  if(tiff) g_string_append_printf(json, ",\"tiffCompression\":{\"default\":-1,\"inherited\":%d,\"values\":[-1,0,1,2]},"
    "\"tiffCompressionLevel\":{\"default\":-1,\"inherited\":%d,\"min\":1,\"max\":9}",
    dt_conf_get_int("plugins/imageio/format/tiff/compress"), dt_conf_get_int("plugins/imageio/format/tiff/compresslevel"));
  if(jpeg || tiff) g_string_append_printf(json, ",\"dpi\":{\"default\":0,\"inherited\":%d,\"min\":72,\"max\":9600}", dt_conf_get_int("metadata/resolution"));
  g_string_append(json, ",\"profiles\":[");
  gboolean first = TRUE;
  for(size_t k = 0; k < sizeof(profiles) / sizeof(profiles[0]); k++)
    if(profile_available(&profiles[k]))
    {
      if(!first) g_string_append_c(json, ',');
      first = FALSE;
      /* All labels are fixed ASCII strings, not translated/external input. */
      g_string_append_printf(json, "{\"value\":%d,\"label\":\"%s\"}", profiles[k].id, profiles[k].label);
    }
  g_string_append(json, "],\"profileDefault\":0,\"intents\":[0,1,2,3,4],\"intentDefault\":0,"
    "\"metadataPolicies\":[{\"value\":0,\"label\":\"desktop default\"},"
    "{\"value\":1,\"label\":\"do not copy source metadata\"}],\"metadataDefault\":0}");
}
