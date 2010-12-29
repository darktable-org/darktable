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
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "dtgtk/resetlabel.h"

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

int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(pipe->type != DT_DEV_PIXELPIPE_PREVIEW && module->dev->image->filters) return sizeof(float);
  else return 4*sizeof(float);
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  const float black = d->black;
  const float white = exp2f(-d->exposure);
  const int ch = piece->colors;
  const float scale = 1.0/(white - black); 
  const float *const in = (float *)i;
  float *const out = (float *)o;
  if(piece->pipe->type != DT_DEV_PIXELPIPE_PREVIEW && self->dev->image->filters)
  { // raw image which needs demosaic later on.
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out) schedule(static)
#endif
    for(int k=0;k<roi_out->width*roi_out->height;k++)
      out[k] = fmaxf(0.0f, (in[k]-black)*scale);
  }
  else
  { // std float image:
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out) schedule(static)
#endif
    for(int k=0;k<roi_out->width*roi_out->height;k++)
      for(int i=0;i<3;i++) out[ch*k+i] = fmaxf(0.0f, (in[ch*k+i]-black)*scale);
  }
  for(int k=0;k<3;k++) piece->pipe->processed_maximum[k] *= scale;
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
  fprintf(stderr, "implement exposure process gegl version! \n");
#else
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  d->black = p->black;
  d->gain = 2.0 - p->gain;
  d->exposure = p->exposure;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
  dt_iop_exposure_params_t *default_params = (dt_iop_exposure_params_t *)self->default_params;
  // piece->input = piece->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:dt-gamma", "linear_value", default_params->linear, "gamma_value", default_params->gamma, NULL);
#else
  piece->data = malloc(sizeof(dt_iop_exposure_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
  // no free necessary, no data is alloc'ed
#else
  free(piece->data);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->black);
  dtgtk_slider_set_value(g->scale2, p->exposure);
  // dtgtk_slider_set_value(g->scale3, p->gain);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_exposure_params_t));
  module->default_params = malloc(sizeof(dt_iop_exposure_params_t));
  module->default_enabled = 0;
  module->priority = 170;
  module->params_size = sizeof(dt_iop_exposure_params_t);
  module->gui_data = NULL;
  dt_iop_exposure_params_t tmp = (dt_iop_exposure_params_t){0., 1., 1.0};

  tmp.black = 0.0f;
  tmp.exposure = 0.0f;

  memcpy(module->params, &tmp, sizeof(dt_iop_exposure_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_exposure_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void dt_iop_exposure_set_white(struct dt_iop_module_t *self, const float white)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale2), -dt_log2f(fmaxf(0.001, white)));
}

static float dt_iop_exposure_get_white(struct dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  return exp2f(-p->exposure);
}

static void dt_iop_exposure_set_black(struct dt_iop_module_t *self, const float black)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale1), fmaxf(0.0, black));
}

static float dt_iop_exposure_get_black(struct dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  return p->black;
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
white_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  p->exposure = dtgtk_slider_get_value(slider);
  // these lines seem to produce bad black points during auto exposure:
  // dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  // const float white = exp2f(-p->exposure);
  // float black = dtgtk_slider_get_value(g->scale1);
  // if(white < black) dtgtk_slider_set_value(g->scale1, white);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
black_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  p->black = dtgtk_slider_get_value(slider);
  float white = exp2f(-dtgtk_slider_get_value(g->scale2));
  if(white < p->black) dtgtk_slider_set_value(g->scale2, - dt_log2f(p->black));
  dt_dev_add_history_item(darktable.develop, self);
}

#if 0
static void
gain_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  p->gain = dtgtk_slider_get_value(DTGTK_SLIDER(slider));
  dt_dev_add_history_item(darktable.develop, self);
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
  dt_iop_exposure_set_white(self, white);
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
  g->vbox1 = GTK_VBOX(gtk_vbox_new(TRUE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(TRUE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  GtkWidget *widget;

  widget = dtgtk_reset_label_new(_("black"), self, &p->black, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), widget, TRUE, TRUE, 0);

  widget = dtgtk_reset_label_new(_("exposure"), self, &p->exposure, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), widget, TRUE, TRUE, 0);

  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range( DARKTABLE_SLIDER_BAR, -.5, 1.0, .001, p->black, 3));
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("adjust the black level"), (char *)NULL);
  
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range( DARKTABLE_SLIDER_BAR, -9.0, 9.0, .02, p->exposure, 3));
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("adjust the exposure correction [ev]"), (char *)NULL);
  
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);

  g->autoexp  = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("auto")));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoexp), FALSE);
  g->autoexpp = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 0.2, .001, 0.01,3));
  gtk_object_set(GTK_OBJECT(g->autoexpp), "tooltip-text", _("percentage of bright values clipped out"), (char *)NULL);
  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), FALSE);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->autoexp), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->autoexpp), TRUE, TRUE, 0);

  darktable.gui->reset = 1;
  self->gui_update(self);
  darktable.gui->reset = 0;

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (black_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (white_callback), self);
  // g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    // G_CALLBACK (gain_callback), self);
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

