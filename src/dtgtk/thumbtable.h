/*
    This file is part of darktable,
    copyright (c) 2019--2020 Aldric Renaudin.

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
/** a class to manage a table of thumbnail for lighttable and filmstrip.  */
#include <gtk/gtk.h>

typedef enum dt_thumbtable_mode_t
{
  DT_THUMBTABLE_MODE_FILEMANAGER,
  DT_THUMBTABLE_MODE_FILMSTRIP
} dt_thumbtable_mode_t;

typedef struct dt_thumbtable_t
{
  dt_thumbtable_mode_t mode;

  GtkWidget *widget; // main widget

  GList *list; // list of thumbnails struct drawn at screen

  int offset; // index of the first shown image

  int thumbs_per_row;          // number of image in a row
  int rows;                    // number of rows (the last one is not fully visible)
  int thumb_size;              // demanded thumb size (real size can differ of 1 due to rounding)
  int view_width, view_height; // last view size

} dt_thumbtable_t;

dt_thumbtable_t *dt_thumbtable_new();

void dt_thumbtable_full_redraw(dt_thumbtable_t *table, gboolean force);

void dt_thumbtable_set_parent(dt_thumbtable_t *table, GtkWidget *new_parent, dt_thumbtable_mode_t mode);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;