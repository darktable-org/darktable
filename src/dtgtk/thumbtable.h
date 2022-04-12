/*
    This file is part of darktable,
    Copyright (C) 2019-2021 darktable developers.

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

// number of images per row in zoomable mode
#define DT_ZOOMABLE_NB_PER_ROW 13

typedef enum dt_thumbtable_mode_t
{
  DT_THUMBTABLE_MODE_NONE,
  DT_THUMBTABLE_MODE_FILEMANAGER,
  DT_THUMBTABLE_MODE_FILMSTRIP,
  DT_THUMBTABLE_MODE_ZOOM
} dt_thumbtable_mode_t;

typedef enum dt_thumbtable_move_t
{
  DT_THUMBTABLE_MOVE_NONE,
  DT_THUMBTABLE_MOVE_LEFT,
  DT_THUMBTABLE_MOVE_UP,
  DT_THUMBTABLE_MOVE_RIGHT,
  DT_THUMBTABLE_MOVE_DOWN,
  DT_THUMBTABLE_MOVE_PAGEUP,
  DT_THUMBTABLE_MOVE_PAGEDOWN,
  DT_THUMBTABLE_MOVE_START,
  DT_THUMBTABLE_MOVE_END,
  DT_THUMBTABLE_MOVE_ALIGN,
  DT_THUMBTABLE_MOVE_RESET_FIRST,
  DT_THUMBTABLE_MOVE_LEAVE
} dt_thumbtable_move_t;

typedef struct dt_thumbtable_t
{
  dt_thumbtable_mode_t mode;
  dt_thumbnail_overlay_t overlays;
  int overlays_block_timeout;
  gboolean show_tooltips;

  GtkWidget *widget; // GtkLayout -- main widget

  // list of thumbnails loaded inside main widget (dt_thumbnail_t)
  // for filmstrip and filemanager, this is all the images drawn at screen (even partially)
  // for zoommable, this is all the images in the row drawn at screen. We don't load laterals images on fly.
  GList *list;

  // rowid of the main shown image inside 'memory.collected_images'
  // for filmstrip this is the image in the center.
  // for zoomable, this is the top-left image (which can be out of screen)
  int offset;
  int offset_imgid;

  int thumbs_per_row; // number of image in a row (1 for filmstrip ; MAX_ZOOM for zoomable)
  int rows; // number of rows (the last one is not fully visible) for filmstrip it's the number of columns
  int thumb_size;              // demanded thumb size (real size can differ of 1 due to rounding)
  int prefs_size;              // size value to determine overlays mode and css class
  int view_width, view_height; // last main widget size
  GdkRectangle thumbs_area;    // coordinate of all the currently loaded thumbs area

  int center_offset; // in filemanager, we can have a gap, esp. for zoom==1, we need to center everything

  gboolean dragging;
  int last_x, last_y;         // last position of cursor during move
  int drag_dx, drag_dy;       // distance of move of the current dragging session
  dt_thumbnail_t *drag_thumb; // thumb currently dragged (under the mouse)

  // for some reasons, in filemanager, first image can not be at x=0
  // in that case, we count the number of "scroll-top" try and reallign after 2 try
  int realign_top_try;

  gboolean mouse_inside; // is the mouse pointer inside thumbtable widget ?
  gboolean key_inside;   // is the key move pointer inside thumbtable widget ?

  // when performing a drag, we store the list of items to drag here
  // as this can change during the drag and drop (esp. because of the image_over_id)
  GList *drag_list;

  // to deactivate scrollbars event because we have updated it by hand in the code
  gboolean code_scrolling;

  // are scrollbars shown ?
  gboolean scrollbars;

  // in lighttable preview or culling, we can navigate inside selection or inside full collection
  gboolean navigate_inside_selection;

  // let's remember previous thumbnail generation settings to detect if they change
  int pref_embedded;
  int pref_hq;
} dt_thumbtable_t;

dt_thumbtable_t *dt_thumbtable_new();
// reload all thumbs from scratch.
void dt_thumbtable_full_redraw(dt_thumbtable_t *table, gboolean force);
// change thumbtable parent widget
void dt_thumbtable_set_parent(dt_thumbtable_t *table, GtkWidget *new_parent, dt_thumbtable_mode_t mode);
// get/set offset (and redraw if needed)
int dt_thumbtable_get_offset(dt_thumbtable_t *table);
gboolean dt_thumbtable_set_offset(dt_thumbtable_t *table, int offset, gboolean redraw);
// set offset at specific imageid (and redraw if needed)
gboolean dt_thumbtable_set_offset_image(dt_thumbtable_t *table, int imgid, gboolean redraw);

// fired when the zoom level change
void dt_thumbtable_zoom_changed(dt_thumbtable_t *table, int oldzoom, int newzoom);

// ensure that the mentioned image is visible by moving the view if needed
gboolean dt_thumbtable_ensure_imgid_visibility(dt_thumbtable_t *table, int imgid);
// check if the mentioned image is visible
gboolean dt_thumbtable_check_imgid_visibility(dt_thumbtable_t *table, int imgid);

// drag & drop receive function - handles dropping of files in the center view (files are added to the library)
void dt_thumbtable_event_dnd_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection_data, guint target_type, guint time, gpointer user_data);

// move by key actions.
// this key accels are not managed here but inside view
gboolean dt_thumbtable_key_move(dt_thumbtable_t *table, dt_thumbtable_move_t move, gboolean select);

// ensure the first image in collection as no offset (is positioned on top-left)
gboolean dt_thumbtable_reset_first_offset(dt_thumbtable_t *table);

// scrollbar change
void dt_thumbtable_scrollbar_changed(dt_thumbtable_t *table, float x, float y);

// change the type of overlays that should be shown (over or under the image)
void dt_thumbtable_set_overlays_mode(dt_thumbtable_t *table, dt_thumbnail_overlay_t over);
// change the timeout of the overlays block
void dt_thumbtable_set_overlays_block_timeout(dt_thumbtable_t *table, const int timeout);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

