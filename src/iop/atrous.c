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
#include "gui/draw.h"
#include <memory.h>
#include <stdlib.h>
#include <xmmintrin.h>
// SSE4 actually not used yet.
// #include <smmintrin.h>

#define INSET 5
#define INFL .3f

DT_MODULE(1)

#define BANDS 6
#define MAX_LEVEL 6
#define RES 64

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
  int32_t octaves; // max is 7 -> 5*2^7 = 640px support
  float x[atrous_none][BANDS], y[atrous_none][BANDS];
}
dt_iop_atrous_params_t;

typedef struct dt_iop_atrous_gui_data_t
{
  GtkDrawingArea *area;
  GtkHBox *hbox;
  GtkComboBox *presets;
  GtkRadioButton *channel_button[atrous_none];
  double mouse_x, mouse_y, mouse_pick;
  float mouse_radius;
  dt_iop_atrous_params_t drag_params;
  int dragging;
  int x_move;
  dt_draw_curve_t *minmax_curve;
  atrous_channel_t channel, channel2;
  double draw_xs[RES], draw_ys[RES];
  double draw_min_xs[RES], draw_min_ys[RES];
  double draw_max_xs[RES], draw_max_ys[RES];
  float band_hist[BANDS];
  float band_max;
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
  return _("equalizer II");
}

int 
groups ()
{
  return IOP_GROUP_CORRECT;
}

#if 0
/* x in -1 .. 1 */
static const __m128
fast_exp_ps(const __m128 exponent)
{
  const __m128 x2 = _mm_mul_ps( exponent, exponent);
  const __m128 term1 =  _mm_add_ps( _mm_set1_ps( 0.496879224f), _mm_mul_ps( _mm_set1_ps(0.190809553f), exponent));
  const __m128 term2 = _mm_add_ps( _mm_set1_ps(1.0f), exponent);
  return  _mm_add_ps( term2,  _mm_mul_ps( x2, term1));
}
#endif

#if 1
static inline float
fastexp(const float x)
{
  return fmaxf(0.0f, (6+x*(6+x*(3+x)))*0.16666666f);
}
#endif

#if 0
static inline float
fastexp(const float x)
{
  return fmaxf(0.0f, (120+x*(120+x*(60+x*(20+x*(5+x)))))*0.0083333333f);
}
#endif

static const __m128
weight (const float *c1, const float *c2, const float sharpen)
{
  // const __m128 dot = _mm_dp_ps(c1, c2, 0xff);
  // float w = expf((*(float *)&dot)*sharpen);
  const float wc = fastexp(-((c1[1] - c2[1])*(c1[1] - c2[1]) + (c1[2] - c2[2])*(c1[2] - c2[2])) * sharpen);
  const float wl = fastexp(-(c1[0] - c2[0])*(c1[0] - c2[0]) * sharpen);
  // printf("w = %f | %f %f %f -- %f %f %f\n", w, c1[0], c1[1], c1[2], c2[0], c2[1], c2[2]);
  return _mm_set_ps(1.0f, wc, wc, wl);
  // return fast_exp_ps(_mm_mul_ps(_mm_dp_ps(c1, c2), sharpen));
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
  for(int j=0;j<height;j++)
  {
    const __m128 *px = ((__m128 *)in) + j*width;
    float *pdetail = detail + 4*j*width;
    float *pcoarse = out + 4*j*width;
    for(int i=0;i<width;i++)
    {
      // TODO: prefetch? _mm_prefetch()
      __m128 sum = _mm_setzero_ps(), wgt = _mm_setzero_ps();
      for(int jj=0;jj<5;jj++) for(int ii=0;ii<5;ii++)
      {
        if(mult*(ii-2) + i < 0 || mult*(ii-2)+i >= width || j+mult*(jj-2) < 0 || j+mult*(jj-2) >= height) continue;
        const __m128 *px2 = px + mult*(ii-2) + width*mult*(jj-2);
        const __m128 w = _mm_mul_ps(_mm_set1_ps(filter[ii]*filter[jj]), weight((float *)px, (float *)px2, sharpen));
        // const __m128 w = _mm_set1_ps(filter[ii]*filter[jj]*weight((float *)px, (float *)px2, sharpen));
        sum = _mm_add_ps(sum, _mm_mul_ps(w, *px2));
        wgt = _mm_add_ps(wgt, w);
      }
      // sum = _mm_div_ps(sum, wgt);
      sum = _mm_mul_ps(sum, _mm_rcp_ps(wgt)); // less precise, but faster

      // write back

      // TODO: check which one's faster:
      // memcpy?
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
  for(int j=0;j<height;j++)
  {
    // TODO: prefetch? _mm_prefetch()
     const __m128 *pin = (__m128 *)in + j*width;
    __m128 *pdetail = (__m128 *)detail + j*width;
    float *pout = out + 4*j*width;
    for(int i=0;i<width;i++)
    {
      const __m128i maski = _mm_set1_epi32(0x80000000u);
      const __m128 *mask = (__m128*)&maski;
      const __m128 absamt = _mm_max_ps(_mm_setzero_ps(), _mm_sub_ps(_mm_andnot_ps(*mask, *pdetail),
                            _mm_mul_ps(_mm_set_ps(1.0f, pout[0], pout[0], 1.0f), threshold)));
      const __m128 amount = _mm_or_ps(_mm_and_ps(*pdetail, *mask), absamt);
      _mm_stream_ps(pout, _mm_add_ps(*pin, _mm_mul_ps(boost, amount)));
      // _mm_stream_ps(pout, _mm_add_ps(*pin, _mm_mul_ps(boost, *pdetail)));
      pdetail ++;
      pin ++;
      pout += 4;
    }
  }
  _mm_sfence();
}

void
process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // TODO:
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)piece->data;
  float *detail = (float *)dt_alloc_align(64, sizeof(float)*4*roi_out->width*roi_out->height*d->octaves);
  float *tmp    = (float *)dt_alloc_align(64, sizeof(float)*4*roi_out->width*roi_out->height);
  float *buf1 = (float *)i, *buf2;
  const int max_scale = 5;//d->octaves;

  float thrs [max_scale][4];
  float boost[max_scale][4];
  float sharp[max_scale];

  for(int i=0;i<max_scale;i++)
  {
    boost[i][3] = boost[i][0] = 2.0f*dt_draw_curve_calc_value(d->curve[atrous_L], 1.0f - (i+.5f)/(max_scale));
    boost[i][1] = boost[i][2] = 2.0f*dt_draw_curve_calc_value(d->curve[atrous_c], 1.0f - (i+.5f)/(max_scale));
    for(int k=0;k<4;k++) boost[i][k] *= boost[i][k];
    thrs [i][0] = thrs [i][3] = powf(2.0f, -i) * 3.0f*dt_draw_curve_calc_value(d->curve[atrous_Lt], 1.0f - (i+.5f)/(max_scale));
    thrs [i][1] = thrs [i][2] = powf(2.0f, -i) * 6.0f*dt_draw_curve_calc_value(d->curve[atrous_ct], 1.0f - (i+.5f)/(max_scale));
    sharp[i]    = 0.001f*dt_draw_curve_calc_value(d->curve[atrous_s], 1.0f - (i+.5f)/(max_scale));
    //for(int k=0;k<4;k++) boost[i][k] *= 1.0f/(1.0f - thrs[i][k]);
    // printf("scale %d boost %f %f thrs %f %f sharpen %f\n", i, boost[i][0], boost[i][2], thrs[i][0], thrs[i][1], sharp[i]);
  }

  buf2 = tmp;

  /*
  in -> tmp
  tmp->out
  out->tmp
  tmp->out

  in ->out
  out->tmp
  tmp->out
  out->tmp
  tmp->out
  */

  for(int scale=0;scale<max_scale;scale++)
  {
    eaw_decompose (buf2, buf1, detail + 4*roi_out->width*roi_out->height*scale, scale, sharp[scale], roi_out->width, roi_out->height);
    if(scale == 0) buf1 = (float *)o;
    float *buf3 = buf2;
    buf2 = buf1;
    buf1 = buf3;
  }

  for(int scale=max_scale-1;scale>=0;scale--)
  {
    eaw_synthesize (buf2, buf1, detail + 4*roi_out->width*roi_out->height*scale,
      thrs[scale], boost[scale], roi_out->width, roi_out->height);
    float *buf3 = buf2;
    buf2 = buf1;
    buf1 = buf3;
  }

  free(detail);
  free(tmp);
}

#ifdef HAVE_OPENCL
void
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
    void *cl_mem_in, void **cl_mem_out)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)piece->data;
  dt_iop_atrous_global_data_t *gd = (dt_iop_atrous_global_data_t *)self->data;
  // global scale is roi scale and pipe input prescale
  const float global_scale = roi_in->scale / piece->iscale;
  float *in  = (float *)i;
  float *out = (float *)o;

  // TODO: use cl_mem_in and out!
  const int max_scale = 5;//d->octaves;
  float thrs [max_scale][4];
  float boost[max_scale][4];
  float sharp[max_scale];

  for(int i=0;i<max_scale;i++)
  {
    boost[i][3] = boost[i][0] = 2.0f*dt_draw_curve_calc_value(d->curve[atrous_L], 1.0f - (i+.5f)/(max_scale));
    boost[i][1] = boost[i][2] = 2.0f*dt_draw_curve_calc_value(d->curve[atrous_c], 1.0f - (i+.5f)/(max_scale));
    for(int k=0;k<4;k++) boost[i][k] *= boost[i][k];
    thrs [i][0] = thrs [i][3] = powf(2.0f, -i) * 3.0f*dt_draw_curve_calc_value(d->curve[atrous_Lt], 1.0f - (i+.5f)/(max_scale));
    thrs [i][1] = thrs [i][2] = powf(2.0f, -i) * 6.0f*dt_draw_curve_calc_value(d->curve[atrous_ct], 1.0f - (i+.5f)/(max_scale));
    sharp[i]    = 0.001f*dt_draw_curve_calc_value(d->curve[atrous_s], 1.0f - (i+.5f)/(max_scale));
    //for(int k=0;k<4;k++) boost[i][k] *= 1.0f/(1.0f - thrs[i][k]);
    // printf("scale %d boost %f %f thrs %f %f sharpen %f\n", i, boost[i][0], boost[i][2], thrs[i][0], thrs[i][1], sharp[i]);
  }

  const int devid = piece->pipe->devid;
  cl_int err;
  size_t sizes[3];
  err = dt_opencl_get_max_work_item_sizes(darktable.opencl, devid, sizes);
  if(err != CL_SUCCESS) fprintf(stderr, "could not get max size! %d\n", err);
  // printf("max work item sizes = %lu %lu %lu\n", sizes[0], sizes[1], sizes[2]);

  // allocate device memory
  cl_mem dev_in, dev_coarse;
  // as images (texture memory)
  cl_image_format fmt = {CL_RGBA, CL_FLOAT};
  dev_in = clCreateImage2D (darktable.opencl->dev[devid].context,
      CL_MEM_READ_WRITE,
      &fmt,
      sizes[0], sizes[1], 0,
      NULL, &err);
  if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy img buffer on device: %d\n", err);
  dev_coarse = clCreateImage2D (darktable.opencl->dev[devid].context,
      CL_MEM_READ_WRITE,
      &fmt,
      sizes[0], sizes[1], 0,
      NULL, &err);
  if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy coarse buffer on device: %d\n", err);


  const int max_filter_radius = global_scale*(1<<max_scale); // 2 * 2^max_scale
  const int tile_wd = sizes[0] - 2*max_filter_radius, tile_ht = sizes[1] - 2*max_filter_radius;

  // details for local contrast enhancement:
  cl_mem dev_detail[max_scale];
  for(int k=0;k<max_scale;k++)
  {
    dev_detail[k] = clCreateImage2D (darktable.opencl->dev[devid].context,
        CL_MEM_READ_WRITE,
        &fmt,
        sizes[0], sizes[1], 0,
        NULL, &err);
    if(err != CL_SUCCESS) fprintf(stderr, "could not alloc/copy detail buffer on device: %d\n", err);
  }

  const int width = roi_out->width, height = roi_out->height;

  // for all tiles:
  for(int tx=0;tx<ceilf(width/(float)tile_wd);tx++)
  for(int ty=0;ty<ceilf(height/(float)tile_ht);ty++)
  {
    size_t orig0[3] = {0, 0, 0};
    size_t origin[3] = {tx*tile_wd, ty*tile_ht, 0};
    size_t wd = origin[0] + sizes[0] > width  ? width  - origin[0] : sizes[0];
    size_t ht = origin[1] + sizes[1] > height ? height - origin[1] : sizes[1];
    size_t region[3] = {wd, ht, 1};

    // TODO: directly read cl_mem_in!
    // TODO: one more buffer and interleaved write/process
    clEnqueueWriteImage(darktable.opencl->dev[devid].cmd_queue, dev_in, CL_FALSE, orig0, region, 4*width*sizeof(float), 0, in + 4*(width*origin[1] + origin[0]), 0, NULL, NULL);
    if(tx > 0) { origin[0] += max_filter_radius; orig0[0] += max_filter_radius; region[0] -= max_filter_radius; }
    if(ty > 0) { origin[1] += max_filter_radius; orig0[1] += max_filter_radius; region[1] -= max_filter_radius; }

    for(int s=0;s<max_scale;s++)
    {
      const int scale = global_scale * s;
      // FIXME: don't do 0x0 filtering!
      // if((int)(global_scale * (s+1)) == 0) continue;
      err = dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 2, sizeof(cl_mem), (void *)&dev_detail[s]);
      if(s & 1)
      {
        dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 0, sizeof(cl_mem), (void *)&dev_coarse);
        dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 1, sizeof(cl_mem), (void *)&dev_in);
      }
      else
      {
        dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 1, sizeof(cl_mem), (void *)&dev_coarse);
        dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 0, sizeof(cl_mem), (void *)&dev_in);
      }
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 3, sizeof(unsigned int), (void *)&scale);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_decompose, 4, sizeof(unsigned int), (void *)&sharp[s]);

      // printf("equeueing kernel with %lu %lu threads\n", local[0], global[0]);
      err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_decompose, sizes);
      if(err != CL_SUCCESS) fprintf(stderr, "enqueueing failed! %d\n", err);
      // clFinish(darktable.opencl->cmd_queue);
    }

    // clFinish(darktable.opencl->cmd_queue);
    // now synthesize again:
    for(int scale=max_scale-1;scale>=0;scale--)
    {
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize,  2, sizeof(cl_mem), (void *)&dev_detail[scale]);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize,  3, sizeof(float), (void *)&thrs[scale][0]);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize,  4, sizeof(float), (void *)&thrs[scale][1]);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize,  5, sizeof(float), (void *)&thrs[scale][2]);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize,  6, sizeof(float), (void *)&thrs[scale][3]);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize,  7, sizeof(float), (void *)&boost[scale][0]);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize,  8, sizeof(float), (void *)&boost[scale][1]);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize,  9, sizeof(float), (void *)&boost[scale][2]);
      dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize, 10, sizeof(float), (void *)&boost[scale][3]);
      if(scale & 1)
      {
        dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize, 0, sizeof(cl_mem), (void *)&dev_coarse);
        dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize, 1, sizeof(cl_mem), (void *)&dev_in);
      }
      else
      {
        dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize, 1, sizeof(cl_mem), (void *)&dev_coarse);
        dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_synthesize, 0, sizeof(cl_mem), (void *)&dev_in);
      }

      err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_synthesize, sizes);
      if(err != CL_SUCCESS) fprintf(stderr, "enqueueing failed! %d\n", err);
      // clFinish(darktable.opencl->cmd_queue);
      // if((int)(global_scale * scale) == 0) break;
    }
    clEnqueueReadImage(darktable.opencl->dev[devid].cmd_queue, dev_in, CL_FALSE, orig0, region, 4*width*sizeof(float), 0, out + 4*(width*origin[1] + origin[0]), 0, NULL, NULL);
  }

  // wait for async read
  clFinish(darktable.opencl->dev[devid].cmd_queue);

  // free(in4);

  // write output images:
  // for(int k=0;k<width*height;k++) for(int c=0;c<3;c++) out[3*k+c] = out[4*k+c];

  // free device mem
  clReleaseMemObject(dev_in);
  clReleaseMemObject(dev_coarse);
  for(int k=0;k<max_scale;k++) clReleaseMemObject(dev_detail[k]);
}
#endif

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_atrous_params_t));
  module->default_params = malloc(sizeof(dt_iop_atrous_params_t));
  module->default_enabled = 0;
  module->priority = 370;
  module->params_size = sizeof(dt_iop_atrous_params_t);
  module->gui_data = NULL;
  dt_iop_atrous_params_t tmp;
  tmp.octaves = 3;
  for(int k=0;k<BANDS;k++)
  {
    tmp.y[atrous_L][k] = tmp.y[atrous_s][k] = tmp.y[atrous_c][k] = 0.5f;
    tmp.x[atrous_L][k] = tmp.x[atrous_s][k] = tmp.x[atrous_c][k] = k/(BANDS-1.0f);
    tmp.y[atrous_Lt][k] = tmp.y[atrous_ct][k] = 0.0f;
    tmp.x[atrous_Lt][k] = tmp.x[atrous_ct][k] = k/(BANDS-1.0f);
  }
  memcpy(module->params, &tmp, sizeof(dt_iop_atrous_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_atrous_params_t));

  const int program = 1; // from programs.conf
  dt_iop_atrous_global_data_t *gd = (dt_iop_atrous_global_data_t *)malloc(sizeof(dt_iop_atrous_global_data_t));
  module->data = gd;
  gd->kernel_decompose  = dt_opencl_create_kernel(darktable.opencl, program, "eaw_decompose");
  gd->kernel_synthesize = dt_opencl_create_kernel(darktable.opencl, program, "eaw_synthesize");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
  dt_iop_atrous_global_data_t *gd = (dt_iop_atrous_global_data_t *)module->data;
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_decompose);
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_synthesize);
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
  for(int ch=0;ch<atrous_none;ch++) for(int k=0;k<BANDS;k++)
  {
    printf("p.x[%d][%d] = %f;\n", ch, k, p->x[ch][k]);
    printf("p.y[%d][%d] = %f;\n", ch, k, p->y[ch][k]);
  }
  printf("---------- atrous preset end\n");
#endif
  d->octaves = p->octaves;
  for(int ch=0;ch<atrous_none;ch++) for(int k=0;k<BANDS;k++)
    dt_draw_curve_set_point(d->curve[ch], k, p->x[ch][k], p->y[ch][k]);
  int l = 0;
  for(int k=(int)MIN(pipe->iwidth*pipe->iscale,pipe->iheight*pipe->iscale);k;k>>=1) l++;
  d->octaves = MIN(MAX_LEVEL, l);
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)malloc(sizeof(dt_iop_atrous_data_t));
  dt_iop_atrous_params_t *default_params = (dt_iop_atrous_params_t *)self->default_params;
  piece->data = (void *)d;
  for(int ch=0;ch<atrous_none;ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0);
    for(int k=0;k<BANDS;k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->x[ch][k], default_params->y[ch][k]);
  }
  int l = 0;
  for(int k=(int)MIN(pipe->iwidth*pipe->iscale,pipe->iheight*pipe->iscale);k;k>>=1) l++;
  d->octaves = MIN(MAX_LEVEL, l);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)(piece->data);
  for(int ch=0;ch<atrous_none;ch++) dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
}

#if 0
void init_presets (dt_iop_module_t *self)
{
  sqlite3_exec(darktable.db, "begin", NULL, NULL, NULL);
  dt_iop_equalizer_params_t p;
  dt_gui_presets_add_generic(_(" (strong)"), self->op, &p, sizeof(p), 1);

  sqlite3_exec(darktable.db, "commit", NULL, NULL, NULL);
}
#endif

void gui_update   (struct dt_iop_module_t *self)
{
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
  for(int k=0;k<BANDS;k++)
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
  for(int k=0;k<BANDS;k++) dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
  const int inset = INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset; height -= 2*inset;

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
    for(int k=0;k<BANDS;k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_min_xs, c->draw_min_ys);

    p = *(dt_iop_atrous_params_t *)self->params;
    get_params(&p, ch2, c->mouse_x, .0, c->mouse_radius);
    for(int k=0;k<BANDS;k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_max_xs, c->draw_max_ys);
  }

  // draw grid
  cairo_set_line_width(cr, .4);
  cairo_set_source_rgb (cr, .1, .1, .1);
  dt_draw_grid(cr, 8, 0, 0, width, height);

  // draw selected cursor
  cairo_set_line_width(cr, 1.);
  cairo_translate(cr, 0, height);

  // draw frequency histogram in bg.
#if 1
  if(c->band_max > 0)
  {
    cairo_save(cr);
    cairo_scale(cr, width/(BANDS-1.0), -(height-5)/c->band_max);
    cairo_set_source_rgba(cr, .2, .2, .2, 0.5);
    cairo_move_to(cr, 0, 0);
    for(int k=0;k<BANDS;k++) cairo_line_to(cr, k, c->band_hist[k]);
    cairo_line_to(cr, BANDS-1.0, 0.);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
  }
#endif
 
  // cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_line_width(cr, 2.);
  for(int i=0;i<=atrous_s;i++)
  { // draw curves, selected last.
    int ch = ((int)c->channel+i+1)%(atrous_s+1);
    int ch2 = -1;
    switch(ch)
    {
      case atrous_L:
        cairo_set_source_rgba(cr, .6, .6, .6, .3);
        ch2 = atrous_Lt;
        break;
      case atrous_c:
        cairo_set_source_rgba(cr, .4, .2, .0, .4);
        ch2 = atrous_ct;
        break;
      default: //case atrous_s:
        cairo_set_source_rgba(cr, .1, .2, .3, .4);
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
      for(int k=0;k<BANDS;k++) dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
      dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_xs, c->draw_ys);
      cairo_move_to(cr, width, -height*p.y[ch2][BANDS-1]);
      for(int k=RES-2;k>=0;k--) cairo_line_to(cr, k*width/(float)(RES-1), - height*c->draw_ys[k]);
    }
    else cairo_move_to(cr, 0, 0);
    for(int k=0;k<BANDS;k++) dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch][k], p.y[ch][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_xs, c->draw_ys);
    for(int k=0;k<RES;k++) cairo_line_to(cr, k*width/(float)(RES-1), - height*c->draw_ys[k]);
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
  for(int k=0;k<BANDS;k++)
  {
    cairo_arc(cr, width*p.x[ch2][k], - height*p.y[ch2][k], 3.0, 0.0, 2.0*M_PI);
    if(c->x_move == k) cairo_fill(cr);
    else               cairo_stroke(cr);
  }
  cairo_restore(cr);

  if(c->mouse_y > 0 || c->dragging)
  { // draw min/max, if selected
    // cairo_set_source_rgba(cr, .6, .6, .6, .5);
    cairo_move_to(cr, 0, - height*c->draw_min_ys[0]);
    for(int k=1;k<RES;k++)    cairo_line_to(cr, k*width/(float)(RES-1), - height*c->draw_min_ys[k]);
    for(int k=RES-1;k>=0;k--) cairo_line_to(cr, k*width/(float)(RES-1), - height*c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgba(cr, .9, .9, .9, .5);
    const float pos = RES * c->mouse_x;
    int k = (int)pos; const float f = k - pos;
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
  for(int k=1;k<BANDS-1;k++)
  {
    cairo_move_to(cr, width*p.x[ch][k], inset-1);
    cairo_rel_line_to(cr, -arrw*.5f, 0);
    cairo_rel_line_to(cr, arrw*.5f, -arrw);
    cairo_rel_line_to(cr, arrw*.5f, arrw);
    cairo_close_path(cr);
    if(c->x_move == k) cairo_fill(cr);
    else               cairo_stroke(cr);
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
    dt_dev_add_history_item(darktable.develop, self);
  }
  else if(event->y > height)
  {
    // move x-positions
    c->x_move = 0;
    float dist = fabsf(p->x[c->channel][0] - c->mouse_x);
    for(int k=1;k<BANDS;k++)
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
    for(int k=0;k<BANDS;k++)
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
{ // set active point
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
    c->drag_params = *(dt_iop_atrous_params_t *)self->params;
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
button_toggled(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  if(gtk_toggle_button_get_active(togglebutton))
  {
    for(int k=0;k<atrous_none;k++) if(c->channel_button[k] == GTK_RADIO_BUTTON(togglebutton))
    {
      c->channel = (atrous_channel_t)k;
      gtk_widget_queue_draw(self->widget);
      return;
    }
  }
}




void gui_init (struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_atrous_gui_data_t));
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  dt_iop_atrous_params_t *p = (dt_iop_atrous_params_t *)self->params;

  c->band_max = 0;
  c->channel = atrous_L;
  c->channel2 = atrous_L;
  int ch = (int)c->channel;
  c->minmax_curve = dt_draw_curve_new(0.0, 1.0);
  for(int k=0;k<BANDS;k++) (void)dt_draw_curve_add_point(c->minmax_curve, p->x[ch][k], p->y[ch][k]);
  c->mouse_x = c->mouse_y = c->mouse_pick = -1.0;
  c->dragging = 0;
  c->x_move = -1;
  c->mouse_radius = 1.0/BANDS;
  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));
  c->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), TRUE, TRUE, 0);
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
  // init gtk stuff
  c->hbox = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->hbox), FALSE, FALSE, 0);

  c->channel_button[atrous_L]  = GTK_RADIO_BUTTON(gtk_radio_button_new_with_label(NULL, _("luma")));
  gtk_object_set(GTK_OBJECT(c->channel_button[atrous_L]), "tooltip-text", _("change lightness at each frequency"), NULL);
  c->channel_button[atrous_c]  = GTK_RADIO_BUTTON(gtk_radio_button_new_with_label_from_widget(c->channel_button[0], _("chroma")));
  gtk_object_set(GTK_OBJECT(c->channel_button[atrous_c]), "tooltip-text", _("change color saturation at each frequency"), NULL);
  // c->channel_button[atrous_Lt] = GTK_RADIO_BUTTON(gtk_radio_button_new_with_label_from_widget(c->channel_button[0], _("Lt")));
  // gtk_object_set(GTK_OBJECT(c->channel_button[atrous_Lt]), "tooltip-text", _("denoise lightness at each frequency"), NULL);
  // c->channel_button[atrous_ct] = GTK_RADIO_BUTTON(gtk_radio_button_new_with_label_from_widget(c->channel_button[0], _("ct")));
  // gtk_object_set(GTK_OBJECT(c->channel_button[atrous_ct]), "tooltip-text", _("denoise color at each frequency"), NULL);
  c->channel_button[atrous_s]  = GTK_RADIO_BUTTON(gtk_radio_button_new_with_label_from_widget(c->channel_button[0], _("sharpness")));
  gtk_object_set(GTK_OBJECT(c->channel_button[atrous_s]), "tooltip-text", _("sharpness of edges at each frequency"), NULL);

  for(int k=atrous_s;k>=0;k--)
  {
    g_signal_connect (G_OBJECT (c->channel_button[k]), "toggled",
                      G_CALLBACK (button_toggled), self);
    gtk_box_pack_end(GTK_BOX(c->hbox), GTK_WIDGET(c->channel_button[k]), FALSE, FALSE, 5);
  }
}

void gui_cleanup  (struct dt_iop_module_t *self)
{
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  dt_draw_curve_destroy(c->minmax_curve);
  free(self->gui_data);
  self->gui_data = NULL;
}

