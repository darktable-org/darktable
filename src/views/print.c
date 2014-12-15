/*
    This file is part of darktable,
    copyright (c) 2014 pascal obry.

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
#include "common/darktable.h"
#include "common/collection.h"
#include "views/view.h"
#include "develop/develop.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "common/debug.h"
#include "common/cups_print.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef struct dt_print_t
{
  int32_t image_id;
  int32_t iwidth, iheight;
  dt_print_info_t *pinfo;
}
dt_print_t;

const char
*name(dt_view_t *self)
{
  return _("print");
}

uint32_t view(dt_view_t *self)
{
  return DT_VIEW_PRINT;
}

static void _print_mipmaps_updated_signal_callback(gpointer instance, gpointer user_data)
{
  dt_control_queue_redraw_center();
}

static void _film_strip_activated(const int imgid, void *data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_print_t *prt = (dt_print_t *)self->data;

  prt->image_id = imgid;
  prt->iwidth = prt->iheight = 0;

  //  guess the image orientation

  prt->pinfo->page.landscape = TRUE;

  const dt_image_t *img = dt_image_cache_read_get(darktable.image_cache, prt->image_id);

  if(img)
  {
    dt_mipmap_buffer_t buf;
    dt_mipmap_cache_read_get(darktable.mipmap_cache, &buf, prt->image_id, DT_MIPMAP_3, DT_MIPMAP_BEST_EFFORT);

    if (buf.width > buf.height)
      prt->pinfo->page.landscape = TRUE;
    else
      prt->pinfo->page.landscape = FALSE;

    if(buf.buf)
      dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);

    dt_image_cache_read_release(darktable.image_cache, img);
  }

  dt_view_filmstrip_scroll_to_image(darktable.view_manager, imgid, FALSE);
  // record the imgid to display when going back to lighttable
  dt_view_lighttable_set_position(darktable.view_manager, dt_collection_image_offset(imgid));

  // force redraw
  dt_control_queue_redraw();
}

static void _view_print_filmstrip_activate_callback(gpointer instance,gpointer user_data)
{
  int32_t imgid = 0;
  if ((imgid=dt_view_filmstrip_get_activated_imgid(darktable.view_manager))>0)
    _film_strip_activated(imgid,user_data);
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

  /* initialize CB to get the print settings from corresponding lib module */
  darktable.view_manager->proxy.print.view = self;
  darktable.view_manager->proxy.print.print_settings = _view_print_settings;

  /* prefetch next few from first selected image on. */
  dt_view_filmstrip_prefetch();
}

void cleanup(dt_view_t *self)
{
  dt_print_t *prt = (dt_print_t *)self->data;
  free(prt);
}

static void expose_print_page(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_print_t *prt = (dt_print_t *)self->data;
  int32_t px=0, py=0, pwidth=0, pheight=0;
  int32_t ax=0, ay=0, awidth=0, aheight=0;
  int32_t ix=0, iy=0, iwidth=0, iheight=0;

  if (prt->pinfo == NULL)
    return;

  dt_get_print_layout (prt->image_id, prt->pinfo, width, height,
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

  // top-left
  int npx = px + (np_left / pg_width) * pwidth;
  int npy = py + (np_top / pg_height) * pheight;
  cairo_move_to (cr, npx, npy); cairo_line_to (cr, npx-10, npy);
  cairo_move_to (cr, npx, npy); cairo_line_to (cr, npx, npy-10);
  cairo_stroke (cr);

  // top-right
  npx = pright - (np_right / pg_width) * pwidth;
  // npy = p_y + (np_top / pg_height) * p_height;
  cairo_move_to (cr, npx, npy); cairo_line_to (cr, npx+10, npy);
  cairo_move_to (cr, npx, npy); cairo_line_to (cr, npx, npy-10);
  cairo_stroke (cr);

  // bottom-left
  npx = px + (np_left / pg_width) * pwidth;
  npy = pbottom - (np_bottom / pg_height) * pheight;
  cairo_move_to (cr, npx, npy); cairo_line_to (cr, npx-10, npy);
  cairo_move_to (cr, npx, npy); cairo_line_to (cr, npx, npy+10);
  cairo_stroke (cr);

  // bottom-right
  npx = pright - (np_right / pg_width) * pwidth;
  // npy = p_bottom - (np_bottom / pg_height) * p_height;
  cairo_move_to (cr, npx, npy); cairo_line_to (cr, npx+10, npy);
  cairo_move_to (cr, npx, npy); cairo_line_to (cr, npx, npy+10);
  cairo_stroke (cr);

  cairo_set_source_rgb (cr, 0.77, 0.77, 0.77);
  cairo_rectangle (cr, ax, ay, awidth, aheight);
  cairo_fill (cr);

  dt_view_image_only_expose(prt->image_id, cr, iwidth, iheight, ix, iy);
}

void expose(dt_view_t *self, cairo_t *cri, int32_t width_i, int32_t height_i, int32_t pointerx, int32_t pointery)
{
  dt_print_t *prt=(dt_print_t*)self->data;

  // clear the current surface
  cairo_set_source_rgb (cri, 0.1, 0.1, 0.1);
  cairo_paint(cri);

  if (prt->image_id > 0)
    expose_print_page (self, cri, width_i, height_i, pointerx, pointery);
}

int try_enter(dt_view_t *self)
{
#if 0
  // enter only if there is some printer available
  if (is_printer_available())
    return 0;
  else
  {
    dt_control_log(_("there is no printer available, cannot print!"));
    return 1;
  }
#endif
  return 0;
}

void enter(dt_view_t *self)
{
  dt_print_t *prt=(dt_print_t*)self->data;

  prt->image_id = -1;

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_print_mipmaps_updated_signal_callback),
                            (gpointer)self);

  dt_control_signal_connect(darktable.signals,
                            DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE,
                            G_CALLBACK(_view_print_filmstrip_activate_callback),
                            self);

  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

  // prefetch next few from first selected image on.
  dt_view_filmstrip_prefetch();

  /* scroll filmstrip to the first selected image */
  GList *selected_images = dt_collection_get_selected(darktable.collection, 1);
  if(selected_images)
  {
    int imgid = GPOINTER_TO_INT(selected_images->data);
    dt_view_filmstrip_scroll_to_image(darktable.view_manager, imgid, FALSE);
    prt->image_id = imgid;
  }
  g_list_free(selected_images);
}

void leave(dt_view_t *self)
{
  /* disconnect from mipmap updated signal */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_print_mipmaps_updated_signal_callback),
                               (gpointer)self);

  /* disconnect from filmstrip image activate */
  dt_control_signal_disconnect(darktable.signals,
                               G_CALLBACK(_view_print_filmstrip_activate_callback),
                               (gpointer)self);
}

static gboolean film_strip_key_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                     GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *m = darktable.view_manager->proxy.filmstrip.module;
  gboolean vs = dt_lib_is_visible(m);
  dt_lib_set_visible(m, !vs);
  return TRUE;
}

void init_key_accels(dt_view_t *self)
{
  // Film strip shortcuts
  dt_accel_register_view(self, NC_("accel", "toggle film strip"), GDK_KEY_f, GDK_CONTROL_MASK);
}

void connect_key_accels(dt_view_t *self)
{
  GClosure *closure;

  // Film strip shortcuts
  closure = g_cclosure_new(G_CALLBACK(film_strip_key_accel), (gpointer)self, NULL);
  dt_accel_connect_view(self, "toggle film strip", closure);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
