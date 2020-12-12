/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
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
  int32_t image_id;
  dt_print_info_t *pinfo;
  gboolean busy;
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

  // if the previous shown image is selected and the selection is unique
  // then we change the selected image to the new one
  if(prt->image_id > 0)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT m.imgid FROM memory.collected_images as m, main.selected_images as s "
                                "WHERE m.imgid=s.imgid",
                                -1, &stmt, NULL);
    gboolean follow = FALSE;
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      if(sqlite3_column_int(stmt, 0) == prt->image_id && sqlite3_step(stmt) != SQLITE_ROW)
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

  prt->image_id = imgid;

  dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), imgid, TRUE);

  // update the active images list
  g_slist_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = NULL;
  darktable.view_manager->active_images
      = g_slist_append(darktable.view_manager->active_images, GINT_TO_POINTER(imgid));
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);

  // force redraw
  dt_control_queue_redraw();
}

static void _view_print_filmstrip_activate_callback(gpointer instance, int imgid, gpointer user_data)
{
  if(imgid > 0) _film_strip_activated(imgid, user_data);
}

static void _view_print_settings(const dt_view_t *view, dt_print_info_t *pinfo)
{
  dt_print_t *prt = (dt_print_t *)view->data;

  prt->pinfo = pinfo;
  dt_control_queue_redraw();
}

void
init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_print_t));
  dt_print_t *prt = (dt_print_t *)self->data;

  prt->image_id = -1;

  /* initialize CB to get the print settings from corresponding lib module */
  darktable.view_manager->proxy.print.view = self;
  darktable.view_manager->proxy.print.print_settings = _view_print_settings;
}

void cleanup(dt_view_t *self)
{
  dt_print_t *prt = (dt_print_t *)self->data;
  free(prt);
}

static gboolean _expose_again(gpointer user_data)
{
  dt_control_queue_redraw_center();
  return FALSE;
}

static void expose_print_page(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_print_t *prt = (dt_print_t *)self->data;
  int32_t px=0, py=0, pwidth=0, pheight=0;
  int32_t ax=0, ay=0, awidth=0, aheight=0;
  int32_t ix=0, iy=0, iwidth=0, iheight=0;
  int32_t iwpix=0, ihpix=0;

  if (prt->pinfo == NULL)
    return;

  dt_get_print_layout (prt->image_id, prt->pinfo, width, height,
                       &iwpix, &ihpix,
                       &px, &py, &pwidth, &pheight,
                       &ax, &ay, &awidth, &aheight,
                       &ix, &iy, &iwidth, &iheight);
  // page w/h
  double pg_width  = prt->pinfo->paper.width;
  double pg_height = prt->pinfo->paper.height;

  // non-printable
  double np_top = prt->pinfo->printer.hw_margin_top;
  double np_left = prt->pinfo->printer.hw_margin_left;
  double np_right = prt->pinfo->printer.hw_margin_right;
  double np_bottom = prt->pinfo->printer.hw_margin_bottom;

  // handle the landscape mode if needed
  if (prt->pinfo->page.landscape)
  {
    double tmp = pg_width;
    pg_width = pg_height;
    pg_height = tmp;

    // rotate the non-printable margins
    tmp       = np_top;
    np_top    = np_right;
    np_right  = np_bottom;
    np_bottom = np_left;
    np_left   = tmp;
  }

  const int32_t pright = px + pwidth;
  const int32_t pbottom = py + pheight;

  // x page -> x display
  // (x / pg_width) * p_width + p_x
  cairo_set_source_rgb (cr, 0.9, 0.9, 0.9);
  cairo_rectangle (cr, px, py, pwidth, pheight);
  cairo_fill (cr);

  // display non-printable area
  cairo_set_source_rgb (cr, 0.1, 0.1, 0.1);

  const int np1x = px + (np_left / pg_width) * pwidth;
  const int np1y = py + (np_top / pg_height) * pheight;
  const int np2x = pright - (np_right / pg_width) * pwidth;
  const int np2y = pbottom - (np_bottom / pg_height) * pheight;

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

  // clip to this area to ensure that the image won't be larger, this is needed when using negative margin to enlarge the print

  cairo_rectangle (cr, np1x, np1y, np2x-np1x, np2y-np1y);
  cairo_clip(cr);

  cairo_set_source_rgb (cr, 0.77, 0.77, 0.77);
  cairo_rectangle (cr, ax, ay, awidth, aheight);
  cairo_fill (cr);

  cairo_surface_t *surf = NULL;
  const int res = dt_view_image_get_surface(prt->image_id, iwidth, iheight, &surf, TRUE);
  if(res)
  {
    // if the image is missing, we reload it again
    g_timeout_add(250, _expose_again, NULL);
    if(!prt->busy) dt_control_log_busy_enter();
    prt->busy = TRUE;
  }
  else
  {
    const float scaler = 1.0f / darktable.gui->ppd_thb;
    cairo_translate(cr, ix, iy);
    cairo_scale(cr, scaler, scaler);
    cairo_set_source_surface(cr, surf, 0, 0);
    cairo_paint(cr);
    cairo_surface_destroy(surf);
    if(prt->busy) dt_control_log_busy_leave();
    prt->busy = FALSE;
  }
}

void expose(dt_view_t *self, cairo_t *cri, int32_t width_i, int32_t height_i, int32_t pointerx, int32_t pointery)
{
  const dt_print_t *prt=(dt_print_t*)self->data;

  // clear the current surface
  dt_gui_gtk_set_source_rgb(cri, DT_GUI_COLOR_PRINT_BG);
  cairo_paint(cri);

  if (prt->image_id > 0)
    expose_print_page (self, cri, width_i, height_i, pointerx, pointery);
}

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  const dt_print_t *prt = (dt_print_t *)self->data;

  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  int32_t mouse_over_id = dt_control_get_mouse_over_id();
  if(mouse_over_id != prt->image_id)
  {
    mouse_over_id = prt->image_id;
    dt_control_set_mouse_over_id(mouse_over_id);
  }
}
void mouse_leave(dt_view_t *self)
{
  // if we are not hovering over a thumbnail in the filmstrip -> show metadata of opened image.
  const dt_print_t *prt = (dt_print_t *)self->data;
  dt_control_set_mouse_over_id(prt->image_id);
}

int try_enter(dt_view_t *self)
{
  dt_print_t *prt=(dt_print_t*)self->data;

  //  now check that there is at least one selected image

  prt->image_id = -1;

  int imgid = dt_view_get_image_to_act_on();

  if(imgid < 0)
  {
    // fail :(
    dt_control_log(_("no image to open !"));
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
  prt->image_id = imgid;
  return 0;
}

void enter(dt_view_t *self)
{
  dt_print_t *prt=(dt_print_t*)self->data;

  /* scroll filmstrip to the first selected image */
  if(prt->image_id)
  {
    // change active image
    dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), prt->image_id, TRUE);
    dt_view_active_images_reset(FALSE);
    dt_view_active_images_add(prt->image_id, TRUE);
  }

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_print_mipmaps_updated_signal_callback),
                            (gpointer)self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                            G_CALLBACK(_view_print_filmstrip_activate_callback), self);

  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

  dt_control_set_mouse_over_id(prt->image_id);
}

void leave(dt_view_t *self)
{
  /* disconnect from mipmap updated signal */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_print_mipmaps_updated_signal_callback),
                               (gpointer)self);

  /* disconnect from filmstrip image activate */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                               G_CALLBACK(_view_print_filmstrip_activate_callback),
                               (gpointer)self);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
