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

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

static int failure, writes;

static gboolean _write_all(GOutputStream *stream, const void *buffer, gsize size,
                           gsize *written, GCancellable *cancel, GError **error)
{
  if(failure == 1 && writes++ > 0)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "injected write failure");
    return FALSE;
  }
  return g_output_stream_write_all(stream, buffer, size, written, cancel, error);
}

static gboolean _close_output(GOutputStream *stream, GCancellable *cancel, GError **error)
{
  const gboolean ok = g_output_stream_close(stream, cancel, error);
  if(ok && failure == 2)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "injected close failure");
    return FALSE;
  }
  return ok;
}

static gssize _read_input(GInputStream *stream, void *buffer, gsize size,
                           GCancellable *cancel, GError **error)
{
  if(failure == 3)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "injected read failure");
    return -1;
  }
  return g_input_stream_read(stream, buffer, size, cancel, error);
}

static gboolean _move(GFile *source, GFile *destination, GFileCopyFlags flags,
                       GCancellable *cancel, GFileProgressCallback progress,
                       gpointer data, GError **error)
{
  if(failure == 4)
  {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "injected move failure");
    return FALSE;
  }
  return g_file_move(source, destination, flags, cancel, progress, data, error);
}

#define g_output_stream_write_all _write_all
#define g_output_stream_close _close_output
#define g_input_stream_read _read_input
#define g_file_move _move
#include "imageio/imageio_staging.h"
#undef g_output_stream_write_all
#undef g_output_stream_close
#undef g_input_stream_read
#undef g_file_move

typedef struct _fixture_t
{
  char *directory, *source, *destination;
  int checks, cancel_at, create_at;
} _fixture_t;

static int _setup(void **state)
{
  _fixture_t *f = g_new0(_fixture_t, 1);
  f->directory = g_dir_make_tmp("dt-imageio-staging-XXXXXX", NULL);
  assert_non_null(f->directory);
  f->source = g_build_filename(f->directory, "source", NULL);
  f->destination = g_build_filename(f->directory, "output", NULL);
  const gsize size = 3 * 1024 * 1024 + 19;
  char *data = g_malloc(size);
  for(gsize i = 0; i < size; i++) data[i] = (char)(i % 251);
  assert_true(g_file_set_contents(f->source, data, size, NULL));
  g_free(data);
  failure = writes = 0;
  *state = f;
  return 0;
}

static void _assert_no_temporaries(_fixture_t *f)
{
  GDir *directory = g_dir_open(f->directory, 0, NULL);
  assert_non_null(directory);
  const char *name;
  while((name = g_dir_read_name(directory)))
    assert_false(g_str_has_prefix(name, ".darktable-export-"));
  g_dir_close(directory);
}

static int _teardown(void **state)
{
  _fixture_t *f = *state;
  _assert_no_temporaries(f);
  g_unlink(f->source);
  g_unlink(f->destination);
  g_rmdir(f->directory);
  g_free(f->source);
  g_free(f->destination);
  g_free(f->directory);
  g_free(f);
  return 0;
}

static void _assert_contents(const char *path, const char *expected)
{
  char *contents = NULL;
  assert_true(g_file_get_contents(path, &contents, NULL, NULL));
  assert_string_equal(contents, expected);
  g_free(contents);
}

static void _test_copy_and_overwrite(void **state)
{
  _fixture_t *f = *state;
  for(int overwrite = 0; overwrite < 2; overwrite++)
  {
    if(overwrite) assert_true(g_file_set_contents(f->destination, "old", -1, NULL));
    GError *error = NULL;
    assert_true(_imageio_publish_staged_file(f->source, f->destination,
                                             overwrite, NULL, NULL, &error));
    assert_null(error);
    char *a = NULL, *b = NULL;
    gsize na, nb;
    assert_true(g_file_get_contents(f->source, &a, &na, NULL));
    assert_true(g_file_get_contents(f->destination, &b, &nb, NULL));
    assert_int_equal(na, nb);
    assert_memory_equal(a, b, na);
    g_free(a);
    g_free(b);
  }
}

static gboolean _cancel_or_create(gpointer data)
{
  _fixture_t *f = data;
  f->checks++;
  if(f->checks == f->create_at)
    assert_true(g_file_set_contents(f->destination, "competitor", -1, NULL));
  return f->cancel_at && f->checks >= f->cancel_at;
}

static void _test_cancellation(void **state)
{
  _fixture_t *f = *state;
  // four chunks plus EOF: the sixth check is immediately before publication
  const int checks[] = { 1, 2, 6 };
  for(size_t i = 0; i < G_N_ELEMENTS(checks); i++)
  {
    f->checks = 0;
    f->cancel_at = checks[i];
    GError *error = NULL;
    assert_false(_imageio_publish_staged_file(f->source, f->destination, FALSE,
                                              _cancel_or_create, f, &error));
    assert_true(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
    g_clear_error(&error);
    assert_false(g_file_test(f->destination, G_FILE_TEST_EXISTS));
    _assert_no_temporaries(f);
  }
}

static void _test_no_overwrite_and_race(void **state)
{
  _fixture_t *f = *state;
  for(int race = 0; race < 2; race++)
  {
    if(!race) assert_true(g_file_set_contents(f->destination, "competitor", -1, NULL));
    else f->create_at = 6;
    f->checks = 0;
    GError *error = NULL;
    assert_false(_imageio_publish_staged_file(f->source, f->destination, FALSE,
                                              _cancel_or_create, f, &error));
    assert_true(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS));
    g_clear_error(&error);
    _assert_contents(f->destination, "competitor");
    g_unlink(f->destination);
    _assert_no_temporaries(f);
  }
}

static void _test_failures_preserve_destination(void **state)
{
  _fixture_t *f = *state;
  assert_true(g_file_set_contents(f->destination, "old", -1, NULL));
  for(failure = 1; failure <= 4; failure++)
  {
    writes = 0;
    GError *error = NULL;
    assert_false(_imageio_publish_staged_file(f->source, f->destination, TRUE,
                                              NULL, NULL, &error));
    assert_non_null(error);
    g_clear_error(&error);
    _assert_contents(f->destination, "old");
    _assert_no_temporaries(f);
  }
}

static void _test_open_failures(void **state)
{
  _fixture_t *f = *state;
  char *missing = g_build_filename(f->directory, "missing", "file", NULL);
  GError *error = NULL;
  assert_false(_imageio_publish_staged_file(missing, f->destination, FALSE,
                                            NULL, NULL, &error));
  assert_non_null(error);
  g_clear_error(&error);
  assert_false(_imageio_publish_staged_file(f->source, missing, FALSE,
                                            NULL, NULL, &error));
  assert_non_null(error);
  g_clear_error(&error);
  assert_false(g_file_test(f->destination, G_FILE_TEST_EXISTS));
  g_free(missing);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(_test_copy_and_overwrite, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_test_cancellation, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_test_no_overwrite_and_race, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_test_failures_preserve_destination, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_test_open_failures, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
