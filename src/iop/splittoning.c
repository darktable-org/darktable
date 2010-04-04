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
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE(1)

typedef struct dt_iop_splittoning_params_t
{
  float shadow_color[3];					// rgb gradient start color
  float shadow_saturation;
  float highlight_color[3];				// rgb gradient stop color
  float highlight_saturation;
  float balance;						// center luminance of gradient
}
dt_iop_splittoning_params_t;

typedef struct dt_iop_splittoning_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkLabel  *label1,*label2,*label3;	 			    // highlight color,shadow color, balance
  GtkDarktableSlider *scale1,*scale2,*scale3;       // highlight saturation, shadow saturation, balance
  GtkDarktableButton *colorpick1,*colorpick2;	   // highlight color, shadow color
}
dt_iop_splittoning_gui_data_t;

typedef struct dt_iop_splittoning_data_t
{
  float shadow_color[3];					// rgb gradient start color
  float shadow_saturation;
  float highlight_color[3];				// rgb gradient stop color
  float highlight_saturation;
  float balance;						// center luminance of gradient}
} 
dt_iop_splittoning_data_t;

const char *name()
{
  return _("splittoning");
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

  float sh,ss,sl;
  float hh,hs,hl;
  float h,s,l;
  
  rgb2hsl(data->shadow_color[0],data->shadow_color[1],data->shadow_color[2],&sh,&ss,&sl);
  rgb2hsl(data->highlight_color[0],data->highlight_color[1],data->highlight_color[2],&hh,&hs,&hl);
  //float ssa=data->shadow_saturation/100.0;
  //float hsa=data->highlight_saturation/100.0;
  
  // Get lowest/highest l in image
  float lhigh=0.0;
  float llow=1.0;
  
  for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
  {
     rgb2hsl(in[0],in[1],in[2],&h,&s,&l);
    lhigh=fmax(lhigh,l);
    llow=fmin(llow,l);
    out += 3; in += 3;
  }
  
  //float lscale=1.0+(1.0-(lhigh-llow));
  
  in  = (float *)ivoid;
  out = (float *)ovoid;
  float mixrgb[3];
  for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
  {
    rgb2hsl(in[0],in[1],in[2],&h,&s,&l);
    //l*=lscale;
    
    h=l<data->balance?sh:hh;
    s=l<data->balance?data->shadow_saturation/100.0:data->highlight_saturation/100.0;
    double ra=CLIP((fabs((-data->balance+l))*2.0));
    double la=(1.0-ra);
     
    hsl2rgb(&mixrgb[0],&mixrgb[1],&mixrgb[2],h,s,l);
    
    out[0]=in[0]*la + mixrgb[0]*ra;
    out[1]=in[1]*la + mixrgb[1]*ra;
    out[2]=in[2]*la + mixrgb[2]*ra;
  
    out += 3; in += 3;
  }
}

static void
highlight_saturation_callback(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  p->highlight_saturation= dtgtk_slider_get_value(slider);
  // update foreground color and redraw colorpick1 button
  dt_dev_add_history_item(darktable.develop, self);
}

static void
shadow_saturation_callback(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  p->shadow_saturation= dtgtk_slider_get_value(slider);
  // update foreground color and redraw colorpick1 button
  dt_dev_add_history_item(darktable.develop, self);
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

static void colorpick_button_callback(GtkButton *button,gpointer user_data)  
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
  
  c.red= 65535 * ((button==g->colorpick1)?p->shadow_color[0]:p->highlight_color[0]);
  c.green= 65535 * ((button==g->colorpick1)?p->shadow_color[1]:p->highlight_color[1]);
  c.blue= 65535 * ((button==g->colorpick1)?p->shadow_color[2]:p->highlight_color[2]);
  gtk_color_selection_set_current_color(cs,&c);
  if(gtk_dialog_run(GTK_DIALOG(csd))==GTK_RESPONSE_ACCEPT) 
  {
    gtk_color_selection_get_current_color(cs,&c);
    if(button==g->colorpick1)
    {
      p->shadow_color[0]=c.red/65535.0;
      p->shadow_color[1]=c.green/65535.0;
      p->shadow_color[2]=c.blue/65535.0;
      gtk_widget_modify_fg(GTK_WIDGET(g->colorpick1),GTK_STATE_NORMAL,&c);
    } 
    else
    {
      p->highlight_color[0]=c.red/65535.0;
      p->highlight_color[1]=c.green/65535.0;
      p->highlight_color[2]=c.blue/65535.0;
      gtk_widget_modify_fg(GTK_WIDGET(g->colorpick2),GTK_STATE_NORMAL,&c);
    }
    gtk_widget_hide(GTK_WIDGET(csd));
  }
  dt_dev_add_history_item(darktable.develop, self);
}



void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[velvia] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_splittoning_data_t *d = (dt_iop_splittoning_data_t *)piece->data;
  memcpy(&d->shadow_color,&p->shadow_color,sizeof(float)*3);
  d->shadow_saturation = p->shadow_saturation;
  memcpy(&d->highlight_color,&p->highlight_color,sizeof(float)*3);
  d->highlight_saturation = p->highlight_saturation;
  d->balance = p->balance;
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
  dtgtk_slider_set_value(g->scale1, p->shadow_saturation);
  dtgtk_slider_set_value(g->scale2, p->highlight_saturation);
  dtgtk_slider_set_value(g->scale3, p->balance);
  GdkColor rgb;
  char color[32];
  sprintf(color,"#%.4X%.4X%.4X",(gint)(65535*p->shadow_color[0]),(gint)(65535*p->shadow_color[1]),(gint)(65535*p->shadow_color[2]));
  gdk_color_parse(color,&rgb);
  gtk_widget_modify_fg(GTK_WIDGET(g->colorpick1),GTK_STATE_NORMAL,&rgb);
  sprintf(color,"#%.4X%.4X%.4X",(gint)(65535*p->highlight_color[0]),(gint)(65535*p->highlight_color[1]),(gint)(65535*p->highlight_color[2]));
  gdk_color_parse(color,&rgb);
  gtk_widget_modify_fg(GTK_WIDGET(g->colorpick2),GTK_STATE_NORMAL,&rgb);
  
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_splittoning_params_t));
  module->default_params = malloc(sizeof(dt_iop_splittoning_params_t));
  module->default_enabled = 0;
  module->priority = 970;
  module->params_size = sizeof(dt_iop_splittoning_params_t);
  module->gui_data = NULL;
  dt_iop_splittoning_params_t tmp = (dt_iop_splittoning_params_t){ {0.2,0.2,0.9},50.0,{0.2,0.9,0.2},50.0, 0.5};
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

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(_("shadow")));
  g->label2 = GTK_LABEL(gtk_label_new(_("highlight")));
  g->label3 = GTK_LABEL(gtk_label_new(_("balance")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  
  GtkWidget *hbox=gtk_hbox_new(TRUE,0);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.5, p->shadow_saturation, 2));
  dtgtk_slider_set_format_type(g->scale1,DARKTABLE_SLIDER_FORMAT_PERCENT);
  g->colorpick1 = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE));
  gtk_widget_set_size_request(GTK_WIDGET(g->colorpick1),32,17);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->colorpick1), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(hbox), TRUE, TRUE, 0);
    
  hbox=gtk_hbox_new(TRUE,0);
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.5, p->highlight_saturation, 2));
  dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);
  g->colorpick2 = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE));
  gtk_widget_set_size_request(GTK_WIDGET(g->colorpick2),32,17);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->colorpick2), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.010, p->balance, 3));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the amount of saturation to apply to shadows"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("the amount of saturation to apply to highlights"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale3), "tooltip-text", _("the balance of center of splittoning"), NULL);
  
  gtk_object_set(GTK_OBJECT(g->colorpick1), "tooltip-text", _("select the color for shadows"), NULL);
  gtk_object_set(GTK_OBJECT(g->colorpick2), "tooltip-text", _("select the color for highlights"), NULL);
  
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (shadow_saturation_callback), self);  
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (highlight_saturation_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
        G_CALLBACK (balance_callback), self);
        
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

