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
#ifndef DARKTABLE_IOP_EQUALIZER_H
#define DARKTABLE_IOP_EQUALIZER_H

#include "develop/imageop.h"
#include "gui/draw.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define DT_IOP_EQUALIZER_RES 64
#define DT_IOP_EQUALIZER_BANDS 6
#define DT_IOP_EQUALIZER_MAX_LEVEL 6

typedef struct dt_iop_equalizer_params_t
{
  float equalizer_x[3][DT_IOP_EQUALIZER_BANDS], equalizer_y[3][DT_IOP_EQUALIZER_BANDS];
}
dt_iop_equalizer_params_t;

typedef enum dt_iop_equalizer_channel_t
{
  DT_IOP_EQUALIZER_L = 0,
  DT_IOP_EQUALIZER_a = 1,
  DT_IOP_EQUALIZER_b = 2
}
dt_iop_equalizer_channel_t;

typedef struct dt_iop_equalizer_gui_data_t
{
  dt_draw_curve_t *minmax_curve;        // curve for gui to draw
  GtkHBox *hbox;
  GtkDrawingArea *area;
  GtkComboBox *presets;
  GtkRadioButton *channel_button[3];
  double mouse_x, mouse_y, mouse_pick;
  float mouse_radius;
  dt_iop_equalizer_params_t drag_params;
  int dragging;
  int x_move;
  dt_iop_equalizer_channel_t channel;
  float draw_xs[DT_IOP_EQUALIZER_RES], draw_ys[DT_IOP_EQUALIZER_RES];
  float draw_min_xs[DT_IOP_EQUALIZER_RES], draw_min_ys[DT_IOP_EQUALIZER_RES];
  float draw_max_xs[DT_IOP_EQUALIZER_RES], draw_max_ys[DT_IOP_EQUALIZER_RES];
  float band_hist[DT_IOP_EQUALIZER_BANDS];
  float band_max;
}
dt_iop_equalizer_gui_data_t;

typedef struct dt_iop_equalizer_data_t
{
  dt_draw_curve_t *curve[3];
  int num_levels;
}
dt_iop_equalizer_data_t;

void init(dt_iop_module_t *module);
void cleanup(dt_iop_module_t *module);

void gui_update    (struct dt_iop_module_t *self);
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);

void gui_init     (struct dt_iop_module_t *self);
void gui_cleanup  (struct dt_iop_module_t *self);

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out);

// static gboolean dt_iop_equalizer_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
// static gboolean dt_iop_equalizer_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
// static gboolean dt_iop_equalizer_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
// static gboolean dt_iop_equalizer_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
// static gboolean dt_iop_equalizer_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
// static gboolean dt_iop_equalizer_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
// static void dt_iop_equalizer_button_toggled(GtkToggleButton *togglebutton, gpointer user_data);

#endif
// These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
