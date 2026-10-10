/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#define dt_wayland_color_available _test_native_available
#define dt_conf_get_bool _test_conf_bool
#define dt_conf_get_int _test_conf_int
#define dt_conf_get_string_const _test_conf_string
#define dt_image_full_path _test_image_path
#define dt_image_altered _test_image_altered
#define dt_image_get_orientation _test_orientation
#define dt_image_cache_get _test_image_get
#define dt_image_cache_read_release _test_image_release
#define dt_imageio_export_with_flags _test_export
#define dt_cache_testget _test_cache_get
#define dt_cache_release_with_caller _test_cache_release
#include "common/mipmap_cache.c"
#include "common/display_transport.h"
#undef dt_wayland_color_available
#undef dt_conf_get_bool
#undef dt_conf_get_int
#undef dt_conf_get_string_const
#undef dt_image_full_path
#undef dt_image_altered
#undef dt_image_get_orientation
#undef dt_image_cache_get
#undef dt_image_cache_read_release
#undef dt_imageio_export_with_flags
#undef dt_cache_testget
#undef dt_cache_release_with_caller

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static gboolean _native;
static char *_source;
static int _exports, _releases;
static dt_image_t _image = { .width = 64, .height = 64 };
static dt_cache_entry_t _larger;

gboolean _test_native_available(void) { return _native; }
gboolean _test_conf_bool(const char *name) { return !strcmp(name, "cache_disk_backend"); }
int _test_conf_int(const char *name) { return 100; }
const char *_test_conf_string(const char *name) { return "always"; }
void _test_image_path(const dt_imgid_t imgid, char *path, const size_t size, gboolean *from_cache)
{
  g_strlcpy(path, _source, size);
}
gboolean _test_image_altered(const dt_imgid_t imgid) { return FALSE; }
dt_image_orientation_t _test_orientation(const dt_imgid_t imgid) { return ORIENTATION_NONE; }
dt_image_t *_test_image_get(const dt_imgid_t imgid, const char mode) { return &_image; }
void _test_image_release(const dt_image_t *image) { }
dt_cache_entry_t *_test_cache_get(dt_cache_t *cache, const uint32_t key, char mode)
{
  return _larger.data && key == _larger.key ? &_larger : NULL;
}
void _test_cache_release(dt_cache_t *cache, dt_cache_entry_t *entry, const char *file, const int line)
{
  assert_ptr_equal(entry, &_larger);
  _releases++;
}

gboolean _test_export(const dt_imgid_t imgid, const char *filename,
                       dt_imageio_module_format_t *format, dt_imageio_module_data_t *params,
                       const gboolean ignore_exif, const gboolean display_byteorder,
                       const gboolean high_quality, const gboolean upscale,
                       const gboolean is_scaling, const double scale_factor,
                       const gboolean thumbnail_export, const char *filter,
                       const gboolean copy_metadata, const gboolean export_masks,
                       dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename,
                       dt_iop_color_intent_t icc_intent, dt_imageio_module_storage_t *storage,
                       dt_imageio_module_data_t *storage_params, int num, const int total,
                       dt_export_metadata_t *metadata, const int history_end)
{
  _exports++;
  assert_true(thumbnail_export);
  assert_int_equal(icc_type, DT_COLORSPACE_NONE);
  // the full image loader can still read the ICC ignored by the thumbnail shortcut
  dt_imageio_jpeg_t jpg;
  assert_int_equal(dt_imageio_jpeg_read_header(_source, &jpg), 0);
  uint8_t *icc = NULL;
  const int size = dt_imageio_jpeg_read_profile(&jpg, &icc);
  assert_true(size > 0);
  cmsHPROFILE profile = cmsOpenProfileFromMem(icc, size);
  assert_non_null(profile);
  cmsCloseProfile(profile);
  free(icc);
  params->width = params->height = 64;
  uint8_t pixels[64 * 64 * 4];
  memset(pixels, 120, sizeof(pixels));
  return format->write_image(params, filename, pixels, DT_COLORSPACE_SRGB, "",
                             NULL, 0, imgid, num, total, NULL, FALSE);
}

static void _write_source(const char *path)
{
  uint8_t pixels[64 * 64 * 4];
  memset(pixels, 80, sizeof(pixels));
  assert_int_equal(dt_imageio_jpeg_write(path, pixels, 64, 64, 100, NULL, 0), 0);
  gchar *jpeg = NULL;
  gsize jpeg_size = 0;
  assert_true(g_file_get_contents(path, &jpeg, &jpeg_size, NULL));
  cmsHPROFILE profile = dt_display_transport_create_profile();
  cmsUInt32Number size = 0;
  assert_true(cmsSaveProfileToMem(profile, NULL, &size));
  assert_true(size < 65519);
  uint8_t *icc = g_malloc(size);
  assert_true(cmsSaveProfileToMem(profile, icc, &size));
  // the APP2 marker embeds a real non-sRGB ICC profile without an EXIF color-space tag
  const uint16_t length = size + 16;
  const uint8_t marker[] = { 0xff, 0xe2, length >> 8, length & 255 };
  const uint8_t label[] = { 'I','C','C','_','P','R','O','F','I','L','E',0,1,1 };
  GByteArray *file = g_byte_array_new();
  g_byte_array_append(file, (uint8_t *)jpeg, 2);
  g_byte_array_append(file, marker, sizeof(marker));
  g_byte_array_append(file, label, sizeof(label));
  g_byte_array_append(file, icc, size);
  g_byte_array_append(file, (uint8_t *)jpeg + 2, jpeg_size - 2);
  assert_true(g_file_set_contents(path, (char *)file->data, file->len, NULL));
  g_byte_array_unref(file);
  g_free(icc);
  g_free(jpeg);
  cmsCloseProfile(profile);
}

static void _test_source_and_disk_encoding(void **state)
{
  gchar *directory = g_dir_make_tmp("darktable-mipmap-color-XXXXXX", NULL);
  assert_non_null(directory);
  _source = g_build_filename(directory, "source.jpg", NULL);
  _write_source(_source);
  dt_mipmap_cache_t cache = { 0 };
  darktable.mipmap_cache = &cache;
  darktable.num_openmp_threads = 1;
  cache.max_width[0] = cache.max_height[0] = 64;
  cache.buffer_size[0] = sizeof(dt_mipmap_buffer_dsc_t) + 64 * 64 * 4;
  g_snprintf(cache.cachedir, sizeof(cache.cachedir), "%s/thumbs", directory);
  _larger = (dt_cache_entry_t){ .key = _get_key(1, DT_MIPMAP_1),
                              .data_size = cache.buffer_size[0] };
  _larger.data = dt_alloc_aligned(_larger.data_size);
  assert_non_null(_larger.data);
  dt_mipmap_buffer_dsc_t *large = _larger.data;
  *large = (dt_mipmap_buffer_dsc_t){ .width = 64, .height = 64, .iscale = 1,
                                   .size = _larger.data_size, .color_space = DT_COLORSPACE_DISPLAY };
  memset(large + 1, 60, 64 * 64 * 4);
  const gchar *mip_name = "thumbs.d/0/1.jpg";
  gchar *cached = g_build_filename(directory, mip_name, NULL);
  for(int native = 0; native < 2; native++)
  {
    _native = native;
    _exports = _releases = 0;
    dt_cache_entry_t entry = { .key = _get_key(1, DT_MIPMAP_0),
                              .data_size = cache.buffer_size[0] };
    entry.data = dt_alloc_aligned(entry.data_size);
    assert_non_null(entry.data);
    dt_mipmap_buffer_dsc_t *dsc = entry.data;
    *dsc = (dt_mipmap_buffer_dsc_t){ .width = 64, .height = 64, .iscale = 1,
                                   .size = entry.data_size, .color_space = DT_COLORSPACE_NONE };
    _init_8((uint8_t *)(dsc + 1), &dsc->width, &dsc->height, &dsc->iscale,
             &dsc->color_space, 1, DT_MIPMAP_0);
    assert_int_equal(_exports, native);
    assert_int_equal(_releases, native);
    const dt_colorspaces_color_profile_type_t expected = native ? DT_COLORSPACE_SRGB : DT_COLORSPACE_DISPLAY;
    assert_int_equal(dsc->color_space, expected);
    _mipmap_cache_deallocate_dynamic(&cache, &entry);
    assert_true(g_file_test(cached, G_FILE_TEST_EXISTS));
    entry.data = NULL;

    // both portable and legacy disk entries load on X11, preserving their identity
    _native = FALSE;
    _mipmap_cache_allocate_dynamic(&cache, &entry);
    dsc = entry.data;
    assert_false(dsc->flags & DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE);
    assert_int_equal(dsc->color_space, expected);
    dt_free_align(entry.data);
    entry.data = NULL;

    // native mode reuses tagged entries but invalidates legacy device RGB
    _native = TRUE;
    _mipmap_cache_allocate_dynamic(&cache, &entry);
    dsc = entry.data;
    assert_int_equal(!!(dsc->flags & DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE), !native);
    if(native) assert_int_equal(dsc->color_space, DT_COLORSPACE_SRGB);
    dt_free_align(entry.data);
    g_unlink(cached);
  }
  dt_free_align(_larger.data);
  _larger.data = NULL;
  _native = FALSE;
  darktable.mipmap_cache = NULL;
  g_unlink(_source);
  g_free(_source);
  _source = NULL;
  g_free(cached);
  gchar *subdir = g_build_filename(directory, "thumbs.d", "0", NULL);
  assert_int_equal(g_rmdir(subdir), 0);
  g_free(subdir);
  subdir = g_build_filename(directory, "thumbs.d", NULL);
  assert_int_equal(g_rmdir(subdir), 0);
  g_free(subdir);
  assert_int_equal(g_rmdir(directory), 0);
  g_free(directory);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = { cmocka_unit_test(_test_source_and_disk_encoding) };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
