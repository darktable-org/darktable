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
#include "dtgtk/thumbnail.h"
#include <gtk/gtk.h>

// Accels are handle by filmstrip lib and repercuted here.
// as filmstrip is enabled for all view where thumbtable my be used, this works
// and this allow to reuse already in place lib accels routines
typedef enum dt_thumbtable_accels_t
{
  // rating MUST BE KEPT IN SYNC WITH dt_view_image_over_t
  DT_THUMBTABLE_ACCEL_RATE_0 = 0,
  DT_THUMBTABLE_ACCEL_RATE_1,
  DT_THUMBTABLE_ACCEL_RATE_2,
  DT_THUMBTABLE_ACCEL_RATE_3,
  DT_THUMBTABLE_ACCEL_RATE_4,
  DT_THUMBTABLE_ACCEL_RATE_5,
  DT_THUMBTABLE_ACCEL_REJECT,
  // history
  DT_THUMBTABLE_ACCEL_COPY,
  DT_THUMBTABLE_ACCEL_COPY_PARTS,
  DT_THUMBTABLE_ACCEL_PASTE,
  DT_THUMBTABLE_ACCEL_PASTE_PARTS,
  DT_THUMBTABLE_ACCEL_HIST_DISCARD,
  DT_THUMBTABLE_ACCEL_DUPLICATE,
  DT_THUMBTABLE_ACCEL_DUPLICATE_VIRGIN,
  // colorlabels
  DT_THUMBTABLE_ACCEL_COLOR_RED,
  DT_THUMBTABLE_ACCEL_COLOR_YELLOW,
  DT_THUMBTABLE_ACCEL_COLOR_GREEN,
  DT_THUMBTABLE_ACCEL_COLOR_BLUE,
  DT_THUMBTABLE_ACCEL_COLOR_PURPLE,
  DT_THUMBTABLE_ACCEL_COLOR_CLEAR,
  // selection
  DT_THUMBTABLE_ACCEL_SELECT_ALL,
  DT_THUMBTABLE_ACCEL_SELECT_NONE,
  DT_THUMBTABLE_ACCEL_SELECT_INVERT,
  DT_THUMBTABLE_ACCEL_SELECT_FILM,
  DT_THUMBTABLE_ACCEL_SELECT_UNTOUCHED
} dt_thumbtable_accels_t;

typedef enum dt_thumbtable_mode_t
{
  DT_THUMBTABLE_MODE_FILEMANAGER,
  DT_THUMBTABLE_MODE_FILMSTRIP,
  DT_THUMBTABLE_MODE_ZOOM
} dt_thumbtable_mode_t;

typedef struct dt_thumbtable_t
{
  dt_thumbtable_mode_t mode;

  GtkWidget *widget; // GtkLayout -- main widget

  // list of thumbnails loaded inside main widget (dt_thumbnail_t)
  // for filmstrip and filemanager, this is all the images drawn at screen (even partially)
  // for zoommable, this is all the images in the row drawn at screen. We don't load laterals images on fly.
  GList *list;

  // rowid of the main shown image inside 'memory.collected_images'
  // for filmstrip this is the image in the center.
  // for zoomable, this is the top-left image (which can be out of screen)
  int offset;

  int thumbs_per_row; // number of image in a row (1 for filmstrip ; MAX_ZOOM for zoomable)
  int rows; // number of rows (the last one is not fully visible) for filmstrip it's the number of columns
  int thumb_size;              // demanded thumb size (real size can differ of 1 due to rounding)
  int view_width, view_height; // last main widget size
  GdkRectangle thumbs_area;    // coordinate of all the currently loaded thumbs area

  gboolean dragging;
  int last_x, last_y;         // last position of cursor during move
  int drag_dx, drag_dy;       // distance of move of the current dragging session
  dt_thumbnail_t *drag_thumb; // thumb currently dragged (under the mouse)

  gboolean mouse_inside; // is the mouse pointer inside thumbatable widget ?
} dt_thumbtable_t;

dt_thumbtable_t *dt_thumbtable_new();
// reload all thumbs from scratch.
void dt_thumbtable_full_redraw(dt_thumbtable_t *table, gboolean force);
// change thumbtable parent widget
void dt_thumbtable_set_parent(dt_thumbtable_t *table, GtkWidget *new_parent, dt_thumbtable_mode_t mode);
// define if overlays should always be shown or just on mouse-over
void dt_thumbtable_set_overlays(dt_thumbtable_t *table, gboolean show);
// repercuted accel click (from filmstrip lib)
gboolean dt_thumbtable_accel_callback(dt_thumbtable_t *table, dt_thumbtable_accels_t accel);
// get images to act on for gloabals change (via libs or accels)
GList *dt_thumbtable_get_images_to_act_on(dt_thumbtable_t *table);
// get the main image to act on during global changes (libs, accels)
int dt_thumbtable_get_image_to_act_on(dt_thumbtable_t *table);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;