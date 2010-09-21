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
#include "dtgtk/gradientslider.h"
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
  float hue;
  float saturation;
}
dt_iop_graduatednd_params_t;

typedef struct dt_iop_graduatednd_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;                                            // left and right controlboxes
  GtkLabel  *label1,*label2,*label3,*label4,*label5,*label6;            			      // density, compression, rotation, offset, hue, saturation
  GtkDarktableSlider *scale1,*scale2,*scale3,*scale4;        // density, compression, rotation, offset
  GtkDarktableGradientSlider *gslider1,*gslider2;		// hue, saturation
}
dt_iop_graduatednd_gui_data_t;

typedef struct dt_iop_graduatednd_data_t
{
  float density;			          	// The density of filter 0-8 EV
  float compression;			        // Default 0% = soft and 100% = hard 
  float rotation;		          	// 2*PI -180 - +180
  float offset;				            // Default 50%, centered, can be offsetted...
  float hue;                      // the hue
  float saturation;             // the saturation
}
dt_iop_graduatednd_data_t;

const char *name()
{
  return _("graduated neutral density");
}


int 
groups () 
{
  return IOP_GROUP_EFFECT;
}

static inline float
f (const float t, const float c, const float x)
{
  return (t/(1.0f + powf(c, -x*6.0f)) + (1.0f-t)*(x*.5f+.5f));
}

typedef struct dt_iop_vector_2d_t
{
  double x;
  double y;
} dt_iop_vector_2d_t;

static inline void hue2rgb(float m1,float m2,float hue,float *channel)
{
  if(hue<0.0) hue+=1.0;
  else if(hue>1.0) hue-=1.0;
  
  if( (6.0*hue) < 1.0) *channel=(m1+(m2-m1)*hue*6.0);
  else if((2.0*hue) < 1.0) *channel=m2;
  else if((3.0*hue) < 2.0) *channel=(m1+(m2-m1)*((2.0/3.0)-hue)*6.0);
  else *channel=m1;
}

static inline void hsl2rgb(float *r,float *g,float *b,float h,float s,float l)
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
  dt_iop_graduatednd_data_t *data = (dt_iop_graduatednd_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  
  const int ix= (roi_in->x);
  const int iy= (roi_in->y);
  const float iw=piece->buf_in.width*roi_out->scale;
  const float ih=piece->buf_in.height*roi_out->scale;
  const float hw=iw/2.0;
  const float hh=ih/2.0;
  float v=(-data->rotation/180)*M_PI;
  const float sinv=sin(v);
  const float cosv=cos(v);
  const float filter_radie=sqrt((hh*hh)+(hw*hw))/hh; 

  float color[3];
  hsl2rgb(&color[0],&color[1],&color[2],data->hue,data->saturation,0.5);
  
  
  //fprintf(stderr,"filter_radie: %f\n",filter_radie);
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out, color, data) schedule(static)
#endif
  for(int y=0;y<roi_out->height;y++)
  {
    for(int x=0;x<roi_out->width;x++)
    {
      int k=(roi_out->width*y+x)*3;
      
      /* rotate pixel around center of offset*/
      dt_iop_vector_2d_t pv={-1,-1};
      float sx=-1.0+((ix+x)/iw)*2.0;
      float sy=-1.0+((iy+y)/ih)*2.0;
      sy+=-1.0+((data->offset/100.0)*2);
      pv.x=cosv*sx-sinv*sy;
      pv.y=sinv*sx-cosv*sy;
      
      float length=pv.y/filter_radie;
#if 1
      float compression = data->compression/100.0;
      length/=1.0-(0.5+(compression/2.0));
      float density = ( 1.0 / exp2f (data->density * CLIP( ((1.0+length)/2.0)) ) );
#else
      const float compression = data->compression/100.0f;
      const float t = 1.0f - .8f/(.8f + compression);
      const float c = 1.0f + 1000.0f*powf(4.0, compression);
      const float density = 1.0f/exp2f(data->density*f(t, c, length));
#endif
      
      for( int l=0;l<3;l++)
        out[k+l] = fmaxf(0.0, (in[k+l]*(density/(1.0-(1.0-density)*color[l])) ));
      
    }
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
  d->hue = p->hue;
  d->saturation = p->saturation;
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
  dtgtk_slider_set_value (g->scale3, p->rotation);
  dtgtk_slider_set_value (g->scale4, p->offset);
  dtgtk_gradient_slider_set_value(g->gslider1,p->hue);
  dtgtk_gradient_slider_set_value(g->gslider2,p->saturation);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_graduatednd_params_t));
  module->default_params = malloc(sizeof(dt_iop_graduatednd_params_t));
  module->default_enabled = 0;
  module->priority = 251;
  module->params_size = sizeof(dt_iop_graduatednd_params_t);
  module->gui_data = NULL;
  dt_iop_graduatednd_params_t tmp = (dt_iop_graduatednd_params_t){2.0,0,0,50,0,0};
  memcpy(module->params, &tmp, sizeof(dt_iop_graduatednd_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_graduatednd_params_t));
  
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void
hue_callback(GtkDarktableGradientSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;

  double hue=dtgtk_gradient_slider_get_value(g->gslider1);
  double saturation=1.0;
  float color[3];
  hsl2rgb(&color[0],&color[1],&color[2],hue,saturation,0.5);
  
  GdkColor c;
  c.red=color[0]*65535.0;
  c.green=color[1]*65535.0;
  c.blue=color[2]*65535.0;
  
  dtgtk_gradient_slider_set_stop(g->gslider2,1.0,c);  // Update saturation end color

  if(self->dt->gui->reset) 
    return;
  gtk_widget_draw(GTK_WIDGET(g->gslider2),NULL);
  
  if(dtgtk_gradient_slider_is_dragging(slider)==FALSE) 
  {
    p->hue = dtgtk_gradient_slider_get_value(slider);
    dt_dev_add_history_item(darktable.develop, self);
  }
}

static void
saturation_callback(GtkDarktableGradientSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  
  if(dtgtk_gradient_slider_is_dragging(slider)==FALSE) 
  {
    p->saturation = dtgtk_gradient_slider_get_value(slider);
    dt_dev_add_history_item(darktable.develop, self);
  }
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_graduatednd_gui_data_t));
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;

  self->widget = gtk_table_new (7,2,FALSE);
  gtk_table_set_col_spacing(GTK_TABLE(self->widget), 0, 10);
  
  /* adding the labels */
  g->label1 = GTK_LABEL(gtk_label_new(_("density")));
  g->label2 = GTK_LABEL(gtk_label_new(_("compression")));
  g->label3 = GTK_LABEL(gtk_label_new(_("rotation")));
  g->label4 = GTK_LABEL(gtk_label_new(_("split")));
  g->label5 = GTK_LABEL(gtk_label_new(_("hue")));
  g->label6 = GTK_LABEL(gtk_label_new(_("saturation")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label4), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label5), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label6), 0.0, 0.5);
  
  
  gtk_table_attach (GTK_TABLE (self->widget), GTK_WIDGET (g->label1), 0,1,0,1,GTK_FILL,0,0,0);
  gtk_table_attach (GTK_TABLE (self->widget), GTK_WIDGET (g->label2), 0,1,1,2,GTK_FILL,0,0,0);
  gtk_table_attach (GTK_TABLE (self->widget), GTK_WIDGET (g->label3), 0,1,2,3,GTK_FILL,0,0,0);
  gtk_table_attach (GTK_TABLE (self->widget), GTK_WIDGET (g->label4), 0,1,3,4,GTK_FILL,0,0,0);
  gtk_table_attach (GTK_TABLE (self->widget), GTK_WIDGET (g->label5), 0,1,4,5,GTK_FILL,0,0,0);
  gtk_table_attach (GTK_TABLE (self->widget), GTK_WIDGET (g->label6), 0,1,5,6,GTK_FILL,0,0,0);
  
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 8.0, 0.01, p->density, 2));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->compression, 0));
  dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-180, 180,0.5, p->rotation, 2));
  g->scale4 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 1.0, p->offset, 0));
  dtgtk_slider_set_format_type(g->scale4,DARKTABLE_SLIDER_FORMAT_PERCENT);
  
  gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (g->scale1), 1,2,0,1);
  gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (g->scale2), 1,2,1,2);
  gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (g->scale3), 1,2,2,3);
  gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (g->scale4), 1,2,3,4);

  /* hue slider */
  int lightness=32768;
  g->gslider1=DTGTK_GRADIENT_SLIDER(dtgtk_gradient_slider_new_with_color((GdkColor){0,lightness,0,0},(GdkColor){0,lightness,0,0}));
  dtgtk_gradient_slider_set_stop(g->gslider1,0.166,(GdkColor){0,lightness,lightness,0});
  dtgtk_gradient_slider_set_stop(g->gslider1,0.332,(GdkColor){0,0,lightness,0});
  dtgtk_gradient_slider_set_stop(g->gslider1,0.498,(GdkColor){0,0,lightness,lightness});
  dtgtk_gradient_slider_set_stop(g->gslider1,0.664,(GdkColor){0,0,0,lightness});
  dtgtk_gradient_slider_set_stop(g->gslider1,0.83,(GdkColor){0,lightness,0,lightness});
  gtk_object_set(GTK_OBJECT(g->gslider1), "tooltip-text", _("select the hue tone of filter"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->gslider1), "value-changed",
        G_CALLBACK (hue_callback), self);

  
  gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (g->gslider1), 1,2,4,5);
  
  /* saturation slider */
  g->gslider2=DTGTK_GRADIENT_SLIDER(dtgtk_gradient_slider_new_with_color((GdkColor){0,lightness,lightness,lightness},(GdkColor){0,lightness,lightness,lightness}));
  gtk_object_set(GTK_OBJECT(g->gslider1), "tooltip-text", _("select the saturation of filter"), (char *)NULL);
   g_signal_connect (G_OBJECT (g->gslider2), "value-changed",
        G_CALLBACK (saturation_callback), self);

  gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (g->gslider2), 1,2,5,6);

  
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the density in EV for the filter"), (char *)NULL);
  /* xgettext:no-c-format */
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("compression of graduation:\n0% = soft, 100% = hard"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale3), "tooltip-text", _("rotation of filter -180 to 180 degrees"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale4), "tooltip-text", _("offset of filter in angle of rotation"), (char *)NULL);
  
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
        G_CALLBACK (density_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
        G_CALLBACK (compression_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
        G_CALLBACK (rotation_callback), self);  
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
        G_CALLBACK (offset_callback), self);  
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

