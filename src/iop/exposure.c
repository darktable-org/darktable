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

DT_MODULE(2)

const char *name()
{
  return _("exposure");
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  float *in =  (float *)i;
  float *out = (float *)o;
  float black = d->black;
  float white = exp2f(-d->exposure)*self->dev->image->maximum;

  if(piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW && (self->dev->image->flags & DT_IMAGE_THUMBNAIL))
  { // path for already exposed preview buffer
    white /= self->dev->image->maximum;
    black -= self->dev->image->black;
  }
  float scale = 1.0/(white - black); 
  // if(fabsf(d->gain - 1.0) < 0.001)
  {
    for(int k=0;k<roi_out->width*roi_out->height;k++)
    {
      for(int i=0;i<3;i++) out[i] = fminf(1.0, fmaxf(0.0, (in[i]-black)*scale));
      out += 3; in += 3;
    }
  }
#if 0 // non-linear doesn't make sense before white balance :(
  else
  {
    for(int k=0;k<roi_out->width*roi_out->height;k++)
    {
      for(int i=0;i<3;i++) out[i] = powf(fmaxf(0.0, fminf(1.0, (in[i]-black))*scale), d->gain);
      out += 3; in += 3;
    }
  }
#endif
  for(int k=0;k<3;k++)
    piece->pipe->processed_maximum[k] = powf(fmaxf(0.0, fminf(1.0, (piece->pipe->processed_maximum[k]-black))*scale), d->gain);
}

void reload_defaults (struct dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *p  = (dt_iop_exposure_params_t *)self->default_params;
  dt_iop_exposure_params_t *fp = (dt_iop_exposure_params_t *)self->factory_params;
  int cp = memcmp(self->default_params, self->params, self->params_size);
  fp->black = p->black = self->dev->image->black;
  // FIXME: this function is called from render threads, but these values
  // should be written by gui threads. but it is only a matter of gui synching..
  if(!cp) memcpy(self->params, self->default_params, self->params_size);
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
  dtgtk_slider_set_value(g->scale1, p->black/self->dev->image->maximum);
  dtgtk_slider_set_value(g->scale2, p->exposure);
  // dtgtk_slider_set_value(g->scale3, p->gain);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_exposure_data_t));
  module->params = malloc(sizeof(dt_iop_exposure_params_t));
  module->default_params = malloc(sizeof(dt_iop_exposure_params_t));
  if(dt_image_is_ldr(module->dev->image)) module->default_enabled = 0;
  else                                    module->default_enabled = 1;
  module->priority = 150;
  module->params_size = sizeof(dt_iop_exposure_params_t);
  module->gui_data = NULL;
  dt_iop_exposure_params_t tmp = (dt_iop_exposure_params_t){0., 1., 1.0};

  tmp.black = module->dev->image->black;
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

void dt_iop_exposure_set_white(struct dt_iop_module_t *self, const float white)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale2), -log2f(fmaxf(0.001, white/self->dev->image->maximum)));
}

float dt_iop_exposure_get_white(struct dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  return exp2f(-p->exposure)*self->dev->image->maximum;
}

static void
autoexp_callback (GtkToggleButton *button, dt_iop_module_t *self)
{
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  if(darktable.gui->reset) return;
  self->request_color_pick = gtk_toggle_button_get_active(button);
  self->dev->gui_module = self;
  gtk_widget_set_sensitive(GTK_WIDGET(g->autoexpp), gtk_toggle_button_get_active(button));
}

static void
white_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  p->exposure = dtgtk_slider_get_value(slider);
  const float white = exp2f(-p->exposure)*self->dev->image->maximum;
  float black = dtgtk_slider_get_value(g->scale1)*self->dev->image->maximum;
  if(white < black) dtgtk_slider_set_value(g->scale1, white);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
black_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  p->black = dtgtk_slider_get_value(slider)*self->dev->image->maximum;
  float white = exp2f(-dtgtk_slider_get_value(g->scale2))*self->dev->image->maximum;
  if(white < p->black) dtgtk_slider_set_value(g->scale2, - log2f(p->black/self->dev->image->maximum));
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

  self->request_color_pick = 0;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(TRUE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(TRUE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(_("black")));
  g->label2 = GTK_LABEL(gtk_label_new(_("exposure")));
  // g->label3 = GTK_LABEL(gtk_label_new(_("gain")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  // gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  // gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range( DARKTABLE_SLIDER_VALUE, -.5, 1.0, .001,p->black/self->dev->image->maximum,3));
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("adjust the black level"), NULL);
  
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range( DARKTABLE_SLIDER_VALUE, -3.0, 6.0, .02, p->exposure,3));
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("adjust the exposure correction [ev]"), NULL);
  
  
  // g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_VALUE,0.0, 2.0, .005, p->gain,3));
  // gtk_object_set(GTK_OBJECT(g->scale3), "tooltip-text", _("leave black and white,\nbut compress brighter\nvalues (non-linear)"), NULL);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  // gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);

  g->autoexp  = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("auto")));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoexp), FALSE);
  g->autoexpp = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_VALUE,0.0, 0.2, .001, 0.01,3));
  gtk_object_set(GTK_OBJECT(g->autoexpp), "tooltip-text", _("percentage of bright values clipped out"), NULL);
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

