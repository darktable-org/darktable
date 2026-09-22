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

/* white-box tests for the MCP bridge's path and name handling.
 *
 * these helpers decide which file on disk a request refers to and which file an
 * export writes. every serious defect found in the MCP server so far has been
 * here: a path spelling that slipped past the "is this already in the catalog?"
 * probe imported a duplicate row and, earlier, deleted the user's image when the
 * scratch row was cleaned up again. the cases below are the spellings that
 * actually broke it, so a future change that reopens one of them fails here
 * rather than in someone's catalog.
 *
 * following test_filmicrgb.c, the implementation is #included directly for
 * access to its static functions; lib_darktable supplies what it links against.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "mcp/dt_bridge.c"

// a scratch directory per test run, removed in teardown
static gchar *_tmpdir = NULL;

static int _setup(void **state)
{
  (void)state;
  GError *e = NULL;
  _tmpdir = g_dir_make_tmp("dt-mcp-paths-XXXXXX", &e);
  if(!_tmpdir)
  {
    if(e) g_error_free(e);
    return -1;
  }
#ifndef _WIN32
  // _canonical_path() resolves symlinks, so the expected paths must be built on
  // the resolved directory: on macOS the temp dir lives under /var, which is a
  // symlink to /private/var
  char *real = realpath(_tmpdir, NULL);
  if(real)
  {
    g_free(_tmpdir);
    _tmpdir = g_strdup(real);
    free(real);
  }
#endif
  return 0;
}

static void _rm_rf(const char *path)
{
  GDir *d = g_dir_open(path, 0, NULL);
  if(d)
  {
    const char *name;
    while((name = g_dir_read_name(d)))
    {
      gchar *full = g_build_filename(path, name, NULL);
      if(g_file_test(full, G_FILE_TEST_IS_DIR)
         && !g_file_test(full, G_FILE_TEST_IS_SYMLINK))
        _rm_rf(full);
      else
        g_remove(full);
      g_free(full);
    }
    g_dir_close(d);
  }
  g_rmdir(path);
}

static int _teardown(void **state)
{
  (void)state;
  if(_tmpdir)
  {
    _rm_rf(_tmpdir);
    g_free(_tmpdir);
    _tmpdir = NULL;
  }
  return 0;
}

static void _touch(const char *path)
{
  gchar *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  g_file_set_contents(path, "x", 1, NULL);
}

// ---------------------------------------------------------------------------
// _canonical_path
// ---------------------------------------------------------------------------

// the spelling that imported a duplicate row and a film roll displayed as "."
static void test_canonical_dot_segment(void **state)
{
  (void)state;
  gchar *in = g_build_filename(_tmpdir, ".", "a.raw", NULL);
  gchar *want = g_build_filename(_tmpdir, "a.raw", NULL);
  _touch(want);

  gchar *got = _canonical_path(in);
  assert_non_null(got);
  assert_string_equal(got, want);

  g_free(got); g_free(want); g_free(in);
}

static void test_canonical_parent_segment(void **state)
{
  (void)state;
  gchar *sub = g_build_filename(_tmpdir, "sub", NULL);
  g_mkdir_with_parents(sub, 0755);
  gchar *file = g_build_filename(_tmpdir, "b.raw", NULL);
  _touch(file);

  gchar *in = g_build_filename(sub, "..", "b.raw", NULL);
  gchar *got = _canonical_path(in);
  assert_non_null(got);
  assert_string_equal(got, file);

  g_free(got); g_free(in); g_free(file); g_free(sub);
}

static void test_canonical_double_separator(void **state)
{
  (void)state;
  gchar *file = g_build_filename(_tmpdir, "c.raw", NULL);
  _touch(file);
  gchar *in = g_strdup_printf("%s//c.raw", _tmpdir);

  gchar *got = _canonical_path(in);
  assert_non_null(got);
  assert_string_equal(got, file);

  g_free(got); g_free(in); g_free(file);
}

// dt_util_normalize_path() handles the URI form; canonicalization must survive it
static void test_canonical_file_uri(void **state)
{
  (void)state;
  gchar *file = g_build_filename(_tmpdir, "d.raw", NULL);
  _touch(file);
  gchar *uri = g_strdup_printf("file://%s/./d.raw", _tmpdir);

  gchar *got = _canonical_path(uri);
  assert_non_null(got);
  assert_string_equal(got, file);

  g_free(got); g_free(uri); g_free(file);
}

// a symlinked parent is a different string for the same file, and the probe
// matches film_rolls.folder exactly
static void test_canonical_symlinked_parent(void **state)
{
  (void)state;
  gchar *real = g_build_filename(_tmpdir, "realdir", NULL);
  g_mkdir_with_parents(real, 0755);
  gchar *file = g_build_filename(real, "e.raw", NULL);
  _touch(file);

  gchar *link = g_build_filename(_tmpdir, "linkdir", NULL);
  if(symlink(real, link) != 0)   // no symlink support: nothing to assert
  {
    g_free(link); g_free(file); g_free(real);
    skip();
  }

  gchar *in = g_build_filename(link, "e.raw", NULL);
  gchar *got = _canonical_path(in);
  assert_non_null(got);
  assert_string_equal(got, file);

  g_free(got); g_free(in); g_free(link); g_free(file); g_free(real);
}

// ".." must not walk above the root
static void test_canonical_cannot_escape_root(void **state)
{
  (void)state;
  gchar *got = _canonical_path("/../../..");
  assert_non_null(got);
  assert_string_equal(got, G_DIR_SEPARATOR_S);
  g_free(got);
}

// an already-canonical path must come back untouched, or every probe shifts
static void test_canonical_is_idempotent(void **state)
{
  (void)state;
  gchar *file = g_build_filename(_tmpdir, "f.raw", NULL);
  _touch(file);

  gchar *once = _canonical_path(file);
  assert_non_null(once);
  gchar *twice = _canonical_path(once);
  assert_non_null(twice);
  assert_string_equal(once, file);
  assert_string_equal(twice, once);

  g_free(twice); g_free(once); g_free(file);
}

// a path naming no existing file still canonicalizes: the probe runs before the
// file-exists check, so realpath() failing must not lose the lexical result
static void test_canonical_missing_file(void **state)
{
  (void)state;
  gchar *in = g_build_filename(_tmpdir, ".", "nope.raw", NULL);
  gchar *want = g_build_filename(_tmpdir, "nope.raw", NULL);

  gchar *got = _canonical_path(in);
  assert_non_null(got);
  assert_string_equal(got, want);

  g_free(got); g_free(want); g_free(in);
}

// ---------------------------------------------------------------------------
// _sql_like_escape
// ---------------------------------------------------------------------------

// '_' matched any character, so a folder filter of "Pictures_MCP" also matched
// "Pictures/MCP"
static void test_like_escape(void **state)
{
  (void)state;
  struct { const char *in, *want; } cases[] = {
    { "plain",      "plain"       },
    { "a_b",        "a\\_b"       },
    { "100%",       "100\\%"      },
    { "a\\b",       "a\\\\b"      },
    { "_%\\",       "\\_\\%\\\\"  },
    { "",           ""            },
  };
  for(size_t i = 0; i < G_N_ELEMENTS(cases); i++)
  {
    gchar *got = _sql_like_escape(cases[i].in);
    assert_non_null(got);
    assert_string_equal(got, cases[i].want);
    g_free(got);
  }
}

// ---------------------------------------------------------------------------
// _film_images_since
// ---------------------------------------------------------------------------

// dt_image_import() files a row per "<name>_NN.<ext>.xmp" sidecar besides the
// image (image.c:2068); cleanup used to remove only the id it handed back, so
// those rows and their film roll survived every render of a loose file.
// covers the query alone: the _import_file() -> _drop_scratch() wiring needs a
// booted darktable with an open library, which this binary has not
static void test_film_images_since_lists_duplicates(void **state)
{
  (void)state;
  sqlite3 *db = NULL;
  assert_int_equal(sqlite3_open(":memory:", &db), SQLITE_OK);
  assert_int_equal(sqlite3_exec(db,
      "CREATE TABLE main.images (id INTEGER PRIMARY KEY, film_id INTEGER,"
      "                          filename TEXT, version INTEGER)",
      NULL, NULL, NULL), SQLITE_OK);

  // the catalog as it stood: another roll, and one image already in ours
  assert_int_equal(sqlite3_exec(db,
      "INSERT INTO main.images VALUES (10, 1, 'other.raw', 0),"
      "                               (11, 2, 'pre.raw', 0)",
      NULL, NULL, NULL), SQLITE_OK);
  const dt_imgid_t high = 11;   // what _max_image_id() reads before the import

  // what one import adds: the image, then a row per sidecar duplicate
  assert_int_equal(sqlite3_exec(db,
      "INSERT INTO main.images VALUES (12, 2, 'a.raw', 0),"
      "                               (13, 2, 'a.raw', 1),"
      "                               (14, 2, 'a.raw', 2)",
      NULL, NULL, NULL), SQLITE_OK);

  GList *got = _film_images_since(db, 2, high);
  assert_int_equal(g_list_length(got), 3);
  const int want[] = { 12, 13, 14 };
  int i = 0;
  for(GList *l = got; l; l = g_list_next(l), i++)
    assert_int_equal(GPOINTER_TO_INT(l->data), want[i]);
  g_list_free(got);

  // an import that added nothing owns nothing
  assert_null(_film_images_since(db, 2, 14));
  // and a roll this request never touched is never swept
  assert_null(_film_images_since(db, 1, high));

  // 0 is a real baseline: an empty library, so every row in the roll is ours
  GList *all = _film_images_since(db, 2, 0);
  assert_int_equal(g_list_length(all), 4);   // 11 was already there
  g_list_free(all);

  // -1 is _max_image_id() saying it could not answer. it must not read as 0:
  // _drop_scratch() would delete the rows the user had before the import
  assert_null(_film_images_since(db, 2, -1));

  sqlite3_close(db);
}

// the sentinel above only helps if the failure path actually produces it. with
// no library open the query cannot run, and returning 0 would claim the
// catalog was empty
static void test_max_image_id_without_database(void **state)
{
  (void)state;
  assert_null(darktable.db);   // the precondition this test is about
  assert_int_equal(_max_image_id(), -1);
}

// ---------------------------------------------------------------------------
// _image_is_undeveloped
// ---------------------------------------------------------------------------

// this decides which images --read-only may roll back, and has to be exact both
// ways: a false yes deletes real edits, a false no leaves read-only having
// developed the image. the end-to-end path needs a booted darktable
static void test_undeveloped_only_when_nothing_is_filed(void **state)
{
  (void)state;
  sqlite3 *db = NULL;
  assert_int_equal(sqlite3_open(":memory:", &db), SQLITE_OK);
  assert_int_equal(sqlite3_exec(db,
      "CREATE TABLE main.images (id INTEGER PRIMARY KEY, history_end INTEGER,"
      "                          flags INTEGER);"
      "CREATE TABLE main.history (imgid INTEGER, num INTEGER);"
      "CREATE TABLE main.masks_history (imgid INTEGER, num INTEGER);"
      "CREATE TABLE main.module_order (imgid INTEGER, version INTEGER);"
      "CREATE TABLE main.history_hash (imgid INTEGER, current_hash BLOB)",
      NULL, NULL, NULL), SQLITE_OK);

  // 1: never developed. 2: what a first render leaves. 3-6: one row in each
  // table the rollback would empty. 7: NULL history_end, which the catalog
  // treats as 0. 9: presets applied but nothing added, what workflow "none"
  // leaves (develop.c:2246 sets the flag even with no module inserted)
  assert_int_equal(sqlite3_exec(db,
      "INSERT INTO main.images VALUES (1, 0, 1089), (2, 11, 1601), (3, 0, 1089),"
      "                               (4, 0, 1089), (5, 0, 1089), (6, 0, 1089),"
      "                               (7, NULL, 1089), (8, 11, 1089), (9, 0, 1601);"
      "INSERT INTO main.history VALUES (3, 0);"
      "INSERT INTO main.masks_history VALUES (4, 0);"
      "INSERT INTO main.module_order VALUES (5, 4);"
      "INSERT INTO main.history_hash VALUES (6, NULL)",
      NULL, NULL, NULL), SQLITE_OK);

  assert_true(_image_is_undeveloped(db, 1));
  assert_true(_image_is_undeveloped(db, 7));

  assert_false(_image_is_undeveloped(db, 2));   // presets already applied
  assert_false(_image_is_undeveloped(db, 3));   // has history
  assert_false(_image_is_undeveloped(db, 4));   // has a mask
  assert_false(_image_is_undeveloped(db, 5));   // has a module order
  assert_false(_image_is_undeveloped(db, 6));   // has a history hash
  assert_false(_image_is_undeveloped(db, 8));   // history_end past the rows
  // clearing this flag would have darktable auto-apply all over again
  assert_false(_image_is_undeveloped(db, 9));

  // an id the catalog does not know is not an image to roll back
  assert_false(_image_is_undeveloped(db, 99));

  sqlite3_close(db);
}

// a query that cannot run must read as "developed": the rollback deletes rows,
// so it may only ever act on a state it has actually read back
static void test_undeveloped_without_database(void **state)
{
  (void)state;
  assert_false(_image_is_undeveloped(NULL, 1));

  sqlite3 *db = NULL;
  assert_int_equal(sqlite3_open(":memory:", &db), SQLITE_OK);
  assert_false(_image_is_undeveloped(db, 1));   // no tables to prepare against
  sqlite3_close(db);
}

// ---------------------------------------------------------------------------
// _resolve_conflict
// ---------------------------------------------------------------------------

static void test_conflict_free_name_passthrough(void **state)
{
  (void)state;
  // no file there: returned as given, and this path never consults the config
  gchar *want = g_build_filename(_tmpdir, "free.jpg", NULL);
  gboolean skipped = TRUE;
  gchar *got = _resolve_conflict(want, 0, NULL, &skipped);
  assert_non_null(got);
  assert_false(skipped);
  assert_string_equal(got, want);
  g_free(got); g_free(want);
}

static void test_conflict_unique_name(void **state)
{
  (void)state;
  gchar *base = g_build_filename(_tmpdir, "clash.jpg", NULL);
  _touch(base);

  gboolean skipped = TRUE;
  gchar *first = _resolve_conflict(base, 0, NULL, &skipped);
  assert_non_null(first);
  assert_false(skipped);
  gchar *want1 = g_build_filename(_tmpdir, "clash_01.jpg", NULL);
  assert_string_equal(first, want1);

  // and it keeps counting rather than reusing _01
  _touch(first);
  gchar *second = _resolve_conflict(base, 0, NULL, &skipped);
  assert_non_null(second);
  gchar *want2 = g_build_filename(_tmpdir, "clash_02.jpg", NULL);
  assert_string_equal(second, want2);

  g_free(second); g_free(want2); g_free(first); g_free(want1); g_free(base);
}

// the extension was split over the whole path, so a directory containing a dot
// sent the retry into a different, newly created folder
static void test_conflict_dotted_directory(void **state)
{
  (void)state;
  gchar *dir = g_build_filename(_tmpdir, "has.dot", NULL);
  g_mkdir_with_parents(dir, 0755);
  gboolean skipped = TRUE;

  // with an extension the last dot of the whole path is the right one anyway,
  // so this alone would not notice the split being done over the full string
  gchar *base = g_build_filename(dir, "g.jpg", NULL);
  _touch(base);
  gchar *got = _resolve_conflict(base, 0, NULL, &skipped);
  assert_non_null(got);
  gchar *want = g_build_filename(dir, "g_01.jpg", NULL);
  assert_string_equal(got, want);
  g_free(want); g_free(got); g_free(base);

  // the case that actually breaks: no dot in the basename, so a split over the
  // whole path lands on the directory's dot and the retry moves to a sibling
  // directory that does not exist
  gchar *noext = g_build_filename(dir, "plain", NULL);
  _touch(noext);
  got = _resolve_conflict(noext, 0, NULL, &skipped);
  assert_non_null(got);
  want = g_build_filename(dir, "plain_01", NULL);
  assert_string_equal(got, want);

  // whatever it picked must stay inside the directory it started in
  gchar *got_dir = g_path_get_dirname(got);
  assert_string_equal(got_dir, dir);

  g_free(got_dir); g_free(want); g_free(got); g_free(noext); g_free(dir);
}

static void test_conflict_overwrite_and_skip(void **state)
{
  (void)state;
  gchar *base = g_build_filename(_tmpdir, "policy.jpg", NULL);
  _touch(base);
  gboolean skipped = TRUE;

  gchar *over = _resolve_conflict(base, 1, NULL, &skipped);   // overwrite
  assert_non_null(over);
  assert_false(skipped);
  assert_string_equal(over, base);
  g_free(over);

  gchar *skip_out = _resolve_conflict(base, 3, NULL, &skipped);   // skip
  assert_null(skip_out);
  assert_true(skipped);

  g_free(base);
}

// colliding basenames resolve their targets before either file is written, so
// the filesystem cannot warn the second. losing an image is never what a policy
// asked for, so an in-batch collision takes a free name under every policy
static void _check_batch_collision(const int action)
{
  gchar *dir = g_strdup_printf("%s/batch%d", _tmpdir, action);
  g_mkdir_with_parents(dir, 0755);
  GHashTable *claimed =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gchar *base = g_build_filename(dir, "dup.jpg", NULL);
  gboolean skipped = TRUE;

  gchar *first = _resolve_conflict(base, action, claimed, &skipped);
  assert_non_null(first);
  assert_false(skipped);
  assert_string_equal(first, base);
  g_hash_table_add(claimed, g_strdup(first));

  // nothing has been written yet: only `claimed` knows this name is taken
  assert_false(g_file_test(base, G_FILE_TEST_EXISTS));
  gchar *second = _resolve_conflict(base, action, claimed, &skipped);
  assert_non_null(second);
  assert_false(skipped);
  gchar *want = g_build_filename(dir, "dup_01.jpg", NULL);
  assert_string_equal(second, want);
  g_hash_table_add(claimed, g_strdup(second));

  // and it keeps counting rather than handing _01 out twice
  gchar *third = _resolve_conflict(base, action, claimed, &skipped);
  assert_non_null(third);
  gchar *want2 = g_build_filename(dir, "dup_02.jpg", NULL);
  assert_string_equal(third, want2);

  g_free(want2); g_free(third); g_free(want); g_free(second);
  g_free(first); g_free(base);
  g_hash_table_destroy(claimed);
  g_free(dir);
}

static void test_conflict_in_batch_collision(void **state)
{
  (void)state;
  for(int action = 0; action <= 3; action++)
    _check_batch_collision(action);
}

// a file that was already on disk and a name the batch has claimed are both
// taken: the retry loop has to step over either
static void test_conflict_in_batch_skips_existing(void **state)
{
  (void)state;
  GHashTable *claimed =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gchar *base = g_build_filename(_tmpdir, "both.jpg", NULL);
  _touch(base);
  gboolean skipped = TRUE;

  gchar *first = _resolve_conflict(base, 0, claimed, &skipped);
  gchar *want1 = g_build_filename(_tmpdir, "both_01.jpg", NULL);
  assert_non_null(first);
  assert_string_equal(first, want1);
  g_hash_table_add(claimed, g_strdup(first));

  gchar *second = _resolve_conflict(base, 0, claimed, &skipped);
  gchar *want2 = g_build_filename(_tmpdir, "both_02.jpg", NULL);
  assert_non_null(second);
  assert_string_equal(second, want2);

  g_free(want2); g_free(second); g_free(want1); g_free(first); g_free(base);
  g_hash_table_destroy(claimed);
}

int main()
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_canonical_dot_segment),
    cmocka_unit_test(test_canonical_parent_segment),
    cmocka_unit_test(test_canonical_double_separator),
    cmocka_unit_test(test_canonical_file_uri),
    cmocka_unit_test(test_canonical_symlinked_parent),
    cmocka_unit_test(test_canonical_cannot_escape_root),
    cmocka_unit_test(test_canonical_is_idempotent),
    cmocka_unit_test(test_canonical_missing_file),
    cmocka_unit_test(test_like_escape),
    cmocka_unit_test(test_film_images_since_lists_duplicates),
    cmocka_unit_test(test_max_image_id_without_database),
    cmocka_unit_test(test_undeveloped_only_when_nothing_is_filed),
    cmocka_unit_test(test_undeveloped_without_database),
    cmocka_unit_test(test_conflict_free_name_passthrough),
    cmocka_unit_test(test_conflict_unique_name),
    cmocka_unit_test(test_conflict_dotted_directory),
    cmocka_unit_test(test_conflict_overwrite_and_skip),
    cmocka_unit_test(test_conflict_in_batch_collision),
    cmocka_unit_test(test_conflict_in_batch_skips_existing),
  };
  return cmocka_run_group_tests(tests, _setup, _teardown);
}
