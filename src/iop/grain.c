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

#define GRAIN_LIGHTNESS_STRENGTH_SCALE 0.0625
#define GRAIN_HUE_STRENGTH_SCALE 0.25
#define GRAIN_SATURATION_STRENGTH_SCALE 0.25
#define GRAIN_RGB_STRENGTH_SCALE 0.0625

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE(1)

typedef enum _dt_iop_grain_channel_t
{
  DT_GRAIN_CHANNEL_HUE=0,
  DT_GRAIN_CHANNEL_SATURATION,
  DT_GRAIN_CHANNEL_LIGHTNESS,
  DT_GRAIN_CHANNEL_RGB
}
_dt_iop_grain_channel_t;

typedef struct dt_iop_grain_params_t
{
  _dt_iop_grain_channel_t channel;
  float smooth;
  float strength;
}
dt_iop_grain_params_t;

typedef struct dt_iop_grain_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkLabel  *label1/*,*label2*/,*label3;	           // channel, smooth, strength
  GtkComboBox *combo1;			                      // channel
  GtkDarktableSlider /**scale1,*/*scale2;        // smooth, strength
}
dt_iop_grain_gui_data_t;

typedef struct dt_iop_grain_data_t
{
  _dt_iop_grain_channel_t channel;
  float smooth;
  float strength;
}
dt_iop_grain_data_t;

void rgb2hsl(float r,float g,float b,float *h,float *s,float *l) 
{
  float pmax=fmax(r,fmax(g,b));
  float pmin=fmin(r,fmin(g,b));
  float delta=(pmax-pmin);
  
  *h=*s=*l=0;
  *l=(pmin+pmax)/2.0;
 
  if(pmax!=pmin) 
  {
    *s=*l<0.5?delta/(pmax+pmin):delta/(2.0-pmax-pmin);
  
    if(pmax==r) *h=(g-b)/delta;
    if(pmax==g) *h=2.0+(b-r)/delta;
    if(pmax==b) *h=4.0+(r-g)/delta;
    *h/=6.0;
    if(*h<0.0) *h+=1.0;
    else if(*h>1.0) *h-=1.0;
  }
}
void hue2rgb(float m1,float m2,float hue,float *channel)
{
  if(hue<0.0) hue+=1.0;
  else if(hue>1.0) hue-=1.0;
  
  if( (6.0*hue) < 1.0) *channel=(m1+(m2-m1)*hue*6.0);
  else if((2.0*hue) < 1.0) *channel=m2;
  else if((3.0*hue) < 2.0) *channel=(m1+(m2-m1)*((2.0/3.0)-hue)*6.0);
  else *channel=m1;
}
void hsl2rgb(float *r,float *g,float *b,float h,float s,float l)
{
  float m1,m2;
  *r=*g=*b=l;
  if( s==0) return;
  m2=l<0.5?l*(1.0+s):l+s-l*s;
  m1=(2.0*l-m2);
  hue2rgb(m1,m2,h +(1.0/3.0), r);
  hue2rgb(m1,m2,h, g);
  hue2rgb(m1,m2,h - (1.0/3.0), b);

}


const char *name()
{
  return _("grain");
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_grain_data_t *data = (dt_iop_grain_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  
  // Allocate and generate noise map, TODO: reuse as static object to speed up 
  int noisesize=((roi_out->width*roi_out->height)*sizeof(float));
  float *noise = malloc(noisesize);
  for(int i=0;i<(noisesize/sizeof(float));i++) {
    noise[i]=(float)(rand()/(double)RAND_MAX);
  }
  
  // Apply grain to image
  in  = (float *)ivoid;
  out = (float *)ovoid;
  float *nmap = noise;
  float h,s,l;
  double strength=(data->strength/100.0);
  for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
  {
    if(data->channel==DT_GRAIN_CHANNEL_LIGHTNESS || data->channel==DT_GRAIN_CHANNEL_SATURATION) 
    {
      rgb2hsl(in[0],in[1],in[2],&h,&s,&l);
      h+=(data->channel==DT_GRAIN_CHANNEL_HUE)? ((-0.5+nmap[0])*2.0)*(strength*GRAIN_HUE_STRENGTH_SCALE) : 0;
      s+=(data->channel==DT_GRAIN_CHANNEL_SATURATION)? ((-0.5+nmap[0])*2.0)*(strength*GRAIN_SATURATION_STRENGTH_SCALE) : 0;
      l+=(data->channel==DT_GRAIN_CHANNEL_LIGHTNESS)? ((-0.5+nmap[0])*2.0)*(strength*GRAIN_LIGHTNESS_STRENGTH_SCALE) : 0;
      s=CLIP(s); 
      l=CLIP(l);
      if(h<0.0) h+=1.0;
      if(h>1.0) h-=1.0;
      hsl2rgb(&out[0],&out[1],&out[2],h,s,l);
    } 
    else if( data->channel==DT_GRAIN_CHANNEL_RGB )
    {
      out[0]=in[0]+(((-0.5+nmap[0])*2.0)*(strength*GRAIN_RGB_STRENGTH_SCALE));
      out[1]=in[1]+(((-0.5+nmap[0])*2.0)*(strength*GRAIN_RGB_STRENGTH_SCALE));
      out[2]=in[2]+(((-0.5+nmap[0])*2.0)*(strength*GRAIN_RGB_STRENGTH_SCALE));
    } 
    else
    { // No noisemethod lets jsut copy source to dest
      out[0]=in[0];
      out[1]=in[1];
      out[2]=in[2];
    }  
    //out[0]=out [1]=out[2]=nmap[0];
    nmap++;
    out += 3; in += 3;
  }
  
  free(noise);
  
}

static void
channel_changed (GtkComboBox *combo, dt_iop_module_t *self)
{
  dt_iop_grain_gui_data_t *g = (dt_iop_grain_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)self->params;
  p->channel = gtk_combo_box_get_active(g->combo1);
  dt_dev_add_history_item(darktable.develop, self);
}

/*static void
smooth_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)self->params;
  p->smooth = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}*/

static void
strength_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)self->params;
  p->strength= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[grain] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_grain_data_t *d = (dt_iop_grain_data_t *)piece->data;
  d->channel = p->channel;
  d->smooth = p->smooth;
  d->strength = p->strength;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_grain_data_t));
  memset(piece->data,0,sizeof(dt_iop_grain_data_t));
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
  dt_iop_grain_gui_data_t *g = (dt_iop_grain_gui_data_t *)self->gui_data;
  dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)module->params;
  gtk_combo_box_set_active(g->combo1, p->channel);
  //dtgtk_slider_set_value(g->scale1, p->smooth);
  dtgtk_slider_set_value(g->scale2, p->strength);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_grain_params_t));
  module->default_params = malloc(sizeof(dt_iop_grain_params_t));
  module->default_enabled = 0;
  module->priority = 965;
  module->params_size = sizeof(dt_iop_grain_params_t);
  module->gui_data = NULL;
  dt_iop_grain_params_t tmp = (dt_iop_grain_params_t){DT_GRAIN_CHANNEL_LIGHTNESS,0.0,50.0};
  memcpy(module->params, &tmp, sizeof(dt_iop_grain_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_grain_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_grain_gui_data_t));
  dt_iop_grain_gui_data_t *g = (dt_iop_grain_gui_data_t *)self->gui_data;
  dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(_("channel")));
  //g->label2 = GTK_LABEL(gtk_label_new(_("smooth")));
  g->label3 = GTK_LABEL(gtk_label_new(_("strength")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  //gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
 // gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  
  g->combo1=GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->combo1,_("hue"));
  gtk_combo_box_append_text(g->combo1,_("saturation"));
  gtk_combo_box_append_text(g->combo1,_("lightness"));
  //gtk_combo_box_append_text(g->combo1,_("rgb"));
  gtk_combo_box_set_active(g->combo1,p->channel);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->combo1), TRUE, TRUE, 0);
  
  //g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->smooth, 2));
  //dtgtk_slider_set_format_type(g->scale1,DARKTABLE_SLIDER_FORMAT_PERCENT);
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->strength, 2));
  dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);
  //gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  //gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the smooth amount of the noise"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("the the strength of applied grain"), NULL);
  
 g_signal_connect (G_OBJECT (g->combo1), "changed",
            G_CALLBACK (channel_changed), self);
 /*g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (smooth_callback), self);*/
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
        G_CALLBACK (strength_callback), self);
 
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

