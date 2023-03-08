/*
    This file is part of darktable,
    Copyright (C) 2012-2023 darktable developers.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/bilateral.h"
#include "common/bilateralcl.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <inttypes.h>

#define REDUCESIZE 64

DT_MODULE_INTROSPECTION(3, dt_iop_global_tonemap_params_t)

typedef enum _iop_operator_t
{
  OPERATOR_REINHARD, // $DESCRIPTION: "reinhard"
  OPERATOR_FILMIC,   // $DESCRIPTION: "filmic"
  OPERATOR_DRAGO     // $DESCRIPTION: "drago"
} _iop_operator_t;

typedef struct dt_iop_global_tonemap_params_t
{
  _iop_operator_t operator; // $DEFAULT: OPERATOR_DRAGO
  struct
  {
    float bias;      // $MIN: 0.5 $MAX: 1 $DEFAULT: 0.85 $DESCRIPTION: "bias"
    float max_light; // cd/m2 $MIN: 1 $MAX: 500 $DEFAULT: 100.0 $DESCRIPTION: "target"
  } drago;
  float detail; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0
} dt_iop_global_tonemap_params_t;

typedef struct dt_iop_global_tonemap_data_t
{
  _iop_operator_t operator;
  struct
  {
    float bias;
    float max_light; // cd/m2
  } drago;
  float detail;
} dt_iop_global_tonemap_data_t;

typedef struct dt_iop_global_tonemap_gui_data_t
{
  GtkWidget *operator;
  struct
  {
    GtkWidget *bias;
    GtkWidget *max_light;
  } drago;
  GtkWidget *detail;
  float lwmax;
  uint64_t hash;
} dt_iop_global_tonemap_gui_data_t;

typedef struct dt_iop_global_tonemap_global_data_t
{
  int kernel_pixelmax_first;
  int kernel_pixelmax_second;
  int kernel_global_tonemap_reinhard;
  int kernel_global_tonemap_drago;
  int kernel_global_tonemap_filmic;
} dt_iop_global_tonemap_global_data_t;


const char *name()
{
  return _("global tonemap");
}

const char *deprecated_msg()
{
  return _("this module is deprecated. please use the filmic rgb module instead.");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_DEPRECATED;
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version < 3 && new_version == 3)
  {
    dt_iop_global_tonemap_params_t *o = (dt_iop_global_tonemap_params_t *)old_params;
    dt_iop_global_tonemap_params_t *n = (dt_iop_global_tonemap_params_t *)new_params;

    // only appended detail, 0 is no-op
    memcpy(n, o, sizeof(dt_iop_global_tonemap_params_t) - sizeof(float));
    n->detail = 0.0f;
    return 0;
  }
  return 1;
}

static inline void process_reinhard(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                    const void *const ivoid, void *const ovoid,
                                    const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                    dt_iop_global_tonemap_data_t *data)
{
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, roi_out) \
  shared(in, out, data) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *inp = in + ch * k;
    float *outp = out + ch * k;
    float l = inp[0] / 100.0;
    outp[0] = 100.0 * (l / (1.0f + l));
    outp[1] = inp[1];
    outp[2] = inp[2];
  }
}

static inline void process_drago(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                 const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                 const dt_iop_roi_t *const roi_out, dt_iop_global_tonemap_data_t *data)
{
  dt_iop_global_tonemap_gui_data_t *g = (dt_iop_global_tonemap_gui_data_t *)self->gui_data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

  /* precalcs */
  const float eps = 0.0001f;
  float lwmax;
  float tmp_lwmax = NAN;

  // Drago needs the absolute Lmax value of the image. In pixelpipe FULL we can not reliably get this value
  // as the pixelpipe might only see part of the image (region of interest). Therefore we try to get lwmax from
  // the PREVIEW pixelpipe which luckily stores it for us.
  if(self->dev->gui_attached && g && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    dt_iop_gui_enter_critical_section(self);
    const uint64_t hash = g->hash;
    dt_iop_gui_leave_critical_section(self);

    // note that the case 'hash == 0' on first invocation in a session implies that g->lwmax
    // is NAN which initiates special handling below to avoid inconsistent results. in all
    // other cases we make sure that the preview pipe has left us with proper readings for
    // lwmax. if data are not yet there we need to wait (with timeout).
    if(hash != 0 && !dt_dev_sync_pixelpipe_hash(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, &self->gui_lock, &g->hash))
      dt_control_log(_("inconsistent output"));

    dt_iop_gui_enter_critical_section(self);
    tmp_lwmax = g->lwmax;
    dt_iop_gui_leave_critical_section(self);
  }

  // in all other cases we calculate lwmax here
  if(isnan(tmp_lwmax))
  {
    lwmax = eps;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(roi_out, in, ch) reduction(max : lwmax)      \
  schedule(static)
#endif
    for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
    {
      const float *inp = in + ch * k;
      lwmax = fmaxf(lwmax, (inp[0] * 0.01f));
    }
  }
  else
  {
    lwmax = tmp_lwmax;
  }

  // PREVIEW pixelpipe stores lwmax
  if(self->dev->gui_attached && g && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW))
  {
    uint64_t hash = dt_dev_hash_plus(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL);
    dt_iop_gui_enter_critical_section(self);
    g->lwmax = lwmax;
    g->hash = hash;
    dt_iop_gui_leave_critical_section(self);
  }

  const float ldc = data->drago.max_light * 0.01 / log10f(lwmax + 1);
  const float bl = logf(fmaxf(eps, data->drago.bias)) / logf(0.5);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, bl, ldc, roi_out, eps) \
  shared(in, out, lwmax) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *inp = in + ch * k;
    float *outp = out + ch * k;
    float lw = inp[0] * 0.01f;
    outp[0] = 100.0f
              * (ldc * logf(fmaxf(eps, lw + 1.0f)) / logf(fmaxf(eps, 2.0f + (powf(lw / lwmax, bl)) * 8.0f)));
    outp[1] = inp[1];
    outp[2] = inp[2];
  }
}

static inline void process_filmic(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                  const void *const ivoid, void *const ovoid,
                                  const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                  dt_iop_global_tonemap_data_t *data)
{
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, roi_out) \
  shared(in, out, data) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *inp = in + ch * k;
    float *outp = out + ch * k;
    float l = inp[0] / 100.0;
    float x = fmaxf(0.0f, l - 0.004f);
    outp[0] = 100.0 * ((x * (6.2 * x + .5)) / (x * (6.2 * x + 1.7) + 0.06));
    outp[1] = inp[1];
    outp[2] = inp[2];
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_global_tonemap_data_t *data = (dt_iop_global_tonemap_data_t *)piece->data;
  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float sigma_r = 8.0f; // does not depend on scale
  const float iw = piece->buf_in.width / scale;
  const float ih = piece->buf_in.height / scale;
  const float sigma_s = fminf(iw, ih) * 0.03f;
  dt_bilateral_t *b = NULL;
  if(data->detail != 0.0f)
  {
    b = dt_bilateral_init(roi_in->width, roi_in->height, sigma_s, sigma_r);
    // get detail from unchanged input buffer
    dt_bilateral_splat(b, (float *)ivoid);
  }

  switch(data->operator)
  {
    case OPERATOR_REINHARD:
      process_reinhard(self, piece, ivoid, ovoid, roi_in, roi_out, data);
      break;
    case OPERATOR_DRAGO:
      process_drago(self, piece, ivoid, ovoid, roi_in, roi_out, data);
      break;
    case OPERATOR_FILMIC:
      process_filmic(self, piece, ivoid, ovoid, roi_in, roi_out, data);
      break;
  }

  if(data->detail != 0.0f)
  {
    dt_bilateral_blur(b);
    // and apply it to output buffer after logscale
    dt_bilateral_slice_to_output(b, (float *)ivoid, (float *)ovoid, data->detail);
    dt_bilateral_free(b);
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_global_tonemap_data_t *d = (dt_iop_global_tonemap_data_t *)piece->data;
  dt_iop_global_tonemap_global_data_t *gd = (dt_iop_global_tonemap_global_data_t *)self->global_data;
  dt_iop_global_tonemap_gui_data_t *g = (dt_iop_global_tonemap_gui_data_t *)self->gui_data;
  dt_bilateral_cl_t *b = NULL;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem dev_m = NULL;
  cl_mem dev_r = NULL;
  float *maximum = NULL;
  const int devid = piece->pipe->devid;
  int gtkernel = -1;

  const int width = roi_out->width;
  const int height = roi_out->height;
  float parameters[4] = { 0.0f };

  switch(d->operator)
  {
    case OPERATOR_REINHARD:
      gtkernel = gd->kernel_global_tonemap_reinhard;
      break;
    case OPERATOR_DRAGO:
      gtkernel = gd->kernel_global_tonemap_drago;
      break;
    case OPERATOR_FILMIC:
      gtkernel = gd->kernel_global_tonemap_filmic;
      break;
  }

  if(d->operator== OPERATOR_DRAGO)
  {
    const float eps = 0.0001f;
    float tmp_lwmax = NAN;

    // see comments in process() about lwmax value
    if(self->dev->gui_attached && g && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
    {
      dt_iop_gui_enter_critical_section(self);
      const uint64_t hash = g->hash;
      dt_iop_gui_leave_critical_section(self);
      if(hash != 0 && !dt_dev_sync_pixelpipe_hash(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, &self->gui_lock, &g->hash))
        dt_control_log(_("inconsistent output"));

      dt_iop_gui_enter_critical_section(self);
      tmp_lwmax = g->lwmax;
      dt_iop_gui_leave_critical_section(self);
    }

    if(isnan(tmp_lwmax))
    {
      dt_opencl_local_buffer_t flocopt
        = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                      .cellsize = sizeof(float), .overhead = 0,
                                      .sizex = 1 << 4, .sizey = 1 << 4 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_pixelmax_first, &flocopt))
        goto error;

      const size_t bwidth = ROUNDUP(width, flocopt.sizex);
      const size_t bheight = ROUNDUP(height, flocopt.sizey);

      const int bufsize = (bwidth / flocopt.sizex) * (bheight / flocopt.sizey);

      dt_opencl_local_buffer_t slocopt
        = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                      .cellsize = sizeof(float), .overhead = 0,
                                      .sizex = 1 << 16, .sizey = 1 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_pixelmax_second, &slocopt))
        goto error;

      const int reducesize = MIN(REDUCESIZE, ROUNDUP(bufsize, slocopt.sizex) / slocopt.sizex);

      size_t sizes[3];
      size_t local[3];

      dev_m = dt_opencl_alloc_device_buffer(devid, sizeof(float) * bufsize);
      if(dev_m == NULL) goto error;

      dev_r = dt_opencl_alloc_device_buffer(devid, sizeof(float) * reducesize);
      if(dev_r == NULL) goto error;

      sizes[0] = bwidth;
      sizes[1] = bheight;
      sizes[2] = 1;
      local[0] = flocopt.sizex;
      local[1] = flocopt.sizey;
      local[2] = 1;
      dt_opencl_set_kernel_args(devid, gd->kernel_pixelmax_first, 0, CLARG(dev_in), CLARG(width), CLARG(height),
        CLARG(dev_m), CLLOCAL(sizeof(float) * flocopt.sizex * flocopt.sizey));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_pixelmax_first, sizes, local);
      if(err != CL_SUCCESS) goto error;

      sizes[0] = (size_t)reducesize * slocopt.sizex;
      sizes[1] = 1;
      sizes[2] = 1;
      local[0] = slocopt.sizex;
      local[1] = 1;
      local[2] = 1;
      dt_opencl_set_kernel_args(devid, gd->kernel_pixelmax_second, 0, CLARG(dev_m), CLARG(dev_r), CLARG(bufsize),
        CLLOCAL(sizeof(float) * slocopt.sizex));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_pixelmax_second, sizes, local);
      if(err != CL_SUCCESS) goto error;

      maximum = dt_alloc_align_float((size_t)reducesize);
      err = dt_opencl_read_buffer_from_device(devid, (void *)maximum, dev_r, 0,
                                            sizeof(float) * reducesize, CL_TRUE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_release_mem_object(dev_r);
      dt_opencl_release_mem_object(dev_m);
      dev_r = dev_m = NULL;

      for(int k = 1; k < reducesize; k++)
      {
        float mine = maximum[0];
        float other = maximum[k];
        maximum[0] = (other > mine) ? other : mine;
      }

      tmp_lwmax = MAX(eps, (maximum[0] * 0.01f));

      dt_free_align(maximum);
      maximum = NULL;
    }

    const float lwmax = tmp_lwmax;
    const float ldc = d->drago.max_light * 0.01f / log10f(lwmax + 1.0f);
    const float bl = logf(MAX(eps, d->drago.bias)) / logf(0.5f);

    parameters[0] = eps;
    parameters[1] = ldc;
    parameters[2] = bl;
    parameters[3] = lwmax;

    if(self->dev->gui_attached && g && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW))
    {
      uint64_t hash = dt_dev_hash_plus(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL);
      dt_iop_gui_enter_critical_section(self);
      g->lwmax = lwmax;
      g->hash = hash;
      dt_iop_gui_leave_critical_section(self);
    }
  }

  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = 8.0f; // does not depend on scale
  const float iw = piece->buf_in.width / scale;
  const float ih = piece->buf_in.height / scale;
  const float sigma_s = fminf(iw, ih) * 0.03f;

  if(d->detail != 0.0f)
  {
    b = dt_bilateral_init_cl(devid, roi_in->width, roi_in->height, sigma_s, sigma_r);
    if(!b) goto error;
    // get detail from unchanged input buffer
    err = dt_bilateral_splat_cl(b, dev_in);
    if(err != CL_SUCCESS) goto error;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gtkernel, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(parameters));
  if(err != CL_SUCCESS) goto error;

  if(d->detail != 0.0f)
  {
    err = dt_bilateral_blur_cl(b);
    if(err != CL_SUCCESS) goto error;
    // and apply it to output buffer after logscale
    err = dt_bilateral_slice_to_output_cl(b, dev_in, dev_out, d->detail);
    if(err != CL_SUCCESS) goto error;
    dt_bilateral_free_cl(b);
  }

  return TRUE;

error:
  if(b) dt_bilateral_free_cl(b);
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_free_align(maximum);
  dt_print(DT_DEBUG_OPENCL, "[opencl_global_tonemap] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif


void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_global_tonemap_data_t *d = (dt_iop_global_tonemap_data_t *)piece->data;

  const float scale = piece->iscale / roi_in->scale;
  const float iw = piece->buf_in.width / scale;
  const float ih = piece->buf_in.height / scale;
  const float sigma_s = fminf(iw, ih) * 0.03f;
  const float sigma_r = 8.0f;
  const int detail = (d->detail != 0.0f);

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const size_t basebuffer = sizeof(float) * channels * width * height;

  tiling->factor = 2.0f + (detail ? (float)dt_bilateral_memory_use2(width, height, sigma_s, sigma_r) / basebuffer : 0.0f);
  tiling->maxbuf
      = (detail ? MAX(1.0f, (float)dt_bilateral_singlebuffer_size2(width, height, sigma_s, sigma_r) / basebuffer) : 1.0f);
  tiling->overhead = 0;
  tiling->overlap = (detail ? ceilf(4 * sigma_s) : 0);
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)p1;
  dt_iop_global_tonemap_data_t *d = (dt_iop_global_tonemap_data_t *)piece->data;

  d->operator= p->operator;
  d->drago.bias = p->drago.bias;
  d->drago.max_light = p->drago.max_light;
  d->detail = p->detail;

  // drago needs the maximum L-value of the whole image so it must not use tiling
  if(d->operator == OPERATOR_DRAGO) piece->process_tiling_ready = 0;

#ifdef HAVE_OPENCL
  if(d->detail != 0.0f)
    piece->process_cl_ready = (piece->process_cl_ready && !dt_opencl_micro_nap(pipe->devid));
#endif
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_global_tonemap_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl from programs.conf
  dt_iop_global_tonemap_global_data_t *gd
      = (dt_iop_global_tonemap_global_data_t *)malloc(sizeof(dt_iop_global_tonemap_global_data_t));
  module->data = gd;
  gd->kernel_pixelmax_first = dt_opencl_create_kernel(program, "pixelmax_first");
  gd->kernel_pixelmax_second = dt_opencl_create_kernel(program, "pixelmax_second");
  gd->kernel_global_tonemap_reinhard = dt_opencl_create_kernel(program, "global_tonemap_reinhard");
  gd->kernel_global_tonemap_drago = dt_opencl_create_kernel(program, "global_tonemap_drago");
  gd->kernel_global_tonemap_filmic = dt_opencl_create_kernel(program, "global_tonemap_filmic");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_global_tonemap_global_data_t *gd = (dt_iop_global_tonemap_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_pixelmax_first);
  dt_opencl_free_kernel(gd->kernel_pixelmax_second);
  dt_opencl_free_kernel(gd->kernel_global_tonemap_reinhard);
  dt_opencl_free_kernel(gd->kernel_global_tonemap_drago);
  dt_opencl_free_kernel(gd->kernel_global_tonemap_filmic);
  free(module->data);
  module->data = NULL;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_global_tonemap_gui_data_t *g = (dt_iop_global_tonemap_gui_data_t *)self->gui_data;
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)self->params;

  if(!w || w == g->operator)
  {
    gtk_widget_set_visible(g->drago.bias, p->operator == OPERATOR_DRAGO);
    gtk_widget_set_visible(g->drago.max_light, p->operator == OPERATOR_DRAGO);
  }
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_global_tonemap_gui_data_t *g = (dt_iop_global_tonemap_gui_data_t *)self->gui_data;

  gui_changed(self, NULL, 0);

  dt_iop_gui_enter_critical_section(self);
  g->lwmax = NAN;
  g->hash = 0;
  dt_iop_gui_leave_critical_section(self);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_global_tonemap_gui_data_t *g = IOP_GUI_ALLOC(global_tonemap);

  g->lwmax = NAN;
  g->hash = 0;

  g->operator = dt_bauhaus_combobox_from_params(self, N_("operator"));
  gtk_widget_set_tooltip_text(g->operator, _("the global tonemap operator"));

  g->drago.bias = dt_bauhaus_slider_from_params(self, "drago.bias");
  gtk_widget_set_tooltip_text(g->drago.bias, _("the bias for tonemapper controls the linearity, "
                                               "the higher the more details in blacks"));

  g->drago.max_light = dt_bauhaus_slider_from_params(self, "drago.max_light");
  gtk_widget_set_tooltip_text(g->drago.max_light, _("the target light for tonemapper specified as cd/m2"));

  g->detail = dt_bauhaus_slider_from_params(self, N_("detail"));
  dt_bauhaus_slider_set_digits(g->detail, 3);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

