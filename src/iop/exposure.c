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

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, int x, int y, float scale, int width, int height)
{
  dt_iop_exposure_data_t *d = (dt_iop_exposure_data_t *)piece->data;
  float *in =  (float *)i;
  float *out = (float *)o;
  for(int k=0;k<width*height;k++)
  {
    out[0] = fmaxf(0.0, (in[0]-d->black))*d->scale;
    if(in[0] > 0)
    {
      out[1] = out[0]*in[1]/in[0];
      out[2] = out[0]*in[2]/in[0];
    }
    else
    {
      out[1] = in[1];
      out[2] = in[2];
    }
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
  d->black = p->black;
  d->scale = 100.0/(p->white-p->black); 
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
  gtk_range_set_value(GTK_RANGE(g->scale2), p->white);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_exposure_data_t));
  module->params = malloc(sizeof(dt_iop_exposure_params_t));
  module->default_params = malloc(sizeof(dt_iop_exposure_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_exposure_params_t);
  module->gui_data = NULL;
  dt_iop_exposure_params_t tmp = (dt_iop_exposure_params_t){0., 100.};
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

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_exposure_gui_data_t));
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new("black"));
  g->label2 = GTK_LABEL(gtk_label_new("white"));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  g->scale1 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 100.0, 1.0));
  g->scale2 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 100.0, 1.0));
  gtk_scale_set_digits(GTK_SCALE(g->scale1), 1);
  gtk_scale_set_digits(GTK_SCALE(g->scale2), 1);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale1), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale2), GTK_POS_LEFT);
  gtk_range_set_value(GTK_RANGE(g->scale1), p->black);
  gtk_range_set_value(GTK_RANGE(g->scale2), p->white);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (black_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (white_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

void white_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_exposure_gui_data_t *g = (dt_iop_exposure_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)self->params;
  p->white = gtk_range_get_value(range);
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
  if(white < p->black) gtk_range_set_value(GTK_RANGE(g->scale2), p->black);
  dt_dev_add_history_item(darktable.develop, self);
}
