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
#include "iop/gamma.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"

const char *name()
{
  return C_("modulename", "gamma");
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_gamma_data_t *d = (dt_iop_gamma_data_t *)piece->data;
  float *in = (float *)i;
  uint8_t *out = (uint8_t *)o;
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
    for(int c=0;c<3;c++) out[2-c] = d->table[(uint16_t)CLAMP((int)(0xfffful*in[c]), 0, 0xffff)];
    out += 4; in += 3;
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_gamma_params_t *p = (dt_iop_gamma_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
  gegl_node_set(piece->input, "linear_value", p->linear, "gamma_value", p->gamma, NULL);
  // gegl_node_set(piece->input, "value", p->gamma, NULL);
#else
  // build gamma table in pipeline piece from committed params:
  dt_iop_gamma_data_t *d = (dt_iop_gamma_data_t *)piece->data;
  float a, b, c, g;
  if(p->linear<1.0)
  {
    g = p->gamma*(1.0-p->linear)/(1.0-p->gamma*p->linear);
    a = 1.0/(1.0+p->linear*(g-1));
    b = p->linear*(g-1)*a;
    c = powf(a*p->linear+b, g)/p->linear;
  }
  else
  {
    a = b = g = 0.0;
    c = 1.0;
  }
  for(int k=0;k<0x10000;k++)
  {
    int32_t tmp;
    if (k<0x10000*p->linear) tmp = MIN(c*k, 0xFFFF);
    else tmp = MIN(powf(a*k/0x10000+b, g)*0x10000, 0xFFFF);
    d->table[k] = tmp>>8;
  }
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
  dt_iop_gamma_params_t *default_params = (dt_iop_gamma_params_t *)self->default_params;
  piece->input = piece->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:dt-gamma", "linear_value", default_params->linear, "gamma_value", default_params->gamma, NULL);
  // piece->input = piece->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:gamma", "value", default_params->gamma, NULL);
#else
  piece->data = malloc(sizeof(dt_iop_gamma_data_t));
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
  dt_iop_gamma_gui_data_t *g = (dt_iop_gamma_gui_data_t *)self->gui_data;
  dt_iop_gamma_params_t *p = (dt_iop_gamma_params_t *)module->params;
  gtk_range_set_value(GTK_RANGE(g->scale1), p->linear);
  gtk_range_set_value(GTK_RANGE(g->scale2), p->gamma);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_gamma_data_t));
  module->params = malloc(sizeof(dt_iop_gamma_params_t));
  module->default_params = malloc(sizeof(dt_iop_gamma_params_t));
  module->params_size = sizeof(dt_iop_gamma_params_t);
  module->gui_data = NULL;
  module->priority = 1000;
  module->hide_enable_button = 1;
  dt_iop_gamma_params_t tmp = (dt_iop_gamma_params_t){0.45, 0.1};
  memcpy(module->params, &tmp, sizeof(dt_iop_gamma_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_gamma_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_gamma_gui_data_t));
  dt_iop_gamma_gui_data_t *g = (dt_iop_gamma_gui_data_t *)self->gui_data;
  dt_iop_gamma_params_t *p = (dt_iop_gamma_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(C_("gammaslider", "linear")));
  g->label2 = GTK_LABEL(gtk_label_new(C_("gammaslider", "gamma")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  g->scale1 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 1.0, 0.01));
  g->scale2 = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 1.0, 0.01));
  gtk_scale_set_digits(GTK_SCALE(g->scale1), 2);
  gtk_scale_set_digits(GTK_SCALE(g->scale2), 2);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale1), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale2), GTK_POS_LEFT);
  gtk_range_set_value(GTK_RANGE(g->scale1), p->linear);
  gtk_range_set_value(GTK_RANGE(g->scale2), p->gamma);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (linear_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (gamma_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

void gamma_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_gamma_params_t *p = (dt_iop_gamma_params_t *)self->params;
  p->gamma = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

void linear_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_gamma_params_t *p = (dt_iop_gamma_params_t *)self->params;
  p->linear = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}
