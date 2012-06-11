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
#ifndef DARKTABLE_IOP_DCURVE_EDITOR_H
#define DARKTABLE_IOP_DCURVE_EDITOR_H

#include "common/colorspaces.h"
#include "develop/imageop.h"
#include "gui/draw.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define DT_IOP_DENSITYCURVE_RES 64
#define MAX_ZONE_SYSTEM_SIZE	24
#define MAX_DENSITY_SYSTEM_SIZE	14
#define GRAY18 0.184186518512444
//#define LabMIN 1.0f
#define LabMIN 0.1f
#define DsMAX 4.0
#define LUT_VALUES 0
#define LUT_COEFFS 1

typedef struct Point
{
  float x;
  float y;
} Point;

typedef struct dt_iop_densitycurve_params_t
{
  int densitycurve_preset;
  int size;
  int spline_type;
  int lut_type;
  int scale_saturation;
  Point points[MAX_DENSITY_SYSTEM_SIZE+2];
}
dt_iop_densitycurve_params_t;

/** gui params. */
typedef struct dt_iop_zonesystem_params_t
{
  int size;
  float zone[MAX_ZONE_SYSTEM_SIZE+1];
}
dt_iop_zonesystem_params_t;

typedef struct Gcurve
{
  int *n_points;
  dt_draw_curve_t *minmax_curve;        // curve for gui to draw
  Point *points;
}
Gcurve;

typedef struct dt_iop_densitycurve_gui_data_t
{
  dt_draw_curve_t *minmax_curve;        // curve for gui to draw
  GtkHBox *hbox;
  GtkDrawingArea *area;
  GtkLabel *label;
  GtkComboBox *spline_type;
  GtkComboBox *calc_type;
  GtkWidget   *zones;
  Gcurve Curve;
  GtkCheckButton *scale_sat;
  double mouse_x, mouse_y;
  int selected, dragging, x_move, numpoints;
  double selected_offset, selected_y, selected_min, selected_max;
  float draw_xs[DT_IOP_DENSITYCURVE_RES], draw_ys[DT_IOP_DENSITYCURVE_RES];
  //float draw_min_xs[DT_IOP_DENSITYCURVE_RES], draw_min_ys[DT_IOP_DENSITYCURVE_RES];
  //float draw_max_xs[DT_IOP_DENSITYCURVE_RES], draw_max_ys[DT_IOP_DENSITYCURVE_RES];
  dt_iop_zonesystem_params_t *zonesystem_params;

}
dt_iop_densitycurve_gui_data_t;

typedef struct dt_iop_densitycurve_data_t
{
  dt_draw_curve_t *curve;      // curve for gegl nodes and pixel processing
  float table[0x10000];        // precomputed look-up table for tone curve
  cmsHTRANSFORM *xformi;
  cmsHTRANSFORM *xformo;
  cmsHPROFILE input;
  cmsHPROFILE Lab;
  int lut_type;
  int scale_saturation;
}
dt_iop_densitycurve_data_t;

typedef struct dt_iop_densitycurve_global_data_t
{
  int kernel_densitycurve;
}
dt_iop_densitycurve_global_data_t;

static gboolean dt_iop_densitycurve_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
static gboolean dt_iop_densitycurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_densitycurve_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_densitycurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
static gboolean dt_iop_densitycurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
int curve_get_closest_point (Gcurve *curve, float x);
static gboolean dt_iop_densitycurve_keypress_notify(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static gboolean dt_iop_densitycurve_on_focus_event(GtkWidget *widget, GdkEventFocus *event, gpointer user_data);
static gboolean dt_iop_zonesystem_bar_expose (GtkWidget *widget, dt_iop_zonesystem_params_t *p);
#endif
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
