/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson, Tobias Ellinghaus.

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
#if defined _POSIX_C_SOURCE >= 1 || defined _XOPEN_SOURCE || defined _BSD_SOURCE || defined _SVID_SOURCE || defined _POSIX_SOURCE
#include <pwd.h>
#include <sys/types.h>
#endif

#include "utility.h"

guint dt_util_str_occurence(const gchar *haystack,const gchar *needle)
{
  guint o=0;
  if( haystack && needle )
  {
    const gchar *p=haystack;
    if( (p=g_strstr_len(p,strlen(p),needle)) != NULL)
    {
      do
      {
        o++;
      }
      while((p=g_strstr_len((p+1),strlen(p+1),needle)) != NULL);
    }
  }
  return o;
}

gchar* dt_util_str_replace(const gchar* string, const gchar* pattern, const gchar* substitute)
{
  gint occurences = dt_util_str_occurence(string, pattern);
  gchar* nstring;
  if(occurences)
  {
    nstring=g_malloc(strlen(string)+(occurences*strlen(substitute))+1);
    const gchar *pend=string+strlen(string);
    const gchar *s = string, *p = string;
    gchar *np = nstring;
    if((s=g_strstr_len(s,strlen(s),pattern)) != NULL)
    {
      do
      {
        memcpy(np,p,s-p);
        np+=(s-p);
        memcpy(np,substitute,strlen(substitute));
        np+=strlen(substitute);
        p=s+strlen(pattern);
      }
      while((s=g_strstr_len((s+1),strlen(s+1),pattern)) != NULL);
    }
    memcpy(np,p,pend-p);
    np[pend-p]='\0';
  }
  else
    nstring = g_strdup(string); // otherwise it's a hell to decide whether to free this string later.
  return nstring;
}

gchar* dt_util_glist_to_str(const gchar* separator, GList * items, const unsigned int count)
{
  if(count == 0)
    return NULL;

  gchar *result = NULL;

  // add the entries to an char* array
  items = g_list_first(items);
  gchar** strings = g_malloc(sizeof(gchar*) * (count+1));
  if(items != NULL)
  {
    int i = 0;
    do
    {
      strings[i++] = items->data;
    }
    while((items=g_list_next(items)) != NULL);
    strings[i] = NULL;
  }

  // join them into a single string
  result = g_strjoinv(separator, strings);

  // free the GList and the array
  items = g_list_first(items);
  if(items != NULL)
  {
    do
    {
      g_free(items->data);
    }
    while((items=g_list_next(items)) != NULL);
  }
  g_list_free(items);
  if(strings != NULL)
    g_free(strings);

  return result;
}

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

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
