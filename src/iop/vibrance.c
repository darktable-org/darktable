/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#ifdef HAVE_GEGL
#include <gegl.h>
#endif
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include <xmmintrin.h>

// NaN-safe clip: NaN compares false and will result in 0.0
#define CLIP(x) (((x)>=0.0)?((x)<=1.0?(x):1.0):0.0)
DT_MODULE(2)

typedef struct dt_iop_vibrance_params_t
{
  float amount;
}
dt_iop_vibrance_params_t;

typedef struct dt_iop_vibrance_gui_data_t
{
  GtkDarktableSlider *amount_scale;
}
dt_iop_vibrance_gui_data_t;

typedef struct dt_iop_vibrance_data_t
{
  float amount;
}
dt_iop_vibrance_data_t;

const char *name()
{
  return _("vibrance");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int
groups ()
{
  return IOP_GROUP_COLOR;
}


void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "vibrance"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_vibrance_gui_data_t *g = (dt_iop_vibrance_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "vibrance",
                              GTK_WIDGET(g->amount_scale));
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_vibrance_data_t *d = (dt_iop_vibrance_data_t *)piece->data;
  const float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

  const float amount = (d->amount*0.01);
  
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out) schedule(static)
#endif
  for (int k=0;k<roi_out->height;k++)
  {
    int offs = k*roi_out->width*ch;
    for(int l=0;l<(roi_out->width*ch);l+=ch)
    {
      /* saturation weight 0 - 1 */
      float sw = 1.0 - fabs(-1 + (fabs(in[offs + l + 1]/128.0) + fabs(in[offs + l + 2]/128.0)));
      float ls = 1.0 - ((amount * sw)*.25);
      float ss = 1.0 + (amount * sw);
      out[offs + l + 0] = in[offs + l + 0] * ls;
      out[offs + l + 1] = in[offs + l + 1] * ss;
      out[offs + l + 2] = in[offs + l + 2] * ss;
      out[offs + l + 3] = in[offs + l + 3];
    }
  }



}

static void
amount_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vibrance_params_t *p = (dt_iop_vibrance_params_t *)self->params;
  p->amount = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_vibrance_params_t *p = (dt_iop_vibrance_params_t *)p1;
  dt_iop_vibrance_data_t *d = (dt_iop_vibrance_data_t *)piece->data;
  d->amount = p->amount;
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_vibrance_data_t));
  memset(piece->data,0,sizeof(dt_iop_vibrance_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_vibrance_gui_data_t *g = (dt_iop_vibrance_gui_data_t *)self->gui_data;
  dt_iop_vibrance_params_t *p = (dt_iop_vibrance_params_t *)module->params;
  dtgtk_slider_set_value(g->amount_scale, p->amount);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_vibrance_params_t));
  module->default_params = malloc(sizeof(dt_iop_vibrance_params_t));
  module->default_enabled = 0;
  module->priority = 396; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_vibrance_params_t);
  module->gui_data = NULL;
  dt_iop_vibrance_params_t tmp = (dt_iop_vibrance_params_t)
  {
    25
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_vibrance_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_vibrance_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_vibrance_gui_data_t));
  dt_iop_vibrance_gui_data_t *g = (dt_iop_vibrance_gui_data_t *)self->gui_data;
  dt_iop_vibrance_params_t *p = (dt_iop_vibrance_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  GtkVBox *vbox =  GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(vbox), TRUE, TRUE, 5);

  g->amount_scale = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 1, p->amount, 0));
  dtgtk_slider_set_format_type(g->amount_scale,DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_label(g->amount_scale,_("vibrance"));
  dtgtk_slider_set_unit(g->amount_scale,"%");
 
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->amount_scale), TRUE, TRUE, 0);
 
  g_object_set(G_OBJECT(g->amount_scale), "tooltip-text", _("the amount of vibrance"), (char *)NULL);

  g_signal_connect (G_OBJECT (g->amount_scale), "value-changed",
                    G_CALLBACK (amount_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}
