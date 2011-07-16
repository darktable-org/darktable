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
#include "iop/exposure.h"
#include "common/opencl.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "dtgtk/resetlabel.h"

#define exposure2white(x)	exp2f(-(x))
#define white2exposure(x)	-dt_log2f(fmaxf(0.001, x))

DT_MODULE(2)

const char *name()
{
  return _("exposure");
}

int
groups ()
{
  return IOP_GROUP_BASIC;
}

void init_key_accels()
{
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/exposure/black");
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/exposure/exposure");
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/exposure/auto-exposure");
}
int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return 4*sizeof(float);
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  dt_iop_exposure_global_data_t *gd = (dt_iop_exposure_global_data_t *)self->data;

  cl_int err = -999;
  const float black = d->black;
  const float white = exposure2white(d->exposure);
  const float scale = 1.0/(white - black);
  const int devid = piece->pipe->devid;
  size_t sizes[] = {roi_in->width, roi_in->height, 1};
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_exposure, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_exposure, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_exposure, 2, sizeof(float), (void *)&black);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_exposure, 3, sizeof(float), (void *)&scale);
  err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_exposure, sizes);
  if(err != CL_SUCCESS) goto error;
  for(int k=0; k<3; k++) piece->pipe->processed_maximum[k] *= scale;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_exposure] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  const float black = d->black;
  const float white = exposure2white(d->exposure);
  const int ch = piece->colors;
  const float scale = 1.0/(white - black);
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out,i,o) schedule(static)
#endif
  for(int k=0; k<roi_out->height; k++)
  {
    const float *in = ((float *)i) + ch*k*roi_out->width;
    float *out = ((float *)o) + ch*k*roi_out->width;
    for (int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
      for(int i=0; i<3; i++)
        out[i] = (in[i]-black)*scale;
  }
  for(int k=0; k<3; k++) piece->pipe->processed_maximum[k] *= scale;
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)p1;
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  d->black = p->black;
  d->gain = 2.0 - p->gain;
  d->exposure = p->exposure;
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_exposure_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)module->params;
  dtgtk_slider_set_value(g->black, p->black);
  dtgtk_slider_set_value(g->exposure, p->exposure);
  // dtgtk_slider_set_value(g->scale3, p->gain);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_exposure_params_t));
  module->default_params = malloc(sizeof(dt_iop_exposure_params_t));
  module->default_enabled = 0;
  module->priority = 200; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_exposure_params_t);
  module->gui_data = NULL;
  dt_iop_exposure_params_t tmp = (dt_iop_exposure_params_t)
  {
    0., 1., 1.0
  };

  tmp.black = 0.0f;
  tmp.exposure = 0.0f;

  memcpy(module->params, &tmp, sizeof(dt_iop_exposure_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_exposure_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // from programs.conf: basic.cl
  dt_iop_exposure_global_data_t *gd = (dt_iop_exposure_global_data_t *)malloc(sizeof(dt_iop_exposure_global_data_t));
  module->data = gd;
  gd->kernel_exposure = dt_opencl_create_kernel(darktable.opencl, program, "exposure");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_exposure_global_data_t *gd = (dt_iop_exposure_global_data_t *)module->data;
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_exposure);
  free(module->data);
  module->data = NULL;
}

static void exposure_set_black(struct dt_iop_module_t *self, const float black);
static void autoexp_disable(dt_iop_module_t *self);

static void exposure_set_white(struct dt_iop_module_t *self, const float white)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  float exposure = white2exposure(white);
  if (p->exposure == exposure) return;

  p->exposure = exposure;
  if (p->black >= white) exposure_set_black(self, white-0.01);

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  darktable.gui->reset = 1;
  dtgtk_slider_set_value(DTGTK_SLIDER(g->exposure), p->exposure);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dt_iop_exposure_set_white(struct dt_iop_module_t *self, const float white)
{
  autoexp_disable(self);
  exposure_set_white(self, white);
}

static float dt_iop_exposure_get_white(struct dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  return exposure2white(p->exposure);
}

static void exposure_set_black(struct dt_iop_module_t *self, const float black)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  float b = fmaxf(0.0f, black);
  if (p->black == b) return;

  p->black = b;
  if (p->black >= exposure2white(p->exposure))
  {
    exposure_set_white(self, p->black+0.01);
  }

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  darktable.gui->reset = 1;
  dtgtk_slider_set_value(DTGTK_SLIDER(g->black), p->black);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dt_iop_exposure_set_black(struct dt_iop_module_t *self, const float black)
{
  autoexp_disable(self);
  exposure_set_black(self, black);
}

static float dt_iop_exposure_get_black(struct dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  return p->black;
}

static void
autoexp_disable(dt_iop_module_t *self)
{
  if (!self->request_color_pick) return;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoexp), FALSE);
  self->request_color_pick = 0;
}

static void
autoexp_callback (GtkToggleButton *button, dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  if(darktable.gui->reset) return;
  self->request_color_pick = gtk_toggle_button_get_active(button);
  dt_iop_request_focus(self);
  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), gtk_toggle_button_get_active(button));
}

static void
autoexpp_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(!self->request_color_pick) return;

  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  const float white = fmaxf(fmaxf(self->picked_color_max[0], self->picked_color_max[1]), self->picked_color_max[2])
                      * (1.0-dtgtk_slider_get_value(DTGTK_SLIDER(g->autoexpp)));
  exposure_set_white(self, white);
}

static void
exposure_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  autoexp_disable(self);
  const float exposure = dtgtk_slider_get_value(slider);
  dt_iop_exposure_set_white(self, exposure2white(exposure));
}

static void
black_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  const float black = dtgtk_slider_get_value(slider);
  dt_iop_exposure_set_black(self, black);
}

#if 0
static void
gain_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  p->gain = dtgtk_slider_get_value(DTGTK_SLIDER(slider));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

static gboolean
expose (GtkWidget *widget, GdkEventExpose *event, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  if(self->picked_color_max[0] < 0) return FALSE;
  if(!self->request_color_pick) return FALSE;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;

  const float white = fmaxf(fmaxf(self->picked_color_max[0], self->picked_color_max[1]), self->picked_color_max[2])
                      * (1.0-dtgtk_slider_get_value(DTGTK_SLIDER(g->autoexpp)));
  const float black = fminf(fminf(self->picked_color_min[0], self->picked_color_min[1]), self->picked_color_min[2]);

  exposure_set_white(self, white);
  exposure_set_black(self, black);
  return FALSE;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_exposure_gui_data_t));
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  // register with histogram
  darktable.gui->histogram.exposure = self;
  darktable.gui->histogram.set_white = dt_iop_exposure_set_white;
  darktable.gui->histogram.get_white = dt_iop_exposure_get_white;
  darktable.gui->histogram.set_black = dt_iop_exposure_set_black;
  darktable.gui->histogram.get_black = dt_iop_exposure_get_black;

  self->request_color_pick = 0;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(TRUE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);

  g->black = DTGTK_SLIDER(dtgtk_slider_new_with_range( DARKTABLE_SLIDER_BAR, -0.1, 0.1, .001, p->black, 3));
  g_object_set(G_OBJECT(g->black), "tooltip-text", _("adjust the black level"), (char *)NULL);
  dtgtk_slider_set_label(g->black,_("black"));
  dtgtk_slider_set_accel(g->black,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/exposure/black");

  g->exposure = DTGTK_SLIDER(dtgtk_slider_new_with_range( DARKTABLE_SLIDER_BAR, -9.0, 9.0, .02, p->exposure, 3));
  g_object_set(G_OBJECT(g->exposure), "tooltip-text", _("adjust the exposure correction"), (char *)NULL);
  dtgtk_slider_set_label(g->exposure,_("exposure"));
  dtgtk_slider_set_unit(g->exposure,"EV");
  dtgtk_slider_set_accel(g->exposure,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/exposure/exposure");

  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->black), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->exposure), TRUE, TRUE, 0);

  g->autoexp  = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("auto")));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoexp), FALSE);
  g->autoexpp = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 0.2, .001, 0.01,3));
  g_object_set(G_OBJECT(g->autoexpp), "tooltip-text", _("percentage of bright values clipped out"), (char *)NULL);
  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), FALSE);
  dtgtk_slider_set_accel(g->autoexpp,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/exposure/auto-exposure");

  GtkHBox *hbox = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->autoexp), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->autoexpp), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  darktable.gui->reset = 1;
  self->gui_update(self);
  darktable.gui->reset = 0;

  g_signal_connect (G_OBJECT (g->black), "value-changed",
                    G_CALLBACK (black_callback), self);
  g_signal_connect (G_OBJECT (g->exposure), "value-changed",
                    G_CALLBACK (exposure_callback), self);
  // g_signal_connect (G_OBJECT (g->scale3), "value-changed",
  // G_CALLBACK (gain_callback), self);
  g_signal_connect (G_OBJECT (g->autoexpp), "value-changed",
                    G_CALLBACK (autoexpp_callback), self);
  g_signal_connect (G_OBJECT (g->autoexp), "toggled",
                    G_CALLBACK (autoexp_callback), self);
  g_signal_connect (G_OBJECT(self->widget), "expose-event",
                    G_CALLBACK(expose), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  darktable.gui->histogram.exposure  = NULL;
  darktable.gui->histogram.set_white = NULL;
  darktable.gui->histogram.get_white = NULL;
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
