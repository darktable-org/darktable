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

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  float *in =  (float *)i;
  float *out = (float *)o;
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
    out[0] = 100.0*powf(fmaxf(0.0, (in[0]-d->black))*d->scale, d->gain);
    out[1] = in[1];
    out[2] = in[2];
    out += 3; in += 3;
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
  fprintf(stderr, "implement exposure process gegl version! \n");
#else
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  d->black = 100.0*(powf(2.0, p->black) - 1.0);
  d->gain = 2.0 - p->gain;
  d->scale = 1.0/(100.0*(powf(2.0, p->white) - 1.0) - p->black); 
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
  gtk_range_set_value(GTK_RANGE(g->scale1), p->black);
  gtk_range_set_value(GTK_RANGE(g->scale2), -log2f(p->white));
  gtk_range_set_value(GTK_RANGE(g->scale3), p->gain);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_exposure_data_t));
  module->params = malloc(sizeof(dt_iop_exposure_params_t));
  module->default_params = malloc(sizeof(dt_iop_exposure_params_t));
  module->default_enabled = 0;
  module->priority = 60;
  module->params_size = sizeof(dt_iop_exposure_params_t);
  module->gui_data = NULL;
  dt_iop_exposure_params_t tmp = (dt_iop_exposure_params_t){0., 1., 1.0};
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
  gtk_range_set_value(GTK_RANGE(g->scale2), -log2f(white));
}

float dt_iop_exposure_get_white(struct dt_iop_module_t *self)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  return p->white;
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

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new("black"));
  g->label2 = GTK_LABEL(gtk_label_new("exposure"));
  g->label3 = GTK_LABEL(gtk_label_new("gain"));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  g->scale1 = GTK_HSCALE(gtk_hscale_new_with_range(-.5, 1.0, .001));
  g->scale2 = GTK_HSCALE(gtk_hscale_new_with_range(-3.0, 3.0, .02));
  g->scale3 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 2.0, .005));
  gtk_scale_set_digits(GTK_SCALE(g->scale1), 3);
  gtk_scale_set_digits(GTK_SCALE(g->scale2), 3);
  gtk_scale_set_digits(GTK_SCALE(g->scale3), 3);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale1), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale2), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale3), GTK_POS_LEFT);
  gtk_range_set_value(GTK_RANGE(g->scale1), p->black);
  gtk_range_set_value(GTK_RANGE(g->scale2), -log2f(p->white));
  gtk_range_set_value(GTK_RANGE(g->scale3), p->gain);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (black_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (white_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (gain_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  darktable.gui->histogram.exposure  = NULL;
  darktable.gui->histogram.set_white = NULL;
  darktable.gui->histogram.get_white = NULL;
  free(self->gui_data);
  self->gui_data = NULL;
}

void white_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  p->white = exp2f(-gtk_range_get_value(range));
  float black = gtk_range_get_value(GTK_RANGE(g->scale1));
  if(p->white < black) gtk_range_set_value(GTK_RANGE(g->scale1), p->white);
  dt_dev_add_history_item(darktable.develop, self);
}

void black_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  p->black = gtk_range_get_value(range);
  float white = gtk_range_get_value(GTK_RANGE(g->scale2));
  if(white < p->black) gtk_range_set_value(GTK_RANGE(g->scale2), - log2f(p->black));
  dt_dev_add_history_item(darktable.develop, self);
}

void gain_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  p->gain = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}
