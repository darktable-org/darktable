/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011--2012 Henrik Andersson.
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
/** this is the view for the lighttable module.  */
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/focus.h"
#include "common/grouping.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/settings.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "views/view.h"
#include "views/view_api.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

DT_MODULE(1)

static gboolean star_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier, gpointer data);
static gboolean go_up_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                         GdkModifierType modifier, gpointer data);
static gboolean go_down_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data);
static gboolean go_pgup_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data);
static gboolean go_pgdown_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data);

static void _update_collected_images(dt_view_t *self);

static gboolean _is_custom_image_order_required();

/**
 * this organises the whole library:
 * previously imported film rolls..
 */
typedef struct dt_library_t
{
  // tmp mouse vars:
  float select_offset_x, select_offset_y;
  int32_t last_selected_idx, selection_origin_idx;
  int button;
  int key_jump_offset;
  int using_arrows;
  int key_select;
  int key_select_direction;
  int layout;
  uint32_t modifiers;
  uint32_t center, pan;
  int32_t track, offset, first_visible_zoomable, first_visible_filemanager;
  float zoom_x, zoom_y;
  dt_view_image_over_t image_over;
  int full_preview;
  int full_preview_sticky;
  int32_t full_preview_id;
  int32_t full_preview_rowid;
  int display_focus;
  gboolean offset_changed;
  int images_in_row;
  int max_rows;

  uint8_t *full_res_thumb;
  int32_t full_res_thumb_id, full_res_thumb_wd, full_res_thumb_ht;
  dt_image_orientation_t full_res_thumb_orientation;
  dt_focus_cluster_t full_res_focus[49];

  int32_t last_mouse_over_id;

  int32_t collection_count;

  // stuff for the audio player
  GPid audio_player_pid;   // the pid of the child process
  int32_t audio_player_id; // the imgid of the image the audio is played for
  guint audio_player_event_source;

  /* prepared and reusable statements */
  struct
  {
    /* main query statement, should be update on listener signal of collection */
    sqlite3_stmt *main_query;
    /* select imgid from selected_images */
    sqlite3_stmt *select_imgid_in_selection;
    /* delete from selected_images where imgid != ?1 */
    sqlite3_stmt *delete_except_arg;
    /* check if the group of the image under the mouse has others, too, ?1: group_id, ?2: imgid */
    sqlite3_stmt *is_grouped;
  } statements;

  GtkWidget *profile_floating_window;

} dt_library_t;

/* drag and drop callbacks to reorder picture sequence (dnd)*/

static void _dnd_get_picture_reorder(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                    GtkSelectionData *selection_data, guint target_type, guint time,
                                    gpointer data);
static void _dnd_begin_picture_reorder(GtkWidget *widget, GdkDragContext *context, gpointer user_data);

static gboolean _dnd_drag_picture_motion(GtkWidget *dest_button, GdkDragContext *dc, gint x, gint y, guint time, gpointer user_data);

static void _register_custom_image_order_drag_n_drop(dt_view_t *self);
static void _unregister_custom_image_order_drag_n_drop(dt_view_t *self);

static void _stop_audio(dt_library_t *lib);

const char *name(dt_view_t *self)
{
  return _("lighttable");
}


uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

typedef enum dt_lighttable_direction_t {
  DIRECTION_UP = 0,
  DIRECTION_DOWN = 1,
  DIRECTION_LEFT = 2,
  DIRECTION_RIGHT = 3,
  DIRECTION_ZOOM_IN = 4,
  DIRECTION_ZOOM_OUT = 5,
  DIRECTION_TOP = 6,
  DIRECTION_BOTTOM = 7,
  DIRECTION_PGUP = 8,
  DIRECTION_PGDOWN = 9,
  DIRECTION_CENTER = 10
} dt_lighttable_direction_t;

static void switch_layout_to(dt_library_t *lib, int new_layout)
{
  lib->layout = new_layout;

  if(new_layout == 1) // filemanager
  {
    if(lib->first_visible_zoomable >= 0)
    {
      lib->offset = lib->first_visible_zoomable;
    }
    lib->first_visible_zoomable = 0;

    if(lib->center) lib->offset = 0;
    lib->center = 0;

    lib->offset_changed = TRUE;
  }
}

static void move_view(dt_library_t *lib, dt_lighttable_direction_t dir)
{
  const int iir = dt_conf_get_int("plugins/lighttable/images_in_row");

  switch(dir)
  {
    case DIRECTION_UP:
    {
      if(lib->offset >= 1) lib->offset = lib->offset - iir;
    }
    break;
    case DIRECTION_DOWN:
    {
      lib->offset = lib->offset + iir;
      while(lib->offset >= lib->collection_count) lib->offset -= iir;
    }
    break;
    case DIRECTION_PGUP:
    {
      lib->offset -= (lib->max_rows - 1) * iir;
      while(lib->offset < 0) lib->offset += iir;
    }
    break;
    case DIRECTION_PGDOWN:
    {
      lib->offset += (lib->max_rows - 1) * iir;
      while(lib->offset >= lib->collection_count) lib->offset -= iir;
    }
    break;
    case DIRECTION_TOP:
    {
      lib->offset = 0;
    }
    break;
    case DIRECTION_BOTTOM:
    {
      lib->offset = lib->collection_count - iir;
    }
    break;
    case DIRECTION_CENTER:
    {
      lib->offset -= lib->offset % iir;
    }
    break;
    default:
      break;
  }

  lib->first_visible_filemanager = lib->offset;
  lib->offset_changed = TRUE;
}

/* This function allows the file manager view to zoom "around" the image
 * currently under the mouse cursor, instead of around the top left image */
static void zoom_around_image(dt_library_t *lib, double pointerx, double pointery, int width, int height,
                              int old_images_in_row, int new_images_in_row)
{
  /* calculate which image number (relative to total collection)
   * is currently under the cursor, i.e. which image is the zoom anchor */
  float wd = width / (float)old_images_in_row;
  float ht = width / (float)old_images_in_row;
  int pi = pointerx / (float)wd;
  int pj = pointery / (float)ht;

  int zoom_anchor_image = lib->offset + pi + (pj * old_images_in_row);

  // make sure that we don't try to zoom around an image that doesn't exist
  if(zoom_anchor_image > lib->collection_count) zoom_anchor_image = lib->collection_count;

  // make sure that we don't try to zoom around an image that doesn't exist
  if(zoom_anchor_image < 0) zoom_anchor_image = 0;

  /* calculate which image number (relative to offset) will be
   * under the cursor after zooming. Then subtract that value
   * from the zoom anchor image number to see what the new offset should be */
  wd = width / (float)new_images_in_row;
  ht = width / (float)new_images_in_row;
  pi = pointerx / (float)wd;
  pj = pointery / (float)ht;

  lib->offset = zoom_anchor_image - pi - (pj * new_images_in_row);
  lib->first_visible_filemanager = lib->offset;
  lib->offset_changed = TRUE;
  lib->images_in_row = new_images_in_row;
}

static void _view_lighttable_collection_listener_callback(gpointer instance, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  _unregister_custom_image_order_drag_n_drop(self);
  _register_custom_image_order_drag_n_drop(self);

  _update_collected_images(self);
}

static void _update_collected_images(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  sqlite3_stmt *stmt;
  int32_t min_before = 0, min_after = 0;

  /* check if we can get a query from collection */
  gchar *query = g_strdup(dt_collection_get_query(darktable.collection));
  if(!query) return;

  // we have a new query for the collection of images to display. For speed reason we collect all images into
  // a temporary (in-memory) table (collected_images).
  //
  // 0. get current lower rowid
  if (lib->full_preview_id != -1)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT MIN(rowid) FROM memory.collected_images",
                                -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      min_before = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }

  // 1. drop previous data

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.collected_images", NULL, NULL,
                        NULL);
  // reset autoincrement. need in star_key_accel_callback
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.sqlite_sequence WHERE "
                                                       "name='collected_images'", NULL, NULL, NULL);

  // 2. insert collected images into the temporary table

  gchar *ins_query = NULL;
  ins_query = dt_util_dstrcat(ins_query, "INSERT INTO memory.collected_images (imgid) %s", query);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), ins_query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  g_free(query);
  g_free(ins_query);

  // 3. get new low-bound, then update the full preview rowid accordingly
  if (lib->full_preview_id != -1)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT MIN(rowid) FROM memory.collected_images",
                                -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      min_after = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    // note that this adjustement is needed as for a memory table the rowid doesn't start to 1 after the DELETE
    // above,
    // but rowid is incremented each time we INSERT.
    lib->full_preview_rowid += (min_after - min_before);

    char col_query[128] = { 0 };
    snprintf(col_query, sizeof(col_query), "SELECT imgid FROM memory.collected_images WHERE rowid=%d", lib->full_preview_rowid);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), col_query, -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int nid = sqlite3_column_int(stmt, 0);
      if (nid != lib->full_preview_id)
      {
        lib->full_preview_id = sqlite3_column_int(stmt, 0);
        dt_control_set_mouse_over_id(lib->full_preview_id);
      }
    }
    sqlite3_finalize(stmt);
  }

  /* if we have a statement lets clean it */
  if(lib->statements.main_query) sqlite3_finalize(lib->statements.main_query);

  /* prepare a new main query statement for collection */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM memory.collected_images ORDER BY rowid LIMIT ?1, ?2", -1,
                              &lib->statements.main_query, NULL);

  dt_control_queue_redraw_center();
}

static void _set_position(dt_view_t *self, uint32_t pos)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->first_visible_filemanager = lib->first_visible_zoomable = lib->offset = pos;
  lib->offset_changed = TRUE;
  dt_control_queue_redraw_center();
}

static uint32_t _get_position(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(lib->layout == 1)
    return MAX(0, lib->first_visible_filemanager);
  else
    return MAX(0, lib->first_visible_zoomable);
}

static int _get_images_in_row(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  return lib->images_in_row;
}

static int _get_full_preview_id(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  return lib->full_preview_id;
}

void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_library_t));
  dt_library_t *lib = (dt_library_t *)self->data;

  darktable.view_manager->proxy.lighttable.set_position = _set_position;
  darktable.view_manager->proxy.lighttable.get_position = _get_position;
  darktable.view_manager->proxy.lighttable.get_images_in_row = _get_images_in_row;
  darktable.view_manager->proxy.lighttable.get_full_preview_id = _get_full_preview_id;
  darktable.view_manager->proxy.lighttable.view = self;

  lib->select_offset_x = lib->select_offset_y = 0.5f;
  lib->last_selected_idx = -1;
  lib->selection_origin_idx = -1;
  lib->key_jump_offset = 0;
  lib->first_visible_zoomable = -1;
  lib->first_visible_filemanager = -1;
  lib->button = 0;
  lib->modifiers = 0;
  lib->center = lib->pan = lib->track = 0;
  lib->zoom_x = dt_conf_get_float("lighttable/ui/zoom_x");
  lib->zoom_y = dt_conf_get_float("lighttable/ui/zoom_y");
  lib->full_preview = 0;
  lib->full_preview_id = -1;
  lib->display_focus = 0;
  lib->last_mouse_over_id = -1;
  lib->full_res_thumb = 0;
  lib->full_res_thumb_id = -1;
  lib->audio_player_id = -1;

  /* setup collection listener and initialize main_query statement */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_view_lighttable_collection_listener_callback), (gpointer)self);

  _view_lighttable_collection_listener_callback(NULL, self);

  /* initialize reusable sql statements */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.selected_images WHERE imgid != ?1",
                              -1, &lib->statements.delete_except_arg, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM main.images WHERE group_id = ?1 AND id != ?2", -1,
                              &lib->statements.is_grouped, NULL); // TODO: only check in displayed images?
}


void cleanup(dt_view_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_view_lighttable_collection_listener_callback), self);

  dt_library_t *lib = (dt_library_t *)self->data;
  dt_conf_set_float("lighttable/ui/zoom_x", lib->zoom_x);
  dt_conf_set_float("lighttable/ui/zoom_y", lib->zoom_y);
  if(lib->audio_player_id != -1) _stop_audio(lib);
  free(lib->full_res_thumb);
  free(self->data);
}

/**
 * \brief A helper function to convert grid coordinates to an absolute index
 *
 * \param[in] row The row
 * \param[in] col The column
 * \param[in] stride The stride (number of columns per row)
 * \param[in] offset The zero-based index of the top-left image (aka the count of images above the viewport,
 *minus 1)
 * \return The absolute, zero-based index of the specified grid location
 */

#if 0
static int
grid_to_index (int row, int col, int stride, int offset)
{
  return row * stride + col + offset;
}
#endif

static int expose_filemanager(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                               int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  gboolean offset_changed = FALSE;
  int missing = 0;

  /* query new collection count */
  lib->collection_count = dt_collection_get_count(darktable.collection);

  if(darktable.gui->center_tooltip == 1) darktable.gui->center_tooltip = 2;

  /* get grid stride */
  const int iir = dt_conf_get_int("plugins/lighttable/images_in_row");
  lib->images_in_row = iir;

  /* get image over id */
  lib->image_over = DT_VIEW_DESERT;
  int32_t mouse_over_id = dt_control_get_mouse_over_id(), mouse_over_group = -1;

  /* fill background */
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
  cairo_paint(cr);

  offset_changed = lib->offset_changed;

  const float wd = width / (float)iir;
  const float ht = width / (float)iir;

  int pi = pointerx / (float)wd;
  int pj = pointery / (float)ht;
  if(pointerx < 0 || pointery < 0) pi = pj = -1;
  // const int pidx = grid_to_index(pj, pi, iir, offset);

  const int img_pointerx = iir == 1 ? pointerx : fmodf(pointerx, wd);
  const int img_pointery = iir == 1 ? pointery : fmodf(pointery, ht);

  const int max_rows = 1 + (int)((height) / ht + .5);
  lib->max_rows = max_rows;
  const int max_cols = iir;

  int id;

  /* get the count of current collection */

  if(lib->collection_count == 0)
  {
    const float fs = DT_PIXEL_APPLY_DPI(15.0f);
    const float ls = 1.5f * fs;
    const float offy = height * 0.2f;
    const float offx = DT_PIXEL_APPLY_DPI(60);
    const float at = 0.3f;
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_absolute_size(desc, fs * PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);
    cairo_set_font_size(cr, fs);
    cairo_set_source_rgba(cr, .7, .7, .7, 1.0f);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    pango_layout_set_text(layout, _("there are no images in this collection"), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, offx, offy - ink.height - ink.x);
    pango_cairo_show_layout(cr, layout);
    pango_layout_set_text(layout, _("if you have not imported any images yet"), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, offx, offy + 2 * ls - ink.height - ink.x);
    pango_cairo_show_layout(cr, layout);
    pango_layout_set_text(layout, _("you can do so in the import module"), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, offx, offy + 3 * ls - ink.height - ink.x);
    pango_cairo_show_layout(cr, layout);
    cairo_move_to(cr, offx - DT_PIXEL_APPLY_DPI(10.0f), offy + 3 * ls - ls * .25f);
    cairo_line_to(cr, 0.0f, 10.0f);
    cairo_set_source_rgba(cr, .7, .7, .7, at);
    cairo_stroke(cr);
    pango_layout_set_text(layout, _("try to relax the filter settings in the top panel"), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, offx, offy + 5 * ls - ink.height - ink.x);
    cairo_set_source_rgba(cr, .7, .7, .7, 1.0f);
    pango_cairo_show_layout(cr, layout);
    cairo_rel_move_to(cr, 10.0f + ink.width, ink.height * 0.5f);
    cairo_line_to(cr, width * 0.5f, 0.0f);
    cairo_set_source_rgba(cr, .7, .7, .7, at);
    cairo_stroke(cr);
    pango_layout_set_text(layout, _("or add images in the collection module in the left panel"), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, offx, offy + 6 * ls - ink.height - ink.x);
    cairo_set_source_rgba(cr, .7, .7, .7, 1.0f);
    pango_cairo_show_layout(cr, layout);
    cairo_move_to(cr, offx - DT_PIXEL_APPLY_DPI(10.0f), offy + 6 * ls - ls * 0.25f);
    cairo_rel_line_to(cr, -offx + 10.0f, 0.0f);
    cairo_set_source_rgba(cr, .7, .7, .7, at);
    cairo_stroke(cr);

    pango_font_description_free(desc);
    g_object_unref(layout);
    return 0;
  }

  /* do we have a main query collection statement */
  if(!lib->statements.main_query) return 0;

  /* safety check added to be able to work with zoom slider. The
  * communication between zoom slider and lighttable should be handled
  * differently (i.e. this is a clumsy workaround) */
  if(lib->images_in_row != iir && lib->first_visible_filemanager < 0)
    lib->offset = lib->first_visible_filemanager = 0;

  int32_t offset = lib->offset
      = MIN(lib->first_visible_filemanager, ((lib->collection_count + iir - 1) / iir - 1) * iir);

  int32_t drawing_offset = 0;
  if(offset < 0)
  {
    drawing_offset = offset;
    offset = 0;
  }

  /* update scroll borders */
  int shown_rows = ceilf((float)lib->collection_count / iir);
  if(iir > 1) shown_rows += max_rows - 2;
  dt_view_set_scrollbar(self, 0, 0, 1, 1, offset, 0, shown_rows * iir, (max_rows - 1) * iir);

  /* let's reset and reuse the main_query statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.main_query);
  DT_DEBUG_SQLITE3_RESET(lib->statements.main_query);

  /* setup offset and row for the main query */
  DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 1, offset);
  DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 2, max_rows * iir);

  if(mouse_over_id != -1)
  {
    const dt_image_t *mouse_over_image = dt_image_cache_get(darktable.image_cache, mouse_over_id, 'r');
    mouse_over_group = mouse_over_image->group_id;
    dt_image_cache_read_release(darktable.image_cache, mouse_over_image);
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.is_grouped);
    DT_DEBUG_SQLITE3_RESET(lib->statements.is_grouped);
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.is_grouped, 1, mouse_over_group);
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.is_grouped, 2, mouse_over_id);
    if(sqlite3_step(lib->statements.is_grouped) != SQLITE_ROW) mouse_over_group = -1;
  }

  // prefetch the ids so that we can peek into the future to see if there are adjacent images in the same
  // group.
  int *query_ids = (int *)calloc(max_rows * max_cols, sizeof(int));
  if(!query_ids) goto after_drawing;
  for(int row = 0; row < max_rows; row++)
  {
    for(int col = 0; col < max_cols; col++)
    {
      if(sqlite3_step(lib->statements.main_query) == SQLITE_ROW)
        query_ids[row * iir + col] = sqlite3_column_int(lib->statements.main_query, 0);
      else
        goto end_query_cache;
    }
  }

end_query_cache:
  mouse_over_id = -1;
  cairo_save(cr);
  int current_image = 0;
  int before_mouse_over_id = 0;

  if (lib->using_arrows)
  {
    before_mouse_over_id = dt_control_get_mouse_over_id();
  }
  for(int row = 0; row < max_rows; row++)
  {
    for(int col = 0; col < max_cols; col++)
    {
      // curidx = grid_to_index(row, col, iir, offset);

      /* skip drawing images until we reach a non-negative offset.
       * This is needed for zooming, so that the image under the
       * mouse cursor can stay there. */
      if(drawing_offset < 0)
      {
        drawing_offset++;
        cairo_translate(cr, wd, 0.0f);
        continue;
      }

      id = query_ids[current_image];
      current_image++;

      if(id > 0)
      {
        if(iir == 1 && row) continue;

        /* set mouse over id if pointer is in current row / col */
        if (lib->using_arrows)
        {

          if(before_mouse_over_id == -1)
          {
            // mouse has never been in filemanager area set mouse on first image and ignore this movement
            before_mouse_over_id = query_ids[0];
          }
          if(before_mouse_over_id == id)
          {
            // I would like to jump from before_mouse_over_id to query_ids[idx]
            int idx = current_image+lib->key_jump_offset-1;
            int current_row = (int)((current_image-1)/iir);
            int current_col = current_image%iir;

            // detect if the current movement need some extra movement (page adjust)
            if (current_row  == (int)(max_rows-1.5) && lib->key_jump_offset == iir)
              // going DOWN from last row
              move_view(lib, DIRECTION_DOWN);
            else if (current_row  == 0 && lib->key_jump_offset == iir*-1)
            {
              // going UP from first row
              move_view(lib, DIRECTION_UP);
            }
            else if (current_row == (int)(max_rows-1.5) && current_col ==  0 && lib->key_jump_offset == 1)
              // going RIGHT from last visible
              move_view(lib, DIRECTION_DOWN);
            else if (current_row == 0 && current_col ==  1 && lib->key_jump_offset == -1)
              // going LEFT from first visible
              move_view(lib, DIRECTION_UP);
            if (idx > -1 && idx < lib->collection_count && query_ids[idx])
            {
                // offset is valid..we know where to jump
                mouse_over_id = query_ids[idx];
            }
            else
              // going into a non existing position. Do nothing
              mouse_over_id = before_mouse_over_id;

            if (lib->key_jump_offset != 0)
            {
              if (lib->key_select)
              {
                // managing shift + movement
                int direction = (lib->key_jump_offset > 0) ? DIRECTION_RIGHT : DIRECTION_LEFT;
                if (lib->key_select_direction != direction)
                {
                  lib->key_select_direction = direction;
                  dt_selection_toggle(darktable.selection, before_mouse_over_id);
                }
                int loop_count = abs(lib->key_jump_offset); // ex: from -10 to 1  // from 10 to 1
                int to_toggle = 0;
                while (loop_count--)
                {
                  // ex shift + down toggle selection on images_in_row images
                  to_toggle =  idx+(-1*lib->key_jump_offset/abs(lib->key_jump_offset)*loop_count);
                  if (query_ids[to_toggle])
                    dt_selection_toggle(darktable.selection, query_ids[to_toggle]);
                }
              }
              lib->key_jump_offset = 0; // avoid key_release events move cursor. TBD: return the right value in key_pressed and trash the flag
            }
          }
        }
        else if(pi == col && pj == row)
        {
          mouse_over_id = id;
        }

        cairo_save(cr);
        // if(iir == 1) dt_image_prefetch(image, DT_IMAGE_MIPF);
        if(iir == 1)
        {
          // we are on the single-image display at a time, in this case we want the selection to be updated to
          // contain
          // this single image.
          dt_selection_select_single(darktable.selection, id);
        }
        missing += dt_view_image_expose(&(lib->image_over), id, cr, wd, iir == 1 ? height : ht, iir, img_pointerx,
                             img_pointery, FALSE, FALSE);

        cairo_restore(cr);
      }
      else
        goto escape_image_loop;

      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols * wd, ht);
  }
escape_image_loop:
  cairo_restore(cr);
  if(!lib->pan && (iir != 1 || mouse_over_id != -1)) dt_control_set_mouse_over_id(mouse_over_id);

  // and now the group borders
  cairo_save(cr);
  current_image = 0;
  if(lib->offset < 0)
  {
    drawing_offset = lib->offset;
    offset = 0;
  }
  for(int row = 0; row < max_rows; row++)
  {
    for(int col = 0; col < max_cols; col++)
    {
      /* skip drawing images until we reach a non-negative offset.
       * This is needed for zooming, so that the image under the
       * mouse cursor can be stay there. */
      if(drawing_offset < 0)
      {
        drawing_offset++;
        cairo_translate(cr, wd, 0.0f);
        continue;
      }

      id = query_ids[current_image];


      if(id > 0)
      {
        const dt_image_t *image = dt_image_cache_get(darktable.image_cache, id, 'r');
        int group_id = -1;
        if(image) group_id = image->group_id;
        dt_image_cache_read_release(darktable.image_cache, image);

        if(iir == 1 && row) continue;

        cairo_save(cr);

        gboolean paint_border = FALSE;
        // regular highlight border
        if(group_id != -1)
        {
          if(mouse_over_group == group_id && iir > 1
             && ((!darktable.gui->grouping && dt_conf_get_bool("plugins/lighttable/draw_group_borders"))
                 || group_id == darktable.gui->expanded_group_id))
          {
            cairo_set_source_rgb(cr, 1, 0.8, 0);
            paint_border = TRUE;
          }
          // border of expanded group
          else if(darktable.gui->grouping && group_id == darktable.gui->expanded_group_id && iir > 1)
          {
            cairo_set_source_rgb(cr, 0, 0, 1);
            paint_border = TRUE;
          }
        }

        if(paint_border)
        {
          int neighbour_group = -1;
          // top border
          if(row > 0 && ((current_image - iir) >= 0))
          {
            int _id = query_ids[current_image - iir];
            if(_id > 0)
            {
              const dt_image_t *_img = dt_image_cache_get(darktable.image_cache, _id, 'r');
              neighbour_group = _img->group_id;
              dt_image_cache_read_release(darktable.image_cache, _img);
            }
          }
          if(neighbour_group != group_id)
          {
            cairo_move_to(cr, 0, 0);
            cairo_line_to(cr, wd, 0);
          }
          // left border
          neighbour_group = -1;
          if(col > 0 && current_image > 0)
          {
            int _id = query_ids[current_image - 1];
            if(_id > 0)
            {
              const dt_image_t *_img = dt_image_cache_get(darktable.image_cache, _id, 'r');
              neighbour_group = _img->group_id;
              dt_image_cache_read_release(darktable.image_cache, _img);
            }
          }
          if(neighbour_group != group_id)
          {
            cairo_move_to(cr, 0, 0);
            cairo_line_to(cr, 0, ht);
          }
          // bottom border
          neighbour_group = -1;
          if(row < max_rows - 1)
          {
            int _id = query_ids[current_image + iir];
            if(_id > 0)
            {
              const dt_image_t *_img = dt_image_cache_get(darktable.image_cache, _id, 'r');
              neighbour_group = _img->group_id;
              dt_image_cache_read_release(darktable.image_cache, _img);
            }
          }
          if(neighbour_group != group_id)
          {
            cairo_move_to(cr, 0, ht);
            cairo_line_to(cr, wd, ht);
          }
          // right border
          neighbour_group = -1;
          if(col < max_cols - 1)
          {
            int _id = query_ids[current_image + 1];
            if(_id > 0)
            {
              const dt_image_t *_img = dt_image_cache_get(darktable.image_cache, _id, 'r');
              neighbour_group = _img->group_id;
              dt_image_cache_read_release(darktable.image_cache, _img);
            }
          }
          if(neighbour_group != group_id)
          {
            cairo_move_to(cr, wd, 0);
            cairo_line_to(cr, wd, ht);
          }
          cairo_set_line_width(cr, 0.01 * wd);
          cairo_stroke(cr);
        }

        cairo_restore(cr);
        current_image++;
      }
      else
        goto escape_border_loop;

      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols * wd, ht);
  }
escape_border_loop:
  cairo_restore(cr);
after_drawing:
  /* check if offset was changed and we need to prefetch thumbs */
  if(offset_changed)
  {
    int32_t imgids_num = 0;
    const int prefetchrows = .5 * max_rows + 1;
    int32_t *imgids = malloc(prefetchrows * iir * sizeof(int32_t));

    /* clear and reset main query */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.main_query);
    DT_DEBUG_SQLITE3_RESET(lib->statements.main_query);

    /* setup offest and row for prefetch */
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 1, offset + max_rows * iir);
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 2, prefetchrows * iir);

    // prefetch jobs in inverse order: supersede previous jobs: most important last
    while(sqlite3_step(lib->statements.main_query) == SQLITE_ROW && imgids_num < prefetchrows * iir)
      imgids[imgids_num++] = sqlite3_column_int(lib->statements.main_query, 0);

    float imgwd = iir == 1 ? 0.97 : 0.8;
    dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, imgwd * wd,
                                                             imgwd * (iir == 1 ? height : ht));
    while(imgids_num > 0)
    {
      imgids_num--;
      dt_mipmap_cache_get(darktable.mipmap_cache, NULL, imgids[imgids_num], mip, DT_MIPMAP_PREFETCH, 'r');
    }

    free(imgids);
  }

  lib->offset_changed = FALSE;

  free(query_ids);
  // oldpan = pan;
  if(darktable.unmuted & DT_DEBUG_CACHE) dt_mipmap_cache_print(darktable.mipmap_cache);

  if(darktable.gui->center_tooltip == 1) // set in this round
  {
    char *tooltip = dt_history_get_items_as_string(mouse_over_id);
    if(tooltip != NULL)
    {
      gtk_widget_set_tooltip_text(dt_ui_center(darktable.gui->ui), tooltip);
      g_free(tooltip);
    }
  }
  else if(darktable.gui->center_tooltip == 2) // not set in this round
  {
    darktable.gui->center_tooltip = 0;
    gtk_widget_set_tooltip_text(dt_ui_center(darktable.gui->ui), "");
  }
  return missing;
}


// TODO: this is also defined in lib/tools/lighttable.c
//       fix so this value is shared.. DT_CTL_SET maybe ?

#define DT_LIBRARY_MAX_ZOOM 13

static int expose_zoomable(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                            int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  float zoom, zoom_x, zoom_y;
  int32_t mouse_over_id, pan, track, center;
  int missing = 0;
  /* query new collection count */
  lib->collection_count = dt_collection_get_count(darktable.collection);

  mouse_over_id = dt_control_get_mouse_over_id();
  zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
  zoom_x = lib->zoom_x;
  zoom_y = lib->zoom_y;
  pan = lib->pan;
  center = lib->center;
  track = lib->track;

  lib->images_in_row = zoom;
  lib->image_over = DT_VIEW_DESERT;

  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
  cairo_paint(cr);

  const float wd = width / zoom;
  const float ht = width / zoom;

  static float oldzoom = -1;
  if(oldzoom < 0) oldzoom = zoom;

  // TODO: exaggerate mouse gestures to pan when zoom == 1
  if(pan) // && mouse_over_id >= 0)
  {
    zoom_x = lib->select_offset_x - /* (zoom == 1 ? 2. : 1.)*/ pointerx;
    zoom_y = lib->select_offset_y - /* (zoom == 1 ? 2. : 1.)*/ pointery;
  }

  if(!lib->statements.main_query) return 0;

  if(track == 0)
    ;
  else if(track > 1)
    zoom_y += ht;
  else if(track > 0)
    zoom_x += wd;
  else if(track > -2)
    zoom_x -= wd;
  else
    zoom_y -= ht;
  if(zoom > DT_LIBRARY_MAX_ZOOM)
  {
    // double speed.
    if(track == 0)
      ;
    else if(track > 1)
      zoom_y += ht;
    else if(track > 0)
      zoom_x += wd;
    else if(track > -2)
      zoom_x -= wd;
    else
      zoom_y -= ht;
    if(zoom > 1.5 * DT_LIBRARY_MAX_ZOOM)
    {
      // quad speed.
      if(track == 0)
        ;
      else if(track > 1)
        zoom_y += ht;
      else if(track > 0)
        zoom_x += wd;
      else if(track > -2)
        zoom_x -= wd;
      else
        zoom_y -= ht;
    }
  }

  if(oldzoom != zoom)
  {
    float oldx = (pointerx + zoom_x) * oldzoom / width;
    float oldy = (pointery + zoom_y) * oldzoom / width;
    if(zoom == 1)
    {
      zoom_x = (int)oldx * wd;
      zoom_y = (int)oldy * ht;
      lib->offset = 0x7fffffff;
    }
    else
    {
      zoom_x = oldx * wd - pointerx;
      zoom_y = oldy * ht - pointery;
    }
  }
  oldzoom = zoom;

  // TODO: replace this with center on top of selected/developed image
  if(center)
  {
    if(mouse_over_id >= 0)
    {
      zoom_x = wd * ((int)(zoom_x) / (int)wd);
      zoom_y = ht * ((int)(zoom_y) / (int)ht);
    }
    else
      zoom_x = zoom_y = 0.0;
    center = 0;
  }

  // mouse left the area, but we leave mouse over as it was, especially during panning
  // if(!pan && pointerx > 0 && pointerx < width && pointery > 0 && pointery < height)
  // dt_control_set_mouse_over_id(-1);
  if(!pan && zoom != 1) dt_control_set_mouse_over_id(-1);

  // set scrollbar positions, clamp zoom positions

  if(lib->collection_count == 0)
  {
    zoom_x = zoom_y = 0.0f;
  }
  else if(zoom < 1.01)
  {
    if(zoom == 1 && zoom_x < 0 && zoom_y > 0) // full view, wrap around
    {
      zoom_x = wd * DT_LIBRARY_MAX_ZOOM - wd;
      zoom_y -= ht;
    }
    if(zoom_x < 0) zoom_x = 0;
    if(zoom == 1 && zoom_x > wd * DT_LIBRARY_MAX_ZOOM - wd) // full view, wrap around
    {
      zoom_x = 0;
      zoom_y += ht;
    }
    if(zoom_x > wd * DT_LIBRARY_MAX_ZOOM - wd) zoom_x = wd * DT_LIBRARY_MAX_ZOOM - wd;
    if(zoom_y < 0) zoom_y = 0;
    if(zoom_y > ht * lib->collection_count / MIN(DT_LIBRARY_MAX_ZOOM, zoom) - ht)
      zoom_y = ht * lib->collection_count / MIN(DT_LIBRARY_MAX_ZOOM, zoom) - ht;
  }
  else
  {
    if(zoom_x < -width + wd) zoom_x = -width + wd;
    if(zoom_x > wd * DT_LIBRARY_MAX_ZOOM - wd) zoom_x = wd * DT_LIBRARY_MAX_ZOOM - wd;
    if(zoom_y < -height + ht) zoom_y = -height + ht;
    if(zoom_y > ht * ceilf((float)lib->collection_count / DT_LIBRARY_MAX_ZOOM) - ht)
      zoom_y = ht * ceilf((float)lib->collection_count / DT_LIBRARY_MAX_ZOOM) - ht;
  }


  int offset_i = (int)(zoom_x / wd);
  int offset_j = (int)(zoom_y / ht);
  if(lib->first_visible_filemanager >= 0)
  {
    offset_i = lib->first_visible_filemanager % DT_LIBRARY_MAX_ZOOM;
    offset_j = lib->first_visible_filemanager / DT_LIBRARY_MAX_ZOOM;
  }
  lib->first_visible_filemanager = -1;
  lib->first_visible_zoomable = offset_i + DT_LIBRARY_MAX_ZOOM * offset_j;
  // arbitrary 1000 to avoid bug due to round towards zero using (int)
  int seli = zoom == 1 ? 0 : ((int)(1000 + (pointerx + zoom_x) / wd) - MAX(offset_i, 0) - 1000);
  int selj = zoom == 1 ? 0 : ((int)(1000 + (pointery + zoom_y) / ht) - offset_j - 1000);
  float offset_x = (zoom == 1) ? 0.0 : (zoom_x / wd - (int)(zoom_x / wd));
  float offset_y = (zoom == 1) ? 0.0 : (zoom_y / ht - (int)(zoom_y / ht));
  const int max_rows = (zoom == 1) ? 1 : (2 + (int)((height) / ht + .5));
  lib->max_rows = max_rows;
  const int max_cols = (zoom == 1) ? 1 : (MIN(DT_LIBRARY_MAX_ZOOM - MAX(0, offset_i), 1 + (int)(zoom + .5)));

  int offset = MAX(0, offset_i) + DT_LIBRARY_MAX_ZOOM * offset_j;
  int img_pointerx = zoom == 1 ? pointerx : fmodf(pointerx + zoom_x, wd);
  int img_pointery = zoom == 1 ? pointery : fmodf(pointery + zoom_y, ht);

  // assure 1:1 is not switching images on resize/tab events:
  if(!track && lib->offset != 0x7fffffff && zoom == 1)
  {
    offset = lib->offset;
    zoom_x = wd * (offset % DT_LIBRARY_MAX_ZOOM);
    zoom_y = ht * (offset / DT_LIBRARY_MAX_ZOOM);
  }
  else
    lib->offset = offset;

  int id;

  dt_view_set_scrollbar(self,
                        zoom_x, -width + wd, wd * DT_LIBRARY_MAX_ZOOM - wd + width, width,
                        zoom_y,  -height + ht,
                        ht * ceilf((float)lib->collection_count / DT_LIBRARY_MAX_ZOOM) - ht + height, height);

  cairo_translate(cr, -offset_x * wd, -offset_y * ht);
  cairo_translate(cr, -MIN(offset_i * wd, 0.0), 0.0);

  for(int row = 0; row < max_rows; row++)
  {
    if(offset < 0)
    {
      cairo_translate(cr, 0, ht);
      offset += DT_LIBRARY_MAX_ZOOM;
      continue;
    }

    /* clear and reset main query */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.main_query);
    DT_DEBUG_SQLITE3_RESET(lib->statements.main_query);

    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 1, offset);
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 2, max_cols);
    for(int col = 0; col < max_cols; col++)
    {
      if(sqlite3_step(lib->statements.main_query) == SQLITE_ROW)
      {
        id = sqlite3_column_int(lib->statements.main_query, 0);

        // set mouse over id
        if((zoom == 1 && mouse_over_id < 0) || ((!pan || track) && seli == col && selj == row && pointerx > 0
                                                && pointerx < width && pointery > 0 && pointery < height))
        {
          mouse_over_id = id;
          dt_control_set_mouse_over_id(mouse_over_id);
        }

        cairo_save(cr);
        // if(zoom == 1) dt_image_prefetch(image, DT_IMAGE_MIPF);
        missing += dt_view_image_expose(&(lib->image_over), id, cr, wd, zoom == 1 ? height : ht, zoom, img_pointerx,
                             img_pointery, FALSE, FALSE);
        cairo_restore(cr);
        if(zoom == 1)
        {
          // we are on the single-image display at a time, in this case we want the selection to be updated to
          // contain
          // this single image.
          dt_selection_select_single(darktable.selection, id);
        }
      }
      else
        goto failure;
      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols * wd, ht);
    offset += DT_LIBRARY_MAX_ZOOM;
  }
failure:

  lib->zoom_x = zoom_x;
  lib->zoom_y = zoom_y;
  lib->track = 0;
  lib->center = center;
  if(darktable.unmuted & DT_DEBUG_CACHE) dt_mipmap_cache_print(darktable.mipmap_cache);
  return missing;
}

/**
 * Displays a full screen preview of the image currently under the mouse pointer.
 */
static int expose_full_preview(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                               int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  int offset = 0;
  if(lib->track > 2) offset = 1;
  if(lib->track < -2) offset = -1;
  lib->track = 0;

  // only look for images to preload or update the one shown when we moved to another image
  if(offset != 0)
  {
    /* If more than one image is selected, iterate over these. */
    /* If only one image is selected, scroll through all known images. */
    sqlite3_stmt *stmt;
    int sel_group_count = 0;
    int current_group = -1;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1,
        &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
      uint32_t imgid  = sqlite3_column_int(stmt, 0);
      const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
      if (image->group_id != current_group)
      {
        sel_group_count++;
        current_group = image->group_id;
      }
      dt_image_cache_read_release(darktable.image_cache, image);
    }
    sqlite3_finalize(stmt);
    dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] selected group: %d\n", sel_group_count);

    /* How many images to preload in advance. */
    int preload_num = dt_conf_get_int("plugins/lighttable/preview/full_size_preload_count");
    gboolean preload = preload_num > 0;
    preload_num = CLAMPS(preload_num, 1, 99999);

    gchar *stmt_string = g_strdup_printf("SELECT col.imgid AS id, col.rowid FROM memory.collected_images AS col %s "
                                         "WHERE col.rowid %s %d ORDER BY col.rowid %s LIMIT %d",
                                         (sel_group_count <= 1) ?
                                           /* We want to operate on the currently collected images,
                                            * so there's no need to match against the selection */
                                           "" :
                                           /* Limit the matches to the current selection */
                                           "INNER JOIN main.selected_images AS sel ON col.imgid = sel.imgid",
                                         (offset >= 0) ? ">" : "<",
                                         lib->full_preview_rowid,
                                         /* Direction of our navigation -- when showing for the first time,
                                          * i.e. when offset == 0, assume forward navigation */
                                         (offset >= 0) ? "ASC" : "DESC",
                                         preload_num);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), stmt_string, -1, &stmt, NULL);

    /* Walk through the "next" images, activate preload and find out where to go if moving */
    int *preload_stack = malloc(preload_num * sizeof(int));
    for(int i = 0; i < preload_num; ++i)
    {
      preload_stack[i] = -1;
    }
    int count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
      /* Check if we're about to move */
      if(count == 0 && offset != 0)
      {
        /* We're moving, so let's update the "next image" bits */
        lib->full_preview_id = sqlite3_column_int(stmt, 0);
        lib->full_preview_rowid = sqlite3_column_int(stmt, 1);
        dt_control_set_mouse_over_id(lib->full_preview_id);
      }
      /* Store the image details for preloading, see below. */
      preload_stack[count] = sqlite3_column_int(stmt, 0);
      ++count;
    }
    g_free(stmt_string);
    sqlite3_finalize(stmt);

    if(preload)
    {
      dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, width, height);
      /* Preload these images.
      * The job queue is not a queue, but a stack, so we have to do it backwards.
      * Simply swapping DESC and ASC in the SQL won't help because we rely on the LIMIT clause, and
      * that LIMIT has to work with the "correct" sort order. One could use a subquery, but I don't
      * think that would be terribly elegant, either. */
      while(--count >= 0 && preload_stack[count] != -1)
        dt_mipmap_cache_get(darktable.mipmap_cache, NULL, preload_stack[count], mip, DT_MIPMAP_PREFETCH, 'r');
    }

    free(preload_stack);
  }

  lib->image_over = DT_VIEW_DESERT;
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_PREVIEW_BG);
  cairo_paint(cr);

  const int frows = 5, fcols = 5;
  if(lib->display_focus)
  {
    if(lib->full_res_thumb_id != lib->full_preview_id)
    {
      gboolean from_cache = TRUE;
      char filename[PATH_MAX] = { 0 };
      dt_image_full_path(lib->full_preview_id, filename, sizeof(filename), &from_cache);
      free(lib->full_res_thumb);
      lib->full_res_thumb = NULL;
      dt_colorspaces_color_profile_type_t color_space;
      if(!dt_imageio_large_thumbnail(filename, &lib->full_res_thumb,
                                               &lib->full_res_thumb_wd,
                                               &lib->full_res_thumb_ht,
                                               &color_space)) {
        lib->full_res_thumb_orientation = ORIENTATION_NONE;
        lib->full_res_thumb_id = lib->full_preview_id;
      }

      if(lib->full_res_thumb_id == lib->full_preview_id)
      {
        dt_focus_create_clusters(lib->full_res_focus, frows, fcols, lib->full_res_thumb,
                                 lib->full_res_thumb_wd, lib->full_res_thumb_ht);
      }
    }
  }
#if 0 // expose full res thumbnail:
  if(lib->full_res_thumb_id == lib->full_preview_id)
  {
    static float pointerx_c = 0, pointery_c = 0;
    const int32_t stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, lib->full_res_thumb_wd);
    cairo_surface_t *surface = cairo_image_surface_create_for_data (lib->full_res_thumb, CAIRO_FORMAT_RGB24, lib->full_res_thumb_wd, lib->full_res_thumb_ht, stride);
    cairo_save(cr);
    int wd = lib->full_res_thumb_wd, ht = lib->full_res_thumb_ht;
    if(lib->full_res_thumb_orientation & ORIENTATION_SWAP_XY)
      wd = lib->full_res_thumb_ht, ht = lib->full_res_thumb_wd;
    if(pointerx >= 0 && pointery >= 0)
    { // avoid jumps in case mouse leaves drawing area
      pointerx_c = pointerx;
      pointery_c = pointery;
    }
    const float tx = -(wd - width ) * CLAMP(pointerx_c/(float)width,  0.0f, 1.0f),
                ty = -(ht - height) * CLAMP(pointery_c/(float)height, 0.0f, 1.0f);
    cairo_translate(cr, tx, ty);
    if(lib->full_res_thumb_orientation & ORIENTATION_SWAP_XY)
    {
      cairo_matrix_t m = (cairo_matrix_t){0.0, 1.0, 1.0, 0.0, 0.0, 0.0};
      cairo_transform(cr, &m);
    }
    if(lib->full_res_thumb_orientation & ORIENTATION_FLIP_X)
    {
      cairo_scale(cr, 1, -1);
      cairo_translate(cr, 0, -lib->full_res_thumb_ht-1);
    }
    if(lib->full_res_thumb_orientation & ORIENTATION_FLIP_Y)
    {
      cairo_scale(cr, -1, 1);
      cairo_translate(cr, -lib->full_res_thumb_wd-1, 0);
    }
    cairo_set_source_surface (cr, surface, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_rectangle(cr, 0, 0, lib->full_res_thumb_wd, lib->full_res_thumb_ht);
    cairo_fill(cr);
    cairo_surface_destroy (surface);

    // draw clustered focus regions
    for(int k=0;k<49;k++)
    {
      const float intens = (lib->full_res_focus[k].thrs - FOCUS_THRS)/FOCUS_THRS;
      if(lib->full_res_focus[k].n > lib->full_res_thumb_wd*lib->full_res_thumb_ht/49.0f * 0.01f)
      // if(intens > 0.5f)
      {
        const float stddevx = sqrtf(lib->full_res_focus[k].x2 - lib->full_res_focus[k].x*lib->full_res_focus[k].x);
        const float stddevy = sqrtf(lib->full_res_focus[k].y2 - lib->full_res_focus[k].y*lib->full_res_focus[k].y);
        cairo_set_source_rgb(cr, intens, 0.0, 0.0);
        cairo_set_line_width(cr, 5.0f*intens);
        cairo_rectangle(cr, lib->full_res_focus[k].x - stddevx, lib->full_res_focus[k].y - stddevy, 2*stddevx, 2*stddevy);
        cairo_stroke(cr);
      }
    }
    cairo_restore(cr);
  }
  else
#endif
  const int missing = dt_view_image_expose(&(lib->image_over), lib->full_preview_id, cr,
                                           width, height, 1, pointerx, pointery, TRUE, FALSE);

  if(lib->display_focus && (lib->full_res_thumb_id == lib->full_preview_id))
    dt_focus_draw_clusters(cr, width, height, lib->full_preview_id, lib->full_res_thumb_wd,
                           lib->full_res_thumb_ht, lib->full_res_focus, frows, fcols);
  return missing;
}

static gboolean _expose_again(gpointer user_data)
{
  // unfortunately there might have been images without thumbnails during expose.
  // this can have multiple reasons: not loaded yet (we'll receive a signal when done)
  // or still locked for writing.. we won't be notified when this changes.
  // so we just track whether there were missing images and expose again.
  dt_control_queue_redraw_center();
  return FALSE; // don't call again
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  const double start = dt_get_wtime();

  // Let's show full preview if in that state...
  dt_library_t *lib = (dt_library_t *)self->data;

  /* TODO: instead of doing a check here, the call to switch_layout_to
     should be done in the place where the layout was actually changed. */
  const int new_layout = dt_conf_get_int("plugins/lighttable/layout");
  if(lib->layout != new_layout) switch_layout_to(lib, new_layout);

  int missing_thumbnails = 0;

  if(lib->full_preview_id != -1)
  {
    missing_thumbnails = expose_full_preview(self, cr, width, height, pointerx, pointery);
  }
  else // we do pass on expose to manager or zoomable
  {
    switch(new_layout)
    {
      case 1: // file manager
        missing_thumbnails = expose_filemanager(self, cr, width, height, pointerx, pointery);
        break;
      default: // zoomable
        missing_thumbnails = expose_zoomable(self, cr, width, height, pointerx, pointery);
        break;
    }
  }
  const double end = dt_get_wtime();
  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] expose took %0.04f sec\n", end - start);
  if(missing_thumbnails)
    g_timeout_add(500, _expose_again, 0);
}

static gboolean go_up_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                         GdkModifierType modifier, gpointer data)
{
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;

  if(layout == 1)
    move_view(lib, DIRECTION_TOP);
  else
    lib->offset = 0;
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean go_down_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;

  if(layout == 1)
    move_view(lib, DIRECTION_BOTTOM);
  else
    lib->offset = 0x1fffffff;
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean go_pgup_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  if(layout == 1)
    move_view(lib, DIRECTION_PGUP);
  else
  {
    const int iir = dt_conf_get_int("plugins/lighttable/images_in_row");
    const int scroll_by_rows = 4; /* This should be the number of visible rows. */
    const int offset_delta = scroll_by_rows * iir;
    lib->offset = MAX(lib->offset - offset_delta, 0);
  }
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean go_pgdown_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  if(layout == 1)
  {
    move_view(lib, DIRECTION_PGDOWN);
  }
  else
  {
    const int iir = dt_conf_get_int("plugins/lighttable/images_in_row");
    const int scroll_by_rows = 4; /* This should be the number of visible rows. */
    const int offset_delta = scroll_by_rows * iir;
    lib->offset = MIN(lib->offset + offset_delta, lib->collection_count);
  }
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean realign_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  if(layout == 1) move_view(lib, DIRECTION_CENTER);
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean select_toggle_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  uint32_t id = dt_control_get_mouse_over_id();
  dt_selection_toggle(darktable.selection, id);
  return TRUE;
}

static gboolean select_single_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  uint32_t id = dt_control_get_mouse_over_id();
  dt_selection_select_single(darktable.selection, id);
  return TRUE;
}

static gboolean star_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = darktable.view_manager->proxy.lighttable.view;
  int num = GPOINTER_TO_INT(data);
  int32_t mouse_over_id;
  int next_image_rowid = -1;

  dt_library_t *lib = (dt_library_t *)self->data;
  if(lib->using_arrows)
  {
    // if using arrows may be the image I'm rating is going to disappear from the collection.
    // So, store where may be we need to jump
    int imgid_for_offset;
    sqlite3_stmt *stmt;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT MIN(imgid) FROM main.selected_images", -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      sqlite3_stmt *inner_stmt;
      imgid_for_offset = sqlite3_column_int(stmt, 0);
      if(!imgid_for_offset)
      {
        // empty selection
        imgid_for_offset = dt_control_get_mouse_over_id();
      }

      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
          //"SELECT imgid FROM memory.collected_images", -1, &inner_stmt,
          "SELECT rowid FROM memory.collected_images WHERE imgid=?1", -1, &inner_stmt,
          NULL);
      DT_DEBUG_SQLITE3_BIND_INT(inner_stmt, 1, imgid_for_offset);
      if(sqlite3_step(inner_stmt) == SQLITE_ROW)
        next_image_rowid = sqlite3_column_int(inner_stmt, 0);
      sqlite3_finalize(inner_stmt);
    }
    sqlite3_finalize(stmt);
  }

  mouse_over_id = dt_view_get_image_to_act_on();
  if(mouse_over_id <= 0)
    dt_ratings_apply_to_selection(num);
  else
    dt_ratings_apply_to_image_or_group(mouse_over_id, num);
  _update_collected_images(self);

  dt_collection_update_query(darktable.collection); // update the counter
  if(lib->collection_count != dt_collection_get_count(darktable.collection))
  {
    // some images disappeared from collection. Selection is now invisible.
    // lib->collection_count  --> before the rating
    // dt_collection_get_count(darktable.collection)  --> after the rating
    dt_selection_clear(darktable.selection);
    if(lib->using_arrows)
    {
      // Jump where stored before
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT imgid FROM memory.collected_images WHERE rowid=?1 OR rowid=?1 - 1 "
                                  "ORDER BY rowid DESC LIMIT 1", -1, &stmt,
                                  NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, next_image_rowid);
      if(sqlite3_step(stmt) == SQLITE_ROW)
        mouse_over_id = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
      dt_control_set_mouse_over_id(mouse_over_id);
    }
  }
  return TRUE;
}

static void _lighttable_mipmaps_updated_signal_callback(gpointer instance, gpointer user_data)
{
  dt_control_queue_redraw_center();
}

static void drag_and_drop_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                   GtkSelectionData *selection_data, guint target_type, guint time,
                                   gpointer data)
{
  gboolean success = FALSE;

  if((target_type == DND_TARGET_URI) && (selection_data != NULL) && (gtk_selection_data_get_length(selection_data) >= 0))
  {
    gchar **uri_list = g_strsplit_set((gchar *)gtk_selection_data_get_data(selection_data), "\r\n", 0);
    if(uri_list)
    {
      gchar **image_to_load = uri_list;
      while(*image_to_load)
      {
        if(**image_to_load)
        {
          dt_load_from_string(*image_to_load, FALSE, NULL); // TODO: do we want to open the image in darkroom mode? If
                                                            // yes -> set to TRUE.
        }
        image_to_load++;
      }
    }
    g_strfreev(uri_list);
    success = TRUE;
  }
  gtk_drag_finish(context, success, FALSE, time);
}

void enter(dt_view_t *self)
{
  gtk_drag_dest_set(dt_ui_center(darktable.gui->ui), GTK_DEST_DEFAULT_ALL, target_list_all, n_targets_all,
                    GDK_ACTION_COPY);

  // dropping images for import
  g_signal_connect(dt_ui_center(darktable.gui->ui), "drag-data-received", G_CALLBACK(drag_and_drop_received),
                   self);

  _register_custom_image_order_drag_n_drop(self);

  /* connect to signals */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_lighttable_mipmaps_updated_signal_callback), (gpointer)self);

  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

  // clear some state variables
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->button = 0;
  lib->pan = 0;
  dt_collection_hint_message(darktable.collection);

  // hide panel if we are in full preview mode
  if(lib->full_preview_id != -1)
  {
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, FALSE, FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, FALSE, FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM, FALSE, FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP, FALSE, FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, FALSE, FALSE);
  }

  char *scrollbars_conf = dt_conf_get_string("scrollbars");

  gboolean scrollbars_visible = FALSE;
  if(scrollbars_conf)
  {
    if(strcmp(scrollbars_conf, "no scrollbars"))
      scrollbars_visible = TRUE;
    g_free(scrollbars_conf);
  }

  dt_ui_scrollbars_show(darktable.gui->ui, scrollbars_visible);
}

void leave(dt_view_t *self)
{
  gtk_drag_dest_unset(dt_ui_center(darktable.gui->ui));

  // disconnect dropping images for import
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(drag_and_drop_received),self);

  _unregister_custom_image_order_drag_n_drop(self);

  /* disconnect from signals */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lighttable_mipmaps_updated_signal_callback),
                               (gpointer)self);

  // clear some state variables
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->button = 0;
  lib->pan = 0;

  // exit preview mode if non-sticky
  if(lib->full_preview_id != -1 && lib->full_preview_sticky == 0)
  {
    lib->full_preview_id = -1;
    lib->full_preview_rowid = -1;
    dt_control_set_mouse_over_id(-1);
    lib->full_preview = 0;
    lib->display_focus = 0;
  }

  dt_ui_scrollbars_show(darktable.gui->ui, FALSE);
}

void reset(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->center = 1;
  lib->track = lib->pan = 0;
  lib->offset = 0x7fffffff;
  lib->first_visible_zoomable = -1;
  lib->first_visible_filemanager = 0;
  dt_control_set_mouse_over_id(-1);
}


void mouse_enter(dt_view_t *self)
{
  // TODO: In gtk.c the function center_leave return true. It is not needed when using arrows. the same for mouse_leave, mouse_move
  dt_library_t *lib = (dt_library_t *)self->data;
  uint32_t id = dt_control_get_mouse_over_id();
  if (lib->using_arrows == 0)
  {
    if(id == -1)
      dt_control_set_mouse_over_id(
          lib->last_mouse_over_id); // this seems to be needed to fix the strange events fluxbox emits
  }
}

void mouse_leave(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if (lib->using_arrows == 0)
  {
    lib->last_mouse_over_id = dt_control_get_mouse_over_id(); // see mouse_enter (re: fluxbox)
    if(!lib->pan && dt_conf_get_int("plugins/lighttable/images_in_row") != 1)
    {
      dt_control_set_mouse_over_id(-1);
      dt_control_queue_redraw_center();
    }
  }
}


void scrollbar_changed(dt_view_t *self, double x, double y)
{
  const int layout = dt_conf_get_int("plugins/lighttable/layout");

  switch(layout)
  {
    case 1: // file manager
    {
      const int iir = dt_conf_get_int("plugins/lighttable/images_in_row");
      _set_position(self, round(y/iir)*iir);
      break;
    }
    default: // zoomable
    {
      dt_library_t *lib = (dt_library_t *) self->data;
      lib->zoom_x = x;
      lib->zoom_y = y;
      dt_control_queue_redraw_center();
      break;
    }
  }
}

void scrolled(dt_view_t *self, double x, double y, int up, int state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  if(lib->full_preview_id > -1)
  {
    if(up)
      lib->track = -DT_LIBRARY_MAX_ZOOM;
    else
      lib->track = +DT_LIBRARY_MAX_ZOOM;
  }
  else if(layout == 1 && state == 0)
  {
    if(up)
      move_view(lib, DIRECTION_UP);
    else
      move_view(lib, DIRECTION_DOWN);
  }
  else
  {
    int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
    if(up)
    {
      zoom--;
      if(zoom < 1)
        zoom = 1;
      else if(layout == 1)
        zoom_around_image(lib, x, y, self->width, self->height, zoom + 1, zoom);
    }
    else
    {
      zoom++;
      if(zoom > 2 * DT_LIBRARY_MAX_ZOOM)
        zoom = 2 * DT_LIBRARY_MAX_ZOOM;
      else if(layout == 1)
        zoom_around_image(lib, x, y, self->width, self->height, zoom - 1, zoom);
    }
    dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
  }
}


void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  dt_control_queue_redraw_center();
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->pan = 0;
  if(which == 1) dt_control_change_cursor(GDK_LEFT_PTR);
  return 1;
}


static void _audio_child_watch(GPid pid, gint status, gpointer data)
{
  dt_library_t *lib = (dt_library_t *)data;
  lib->audio_player_id = -1;
  g_spawn_close_pid(pid);
}

static void _stop_audio(dt_library_t *lib)
{
  // make sure that the process didn't finish yet and that _audio_child_watch() hasn't run
  if(lib->audio_player_id == -1) return;
  // we don't want to trigger the callback due to a possible race condition
  g_source_remove(lib->audio_player_event_source);
#ifdef _WIN32
// TODO: add Windows code to actually kill the process
#else  // _WIN32
  if(lib->audio_player_id != -1)
  {
    if(getpgid(0) != getpgid(lib->audio_player_pid))
      kill(-lib->audio_player_pid, SIGKILL);
    else
      kill(lib->audio_player_pid, SIGKILL);
  }
#endif // _WIN32
  g_spawn_close_pid(lib->audio_player_pid);
  lib->audio_player_id = -1;
}

int button_pressed(dt_view_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->modifiers = state;
  lib->key_jump_offset = 0;
  lib->button = which;
  lib->select_offset_x = lib->zoom_x;
  lib->select_offset_y = lib->zoom_y;
  lib->select_offset_x += x;
  lib->select_offset_y += y;

  if (dt_control_get_mouse_over_id() < 0 || !_is_custom_image_order_required(self))
  {
    lib->pan = 1;
  }

  if(which == 1) dt_control_change_cursor(GDK_HAND1);
  if(which == 1 && type == GDK_2BUTTON_PRESS) return 0;
  // image button pressed?
  if(which == 1)
  {
    switch(lib->image_over)
    {
      case DT_VIEW_DESERT:
      {

        if (lib->using_arrows)
        {
          // in this case dt_control_get_mouse_over_id() means "last image visited with arrows"
          lib->using_arrows = 0;
          return 0;
        }

        int32_t id = dt_control_get_mouse_over_id();
        if((lib->modifiers & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) == 0)
          dt_selection_select_single(darktable.selection, id);
        else if((lib->modifiers & (GDK_CONTROL_MASK)) == GDK_CONTROL_MASK)
          dt_selection_toggle(darktable.selection, id);
        else if((lib->modifiers & (GDK_SHIFT_MASK)) == GDK_SHIFT_MASK)
          dt_selection_select_range(darktable.selection, id);

        break;
      }
      case DT_VIEW_REJECT:
      case DT_VIEW_STAR_1:
      case DT_VIEW_STAR_2:
      case DT_VIEW_STAR_3:
      case DT_VIEW_STAR_4:
      case DT_VIEW_STAR_5:
      {
        int32_t mouse_over_id = dt_control_get_mouse_over_id();
        dt_ratings_apply_to_image_or_group(mouse_over_id, lib->image_over);
        _update_collected_images(self);
        break;
      }
      case DT_VIEW_GROUP:
      {
        int32_t mouse_over_id = dt_control_get_mouse_over_id();
        const dt_image_t *image = dt_image_cache_get(darktable.image_cache, mouse_over_id, 'r');
        if(!image) return 0;
        int group_id = image->group_id;
        int id = image->id;
        dt_image_cache_read_release(darktable.image_cache, image);
        if(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) // just add the whole group to the selection. TODO:
                                                        // make this also work for collapsed groups.
        {
          sqlite3_stmt *stmt;
          DT_DEBUG_SQLITE3_PREPARE_V2(
              dt_database_get(darktable.db),
              "INSERT OR IGNORE INTO main.selected_images SELECT id FROM main.images WHERE group_id = ?1",
              -1, &stmt, NULL);
          DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, group_id);
          sqlite3_step(stmt);
          sqlite3_finalize(stmt);
        }
        else if(group_id == darktable.gui->expanded_group_id) // the group is already expanded, so ...
        {
          if(id == darktable.gui->expanded_group_id) // ... collapse it
            darktable.gui->expanded_group_id = -1;
          else // ... make the image the new representative of the group
            darktable.gui->expanded_group_id = dt_grouping_change_representative(id);
        }
        else // expand the group
          darktable.gui->expanded_group_id = group_id;
        dt_collection_update_query(darktable.collection);
        break;
      }
      case DT_VIEW_AUDIO:
      {
        int32_t mouse_over_id = dt_control_get_mouse_over_id();
        gboolean start_audio = TRUE;
        if(lib->audio_player_id != -1)
        {
          // don't start the audio for the image we just killed it for
          if(lib->audio_player_id == mouse_over_id) start_audio = FALSE;

          _stop_audio(lib);
        }

        if(start_audio)
        {
          // if no audio is played at the moment -> play audio
          char *player = dt_conf_get_string("plugins/lighttable/audio_player");
          if(player && *player)
          {
            char *filename = dt_image_get_audio_path(mouse_over_id);
            if(filename)
            {
              char *argv[] = { player, filename, NULL };
              gboolean ret
                  = g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH
                                                    | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                  NULL, NULL, &lib->audio_player_pid, NULL);

              if(ret)
              {
                lib->audio_player_id = mouse_over_id;
                lib->audio_player_event_source
                    = g_child_watch_add(lib->audio_player_pid, (GChildWatchFunc)_audio_child_watch, lib);
              }
              else
                lib->audio_player_id = -1;

              g_free(filename);
            }
          }
          g_free(player);
        }

        break;
      }
      default:
        return 0;
    }
  }
  return 1;
}

int key_released(dt_view_t *self, guint key, guint state)
{
  dt_control_accels_t *accels = &darktable.control->accels;
  dt_library_t *lib = (dt_library_t *)self->data;
  if(lib->key_select && (key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R))
  {
    lib->key_select = 0;
    lib->key_select_direction = -1;
  }

  if(!darktable.control->key_accelerators_on) return 0;

  if(((key == accels->lighttable_preview.accel_key && state == accels->lighttable_preview.accel_mods)
      || (key == accels->lighttable_preview_display_focus.accel_key
          && state == accels->lighttable_preview_display_focus.accel_mods)) && lib->full_preview_id != -1)
  {

    lib->full_preview_id = -1;
    lib->full_preview_rowid = -1;
    if(!lib->using_arrows)
      dt_control_set_mouse_over_id(-1);

    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, (lib->full_preview & 1), FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, (lib->full_preview & 2), FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM, (lib->full_preview & 4), FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP, (lib->full_preview & 8), FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, (lib->full_preview & 16), FALSE);

    lib->full_preview = 0;
    lib->display_focus = 0;
  }

  return 1;
}

int key_pressed(dt_view_t *self, guint key, guint state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  dt_control_accels_t *accels = &darktable.control->accels;

  if(!darktable.control->key_accelerators_on) return 0;

  int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");

  const int layout = dt_conf_get_int("plugins/lighttable/layout");

  if(lib->full_preview_id != -1 && ((key == accels->lighttable_preview_sticky_exit.accel_key
                                     && state == accels->lighttable_preview_sticky_exit.accel_mods)
                                    || (key == accels->lighttable_preview_sticky.accel_key
                                        && state == accels->lighttable_preview_sticky.accel_mods)
                                    || (key == accels->lighttable_preview_sticky_focus.accel_key
                                        && state == accels->lighttable_preview_sticky_focus.accel_mods)))
  {
    lib->full_preview_id = -1;
    lib->full_preview_rowid = -1;
    if(!lib->using_arrows)
      dt_control_set_mouse_over_id(-1);

    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, (lib->full_preview & 1), FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, (lib->full_preview & 2), FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM, (lib->full_preview & 4), FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP, (lib->full_preview & 8), FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, (lib->full_preview & 16), FALSE);

    lib->full_preview = 0;
    lib->display_focus = 0;
    return 1;
  }

  if((key == accels->lighttable_preview.accel_key && state == accels->lighttable_preview.accel_mods)
     || (key == accels->lighttable_preview_display_focus.accel_key
         && state == accels->lighttable_preview_display_focus.accel_mods)
     || (key == accels->lighttable_preview_sticky.accel_key
         && state == accels->lighttable_preview_sticky.accel_mods)
     || (key == accels->lighttable_preview_sticky_focus.accel_key
         && state == accels->lighttable_preview_sticky_focus.accel_mods))
  {
    int32_t mouse_over_id = dt_control_get_mouse_over_id();
    if(lib->full_preview_id == -1 && mouse_over_id != -1)
    {
      if((key == accels->lighttable_preview.accel_key && state == accels->lighttable_preview.accel_mods)
         || (key == accels->lighttable_preview_display_focus.accel_key
             && state == accels->lighttable_preview_display_focus.accel_mods))
      {
        lib->full_preview_sticky = 0;
      }
      else
      {
        lib->full_preview_sticky = 1;
      }
      // encode panel visibility into full_preview
      lib->full_preview = 0;
      lib->full_preview_id = mouse_over_id;

      // set corresponding rowid in the collected images
      {
        sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT rowid FROM memory.collected_images WHERE imgid=?1", -1, &stmt,
                                    NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, lib->full_preview_id);
        if(sqlite3_step(stmt) == SQLITE_ROW)
        {
          lib->full_preview_rowid = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
      }

      // let's hide some gui components
      lib->full_preview |= (dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_LEFT) & 1) << 0;
      dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, FALSE, FALSE);
      lib->full_preview |= (dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_RIGHT) & 1) << 1;
      dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, FALSE, FALSE);
      lib->full_preview |= (dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM) & 1) << 2;
      dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM, FALSE, FALSE);
      lib->full_preview |= (dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP) & 1) << 3;
      dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP, FALSE, FALSE);
      lib->full_preview |= (dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_TOP) & 1) << 4;
      dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, FALSE, FALSE);

      // preview with focus detection
      if((key == accels->lighttable_preview_display_focus.accel_key
          && state == accels->lighttable_preview_display_focus.accel_mods)
         || (key == accels->lighttable_preview_sticky_focus.accel_key
             && state == accels->lighttable_preview_sticky_focus.accel_mods))
      {
        lib->display_focus = 1;
      }

      // dt_dev_invalidate(darktable.develop);
      return 1;
    }
    return 0;
  }

  if (key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R)
  {
    lib->key_select = 1;
  }

  if((key == accels->lighttable_left.accel_key && state == accels->lighttable_left.accel_mods) || (key == accels->lighttable_left.accel_key && layout == 1 && zoom != 1))
  {
    if(lib->full_preview_id > -1)
      lib->track = -DT_LIBRARY_MAX_ZOOM;
    else if(layout == 1)
    {
      if (zoom == 1)
      {
        move_view(lib, DIRECTION_UP);
        lib->using_arrows = 0;
      }
      else
      {
        lib->using_arrows = 1;
        lib->key_jump_offset = -1;
        lib->track = -1;
      }
    }
    else
      lib->track = -1;
    return 1;
  }

  //if(key == accels->lighttable_right.accel_key && state == accels->lighttable_right.accel_mods)
  if((key == accels->lighttable_right.accel_key && state == accels->lighttable_right.accel_mods) || (key == accels->lighttable_right.accel_key && layout == 1 && zoom != 1))
  {
    if(lib->full_preview_id > -1)
      lib->track = +DT_LIBRARY_MAX_ZOOM;
    else if(layout == 1)
    {
      if (zoom == 1)
      {
        move_view(lib, DIRECTION_DOWN);
        lib->using_arrows = 0;
      }
      else
      {
        lib->using_arrows = 1;
        lib->key_jump_offset = 1;
        lib->track = -1;
      }
    }
    else
      lib->track = 1;
    return 1;
  }

  if((key == accels->lighttable_up.accel_key && state == accels->lighttable_up.accel_mods) || (key == accels->lighttable_up.accel_key && layout == 1 && zoom != 1))
  {
    if(lib->full_preview_id > -1)
      lib->track = -DT_LIBRARY_MAX_ZOOM;
    else if(layout == 1)
    {
      if (zoom == 1)
      {
        move_view(lib, DIRECTION_UP);
        lib->using_arrows = 0;
      }
      else {
        lib->using_arrows = 1;
        lib->key_jump_offset = zoom*-1;
      }
    }
    else
      lib->track = -DT_LIBRARY_MAX_ZOOM;
    return 1;
  }

  if((key == accels->lighttable_down.accel_key && state == accels->lighttable_down.accel_mods) || (key == accels->lighttable_down.accel_key && layout == 1 && zoom != 1))
  {
    if(lib->full_preview_id > -1)
      lib->track = +DT_LIBRARY_MAX_ZOOM;
    else if(layout == 1)
    {
      if (zoom == 1)
      {
        move_view(lib, DIRECTION_DOWN);
        lib->using_arrows = 0;
      }
      else
      {
        lib->using_arrows = 1;
        lib->key_jump_offset = zoom;
      }
    }
    else
      lib->track = DT_LIBRARY_MAX_ZOOM;
    return 1;
  }

  if(key == accels->lighttable_center.accel_key && state == accels->lighttable_center.accel_mods)
  {
    lib->center = 1;
    return 1;
  }

  if(key == accels->global_zoom_in.accel_key && state == accels->global_zoom_in.accel_mods)
  {
    zoom--;
    if(zoom < 1) zoom = 1;

    dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
    return 1;
  }

  if(key == accels->global_zoom_out.accel_key && state == accels->global_zoom_out.accel_mods)
  {
    zoom++;
    if(zoom > 2 * DT_LIBRARY_MAX_ZOOM) zoom = 2 * DT_LIBRARY_MAX_ZOOM;

    dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
    return 1;
  }

  return 0;
}

void init_key_accels(dt_view_t *self)
{
  // Initializing accelerators

  // Rating keys
  dt_accel_register_view(self, NC_("accel", "rate 0"), GDK_KEY_0, 0);
  dt_accel_register_view(self, NC_("accel", "rate 1"), GDK_KEY_1, 0);
  dt_accel_register_view(self, NC_("accel", "rate 2"), GDK_KEY_2, 0);
  dt_accel_register_view(self, NC_("accel", "rate 3"), GDK_KEY_3, 0);
  dt_accel_register_view(self, NC_("accel", "rate 4"), GDK_KEY_4, 0);
  dt_accel_register_view(self, NC_("accel", "rate 5"), GDK_KEY_5, 0);
  dt_accel_register_view(self, NC_("accel", "rate reject"), GDK_KEY_r, 0);

  // Navigation keys
  dt_accel_register_view(self, NC_("accel", "navigate up"), GDK_KEY_g, 0);
  dt_accel_register_view(self, NC_("accel", "navigate down"), GDK_KEY_g, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "navigate page up"), GDK_KEY_Page_Up, 0);
  dt_accel_register_view(self, NC_("accel", "navigate page down"), GDK_KEY_Page_Down, 0);

  // Scroll keys
  dt_accel_register_view(self, NC_("accel", "scroll up"), GDK_KEY_Up, 0);
  dt_accel_register_view(self, NC_("accel", "scroll down"), GDK_KEY_Down, 0);
  dt_accel_register_view(self, NC_("accel", "scroll left"), GDK_KEY_Left, 0);
  dt_accel_register_view(self, NC_("accel", "scroll right"), GDK_KEY_Right, 0);
  dt_accel_register_view(self, NC_("accel", "scroll center"), GDK_KEY_apostrophe, 0);
  dt_accel_register_view(self, NC_("accel", "realign images to grid"), GDK_KEY_l, 0);
  dt_accel_register_view(self, NC_("accel", "select toggle image"), GDK_KEY_space, 0);
  dt_accel_register_view(self, NC_("accel", "select single image"), GDK_KEY_Return, 0);

  // Preview key
  dt_accel_register_view(self, NC_("accel", "preview"), GDK_KEY_z, 0);
  dt_accel_register_view(self, NC_("accel", "preview with focus detection"), GDK_KEY_z, GDK_CONTROL_MASK);
  dt_accel_register_view(self, NC_("accel", "sticky preview"), 0, 0);
  dt_accel_register_view(self, NC_("accel", "sticky preview with focus detection"), 0, 0);
  dt_accel_register_view(self, NC_("accel", "exit sticky preview"), 0, 0);
}

void connect_key_accels(dt_view_t *self)
{
  GClosure *closure;

  // Rating keys
  closure = g_cclosure_new(G_CALLBACK(star_key_accel_callback), GINT_TO_POINTER(DT_VIEW_DESERT), NULL);
  dt_accel_connect_view(self, "rate 0", closure);
  closure = g_cclosure_new(G_CALLBACK(star_key_accel_callback), GINT_TO_POINTER(DT_VIEW_STAR_1), NULL);
  dt_accel_connect_view(self, "rate 1", closure);
  closure = g_cclosure_new(G_CALLBACK(star_key_accel_callback), GINT_TO_POINTER(DT_VIEW_STAR_2), NULL);
  dt_accel_connect_view(self, "rate 2", closure);
  closure = g_cclosure_new(G_CALLBACK(star_key_accel_callback), GINT_TO_POINTER(DT_VIEW_STAR_3), NULL);
  dt_accel_connect_view(self, "rate 3", closure);
  closure = g_cclosure_new(G_CALLBACK(star_key_accel_callback), GINT_TO_POINTER(DT_VIEW_STAR_4), NULL);
  dt_accel_connect_view(self, "rate 4", closure);
  closure = g_cclosure_new(G_CALLBACK(star_key_accel_callback), GINT_TO_POINTER(DT_VIEW_STAR_5), NULL);
  dt_accel_connect_view(self, "rate 5", closure);
  closure = g_cclosure_new(G_CALLBACK(star_key_accel_callback), GINT_TO_POINTER(DT_VIEW_REJECT), NULL);
  dt_accel_connect_view(self, "rate reject", closure);

  // Navigation keys
  closure = g_cclosure_new(G_CALLBACK(go_up_key_accel_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "navigate up", closure);
  closure = g_cclosure_new(G_CALLBACK(go_down_key_accel_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "navigate down", closure);
  closure = g_cclosure_new(G_CALLBACK(go_pgup_key_accel_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "navigate page up", closure);
  closure = g_cclosure_new(G_CALLBACK(go_pgdown_key_accel_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "navigate page down", closure);
  closure = g_cclosure_new(G_CALLBACK(select_toggle_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "select toggle image", closure);
  closure = g_cclosure_new(G_CALLBACK(select_single_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "select single image", closure);
  closure = g_cclosure_new(G_CALLBACK(realign_key_accel_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "realign images to grid", closure);
}

static void display_intent_callback(GtkWidget *combo, gpointer user_data)
{
  const int pos = dt_bauhaus_combobox_get(combo);

  dt_iop_color_intent_t new_intent = darktable.color_profiles->display_intent;

  // we are not using the int value directly so it's robust against changes on lcms' side
  switch(pos)
  {
    case 0:
      new_intent = DT_INTENT_PERCEPTUAL;
      break;
    case 1:
      new_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
      break;
    case 2:
      new_intent = DT_INTENT_SATURATION;
      break;
    case 3:
      new_intent = DT_INTENT_ABSOLUTE_COLORIMETRIC;
      break;
  }

  if(new_intent != darktable.color_profiles->display_intent)
  {
    darktable.color_profiles->display_intent = new_intent;
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    dt_control_queue_redraw_center();
  }
}

static void display_profile_callback(GtkWidget *combo, gpointer user_data)
{
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
    if(pp->display_pos == pos)
    {
      if(darktable.color_profiles->display_type != pp->type
        || (darktable.color_profiles->display_type == DT_COLORSPACE_FILE
        && strcmp(darktable.color_profiles->display_filename, pp->filename)))
      {
        darktable.color_profiles->display_type = pp->type;
        g_strlcpy(darktable.color_profiles->display_filename, pp->filename,
                  sizeof(darktable.color_profiles->display_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to system display profile. shouldn't happen
  fprintf(stderr, "can't find display profile `%s', using system display profile instead\n", dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->display_type != DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_type = DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_filename[0] = '\0';

end:
  if(profile_changed)
  {
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    dt_control_queue_redraw_center();
  }
}

void gui_init(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  // create display profile button
  GtkWidget *const profile_button = dtgtk_button_new(dtgtk_cairo_paint_display, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER,
                                                     NULL);
  gtk_widget_set_tooltip_text(profile_button, _("set display profile"));
  dt_view_manager_module_toolbox_add(darktable.view_manager, profile_button, DT_VIEW_LIGHTTABLE);

  // and the popup window
  const int panel_width = dt_conf_get_int("panel_width");
  lib->profile_floating_window = gtk_popover_new(profile_button);

  gtk_widget_set_size_request(GTK_WIDGET(lib->profile_floating_window), panel_width, -1);
#if GTK_CHECK_VERSION(3, 16, 0)
  g_object_set(G_OBJECT(lib->profile_floating_window), "transitions-enabled", FALSE, NULL);
#endif
  g_signal_connect_swapped(G_OBJECT(profile_button), "button-press-event", G_CALLBACK(gtk_widget_show_all), lib->profile_floating_window);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_set_margin_start(vbox, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_end(vbox, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_top(vbox, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_bottom(vbox, DT_PIXEL_APPLY_DPI(8));

  gtk_container_add(GTK_CONTAINER(lib->profile_floating_window), vbox);

  /** let's fill the encapsulating widgets */
  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));

  GtkWidget *display_intent = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(display_intent, NULL, _("display intent"));
  gtk_box_pack_start(GTK_BOX(vbox), display_intent, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(display_intent, _("perceptual"));
  dt_bauhaus_combobox_add(display_intent, _("relative colorimetric"));
  dt_bauhaus_combobox_add(display_intent, C_("rendering intent", "saturation"));
  dt_bauhaus_combobox_add(display_intent, _("absolute colorimetric"));

  GtkWidget *display_profile = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(display_profile, NULL, _("display profile"));
  gtk_box_pack_start(GTK_BOX(vbox), display_profile, TRUE, TRUE, 0);

  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)profiles->data;
    if(prof->display_pos > -1)
    {
      dt_bauhaus_combobox_add(display_profile, prof->name);
      if(prof->type == darktable.color_profiles->display_type
        && (prof->type != DT_COLORSPACE_FILE
        || !strcmp(prof->filename, darktable.color_profiles->display_filename)))
      {
        dt_bauhaus_combobox_set(display_profile, prof->display_pos);
      }
    }
  }


  char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
  char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
  char *tooltip = g_strdup_printf(_("display ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
  gtk_widget_set_tooltip_text(display_profile, tooltip);
  g_free(system_profile_dir);
  g_free(user_profile_dir);
  g_free(tooltip);


  g_signal_connect(G_OBJECT(display_intent), "value-changed", G_CALLBACK(display_intent_callback), NULL);
  g_signal_connect(G_OBJECT(display_profile), "value-changed", G_CALLBACK(display_profile_callback), NULL);
}

static gboolean _is_custom_image_order_required(dt_view_t *self)
{
  if (darktable.gui)
  {
    const int layout = dt_conf_get_int("plugins/lighttable/layout");
    const int file_manager_layout = 1;

    // only in file manager
    // only in light table
    // only if custom image order is selected
    dt_view_t *current_view = darktable.view_manager->current_view;
    if (layout == file_manager_layout &&
        darktable.collection->params.sort == DT_COLLECTION_SORT_CUSTOM_ORDER &&
        current_view &&
        current_view->view(self) == DT_VIEW_LIGHTTABLE)
    {
      return TRUE;
    }
  }

  return FALSE;
}

static void _register_custom_image_order_drag_n_drop(dt_view_t *self)
{
  // register drag and drop for custom image ordering only
  // if "custom order" is selected and if the view "Lighttable"
  // is active
  if (_is_custom_image_order_required(self))
  {
    // drag and drop for custom order of picture sequence (dnd) and drag&drop of external files/folders into darktable
    gtk_drag_source_set(dt_ui_center(darktable.gui->ui), GDK_BUTTON1_MASK, target_list_internal, n_targets_internal, GDK_ACTION_COPY);

    // check if already connected
    const int is_connected = g_signal_handler_find(dt_ui_center(darktable.gui->ui),
                                     G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA,
                                     0, 0, NULL, G_CALLBACK(_dnd_begin_picture_reorder), (gpointer)self) != 0;

    if (!is_connected)
    {
      g_signal_connect(dt_ui_center(darktable.gui->ui), "drag-begin",    G_CALLBACK(_dnd_begin_picture_reorder), (gpointer)self);
      g_signal_connect(dt_ui_center(darktable.gui->ui), "drag-data-get", G_CALLBACK(_dnd_get_picture_reorder),   (gpointer)self);
      g_signal_connect(dt_ui_center(darktable.gui->ui), "drag_motion",   G_CALLBACK(_dnd_drag_picture_motion),   (gpointer)self);
    }
  }
}

static void _unregister_custom_image_order_drag_n_drop(dt_view_t *self)
{
  if (darktable.gui)
  {
    gtk_drag_source_unset(dt_ui_center(darktable.gui->ui));

    g_signal_handlers_disconnect_matched(dt_ui_center(darktable.gui->ui), G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_dnd_begin_picture_reorder), (gpointer)self);
    g_signal_handlers_disconnect_matched(dt_ui_center(darktable.gui->ui), G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_dnd_get_picture_reorder), (gpointer)self);
    g_signal_handlers_disconnect_matched(dt_ui_center(darktable.gui->ui), G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_dnd_drag_picture_motion), (gpointer)self);
  }
}

static void _dnd_get_picture_reorder(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                    GtkSelectionData *selection_data, guint target_type, guint time,
                                    gpointer data)
{
  GList *selected_images = dt_collection_get_selected(darktable.collection, -1);
  const int32_t mouse_over_id = dt_control_get_mouse_over_id();
  dt_collection_move_before(mouse_over_id, selected_images);

  dt_control_button_released(x, y, GDK_BUTTON1_MASK, 0 & 0xf);
  //gtk_widget_queue_draw(widget);
  _update_collected_images(darktable.view_manager->proxy.lighttable.view);
  g_list_free(selected_images);
}

static void _dnd_begin_picture_reorder(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  const int ts = DT_PIXEL_APPLY_DPI(64);

  GList *selected_images = dt_collection_get_selected(darktable.collection, 1);

  // if we are dragging a single image -> use the thumbnail of that image
  // otherwise use the generic d&d icon
  // TODO: have something pretty in the 2nd case, too.
  if(dt_collection_get_selected_count(NULL) == 1 && selected_images)
  {
    const int imgid = GPOINTER_TO_INT(selected_images->data);

    dt_mipmap_buffer_t buf;
    dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, ts, ts);
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, mip, DT_MIPMAP_BLOCKING, 'r');

    if(buf.buf)
    {
      const int32_t width = buf.width;
      const int32_t height = buf.height;

      if (width > 0 && height > 0)
      {
        for(size_t i = 3; i < (size_t)4 * width * height; i += 4) buf.buf[i] = UINT8_MAX;

        int w = ts, h = ts;
        if(width < height)
          w = (width * ts) / height; // portrait
        else
          h = (height * ts) / width; // landscape

        GdkPixbuf *source = gdk_pixbuf_new_from_data(buf.buf, GDK_COLORSPACE_RGB, TRUE, 8, width,
                                                     height, width * 4, NULL, NULL);
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(source, w, h, GDK_INTERP_HYPER);
        gtk_drag_set_icon_pixbuf(context, scaled, 0, h);

        if(source) g_object_unref(source);
        if(scaled) g_object_unref(scaled);
      }
    }

    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  }

  g_list_free(selected_images);
}

static gboolean _dnd_drag_picture_motion(GtkWidget *dest_button, GdkDragContext *dc, gint x, gint y, guint time, gpointer user_data)
{
  dt_control_queue_redraw_center();
  return FALSE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
