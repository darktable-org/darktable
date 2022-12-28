/*
    This file is part of darktable,
    copyright (c) 2020 Aldric Renaudin.

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
/** a class to manage a collection of zoomable thumbnails for culling or full preview.  */
#include "dtgtk/thumbnail.h"
#include <gtk/gtk.h>

typedef enum dt_culling_mode_t
{
  DT_CULLING_MODE_CULLING = 0, // classic culling mode
  DT_CULLING_MODE_PREVIEW      // full preview mode
} dt_culling_mode_t;

typedef enum dt_culling_move_t
{
  DT_CULLING_MOVE_NONE,
  DT_CULLING_MOVE_LEFT,
  DT_CULLING_MOVE_UP,
  DT_CULLING_MOVE_RIGHT,
  DT_CULLING_MOVE_DOWN,
  DT_CULLING_MOVE_PAGEUP,
  DT_CULLING_MOVE_PAGEDOWN,
  DT_CULLING_MOVE_START,
  DT_CULLING_MOVE_END
} dt_culling_move_t;

typedef struct dt_culling_t
{
  dt_culling_mode_t mode;

  GtkWidget *widget; // GtkLayout -- main widget

  // list of thumbnails loaded inside main widget (dt_thumbnail_t)
  GList *list;

  // rowid of the main shown image inside 'memory.collected_images'
  int offset;
  int offset_imgid;

  int thumbs_count;            // last nb of thumb to display
  int view_width, view_height; // last main widget size
  GdkRectangle thumbs_area;    // coordinate of all the currently loaded thumbs area

  gboolean navigate_inside_selection; // do we navigate inside selection or inside full collection
  gboolean selection_sync;            // should the selection follow current culling images

  gboolean select_desactivate;

  // the global zoom level of all images in the culling view.
  // scales images from 0 "image to fit" to 1 "100% zoom".
  float zoom_ratio;

  gboolean panning;      // are we moving zoomed images ?
  double pan_x;          // last position during panning
  double pan_y;          //
  gboolean mouse_inside; // is the mouse inside culling center view ?

  gboolean focus; // do we show focus rectangles on images ?

  dt_thumbnail_overlay_t overlays; // overlays type
  int overlays_block_timeout;      // overlay block visibility duration
  gboolean show_tooltips;          // are tooltips visible ?
} dt_culling_t;

dt_culling_t *dt_culling_new(dt_culling_mode_t mode);
// reload all thumbs from scratch.
void dt_culling_full_redraw(dt_culling_t *table, gboolean force);
// initialise culling offset/navigation mode, etc before entering.
// if offset is > 0 it'll be used as offset, otherwise offset will be determined by other means
void dt_culling_init(dt_culling_t *table, int offset);
// move by key actions.
// this key accels are not managed here but inside view
gboolean dt_culling_key_move(dt_culling_t *table, dt_culling_move_t move);

// change the offset imgid. This will recompute everything even if offset doesn't change
// because this may means that other images have changed
void dt_culling_change_offset_image(dt_culling_t *table, int offset);

void dt_culling_zoom_max(dt_culling_t *table);
void dt_culling_zoom_fit(dt_culling_t *table);

// set the overlays type
void dt_culling_set_overlays_mode(dt_culling_t *table, dt_thumbnail_overlay_t over);
void dt_culling_force_overlay(dt_culling_t *table, const gboolean force);

// update active images list
void dt_culling_update_active_images_list(dt_culling_t *table);
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

