/*
    This file is part of darktable,
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

gboolean win_valid_duplicate_filename(const char *filename)
{
  // Windows only accepts generic wildcards for filename search
  // therefore we must filter out invalid duplicate filenames
  // valid filenames must have from 2 to 4 decimal digits between 
  // last "_" and second last "." (or no "_" for the primary version)

  gboolean valid_filename = TRUE;

  const gchar *c4 = filename + strlen(filename);
  while(*c4 != '.') c4--;
  c4--;
  while(*c4 != '.') c4--;
  const gchar *c3 = c4;
  gboolean underscore_found = FALSE; 
  while(!underscore_found && c3 > filename) 
  {
    c3--;
    underscore_found = (*c3 == '_');
  }
  if(underscore_found)
  {
    c3++;
    c4--;
    valid_filename = (c3 != c4);

    while((c3 <= c4) && valid_filename)
    {
      if(!( *c3 >= '0' && *c3 <= '9' )) valid_filename = FALSE;
      c3++;
    }
  
  }
  return valid_filename;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
