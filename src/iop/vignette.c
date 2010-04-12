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

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)

typedef struct dt_iop_vector_2d_t
{
  double x;
  double y;
} dt_iop_vector_2d_t;

typedef struct dt_iop_vignette_params_t
{
  double scale;              // 0 - 1 Radie
  double falloff_scale;   // 0 - 1 Radie for falloff inner radie of falloff=scale and outer=scale+falloff_scale
  double strength;         // 0 - 1 strength of effect
  double uniformity;       // 0 - 1 uniformity of center
  double bsratio;            // -1 - +1 ratio of brightness/saturation effect
  gboolean invert_falloff;
  gboolean invert_saturation;
  dt_iop_vector_2d_t center;            // Center of vignette
}
dt_iop_vignette_params_t;

typedef struct dt_iop_vignette_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkLabel  *label1,*label2,*label3,*label4,*label5,*label6,*label7;			                	// scale, strength, uniformity, b/s ratio, invert saturation, invert falloff, falloff scale
  GtkToggleButton *togglebutton1,*togglebutton2;				// invert saturation, invert falloff vignette
  GtkDarktableSlider *scale1,*scale2,*scale3,*scale4,*scale5;	  	// scale, strength, uniformity, b/s ratio, falloff sclae
}
dt_iop_vignette_gui_data_t;

typedef struct dt_iop_vignette_data_t
{
  double scale;
  double falloff_scale;  
  double strength;
  double uniformity;
  double bsratio;
  gboolean invert_falloff;
  gboolean  invert_saturation;
  dt_iop_vector_2d_t center;            // Center of vignette
}
dt_iop_vignette_data_t;

const char *name()
{
  return _("vignette");
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_vignette_data_t *data = (dt_iop_vignette_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  
  int iw=piece->buf_in.width*roi_out->scale;
  int ih=piece->buf_in.height*roi_out->scale;
  int ix= (roi_in->x)*roi_out->scale;
  int iy= (roi_in->y)*roi_out->scale;
    
  for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
  {
    dt_iop_vector_2d_t pv, vv;
    
    // Lets translate current pixel coord to local coord
    
    
    pv.x=-1.0+(((double) (ix+i)/iw)*2.0);
    pv.y=-1.0+(((double) (iy+j)/ih)*2.0);
  
    /*pv.x=-1.0+(((double) roi_in->x+i / piece->buf_out.width)*2.0);
    pv.y=-1.0+(((double) roi_in->y+j / piece->buf_out.height )*2.0);*/
     
    // Calculate the pixel weight in vinjett
    double v=tan(pv.y/pv.x);                    // get the pixel v of tan opp. / adj.
    if(pv.x==0)
      v=(pv.y>0)?0:M_PI;
    double dscale=data->scale/100.0;
    double fscale=data->falloff_scale/100.0;
    
    double sinv=sin(v)+data->center.x;
    double cosv=cos(v)+data->center.y;
    vv.x=cosv*dscale;                        // apply uniformity and scale vignette to radie 
    vv.y=sinv*dscale;
    double weight=0.0;
    double cvlen=sqrt((vv.x*vv.x)+(vv.y*vv.y));    // Length from center to vv
    double cplen=sqrt((pv.x*pv.x)+(pv.y*pv.y));  // Length from center to pv
    
    if( cplen>=cvlen ) // pixel is outside the inner vingett circle, lets calculate weight of vignette
    {
      double ox=cosv*(dscale+fscale);    // scale outer vignette circle
      double oy=sinv*(dscale+fscale);
      double blen=sqrt(((vv.x-ox)*(vv.x-ox))+((vv.y-oy)*(vv.y-oy)));             // lenght between vv and outer circle
      weight=((cplen-cvlen)/blen);
      if(weight <=1.0 && weight>0.0) 
        weight=1.0-( 1.0+sin( ((M_PI/2.0)+(M_PI*weight)) ) )/2.0;
        //weight=( 1.0+tan( (M_PI-(M_PI/4.0) +((M_PI/4.0)*weight)) ) )/2.0;
      else 
        weight=weight>1.0?1.0:0.0;
    }
    
    // Let's apply weighted effect on brightness and desaturation
    float col[3];
    for(int c=0;c<3;c++) col[c]=in[c];
    if( weight > 0 ) {
      double bs=1.0;
      double ss=1.0;
      
      if(data->bsratio>0.0) 
        bs-=data->bsratio;
      else
        ss-=fabs(data->bsratio);
      double strength=data->strength/100.0;
      
      // Then apply falloff vignette
      double falloff=(data->invert_falloff==FALSE)?(1.0-(weight*bs*strength)):(weight*bs*strength);
      col[0]=CLIP( ((data->invert_falloff==FALSE)? in[0]*falloff: in[0]+falloff) );
      col[1]=CLIP( ((data->invert_falloff==FALSE)? in[1]*falloff: in[1]+falloff) );
      col[2]=CLIP( ((data->invert_falloff==FALSE)? in[2]*falloff: in[2]+falloff) );
      
      // apply saturation
      double mv=(col[0]+col[1]+col[2])/3.0;
      double wss=CLIP(weight*ss)*strength;
      if(data->invert_saturation==FALSE)
      { // Desaturate
        col[0]=CLIP( col[0]+((mv-col[0])* wss) );
        col[1]=CLIP( col[1]+((mv-col[1])* wss) );
        col[2]=CLIP( col[2]+((mv-col[2])* wss) );    
      } 
      else
      {
        wss*=2.0;	// Double effect if we gonna saturate
        col[0]=CLIP( col[0]-((mv-col[0])* wss) );
        col[1]=CLIP( col[1]-((mv-col[1])* wss) );
        col[2]=CLIP( col[2]-((mv-col[2])* wss) );    
      }
      
    } 
    for(int c=0;c<3;c++) out[c]=col[c];
    out += 3; in += 3;
  }
}

static void
scale_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->scale= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
falloff_scale_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->falloff_scale= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
strength_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->strength= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
uniformity_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->uniformity = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
bsratio_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->bsratio = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void 
invert_saturation_callback (GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->invert_saturation = gtk_toggle_button_get_active(button);
  dt_dev_add_history_item(darktable.develop, self);
}

static void 
invert_falloff_callback (GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->invert_falloff = gtk_toggle_button_get_active(button);
  dt_dev_add_history_item(darktable.develop, self);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[vignette] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_vignette_data_t *d = (dt_iop_vignette_data_t *)piece->data;
  d->scale = p->scale;
  d->falloff_scale = p->falloff_scale;
  d->strength= p->strength;
  d->uniformity= p->uniformity;
  d->bsratio= p->bsratio;
  d->invert_saturation= p->invert_saturation;
  d->center=p->center;
  d->invert_falloff = p->invert_falloff;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_vignette_data_t));
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
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->scale);
  dtgtk_slider_set_value(g->scale2, p->strength);
  dtgtk_slider_set_value(g->scale3, p->uniformity);
  dtgtk_slider_set_value(g->scale4, p->bsratio);
  dtgtk_slider_set_value(g->scale5, p->falloff_scale);
  gtk_toggle_button_set_active(g->togglebutton1, p->invert_saturation);
  gtk_toggle_button_set_active(g->togglebutton2, p->invert_falloff);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_vignette_params_t));
  module->default_params = malloc(sizeof(dt_iop_vignette_params_t));
  module->default_enabled = 0;
  module->priority = 970;
  module->params_size = sizeof(dt_iop_vignette_params_t);
  module->gui_data = NULL;
  dt_iop_vignette_params_t tmp = (dt_iop_vignette_params_t){80,50,50,.0,0,FALSE,FALSE,{0,0}};
  memcpy(module->params, &tmp, sizeof(dt_iop_vignette_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_vignette_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_vignette_gui_data_t));
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(_("scale")));
  g->label7 = GTK_LABEL(gtk_label_new(_("falloff strength")));
  g->label2 = GTK_LABEL(gtk_label_new(_("strength")));
  g->label3 = GTK_LABEL(gtk_label_new(_("uniformity")));
  g->label4 = GTK_LABEL(gtk_label_new(_("b/s ratio")));
  g->label5 = GTK_LABEL(gtk_label_new(_("saturation")));
  g->label6 = GTK_LABEL(gtk_label_new(_("falloff")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label7), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label4), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label5), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label6), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label7), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label5), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label6), TRUE, TRUE, 0);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0000, 0.50, p->scale, 2));
  g->scale5 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.000, 0.010, p->falloff_scale, 2));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0000, 0.50, p->strength, 2));
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0000, 0.010, p->uniformity, 3));
  g->scale4 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_VALUE,-1.0000, 1.0000, 0.010, p->bsratio, 1));
  g->togglebutton1 = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(_("invert")));
  g->togglebutton2 = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(_("invert")));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale5), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->togglebutton1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->togglebutton2), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the radie scale of vignette for start of falloff"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale5), "tooltip-text", _("the radie scale of vignette for end of falloff"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("strength of effect"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale3), "tooltip-text", _("uniformity of vignette"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale4), "tooltip-text", _("brightness/saturation ratio\nof the result,\n-1 - Only brightness\n 0 - 50/50 mix of brightness and saturation\n+1 - Only saturation"), NULL);
  gtk_object_set(GTK_OBJECT(g->togglebutton1), "tooltip-text", _("inverts effect of saturation..."), NULL);
  gtk_object_set(GTK_OBJECT(g->togglebutton2), "tooltip-text", _("inverts effect of falloff, default is dark falloff..."), NULL);
 
 dtgtk_slider_set_format_type(DTGTK_SLIDER(g->scale1),DARKTABLE_SLIDER_FORMAT_PERCENT);
 dtgtk_slider_set_format_type(DTGTK_SLIDER(g->scale2),DARKTABLE_SLIDER_FORMAT_PERCENT);
 dtgtk_slider_set_format_type(DTGTK_SLIDER(g->scale5),DARKTABLE_SLIDER_FORMAT_PERCENT);
 
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->scale4),DARKTABLE_SLIDER_FORMAT_RATIO);
 
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (scale_callback), self);
  g_signal_connect (G_OBJECT (g->scale5), "value-changed",
                    G_CALLBACK (falloff_scale_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
        G_CALLBACK (strength_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
        G_CALLBACK (uniformity_callback), self);
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
        G_CALLBACK (bsratio_callback), self);
 g_signal_connect (G_OBJECT (g->togglebutton1), "toggled",
        G_CALLBACK (invert_saturation_callback), self);	
  g_signal_connect (G_OBJECT (g->togglebutton2), "toggled",
        G_CALLBACK (invert_falloff_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

