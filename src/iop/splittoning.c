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
#include "dtgtk/label.h"
#include "dtgtk/slider.h"
#include "dtgtk/gradientslider.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE(1)

typedef struct dt_iop_splittoning_params_t
{
  float shadow_hue;
  float shadow_saturation;
  float highlight_hue;
  float highlight_saturation;
  float balance;						// center luminance of gradient
  float compress;						// Compress range
}
dt_iop_splittoning_params_t;

typedef struct dt_iop_splittoning_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkLabel  *label1,*label2,*label3,*label4,*label5,*label6;	 			 // highlight hue, h sat, shadow hue, s sat, balance, compress
  GtkDarktableSlider *scale1,*scale2;       							//  balance, compress
  GtkDarktableButton *colorpick1,*colorpick2;	   					// shadow, highlight
  GtkDarktableGradientSlider *gslider1,*gslider2,*gslider3,*gslider4;		//highligh hue, highlight saturation, shadow hue, shadow saturation
}
dt_iop_splittoning_gui_data_t;

typedef struct dt_iop_splittoning_data_t
{
  float shadow_hue;
  float shadow_saturation;
  float highlight_hue;
  float highlight_saturation;
  float balance;						// center luminance of gradient}
  float compress;						// Compress range
} 
dt_iop_splittoning_data_t;

const char *name()
{
  return _("split toning");
}


int 
groups () 
{
	return IOP_GROUP_EFFECT;
}




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

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_splittoning_data_t *data = (dt_iop_splittoning_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  
  // Apply velvia saturation
  in  = (float *)ivoid;
  out = (float *)ovoid;

  // Get lowest/highest l in image
  float lhigh=0.0;
  float llow=1.0;
  
  // TODO: openmp reduction
  // FIXME: this will depend on current view!
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
    float h,s,l;
    rgb2hsl(in[3*k+0],in[3*k+1],in[3*k+2],&h,&s,&l);
    lhigh=fmax(lhigh,l);
    llow=fmin(llow,l);
  }
  
  in  = (float *)ivoid;
  out = (float *)ovoid;
  const float compress=(data->compress/110.0)/2.0;  // Dont allow 100% compression..
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(in,out,roi_out,data) schedule(static)
#endif
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
    double ra,la;
    float mixrgb[3];
    float h,s,l;
    rgb2hsl(in[3*k+0],in[3*k+1],in[3*k+2],&h,&s,&l);
    if(l < data->balance-compress || l > data->balance+compress)
    {      
      h=l<data->balance?data->shadow_hue:data->highlight_hue;
      s=l<data->balance?data->shadow_saturation:data->highlight_saturation;
      ra=l<data->balance?CLIP((fabs(-data->balance+compress+l)*2.0)):CLIP((fabs(-data->balance-compress+l)*2.0));
      la=(1.0-ra);
     
      hsl2rgb(&mixrgb[0],&mixrgb[1],&mixrgb[2],h,s,l);
      
      out[3*k+0]=in[3*k+0]*la + mixrgb[0]*ra;
      out[3*k+1]=in[3*k+1]*la + mixrgb[1]*ra;
      out[3*k+2]=in[3*k+2]*la + mixrgb[2]*ra;
    } 
    else 
    {
      out[3*k+0]=in[3*k+0];
      out[3*k+1]=in[3*k+1];
      out[3*k+2]=in[3*k+2];
    }
  }
}

static void
balance_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  p->balance= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
compress_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  p->compress= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
hue_callback(GtkDarktableGradientSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;

  double hue=0;
  double saturation=0;
  float color[3];
  GtkWidget *preview;
  GtkDarktableGradientSlider *sslider=NULL;
  if( slider == g->gslider1 )
  { // Shadows
    p->shadow_hue=hue=dtgtk_gradient_slider_get_value(slider);
    saturation=p->shadow_saturation;
    preview=GTK_WIDGET(g->colorpick1);
    sslider=g->gslider2;
  } 
  else
  {
    p->highlight_hue=hue=dtgtk_gradient_slider_get_value(slider);
    saturation=p->highlight_saturation;
    preview=GTK_WIDGET(g->colorpick2);
    sslider=g->gslider4;
  }
    
  hsl2rgb(&color[0],&color[1],&color[2],hue,saturation,0.5);
  
  GdkColor c;
  c.red=color[0]*65535.0;
  c.green=color[1]*65535.0;
  c.blue=color[2]*65535.0;
  
  dtgtk_gradient_slider_set_stop(sslider,1.0,c);  // Update saturation end color
  gtk_widget_modify_fg(preview,GTK_STATE_NORMAL,&c); // update color preview

  if(self->dt->gui->reset) return;
  gtk_widget_draw(GTK_WIDGET(sslider),NULL);
  
  if(dtgtk_gradient_slider_is_dragging(slider)==FALSE) dt_dev_add_history_item(darktable.develop, self);
}

static void
saturation_callback(GtkDarktableGradientSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;

  double hue=0;
  double saturation=0;
  float color[3];
  GtkWidget *preview;
  GtkDarktableGradientSlider *sslider=NULL;
  if( slider == g->gslider2 )
  { // Shadows
    hue=dtgtk_gradient_slider_get_value(g->gslider1);
    p->shadow_saturation=saturation=dtgtk_gradient_slider_get_value(slider);
    preview=GTK_WIDGET(g->colorpick1);
    sslider=g->gslider1;
  } 
  else
  {
    hue=dtgtk_gradient_slider_get_value(g->gslider3);
    p->highlight_saturation=saturation=dtgtk_gradient_slider_get_value(slider);
    preview=GTK_WIDGET(g->colorpick2);
    sslider=g->gslider3;
  }
    
  hsl2rgb(&color[0],&color[1],&color[2],hue,saturation,0.5);
  
  GdkColor c;
  c.red=color[0]*65535.0;
  c.green=color[1]*65535.0;
  c.blue=color[2]*65535.0;
  
  gtk_widget_modify_fg(preview,GTK_STATE_NORMAL,&c); // Update color preview
  
  if(self->dt->gui->reset) return;
  if(dtgtk_gradient_slider_is_dragging(slider)==FALSE) dt_dev_add_history_item(darktable.develop, self);
}


static void
colorpick_button_callback(GtkButton *button,gpointer user_data)  
{
  GtkColorSelectionDialog  *csd=(GtkColorSelectionDialog  *)user_data;
  gtk_dialog_response(GTK_DIALOG(csd),(GTK_WIDGET(button)==csd->ok_button)?GTK_RESPONSE_ACCEPT:0);
}

static void
colorpick_callback (GtkDarktableButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  
  GtkColorSelectionDialog  *csd = GTK_COLOR_SELECTION_DIALOG(gtk_color_selection_dialog_new(_("Select tone color")));
  g_signal_connect (G_OBJECT (csd->ok_button), "clicked",
      G_CALLBACK (colorpick_button_callback), csd);
  g_signal_connect (G_OBJECT (csd->cancel_button), "clicked",
      G_CALLBACK (colorpick_button_callback), csd);
  
  GtkColorSelection *cs = GTK_COLOR_SELECTION(gtk_color_selection_dialog_get_color_selection(csd));
  GdkColor c;
  float color[3],h,s,l;
  h=(button==g->colorpick1)?p->shadow_hue:p->highlight_hue;
  s=(button==g->colorpick1)?p->shadow_saturation:p->highlight_saturation;
  l=0.5;	
  hsl2rgb(&color[0],&color[1],&color[2],h,s,l);
	
  c.red= 65535 * color[0];
  c.green= 65535 * color[1];
  c.blue= 65535 * color[2];
  gtk_color_selection_set_current_color(cs,&c);
  if(gtk_dialog_run(GTK_DIALOG(csd))==GTK_RESPONSE_ACCEPT) 
  {
    gtk_color_selection_get_current_color(cs,&c);
    color[0]=c.red/65535.0;
    color[1]=c.green/65535.0;
    color[2]=c.blue/65535.0;
    rgb2hsl(color[0],color[1],color[2],&h,&s,&l);
    l=0.5;
    hsl2rgb(&color[0],&color[1],&color[2],h,s,l);

    dtgtk_gradient_slider_set_value( (button==g->colorpick1)? g->gslider1: g->gslider3 ,h );
    dtgtk_gradient_slider_set_value( (button==g->colorpick1)? g->gslider2: g->gslider4 ,s );
  }
  gtk_widget_destroy(GTK_WIDGET(csd));
  dt_dev_add_history_item(darktable.develop, self);
}



void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[splittoning] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_splittoning_data_t *d = (dt_iop_splittoning_data_t *)piece->data;
  d->shadow_hue= p->shadow_hue;
  d->highlight_hue= p->highlight_hue;
  d->shadow_saturation = p->shadow_saturation;
  d->highlight_saturation = p->highlight_saturation;
  d->balance = p->balance;
  d->compress=p->compress;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_splittoning_data_t));
  memset(piece->data,0,sizeof(dt_iop_splittoning_data_t));
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
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)module->params;
  
  dtgtk_gradient_slider_set_value(g->gslider1,p->shadow_hue);
  dtgtk_gradient_slider_set_value(g->gslider3,p->highlight_hue);
  dtgtk_gradient_slider_set_value(g->gslider4,p->highlight_saturation);
  dtgtk_gradient_slider_set_value(g->gslider2,p->shadow_saturation);
  dtgtk_slider_set_value(g->scale1, p->balance);
  dtgtk_slider_set_value(g->scale2, p->compress);
	
  float color[3];
  hsl2rgb(&color[0],&color[1],&color[2],p->shadow_hue,p->shadow_saturation,0.5);
  
  GdkColor c;
  c.red=color[0]*65535.0;
  c.green=color[1]*65535.0;
  c.blue=color[2]*65535.0;
  
  gtk_widget_modify_fg(GTK_WIDGET(g->colorpick1),GTK_STATE_NORMAL,&c);
 
  hsl2rgb(&color[0],&color[1],&color[2],p->highlight_hue,p->highlight_saturation,0.5);
  c.red=color[0]*65535.0;
  c.green=color[1]*65535.0;
  c.blue=color[2]*65535.0;
  	
  gtk_widget_modify_fg(GTK_WIDGET(g->colorpick2),GTK_STATE_NORMAL,&c);
	
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_splittoning_params_t));
  module->default_params = malloc(sizeof(dt_iop_splittoning_params_t));
  module->default_enabled = 0;
  module->priority = 998;
  module->params_size = sizeof(dt_iop_splittoning_params_t);
  module->gui_data = NULL;
  dt_iop_splittoning_params_t tmp = (dt_iop_splittoning_params_t){ 0,0.5,0.2,0.5, 0.5,33.0};
  memcpy(module->params, &tmp, sizeof(dt_iop_splittoning_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_splittoning_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_splittoning_gui_data_t));
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));
  // Shadows
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(dtgtk_label_new(_("shadows"),DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT)), FALSE, FALSE, 5);  
  
  g->colorpick1 = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE));
  gtk_widget_set_size_request(GTK_WIDGET(g->colorpick1),32,32);
      
  GtkWidget *hbox=GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  gtk_box_pack_end(GTK_BOX(hbox),GTK_WIDGET(g->colorpick1),FALSE,FALSE,0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 5);

  
  g->label1 = GTK_LABEL(gtk_label_new(_("hue")));
  g->label2 = GTK_LABEL(gtk_label_new(_("saturation")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  int lightness=32768;
  g->gslider1=DTGTK_GRADIENT_SLIDER(dtgtk_gradient_slider_new_with_color((GdkColor){0,lightness,0,0},(GdkColor){0,lightness,0,0}));
  dtgtk_gradient_slider_set_stop(g->gslider1,0.166,(GdkColor){0,lightness,lightness,0});
  dtgtk_gradient_slider_set_stop(g->gslider1,0.332,(GdkColor){0,0,lightness,0});
  dtgtk_gradient_slider_set_stop(g->gslider1,0.498,(GdkColor){0,0,lightness,lightness});
  dtgtk_gradient_slider_set_stop(g->gslider1,0.664,(GdkColor){0,0,0,lightness});
  dtgtk_gradient_slider_set_stop(g->gslider1,0.83,(GdkColor){0,lightness,0,lightness});
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->gslider1), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->gslider1), "tooltip-text", _("select the hue tone for shadows"), (char *)NULL);
  
  g->gslider2=DTGTK_GRADIENT_SLIDER(dtgtk_gradient_slider_new_with_color((GdkColor){0,lightness,lightness,lightness},(GdkColor){0,lightness,lightness,lightness}));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->gslider2), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->gslider2), "tooltip-text", _("select the saturation shadow tone"), (char *)NULL);
  
  // Highlights
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(dtgtk_label_new(_("highlights"),DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT)), FALSE, FALSE, 5);
  
  g->colorpick2 = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE));
  gtk_widget_set_size_request(GTK_WIDGET(g->colorpick2),32,32);
  
  hbox=GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  gtk_box_pack_end(GTK_BOX(hbox),GTK_WIDGET(g->colorpick2),FALSE,FALSE,0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 5);

  g->label3 = GTK_LABEL(gtk_label_new(_("hue")));
  g->label4 = GTK_LABEL(gtk_label_new(_("saturation")));
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label4), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label4), TRUE, TRUE, 0);

  
  g->gslider3=DTGTK_GRADIENT_SLIDER(dtgtk_gradient_slider_new_with_color((GdkColor){0,lightness,0,0},(GdkColor){0,lightness,0,0}));
  dtgtk_gradient_slider_set_stop(g->gslider3,0.166,(GdkColor){0,lightness,lightness,0});
  dtgtk_gradient_slider_set_stop(g->gslider3,0.332,(GdkColor){0,0,lightness,0});
  dtgtk_gradient_slider_set_stop(g->gslider3,0.498,(GdkColor){0,0,lightness,lightness});
  dtgtk_gradient_slider_set_stop(g->gslider3,0.664,(GdkColor){0,0,0,lightness});
  dtgtk_gradient_slider_set_stop(g->gslider3,0.83,(GdkColor){0,lightness,0,lightness});
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->gslider3), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->gslider3), "tooltip-text", _("select the hue tone for highlights"), (char *)NULL);
  
  g->gslider4=DTGTK_GRADIENT_SLIDER(dtgtk_gradient_slider_new_with_color((GdkColor){0,lightness,lightness,lightness},(GdkColor){0,lightness,lightness,lightness}));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->gslider4), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->gslider4), "tooltip-text", _("select the saturation highlights tone"), (char *)NULL);
    
  // Additional paramters
  hbox=GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 5);

  g->label5 = GTK_LABEL(gtk_label_new(_("balance")));
  g->label6 = GTK_LABEL(gtk_label_new(_("compress")));
  gtk_misc_set_alignment(GTK_MISC(g->label5), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label6), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label5), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label6), TRUE, TRUE, 0);
  
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.010, p->balance, 0));
  dtgtk_slider_set_format_type(g->scale1,DARKTABLE_SLIDER_FORMAT_RATIO);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->compress, 2));
  dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);

  
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the balance of center of splittoning"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("compress the effect on highlighs/shadows and\npreserve midtones"), (char *)NULL);

  g_signal_connect (G_OBJECT (g->gslider1), "value-changed",
        G_CALLBACK (hue_callback), self);
  g_signal_connect (G_OBJECT (g->gslider3), "value-changed",
        G_CALLBACK (hue_callback), self);
 
 g_signal_connect (G_OBJECT (g->gslider2), "value-changed",
        G_CALLBACK (saturation_callback), self);
  g_signal_connect (G_OBJECT (g->gslider4), "value-changed",
        G_CALLBACK (saturation_callback), self);
  
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
        G_CALLBACK (balance_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
        G_CALLBACK (compress_callback), self);
       
        
  g_signal_connect (G_OBJECT (g->colorpick1), "clicked",
        G_CALLBACK (colorpick_callback), self);
  g_signal_connect (G_OBJECT (g->colorpick2), "clicked",
        G_CALLBACK (colorpick_callback), self);

}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

