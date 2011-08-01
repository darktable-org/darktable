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

#include "develop/tiling.h"
#include "develop/pixelpipe.h"
#include "develop/blend.h"
#include "common/opencl.h"

#include <assert.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <assert.h>

#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))

#ifdef HAVE_OPENCL
/* if a module does not implement process_tiling_cl() by itself, this function is called instead.
   default_process_tiling_cl() is able to handle standard cases where pixels change their values
   but not their places. */
int
default_process_tiling_cl (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, const int in_bpp)
{
  cl_int err = -999;
  cl_mem input = NULL;
  cl_mem output = NULL;
  float factor;
  unsigned overhead;
  unsigned overlap;

  //fprintf(stderr, "roi_in: {%d, %d, %d, %d, %5.3f} roi_out: {%d, %d, %d, %d, %5.3f} in module '%s'\n",
  //      roi_in->x, roi_in->y, roi_in->width, roi_in->height, (double)roi_in->scale,
  //      roi_out->x, roi_out->y, roi_out->width, roi_out->height, (double)roi_out->scale, self->op);


  /* We only care for the most simple cases ATM. Delegate other stuff to CPU path. */
  if(memcmp(roi_in, roi_out, sizeof(struct dt_iop_roi_t)))
  {
    dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl] can not handle requested roi's. tiling for module '%s' not possible.\n", self->op);
    return FALSE;
  }

  const int devid = piece->pipe->devid;
  const int out_bpp = self->output_bpp(self, piece->pipe, piece);
  const int ipitch = roi_in->width * in_bpp;
  const int opitch = roi_out->width * out_bpp;
  
  /* get memory requirements of module */
  self->tiling_callback(self, piece, roi_in, roi_out, &factor, &overhead, &overlap);

  /* calculate optimal size of tiles */
  const size_t available = darktable.opencl->dev[devid].max_global_mem - DT_OPENCL_MEMORY_HEADROOM;
  const size_t singlebuffer = min(((float)(available - overhead)) / factor, darktable.opencl->dev[devid].max_mem_alloc);
  int width = min(roi_out->width, darktable.opencl->dev[devid].max_image_width);
  int height = min(roi_out->height, darktable.opencl->dev[devid].max_image_height);

  if (width*height*max(in_bpp, out_bpp) > singlebuffer)
  {
    const float scale = (float)singlebuffer/(width*height*max(in_bpp, out_bpp));
    width = floorf(width * sqrt(scale));
    height = floorf(height * sqrt(scale));
  }

  /* calculate effective tile size */
  const int tile_wd = width - 2*overlap;
  const int tile_ht = height - 2*overlap;

  /* make sure we have a reasonably effective tile size, else return FALSE and leave it to CPU path */
  if(2*tile_wd < width || 2*tile_ht < height)
  {
    dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl] aborted tiling for module '%s'. too small effective tiles: %d x %d.\n", self->op, tile_wd, tile_ht);
    return FALSE;
  }

  /* calculate number of tiles */
  const int tiles_x = width < roi_out->width ? ceilf(roi_out->width /(float)tile_wd) : 1;
  const int tiles_y = height < roi_out->height ? ceilf(roi_out->height/(float)tile_ht) : 1;

  /* sanity check: don't run wild on too many tiles */
  if(tiles_x * tiles_y > DT_TILING_MAXTILES)
  {
    dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl] aborted tiling for module '%s'. too many tiles: %d.\n", self->op, tiles_x * tiles_y);
    return FALSE;
  }

  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl] use tiling on module '%s' for image with full size %d x %d\n", self->op, roi_out->width, roi_out->height);
  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl] (%d x %d) tiles with max dimensions %d x %d and overlap %d\n", tiles_x, tiles_y, width, height, overlap);

  /* iterate over tiles */
  for(int tx=0; tx<tiles_x; tx++)
    for(int ty=0; ty<tiles_y; ty++)  
  {
    size_t wd = tx * tile_wd + width > roi_out->width  ? roi_out->width - tx * tile_wd : width;
    size_t ht = ty * tile_ht + height > roi_out->height ? roi_out->height- ty * tile_ht : height;

    /* no need to process (end)tiles that are smaller than overlap */
    if((wd <= overlap && tx > 0) || (ht <= overlap && ty > 0)) continue;

    /* origin and region of effective part of tile, which we want to store later */
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { wd, ht, 1 };

    /* roi_in and roi_out for process_cl on subbuffer */
    dt_iop_roi_t iroi = { 0, 0, wd, ht, roi_in->scale };
    dt_iop_roi_t oroi = { 0, 0, wd, ht, roi_out->scale };

    /* offsets of tile into ivoid and ovoid */
    size_t ioffs = (ty * tile_ht)*ipitch + (tx * tile_wd)*in_bpp;
    size_t ooffs = (ty * tile_ht)*opitch + (tx * tile_wd)*out_bpp;

    /* correct tile for overlap */
    if(tx > 0)
    {
      origin[0] += overlap;
      region[0] -= overlap;
      ooffs += overlap*out_bpp;
    }
    if(ty > 0)
    {
      origin[1] += overlap;
      region[1] -= overlap;
      ooffs += overlap*opitch;
    }

    dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl] tile (%d, %d) with %d x %d at origin [%d, %d]\n", tx, ty, wd, ht, tx*tile_wd, ty*tile_ht);

    /* This is a first implementation. It has significant overhead in generating and releasing
       OpenCL image object and additionally might lead to GPU memory fragmentation.
       TODO: check alternative route with a permanent input/output buffer */
    input = dt_opencl_copy_host_to_device_rowpitch(devid, (char *)ivoid + ioffs, wd, ht, in_bpp, ipitch);
    if(input == NULL) goto error;

    output = dt_opencl_alloc_device(devid, wd, ht, out_bpp);
    if(output == NULL) goto error;

    if(!self->process_cl(self, piece, input, output, &iroi, &oroi)) goto error;

    dt_opencl_finish(devid);

    err = dt_opencl_read_host_from_device_raw(devid, (char *)ovoid + ooffs, output, origin, region, opitch, CL_TRUE);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_release_mem_object(input);
    input = NULL;
    dt_opencl_release_mem_object(output);
    output = NULL;
  }

  if(input != NULL) dt_opencl_release_mem_object(input);
  if(output != NULL) dt_opencl_release_mem_object(output);
  return TRUE;

error:
  if(input != NULL) dt_opencl_release_mem_object(input);
  if(output != NULL) dt_opencl_release_mem_object(output);
  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_opencl] couldn't run process_cl() for module '%s' in tiling mode: %d\n", self->op, err);
  return FALSE;
}

/* If a module does not implement tiling_callback() by itself, this function is called instead.
   Default is an image size factor of 2 (i.e. input + output buffer needed), no overhead (1),
   and no overlap between tiles. Simple pixel to pixel modules (take tonecurve as an example)
   can happily live with that.
   (1) Small overhead like look-up-tables in tonecurve can be ignored safely. */
void default_tiling_callback  (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, float *factor, unsigned *overhead, unsigned *overlap)
{
  *factor = 2.0f;
  *overhead = 0;
  *overlap = 0;
  return;
}

#else
int
default_process_tiling_cl (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, const int bpp)
{
  return FALSE;
}

void default_tiling_callback  (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, float *factor, unsigned *overhead, unsigned *overlap)
{
  *factor = 2.0f;
  *overhead = 0;
  *overlap = 0;
  return;
}
#endif

