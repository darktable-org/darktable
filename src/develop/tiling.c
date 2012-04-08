/*
    This file is part of darktable,
    copyright (c) 2011--2012 ulrich pegelow.

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
#include "control/control.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <assert.h>

#define CLAMPI(a, mn, mx) ((a) < (mn) ? (mn) : ((a) > (mx) ? (mx) : (a)))


/* this defines an additional alignment requirement for opencl image width. 
   It can have strong effects on processing speed. Reasonable values are a 
   power of 2. set to 1 for no effect. */
#define CL_ALIGNMENT 4

/* parameter RESERVE for extended roi_in sizes due to inaccuracies when doing 
   roi_out -> roi_in estimations.
   Needs to be increased if tiling fails due to insufficient buffer sizes. */
#define RESERVE 5

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


static inline int
_min(int a, int b)
{
  return a < b ? a : b;
}

static inline int
_max(int a, int b)
{
  return a > b ? a : b;
}


static inline int
_align_up(int n, int a)
{
  return n % a !=0 ? (n/a + 1) * a : n;
}

static inline int
_align_down(int n, int a)
{
  return n % a !=0 ? (n/a) * a : n;
}


void
_print_roi(const dt_iop_roi_t *roi, const char *label)
{
  printf("{ %5d  %5d  %5d  %5d  %.6f } %s\n", roi->x, roi->y, roi->width, roi->height, roi->scale, label);
}


/* iterative search to get a matching oroi_full. start by probing start value of oroi and
   get corresponding input roi into iroi_probe. If search converges, then iroi_probe gets identical
   to iroi (taking limit delta into account).
   We try two times, second time with assumption that image is flipped. */
static int
_fit_output_to_input_roi(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *iroi, dt_iop_roi_t *oroi, int delta, int iter) 
{
  dt_iop_roi_t iroi_probe = *iroi;

  int save_iter = iter;
  dt_iop_roi_t save_oroi = *oroi;

  self->modify_roi_in(self, piece, oroi, &iroi_probe);
    
  while ((abs((int)iroi_probe.x - (int)iroi->x) > delta || 
         abs((int)iroi_probe.y - (int)iroi->y) > delta ||
         abs((int)iroi_probe.width - (int)iroi->width) > delta ||
         abs((int)iroi_probe.height - (int)iroi->height) > delta) &&
         iter > 0)
  {
    //_print_roi(&iroi_probe, "tile iroi_probe");
    //_print_roi(oroi, "tile oroi old");
      
    oroi->x += (iroi->x - iroi_probe.x) * oroi->scale / iroi->scale;
    oroi->y += (iroi->y - iroi_probe.y) * oroi->scale / iroi->scale;
    oroi->width += (iroi->width - iroi_probe.width) * oroi->scale / iroi->scale;
    oroi->height += (iroi->height - iroi_probe.height) * oroi->scale / iroi->scale;

    //_print_roi(oroi, "tile oroi new");

    self->modify_roi_in(self, piece, oroi, &iroi_probe);
    iter--;
  }

  if (iter > 0) return TRUE;

  /* it did not converge. retry with the assumption that the image is flipped */
  iter = save_iter;
  *oroi = save_oroi;

  self->modify_roi_in(self, piece, oroi, &iroi_probe);
    
  while ((abs((int)iroi_probe.x - (int)iroi->x) > delta || 
         abs((int)iroi_probe.y - (int)iroi->y) > delta ||
         abs((int)iroi_probe.width - (int)iroi->width) > delta ||
         abs((int)iroi_probe.height - (int)iroi->height) > delta) &&
         iter > 0)
  {
    //_print_roi(&iroi_probe, "tile iroi_probe");
    //_print_roi(oroi, "tile oroi old");
      
    oroi->x += (iroi->y - iroi_probe.y) * oroi->scale / iroi->scale;
    oroi->y += (iroi->x - iroi_probe.x) * oroi->scale / iroi->scale;
    oroi->width += (iroi->height - iroi_probe.height) * oroi->scale / iroi->scale;
    oroi->height += (iroi->width - iroi_probe.width) * oroi->scale / iroi->scale;

    //_print_roi(oroi, "tile oroi new");

    self->modify_roi_in(self, piece, oroi, &iroi_probe);
    iter--;
  }

  if (iter > 0) return TRUE;  

  return FALSE;
}


/* simple tiling algorithm for roi_in == roi_out, i.e. for pixel to pixel modules/operations */
static void
_default_process_tiling_ptp (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, const int in_bpp)
{
  void *input = NULL;
  void *output = NULL;

  const int out_bpp = self->output_bpp(self, piece->pipe, piece);
  const int ipitch = roi_in->width * in_bpp;
  const int opitch = roi_out->width * out_bpp;
  const int max_bpp = _max(in_bpp, out_bpp);

  /* get tiling requirements of module */
  dt_develop_tiling_t tiling = { 0 };
  self->tiling_callback(self, piece, roi_in, roi_out, &tiling);

  /* tiling really does not make sense in these cases. standard process() is not better or worse than we are */
  if(tiling.factor < 2.2f && tiling.overhead < 0.2f * roi_in->width * roi_in->height * max_bpp)
  {
    dt_print(DT_DEBUG_DEV, "[default_process_tiling_ptp] no need to use tiling for module '%s' as no real memory saving to be expected\n", self->op);
    goto fallback;
  }

  /* calculate optimal size of tiles */
  float available = (float)dt_conf_get_int("host_memory_limit")*1024.0f*1024.0f;
  assert(available >= 500.0f*1024.0f*1024.0f);
  /* correct for size of ivoid and ovoid which are needed on top of tiling */
  available = fmax(available - (roi_out->width*roi_out->height*out_bpp) - (roi_in->width*roi_in->height*in_bpp) - tiling.overhead, 0);

  /* we ignore the above value if singlebuffer_limit (is defined and) is higher than available/tiling.factor.
     this will mainly allow tiling for modules with high and "unpredictable" memory demand which is
     reflected in high values of tiling.factor (take bilateral noise reduction as an example). */
  float singlebuffer = (float)dt_conf_get_int("singlebuffer_limit")*1024.0f*1024.0f;
  singlebuffer = fmax(singlebuffer, 1024.0f*1024.0f);
  assert(tiling.factor > 1.0f);
  singlebuffer = fmax(available / tiling.factor, singlebuffer);

  int width = roi_in->width;
  int height = roi_in->height;

  /* shrink tile size in case it would exceed singlebuffer size */
  if((float)width*height*max_bpp > singlebuffer)
  {
    const float scale = singlebuffer/(width*height*max_bpp);

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

  /* Alignment rules: we need to make sure that alignment requirements of module are fulfilled.
     Modules will report alignment requirements via xalign and yalign within tiling_callback().
     Typical use case is demosaic where Bayer pattern requires alignment to a multiple of 2 in x and y
     direction.
     We guarantee alignment by selecting image width/height and overlap accordingly. For a tile width/height
     that is identical to image width/height no special alignment is needed. */

  const unsigned int xyalign = _lcm(tiling.xalign, tiling.yalign);

  assert(xyalign != 0);

  /* properly align tile width and height by making them smaller if needed */
  if(width < roi_in->width) width = (width / xyalign) * xyalign;
  if(height < roi_in->height) height = (height / xyalign) * xyalign;

  /* also make sure that overlap follows alignment rules by making it wider when needed */
  const int overlap = tiling.overlap % xyalign != 0 ? (tiling.overlap / xyalign + 1) * xyalign : tiling.overlap;

  /* calculate effective tile size */
  const int tile_wd = width - 2*overlap > 0 ? width - 2*overlap : 1;
  const int tile_ht = height - 2*overlap > 0 ? height - 2*overlap : 1;

  /* calculate number of tiles */
  const int tiles_x = width < roi_in->width ? ceilf(roi_in->width /(float)tile_wd) : 1;
  const int tiles_y = height < roi_in->height ? ceilf(roi_in->height/(float)tile_ht) : 1;

  /* sanity check: don't run wild on too many tiles */
  if(tiles_x * tiles_y > DT_TILING_MAXTILES)
  {
    dt_print(DT_DEBUG_DEV, "[default_process_tiling_ptp] gave up tiling for module '%s'. too many tiles: %d x %d\n", self->op, tiles_x, tiles_y);
    goto error;
  }


  dt_print(DT_DEBUG_DEV, "[default_process_tiling_ptp] use tiling on module '%s' for image with full size %d x %d\n", self->op, roi_in->width, roi_in->height);
  dt_print(DT_DEBUG_DEV, "[default_process_tiling_ptp] (%d x %d) tiles with max dimensions %d x %d and overlap %d\n", tiles_x, tiles_y, width, height, overlap);

  /* reserve input and output buffers for tiles */
  input = dt_alloc_align(64, width*height*in_bpp);
  if(input == NULL)
  {
    dt_print(DT_DEBUG_DEV, "[default_process_tiling_ptp] could not alloc input buffer for module '%s'\n", self->op);
    goto error;
  }
  output = dt_alloc_align(64, width*height*out_bpp);
  if(output == NULL)
  {
    dt_print(DT_DEBUG_DEV, "[default_process_tiling_ptp] could not alloc output buffer for module '%s'\n", self->op);
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
    size_t wd = tx * tile_wd + width > roi_in->width  ? roi_in->width - tx * tile_wd : width;
    size_t ht = ty * tile_ht + height > roi_in->height ? roi_in->height- ty * tile_ht : height;

    /* no need to process end-tiles that are smaller than overlap */
    if((wd <= overlap && tx > 0) || (ht <= overlap && ty > 0)) continue;

    /* origin and region of effective part of tile, which we want to store later */
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { wd, ht, 1 };

    /* roi_in and roi_out for process_cl on subbuffer */
    dt_iop_roi_t iroi = { roi_in->x+tx*tile_wd, roi_in->y+ty*tile_ht, wd, ht, roi_in->scale };
    dt_iop_roi_t oroi = { roi_out->x+tx*tile_wd, roi_out->y+ty*tile_ht, wd, ht, roi_out->scale };

    /* offsets of tile into ivoid and ovoid */
    size_t ioffs = (ty * tile_ht)*ipitch + (tx * tile_wd)*in_bpp;
    size_t ooffs = (ty * tile_ht)*opitch + (tx * tile_wd)*out_bpp;



    dt_print(DT_DEBUG_DEV, "[default_process_tiling_ptp] tile (%d, %d) with %d x %d at origin [%d, %d]\n", tx, ty, wd, ht, tx*tile_wd, ty*tile_ht);

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
        dt_print(DT_DEBUG_DEV, "[default_process_tiling_ptp] processed_maximum[%d] differs between tiles in module '%s'\n", k, self->op);
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
  dt_control_log(_("tiling failed for module '%s'. output might be garbled."), self->op);
  // fall through

fallback:
  if(input != NULL) free(input);
  if(output != NULL) free(output);
  dt_print(DT_DEBUG_DEV, "[default_process_tiling_ptp] fall back to standard processing for module '%s'\n", self->op);
  self->process(self, piece, ivoid, ovoid, roi_in, roi_out);
  return;

}



/* more elaborate tiling algorithm for roi_in != roi_out: slower than the ptp variant,
   more tiles and larger overlap */
static void
_default_process_tiling_roi (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, const int in_bpp)
{
  void *input = NULL;
  void *output = NULL;

  //_print_roi(roi_in, "module roi_in");
  //_print_roi(roi_out, "module roi_out");

  const int out_bpp = self->output_bpp(self, piece->pipe, piece);
  const int ipitch = roi_in->width * in_bpp;
  const int opitch = roi_out->width * out_bpp;
  const int max_bpp = _max(in_bpp, out_bpp);

  /* inaccuracy for roi_in elements in roi_out -> roi_in calculations */
  const int delta = ceilf(roi_in->scale / roi_out->scale);

  /* estimate for additional (space) requirement in buffer dimensions due to inaccuracies */
  const int inacc = RESERVE*delta;

  /* get tiling requirements of module */
  dt_develop_tiling_t tiling = { 0 };
  self->tiling_callback(self, piece, roi_in, roi_out, &tiling);

  /* tiling really does not make sense in these cases. standard process() is not better or worse than we are */
  if(tiling.factor < 2.2f && tiling.overhead < 0.2f * roi_in->width * roi_in->height * max_bpp)
  {
    dt_print(DT_DEBUG_DEV, "[default_process_tiling_roi] no need to use tiling for module '%s' as no real memory saving to be expected\n", self->op);
    goto fallback;
  }

  /* calculate optimal size of tiles */
  float available = (float)dt_conf_get_int("host_memory_limit")*1024.0f*1024.0f;
  assert(available >= 500.0f*1024.0f*1024.0f);
  /* correct for size of ivoid and ovoid which are needed on top of tiling */
  available = fmax(available - (roi_out->width*roi_out->height*out_bpp) - (roi_in->width*roi_in->height*in_bpp) - tiling.overhead, 0);

  /* we ignore the above value if singlebuffer_limit (is defined and) is higher than available/tiling.factor.
     this will mainly allow tiling for modules with high and "unpredictable" memory demand which is
     reflected in high values of tiling.factor (take bilateral noise reduction as an example). */
  float singlebuffer = (float)dt_conf_get_int("singlebuffer_limit")*1024.0f*1024.0f;
  singlebuffer = fmax(singlebuffer, 1024.0f*1024.0f);
  assert(tiling.factor > 1.0f);
  singlebuffer = fmax(available / tiling.factor, singlebuffer);

  int width = _max(roi_in->width, roi_out->width);
  int height = _max(roi_in->height, roi_out->height);

  /* shrink tile size in case it would exceed singlebuffer size */
  if((float)width*height*max_bpp > singlebuffer)
  {
    const float scale = singlebuffer/(width*height*max_bpp);

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

  /* Alignment rules: we need to make sure that alignment requirements of module are fulfilled.
     Modules will report alignment requirements via xalign and yalign within tiling_callback().
     Typical use case is demosaic where Bayer pattern requires alignment to a multiple of 2 in x and y
     direction. */

  /* for simplicity reasons we use only one alignment that fits to x and y requirements at the same time */
  unsigned int xyalign = _lcm(tiling.xalign, tiling.yalign);

  assert(xyalign != 0);

  /* make sure that overlap follows alignment rules by making it wider when needed.
     overlap_in needs to be aligned, overlap_out is only here to calculate output buffer size */
  const int overlap_in = _align_up(tiling.overlap, xyalign);
  const int overlap_out = ceilf(overlap_in * roi_out->scale / roi_in->scale);

  int tiles_x = 1, tiles_y = 1;

  /* calculate number of tiles taking the larger buffer (input or output) as a guiding one.
     normally it is roi_in > roi_out; but let's be prepared */
  if(roi_in->width > roi_out->width)
    tiles_x = width < roi_in->width ? ceilf((float)roi_in->width / (float)_max(width - 2*overlap_in - inacc, 1)) : 1;
  else
    tiles_x = width < roi_out->width ? ceilf((float)roi_out->width / (float)_max(width - 2*overlap_out, 1)) : 1;

  if(roi_in->height > roi_out->height)
    tiles_y = height < roi_in->height ? ceilf((float)roi_in->height / (float)_max(height - 2*overlap_in - inacc, 1)) : 1;
  else
    tiles_y = height < roi_out->height ? ceilf((float)roi_out->height / (float)_max(height - 2*overlap_out, 1)) : 1;

  /* sanity check: don't run wild on too many tiles */
  if(tiles_x * tiles_y > DT_TILING_MAXTILES)
  {
    dt_print(DT_DEBUG_DEV, "[default_process_tiling_roi] gave up tiling for module '%s'. too many tiles: %d x %d\n", self->op, tiles_x, tiles_y);
    goto error;
  }

  /* calculate tile width and height excl. overlap (i.e. the good part) for output.
     values are important for all following processing steps. */
  const int tile_wd = roi_out->width % tiles_x == 0 ? roi_out->width / tiles_x : roi_out->width / tiles_x + 1;
  const int tile_ht = roi_out->height % tiles_y == 0 ? roi_out->height / tiles_y : roi_out->height / tiles_y + 1;

  /* calculate tile width and height incl. output overlap with some additional reserve.
     only needed to select sufficient output buffer size */
  const int tile_wd_out = _min(roi_out->width / tiles_x + 2*overlap_out, roi_out->width) + RESERVE;
  const int tile_ht_out = _min(roi_out->height / tiles_y + 2*overlap_out, roi_out->height) + RESERVE;

  /* calculate tile width and height incl. input overlap with some additional reserve.
     only needed to select sufficient input buffer size */
  const int tile_wd_in = _min(tile_wd_out * roi_in->scale / roi_out->scale, roi_in->width);
  const int tile_ht_in = _min(tile_ht_out * roi_in->scale / roi_out->scale, roi_in->height);

  dt_print(DT_DEBUG_DEV, "[default_process_tiling_roi] use tiling on module '%s' for image with full input size %d x %d\n", self->op, roi_in->width, roi_in->height);
  dt_print(DT_DEBUG_DEV, "[default_process_tiling_roi] (%d x %d) tiles with max input dimensions %d x %d\n", tiles_x, tiles_y, tile_wd_in, tile_ht_in);


  /* store processed_maximum to be re-used and aggregated */
  float processed_maximum_saved[3];
  float processed_maximum_new[3] = { 1.0f };
  for(int k=0; k<3; k++)
    processed_maximum_saved[k] = piece->pipe->processed_maximum[k];


  /* reserve input and output buffers for tiles */
  input = dt_alloc_align(64, tile_wd_in*tile_ht_in*in_bpp);
  if(input == NULL)
  {
    dt_print(DT_DEBUG_DEV, "[default_process_tiling_roi] could not alloc input buffer for module '%s'\n", self->op);
    goto error;
  }
  output = dt_alloc_align(64, tile_wd_out*tile_ht_out*out_bpp);
  if(output == NULL)
  {
    dt_print(DT_DEBUG_DEV, "[default_process_tiling_roi] could not alloc output buffer for module '%s'\n", self->op);
    goto error;
  }

  /* iterate over tiles */
  for(int tx=0; tx<tiles_x; tx++)
    for(int ty=0; ty<tiles_y; ty++)  
  {
    /* the output dimensions of the good part of this specific tile */
    size_t wd = (tx + 1) * tile_wd > roi_out->width  ? roi_out->width - tx * tile_wd : tile_wd;
    size_t ht = (ty + 1) * tile_ht > roi_out->height ? roi_out->height- ty * tile_ht : tile_ht;

    /* roi_in and roi_out of good part: oroi_good easy to calculate based on number and dimension of tile.
       iroi_good is calculated by modify_roi_in() of respective module */
    dt_iop_roi_t iroi_good = { roi_in->x+tx*tile_wd, roi_in->y+ty*tile_ht, wd, ht, roi_in->scale };
    dt_iop_roi_t oroi_good = { roi_out->x+tx*tile_wd, roi_out->y+ty*tile_ht, wd, ht, roi_out->scale };

    self->modify_roi_in(self, piece, &oroi_good, &iroi_good);

    //_print_roi(&iroi_good, "tile iroi_good");
    //_print_roi(&oroi_good, "tile oroi_good");

    /* now we need to calculate full region of this tile: increase input roi to take care of overlap requirements 
       and alignment and add additional delta to correct for possible rounding errors in modify_roi_in() 
       -> generates first estimate of iroi_full */
    const int x_in = iroi_good.x;
    const int y_in = iroi_good.y;
    const int width_in = iroi_good.width;
    const int height_in = iroi_good.height;
    const int new_x_in = _max(_align_down(x_in - overlap_in - delta, xyalign), roi_in->x);
    const int new_y_in = _max(_align_down(y_in - overlap_in - delta, xyalign), roi_in->y);
    const int new_width_in = _min(_align_up(width_in + overlap_in + delta + (x_in - new_x_in), xyalign), roi_in->width + roi_in->x - new_x_in);
    const int new_height_in = _min(_align_up(height_in + overlap_in + delta + (y_in - new_y_in), xyalign), roi_in->height + roi_in->y - new_y_in);

    /* iroi_full based on calculated numbers and dimensions. oroi_full just set as a starting point for the following iterative search */
    dt_iop_roi_t iroi_full = { new_x_in, new_y_in, new_width_in, new_height_in, iroi_good.scale };
    dt_iop_roi_t oroi_full = { roi_out->x, roi_out->y, roi_out->width, roi_out->height, oroi_good.scale };

    //_print_roi(&iroi_full, "tile iroi_full before optimization");
    //_print_roi(&oroi_full, "tile oroi_full before optimization");

     
    /* try to find a matching oroi_full */
    if (!_fit_output_to_input_roi(self, piece, &iroi_full, &oroi_full, delta, 10))
    {
      dt_print(DT_DEBUG_DEV, "[default_process_tiling_roi] can not handle requested roi's. tiling for module '%s' not possible.\n", self->op);
      goto error;
    }

    /* make sure that oroi_full at least covers the range of oroi_good.
       this step is needed due to the possibility of rounding errors */
    oroi_full.x = _min(oroi_full.x, oroi_good.x);
    oroi_full.y = _min(oroi_full.y, oroi_good.y);
    oroi_full.width = _max(oroi_full.width, oroi_good.x + oroi_good.width - oroi_full.x);
    oroi_full.height = _max(oroi_full.height, oroi_good.y + oroi_good.height - oroi_full.y);

    /* clamp to not exceed roi_out */
    oroi_full.x = _max(oroi_full.x, roi_out->x);
    oroi_full.y = _max(oroi_full.y, roi_out->y);
    oroi_full.width = _min(oroi_full.width, roi_out->width + roi_out->x - oroi_full.x);
    oroi_full.height = _min(oroi_full.height, roi_out->height + roi_out->y - oroi_full.y);

    /* calculate final iroi_full */
    self->modify_roi_in(self, piece, &oroi_full, &iroi_full);

    /* clamp to not exceed roi_in */
    iroi_full.x = _max(iroi_full.x, roi_in->x);
    iroi_full.y = _max(iroi_full.y, roi_in->y);
    iroi_full.width = _min(iroi_full.width, roi_in->width + roi_in->x - iroi_full.x);
    iroi_full.height = _min(iroi_full.height, roi_in->height + roi_in->y - iroi_full.y);


    //_print_roi(&iroi_full, "tile iroi_full");
    //_print_roi(&oroi_full, "tile oroi_full");

    /* offsets of tile into ivoid and ovoid */
    size_t ioffs = (iroi_full.y - roi_in->y)*ipitch + (iroi_full.x - roi_in->x)*in_bpp;
    size_t ooffs = (oroi_good.y - roi_out->y)*opitch + (oroi_good.x - roi_out->x)*out_bpp;

    dt_print(DT_DEBUG_DEV, "[default_process_tiling_roi] tile (%d, %d) with %d x %d at origin [%d, %d]\n", tx, ty, iroi_full.width, iroi_full.height, iroi_full.x, iroi_full.y);

    /* make sure data fits into input/output buffers */
    if(tile_wd_in*tile_ht_in < iroi_full.width*iroi_full.height || tile_wd_out*tile_ht_out < oroi_full.width*oroi_full.height)
    {
      dt_print(DT_DEBUG_DEV, "[default_process_tiling_roi] input/output buffers exceed estimated maximum size for module '%s'\n", self->op);
      dt_print(DT_DEBUG_DEV, "[default_process_tiling_roi] input given %d x %d vs needed: %d x %d\n", tile_wd_in, tile_ht_in, iroi_full.width, iroi_full.height);
      dt_print(DT_DEBUG_DEV, "[default_process_tiling_roi] output given: %d x %d vs needed: %d x %d\n", tile_wd_out, tile_ht_out, oroi_full.width, oroi_full.height);
      goto error;
    }

    /* prepare input tile buffer */
    assert(tile_wd_in*tile_ht_in >= iroi_full.width*iroi_full.height);
    assert(tile_wd_out*tile_ht_out >= oroi_full.width*oroi_full.height);
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(input,ivoid,ioffs,iroi_full) schedule(static)
#endif
    for(int j=0; j<iroi_full.height; j++)
      memcpy((char *)input+j*iroi_full.width*in_bpp, (char *)ivoid+ioffs+j*ipitch, iroi_full.width*in_bpp);

    /* take original processed_maximum as starting point */
    for(int k=0; k<3; k++)
      piece->pipe->processed_maximum[k] = processed_maximum_saved[k];

    /* call process() of module */
    self->process(self, piece, input, output, &iroi_full, &oroi_full);

    /* aggregate resulting processed_maximum */
    /* TODO: check if there really can be differences between tiles and take
             appropriate action (calculate minimum, maximum, average, ...?) */
    for(int k=0; k<3; k++)
    {
      if(tx+ty > 0 && fabs(processed_maximum_new[k] - piece->pipe->processed_maximum[k]) > 1.0e-6f)
        dt_print(DT_DEBUG_DEV, "[default_process_tiling_roi] processed_maximum[%d] differs between tiles in module '%s'\n", k, self->op);
      processed_maximum_new[k] = piece->pipe->processed_maximum[k];
    }


    /* copy "good" part of tile to output buffer */
    const int origin_x = oroi_good.x - oroi_full.x;
    const int origin_y = oroi_good.y - oroi_full.y;
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(ovoid,ooffs,output,oroi_good,oroi_full) schedule(static)
#endif
    for(int j=0; j<oroi_good.height; j++)
      memcpy((char *)ovoid+ooffs+j*opitch, (char *)output+((j+origin_y)*oroi_full.width+origin_x)*out_bpp, oroi_good.width*out_bpp);
  }

  /* copy back final processed_maximum */
  for(int k=0; k<3; k++)
    piece->pipe->processed_maximum[k] = processed_maximum_new[k];

  if(input != NULL) free(input);
  if(output != NULL) free(output);
  return;

error:
  dt_control_log(_("tiling failed for module '%s'. output might be garbled."), self->op);
  // fall through

fallback:
  if(input != NULL) free(input);
  if(output != NULL) free(output);
  dt_print(DT_DEBUG_DEV, "[default_process_tiling_roi] fall back to standard processing for module '%s'\n", self->op);
  self->process(self, piece, ivoid, ovoid, roi_in, roi_out);
  return;
}




/* if a module does not implement process_tiling() by itself, this function is called instead.
   _default_process_tiling_ptp() is able to handle standard cases where pixels do not change their places.
   _default_process_tiling_roi() takes care of all other cases where image gets distorted. */
void
default_process_tiling (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, const int in_bpp)
{
  if(memcmp(roi_in, roi_out, sizeof(struct dt_iop_roi_t)))
    _default_process_tiling_roi (self, piece, ivoid, ovoid, roi_in, roi_out, in_bpp);
  else
    _default_process_tiling_ptp (self, piece, ivoid, ovoid, roi_in, roi_out, in_bpp);
  return;
}



#ifdef HAVE_OPENCL
/* simple tiling algorithm for roi_in == roi_out, i.e. for pixel to pixel modules/operations */
static int
_default_process_tiling_cl_ptp (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, const int in_bpp)
{
  cl_int err = -999;
  cl_mem input = NULL;
  cl_mem output = NULL;


  const int devid = piece->pipe->devid;
  const int out_bpp = self->output_bpp(self, piece->pipe, piece);
  const int ipitch = roi_in->width * in_bpp;
  const int opitch = roi_out->width * out_bpp;
  const int max_bpp = _max(in_bpp, out_bpp);

  /* get tiling requirements of module */
  dt_develop_tiling_t tiling = { 0 };
  self->tiling_callback(self, piece, roi_in, roi_out, &tiling);


  /* calculate optimal size of tiles */
  float headroom = (float)dt_conf_get_int("opencl_memory_headroom")*1024.0f*1024.0f;
  headroom = fmin(fmax(headroom, 0.0f), (float)darktable.opencl->dev[devid].max_global_mem);
  const float available = darktable.opencl->dev[devid].max_global_mem - headroom;
  const float singlebuffer = fmin(fmax((available - tiling.overhead) / tiling.factor, 0.0f), darktable.opencl->dev[devid].max_mem_alloc);
  int width = _min(roi_in->width, darktable.opencl->dev[devid].max_image_width);
  int height = _min(roi_in->height, darktable.opencl->dev[devid].max_image_height);

  /* shrink tile size in case it would exceed singlebuffer size */
  if((float)width*height*max_bpp > singlebuffer)
  {
    const float scale = singlebuffer/(width*height*max_bpp);

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
  if(width < roi_in->width) width = (width / walign) * walign;
  if(height < roi_in->height) height = (height / halign) * halign;

  /* also make sure that overlap follows alignment rules by making it wider when needed */
  const int overlap = tiling.overlap % xyalign != 0 ? (tiling.overlap / xyalign + 1) * xyalign : tiling.overlap;


  /* calculate effective tile size */
  const int tile_wd = width - 2*overlap > 0 ? width - 2*overlap : 1;
  const int tile_ht = height - 2*overlap > 0 ? height - 2*overlap : 1;


  /* calculate number of tiles */
  const int tiles_x = width < roi_in->width ? ceilf(roi_in->width /(float)tile_wd) : 1;
  const int tiles_y = height < roi_in->height ? ceilf(roi_in->height/(float)tile_ht) : 1;

  /* sanity check: don't run wild on too many tiles */
  if(tiles_x * tiles_y > DT_TILING_MAXTILES)
  {
    dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_ptp] aborted tiling for module '%s'. too many tiles: %d x %d\n", self->op, tiles_x, tiles_y);
    return FALSE;
  }


  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_ptp] use tiling on module '%s' for image with full size %d x %d\n", self->op, roi_in->width, roi_in->height);
  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_ptp] (%d x %d) tiles with max dimensions %d x %d and overlap %d\n", tiles_x, tiles_y, width, height, overlap);


  /* store processed_maximum to be re-used and aggregated */
  float processed_maximum_saved[3];
  float processed_maximum_new[3] = { 1.0f };
  for(int k=0; k<3; k++)
    processed_maximum_saved[k] = piece->pipe->processed_maximum[k];

#if 0
  /* get opencl input and output buffers, to be re-used for all tiles.
     For "end-tiles" these buffers will only be partly filled; the acutally used part
     is then correctly reflected in iroi and oroi which we give to the respective
     process_cl(). Attention! opencl kernels may not simply read beyond limits (given by width and height)
     as they can no longer rely on CLK_ADDRESS_CLAMP_TO_EDGE to give reasonable results! */
  input = dt_opencl_alloc_device(devid, width, height, in_bpp);
  if(input == NULL) goto error;
  output = dt_opencl_alloc_device(devid, width, height, out_bpp);
  if(output == NULL) goto error;
#endif

  /* iterate over tiles */
  for(int tx=0; tx<tiles_x; tx++)
    for(int ty=0; ty<tiles_y; ty++)  
  {
    size_t wd = tx * tile_wd + width > roi_in->width  ? roi_in->width - tx * tile_wd : width;
    size_t ht = ty * tile_ht + height > roi_in->height ? roi_in->height- ty * tile_ht : height;

    /* no need to process (end)tiles that are smaller than overlap */
    if((wd <= overlap && tx > 0) || (ht <= overlap && ty > 0)) continue;

    /* origin and region of effective part of tile, which we want to store later */
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { wd, ht, 1 };

    /* roi_in and roi_out for process_cl on subbuffer */
    dt_iop_roi_t iroi = { roi_in->x+tx*tile_wd, roi_in->y+ty*tile_ht, wd, ht, roi_in->scale };
    dt_iop_roi_t oroi = { roi_out->x+tx*tile_wd, roi_out->y+ty*tile_ht, wd, ht, roi_out->scale };


    /* offsets of tile into ivoid and ovoid */
    size_t ioffs = (ty * tile_ht)*ipitch + (tx * tile_wd)*in_bpp;
    size_t ooffs = (ty * tile_ht)*opitch + (tx * tile_wd)*out_bpp;


    dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_ptp] tile (%d, %d) with %d x %d at origin [%d, %d]\n", tx, ty, wd, ht, tx*tile_wd, ty*tile_ht);

    /* get input and output buffers */
    input = dt_opencl_alloc_device(devid, wd, ht, in_bpp);
    if(input == NULL) goto error;
    output = dt_opencl_alloc_device(devid, wd, ht, out_bpp);
    if(output == NULL) goto error;

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
        dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_ptp] processed_maximum[%d] differs between tiles in module '%s'\n", k, self->op);
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

    /* release input and output buffers */
    dt_opencl_release_mem_object(input);
    input = NULL;
    dt_opencl_release_mem_object(output);
    output = NULL;

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
  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_opencl_ptp] couldn't run process_cl() for module '%s' in tiling mode: %d\n", self->op, err);
  return FALSE;
}


/* more elaborate tiling algorithm for roi_in != roi_out: slower than the ptp variant,
   more tiles and larger overlap */
static int
_default_process_tiling_cl_roi (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, const int in_bpp)
{
  cl_int err = -999;
  cl_mem input = NULL;
  cl_mem output = NULL;

  //_print_roi(roi_in, "module roi_in");
  //_print_roi(roi_out, "module roi_out");

  const int devid = piece->pipe->devid;
  const int out_bpp = self->output_bpp(self, piece->pipe, piece);
  const int ipitch = roi_in->width * in_bpp;
  const int opitch = roi_out->width * out_bpp;
  const int max_bpp = _max(in_bpp, out_bpp);

  /* inaccuracy for roi_in elements in roi_out -> roi_in calculations */
  const int delta = ceilf(roi_in->scale / roi_out->scale);

  /* estimate for additional (space) requirement in buffer dimensions due to inaccuracies */
  const int inacc = RESERVE*delta;


  /* get tiling requirements of module */
  dt_develop_tiling_t tiling = { 0 };
  self->tiling_callback(self, piece, roi_in, roi_out, &tiling);


  /* calculate optimal size of tiles */
  float headroom = (float)dt_conf_get_int("opencl_memory_headroom")*1024*1024;
  headroom = fmin(fmax(headroom, 0.0f), (float)darktable.opencl->dev[devid].max_global_mem);
  const float available = darktable.opencl->dev[devid].max_global_mem - headroom;
  const float singlebuffer = fmin(fmax((available - tiling.overhead) / tiling.factor, 0.0f), darktable.opencl->dev[devid].max_mem_alloc);
  int width = _min(_max(roi_in->width, roi_out->width), darktable.opencl->dev[devid].max_image_width);
  int height = _min(_max(roi_in->height, roi_out->height), darktable.opencl->dev[devid].max_image_height);

  /* shrink tile size in case it would exceed singlebuffer size */
  if((float)width*height*max_bpp > singlebuffer)
  {
    const float scale = singlebuffer/(width*height*max_bpp);

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
     direction. Additional alignment requirements are set via definition of CL_ALIGNMENT. */

  /* for simplicity reasons we use only one alignment that fits to x and y requirements at the same time */
  unsigned int xyalign = _lcm(tiling.xalign, tiling.yalign);
  xyalign = _lcm(xyalign, CL_ALIGNMENT);

  assert(xyalign != 0);

  /* make sure that overlap follows alignment rules by making it wider when needed.
     overlap_in needs to be aligned, overlap_out is only here to calculate output buffer size */
  const int overlap_in = _align_up(tiling.overlap, xyalign);
  const int overlap_out = ceilf(overlap_in * roi_out->scale / roi_in->scale);

  int tiles_x = 1, tiles_y = 1;

  /* calculate number of tiles taking the larger buffer (input or output) as a guiding one.
     normally it is roi_in > roi_out; but let's be prepared */
  if(roi_in->width > roi_out->width)
    tiles_x = width < roi_in->width ? ceilf((float)roi_in->width / (float)_max(width - 2*overlap_in - inacc, 1)) : 1;
  else
    tiles_x = width < roi_out->width ? ceilf((float)roi_out->width / (float)_max(width - 2*overlap_out, 1)) : 1;

  if(roi_in->height > roi_out->height)
    tiles_y = height < roi_in->height ? ceilf((float)roi_in->height / (float)_max(height - 2*overlap_in - inacc, 1)) : 1;
  else
    tiles_y = height < roi_out->height ? ceilf((float)roi_out->height / (float)_max(height - 2*overlap_out, 1)) : 1;

  /* sanity check: don't run wild on too many tiles */
  if(tiles_x * tiles_y > DT_TILING_MAXTILES)
  {
    dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_roi] aborted tiling for module '%s'. too many tiles: %d x %d\n", self->op, tiles_x, tiles_y);
    return FALSE;
  }

  /* calculate tile width and height excl. overlap (i.e. the good part) for output.
     important for all following processing steps. */
  const int tile_wd = roi_out->width % tiles_x == 0 ? roi_out->width / tiles_x : roi_out->width / tiles_x + 1;
  const int tile_ht = roi_out->height % tiles_y == 0 ? roi_out->height / tiles_y : roi_out->height / tiles_y + 1;


  /* calculate tile width and height incl. output overlap with some additional reserve.
     only needed to select sufficient output buffer size */
  const int tile_wd_out = _min(roi_out->width / tiles_x + 2*overlap_out, roi_out->width) + RESERVE;
  const int tile_ht_out = _min(roi_out->height / tiles_y + 2*overlap_out, roi_out->height) + RESERVE;

  /* calculate tile width and height incl. input overlap with some additional reserve.
     only needed to select sufficient input buffer size */
  const int tile_wd_in = _min(tile_wd_out * roi_in->scale / roi_out->scale, roi_in->width);
  const int tile_ht_in = _min(tile_ht_out * roi_in->scale / roi_out->scale, roi_in->height);

  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_roi] use tiling on module '%s' for image with full input size %d x %d\n", self->op, roi_in->width, roi_in->height);
  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_roi] (%d x %d) tiles with max input dimensions %d x %d\n", tiles_x, tiles_y, tile_wd_in, tile_ht_in);


  /* store processed_maximum to be re-used and aggregated */
  float processed_maximum_saved[3];
  float processed_maximum_new[3] = { 1.0f };
  for(int k=0; k<3; k++)
    processed_maximum_saved[k] = piece->pipe->processed_maximum[k];


#if 0
  /* get opencl input and output buffers, to be re-used for all tiles.
     These buffers will only be partly filled. */
  input = dt_opencl_alloc_device(devid, tile_wd_in, tile_ht_in, in_bpp);
  if(input == NULL) goto error;
  output = dt_opencl_alloc_device(devid, tile_wd_out, tile_ht_out, out_bpp);
  if(output == NULL) goto error;
#endif

  /* iterate over tiles */
  for(int tx=0; tx<tiles_x; tx++)
    for(int ty=0; ty<tiles_y; ty++)  
  {
    /* the output dimensions of the good part of this specific tile */
    size_t wd = (tx + 1) * tile_wd > roi_out->width  ? roi_out->width - tx * tile_wd : tile_wd;
    size_t ht = (ty + 1) * tile_ht > roi_out->height ? roi_out->height- ty * tile_ht : tile_ht;

    /* roi_in and roi_out of good part: oroi_good easy to calculate based on number and dimension of tile.
       iroi_good is calculated by modify_roi_in() of respective module */
    dt_iop_roi_t iroi_good = { roi_in->x+tx*tile_wd, roi_in->y+ty*tile_ht, wd, ht, roi_in->scale };
    dt_iop_roi_t oroi_good = { roi_out->x+tx*tile_wd, roi_out->y+ty*tile_ht, wd, ht, roi_out->scale };

    self->modify_roi_in(self, piece, &oroi_good, &iroi_good);

    //_print_roi(&iroi_good, "tile iroi_good");
    //_print_roi(&oroi_good, "tile oroi_good");

    /* now we need to calculate full region of this tile: increase input roi to take care of overlap requirements 
       and alignment and add additional delta to correct for possible rounding errors in modify_roi_in() 
       -> generates first estimate of iroi_full */
    const int x_in = iroi_good.x;
    const int y_in = iroi_good.y;
    const int width_in = iroi_good.width;
    const int height_in = iroi_good.height;
    const int new_x_in = _max(_align_down(x_in - overlap_in - delta, xyalign), roi_in->x);
    const int new_y_in = _max(_align_down(y_in - overlap_in - delta, xyalign), roi_in->y);
    const int new_width_in = _min(_align_up(width_in + overlap_in + delta + (x_in - new_x_in), xyalign), roi_in->width + roi_in->x - new_x_in);
    const int new_height_in = _min(_align_up(height_in + overlap_in + delta + (y_in - new_y_in), xyalign), roi_in->height + roi_in->y - new_y_in);

    /* iroi_full based on calculated numbers and dimensions. oroi_full just set as a starting point for the following iterative search */
    dt_iop_roi_t iroi_full = { new_x_in, new_y_in, new_width_in, new_height_in, iroi_good.scale };
    dt_iop_roi_t oroi_full = { roi_out->x, roi_out->y, roi_out->width, roi_out->height, oroi_good.scale };

    //_print_roi(&iroi_full, "tile iroi_full before optimization");
    //_print_roi(&oroi_full, "tile oroi_full before optimization");

    /* try to find a matching oroi_full */
    if (!_fit_output_to_input_roi(self, piece, &iroi_full, &oroi_full, delta, 10))
    {
      dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_roi] can not handle requested roi's. tiling for module '%s' not possible.\n", self->op);
      goto error;
    }


    /* make sure that oroi_full at least covers the range of oroi_good.
       this step is needed due to the possibility of rounding errors */
    oroi_full.x = _min(oroi_full.x, oroi_good.x);
    oroi_full.y = _min(oroi_full.y, oroi_good.y);
    oroi_full.width = _max(oroi_full.width, oroi_good.x + oroi_good.width - oroi_full.x);
    oroi_full.height = _max(oroi_full.height, oroi_good.y + oroi_good.height - oroi_full.y);

    /* clamp to not exceed roi_out */
    oroi_full.x = _max(oroi_full.x, roi_out->x);
    oroi_full.y = _max(oroi_full.y, roi_out->y);
    oroi_full.width = _min(oroi_full.width, roi_out->width + roi_out->x - oroi_full.x);
    oroi_full.height = _min(oroi_full.height, roi_out->height + roi_out->y - oroi_full.y);


    /* calculate final iroi_full */
    self->modify_roi_in(self, piece, &oroi_full, &iroi_full);

    /* clamp to not exceed roi_in */
    iroi_full.x = _max(iroi_full.x, roi_in->x);
    iroi_full.y = _max(iroi_full.y, roi_in->y);
    iroi_full.width = _min(iroi_full.width, roi_in->width + roi_in->x - iroi_full.x);
    iroi_full.height = _min(iroi_full.height, roi_in->height + roi_in->y - iroi_full.y);

    //_print_roi(&iroi_full, "tile iroi_full");
    //_print_roi(&oroi_full, "tile oroi_full");

    /* offsets of tile into ivoid and ovoid */
    size_t ioffs = (iroi_full.y - roi_in->y)*ipitch + (iroi_full.x - roi_in->x)*in_bpp;
    size_t ooffs = (oroi_good.y - roi_out->y)*opitch + (oroi_good.x - roi_out->x)*out_bpp;

    dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_roi] tile (%d, %d) with %d x %d at origin [%d, %d]\n", tx, ty, iroi_full.width, iroi_full.height, iroi_full.x, iroi_full.y);

    /* origin and region of full input tile */
    size_t iorigin[] = { 0, 0, 0 };
    size_t iregion[] = { iroi_full.width, iroi_full.height, 1 };

    /* origin and region of good part of output tile */
    size_t oorigin[] = { oroi_good.x - oroi_full.x, oroi_good.y - oroi_full.y, 0 };
    size_t oregion[] = { oroi_good.width, oroi_good.height, 1 };


    /* make sure data fits into input/output buffers */
    if(tile_wd_in*tile_ht_in < iroi_full.width*iroi_full.height || tile_wd_out*tile_ht_out < oroi_full.width*oroi_full.height)
    {
      dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_roi] input/output buffers exceed estimated maximum size for module '%s'\n", self->op);
      dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_roi] input given %d x %d vs needed: %d x %d\n", tile_wd_in, tile_ht_in, iroi_full.width, iroi_full.height);
      dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_roi] output given: %d x %d vs needed: %d x %d\n", tile_wd_out, tile_ht_out, oroi_full.width, oroi_full.height);
      goto error;
    }

    /* get opencl input and output buffers */
    input = dt_opencl_alloc_device(devid, iroi_full.width, iroi_full.height, in_bpp);
    if(input == NULL) goto error;

    output = dt_opencl_alloc_device(devid, oroi_full.width, oroi_full.height, out_bpp);
    if(output == NULL) goto error;

    /* non-blocking memory transfer: host input buffer -> opencl/device tile */
    err = dt_opencl_write_host_to_device_raw(devid, (char *)ivoid + ioffs, input, iorigin, iregion, ipitch, CL_FALSE);
    if(err != CL_SUCCESS) goto error;

    /* take original processed_maximum as starting point */
    for(int k=0; k<3; k++)
      piece->pipe->processed_maximum[k] = processed_maximum_saved[k];

    /* call process_cl of module */
    if(!self->process_cl(self, piece, input, output, &iroi_full, &oroi_full)) goto error;

    /* aggregate resulting processed_maximum */
    /* TODO: check if there really can be differences between tiles and take
             appropriate action (calculate minimum, maximum, average, ...?) */
    for(int k=0; k<3; k++)
    {
      if(tx+ty > 0 && fabs(processed_maximum_new[k] - piece->pipe->processed_maximum[k]) > 1.0e-6f)
        dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_cl_roi] processed_maximum[%d] differs between tiles in module '%s'\n", k, self->op);
      processed_maximum_new[k] = piece->pipe->processed_maximum[k];
    }

    /* non-blocking memory transfer: opencl/device tile -> host output buffer */
    err = dt_opencl_read_host_from_device_raw(devid, (char *)ovoid + ooffs, output, oorigin, oregion, opitch, CL_FALSE);
    if(err != CL_SUCCESS) goto error;

    /* release input and output buffers */
    dt_opencl_release_mem_object(input);
    input = NULL;
    dt_opencl_release_mem_object(output);
    output = NULL;

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
  dt_print(DT_DEBUG_OPENCL, "[default_process_tiling_opencl_roi] couldn't run process_cl() for module '%s' in tiling mode: %d\n", self->op, err);
  return FALSE;
}




/* if a module does not implement process_tiling_cl() by itself, this function is called instead.
   _default_process_tiling_cl_ptp() is able to handle standard cases where pixels do not change their places.
   _default_process_tiling_cl_roi() takes care of all other cases where image gets distorted. */
int
default_process_tiling_cl (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, const int in_bpp)
{
  if(memcmp(roi_in, roi_out, sizeof(struct dt_iop_roi_t)))
    return _default_process_tiling_cl_roi(self, piece, ivoid, ovoid, roi_in, roi_out, in_bpp);
  else
    return _default_process_tiling_cl_ptp(self, piece, ivoid, ovoid, roi_in, roi_out, in_bpp);
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

