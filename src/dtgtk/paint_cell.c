/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "dtgtk/paint_cell.h"
#include "common/darktable.h"
#include "gui/gtk.h"

G_DEFINE_TYPE(GtkDarktablePaintCell, dtgtk_paint_cell, GTK_TYPE_CELL_RENDERER)

// icon edge length, derived from the widget's line height
static int _paint_cell_compute_size(GtkWidget *widget)
{
  int s = DT_PIXEL_APPLY_DPI(12);
  if(widget)
  {
    PangoContext *pctx = gtk_widget_get_pango_context(widget);
    const PangoFontDescription *fd = pango_context_get_font_description(pctx);
    PangoFontMetrics *m = pango_context_get_metrics(pctx, fd, NULL);
    const int line_h = (pango_font_metrics_get_ascent(m)
                        + pango_font_metrics_get_descent(m)) / PANGO_SCALE;
    pango_font_metrics_unref(m);
    if(line_h > 0) s = line_h;
  }
  return s;
}

static void _paint_cell_get_preferred_width(GtkCellRenderer *r,
                                            GtkWidget *widget,
                                            gint *minimum_size,
                                            gint *natural_size)
{
  (void)r;
  const int s = _paint_cell_compute_size(widget);
  if(minimum_size) *minimum_size = s;
  if(natural_size) *natural_size = s;
}

static void _paint_cell_get_preferred_height(GtkCellRenderer *r,
                                             GtkWidget *widget,
                                             gint *minimum_size,
                                             gint *natural_size)
{
  (void)r;
  const int s = _paint_cell_compute_size(widget);
  if(minimum_size) *minimum_size = s;
  if(natural_size) *natural_size = s;
}

static void _paint_cell_render(GtkCellRenderer *r,
                               cairo_t *cr,
                               GtkWidget *widget,
                               const GdkRectangle *bg_area,
                               const GdkRectangle *cell_area,
                               GtkCellRendererState flags)
{
  (void)bg_area; (void)flags;
  GtkDarktablePaintCell *self = DTGTK_PAINT_CELL(r);
  if(!self->paint) return;

  GdkRGBA fg;
  GtkStyleContext *ctx = gtk_widget_get_style_context(widget);
  gtk_style_context_get_color(ctx, gtk_widget_get_state_flags(widget), &fg);

  const int mx = cell_area->width  / 5;
  const int my = cell_area->height / 5;

  cairo_save(cr);
  gdk_cairo_set_source_rgba(cr, &fg);
  self->paint(cr,
              cell_area->x + mx, cell_area->y + my,
              cell_area->width  - 2 * mx,
              cell_area->height - 2 * my,
              self->paint_flags, self->paint_data);
  cairo_restore(cr);
}

static void dtgtk_paint_cell_class_init(GtkDarktablePaintCellClass *klass)
{
  GtkCellRendererClass *cr_class = GTK_CELL_RENDERER_CLASS(klass);
  cr_class->get_preferred_width  = _paint_cell_get_preferred_width;
  cr_class->get_preferred_height = _paint_cell_get_preferred_height;
  cr_class->render               = _paint_cell_render;
}

static void dtgtk_paint_cell_init(GtkDarktablePaintCell *self)
{
  self->paint = NULL;
  self->paint_flags = 0;
  self->paint_data = NULL;
}

GtkCellRenderer *dtgtk_paint_cell_new(DTGTKCairoPaintIconFunc paint,
                                      gint paint_flags,
                                      void *paint_data)
{
  GtkDarktablePaintCell *cell = g_object_new(dtgtk_paint_cell_get_type(), NULL);
  cell->paint = paint;
  cell->paint_flags = paint_flags;
  cell->paint_data = paint_data;
  return GTK_CELL_RENDERER(cell);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
