/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/eaw.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "dtgtk/drawingarea.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <math.h>
#include <memory.h>
#include <stdlib.h>

//#define USE_NEW_CL  //uncomment to use the new, more memory-efficient OpenCL code (not yet finished)

#define INSET DT_PIXEL_APPLY_DPI(5)

DT_MODULE_INTROSPECTION(2, dt_iop_atrous_params_t)

#define BANDS 6
#define MAX_NUM_SCALES 8 // 2*2^(i+1) + 1 = 1025px support for i = 8
#define RES 64

#define dt_atrous_show_upper_label(cr, text, layout, ink)                                                    \
  pango_layout_set_text(layout, text, -1);                                                                   \
  pango_layout_get_pixel_extents(layout, &ink, NULL);                                                        \
  cairo_move_to(cr, .5 * (width - ink.width), (.08 * height) - ink.height);                                  \
  pango_cairo_show_layout(cr, layout);


#define dt_atrous_show_lower_label(cr, text, layout, ink)                                                    \
  pango_layout_set_text(layout, text, -1);                                                                   \
  pango_layout_get_pixel_extents(layout, &ink, NULL);                                                        \
  cairo_move_to(cr, .5 * (width - ink.width), (.98 * height) - ink.height);                                  \
  pango_cairo_show_layout(cr, layout);


typedef enum atrous_channel_t
{
  atrous_L = 0,  // luminance boost
  atrous_c = 1,  // chrominance boost
  atrous_s = 2,  // edge sharpness
  atrous_Lt = 3, // luminance noise threshold
  atrous_ct = 4, // chrominance noise threshold
  atrous_none = 5
} atrous_channel_t;

typedef struct dt_iop_atrous_params_t
{
  int32_t octaves;             // $DEFAULT: 3
  float x[atrous_none][BANDS];
  float y[atrous_none][BANDS]; // $DEFAULT: 0.5
  float mix;                   // $DEFAULT: 1.0 $MIN: -2.0 $MAX: 2.0
} dt_iop_atrous_params_t;

typedef struct dt_iop_atrous_gui_data_t
{
  GtkWidget *mix;
  GtkDrawingArea *area;
  GtkNotebook *channel_tabs;
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
  int num_samples;
  gboolean in_curve;
} dt_iop_atrous_gui_data_t;

typedef struct dt_iop_atrous_global_data_t
{
  int kernel_zero;
  int kernel_decompose;
  int kernel_synthesize;
  int kernel_addbuffers;
} dt_iop_atrous_global_data_t;

typedef struct dt_iop_atrous_data_t
{
  // demosaic pattern
  int32_t octaves;
  dt_draw_curve_t *curve[atrous_none];
} dt_iop_atrous_data_t;


const char *name()
{
  return _("contrast equalizer");
}

const char *aliases()
{
  return _("sharpness|acutance|local contrast");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("add or remove local contrast, sharpness, acutance"),
                                      _("corrective and creative"),
                                      _("linear, Lab, scene-referred"),
                                      _("frequential, RGB"),
                                      _("linear, Lab, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_iop_atrous_params_v1_t
    {
      int32_t octaves;             // $DEFAULT: 3
      float x[atrous_none][BANDS];
      float y[atrous_none][BANDS]; // $DEFAULT: 0.5
    } dt_iop_atrous_params_v1_t;

    dt_iop_atrous_params_v1_t *o = (dt_iop_atrous_params_v1_t *)old_params;
    dt_iop_atrous_params_t *n = (dt_iop_atrous_params_t *)new_params;
    dt_iop_atrous_params_t *d = (dt_iop_atrous_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    memcpy(n, o, sizeof(dt_iop_atrous_params_v1_t));
    n->mix = 1.0f;
    return 0;
  }

  return 1;
}

static int get_samples(float *t, const dt_iop_atrous_data_t *const d, const dt_iop_roi_t *roi_in,
                       const dt_dev_pixelpipe_iop_t *const piece)
{
  const float scale = roi_in->scale;
  const float supp0
      = MIN(2 * (2 << (MAX_NUM_SCALES - 1)) + 1, MAX(piece->buf_in.height, piece->buf_in.width) * 0.2f);
  const float i0 = dt_log2f((supp0 - 1.0f) * .5f);
  int i = 0;
  for(; i < MAX_NUM_SCALES; i++)
  {
    // actual filter support on scaled buffer
    const float supp = 2 * (2 << i) + 1;
    // approximates this filter size on unscaled input image:
    const float supp_in = supp * (1.0f / scale);
    const float i_in = dt_log2f((supp_in - 1) * .5f) - 1.0f;
    t[i] = 1.0f - (i_in + .5f) / i0;
    if(t[i] < 0.0f) break;
  }
  return i;
}

static int get_scales(float (*thrs)[4], float (*boost)[4], float *sharp, const dt_iop_atrous_data_t *const d,
                      const dt_iop_roi_t *roi_in, const dt_dev_pixelpipe_iop_t *const piece)
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
  const float scale = roi_in->scale / piece->iscale;
  // largest desired filter on input buffer (20% of input dim)
  const float supp0
      = MIN(2 * (2 << (MAX_NUM_SCALES - 1)) + 1,
            MAX(piece->buf_in.height * piece->iscale, piece->buf_in.width * piece->iscale) * 0.2f);
  const float i0 = dt_log2f((supp0 - 1.0f) * .5f);
  int i = 0;
  for(; i < MAX_NUM_SCALES; i++)
  {
    // actual filter support on scaled buffer
    const float supp = 2 * (2 << i) + 1;
    // approximates this filter size on unscaled input image:
    const float supp_in = supp * (1.0f / scale);
    const float i_in = dt_log2f((supp_in - 1) * .5f) - 1.0f;
    // i_in = max_scale .. .. .. 0
    const float t = 1.0f - (i_in + .5f) / i0;
    boost[i][3] = boost[i][0] = 2.0f * dt_draw_curve_calc_value(d->curve[atrous_L], t);
    boost[i][1] = boost[i][2] = 2.0f * dt_draw_curve_calc_value(d->curve[atrous_c], t);
    for(int k = 0; k < 4; k++) boost[i][k] *= boost[i][k];
    thrs[i][0] = thrs[i][3] = powf(2.0f, -7.0f * (1.0f - t)) * 10.0f
                              * dt_draw_curve_calc_value(d->curve[atrous_Lt], t);
    thrs[i][1] = thrs[i][2] = powf(2.0f, -7.0f * (1.0f - t)) * 20.0f
                              * dt_draw_curve_calc_value(d->curve[atrous_ct], t);
    sharp[i] = 0.0025f * dt_draw_curve_calc_value(d->curve[atrous_s], t);
    // printf("scale %d boost %f %f thrs %f %f sharpen %f\n", i, boost[i][0], boost[i][2], thrs[i][0],
    // thrs[i][1], sharp[i]);
    if(t < 0.0f) break;
  }
  // ensure that return value max_scale is such that
  // 2 * 2 *(1 << max_scale) <= min(width, height)
  const int max_scale_roi = (int)floorf(dt_log2f((float)MIN(roi_in->width, roi_in->height))) - 2;
  return MIN(max_scale_roi, i);
}

/* just process the supplied image buffer, upstream default_process_tiling() does the rest */
static void process_wavelets(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                             const void *const i, void *const o, const dt_iop_roi_t *const roi_in,
                             const dt_iop_roi_t *const roi_out, const eaw_decompose_t decompose,
                             const eaw_synthesize_t synthesize)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)piece->data;
  dt_aligned_pixel_t thrs[MAX_NUM_SCALES];
  dt_aligned_pixel_t boost[MAX_NUM_SCALES];
  float sharp[MAX_NUM_SCALES];
  const int max_scale = get_scales(thrs, boost, sharp, d, roi_in, piece);
  const int max_mult = 1u << (max_scale - 1);

  const int width = roi_out->width;
  const int height = roi_out->height;

  if(self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    dt_iop_atrous_gui_data_t *g = (dt_iop_atrous_gui_data_t *)self->gui_data;
    g->num_samples = get_samples(g->sample, d, roi_in, piece);
    // tries to acquire gdk lock and this prone to deadlock:
    // dt_control_queue_draw(GTK_WIDGET(g->area));
  }

  // corner case of extremely small image. this is not really likely to happen but would
  // lead to out of bounds memory access
  if(width < 2 * max_mult || height < 2 * max_mult)
  {
    dt_iop_image_copy_by_size(o, i, width, height, 4);
    return;
  }

  float *const restrict out = (float*)o;
  float *restrict detail = NULL;
  float *restrict tmp = NULL;
  float *restrict tmp2 = NULL;

  if(!dt_iop_alloc_image_buffers(self, roi_in, roi_out, 4, &tmp, 4, &tmp2, 4, &detail, 0))
  {
    dt_iop_copy_image_roi(out, i, piece->colors, roi_in, roi_out, TRUE);
    return;
  }

  float *buf1 = (float *)i;
  float *buf2 = tmp;

  // clear the output buffer, which will be accumulating all of the detail scales
  dt_iop_image_fill(out, 0.0f, width, height, 4);

  // now do the wavelet decomposition, immediately synthesizing the detail scale into the final output so
  // that we don't need to store it past the current scale's iteration
  for(int scale = 0; scale < max_scale; scale++)
  {
    decompose(buf2, buf1, detail, scale, sharp[scale], width, height);
    synthesize(out, out, detail, thrs[scale], boost[scale], width, height);
    if(scale == 0) buf1 = (float *)tmp2; // now switch to second scratch for buffer ping-pong between buf1 and buf2
    float *buf3 = buf2;
    buf2 = buf1;
    buf1 = buf3;
  }

  // add in the final residue
#ifdef _OPENMP
#pragma omp simd aligned(buf1, out : 64)
#endif
  for(size_t k = 0; k < (size_t)4 * width * height; k++)
    out[k] += buf1[k];

  dt_free_align(detail);
  dt_free_align(tmp);
  dt_free_align(tmp2);
  return;
}

void process(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
             void *const o, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  process_wavelets(self, piece, i, o, roi_in, roi_out, eaw_decompose, eaw_synthesize);
}

#if defined(__SSE2__)
void process_sse2(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
                  void *const o, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  process_wavelets(self, piece, i, o, roi_in, roi_out, eaw_decompose_sse2, eaw_synthesize);
}
#endif

#ifdef HAVE_OPENCL

#ifdef USE_NEW_CL
/* this version is adapted to the new global tiling mechanism. it no longer does tiling by itself. */
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)piece->data;
  dt_aligned_pixel_t thrs[MAX_NUM_SCALES];
  dt_aligned_pixel_t boost[MAX_NUM_SCALES];
  float sharp[MAX_NUM_SCALES];
  const int max_scale = get_scales(thrs, boost, sharp, d, roi_in, piece);

  if(self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    dt_iop_atrous_gui_data_t *g = (dt_iop_atrous_gui_data_t *)self->gui_data;
    g->num_samples = get_samples(g->sample, d, roi_in, piece);
    // dt_control_queue_redraw_widget(GTK_WIDGET(g->area));
    // tries to acquire gdk lock and this prone to deadlock:
    // dt_control_queue_draw(GTK_WIDGET(g->area));
  }

  dt_iop_atrous_global_data_t *gd = (dt_iop_atrous_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem dev_filter = NULL;
  cl_mem dev_tmp = NULL;
  cl_mem dev_tmp2 = NULL;
  cl_mem dev_detail = NULL;

  float m[] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f }; // 1/16, 4/16, 6/16, 4/16, 1/16
  float mm[5][5];
  for(int j = 0; j < 5; j++)
    for(int i = 0; i < 5; i++) mm[j][i] = m[i] * m[j];

  dev_filter = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 25, mm);
  if(dev_filter == NULL) goto error;

  /* allocate space for two temporary buffer to participate_in in the buffer ping-pong below.  We need dev_out
     to accumulate the result and dev_in needs to stay unchanged for blendops */
  dev_tmp = dt_opencl_alloc_device(devid, roi_out->width, roi_out->height, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;
  dev_tmp2 = dt_opencl_alloc_device(devid, roi_out->width, roi_out->height, sizeof(float) * 4);
  if(dev_tmp2 == NULL) goto error;

  /* allocate a buffer for storing the detail information. */
  dev_detail = dt_opencl_alloc_device(devid, roi_out->width, roi_out->height, sizeof(float) * 4);
  if(dev_detail == NULL) goto error;

  const int width = roi_out->width;
  const int height = roi_out->height;
  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  // clear dev_out to zeros, as we will be incrementally accumulating results there
  dt_opencl_set_kernel_args(devid, gd->kernel_zero, 0, CLARG(dev_out));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zero, sizes);
  if(err != CL_SUCCESS) goto error;

  // the buffers for the buffer ping-pong.  We start with dev_in as the input half for the first
  // scale, then switch to using dev_tmp and dev_tmp2 as the two scratch buffers
  void* dev_buf1 = &dev_in;
  void* dev_buf2 = &dev_tmp;

  /* decompose image into detail scales and coarse (the latter is left in dev_tmp or dev_out) */
  for(int s = 0; s < max_scale; s++)
  {
    const int scale = s;

    // run the decomposition
    dt_opencl_set_kernel_args(devid, gd->kernel_decompose, 0, CLARG(dev_buf2), CLARG(dev_buf1), CLARG(dev_detail),
      CLARG(width), CLARG(height), CLARG(scale), CLARG(sharp[s]), CLARG(dev_filter));

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_decompose, sizes);
    if(err != CL_SUCCESS) goto error;

    // indirectly give gpu some air to breathe (and to do display related stuff)
    dt_iop_nap(darktable.opencl->micro_nap);

    // now immediately run the synthesis for the current scale, accumulating the details into dev_out
    dt_opencl_set_kernel_args(devid, gd->kernel_synthesize, 0, CLARG(dev_out), CLARG(dev_out), CLARG(dev_detail),
      CLARG(width), CLARG(height), CLARG(thrs[scale][0]), CLARG(thrs[scale][1]), CLARG(thrs[scale][2]),
      CLARG(thrs[scale][3]), CLARG(boost[scale][0]), CLARG(boost[scale][1]), CLARG(boost[scale][2]),
      CLARG(boost[scale][3]));

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_synthesize, sizes);
    if(err != CL_SUCCESS) goto error;

    // indirectly give gpu some air to breathe (and to do display related stuff)
    dt_iop_nap(darktable.opencl->micro_nap);

    // swap scratch buffers
    if(scale == 0) dev_buf1 = dev_tmp2;
    void* tmp = dev_buf2;
    dev_buf2 = dev_buf1;
    dev_buf1 = tmp;
  }

  // add the residue (the coarse scale from the final decomposition) to the accumulated details
  dt_opencl_set_kernel_args(devid, gd->kernel_addbuffers, 0, CLARG(dev_out), CLARG(dev_buf1));

  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_addbuffers, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_finish_sync_pipe(devid, piece->pipe->type);

  dt_opencl_release_mem_object(dev_filter);
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_tmp2);
  dt_opencl_release_mem_object(dev_detail);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_filter);
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_tmp2);
  dt_opencl_release_mem_object(dev_detail);
  dt_print(DT_DEBUG_OPENCL, "[opencl_atrous] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

#else // ======== old, memory-hungry implementation ========================================================

/* this version is adapted to the new global tiling mechanism. it no longer does tiling by itself. */
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)piece->data;
  dt_aligned_pixel_t thrs[MAX_NUM_SCALES];
  dt_aligned_pixel_t boost[MAX_NUM_SCALES];
  float sharp[MAX_NUM_SCALES];
  const int max_scale = get_scales(thrs, boost, sharp, d, roi_in, piece);

  if(self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    dt_iop_atrous_gui_data_t *g = (dt_iop_atrous_gui_data_t *)self->gui_data;
    g->num_samples = get_samples(g->sample, d, roi_in, piece);
    // dt_control_queue_redraw_widget(GTK_WIDGET(g->area));
    // tries to acquire gdk lock and this prone to deadlock:
    // dt_control_queue_draw(GTK_WIDGET(g->area));
  }

  dt_iop_atrous_global_data_t *gd = (dt_iop_atrous_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem dev_filter = NULL;
  cl_mem dev_tmp = NULL;
  cl_mem *dev_detail = calloc(max_scale, sizeof(cl_mem));

  float m[] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f }; // 1/16, 4/16, 6/16, 4/16, 1/16
  float mm[5][5];
  for(int j = 0; j < 5; j++)
    for(int i = 0; i < 5; i++)
      mm[j][i] = m[i] * m[j];

  dev_filter = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 25, mm);
  if(dev_filter == NULL) goto error;

  /* allocate space for a temporary buffer. we don't want to use dev_in in the buffer ping-pong below, as we
     need to keep it for blendops */
  dev_tmp = dt_opencl_alloc_device(devid, roi_out->width, roi_out->height, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;

  /* allocate space to store detail information. Requires a number of additional buffers, each with full image
   * size */
  for(int k = 0; k < max_scale; k++)
  {
    dev_detail[k] = dt_opencl_alloc_device(devid, roi_out->width, roi_out->height, sizeof(float) * 4);
    if(dev_detail[k] == NULL) goto error;
  }

  const int width = roi_out->width;
  const int height = roi_out->height;
  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };

  // copy original input from dev_in -> dev_out as starting point
  err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
  if(err != CL_SUCCESS) goto error;

  /* decompose image into detail scales and coarse (the latter is left in dev_tmp or dev_out) */
  for(int s = 0; s < max_scale; s++)
  {
    const int scale = s;

    if(s & 1)
    {
      dt_opencl_set_kernel_args(devid, gd->kernel_decompose, 0, CLARG(dev_tmp), CLARG(dev_out));
    }
    else
    {
      dt_opencl_set_kernel_args(devid, gd->kernel_decompose, 0, CLARG(dev_out), CLARG(dev_tmp));
    }
    dt_opencl_set_kernel_args(devid, gd->kernel_decompose, 2, CLARG(dev_detail[s]), CLARG(width), CLARG(height),
      CLARG(scale), CLARG(sharp[s]), CLARG(dev_filter));

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_decompose, sizes);
    if(err != CL_SUCCESS) goto error;

    // indirectly give gpu some air to breathe (and to do display related stuff)
    dt_iop_nap(dt_opencl_micro_nap(devid));
  }

  /* now synthesize again */
  for(int scale = max_scale - 1; scale >= 0; scale--)
  {
    if(scale & 1)
    {
      dt_opencl_set_kernel_args(devid, gd->kernel_synthesize, 0, CLARG(dev_tmp), CLARG(dev_out));
    }
    else
    {
      dt_opencl_set_kernel_args(devid, gd->kernel_synthesize, 0, CLARG(dev_out), CLARG(dev_tmp));
    }

    dt_opencl_set_kernel_args(devid, gd->kernel_synthesize, 2, CLARG(dev_detail[scale]), CLARG(width),
      CLARG(height), CLARG(thrs[scale][0]), CLARG(thrs[scale][1]), CLARG(thrs[scale][2]), CLARG(thrs[scale][3]),
      CLARG(boost[scale][0]), CLARG(boost[scale][1]), CLARG(boost[scale][2]), CLARG(boost[scale][3]));

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_synthesize, sizes);
    if(err != CL_SUCCESS) goto error;

    // indirectly give gpu some air to breathe (and to do display related stuff)
    dt_iop_nap(dt_opencl_micro_nap(devid));
  }

  dt_opencl_finish_sync_pipe(devid, piece->pipe->type);

  dt_opencl_release_mem_object(dev_filter);
  dt_opencl_release_mem_object(dev_tmp);
  for(int k = 0; k < max_scale; k++)
    dt_opencl_release_mem_object(dev_detail[k]);
  free(dev_detail);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_filter);
  dt_opencl_release_mem_object(dev_tmp);
  for(int k = 0; k < max_scale; k++)
    dt_opencl_release_mem_object(dev_detail[k]);
  free(dev_detail);
  dt_print(DT_DEBUG_OPENCL, "[opencl_atrous] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif // USE_NEW_CL

#endif // HAVE_OPENCL

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)piece->data;
  dt_aligned_pixel_t thrs[MAX_NUM_SCALES];
  dt_aligned_pixel_t boost[MAX_NUM_SCALES];
  float sharp[MAX_NUM_SCALES];
  const int max_scale = get_scales(thrs, boost, sharp, d, roi_in, piece);
  const int max_filter_radius = 2 * (1 << max_scale); // 2 * 2^max_scale

  tiling->factor = 5.0f;                // in + out + 2*tmp + details
  tiling->factor_cl = 3.0f + max_scale; // in + out + tmp + scale buffers
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = max_filter_radius;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  dt_iop_atrous_params_t *d = module->default_params;

  for(int k = 0; k < BANDS; k++)
  {
    d->y[atrous_Lt][k] = d->y[atrous_ct][k] = 0.0f;
    for(int c = atrous_L; c <= atrous_ct; c++)
      d->x[c][k] = k / (BANDS - 1.0f);
  }
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 1; // from programs.conf
  dt_iop_atrous_global_data_t *gd
      = (dt_iop_atrous_global_data_t *)malloc(sizeof(dt_iop_atrous_global_data_t));
  module->data = gd;
  gd->kernel_decompose = dt_opencl_create_kernel(program, "eaw_decompose");
  gd->kernel_synthesize = dt_opencl_create_kernel(program, "eaw_synthesize");
#ifdef USE_NEW_CL
  gd->kernel_zero = dt_opencl_create_kernel(program, "eaw_zero");
  gd->kernel_addbuffers = dt_opencl_create_kernel(program, "eaw_addbuffers");
#endif
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_atrous_global_data_t *gd = (dt_iop_atrous_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_decompose);
  dt_opencl_free_kernel(gd->kernel_synthesize);
#ifdef USE_NEW_CL
  dt_opencl_free_kernel(gd->kernel_zero);
  dt_opencl_free_kernel(gd->kernel_addbuffers);
#endif
  free(module->data);
  module->data = NULL;
}

static inline void _apply_mix(dt_iop_module_t *self,
                              const int ch, const int k,
                              const float mix,
                              const float px, const float py, float *x, float *y)
{
  dt_iop_atrous_params_t *dp = (dt_iop_atrous_params_t *)self->default_params;
  *x = fminf(1.0f, fmaxf(0.0f, px + (mix - 1.0f) * (px - dp->x[ch][k])));
  *y = fminf(1.0f, fmaxf(0.0f, py + (mix - 1.0f) * (py - dp->y[ch][k])));
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_atrous_params_t *p = (dt_iop_atrous_params_t *)params;
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)piece->data;

#if 0
  printf("---------- atrous preset begin\n");
  printf("p.octaves = %d;  p.mix = %.2f\n", p->octaves, p->mix);
  for(int ch=0; ch<atrous_none; ch++) for(int k=0; k<BANDS; k++)
    {
      printf("p.x[%d][%d] = %f;\n", ch, k, p->x[ch][k]);
      printf("p.y[%d][%d] = %f;\n", ch, k, p->y[ch][k]);
    }
  printf("---------- atrous preset end\n");
#endif
  d->octaves = p->octaves;
  for(int ch = 0; ch < atrous_none; ch++)
    for(int k = 0; k < BANDS; k++)
    {
      float x, y;
      _apply_mix(self, ch, k, p->mix, p->x[ch][k], p->y[ch][k], &x, &y);
      dt_draw_curve_set_point(d->curve[ch], k, x, y);
    }
  int l = 0;
  for(int k = (int)MIN(pipe->iwidth * pipe->iscale, pipe->iheight * pipe->iscale); k; k >>= 1) l++;
  d->octaves = MIN(BANDS, l);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)malloc(sizeof(dt_iop_atrous_data_t));
  dt_iop_atrous_params_t *default_params = (dt_iop_atrous_params_t *)self->default_params;
  piece->data = (void *)d;
  for(int ch = 0; ch < atrous_none; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
    for(int k = 0; k < BANDS; k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->x[ch][k], default_params->y[ch][k]);
  }
  int l = 0;
  for(int k = (int)MIN(pipe->iwidth * pipe->iscale, pipe->iheight * pipe->iscale); k; k >>= 1) l++;
  d->octaves = MIN(BANDS, l);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_atrous_data_t *d = (dt_iop_atrous_data_t *)(piece->data);
  for(int ch = 0; ch < atrous_none; ch++)
    dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
  piece->data = NULL;
}

#define GAUSS(x, sigma) expf( -(1.0f - x) * (1.0f - x) / (sigma * sigma)) / (2.0 * sigma * powf(M_PI, 0.5f))

void init_presets(dt_iop_module_so_t *self)
{
  dt_database_start_transaction(darktable.db);
  dt_iop_atrous_params_t p;
  p.octaves = 7;
  p.mix = 1.0f;

  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = fmaxf(.5f, .75f - .5f * k / (BANDS - 1.0));
    p.y[atrous_c][k] = fmaxf(.5f, .55f - .5f * k / (BANDS - 1.0));
    p.y[atrous_s][k] = fminf(.5f, .2f + .35f * k / (BANDS - 1.0));
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  dt_gui_presets_add_generic(C_("eq_preset", "coarse"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = .5f + .25f * k / (float)BANDS;
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .5f;
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = .2f * k / (float)BANDS;
    p.y[atrous_ct][k] = .3f * k / (float)BANDS;
  }
  dt_gui_presets_add_generic(_("denoise & sharpen"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = .5f + .25f * k / (float)BANDS;
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .5f;
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  dt_gui_presets_add_generic(C_("atrous", "sharpen"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = .5f;
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .0f;
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = .0f;
    p.y[atrous_ct][k] = fmaxf(0.0f, (.60f * k / (float)BANDS) - 0.30f);
  }
  dt_gui_presets_add_generic(_("denoise chroma"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = .5f; //-.2f*k/(float)BANDS;
    p.y[atrous_c][k] = .5f; // fmaxf(0.0f, .5f-.3f*k/(float)BANDS);
    p.y[atrous_s][k] = .5f;
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = .2f * k / (float)BANDS;
    p.y[atrous_ct][k] = .3f * k / (float)BANDS;
  }
  dt_gui_presets_add_generic(_("denoise"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = fminf(.5f, .3f + .35f * k / (BANDS - 1.0));
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .0f;
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  p.y[atrous_L][0] = .5f;
  dt_gui_presets_add_generic(_("bloom"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = 0.6f;
    p.y[atrous_c][k] = .55f;
    p.y[atrous_s][k] = .0f;
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  dt_gui_presets_add_generic(_("clarity"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  float sigma = 3.f / (float)(BANDS - 1);

  for(int k = 0; k < BANDS; k++)
  {
    const float x = k / (float)(BANDS - 1);
    const float fine = GAUSS(x, 0.5 * sigma);
    const float medium = GAUSS(x, sigma);
    const float coarse = GAUSS(x, 2 * sigma);
    const float coeff = 0.5f + (coarse + medium + fine) / 16.0f;
    const float noise = (coarse + medium + fine) / 128.f;

    p.x[atrous_L][k] = p.x[atrous_c][k] = p.x[atrous_s][k] = x;
    p.y[atrous_L][k] = p.y[atrous_s][k] = coeff;
    p.y[atrous_c][k] = 0.5f;
    p.x[atrous_Lt][k] = p.x[atrous_ct][k] = x;
    p.y[atrous_Lt][k] = p.y[atrous_ct][k] = noise;
  }
  dt_gui_presets_add_generic(_("deblur: large blur, strength 3"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < BANDS; k++)
  {
    const float x = k / (float)(BANDS - 1);
    const float fine = GAUSS(x, 0.5 * sigma);
    const float medium = GAUSS(x, sigma);
    const float coeff = 0.5f + (medium + fine) / 16.0f;
    const float noise = (medium + fine) / 128.f;

    p.x[atrous_L][k] = p.x[atrous_c][k] = p.x[atrous_s][k] = x;
    p.y[atrous_L][k] = p.y[atrous_s][k] = coeff;
    p.y[atrous_c][k] = 0.5f;
    p.x[atrous_Lt][k] = p.x[atrous_ct][k] = x;
    p.y[atrous_Lt][k] = p.y[atrous_ct][k] = noise;
  }
  dt_gui_presets_add_generic(_("deblur: medium blur, strength 3"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < BANDS; k++)
  {
    const float x =  k / (float)(BANDS - 1);
    const float fine = GAUSS(x, 0.5 * sigma);
    const float coeff = 0.5f + fine / 16.f;
    const float noise = fine / 128.f;

    p.x[atrous_L][k] = p.x[atrous_c][k] = p.x[atrous_s][k] = x;
    p.y[atrous_L][k] = p.y[atrous_s][k] = coeff;
    p.y[atrous_c][k] = 0.5f;
    p.x[atrous_Lt][k] = p.x[atrous_ct][k] = x;
    p.y[atrous_Lt][k] = p.y[atrous_ct][k] = noise;
  }
  dt_gui_presets_add_generic(_("deblur: fine blur, strength 3"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < BANDS; k++)
  {
    const float x =  k / (float)(BANDS - 1);
    const float fine = GAUSS(x, 0.5 * sigma);
    const float medium = GAUSS(x, sigma);
    const float coarse = GAUSS(x, 2 * sigma);
    const float coeff = 0.5f + (coarse + medium + fine) / 24.0f;
    const float noise = (coarse + medium + fine) / 192.f;

    p.x[atrous_L][k] = p.x[atrous_c][k] = p.x[atrous_s][k] = x;
    p.y[atrous_L][k] = p.y[atrous_s][k] = coeff;
    p.y[atrous_c][k] = 0.5f;
    p.x[atrous_Lt][k] = p.x[atrous_ct][k] = x;
    p.y[atrous_Lt][k] = p.y[atrous_ct][k] = noise;
  }
  dt_gui_presets_add_generic(_("deblur: large blur, strength 2"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < BANDS; k++)
  {
    const float x =  k / (float)(BANDS - 1);
    const float fine = GAUSS(x, 0.5 * sigma);
    const float medium = GAUSS(x, sigma);
    const float coeff = 0.5f + (medium + fine) / 24.0f;
    const float noise = (medium + fine) / 192.f;

    p.x[atrous_L][k] = p.x[atrous_c][k] = p.x[atrous_s][k] = x;
    p.y[atrous_L][k] = p.y[atrous_s][k] = coeff;
    p.y[atrous_c][k] = 0.5f;
    p.x[atrous_Lt][k] = p.x[atrous_ct][k] = x;
    p.y[atrous_Lt][k] = p.y[atrous_ct][k] = noise;
  }
  dt_gui_presets_add_generic(_("deblur: medium blur, strength 2"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < BANDS; k++)
  {
    const float x =  k / (float)(BANDS - 1);
    const float fine = GAUSS(x, 0.5 * sigma);
    const float coeff = 0.5f + fine / 24.0f;
    const float noise = fine / 192.f;

    p.x[atrous_L][k] = p.x[atrous_c][k] = p.x[atrous_s][k] = x;
    p.y[atrous_L][k] = p.y[atrous_s][k] = coeff;
    p.y[atrous_c][k] = 0.5f;
    p.x[atrous_Lt][k] = p.x[atrous_ct][k] = x;
    p.y[atrous_Lt][k] = p.y[atrous_ct][k] = noise;
  }
  dt_gui_presets_add_generic(_("deblur: fine blur, strength 2"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < BANDS; k++)
  {
    const float x =  k / (float)(BANDS - 1);
    const float fine = GAUSS(x, 0.5 * sigma);
    const float medium = GAUSS(x, sigma);
    const float coarse = GAUSS(x, 2 * sigma);
    const float coeff = 0.5f + (coarse + medium + fine) / 32.0f;
    const float noise = (coarse + medium + fine) / 128.f;

    p.x[atrous_L][k] = p.x[atrous_c][k] = p.x[atrous_s][k] = x;
    p.y[atrous_L][k] = p.y[atrous_s][k] = coeff;
    p.y[atrous_c][k] = 0.5f;
    p.x[atrous_Lt][k] = p.x[atrous_ct][k] = x;
    p.y[atrous_Lt][k] = p.y[atrous_ct][k] = noise;
  }
  dt_gui_presets_add_generic(_("deblur: large blur, strength 1"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < BANDS; k++)
  {
    const float x =  k / (float)(BANDS - 1);
    const float fine = GAUSS(x, 0.5 * sigma);
    const float medium = GAUSS(x, sigma);
    const float coeff = 0.5f + (medium + fine) / 32.0f;
    const float noise = (medium + fine) / 128.f;

    p.x[atrous_L][k] = p.x[atrous_c][k] = p.x[atrous_s][k] = x;
    p.y[atrous_L][k] = p.y[atrous_s][k] = coeff;
    p.y[atrous_c][k] = 0.5f;
    p.x[atrous_Lt][k] = p.x[atrous_ct][k] = x;
    p.y[atrous_Lt][k] = p.y[atrous_ct][k] = noise;
  }
  dt_gui_presets_add_generic(_("deblur: medium blur, strength 1"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < BANDS; k++)
  {
    const float x =  k / (float)(BANDS - 1);
    const float fine = GAUSS(x, 0.5 * sigma);
    const float coeff = 0.5f + fine / 32.f;
    const float noise = fine / 128.f;

    p.x[atrous_L][k] = p.x[atrous_c][k] = p.x[atrous_s][k] = x;
    p.y[atrous_L][k] = p.y[atrous_s][k] = coeff;
    p.y[atrous_c][k] = 0.5f;
    p.x[atrous_Lt][k] = p.x[atrous_ct][k] = x;
    p.y[atrous_Lt][k] = p.y[atrous_ct][k] = noise;
  }
  dt_gui_presets_add_generic(_("deblur: fine blur, strength 1"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_database_release_transaction(darktable.db);
}

static void reset_mix(dt_iop_module_t *self)
{
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  dt_iop_atrous_params_t *p = (dt_iop_atrous_params_t *)self->params;
  c->drag_params = *p;
  ++darktable.gui->reset;
  dt_bauhaus_slider_set(c->mix, p->mix);
  --darktable.gui->reset;
}

void gui_update(struct dt_iop_module_t *self)
{
  reset_mix(self);
  dt_iop_cancel_history_update(self);
  gtk_widget_queue_draw(self->widget);
}


// gui stuff:

static gboolean area_enter_leave_notify(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *self)
{
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  c->in_curve = event->type == GDK_ENTER_NOTIFY;
  if(!c->dragging)
    c->x_move = -1;

  gtk_widget_queue_draw(widget);
  return FALSE;
}

// fills in new parameters based on mouse position (in 0,1)
static void get_params(dt_iop_atrous_params_t *p, const int ch, const double mouse_x, const double mouse_y,
                       const float rad)
{
  for(int k = 0; k < BANDS; k++)
  {
    const float f = expf(-(mouse_x - p->x[ch][k]) * (mouse_x - p->x[ch][k]) / (rad * rad));
    p->y[ch][k] = MAX(0.0f, MIN(1.0f, (1 - f) * p->y[ch][k] + f * mouse_y));
  }
}

static gboolean area_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  dt_iop_atrous_params_t p = *(dt_iop_atrous_params_t *)self->params;

  const float mix = c->in_curve ? 1.0f : p.mix;

  for(int k = 0; k < BANDS; k++)
  {
    const int ch2 = (int)c->channel2;
    float x, y;
    _apply_mix(self, ch2, k, mix, p.x[ch2][k], p.y[ch2][k], &x, &y);
    dt_draw_curve_set_point(c->minmax_curve, k, x, y);
  }

  const int inset = INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg, match color of the notebook tabs:
  GdkRGBA bright_bg_color, really_dark_bg_color;
  GtkStyleContext *context = gtk_widget_get_style_context(self->expander);
  gboolean color_found = gtk_style_context_lookup_color (context, "selected_bg_color", &bright_bg_color);
  if(!color_found)
  {
    bright_bg_color.red = 1.0;
    bright_bg_color.green = 0.0;
    bright_bg_color.blue = 0.0;
    bright_bg_color.alpha = 1.0;
  }

  color_found = gtk_style_context_lookup_color (context, "really_dark_bg_color", &really_dark_bg_color);
  if(!color_found)
  {
    really_dark_bg_color.red = 1.0;
    really_dark_bg_color.green = 0.0;
    really_dark_bg_color.blue = 0.0;
    really_dark_bg_color.alpha = 1.0;
  }

  gdk_cairo_set_source_rgba(cr, &bright_bg_color);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  gdk_cairo_set_source_rgba(cr, &really_dark_bg_color);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  gdk_cairo_set_source_rgba(cr, &bright_bg_color);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if(c->mouse_y > 0 || c->dragging)
  {
    const int ch2 = (int)c->channel2;

    // draw min/max curves:
    get_params(&p, ch2, c->mouse_x, 1., c->mouse_radius);
    for(int k = 0; k < BANDS; k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_min_xs, c->draw_min_ys);

    p = *(dt_iop_atrous_params_t *)self->params;
    get_params(&p, ch2, c->mouse_x, .0, c->mouse_radius);
    for(int k = 0; k < BANDS; k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_max_xs, c->draw_max_ys);
  }

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  gdk_cairo_set_source_rgba(cr, &really_dark_bg_color);
  dt_draw_grid(cr, 8, 0, 0, width, height);

  cairo_save(cr);

  // draw selected cursor
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_translate(cr, 0, height);

// draw frequency histogram in bg.
#if 1
  if(c->num_samples > 0)
  {
    cairo_save(cr);
    for(int k = 1; k < c->num_samples; k += 2)
    {
      cairo_set_source_rgba(cr, really_dark_bg_color.red, really_dark_bg_color.green, really_dark_bg_color.blue, .3);
      cairo_move_to(cr, width * c->sample[k - 1], 0.0f);
      cairo_line_to(cr, width * c->sample[k - 1], -height);
      cairo_line_to(cr, width * c->sample[k], -height);
      cairo_line_to(cr, width * c->sample[k], 0.0f);
      cairo_fill(cr);
    }
    if(c->num_samples & 1)
    {
      cairo_move_to(cr, width * c->sample[c->num_samples - 1], 0.0f);
      cairo_line_to(cr, width * c->sample[c->num_samples - 1], -height);
      cairo_line_to(cr, 0.0f, -height);
      cairo_line_to(cr, 0.0f, 0.0f);
      cairo_fill(cr);
    }
    cairo_restore(cr);
  }
  if(c->band_max > 0)
  {
    cairo_save(cr);
    cairo_scale(cr, width / (BANDS - 1.0), -(height - DT_PIXEL_APPLY_DPI(5)) / c->band_max);
    cairo_set_source_rgba(cr, really_dark_bg_color.red, really_dark_bg_color.green, really_dark_bg_color.blue, .3);
    cairo_move_to(cr, 0, 0);
    for(int k = 0; k < BANDS; k++) cairo_line_to(cr, k, c->band_hist[k]);
    cairo_line_to(cr, BANDS - 1.0, 0.);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
  }
#endif

  // cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  for(int i = 0; i <= atrous_s; i++)
  {
    // draw curves, selected last.
    int ch = ((int)c->channel + i + 1) % (atrous_s + 1);
    int ch2 = -1;
    const float bgmul = i < atrous_s ? 0.5f : 1.0f;
    switch(ch)
    {
      case atrous_L:
        cairo_set_source_rgba(cr, .6, .6, .6, .3 * bgmul);
        ch2 = atrous_Lt;
        break;
      case atrous_c:
        cairo_set_source_rgba(cr, .4, .2, .0, .4 * bgmul);
        ch2 = atrous_ct;
        break;
      default: // case atrous_s:
        cairo_set_source_rgba(cr, .1, .2, .3, .4 * bgmul);
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
      for(int k = 0; k < BANDS; k++)
      {
        float x, y;
        _apply_mix(self, ch2, k, mix, p.x[ch2][k], p.y[ch2][k], &x, &y);
        dt_draw_curve_set_point(c->minmax_curve, k, x, y);
      }
      dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_xs, c->draw_ys);
      cairo_move_to(cr, width, -height * p.y[ch2][BANDS - 1]);
      for(int k = RES - 2; k >= 0; k--)
        cairo_line_to(cr, k * width / (float)(RES - 1), -height * c->draw_ys[k]);
    }
    else
      cairo_move_to(cr, 0, 0);
    for(int k = 0; k < BANDS; k++)
    {
      float x, y;
      _apply_mix(self, ch, k, mix, p.x[ch][k], p.y[ch][k], &x, &y);
      dt_draw_curve_set_point(c->minmax_curve, k, x, y);
    }
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_xs, c->draw_ys);
    for(int k = 0; k < RES; k++)
      cairo_line_to(cr, k * width / (float)(RES - 1), -height * c->draw_ys[k]);
    if(ch2 < 0)
      cairo_line_to(cr, width, 0);
    cairo_close_path(cr);
    cairo_stroke_preserve(cr);
    cairo_fill(cr);
  }

  if(c->mouse_y > 0 || c->dragging)
  {
    const int ch = (int)c->channel;
    const int ch2 = (int)c->channel2;

    // draw dots on knots
    cairo_save(cr);
    if(ch != ch2)
      cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    else
      cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
    for(int k = 0; k < BANDS; k++)
    {
      float x, y;
      _apply_mix(self, ch, k, mix, p.x[ch2][k], p.y[ch2][k], &x, &y);
      cairo_arc(cr, width * x, -height * y, DT_PIXEL_APPLY_DPI(3.0), 0.0, 2.0 * M_PI);
      if(c->x_move == k)
        cairo_fill(cr);
      else
        cairo_stroke(cr);
    }
    cairo_restore(cr);
  }

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max, if selected
    // cairo_set_source_rgba(cr, .6, .6, .6, .5);
    cairo_move_to(cr, 0, -height * c->draw_min_ys[0]);
    for(int k = 1; k < RES; k++)
      cairo_line_to(cr, k * width / (float)(RES - 1), -height * c->draw_min_ys[k]);
    for(int k = RES - 1; k >= 0; k--)
      cairo_line_to(cr, k * width / (float)(RES - 1), -height * c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgba(cr, .9, .9, .9, .5);
    const float pos = RES * c->mouse_x;
    int k = (int)pos;
    const float f = k - pos;
    if(k >= RES - 1) k = RES - 2;
    const float ht = -height * (f * c->draw_ys[k] + (1 - f) * c->draw_ys[k + 1]);
    cairo_arc(cr, c->mouse_x * width, ht, c->mouse_radius * width, 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

  // draw x positions
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  const float arrw = DT_PIXEL_APPLY_DPI(7.0f);
  for(int k = 1; k < BANDS - 1; k++)
  {
    cairo_move_to(cr, width * p.x[(int)c->channel][k], inset - DT_PIXEL_APPLY_DPI(1));
    cairo_rel_line_to(cr, -arrw * .5f, 0);
    cairo_rel_line_to(cr, arrw * .5f, -arrw);
    cairo_rel_line_to(cr, arrw * .5f, arrw);
    cairo_close_path(cr);
    if(c->x_move == k)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  cairo_restore(cr);

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw labels:
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, (.06 * height) * PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);
    gdk_cairo_set_source_rgba(cr, &really_dark_bg_color);
    //cairo_select_font_face(cr, "Roboto", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, .06 * height);
    pango_layout_set_text(layout, _("coarse"), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, .02 * width - ink.y, .14 * height + ink.width);
    cairo_save(cr);
    cairo_rotate(cr, -M_PI * .5f);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    pango_layout_set_text(layout, _("fine"), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, .98 * width - ink.height, .14 * height + ink.width);
    cairo_save(cr);
    cairo_rotate(cr, -M_PI * .5f);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    switch(c->channel2)
    {
      case atrous_L:
      case atrous_c:
        dt_atrous_show_upper_label(cr, _("contrasty"), layout, ink);
        dt_atrous_show_lower_label(cr, _("smooth"), layout, ink);
        break;
      case atrous_Lt:
      case atrous_ct:
        dt_atrous_show_upper_label(cr, _("smooth"), layout, ink);
        dt_atrous_show_lower_label(cr, _("noisy"), layout, ink);
        break;
      default: // case atrous_s:
        dt_atrous_show_upper_label(cr, _("bold"), layout, ink);
        dt_atrous_show_lower_label(cr, _("dull"), layout, ink);
        break;
    }
    pango_font_description_free(desc);
    g_object_unref(layout);
  }


  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean area_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  dt_iop_atrous_params_t *p = (dt_iop_atrous_params_t *)self->params;
  const int inset = INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int height = allocation.height - 2 * inset;
  const int width = allocation.width - 2 * inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
  c->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;

  darktable.control->element = 0;

  int ch2 = c->channel;
  if(c->channel == atrous_L) ch2 = atrous_Lt;
  if(c->channel == atrous_c) ch2 = atrous_ct;

  if(c->dragging)
  {
    // drag y-positions
    *p = c->drag_params;
    if(c->x_move >= 0)
    {
      const float mx = CLAMP(event->x - inset, 0, width) / (float)width;
      if(c->x_move > 0 && c->x_move < BANDS - 1)
      {
        const float minx = p->x[c->channel][c->x_move - 1] + 0.001f;
        const float maxx = p->x[c->channel][c->x_move + 1] - 0.001f;
        p->x[ch2][c->x_move] = p->x[c->channel][c->x_move] = fminf(maxx, fmaxf(minx, mx));
      }
    }
    else
    {
      get_params(p, c->channel2, c->mouse_x, c->mouse_y + c->mouse_pick, c->mouse_radius);
    }
    gtk_widget_queue_draw(widget);
    dt_iop_queue_history_update(self, FALSE);
  }
  else if(event->y > height)
  {
    // move x-positions
    c->x_move = 0;
    float dist = fabs(p->x[c->channel][0] - c->mouse_x);
    for(int k = 1; k < BANDS; k++)
    {
      const float d2 = fabs(p->x[c->channel][k] - c->mouse_x);
      if(d2 < dist)
      {
        c->x_move = k;
        dist = d2;
      }
    }
    darktable.control->element = c->x_move + 1;

    gtk_widget_queue_draw(widget);
  }
  else
  {
    // choose between bottom and top curve:
    const int ch = c->channel;
    float dist = 1000000.0f;
    for(int k = 0; k < BANDS; k++)
    {
      float d2 = fabs(p->x[c->channel][k] - c->mouse_x);
      if(d2 < dist)
      {
        if(fabs(c->mouse_y - p->y[ch][k]) < fabs(c->mouse_y - p->y[ch2][k]))
          c->channel2 = ch;
        else
          c->channel2 = ch2;
        dist = d2;
      }
    }
    // don't move x-positions:
    c->x_move = -1;
    gtk_widget_queue_draw(widget);
  }
  return TRUE;
}

static gboolean area_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // reset current curve
    dt_iop_atrous_params_t *p = (dt_iop_atrous_params_t *)self->params;
    dt_iop_atrous_params_t *d = (dt_iop_atrous_params_t *)self->default_params;
    dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
    reset_mix(self);
    for(int k = 0; k < BANDS; k++)
    {
      p->x[c->channel2][k] = d->x[c->channel2][k];
      p->y[c->channel2][k] = d->y[c->channel2][k];
    }
    gtk_widget_queue_draw(self->widget);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else if(event->button == 1)
  {
    // set active point
    dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
    reset_mix(self);
    const int inset = INSET;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    const int height = allocation.height - 2 * inset;
    const int width = allocation.width - 2 * inset;
    c->mouse_pick
        = dt_draw_curve_calc_value(c->minmax_curve, CLAMP(event->x - inset, 0, width) / (float)width);
    c->mouse_pick -= 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
    c->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean area_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
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

static gboolean area_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  int delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    c->mouse_radius = CLAMP(c->mouse_radius * (1.0 + 0.1 * delta_y), 0.25 / BANDS, 1.0);
    gtk_widget_queue_draw(widget);
  }
  return TRUE;
}

static void tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  if(darktable.gui->reset) return;
  c->channel = c->channel2 = (atrous_channel_t)page_num;
  gtk_widget_queue_draw(self->widget);
}

static void mix_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_atrous_params_t *p = (dt_iop_atrous_params_t *)self->params;
  p->mix = dt_bauhaus_slider_get(slider);
  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

enum
{
  DT_ACTION_EFFECT_ATROUS_RESET = DT_ACTION_EFFECT_DEFAULT_KEY,
  DT_ACTION_EFFECT_BOOST = DT_ACTION_EFFECT_DEFAULT_UP,
  DT_ACTION_EFFECT_REDUCE = DT_ACTION_EFFECT_DEFAULT_DOWN,
  DT_ACTION_EFFECT_RAISE = 3,
  DT_ACTION_EFFECT_LOWER = 4,
  DT_ACTION_EFFECT_RIGHT = 5,
  DT_ACTION_EFFECT_LEFT = 6,
};

const gchar *dt_action_effect_equalizer[]
  = { N_("reset"),
      N_("boost"),
      N_("reduce"),
      N_("raise"),
      N_("lower"),
      N_("right"),
      N_("left"),
      NULL };

const dt_action_element_def_t _action_elements_equalizer[]
  = { { N_("radius"  ), dt_action_effect_value     },
      { N_("coarsest"), dt_action_effect_equalizer },
      { N_("coarser" ), dt_action_effect_equalizer },
      { N_("coarse"  ), dt_action_effect_equalizer },
      { N_("fine"    ), dt_action_effect_equalizer },
      { N_("finer"   ), dt_action_effect_equalizer },
      { N_("finest"  ), dt_action_effect_equalizer },
      { } };

static float _action_process_equalizer(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  dt_iop_module_t *self = g_object_get_data(G_OBJECT(target), "iop-instance");
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  dt_iop_atrous_params_t *p = (dt_iop_atrous_params_t *)self->params;
  dt_iop_atrous_params_t *d = (dt_iop_atrous_params_t *)self->default_params;

  const int node = element - 1;
  const int ch1 = c->channel;
  const int ch2 = ch1 == atrous_L ? atrous_Lt
                : ch1 == atrous_c ? atrous_ct
                : ch1;

  if(!isnan(move_size))
  {
    gchar *toast = NULL;

    if(element)
    {
      switch(effect)
      {
      case DT_ACTION_EFFECT_ATROUS_RESET:
        p->y[ch1][node] = d->y[ch1][node];
        p->y[ch2][node] = d->y[ch2][node];

        toast = g_strdup_printf("%s, %s", _action_elements_equalizer[element].name, "reset");
        break;
      case DT_ACTION_EFFECT_REDUCE:
        move_size *= -1;
      case DT_ACTION_EFFECT_BOOST:
        get_params(p, ch1, p->x[ch1][node], p->y[ch1][node] + move_size / 100, c->mouse_radius);

        toast = g_strdup_printf("%s, %s %+.2f", _action_elements_equalizer[element].name,
                                ch1 == atrous_s ? _("sharpness") : _("boost"), p->y[ch1][node] * 2. - 1.);
        break;
      case DT_ACTION_EFFECT_LOWER:
        move_size *= -1;
      case DT_ACTION_EFFECT_RAISE:
        get_params(p, ch2, p->x[ch2][node], p->y[ch2][node] + move_size / 100, c->mouse_radius);

        toast = g_strdup_printf("%s, %s %.2f", _action_elements_equalizer[element].name,
                                _("threshold"), p->y[ch2][node]);
        break;
      case DT_ACTION_EFFECT_LEFT:
        move_size *= -1;
      case DT_ACTION_EFFECT_RIGHT:
        if(element > 1 && element < BANDS)
        {
          const float minx = p->x[ch1][node - 1] + 0.001f;
          const float maxx = p->x[ch1][node + 1] - 0.001f;
          p->x[ch1][node] = p->x[ch2][node]
            = fminf(maxx, fmaxf(minx, p->x[ch1][node] + move_size * (maxx - minx) / 100));
        }

        toast = g_strdup_printf("%s, %s %+.2f", _action_elements_equalizer[element].name,
                                _("x"), p->x[ch1][node]);
        break;
      default:
        fprintf(stderr, "[_action_process_equalizer] unknown shortcut effect (%d) for contrast equalizer node\n", effect);
        break;
      }

      dt_iop_queue_history_update(self, FALSE);
    }
    else // radius
    {
      float bottop = -1e6;
      switch(effect)
      {
      case DT_ACTION_EFFECT_RESET:
        c->mouse_radius = 1.0 / BANDS;
        break;
      case DT_ACTION_EFFECT_BOTTOM:
        bottop *= -1;
      case DT_ACTION_EFFECT_TOP:
        move_size = bottop;
      case DT_ACTION_EFFECT_DOWN:
        move_size *= -1;
      case DT_ACTION_EFFECT_UP:
        c->mouse_radius = CLAMP(c->mouse_radius * (1.0 + 0.1 * move_size), 0.25 / BANDS, 1.0);
        break;
      default:
        fprintf(stderr, "[_action_process_equalizer] unknown shortcut effect (%d) for contrast equalizer radius\n", effect);
        break;
      }

      toast = g_strdup_printf("%s %+.2f", _action_elements_equalizer[element].name, c->mouse_radius);
    }
    dt_action_widget_toast(DT_ACTION(self), target, toast);
    g_free(toast);

    gtk_widget_queue_draw(self->widget);
  }

  return element ? effect >= DT_ACTION_EFFECT_RIGHT ? p->x[ch1][node] :
                   effect >= DT_ACTION_EFFECT_RAISE ? p->y[ch2][node] + DT_VALUE_PATTERN_PERCENTAGE :
                   effect >= DT_ACTION_EFFECT_BOOST ? p->y[ch1][node] + DT_VALUE_PATTERN_PLUS_MINUS :
                   p->y[ch1][node] != d->y[ch1][node] || p->y[ch2][node] != d->y[ch2][node]
                 : c->mouse_radius + DT_VALUE_PATTERN_PERCENTAGE;
}

static const dt_shortcut_fallback_t _action_fallbacks_equalizer[]
  = { { .mods = GDK_SHIFT_MASK,   .effect = DT_ACTION_EFFECT_RAISE },
      { .mods = GDK_CONTROL_MASK, .effect = DT_ACTION_EFFECT_RIGHT },
      { } };

const dt_action_def_t _action_def_equalizer
  = { N_("contrast equalizer"),
      _action_process_equalizer,
      _action_elements_equalizer,
      _action_fallbacks_equalizer };

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_atrous_gui_data_t *c = IOP_GUI_ALLOC(atrous);
  dt_iop_atrous_params_t *p = (dt_iop_atrous_params_t *)self->default_params;

  c->num_samples = 0;
  c->band_max = 0;
  c->channel = c->channel2 = dt_conf_get_int("plugins/darkroom/atrous/gui_channel");
  int ch = (int)c->channel;
  c->minmax_curve = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
  for(int k = 0; k < BANDS; k++)
    (void)dt_draw_curve_add_point(c->minmax_curve, p->x[ch][k], p->y[ch][k]);
  c->mouse_x = c->mouse_y = c->mouse_pick = -1.0;
  c->dragging = 0;
  self->timeout_handle = 0;
  c->x_move = -1;
  c->mouse_radius = 1.0 / BANDS;
  c->in_curve = FALSE;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  static struct dt_action_def_t notebook_def = { };
  c->channel_tabs = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("channel"), GTK_WIDGET(c->channel_tabs), &notebook_def);
  dt_ui_notebook_page(c->channel_tabs, N_("luma"), _("change lightness at each feature size"));
  dt_ui_notebook_page(c->channel_tabs, N_("chroma"), _("change color saturation at each feature size"));
  dt_ui_notebook_page(c->channel_tabs, N_("edges"), _("change edge halos at each feature size\nonly changes results of luma and chroma tabs"));
  gtk_widget_show(gtk_notebook_get_nth_page(c->channel_tabs, c->channel));
  gtk_notebook_set_current_page(c->channel_tabs, c->channel);
  g_signal_connect(G_OBJECT(c->channel_tabs), "switch_page", G_CALLBACK(tab_switch), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->channel_tabs), FALSE, FALSE, 0);

  // graph
  c->area = GTK_DRAWING_AREA(dt_ui_resize_wrap(NULL, 0, "plugins/darkroom/atrous/aspect_percent"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), TRUE, TRUE, 0);

  g_object_set_data(G_OBJECT(c->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("graph"), GTK_WIDGET(c->area), &_action_def_equalizer);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(area_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(area_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "button-release-event", G_CALLBACK(area_button_release), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(area_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(area_enter_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "enter-notify-event", G_CALLBACK(area_enter_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(area_scrolled), self);

  // mix slider
  c->mix = dt_bauhaus_slider_from_params(self, N_("mix"));
  gtk_widget_set_tooltip_text(c->mix, _("make effect stronger or weaker"));
  g_signal_connect(G_OBJECT(c->mix), "value-changed", G_CALLBACK(mix_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_atrous_gui_data_t *c = (dt_iop_atrous_gui_data_t *)self->gui_data;
  dt_conf_set_int("plugins/darkroom/atrous/gui_channel", c->channel);
  dt_draw_curve_destroy(c->minmax_curve);
  dt_iop_cancel_history_update(self);

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

