/*
    This file is part of darktable,
    copyright (c) 2012 Ulrich Pegelow.

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

#ifndef DT_COMMON_GAUSSIAN_CL_H
#define DT_COMMON_GAUSSIAN_CL_H

#ifdef HAVE_OPENCL
#include <math.h>
#include <assert.h>
#include "common/opencl.h"

#ifndef DT_GAUSSIAN_ORDER_T
#define DT_GAUSSIAN_ORDER_T
typedef enum dt_gaussian_order_t
{
  DT_IOP_GAUSSIAN_ZERO = 0,
  DT_IOP_GAUSSIAN_ONE = 1,
  DT_IOP_GAUSSIAN_TWO = 2
}
dt_gaussian_order_t;
#endif


typedef struct dt_gaussian_cl_global_t
{
  int kernel_gaussian_column, kernel_gaussian_transpose;
}
dt_gaussian_cl_global_t;

typedef struct dt_gaussian_cl_t
{
  dt_gaussian_cl_global_t *global;
  int devid;
  int width, height, channels;
  int blocksize, blockwd, blockht;
  size_t bwidth, bheight;
  float sigma;
  int order;
  float *min;
  float *max;
  cl_mem dev_temp1;
  cl_mem dev_temp2;
}
dt_gaussian_cl_t;


static 
void compute_gauss_params_cl(const float sigma, dt_gaussian_order_t order, float *a0, float *a1, float *a2, float *a3, 
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

  switch(order)
  {
    default:
    case DT_IOP_GAUSSIAN_ZERO:
    {
      const float k = (1.0f - ema)*(1.0f - ema)/(1.0f + (2.0f * alpha * ema) - ema2);
      *a0 = k;
      *a1 = k * (alpha - 1.0f) * ema;
      *a2 = k * (alpha + 1.0f) * ema;
      *a3 = -k * ema2;
    }
    break;

    case DT_IOP_GAUSSIAN_ONE:
    {
      *a0 = (1.0f - ema)*(1.0f - ema);
      *a1 = 0.0f;
      *a2 = -*a0;
      *a3 = 0.0f;
    }
    break;

    case DT_IOP_GAUSSIAN_TWO:
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




dt_gaussian_cl_global_t *
dt_gaussian_init_cl_global()
{
  dt_gaussian_cl_global_t *g = (dt_gaussian_cl_global_t *)malloc(sizeof(dt_gaussian_cl_global_t));

  const int program = 6;   // gaussian.cl, from programs.conf
  g->kernel_gaussian_column        = dt_opencl_create_kernel(program, "gaussian_column");
  g->kernel_gaussian_transpose     = dt_opencl_create_kernel(program, "gaussian_transpose");
  return g;
}

void
dt_gaussian_free_cl(
    dt_gaussian_cl_t *g)
{
  if(!g) return;
  // be sure we're done with the memory:
  dt_opencl_finish(g->devid);

  free(g->min);
  free(g->max);
  // free device mem
  dt_opencl_release_mem_object(g->dev_temp1);
  dt_opencl_release_mem_object(g->dev_temp2);
  free(g);
}

dt_gaussian_cl_t *
dt_gaussian_init_cl(
    const int devid,
    const int width,       // width of input image
    const int height,      // height of input image
    const int channels,    // channels per pixel
    const float *max,      // maximum allowed values per channel for clamping
    const float *min,      // minimum allowed values per channel for clamping
    const float sigma,     // gaussian sigma
    const int order)       // order of gaussian blur
{
  dt_gaussian_cl_t *g = (dt_gaussian_cl_t *)malloc(sizeof(dt_gaussian_cl_t));
  if(!g) return NULL;

  assert(channels > 0 && channels <= 4);

  g->global = darktable.opencl->gaussian;
  g->devid = devid;
  g->width = width;
  g->height = height;
  g->channels = channels;
  g->sigma = sigma;
  g->order = order;
  g->dev_temp1 = NULL;
  g->dev_temp2 = NULL;
  g->max = (float *)malloc(channels * sizeof(float));
  g->min = (float *)malloc(channels * sizeof(float));

  if(!g->min || !g->max) goto error;

  for(int k=0; k < channels; k++)
  {
    g->max[k] = max[k];
    g->min[k] = min[k];
  }

  // check if we need to reduce blocksize
  size_t maxsizes[3] = { 0 };        // the maximum dimensions for a work group
  size_t workgroupsize = 0;          // the maximum number of items in a work group
  unsigned long localmemsize = 0;    // the maximum amount of local memory we can use
  size_t kernelworkgroupsize = 0;    // the maximum amount of items in work group for this kernel
   
  // make sure blocksize is not too large
  size_t blocksize = 64;
  int blockwd;
  int blockht;
  if(dt_opencl_get_work_group_limits(devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS &&
     dt_opencl_get_kernel_work_group_size(devid, g->global->kernel_gaussian_transpose, &kernelworkgroupsize) == CL_SUCCESS)
  {
    // reduce blocksize step by step until it fits to limits
    while(blocksize > maxsizes[0] || blocksize > maxsizes[1] 
          || blocksize*blocksize > workgroupsize || blocksize*(blocksize+1)*channels*sizeof(float) > localmemsize)
    {
      if(blocksize == 1) break;
      blocksize >>= 1;    
    }

    blockwd = blockht = blocksize;

    if(blockwd * blockht > kernelworkgroupsize)
      blockht = kernelworkgroupsize / blockwd;
  }
  else
  {
    blockwd = blockht = 1;   // slow but safe
  }

  // width and height of intermediate buffers. Need to be multiples of BLOCKSIZE
  const size_t bwidth = width % blockwd == 0 ? width : (width / blockwd + 1)*blockwd;
  const size_t bheight = height % blockht == 0 ? height : (height / blockht + 1)*blockht;

  g->blocksize = blocksize;
  g->blockwd = blockwd;
  g->blockht = blockht;
  g->bwidth = bwidth;
  g->bheight = bheight;

  // get intermediate vector buffers with read-write access
  g->dev_temp1 = dt_opencl_alloc_device_buffer(devid, bwidth*bheight*channels*sizeof(float));
  if(!g->dev_temp1) goto error;
  g->dev_temp2 = dt_opencl_alloc_device_buffer(devid, bwidth*bheight*channels*sizeof(float));
  if(!g->dev_temp2) goto error;

  return g;

error:
  free(g->min);
  free(g->max);
  if(g->dev_temp1) dt_opencl_release_mem_object(g->dev_temp1);
  if(g->dev_temp2) dt_opencl_release_mem_object(g->dev_temp2);
  free(g);
  return NULL;
}


cl_int
dt_gaussian_blur_cl(
    dt_gaussian_cl_t *g,
    cl_mem           dev_in,
    cl_mem           dev_out)
{
  cl_int err = -999;
  const int devid = g->devid;

  const int width = g->width;
  const int height = g->height;
  const int channels = g->channels;
  const int bpp = channels*sizeof(float);
  cl_mem dev_temp1 = g->dev_temp1;
  cl_mem dev_temp2 = g->dev_temp2;

  const int blocksize = g->blocksize;
  const int blockwd = g->blockwd;
  const int blockht = g->blockht;
  const int bwidth = g->bwidth;
  const int bheight = g->bheight;

  float Labmax[4] = { 0.0f };
  float Labmin[4] = { 0.0f };

  assert(channels <= 4);

  for(int k=0; k<MIN(channels, 4); k++)
  {
    Labmax[k] = g->max[k];
    Labmin[k] = g->min[k];
  }

  size_t origin[] = {0, 0, 0};
  size_t region[] = {width, height, 1};
  size_t local[]  = {blockwd, blockht, 1};
  size_t sizes[3];

  // compute gaussian parameters
  float a0, a1, a2, a3, b1, b2, coefp, coefn;
  compute_gauss_params_cl(g->sigma, g->order, &a0, &a1, &a2, &a3, &b1, &b2, &coefp, &coefn);

  // copy dev_in to intermediate buffer dev_temp1
  err = dt_opencl_enqueue_copy_image_to_buffer(devid, dev_in, dev_temp1, origin, region, 0);
  if(err != CL_SUCCESS) return err;

  // first blur step: column by column with dev_temp1 -> dev_temp2
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = 1;
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 0, sizeof(cl_mem), (void *)&dev_temp1);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 1, sizeof(cl_mem), (void *)&dev_temp2);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 4, sizeof(float), (void *)&a0);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 5, sizeof(float), (void *)&a1);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 6, sizeof(float), (void *)&a2);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 7, sizeof(float), (void *)&a3);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 8, sizeof(float), (void *)&b1);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 9, sizeof(float), (void *)&b2);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 10, sizeof(float), (void *)&coefp);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 11, sizeof(float), (void *)&coefn);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 12, 4*sizeof(float), (void *)&Labmax);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 13, 4*sizeof(float), (void *)&Labmin);
  err = dt_opencl_enqueue_kernel_2d(devid, g->global->kernel_gaussian_column, sizes);
  if(err != CL_SUCCESS) return err;

  // intermediate step: transpose dev_temp2 -> dev_temp1
  sizes[0] = bwidth;
  sizes[1] = bheight;
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_transpose, 0, sizeof(cl_mem), (void *)&dev_temp2);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_transpose, 1, sizeof(cl_mem), (void *)&dev_temp1);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_transpose, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_transpose, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_transpose, 4, sizeof(int), (void *)&blocksize);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_transpose, 5, bpp*blocksize*(blocksize+1), NULL);
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, g->global->kernel_gaussian_transpose, sizes, local);
  if(err != CL_SUCCESS) return err;


  // second blur step: column by column of transposed image with dev_temp1 -> dev_temp2 (!! height <-> width !!)
  sizes[0] = ROUNDUPWD(height);
  sizes[1] = 1;
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 0, sizeof(cl_mem), (void *)&dev_temp1);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 1, sizeof(cl_mem), (void *)&dev_temp2);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 3, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 4, sizeof(float), (void *)&a0);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 5, sizeof(float), (void *)&a1);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 6, sizeof(float), (void *)&a2);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 7, sizeof(float), (void *)&a3);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 8, sizeof(float), (void *)&b1);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 9, sizeof(float), (void *)&b2);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 10, sizeof(float), (void *)&coefp);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 11, sizeof(float), (void *)&coefn);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 12, 4*sizeof(float), (void *)&Labmax);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_column, 13, 4*sizeof(float), (void *)&Labmin);
  err = dt_opencl_enqueue_kernel_2d(devid, g->global->kernel_gaussian_column, sizes);
  if(err != CL_SUCCESS) return err;


  // transpose back dev_temp2 -> dev_temp1
  sizes[0] = bheight;
  sizes[1] = bwidth;
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_transpose, 0, sizeof(cl_mem), (void *)&dev_temp2);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_transpose, 1, sizeof(cl_mem), (void *)&dev_temp1);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_transpose, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_transpose, 3, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_transpose, 4, sizeof(int), (void *)&blocksize);
  dt_opencl_set_kernel_arg(devid, g->global->kernel_gaussian_transpose, 5, bpp*blocksize*(blocksize+1), NULL);
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, g->global->kernel_gaussian_transpose, sizes, local);
  if(err != CL_SUCCESS) return err;

  // finally produce output in dev_out
  err = dt_opencl_enqueue_copy_buffer_to_image(devid, dev_temp1, dev_out, 0, origin, region);
  if(err != CL_SUCCESS) return err;

  return CL_SUCCESS;
}


void
dt_gaussian_free_cl_global(
    dt_gaussian_cl_global_t *g)
{
  if(!g) return;
  // destroy kernels
  dt_opencl_free_kernel(g->kernel_gaussian_column);
  dt_opencl_free_kernel(g->kernel_gaussian_transpose);
  free(g);
}

#endif
#endif
