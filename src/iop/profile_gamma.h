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
#ifndef DARKTABLE_IOP_PROFILE_GAMMA_H
#define DARKTABLE_IOP_PROFILE_GAMMA_H

#include "develop/imageop.h"
#include "dtgtk/slider.h"
#include <gtk/gtk.h>
#include <inttypes.h>

typedef struct dt_iop_profile_gamma_params_t
{
  float gamma, linear;
}
dt_iop_profile_gamma_params_t;

typedef struct dt_iop_profile_gamma_gui_data_t
{
  GtkVBox *vbox1, *vbox2;
  GtkLabel *label1, *label2;
  GtkDarktableSlider *scale1, *scale2;
}
dt_iop_profile_gamma_gui_data_t;

typedef struct dt_iop_gamma_data_t
{
  float table[0x10000];
}
dt_iop_profile_gamma_data_t;

void init(dt_iop_module_t *module);
void cleanup(dt_iop_module_t *module);

void gui_update    (struct dt_iop_module_t *self);
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void reset_params  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);

void gui_init     (struct dt_iop_module_t *self);
void gui_cleanup  (struct dt_iop_module_t *self);

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out);

static void gamma_callback  (GtkDarktableSlider *slider, gpointer user_data);
static void linear_callback (GtkDarktableSlider *slider, gpointer user_data);

#endif
// These editor modelines have been set for all relevant files by tools/update_modelines.sh
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
