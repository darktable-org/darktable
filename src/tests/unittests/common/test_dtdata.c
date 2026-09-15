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

// the .dtdata sidecar: zip container, PNG entries, checksum naming, merge.
// works on explicit file paths so no database or image is needed

#include "common/dtdata.h"
#include "common/darktable.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

typedef struct _fixture_t
{
  gchar *dir;
  gchar *path;
} _fixture_t;

static int _setup(void **state)
{
  _fixture_t *f = g_new0(_fixture_t, 1);
  GError *err = NULL;
  f->dir = g_dir_make_tmp("dt-dtdata-XXXXXX", &err);
  if(!f->dir) return 1;
  f->path = g_build_filename(f->dir, "IMG_0001.RAF.dtdata", NULL);
  *state = f;
  return 0;
}

static int _teardown(void **state)
{
  _fixture_t *f = *state;
  GDir *d = g_dir_open(f->dir, 0, NULL);
  if(d)
  {
    const gchar *name;
    while((name = g_dir_read_name(d)))
    {
      gchar *p = g_build_filename(f->dir, name, NULL);
      g_unlink(p);
      g_free(p);
    }
    g_dir_close(d);
  }
  g_rmdir(f->dir);
  g_free(f->path);
  g_free(f->dir);
  g_free(f);
  return 0;
}

// a gradient with a hard edge, so bit depth and orientation both show
static float *_make_mask(const int w, const int h)
{
  float *m = dt_alloc_align_float((size_t)w * h);
  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
      m[(size_t)y * w + x] = (x < w / 2) ? (float)y / (h - 1) : 1.0f;
  return m;
}

static float _max_abs_diff(const float *a, const float *b, const size_t n)
{
  float d = 0.0f;
  for(size_t k = 0; k < n; k++) d = fmaxf(d, fabsf(a[k] - b[k]));
  return d;
}

static void test_roundtrip_8bit(void **state)
{
  _fixture_t *f = *state;
  const int w = 37, h = 23;
  float *mask = _make_mask(w, h);
  dt_dtdata_ref_t ref;

  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_MASK,
                                        DT_DTDATA_ORIGIN_AUTHORITATIVE, "test",
                                        mask, w, h, 8, &ref));
  assert_true(g_file_test(f->path, G_FILE_TEST_EXISTS));
  assert_true(g_str_has_prefix(ref.entry, "mask-"));
  assert_true(g_str_has_suffix(ref.entry, ".png"));
  assert_int_equal(strlen(ref.entry), strlen("mask-") + 40 + strlen(".png"));
  assert_int_equal(ref.width, w);
  assert_int_equal(ref.height, h);
  assert_int_equal(ref.bpc, 8);
  assert_int_equal(ref.origin, DT_DTDATA_ORIGIN_AUTHORITATIVE);
  assert_string_equal(ref.producer, "test");
  assert_int_equal(dt_dtdata_entry_kind(ref.entry), DT_DTDATA_KIND_MASK);

  int rw = 0, rh = 0;
  float *back = dt_dtdata_file_read_gray(f->path, &ref, &rw, &rh);
  assert_non_null(back);
  assert_int_equal(rw, w);
  assert_int_equal(rh, h);
  // 8 bit quantization: half a step
  assert_true(_max_abs_diff(mask, back, (size_t)w * h) <= 0.5f / 255.0f + 1e-6f);

  dt_free_align(back);
  dt_free_align(mask);
}

static void test_roundtrip_16bit(void **state)
{
  _fixture_t *f = *state;
  const int w = 64, h = 9;
  float *mask = _make_mask(w, h);
  dt_dtdata_ref_t ref;

  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_DEPTH,
                                        DT_DTDATA_ORIGIN_REGENERABLE, "depth-model-1",
                                        mask, w, h, 16, &ref));
  assert_true(g_str_has_prefix(ref.entry, "depth-"));
  assert_int_equal(ref.bpc, 16);

  int rw = 0, rh = 0;
  float *back = dt_dtdata_file_read_gray(f->path, &ref, &rw, &rh);
  assert_non_null(back);
  assert_int_equal(rw, w);
  assert_int_equal(rh, h);
  assert_true(_max_abs_diff(mask, back, (size_t)w * h) <= 0.5f / 65535.0f + 1e-7f);

  dt_free_align(back);
  dt_free_align(mask);
}

// a second write keeps the first entry, and identical content is not
// stored twice
static void test_multiple_entries_and_dedup(void **state)
{
  _fixture_t *f = *state;
  const int w = 16, h = 16;
  float *a = _make_mask(w, h);
  float *b = _make_mask(w, h);
  for(size_t k = 0; k < (size_t)w * h; k++) b[k] = 1.0f - b[k];
  dt_dtdata_ref_t ra, rb, ra2;

  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_MASK, 0, NULL, a, w, h, 8, &ra));
  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_BRUSH, 1, NULL, b, w, h, 8, &rb));
  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_MASK, 0, NULL, a, w, h, 8, &ra2));
  assert_string_equal(ra.entry, ra2.entry);
  assert_string_not_equal(ra.entry, rb.entry);

  GList *entries = dt_dtdata_file_list_entries(f->path);
  assert_int_equal(g_list_length(entries), 2);
  assert_non_null(g_list_find_custom(entries, ra.entry, (GCompareFunc)g_strcmp0));
  assert_non_null(g_list_find_custom(entries, rb.entry, (GCompareFunc)g_strcmp0));
  g_list_free_full(entries, g_free);

  int rw, rh;
  float *back = dt_dtdata_file_read_gray(f->path, &ra, &rw, &rh);
  assert_non_null(back);
  assert_true(_max_abs_diff(a, back, (size_t)w * h) <= 0.5f / 255.0f + 1e-6f);
  dt_free_align(back);
  back = dt_dtdata_file_read_gray(f->path, &rb, &rw, &rh);
  assert_non_null(back);
  assert_true(_max_abs_diff(b, back, (size_t)w * h) <= 0.5f / 255.0f + 1e-6f);
  dt_free_align(back);

  dt_free_align(a);
  dt_free_align(b);
}

// a reference whose name does not match the bytes is refused, as is a
// reference into a file that does not exist
static void test_missing_and_corrupt(void **state)
{
  _fixture_t *f = *state;
  const int w = 8, h = 8;
  float *a = _make_mask(w, h);
  dt_dtdata_ref_t ra = { .entry = "mask-0000000000000000000000000000000000000000.png" };
  int rw, rh;

  assert_null(dt_dtdata_file_read_gray(f->path, &ra, &rw, &rh));
  assert_null(dt_dtdata_file_list_entries(f->path));

  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_MASK, 0, NULL, a, w, h, 8, &ra));

  dt_dtdata_ref_t wrong = ra;
  wrong.entry[5] = (wrong.entry[5] == 'a') ? 'b' : 'a';
  assert_null(dt_dtdata_file_read_gray(f->path, &wrong, &rw, &rh));

  dt_dtdata_ref_t empty = { 0 };
  assert_null(dt_dtdata_file_read_gray(f->path, &empty, &rw, &rh));

  dt_free_align(a);
}

// merging copies what the destination lacks and leaves what it has
static void test_merge(void **state)
{
  _fixture_t *f = *state;
  gchar *other = g_build_filename(f->dir, "IMG_0001_01.RAF.dtdata", NULL);
  const int w = 12, h = 5;
  float *a = _make_mask(w, h);
  float *b = _make_mask(w, h);
  for(size_t k = 0; k < (size_t)w * h; k++) b[k] *= 0.5f;
  dt_dtdata_ref_t ra, rb;

  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_MASK, 0, NULL, a, w, h, 8, &ra));

  // into a file that does not exist yet: a plain copy
  assert_true(dt_dtdata_file_merge(f->path, other));
  GList *entries = dt_dtdata_file_list_entries(other);
  assert_int_equal(g_list_length(entries), 1);
  g_list_free_full(entries, g_free);

  // into a file with its own entry: union
  assert_true(dt_dtdata_file_write_gray(other, DT_DTDATA_KIND_MASK, 0, NULL, b, w, h, 8, &rb));
  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_BRUSH, 1, NULL, b, w, h, 8, &rb));
  assert_true(dt_dtdata_file_merge(f->path, other));
  entries = dt_dtdata_file_list_entries(other);
  assert_int_equal(g_list_length(entries), 3);
  g_list_free_full(entries, g_free);

  // a missing source is not an error
  gchar *none = g_build_filename(f->dir, "nope.dtdata", NULL);
  assert_true(dt_dtdata_file_merge(none, other));

  int rw, rh;
  float *back = dt_dtdata_file_read_gray(other, &ra, &rw, &rh);
  assert_non_null(back);
  dt_free_align(back);

  g_free(none);
  g_free(other);
  dt_free_align(a);
  dt_free_align(b);
}

// the sweep drops unreferenced entries of the kinds it is told about,
// keeps the rest, and removes the file once nothing is left
static void test_sweep(void **state)
{
  _fixture_t *f = *state;
  const int w = 10, h = 10;
  float *a = _make_mask(w, h);
  float *b = _make_mask(w, h);
  for(size_t k = 0; k < (size_t)w * h; k++) b[k] *= 0.25f;
  dt_dtdata_ref_t ra, rb, rc;

  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_MASK, 0, NULL, a, w, h, 8, &ra));
  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_MASK, 0, NULL, b, w, h, 8, &rb));
  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_BRUSH, 1, NULL, a, w, h, 8, &rc));

  // keep b; a is dropped, the brush is of a kind nobody claims and stays
  GList *keep = g_list_append(NULL, g_strdup(rb.entry));
  assert_true(dt_dtdata_file_sweep(f->path, keep, 1u << DT_DTDATA_KIND_MASK));
  GList *entries = dt_dtdata_file_list_entries(f->path);
  assert_int_equal(g_list_length(entries), 2);
  assert_null(g_list_find_custom(entries, ra.entry, (GCompareFunc)g_strcmp0));
  assert_non_null(g_list_find_custom(entries, rb.entry, (GCompareFunc)g_strcmp0));
  assert_non_null(g_list_find_custom(entries, rc.entry, (GCompareFunc)g_strcmp0));
  g_list_free_full(entries, g_free);
  g_list_free_full(keep, g_free);

  int rw, rh;
  float *back = dt_dtdata_file_read_gray(f->path, &rb, &rw, &rh);
  assert_non_null(back);
  assert_true(_max_abs_diff(b, back, (size_t)w * h) <= 0.5f / 255.0f + 1e-6f);
  dt_free_align(back);

  // nothing referenced, both kinds claimed: the file goes away
  const uint32_t all = (1u << DT_DTDATA_KIND_MASK) | (1u << DT_DTDATA_KIND_BRUSH);
  assert_true(dt_dtdata_file_sweep(f->path, NULL, all));
  assert_false(g_file_test(f->path, G_FILE_TEST_EXISTS));
  assert_true(dt_dtdata_file_sweep(f->path, NULL, all));

  dt_free_align(a);
  dt_free_align(b);
}

// names of files in the fixture directory ending in suffix
static int _count_files_with_suffix(const char *dir, const char *suffix)
{
  int n = 0;
  GDir *d = g_dir_open(dir, 0, NULL);
  const gchar *name;
  while(d && (name = g_dir_read_name(d)))
    if(g_str_has_suffix(name, suffix)) n++;
  if(d) g_dir_close(d);
  return n;
}

// noise does not deflate, so the entries make up most of the file and a
// cut at 60% lands inside their data rather than in the central directory
static float *_make_noise(const int w, const int h, uint32_t seed)
{
  float *m = dt_alloc_align_float((size_t)w * h);
  for(size_t k = 0; k < (size_t)w * h; k++)
  {
    seed = seed * 1664525u + 1013904223u;
    m[k] = (seed >> 8) / 16777216.0f;
  }
  return m;
}

// a zip cut short is left exactly as it is: no write over it, no sweep,
// and no temp file left behind
static void test_truncated(void **state)
{
  _fixture_t *f = *state;
  const int w = 64, h = 64;
  float *a = _make_noise(w, h, 1);
  float *b = _make_noise(w, h, 2);
  float *c = _make_noise(w, h, 3);
  dt_dtdata_ref_t ra, rb, rc;
  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_MASK, 0, NULL, a, w, h, 8, &ra));
  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_MASK, 0, NULL, b, w, h, 8, &rb));
  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_BRUSH, 0, NULL, c, w, h, 8, &rc));

  gchar *whole = NULL;
  gsize whole_len = 0;
  assert_true(g_file_get_contents(f->path, &whole, &whole_len, NULL));
  const gsize cut = whole_len * 6 / 10;
  assert_true(g_file_set_contents(f->path, whole, cut, NULL));

  dt_dtdata_ref_t rd;
  for(size_t k = 0; k < (size_t)w * h; k++) a[k] *= 0.75f;
  assert_false(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_MASK, 0, NULL, a, w, h, 8, &rd));
  gchar *after = NULL;
  gsize after_len = 0;
  assert_true(g_file_get_contents(f->path, &after, &after_len, NULL));
  assert_int_equal(after_len, cut);
  assert_memory_equal(after, whole, cut);
  g_free(after);

  const uint32_t all = (1u << DT_DTDATA_KIND_MASK) | (1u << DT_DTDATA_KIND_BRUSH);
  assert_false(dt_dtdata_file_sweep(f->path, NULL, all));
  assert_true(g_file_test(f->path, G_FILE_TEST_EXISTS));
  assert_true(g_file_get_contents(f->path, &after, &after_len, NULL));
  assert_int_equal(after_len, cut);
  assert_memory_equal(after, whole, cut);
  g_free(after);

  assert_null(dt_dtdata_file_list_entries(f->path));
  assert_int_equal(_count_files_with_suffix(f->dir, ".tmp"), 0);

  g_free(whole);
  dt_free_align(a);
  dt_free_align(b);
  dt_free_align(c);
}

// a keep list naming an entry the file lacks is harmless, and a sweep with
// nothing to drop does not rewrite the file
static void test_sweep_keep_absent(void **state)
{
  _fixture_t *f = *state;
  const int w = 10, h = 10;
  float *a = _make_mask(w, h);
  dt_dtdata_ref_t ra, rb;
  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_MASK, 0, NULL, a, w, h, 8, &ra));
  assert_true(dt_dtdata_file_write_gray(f->path, DT_DTDATA_KIND_BRUSH, 0, NULL, a, w, h, 8, &rb));

  gchar *before = NULL;
  gsize before_len = 0;
  assert_true(g_file_get_contents(f->path, &before, &before_len, NULL));

  GList *keep = g_list_append(NULL, g_strdup(ra.entry));
  keep = g_list_append(keep, g_strdup("mask-0000000000000000000000000000000000000000.png"));
  assert_true(dt_dtdata_file_sweep(f->path, keep, 1u << DT_DTDATA_KIND_MASK));
  g_list_free_full(keep, g_free);

  gchar *after = NULL;
  gsize after_len = 0;
  assert_true(g_file_get_contents(f->path, &after, &after_len, NULL));
  assert_int_equal(after_len, before_len);
  assert_memory_equal(after, before, before_len);
  assert_int_equal(_count_files_with_suffix(f->dir, ".tmp"), 0);

  g_free(before);
  g_free(after);
  dt_free_align(a);
}

static void test_path_for_image(void **state)
{
  char path[64];
  dt_dtdata_path_for_image("/photos/IMG_0001_01.RAF", path, sizeof(path));
  assert_string_equal(path, "/photos/IMG_0001_01.RAF.dtdata");
  assert_int_equal(dt_dtdata_entry_kind("brush-abc.png"), DT_DTDATA_KIND_BRUSH);
  assert_int_equal(dt_dtdata_entry_kind("maskabc.png"), -1);
  assert_int_equal(dt_dtdata_entry_kind("version"), -1);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_roundtrip_8bit, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_roundtrip_16bit, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_multiple_entries_and_dedup, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_missing_and_corrupt, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_merge, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_sweep, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_truncated, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_sweep_keep_absent, _setup, _teardown),
    cmocka_unit_test(test_path_for_image),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
