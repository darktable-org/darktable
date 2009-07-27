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
#include "iop/temperature.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"

/* Coefficients of rational functions of degree 5 fitted per color channel to
 * the linear RGB coordinates of the range 1000K-12000K of the Planckian locus
 * with the 20K step. Original CIE-xy data from
 *
 * http://www.aim-dtp.net/aim/technology/cie_xyz/k2xy.txt
 *
 * converted to the linear RGB space assuming the ITU-R BT.709-5/sRGB primaries
 */
static const float dt_iop_temperature_rgb_r55[][12] =
{
  {
     6.9389923563552169e-01,  2.7719388100974670e+03,
     2.0999316761104289e+07, -4.8889434162208414e+09,
    -1.1899785506796783e+07, -4.7418427686099203e+04,
     1.0000000000000000e+00,  3.5434394338546258e+03,
    -5.6159353379127791e+05,  2.7369467137870544e+08,
     1.6295814912940913e+08,  4.3975072422421846e+05
  },
  {
     9.5417426141210926e-01,  2.2041043287098860e+03,
    -3.0142332673634286e+06, -3.5111986367681120e+03,
    -5.7030969525354260e+00,  6.1810926909962016e-01,
     1.0000000000000000e+00,  1.3728609973644000e+03,
     1.3099184987576159e+06, -2.1757404458816318e+03,
    -2.3892456292510311e+00,  8.1079012401293249e-01
  },
  {
    -7.1151622540856201e+10,  3.3728185802339764e+16,
    -7.9396187338868539e+19,  2.9699115135330123e+22,
    -9.7520399221734228e+22, -2.9250107732225114e+20,
     1.0000000000000000e+00,  1.3888666482167408e+16,
     2.3899765140914549e+19,  1.4583606312383295e+23,
     1.9766018324502894e+22,  2.9395068478016189e+18
  }
};



static void convert_k_to_rgb (float temperature, float *rgb)
{
  int channel;

  if (temperature < DT_IOP_LOWEST_TEMPERATURE)  temperature = DT_IOP_LOWEST_TEMPERATURE;
  if (temperature > DT_IOP_HIGHEST_TEMPERATURE) temperature = DT_IOP_HIGHEST_TEMPERATURE;

  /* Evaluation of an approximation of the Planckian locus in linear RGB space
   * by rational functions of degree 5 using Horner's scheme
   * f(x) =  (p1*x^5 + p2*x^4 + p3*x^3 + p4*x^2 + p5*x + p6) /
   *            (x^5 + q1*x^4 + q2*x^3 + q3*x^2 + q4*x + q5)
   */
  for (channel = 0; channel < 3; channel++)
  {
    float nomin, denom;
    int   deg;

    nomin = dt_iop_temperature_rgb_r55[channel][0];
    for (deg = 1; deg < 6; deg++)
      nomin = nomin * temperature + dt_iop_temperature_rgb_r55[channel][deg];

    denom = dt_iop_temperature_rgb_r55[channel][6];
    for (deg = 1; deg < 6; deg++)
      denom = denom * temperature + dt_iop_temperature_rgb_r55[channel][6 + deg];

    rgb[channel] = nomin / denom;
  }
}

#if 0
  convert_k_to_rgb (o->original_temperature, original_temperature_rgb);
  convert_k_to_rgb (o->intended_temperature, intended_temperature_rgb);

  coeffs[0] = original_temperature_rgb[0] / intended_temperature_rgb[0];
  coeffs[1] = original_temperature_rgb[1] / intended_temperature_rgb[1];
  coeffs[2] = original_temperature_rgb[2] / intended_temperature_rgb[2];



  out_pixel[0] = in_pixel[0] * coeffs[0];
      out_pixel[1] = in_pixel[1] * coeffs[1];
      out_pixel[2] = in_pixel[2] * coeffs[2];

#endif

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, int x, int y, float scale, int width, int height)
{
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
  for(int k=0;k<width*height;k++)
  {
    for(int c=0;c<3;c++) out[c] = in[c]*d->coeffs[c];
    out += 3; in += 3;
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
  gegl_node_set(piece->input, "original_temperature", p->temperature_in, "intended_temperature", p->temperature_out, NULL);
#else
  // build gamma table in pipeline piece from committed params:
  float original_temperature_rgb[3], intended_temperature_rgb[3];
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  convert_k_to_rgb (p->temperature_in,  original_temperature_rgb);
  convert_k_to_rgb (p->temperature_out, intended_temperature_rgb);

  d->coeffs[0] = original_temperature_rgb[0] / intended_temperature_rgb[0];
  d->coeffs[1] = original_temperature_rgb[1] / intended_temperature_rgb[1];
  d->coeffs[2] = original_temperature_rgb[2] / intended_temperature_rgb[2];
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
  dt_iop_temperature_params_t *default_params = (dt_iop_temperature_params_t *)self->default_params;
  piece->input = piece->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:color-temperature", "original_temperature", default_params->temperature_in, "intended_temperature", default_params->temperature_out, NULL);
#else
  piece->data = malloc(sizeof(dt_iop_temperature_data_t));
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
  piece->data = NULL;
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)module->params;
  gtk_range_set_value(GTK_RANGE(g->scale1), p->temperature_in);
  gtk_range_set_value(GTK_RANGE(g->scale2), p->temperature_out);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_temperature_data_t));
  module->params = malloc(sizeof(dt_iop_temperature_params_t));
  module->default_params = malloc(sizeof(dt_iop_temperature_params_t));
  module->params_size = sizeof(dt_iop_temperature_params_t);
  module->gui_data = NULL;
  dt_iop_temperature_params_t tmp = (dt_iop_temperature_params_t){6500.0, 6500.0};
  memcpy(module->params, &tmp, sizeof(dt_iop_temperature_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_temperature_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_temperature_gui_data_t));
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));
  g->label1 = GTK_LABEL(gtk_label_new("original temperature"));
  g->label2 = GTK_LABEL(gtk_label_new("intended temperature"));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  g->scale1 = GTK_HSCALE(gtk_hscale_new_with_range(DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE, 10.));
  g->scale2 = GTK_HSCALE(gtk_hscale_new_with_range(DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE, 10.));
  gtk_scale_set_digits(GTK_SCALE(g->scale1), 0);
  gtk_scale_set_digits(GTK_SCALE(g->scale2), 0);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale1), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale2), GTK_POS_LEFT);
  gtk_range_set_value(GTK_RANGE(g->scale1), p->temperature_in);
  gtk_range_set_value(GTK_RANGE(g->scale2), p->temperature_out);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (temperature_in_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (temperature_out_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

void temperature_in_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  p->temperature_in = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}

void temperature_out_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  p->temperature_out = gtk_range_get_value(range);
  dt_dev_add_history_item(darktable.develop, self);
}
