/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#ifndef DARKTABLE_IOP_CURVE_EDITOR_H
#define DARKTABLE_IOP_CURVE_EDITOR_H

#include "develop/imageop.h"
#include "gui/draw.h"
#include <gtk/gtk.h>
#include <inttypes.h>

typedef struct dt_iop_levels_params_t
{
  float levels[3];
  int levels_preset;
}
dt_iop_levels_params_t;

typedef enum dt_iop_levels_pick_t
{
  NONE,
  BLACK,
  GREY,
  WHITE
}
dt_iop_levels_pick_t;

typedef struct dt_iop_levels_gui_data_t
{
  GtkHBox *hbox;
  GtkDrawingArea *area;
  GtkLabel *label;
  double mouse_x, mouse_y;
  int dragging, handle_move;
  float drag_start_percentage;
  dt_iop_levels_pick_t current_pick;
  GtkToggleButton *activeToggleButton;
  float last_picked_color;
  double pick_xy_positions[3][2];
}
dt_iop_levels_gui_data_t;

typedef struct dt_iop_levels_data_t
{
  float in_low;
  float in_high;
  float in_inv_gamma;
  float lut[0x10000];
}
dt_iop_levels_data_t;

typedef struct dt_iop_levels_global_data_t
{
  int kernel_levels;
}
dt_iop_levels_global_data_t;

void init(dt_iop_module_t *module);
void cleanup(dt_iop_module_t *module);

void gui_update    (struct dt_iop_module_t *self);
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);

void gui_init     (struct dt_iop_module_t *self);
void gui_cleanup  (struct dt_iop_module_t *self);

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out);

static gboolean dt_iop_levels_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
static gboolean dt_iop_levels_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
static gboolean dt_iop_levels_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_levels_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_levels_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean dt_iop_levels_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
static void dt_iop_levels_pick_black_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self);
static void dt_iop_levels_pick_grey_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self);
static void dt_iop_levels_pick_white_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self);
static void dt_iop_levels_autoadjust_callback(GtkRange *range, dt_iop_module_t *self);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
