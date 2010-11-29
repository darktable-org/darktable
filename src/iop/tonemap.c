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

typedef struct dt_iop_tonemap_params_t
{
  float strength;
}
dt_iop_tonemap_params_t;

typedef struct dt_iop_tonemap_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkWidget  *label1;			// strength
  GtkDarktableSlider *scale1;       // strength
}
dt_iop_tonemap_gui_data_t;

typedef struct dt_iop_tonemap_data_t
{
  float strength;
}
dt_iop_tonemap_data_t;

const char *name()
{
  return _("tonemap");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES;
}

int 
groups () 
{
  return IOP_GROUP_CORRECT;
}


 #define rgb2yuv(rgb,yuv) { \
    const float Wr=0.299,Wb=0.114,Wg=1-Wr-Wb;\
    const float UMax=0.436, VMax=0.615; \
    yuv[0]=(Wr*rgb[0])+(Wg*rgb[1])+(Wb*rgb[2]); \
    yuv[1]=UMax*((rgb[2]-yuv[0])/(1-Wb)); \
    yuv[2]=VMax*((rgb[0]-yuv[0])/(1-Wr)); \
  }
  
 #define yuv2rgb(yuv,rgb) { \
    const float Wr=0.299,Wb=0.114,Wg=1-Wr-Wb;\
    const float UMax=0.436, VMax=0.615; \
    rgb[0]=yuv[0]+yuv[2]*((1-Wr)/VMax); \
    rgb[1]=yuv[0]-yuv[1]*(Wb*(1-Wb)/(UMax*Wg))-yuv[2]*((Wr*(1-Wr))/VMax*Wg); \
    rgb[2]=yuv[0]+yuv[1]*(1-Wb/UMax); \
  }


void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_tonemap_data_t *data = (dt_iop_tonemap_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  
  const float scale=(data->strength/100.0);
  in  = (float *)ivoid;
  out = (float *)ovoid;
  
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out, data) schedule(static)
#endif
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
    // Apply a simple tonemap 
    float yuv[3];
    rgb2yuv((in+(ch*k)),yuv);
    yuv[0] = (yuv[0]/(yuv[0]+scale));
    yuv2rgb(yuv,(out+(ch*k)));
  }

}

static void
strength_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_tonemap_params_t *p = (dt_iop_tonemap_params_t *)self->params;
  p->strength = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_tonemap_params_t *p = (dt_iop_tonemap_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[tonemap] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_tonemap_data_t *d = (dt_iop_tonemap_data_t *)piece->data;
  d->strength = p->strength;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_tonemap_data_t));
  memset(piece->data,0,sizeof(dt_iop_tonemap_data_t));
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
  dt_iop_tonemap_gui_data_t *g = (dt_iop_tonemap_gui_data_t *)self->gui_data;
  dt_iop_tonemap_params_t *p = (dt_iop_tonemap_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->strength);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_tonemap_params_t));
  module->default_params = malloc(sizeof(dt_iop_tonemap_params_t));
  module->default_enabled = 0;
  module->priority = 151;
  module->params_size = sizeof(dt_iop_tonemap_params_t);
  module->gui_data = NULL;
  dt_iop_tonemap_params_t tmp = (dt_iop_tonemap_params_t){50};
  memcpy(module->params, &tmp, sizeof(dt_iop_tonemap_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_tonemap_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_tonemap_gui_data_t));
  dt_iop_tonemap_gui_data_t *g = (dt_iop_tonemap_gui_data_t *)self->gui_data;
  dt_iop_tonemap_params_t *p = (dt_iop_tonemap_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  
  g->label1 = dtgtk_reset_label_new(_("strength"), self, &p->strength, sizeof(float));
  
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->strength, 2));
  dtgtk_slider_set_format_type(g->scale1,DARKTABLE_SLIDER_FORMAT_PERCENT);
  
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the amount of tonemap to apply"), (char *)NULL);
  
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (strength_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

