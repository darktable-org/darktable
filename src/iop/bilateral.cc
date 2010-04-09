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
extern "C"
{
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
#include "iop/Permutohedral.h"
#include <gtk/gtk.h>
#include <inttypes.h>

/**
 * implementation of the 5d-color bilateral filter using andrew adams et al.'s
 * permutohedral lattice, which they kindly provided online as c++ code, under new bsd license.
 */

DT_MODULE(1)

typedef struct dt_iop_bilateral_params_t
{
  // standard deviations of the gauss to use for blurring in the dimensions x,y,r,g,b (or L*,a*,b*)
  float sigma[5];
}
dt_iop_bilateral_params_t;

typedef struct dt_iop_bilateral_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkLabel  *label1, *label2, *label3, *label4, *label5;
  GtkDarktableSlider *scale1, *scale2, *scale3, *scale4, *scale5;
}
dt_iop_bilateral_gui_data_t;

typedef struct dt_iop_bilateral_data_t
{
  float sigma[5];
}
dt_iop_bilateral_data_t;

const char *name()
{
  return _("bilateral filter");
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_bilateral_data_t *data = (dt_iop_bilateral_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;

  float sigma[5], maxs = 0.0f;
  for(int k=0;k<5;k++)
  {
    // TODO: is this really correct ?
    sigma[k] = data->sigma[k] * roi_in->scale / piece->iscale;
    maxs = fmaxf(maxs, sigma[k]);
    sigma[k] = 1.0f/sigma[k];
  }
  if(maxs < 1.0)
  {
    memcpy(out, in, sizeof(float)*3*roi_out->width*roi_out->height);
    return;
  }

	PermutohedralLattice lattice(5, 5, roi_in->width*roi_in->height);
	
	// Splat into the lattice
	//printf("Splatting...\n");

  for(int j=0;j<roi_in->height;j++) for(int i=0;i<roi_in->width;i++)
  {
    float pos[5] = {i*sigma[0], j*sigma[1], in[0]*sigma[2], in[1]*sigma[3], in[2]*sigma[4]};
    float val[5] = {i, j, in[0], in[1], in[2]};
    lattice.splat(pos, val);
    in += 3;
	}
	
	// Blur the lattice
	//printf("Blurring...\n");
	lattice.blur();
	
	// Slice from the lattice
	//printf("Slicing...\n");  
	
	lattice.beginSlice();
  for(int j=0;j<roi_in->height;j++) for(int i=0;i<roi_in->width;i++)
  {
    float pos[5];
    lattice.slice(pos);
    for(int k=0;k<3;k++) out[k] = pos[2+k];
    out += 3;
  }
}

static void
sigma_callback (GtkDarktableSlider *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_bilateral_params_t   *p = (dt_iop_bilateral_params_t *)self->params;
  dt_iop_bilateral_gui_data_t *g = (dt_iop_bilateral_gui_data_t *)self->params;
  int i = 0;
  if     (slider == g->scale1) i = 0;
  else if(slider == g->scale2) i = 1;
  else if(slider == g->scale3) i = 2;
  else if(slider == g->scale4) i = 3;
  else if(slider == g->scale5) i = 4;
  p->sigma[i] = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_bilateral_params_t *p = (dt_iop_bilateral_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[bilateral] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_bilateral_data_t *d = (dt_iop_bilateral_data_t *)piece->data;
  for(int k=0;k<5;k++) d->sigma[k] = p->sigma[k];
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_bilateral_data_t));
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
  dt_iop_bilateral_gui_data_t *g = (dt_iop_bilateral_gui_data_t *)self->gui_data;
  dt_iop_bilateral_params_t *p = (dt_iop_bilateral_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->sigma[0]);
  dtgtk_slider_set_value(g->scale2, p->sigma[1]);
  dtgtk_slider_set_value(g->scale3, p->sigma[2]);
  dtgtk_slider_set_value(g->scale4, p->sigma[3]);
  dtgtk_slider_set_value(g->scale5, p->sigma[4]);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_bilateral_data_t));
  module->params = (dt_iop_params_t *)malloc(sizeof(dt_iop_bilateral_params_t));
  module->default_params = (dt_iop_params_t *)malloc(sizeof(dt_iop_bilateral_params_t));
  module->default_enabled = 0;
  module->priority = 200;
  module->params_size = sizeof(dt_iop_bilateral_params_t);
  module->gui_data = NULL;
  dt_iop_bilateral_params_t tmp = (dt_iop_bilateral_params_t){{30.0, 30.0, 0.125, 0.125, 0.125}};
  memcpy(module->params, &tmp, sizeof(dt_iop_bilateral_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_bilateral_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = (dt_iop_gui_data_t *)malloc(sizeof(dt_iop_bilateral_gui_data_t));
  dt_iop_bilateral_gui_data_t *g = (dt_iop_bilateral_gui_data_t *)self->gui_data;
  dt_iop_bilateral_params_t *p = (dt_iop_bilateral_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(_("sigma x")));
  g->label2 = GTK_LABEL(gtk_label_new(_("sigma y")));
  g->label3 = GTK_LABEL(gtk_label_new(_("sigma r")));
  g->label4 = GTK_LABEL(gtk_label_new(_("sigma g")));
  g->label5 = GTK_LABEL(gtk_label_new(_("sigma b")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label4), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label5), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label5), TRUE, TRUE, 0);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0, 100.0000, 1.0, p->sigma[0], 0));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0, 100.0000, 1.0, p->sigma[1], 0));
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0, 1.0000, 0.001, p->sigma[2], 3));
  g->scale4 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0, 1.0000, 0.001, p->sigma[3], 3));
  g->scale5 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0, 1.0000, 0.001, p->sigma[4], 3));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale5), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (sigma_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (sigma_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (sigma_callback), self);
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
                    G_CALLBACK (sigma_callback), self);
  g_signal_connect (G_OBJECT (g->scale5), "value-changed",
                    G_CALLBACK (sigma_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

}

