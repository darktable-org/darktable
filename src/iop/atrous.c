/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include "develop/imageop.h"
#include "common/opencl.h"
#include "gui/draw.h"
#include <memory.h>
#include <stdlib.h>

#define DT_GUI_EQUALIZER_INSET 5
#define DT_GUI_CURVE_INFL .3f

DT_MODULE(1)

typedef struct dt_iop_atrous_params_t
{
  int32_t octaves; // max is 7 -> 5*2^7 = 640px support
  // noise threshold for each band
  float thrs[7];
  // contrast enhancement for each band
  float boost[7];
  // how sharp do you want the edges of the wavelets for each band
  float sharpen[7];
}
dt_iop_atrous_params_t;

typedef struct dt_iop_atrous_gui_data_t
{
#if 0 // draw curve gui? use sliders?
  dt_draw_curve_t *minmax_curve;
  GtkDrawingArea *area;
  double draw_xs[DT_IOP_EQUALIZER_RES], draw_ys[DT_IOP_EQUALIZER_RES];
  double draw_min_xs[DT_IOP_EQUALIZER_RES], draw_min_ys[DT_IOP_EQUALIZER_RES];
  double draw_max_xs[DT_IOP_EQUALIZER_RES], draw_max_ys[DT_IOP_EQUALIZER_RES];
  // have histogram in bg?
  float band_hist[DT_IOP_EQUALIZER_BANDS];
  float band_max;
#endif
}
dt_iop_atrous_gui_data_t;

typedef struct dt_iop_atrous_global_data_t
{
  int kernel_decompose;
  int kernel_synthesize;
}
dt_iop_atrous_global_data_t;

typedef struct dt_iop_atrous_data_t
{
  // demosaic pattern
  int32_t octaves;
  float thrs[7];
  float boost[7];
  float sharpen[7];
}
dt_iop_atrous_data_t;

const char *
name()
{
  return _("equalizer II");
}

int 
groups ()
{
  return IOP_GROUP_CORRECT;
}


void
process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem *i, cl_mem *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // TODO:
}

#ifdef HAVE_OPENCL
void
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
#if 0
  if(piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    memcpy(o, i, sizeof(float)*3*roi_in->width*roi_in->height);
    return;
  }
#endif
  dt_iop_atrous_data_t *data = (dt_iop_atrous_data_t *)piece->data;
  dt_iop_atrous_global_data_t *gd = (dt_iop_atrous_global_data_t *)self->data;
  // global scale is roi scale and pipe input prescale
  const float global_scale = roi_in->scale / piece->iscale;
  float *in  = (float *)i;
  float *out = (float *)o;


  const int devid = piece->pipe->devid;
  cl_int err;
  size_t sizes[3];
  err = dt_opencl_get_max_work_item_sizes(darktable.opencl, devid, sizes);
  if(err != CL_SUCCESS) fprintf(stderr, "could not get max size! %d\n", err);
  // printf("max work item sizes = %lu %lu %lu\n", sizes[0], sizes[1], sizes[2]);

  // allocate device memory
  cl_mem dev_in, dev_coarse;
  // as images (texture memory)
  cl_image_format fmt = {CL_RGBA, CL_FLOAT};
  dev_in = clCreateImage2D (darktable.opencl->dev[devid].context,
      CL_MEM_READ_WRITE,
      &fmt,
      sizes[0], sizes[1], 0,
      NULL, &err);
  if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy img buffer on device: %d\n", err);
  dev_coarse = clCreateImage2D (darktable.opencl->dev[devid].context,
      CL_MEM_READ_WRITE,
      &fmt,
      sizes[0], sizes[1], 0,
      NULL, &err);
  if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy coarse buffer on device: %d\n", err);


  const int max_scale = MIN(7, data->octaves);
  const int max_filter_radius = global_scale*(1<<max_scale); // 2 * 2^max_scale
  const int tile_wd = sizes[0] - 2*max_filter_radius, tile_ht = sizes[1] - 2*max_filter_radius;

  // details for local contrast enhancement:
  cl_mem dev_detail[max_scale];
  for(int k=0;k<max_scale;k++)
  {
    dev_detail[k] = clCreateImage2D (darktable.opencl->dev[devid].context,
        CL_MEM_READ_WRITE,
        &fmt,
        sizes[0], sizes[1], 0,
        NULL, &err);
    if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy detail buffer on device: %d\n", err);
  }

  const int width = roi_out->width, height = roi_out->height;

  // float *in4 = (float *)malloc(4*sizeof(float)*sizes[0]*sizes[1]);

  // for all tiles:
  for(int tx=0;tx<ceilf(width/(float)tile_wd);tx++)
  for(int ty=0;ty<ceilf(height/(float)tile_ht);ty++)
    // const int tx=0,ty=0;
  {
    size_t orig0[3] = {0, 0, 0};
    size_t origin[3] = {tx*tile_wd, ty*tile_ht, 0};
    size_t wd = origin[0] + sizes[0] > width  ? width  - origin[0] : sizes[0];
    size_t ht = origin[1] + sizes[1] > height ? height - origin[1] : sizes[1];
    size_t region[3] = {wd, ht, 1};
    // printf("origin %lu %lu and size: %lu %lu\n", origin[0], origin[1], wd, ht);

    // TODO: one more buffer and interleaved write/process
    // for(int j=0;j<ht;j++) for(int i=0;i<wd;i++) for(int c=0;c<3;c++) in4[4*(sizes[0]*j+i)+c] = in[3*((origin[1]+j)*width + (origin[0]+i))+c];
    // clEnqueueWriteImage(darktable.opencl->dev[devid].cmd_queue, dev_in, CL_FALSE, orig0, region, 4*sizes[0]*sizeof(float), 0, in4, 0, NULL, NULL);
    clEnqueueWriteImage(darktable.opencl->dev[devid].cmd_queue, dev_in, CL_FALSE, orig0, region, 4*width*sizeof(float), 0, in + 4*(width*origin[1] + origin[0]), 0, NULL, NULL);
    if(tx > 0) { origin[0] += max_filter_radius; orig0[0] += max_filter_radius; region[0] -= max_filter_radius; }
    if(ty > 0) { origin[1] += max_filter_radius; orig0[1] += max_filter_radius; region[1] -= max_filter_radius; }
    // total number of work-items (threads*blocks):
    // size_t global[] = {local[0]*((512+local[0])/local[0]), local[1]*((512+local[1])/local[1])};//64*((height+64)/64)};//{width, height};

    for(int s=0;s<max_scale;s++)
    {
      const int scale = global_scale * s;
      // FIXME: don't do 0x0 filtering!
      // if((int)(global_scale * (s+1)) == 0) continue;
      // expected noise level on this scale:
      const float sharp = data->sharpen[s];
      err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 2, sizeof(cl_mem), (void *)&dev_detail[s]);
      if(s & 1)
      {
        err |= dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 0, sizeof(cl_mem), (void *)&dev_coarse);
        err |= dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 1, sizeof(cl_mem), (void *)&dev_in);
      }
      else
      {
        err |= dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 1, sizeof(cl_mem), (void *)&dev_coarse);
        err |= dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 0, sizeof(cl_mem), (void *)&dev_in);
      }
      err |= dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 3, sizeof(unsigned int), (void *)&scale);
      err |= dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 4, sizeof(unsigned int), (void *)&sharp);
      if(err != CL_SUCCESS) fprintf(stderr, "param setting failed! %d\n", err);

      // printf("equeueing kernel with %lu %lu threads\n", local[0], global[0]);
      err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_decompose, sizes);
      if(err != CL_SUCCESS) fprintf(stderr, "enqueueing failed! %d\n", err);
      // clFinish(darktable.opencl->cmd_queue);
    }

    // clFinish(darktable.opencl->cmd_queue);
    // now synthesize again:
    for(int scale=max_scale-1;scale>=0;scale--)
    {
      const float boost = data->boost[scale];
      const float thrs  = data->thrs [scale];
      err  = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize, 2, sizeof(cl_mem), (void *)&dev_detail[scale]);
      err |= dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize, 3, sizeof(float), (void *)&thrs);
      err |= dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize, 4, sizeof(float), (void *)&boost);
      if(scale & 1)
      {
        err |= dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize, 0, sizeof(cl_mem), (void *)&dev_coarse);
        err |= dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize, 1, sizeof(cl_mem), (void *)&dev_in);
      }
      else
      {
        err |= dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize, 1, sizeof(cl_mem), (void *)&dev_coarse);
        err |= dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize, 0, sizeof(cl_mem), (void *)&dev_in);
      }
      if(err != CL_SUCCESS) fprintf(stderr, "param setting failed! %d\n", err);

      err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_synthesize, sizes);
      if(err != CL_SUCCESS) fprintf(stderr, "enqueueing failed! %d\n", err);
      // clFinish(darktable.opencl->cmd_queue);
      // if((int)(global_scale * scale) == 0) break;
    }
    clEnqueueReadImage(darktable.opencl->dev[devid].cmd_queue, dev_in, CL_FALSE, orig0, region, 4*width*sizeof(float), 0, out + 4*(width*origin[1] + origin[0]), 0, NULL, NULL);
  }

  // wait for async read
  clFinish(darktable.opencl->dev[devid].cmd_queue);

  // free(in4);

  // write output images:
  // for(int k=0;k<width*height;k++) for(int c=0;c<3;c++) out[3*k+c] = out[4*k+c];

  // free device mem
  clReleaseMemObject(dev_in);
  clReleaseMemObject(dev_coarse);
  for(int k=0;k<max_scale;k++) clReleaseMemObject(dev_detail[k]);
}
#endif

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_atrous_params_t));
  module->default_params = malloc(sizeof(dt_iop_atrous_params_t));
  module->default_enabled = 0;
  module->priority = 370;
  module->params_size = sizeof(dt_iop_atrous_params_t);
  module->gui_data = NULL;
  dt_iop_atrous_params_t tmp;
  tmp.octaves = 5;
  for(int k=0;k<7;k++)
  {
    tmp.sharpen[k] = .03f;
    tmp.boost  [k] = 0.0f;
    tmp.thrs   [k] = 0.001f;
  }
  tmp.sharpen[0] = 0.0f;
  memcpy(module->params, &tmp, sizeof(dt_iop_atrous_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_atrous_params_t));

  const int program = 1; // from programs.conf
  dt_iop_atrous_global_data_t *gd = (dt_iop_atrous_global_data_t *)malloc(sizeof(dt_iop_atrous_global_data_t));
  module->data = gd;
  gd->kernel_decompose  = dt_opencl_create_kernel(darktable.opencl, program, "eaw_decompose");
  gd->kernel_synthesize = dt_opencl_create_kernel(darktable.opencl, program, "eaw_synthesize");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
  dt_iop_atrous_global_data_t *gd = (dt_iop_atrous_global_data_t *)module->data;
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_decompose);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_synthesize);
  free(module->data);
  module->data = NULL;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_atrous_params_t *p = (dt_iop_atrous_params_t *)params;
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)piece->data;
  d->octaves = p->octaves;
  for(int k=0;k<7;k++)
  {
    d->thrs[k] = p->thrs[k];
    d->boost[k] = p->boost[k];
    d->sharpen[k] = p->sharpen[k];
  }
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_atrous_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update   (struct dt_iop_module_t *self)
{
  // nothing
  gtk_widget_queue_draw(self->widget);
}

void gui_init     (struct dt_iop_module_t *self)
{
  self->widget = gtk_label_new(_("this module doesn't have any options"));
}

void gui_cleanup  (struct dt_iop_module_t *self)
{
  // nothing
}

