/*
    This file is part of darktable,
    Copyright (C) 2014-2022 darktable developers.

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

/** this is the view for the print module.  */
#include "common/collection.h"
#include "common/cups_print.h"
#include "common/printing.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "views/view.h"
#include "views/view_api.h"

#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE(1)

typedef struct dt_print_t
{
  dt_print_info_t *pinfo;
  dt_images_box *imgs;
  int32_t last_selected;
}
dt_print_t;

const char *name(const dt_view_t *self)
{
  return C_("view", "print");
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_PRINT;
}

static void _print_mipmaps_updated_signal_callback(gpointer instance, int imgid, gpointer user_data)
{
  dt_control_queue_redraw_center();
}

static void _film_strip_activated(const int imgid, void *data)
{
  const dt_view_t *self = (dt_view_t *)data;
  dt_print_t *prt = (dt_print_t *)self->data;

  prt->last_selected = imgid;

  // only select from filmstrip if there is a single image displayed, otherwise
  // we will drag and drop into different areas.

  if(prt->imgs->count != 1) return;

  // if the previous shown image is selected and the selection is unique
  // then we change the selected image to the new one
  if(prt->imgs->box[0].imgid > 0)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT m.imgid"
                                " FROM memory.collected_images as m, main.selected_images as s"
                                " WHERE m.imgid=s.imgid",
                                -1, &stmt, NULL);
    gboolean follow = FALSE;
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      if(sqlite3_column_int(stmt, 0) == prt->imgs->box[0].imgid
         && sqlite3_step(stmt) != SQLITE_ROW)
      {
        follow = TRUE;
      }
    }
    sqlite3_finalize(stmt);
    if(follow)
    {
      dt_selection_select_single(darktable.selection, imgid);
    }
  }

  prt->imgs->box[0].imgid = imgid;

  dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), imgid, TRUE);

  // update the active images list
  g_slist_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = g_slist_prepend(NULL, GINT_TO_POINTER(imgid));
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);

  // force redraw
  dt_control_queue_redraw();
}

static void _view_print_filmstrip_activate_callback(gpointer instance, int imgid, gpointer user_data)
{
  if(imgid > 0) _film_strip_activated(imgid, user_data);
}

static void _view_print_settings(const dt_view_t *view, dt_print_info_t *pinfo, dt_images_box *imgs)
{
  dt_print_t *prt = (dt_print_t *)view->data;

  prt->pinfo = pinfo;
  prt->imgs = imgs;
  dt_control_queue_redraw();
}

static void _drag_and_drop_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                   GtkSelectionData *selection_data, guint target_type, guint time,
                                   gpointer data)
{
  const dt_view_t *self = (dt_view_t *)data;
  dt_print_t *prt = (dt_print_t *)self->data;

  const int bidx = dt_printing_get_image_box(prt->imgs, x, y);

  if(bidx != -1)
    dt_printing_setup_image(prt->imgs, bidx, prt->last_selected,
                            100, 100, ALIGNMENT_CENTER);

  prt->imgs->motion_over = -1;
  dt_control_queue_redraw_center();
}

static gboolean _drag_motion_received(GtkWidget *widget, GdkDragContext *dc,
                                      gint x, gint y, guint time,
                                      gpointer data)
{
  const dt_view_t *self = (dt_view_t *)data;
  dt_print_t *prt = (dt_print_t *)self->data;

  const int bidx = dt_printing_get_image_box(prt->imgs, x, y);
  prt->imgs->motion_over = bidx;

  if(bidx != -1) dt_control_queue_redraw_center();

  return TRUE;
}

void
init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_print_t));

  /* initialize CB to get the print settings from corresponding lib module */
  darktable.view_manager->proxy.print.view = self;
  darktable.view_manager->proxy.print.print_settings = _view_print_settings;
}

void cleanup(dt_view_t *self)
{
  dt_print_t *prt = (dt_print_t *)self->data;
  free(prt);
}

static void expose_print_page(dt_view_t *self, cairo_t *cr,
                              int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_print_t *prt = (dt_print_t *)self->data;

  if(prt->pinfo == NULL)
    return;

  float px=.0f, py=.0f, pwidth=.0f, pheight=.0f;
  float ax=.0f, ay=.0f, awidth=.0f, aheight=.0f;

  gboolean borderless = FALSE;

  dt_get_print_layout(prt->pinfo, width, height,
                      &px, &py, &pwidth, &pheight,
                      &ax, &ay, &awidth, &aheight, &borderless);

  // page w/h
  float pg_width  = prt->pinfo->paper.width;
  float pg_height = prt->pinfo->paper.height;

  // non-printable
  float np_top = prt->pinfo->printer.hw_margin_top;
  float np_left = prt->pinfo->printer.hw_margin_left;
  float np_right = prt->pinfo->printer.hw_margin_right;
  float np_bottom = prt->pinfo->printer.hw_margin_bottom;

  // handle the landscape mode if needed
  if(prt->pinfo->page.landscape)
  {
    float tmp = pg_width;
    pg_width = pg_height;
    pg_height = tmp;

    // rotate the non-printable margins
    tmp       = np_top;
    np_top    = np_right;
    np_right  = np_bottom;
    np_bottom = np_left;
    np_left   = tmp;
  }

  const float pright = px + pwidth;
  const float pbottom = py + pheight;

  // x page -> x display
  // (x / pg_width) * p_width + p_x
  cairo_set_source_rgb (cr, 0.9, 0.9, 0.9);
  cairo_rectangle (cr, px, py, pwidth, pheight);
  cairo_fill (cr);

  // record the screen page dimension. this will be used to compute the actual
  // layout of the areas placed over the page.

  dt_printing_setup_display(prt->imgs,
                            px, py, pwidth, pheight,
                            ax, ay, awidth, aheight,
                            borderless);

  // display non-printable area
  cairo_set_source_rgb (cr, 0.1, 0.1, 0.1);

  const float np1x = px + (np_left / pg_width) * pwidth;
  const float np1y = py + (np_top / pg_height) * pheight;
  const float np2x = pright - (np_right / pg_width) * pwidth;
  const float np2y = pbottom - (np_bottom / pg_height) * pheight;

  // top-left
  cairo_move_to (cr, np1x-10, np1y);
  cairo_line_to (cr, np1x, np1y); cairo_line_to (cr, np1x, np1y-10);
  cairo_stroke (cr);

  // top-right
  // npy = p_y + (np_top / pg_height) * p_height;
  cairo_move_to (cr, np2x+10, np1y);
  cairo_line_to (cr, np2x, np1y); cairo_line_to (cr, np2x, np1y-10);
  cairo_stroke (cr);

  // bottom-left
  cairo_move_to (cr, np1x-10, np2y);
  cairo_line_to (cr, np1x, np2y); cairo_line_to (cr, np1x, np2y+10);
  cairo_stroke (cr);

  // bottom-right
  cairo_move_to (cr, np2x+10, np2y);
  cairo_line_to (cr, np2x, np2y); cairo_line_to (cr, np2x, np2y+10);
  cairo_stroke (cr);

  // clip to this area to ensure that the image won't be larger,
  // this is needed when using negative margin to enlarge the print

  cairo_rectangle (cr, np1x, np1y, np2x-np1x, np2y-np1y);
  cairo_clip(cr);

  cairo_set_source_rgb (cr, 0.77, 0.77, 0.77);
  cairo_rectangle (cr, ax, ay, awidth, aheight);
  cairo_fill (cr);
}

void expose(dt_view_t *self, cairo_t *cri, int32_t width_i, int32_t height_i, int32_t pointerx, int32_t pointery)
{
  // clear the current surface
  dt_gui_gtk_set_source_rgb(cri, DT_GUI_COLOR_PRINT_BG);
  cairo_paint(cri);

  // print page & borders only. Images are displayed in gui_post_expose in print_settings module
  expose_print_page(self, cri, width_i, height_i, pointerx, pointery);
}

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  const dt_print_t *prt = (dt_print_t *)self->data;

  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of first opened image.
  const int32_t mouse_over_id = dt_control_get_mouse_over_id();

  if(prt->imgs->count == 1 && mouse_over_id != prt->imgs->box[0].imgid)
  {
    dt_control_set_mouse_over_id(prt->imgs->box[0].imgid);
  }
  else if(prt->imgs->count > 1)
  {
    const int bidx = dt_printing_get_image_box(prt->imgs, x, y);
    if(bidx == -1)
      dt_control_set_mouse_over_id(-1);
    else if(mouse_over_id != prt->imgs->box[bidx].imgid)
    {
      dt_control_set_mouse_over_id(prt->imgs->box[bidx].imgid);
    }
  }
}

int try_enter(dt_view_t *self)
{
  dt_print_t *prt = (dt_print_t*)self->data;

  //  now check that there is at least one selected image

  const int imgid = dt_act_on_get_main_image();

  if(imgid < 0)
  {
    // fail :(
    dt_control_log(_("no image to open!"));
    return 1;
  }

  // this loads the image from db if needed:
  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  // get image and check if it has been deleted from disk first!

  char imgfilename[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(img->id, imgfilename, sizeof(imgfilename), &from_cache);
  if(!g_file_test(imgfilename, G_FILE_TEST_IS_REGULAR))
  {
    dt_control_log(_("image `%s' is currently unavailable"), img->filename);
    dt_image_cache_read_release(darktable.image_cache, img);
    return 1;
  }
  // and drop the lock again.
  dt_image_cache_read_release(darktable.image_cache, img);

  // we need to setup the selected image
  prt->imgs->imgid_to_load = imgid;

  return 0;
}

void enter(dt_view_t *self)
{
  dt_print_t *prt=(dt_print_t*)self->data;

  /* scroll filmstrip to the first selected image */
  if(prt->imgs->imgid_to_load >= 0)
  {
    // change active image
    dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), prt->imgs->box[0].imgid, TRUE);
    dt_view_active_images_reset(FALSE);
    dt_view_active_images_add(prt->imgs->imgid_to_load, TRUE);
  }

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_print_mipmaps_updated_signal_callback),
                            (gpointer)self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                            G_CALLBACK(_view_print_filmstrip_activate_callback), self);

  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

  GtkWidget *widget = dt_ui_center(darktable.gui->ui);

  gtk_drag_dest_set(widget, GTK_DEST_DEFAULT_ALL,
                    target_list_all, n_targets_all, GDK_ACTION_MOVE);
  g_signal_connect(widget, "drag-data-received", G_CALLBACK(_drag_and_drop_received), self);
  g_signal_connect(widget, "drag-motion", G_CALLBACK(_drag_motion_received), self);

  dt_control_set_mouse_over_id(prt->imgs->imgid_to_load);
}

void leave(dt_view_t *self)
{
  dt_print_t *prt=(dt_print_t*)self->data;

  /* disconnect from mipmap updated signal */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_print_mipmaps_updated_signal_callback),
                               (gpointer)self);

  /* disconnect from filmstrip image activate */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                               G_CALLBACK(_view_print_filmstrip_activate_callback),
                               (gpointer)self);

  dt_printing_clear_boxes(prt->imgs);
//  g_signal_disconnect(widget, "drag-data-received", G_CALLBACK(_drag_and_drop_received));
//  g_signal_disconnect(widget, "drag-motion", G_CALLBACK(_drag_motion_received));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

