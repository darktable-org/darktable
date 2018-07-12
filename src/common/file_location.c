/*
    This file is part of darktable,
    copyright (c) 2012 Jeremy Rosen

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

/* getpwnam_r availability check */
#if defined __APPLE__ || defined _POSIX_C_SOURCE >= 1 || defined _XOPEN_SOURCE || defined _BSD_SOURCE        \
    || defined _SVID_SOURCE || defined _POSIX_SOURCE || defined __DragonFly__ || defined __FreeBSD__         \
    || defined __NetBSD__ || defined __OpenBSD__
#include "config.h"
#include <pwd.h>
#include <sys/types.h>
#define HAVE_GETPWNAM_R 1
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef __APPLE__
#include "osx/osx.h"
#endif

#include "darktable.h"
#include "file_location.h"

gchar *dt_loc_get_home_dir(const gchar *user)
{
  if(user == NULL || g_strcmp0(user, g_get_user_name()) == 0)
  {
    const char *home_dir = g_getenv("HOME");
    return g_strdup((home_dir != NULL) ? home_dir : g_get_home_dir());
  }

#if defined HAVE_GETPWNAM_R
  /* if the given username is not the same as the current one, we try
   * to retrieve the pw dir from the password file entry */
  struct passwd pwd;
  struct passwd *result;
#ifdef _SC_GETPW_R_SIZE_MAX
  int bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
  if(bufsize < 0)
  {
    bufsize = 4096;
  }
#else
  int bufsize = 4096;
#endif

  gchar *buffer = g_malloc0_n(bufsize, sizeof(gchar));
  if(buffer == NULL)
  {
    return NULL;
  }

  getpwnam_r(user, &pwd, buffer, bufsize, &result);
  if(result == NULL)
  {
    g_free(buffer);
    return NULL;
  }

  gchar *dir = g_strdup(pwd.pw_dir);
  g_free(buffer);

  return dir;
#else
  return NULL;
#endif
}

static gchar *dt_loc_init_generic(const char *value, const char *default_value)
{
  const gchar *path = value ? value : default_value;
  gchar *result = dt_util_fix_path(path);
  if(g_file_test(result, G_FILE_TEST_EXISTS) == FALSE) g_mkdir_with_parents(result, 0700);
  return result;
}

void dt_loc_init_user_config_dir(const char *configdir)
{
  char *default_config_dir = g_build_filename(g_get_user_config_dir(), "darktable", NULL);
  darktable.configdir = dt_loc_init_generic(configdir, default_config_dir);
  g_free(default_config_dir);
}

#ifdef __APPLE__
char *dt_loc_find_install_dir(const char *suffix, const char *searchname)
{
  char *result = NULL;
  char *res_path = dt_osx_get_bundle_res_path();

  if(res_path)
    result = g_build_filename(res_path, suffix, NULL);
  g_free(res_path);

  return result;
}
#elif defined(_WIN32)
char *dt_loc_find_install_dir(const char *suffix, const char *searchname)
{
  gchar *runtime_prefix;
  gchar *slash;
  gchar *finaldir;
  wchar_t fn[PATH_MAX];

  GetModuleFileNameW(NULL, fn, G_N_ELEMENTS(fn));
  runtime_prefix = g_utf16_to_utf8(fn, -1, NULL, NULL, NULL);

  // strip off /darktable
  slash = strrchr(runtime_prefix, '\\');
  *slash = '\0';

  // strip off /bin
  slash = strrchr(runtime_prefix, '\\');
  *slash = '\0';

  finaldir = g_build_filename(runtime_prefix, suffix, NULL);
  g_free(runtime_prefix);

  return finaldir;
}
#endif

int dt_loc_init_tmp_dir(const char *tmpdir)
{
  darktable.tmpdir = dt_loc_init_generic(tmpdir, g_get_tmp_dir());
  if(darktable.tmpdir == NULL) return 1;
  return 0;
}

void dt_loc_init_user_cache_dir(const char *cachedir)
{
  char *default_cache_dir = g_build_filename(g_get_user_cache_dir(), "darktable", NULL);
  darktable.cachedir = dt_loc_init_generic(cachedir, default_cache_dir);
  g_free(default_cache_dir);
}

void dt_loc_init_plugindir(const char *plugindir)
{
#if defined(__APPLE__) || defined(_WIN32)
  char *suffix = g_build_filename("lib", "darktable", NULL);
  char *directory = dt_loc_find_install_dir(suffix, darktable.progname);
  g_free(suffix);
  darktable.plugindir = dt_loc_init_generic(plugindir, directory ? directory : DARKTABLE_LIBDIR);
  g_free(directory);
#else
  darktable.plugindir = dt_loc_init_generic(plugindir, DARKTABLE_LIBDIR);
#endif
}

void dt_loc_init_localedir(const char *localedir)
{
#if defined(__APPLE__) || defined(_WIN32)
  char *suffix = g_build_filename("share", "locale", NULL);
  char *directory = dt_loc_find_install_dir(suffix, darktable.progname);
  g_free(suffix);
  darktable.localedir = dt_loc_init_generic(localedir, directory ? directory : DARKTABLE_LOCALEDIR);
#ifdef __APPLE__
  if(directory && !localedir) //bind to bundle path
    bindtextdomain(GETTEXT_PACKAGE, darktable.localedir);
#endif
  g_free(directory);
#else
  darktable.localedir = dt_loc_init_generic(localedir, DARKTABLE_LOCALEDIR);
#endif
}

void dt_loc_init_datadir(const char *datadir)
{
#if defined(__APPLE__) || defined(_WIN32)
  char *suffix = g_build_filename("share", "darktable", NULL);
  char *directory = dt_loc_find_install_dir(suffix, darktable.progname);
  g_free(suffix);
  darktable.datadir = dt_loc_init_generic(datadir, directory ? directory : DARKTABLE_DATADIR);
  g_free(directory);
#else
  darktable.datadir = dt_loc_init_generic(datadir, DARKTABLE_DATADIR);
#endif
}


void dt_loc_get_plugindir(char *plugindir, size_t bufsize)
{
  snprintf(plugindir, bufsize, "%s", darktable.plugindir);
}

void dt_loc_get_localedir(char *localedir, size_t bufsize)
{
  snprintf(localedir, bufsize, "%s", darktable.localedir);
}

void dt_loc_get_user_config_dir(char *configdir, size_t bufsize)
{
  snprintf(configdir, bufsize, "%s", darktable.configdir);
}
void dt_loc_get_user_cache_dir(char *cachedir, size_t bufsize)
{
  snprintf(cachedir, bufsize, "%s", darktable.cachedir);
}
void dt_loc_get_tmp_dir(char *tmpdir, size_t bufsize)
{
  snprintf(tmpdir, bufsize, "%s", darktable.tmpdir);
}
void dt_loc_get_datadir(char *datadir, size_t bufsize)
{
  snprintf(datadir, bufsize, "%s", darktable.datadir);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
