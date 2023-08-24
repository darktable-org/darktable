#ifdef HAVE_OPENCL
/*
    This file is part of darktable,
    Copyright (C) 2016-2020 darktable developers.

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
#include "common/darktable.h"
#include "common/opencl.h"
#include "common/locallaplaciancl.h"

#define max_levels 30
#define num_gamma 6

// downsample width/height to given level
static inline uint64_t dl(uint64_t size, const int level)
{
  for(int l=0;l<level;l++)
    size = (size-1)/2+1;
  return size;
}

dt_local_laplacian_cl_global_t *dt_local_laplacian_init_cl_global()
{
  dt_local_laplacian_cl_global_t *g = malloc(sizeof(dt_local_laplacian_cl_global_t));

  const int program = 19; // locallaplacian.cl, from programs.conf
  g->kernel_pad_input          = dt_opencl_create_kernel(program, "pad_input");
  g->kernel_gauss_expand       = dt_opencl_create_kernel(program, "gauss_expand");
  g->kernel_gauss_reduce       = dt_opencl_create_kernel(program, "gauss_reduce");
  g->kernel_laplacian_assemble = dt_opencl_create_kernel(program, "laplacian_assemble");
  g->kernel_process_curve      = dt_opencl_create_kernel(program, "process_curve");
  g->kernel_write_back         = dt_opencl_create_kernel(program, "write_back");
  return g;
}

void dt_local_laplacian_free_cl(dt_local_laplacian_cl_t *g)
{
  if(!g) return;
  // be sure we're done with the memory:
  dt_opencl_finish(g->devid);

  // free device mem
  for(int l=0;l<max_levels;l++)
  {
    dt_opencl_release_mem_object(g->dev_padded[l]);
    dt_opencl_release_mem_object(g->dev_output[l]);
    for(int k=0;k<num_gamma;k++)
      dt_opencl_release_mem_object(g->dev_processed[k][l]);
  }
  for(int k=0;k<num_gamma;k++) free(g->dev_processed[k]);
  free(g->dev_padded);
  free(g->dev_output);
  free(g->dev_processed);
  g->dev_padded = g->dev_output = 0;
  g->dev_processed = 0;
  free(g);
}

dt_local_laplacian_cl_t *dt_local_laplacian_init_cl(
    const int devid,
    const int width,            // width of input image
    const int height,           // height of input image
    const float sigma,          // user param: separate shadows/mid-tones/highlights
    const float shadows,        // user param: lift shadows
    const float highlights,     // user param: compress highlights
    const float clarity)        // user param: increase clarity/local contrast
{
  dt_local_laplacian_cl_t *g = malloc(sizeof(dt_local_laplacian_cl_t));
  if(!g) return NULL;

  g->global = darktable.opencl->local_laplacian;
  g->devid = devid;
  g->width = width;
  g->height = height;
  g->sigma = sigma;
  g->shadows = shadows;
  g->highlights = highlights;
  g->clarity = clarity;
  g->dev_padded = calloc(max_levels, sizeof(cl_mem));
  g->dev_output = calloc(max_levels, sizeof(cl_mem));
  g->dev_processed = calloc(num_gamma, sizeof(cl_mem *));
  for(int k=0;k<num_gamma;k++)
    g->dev_processed[k] = calloc(max_levels, sizeof(cl_mem));

  g->num_levels = MIN(max_levels, 31-__builtin_clz(MIN(width,height)));
  g->max_supp = 1<<(g->num_levels-1);
  g->bwidth = ROUNDUPDWD(width  + 2*g->max_supp, devid);
  g->bheight = ROUNDUPDHT(height + 2*g->max_supp, devid);

  // get intermediate vector buffers with read-write access
  for(int l=0;l<g->num_levels;l++)
  {
    g->dev_padded[l] = dt_opencl_alloc_device(devid, ROUNDUPDWD(dl(g->bwidth, l), devid), ROUNDUPDHT(dl(g->bheight, l), devid), sizeof(float));
    if(!g->dev_padded[l]) goto error;
    g->dev_output[l] = dt_opencl_alloc_device(devid, ROUNDUPDWD(dl(g->bwidth, l), devid), ROUNDUPDHT(dl(g->bheight, l), devid), sizeof(float));
    if(!g->dev_output[l]) goto error;
    for(int k=0;k<num_gamma;k++)
    {
      g->dev_processed[k][l] = dt_opencl_alloc_device(devid, ROUNDUPDWD(dl(g->bwidth, l), devid), ROUNDUPDHT(dl(g->bheight, l), devid), sizeof(float));
      if(!g->dev_processed[k][l]) goto error;
    }
  }

  return g;

error:
  dt_print(DT_DEBUG_OPENCL, "[local laplacian cl] could not allocate temporary buffers\n");
  dt_local_laplacian_free_cl(g);
  return NULL;
}

cl_int dt_local_laplacian_cl(
    dt_local_laplacian_cl_t *b, // opencl context with temp buffers
    cl_mem input,               // input buffer in some Labx or yuvx format
    cl_mem output)              // output buffer with colour
{
  cl_int err = -666;

  if(b->bwidth <= 1 || b->bheight <= 1) return err;

  err = dt_opencl_enqueue_kernel_2d_args(b->devid, b->global->kernel_pad_input, b->bwidth, b->bheight,
    CLARG(input), CLARG(b->dev_padded[0]), CLARG(b->width), CLARG(b->height), CLARG(b->max_supp), CLARG(b->bwidth),
    CLARG(b->bheight));
  if(err != CL_SUCCESS) goto error;

  // create gauss pyramid of padded input, write coarse directly to output
  for(int l=1;l<b->num_levels;l++)
  {
    const int wd = dl(b->bwidth, l), ht = dl(b->bheight, l);
    size_t sizes[] = { ROUNDUPDWD(wd, b->devid), ROUNDUPDHT(ht, b->devid), 1 };
    dt_opencl_set_kernel_args(b->devid, b->global->kernel_gauss_reduce, 0, CLARG(b->dev_padded[l-1]));
    if(l == b->num_levels-1)
      dt_opencl_set_kernel_args(b->devid, b->global->kernel_gauss_reduce, 1, CLARG(b->dev_output[l]));
    else
      dt_opencl_set_kernel_args(b->devid, b->global->kernel_gauss_reduce, 1, CLARG(b->dev_padded[l]));
    dt_opencl_set_kernel_args(b->devid, b->global->kernel_gauss_reduce, 2, CLARG(wd), CLARG(ht));
    err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_gauss_reduce, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  for(int k=0;k<num_gamma;k++)
  { // process images
    const float g = (k+.5f)/(float)num_gamma;
    err = dt_opencl_enqueue_kernel_2d_args(b->devid, b->global->kernel_process_curve, b->bwidth, b->bheight,
      CLARG(b->dev_padded[0]), CLARG(b->dev_processed[k][0]), CLARG(g), CLARG(b->sigma), CLARG(b->shadows),
      CLARG(b->highlights), CLARG(b->clarity), CLARG(b->bwidth), CLARG(b->bheight));
    if(err != CL_SUCCESS) goto error;

    // create gaussian pyramids
    for(int l=1;l<b->num_levels;l++)
    {
      const int wd = dl(b->bwidth, l), ht = dl(b->bheight, l);
      err = dt_opencl_enqueue_kernel_2d_args(b->devid, b->global->kernel_gauss_reduce, wd, ht,
        CLARG(b->dev_processed[k][l-1]), CLARG(b->dev_processed[k][l]), CLARG(wd), CLARG(ht));
      if(err != CL_SUCCESS) goto error;
    }
  }

  // assemble output pyramid coarse to fine
  for(int l=b->num_levels-2;l >= 0; l--)
  {
    const int pw = dl(b->bwidth,l), ph = dl(b->bheight,l);
    err = dt_opencl_enqueue_kernel_2d_args(b->devid, b->global->kernel_laplacian_assemble, pw, ph,
      CLARG(b->dev_padded[l]), CLARG(b->dev_output[l+1]), CLARG(b->dev_output[l]), CLARG(b->dev_processed[0][l]),
      CLARG(b->dev_processed[0][l+1]), CLARG(b->dev_processed[1][l]), CLARG(b->dev_processed[1][l+1]),
      CLARG(b->dev_processed[2][l]), CLARG(b->dev_processed[2][l+1]), CLARG(b->dev_processed[3][l]),
      CLARG(b->dev_processed[3][l+1]), CLARG(b->dev_processed[4][l]), CLARG(b->dev_processed[4][l+1]),
      CLARG(b->dev_processed[5][l]), CLARG(b->dev_processed[5][l+1]), CLARG(pw), CLARG(ph));
    if(err != CL_SUCCESS) goto error;
  }

  // read back processed L channel and copy colours:
  err = dt_opencl_enqueue_kernel_2d_args(b->devid, b->global->kernel_write_back, b->width, b->height,
    CLARG(input), CLARG(b->dev_output[0]), CLARG(output), CLARG(b->max_supp), CLARG(b->width), CLARG(b->height));
  if(err != CL_SUCCESS) goto error;

  return CL_SUCCESS;

error:
  dt_print(DT_DEBUG_OPENCL, "[local laplacian cl] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return err;
}

#undef max_levels
#undef num_gamma
#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
