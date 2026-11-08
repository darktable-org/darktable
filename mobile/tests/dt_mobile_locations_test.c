/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "common/darktable.h"
#include "common/file_location.h"

/* Compile the real file_location.c twice on the host. Defining __ANDROID__
 * exercises only that resolver's platform branch, not Android execution. A
 * separate, valid executable-relative layout makes choosing it by mistake an
 * observable assertion failure, instead of terminating in g_realpath(). */
int main(void)
{
  GError *error = NULL;
  gchar *root = g_dir_make_tmp("dt-mobile-locations-XXXXXX", &error);
  g_assert_no_error(error);
  g_assert_nonnull(root);
  gchar *runtime = g_build_filename(root, "private-runtime", NULL);
  gchar *data = g_build_filename(runtime, "data", NULL);
  gchar *locale = g_build_filename(runtime, "locale", NULL);
  gchar *desktop = g_build_filename(root, "desktop", NULL);
  gchar *executable_directory = g_build_filename(desktop, "bin", NULL);
  gchar *desktop_share = g_build_filename(desktop, "share", NULL);
  gchar *desktop_locale = g_build_filename(desktop_share, "locale", NULL);
  gchar *override = g_build_filename(root, "explicit-locale", NULL);
  gchar *data_link = g_build_filename(root, "data-link", NULL);
  gchar *data_argument = g_strconcat(data_link, "/", NULL);
  g_assert_cmpint(g_mkdir_with_parents(data, 0700), ==, 0);
  g_assert_cmpint(g_mkdir_with_parents(locale, 0700), ==, 0);
  g_assert_cmpint(g_mkdir_with_parents(executable_directory, 0700), ==, 0);
  g_assert_cmpint(g_mkdir_with_parents(desktop_locale, 0700), ==, 0);
  g_assert_cmpint(g_mkdir_with_parents(override, 0700), ==, 0);
  g_assert_cmpint(symlink(data, data_link), ==, 0);

  /* Public configuration may use a symlink and trailing separator. Defaults
   * must follow the canonical datadir, just like the native preflight check. */
  dt_loc_init_datadir(executable_directory, data_argument);
  dt_loc_init_localedir(executable_directory, NULL);
  dt_loc_init_sharedir(executable_directory);
  char resolved[PATH_MAX] = { 0 };
  dt_loc_get_datadir(resolved, sizeof(resolved));
  g_assert_cmpstr(resolved, ==, data);
  dt_loc_get_localedir(resolved, sizeof(resolved));
#ifdef __ANDROID__
  g_assert_cmpstr(resolved, ==, locale);
  dt_loc_get_sharedir(resolved, sizeof(resolved));
  g_assert_cmpstr(resolved, ==, runtime);
#else
  g_assert_cmpstr(resolved, ==, desktop_locale);
  dt_loc_get_sharedir(resolved, sizeof(resolved));
  g_assert_cmpstr(resolved, ==, desktop_share);
#endif
  g_free(darktable.localedir);
  dt_loc_init_localedir(executable_directory, override);
  dt_loc_get_localedir(resolved, sizeof(resolved));
  g_assert_cmpstr(resolved, ==, override);
  g_free(darktable.datadir);
  g_free(darktable.localedir);
  g_free(darktable.sharedir);

  g_assert_cmpint(g_unlink(data_link), ==, 0);
  g_assert_cmpint(g_rmdir(data), ==, 0);
  g_assert_cmpint(g_rmdir(locale), ==, 0);
  g_assert_cmpint(g_rmdir(runtime), ==, 0);
  g_assert_cmpint(g_rmdir(desktop_locale), ==, 0);
  g_assert_cmpint(g_rmdir(desktop_share), ==, 0);
  g_assert_cmpint(g_rmdir(executable_directory), ==, 0);
  g_assert_cmpint(g_rmdir(desktop), ==, 0);
  g_assert_cmpint(g_rmdir(override), ==, 0);
  g_assert_cmpint(g_rmdir(root), ==, 0);
  g_free(data);
  g_free(locale);
  g_free(runtime);
  g_free(desktop_locale);
  g_free(desktop_share);
  g_free(executable_directory);
  g_free(desktop);
  g_free(override);
  g_free(data_link);
  g_free(data_argument);
  g_free(root);
  return 0;
}
