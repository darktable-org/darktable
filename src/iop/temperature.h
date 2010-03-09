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

#ifndef DT_IOP_COLOR_TEMPERATURE
#define DT_IOP_COLOR_TEMPERATURE
// plug-in frontend for gegl:color-temperature: (in sRGB)

#include "dtgtk/slider.h"
#include "develop/imageop.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define DT_IOP_LOWEST_TEMPERATURE     3000
#define DT_IOP_HIGHEST_TEMPERATURE   12000


static const float dt_iop_temperature_rgb_r55[][12];

typedef struct dt_iop_temperature_params_t
{
  float temp_out;
  float coeffs[3];
}
dt_iop_temperature_params_t;

typedef struct dt_iop_temperature_gui_data_t
{
  GtkVBox *vbox1, *vbox2;
  GtkLabel *label1, *label2;
  GtkDarktableSlider *scale_k, *scale_tint, *scale_k_out, *scale_r, *scale_g, *scale_b;
  GtkComboBox *presets;
  GtkSpinButton *finetune;
  int preset_cnt;
  int preset_num[50];
}
dt_iop_temperature_gui_data_t;

typedef struct dt_iop_temperature_data_t
{
  float coeffs[3];
}
dt_iop_temperature_data_t;

void init(dt_iop_module_t *module);
void cleanup(dt_iop_module_t *module);

void gui_update    (struct dt_iop_module_t *self);
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);

void gui_init     (struct dt_iop_module_t *self);
void gui_cleanup  (struct dt_iop_module_t *self);

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out);

static void tint_callback     (GtkDarktableSlider *slider, gpointer user_data);
static void temp_callback     (GtkDarktableSlider *slider, gpointer user_data);
static void temp_out_callback (GtkDarktableSlider *slider, gpointer user_data);
static void rgb_callback      (GtkDarktableSlider *slider, gpointer user_data);
static void presets_changed   (GtkComboBox *widget, gpointer user_data);
static void finetune_changed  (GtkSpinButton *widget, gpointer user_data);

#endif
