/*
    This file is part of darktable,
    copyright (c) 2010 Tobias Ellinghaus.
    copyright (c) 2011-2012 henrik andersson.

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
#include <cairo.h>
#include "bauhaus/bauhaus.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"

DT_MODULE(1)

typedef struct dt_iop_overexposed_params_t
{
  float lower;
  float upper;
} dt_iop_overexposed_params_t;

typedef struct dt_iop_overexposed_gui_data_t
{
  GtkWidget           *lower, *upper;
// 	int                  width;
// 	int                  height;
// 	unsigned char       *mask;
} dt_iop_overexposed_gui_data_t;

typedef struct dt_iop_overexposed_data_t
{
  float lower;
  float upper;
} dt_iop_overexposed_data_t;

const char* name()
{
  return _("overexposed");
}

int groups()
{
  return IOP_GROUP_COLOR;
}

#if 0 // BAUHAUS dont support keyaccels yet.
void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "lower threshold"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "upper threshold"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_overexposed_gui_data_t *g =
      (dt_iop_overexposed_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "lower threshold", GTK_WIDGET(g->lower));
  dt_accel_connect_slider_iop(self, "upper threshold", GTK_WIDGET(g->upper));
}
#endif

// FIXME: I'm not sure if this is the best test (all >= / <= threshold), but it seems to work.
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_overexposed_data_t *data = (dt_iop_overexposed_data_t *)piece->data;
  float lower  = data->lower / 100.0;
  float upper  = data->upper / 100.0;

  float *in    = (float *)i;
  float *out   = (float *)o;
  const int ch = piece->colors;

// 	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, roi_out->width);

// 	if(((dt_iop_overexposed_gui_data_t*)self->gui_data)->mask != NULL)
// 		free(((dt_iop_overexposed_gui_data_t*)self->gui_data)->mask);
// 	((dt_iop_overexposed_gui_data_t*)self->gui_data)->mask = calloc(sizeof(char), 2*stride*roi_out->height);

// 	((dt_iop_overexposed_gui_data_t*)self->gui_data)->width  = roi_out->width;
// 	((dt_iop_overexposed_gui_data_t*)self->gui_data)->height = roi_out->height;
// 	char *mask = (char*)((dt_iop_overexposed_gui_data_t*)self->gui_data)->mask;

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out, upper, lower) schedule(static)
// 	#pragma omp parallel for default(none) shared(roi_out, in, out, upper, lower, mask, stride) schedule(static)
#endif
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    float *inp = in + ch*k;
    float *outp = out + ch*k;
// 		int x=k%roi_out->width;
// 		int y=k/roi_out->width;
    if(inp[0] >= upper || inp[1] >= upper || inp[2] >= upper)
    {
// 			mask[y*stride+x] = 255;
      outp[0] = 1;
      outp[1] = 0;
      outp[2] = 0;
    }
    else if(inp[0] <= lower && inp[1] <= lower && inp[2] <= lower)
    {
// 			mask[(y*stride+x)+(stride*roi_out->height)] = 255;
      outp[0] = 0;
      outp[1] = 0;
      outp[2] = 1;
    }
    else
    {
      // TODO: memcpy()?
      outp[0] = inp[0];
      outp[1] = inp[1];
      outp[2] = inp[2];
    }
  }
}

// void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery){
// 	dt_iop_overexposed_gui_data_t *data = (dt_iop_overexposed_gui_data_t*)self->gui_data;
// 	if(data->mask == NULL)
// 		return;

// 	int w = data->width;
// 	int h = data->height;
// 	unsigned char* mask = data->mask;
// 	g_print("P2\n%d %d\n255\n", w, h);
// 	for(int i=0; i<w*h; ++i)
// 		g_print("%d ", mask[i]);
// 	g_print("\n");
/*
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, w);
	cairo_surface_t *m1 = cairo_image_surface_create_for_data(data->mask, CAIRO_FORMAT_A8, w, h, stride);
	cairo_surface_t *m2 = cairo_image_surface_create_for_data(data->mask+stride*h, CAIRO_FORMAT_A8, w, h, stride);

	cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);
	cairo_mask_surface(cr, m1, 0, 0);
	cairo_fill(cr);

	cairo_set_source_rgb(cr, 0.0, 0.0, 1.0);
	cairo_mask_surface(cr, m2, 0, 0);
	cairo_fill(cr);
*/
// 	unsigned char lower  = 255.0 * dtgtk_slider_get_value(data->lower) / 100.0;
// 	unsigned char upper; upper  = 255.0 * dtgtk_slider_get_value(data->upper) / 100.0;

// 	cairo_surface_t *surface = cairo_get_target(cr);
// 	unsigned char* s = cairo_image_surface_get_data(surface);
// 	int stride       = cairo_image_surface_get_stride(surface);
// #ifdef _OPENMP
// 	#pragma omp parallel for default(none) shared(stride, s, height, width, upper) schedule(static)
// #endif
// 	for(int y = 7; y<height+9; ++y){
// 		for(int x=7; x<width+9; ++x){
// 			if(s[4*x+0+stride*y] >= upper && s[4*x+1+stride*y] >= upper && s[4*x+2+stride*y] >= upper){ //FIXME: is the order the same on MAC?
// 				s[4*x+0+stride*y] = 0;   //B
// 				s[4*x+1+stride*y] = 0;   //G
// 				s[4*x+2+stride*y] = 255; //R
// 			}
// 		}
// 	}
// }

static void lower_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_overexposed_params_t *p = (dt_iop_overexposed_params_t *)self->params;
  p->lower = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void upper_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_overexposed_params_t *p = (dt_iop_overexposed_params_t *)self->params;
  p->upper = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_overexposed_params_t *p = (dt_iop_overexposed_params_t *)p1;
  dt_iop_overexposed_data_t *d   = (dt_iop_overexposed_data_t *)piece->data;
  d->lower = p->lower;
  d->upper = p->upper;
  if(pipe->type != DT_DEV_PIXELPIPE_FULL) piece->enabled = 0;
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_overexposed_data_t));
  memset(piece->data,0,sizeof(dt_iop_overexposed_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
// 	if(((dt_iop_overexposed_gui_data_t*)(self->gui_data))->mask != NULL)
// 		free(((dt_iop_overexposed_gui_data_t*)(self->gui_data))->mask);
// 	((dt_iop_overexposed_gui_data_t*)(self->gui_data))->mask = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_overexposed_gui_data_t *g = (dt_iop_overexposed_gui_data_t *)self->gui_data;
  dt_iop_overexposed_params_t *p   = (dt_iop_overexposed_params_t *)module->params;
  dt_bauhaus_slider_set(g->lower, p->lower);
  dt_bauhaus_slider_set(g->upper, p->upper);
}

void init(dt_iop_module_t *module)
{
  module->params                  = malloc(sizeof(dt_iop_overexposed_params_t));
  module->default_params          = malloc(sizeof(dt_iop_overexposed_params_t));
  module->default_enabled         = 0;
  module->priority = 941; // module order created by iop_dependencies.py, do not edit!
  module->params_size             = sizeof(dt_iop_overexposed_params_t);
  module->gui_data                = NULL;
  dt_iop_overexposed_params_t tmp = (dt_iop_overexposed_params_t)
  {
    2,98
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_overexposed_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_overexposed_params_t));
}

void cleanup(dt_iop_module_t *module)
{
// 	if(((dt_iop_overexposed_gui_data_t*)(module->gui_data))->mask != NULL)
// 		free(((dt_iop_overexposed_gui_data_t*)(module->gui_data))->mask);
// 	((dt_iop_overexposed_gui_data_t*)(module->gui_data))->mask = NULL;
  if(module->gui_data != NULL)
    free(module->gui_data);
  module->gui_data = NULL;
  if(module->params != NULL)
    free(module->params);
  module->params = NULL;
}

static void _iop_overexposed_quickbutton(GtkWidget *w, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  gtk_button_clicked(GTK_BUTTON(self->off));

}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data                   = malloc(sizeof(dt_iop_overexposed_gui_data_t));
  dt_iop_overexposed_gui_data_t *g = (dt_iop_overexposed_gui_data_t *)self->gui_data;
  dt_iop_overexposed_params_t *p   = (dt_iop_overexposed_params_t *)self->params;

// 	g->mask = NULL;

  self->widget =gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE);

  /* lower */
  g->lower = dt_bauhaus_slider_new_with_range(self,0.0, 100.0, 0.1, p->lower, 2);
  dt_bauhaus_slider_set_format(g->lower,"%.0f%%");
  dt_bauhaus_widget_set_label(g->lower,_("lower threshold"));
  g_object_set(G_OBJECT(g->lower), "tooltip-text", _("threshold of what shall be considered underexposed"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->lower), "value-changed", 
		    G_CALLBACK (lower_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->lower), TRUE, TRUE, 0);

  /* upper */
  g->upper = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 0.1, p->upper, 2);
  dt_bauhaus_slider_set_format(g->upper,"%.0f%%");
  dt_bauhaus_widget_set_label(g->upper,_("upper threshold"));  
  g_object_set(G_OBJECT(g->upper), "tooltip-text", _("threshold of what shall be considered overexposed"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->upper), "value-changed", 
		    G_CALLBACK (upper_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->upper), TRUE, TRUE, 0);


  /* add quicktool button for enable/disable the plugin */
  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_overexposed, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
  g_object_set(G_OBJECT(button), "tooltip-text", _("toggle over/under exposed indication"),
               (char *)NULL);
  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (_iop_overexposed_quickbutton),
                    self);
  dt_view_manager_module_toolbox_add(darktable.view_manager, button);



}

void gui_cleanup(struct dt_iop_module_t *self)
{
  if(self->gui_data)
    free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
