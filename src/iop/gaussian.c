/*
		This file is part of darktable,
		copyright (c) 2011 ulrich pegelow.

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


#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
#define LCLIP(x) ((x<0)?0.0:(x>100.0)?100.0:x)
#define CLAMP_RANGE(x,y,z) (CLAMP(x,y,z))
#define GORDER 0

DT_MODULE(1)

typedef struct dt_iop_gaussian_t
{
  unsigned int gorder;
  float radius;
  float contrast;
  float saturation;
}
dt_iop_gaussian_params_t;

typedef struct dt_iop_gaussian_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2, vbox3;
  GtkWidget  *label1,*label2, label3;		     // radius, contrast, saturation
  GtkDarktableSlider *scale1,*scale2,*scale3;       // radius, contrast, saturation
}
dt_iop_gaussian_gui_data_t;

typedef struct dt_iop_gaussian_data_t
{
  unsigned int gorder;
  float radius;
  float contrast;
  float saturation;
}
dt_iop_gaussian_data_t;

typedef struct dt_iop_gaussian_global_data_t
{
  int kernel_gaussian_column;
  int kernel_gaussian_row;
  int kernel_gaussian_mix;
}
dt_iop_gaussian_global_data_t;


const char *name()
{
  return _("gaussian blur");
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


static 
void compute_gauss_params(const float sigma, unsigned int gorder, float *a0, float *a1, float *a2, float *a3, 
                          float *b1, float *b2, float *coefp, float *coefn)
{
  const float alpha = 1.695f / sigma;
  const float ema = exp(-alpha);
  const float ema2 = exp(-2.0f * alpha);
  *b1 = -2.0f * ema;
  *b2 = ema2;
  *a0 = 0.0f;
  *a1 = 0.0f;
  *a2 = 0.0f;
  *a3 = 0.0f;
  *coefp = 0.0f;
  *coefn = 0.0f;

  switch(gorder)
  {
    default:
    case 0:
    {
      const float k = (1.0f - ema)*(1.0f - ema)/(1.0f + (2.0f * alpha * ema) - ema2);
      *a0 = k;
      *a1 = k * (alpha - 1.0f) * ema;
      *a2 = k * (alpha + 1.0f) * ema;
      *a3 = -k * ema2;
    }
    break;

    case 1:
    {
      *a0 = (1.0f - ema)*(1.0f - ema);
      *a1 = 0.0f;
      *a2 = -*a0;
      *a3 = 0.0f;
    }
    break;

    case 2:
    {
      const float k = -(ema2 - 1.0f) / (2.0f * alpha * ema);
      float kn = -2.0f * (-1.0f + (3.0f * ema) - (3.0f * ema * ema) + (ema * ema * ema));
      kn /= ((3.0f * ema) + 1.0f + (3.0f * ema * ema) + (ema * ema * ema));
      *a0 = kn;
      *a1 = -kn * (1.0f + (k * alpha)) * ema;
      *a2 = kn * (1.0f - (k * alpha)) * ema;
      *a3 = -kn * ema2;
    }
  }

  *coefp = (*a0 + *a1)/(1.0f + *b1 + *b2);
  *coefn = (*a2 + *a3)/(1.0f + *b1 + *b2);
}


#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_gaussian_data_t *d = (dt_iop_gaussian_data_t *)piece->data;
  dt_iop_gaussian_global_data_t *gd = (dt_iop_gaussian_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  size_t width = roi_in->width;
  size_t height = roi_in->height;
  size_t bpp = self->output_bpp(self, piece->pipe, piece);

  size_t origin[] = {0, 0, 0};
  size_t region[] = {width, height, 1};


  /* get intermediate vector buffer with read-write access */
  cl_mem dev_temp = dt_opencl_alloc_device_buffer(width*height*bpp, devid);
  if(dev_temp == NULL) goto error;

  float sigma = fmax(0.0f,d->radius * roi_in->scale / piece ->iscale);
  float contrast = d->contrast;
  float saturation = d->saturation;

  if(sigma < 0.1f)
  {
    // do not blur for small values of sigma; just copy input -> dev_temp
    err = dt_opencl_enqueue_copy_image_to_buffer(darktable.opencl->dev[devid].cmd_queue, dev_in, dev_temp, origin, region, 0, 0, NULL, NULL);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {  
    size_t sizes[3];

    // compute gaussian parameters
    float a0, a1, a2, a3, b1, b2, coefp, coefn;
    compute_gauss_params(sigma, d->gorder, &a0, &a1, &a2, &a3, &b1, &b2, &coefp, &coefn);

    /* first blur step: column by column with dev_in -> dev_temp */
    sizes[0] = width;
    sizes[1] = 1;
    sizes[2] = 1;
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_column, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_column, 1, sizeof(cl_mem), (void *)&dev_temp);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_column, 2, sizeof(size_t), (void *)&width);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_column, 3, sizeof(size_t), (void *)&height);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_column, 4, sizeof(float), (void *)&a0);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_column, 5, sizeof(float), (void *)&a1);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_column, 6, sizeof(float), (void *)&a2);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_column, 7, sizeof(float), (void *)&a3);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_column, 8, sizeof(float), (void *)&b1);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_column, 9, sizeof(float), (void *)&b2);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_column, 10, sizeof(float), (void *)&coefp);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_column, 11, sizeof(float), (void *)&coefn);
    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_gaussian_column, sizes);
    if(err != CL_SUCCESS) goto error;

    // copy intermediate result from dev_temp -> dev_out
    err = dt_opencl_enqueue_copy_buffer_to_image(darktable.opencl->dev[devid].cmd_queue, dev_temp, dev_out, 0, origin, region, 0, NULL, NULL);
    if(err != CL_SUCCESS) goto error;

    /* second blur step: row by row with dev_out -> dev_temp */
    sizes[0] = 1;
    sizes[1] = height;
    sizes[2] = 1;
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_row, 0, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_row, 1, sizeof(cl_mem), (void *)&dev_temp);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_row, 2, sizeof(size_t), (void *)&width);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_row, 3, sizeof(size_t), (void *)&height);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_row, 4, sizeof(float), (void *)&a0);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_row, 5, sizeof(float), (void *)&a1);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_row, 6, sizeof(float), (void *)&a2);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_row, 7, sizeof(float), (void *)&a3);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_row, 8, sizeof(float), (void *)&b1);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_row, 9, sizeof(float), (void *)&b2);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_row, 10, sizeof(float), (void *)&coefp);
    dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_row, 11, sizeof(float), (void *)&coefn);
    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_gaussian_row, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  /* final mixing step dev_temp -> dev_out */
  size_t sizes[] = { width, height, 1 };
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_mix, 0, sizeof(cl_mem), (void *)&dev_temp);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_mix, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_mix, 2, sizeof(size_t), (void *)&width);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_mix, 3, sizeof(float), (void *)&contrast);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_gaussian_mix, 4, sizeof(float), (void *)&saturation);
  err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_gaussian_mix, sizes);
  if(err != CL_SUCCESS) goto error;

  clReleaseMemObject(dev_temp);
  return TRUE;

error:
  if (dev_temp != NULL) dt_opencl_release_mem_object(dev_temp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_gaussian] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif



void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_gaussian_data_t *data = (dt_iop_gaussian_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  float a0, a1, a2, a3, b1, b2, coefp, coefn;

  float sigma = fmax(0.0f,data->radius * roi_in->scale / piece ->iscale);

  // no gaussian blur for very small sigma
  if (sigma < 0.1f)
  {
    for(int k=0; k<roi_out->width*roi_out->height; k++)
    {
      out[k*ch+0] = CLAMP(in[k*ch+0]*data->contrast + 50.0f * (1.0f - data->contrast), 0.0f, 100.0f);
      out[k*ch+1] = CLAMP(in[k*ch+1]*data->saturation, -128.0f, 128.0f);
      out[k*ch+2] = CLAMP(in[k*ch+2]*data->saturation, -128.0f, 128.0f);
      out[k*ch+3] = in[k*ch+3];
    }
    return;
  }

  // as the function name implies
  compute_gauss_params(sigma, data->gorder, &a0, &a1, &a2, &a3, &b1, &b2, &coefp, &coefn);



  float *temp = malloc(roi_out->width*roi_out->height*ch*sizeof(float));
  if(temp==NULL) return;

  // vertical blur column by column
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(in,out,temp,roi_out,data,a0,a1,a2,a3,b1,b2,coefp,coefn) schedule(static)
#endif
  for(int i=0; i<roi_out->width; i++)
  {
    float xp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yb[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float xc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float xn[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float xa[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yn[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float ya[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // forward filter
    for(int k=0; k < 4; k++)
    {
      xp[k] = in[i*ch+k];
      yb[k] = xp[k] * coefp;
      yp[k] = yb[k];
    }
 
    for(int j=0; j<roi_out->height; j++)
    {
      int offset = (i + j * roi_out->width)*ch;

      for(int k=0; k < 4; k++)
      {
        xc[k] = in[offset+k];
        yc[k] = (a0 * xc[k]) + (a1 * xp[k]) - (b1 * yp[k]) - (b2 * yb[k]);

        temp[offset+k] = yc[k];

        xp[k] = xc[k];
        yb[k] = yp[k];
        yp[k] = yc[k];
      }
    }

    // backward filter
    for(int k=0; k < 4; k++)
    {
      xn[k] = in[((roi_out->height - 1) * roi_out->width + i)*ch+k];
      xa[k] = xn[k];
      yn[k] = xn[k] * coefn;
      ya[k] = yn[k];
    }

    for(int j=roi_out->height - 1; j > -1; j--)
    {
      int offset = (i + j * roi_out->width)*ch;

      for(int k=0; k < 4; k++)
      {      
        xc[k] = in[offset+k];

        yc[k] = (a2 * xn[k]) + (a3 * xa[k]) - (b1 * yn[k]) - (b2 * ya[k]);

        xa[k] = xn[k]; 
        xn[k] = xc[k]; 
        ya[k] = yn[k]; 
        yn[k] = yc[k];

        temp[offset+k] += yc[k];
      }
    }
  }

  // horizontal blur line by line
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out,temp,roi_out,data,a0,a1,a2,a3,b1,b2,coefp,coefn) schedule(static)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    float xp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yb[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float xc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float xn[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float xa[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yn[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float ya[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // forward filter
    for(int k=0; k < 4; k++)
    {
      xp[k] = temp[j*roi_out->width*ch+k];
      yb[k] = xp[k] * coefp;
      yp[k] = yb[k];
    }
 
    for(int i=0; i<roi_out->width; i++)
    {
      int offset = (i + j * roi_out->width)*ch;

      for(int k=0; k < 4; k++)
      {
        xc[k] = temp[offset+k];
        yc[k] = (a0 * xc[k]) + (a1 * xp[k]) - (b1 * yp[k]) - (b2 * yb[k]);

        out[offset+k] = yc[k];

        xp[k] = xc[k];
        yb[k] = yp[k];
        yp[k] = yc[k];
      }
    }

    // backward filter
    for(int k=0; k < 4; k++)
    {
      xn[k] = temp[((j + 1)*roi_out->width - 1)*ch + k];
      xa[k] = xn[k];
      yn[k] = xn[k] * coefn;
      ya[k] = yn[k];
    }

    for(int i=roi_out->width - 1; i > -1; i--)
    {
      int offset = (i + j * roi_out->width)*ch;

      for(int k=0; k < 4; k++)
      {      
        xc[k] = temp[offset+k];

        yc[k] = (a2 * xn[k]) + (a3 * xa[k]) - (b1 * yn[k]) - (b2 * ya[k]);

        xa[k] = xn[k]; 
        xn[k] = xc[k]; 
        ya[k] = yn[k]; 
        yn[k] = yc[k];

        out[offset+k] += yc[k];
      }
    }
  }


  free(temp);

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out,data,roi_out) schedule(static)
#endif
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    out[k*ch+0] = CLAMP(out[k*ch+0]*data->contrast + 50.0f * (1.0f - data->contrast), 0.0f, 100.0f);
    out[k*ch+1] = CLAMP(out[k*ch+1]*data->saturation, -128.0f, 128.0f);
    out[k*ch+2] = CLAMP(out[k*ch+2]*data->saturation, -128.0f, 128.0f);
  }
}


static void
radius_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_gaussian_params_t *p = (dt_iop_gaussian_params_t *)self->params;
  p->radius= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
contrast_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_gaussian_params_t *p = (dt_iop_gaussian_params_t *)self->params;
  p->contrast = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
saturation_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_gaussian_params_t *p = (dt_iop_gaussian_params_t *)self->params;
  p->saturation = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_gaussian_params_t *p = (dt_iop_gaussian_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[highpass] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_gaussian_data_t *d = (dt_iop_gaussian_data_t *)piece->data;
  d->gorder = p->gorder;
  d->radius = p->radius;
  d->contrast = p->contrast;
  d->saturation = p->saturation;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_gaussian_data_t));
  memset(piece->data,0,sizeof(dt_iop_gaussian_data_t));
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
  dt_iop_gaussian_gui_data_t *g = (dt_iop_gaussian_gui_data_t *)self->gui_data;
  dt_iop_gaussian_params_t *p = (dt_iop_gaussian_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->radius);
  dtgtk_slider_set_value(g->scale2, p->contrast);
  dtgtk_slider_set_value(g->scale3, p->saturation);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_gaussian_params_t));
  module->default_params = malloc(sizeof(dt_iop_gaussian_params_t));
  module->default_enabled = 0;
  module->priority = 714;
  module->params_size = sizeof(dt_iop_gaussian_params_t);
  module->gui_data = NULL;
  dt_iop_gaussian_params_t tmp = (dt_iop_gaussian_params_t)
  {
    0, 25, 1, 1
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_gaussian_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_gaussian_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 6; // gaussian.cl, from programs.conf
  dt_iop_gaussian_global_data_t *gd = (dt_iop_gaussian_global_data_t *)malloc(sizeof(dt_iop_gaussian_global_data_t));
  module->data = gd;
  gd->kernel_gaussian_column = dt_opencl_create_kernel(darktable.opencl, program, "gaussian_column");
  gd->kernel_gaussian_row = dt_opencl_create_kernel(darktable.opencl, program, "gaussian_row");
  gd->kernel_gaussian_mix = dt_opencl_create_kernel(darktable.opencl, program, "gaussian_mix");
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
  dt_iop_gaussian_global_data_t *gd = (dt_iop_gaussian_global_data_t *)module->data;
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_gaussian_column);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_gaussian_row);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_gaussian_mix);
  free(module->data);
  module->data = NULL;
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_gaussian_gui_data_t));
  dt_iop_gaussian_gui_data_t *g = (dt_iop_gaussian_gui_data_t *)self->gui_data;
  dt_iop_gaussian_params_t *p = (dt_iop_gaussian_params_t *)self->params;

  self->widget = gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 200.0, 0.1, p->radius, 2));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-1.0, 1.0, 0.01, p->contrast, 2));
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-3.0, 3.0, 0.01, p->saturation, 2));
  dtgtk_slider_set_label(g->scale1,_("radius"));
  dtgtk_slider_set_label(g->scale2,_("contrast"));
  dtgtk_slider_set_label(g->scale3,_("saturation"));


  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the radius of gaussian blur filter"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("the contrast of gaussian blur filter"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale3), "tooltip-text", _("the color saturation of gaussian blur filter"), (char *)NULL);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (radius_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (contrast_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (saturation_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}
