/*
    This file is part of darktable,
    copyright (c) 2009--2014 johannes hanika.

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

#define DT_IOP_TONECURVE_RES 64
#define DT_IOP_TONECURVE_MAXNODES 20

typedef enum tonecurve_channel_t
{
  ch_L    = 0,
  ch_a    = 1,
  ch_b    = 2,
  ch_max  = 3
}
tonecurve_channel_t;

typedef struct dt_iop_tonecurve_node_t
{
  float x;
  float y;
}
dt_iop_tonecurve_node_t;


// parameter structure of tonecurve 1st version, needed for use in legacy_params()
typedef struct dt_iop_tonecurve_params1_t
{
  float tonecurve_x[6], tonecurve_y[6];
  int tonecurve_preset;
}
dt_iop_tonecurve_params1_t;

// parameter structure of tonecurve 3rd version, needed for use in legacy_params()
typedef struct dt_iop_tonecurve_params3_t
{
  dt_iop_tonecurve_node_t tonecurve[3][DT_IOP_TONECURVE_MAXNODES];  // three curves (L, a, b) with max number of nodes
  int tonecurve_nodes[3];
  int tonecurve_type[3];
  int tonecurve_autoscale_ab;
  int tonecurve_preset;
}
dt_iop_tonecurve_params3_t;

typedef struct dt_iop_tonecurve_params_t
{
  dt_iop_tonecurve_node_t tonecurve[3][DT_IOP_TONECURVE_MAXNODES];  // three curves (L, a, b) with max number of nodes
  int tonecurve_nodes[3];
  int tonecurve_type[3];
  int tonecurve_autoscale_ab;
  int tonecurve_preset;
  int tonecurve_unbound_ab;
}
dt_iop_tonecurve_params_t;



typedef struct dt_iop_tonecurve_gui_data_t
{
  dt_draw_curve_t *minmax_curve[3];        // curves for gui to draw
  int minmax_curve_nodes[3];
  int minmax_curve_type[3];
  GtkHBox *hbox;
  GtkDrawingArea *area;
  GtkSizeGroup *sizegroup;
  GtkWidget *autoscale_ab;
  GtkNotebook* channel_tabs;
  tonecurve_channel_t channel;
  double mouse_x, mouse_y;
  int selected;
  float draw_xs[DT_IOP_TONECURVE_RES], draw_ys[DT_IOP_TONECURVE_RES];
  float draw_min_xs[DT_IOP_TONECURVE_RES], draw_min_ys[DT_IOP_TONECURVE_RES];
  float draw_max_xs[DT_IOP_TONECURVE_RES], draw_max_ys[DT_IOP_TONECURVE_RES];
}
dt_iop_tonecurve_gui_data_t;

typedef struct dt_iop_tonecurve_data_t
{
  dt_draw_curve_t *curve[3];     // curves for gegl nodes and pixel processing
  int curve_nodes[3];            // number of nodes
  int curve_type[3];             // curve style (e.g. CUBIC_SPLINE)
  float table[3][0x10000];       // precomputed look-up tables for tone curve
  float unbounded_coeffs_L[3];   // approximation for extrapolation of L
  float unbounded_coeffs_ab[12]; // approximation for extrapolation of ab (left and right)
  int autoscale_ab;
  int unbound_ab;
}
dt_iop_tonecurve_data_t;

typedef struct dt_iop_tonecurve_global_data_t
{
  int kernel_tonecurve;
}
dt_iop_tonecurve_global_data_t;

void init(dt_iop_module_t *module);
void cleanup(dt_iop_module_t *module);

void gui_update    (struct dt_iop_module_t *self);
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);

void gui_init     (struct dt_iop_module_t *self);
void gui_cleanup  (struct dt_iop_module_t *self);

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
