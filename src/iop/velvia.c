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

DT_MODULE(1)

typedef struct dt_iop_velvia_params_t
{
  float saturation;
  float vibrance;
}
dt_iop_velvia_params_t;

typedef struct dt_iop_velvia_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkLabel  *label1,*label2;
  GtkDarktableSlider *scale1,*scale2;
}
dt_iop_velvia_gui_data_t;

typedef struct dt_iop_velvia_data_t
{
  float saturation;
  float vibrance;
}
dt_iop_velvia_data_t;

const char *name()
{
  return _("velvia");
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_velvia_data_t *data = (dt_iop_velvia_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
	
  // Get max and min pixel luminocity value in image
  double plummin=1.0;
  double plummax=0.0;
  for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
  {
	double plum=(in[0]+in[1]+in[2])/3;					
	if( plummin > plum ) plummin=plum;
	if( plummax < plum ) plummax=plum;
	in += 3;
  }
  
  // Apply velvia saturation
 in  = (float *)ivoid;
 for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
  {
    // calculate vibrance, and apply boost velvia saturation at least saturated pixles
   double pmax=fmax(in[0],fmax(in[1],in[2]));			// Max value amonug RGB set
   double pmin=fmin(in[0],fmin(in[1],in[2]));			// Min value among RGB set
   double plum = (pmax+pmin)/2.0;					// Pixel luminocity
   double psat =(plum<=0.5) ? (pmax-pmin)/(pmax+pmin): (pmax-pmin)/(2-pmax-pmin);
   //plum= (plum-plummin)*(1.0/(plummax-plummin));	        // translate plum and scale to dynamic lumin range

   double pweight=((1.0- (2.0*psat)) + (1+(fabs(plum-0.5)*2.0))) / 2.0;		// The weight of pixel
   double saturation = (data->saturation*pweight)*data->vibrance;
   
   // Apply velvia saturation values
   double sba=1.0+saturation;
   double sda=(sba/2.0)-0.5;
    out[0]=(in[0]*(sba))-(in[1]*(sda))-(in[2]*(sda));
    out[1]=(in[1]*(sba))-(in[0]*(sda))-(in[2]*(sda));
    out[2]=(in[2]*(sba))-(in[0]*(sda))-(in[1]*(sda));  
    out += 3; in += 3;
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
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_velvia_data_t));
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
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_velvia_params_t));
  module->default_params = malloc(sizeof(dt_iop_velvia_params_t));
  module->default_enabled = 0;
  module->priority = 970;
  module->params_size = sizeof(dt_iop_velvia_params_t);
  module->gui_data = NULL;
  dt_iop_velvia_params_t tmp = (dt_iop_velvia_params_t){.5,.5};
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
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(_("saturation")));
  g->label2 = GTK_LABEL(gtk_label_new(_("vibrance")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0000, 0.010, p->saturation, 3));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0000, 0.010, p->vibrance, 3));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
 
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (saturation_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
		    G_CALLBACK (vibrance_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

