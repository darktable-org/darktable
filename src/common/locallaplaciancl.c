#ifdef HAVE_OPENCL_TODO
/*
    This file is part of darktable,
    copyright (c) 2016 johannes hanika.

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
#include "locallaplaciancl.h"


local_laplacian_cl_global_t *local_laplacian_init_cl_global()
{
  local_laplacian_cl_global_t *g = (local_laplacian_cl_global_t *)malloc(sizeof(local_laplacian_cl_global_t));

  const int program = 19; // locallaplacian.cl, from programs.conf
  g->kernel_pad_input          = dt_opencl_create_kernel(program, "pad_input");
  g->kernel_gauss_expand       = dt_opencl_create_kernel(program, "gauss_expand");
  g->kernel_gauss_reduce       = dt_opencl_create_kernel(program, "gauss_reduce");
  g->kernel_laplacian_assemble = dt_opencl_create_kernel(program, "laplacian_assemble");
  g->kernel_process_curve      = dt_opencl_create_kernel(program, "process_curve");
  g->kernel_write_back         = dt_opencl_create_kernel(program, "write_back");
  return g;
}

void local_laplacian_free_cl(local_laplacian_cl_t *g)
{
  if(!g) return;
  // be sure we're done with the memory:
  dt_opencl_finish(g->devid);

  // free device mem
  dt_opencl_release_mem_object(g->dev_temp1);
  dt_opencl_release_mem_object(g->dev_temp2);
  free(g);
}

local_laplacian_cl_t *local_laplacian_init_cl(
    const int devid,
    const int width,    // width of input image
    const int height,   // height of input image
    const int channels, // channels per pixel
    const float *max,   // maximum allowed values per channel for clamping
    const float *min,   // minimum allowed values per channel for clamping
    const float sigma,  // gaussian sigma
    const int order)    // order of gaussian blur
{
  assert(channels == 1 || channels == 4);

  if(!(channels == 1 || channels == 4)) return NULL;

  dt_gaussian_cl_t *g = (dt_gaussian_cl_t *)malloc(sizeof(dt_gaussian_cl_t));
  if(!g) return NULL;

  g->global = darktable.opencl->gaussian;
  g->devid = devid;
  g->width = width;
  g->height = height;
  g->channels = channels;
  g->sigma = sigma;
  g->order = order;
  g->dev_temp1 = NULL;
  g->dev_temp2 = NULL;
  g->max = (float *)calloc(channels, sizeof(float));
  g->min = (float *)calloc(channels, sizeof(float));

  if(!g->min || !g->max) goto error;

  for(int k = 0; k < channels; k++)
  {
    g->max[k] = max[k];
    g->min[k] = min[k];
  }

  // check if we need to reduce blocksize
  size_t maxsizes[3] = { 0 };     // the maximum dimensions for a work group
  size_t workgroupsize = 0;       // the maximum number of items in a work group
  unsigned long localmemsize = 0; // the maximum amount of local memory we can use
  size_t kernelworkgroupsize = 0; // the maximum amount of items in work group for this kernel

  // make sure blocksize is not too large
  int kernel_gaussian_transpose = (channels == 1) ? g->global->kernel_gaussian_transpose_1c
                                                  : g->global->kernel_gaussian_transpose_4c;
  size_t blocksize = 64;
  int blockwd;
  int blockht;
  if(dt_opencl_get_work_group_limits(devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS
     && dt_opencl_get_kernel_work_group_size(devid, kernel_gaussian_transpose, &kernelworkgroupsize)
        == CL_SUCCESS)
  {
    // reduce blocksize step by step until it fits to limits
    while(blocksize > maxsizes[0] || blocksize > maxsizes[1] || blocksize * blocksize > workgroupsize
          || blocksize * (blocksize + 1) * channels * sizeof(float) > localmemsize)
    {
      if(blocksize == 1) break;
      blocksize >>= 1;
    }

    blockwd = blockht = blocksize;

    if(blockwd * blockht > kernelworkgroupsize) blockht = kernelworkgroupsize / blockwd;
  }
  else
  {
    blockwd = blockht = 1; // slow but safe
  }

  // width and height of intermediate buffers. Need to be multiples of BLOCKSIZE
  const size_t bwidth = width % blockwd == 0 ? width : (width / blockwd + 1) * blockwd;
  const size_t bheight = height % blockht == 0 ? height : (height / blockht + 1) * blockht;

  g->blocksize = blocksize;
  g->blockwd = blockwd;
  g->blockht = blockht;
  g->bwidth = bwidth;
  g->bheight = bheight;

  // get intermediate vector buffers with read-write access
  g->dev_temp1 = dt_opencl_alloc_device_buffer(devid, (size_t)bwidth * bheight * channels * sizeof(float));
  if(!g->dev_temp1) goto error;
  g->dev_temp2 = dt_opencl_alloc_device_buffer(devid, (size_t)bwidth * bheight * channels * sizeof(float));
  if(!g->dev_temp2) goto error;

  return g;

error:
  free(g->min);
  free(g->max);
  dt_opencl_release_mem_object(g->dev_temp1);
  dt_opencl_release_mem_object(g->dev_temp2);
  g->dev_temp1 = g->dev_temp2 = NULL;
  free(g);
  return NULL;
}

static inline void local_laplacian_cl(
    cl_mem input,               // input buffer in some Labx or yuvx format
    cl_mem out,                 // output buffer with colour
    const int wd,               // width and
    const int ht,               // height of the input buffer
    const float sigma,          // user param: separate shadows/midtones/highlights
    const float shadows,        // user param: lift shadows
    const float highlights,     // user param: compress highlights
    const float clarity)        // user param: increase clarity/local contrast
{
  // XXX TODO: the paper says level 5 is good enough, too? more does look significantly different.
#define num_levels 10
#define num_gamma 8
  const int max_supp = 1<<(num_levels-1);
  int w, h;

  // TODO: CL allocate all these on device! (including padded[0])
  // allocate pyramid pointers for padded input
  for(int l=1;l<num_levels;l++)
    padded[l] = (float *)malloc(sizeof(float)*dl(w,l)*dl(h,l));

  // TODO: call ll_pad_input on device (dimensions: padded image):
  float *padded[num_levels] = {0};
  ll_pad_input(input, padded[0], wd, ht, max_supp, &w, &h);

  // TODO: CL allocate these on device, too!
  // allocate pyramid pointers for output
  float *output[num_levels] = {0};
  for(int l=0;l<num_levels;l++)
    output[l] = (float *)malloc(sizeof(float)*dl(w,l)*dl(h,l));

  // TODO: CL run gauss_reduce on the device! (dimensions: coarse buffer)
  // create gauss pyramid of padded input, write coarse directly to output
  for(int l=1;l<num_levels-1;l++)
    gauss_reduce(padded[l-1], padded[l], dl(w,l-1), dl(h,l-1));
  gauss_reduce(padded[num_levels-2], output[num_levels-1], dl(w,num_levels-2), dl(h,num_levels-2));

  // note that this is hardcoded in cl now:
  // evenly sample brightness [0,1]:
  float gamma[num_gamma] = {0.0f};
  for(int k=0;k<num_gamma;k++) gamma[k] = (k+.5f)/(float)num_gamma;
  // for(int k=0;k<num_gamma;k++) gamma[k] = k/(num_gamma-1.0f);

  // TODO: CL allocate these buffers on the device!
  // XXX FIXME: don't need to alloc all the memory at once!
  // XXX FIXME: accumulate into output pyramid one by one? (would require more passes, potentially bad idea)
  // allocate memory for intermediate laplacian pyramids
  float *buf[num_gamma][num_levels] = {{0}};
  for(int k=0;k<num_gamma;k++) for(int l=0;l<num_levels;l++)
    buf[k][l] = (float *)malloc(sizeof(float)*dl(w,l)*dl(h,l));

  // XXX TODO: the paper says remapping only level 3 not 0 does the trick, too:
  for(int k=0;k<num_gamma;k++)
  { // process images
    // TODO: process image on device!
    // TODO: run kernel process_curve on resolution of buffer (w,h)
      buf[k][0][w*j+i] = ll_curve(padded[0][w*j+i], gamma[k], sigma, shadows, highlights, clarity);

    for(int l=1;l<num_levels;l++)
    // TODO: run gauss_reduce on device! (dimensions: coarse buffer)
    // create gaussian pyramids
      gauss_reduce(buf[k][l-1], buf[k][l], dl(w,l-1), dl(h,l-1));
  }

  // assemble output pyramid coarse to fine
  for(int l=num_levels-2;l >= 0; l--)
  {
    const int pw = dl(w,l), ph = dl(h,l);

    // TODO: CL run on device (dimensions: pw, ph, fine buffer):
    gauss_expand(output[l+1], output[l], pw, ph);
    // TODO: this is laplacian_assemble() on device: (dimensions: fine buffer)
    // go through all coefficients in the upsampled gauss buffer:
    laplacian_assemble( pass all buffers buf[.][l, l+1])
  }
  // TODO: read back processed L chanel and copy colours:
  write_back(
      read_only  image2d_t input,
      read_only  image2d_t processed,
      write_only image2d_t output,
      const int wd,
      const int ht);
  // TODO: free all buffers!
#undef num_levels
#undef num_gamma
}
#endif
