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
#include "libraw/libraw.h"
#include "iop/wb_presets.c"

/** this wraps gegl:temperature plus some additional whitebalance adjustments. */

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


const char *name()
{
  return C_("modulename", "temperature");
}

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

// binary search inversion inspired by ufraw's RGB_to_Temperature:
static void convert_rgb_to_k(float rgb[3], const float temp_out, float *temp, float *tint)
{
  float tmin, tmax, tmp[3], original_temperature_rgb[3], intended_temperature_rgb[3];
  for(int k=0;k<3;k++) tmp[k] = rgb[k];
  tmin = DT_IOP_LOWEST_TEMPERATURE;
  tmax = DT_IOP_HIGHEST_TEMPERATURE;
  convert_k_to_rgb (temp_out,  intended_temperature_rgb);
  for (*temp=(tmax+tmin)/2; tmax-tmin>10; *temp=(tmax+tmin)/2)
  {
    convert_k_to_rgb (*temp, original_temperature_rgb);

    tmp[0] = intended_temperature_rgb[0] / original_temperature_rgb[0];
    tmp[1] = intended_temperature_rgb[1] / original_temperature_rgb[1];
    tmp[2] = intended_temperature_rgb[2] / original_temperature_rgb[2];
    
    if (tmp[2]/tmp[0] < rgb[2]/rgb[0])
      tmax = *temp;
    else
      tmin = *temp;
  }
  *tint =  (rgb[1]/rgb[0]) / (tmp[1]/tmp[0]);
}


void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  float *in  = (float *)i;
  float *out = (float *)o;
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
    for(int c=0;c<3;c++) out[c] = in[c]*d->coeffs[c];
    out += 3; in += 3;
  }
  // get mean rgb from preview pipe.
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  if(self->dev->gui_attached && piece->iscale != 1.0 && self->dev->preview_pipe && g->grayboxmode)
  {
    int box[4];
    float rgb[3];
    in = (float *)i;
    for(int k=0;k<3;k++) rgb[k] = 0.0f;
    for(int k=0;k<4;k+=2) box[k] = MIN(roi_in->width -1, MAX(0, g->graybox[k]*roi_in->width));
    for(int k=1;k<4;k+=2) box[k] = MIN(roi_in->height-1, MAX(0, g->graybox[k]*roi_in->height));
    const float w = 1.0/((box[3]-box[1]+1)*(box[2]-box[0]+1));
    for(int j=box[1];j<=box[3];j++) for(int i=box[0];i<=box[2];i++)
    {
      for(int k=0;k<3;k++) rgb[k] += w*in[3*(roi_in->width*j + i) + k];
    }
    for(int k=0;k<3;k++) g->grayrgb[k] = rgb[k];
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
  gegl_node_set(piece->input, "original_temperature", 5000.0, "intended_temperature", p->temperature, NULL);
#else
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  for(int k=0;k<3;k++) d->coeffs[k]  = p->coeffs[k];
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  if(self->dev->gui_attached && piece->iscale != 1.0 && self->dev->preview_pipe && g->grayboxmode)
  {
    for(int k=0;k<4;k++) d->graybox[k] = g->graybox[k];
  }
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
  dt_iop_temperature_params_t *default_params = (dt_iop_temperature_params_t *)self->default_params;
  piece->input = piece->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:color-temperature", "original_temperature", 5000.0, "intended_temperature", default_params->temperature, NULL);
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
  float temp, tint, mul[3];
  for(int k=0;k<3;k++) mul[k] = p->coeffs[k]*g->cam_mul[k];
  convert_rgb_to_k(p->coeffs, p->temp_out, &temp, &tint);
  gtk_range_set_value(GTK_RANGE(g->scale_k_out), p->temp_out);
  gtk_range_set_value(GTK_RANGE(g->scale_r), mul[0]);
  gtk_range_set_value(GTK_RANGE(g->scale_g), mul[1]);
  gtk_range_set_value(GTK_RANGE(g->scale_b), mul[2]);
  gtk_range_set_value(GTK_RANGE(g->scale_k), temp);
  gtk_range_set_value(GTK_RANGE(g->scale_tint), tint);
  g->grayboxmode = 0;
  if(fabsf(p->coeffs[0]-1.0) + fabsf(p->coeffs[1]-1.0) + fabsf(p->coeffs[2]-1.0) < 0.01)
    gtk_combo_box_set_active(g->presets, 0);
  else
    gtk_combo_box_set_active(g->presets, -1);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_temperature_data_t));
  module->params = malloc(sizeof(dt_iop_temperature_params_t));
  module->default_params = malloc(sizeof(dt_iop_temperature_params_t));
  module->default_enabled = 0;
  module->priority = 200;
  module->params_size = sizeof(dt_iop_temperature_params_t);
  module->gui_data = NULL;
  dt_iop_temperature_params_t tmp = (dt_iop_temperature_params_t){0, 5000.0, {1.0, 1.0, 1.0}};
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

  // get white balance coefficients, as shot
  char filename[1024];
  int ret;
  dt_image_full_path(self->dev->image, filename, 1024);
  libraw_data_t *raw = libraw_init(0);
  ret = libraw_open_file(raw, filename);
  if(!ret) for(int k=0;k<4;k++) g->cam_mul[k] = raw->color.cam_mul[k]/1024.0;
  else     for(int k=0;k<4;k++) g->cam_mul[k] = 1.0;
  libraw_close(raw);

  g->grayboxmode = 0;
  g->graybox[0] = g->graybox[1] = -0.1;
  g->graybox[2] = g->graybox[3] =  0.1;
  GtkWidget *label;
  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));
  GtkBox *hbox  = GTK_BOX(gtk_hbox_new(FALSE, 0));
  GtkBox *vbox1 = GTK_BOX(gtk_vbox_new(TRUE, 0));
  GtkBox *vbox2 = GTK_BOX(gtk_vbox_new(TRUE, 0));
  g->label1 = GTK_LABEL(gtk_label_new(_("tint")));
  g->label2 = GTK_LABEL(gtk_label_new(_("temperature out")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  g->scale_tint  = GTK_HSCALE(gtk_hscale_new_with_range(0.1, 3.0, .001));
  g->scale_k     = GTK_HSCALE(gtk_hscale_new_with_range(DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE, 10.));
  g->scale_k_out = GTK_HSCALE(gtk_hscale_new_with_range(DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE, 10.));
  g->scale_r     = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 3.0, .001));
  g->scale_g     = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 3.0, .001));
  g->scale_b     = GTK_HSCALE(gtk_hscale_new_with_range(0.0, 3.0, .001));
  gtk_scale_set_digits(GTK_SCALE(g->scale_tint),  3);
  gtk_scale_set_digits(GTK_SCALE(g->scale_k),     0);
  gtk_scale_set_digits(GTK_SCALE(g->scale_k_out), 0);
  gtk_scale_set_digits(GTK_SCALE(g->scale_r),     3);
  gtk_scale_set_digits(GTK_SCALE(g->scale_g),     3);
  gtk_scale_set_digits(GTK_SCALE(g->scale_b),     3);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale_tint),  GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale_k),     GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale_k_out), GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale_r),     GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale_g),     GTK_POS_LEFT);
  gtk_scale_set_value_pos(GTK_SCALE(g->scale_b),     GTK_POS_LEFT);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(hbox, GTK_WIDGET(vbox2), TRUE, TRUE, 5);
  gtk_box_pack_start(vbox1, GTK_WIDGET(g->label1), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox2, GTK_WIDGET(g->scale_tint), FALSE, FALSE, 0);
  label = gtk_label_new(_("temperature in"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(vbox1, GTK_WIDGET(label), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox2, GTK_WIDGET(g->scale_k), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox1, GTK_WIDGET(g->label2), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox2, GTK_WIDGET(g->scale_k_out), FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), gtk_hseparator_new(), FALSE, FALSE, 5);
  hbox  = GTK_BOX(gtk_hbox_new(FALSE, 0));
  vbox1 = GTK_BOX(gtk_vbox_new(TRUE, 0));
  vbox2 = GTK_BOX(gtk_vbox_new(TRUE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(hbox, GTK_WIDGET(vbox2), TRUE, TRUE, 5);

  label = gtk_label_new(_("red"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(vbox1, GTK_WIDGET(label), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox2, GTK_WIDGET(g->scale_r), FALSE, FALSE, 0);
  label = gtk_label_new(_("green"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(vbox1, GTK_WIDGET(label), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox2, GTK_WIDGET(g->scale_g), FALSE, FALSE, 0);
  label = gtk_label_new(_("blue"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(vbox1, GTK_WIDGET(label), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox2, GTK_WIDGET(g->scale_b), FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), gtk_hseparator_new(), FALSE, FALSE, 5);
  hbox  = GTK_BOX(gtk_hbox_new(FALSE, 0));
  vbox1 = GTK_BOX(gtk_vbox_new(TRUE, 0));
  vbox2 = GTK_BOX(gtk_vbox_new(TRUE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(vbox1), TRUE, TRUE, 5);
  gtk_box_pack_start(hbox, GTK_WIDGET(vbox2), TRUE, TRUE, 5);

  g->presets = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->presets, _("camera whitebalance"));
  gtk_combo_box_append_text(g->presets, _("passthrough"));
  g->preset_cnt = 2;
  for(int i=0;i<wb_preset_count;i++)
  {
    if(g->preset_cnt >= 50) break;
    if(!strcmp(wb_preset[i].make,  self->dev->image->exif_maker) &&
       !strcmp(wb_preset[i].model, self->dev->image->exif_model))
    {
      gtk_combo_box_append_text(g->presets, _(wb_preset[i].name));
      g->preset_num[g->preset_cnt++] = i;
    }
  }
  GtkWidget *button = gtk_button_new_with_label(_("spot whitebalance"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("select a gray box\nin the image"), NULL);
  gtk_box_pack_start(vbox1, GTK_WIDGET(g->presets), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox2, button, FALSE, FALSE, 0);

  // gui_update(self); <= crash :(
  float temp, tint;
  float mul[3];
  for(int k=0;k<3;k++) mul[k] = p->coeffs[k] * g->cam_mul[k];
  convert_rgb_to_k(p->coeffs, p->temp_out, &temp, &tint);
  gtk_range_set_value(GTK_RANGE(g->scale_k_out), p->temp_out);
  gtk_range_set_value(GTK_RANGE(g->scale_r), mul[0]);
  gtk_range_set_value(GTK_RANGE(g->scale_g), mul[1]);
  gtk_range_set_value(GTK_RANGE(g->scale_b), mul[2]);
  gtk_range_set_value(GTK_RANGE(g->scale_k), temp);
  gtk_range_set_value(GTK_RANGE(g->scale_tint), tint);

  g_signal_connect (G_OBJECT (g->scale_tint), "value-changed",
                    G_CALLBACK (tint_callback), self);
  g_signal_connect (G_OBJECT (g->scale_k), "value-changed",
                    G_CALLBACK (temp_callback), self);
  g_signal_connect (G_OBJECT (g->scale_k_out), "value-changed",
                    G_CALLBACK (temp_out_callback), self);
  g_signal_connect (G_OBJECT (g->scale_r), "value-changed",
                    G_CALLBACK (rgb_callback), self);
  g_signal_connect (G_OBJECT (g->scale_g), "value-changed",
                    G_CALLBACK (rgb_callback), self);
  g_signal_connect (G_OBJECT (g->scale_b), "value-changed",
                    G_CALLBACK (rgb_callback), self);
  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (button_callback), self);
  g_signal_connect (G_OBJECT (g->presets), "changed",
                    G_CALLBACK (presets_changed),
                    (gpointer)self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

static void temp_changed(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;

  g->grayboxmode = 0;
  const float temp_out = gtk_range_get_value(GTK_RANGE(g->scale_k_out));
  const float temp_in  = gtk_range_get_value(GTK_RANGE(g->scale_k));
  const float tint     = gtk_range_get_value(GTK_RANGE(g->scale_tint));

  float original_temperature_rgb[3], intended_temperature_rgb[3];
  convert_k_to_rgb (temp_in,  original_temperature_rgb);
  convert_k_to_rgb (temp_out, intended_temperature_rgb);

  p->temp_out = temp_out;
  p->coeffs[0] =        intended_temperature_rgb[0] / original_temperature_rgb[0];
  p->coeffs[1] = tint * intended_temperature_rgb[1] / original_temperature_rgb[1];
  p->coeffs[2] =        intended_temperature_rgb[2] / original_temperature_rgb[2];

  darktable.gui->reset = 1;
  gtk_range_set_value(GTK_RANGE(g->scale_r), p->coeffs[0]*g->cam_mul[0]);
  gtk_range_set_value(GTK_RANGE(g->scale_g), p->coeffs[1]*g->cam_mul[1]);
  gtk_range_set_value(GTK_RANGE(g->scale_b), p->coeffs[2]*g->cam_mul[2]);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self);
}

static void tint_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  temp_changed(self);
}

static void temp_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  temp_changed(self);
}

static void temp_out_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  temp_changed(self);
}

static void gui_update_from_coeffs(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  // now get temp/tint from rgb. leave temp_out as it was:
  float temp, tint, mul[3];
  for(int k=0;k<3;k++) mul[k] = p->coeffs[k] * g->cam_mul[k];
  p->temp_out = gtk_range_get_value(GTK_RANGE(g->scale_k_out));
  convert_rgb_to_k(p->coeffs, p->temp_out, &temp, &tint);

  darktable.gui->reset = 1;
  gtk_range_set_value(GTK_RANGE(g->scale_k),    temp);
  gtk_range_set_value(GTK_RANGE(g->scale_tint), tint);
  gtk_range_set_value(GTK_RANGE(g->scale_r), mul[0]);
  gtk_range_set_value(GTK_RANGE(g->scale_g), mul[1]);
  gtk_range_set_value(GTK_RANGE(g->scale_b), mul[2]);
  darktable.gui->reset = 0;
}

static void rgb_callback (GtkRange *range, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  g->grayboxmode = 0;
  const float value = gtk_range_get_value(range);
  if     (range == GTK_RANGE(g->scale_r)) p->coeffs[0] = value/g->cam_mul[0];
  else if(range == GTK_RANGE(g->scale_g)) p->coeffs[1] = value/g->cam_mul[1];
  else if(range == GTK_RANGE(g->scale_b)) p->coeffs[2] = value/g->cam_mul[2];

  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self);
}

static void button_callback (GtkButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  self->dev->gui_module = self; // assert focus.
  gtk_combo_box_set_active(g->presets, -1);
  g->button_down_zoom_x = g->button_down_zoom_y = 0.0f;
  g->grayboxmode = 1;
}

void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  if(!g->grayboxmode) return;
  dt_develop_t *dev = self->dev;
  int32_t zoom, closeup;
  float zoom_x, zoom_y;
  float wd = dev->preview_pipe->backbuf_width;
  float ht = dev->preview_pipe->backbuf_height;
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

  cairo_translate(cr, width/2.0, height/2.0f);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f*wd-zoom_x*wd, -.5f*ht-zoom_y*ht);

  // cairo_set_operator(cr, CAIRO_OPERATOR_XOR);
  cairo_set_line_width(cr, 1.0/zoom_scale);
  cairo_set_source_rgb(cr, .2, .2, .2);

  for(int k=0;k<2;k++)
  {
    cairo_rectangle(cr, g->graybox[0]*wd, g->graybox[1]*ht, (g->graybox[2] - g->graybox[0])*wd, (g->graybox[3] - g->graybox[1])*ht);
    cairo_stroke(cr);
    cairo_translate(cr, 1.0/zoom_scale, 1.0/zoom_scale);
    cairo_set_source_rgb(cr, .8, .8, .8);
  }
}

int mouse_moved(struct dt_iop_module_t *self, double x, double y, int which)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  if(!g->grayboxmode) return 0;
  if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    float zoom_x, zoom_y;
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &zoom_x, &zoom_y);
    g->graybox[0] = fminf(.5f+g->button_down_zoom_x, .5f+zoom_x);
    g->graybox[1] = fminf(.5f+g->button_down_zoom_y, .5f+zoom_y);
    g->graybox[2] = fmaxf(.5f+g->button_down_zoom_x, .5f+zoom_x);
    g->graybox[3] = fmaxf(.5f+g->button_down_zoom_y, .5f+zoom_y);
    dt_control_gui_queue_draw();
    dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
    p->trigger++;
    for(int k=0;k<3;k++) p->coeffs[k] = 1.0/(0.01 + g->grayrgb[k]);
    float len = 0.0, lenc = 0.0f;
    for(int k=0;k<3;k++) len  += g->grayrgb[k]*g->grayrgb[k];
    for(int k=0;k<3;k++) lenc += g->grayrgb[k]*g->grayrgb[k]*p->coeffs[k]*p->coeffs[k];
    if(lenc > 0.0001f) for(int k=0;k<3;k++) p->coeffs[k] *= sqrtf(len/lenc);
    for(int k=0;k<3;k++) p->coeffs[k] = fmaxf(0.0f, fminf(3.0, p->coeffs[k]));
    gui_update_from_coeffs(self);
    dt_dev_add_history_item(darktable.develop, self);
    return 1;
  }
  else return 0;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, int which, int type, uint32_t state)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  if(!g->grayboxmode) return 0;
  if(which == 1)
  {
    // init grayrgb to be current wb + trigger, so a redraw will be triggered.
    dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
    p->trigger++;
    for(int k=0;k<3;k++) g->grayrgb[k] = 1.0/(0.01 + p->coeffs[k]);
    float len = 0.0, lenc = 0.0f;
    for(int k=0;k<3;k++) len  += g->grayrgb[k]*g->grayrgb[k];
    for(int k=0;k<3;k++) lenc += g->grayrgb[k]*g->grayrgb[k]*p->coeffs[k]*p->coeffs[k];
    if(len > 0.0001f) for(int k=0;k<3;k++) g->grayrgb[k] *= sqrtf(lenc/len);
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &g->button_down_zoom_x, &g->button_down_zoom_y);
    g->graybox[0] = .5f+g->button_down_zoom_x;
    g->graybox[1] = .5f+g->button_down_zoom_y;
    g->graybox[2] = .5f+g->button_down_zoom_x;
    g->graybox[3] = .5f+g->button_down_zoom_y;
    dt_control_gui_queue_draw();
    return 1;
  }
  else return 0;
}

static void
presets_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  const int pos = gtk_combo_box_get_active(widget);
  switch(pos)
  {
    case 0: // camera wb
      for(int k=0;k<3;k++) p->coeffs[k] = 1.0;
      break;
    case 1: // passthrough mode, raw data
      for(int k=0;k<3;k++) p->coeffs[k] = 1.0/g->cam_mul[k];
      break;
    default:
      for(int k=0;k<3;k++)
        p->coeffs[k] = wb_preset[g->preset_num[pos]].channel[k]/g->cam_mul[k];
      break;
  }
  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self);
}

