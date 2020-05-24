/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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
#include "common/grealpath.h"

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

gchar *dt_loc_init_generic(const char *value, const char *default_value)
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
  dt_check_opendir("darktable.configdir", darktable.configdir);
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
  // return 1 instead of error originally
  dt_check_opendir("darktable.tmpdir", darktable.tmpdir);
  if(darktable.tmpdir == NULL) return 1;
  return 0;
}

void dt_loc_init_user_cache_dir(const char *cachedir)
{
  char *default_cache_dir = g_build_filename(g_get_user_cache_dir(), "darktable", NULL);
  darktable.cachedir = dt_loc_init_generic(cachedir, default_cache_dir);
  dt_check_opendir("darktable.cachedir", darktable.cachedir);
  g_free(default_cache_dir);
}

void dt_loc_init_plugindir(const char* application_directory, const char *plugindir)
{
#if defined(__APPLE__) || defined(_WIN32)
  char *suffix = g_build_filename("lib", "darktable", NULL);
  char *directory = dt_loc_find_install_dir(suffix, darktable.progname);
  g_free(suffix);
  darktable.plugindir = dt_loc_init_generic(plugindir, directory ? directory : DARKTABLE_LIBDIR);
  g_free(directory);
#else
  gchar* path = dt_loc_init_generic(plugindir, DARKTABLE_LIBDIR);
  gchar complete_path[PATH_MAX] = { 0 };
  g_snprintf(complete_path, sizeof(complete_path), "%s%s", application_directory, path);
  free(path);
  darktable.plugindir = g_realpath(complete_path);
  dt_check_opendir("darktable.plugindir", darktable.plugindir);
#endif
}

void dt_check_opendir(const char* text, const char* directory)
{
  if (!directory) {
    printf("directory for %s has not been set.\n", text);
    exit(EXIT_FAILURE);
  } 

  DIR* dir = opendir(directory);
  if (dir) {
    dt_print(DT_DEBUG_DEV, "%s: %s\n", text, directory);
    closedir(dir);
  } 
  else if ( errno == EACCES ) 
  {
    printf("Permission denied.\n");
    exit(EXIT_FAILURE);
  } 
  else if ( errno == ENOENT ) 
  {
    printf("%s %s does not exist.\n", text, directory);
    exit(EXIT_FAILURE);
  } 
  else if ( errno == EBADF ) 
  {
    printf("fd is not a valid file descriptor opened for reading.\n");
    exit(EXIT_FAILURE);
  } 
  else if ( errno == EMFILE ) 
  {
    printf("The per-process limit on the number of open file descriptors has been reached.\n");
    exit(EXIT_FAILURE);
  } 
  else if ( errno == ENFILE ) 
  {
    printf("The system-wide limit on the total number of open files has been reached.\n");
    exit(EXIT_FAILURE);
  } 
  else if ( errno == ENOENT ) 
  {
    printf("Directory does not exist, or name is an empty string.\n");
    exit(EXIT_FAILURE);
  }  
  else if ( errno == ENOMEM ) 
  {
    printf("Insufficient memory to complete the operation.\n");
    exit(EXIT_FAILURE);
  }  
  else if ( errno == ENOTDIR ) 
  {
    printf("Name is not a directory.\n");
    exit(EXIT_FAILURE);
  } 
  else 
  {
    printf("opendir() failed for some other reason.\n");
    exit(EXIT_FAILURE);
  }
}

void dt_loc_init_localedir(const char* application_directory, const char *localedir)
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
  gchar* path = dt_loc_init_generic(localedir, DARKTABLE_LOCALEDIR);
  gchar complete_path[PATH_MAX] = { 0 };
  g_snprintf(complete_path, sizeof(complete_path), "%s%s", application_directory, path);
  free(path);
  darktable.localedir = g_realpath(complete_path);
  dt_check_opendir("darktable.localedir", darktable.localedir);
#endif
}

void dt_loc_init_datadir(const char* application_directory, const char *datadir)
{
#if defined(__APPLE__) || defined(_WIN32)
  char *suffix = g_build_filename("share", "darktable", NULL);
  char *directory = dt_loc_find_install_dir(suffix, darktable.progname);
  g_free(suffix);
  darktable.datadir = dt_loc_init_generic(datadir, directory ? directory : DARKTABLE_DATADIR);
  g_free(directory);
#else

  gchar* path = dt_loc_init_generic(datadir, DARKTABLE_DATADIR);
  // printf("path: %s\n", path);

  gchar complete_path[PATH_MAX] = { 0 };
  g_snprintf(complete_path, sizeof(complete_path), "%s%s", application_directory, path);
  free(path);
  darktable.datadir = g_realpath(complete_path);
  dt_check_opendir("darktable.datadir", darktable.datadir);
#endif
}


void dt_loc_get_plugindir(char *plugindir, size_t bufsize)
{
  g_strlcpy(plugindir, darktable.plugindir, bufsize);
}

void dt_loc_get_localedir(char *localedir, size_t bufsize)
{
  g_strlcpy(localedir, darktable.localedir, bufsize);
}

void dt_loc_get_user_config_dir(char *configdir, size_t bufsize)
{
  g_strlcpy(configdir, darktable.configdir, bufsize);
}
void dt_loc_get_user_cache_dir(char *cachedir, size_t bufsize)
{
  g_strlcpy(cachedir, darktable.cachedir, bufsize);
}
void dt_loc_get_tmp_dir(char *tmpdir, size_t bufsize)
{
  g_strlcpy(tmpdir, darktable.tmpdir, bufsize);
}
void dt_loc_get_datadir(char *datadir, size_t bufsize)
{
  g_strlcpy(datadir, darktable.datadir, bufsize);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
