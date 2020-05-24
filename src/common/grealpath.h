/*
 This code is taken from http://git.gnome.org/browse/gobject-introspection/tree/giscanner/grealpath.h .
 According to http://git.gnome.org/browse/gobject-introspection/tree/COPYING it's licensed under the LGPLv2+.
*/

#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <glib.h>

/**
 * g_realpath:
 *
 * this should be a) filled in for win32 and b) put in glib...
 */

static inline gchar *g_realpath(const char *path)
{
#ifndef _WIN32
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
  char buffer[PATH_MAX] = { 0 };
  char* res = realpath(path, buffer);

  if(res) 
  {
    return g_strdup(buffer);
  }
  else 
  {
    if ( errno == EACCES ) 
    {
      printf("Read or search permission was denied for a component of file_name: %s\n", path);
    } 
    else if ( errno == EINVAL ) 
    {
      printf("Either the file_name or resolved_name argument is a null pointer: %s\n", path);
    } 
    else if ( errno == EIO ) 
    {
      printf("An error occurred while reading from the file system: %s\n", path);
    } 
    else if ( errno == ELOOP ) 
    {
      printf("Too many symbolic links were encountered in resolving path: %s\n", path);
    } 
    else if ( errno == ENAMETOOLONG ) 
    {
      printf("Pathname resolution of a symbolic link produced an intermediate result whose length exceeds {PATH_MAX}. The file_name argument is longer than {PATH_MAX} or a pathname component is longer than {NAME_MAX}: %s\n", path);
    } 
    else if ( errno == ENOENT ) 
    {
      printf("A component of file_name does not name an existing file or file_name points to an empty string: %s\n", path);
    } 
    else if ( errno == ENOTDIR ) 
    {
      printf("A component of the path prefix is not a directory: %s\n", path);
    } 
    else if ( errno == ENOMEM ) 
    {
      printf("Insufficient storage space is available.\n");
    }
    exit(EXIT_FAILURE);
  }
#else
  char *buffer;
  char dummy;
  int rc, len;

  rc = GetFullPathNameA(path, 1, &dummy, NULL);

  if(rc == 0)
  {
    /* Weird failure, so just return the input path as such */
    return g_strdup(path);
  }

  len = rc + 1;
  buffer = g_malloc(len);

  rc = GetFullPathNameA(path, len, buffer, NULL);

  if(rc == 0 || rc > len)
  {
    /* Weird failure again */
    g_free(buffer);
    return g_strdup(path);
  }

  return buffer;
#endif
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
