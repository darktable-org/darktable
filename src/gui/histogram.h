/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DARKTABLE_GUI_HISTOGRAM_H
#define DARKTABLE_GUI_HISTOGRAM_H

#include <gtk/gtk.h>
#include <inttypes.h>
#include "develop/imageop.h"

typedef struct dt_gui_histogram_t
{
  dt_iop_module_t *exposure;
  float white, black;
  void  (*set_white)(dt_iop_module_t *exp, const float white);
  float (*get_white)(dt_iop_module_t *exp);
  void  (*set_black)(dt_iop_module_t *exp, const float black);
  float (*get_black)(dt_iop_module_t *exp);
  int32_t dragging;
  int32_t button_down_x, button_down_y;
  int32_t highlight;
}
dt_gui_histogram_t;

void dt_gui_histogram_init(dt_gui_histogram_t *n, GtkWidget *widget);
void dt_gui_histogram_cleanup(dt_gui_histogram_t *n);
gboolean dt_gui_histogram_expose(GtkWidget *widget, GdkEventExpose *event, dt_gui_histogram_t *n);
gboolean dt_gui_histogram_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
gboolean dt_gui_histogram_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_gui_histogram_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_gui_histogram_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
gboolean dt_gui_histogram_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
void dt_gui_histogram_draw_8(cairo_t *cr, float *hist, int32_t channel);

#endif
