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
#include <memory.h>
#include <stdlib.h>

DT_MODULE(1)

typedef struct dt_iop_demosaic_params_t
{
  // TODO: hot pixels removal/denoise/green eq/whatever
  int32_t flags;
}
dt_iop_demosaic_params_t;

typedef struct dt_iop_demosaic_gui_data_t
{
}
dt_iop_demosaic_gui_data_t;

typedef struct dt_iop_demosaic_global_data_t
{
  // demosaic pattern
  int kernel_ppg_green;
  int kernel_ppg_redblue;
  int kernel_zoom_half_size;
  int kernel_downsample;
}
dt_iop_demosaic_global_data_t;

typedef struct dt_iop_demosaic_data_t
{
  // demosaic pattern
  uint32_t filters;
}
dt_iop_demosaic_data_t;

const char *
name()
{
  return _("demosaic");
}

int 
groups ()
{
  return IOP_GROUP_BASIC;
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
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->data;
  // global scale is roi scale and pipe input prescale
  const float global_scale = roi_in->scale / piece->iscale;
  // const uint16_t *in = (const uint16_t *)i;
  float *out = (float *)o;

  const int devid = piece->pipe->devid;
  size_t sizes[2] = {roi_out->width, roi_out->height};
  size_t origin[] = {0, 0, 0};
  // TODO: input image region!
  size_t region[] = {self->dev->image->width, self->dev->image->height, 1};
  size_t region_out[] = {roi_out->width, roi_out->height, 1};
  cl_int err;
  cl_mem dev_in, dev_out, dev_tmp = NULL;
  // as images (texture memory)
  cl_image_format fmt1 = {CL_LUMINANCE, CL_UNSIGNED_INT16};
  cl_image_format fmt4 = {CL_RGBA, CL_FLOAT};
  dev_out = clCreateImage2D (darktable.opencl->dev[devid].context,
      CL_MEM_READ_WRITE,
      &fmt4,
      sizes[0], sizes[1], 0,
      NULL, &err);
  if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy out buffer on device: %d\n", err);
  

  if(global_scale > .999f)
  {
    // 1:1 demosaic
    size_t origin_in[] = {((size_t)roi_out->x)&~0x1, ((size_t)roi_out->y)&~0x1, 0};
    dev_in = clCreateImage2D (darktable.opencl->dev[devid].context,
        CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
        &fmt1,
        region_out[0], region_out[1], sizeof(uint16_t)*region[0],
        ((uint16_t *)self->dev->image->pixels) + origin_in[1]*region[0] + origin_in[0], &err);
    if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy img buffer on device: %d\n", err);
    // clEnqueueWriteImage(darktable.opencl->cmd_queue, dev_in, CL_FALSE, origin_in, region_out, region[0]*sizeof(uint16_t), 0, ((uint16_t *)self->dev->image->pixels) + origin_in[1]*region[0] + origin_in[0], 0, NULL, NULL);
    // demosaic!
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 0, sizeof(cl_mem), &dev_in);
    if(err != CL_SUCCESS) fprintf(stderr, "param 1 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 1, sizeof(cl_mem), &dev_out);
    if(err != CL_SUCCESS) fprintf(stderr, "param 2 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 2, sizeof(uint32_t), (void*)&data->filters);
    if(err != CL_SUCCESS) fprintf(stderr, "param 3 setting failed: %d\n", err);
    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_green, sizes);

    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 0, sizeof(cl_mem), &dev_out);
    if(err != CL_SUCCESS) fprintf(stderr, "param 1 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 1, sizeof(cl_mem), &dev_out);
    if(err != CL_SUCCESS) fprintf(stderr, "param 2 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 2, sizeof(uint32_t), (void*)&data->filters);
    if(err != CL_SUCCESS) fprintf(stderr, "param 3 setting failed: %d\n", err);
    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_redblue, sizes);
  }
  else if(global_scale > .5f)
  {
    // need to scale to right res
    dt_iop_roi_t roi;
    roi.x = ((int)(roi_in->x/global_scale)) & ~0x1; roi.y = ((int)(roi_in->y/global_scale)) & ~0x1;
    roi.width = roi_in->width/global_scale; roi.height = roi_in->height/global_scale;
    dev_in = clCreateImage2D (darktable.opencl->dev[devid].context,
        CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
        &fmt1,
        roi.width, roi.height, sizeof(uint16_t)*region[0],
        ((uint16_t *)self->dev->image->pixels) + roi.y*region[0] + roi.x, &err);
    if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy img buffer on device: %d\n", err);
    dev_tmp = clCreateImage2D (darktable.opencl->dev[devid].context,
        CL_MEM_READ_WRITE,
        &fmt4,
        roi.width, roi.height, 0,
        NULL, &err);
    if(err != CL_SUCCESS) fprintf(stderr, "could not alloc tmp buffer on device: %d\n", err);

    sizes[0] = roi.width; sizes[1] = roi.height;
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 0, sizeof(cl_mem), &dev_in);
    if(err != CL_SUCCESS) fprintf(stderr, "param 1 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 1, sizeof(cl_mem), &dev_tmp);
    if(err != CL_SUCCESS) fprintf(stderr, "param 2 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_green, 2, sizeof(uint32_t), (void*)&data->filters);
    if(err != CL_SUCCESS) fprintf(stderr, "param 3 setting failed: %d\n", err);
    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_green, sizes);

    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 0, sizeof(cl_mem), &dev_tmp);
    if(err != CL_SUCCESS) fprintf(stderr, "param 1 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 1, sizeof(cl_mem), &dev_tmp);
    if(err != CL_SUCCESS) fprintf(stderr, "param 2 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_ppg_redblue, 2, sizeof(uint32_t), (void*)&data->filters);
    if(err != CL_SUCCESS) fprintf(stderr, "param 3 setting failed: %d\n", err);
    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_ppg_redblue, sizes);

    // FIXME: produces weird red cast..??
    // scale temp buffer to output buffer
    int zero = 0;
    sizes[0] = roi_out->width; sizes[1] = roi_out->height;
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 0, sizeof(cl_mem), &dev_tmp);
    if(err != CL_SUCCESS) fprintf(stderr, "param 1 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 1, sizeof(cl_mem), &dev_out);
    if(err != CL_SUCCESS) fprintf(stderr, "param 2 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 2, sizeof(int), (void*)&zero);
    if(err != CL_SUCCESS) fprintf(stderr, "param 3 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 3, sizeof(int), (void*)&zero);
    if(err != CL_SUCCESS) fprintf(stderr, "param 4 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 4, sizeof(int), (void*)&roi_out->width);
    if(err != CL_SUCCESS) fprintf(stderr, "param 5 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 5, sizeof(int), (void*)&roi_out->height);
    if(err != CL_SUCCESS) fprintf(stderr, "param 6 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_downsample, 6, sizeof(float), (void*)&global_scale);
    if(err != CL_SUCCESS) fprintf(stderr, "param 7 setting failed: %d\n", err);
    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_downsample, sizes);
  }
  else
  {
    // sample half-size image:
    dev_in = clCreateImage2D (darktable.opencl->dev[devid].context,
        CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
        &fmt1,
        region[0], region[1], 0,
        self->dev->image->pixels, &err);
    if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy img buffer on device: %d\n", err);
    // clEnqueueWriteImage(darktable.opencl->cmd_queue, dev_in, CL_FALSE, origin, region, region[0]*sizeof(uint16_t), 0, (uint16_t *)self->dev->image->pixels, 0, NULL, NULL);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 0, sizeof(cl_mem), &dev_in);
    if(err != CL_SUCCESS) fprintf(stderr, "param 1 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 1, sizeof(cl_mem), &dev_out);
    if(err != CL_SUCCESS) fprintf(stderr, "param 2 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 2, sizeof(int), (void*)&roi_out->x);
    if(err != CL_SUCCESS) fprintf(stderr, "param 3 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 3, sizeof(int), (void*)&roi_out->y);
    if(err != CL_SUCCESS) fprintf(stderr, "param 4 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 4, sizeof(int), (void*)&roi_out->width);
    if(err != CL_SUCCESS) fprintf(stderr, "param 5 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 5, sizeof(int), (void*)&roi_out->height);
    if(err != CL_SUCCESS) fprintf(stderr, "param 6 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 6, sizeof(float), (void*)&global_scale);
    if(err != CL_SUCCESS) fprintf(stderr, "param 7 setting failed: %d\n", err);
    err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_zoom_half_size, 7, sizeof(uint32_t), (void*)&data->filters);
    if(err != CL_SUCCESS) fprintf(stderr, "param 8 setting failed: %d\n", err);

    err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_zoom_half_size, sizes);
  }

  // double start = dt_get_wtime();
  // clEnqueueReadImage(cmd_queue, dev_out, CL_FALSE, orig0, region, 4*width*sizeof(float), 0, out + 4*(width*origin[1] + origin[0]), 0, NULL, NULL);
  // blocking:
  clEnqueueReadImage(darktable.opencl->dev[devid].cmd_queue, dev_out, CL_TRUE, origin, region_out, 4*region_out[0]*sizeof(float), 0, out, 0, NULL, NULL);
  // double end = dt_get_wtime();
  // dt_print(DT_DEBUG_PERF, "[demosaic] took %.3f secs\n", end - start);

  for(int k=0;k<roi_out->width*roi_out->height;k++) for(int c=0;c<3;c++) out[3*k+c] = out[4*k+c];

  clReleaseMemObject(dev_in);
  clReleaseMemObject(dev_out);
  if(dev_tmp) clReleaseMemObject(dev_tmp);
}
#endif

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_demosaic_params_t));
  module->default_params = malloc(sizeof(dt_iop_demosaic_params_t));
  // if(dt_image_is_ldr(module->dev->image)) module->default_enabled = 0;
  // else                                    module->default_enabled = 1;
  // FIXME: only enable it for raw images? or handle zoom for non raws, too?
  module->default_enabled = 1;
  module->priority = 1;
  module->hide_enable_button = 1;
  module->params_size = sizeof(dt_iop_demosaic_params_t);
  module->gui_data = NULL;
  dt_iop_demosaic_params_t tmp = (dt_iop_demosaic_params_t){0};
  memcpy(module->params, &tmp, sizeof(dt_iop_demosaic_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_demosaic_params_t));

  const int program = 0; // from programs.conf
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)malloc(sizeof(dt_iop_demosaic_global_data_t));
  module->data = gd;
  gd->kernel_zoom_half_size = dt_opencl_create_kernel(darktable.opencl, program, "clip_and_zoom_demosaic_half_size");
  gd->kernel_ppg_green      = dt_opencl_create_kernel(darktable.opencl, program, "ppg_demosaic_green");
  gd->kernel_ppg_redblue    = dt_opencl_create_kernel(darktable.opencl, program, "ppg_demosaic_redblue");
  gd->kernel_downsample     = dt_opencl_create_kernel(darktable.opencl, program, "clip_and_zoom");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)module->data;
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_zoom_half_size);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_ppg_green);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_ppg_redblue);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_downsample);
  free(module->data);
  module->data = NULL;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)params;
  dt_iop_demosaic_data_t *d = (dt_iop_demosaic_data_t *)piece->data;
  d->filters = self->dev->image->filters;
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_demosaic_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update   (struct dt_iop_module_t *self)
{
  // nothing
}

void gui_init     (struct dt_iop_module_t *self)
{
  self->widget = gtk_label_new(_("this module doesn't have any options"));
}

void gui_cleanup  (struct dt_iop_module_t *self)
{
  // nothing
}

