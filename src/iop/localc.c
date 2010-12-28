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
#include "develop/imageop.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <math.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE(1)

/* Description: This module implements a local contrast enhancement method
 * based on apaptive nonlinear filters. The algorithm is based on the article
 * "Image Local Contrast Enhancement using Adaptive Non-Linear Filters" by 
 * T. Arici and Y. Altunbasak in  IEEE International Conference on Image 
 * Processing, (2006). It is significantly faster than local contrast
 * enhancement by unsharp mask or apaptive histogram methods, and is not very
 * prone to produce halos. However the results look quite different as well.
 * This implementation uses three parameters: Alpha, similar to an inverse
 * radius leaving this above 5 is usually a good idea. Scaling instead of
 * exposing the parameters a, b, c, from the above paper this is a single
 * parameter that is multiplied with a, b and c. Strength, the K parameter in
 * the above article.
 */


// TODO: some build system to support dt-less compilation and translation!

typedef struct dt_iop_localc_params_t
{
  float alpha, scale, strength;
}
dt_iop_localc_params_t;

typedef struct dt_iop_localc_gui_data_t
{
  GtkDarktableSlider *scale1, *scale2, *scale3; // this is needed by gui_update
  GtkVBox   *vbox1, *vbox2;
}
dt_iop_localc_gui_data_t;

typedef struct dt_iop_localc_data_t
{
  float alpha, scale, strength; // in our case, no precomputation or
                     // anything is possible, so this is just a copy.
}
dt_iop_localc_data_t;

typedef struct dt_iop_localc_global_data_t
{
  // we don't need it for this example (as for most dt plugins)
}
dt_iop_localc_global_data_t;

// this returns a translatable name
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("local contrast 2");
}

int
groups () 
{
	return IOP_GROUP_EFFECT;
}

static float lambda(float x, float mu, float alpha)
{
    return pow((1-abs(mu-x)/100.),alpha);
}

static float yeni(float x, float mu, float alpha)
{
    float l = lambda(x, mu, alpha);
    return l*mu + (1-l)*x;
}

static float gain(float x, float a, float b, float c, float K)
{
    if(x<=a) return 0.;
    if(x<=b) return K*(cosf(M_PI+(x-a)*(0.5*M_PI)/(b-a))+1);
    if(x<=c) return K*cosf((x-b)*(0.5*M_PI/(c-b)));
    return 0.;
}

/** process, all real work is done here. */
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_localc_params_t *d = (dt_iop_localc_params_t *)piece->data;
  // how many colors in our buffer?
  const int ch = piece->colors;
  //int j,k,i;
  // TODO: check if I can find better default values for a, b, c
  const float a = d->scale*1.;
  const float b = d->scale*7;
  const float c = d->scale*21;
  float mu_f[roi_out->width], mu_b[roi_out->width];
  float highpass;
  // iterate over all output pixels (same coordinates as input)
#ifdef _OPENMP
   //optional: parallelize it!
   //TODO: IMPORTANT!! I am not 100% sure if this can actually be parallised. I
   //should check that omp really only parallises the outer loop.
  #pragma omp parallel for default(none) schedule(static) shared(ivoid,ovoid,roi_in,roi_out,d) private(mu_f, mu_b, highpass)
#endif
    for(int j=0;j<roi_out->height;j++)
    {
      int i,k;
      float *in  = ((float *)ivoid) + ch*roi_in->width *j;
      float *out = ((float *)ovoid) + ch*roi_out->width*j;
      mu_f[0] = yeni(in[0], in[0], d->alpha);
      mu_b[0] = yeni(in[ch*(roi_out->width-1)+0], 
             in[ch*(roi_out->width -1) + 0], d->alpha);
      out[1] = in[1];
      out[2] = in[2];
    for(i=1;i<roi_out->width/2;i++)
    {
      mu_f[i] = yeni(in[ch*i+0], mu_f[i-1], d->alpha);
      mu_b[i] = yeni(in[ch*(roi_out->width-(1+i))+0], 
              mu_b[i-1], d->alpha);
      out[ch*i+1] = in[ch*i+1];
      out[ch*i+2] = in[ch*i+2];
    }
    for(k=i;k<roi_out->width;k++)
    {
      out[ch*k+1] = in[ch*k+1];
      out[ch*k+2] = in[ch*k+2];
      mu_f[k] = yeni(in[ch*k+0], mu_f[k-1], d->alpha);
      mu_b[k] = yeni(in[ch*(roi_out->width-(1+k))+0], 
              mu_b[k-1], d->alpha);
      highpass = in[ch*k+0] - (mu_f[k]+mu_b[roi_out->width-(1+k)])/2;
      out[ch*k+0] = in[ch*k+0]+gain(highpass, a, b, c, d->strength)*highpass;
      highpass = in[ch*(roi_out->width-(1+k))+0] - 
                 (mu_f[roi_out->width-(1+k)]+ mu_b[k])/2;
      out[ch*(roi_out->width-(1+k))+0] = in[ch*(roi_out->width-(1+k))+0] +
                gain(highpass, a, b, c, d->strength)*highpass;
    }
  }
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  module->data = NULL; 
  module->params = malloc(sizeof(dt_iop_localc_params_t));
  module->default_params = malloc(sizeof(dt_iop_localc_params_t));
  module->default_enabled = 0;
  module->priority = 858; //TODO: need to check a good place in the pipe
  module->params_size = sizeof(dt_iop_localc_params_t);
  module->gui_data = NULL;
  // TODO: check the defaults if there's better ones
  dt_iop_localc_params_t tmp = (dt_iop_localc_params_t){5.0f, 1.0f, 1.0f};

  memcpy(module->params, &tmp, sizeof(dt_iop_localc_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_localc_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL; // just to be sure
  free(module->params);
  module->params = NULL;
  free(module->data); // just to be sure
  module->data = NULL;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_localc_params_t *p = (dt_iop_localc_params_t *)params;
  dt_iop_localc_data_t *d = (dt_iop_localc_data_t *)piece->data;
  d->alpha = p->alpha;
  d->scale = p->scale;
  d->strength = p->strength;
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_localc_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

static void
alpha_callback(GtkRange *range, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_localc_gui_data_t *g = (dt_iop_localc_gui_data_t *)self->gui_data;
  dt_iop_localc_params_t *p = (dt_iop_localc_params_t *)self->params;
  p->alpha = dtgtk_slider_get_value(g->scale1);
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self);
}

static void
scale_callback(GtkRange *range, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_localc_gui_data_t *g = (dt_iop_localc_gui_data_t *)self->gui_data;
  dt_iop_localc_params_t *p = (dt_iop_localc_params_t *)self->params;
  p->strength = dtgtk_slider_get_value(g->scale2);
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self);
}

static void
strength_callback(GtkRange *range, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_localc_gui_data_t *g = (dt_iop_localc_gui_data_t *)self->gui_data;
  dt_iop_localc_params_t *p = (dt_iop_localc_params_t *)self->params;
  p->strength = dtgtk_slider_get_value(g->scale3);
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self);
}
 
/** gui callbacks, these are needed. */
void gui_update    (dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_localc_gui_data_t *g = (dt_iop_localc_gui_data_t *)self->gui_data;
  dt_iop_localc_params_t *p = (dt_iop_localc_params_t *)self->params;
  dtgtk_slider_set_value(g->scale1, p->alpha);
  dtgtk_slider_set_value(g->scale2, p->scale);
  dtgtk_slider_set_value(g->scale3, p->strength);
}

void gui_init(struct dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_localc_gui_data_t));
  dt_iop_localc_gui_data_t *g = (dt_iop_localc_gui_data_t *)self->gui_data;
  dt_iop_localc_params_t *p = (dt_iop_localc_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);

  GtkWidget *widget;
  widget = dtgtk_reset_label_new(_("alpha"), self, &p->alpha, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), widget, TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new(_("scale"), self, &p->scale, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), widget, TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new(_("strength"), self, &p->strength, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), widget, TRUE, TRUE, 0);

  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 30.0000, 0.100, p->alpha, 3));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 4.0000, 0.010, p->scale, 3));
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 10.0000, 0.001, p->strength, 3));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (alpha_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (scale_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (strength_callback), self);
}

void gui_cleanup  (dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

