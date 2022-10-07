/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.
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

#include "dtwin.h"

GList* win_image_find_duplicates(const char* filename)
{
  // find all duplicates of an image
  gchar pattern[PATH_MAX] = { 0 };
  GList* files = NULL;
  gchar *imgpath = g_path_get_dirname(filename);
  // Windows only accepts generic wildcards for filename
  static const gchar *glob_patterns[] = { "", "_????", NULL };
  const gchar *c3 = filename + strlen(filename);
  while(*c3 != '\\' && c3 > filename) c3--;
  if(*c3 == '\\') c3++;
  const gchar **glob_pattern = glob_patterns;
  files = NULL;
  while(*glob_pattern)
  {
    g_strlcpy(pattern, filename, sizeof(pattern));
    gchar *c1 = pattern + strlen(pattern);
    while(*c1 != '.' && c1 > pattern) c1--;
    g_strlcpy(c1, *glob_pattern, pattern + sizeof(pattern) - c1);
    const gchar *c2 = filename + strlen(filename);
    while(*c2 != '.' && c2 > filename) c2--;
    snprintf(c1 + strlen(*glob_pattern), pattern + sizeof(pattern) - c1 - strlen(*glob_pattern), "%s.xmp", c2);
    wchar_t *wpattern = g_utf8_to_utf16(pattern, -1, NULL, NULL, NULL);
    WIN32_FIND_DATAW data;
    HANDLE handle = FindFirstFileW(wpattern, &data);
    g_free(wpattern);
    gchar *imgfile_without_path=g_strndup(c3,c2-c3); /*Need to remove path from front of filename*/
    if(handle != INVALID_HANDLE_VALUE)
    {
      do
      {
        gchar *file = g_utf16_to_utf8(data.cFileName, -1, NULL, NULL, NULL);
        gchar *short_file_name = g_strndup(file, strlen(file) - 4 + c2 - filename - strlen(filename));
        gboolean valid_xmp_name = FALSE;
        if(!(valid_xmp_name = (strlen(short_file_name) == strlen(imgfile_without_path))))
        {
          // if not the same length, make sure the extra char are 2-4 digits preceded by '_'
          gchar *c4 = short_file_name + strlen(short_file_name);
          int i=0;
          do
          {
            c4--;
            i++;
          }
          while(g_ascii_isdigit(*c4) && c4 > short_file_name && i <= 4);
          valid_xmp_name = (*c4 == '_' && strlen(short_file_name) == strlen(imgfile_without_path) + i);
        }

        if(valid_xmp_name)
            files = g_list_append(files, g_build_filename(imgpath, file, NULL));

        g_free(short_file_name);
        g_free(file);
      }
      while(FindNextFileW(handle, &data));

    }

    g_free(imgfile_without_path);
    FindClose(handle);
    glob_pattern++;
  }

  g_free(imgpath);
  return files;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

