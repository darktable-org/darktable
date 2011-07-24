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

/* this is a temporary hack, so that we can test the code without having all
   modules already adapted. Will finaly go away */
static int
_in_positive_list(const char* op)
{
  static const char *positive_list[] = { "basecurve", "tonecurve", "colorin", "colorout", "exposure", "sharpen", "highpass", "lowpass", "highlights" };

  const int listlength = sizeof(positive_list)/sizeof(char *);
  int found = 0;

  for(int k=0; k<listlength; k++)
  {
    if(!strcmp(op, positive_list[k]))
    {
      found = 1;
      break;
    }
  }
  return found;
}


int
default_process_tiling_cl (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, const int in_bpp)
{
  cl_int err = -999;
  cl_mem input = NULL;
  cl_mem output = NULL;
  float factor;
  unsigned overhead;
  unsigned overlap;

  if(!_in_positive_list(self->op))
  {
    dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl] module '%s' skipped!\n", self->op);
    return FALSE;
  }

  /* We only care for the most simple cases ATM. Delegate other stuff to CPU path. */
  if(roi_in->width != roi_out->width || roi_in->height != roi_out->height ||
     roi_in->x != roi_out->x || roi_in->y != roi_out->y || roi_in->scale != roi_out->scale ||
     roi_in->x + roi_in->y != 0) return FALSE;

  const int devid = piece->pipe->devid;
  const int out_bpp = self->output_bpp(self, piece->pipe, piece);
  const int ipitch = roi_in->width * in_bpp;
  const int opitch = roi_out->width * out_bpp;
  
  /* get memory requirements of module */
  self->tiling_callback(self, piece, roi_in, roi_out, &factor, &overhead, &overlap);

  /* calculate optimal size of tiles */
  const size_t available = darktable.opencl->dev[devid].max_global_mem - DT_OPENCL_MEMORY_HEADROOM;
  const size_t singlebuffer = min(((float)(available - overhead)) / factor, darktable.opencl->dev[devid].max_mem_alloc);
  int width = min(max(roi_in->width, roi_out->width), darktable.opencl->dev[devid].max_image_width);
  int height = min(max(roi_in->height, roi_out->height), darktable.opencl->dev[devid].max_image_height);

  if (width*height*max(in_bpp, out_bpp) > singlebuffer)
  {
    const float scale = (float)singlebuffer/(width*height*max(in_bpp, out_bpp));
    width = floorf(width * sqrt(scale));
    height = floorf(height * sqrt(scale));
  }

  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl] tiling module '%s' with dimensions %d x %d and overlap %d\n", self->op, width, height, overlap);

  /* calculate effective tile size */
  const int tile_wd = width - 2*overlap;
  const int tile_ht = height - 2*overlap;

  /* make sure we have a reasonably effective tile size, else return FALSE and leave it to CPU path */
  if(2*tile_wd < width || 2*tile_ht < height) return FALSE;

  /* calculate number of tiles */
  const int tiles_x = width < roi_in->width ? ceilf(roi_in->width /(float)tile_wd) : 1;
  const int tiles_y = height < roi_in->height ? ceilf(roi_in->height/(float)tile_ht) : 1;

  /* sanity check: don't run wild on too many tiles */
  if(tiles_x * tiles_y > DT_TILING_MAXTILES) return FALSE;

  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl] tiling module '%s' for full size image %d x %d\n", self->op, roi_in->width, roi_in->height);
  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl] tiling module '%s' with %d x %d tiles\n", self->op, tiles_x, tiles_y);

  /* iterate over tiles */
  for(int tx=0; tx<tiles_x; tx++)
    for(int ty=0; ty<tiles_y; ty++)  
  {
    size_t wd = tx * tile_wd + width > roi_in->width  ? roi_in->width - tx * tile_wd : width;
    size_t ht = ty * tile_ht + height > roi_in->height ? roi_in->height- ty * tile_ht : height;

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

    if(region[0] <= 0 || region[1] <= 0) continue;

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
  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_opencl] couldn't run module '%s' in tiling mode: %d\n", self->op, err);
  return FALSE;
}

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

