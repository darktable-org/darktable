#ifdef HAVE_OPENCL
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
  dt_local_laplacian_cl_global_t *g = (dt_local_laplacian_cl_global_t *)malloc(sizeof(dt_local_laplacian_cl_global_t));

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
    const float sigma,          // user param: separate shadows/midtones/highlights
    const float shadows,        // user param: lift shadows
    const float highlights,     // user param: compress highlights
    const float clarity)        // user param: increase clarity/local contrast
{
  dt_local_laplacian_cl_t *g = (dt_local_laplacian_cl_t *)malloc(sizeof(dt_local_laplacian_cl_t));
  if(!g) return 0;

  g->global = darktable.opencl->local_laplacian;
  g->devid = devid;
  g->width = width;
  g->height = height;
  g->sigma = sigma;
  g->shadows = shadows;
  g->highlights = highlights;
  g->clarity = clarity;
  g->dev_padded = (cl_mem *)calloc(max_levels, sizeof(cl_mem *));
  g->dev_output = (cl_mem *)calloc(max_levels, sizeof(cl_mem *));
  g->dev_processed = (cl_mem **)calloc(num_gamma, sizeof(cl_mem **));
  for(int k=0;k<num_gamma;k++)
    g->dev_processed[k] = (cl_mem *)calloc(max_levels, sizeof(cl_mem *));

  g->num_levels = MIN(max_levels, 31-__builtin_clz(MIN(width,height)));
  const int max_supp = 1<<(g->num_levels-1);
  const int paddwd = width  + 2*max_supp;
  const int paddht = height + 2*max_supp;

  const size_t bwidth = ROUNDUPWD(paddwd);
  const size_t bheight = ROUNDUPWD(paddht);

  // get intermediate vector buffers with read-write access
  for(int l=0;l<g->num_levels;l++)
  {
    g->dev_padded[l] = dt_opencl_alloc_device(devid, ROUNDUPWD(dl(bwidth, l)), ROUNDUPHT(dl(bheight, l)), sizeof(float));
    if(!g->dev_padded[l]) goto error;
    g->dev_output[l] = dt_opencl_alloc_device(devid, ROUNDUPWD(dl(bwidth, l)), ROUNDUPHT(dl(bheight, l)), sizeof(float));
    if(!g->dev_output[l]) goto error;
    for(int k=0;k<num_gamma;k++)
    {
      g->dev_processed[k][l] = dt_opencl_alloc_device(devid, ROUNDUPWD(dl(bwidth, l)), ROUNDUPHT(dl(bheight, l)), sizeof(float));
      if(!g->dev_processed[k][l]) goto error;
    }
  }

  return g;

error:
  fprintf(stderr, "[local laplacian cl] could not allocate temporary buffers\n");
  dt_local_laplacian_free_cl(g);
  return 0;
}

cl_int dt_local_laplacian_cl(
    dt_local_laplacian_cl_t *b, // opencl context with temp buffers
    cl_mem input,               // input buffer in some Labx or yuvx format
    cl_mem output)              // output buffer with colour
{
  const int max_supp = 1<<(b->num_levels-1);
  const int wd2 = 2*max_supp + b->width;
  const int ht2 = 2*max_supp + b->height;
  cl_int err = -666;

  size_t sizes_pad[] = { ROUNDUPWD(wd2), ROUNDUPHT(ht2), 1 };
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_pad_input, 0, sizeof(cl_mem), (void *)&input);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_pad_input, 1, sizeof(cl_mem), (void *)&b->dev_padded[0]);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_pad_input, 2, sizeof(int), (void *)&b->width);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_pad_input, 3, sizeof(int), (void *)&b->height);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_pad_input, 4, sizeof(int), (void *)&max_supp);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_pad_input, 5, sizeof(int), (void *)&wd2);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_pad_input, 6, sizeof(int), (void *)&ht2);
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_pad_input, sizes_pad);
  if(err != CL_SUCCESS) goto error;

  // create gauss pyramid of padded input, write coarse directly to output
  for(int l=1;l<b->num_levels;l++)
  {
    const int wd = dl(wd2, l), ht = dl(ht2, l);
    size_t sizes[] = { ROUNDUPWD(wd), ROUNDUPHT(ht), 1 };
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_gauss_reduce, 0, sizeof(cl_mem), (void *)&b->dev_padded[l-1]);
    if(l == b->num_levels-1)
      dt_opencl_set_kernel_arg(b->devid, b->global->kernel_gauss_reduce, 1, sizeof(cl_mem), (void *)&b->dev_output[l]);
    else
      dt_opencl_set_kernel_arg(b->devid, b->global->kernel_gauss_reduce, 1, sizeof(cl_mem), (void *)&b->dev_padded[l]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_gauss_reduce, 2, sizeof(int), (void *)&wd);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_gauss_reduce, 3, sizeof(int), (void *)&ht);
    err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_gauss_reduce, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  for(int k=0;k<num_gamma;k++)
  { // process images
    const float g = (k+.5f)/(float)num_gamma;
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_process_curve, 0, sizeof(cl_mem), (void *)&b->dev_padded[0]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_process_curve, 1, sizeof(cl_mem), (void *)&b->dev_processed[k][0]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_process_curve, 2, sizeof(float), (void *)&g);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_process_curve, 3, sizeof(float), (void *)&b->sigma);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_process_curve, 4, sizeof(float), (void *)&b->shadows);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_process_curve, 5, sizeof(float), (void *)&b->highlights);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_process_curve, 6, sizeof(float), (void *)&b->clarity);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_process_curve, 7, sizeof(int), (void *)&wd2);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_process_curve, 8, sizeof(int), (void *)&ht2);
    err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_process_curve, sizes_pad);
    if(err != CL_SUCCESS) goto error;

    // create gaussian pyramids
    for(int l=1;l<b->num_levels;l++)
    {
      const int wd = dl(wd2, l), ht = dl(ht2, l);
      size_t sizes[] = { ROUNDUPWD(wd), ROUNDUPHT(ht), 1 };
      dt_opencl_set_kernel_arg(b->devid, b->global->kernel_gauss_reduce, 0, sizeof(cl_mem), (void *)&b->dev_processed[k][l-1]);
      dt_opencl_set_kernel_arg(b->devid, b->global->kernel_gauss_reduce, 1, sizeof(cl_mem), (void *)&b->dev_processed[k][l]);
      dt_opencl_set_kernel_arg(b->devid, b->global->kernel_gauss_reduce, 2, sizeof(int), (void *)&wd);
      dt_opencl_set_kernel_arg(b->devid, b->global->kernel_gauss_reduce, 3, sizeof(int), (void *)&ht);
      err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_gauss_reduce, sizes);
      if(err != CL_SUCCESS) goto error;
    }
  }

  // assemble output pyramid coarse to fine
  for(int l=b->num_levels-2;l >= 0; l--)
  {
    const int pw = dl(wd2,l), ph = dl(ht2,l);
    size_t sizes[] = { ROUNDUPWD(pw), ROUNDUPHT(ph), 1 };
    // this is so dumb:
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble,  0, sizeof(cl_mem), (void *)&b->dev_padded[l]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble,  1, sizeof(cl_mem), (void *)&b->dev_output[l+1]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble,  2, sizeof(cl_mem), (void *)&b->dev_output[l]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble,  3, sizeof(cl_mem), (void *)&b->dev_processed[0][l]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble,  4, sizeof(cl_mem), (void *)&b->dev_processed[0][l+1]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble,  5, sizeof(cl_mem), (void *)&b->dev_processed[1][l]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble,  6, sizeof(cl_mem), (void *)&b->dev_processed[1][l+1]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble,  7, sizeof(cl_mem), (void *)&b->dev_processed[2][l]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble,  8, sizeof(cl_mem), (void *)&b->dev_processed[2][l+1]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble,  9, sizeof(cl_mem), (void *)&b->dev_processed[3][l]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble, 10, sizeof(cl_mem), (void *)&b->dev_processed[3][l+1]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble, 11, sizeof(cl_mem), (void *)&b->dev_processed[4][l]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble, 12, sizeof(cl_mem), (void *)&b->dev_processed[4][l+1]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble, 13, sizeof(cl_mem), (void *)&b->dev_processed[5][l]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble, 14, sizeof(cl_mem), (void *)&b->dev_processed[5][l+1]);
    // dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble, 15, sizeof(cl_mem), (void *)&b->dev_processed[6][l]);
    // dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble, 16, sizeof(cl_mem), (void *)&b->dev_processed[6][l+1]);
    // dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble, 17, sizeof(cl_mem), (void *)&b->dev_processed[7][l]);
    // dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble, 18, sizeof(cl_mem), (void *)&b->dev_processed[7][l+1]);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble, 15, sizeof(int), (void *)&pw);
    dt_opencl_set_kernel_arg(b->devid, b->global->kernel_laplacian_assemble, 16, sizeof(int), (void *)&ph);
    err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_laplacian_assemble, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  // read back processed L channel and copy colours:
  size_t sizes[] = { ROUNDUPWD(b->width), ROUNDUPHT(b->height), 1 };
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_write_back, 0, sizeof(cl_mem), (void *)&input);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_write_back, 1, sizeof(cl_mem), (void *)&b->dev_output[0]);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_write_back, 2, sizeof(cl_mem), (void *)&output);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_write_back, 3, sizeof(int), (void *)&max_supp);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_write_back, 4, sizeof(int), (void *)&b->width);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_write_back, 5, sizeof(int), (void *)&b->height);
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_write_back, sizes);
  if(err != CL_SUCCESS) goto error;
  return CL_SUCCESS;
error:
  fprintf(stderr, "[local laplacian cl] failed: %d\n", err);
  return err;
}
#undef max_levels
#undef num_gamma
#endif
