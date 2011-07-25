/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include "common/debug.h"
#include "control/conf.h"
#include "gui/draw.h"
#include "gui/presets.h"
#include "gui/gtk.h"
#include "dtgtk/slider.h"
#include "control/control.h"
#include <memory.h>
#include <stdlib.h>
#include <xmmintrin.h>
// SSE4 actually not used yet.
// #include <smmintrin.h>

#define INSET 5
#define INFL .3f

DT_MODULE(1)

#define BANDS 6
#define MAX_NUM_SCALES 8 // 2*2^(i+1) + 1 = 1025px support for i = 8
#define RES 64

#define dt_atrous_show_upper_label(cr, text, ext) 	cairo_text_extents (cr, text, &ext);\
													cairo_move_to (cr, .5*(width-ext.width), .08*height);\
													cairo_show_text(cr, text);


#define dt_atrous_show_lower_label(cr, text, ext) 	cairo_text_extents (cr, text, &ext);\
													cairo_move_to (cr, .5*(width-ext.width), .98*height);\
													cairo_show_text(cr, text);

#define min(a,b)	((a) < (b) ? (a) : (b))

typedef enum atrous_channel_t
{
  atrous_L    = 0,  // luminance boost
  atrous_c    = 1,  // chrominance boost
  atrous_s    = 2,  // edge sharpness
  atrous_Lt   = 3,  // luminance noise threshold
  atrous_ct   = 4,  // chrominance noise threshold
  atrous_none = 5
}
atrous_channel_t;

typedef struct dt_iop_atrous_params_t
{
  int32_t octaves;
  float x[atrous_none][BANDS], y[atrous_none][BANDS];
}
dt_iop_atrous_params_t;

typedef struct dt_iop_atrous_gui_data_t
{
  GtkWidget *mix;
  GtkDrawingArea *area;
  GtkComboBox *presets;
  GtkNotebook* channel_tabs;
  double mouse_x, mouse_y, mouse_pick;
  float mouse_radius;
  dt_iop_atrous_params_t drag_params;
  int dragging;
  int x_move;
  dt_draw_curve_t *minmax_curve;
  atrous_channel_t channel, channel2;
  float draw_xs[RES], draw_ys[RES];
  float draw_min_xs[RES], draw_min_ys[RES];
  float draw_max_xs[RES], draw_max_ys[RES];
  float band_hist[MAX_NUM_SCALES];
  float band_max;
  float sample[MAX_NUM_SCALES];
  int   num_samples;
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
  dt_draw_curve_t *curve[atrous_none];
}
dt_iop_atrous_data_t;

const char *
name()
{
  return _("equalizer");
}

int
groups ()
{
  return IOP_GROUP_CORRECT;
}

int
flags ()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

static __m128
weight (const float *c1, const float *c2, const float sharpen)
{
  const float wc = dt_fast_expf(-((c1[1] - c2[1])*(c1[1] - c2[1]) + (c1[2] - c2[2])*(c1[2] - c2[2])) * sharpen);
  const float wl = dt_fast_expf(-(c1[0] - c2[0])*(c1[0] - c2[0]) * sharpen);
  return _mm_set_ps(1.0f, wc, wc, wl);
}

static void
eaw_decompose (float *const out, const float *const in, float *const detail, const int scale,
               const float sharpen, const int32_t width, const int32_t height)
{
  const int mult = 1<<scale;
  const float filter[5] = {1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f};

#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=0; j<height; j++)
  {
    const __m128 *px = ((__m128 *)in) + j*width;
    float *pdetail = detail + 4*j*width;
    float *pcoarse = out + 4*j*width;
    for(int i=0; i<width; i++)
    {
      // TODO: prefetch? _mm_prefetch()
      __m128 sum = _mm_setzero_ps(), wgt = _mm_setzero_ps();
      for(int jj=0; jj<5; jj++) for(int ii=0; ii<5; ii++)
        {
          const int iii = ii-2;
          const int jjj = jj-2;
          int x = i + mult*iii, y = j + mult*jjj;
          // if(x < 0 || x >= width || y < 0 || y >= height) continue;
          // clamp to edge
          if(x < 0)       x = 0;
          if(x >= width)  x = width  - 1;
          if(y < 0)       y = 0;
          if(y >= height) y = height - 1;
          const __m128 *px2 = ((__m128 *)in) + x + y*width;
          const __m128 w = _mm_mul_ps(_mm_set1_ps(filter[ii]*filter[jj]), weight((float *)px, (float *)px2, sharpen));
          sum = _mm_add_ps(sum, _mm_mul_ps(w, *px2));
          wgt = _mm_add_ps(wgt, w);
        }
      // sum = _mm_div_ps(sum, wgt);
      sum = _mm_mul_ps(sum, _mm_rcp_ps(wgt)); // less precise, but faster

      // memcpy is a tad slower than writing without updating caches:
      // const __m128 d = _mm_sub_ps(*px, sum);
      // memcpy(pdetail, &d, sizeof(float)*4);
      // memcpy(pcoarse, &sum, sizeof(float)*4);
      _mm_stream_ps(pdetail, _mm_sub_ps(*px, sum));
      _mm_stream_ps(pcoarse, sum);
      px++;
      pdetail+=4;
      pcoarse+=4;
    }
  }
  _mm_sfence();
}

static void
eaw_synthesize (float *const out, const float *const in, const float *const detail,
                const float *thrsf, const float *boostf, const int32_t width, const int32_t height)
{
  const __m128 threshold = _mm_set_ps(thrsf[3], thrsf[2], thrsf[1], thrsf[0]);
  const __m128 boost     = _mm_set_ps(boostf[3], boostf[2], boostf[1], boostf[0]);

#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=0; j<height; j++)
  {
    // TODO: prefetch? _mm_prefetch()
    const __m128 *pin = (__m128 *)in + j*width;
    __m128 *pdetail = (__m128 *)detail + j*width;
    float *pout = out + 4*j*width;
    for(int i=0; i<width; i++)
    {
      const __m128i maski = _mm_set1_epi32(0x80000000u);
      const __m128 *mask = (__m128*)&maski;
      const __m128 absamt = _mm_max_ps(_mm_setzero_ps(), _mm_sub_ps(_mm_andnot_ps(*mask, *pdetail), threshold));
      const __m128 amount = _mm_or_ps(_mm_and_ps(*pdetail, *mask), absamt);
      _mm_stream_ps(pout, _mm_add_ps(*pin, _mm_mul_ps(boost, amount)));
      pdetail ++;
      pin ++;
      pout += 4;
    }
  }
  _mm_sfence();
}

static int
get_samples (float *t, const dt_iop_atrous_data_t *const d, const dt_iop_roi_t *roi_in, const dt_dev_pixelpipe_iop_t *const piece)
{
  const float scale = roi_in->scale;
  const float supp0 = MIN(2*(2<<(MAX_NUM_SCALES-1)) + 1, MAX(piece->buf_in.height, piece->buf_in.width) * 0.2f);
  const float i0 = dt_log2f((supp0 - 1.0f)*.5f);
  int i=0;
  for(; i<MAX_NUM_SCALES; i++)
  {
    // actual filter support on scaled buffer
    const float supp  = 2*(2<<i) + 1;
    // approximates this filter size on unscaled input image:
    const float supp_in = supp * (1.0f/scale);
    const float i_in = dt_log2f((supp_in - 1)*.5f) - 1.0f;
    t[i] = 1.0f - (i_in+.5f)/i0;
    if(t[i] < 0.0f) break;
  }
  return i;
}

static int
get_scales (float (*thrs)[4], float (*boost)[4], float *sharp, const dt_iop_atrous_data_t *const d, const dt_iop_roi_t *roi_in, const dt_dev_pixelpipe_iop_t *const piece)
{
  // we want coeffs to span max 20% of the image
  // finest is 5x5 filter
  //
  // 1:1 : w=20% buf_in.width                     w=5x5
  //     : ^ ...            ....            ....  ^
  // buf :  17x17  9x9  5x5     2*2^k+1
  // .....
  // . . . . .
  // .   .   .   .   .
  // cut off too fine ones, if image is not detailed enough (due to roi_in->scale)
  const float scale = roi_in->scale/piece->iscale;
  // largest desired filter on input buffer (20% of input dim)
  const float supp0 = MIN(2*(2<<(MAX_NUM_SCALES-1)) + 1, MAX(piece->buf_in.height*piece->iscale, piece->buf_in.width*piece->iscale) * 0.2f);
  const float i0 = dt_log2f((supp0 - 1.0f)*.5f);
  int i=0;
  for(; i<MAX_NUM_SCALES; i++)
  {
    // actual filter support on scaled buffer
    const float supp  = 2*(2<<i) + 1;
    // approximates this filter size on unscaled input image:
    const float supp_in = supp * (1.0f/scale);
    const float i_in = dt_log2f((supp_in - 1)*.5f) - 1.0f;
    // i_in = max_scale .. .. .. 0
    const float t = 1.0f - (i_in+.5f)/i0;
    boost[i][3] = boost[i][0] = 2.0f*dt_draw_curve_calc_value(d->curve[atrous_L], t);
    boost[i][1] = boost[i][2] = 2.0f*dt_draw_curve_calc_value(d->curve[atrous_c], t);
    for(int k=0; k<4; k++) boost[i][k] *= boost[i][k];
    thrs [i][0] = thrs [i][3] = powf(2.0f, -7.0f*(1.0f-t)) * 10.0f*dt_draw_curve_calc_value(d->curve[atrous_Lt], t);
    thrs [i][1] = thrs [i][2] = powf(2.0f, -7.0f*(1.0f-t)) * 20.0f*dt_draw_curve_calc_value(d->curve[atrous_ct], t);
    sharp[i]    = 0.0025f*dt_draw_curve_calc_value(d->curve[atrous_s], t);
    // printf("scale %d boost %f %f thrs %f %f sharpen %f\n", i, boost[i][0], boost[i][2], thrs[i][0], thrs[i][1], sharp[i]);
    if(t < 0.0f) break;
  }
  return i;
}

void
process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)piece->data;
  float thrs [MAX_NUM_SCALES][4];
  float boost[MAX_NUM_SCALES][4];
  float sharp[MAX_NUM_SCALES];
  const int max_scale = get_scales(thrs, boost, sharp, d, roi_in, piece);

  if(self->dev->gui_attached && piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    dt_iop_atrous_gui_data_t *g = (dt_iop_atrous_gui_data_t *)self->gui_data;
    g->num_samples = get_samples (g->sample, d, roi_in, piece);
    // tries to acquire gdk lock and this prone to deadlock:
    // dt_control_queue_draw(GTK_WIDGET(g->area));
  }

  float *detail = NULL;
  float *tmp    = NULL;
  float *tmp2   = NULL;
  float *buf2   = NULL;
  float *buf1   = NULL;

  // should be >= DT_IMAGE_WINDOW_SIZE, as dr mode won't use tiling then.
  // border is with MAX_NUM_SCALES=8 502 pixels, the ratio (tile_wd_full-border)/tile_wd_full*(same for y) is directly proportional to the slowdown.
  // int tile_wd_full = DT_IMAGE_WINDOW_SIZE, tile_ht_full = DT_IMAGE_WINDOW_SIZE;
  // results in 3x2 for 5D mk II 21MPix images (24s 1300^2 -> 20s full export)
  // and        2x1 for 12MPix (14s -> 7s)
  int tile_wd_full = 2390, tile_ht_full = 2390;
  // int tile_wd_full = 3350, tile_ht_full = 3350; // results in 2x2 for 5D mk II 21MPix images (24s 1300^2 -> 19s full export)
  const int need_tiles = roi_in->width > tile_wd_full || roi_out->height > tile_ht_full;

  if(need_tiles)
  {
    tmp2 = (float *)dt_alloc_align(64, sizeof(float)*4*tile_wd_full*tile_ht_full);
    buf1 = tmp2;
  }
  else
  {
    tile_wd_full = roi_out->width;
    tile_ht_full = roi_out->height;
    buf1   = (float *)i;
  }
  detail = (float *)dt_alloc_align(64, sizeof(float)*4*tile_wd_full*tile_ht_full*max_scale);
  tmp    = (float *)dt_alloc_align(64, sizeof(float)*4*tile_wd_full*tile_ht_full);
  buf2   = tmp;

  const int max_filter_radius = (1<<max_scale); // 2 * 2^max_scale
  const int width = roi_out->width, height = roi_out->height;
  const int tile_wd = tile_wd_full - 2*max_filter_radius, tile_ht = tile_ht_full - 2*max_filter_radius;
  const int tiles_x = need_tiles ? ceilf(width /(float)tile_wd) : 1;
  const int tiles_y = need_tiles ? ceilf(height/(float)tile_ht) : 1;

  // printf("need %d x %d tiles with %d x %d (%d x %d) resolution\n", tiles_x, tiles_y, tile_wd, tile_ht, tile_wd_full, tile_ht_full);

  // for all tiles:
  for(int tx=0; tx<tiles_x; tx++)
    for(int ty=0; ty<tiles_y; ty++)
    {
      // size_t orig0[3] = {0, 0, 0};
      // size_t origin[3] = {tx*tile_wd, ty*tile_ht, 0};
      int orig0_x = 0, orig0_y = 0;
      int origin_x = tx*tile_wd, origin_y = ty*tile_ht;
      int wd = origin_x + tile_wd_full > width  ? width  - origin_x : tile_wd_full;
      int ht = origin_y + tile_ht_full > height ? height - origin_y : tile_ht_full;
      int vwd = wd, vht = ht; // valid region
      // size_t region[3] = {wd, ht, 1};

      if(need_tiles)
      {
        for(int j=0; j<ht; j++)
          memcpy(tmp2 + j*4*wd, ((float *)i) + ((origin_y + j)*roi_in->width + origin_x)*4, sizeof(float)*4*wd);
        buf1 = tmp2;
        buf2 = tmp;
      }
      if(tx > 0)
      {
        origin_x += max_filter_radius;
        orig0_x += max_filter_radius;
        vwd -= max_filter_radius;
      }
      if(ty > 0)
      {
        origin_y += max_filter_radius;
        orig0_y += max_filter_radius;
        vht -= max_filter_radius;
      }

      // FIXME: can vwd/vht be < 0?
      if(vwd <= 0 || vht <= 0) continue;

      for(int scale=0; scale<max_scale; scale++)
      {
        eaw_decompose (buf2, buf1, detail + 4*wd*ht*scale, scale, sharp[scale], wd, ht);
        if(!need_tiles && scale == 0) buf1 = (float *)o;
        float *buf3 = buf2;
        buf2 = buf1;
        buf1 = buf3;
      }

      for(int scale=max_scale-1; scale>=0; scale--)
      {
        eaw_synthesize (buf2, buf1, detail + 4*wd*ht*scale,
                        thrs[scale], boost[scale], wd, ht);
        float *buf3 = buf2;
        buf2 = buf1;
        buf1 = buf3;
      }

      if(need_tiles)
      {
        for(int j=0; j<vht; j++)
          memcpy(((float *)o) + ((origin_y + j)*roi_out->width + origin_x)*4, buf1 + 4*(orig0_x + (orig0_y + j)*wd), sizeof(float)*4*vwd);
      }
    }

  free(detail);
  free(tmp);
  free(tmp2);
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem _dev_in, cl_mem _dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)piece->data;
  float thrs [MAX_NUM_SCALES][4];
  float boost[MAX_NUM_SCALES][4];
  float sharp[MAX_NUM_SCALES];
  const int max_scale = get_scales(thrs, boost, sharp, d, roi_in, piece);

  if(self->dev->gui_attached && piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    dt_iop_atrous_gui_data_t *g = (dt_iop_atrous_gui_data_t *)self->gui_data;
    g->num_samples = get_samples (g->sample, d, roi_in, piece);
    // tries to acquire gdk lock and this prone to deadlock:
    // dt_control_queue_draw(GTK_WIDGET(g->area));
  }

  dt_iop_atrous_global_data_t *gd = (dt_iop_atrous_global_data_t *)self->data;
  // global scale is roi scale and pipe input prescale

  const int devid = piece->pipe->devid;
  cl_int err = -999;
  cl_mem mdev_in = NULL;
  cl_mem mdev_out = NULL;
  cl_mem dev_detail = NULL;
  cl_mem mdev_detail[max_scale];
  float *detail_buffer = NULL;
  int lowmem = 0;

  for(int k=0; k<max_scale; k++) mdev_detail[k] = NULL;

  // Try to estimate free memory on GPU: maximum global mem minus two full buffers (in + out) with current image dimensions
  // This can only be valid if there are no other applications consuming GPU memory, but there is no way for us to tell :(
  unsigned int max_free_mem_mb = (dt_opencl_get_max_global_mem(devid) - 2*roi_out->width*roi_out->height*4*sizeof(float)) >> 20;

  //dt_print(DT_DEBUG_OPENCL, "[opencl_atrous] maximum free memory %dMB on device %d\n", max_free_mem_mb, devid);

  // pure heuristic limits
  if(max_free_mem_mb < 300) goto error;  // no chance -> give back to pixelpipe and hope for the CPU path to take over
  if(max_free_mem_mb < 500) lowmem = 1;  // tight but managable with serious penalty on memory transfers

  size_t sizes[3];
  // err = dt_opencl_get_max_work_item_sizes(devid, sizes);
  // if(err != CL_SUCCESS) fprintf(stderr, "could not get max size! %d\n", err);
  // use WINDOW_SIZE instead of max threads (in, out, 7 detail = 232 MB GPU mem)
  sizes[0] = min(roi_out->width, DT_IMAGE_WINDOW_SIZE);
  sizes[1] = min(roi_out->height, DT_IMAGE_WINDOW_SIZE);
  sizes[2] = 1;

  // allocate device memory, if needed. else use input from pipe directly.
  const int need_tiles = roi_in->width > sizes[0] || roi_out->height > sizes[1];
  cl_mem dev_in = _dev_in, dev_out = _dev_out;
  if(need_tiles)
  {
    mdev_in  = dt_opencl_alloc_device(devid, sizes[0], sizes[1], 4*sizeof(float));
    if (mdev_in == NULL) goto error;
    mdev_out = dt_opencl_alloc_device(devid, sizes[0], sizes[1], 4*sizeof(float));
    if (mdev_out == NULL) goto error;
    dev_in = mdev_in;
    dev_out = mdev_out;
  }
  else
  {
    sizes[0] = roi_in->width;
    sizes[1] = roi_in->height;
  }

  const int max_filter_radius = (1<<max_scale); // 2 * 2^max_scale
  const int width = roi_out->width;
  const int height = roi_out->height;
  const int tile_wd = sizes[0] - 2*max_filter_radius;
  const int tile_ht = sizes[1] - 2*max_filter_radius;
  const int tiles_x = (need_tiles && (sizes[0] != width)) ? ceilf(width /(float)tile_wd) : 1;
  const int tiles_y = (need_tiles && (sizes[1] != height)) ? ceilf(height/(float)tile_ht) : 1;

  //printf("\natrous run for %d x % d image with %d x %d tiles\n", roi_out->width, roi_out->height, tiles_x, tiles_y);


  // details for local contrast enhancement:
  if(lowmem)
  {
    // GPU memory is tight -> allocate storage buffer for detail in core memory and use only small
    // intermediate GPU buffer
    detail_buffer = (float *)dt_alloc_align(64, 4*sizeof(float)*sizes[0]*sizes[1]*(max_scale-1));
    mdev_detail[0] = dt_opencl_alloc_device(devid, sizes[0], sizes[1], 4*sizeof(float));
    if (detail_buffer == NULL || mdev_detail[0] == NULL) goto error;
    dev_detail = mdev_detail[0];
  }
  else
  {
    // hopefully enough GPU memory
    for(int k=0; k<max_scale; k++)
    {
      mdev_detail[k] = dt_opencl_alloc_device(devid, sizes[0], sizes[1], 4*sizeof(float));
      if (mdev_detail[k] == NULL) goto error;
    }
  }


  // FIXME: boundary handling inside tiles!
  // FIXME: tiles > 1x1 => infinite loop (dim: 7x1024)
  // for all tiles:
  for(int tx=0; tx<tiles_x; tx++)
    for(int ty=0; ty<tiles_y; ty++)
    {
      size_t orig0[3] = {0, 0, 0};
      size_t origin[3];
      origin[0] = tx*tile_wd + sizes[0] > width ? width - sizes[0] : tx*tile_wd;
      origin[1] = ty*tile_ht + sizes[1] > height ? height - sizes[1] : ty*tile_ht;
      origin[2] = 0;
      size_t region[3] = {sizes[0], sizes[1], 1};


      // printf("tile extents (%d, %d): %zd %zd -- %zd %zd\n", tx, ty, origin[0], origin[1], region[0], region[1]);

      if(need_tiles)
      {
        err = dt_opencl_enqueue_copy_image(devid, _dev_in, dev_in, origin, orig0, region);
        if(err != CL_SUCCESS) goto error;
        dt_opencl_finish(devid);
      }

      if(tx > 0)
      {
        origin[0] += max_filter_radius;
        orig0[0] += max_filter_radius;
        region[0] -= max_filter_radius;
      }
      if(ty > 0)
      {
        origin[1] += max_filter_radius;
        orig0[1] += max_filter_radius;
        region[1] -= max_filter_radius;
      }

      if(region[0] <= 0 || region[1] <= 0) continue;  // this should never happen

      for(int s=0; s<max_scale; s++)
      {
        const int scale = s;
        //printf("decompose scale %d of %d with sizes %d x %d\n", s, max_scale, sizes[0], sizes[1]);

        if(!lowmem)
        {
          dev_detail = mdev_detail[s];
        }


        if(s & 1)
        {
          dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 0, sizeof(cl_mem), (void *)&dev_out);
          dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 1, sizeof(cl_mem), (void *)&dev_in);
        }
        else
        {
          dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 0, sizeof(cl_mem), (void *)&dev_in);
          dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 1, sizeof(cl_mem), (void *)&dev_out);
        }
        dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 2, sizeof(cl_mem), (void *)&dev_detail);
        dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 3, sizeof(unsigned int), (void *)&scale);
        dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 4, sizeof(float), (void *)&sharp[s]);

        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_decompose, sizes);
        if(err != CL_SUCCESS) goto error;
        dt_opencl_finish(devid);

        if(lowmem && s!=max_scale-1)
        {
          err = dt_opencl_read_host_from_device(devid, detail_buffer+4*sizeof(float)*sizes[0]*sizes[1]*s, dev_detail, sizes[0], sizes[1], 4*sizeof(float));
          if(err != CL_SUCCESS) goto error;
        }

      }

      // now synthesize again:
      for(int scale=max_scale-1; scale>=0; scale--)
      {
        //printf("synthesize scale %d of %d with sizes %d x %d\n", scale, max_scale, sizes[0], sizes[1]);

        if(lowmem && scale!=max_scale-1)
        {
          err = dt_opencl_write_host_to_device(devid, detail_buffer+4*sizeof(float)*sizes[0]*sizes[1]*scale, dev_detail, sizes[0], sizes[1], 4*sizeof(float));
          if(err != CL_SUCCESS) goto error;
        }
        
        if(!lowmem)
        {
          dev_detail = mdev_detail[scale];
        }

        if(scale & 1)
        {
          dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 0, sizeof(cl_mem), (void *)&dev_out);
          dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 1, sizeof(cl_mem), (void *)&dev_in);
        }
        else
        {
          dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 0, sizeof(cl_mem), (void *)&dev_in);
          dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 1, sizeof(cl_mem), (void *)&dev_out);
        }

        dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize,  2, sizeof(cl_mem), (void *)&dev_detail);
        dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize,  3, sizeof(float), (void *)&thrs[scale][0]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize,  4, sizeof(float), (void *)&thrs[scale][1]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize,  5, sizeof(float), (void *)&thrs[scale][2]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize,  6, sizeof(float), (void *)&thrs[scale][3]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize,  7, sizeof(float), (void *)&boost[scale][0]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize,  8, sizeof(float), (void *)&boost[scale][1]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize,  9, sizeof(float), (void *)&boost[scale][2]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 10, sizeof(float), (void *)&boost[scale][3]);

        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_synthesize, sizes);
        if(err != CL_SUCCESS) goto error;
        dt_opencl_finish(devid);
      }

      if(need_tiles)
      {
        err = dt_opencl_enqueue_copy_image(devid, dev_in, _dev_out, orig0, origin, region);
        if(err != CL_SUCCESS) goto error;
      }
      else
      {
        err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, orig0, orig0, region);
        if(err != CL_SUCCESS) goto error;
      }
      dt_opencl_finish(devid);
    }

  // free device mem
  if (detail_buffer != NULL) free(detail_buffer);
  if (mdev_in != NULL) dt_opencl_release_mem_object(mdev_in);
  if (mdev_out != NULL) dt_opencl_release_mem_object(mdev_out);
  for(int k=0; k<max_scale; k++)
    if (mdev_detail[k] != NULL) dt_opencl_release_mem_object(mdev_detail[k]);
  return TRUE;

error:
  if (detail_buffer != NULL) free(detail_buffer);
  if (mdev_in != NULL) dt_opencl_release_mem_object(mdev_in);
  if (mdev_out != NULL) dt_opencl_release_mem_object(mdev_out);
  for(int k=0; k<max_scale; k++)
    if (mdev_detail[k] != NULL) dt_opencl_release_mem_object(mdev_detail[k]);
  dt_print(DT_DEBUG_OPENCL, "[opencl_atrous] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, float *factor, unsigned *overhead, unsigned *overlap)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)piece->data;
  float thrs [MAX_NUM_SCALES][4];
  float boost[MAX_NUM_SCALES][4];
  float sharp[MAX_NUM_SCALES];
  const int max_scale = get_scales(thrs, boost, sharp, d, roi_in, piece);
  const int max_filter_radius = (1<<max_scale); // 2 * 2^max_scale

  *factor = 2 + max_scale;
  *overhead = 0;
  *overlap = max_filter_radius;
  return;
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_atrous_params_t));
  module->default_params = malloc(sizeof(dt_iop_atrous_params_t));
  module->default_enabled = 0;
  module->priority = 466; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_atrous_params_t);
  module->gui_data = NULL;
  dt_iop_atrous_params_t tmp;
  tmp.octaves = 3;
  for(int k=0; k<BANDS; k++)
  {
    tmp.y[atrous_L][k] = tmp.y[atrous_s][k] = tmp.y[atrous_c][k] = 0.5f;
    tmp.x[atrous_L][k] = tmp.x[atrous_s][k] = tmp.x[atrous_c][k] = k/(BANDS-1.0f);
    tmp.y[atrous_Lt][k] = tmp.y[atrous_ct][k] = 0.0f;
    tmp.x[atrous_Lt][k] = tmp.x[atrous_ct][k] = k/(BANDS-1.0f);
  }
  memcpy(module->params, &tmp, sizeof(dt_iop_atrous_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_atrous_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 1; // from programs.conf
  dt_iop_atrous_global_data_t *gd = (dt_iop_atrous_global_data_t *)malloc(sizeof(dt_iop_atrous_global_data_t));
  module->data = gd;
  gd->kernel_decompose  = dt_opencl_create_kernel(program, "eaw_decompose");
  gd->kernel_synthesize = dt_opencl_create_kernel(program, "eaw_synthesize");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_atrous_global_data_t *gd = (dt_iop_atrous_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_decompose);
  dt_opencl_free_kernel(gd->kernel_synthesize);
  free(module->data);
  module->data = NULL;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_atrous_params_t *p = (dt_iop_atrous_params_t *)params;
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)piece->data;
#if 0
  printf("---------- atrous preset begin\n");
  printf("p.octaves = %d;\n", p->octaves);
  for(int ch=0; ch<atrous_none; ch++) for(int k=0; k<BANDS; k++)
    {
      printf("p.x[%d][%d] = %f;\n", ch, k, p->x[ch][k]);
      printf("p.y[%d][%d] = %f;\n", ch, k, p->y[ch][k]);
    }
  printf("---------- atrous preset end\n");
#endif
  d->octaves = p->octaves;
  for(int ch=0; ch<atrous_none; ch++) for(int k=0; k<BANDS; k++)
      dt_draw_curve_set_point(d->curve[ch], k, p->x[ch][k], p->y[ch][k]);
  int l = 0;
  for(int k=(int)MIN(pipe->iwidth*pipe->iscale,pipe->iheight*pipe->iscale); k; k>>=1) l++;
  d->octaves = MIN(BANDS, l);
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)malloc(sizeof(dt_iop_atrous_data_t));
  dt_iop_atrous_params_t *default_params = (dt_iop_atrous_params_t *)self->default_params;
  piece->data = (void *)d;
  for(int ch=0; ch<atrous_none; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
    for(int k=0; k<BANDS; k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->x[ch][k], default_params->y[ch][k]);
  }
  int l = 0;
  for(int k=(int)MIN(pipe->iwidth*pipe->iscale,pipe->iheight*pipe->iscale); k; k>>=1) l++;
  d->octaves = MIN(BANDS, l);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)(piece->data);
  for(int ch=0; ch<atrous_none; ch++) dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
}

void init_presets (dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "begin", NULL, NULL, NULL);
  dt_iop_atrous_params_t p;
  p.octaves = 7;

  for(int k=0; k<BANDS; k++)
  {
    p.x[atrous_L][k] = k/(BANDS-1.0);
    p.x[atrous_c][k] = k/(BANDS-1.0);
    p.x[atrous_s][k] = k/(BANDS-1.0);
    p.y[atrous_L][k] = fmaxf(.5f, .75f-.5f*k/(BANDS-1.0));
    p.y[atrous_c][k] = fmaxf(.5f, .55f-.5f*k/(BANDS-1.0));
    p.y[atrous_s][k] = fminf(.5f, .2f + .35f * k/(BANDS-1.0));
    p.x[atrous_Lt][k] = k/(BANDS-1.0);
    p.x[atrous_ct][k] = k/(BANDS-1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  dt_gui_presets_add_generic(_("enhance coarse"), self->op, &p, sizeof(p), 1);
  for(int k=0; k<BANDS; k++)
  {
    p.x[atrous_L][k] = k/(BANDS-1.0);
    p.x[atrous_c][k] = k/(BANDS-1.0);
    p.x[atrous_s][k] = k/(BANDS-1.0);
    p.y[atrous_L][k] = .5f+.5f*k/(float)BANDS;
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .5f;
    p.x[atrous_Lt][k] = k/(BANDS-1.0);
    p.x[atrous_ct][k] = k/(BANDS-1.0);
    p.y[atrous_Lt][k] = .4f*k/(float)BANDS;
    p.y[atrous_ct][k] = .6f*k/(float)BANDS;
  }
  dt_gui_presets_add_generic(_("sharpen and denoise (strong)"), self->op, &p, sizeof(p), 1);
  for(int k=0; k<BANDS; k++)
  {
    p.x[atrous_L][k] = k/(BANDS-1.0);
    p.x[atrous_c][k] = k/(BANDS-1.0);
    p.x[atrous_s][k] = k/(BANDS-1.0);
    p.y[atrous_L][k] = .5f+.25f*k/(float)BANDS;
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .5f;
    p.x[atrous_Lt][k] = k/(BANDS-1.0);
    p.x[atrous_ct][k] = k/(BANDS-1.0);
    p.y[atrous_Lt][k] = .2f*k/(float)BANDS;
    p.y[atrous_ct][k] = .3f*k/(float)BANDS;
  }
  dt_gui_presets_add_generic(_("sharpen and denoise"), self->op, &p, sizeof(p), 1);
  for(int k=0; k<BANDS; k++)
  {
    p.x[atrous_L][k] = k/(BANDS-1.0);
    p.x[atrous_c][k] = k/(BANDS-1.0);
    p.x[atrous_s][k] = k/(BANDS-1.0);
    p.y[atrous_L][k] = .5f+.5f*k/(float)BANDS;
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .5f;
    p.x[atrous_Lt][k] = k/(BANDS-1.0);
    p.x[atrous_ct][k] = k/(BANDS-1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  dt_gui_presets_add_generic(_("sharpen (strong)"), self->op, &p, sizeof(p), 1);
  for(int k=0; k<BANDS; k++)
  {
    p.x[atrous_L][k] = k/(BANDS-1.0);
    p.x[atrous_c][k] = k/(BANDS-1.0);
    p.x[atrous_s][k] = k/(BANDS-1.0);
    p.y[atrous_L][k] = .5f+.25f*k/(float)BANDS;
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .5f;
    p.x[atrous_Lt][k] = k/(BANDS-1.0);
    p.x[atrous_ct][k] = k/(BANDS-1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  dt_gui_presets_add_generic(C_("atrous", "sharpen"), self->op, &p, sizeof(p), 1);
  for(int k=0; k<BANDS; k++)
  {
    p.x[atrous_L][k] = k/(BANDS-1.0);
    p.x[atrous_c][k] = k/(BANDS-1.0);
    p.x[atrous_s][k] = k/(BANDS-1.0);
    p.y[atrous_L][k] = .5f;//-.2f*k/(float)BANDS;
    p.y[atrous_c][k] = .5f;//fmaxf(0.0f, .5f-.3f*k/(float)BANDS);
    p.y[atrous_s][k] = .5f;
    p.x[atrous_Lt][k] = k/(BANDS-1.0);
    p.x[atrous_ct][k] = k/(BANDS-1.0);
    p.y[atrous_Lt][k] = .2f*k/(float)BANDS;
    p.y[atrous_ct][k] = .3f*k/(float)BANDS;
  }
  dt_gui_presets_add_generic(_("denoise"), self->op, &p, sizeof(p), 1);
  for(int k=0; k<BANDS; k++)
  {
    p.x[atrous_L][k] = k/(BANDS-1.0);
    p.x[atrous_c][k] = k/(BANDS-1.0);
    p.x[atrous_s][k] = k/(BANDS-1.0);
    p.y[atrous_L][k] = .5f;//-.4f*k/(float)BANDS;
    p.y[atrous_c][k] = .5f;//fmaxf(0.0f, .5f-.6f*k/(float)BANDS);
    p.y[atrous_s][k] = .5f;
    p.x[atrous_Lt][k] = k/(BANDS-1.0);
    p.x[atrous_ct][k] = k/(BANDS-1.0);
    p.y[atrous_Lt][k] = .4f*k/(float)BANDS;
    p.y[atrous_ct][k] = fminf(.5f, .8f*k/(float)BANDS);
  }
  p.y[atrous_s][BANDS-1] = 0.0f;
  p.y[atrous_s][BANDS-2] = 0.42f;
  dt_gui_presets_add_generic(_("denoise (strong)"), self->op, &p, sizeof(p), 1);
  for(int k=0; k<BANDS; k++)
  {
    p.x[atrous_L][k] = k/(BANDS-1.0);
    p.x[atrous_c][k] = k/(BANDS-1.0);
    p.x[atrous_s][k] = k/(BANDS-1.0);
    p.y[atrous_L][k] = fminf(.5f, .3f + .35f * k/(BANDS-1.0));
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .0f;
    p.x[atrous_Lt][k] = k/(BANDS-1.0);
    p.x[atrous_ct][k] = k/(BANDS-1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  p.y[atrous_L][0] = .5f;
  dt_gui_presets_add_generic(_("bloom"), self->op, &p, sizeof(p), 1);
  for(int k=0; k<BANDS; k++)
  {
    p.x[atrous_L][k] = k/(BANDS-1.0);
    p.x[atrous_c][k] = k/(BANDS-1.0);
    p.x[atrous_s][k] = k/(BANDS-1.0);
    p.y[atrous_L][k] = 0.55f;
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .0f;
    p.x[atrous_Lt][k] = k/(BANDS-1.0);
    p.x[atrous_ct][k] = k/(BANDS-1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  dt_gui_presets_add_generic(_("clarity (subtle)"), self->op, &p, sizeof(p), 1);
  for(int k=0; k<BANDS; k++)
  {
    p.x[atrous_L][k] = k/(BANDS-1.0);
    p.x[atrous_c][k] = k/(BANDS-1.0);
    p.x[atrous_s][k] = k/(BANDS-1.0);
    p.y[atrous_L][k] = 0.6f;
    p.y[atrous_c][k] = .55f;
    p.y[atrous_s][k] = .0f;
    p.x[atrous_Lt][k] = k/(BANDS-1.0);
    p.x[atrous_ct][k] = k/(BANDS-1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  dt_gui_presets_add_generic(_("clarity"), self->op, &p, sizeof(p), 1);
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "commit", NULL, NULL, NULL);
}

static void
reset_mix (dt_iop_module_t *self)
{
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  c->drag_params = *(dt_iop_atrous_params_t *)self->params;
  const int old = self->dt->gui->reset;
  self->dt->gui->reset = 1;
  dtgtk_slider_set_value(DTGTK_SLIDER(c->mix), 1.0f);
  self->dt->gui->reset = old;
}

void gui_update   (struct dt_iop_module_t *self)
{
  reset_mix(self);
  gtk_widget_queue_draw(self->widget);
}


// gui stuff:

static gboolean
area_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  if(!c->dragging) c->mouse_x = c->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

// fills in new parameters based on mouse position (in 0,1)
static void
get_params(dt_iop_atrous_params_t *p, const int ch, const double mouse_x, const double mouse_y, const float rad)
{
  for(int k=0; k<BANDS; k++)
  {
    const float f = expf(-(mouse_x - p->x[ch][k])*(mouse_x - p->x[ch][k])/(rad*rad));
    p->y[ch][k] = MAX(0.0f, MIN(1.0f, (1-f)*p->y[ch][k] + f*mouse_y));
  }
}

static gboolean
area_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  dt_iop_atrous_params_t p = *(dt_iop_atrous_params_t *)self->params;
  int ch  = (int)c->channel;
  int ch2 = (int)c->channel2;
  for(int k=0; k<BANDS; k++) dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
  const int inset = INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg, match color of the notebook tabs:
  GtkStyle *style = gtk_widget_get_style(GTK_WIDGET(c->channel_tabs));
  cairo_set_source_rgb (cr, style->bg[GTK_STATE_NORMAL].red/65535.0f,
                        style->bg[GTK_STATE_NORMAL].green/65535.0f,
                        style->bg[GTK_STATE_NORMAL].blue/65535.0f);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset;
  height -= 2*inset;

  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgb (cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb (cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max curves:
    get_params(&p, ch2, c->mouse_x, 1., c->mouse_radius);
    for(int k=0; k<BANDS; k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_min_xs, c->draw_min_ys);

    p = *(dt_iop_atrous_params_t *)self->params;
    get_params(&p, ch2, c->mouse_x, .0, c->mouse_radius);
    for(int k=0; k<BANDS; k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_max_xs, c->draw_max_ys);
  }

  // draw grid
  cairo_set_line_width(cr, .4);
  cairo_set_source_rgb (cr, .1, .1, .1);
  dt_draw_grid(cr, 8, 0, 0, width, height);

  cairo_save(cr);

  // draw selected cursor
  cairo_set_line_width(cr, 1.);
  cairo_translate(cr, 0, height);

  // draw frequency histogram in bg.
#if 1
  if(c->num_samples > 0)
  {
    cairo_save(cr);
    for(int k=1; k<c->num_samples; k+=2)
    {
      cairo_set_line_width(cr, 1.f);
      cairo_set_source_rgba(cr, .2, .2, .2, 0.3);
      cairo_move_to(cr, width*c->sample[k-1], 0.0f);
      cairo_line_to(cr, width*c->sample[k-1], -height);
      cairo_line_to(cr, width*c->sample[k], -height);
      cairo_line_to(cr, width*c->sample[k], 0.0f);
      cairo_fill(cr);
    }
    if(c->num_samples & 1)
    {
      cairo_move_to(cr, width*c->sample[c->num_samples-1], 0.0f);
      cairo_line_to(cr, width*c->sample[c->num_samples-1], -height);
      cairo_line_to(cr, 0.0f, -height);
      cairo_line_to(cr, 0.0f, 0.0f);
      cairo_fill(cr);
    }
    cairo_restore(cr);
  }
  if(c->band_max > 0)
  {
    cairo_save(cr);
    cairo_scale(cr, width/(BANDS-1.0), -(height-5)/c->band_max);
    cairo_set_source_rgba(cr, .2, .2, .2, 0.5);
    cairo_move_to(cr, 0, 0);
    for(int k=0; k<BANDS; k++) cairo_line_to(cr, k, c->band_hist[k]);
    cairo_line_to(cr, BANDS-1.0, 0.);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
  }
#endif

  // cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_line_width(cr, 2.);
  for(int i=0; i<=atrous_s; i++)
  {
    // draw curves, selected last.
    int ch = ((int)c->channel+i+1)%(atrous_s+1);
    int ch2 = -1;
    const float bgmul = i < atrous_s ? 0.5f : 1.0f;
    switch(ch)
    {
      case atrous_L:
        cairo_set_source_rgba(cr, .6, .6, .6, .3*bgmul);
        ch2 = atrous_Lt;
        break;
      case atrous_c:
        cairo_set_source_rgba(cr, .4, .2, .0, .4*bgmul);
        ch2 = atrous_ct;
        break;
      default: //case atrous_s:
        cairo_set_source_rgba(cr, .1, .2, .3, .4*bgmul);
        break;
    }
    p = *(dt_iop_atrous_params_t *)self->params;

    // reverse order if bottom is active (to end up with correct values in minmax_curve):
    if(c->channel2 == ch2)
    {
      ch2 = ch;
      ch = c->channel2;
    }

    if(ch2 >= 0)
    {
      for(int k=0; k<BANDS; k++) dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
      dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_xs, c->draw_ys);
      cairo_move_to(cr, width, -height*p.y[ch2][BANDS-1]);
      for(int k=RES-2; k>=0; k--) cairo_line_to(cr, k*width/(float)(RES-1), - height*c->draw_ys[k]);
    }
    else cairo_move_to(cr, 0, 0);
    for(int k=0; k<BANDS; k++) dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch][k], p.y[ch][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_xs, c->draw_ys);
    for(int k=0; k<RES; k++) cairo_line_to(cr, k*width/(float)(RES-1), - height*c->draw_ys[k]);
    if(ch2 < 0) cairo_line_to(cr, width, 0);
    cairo_close_path(cr);
    cairo_stroke_preserve(cr);
    cairo_fill(cr);
  }

  // draw dots on knots
  cairo_save(cr);
  if(ch != ch2) cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
  else          cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
  cairo_set_line_width(cr, 1.);
  for(int k=0; k<BANDS; k++)
  {
    cairo_arc(cr, width*p.x[ch2][k], - height*p.y[ch2][k], 3.0, 0.0, 2.0*M_PI);
    if(c->x_move == k) cairo_fill(cr);
    else               cairo_stroke(cr);
  }
  cairo_restore(cr);

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max, if selected
    // cairo_set_source_rgba(cr, .6, .6, .6, .5);
    cairo_move_to(cr, 0, - height*c->draw_min_ys[0]);
    for(int k=1; k<RES; k++)    cairo_line_to(cr, k*width/(float)(RES-1), - height*c->draw_min_ys[k]);
    for(int k=RES-1; k>=0; k--) cairo_line_to(cr, k*width/(float)(RES-1), - height*c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgba(cr, .9, .9, .9, .5);
    const float pos = RES * c->mouse_x;
    int k = (int)pos;
    const float f = k - pos;
    if(k >= RES-1) k = RES - 2;
    float ht = -height*(f*c->draw_ys[k] + (1-f)*c->draw_ys[k+1]);
    cairo_arc(cr, c->mouse_x*width, ht, c->mouse_radius*width, 0, 2.*M_PI);
    cairo_stroke(cr);
  }

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

  // draw x positions
  cairo_set_line_width(cr, 1.);
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  const float arrw = 7.0f;
  for(int k=1; k<BANDS-1; k++)
  {
    cairo_move_to(cr, width*p.x[ch][k], inset-1);
    cairo_rel_line_to(cr, -arrw*.5f, 0);
    cairo_rel_line_to(cr, arrw*.5f, -arrw);
    cairo_rel_line_to(cr, arrw*.5f, arrw);
    cairo_close_path(cr);
    if(c->x_move == k) cairo_fill(cr);
    else               cairo_stroke(cr);
  }

  cairo_restore(cr);
  // draw labels:
  cairo_text_extents_t ext;
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size (cr, .06*height);
  cairo_text_extents (cr, _("coarse"), &ext);
  cairo_move_to (cr, .02*width+ext.height, .14*height+ext.width);
  cairo_save (cr);
  cairo_rotate (cr, -M_PI*.5f);
  cairo_show_text(cr, _("coarse"));
  cairo_restore (cr);
  cairo_text_extents (cr, _("fine"), &ext);
  cairo_move_to (cr, .98*width, .14*height+ext.width);
  cairo_save (cr);
  cairo_rotate (cr, -M_PI*.5f);
  cairo_show_text(cr, _("fine"));
  cairo_restore (cr);

  switch(c->channel2)
  {
    case atrous_L:
    case atrous_c:
      dt_atrous_show_upper_label(cr, _("contrasty"), ext);
      dt_atrous_show_lower_label(cr, _("smooth"), ext);
      break;
    case atrous_Lt:
    case atrous_ct:
      dt_atrous_show_upper_label(cr, _("smooth"), ext);
      dt_atrous_show_lower_label(cr, _("noisy"), ext);
      break;
    default: //case atrous_s:
      dt_atrous_show_upper_label(cr, _("bold"), ext);
      dt_atrous_show_lower_label(cr, _("dull"), ext);
      break;
  }


  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean
area_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  dt_iop_atrous_params_t *p = (dt_iop_atrous_params_t *)self->params;
  const int inset = INSET;
  int height = widget->allocation.height - 2*inset, width = widget->allocation.width - 2*inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width)/(float)width;
  c->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height)/(float)height;
  int ch2 = c->channel;
  if(c->channel == atrous_L) ch2 = atrous_Lt;
  if(c->channel == atrous_c) ch2 = atrous_ct;
  if(c->dragging)
  {
    // drag y-positions
    *p = c->drag_params;
    if(c->x_move >= 0)
    {
      const float mx = CLAMP(event->x - inset, 0, width)/(float)width;
      if(c->x_move > 0 && c->x_move < BANDS-1)
      {
        const float minx = p->x[c->channel][c->x_move-1] + 0.001f;
        const float maxx = p->x[c->channel][c->x_move+1] - 0.001f;
        p->x[ch2][c->x_move] = p->x[c->channel][c->x_move] = fminf(maxx, fmaxf(minx, mx));
      }
    }
    else
    {
      get_params(p, c->channel2, c->mouse_x, c->mouse_y + c->mouse_pick, c->mouse_radius);
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else if(event->y > height)
  {
    // move x-positions
    c->x_move = 0;
    float dist = fabsf(p->x[c->channel][0] - c->mouse_x);
    for(int k=1; k<BANDS; k++)
    {
      float d2 = fabsf(p->x[c->channel][k] - c->mouse_x);
      if(d2 < dist)
      {
        c->x_move = k;
        dist = d2;
      }
    }
  }
  else
  {
    // choose between bottom and top curve:
    int ch = c->channel;
    float dist = 1000000.0f;
    for(int k=0; k<BANDS; k++)
    {
      float d2 = fabsf(p->x[c->channel][k] - c->mouse_x);
      if(d2 < dist)
      {
        if(fabsf(c->mouse_y - p->y[ch][k]) < fabsf(c->mouse_y - p->y[ch2][k])) c->channel2 = ch;
        else c->channel2 = ch2;
        dist = d2;
      }
    }
    // don't move x-positions:
    c->x_move = -1;
  }
  gtk_widget_queue_draw(widget);
  gint x, y;
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

static gboolean
area_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  // set active point
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
    reset_mix(self);
    const int inset = INSET;
    int height = widget->allocation.height - 2*inset, width = widget->allocation.width - 2*inset;
    c->mouse_pick = dt_draw_curve_calc_value(c->minmax_curve, CLAMP(event->x - inset, 0, width)/(float)width);
    c->mouse_pick -= 1.0 - CLAMP(event->y - inset, 0, height)/(float)height;
    c->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean
area_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
    c->dragging = 0;
    reset_mix(self);
    return TRUE;
  }
  return FALSE;
}

static gboolean
area_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  if(event->direction == GDK_SCROLL_UP   && c->mouse_radius > 0.25/BANDS) c->mouse_radius *= 0.9; //0.7;
  if(event->direction == GDK_SCROLL_DOWN && c->mouse_radius < 1.0) c->mouse_radius *= (1.0/0.9); //1.42;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static void
tab_switch(GtkNotebook *notebook, GtkNotebookPage *page, guint page_num, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  c->channel = c->channel2 = (atrous_channel_t)page_num;
  gtk_widget_queue_draw(self->widget);
}

static void
mix_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_atrous_params_t *p = (dt_iop_atrous_params_t *)self->params;
  dt_iop_atrous_params_t *d = (dt_iop_atrous_params_t *)self->factory_params;
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  const float mix = dtgtk_slider_get_value(slider);
  for(int ch=0; ch<atrous_none; ch++) for(int k=0; k<BANDS; k++)
    {
      p->x[ch][k] = fminf(1.0f, fmaxf(0.0f, d->x[ch][k] + mix * (c->drag_params.x[ch][k] - d->x[ch][k])));
      p->y[ch][k] = fminf(1.0f, fmaxf(0.0f, d->y[ch][k] + mix * (c->drag_params.y[ch][k] - d->y[ch][k])));
    }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

void gui_init (struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_atrous_gui_data_t));
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  dt_iop_atrous_params_t *p = (dt_iop_atrous_params_t *)self->params;

  c->num_samples = 0;
  c->band_max = 0;
  c->channel = c->channel2 = dt_conf_get_int("plugins/darkroom/atrous/gui_channel");
  int ch = (int)c->channel;
  c->minmax_curve = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
  for(int k=0; k<BANDS; k++) (void)dt_draw_curve_add_point(c->minmax_curve, p->x[ch][k], p->y[ch][k]);
  c->mouse_x = c->mouse_y = c->mouse_pick = -1.0;
  c->dragging = 0;
  c->x_move = -1;
  c->mouse_radius = 1.0/BANDS;
  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));

  // tabs
  GtkVBox *vbox = GTK_VBOX(gtk_vbox_new(FALSE, 0));

  c->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());

  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs), GTK_WIDGET(gtk_hbox_new(FALSE,0)), gtk_label_new(_("luma")));
  g_object_set(G_OBJECT(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1))), "tooltip-text", _("change lightness at each feature size"), NULL);
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs), GTK_WIDGET(gtk_hbox_new(FALSE,0)), gtk_label_new(_("chroma")));
  g_object_set(G_OBJECT(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1))), "tooltip-text", _("change color saturation at each feature size"), NULL);
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs), GTK_WIDGET(gtk_hbox_new(FALSE,0)), gtk_label_new(_("sharpness")));
  g_object_set(G_OBJECT(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1))), "tooltip-text", _("sharpness of edges at each feature size"), NULL);

  gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(c->channel_tabs, c->channel)));
  gtk_notebook_set_current_page(GTK_NOTEBOOK(c->channel_tabs), c->channel);

  g_object_set(G_OBJECT(c->channel_tabs), "homogeneous", TRUE, (char *)NULL);

  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(c->channel_tabs), FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(c->channel_tabs), "switch_page",
                   G_CALLBACK (tab_switch), self);

  // graph
  c->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(c->area), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(vbox), TRUE, TRUE, 5);
  gtk_drawing_area_size(c->area, 195, 195);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect (G_OBJECT (c->area), "expose-event",
                    G_CALLBACK (area_expose), self);
  g_signal_connect (G_OBJECT (c->area), "button-press-event",
                    G_CALLBACK (area_button_press), self);
  g_signal_connect (G_OBJECT (c->area), "button-release-event",
                    G_CALLBACK (area_button_release), self);
  g_signal_connect (G_OBJECT (c->area), "motion-notify-event",
                    G_CALLBACK (area_motion_notify), self);
  g_signal_connect (G_OBJECT (c->area), "leave-notify-event",
                    G_CALLBACK (area_leave_notify), self);
  g_signal_connect (G_OBJECT (c->area), "scroll-event",
                    G_CALLBACK (area_scrolled), self);

  // mix slider
  c->mix = dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, -2.0f, 2.0f, 0.1f, 1.0f, 3);
  GtkWidget *hbox = gtk_hbox_new(FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 5);
  dtgtk_slider_set_label(DTGTK_SLIDER(c->mix), _("mix"));
  g_object_set(G_OBJECT(c->mix), "tooltip-text", _("make effect stronger or weaker"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(hbox), c->mix, TRUE, TRUE, 0);
  g_signal_connect (G_OBJECT (c->mix), "value-changed", G_CALLBACK (mix_callback), self);
}

void gui_cleanup  (struct dt_iop_module_t *self)
{
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  dt_conf_set_int("plugins/darkroom/atrous/gui_channel", c->channel);
  dt_draw_curve_destroy(c->minmax_curve);
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
