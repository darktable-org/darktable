/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE(1)

typedef struct dt_iop_bloom_params_t
{
  float size;
  float threshold;
  float strength;
}
dt_iop_bloom_params_t;

typedef struct dt_iop_bloom_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkWidget  *label1,*label2,*label3;			// size,threshold,strength
  GtkDarktableSlider *scale1,*scale2,*scale3;       // size,threshold,strength
}
dt_iop_bloom_gui_data_t;

typedef struct dt_iop_bloom_data_t
{
  float size;
  float threshold;
  float strength;
}
dt_iop_bloom_data_t;

const char *name()
{
  return _("bloom");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES;
}

int 
groups () 
{
  return IOP_GROUP_EFFECT;
}


#define GAUSS(a,b,c,x) (a*pow(2.718281828,(-pow((x-b),2)/(pow(c,2)))))


void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_bloom_data_t *data = (dt_iop_bloom_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  
  in  = (float *)ivoid;
  out = (float *)ovoid;

    
  // gauss blur the L
  int MAXR=128;
  int radius=MAXR*(data->size/100.0);
  int rad = MIN(MAXR, ceilf(radius* roi_in->scale / piece->iscale));
  float mat[2*(MAXR+1)*2*(MAXR+1)];
  const int wd = 2*rad+1;
  float *m = mat + rad*wd + rad;
  const float sigma2 = (2.5*2.5)*(radius*roi_in->scale/piece->iscale)*(radius*roi_in->scale/piece->iscale);
  float weight = 0.0f;
  // init gaussian kernel
  for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
    weight += m[l*wd + k] = expf(- (l*l + k*k)/(2.f*sigma2));
  for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
    m[l*wd + k] /= weight;

  /* gather light by threshold */
  memcpy(out,in,roi_out->width*roi_out->height*ch*sizeof(float));
  const float scale = 1.0 / exp2f ( -0.5*(data->strength/100.0));
#ifdef _OPENMP
  #pragma omp parallel for default(none) private(in, out) shared(m, ivoid, ovoid, roi_out, roi_in) schedule(static)
#endif
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
    out[0] *= scale;
    if (out[0]<data->threshold) 
      out[0]=0.0;
    out +=ch;
  }
  
/* apply gaussian pass on gathered light */
#ifdef _OPENMP
  #pragma omp parallel for default(none) private(in, out) shared(m, ivoid, ovoid, roi_out, roi_in) schedule(static)
#endif
  for (int j=rad;j<roi_out->height-rad;j++)
  {
    in  = ((float *)ivoid) + ch*(j*roi_in->width  + rad);
    out = ((float *)ovoid) + ch*(j*roi_out->width + rad);
    for(int i=rad;i<roi_out->width-rad;i++)
    {
      //out[0] = 0.0f;
      for (int l=-rad;l<=rad;l++) for (int k=-rad;k<=rad;k++) 
        out[0] += m[l*wd+k]*out[ch*(l*roi_in->width+k)];
      out += ch; in += ch;
    }
  }
  in  = (float *)ivoid;
  out = (float *)ovoid;

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(in, out, roi_out, roi_in) schedule(static)
#endif
  for (int j=rad;j<roi_out->height-rad;j++)
  {
    for (int i=0;i<rad;i++)
      out[ch*(roi_out->width*j + i)] = out[ch*(roi_out->width*j + i)];
    for (int i=roi_out->width-rad;i<roi_out->width;i++)
      out[ch*(roi_out->width*j + i)] = out[ch*(roi_out->width*j + i)];
  }

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out, data) schedule(static)
#endif
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
      float lightness = out[ch*k];
      out[ch*k] = 100-(((100-in[ch*k])*(100-lightness))/100); // Screen blend
      out[ch*k+1] = in[ch*k+1];
      out[ch*k+2] = in[ch*k+2];
  }
  
}

static void
strength_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)self->params;
  p->strength = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
threshold_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)self->params;
  p->threshold = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
size_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)self->params;
  p->size= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[bloom] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_bloom_data_t *d = (dt_iop_bloom_data_t *)piece->data;
  d->strength = p->strength;
  d->size = p->size;
  d->threshold = p->threshold;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_bloom_data_t));
  memset(piece->data,0,sizeof(dt_iop_bloom_data_t));
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
  dt_iop_bloom_gui_data_t *g = (dt_iop_bloom_gui_data_t *)self->gui_data;
  dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->size);
  dtgtk_slider_set_value(g->scale2, p->threshold);
  dtgtk_slider_set_value(g->scale3, p->strength);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_bloom_params_t));
  module->default_params = malloc(sizeof(dt_iop_bloom_params_t));
  module->default_enabled = 0;
  module->priority = 301;
  module->params_size = sizeof(dt_iop_bloom_params_t);
  module->gui_data = NULL;
  dt_iop_bloom_params_t tmp = (dt_iop_bloom_params_t){10,98,25};
  memcpy(module->params, &tmp, sizeof(dt_iop_bloom_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_bloom_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_bloom_gui_data_t));
  dt_iop_bloom_gui_data_t *g = (dt_iop_bloom_gui_data_t *)self->gui_data;
  dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  
  g->label1 = dtgtk_reset_label_new(_("size"), self, &p->size, sizeof(float));
  g->label2 = dtgtk_reset_label_new(_("threshold"), self, &p->threshold, sizeof(float));
  g->label3 = dtgtk_reset_label_new(_("strength"), self, &p->strength, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->size, 2));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->threshold, 2));
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->strength, 2));
  dtgtk_slider_set_format_type(g->scale1,DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_format_type(g->scale3,DARKTABLE_SLIDER_FORMAT_PERCENT);
  
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the size of bloom"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("the threshold of light"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale3), "tooltip-text", _("the strength of bloom"), (char *)NULL);
  
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (size_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (threshold_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (strength_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

