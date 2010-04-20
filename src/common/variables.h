/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson.

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

#ifndef VARIABLES_H
#define VARIABLES_H
#include <glib.h>
typedef struct dt_variables_params_t 
{
  gchar *source;
  gchar *result;
  
  /** only validate source */
  gboolean validate_only;
  
  /** Used for expanding variables that uses a image filename. */
  gchar *image_filename;
  /** Used for expanding variables that uses a image. */
  struct dt_image_t *img;
  time_t time;
  gchar *jobcode;
  
} dt_string_params_t;

/** expands variables in string into dest, size = size of dest*/
gboolean dt_variables_expand(dt_string_params_t *params);

#endif