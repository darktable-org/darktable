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
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE(1)

#define MAXR 8

typedef struct dt_iop_sharpen_params_t
{
  float radius, amount, threshold;
}
dt_iop_sharpen_params_t;

typedef struct dt_iop_sharpen_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkDarktableSlider *scale1, *scale2, *scale3;
}
dt_iop_sharpen_gui_data_t;

typedef struct dt_iop_sharpen_data_t
{
  float radius, amount, threshold;
}
dt_iop_sharpen_data_t;

const char *name()
{
  return _("sharpen");
}


int 
groups () 
{
	return IOP_GROUP_CORRECT;
}



void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
	dt_iop_sharpen_data_t *data = (dt_iop_sharpen_data_t *)piece->data;
  const int rad = MIN(MAXR, ceilf(data->radius * roi_in->scale / piece->iscale));
 	if(rad == 0)
  {
    memcpy(ovoid, ivoid, sizeof(float)*3*roi_out->width*roi_out->height);
    return;
  }

  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;

  float mat[2*(MAXR+1)*2*(MAXR+1)];
  const int wd = 2*rad+1;
  float *m = mat + rad*wd + rad;
  const float sigma2 = (2.5*2.5)*(data->radius*roi_in->scale/piece->iscale)*(data->radius*roi_in->scale/piece->iscale);
  float weight = 0.0f;
  // init gaussian kernel
  for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
    weight += m[l*wd + k] = expf(- (l*l + k*k)/(2.f*sigma2));
  for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
    m[l*wd + k] /= weight;

  // gauss blur the image
#ifdef _OPENMP
  #pragma omp parallel for default(none) private(in, out) shared(m, ivoid, ovoid, roi_out, roi_in) schedule(static)
#endif
  for(int j=rad;j<roi_out->height-rad;j++)
  {
    in  = ((float *)ivoid) + 3*(j*roi_in->width  + rad);
    out = ((float *)ovoid) + 3*(j*roi_out->width + rad);
    for(int i=rad;i<roi_out->width-rad;i++)
    {
      out[0] = 0.0f;
      for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
        out[0] += m[l*wd+k]*in[3*(l*roi_in->width+k)];
      out += 3; in += 3;
    }
  }
  in  = (float *)ivoid;
  out = (float *)ovoid;

  // fill unsharpened border
  for(int j=0;j<rad;j++)
    memcpy(((float*)ovoid) + 3*j*roi_out->width, ((float*)ivoid) + 3*j*roi_in->width, 3*sizeof(float)*roi_out->width);
  for(int j=roi_out->height-rad;j<roi_out->height;j++)
    memcpy(((float*)ovoid) + 3*j*roi_out->width, ((float*)ivoid) + 3*j*roi_in->width, 3*sizeof(float)*roi_out->width);
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(in, out, roi_out, roi_in) schedule(static)
#endif
  for(int j=rad;j<roi_out->height-rad;j++)
  {
    for(int i=0;i<rad;i++)
      out[3*(roi_out->width*j + i)] = in[3*(roi_in->width*j + i)];
    for(int i=roi_out->width-rad;i<roi_out->width;i++)
      out[3*(roi_out->width*j + i)] = in[3*(roi_in->width*j + i)];
  }
#ifdef _OPENMP
  #pragma omp parallel for default(none) private(in, out) shared(data, ivoid, ovoid, roi_out, roi_in) schedule(static)
#endif
  // subtract blurred image, if diff > thrs, add *amount to orginal image
  for(int j=0;j<roi_out->height;j++)
  { 
    in  = (float *)ivoid + j*3*roi_out->width;
    out = (float *)ovoid + j*3*roi_out->width;
	  
	  for(int i=0;i<roi_out->width;i++)
    {
      out[1] = in[1];
      out[2] = in[2];
      const float diff = in[0] - out[0];
      if(fabsf(diff) > data->threshold)
      {
        const float detail = copysignf(fmaxf(fabsf(diff) - data->threshold, 0.0), diff);
        out[0] = fmaxf(0.0, in[0] + detail*data->amount);
      }
      else out[0] = in[0];
      out += 3; in += 3;
    }
	}
}

static void
radius_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_sharpen_params_t *p = (dt_iop_sharpen_params_t *)self->params;
  p->radius = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
amount_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_sharpen_params_t *p = (dt_iop_sharpen_params_t *)self->params;
  p->amount = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
threshold_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_sharpen_params_t *p = (dt_iop_sharpen_params_t *)self->params;
  p->threshold = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_sharpen_params_t *p = (dt_iop_sharpen_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[sharpen] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_sharpen_data_t *d = (dt_iop_sharpen_data_t *)piece->data;
  d->radius = p->radius;
  d->amount = p->amount;
  d->threshold = p->threshold;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_sharpen_data_t));
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
  dt_iop_sharpen_gui_data_t *g = (dt_iop_sharpen_gui_data_t *)self->gui_data;
  dt_iop_sharpen_params_t *p = (dt_iop_sharpen_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->radius);
  dtgtk_slider_set_value(g->scale2, p->amount);
  dtgtk_slider_set_value(g->scale3, p->threshold);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_sharpen_data_t));
  module->params = malloc(sizeof(dt_iop_sharpen_params_t));
  module->default_params = malloc(sizeof(dt_iop_sharpen_params_t));
  module->default_enabled = 1;
  module->priority = 850;
  module->params_size = sizeof(dt_iop_sharpen_params_t);
  module->gui_data = NULL;
  dt_iop_sharpen_params_t tmp = (dt_iop_sharpen_params_t){2.0, 0.5, 0.005};
  memcpy(module->params, &tmp, sizeof(dt_iop_sharpen_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_sharpen_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_sharpen_gui_data_t));
  dt_iop_sharpen_gui_data_t *g = (dt_iop_sharpen_gui_data_t *)self->gui_data;
  dt_iop_sharpen_params_t *p = (dt_iop_sharpen_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);


  GtkWidget *widget;
  widget = dtgtk_reset_label_new(_("radius"), self, &p->radius, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), widget, TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new(_("amount"), self, &p->amount, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), widget, TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new(_("threshold"), self, &p->threshold, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), widget, TRUE, TRUE, 0);

  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 8.0000, 0.100, p->radius, 3));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 2.0000, 0.010, p->amount, 3));
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0000, 0.001, p->threshold, 3));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (radius_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (amount_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (threshold_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

#undef MAXR
