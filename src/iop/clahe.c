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

#define ROUND_POSISTIVE(f) ((unsigned int)((f)+0.5))

DT_MODULE(1)

typedef struct dt_iop_rlce_params_t
{
  double radius;
  double slope;
}
dt_iop_rlce_params_t;

typedef struct dt_iop_rlce_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkWidget  *label1,*label2;
  GtkDarktableSlider *scale1,*scale2;       // radie pixels, slope
}
dt_iop_rlce_gui_data_t;

typedef struct dt_iop_rlce_data_t
{
  double radius;
  double slope;
}
dt_iop_rlce_data_t;

const char *name()
{
  return _("local contrast (slow)");
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
  dt_iop_rlce_data_t *data = (dt_iop_rlce_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  
  // PASS1: Get a luminance map of image...
  float *luminance=(float *)malloc((roi_out->width*roi_out->height)*sizeof(float));
  float *lm=luminance;
  double lsmax=0.0,lsmin=1.0;
  for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
  {
    double pmax=CLIP(fmax(in[0],fmax(in[1],in[2]))); // Max value in RGB set
    double pmin=CLIP(fmin(in[0],fmin(in[1],in[2]))); // Min value in RGB set
    *lm=(pmax+pmin)/2.0;        // Pixel luminocity
    in+=3; lm++;
    
    if( pmax > lsmax ) lsmax=pmax;
    if( pmin < lsmin ) lsmin=pmin;
  }
  
  
  // Params
  int rad=data->radius*roi_in->scale/piece->iscale;
 
  int bins=256;
  float slope=data->slope;
  
  // CLAHE
  in  = (float *)ivoid;
  out = (float *)ovoid;
  lm = luminance;
  
  int *hist = malloc((bins+1)*sizeof(int));
  int *clippedhist = malloc((bins+1)*sizeof(int));
  memset(hist,0,(bins+1)*sizeof(int));
  float *dest=malloc(roi_out->width*sizeof(float));

  float H,S,L;
  for(int j=0;j<roi_out->height;j++) 
  {
    int yMin = fmax( 0, j - rad );
    int yMax = fmin( roi_in->height, j + rad + 1 );
    int h = yMax - yMin;

    int xMin0 = fmax( 0, 0-rad );
    int xMax0 = fmin( roi_in->width - 1, rad );

    
    /* initially fill histogram */
    memset(hist,0,(bins+1)*sizeof(int));
    for ( int yi = yMin; yi < yMax; ++yi ) 
      for ( int xi = xMin0; xi < xMax0; ++xi )
        ++hist[ ROUND_POSISTIVE(luminance[yi*roi_in->width+xi] * (float)bins) ];

    // Destination row
    memset(dest,0,roi_out->width*sizeof(float));
    float *ld=dest;
   
    for(int i=0;i<roi_out->width;i++)
    {
      
      int v = ROUND_POSISTIVE(luminance[j*roi_in->width+i] * (float)bins);

      int xMin = fmax( 0, i - rad );
      int xMax = i + rad + 1;
      int w = fmin( roi_in->width, xMax ) - xMin;
      int n = h * w;

      int limit = ( int )( slope * n /  bins + 0.5f );
      
      /* remove left behind values from histogram */
      if ( xMin > 0 )
      {
        int xMin1 = xMin - 1;
        for ( int yi = yMin; yi < yMax; ++yi )
          --hist[  ROUND_POSISTIVE(luminance[yi*roi_in->width+xMin1] * (float)bins) ];
      }

      /* add newly included values to histogram */
      if ( xMax <= roi_in->width )
      {
        int xMax1 = xMax - 1;
        for ( int yi = yMin; yi < yMax; ++yi )
          ++hist[  ROUND_POSISTIVE(luminance[yi*roi_in->width+xMax1] * (float)bins) ];
      }

      /* clip histogram and redistribute clipped entries */
      memcpy(clippedhist,hist,(bins+1)*sizeof(int));
      int ce = 0, ceb=0;
      do
      {
        ceb = ce;
        ce = 0;
        for ( int b = 0; b <= bins; b++ )
        {
          int d = clippedhist[ b ] - limit;
          if ( d > 0 )
          {
            ce += d;
            clippedhist[ b ] = limit;
          }
        }
        
        int d = (ce / (float) ( bins + 1 ));
        int m = ce % ( bins + 1 );
        for ( int h = 0; h <= bins; h++)
          clippedhist[ h ] += d;
        
        if ( m != 0 )
        {
          int s = bins / (float)m;
          for ( int h = 0; h <= bins; h += s )
            ++clippedhist[ h ];
        }
      }
      while ( ce != ceb);
      
      /* build cdf of clipped histogram */
      int hMin = bins;
      for ( int h = 0; h < hMin; h++ )
        if ( clippedhist[ h ] != 0 ) hMin = h;

      int cdf = 0;
      for ( int h = hMin; h <= v; h++ )
        cdf += clippedhist[ h ];

      int cdfMax = cdf;
      for ( int h = v + 1; h <= bins; h++ )
        cdfMax += clippedhist[ h ];

      int cdfMin = clippedhist[ hMin ];

      *ld=( cdf - cdfMin ) / ( float )( cdfMax - cdfMin );      
     
      lm++,ld++;
    }
    
    // Apply row
    for(int r=0;r<roi_out->width;r++)
    {
      rgb2hsl(in[0],in[1],in[2],&H,&S,&L);
      //hsl2rgb(&out[0],&out[1],&out[2],H,S,( L / dest[r] ) * (L-lsmin) + lsmin );
      hsl2rgb(&out[0],&out[1],&out[2],H,S,dest[r] );
      out += 3; in += 3;ld++; 
    }
    
  }
  
  // Cleanup
  free(hist);
  free(clippedhist);
  free(luminance);
  
}

static void
radius_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rlce_params_t *p = (dt_iop_rlce_params_t *)self->params;
  p->radius= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
slope_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_rlce_params_t *p = (dt_iop_rlce_params_t *)self->params;
  p->slope = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}



void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rlce_params_t *p = (dt_iop_rlce_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[local contrast] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_rlce_data_t *d = (dt_iop_rlce_data_t *)piece->data;
  d->radius = p->radius;
  d->slope = p->slope;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_rlce_data_t));
  memset(piece->data,0,sizeof(dt_iop_rlce_data_t));
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
  dt_iop_rlce_gui_data_t *g = (dt_iop_rlce_gui_data_t *)self->gui_data;
  dt_iop_rlce_params_t *p = (dt_iop_rlce_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->radius);
  dtgtk_slider_set_value(g->scale2, p->slope);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_rlce_params_t));
  module->default_params = malloc(sizeof(dt_iop_rlce_params_t));
  module->default_enabled = 0;
  module->priority = 966;
  module->params_size = sizeof(dt_iop_rlce_params_t);
  module->gui_data = NULL;
  dt_iop_rlce_params_t tmp = (dt_iop_rlce_params_t){64,1.25};
  memcpy(module->params, &tmp, sizeof(dt_iop_rlce_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_rlce_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_rlce_gui_data_t));
  dt_iop_rlce_gui_data_t *g = (dt_iop_rlce_gui_data_t *)self->gui_data;
  dt_iop_rlce_params_t *p = (dt_iop_rlce_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);

  g->label1 = dtgtk_reset_label_new(_("radius"), self, &p->radius, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), g->label1, TRUE, TRUE, 0);
  g->label2 = dtgtk_reset_label_new(_("amount"), self, &p->slope, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), g->label2, TRUE, TRUE, 0);
  
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 256.0, 1.0, p->radius, 0));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,1.0, 3.0, 0.05, p->slope, 2));
  //dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);
  
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("size of features to preserve"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("strength of the effect"), (char *)NULL);
  
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (radius_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
        G_CALLBACK (slope_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

