/*
  This file is part of darktable,
  copyright (c) 2011 Denis Cheremisov.

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
#include "common/colorspaces.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define min(x,y) ((x<y)?x:y)
#define max(x,y) ((x<y)?y:x)

DT_MODULE_INTROSPECTION(3, dt_iop_shrecovery_params_t)


inline float sqr(float x)
{
  return x*x;
}


typedef struct dt_iop_shrecovery_params_t
{
  // Exposure scale compared to currently (taken with black == 0 in mind)
  float strength;

  // Weights
  float mu;
  float sigma;

  int size_limit;  // Gauss and Laplace pyramide size limit
}
dt_iop_shrecovery_params_t;

typedef struct dt_iop_shrecovery_data_t
{
  float strength, mu, sigma;
  int size_limit;

}
dt_iop_shrecovery_data_t;


typedef struct dt_iop_shrecovery_gui_data_t
{
  GtkDarktableSlider *scale1,*scale2,*scale3,*scale4;       // strength, sigma, mu
}
dt_iop_shrecovery_gui_data_t;

const char *name()
{
  return _("shadow recovery (experimental)");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_ONE_INSTANCE;
}

int
groups ()
{
  return IOP_GROUP_TONE;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "strength"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "mu"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "sigma"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "size_limit"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_shrecovery_gui_data_t *g = (dt_iop_shrecovery_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "strength", GTK_WIDGET(g->scale1));
  dt_accel_connect_slider_iop(self, "mu", GTK_WIDGET(g->scale2));
  dt_accel_connect_slider_iop(self, "sigma", GTK_WIDGET(g->scale3));
  dt_accel_connect_slider_iop(self, "size_limit", GTK_WIDGET(g->scale4));
}

static
const float _w_[5] = {0.05, 0.25, 0.4, 0.25, 0.05};


static void
create_image_weight(int height, int width, int ch, float *in, float *W, float scale, float mu, float sigma)
{
  float e1, e2, r, g, b, factor = -0.5/sqr(sigma), t1, t2;
  for(int i = 0; i < height; i++)
  {
    for(int j = 0; j < width; j++)
    {
      size_t index = (size_t)i*width+j;
      r = in[index*ch];
      g = in[index*ch+1];
      b = in[index*ch+2];
      t1 = factor*(sqr(r-mu) + sqr(g-mu) + sqr(b-mu));
      t2 = factor*(sqr(r*scale-mu) + sqr(g*scale-mu) + sqr(b*scale-mu));
      if(t2 < t1)
      {
        e1 = 1.0;
        e2 = exp(t2 - t1);
      }
      else
      {
        e1 = exp(t1 - t2);
        e2 = 1.0;
      }
      W[index] = e1/(e1+e2);
    }
  }
}

static void
gauss_image_weight(const int size_limit, int width, int height, float *W)
{
  float *wn = W + width*height;
  int w = width/2, h = height/2;
  float *l1, *l2, *l3, *l4, *l5;
  int st1, st2, st3, st4, st5;
  for(int i = 0; i < h; i++)
  {
    l1 = W + max(2*i-2,0)*width;
    l2 = W + max(2*i-1,0)*width;
    l3 = W + 2*i*width;
    l4 = W + min(2*i+1,height-1)*width;
    l5 = W + min(2*i+2,height-1)*width;
    for(int j = 0; j < w; j++)
    {
      st1 = max(2*j-2,0);
      st2 = max(2*j-1,0);
      st3 = 2*j;
      st4 = min(2*j+1,width-1);
      st5 = min(2*j+2,width-1);
      wn[i*w+j] = 0.0625*(l2[st2]+l2[st4]+l4[st2]+l4[st4])+
                  0.0125*(l1[st2]+l1[st4]+l2[st1]+l2[st5]+l4[st1]+l4[st5]+l5[st2]+l5[st4])+
                  0.1*(l2[st3]+l3[st2]+l3[st4]+l4[st3])+
                  0.16*(l3[st3])+
                  0.0025*(l1[st1]+l1[st5]+l5[st1]+l5[st5])+
                  0.02*(l1[st3]+l3[st1]+l3[st5]+l5[st3]);
    }
  }
  if((w/2>=size_limit) && (h/2>=size_limit))
    gauss_image_weight(size_limit,w,h,wn);
}

static void
laplace_image(const int size_limit, int width, int height, float *im)
{
  float *imn = im + 3*width*height;
  int w = width/2, h = height/2, i1, j1;
  float *l1, *l2, *l3, *l4, *l5;
  int st1, st2, st3, st4, st5;
  for(int i = 0; i < h; i++)
  {
    l1 = im + (size_t)max(2*i-2,0)*width*3;
    l2 = im + (size_t)max(2*i-1,0)*width*3;
    l3 = im + (size_t)2*i*width*3;
    l4 = im + (size_t)min(2*i+1,height-1)*width*3;
    l5 = im + (size_t)min(2*i+2,height-1)*width*3;
    for(int j = 0; j < w; j++)
    {
      st1 = max(2*j-2,0)*3;
      st2 = max(2*j-1,0)*3;
      st3 = 6*j;
      st4 = min(2*j+1,width-1)*3;
      st5 = min(2*j+2,width-1)*3;
      imn[3*(i*w+j)] = 0.0625*(l2[st2]+l2[st4]+l4[st2]+l4[st4])+
                       0.0125*(l1[st2]+l1[st4]+l2[st1]+l2[st5]+l4[st1]+l4[st5]+l5[st2]+l5[st4])+
                       0.1*(l2[st3]+l3[st2]+l3[st4]+l4[st3])+
                       0.16*(l3[st3])+
                       0.0025*(l1[st1]+l1[st5]+l5[st1]+l5[st5])+
                       0.02*(l1[st3]+l3[st1]+l3[st5]+l5[st3]);
      imn[3*(i*w+j)+1] = 0.0625*(l2[st2+1]+l2[st4+1]+l4[st2+1]+l4[st4+1])+
                         0.0125*(l1[st2+1]+l1[st4+1]+l2[st1+1]+l2[st5+1]+l4[st1+1]+l4[st5+1]+l5[st2+1]+l5[st4+1])+
                         0.1*(l2[st3+1]+l3[st2+1]+l3[st4+1]+l4[st3+1])+
                         0.16*(l3[st3+1])+
                         0.0025*(l1[st1+1]+l1[st5+1]+l5[st1+1]+l5[st5+1])+
                         0.02*(l1[st3+1]+l3[st1+1]+l3[st5+1]+l5[st3+1]);
      imn[3*(i*w+j)+2] = 0.0625*(l2[st2+2]+l2[st4+2]+l4[st2+2]+l4[st4+2])+
                         0.0125*(l1[st2+2]+l1[st4+2]+l2[st1+2]+l2[st5+2]+l4[st1+2]+l4[st5+2]+l5[st2+2]+l5[st4+2])+
                         0.1*(l2[st3+2]+l3[st2+2]+l3[st4+2]+l4[st3+2])+
                         0.16*(l3[st3+2])+
                         0.0025*(l1[st1+2]+l1[st5+2]+l5[st1+2]+l5[st5+2])+
                         0.02*(l1[st3+2]+l3[st1+2]+l3[st5+2]+l5[st3+2]);
    }
  }
  for(int i = 0; i < height; i++)
  {
    for(int j = 0; j < width; j++)
    {
      for(int m = -2; m < 3; m++)
      {
        for(int n = -2; n < 3; n++)
        {
          if((i-m)%2)
            continue;
          if((j-n)%2)
            continue;
          i1 = min(max((i-m)/2,0),h-1);
          j1 = min(max((j-n)/2,0),w-1);
          im[3*((size_t)i*width+j)]   -= 4*_w_[n+2]*_w_[m+2]*imn[3*((size_t)i1*w+j1)];
          im[3*((size_t)i*width+j)+1] -= 4*_w_[n+2]*_w_[m+2]*imn[3*((size_t)i1*w+j1)+1];
          im[3*((size_t)i*width+j)+2] -= 4*_w_[n+2]*_w_[m+2]*imn[3*((size_t)i1*w+j1)+2];
        }
      }
    }
  }
  if((w/2>=size_limit)&&(h/2>=size_limit))
  {
    laplace_image(size_limit, w,h,imn);
  }
}

static void
weighted_image(const int size_limit, int width, int height, float *im1, float *im2, float *W)
{
  int i1, j1, w = width/2, h = height/2;
  float *imn1 = im1 + width*height*3, *imn2 = im2 + width*height*3, *wn = W + width*height;

  const size_t LENGTH = width*height;
  for(size_t i = 0; i < LENGTH; i++)
  {
    im1[3*i]   = im1[3*i]*W[i] + im2[3*i]*(1.0-W[i]);
    im1[3*i+1] = im1[3*i+1]*W[i] + im2[3*i+1]*(1.0-W[i]);
    im1[3*i+2] = im1[3*i+2]*W[i] + im2[3*i+2]*(1.0-W[i]);
  }
  if((w < size_limit) || (h < size_limit))
    return;
  weighted_image(size_limit, w, h, imn1, imn2, wn);
  for(int i = 0; i < height; i++)
  {
    for(int j = 0; j < width; j++)
    {
      for(int m = -2; m < 3; m++)
      {
        for(int n = -2; n < 3; n++)
        {
          if((i-m)%2)
            continue;
          if((j-n)%2)
            continue;
          i1 = min(max((i-m)/2,0),h-1);
          j1 = min(max((j-n)/2,0),w-1);
          im1[3*((size_t)i*width+j)]   += 4*_w_[n+2]*_w_[m+2]*imn1[3*((size_t)i1*w+j1)];
          im1[3*((size_t)i*width+j)+1] += 4*_w_[n+2]*_w_[m+2]*imn1[3*((size_t)i1*w+j1)+1];
          im1[3*((size_t)i*width+j)+2] += 4*_w_[n+2]*_w_[m+2]*imn1[3*((size_t)i1*w+j1)+2];

        }
      }
    }
  }
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_shrecovery_data_t *d = (dt_iop_shrecovery_data_t *)piece->data;
  const float scale = 1.0 + d->strength;
  const int ch = piece->colors;
  /*
  #ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out,i,o) schedule(static)
  #endif
   */
  float *in = (float*)i, *out = (float*)o;
  float *W = (float*)malloc(2*sizeof(float)*roi_out->height*roi_out->width);
  float t1, t2, t3, mx;
  create_image_weight(roi_out->width, roi_out->height, ch, in, W, scale, d->mu, d->sigma);
  gauss_image_weight(d->size_limit, roi_out->width, roi_out->height, W);

  float *im1 = (float*)malloc((size_t)6*sizeof(float)*roi_out->height*roi_out->width);
  float *im2 = (float*)malloc((size_t)6*sizeof(float)*roi_out->height*roi_out->width);

  const size_t LENGTH = (size_t)roi_out->width*roi_out->height;
  for(int i = 0; i < LENGTH; i++)
  {
    im1[i*3] = in[i*ch];
    im1[i*3+1] = in[i*ch+1];
    im1[i*3+2] = in[i*ch+2];
    t1 = in[i*ch]*scale;
    t2 = in[i*ch+1]*scale;
    t3 = in[i*ch+2]*scale;
    mx = 1.0;
    mx = max(mx,t1);
    mx = max(mx,t2);
    mx = max(mx,t3);
    im2[i*3] = t1/mx;
    im2[i*3+1] = t2/mx;
    im2[i*3+2] = t3/mx;
  }
  laplace_image(d->size_limit, roi_out->width, roi_out->height, im1);
  laplace_image(d->size_limit, roi_out->width, roi_out->height, im2);
  weighted_image(d->size_limit, roi_out->width, roi_out->height, im1, im2, W);


  for(size_t i = 0; i < LENGTH; i++)
  {
    out[i*ch] = im1[3*i];
    out[i*ch + 1] = im1[3*i+1];
    out[i*ch + 2] = im1[3*i+2];
  }


  free(W);
  free(im1);
  free(im2);
}

static void
strength_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shrecovery_params_t *p = (dt_iop_shrecovery_params_t *)self->params;
  p->strength= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
mu_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shrecovery_params_t *p = (dt_iop_shrecovery_params_t *)self->params;
  p->mu = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
sigma_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shrecovery_params_t *p = (dt_iop_shrecovery_params_t *)self->params;
  p->sigma = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
size_limit_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shrecovery_params_t *p = (dt_iop_shrecovery_params_t *)self->params;
  p->size_limit = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_shrecovery_params_t *p = (dt_iop_shrecovery_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[bloom] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_shrecovery_data_t *d = (dt_iop_shrecovery_data_t *)piece->data;
  d->strength= p->strength;
  d->mu = p->mu;
  d->sigma = p->sigma;
  d->size_limit = p->size_limit;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_shrecovery_data_t));
  memset(piece->data,0,sizeof(dt_iop_shrecovery_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_shrecovery_gui_data_t *g = (dt_iop_shrecovery_gui_data_t *)self->gui_data;
  dt_iop_shrecovery_params_t *p = (dt_iop_shrecovery_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->strength);
  dtgtk_slider_set_value(g->scale2, p->mu);
  dtgtk_slider_set_value(g->scale3, p->sigma);
  dtgtk_slider_set_value(g->scale4, p->size_limit);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_shrecovery_params_t));
  module->default_params = malloc(sizeof(dt_iop_shrecovery_params_t));
  module->default_enabled = 0;
  module->priority = 280; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_shrecovery_params_t);
  module->gui_data = NULL;
  dt_iop_shrecovery_params_t tmp = (dt_iop_shrecovery_params_t)
  {
    2.0, 0.5, 0.2, 4
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_shrecovery_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_shrecovery_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_shrecovery_gui_data_t));
  dt_iop_shrecovery_gui_data_t *g = (dt_iop_shrecovery_gui_data_t *)self->gui_data;
  dt_iop_shrecovery_params_t *p = (dt_iop_shrecovery_params_t *)self->params;

  self->widget = gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING);

  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-2.0, 6.0, 0.01, p->strength, 2));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.1, 0.9, 0.01, p->mu, 2));
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.05, 0.6, 0.01, p->sigma, 2));
  g->scale4 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 2.0, 64.0, 1, p->size_limit, 0));
  dtgtk_slider_set_snap(g->scale4, 1);
  dtgtk_slider_set_label(g->scale1, NULL, _("strength"));
  dtgtk_slider_set_label(g->scale2, NULL, _("mean"));
  dtgtk_slider_set_label(g->scale3, NULL, _("deviation"));
  dtgtk_slider_set_label(g->scale4, NULL, _("minimal pyramid limit"));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->scale1), "tooltip-text", _("the strength of lighten"), (char *)NULL);
  g_object_set(G_OBJECT(g->scale2), "tooltip-text", _("optimal exposedness"), (char *)NULL);
  g_object_set(G_OBJECT(g->scale3), "tooltip-text", _("exposedness deviation"), (char *)NULL);
  g_object_set(G_OBJECT(g->scale4), "tooltip-text", _("minimal pyramid limit size"), (char *)NULL);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (strength_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (mu_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (sigma_callback), self);
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
                    G_CALLBACK (size_limit_callback), self);

}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
