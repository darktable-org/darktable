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
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE(1)

typedef struct dt_iop_velvia_params_t
{
  float saturation;
  float vibrance;
  float luminance;
  float clarity;
}
dt_iop_velvia_params_t;

typedef struct dt_iop_velvia_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkLabel  *label1,*label2,*label3;
  GtkDarktableSlider *scale1,*scale2,*scale3;       // saturation, vibrance, luminance
}
dt_iop_velvia_gui_data_t;

typedef struct dt_iop_velvia_data_t
{
  float saturation;
  float vibrance;
  float luminance;
}
dt_iop_velvia_data_t;

const char *name()
{
  return _("velvia");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES;
}

int 
groups () 
{
	return IOP_GROUP_COLOR;
}




void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_velvia_data_t *data = (dt_iop_velvia_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  
  // Apply velvia saturation
  in  = (float *)ivoid;
  out = (float *)ovoid;
  if(data->saturation <= 0.0)
  {
    memcpy(in, out, sizeof(float)*3*roi_out->width*roi_out->height);
  }
  else
  {
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out, data) schedule(static)
#endif
    for(int k=0;k<roi_out->width*roi_out->height;k++)
    {
      // calculate vibrance, and apply boost velvia saturation at least saturated pixles
      double pmax=fmax(in[3*k],fmax(in[3*k+1],in[3*k+2]));			// max value in RGB set
      double pmin=fmin(in[3*k],fmin(in[3*k+1],in[3*k+2]));			// min value in RGB set
      double plum = (pmax+pmin)/2.0;					        // pixel luminocity
      double psat =(plum<=0.5) ? (pmax-pmin)/(1e-5 + pmax+pmin): (pmax-pmin)/(1e-5 + MAX(0.0, 2.0-pmax-pmin));

      double pweight=((1.0- (1.5*psat)) + ((1+(fabs(plum-0.5)*2.0))*(1.0-data->luminance))) / (1.0+(1.0-data->luminance));		// The weight of pixel
      double saturation = ((data->saturation/100.0)*pweight)*(data->vibrance/100.0);			// So lets calculate the final affection of filter on pixel

      // Apply velvia saturation values
      double sba=1.0+saturation;
      double sda=(sba/2.0)-0.5;
      out[3*k+0]=(in[3*k+0]*(sba))-(in[3*k+1]*(sda))-(in[3*k+2]*(sda));
      out[3*k+1]=(in[3*k+1]*(sba))-(in[3*k+0]*(sda))-(in[3*k+2]*(sda));
      out[3*k+2]=(in[3*k+2]*(sba))-(in[3*k+0]*(sda))-(in[3*k+1]*(sda));  
    }
  }
}

static void
saturation_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)self->params;
  p->saturation = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
vibrance_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)self->params;
  p->vibrance = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
luminance_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)self->params;
  p->luminance= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[velvia] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_velvia_data_t *d = (dt_iop_velvia_data_t *)piece->data;
  d->saturation = p->saturation;
  d->vibrance = p->vibrance;
  d->luminance = p->luminance;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_velvia_data_t));
  memset(piece->data,0,sizeof(dt_iop_velvia_data_t));
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
  dt_iop_velvia_gui_data_t *g = (dt_iop_velvia_gui_data_t *)self->gui_data;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->saturation);
  dtgtk_slider_set_value(g->scale2, p->vibrance);
  dtgtk_slider_set_value(g->scale3, p->luminance);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_velvia_params_t));
  module->default_params = malloc(sizeof(dt_iop_velvia_params_t));
  module->default_enabled = 0;
  module->priority = 967;
  module->params_size = sizeof(dt_iop_velvia_params_t);
  module->gui_data = NULL;
  dt_iop_velvia_params_t tmp = (dt_iop_velvia_params_t){50,50,.5};
  memcpy(module->params, &tmp, sizeof(dt_iop_velvia_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_velvia_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_velvia_gui_data_t));
  dt_iop_velvia_gui_data_t *g = (dt_iop_velvia_gui_data_t *)self->gui_data;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(_("saturation")));
  g->label2 = GTK_LABEL(gtk_label_new(_("vibrance")));
  g->label3 = GTK_LABEL(gtk_label_new(_("mid-tones bias")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->saturation, 2));
  dtgtk_slider_set_format_type(g->scale1,DARKTABLE_SLIDER_FORMAT_PERCENT);
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->vibrance, 2));
  dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.010, p->luminance, 0));
  dtgtk_slider_set_format_type(g->scale3,DARKTABLE_SLIDER_FORMAT_RATIO);
  
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the amount of saturation to apply"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("the vibrance amount"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale3), "tooltip-text", _("how much to spare highlights and shadows"), (char *)NULL);
  
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (saturation_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
        G_CALLBACK (vibrance_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
        G_CALLBACK (luminance_callback), self);  
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

