/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _XOPEN_SOURCE 700
#include "common/sidecar.h"

#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void remove_directory(const char *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if(dir)
  {
    const char *entry;
    while((entry = g_dir_read_name(dir)))
    {
      gchar *child = g_build_filename(path, entry, NULL);
      GStatBuf info;
      g_assert_cmpint(g_lstat(child, &info), ==, 0);
      if(S_ISDIR(info.st_mode)) remove_directory(child);
      else g_assert_cmpint(g_unlink(child), ==, 0);
      g_free(child);
    }
    g_dir_close(dir);
  }
  g_assert_cmpint(g_rmdir(path), ==, 0);
}

static gchar *new_directory(void)
{
  GError *error = NULL;
  gchar *path = g_dir_make_tmp("dt-sidecar-test-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(path);
  return path;
}

static void write_file(const char *directory, const char *name, const char *contents)
{
  gchar *path = g_build_filename(directory, name, NULL);
  GError *error = NULL;
  g_assert_true(g_file_set_contents(path, contents, -1, &error));
  g_assert_no_error(error);
  g_free(path);
}

static void assert_paths(GList *paths, const char *directory, const char *const *expected)
{
  GList *item = paths;
  for(size_t i = 0; expected[i]; i++)
  {
    g_assert_nonnull(item);
    gchar *path = directory ? g_build_filename(directory, expected[i], NULL) : g_strdup(expected[i]);
    g_assert_cmpstr(item->data, ==, path);
    g_free(path);
    item = item->next;
  }
  g_assert_null(item);
  g_list_free_full(paths, g_free);
}

static void literal_names_and_order(void)
{
  gchar *root = new_directory();
  gchar *directory = g_build_filename(root, "相片.[旅行]*?", NULL);
  g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
  const char *const expected[] = {
    "夕陽[1]*?\\_2.GPR.xmp",
    "夕陽[1]*?\\_2_000.GPR.xmp",
    "夕陽[1]*?\\_2_01.GPR.xmp",
    "夕陽[1]*?\\_2_10.GPR.xmp",
    "夕陽[1]*?\\_2_100.GPR.xmp",
    "夕陽[1]*?\\_2_99.GPR.xmp",
    NULL
  };
  /* Deliberately create in reverse order to verify enumeration order is not
   * exposed to the caller, and that unnumbered history remains first. */
  for(int i = 5; i >= 0; i--) write_file(directory, expected[i], "<xmp/>");
  const char *const rejected[] = {
    "夕陽[1]*?\\_2_1.GPR.xmp", "夕陽[1]*?\\_2_.GPR.xmp",
    "夕陽[1]*?\\_2_01_more.GPR.xmp", "夕陽[1]*?\\_2_01a.GPR.xmp",
    "夕陽[1]*?\\_2_01.GPR.xmp.bak", "夕陽[1]*?\\_2_01.GPR.XMP",
    "夕陽[1]*?\\_2_01.gpr.xmp", "夕陽[1]*?\\_2_０１.GPR.xmp",
    "夕陽[1]*?\\_2_+01.GPR.xmp", "夕陽[1]*?\\_20_01.GPR.xmp",
    "夕陽1something_2_01.GPR.xmp", NULL
  };
  for(int i = 0; rejected[i]; i++) write_file(directory, rejected[i], "<xmp/>");
  gchar *source = g_build_filename(directory, "夕陽[1]*?\\_2.GPR", NULL);
  assert_paths(dt_sidecar_find_duplicates(source), directory, expected);
  g_free(source);
  remove_directory(root);
  g_free(directory);
  g_free(root);
}

static void valid_files_only(void)
{
  gchar *directory = new_directory();
  write_file(directory, "photo.GPR.xmp", "");
  write_file(directory, "photo_01.GPR.xmp", "");
  write_file(directory, "history.xmp", "<xmp/>");
  gchar *subdir = g_build_filename(directory, "photo_02.GPR.xmp", NULL);
  g_assert_cmpint(g_mkdir(subdir, 0700), ==, 0);
  gchar *link = g_build_filename(directory, "photo_03.GPR.xmp", NULL);
  g_assert_cmpint(symlink("history.xmp", link), ==, 0);
  g_free(link);
  link = g_build_filename(directory, "photo_04.GPR.xmp", NULL);
  g_assert_cmpint(symlink("missing.xmp", link), ==, 0);
  g_free(link);
  link = g_build_filename(directory, "photo_05.GPR.xmp", NULL);
  g_assert_cmpint(symlink("photo_02.GPR.xmp", link), ==, 0);
  g_free(link);
  gchar *fifo = g_build_filename(directory, "photo_06.GPR.xmp", NULL);
  g_assert_cmpint(mkfifo(fifo, 0600), ==, 0);
  gchar *source = g_build_filename(directory, "photo.GPR", NULL);
  const char *const expected[] = { "photo_03.GPR.xmp", NULL };
  assert_paths(dt_sidecar_find_duplicates(source), directory, expected);
  g_free(source);
  g_free(subdir);
  g_free(fifo);
  remove_directory(directory);
  g_free(directory);
}

static void relative_paths_and_missing_inputs(void)
{
  gchar *directory = new_directory();
  write_file(directory, "photo.GPR.xmp", "<xmp/>");
  write_file(directory, "photo_01.GPR.xmp", "<xmp/>");
  gchar *previous = g_get_current_dir();
  g_assert_cmpint(g_chdir(directory), ==, 0);
  const char *const expected[] = { "photo.GPR.xmp", "photo_01.GPR.xmp", NULL };
  assert_paths(dt_sidecar_find_duplicates("photo.GPR"), NULL, expected);
  const char *const expected_dot[] = { "./photo.GPR.xmp", "./photo_01.GPR.xmp", NULL };
  assert_paths(dt_sidecar_find_duplicates("./photo.GPR"), NULL, expected_dot);
  g_assert_null(dt_sidecar_find_duplicates("missing/photo.GPR"));
  g_assert_null(dt_sidecar_find_duplicates(NULL));
  g_assert_null(dt_sidecar_find_duplicates(""));
  g_assert_null(dt_sidecar_find_duplicates("./"));
  g_assert_cmpint(g_chdir(previous), ==, 0);
  g_free(previous);
  remove_directory(directory);
  g_free(directory);
}

static void extension_in_basename_only(void)
{
  gchar *root = new_directory();
  gchar *directory = g_build_filename(root, "folder.with.dots", NULL);
  g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
  write_file(directory, "photo.xmp", "<xmp/>");
  write_file(directory, "photo_01.xmp", "<xmp/>");
  gchar *source = g_build_filename(directory, "photo", NULL);
  const char *const expected[] = { "photo.xmp", NULL };
  assert_paths(dt_sidecar_find_duplicates(source), directory, expected);
  g_free(source);
  remove_directory(root);
  g_free(directory);
  g_free(root);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/sidecar/literal-names-and-order", literal_names_and_order);
  g_test_add_func("/sidecar/valid-files-only", valid_files_only);
  g_test_add_func("/sidecar/relative-and-missing", relative_paths_and_missing_inputs);
  g_test_add_func("/sidecar/extension-in-basename-only", extension_in_basename_only);
  return g_test_run();
}
