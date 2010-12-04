/*
    This file is part of darktable,
    copyright (c) 2009--2010 Thierry Leconte.

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

//
// A tonemapping module using Durand's process :
// <http://graphics.lcs.mit.edu/~fredo/PUBLI/Siggraph2002/>
// without the fast bilateral filtering, just a classical slow one.
//
// Some parts of this code come from pfstm sources :
// http://pfstools.sourceforge.net/pfstmo.html
// but with lots of mods
//

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

DT_MODULE(1)

typedef struct dt_iop_tonemapping_params_t
{
  float contrast,Fsize;
}
dt_iop_tonemapping_params_t;

typedef struct dt_iop_tonemapping_gui_data_t
{
  GtkDarktableSlider *contrast, *Fsize;
}
dt_iop_tonemapping_gui_data_t;

typedef struct dt_iop_tonemapping_data_t
{
  float contrast,Fsize;
  float gauss[512];
  float maxVal;
}
dt_iop_tonemapping_data_t;

const char *name()
{
  return _("tonemapping");
}


int 
groups () 
{
	return IOP_GROUP_BASIC;
}

// test if something change during processing in order to abort process immediatly;
int aborted_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t 
*piece)
{
  struct dt_dev_pixelpipe_t *pipe=piece->pipe;
  struct dt_develop_t *dev=self->dev;

  if(pipe != dev->preview_pipe)
   {
     if(pipe != dev->preview_pipe && pipe->changed == DT_DEV_PIPE_ZOOMED) return 1;
     if((pipe->changed != DT_DEV_PIPE_UNCHANGED && pipe->changed != DT_DEV_PIPE_ZOOMED) || dev->gui_leaving) return 1;
   }

  return 0;
}

static inline int max( int a, int b )
{ return (a>b) ? a : b; }

static inline int min( int a, int b )
{ return (a<b) ? a : b; }

static inline void gaussianKernel( float *kern, int size, float sigma)
{
  for( int y = 0; y < size; y++ ) {
    for( int x = 0; x < size; x++ ) {
      float rx = (float)(x - size/2);
      float ry = (float)(y - size/2);
      double d2 = rx*rx + ry*ry;
      kern[x+y*size] = exp( -d2 / (2.*sigma*sigma) );
    }
  }
}

static inline void gaussiantab(float *gauss, float*maxVal, int len, float sigma) 
{
  float sigma2 = sigma*sigma;
  *maxVal = sqrtf(-logf(0.01)*2.0*sigma2);
  for( int i = 0; i < len; i++ ) {
	float x = (float)i/(float)(len-1)*(*maxVal);
	gauss[i] = expf(-x*x/(2.0*sigma2));
  }
}

#define gausstablen 512

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_tonemapping_data_t *data = (dt_iop_tonemapping_data_t *)piece->data;

 int width,height,size;
 float *I,*BASE;
 float avgB;
 float scaleFactor;
 int sKernelSize;
 float *sKernel;
 float sigma_s;

  width=roi_in->width; 
  height=roi_in->height;
  size=width*height;

  I=(float*)malloc(sizeof(float)*size);
  BASE=(float*)malloc(sizeof(float)*size);

  // Build I=log(L)
  for(int i=0;i<size;i++) {
        float L= 0.2126*((float*)ivoid)[3*i] + 0.7152*((float*)ivoid)[3*i+1] + 0.0722*((float*)ivoid)[3*i+2];
	if(L<=0.0) L=1e-6;
  	I[i]=logf(L);
  }

  sigma_s=data->Fsize/100.0*sqrtf(size);
  if(piece->pipe==self->dev->preview_pipe)
	// minimum kernel size for preview
  	sKernelSize=3;
   else
  	sKernelSize=1.0+4.0*sigma_s;
  if(sKernelSize<3) sKernelSize=3;
  sKernel=malloc(sKernelSize*sKernelSize*sizeof(float));
  gaussianKernel( sKernel,sKernelSize,sigma_s);

  scaleFactor = (float)(gausstablen-1) / data->maxVal;

  // Bilateral filter
  avgB=0.0;
  for( int y = 0; y < height; y++ )
  {
    // test if some paramters have changed 
    if(y%30==0 && aborted_pipe(self,piece)) {
	// abort processing
  	free(sKernel);
  	free(BASE);
  	free(I);
	return;
    }
    for( int x = 0; x < width; x++ )
    {
      float val = 0;
      float k = 0;
      float I_s = I[x+y*width];
      float l;

      for( int py = max( 0, y - sKernelSize/2);
	   py < min( height, y + sKernelSize/2); py++ )
      {
	for( int px = max( 0, x - sKernelSize/2);
	     px < min( width, x + sKernelSize/2); px++ )
	{
	  float I_p = I[px+py*width];
	  float G_s;

      	  float dt = fabs( I_p - I_s );
      	  if(dt > data->maxVal)
		G_s=0.0;
      	  else 
		G_s=data->gauss[ (int)(dt*scaleFactor) ];

	  float mult = sKernel[(px-x + sKernelSize/2)+(py-y + sKernelSize/2)*sKernelSize] * G_s;

	  float Ixy = I[px+py*width];
	  
	  val += Ixy*mult;
	  k += mult;
	}
      }
      l = val/k;
      BASE[x+y*width] = l;
      avgB+=l;
    }
  }
  avgB/=height*width;
  free(sKernel);

  //
  // Durand process :
  // r=R/(input intensity), g=G/input intensity, B=B/input intensity
  // log(base)=Bilateral(log(input intensity))
  // log(detail)=log(input intensity)-log(base)
  // log (output intensity)=log(base)*compressionfactor+log(detail)
  // R output = r*exp(log(output intensity)), etc.
  //
  // Simplyfing :
  // R output = R/(input intensity)*exp(log(output intensity))
  //          = R*exp(log(output intensity)-log(input intensity))
  //          = R*exp(log(base)*compressionfactor+log(input intensity)-log(base)-log(input intensity))
  //          = R*exp(log(base)*(compressionfactor-1))
  //
  // Plus :
  //  Before compressing the base intensity , we remove average base intensity in order to not have
  //  variable average intensity when varying compression factor.
  //  after compression we substract 2.0 to have an average intensiy at middle tone.
  //
  for( int i=0 ; i<size ; i++ )
  {
    float L;
    
    L=(BASE[i]-avgB)/data->contrast-2.0;
    L=expf(L-BASE[i]);

   ((float *)ovoid)[3*i]=((float*)ivoid)[3*i]*L;
   ((float *)ovoid)[3*i+1]=((float*)ivoid)[3*i+1]*L;
   ((float *)ovoid)[3*i+2]=((float*)ivoid)[3*i+2]*L;

  }

  free(BASE);
  free(I);
}


// GUI
//
static void
contrast_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_tonemapping_params_t *p = (dt_iop_tonemapping_params_t *)self->params;
  p->contrast = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
Fsize_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_tonemapping_params_t *p = (dt_iop_tonemapping_params_t *)self->params;
  p->Fsize = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_tonemapping_params_t *p = (dt_iop_tonemapping_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[tonemapping] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_tonemapping_data_t *d = (dt_iop_tonemapping_data_t *)piece->data;
  d->contrast = p->contrast;
  d->Fsize = p->Fsize;

#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_tonemapping_data_t));
  dt_iop_tonemapping_data_t *data = (dt_iop_tonemapping_data_t *)piece->data;

  gaussiantab(data->gauss, &(data->maxVal),512,0.4);
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_tonemapping_gui_data_t *g = (dt_iop_tonemapping_gui_data_t *)self->gui_data;
  dt_iop_tonemapping_params_t *p = (dt_iop_tonemapping_params_t *)module->params;
  dtgtk_slider_set_value(g->contrast, p->contrast);
  dtgtk_slider_set_value(g->Fsize, p->Fsize);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_tonemapping_data_t));
  module->params = malloc(sizeof(dt_iop_tonemapping_params_t));
  module->default_params = malloc(sizeof(dt_iop_tonemapping_params_t));
  module->default_enabled = 0;
  module->priority = 149;
  module->params_size = sizeof(dt_iop_tonemapping_params_t);
  module->gui_data = NULL;
  dt_iop_tonemapping_params_t tmp = (dt_iop_tonemapping_params_t){2.5,0.5};
  memcpy(module->params, &tmp, sizeof(dt_iop_tonemapping_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_tonemapping_params_t));
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
  GtkWidget *widget;
  GtkHBox *box;

  self->gui_data = malloc(sizeof(dt_iop_tonemapping_gui_data_t));
  dt_iop_tonemapping_gui_data_t *g = (dt_iop_tonemapping_gui_data_t *)self->gui_data;
  dt_iop_tonemapping_params_t *p = (dt_iop_tonemapping_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));

  box = GTK_HBOX(gtk_hbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), FALSE, FALSE, 5);
  widget = dtgtk_reset_label_new(_("Contrast compression"), self, &p->contrast, sizeof(float));
  gtk_box_pack_start(GTK_BOX(box), widget, TRUE, TRUE, 0);
  g->contrast = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,1.0, 5.0000, 0.1, p->contrast, 3));
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->contrast), TRUE, TRUE, 0);
  g_signal_connect (G_OBJECT (g->contrast), "value-changed",G_CALLBACK (contrast_callback), self);

  box = GTK_HBOX(gtk_hbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), FALSE, FALSE, 5);
  widget = dtgtk_reset_label_new(_("Sigma s %"), self, &p->Fsize, sizeof(float));
  gtk_box_pack_start(GTK_BOX(box), widget, TRUE, TRUE, 0);
  g->Fsize = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0,3.0, 0.1, p->Fsize, 3));
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->Fsize), TRUE, TRUE, 0);
  g_signal_connect (G_OBJECT (g->Fsize), "value-changed",G_CALLBACK (Fsize_callback), self);

}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}


