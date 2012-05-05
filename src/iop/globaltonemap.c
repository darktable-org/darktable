/*
    This file is part of darktable,
    copyright (c) 2012 Henrik Andersson.

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
#include "bauhaus/bauhaus.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include <xmmintrin.h>

// NaN-safe clip: NaN compares false and will result in 0.0
#define CLIP(x) (((x)>=0.0)?((x)<=1.0?(x):1.0):0.0)
DT_MODULE(2)

typedef enum _iop_operator_t
{
  OPERATOR_REINHARD,
  OPERATOR_FILMIC,
  OPERATOR_DRAGO
} _iop_operator_t;

typedef struct dt_iop_global_tonemap_params_t
{
  _iop_operator_t operator;
  struct {
    float bias;
    float max_light; // cd/m2
  } drago;
}
dt_iop_global_tonemap_params_t;

typedef struct dt_iop_global_tonemap_gui_data_t
{
  GtkWidget *operator;
  struct {
    GtkWidget *ui;
    GtkWidget *bias;
    GtkWidget *max_light;
  } drago;
}
dt_iop_global_tonemap_gui_data_t;

typedef struct dt_iop_global_tonemap_data_t
{
  _iop_operator_t operator;
  struct {
    float bias;
    float max_light; // cd/m2
  } drago;

}
dt_iop_global_tonemap_data_t;

const char *name()
{
  return _("global tonemap");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int
groups ()
{
  return IOP_GROUP_TONE;
}

#if 0
void init_key_accels(dt_iop_module_so_t *self)
{
}

void connect_key_accels(dt_iop_module_t *self)
{
}
#endif

static inline void process_reinhard(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, 
				    void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
				    dt_iop_global_tonemap_data_t *data)
{
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, in, out, data) schedule(static)
#endif
    for(int k=0; k<roi_out->width*roi_out->height; k++)
    {
      float *inp = in + ch*k;
      float *outp = out + ch*k;
      float l = inp[0]/100.0;
      outp[0] = 100.0 * (l/(1.0f+l));
      outp[1] = inp[1];
      outp[2] = inp[2];
    }
}

static inline void process_drago(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, 
				    void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
				    dt_iop_global_tonemap_data_t *data)
{
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

  /* precalcs */
  const float eps = 0.0001f;
  float lwmax = eps;
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    float *inp = in + ch*k;
    lwmax = MAX(lwmax, (inp[0]*0.01f));
  }
  const float ldc = data->drago.max_light * 0.01 / log10f(lwmax+1); 
  const float bl = logf(MAX(eps, data->drago.bias)) / logf(0.5);

#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, in, out, lwmax) schedule(static)
#endif
    for(int k=0; k<roi_out->width*roi_out->height; k++)
    {
      float *inp = in + ch*k;
      float *outp = out + ch*k;
      float lw = inp[0]*0.01f;
      outp[0] = 100.0f * (ldc * logf(MAX(eps, lw + 1.0f)) / logf(MAX(eps, 2.0f + (powf(lw/lwmax,bl)) * 8.0f)));
      outp[1] = inp[1];
      outp[2] = inp[2];
    }
}

static inline void process_filmic(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, 
				    void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
				    dt_iop_global_tonemap_data_t *data)
{
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, in, out, data) schedule(static)
#endif
    for(int k=0; k<roi_out->width*roi_out->height; k++)
    {
      float *inp = in + ch*k;
      float *outp = out + ch*k;
      float l = inp[0]/100.0;
      float x = MAX(0.0f, l-0.004f);
      outp[0] = 100.0 * ((x*(6.2*x+.5))/(x*(6.2*x+1.7)+0.06));
      outp[1] = inp[1];
      outp[2] = inp[2];
    }
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_global_tonemap_data_t *data = (dt_iop_global_tonemap_data_t *)piece->data;

  switch(data->operator) {
  case OPERATOR_REINHARD:
    process_reinhard(self, piece, ivoid, ovoid, roi_in, roi_out, data);
    break;
  case OPERATOR_DRAGO:
    process_drago(self, piece, ivoid, ovoid, roi_in, roi_out, data);
    break;
  case OPERATOR_FILMIC:
    process_filmic(self, piece, ivoid, ovoid, roi_in, roi_out, data);
    break;
  }
}

static void
operator_callback (GtkWidget *combobox, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_global_tonemap_gui_data_t *g = (dt_iop_global_tonemap_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)self->params;
  p->operator = dt_bauhaus_combobox_get(combobox);
  
  gtk_widget_set_visible(g->drago.ui, FALSE);
  /* show ui for selected operator */
  if (p->operator == OPERATOR_DRAGO)
    gtk_widget_set_visible(g->drago.ui, TRUE);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
_drago_bias_callback (GtkWidget *w, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)self->params;
  p->drago.bias = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
_drago_max_light_callback (GtkWidget *w, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)self->params;
  p->drago.max_light = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)p1;
  dt_iop_global_tonemap_data_t *d = (dt_iop_global_tonemap_data_t *)piece->data;
  d->operator = p->operator;
  d->drago.bias = p->drago.bias;
  d->drago.max_light = p->drago.max_light;
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_global_tonemap_data_t));
  memset(piece->data,0,sizeof(dt_iop_global_tonemap_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_global_tonemap_gui_data_t *g = (dt_iop_global_tonemap_gui_data_t *)self->gui_data;
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)module->params;
  dt_bauhaus_combobox_set(g->operator, p->operator);
  /* drago */
  dt_bauhaus_slider_set(g->drago.bias, p->drago.bias);
  dt_bauhaus_slider_set(g->drago.max_light, p->drago.max_light);

}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_global_tonemap_params_t));
  module->default_params = malloc(sizeof(dt_iop_global_tonemap_params_t));
  module->default_enabled = 0;
  module->priority = 353; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_global_tonemap_params_t);
  module->gui_data = NULL;
  dt_iop_global_tonemap_params_t tmp = (dt_iop_global_tonemap_params_t)
  {
    OPERATOR_DRAGO,
    {0.85f, 100.0f}
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_global_tonemap_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_global_tonemap_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_global_tonemap_gui_data_t));
  dt_iop_global_tonemap_gui_data_t *g = (dt_iop_global_tonemap_gui_data_t *)self->gui_data;
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)self->params;

  self->widget = gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE);

  /* operator */
  g->operator = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->operator,_("operator"));

  dt_bauhaus_combobox_add(g->operator, "reinhard");
  dt_bauhaus_combobox_add(g->operator, "filmic");
  dt_bauhaus_combobox_add(g->operator, "drago");

  g_object_set(G_OBJECT(g->operator), "tooltip-text", _("the global tonemap operator"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->operator), "value-changed",
                    G_CALLBACK (operator_callback), self);  
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->operator), TRUE, TRUE, 0);

  /* drago bias */
  g->drago.ui = gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE);
  g->drago.bias = dt_bauhaus_slider_new_with_range(self,0.5, 1.0, 0.05, p->drago.bias, 2);
  dt_bauhaus_widget_set_label(g->drago.bias,_("bias"));
  g_object_set(G_OBJECT(g->drago.bias), "tooltip-text", _("the bias for tonemapper controls the linearity, the higher the more details in blacks"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->drago.bias), "value-changed",
                    G_CALLBACK (_drago_bias_callback), self);
  gtk_box_pack_start(GTK_BOX(g->drago.ui), GTK_WIDGET(g->drago.bias), TRUE, TRUE, 0);
  

  /* drago bias */
  g->drago.max_light = dt_bauhaus_slider_new_with_range(self,1, 500, 10, p->drago.max_light, 2);
  dt_bauhaus_widget_set_label(g->drago.max_light,_("target"));
  g_object_set(G_OBJECT(g->drago.max_light), "tooltip-text", _("the target light for tonemapper specified as cd/m2"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->drago.max_light), "value-changed",
                    G_CALLBACK (_drago_max_light_callback), self);
  gtk_box_pack_start(GTK_BOX(g->drago.ui), GTK_WIDGET(g->drago.max_light), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->drago.ui), TRUE, TRUE, 0);

  /* set operator */
  dt_bauhaus_combobox_set(g->operator,p->operator);

}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
