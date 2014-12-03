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
#include <pwd.h>
#include <sys/types.h>
#include "config.h"
#define HAVE_GETPWNAM_R 1
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "file_location.h"
#include "darktable.h"

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
  gchar *xdg_config_dir = NULL;
  gchar *default_config_dir = NULL;

  const char *xdg_config_home = g_getenv("XDG_CONFIG_HOME");
  gchar *homedir = dt_loc_get_home_dir(NULL);

  if(xdg_config_home) xdg_config_dir = g_strconcat(xdg_config_home, "/darktable", NULL);

  if(homedir)
  {
    default_config_dir = g_strconcat(homedir, "/.config/darktable", NULL);
    g_free(homedir);
  }

  darktable.configdir = dt_loc_init_generic(configdir, xdg_config_dir ? xdg_config_dir : default_config_dir);
  g_free(xdg_config_dir);
  g_free(default_config_dir);
}

#if defined(__MACH__) || defined(__APPLE__)
static char *find_install_dir(const char *suffix)
{
  gchar *curr = g_get_current_dir();
  int contains = 0;
  char tmp[PATH_MAX] = { 0 };
  for(int k = 0; darktable.progname[k] != 0; k++)
    if(darktable.progname[k] == '/')
    {
      contains = 1;
      break;
    }
  if(darktable.progname[0] == '/') // absolute path
    snprintf(tmp, sizeof(tmp), "%s", darktable.progname);
  else if(contains) // relative path
    snprintf(tmp, sizeof(tmp), "%s/%s", curr, darktable.progname);
  else
  {
    // no idea where we have been called. use compiled in path
    g_free(curr);
    return NULL;
  }
  size_t len = MIN(strlen(tmp), sizeof(tmp));
  char *t = tmp + len; // strip off bin/darktable
  for(; t > tmp && *t != '/'; t--)
    ;
  t--;
  if(*t == '.' && *(t - 1) != '.')
  {
    for(; t > tmp && *t != '/'; t--)
      ;
    t--;
  }
  for(; t > tmp && *t != '/'; t--)
    ;
  g_strlcpy(t, suffix, sizeof(tmp) - (t - tmp));
  g_free(curr);
  return g_strdup(tmp);
}
#endif
int dt_loc_init_tmp_dir(const char *tmpdir)
{
  darktable.tmpdir = dt_loc_init_generic(tmpdir, DARKTABLE_TMPDIR);
  if(darktable.tmpdir == NULL) return 1;
  return 0;
}
void dt_loc_init_user_cache_dir(const char *cachedir)
{
  gchar *xdg_cache_dir = NULL;
  const char *xdg_cache_home = g_getenv("XDG_CACHE_HOME");
  if(xdg_cache_home) xdg_cache_dir = g_strconcat(xdg_cache_home, "/darktable", NULL);
  darktable.cachedir = dt_loc_init_generic(cachedir, xdg_cache_dir ? xdg_cache_dir : DARKTABLE_CACHEDIR);
  g_free(xdg_cache_dir);
}

void dt_loc_init_plugindir(const char *plugindir)
{
#if defined(__MACH__) || defined(__APPLE__)
  char *directory = find_install_dir("/lib/darktable");
  if(plugindir || !directory)
  {
    darktable.plugindir = dt_loc_init_generic(plugindir, DARKTABLE_LIBDIR);
  }
  else
  {
    darktable.plugindir = directory;
  }
#else
  darktable.plugindir = dt_loc_init_generic(plugindir, DARKTABLE_LIBDIR);
#endif
}

void dt_loc_init_datadir(const char *datadir)
{
#if defined(__MACH__) || defined(__APPLE__)
  char *directory = find_install_dir("/share/darktable");
  if(datadir || !directory)
  {
    darktable.datadir = dt_loc_init_generic(datadir, DARKTABLE_DATADIR);
  }
  else
  {
    darktable.datadir = directory;
  }
#else
  darktable.datadir = dt_loc_init_generic(datadir, DARKTABLE_DATADIR);
#endif
}


void dt_loc_get_plugindir(char *plugindir, size_t bufsize)
{
  snprintf(plugindir, bufsize, "%s", darktable.plugindir);
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
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
