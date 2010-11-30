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
#include "iop/temperature.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "libraw/libraw.h"
#include "iop/wb_presets.c"

DT_MODULE(2)

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
  return C_("modulename", "white balance");
}


int 
groups () 
{
	return IOP_GROUP_BASIC;
}

int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(pipe->type != DT_DEV_PIXELPIPE_PREVIEW && module->dev->image->filters) return sizeof(uint16_t);
  else return 4*sizeof(float);
}


static void
convert_k_to_rgb (float temperature, float *rgb)
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
static void
convert_rgb_to_k(float rgb[3], const float temp_out, float *temp, float *tint)
{
  float tmin, tmax, tmp[3], original_temperature_rgb[3], intended_temperature_rgb[3];
  for(int k=0;k<3;k++) tmp[k] = rgb[k];
  tmin = DT_IOP_LOWEST_TEMPERATURE;
  tmax = DT_IOP_HIGHEST_TEMPERATURE;
  convert_k_to_rgb (temp_out,  intended_temperature_rgb);
  for (*temp=(tmax+tmin)/2; tmax-tmin>1; *temp=(tmax+tmin)/2)
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

static int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const int filters = dt_image_flipped_filter(self->dev->image);
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  for(int k=0;k<3;k++)
    piece->pipe->processed_maximum[k] = d->coeffs[k] * piece->pipe->processed_maximum[k];
  if(piece->pipe->type != DT_DEV_PIXELPIPE_PREVIEW && filters)
  {
    const uint16_t *const in  = (const uint16_t *const)i;
    uint16_t *const out = (uint16_t *const)o;
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, out, d) schedule(static)
#endif
    for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
      out[j*roi_out->width+i] = CLAMP(in[j*roi_out->width+i]*d->coeffs[FC(j+roi_out->x, i+roi_out->y, filters)], 0, 0xffff);
  }
  else
  {
    float *in  = (float *)i;
    float *out = (float *)o;
    const int ch = piece->colors;
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, out, in, d) schedule(static)
#endif
    for(int k=0;k<roi_out->width*roi_out->height;k++)
      for(int c=0;c<3;c++) out[ch*k+c] = in[ch*k+c]*d->coeffs[c];
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

void gui_update (struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  self->request_color_pick = 0;
  self->color_picker_box[0] = self->color_picker_box[1] = .25f;
  self->color_picker_box[2] = self->color_picker_box[3] = .75f;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p  = (dt_iop_temperature_params_t *)module->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)module->factory_params;
  float temp, tint, mul[3];
  for(int k=0;k<3;k++) mul[k] = p->coeffs[k]/fp->coeffs[k];
  convert_rgb_to_k(mul, p->temp_out, &temp, &tint);
  
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale_k_out), p->temp_out);
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale_r), p->coeffs[0]);
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale_g), p->coeffs[1]);
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale_b), p->coeffs[2]);
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale_k), temp);
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale_tint), tint);
  if(fabsf(p->coeffs[0]-fp->coeffs[0]) + fabsf(p->coeffs[1]-fp->coeffs[1]) + fabsf(p->coeffs[2]-fp->coeffs[2]) < 0.01)
    gtk_combo_box_set_active(g->presets, 0);
  else
    gtk_combo_box_set_active(g->presets, -1);
  gtk_spin_button_set_value(g->finetune, 0);
}

void init (dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_temperature_params_t));
  module->default_params = malloc(sizeof(dt_iop_temperature_params_t));
  // raw images need wb:
  if(dt_image_is_ldr(module->dev->image)) module->default_enabled = 0;
  else
  {
    module->default_enabled = 1;
    // module->hide_enable_button = 1;
  }
  module->priority = 170;
  module->params_size = sizeof(dt_iop_temperature_params_t);
  module->gui_data = NULL;
  dt_iop_temperature_params_t tmp = (dt_iop_temperature_params_t){5000.0, {1.0, 1.0, 1.0}};

  // get white balance coefficients, as shot
  char filename[1024];
  int ret;
  dt_image_full_path(module->dev->image, filename, 1024);
  libraw_data_t *raw = libraw_init(0);
  ret = libraw_open_file(raw, filename);
  if(!ret)
  {
    for(int k=0;k<3;k++) tmp.coeffs[k] = raw->color.cam_mul[k];
    if(tmp.coeffs[0] < 0.0) for(int k=0;k<3;k++) tmp.coeffs[k] = raw->color.pre_mul[k];
    if(tmp.coeffs[0] == 0 || tmp.coeffs[1] == 0 || tmp.coeffs[2] == 0)
    { // could not get useful info!
      tmp.coeffs[0] = tmp.coeffs[1] = tmp.coeffs[2] = 1.0f;
    }
    else
    {
      tmp.coeffs[0] /= tmp.coeffs[1];
      tmp.coeffs[2] /= tmp.coeffs[1];
      tmp.coeffs[1] = 1.0f;
    }
  }
  libraw_close(raw);

  memcpy(module->params, &tmp, sizeof(dt_iop_temperature_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_temperature_params_t));
}

void cleanup (dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void
gui_update_from_coeffs (dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p  = (dt_iop_temperature_params_t *)self->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)self->factory_params;
  // now get temp/tint from rgb. leave temp_out as it was:
  float temp, tint, mul[3];

  for(int k=0;k<3;k++) mul[k] = p->coeffs[k]/fp->coeffs[k];
  p->temp_out = dtgtk_slider_get_value(DTGTK_SLIDER(g->scale_k_out));
  convert_rgb_to_k(mul, p->temp_out, &temp, &tint);

  darktable.gui->reset = 1;
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale_k),    temp);
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale_tint), tint);
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale_r), p->coeffs[0]);
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale_g), p->coeffs[1]);
  dtgtk_slider_set_value(DTGTK_SLIDER(g->scale_b), p->coeffs[2]);
  darktable.gui->reset = 0;
}


static gboolean
expose (GtkWidget *widget, GdkEventExpose *event, dt_iop_module_t *self)
{ // capture gui color picked event.
  if(darktable.gui->reset) return FALSE;
  if(self->picked_color_max[0] < self->picked_color_min[0]) return FALSE;
  if(!self->request_color_pick) return FALSE;
  static float old[3] = {0, 0, 0};
  const float *grayrgb = self->picked_color;
  if(grayrgb[0] == old[0] && grayrgb[1] == old[1] && grayrgb[2] == old[2]) return FALSE;
  for(int k=0;k<3;k++) old[k] = grayrgb[k];
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  for(int k=0;k<3;k++) p->coeffs[k] = 1.0/(0.01 + grayrgb[k]);
  float len = 0.0, lenc = 0.0f;
  for(int k=0;k<3;k++) len  += grayrgb[k]*grayrgb[k];
  for(int k=0;k<3;k++) lenc += grayrgb[k]*grayrgb[k]*p->coeffs[k]*p->coeffs[k];
  if(lenc > 0.0001f) for(int k=0;k<3;k++) p->coeffs[k] *= sqrtf(len/lenc);
  // normalize green:
  p->coeffs[0] /= p->coeffs[1];
  p->coeffs[2] /= p->coeffs[1];
  p->coeffs[1] = 1.0;
  for(int k=0;k<3;k++) p->coeffs[k] = fmaxf(0.0f, fminf(5.0, p->coeffs[k]));
  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self);
  return FALSE;
}

void gui_init (struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_temperature_gui_data_t));
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;

  self->request_color_pick = 0;
  GtkWidget *label;
  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));
  g_signal_connect(G_OBJECT(self->widget), "expose-event", G_CALLBACK(expose), self);
  GtkBox *hbox  = GTK_BOX(gtk_hbox_new(FALSE, 0));
  GtkBox *vbox1 = GTK_BOX(gtk_vbox_new(TRUE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  GtkBox *vbox2 = GTK_BOX(gtk_vbox_new(TRUE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->label1 = GTK_LABEL(gtk_label_new(_("tint")));
  g->label2 = GTK_LABEL(gtk_label_new(_("temperature out")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  g->scale_tint  = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.1, 5.0, .001,0,0));
  g->scale_k     = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE, 10.,0,0));
  g->scale_k_out = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE, 10.,0,0));
  g->scale_r     = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 5.0, .001,0,0));
  g->scale_g     = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 5.0, .001,0,0));
  g->scale_b     = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 5.0, .001,0,0));
  dtgtk_slider_set_digits((g->scale_tint),  3);
  dtgtk_slider_set_digits((g->scale_k),     0);
  dtgtk_slider_set_digits((g->scale_k_out), 0);
  dtgtk_slider_set_digits((g->scale_r),     3);
  dtgtk_slider_set_digits((g->scale_g),     3);
  dtgtk_slider_set_digits((g->scale_b),     3);
  
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
  vbox1 = GTK_BOX(gtk_vbox_new(TRUE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  vbox2 = GTK_BOX(gtk_vbox_new(TRUE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
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
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  g->presets = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->presets, _("camera white balance"));
  gtk_combo_box_append_text(g->presets, _("spot white balance"));
  gtk_combo_box_append_text(g->presets, _("passthrough"));
  g->preset_cnt = 3;
  const char *wb_name = NULL;
  if(!dt_image_is_ldr(self->dev->image)) for(int i=0;i<wb_preset_count;i++)
  {
    if(g->preset_cnt >= 50) break;
    if(!strcmp(wb_preset[i].make,  self->dev->image->exif_maker) &&
       !strcmp(wb_preset[i].model, self->dev->image->exif_model))
    {
      if(!wb_name || strcmp(wb_name, wb_preset[i].name))
      {
        wb_name = wb_preset[i].name;
        gtk_combo_box_append_text(g->presets, _(wb_preset[i].name));
        g->preset_num[g->preset_cnt++] = i;
      }
    }
  }
  gtk_box_pack_start(hbox, GTK_WIDGET(g->presets), TRUE, TRUE, 5);

  g->finetune = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(-9, 9, 1));
  gtk_spin_button_set_value (g->finetune, 0);
  gtk_spin_button_set_digits(g->finetune, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(g->finetune), FALSE, FALSE, 0);
  gtk_object_set(GTK_OBJECT(g->finetune), "tooltip-text", _("fine tune white balance preset"), (char *)NULL);

  self->gui_update(self);

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
  g_signal_connect (G_OBJECT (g->presets), "changed",
                    G_CALLBACK (presets_changed), self);
  g_signal_connect (G_OBJECT (g->finetune), "value-changed",
                    G_CALLBACK (finetune_changed), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  self->request_color_pick = 0;
  free(self->gui_data);
  self->gui_data = NULL;
}

static void
temp_changed(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p  = (dt_iop_temperature_params_t *)self->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)self->factory_params;

  const float temp_out = dtgtk_slider_get_value(DTGTK_SLIDER(g->scale_k_out));
  const float temp_in  = dtgtk_slider_get_value(DTGTK_SLIDER(g->scale_k));
  const float tint     = dtgtk_slider_get_value(DTGTK_SLIDER(g->scale_tint));

  float original_temperature_rgb[3], intended_temperature_rgb[3];
  convert_k_to_rgb (temp_in,  original_temperature_rgb);
  convert_k_to_rgb (temp_out, intended_temperature_rgb);

  p->temp_out = temp_out;
  p->coeffs[0] = fp->coeffs[0] *        intended_temperature_rgb[0] / original_temperature_rgb[0];
  p->coeffs[1] = fp->coeffs[1] * tint * intended_temperature_rgb[1] / original_temperature_rgb[1];
  p->coeffs[2] = fp->coeffs[2] *        intended_temperature_rgb[2] / original_temperature_rgb[2];

  darktable.gui->reset = 1;
  dtgtk_slider_set_value(g->scale_r, p->coeffs[0]);
  dtgtk_slider_set_value(g->scale_g, p->coeffs[1]);
  dtgtk_slider_set_value(g->scale_b, p->coeffs[2]);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self);
}

static void
tint_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  gtk_combo_box_set_active(g->presets, -1);
}

static void
temp_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  gtk_combo_box_set_active(g->presets, -1);
}

static void
temp_out_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  gtk_combo_box_set_active(g->presets, -1);
}

static void
rgb_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  const float value = dtgtk_slider_get_value( slider );
  if     (slider == DTGTK_SLIDER(g->scale_r)) p->coeffs[0] = value;
  else if(slider == DTGTK_SLIDER(g->scale_g)) p->coeffs[1] = value;
  else if(slider == DTGTK_SLIDER(g->scale_b)) p->coeffs[2] = value;
 
  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self);
  gtk_combo_box_set_active(g->presets, -1);
}

static void
apply_preset(dt_iop_module_t *self)
{
  self->request_color_pick = 0;
  if(self->dt->gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p  = (dt_iop_temperature_params_t *)self->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)self->factory_params;
  const int tune = gtk_spin_button_get_value(g->finetune);
  const int pos = gtk_combo_box_get_active(g->presets);
  switch(pos)
  {
    case -1: // just un-setting.
      return;
    case 0: // camera wb
      for(int k=0;k<3;k++) p->coeffs[k] = fp->coeffs[k];
      break;
    case 1: // spot wb, exposure callback will set p->coeffs.
      for(int k=0;k<3;k++) p->coeffs[k] = fp->coeffs[k];
      dt_iop_request_focus(self);
      self->request_color_pick = 1;
      break;
    case 2: // passthrough mode, raw data
      for(int k=0;k<3;k++) p->coeffs[k] = 1.0;
      break;
    default:
      for(int i=g->preset_num[pos];i<wb_preset_count;i++)
      {
        if(!strcmp(wb_preset[i].make,  self->dev->image->exif_maker) &&
           !strcmp(wb_preset[i].model, self->dev->image->exif_model) && wb_preset[i].tuning == tune)
        {
          for(int k=0;k<3;k++) p->coeffs[k] = wb_preset[i].channel[k];
          break;
        }
      }
      break;
  }
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
presets_changed (GtkComboBox *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  apply_preset(self);
  const int pos = gtk_combo_box_get_active(widget);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  gtk_widget_set_sensitive(GTK_WIDGET(g->finetune), pos > 2);
  // TODO: get preset finetunings and insert in combobox
  // gtk_spin_button_set_(g->finetune, 0);
}

static void
finetune_changed (GtkSpinButton *widget, gpointer user_data)
{
  apply_preset((dt_iop_module_t *)user_data);
}

