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
#include "LibRaw/libraw/libraw.h"

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE(1)

typedef struct dt_iop_graduatednd_params_t
{
  float density;
  float compression;
  float rotation;
  float offset;
  int type;
}
dt_iop_graduatednd_params_t;

typedef struct dt_iop_graduatednd_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;                                            // left and right controlboxes
  GtkLabel  *label1,*label2,*label3,*label4;            			      // density, compression, rotation, offset
  GtkToggleButton *tbutton1;                                             // half/full filter
  GtkDarktableSlider *scale1,*scale2,*scale3,*scale4;        // density, compression, rotation, offset
}
dt_iop_graduatednd_gui_data_t;

typedef struct dt_iop_graduatednd_data_t
{
  float density;			          	// The density of filter 0-8 EV
  float compression;			        // Default 0% = soft and 100% = hard 
  float rotation;		          	// 2*PI -180 - +180
  float offset;				            // Default 50%, centered, can be offsetted...
  int type;				                // 0 = full, 1 = half (clear below line)
  float coeffs[3];
}
dt_iop_graduatednd_data_t;

const char *name()
{
  return _("graduated nd");
}


int 
groups () 
{
  return IOP_GROUP_EFFECT;
}


void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_graduatednd_data_t *data = (dt_iop_graduatednd_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  
//  const int ix= (roi_in->x);
  const int iy= (roi_in->y);
  const float iw=piece->buf_in.width*roi_out->scale;
  const float ih=piece->buf_in.height*roi_out->scale;
  const float hw=(iw/2.0);
  const float hh=(ih/2.0);
  const float radie = sqrt((hw*hw)+(hh*hh)); 
      
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out,data) schedule(static)
#endif
  for(int y=0;y<roi_out->height;y++)
    for(int x=0;x<roi_out->width;x++)
    {
      int k=(roi_out->width*y+x)*3;
      float length = radie*(1.0-(data->compression/100.0));
      float amount = (data->offset/100.0)+((hh-(y+iy))/length);
      amount=CLIP(amount);
      
      float density = ( 1.0 / exp2f (data->density*amount) );
      
      
      out[k+0]=in[k+0]*density;
      out[k+1]=in[k+1]*density;
      out[k+2]=in[k+2]*density;
    }
}

static void
density_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  p->density = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
compression_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  p->compression = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
rotation_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  p->rotation= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}


static void
offset_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  p->offset= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}


static void
type_callback (GtkToggleButton *tb,gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  p->type =  gtk_toggle_button_get_active(tb);
  dt_dev_add_history_item(darktable.develop, self);
}



void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[velvia] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_graduatednd_data_t *d = (dt_iop_graduatednd_data_t *)piece->data;
  d->density = p->density;
  d->compression = p->compression;
  d->rotation = p->rotation;
  d->offset = p->offset;
  d->type = p->type;
  for(int k=0;k<3;k++) d->coeffs[k] = ((float *)(self->data))[k];
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_graduatednd_data_t));
  memset(piece->data,0,sizeof(dt_iop_graduatednd_data_t));
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
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)module->params;
  dtgtk_slider_set_value (g->scale1, p->density);
  dtgtk_slider_set_value (g->scale2, p->compression);
  //dtgtk_slider_set_value (g->scale3, p->rotation);
  dtgtk_slider_set_value (g->scale4, p->offset);
  //gtk_toggle_button_set_active (g->tbutton1, p->type);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_graduatednd_params_t));
  module->default_params = malloc(sizeof(dt_iop_graduatednd_params_t));
  module->default_enabled = 0;
  module->priority = 151;
  module->params_size = sizeof(dt_iop_graduatednd_params_t);
  module->gui_data = NULL;
  dt_iop_graduatednd_params_t tmp = (dt_iop_graduatednd_params_t){4.0,50,0,0,0};
  memcpy(module->params, &tmp, sizeof(dt_iop_graduatednd_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_graduatednd_params_t));
  
  // get white balance coefficients, as shot
  float *coeffs = (float *)malloc(4*sizeof(float));
  module->data = coeffs;
  coeffs[0] = coeffs[1] = coeffs[2] = 1.0;
  char filename[1024];
  int ret;
  dt_image_full_path(module->dev->image, filename, 1024);
  libraw_data_t *raw = libraw_init(0);
  ret = libraw_open_file(raw, filename);
  if(!ret)
  {
    for(int k=0;k<3;k++) coeffs[k] = raw->color.cam_mul[k];
    if(coeffs[0] < 0.0) for(int k=0;k<3;k++) coeffs[k] = raw->color.pre_mul[k];
    if(coeffs[0] == 0 || coeffs[1] == 0 || coeffs[2] == 0)
    { // could not get useful info!
      coeffs[0] = coeffs[1] = coeffs[2] = 1.0f;
    }
    else
    {
      coeffs[0] /= coeffs[1];
      coeffs[2] /= coeffs[1];
      coeffs[1] = 1.0f;
    }
  }
  libraw_close(raw);
  float dmin=coeffs[0], dmax=coeffs[0];
  for (int c=1; c < 3; c++)
  {
    if (dmin > coeffs[c])
      dmin = coeffs[c];
    if (dmax < coeffs[c])
      dmax = coeffs[c];
  }
  for(int k=0;k<3;k++) coeffs[k] = dmax/(dmin*coeffs[k]);
  coeffs[3] = dmin/dmax;
  
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
  self->gui_data = malloc(sizeof(dt_iop_graduatednd_gui_data_t));
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;

  self->widget = gtk_table_new (5,2,FALSE);
  
  /* adding the labels */
  g->label1 = GTK_LABEL(gtk_label_new(_("density")));
  g->label2 = GTK_LABEL(gtk_label_new(_("compression")));
  g->label3 = GTK_LABEL(gtk_label_new(_("rotation")));
  g->label4 = GTK_LABEL(gtk_label_new(_("offset")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label4), 0.0, 0.5);
  
  gtk_table_attach (GTK_TABLE (self->widget), GTK_WIDGET (g->label1), 0,1,0,1,0,GTK_FILL,0,0);
  gtk_table_attach (GTK_TABLE (self->widget), GTK_WIDGET (g->label2), 0,1,1,2,0,GTK_FILL,0,0);
 // gtk_table_attach (GTK_TABLE (self->widget), GTK_WIDGET (g->label3), 0,1,2,3,0,GTK_FILL,0,0);
  gtk_table_attach (GTK_TABLE (self->widget), GTK_WIDGET (g->label4), 0,1,3,4,0,GTK_FILL,0,0);
  
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 8.0, 0.01, p->density, 2));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->compression, 0));
  dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-180, 180,0.5, p->rotation, 2));
  g->scale4 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 1.0, p->offset, 0));
  dtgtk_slider_set_format_type(g->scale4,DARKTABLE_SLIDER_FORMAT_PERCENT);
  
  g->tbutton1 = GTK_TOGGLE_BUTTON (gtk_toggle_button_new_with_label (_("half")));
  
  gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (g->scale1), 1,2,0,1);
  gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (g->scale2), 1,2,1,2);
 // gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (g->scale3), 1,2,2,3);
  gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (g->scale4), 1,2,3,4);
 // gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (g->tbutton1), 1,2,4,5);
  
  
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the density in EV for the filter"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("compression of graduation:\n0%% = soft, 100%% = hard"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale3), "tooltip-text", _("rotation of filter -180 to 180 degrees"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale4), "tooltip-text", _("offset of filter in angle of rotation"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->tbutton1), "tooltip-text", _("if selected density below split is clear"), (char *)NULL);
  
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
        G_CALLBACK (density_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
        G_CALLBACK (compression_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
        G_CALLBACK (rotation_callback), self);  
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
        G_CALLBACK (offset_callback), self);  
  g_signal_connect (G_OBJECT (g->tbutton1), "toggled",
        G_CALLBACK (type_callback), self);  
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

