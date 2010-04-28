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
    
  /** only validates string */
  gboolean validate_only;
  
  /** The offset for sequencenumber */
  guint sequence_offset;
  
  /** Used for expanding variables that uses filename $(FILE_NAME) and $(FILE_EXTENSION). */
  const gchar *filename;
  
  /** Used for expanding variable $(JOBCODE) */
  const gchar *jobcode;

  /** Used for expanding variables such as $(IMAGE_WIDTH) $(IMAGE_HEIGT). */
  struct dt_image_t *img;

  /** Internal variables data */	
  struct dt_variables_data_t *data;
  
} dt_variables_params_t;

/** allocate and init params */
void dt_variables_params_init(dt_variables_params_t **params);
void dt_variables_params_destroy(dt_variables_params_t *params);

/** expands variables in string */
gboolean dt_variables_expand(dt_variables_params_t *params, gchar *string, gboolean iterate);
/** get the expanded string result*/
const gchar *dt_variables_get_result(dt_variables_params_t *params);
#endif