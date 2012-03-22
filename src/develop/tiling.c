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
#define CLAMPI(a, mn, mx) ((a) < (mn) ? (mn) : ((a) > (mx) ? (mx) : (a)))



/* this defines an additional alignment requirement for opencl image width. 
   It can have strong effects on processing speed. Reasonable values are a 
   power of 2. set to 1 for no effect. */
#define CL_ALIGNMENT 4


/* greatest common divisor */
static unsigned
_gcd(unsigned a, unsigned b)
{
  unsigned t;
  while(b != 0)
  {
    t = b;
    b = a % b;
    a = t;
  }
  return a;
}

/* least common multiple */
static unsigned
_lcm(unsigned a, unsigned b)
{
  return (((unsigned long)a * b) / _gcd(a, b));
}


/* if a module does not implement process_tiling() by itself, this function is called instead.
   default_process_tiling() is able to handle standard cases where pixels change their values
   but not their places. */
void
default_process_tiling (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, const int in_bpp)
{
  void *input = NULL;
  void *output = NULL;

  /* we only care for the most simple cases ATM. else try to process the standard way, i.e. in one chunk. let's hope for the best... */
  if(memcmp(roi_in, roi_out, sizeof(struct dt_iop_roi_t)))
  {
    dt_print(DT_DEBUG_DEV, "[default_process_tiling] cannot handle requested roi's. fall back to standard method for module '%s'\n", self->op);
    goto fallback;
  }

  const int out_bpp = self->output_bpp(self, piece->pipe, piece);
  const int ipitch = roi_in->width * in_bpp;
  const int opitch = roi_out->width * out_bpp;

  /* get tiling requirements of module */
  dt_develop_tiling_t tiling = { 0 };
  self->tiling_callback(self, piece, roi_in, roi_out, &tiling);

  /* tiling really does not make sense in these cases. standard process() is not better or worse than we are */
  if(tiling.factor < 2.2f && tiling.overhead < 0.2f * roi_out->width * roi_out->height * max(in_bpp, out_bpp))
  {
    dt_print(DT_DEBUG_DEV, "[default_process_tiling] don't use tiling for module '%s'. no real memory saving could be reached\n", self->op);
    goto fallback;
  }

  /* calculate optimal size of tiles */
  float available = (float)dt_conf_get_int("host_memory_limit")*1024.0f*1024.0f;
  assert(available >= 500.0f*1024.0f*1024.0f);
  /* correct for size of ivoid and ovoid which are needed on top of tiling */
  available = fmax(available - roi_out->width * roi_out->height * (in_bpp + out_bpp) - tiling.overhead, 0);

  /* we ignore the above value if singlebuffer_limit (is defined and) is higher than available/tiling.factor.
     this will mainly allow tiling for modules with high and "unpredictable" memory demand which is
     reflected in high values of tiling.factor (take bilateral noise reduction as an example). */
  float singlebuffer = (float)dt_conf_get_int("singlebuffer_limit")*1024.0f*1024.0f;
  singlebuffer = fmax(singlebuffer, 1024.0f*1024.0f);
  assert(tiling.factor > 1.0f);
  singlebuffer = fmax(available / tiling.factor, singlebuffer);

  int width = roi_out->width;
  int height = roi_out->height;

  /* shrink tile size in case it would exceed singlebuffer size */
  if(width*height*max(in_bpp, out_bpp) > singlebuffer)
  {
    const float scale = singlebuffer/(width*height*max(in_bpp, out_bpp));

    /* TODO: can we make this more efficient to minimize total overlap between tiles? */
    if(width < height && scale >= 0.333f)
    { 
      height = floorf(height * scale);
    }
    else if(height <= width && scale >= 0.333f)
    {
      width = floorf(width * scale);
    }
    else
    {
      width = floorf(width * sqrt(scale));
      height = floorf(height * sqrt(scale));
    }
  }

  /* make sure we have a reasonably effective tile dimension. if not try square tiles */
  if(3*tiling.overlap > width || 3*tiling.overlap > height)
  {
    width = height = floorf(sqrtf((float)width*height));
  }

#if 0
  /* we might want to grow dimensions a bit */
  width = max(4*tiling.overlap, width);
  height = max(4*tiling.overlap, height);
#endif

  /* Alignment rules: we need to make sure that alignment requirements of module are fulfilled.
     Modules will report alignment requirements via xalign and yalign within tiling_callback().
     Typical use case is demosaic where Bayer pattern requires alignment to a multiple of 2 in x and y
     direction.
     We guarantee alignment by selecting image width/height and overlap accordingly. For a tile width/height
     that is identical to image width/height no special alignment is needed. */

  const unsigned int xyalign = _lcm(tiling.xalign, tiling.yalign);

  assert(xyalign != 0);

  /* properly align tile width and height by making them smaller if needed */
  if(width < roi_out->width) width = (width / xyalign) * xyalign;
  if(height < roi_out->height) height = (height / xyalign) * xyalign;

  /* also make sure that overlap follows alignment rules by making it wider when needed */
  const int overlap = tiling.overlap % xyalign != 0 ? (tiling.overlap / xyalign + 1) * xyalign : tiling.overlap;

  /* calculate effective tile size */
  const int tile_wd = width - 2*overlap > 0 ? width - 2*overlap : 1;
  const int tile_ht = height - 2*overlap > 0 ? height - 2*overlap : 1;

  /* calculate number of tiles */
  const int tiles_x = width < roi_out->width ? ceilf(roi_out->width /(float)tile_wd) : 1;
  const int tiles_y = height < roi_out->height ? ceilf(roi_out->height/(float)tile_ht) : 1;

  /* sanity check: don't run wild on too many tiles */
  if(tiles_x * tiles_y > DT_TILING_MAXTILES)
  {
    dt_print(DT_DEBUG_DEV, "[default_process_tiling] gave up tiling for module '%s'. too many tiles: %d x %d\n", self->op, tiles_x, tiles_y);
    goto error;
  }


  dt_print(DT_DEBUG_DEV, "[default_process_tiling] use tiling on module '%s' for image with full size %d x %d\n", self->op, roi_out->width, roi_out->height);
  dt_print(DT_DEBUG_DEV, "[default_process_tiling] (%d x %d) tiles with max dimensions %d x %d and overlap %d\n", tiles_x, tiles_y, width, height, overlap);

  /* reserve input and output buffers for tiles */
  input = dt_alloc_align(64, width*height*in_bpp);
  if(input == NULL)
  {
    dt_print(DT_DEBUG_DEV, "[default_process_tiling] could not alloc input buffer for module '%s'\n", self->op);
    goto error;
  }
  output = dt_alloc_align(64, width*height*out_bpp);
  if(output == NULL)
  {
    dt_print(DT_DEBUG_DEV, "[default_process_tiling] could not alloc output buffer for module '%s'\n", self->op);
    goto error;
  }

  /* store processed_maximum to be re-used and aggregated */
  float processed_maximum_saved[3];
  float processed_maximum_new[3] = { 1.0f };
  for(int k=0; k<3; k++)
    processed_maximum_saved[k] = piece->pipe->processed_maximum[k];


  /* iterate over tiles */
  for(int tx=0; tx<tiles_x; tx++)
    for(int ty=0; ty<tiles_y; ty++)  
  {
    size_t wd = tx * tile_wd + width > roi_out->width  ? roi_out->width - tx * tile_wd : width;
    size_t ht = ty * tile_ht + height > roi_out->height ? roi_out->height- ty * tile_ht : height;

    /* no need to process end-tiles that are smaller than overlap */
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

    dt_print(DT_DEBUG_DEV, "[default_process_tiling] tile (%d, %d) with %d x %d at origin [%d, %d]\n", tx, ty, wd, ht, tx*tile_wd, ty*tile_ht);

    /* prepare input tile buffer */
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(input,width,ivoid,ioffs,wd,ht) schedule(static)
#endif
    for(int j=0; j<ht; j++)
      memcpy((char *)input+j*wd*in_bpp, (char *)ivoid+ioffs+j*ipitch, wd*in_bpp);

    /* take original processed_maximum as starting point */
    for(int k=0; k<3; k++)
      piece->pipe->processed_maximum[k] = processed_maximum_saved[k];

    /* call process() of module */
    self->process(self, piece, input, output, &iroi, &oroi);

    /* aggregate resulting processed_maximum */
    /* TODO: check if there really can be differences between tiles and take
             appropriate action (calculate minimum, maximum, average, ...?) */
    for(int k=0; k<3; k++)
    {
      if(tx+ty > 0 && fabs(processed_maximum_new[k] - piece->pipe->processed_maximum[k]) > 1.0e-6f)
        dt_print(DT_DEBUG_DEV, "[default_process_tiling] processed_maximum[%d] differs between tiles in module '%s'\n", k, self->op);
      processed_maximum_new[k] = piece->pipe->processed_maximum[k];
    }

    /* correct origin and region of tile for overlap.
       make sure that we only copy back the "good" part. */
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

    /* copy "good" part of tile to output buffer */
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(ovoid,ooffs,output,width,origin,region,wd) schedule(static)
#endif
    for(int j=0; j<region[1]; j++)
      memcpy((char *)ovoid+ooffs+j*opitch, (char *)output+((j+origin[1])*wd+origin[0])*out_bpp, region[0]*out_bpp);
  }

  /* copy back final processed_maximum */
  for(int k=0; k<3; k++)
    piece->pipe->processed_maximum[k] = processed_maximum_new[k];

  if(input != NULL) free(input);
  if(output != NULL) free(output);
  return;

error:
  if(input != NULL) free(input);
  if(output != NULL) free(output);
  dt_print(DT_DEBUG_DEV, "[default_process_tiling] tiling failed for module '%s'\n", self->op);
  /* TODO: give a warning message to user */
  return;

fallback:
  if(input != NULL) free(input);
  if(output != NULL) free(output);
  dt_print(DT_DEBUG_DEV, "[default_process_tiling] fall back to standard processing for module '%s'\n", self->op);
  self->process(self, piece, ivoid, ovoid, roi_in, roi_out);
  return;

}



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

  /* get tiling requirements of module */
  dt_develop_tiling_t tiling = { 0 };
  self->tiling_callback(self, piece, roi_in, roi_out, &tiling);


  /* calculate optimal size of tiles */
  float headroom = (float)dt_conf_get_int("opencl_memory_headroom")*1024.0f*1024.0f;
  headroom = fmin(fmax(headroom, 0.0f), (float)darktable.opencl->dev[devid].max_global_mem);
  const float available = darktable.opencl->dev[devid].max_global_mem - headroom;
  const float singlebuffer = fmin(fmax((available - tiling.overhead) / tiling.factor, 0.0f), darktable.opencl->dev[devid].max_mem_alloc);
  int width = min(roi_out->width, darktable.opencl->dev[devid].max_image_width);
  int height = min(roi_out->height, darktable.opencl->dev[devid].max_image_height);

  /* shrink tile size in case it would exceed singlebuffer size */
  if((float)width*height*max(in_bpp, out_bpp) > singlebuffer)
  {
    const float scale = singlebuffer/(width*height*max(in_bpp, out_bpp));

    if(width < height && scale >= 0.333f)
    { 
      height = floorf(height * scale);
    }
    else if(height <= width && scale >= 0.333f)
    {
      width = floorf(width * scale);
    }
    else
    {
      width = floorf(width * sqrt(scale));
      height = floorf(height * sqrt(scale));
    }
  }

  /* make sure we have a reasonably effective tile dimension. if not try square tiles */
  if(3*tiling.overlap > width || 3*tiling.overlap > height)
  {
    width = height = floorf(sqrtf((float)width*height));
  }


  /* Alignment rules: we need to make sure that alignment requirements of module are fulfilled.
     Modules will report alignment requirements via xalign and yalign within tiling_callback().
     Typical use case is demosaic where Bayer pattern requires alignment to a multiple of 2 in x and y
     direction. Additional alignment requirements are set via definition of CL_ALIGNMENT.
     We guarantee alignment by selecting image width/height and overlap accordingly. For a tile width/height
     that is identical to image width/height no special alignment is done. */

  /* for simplicity reasons we use only one alignment that fits to x and y requirements at the same time */
  const unsigned int xyalign = _lcm(tiling.xalign, tiling.yalign);

  /* determing alignment requirement for tile width/height.
     in case of tile width also align according to definition of CL_ALIGNMENT */
  const unsigned int walign = _lcm(xyalign, CL_ALIGNMENT);
  const unsigned int halign = xyalign;

  assert(xyalign != 0 && walign != 0 && halign != 0);

  /* properly align tile width and height by making them smaller if needed */
  if(width < roi_out->width) width = (width / walign) * walign;
  if(height < roi_out->height) height = (height / halign) * halign;

  /* also make sure that overlap follows alignment rules by making it wider when needed */
  const int overlap = tiling.overlap % xyalign != 0 ? (tiling.overlap / xyalign + 1) * xyalign : tiling.overlap;


  /* calculate effective tile size */
  const int tile_wd = width - 2*overlap > 0 ? width - 2*overlap : 1;
  const int tile_ht = height - 2*overlap > 0 ? height - 2*overlap : 1;

#if 0 // moved upwards
  /* make sure we have a reasonably effective tile size, else return FALSE and leave it to CPU path */
  if(2*tile_wd < width || 2*tile_ht < height)
  {
    dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl] aborted tiling for module '%s'. too small effective tiles: %d x %d.\n", self->op, tile_wd, tile_ht);
    return FALSE;
  }
#endif

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


  /* store processed_maximum to be re-used and aggregated */
  float processed_maximum_saved[3];
  float processed_maximum_new[3] = { 1.0f };
  for(int k=0; k<3; k++)
    processed_maximum_saved[k] = piece->pipe->processed_maximum[k];


  /* get opencl input and output buffers, to be re-used for all tiles.
     For "end-tiles" these buffers will only be partly filled; the acutally used part
     is then correctly reflected in iroi and oroi which we give to the respective
     process_cl(). Attention! opencl kernels may not simply read beyond limits (given by width and height)
     as they can no longer rely on CLK_ADDRESS_CLAMP_TO_EDGE to give reasonable results! */
  input = dt_opencl_alloc_device(devid, width, height, in_bpp);
  if(input == NULL) goto error;
  output = dt_opencl_alloc_device(devid, width, height, out_bpp);
  if(output == NULL) goto error;


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
    size_t ioffs = (ty * tile_ht)*ipitch + tx * tile_wd*in_bpp;
    size_t ooffs = (ty * tile_ht)*opitch + tx * tile_wd*out_bpp;

    dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl] tile (%d, %d) with %d x %d at origin [%d, %d]\n", tx, ty, wd, ht, tx*tile_wd, ty*tile_ht);


    /* non-blocking memory transfer: host input buffer -> opencl/device tile */
    err = dt_opencl_write_host_to_device_raw(devid, (char *)ivoid + ioffs, input, origin, region, ipitch, CL_FALSE);
    if(err != CL_SUCCESS) goto error;

    /* take original processed_maximum as starting point */
    for(int k=0; k<3; k++)
      piece->pipe->processed_maximum[k] = processed_maximum_saved[k];

    /* call process_cl of module */
    if(!self->process_cl(self, piece, input, output, &iroi, &oroi)) goto error;

    /* aggregate resulting processed_maximum */
    /* TODO: check if there really can be differences between tiles and take
             appropriate action (calculate minimum, maximum, average, ...?) */
    for(int k=0; k<3; k++)
    {
      if(tx+ty > 0 && fabs(processed_maximum_new[k] - piece->pipe->processed_maximum[k]) > 1.0e-6f)
        dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl] processed_maximum[%d] differs between tiles in module '%s'\n", k, self->op);
      processed_maximum_new[k] = piece->pipe->processed_maximum[k];
    }

    /* correct origin and region of tile for overlap.
       makes sure that we only copy back the "good" part. */
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

    /* non-blocking memory transfer: opencl/device tile -> host output buffer */
    err = dt_opencl_read_host_from_device_raw(devid, (char *)ovoid + ooffs, output, origin, region, opitch, CL_FALSE);
    if(err != CL_SUCCESS) goto error;

    /* block until opencl queue has finished to free all used event handlers. needed here as with
       some OpenCL implementations we would otherwise run out of them */
    dt_opencl_finish(devid);
  }

  /* copy back final processed_maximum */
  for(int k=0; k<3; k++)
    piece->pipe->processed_maximum[k] = processed_maximum_new[k];

  if(input != NULL) dt_opencl_release_mem_object(input);
  if(output != NULL) dt_opencl_release_mem_object(output);
  return TRUE;

error:
  /* copy back stored processed_maximum */
  for(int k=0; k<3; k++)
    piece->pipe->processed_maximum[k] = processed_maximum_saved[k];
  if(input != NULL) dt_opencl_release_mem_object(input);
  if(output != NULL) dt_opencl_release_mem_object(output);
  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_opencl] couldn't run process_cl() for module '%s' in tiling mode: %d\n", self->op, err);
  return FALSE;
}
#else
int
default_process_tiling_cl (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, const int bpp)
{
  return FALSE;
}
#endif



/* If a module does not implement tiling_callback() by itself, this function is called instead.
   Default is an image size factor of 2 (i.e. input + output buffer needed), no overhead (1),
   no overlap between tiles, and an pixel alignment of 1 in x and y direction, i.e. no special
   alignment required. Simple pixel to pixel modules (take tonecurve as an example) can happily
   live with that.
   (1) Small overhead like look-up-tables in tonecurve can be ignored safely. */
void default_tiling_callback  (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 2.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

int 
dt_tiling_piece_fits_host_memory(const size_t width, const size_t height, const unsigned bpp, const float factor, const size_t overhead)
{
  static int host_memory_limit = -1;

  /* first time run */
  if(host_memory_limit < 0)
  {
    host_memory_limit = dt_conf_get_int("host_memory_limit");

    /* don't let the user play games with us */
    if(host_memory_limit != 0) host_memory_limit = CLAMPI(host_memory_limit, 500, 50000);
    dt_conf_set_int("host_memory_limit", host_memory_limit);
  }

  float requirement = factor * width * height * bpp + overhead;

  if(host_memory_limit == 0 || requirement <= host_memory_limit * 1024.0f * 1024.0f) return TRUE;

  return FALSE;
}

