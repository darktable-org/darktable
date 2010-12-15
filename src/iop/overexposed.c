/*
    This file is part of darktable,
    copyright (c) 2010 Tobias Ellinghaus.

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
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"

DT_MODULE(1)

typedef struct dt_iop_overexposed_params_t{
	float lower;
	float upper;
}dt_iop_overexposed_params_t;

typedef struct dt_iop_overexposed_gui_data_t{
	GtkVBox             *vbox1;
	GtkVBox             *vbox2;
	GtkWidget           *label1;
	GtkWidget           *label2;
	GtkDarktableSlider  *lower;
	GtkDarktableSlider  *upper;
}dt_iop_overexposed_gui_data_t;

typedef struct dt_iop_overexposed_data_t{
	float lower;
	float upper;
}dt_iop_overexposed_data_t;

const char* name(){
	return _("overexposed");
}

int groups(){
	return IOP_GROUP_COLOR;
}

// FIXME: I'm not sure if this is the best test (all >= / <= threshold), but it seems to work.
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out){
	dt_iop_overexposed_data_t *data = (dt_iop_overexposed_data_t *)piece->data;
	float lower  = data->lower / 100.0;
	float upper  = data->upper / 100.0;
	float *in    = (float *)i;
	float *out   = (float *)o;
	const int ch = piece->colors;

	if(self->dev->gui_attached){
#ifdef _OPENMP
	#pragma omp parallel for default(none) shared(roi_out, in, out, data, lower, upper) schedule(static)
#endif
		for(int k=0;k<roi_out->width*roi_out->height;k++){
			if(in[ch*k+0] >= upper && in[ch*k+1] >= upper && in[ch*k+2] >= upper){
				out[ch*k+0] = 1;
				out[ch*k+1] = 0;
				out[ch*k+2] = 0;
			} else if(in[ch*k+0] <= lower && in[ch*k+1] <= lower && in[ch*k+2] <= lower){
				out[ch*k+0] = 0;
				out[ch*k+1] = 0;
				out[ch*k+2] = 1;
			} else {
				out[ch*k+0] = in[ch*k+0];
				out[ch*k+1] = in[ch*k+1];
				out[ch*k+2] = in[ch*k+2];
			}
		}
	} else {
#ifdef _OPENMP
	#pragma omp parallel for default(none) shared(roi_out, in, out) schedule(static)
#endif
		for(int k=0;k<roi_out->width*roi_out->height;k++){
			out[ch*k+0] = in[ch*k+0];
			out[ch*k+1] = in[ch*k+1];
			out[ch*k+2] = in[ch*k+2];
		}  
	}
}

static void lower_callback (GtkDarktableSlider *slider, gpointer user_data){
	dt_iop_module_t *self = (dt_iop_module_t *)user_data;
	if(self->dt->gui->reset) return;
	dt_iop_overexposed_params_t *p = (dt_iop_overexposed_params_t *)self->params;
	p->lower = dtgtk_slider_get_value(slider);
	dt_dev_add_history_item(darktable.develop, self);
}

static void upper_callback (GtkDarktableSlider *slider, gpointer user_data){
	dt_iop_module_t *self = (dt_iop_module_t *)user_data;
	if(self->dt->gui->reset) return;
	dt_iop_overexposed_params_t *p = (dt_iop_overexposed_params_t *)self->params;
	p->upper = dtgtk_slider_get_value(slider);
	dt_dev_add_history_item(darktable.develop, self);
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece){
	dt_iop_overexposed_params_t *p = (dt_iop_overexposed_params_t *)p1;
	dt_iop_overexposed_data_t *d   = (dt_iop_overexposed_data_t *)piece->data;
	d->lower = p->lower;
	d->upper = p->upper;
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece){
	piece->data = malloc(sizeof(dt_iop_overexposed_data_t));
	memset(piece->data,0,sizeof(dt_iop_overexposed_data_t));
	self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece){
	free(piece->data);
}

void gui_update(struct dt_iop_module_t *self){
	dt_iop_module_t *module = (dt_iop_module_t *)self;
	dt_iop_overexposed_gui_data_t *g = (dt_iop_overexposed_gui_data_t *)self->gui_data;
	dt_iop_overexposed_params_t *p   = (dt_iop_overexposed_params_t *)module->params;
	dtgtk_slider_set_value(g->lower, p->lower);
	dtgtk_slider_set_value(g->upper, p->upper);
}

void init(dt_iop_module_t *module){
	module->params                  = malloc(sizeof(dt_iop_overexposed_params_t));
	module->default_params          = malloc(sizeof(dt_iop_overexposed_params_t));
	module->default_enabled         = 0;
	module->priority                = 999;
	module->params_size             = sizeof(dt_iop_overexposed_params_t);
	module->gui_data                = NULL;
	dt_iop_overexposed_params_t tmp = (dt_iop_overexposed_params_t){2,98};
	memcpy(module->params, &tmp, sizeof(dt_iop_overexposed_params_t));
	memcpy(module->default_params, &tmp, sizeof(dt_iop_overexposed_params_t));
}

void cleanup(dt_iop_module_t *module){
	if(module->gui_data != NULL)
		free(module->gui_data);
	module->gui_data = NULL;
	if(module->params != NULL)
		free(module->params);
	module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self){
	self->gui_data                   = malloc(sizeof(dt_iop_overexposed_gui_data_t));
	dt_iop_overexposed_gui_data_t *g = (dt_iop_overexposed_gui_data_t *)self->gui_data;
	dt_iop_overexposed_params_t *p   = (dt_iop_overexposed_params_t *)self->params;

	self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
	g->vbox1     = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
	g->vbox2     = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
	gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
	gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
	
	g->label1 = dtgtk_reset_label_new(_("lower threshold"), self, &p->lower, sizeof(float));
	g->label2 = dtgtk_reset_label_new(_("upper threshold"), self, &p->upper, sizeof(float));
	
	gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
	g->lower = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->lower, 2));
	dtgtk_slider_set_format_type(g->lower,DARKTABLE_SLIDER_FORMAT_PERCENT);
	g->upper = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->upper, 2));
	dtgtk_slider_set_format_type(g->upper,DARKTABLE_SLIDER_FORMAT_PERCENT);
	
	gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->lower), TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->upper), TRUE, TRUE, 0);
	gtk_object_set(GTK_OBJECT(g->lower), "tooltip-text", _("threshold of what shall be considered underexposed"), (char *)NULL);
	gtk_object_set(GTK_OBJECT(g->upper), "tooltip-text", _("threshold of what shall be considered overexposed"), (char *)NULL);
	
	g_signal_connect (G_OBJECT (g->lower), "value-changed", G_CALLBACK (lower_callback), self);
	g_signal_connect (G_OBJECT (g->upper), "value-changed", G_CALLBACK (upper_callback), self);  
}

void gui_cleanup(struct dt_iop_module_t *self){
	if(self->gui_data)
		free(self->gui_data);
	self->gui_data = NULL;
}
