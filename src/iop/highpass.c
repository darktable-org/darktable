/*
		This file is part of darktable,
		copyright (c) 2011 Henrik Andersson.

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
#include "common/opencl.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define MAX_RADIUS  16

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
#define LCLIP(x) ((x<0)?0.0:(x>100.0)?100.0:x)
DT_MODULE(1)

typedef struct dt_iop_highpass_params_t
{
  float sharpness;
  float contrast;
}
dt_iop_highpass_params_t;

typedef struct dt_iop_highpass_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkWidget  *label1,*label2;			// sharpness,contrast
  GtkDarktableSlider *scale1,*scale2;       // sharpness,contrast
}
dt_iop_highpass_gui_data_t;

typedef struct dt_iop_highpass_data_t
{
  float sharpness;
  float contrast;
}
dt_iop_highpass_data_t;

typedef struct dt_iop_highpass_global_data_t
{
  int kernel_highpass;
}
dt_iop_highpass_global_data_t;


const char *name()
{
  return _("highpass");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int
groups ()
{
  return IOP_GROUP_EFFECT;
}

#ifdef HAVE_OPENCL
void
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_highpass_data_t *d = (dt_iop_highpass_data_t *)piece->data;
  dt_iop_highpass_global_data_t *gd = (dt_iop_highpass_global_data_t *)self->data;

  cl_int err;
  const int devid = piece->pipe->devid;

  float rad = MAX_RADIUS*(fmin(100.0f,d->sharpness+1)/100.0f);
  const int radius = MIN(MAX_RADIUS, ceilf(rad * roi_in->scale / piece->iscale));
  
  // printf("highpass opencl radius: %d, rad: %f\n", radius, rad);

  float contrast_scale = ((d->contrast/100.0f)*7.5f);

  if(fabs(rad) < 0.0001f)
  {
    size_t origin[] = {0, 0, 0};
    size_t region[] = {roi_in->width, roi_in->height, 1};
    err = clEnqueueCopyImage(darktable.opencl->dev[devid].cmd_queue, dev_in, dev_out, origin, origin, region, 0, NULL, NULL);
    return;
  }
  float mat[2*(MAX_RADIUS+1)];
  const int wd = 2*radius+1;
  float *m = mat + radius;
  const float sigma2 = (2.5f*2.5f)*(rad*roi_in->scale/piece->iscale)*(rad*roi_in->scale/piece->iscale);
  float weight = 0.0f;
  // init gaussian kernel
  for(int l=-radius; l<=radius; l++) weight += m[l] = expf(- (l*l)/(2.f*sigma2));
  for(int l=-radius; l<=radius; l++) m[l] /= weight;
  size_t sizes[] = {roi_in->width, roi_in->height, 1};
  cl_mem dev_m = dt_opencl_copy_host_to_device_constant(sizeof(float)*wd, devid, mat);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_highpass, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_highpass, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_highpass, 2, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_highpass, 3, sizeof(int), (void *)&radius);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_highpass, 4, sizeof(float), (void *)&contrast_scale);
  err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_highpass, sizes);
  if(err != CL_SUCCESS) fprintf(stderr, "couldn't enqueue highpass kernel! %d\n", err);
  clReleaseMemObject(dev_m);
}
#endif


void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_highpass_data_t *data = (dt_iop_highpass_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

  /* create inverted image and then blur */
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(in,out,roi_out) schedule(static)
#endif
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    int index = ch*k;
    out[index] =    1.0-CLIP(in[index]);
    out[index+1] =  1.0-CLIP(in[index+1]);
    out[index+2] =  1.0-CLIP(in[index+2]);
  }



  int rad = MAX_RADIUS*(fmin(100.0,data->sharpness+1)/100.0);
  const int radius = MIN(MAX_RADIUS, ceilf(rad * roi_in->scale / piece->iscale));

  // printf("highpass non-opencl radius: %d, rad: %d\n", radius, rad);

  /* horizontal blur out into out */
  const int range = 2*radius+1;
  const int hr = range/2;

  const int size = roi_out->width>roi_out->height?roi_out->width:roi_out->height;
  float *scanline[3]= {0};
  scanline[0]  = malloc((size*sizeof(float))*ch);
  scanline[1]  = malloc((size*sizeof(float))*ch);
  scanline[2]  = malloc((size*sizeof(float))*ch);

  for(int iteration=0; iteration<8; iteration++)
  {
    int index=0;
    for(int y=0; y<roi_out->height; y++)
    {
      for(int k=0; k<3; k++)
      {
        float L=0;
        int hits = 0;
        for(int x=-hr; x<roi_out->width; x++)
        {
          int op = x - hr-1;
          int np = x+hr;
          if(op>=0)
          {
            L-=out[(index+op)*ch+k];
            hits--;
          }
          if(np < roi_out->width)
          {
            L+=out[(index+np)*ch+k];
            hits++;
          }
          if(x>=0)
            scanline[k][x] = L/hits;
        }
      }

      for (int k=0; k<3; k++)
        for (int x=0; x<roi_out->width; x++)
          out[(index+x)*ch+k] = scanline[k][x];

      index+=roi_out->width;
    }

    /* vertical pass on blurlightness */
    const int opoffs = -(hr+1)*roi_out->width;
    const int npoffs = (hr)*roi_out->width;
    for(int x=0; x < roi_out->width; x++)
    {
      for(int k=0; k<3; k++)
      {
        float L=0;
        int hits=0;
        int index = -hr*roi_out->width+x;
        for(int y=-hr; y<roi_out->height; y++)
        {
          int op=y-hr-1;
          int np= y + hr;

          if(op>=0)
          {
            L-=out[(index+opoffs)*ch+k];
            hits--;
          }
          if(np < roi_out->height)
          {
            L+=out[(index+npoffs)*ch+k];
            hits++;
          }
          if(y>=0)
            scanline[k][y] = L/hits;
          index += roi_out->width;
        }
      }

      for(int k=0; k<3; k++)
        for (int y=0; y<roi_out->height; y++)
          out[(y*roi_out->width+x)*ch+k] = scanline[k][y];

    }
  }

  const float contrast_scale=((data->contrast/100.0)*7.5);
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out, data) schedule(static)
#endif
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    int index = ch*k;
    // Mix out and in
    out[index+0] = out[index+0]*0.5 + in[index+0]*0.5;
    out[index+1] = out[index+1]*0.5 + in[index+1]*0.5;
    out[index+2] = out[index+2]*0.5 + in[index+2]*0.5;
    // calculate the average
    const float average=(out[index+0]+out[index+1]+out[index+2])/3.0;
    out[index+0] = out[index+1] = out[index+2] = CLIP(0.5+((average-0.5)*contrast_scale));

  }

}

static void
sharpness_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;
  p->sharpness= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
contrast_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;
  p->contrast = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[highpass] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_highpass_data_t *d = (dt_iop_highpass_data_t *)piece->data;
  d->sharpness= p->sharpness;
  d->contrast = p->contrast;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_highpass_data_t));
  memset(piece->data,0,sizeof(dt_iop_highpass_data_t));
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
  dt_iop_highpass_gui_data_t *g = (dt_iop_highpass_gui_data_t *)self->gui_data;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->sharpness);
  dtgtk_slider_set_value(g->scale2, p->contrast);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_highpass_params_t));
  module->default_params = malloc(sizeof(dt_iop_highpass_params_t));
  module->default_enabled = 0;
  module->priority = 994;
  module->params_size = sizeof(dt_iop_highpass_params_t);
  module->gui_data = NULL;
  dt_iop_highpass_params_t tmp = (dt_iop_highpass_params_t)
  {
    50,50
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_highpass_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_highpass_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 4; // highpass.cl, from programs.conf
  dt_iop_highpass_global_data_t *gd = (dt_iop_highpass_global_data_t *)malloc(sizeof(dt_iop_highpass_global_data_t));
  module->data = gd;
  gd->kernel_highpass = dt_opencl_create_kernel(darktable.opencl, program, "highpass");
}


void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_highpass_global_data_t *gd = (dt_iop_highpass_global_data_t *)module->data;
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_highpass);
  free(module->data);
  module->data = NULL;
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_highpass_gui_data_t));
  dt_iop_highpass_gui_data_t *g = (dt_iop_highpass_gui_data_t *)self->gui_data;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;

  self->widget = gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->sharpness, 2));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->contrast, 2));
  dtgtk_slider_set_label(g->scale1,_("sharpness"));
  dtgtk_slider_set_unit(g->scale1,"%");
  dtgtk_slider_set_label(g->scale2,_("contrast boost"));
  dtgtk_slider_set_unit(g->scale2,"%");

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the sharpness of highpass filter"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("the contrast of highpass filter"), (char *)NULL);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (sharpness_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (contrast_callback), self);

}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}
