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

/* getpwnam_r availibility check */
#if defined __APPLE__ || defined _POSIX_C_SOURCE >= 1 || defined _XOPEN_SOURCE || defined _BSD_SOURCE || defined _SVID_SOURCE || defined _POSIX_SOURCE
#include <pwd.h>
#include <sys/types.h>
#include "darktable.h"
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "utility.h"

gchar* dt_util_get_home_dir(const gchar* user)
{
  if (user == NULL || g_strcmp0(user, g_get_user_name()) == 0) {
    const char* home_dir = g_getenv("HOME");
    return g_strdup((home_dir != NULL) ? home_dir : g_get_home_dir());
  }

#if defined _POSIX_C_SOURCE >= 1 || defined _XOPEN_SOURCE || defined _BSD_SOURCE || defined _SVID_SOURCE || defined _POSIX_SOURCE
  /* if the given username is not the same as the current one, we try
   * to retreive the pw dir from the password file entry */
  struct passwd pwd;
  struct passwd* result;
#ifdef _SC_GETPW_R_SIZE_MAX
  int bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (bufsize < 0) {
    bufsize = 4096;
  }
#else
  int bufsize = 4096;
#endif

  gchar* buffer = g_malloc0(sizeof(gchar) * bufsize);
  if (buffer == NULL) {
    return NULL;
  }

  getpwnam_r(user, &pwd, buffer, bufsize, &result);
  if (result == NULL) {
    g_free(buffer);
    return NULL;
  }

  gchar* dir = g_strdup(pwd.pw_dir);
  g_free(buffer);

  return dir;
#else
  return NULL;
#endif
}

gchar* dt_util_fix_path(const gchar* path)
{
  if (path == NULL || strlen(path) == 0) {
    return NULL;
  }

  gchar* rpath = NULL;

  /* check if path has a prepended tilde */
  if (path[0] == '~') {
    int len    = strlen(path);
    char* user = NULL;
    int off    = 1;

    /* if the character after the tilde is not a slash we parse
     * the path until the next slash to extend this part with the
     * home directory of the specified user
     *
     * e.g.: ~foo will be evaluated as the home directory of the
     * user foo */

    if (len > 1 && path[1] != '/') {
      while (path[off] != '\0' && path[off] != '/') {
        ++off;
      }

      user = g_strndup(path + 1, off - 1);
    }

    gchar* home_path = dt_util_get_home_dir(user);
    g_free(user);

    if (home_path == NULL) {
      return g_strdup(path);
    }

    rpath = g_build_filename(home_path, path + off, NULL);
  } else {
    rpath = g_strdup(path);
  }

  return rpath;
}

void dt_util_get_user_config_dir (char *data, size_t bufsize)
{
  gchar *homedir = dt_util_get_home_dir(NULL);

  if(homedir)
  {
    g_snprintf (data,bufsize,"%s/.config/darktable",homedir);
    if (g_file_test (data,G_FILE_TEST_EXISTS)==FALSE)
      g_mkdir_with_parents (data,0700);

    g_free(homedir);
  }
}


void dt_util_get_user_cache_dir (char *data, size_t bufsize)
{
  gchar *homedir = dt_util_get_home_dir(NULL);
  if(homedir)
  {
    g_snprintf (data,bufsize,"%s/.cache/darktable",homedir);
    if (g_file_test (data,G_FILE_TEST_EXISTS)==FALSE)
      g_mkdir_with_parents (data,0700);
    g_free(homedir);
  }
}

void dt_util_get_user_local_dir (char *data, size_t bufsize)
{
  gchar *homedir = dt_util_get_home_dir(NULL);
  if(homedir)
  {
    g_snprintf(data,bufsize,"%s/.local",homedir);
    if (g_file_test (data,G_FILE_TEST_EXISTS)==FALSE)
      g_mkdir_with_parents (data,0700);
    g_free(homedir);
  }
}

void dt_util_get_plugindir(char *datadir, size_t bufsize)
{
#if defined(__MACH__) || defined(__APPLE__)
  gchar *curr = g_get_current_dir();
  int contains = 0;
  for(int k=0; darktable.progname[k] != 0; k++) if(darktable.progname[k] == '/')
    {
      contains = 1;
      break;
    }
  if(darktable.progname[0] == '/') // absolute path
    snprintf(datadir, bufsize, "%s", darktable.progname);
  else if(contains) // relative path
    snprintf(datadir, bufsize, "%s/%s", curr, darktable.progname);
  else
  {
    // no idea where we have been called. use compiled in path
    g_free(curr);
    snprintf(datadir, bufsize, "%s/darktable", DARKTABLE_LIBDIR);
    return;
  }
  size_t len = MIN(strlen(datadir), bufsize);
  char *t = datadir + len; // strip off bin/darktable
  for(; t>datadir && *t!='/'; t--);
  t--;
  if(*t == '.' && *(t-1) != '.')
  {
    for(; t>datadir && *t!='/'; t--);
    t--;
  }
  for(; t>datadir && *t!='/'; t--);
  g_strlcpy(t, "/lib/darktable", bufsize-(t-datadir));
  g_free(curr);
#else
  snprintf(datadir, bufsize, "%s/darktable", DARKTABLE_LIBDIR);
#endif
}

void dt_util_get_datadir(char *datadir, size_t bufsize)
{
#if defined(__MACH__) || defined(__APPLE__)
  gchar *curr = g_get_current_dir();
  int contains = 0;
  for(int k=0; darktable.progname[k] != 0; k++) if(darktable.progname[k] == '/')
    {
      contains = 1;
      break;
    }
  if(darktable.progname[0] == '/') // absolute path
    snprintf(datadir, bufsize, "%s", darktable.progname);
  else if(contains) // relative path
    snprintf(datadir, bufsize, "%s/%s", curr, darktable.progname);
  else
  {
    // no idea where we have been called. use compiled in path
    g_free(curr);
    snprintf(datadir, bufsize, "%s", DARKTABLE_DATADIR);
    return;
  }
  size_t len = MIN(strlen(datadir), bufsize);
  char *t = datadir + len; // strip off bin/darktable
  for(; t>datadir && *t!='/'; t--);
  t--;
  if(*t == '.' && *(t-1) != '.')
  {
    for(; t>datadir && *t!='/'; t--);
    t--;
  }
  for(; t>datadir && *t!='/'; t--);
  g_strlcpy(t, "/share/darktable", bufsize-(t-datadir));
  g_free(curr);
#else
  snprintf(datadir, bufsize, "%s", DARKTABLE_DATADIR);
#endif
}



// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
