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
#include "dtgtk/togglebutton.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/slider.h"
#include "dtgtk/gradientslider.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "LibRaw/libraw/libraw.h"

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE(1)

typedef struct dt_iop_relight_params_t
{
  float ev;
  float center;
  float width;
}
dt_iop_relight_params_t;

void init_presets (dt_iop_module_t *self)
{
  sqlite3_exec(darktable.db, "begin", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("Fill-light 0.25EV with 4 zones"), self->op, &(dt_iop_relight_params_t){0.25,0.25,4.0} , sizeof(dt_iop_relight_params_t), 1);
  dt_gui_presets_add_generic(_("Fill-shadow -0.25EV with 4 zones"), self->op, &(dt_iop_relight_params_t){-0.25,0.25,4.0} , sizeof(dt_iop_relight_params_t), 1);

  sqlite3_exec(darktable.db, "commit", NULL, NULL, NULL);
}

typedef struct dt_iop_relight_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;                                            // left and right controlboxes
  GtkLabel  *label1,*label2,*label3;            		       	// ev, center, width
  GtkDarktableSlider *scale1,*scale2;        			// ev,width
  GtkDarktableGradientSlider *gslider1;				// center
  GtkDarktableToggleButton *tbutton1;                     // Pick median lightess
}
dt_iop_relight_gui_data_t;

typedef struct dt_iop_relight_data_t
{
  float ev;			          	// The ev of relight -4 - +4 EV
  float center;		          		// the center light value for relight
  float width;			        	// the width expressed in zones
}
dt_iop_relight_data_t;

const char *name()
{
  return _("relight");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES;
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

static inline void hue2rgb(float m1,float m2,float hue,float *channel)
{
  if(hue<0.0) hue+=1.0;
  else if(hue>1.0) hue-=1.0;
  
  if( (6.0*hue) < 1.0) *channel=(m1+(m2-m1)*hue*6.0);
  else if((2.0*hue) < 1.0) *channel=m2;
  else if((3.0*hue) < 2.0) *channel=(m1+(m2-m1)*((2.0/3.0)-hue)*6.0);
  else *channel=m1;
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

#define GAUSS(a,b,c,x) (a*pow(2.718281828,(-pow((x-b),2)/(pow(c,2)))))

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_relight_data_t *data = (dt_iop_relight_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
 
  // Precalculate parameters for gauss function
  const float a = 1.0;                                                                // Height of top
  const float b = -1.0+(data->center*2);                                 // Center of top
  const float c = (data->width/10.0)/2.0;      				                    // Width
  
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out,data) schedule(static)
#endif
  for(int k=0;k<roi_out->width*roi_out->height;k++)
  {
    const float lightness = in[ch*k]/100.0;
    const float x = -1.0+(lightness*2.0);
    float gauss = GAUSS(a,b,c,x);

    if(isnan(gauss) || isinf(gauss)) 
      gauss = 0.0;
    
    float relight = 1.0 / exp2f ( -data->ev * CLIP(gauss));
    
    if(isnan(relight) || isinf(relight)) 
      relight = 1.0;

    out[ch*k+0] = 100.0*CLIP (lightness*relight);
    out[ch*k+1] = in[ch*k+1];
    out[ch*k+2] = in[ch*k+2];
  }
}

static void
picker_callback (GtkDarktableToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button)))
  {
    dt_iop_request_focus (self);
    self->request_color_pick = 1;
  }
  else
    self->request_color_pick = 0;
}

static void
ev_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;
  p->ev = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
width_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;
  p->width = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
center_callback(GtkDarktableGradientSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;
  
  {
    p->center = dtgtk_gradient_slider_get_value(slider);
    dt_dev_add_history_item(darktable.develop, self);
  }
}



void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[relight] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_relight_data_t *d = (dt_iop_relight_data_t *)piece->data;
  d->ev = p->ev;
  d->width = p->width;
  d->center = p->center;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_relight_data_t));
  memset(piece->data,0,sizeof(dt_iop_relight_data_t));
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
  
  self->request_color_pick = 0;
  self->color_picker_box[0] = self->color_picker_box[1] = .25f;
  self->color_picker_box[2] = self->color_picker_box[3] = .75f;
  
  dt_iop_relight_gui_data_t *g = (dt_iop_relight_gui_data_t *)self->gui_data;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)module->params;
  dtgtk_slider_set_value (g->scale1, p->ev);
  dtgtk_slider_set_value (g->scale2, p->width);
  dtgtk_gradient_slider_set_value(g->gslider1,p->center);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_relight_params_t));
  module->default_params = malloc(sizeof(dt_iop_relight_params_t));
  module->default_enabled = 0;
  module->priority = 720;
  module->params_size = sizeof(dt_iop_relight_params_t);
  module->gui_data = NULL;
  dt_iop_relight_params_t tmp = (dt_iop_relight_params_t){0.33,0,4};
  memcpy(module->params, &tmp, sizeof(dt_iop_relight_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_relight_params_t));
  
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
    self->request_color_pick=0;
    return 1;
}

static gboolean
expose (GtkWidget *widget, GdkEventExpose *event, dt_iop_module_t *self)
{ 
  // capture gui color picked event.
  if(darktable.gui->reset) return FALSE;
  if(self->picked_color_max_Lab[0] < self->picked_color_min_Lab[0]) return FALSE;
  if(!self->request_color_pick) return FALSE;
  const float *Lab = self->picked_color_Lab;

  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;
  p->center = Lab[0];
  dt_dev_add_history_item(darktable.develop, self);
  darktable.gui->reset = 1;
  dt_iop_relight_gui_data_t *g = (dt_iop_relight_gui_data_t *)self->gui_data;
  dtgtk_gradient_slider_set_value(DTGTK_GRADIENT_SLIDER(g->gslider1),p->center);
  darktable.gui->reset = 0;

  return FALSE;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_relight_gui_data_t));
  dt_iop_relight_gui_data_t *g = (dt_iop_relight_gui_data_t *)self->gui_data;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;

  self->widget = gtk_table_new (3,2,FALSE);
  g_signal_connect (G_OBJECT (self->widget), "expose-event", G_CALLBACK (expose), self);
  
  gtk_table_set_col_spacing (GTK_TABLE (self->widget), 0, 10);
  gtk_table_set_row_spacings(GTK_TABLE (self->widget), DT_GUI_IOP_MODULE_CONTROL_SPACING);
  
  /* adding the labels */
  GtkWidget *label1 = dtgtk_reset_label_new (_("exposure"), self, &p->ev, sizeof(float));     // EV
  GtkWidget *label2 = dtgtk_reset_label_new (_("center"), self, &p->center, sizeof(float));
  GtkWidget *label3 = dtgtk_reset_label_new (_("width"), self, &p->width, sizeof(float));
  
  gtk_table_attach (GTK_TABLE (self->widget), label1, 0,1,0,1,GTK_FILL,0,0,0);
  gtk_table_attach (GTK_TABLE (self->widget), label2, 0,1,1,2,GTK_FILL,0,0,0);
  gtk_table_attach (GTK_TABLE (self->widget), label3, 0,1,2,3,GTK_FILL,0,0,0);
  
  g->scale1 = DTGTK_SLIDER (dtgtk_slider_new_with_range (DARKTABLE_SLIDER_BAR,-2.0, 2.0,0.05, p->ev, 2));
  g->scale2 = DTGTK_SLIDER (dtgtk_slider_new_with_range (DARKTABLE_SLIDER_BAR,2, 10, 0.5, p->width, 1));
  
  gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (g->scale1), 1,2,0,1);
  gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (g->scale2), 1,2,2,3);
 

 
  /* lightnessslider */
  GtkBox *hbox=GTK_BOX (gtk_hbox_new (FALSE,2));
  int lightness=32768;
  g->gslider1=DTGTK_GRADIENT_SLIDER (dtgtk_gradient_slider_new_with_color ((GdkColor){0,0,0,0},(GdkColor){0,lightness,lightness,lightness}));
  gtk_object_set (GTK_OBJECT (g->gslider1), "tooltip-text", _("select the center of fill-light"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->gslider1), "value-changed",
        G_CALLBACK (center_callback), self);
  g->tbutton1 = DTGTK_TOGGLEBUTTON (dtgtk_togglebutton_new (dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT));
  g_signal_connect (G_OBJECT (g->tbutton1), "toggled",
        G_CALLBACK (picker_callback), self);
 
  gtk_box_pack_start (hbox,GTK_WIDGET (g->gslider1),TRUE,TRUE,0);
  gtk_box_pack_start (hbox,GTK_WIDGET (g->tbutton1),FALSE,FALSE,0);
  gtk_table_attach_defaults (GTK_TABLE (self->widget), GTK_WIDGET (hbox), 1,2,1,2);

  
  gtk_object_set(GTK_OBJECT(g->tbutton1), "tooltip-text", _("toggle tool for picking median lightness in image"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the fill-light in EV"), (char *)NULL);
  /* xgettext:no-c-format */
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("width of fill-light area defined in zones"), (char *)NULL);
  
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
        G_CALLBACK (ev_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
        G_CALLBACK (width_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  self->request_color_pick = 0;
  free(self->gui_data);
  self->gui_data = NULL;
}

