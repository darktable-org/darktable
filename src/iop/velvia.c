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

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE(1)

typedef struct dt_iop_velvia_params_t
{
  float saturation;
  float vibrance;
  float luminance;
  float clarity;
}
dt_iop_velvia_params_t;

typedef struct dt_iop_velvia_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkLabel  *label1,*label2,*label3,*label4;
  GtkDarktableSlider *scale1,*scale2,*scale3,*scale4;       // saturation, vibrance, luminance, clarity
}
dt_iop_velvia_gui_data_t;

typedef struct dt_iop_velvia_data_t
{
  float saturation;
  float vibrance;
  float luminance;
  float clarity;
  
  float *_contrast_layer;
  float *_blurred_layer;
  int _prev_x;
  int _prev_y;
  int _prev_width;
  int _prev_height;
}
dt_iop_velvia_data_t;

const char *name()
{
  return _("velvia");
}

static void rgb2hsl(float r,float g,float b, float *h,float *s,float *l) {
  float pmax=fmax(r,fmax(g,b));
  float pmin=fmin(r,fmin(g,b));
  *h=*s=0.0;
  *l=(pmax+pmin)/2.0;
  if( pmax!=pmin )
  {
    *s=(*l<0.5)?(pmax-pmin)/(pmax+pmin):(pmax-pmin)/(2.0-pmax-pmin);
    *h= (pmax==r) ? ((g-b)/(pmax-pmin)) : ( (pmax==g)?2.0+(b-r)/(pmax-pmin):4.0+(r-g)/(pmax-pmin) );
    *h= (*h*60.0>0.0)?*h*60.0:(*h*60.0)+360.0;
  }
}

static void hsl2rgb(float *r,float *g,float *b, float h,float s,float l)  {
  *r=*g=*b=l;
  if(s!=0)
  {
    float temp2=(l<0.5)?l*(1.0+s):l+s-l*s;
    float temp1=2.0*l-temp2;
    float th=h/360.0;
    float temp3[3];
    temp3[0]=th+1.0/3.0;
    temp3[1]=th;
    temp3[2]=th-1.0/3.0;
    temp3[0] += (temp3[0]<0)?1.0:(temp3[0]>1)?-1.0:0.0;
    temp3[1] += (temp3[1]<0)?1.0:(temp3[1]>1)?-1.0:0.0;
    temp3[2] += (temp3[2]<0)?1.0:(temp3[2]>1)?-1.0:0.0;
    *r=(6.0*temp3[0])<1?temp1+(temp2-temp1)*6.0*temp3[0]:(2.0*temp3[0])<1.0?temp2:(3.0*temp3[0])<2.0?temp1+(temp2-temp1)*((2.0/3.0)-temp3[0])*6.0:temp1;
    *g=(6.0*temp3[1])<1?temp1+(temp2-temp1)*6.0*temp3[1]:(2.0*temp3[1])<1.0?temp2:(3.0*temp3[1])<2.0?temp1+(temp2-temp1)*((2.0/3.0)-temp3[1])*6.0:temp1;
    *b=((6.0*temp3[2])<1.0) ? (temp1+(temp2-temp1)*6.0*temp3[2]) : ( ((2.0*temp3[2])<1.0) ? (temp2) : ((3.0*temp3[2])<2.0) ? (temp1+(temp2-temp1)*((2.0/3.0)-temp3[2])*6.0) : temp1);
    
  }
}


static void channel_gauss(float *pin,float *pout,int width,int height,float amount,int radie,float scaled_radie,float threshold,float sigma2) {
  const int rad = scaled_radie;
  float mat[2*(rad+1)*2*(rad+1)];
  const int wd = 2*rad+1;
  float *m = mat + rad*wd + rad;
  float weight = 0.0f;
  // init gaussian kernel
  for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
    weight += m[l*wd + k] = expf(- (l*l + k*k)/(2.f*sigma2));
  for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
    m[l*wd + k] /= weight;

  // gauss blur the image
  float *in=pin;
  float *out=pout;
  
  for(int j=rad;j<height-rad;j++)
  {
    in  = pin+ (j*width+ rad);
    out = pout+ (j*width+rad);
    for(int i=rad;i<width-rad;i++)
    {
      *out = 0.0f;
      for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
        *out += m[l*wd+k]*in[(l*width+k)]*amount;
      out++;in++;
    }
  }
  
  // fill unsharpened border
  for(int j=0;j<rad;j++) // top
    memcpy( pout + j*width, pin+ j*width, sizeof(float)*width);
  for(int j=height-rad;j<height;j++) // bottom
    memcpy( pout + j*width, pin + j*width, sizeof(float)*width);
  
  in  = pin;
  out = pout;
  for(int j=rad;j<height-rad;j++)
  {
    for(int i=0;i<rad;i++) // left
      out[(width*j + i) ] = in[(width*j + i)];
    for(int i=width-rad;i<width;i++) // right
      out[(width*j + i) ] = in[(width*j + i)];
  }  
}



void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_velvia_data_t *data = (dt_iop_velvia_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  
  // Apply velvia saturation
  
    in  = (float *)ivoid;
    out = (float *)ovoid;
    for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
    {
      if(data->saturation > 0.0)
      {
        // calculate vibrance, and apply boost velvia saturation at least saturated pixles
        double pmax=fmax(in[0],fmax(in[1],in[2]));			// Max value amonug RGB set
        double pmin=fmin(in[0],fmin(in[1],in[2]));			// Min value among RGB set
        double plum = (pmax+pmin)/2.0;					// Pixel luminocity
        double psat =(plum<=0.5) ? (pmax-pmin)/(pmax+pmin): (pmax-pmin)/(2-pmax-pmin);

        double pweight=((1.0- (1.5*psat)) + ((1+(fabs(plum-0.5)*2.0))*(1.0-data->luminance))) / (1.0+(1.0-data->luminance));		// The weight of pixel
        double saturation = (data->saturation*pweight)*data->vibrance;			// So lets calculate the final affection of filter on pixel

        // Apply velvia saturation values
        double sba=1.0+saturation;
        double sda=(sba/2.0)-0.5;
        out[0]=(in[0]*(sba))-(in[1]*(sda))-(in[2]*(sda));
        out[1]=(in[1]*(sba))-(in[0]*(sda))-(in[2]*(sda));
        out[2]=(in[2]*(sba))-(in[0]*(sda))-(in[1]*(sda));  
      }
      else
        for(int c=0;c<3;c++) out[c]=in[c];
      out += 3; in += 3;
    }
  
  
  // Clarity, local contrast enhanced...  
  in  = (float *)ivoid;
  out = (float *)ovoid;
 
  float *orginal_layer=malloc((roi_out->width*roi_out->height)*sizeof(float));	// The orginal lighness channel temp used to generate blurred & contrast
  
  float H,S;
  for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)  {
    rgb2hsl(in[0],in[1],in[2],&H,&S,&orginal_layer[j*roi_out->width+i]);    // Get  pixel lightness
    in+=3;
  }
  
  // Lets create high contrast layer = USM , 50px radie, 1% amount, if size or offset has change
  if(data->_contrast_layer==NULL || data->_blurred_layer==0 || (roi_out->width != data->_prev_width && roi_out->width != data->_prev_height && roi_out->x != data->_prev_x && roi_out->y!=data->_prev_y)) {
   
    if(data->_contrast_layer!=NULL) free(data->_contrast_layer);
    if(data->_blurred_layer!=NULL) free(data->_blurred_layer);
    data->_contrast_layer=malloc((roi_out->width*roi_out->height)*sizeof(float)); // 50px radius 1% amount
    data->_blurred_layer=malloc((roi_out->width*roi_out->height)*sizeof(float));	// 10% of image size
    channel_gauss(orginal_layer,data->_contrast_layer,roi_out->width,roi_out->height,1.0,50,(50*roi_in->scale / (piece->iscale * piece->iscale)),0.0, (2.5*2.5)*(50*roi_in->scale)*(50*roi_in->scale));
    data->_prev_x=roi_out->x;
    data->_prev_y=roi_out->y;
    data->_prev_width=roi_out->width;
    data->_prev_height=roi_out->height;
   
    //double blurradius=roi_out->width*0.1;
    //channel_gauss(orginal_layer,blurred_layer,roi_out->width,roi_out->height,1.0,blurradius,(blurradius*roi_in->scale / (piece->iscale * piece->iscale)),0.0, (2.5*2.5)*(blurradius*roi_in->scale)*(blurradius*roi_in->scale));
    memcpy(data->_blurred_layer,data->_contrast_layer,(roi_out->width*roi_out->height)*sizeof(float));
  
    in  = (float *)orginal_layer;
    out = (float *)data->_contrast_layer;

    // subtract blurred image, if diff > thrs, add *amount to orginal image
    for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
    {
      const float diff = *in - *out;
      if(fabsf(diff) > 0) // Threshold = 0
      {
        const float detail = copysignf(fmaxf(fabsf(diff) - 0, 0.0), diff);
        *out = fmaxf(0.0, *in + detail*1.0);
      }
      else *out = *in;
      out++;in++;
    }
 }
  
    
  
  in  = (float *)ivoid;
  out = (float *)ovoid;
  float L,CL;
  float *lin=orginal_layer, *blin=data->_blurred_layer, *clin=data->_contrast_layer;
  
  for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
  {
    rgb2hsl(out[0],out[1],out[2],&H,&S,&L);
    CL=(*clin* (1-(CLIP(*lin-*blin)+CLIP(*blin-*lin))));
    CL-=(-0.5+CL)*(1.0-data->clarity);
    double cla=CL;
    double la=L;
    if(L<0.5) L=2*cla*la;
    else L=1.0-2*(1.0-cla)*(1.0-la);
    hsl2rgb(&out[0],&out[1],&out[2],H,S,L);
    //out[0]=out[1]=out[2]=CLIP((*lin-*blin))+CLIP((*blin-*lin));
    lin++;blin++;clin++;
    in+=3; out+=3;
    
  }
  
  
  free(orginal_layer);
  
  /*double adaptive=0.5;  // get you own slider...
  double amount=data->clarity*0.6;  // get your own slider...
  double radius=50;
  adaptive=sin(0.5*M_PI*adaptive);
  
  const int rad =  radius * roi_in->scale / (piece->iscale * piece->iscale);
  if(rad == 0) return;
    
  // Pass1, Allocate and create blurred image layer
  float *blurred=malloc(3*(roi_out->width*roi_out->height)*sizeof(float));
  for(int j=rad;j<roi_out->height-rad;j++)
  {
    in  = ((float *)ovoid) + 3*(j*roi_in->width  + rad);
    out = blurred + 3*(j*roi_out->width + rad);
    for(int i=rad;i<roi_out->width-rad;i++)
    {
      for(int c=0;c<3;c++) out[c] = 0.0f; // Clear avglightness layer
      
      for(int l=-rad;l<=rad;l++) for(int k=-rad;k<=rad;k++)
        for(int c=0;c<3;c++) out[c] += in[3*(l*roi_in->width+k)+c];
      
      for(int c=0;c<3;c++) out[c]/=2*rad+1; // Average block lightness evenly
      
      out += 3; in += 3;
    }
  }
  
  // Pass2
  in  = (float *)blurred;
  out = (float *)ovoid;
  float H,S,ln,avgln;
  for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
  {
    double ca=fmax(0.0,amount*(1-pow(((fabs(out[0]-in[0])+fabs(out[1]-in[1])+fabs(out[2]-in[2]))/3.0), 1-adaptive))); // addative contrast amount...
    rgb2hsl(in[0],in[1],in[2],&H,&S,&avgln);    // Get HSL from average lightness layer
    rgb2hsl(out[0],out[1],out[2],&H,&S,&ln);   // Get HSL of output 
    hsl2rgb(&out[0],&out[1],&out[2],H,S,((ca+1)*ln)+((1-(ca+1))*avgln)); // Set new RGB output with calculated HSL lightness
    in+=3;out+=3;
  }
  
  free(blurred);*/
  
}

static void
saturation_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)self->params;
  p->saturation = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
vibrance_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)self->params;
  p->vibrance = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
luminance_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)self->params;
  p->luminance= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
clarity_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)self->params;
  p->clarity = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[velvia] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_velvia_data_t *d = (dt_iop_velvia_data_t *)piece->data;
  d->saturation = p->saturation;
  d->vibrance = p->vibrance;
  d->luminance = p->luminance;
  d->clarity = p->clarity;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_velvia_data_t));
  memset(piece->data,0,sizeof(dt_iop_velvia_data_t));
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
  dt_iop_velvia_gui_data_t *g = (dt_iop_velvia_gui_data_t *)self->gui_data;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->saturation);
  dtgtk_slider_set_value(g->scale2, p->vibrance);
  dtgtk_slider_set_value(g->scale3, p->luminance);
  dtgtk_slider_set_value(g->scale4, p->clarity);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_velvia_params_t));
  module->default_params = malloc(sizeof(dt_iop_velvia_params_t));
  module->default_enabled = 0;
  module->priority = 970;
  module->params_size = sizeof(dt_iop_velvia_params_t);
  module->gui_data = NULL;
  dt_iop_velvia_params_t tmp = (dt_iop_velvia_params_t){.5,.5,.0,.0};
  memcpy(module->params, &tmp, sizeof(dt_iop_velvia_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_velvia_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_velvia_gui_data_t));
  dt_iop_velvia_gui_data_t *g = (dt_iop_velvia_gui_data_t *)self->gui_data;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(_("saturation")));
  g->label2 = GTK_LABEL(gtk_label_new(_("vibrance")));
  g->label3 = GTK_LABEL(gtk_label_new(_("mid-tones bias")));
  g->label4 = GTK_LABEL(gtk_label_new(_("clarity")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label4), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label4), TRUE, TRUE, 0);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0000, 0.010, p->saturation, 3));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0000, 0.010, p->vibrance, 3));
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0000, 0.010, p->luminance, 3));
  g->scale4 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0000, 0.010, p->clarity, 3));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the amount of saturation to apply"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("the vibrance amount"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale3), "tooltip-text", _("how much to spare highlights and shadows"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale4), "tooltip-text", _("the amount of local contrast to apply"), NULL);
 
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (saturation_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
        G_CALLBACK (vibrance_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
        G_CALLBACK (luminance_callback), self);  
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
        G_CALLBACK (clarity_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

