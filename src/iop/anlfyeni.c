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
#include "control/control.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <math.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(1, dt_iop_anlfyeni_params_t)

/* Description: This module implements a local contrast enhancement method
 * based on adaptive nonlinear filters. The algorithm is based on the article
 * "Image Local Contrast Enhancement using Adaptive Non-Linear Filters" by
 * T. Arici and Y. Altunbasak in  IEEE International Conference on Image
 * Processing, (2006). It is significantly faster than local contrast
 * enhancement by unsharp mask or adaptive histogram methods, and is not very
 * prone to produce halos. However the results look quite different as well.
 * This implementation uses three parameters: Alpha, similar to an inverse
 * radius leaving this above 5 is usually a good idea. Scaling instead of
 * exposing the parameters a, b, c, from the above paper this is a single
 * parameter that is multiplied with a, b and c. Strength, the K parameter in
 * the above article.
 */


// TODO: some build system to support dt-less compilation and translation!

typedef struct dt_iop_anlfyeni_params_t
{
  float alpha, scale, strength;
}
dt_iop_anlfyeni_params_t;

typedef struct dt_iop_anlfyeni_gui_data_t
{
  GtkDarktableSlider *scale1, *scale2, *scale3; // this is needed by gui_update
  GtkVBox   *vbox;
}
dt_iop_anlfyeni_gui_data_t;

typedef struct dt_iop_anlfyeni_data_t
{
  float alpha, scale, strength; // in our case, no precomputation or
  // anything is possible, so this is just a copy.
}
dt_iop_anlfyeni_data_t;

typedef struct dt_iop_anlfyeni_global_data_t
{
  // we don't need it for this example (as for most dt plugins)
}
dt_iop_anlfyeni_global_data_t;

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

int flags()
{
  return IOP_FLAGS_DEPRECATED;
}

void init_key_accels(dt_iop_module_so_t *self)
{
#if 0 // we are deprecated.
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/modules/anlfyeni/sensitivity");
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/modules/anlfyeni/scale");
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/modules/anlfyeni/strength");
#endif
}

static float lambda(float x, float mu, float alpha)
{
  //return pow((1-abs(mu-x)/100.),alpha);
  return exp(-abs(mu-x)*alpha);
}

static float yeni(float x, float mu, float alpha)
{
  float l = lambda(x, mu, alpha);
  return l*mu + (1-l)*x;
}

static float gain(float x, float a, float b, float c, float K)
{
  if(abs(x)<=a) return 0.;
  if(abs(x)<=b) return K*(cosf(M_PI+(x-a)*(0.5*M_PI)/(b-a))+1);
  if(abs(x)<=c) return K*cosf((x-b)*(0.5*M_PI/(c-b)));
  return 0.;
}

/** process, all real work is done here. */
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_anlfyeni_params_t *d = (dt_iop_anlfyeni_params_t *)piece->data;
  // how many colors in our buffer?
  const int ch = piece->colors;
  const int ch_width_1 = ch * (roi_out->width-1);
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
  //TODO: IMPORTANT!! I am not 100% sure if this can actually be parallelized. I
  //should check that omp really only parallelizes the outer loop.
  #pragma omp parallel for default(none) schedule(static) shared(ivoid,ovoid,roi_in,roi_out,d, piece) private(mu_f, mu_b, highpass)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    int i;
    const float *in  = ((float *)ivoid) + (size_t)ch*roi_in->width *j;
    float *out = ((float *)ovoid) + (size_t)ch*roi_out->width*j;
    const float *inp0 = in;
    const float *inp1 = in + ch_width_1;
    mu_f[0] = yeni(inp0[0], inp0[0], d->alpha/roi_in->scale*piece->iscale);
    mu_b[0] = yeni(inp1[0], inp1[0], d->alpha/roi_in->scale*piece->iscale);
    out[1] = in[1];
    out[2] = in[2];
    inp0 += ch;
    inp1 -= ch;
    float *outp = out + ch;
    for(i=1; i<roi_out->width/2; i++,outp+=ch,inp0+=ch,inp1-=ch)
    {
      mu_f[i] = yeni(inp0[0], mu_f[i-1], d->alpha/roi_in->scale*piece->iscale);
      mu_b[i] = yeni(inp1[0], mu_b[i-1], d->alpha/roi_in->scale*piece->iscale);
      outp[1] = inp0[1];
      outp[2] = inp0[2];
    }
    float *outp1 = out + ch*(roi_out->width-(1+i));
    for(; i<roi_out->width; i++,outp+=ch,outp1-=ch,inp0+=ch,inp1-=ch)
    {
      outp[1] = inp0[1];
      outp[2] = inp0[2];
      mu_f[i] = yeni(inp0[0], mu_f[i-1], d->alpha/roi_in->scale*piece->iscale);
      mu_b[i] = yeni(inp1[0], mu_b[i-1], d->alpha/roi_in->scale*piece->iscale);
      highpass = inp0[0] - (mu_f[i]+mu_b[roi_out->width-(1+i)])/2;
      outp[0] = inp0[0]+gain(highpass, a, b, c, d->strength)*highpass;
      highpass = inp1[0] - (mu_f[roi_out->width-(1+i)]+ mu_b[i])/2;
      outp1[0] = inp1[0] + gain(highpass, a, b, c, d->strength)*highpass;
    }
  }
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  module->data = NULL;
  module->params = malloc(sizeof(dt_iop_anlfyeni_params_t));
  module->default_params = malloc(sizeof(dt_iop_anlfyeni_params_t));
  module->default_enabled = 0;
  module->priority = 719; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_anlfyeni_params_t);
  module->gui_data = NULL;
  // TODO: check the defaults if there's better ones
  dt_iop_anlfyeni_params_t tmp = (dt_iop_anlfyeni_params_t)
  {
    0.01f, 1.0f, 1.0f
  };

  memcpy(module->params, &tmp, sizeof(dt_iop_anlfyeni_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_anlfyeni_params_t));
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
  dt_iop_anlfyeni_params_t *p = (dt_iop_anlfyeni_params_t *)params;
  dt_iop_anlfyeni_data_t *d = (dt_iop_anlfyeni_data_t *)piece->data;
  d->alpha = p->alpha;
  d->scale = p->scale;
  d->strength = p->strength;
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_anlfyeni_data_t));
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
  dt_iop_anlfyeni_gui_data_t *g = (dt_iop_anlfyeni_gui_data_t *)self->gui_data;
  dt_iop_anlfyeni_params_t *p = (dt_iop_anlfyeni_params_t *)self->params;
  p->alpha = dtgtk_slider_get_value(g->scale1);
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
scale_callback(GtkRange *range, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_anlfyeni_gui_data_t *g = (dt_iop_anlfyeni_gui_data_t *)self->gui_data;
  dt_iop_anlfyeni_params_t *p = (dt_iop_anlfyeni_params_t *)self->params;
  p->scale = dtgtk_slider_get_value(g->scale2);
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
strength_callback(GtkRange *range, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_anlfyeni_gui_data_t *g = (dt_iop_anlfyeni_gui_data_t *)self->gui_data;
  dt_iop_anlfyeni_params_t *p = (dt_iop_anlfyeni_params_t *)self->params;
  p->strength = dtgtk_slider_get_value(g->scale3);
  // let core know of the changes
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/** gui callbacks, these are needed. */
void gui_update    (dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_anlfyeni_gui_data_t *g = (dt_iop_anlfyeni_gui_data_t *)self->gui_data;
  dt_iop_anlfyeni_params_t *p = (dt_iop_anlfyeni_params_t *)self->params;
  dtgtk_slider_set_value(g->scale1, p->alpha);
  dtgtk_slider_set_value(g->scale2, p->scale);
  dtgtk_slider_set_value(g->scale3, p->strength);
}

void gui_init(struct dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_anlfyeni_gui_data_t));
  dt_iop_anlfyeni_gui_data_t *g = (dt_iop_anlfyeni_gui_data_t *)self->gui_data;
  dt_iop_anlfyeni_params_t *p = (dt_iop_anlfyeni_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox), TRUE, TRUE, 5);

  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.001, 0.07, 0.001, p->alpha, 3));
  g_object_set (GTK_OBJECT(g->scale1), "tooltip-text", _("sensitivity of edge detection"), (char *)NULL);
  dtgtk_slider_set_label(g->scale1,_("sensitivity"));
  // dtgtk_slider_set_accel(g->scale1,darktable.control->accels_darkroom,"<Darktable>/darkroom/modules/local_contrast_2/sensitivity");
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 6.0000, 0.010, p->scale, 3));
  g_object_set (GTK_OBJECT(g->scale2), "tooltip-text", _("spatial extent of the effect around edges"), (char *)NULL);
  dtgtk_slider_set_label(g->scale2,_("scale"));
  // dtgtk_slider_set_accel(g->scale2,darktable.control->accels_darkroom,"<Darktable>/darkroom/modules/local_contrast_2/scale");
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 10.0000, 0.001, p->strength, 3));
  g_object_set (GTK_OBJECT(g->scale3), "tooltip-text", _("strength of the local contrast"), (char *)NULL);
  dtgtk_slider_set_label(g->scale3,_("strength"));
  // dtgtk_slider_set_accel(g->scale3,darktable.control->accels_darkroom,"<Darktable>/darkroom/modules/local_contrast_2/strength");
  gtk_box_pack_start(GTK_BOX(g->vbox), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);

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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
