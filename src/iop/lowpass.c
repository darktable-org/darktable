/*
  This file is part of darktable,
  copyright (c) 2011--2013 ulrich pegelow.

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
#include "common/debug.h"
#include "common/gaussian.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <inttypes.h>

#define CLAMPF(a, mn, mx) ((a) < (mn) ? (mn) : ((a) > (mx) ? (mx) : (a)))

#define BLOCKSIZE 64 /* maximum blocksize. must be a power of 2 and will be automatically reduced if needed  \
                        */

DT_MODULE_INTROSPECTION(4, dt_iop_lowpass_params_t)

typedef enum dt_iop_lowpass_algo_t
{
  LOWPASS_ALGO_GAUSSIAN,
  LOWPASS_ALGO_BILATERAL
} dt_iop_lowpass_algo_t;

/* legacy version 1 params */
typedef struct dt_iop_lowpass_params1_t
{
  dt_gaussian_order_t order;
  float radius;
  float contrast;
  float saturation;
} dt_iop_lowpass_params1_t;

typedef struct dt_iop_lowpass_params2_t
{
  dt_gaussian_order_t order;
  float radius;
  float contrast;
  float brightness;
  float saturation;
} dt_iop_lowpass_params2_t;

typedef struct dt_iop_lowpass_params3_t
{
  dt_gaussian_order_t order;
  float radius;
  float contrast;
  float brightness;
  float saturation;
  int unbound;
} dt_iop_lowpass_params3_t;

typedef struct dt_iop_lowpass_params_t
{
  dt_gaussian_order_t order;
  float radius;
  float contrast;
  float brightness;
  float saturation;
  dt_iop_lowpass_algo_t lowpass_algo;
  int unbound;
} dt_iop_lowpass_params_t;


typedef struct dt_iop_lowpass_gui_data_t
{
  GtkWidget *radius;
  GtkWidget *contrast;
  GtkWidget *brightness;
  GtkWidget *saturation;
  GtkWidget *order;
  GtkWidget *lowpass_algo;
} dt_iop_lowpass_gui_data_t;

typedef struct dt_iop_lowpass_data_t
{
  dt_gaussian_order_t order;
  float radius;
  float contrast;
  float brightness;
  float saturation;
  dt_iop_lowpass_algo_t lowpass_algo;
  int unbound;
  float ctable[0x10000];      // precomputed look-up table for contrast curve
  float cunbounded_coeffs[3]; // approximation for extrapolation of contrast curve
  float ltable[0x10000];      // precomputed look-up table for brightness curve
  float lunbounded_coeffs[3]; // approximation for extrapolation of brightness curve
} dt_iop_lowpass_data_t;

typedef struct dt_iop_lowpass_global_data_t
{
  int kernel_lowpass_mix;
} dt_iop_lowpass_global_data_t;


const char *name()
{
  return _("lowpass");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int groups()
{
  return IOP_GROUP_EFFECT;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 4)
  {
    const dt_iop_lowpass_params1_t *old = old_params;
    dt_iop_lowpass_params_t *new = new_params;
    new->order = old->order;
    new->radius = fabs(old->radius);
    new->contrast = old->contrast;
    new->saturation = old->saturation;
    new->brightness = 0.0f;
    new->lowpass_algo = old->radius < 0.0f ? LOWPASS_ALGO_BILATERAL : LOWPASS_ALGO_GAUSSIAN;
    new->unbound = 0;

    return 0;
  }
  if(old_version == 2 && new_version == 4)
  {
    const dt_iop_lowpass_params2_t *old = old_params;
    dt_iop_lowpass_params_t *new = new_params;
    new->order = old->order;
    new->radius = fabs(old->radius);
    new->contrast = old->contrast;
    new->saturation = old->saturation;
    new->brightness = old->brightness;
    new->lowpass_algo = old->radius < 0.0f ? LOWPASS_ALGO_BILATERAL : LOWPASS_ALGO_GAUSSIAN;
    new->unbound = 0;

    return 0;
  }
  if(old_version == 3 && new_version == 4)
  {
    const dt_iop_lowpass_params3_t *old = old_params;
    dt_iop_lowpass_params_t *new = new_params;
    new->order = old->order;
    new->radius = fabs(old->radius);
    new->contrast = old->contrast;
    new->saturation = old->saturation;
    new->brightness = old->brightness;
    new->lowpass_algo = old->radius < 0.0f ? LOWPASS_ALGO_BILATERAL : LOWPASS_ALGO_GAUSSIAN;
    new->unbound = old->unbound;

    return 0;
  }
  return 1;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "radius"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "contrast"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "brightness"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "saturation"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_lowpass_gui_data_t *g = (dt_iop_lowpass_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "radius", GTK_WIDGET(g->radius));
  dt_accel_connect_slider_iop(self, "contrast", GTK_WIDGET(g->contrast));
  dt_accel_connect_slider_iop(self, "brightness", GTK_WIDGET(g->brightness));
  dt_accel_connect_slider_iop(self, "saturation", GTK_WIDGET(g->saturation));
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_lowpass_data_t *d = (dt_iop_lowpass_data_t *)piece->data;
  dt_iop_lowpass_global_data_t *gd = (dt_iop_lowpass_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const float radius = fmax(0.1f, d->radius);
  const float sigma = radius * roi_in->scale / piece->iscale;
  const float saturation = d->saturation;
  const int order = d->order;
  const int unbound = d->unbound;

  size_t sizes[3];

  cl_mem dev_cm = NULL;
  cl_mem dev_ccoeffs = NULL;
  cl_mem dev_lm = NULL;
  cl_mem dev_lcoeffs = NULL;
  cl_mem dev_tmp = NULL;

  dt_gaussian_cl_t *g = NULL;
  dt_bilateral_cl_t *b = NULL;

  float Labmax[] = { 100.0f, 128.0f, 128.0f, 1.0f };
  float Labmin[] = { 0.0f, -128.0f, -128.0f, 0.0f };

  if(unbound)
  {
    for(int k = 0; k < 4; k++) Labmax[k] = INFINITY;
    for(int k = 0; k < 4; k++) Labmin[k] = -INFINITY;
  }

  if(d->lowpass_algo == LOWPASS_ALGO_GAUSSIAN)
  {
    g = dt_gaussian_init_cl(devid, width, height, channels, Labmax, Labmin, sigma, order);
    if(!g) goto error;
    err = dt_gaussian_blur_cl(g, dev_in, dev_out);
    if(err != CL_SUCCESS) goto error;
    dt_gaussian_free_cl(g);
    g = NULL;
  }
  else
  {
    const float sigma_r = 100.0f; // does not depend on scale
    const float sigma_s = sigma;
    const float detail = -1.0f; // we want the bilateral base layer

    b = dt_bilateral_init_cl(devid, width, height, sigma_s, sigma_r);
    if(!b) goto error;
    err = dt_bilateral_splat_cl(b, dev_in);
    if(err != CL_SUCCESS) goto error;
    err = dt_bilateral_blur_cl(b);
    if(err != CL_SUCCESS) goto error;
    err = dt_bilateral_slice_cl(b, dev_in, dev_out, detail);
    if(err != CL_SUCCESS) goto error;
    dt_bilateral_free_cl(b);
    b = NULL; // make sure we don't clean it up twice
  }

  dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
  if(dev_tmp == NULL) goto error;

  dev_cm = dt_opencl_copy_host_to_device(devid, d->ctable, 256, 256, sizeof(float));
  if(dev_cm == NULL) goto error;

  dev_ccoeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->cunbounded_coeffs);
  if(dev_ccoeffs == NULL) goto error;

  dev_lm = dt_opencl_copy_host_to_device(devid, d->ltable, 256, 256, sizeof(float));
  if(dev_lm == NULL) goto error;

  dev_lcoeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->lunbounded_coeffs);
  if(dev_lcoeffs == NULL) goto error;

  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };
  err = dt_opencl_enqueue_copy_image(devid, dev_out, dev_tmp, origin, origin, region);
  if(err != CL_SUCCESS) goto error;

  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPWD(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_lowpass_mix, 0, sizeof(cl_mem), (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_lowpass_mix, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_lowpass_mix, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_lowpass_mix, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_lowpass_mix, 4, sizeof(float), (void *)&saturation);
  dt_opencl_set_kernel_arg(devid, gd->kernel_lowpass_mix, 5, sizeof(cl_mem), (void *)&dev_cm);
  dt_opencl_set_kernel_arg(devid, gd->kernel_lowpass_mix, 6, sizeof(cl_mem), (void *)&dev_ccoeffs);
  dt_opencl_set_kernel_arg(devid, gd->kernel_lowpass_mix, 7, sizeof(cl_mem), (void *)&dev_lm);
  dt_opencl_set_kernel_arg(devid, gd->kernel_lowpass_mix, 8, sizeof(cl_mem), (void *)&dev_lcoeffs);
  dt_opencl_set_kernel_arg(devid, gd->kernel_lowpass_mix, 9, sizeof(int), (void *)&unbound);

  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lowpass_mix, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_lcoeffs);
  dt_opencl_release_mem_object(dev_lm);
  dt_opencl_release_mem_object(dev_ccoeffs);
  dt_opencl_release_mem_object(dev_cm);

  return TRUE;

error:
  if(g) dt_gaussian_free_cl(g);
  if(b) dt_bilateral_free_cl(b);

  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  if(dev_lcoeffs != NULL) dt_opencl_release_mem_object(dev_lcoeffs);
  if(dev_lm != NULL) dt_opencl_release_mem_object(dev_lm);
  if(dev_ccoeffs != NULL) dt_opencl_release_mem_object(dev_ccoeffs);
  if(dev_cm != NULL) dt_opencl_release_mem_object(dev_cm);
  dt_print(DT_DEBUG_OPENCL, "[opencl_lowpass] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_lowpass_data_t *d = (dt_iop_lowpass_data_t *)piece->data;

  const float radius = fmax(0.1f, d->radius);
  const float sigma = radius * roi_in->scale / piece->iscale;
  const float sigma_r = 100.0f; // does not depend on scale
  const float sigma_s = sigma;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const size_t basebuffer = width * height * channels * sizeof(float);

  if(d->lowpass_algo == LOWPASS_ALGO_BILATERAL)
  {
    // bilateral filter
    tiling->factor = 2.0f + fmax(1.0f, (float)dt_bilateral_memory_use(width, height, sigma_s, sigma_r) / basebuffer);
    tiling->maxbuf
        = fmax(1.0f, (float)dt_bilateral_singlebuffer_size(width, height, sigma_s, sigma_r) / basebuffer);
  }
  else
  {
    // gaussian blur
    tiling->factor = 2.0f + fmax(1.0f, (float)dt_gaussian_memory_use(width, height, channels) / basebuffer);
    tiling->maxbuf = fmax(1.0f, (float)dt_gaussian_singlebuffer_size(width, height, channels) / basebuffer);
  }
  tiling->overhead = 0;
  tiling->overlap = ceilf(4 * sigma);
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_lowpass_data_t *data = (dt_iop_lowpass_data_t *)piece->data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;


  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = piece->colors;

  const float radius = fmax(0.1f, data->radius);
  const float sigma = radius * roi_in->scale / piece->iscale;
  const int order = data->order;
  const int unbound = data->unbound;

  float Labmax[] = { 100.0f, 128.0f, 128.0f, 1.0f };
  float Labmin[] = { 0.0f, -128.0f, -128.0f, 0.0f };

  if(unbound)
  {
    for(int k = 0; k < 4; k++) Labmax[k] = INFINITY;
    for(int k = 0; k < 4; k++) Labmin[k] = -INFINITY;
  }

  if(data->lowpass_algo == LOWPASS_ALGO_GAUSSIAN)
  {
    dt_gaussian_t *g = dt_gaussian_init(width, height, ch, Labmax, Labmin, sigma, order);
    if(!g) return;
    dt_gaussian_blur_4c(g, in, out);
    dt_gaussian_free(g);
  }
  else
  {
    const float sigma_r = 100.0f; // d->sigma_r; // does not depend on scale
    const float sigma_s = sigma;
    const float detail = -1.0f; // we want the bilateral base layer

    dt_bilateral_t *b = dt_bilateral_init(width, height, sigma_s, sigma_r);
    if(!b) return;
    dt_bilateral_splat(b, in);
    dt_bilateral_blur(b);
    dt_bilateral_slice(b, in, out, detail);
    dt_bilateral_free(b);
  }

  // some aliased pointers for compilers that don't yet understand operators on __m128
  const float *const Labminf = (float *)&Labmin;
  const float *const Labmaxf = (float *)&Labmax;
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(in, out, data) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    out[k * ch + 0] = (out[k * ch + 0] < 100.0f)
                          ? data->ctable[CLAMP((int)(out[k * ch + 0] / 100.0f * 0x10000ul), 0, 0xffff)]
                          : dt_iop_eval_exp(data->cunbounded_coeffs, out[k * ch + 0] / 100.0f);
    out[k * ch + 0] = (out[k * ch + 0] < 100.0f)
                          ? data->ltable[CLAMP((int)(out[k * ch + 0] / 100.0f * 0x10000ul), 0, 0xffff)]
                          : dt_iop_eval_exp(data->lunbounded_coeffs, out[k * ch + 0] / 100.0f);
    out[k * ch + 1] = CLAMPF(out[k * ch + 1] * data->saturation, Labminf[1],
                             Labmaxf[1]); // will not clip in unbound case (see definition of Labmax/Labmin)
    out[k * ch + 2]
        = CLAMPF(out[k * ch + 2] * data->saturation, Labminf[2], Labmaxf[2]); //                         - " -
    out[k * ch + 3] = in[k * ch + 3];
  }
}


static void radius_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_lowpass_params_t *p = (dt_iop_lowpass_params_t *)self->params;
  p->radius = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lowpass_algo_callback(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_lowpass_params_t *p = (dt_iop_lowpass_params_t *)self->params;
  p->lowpass_algo = dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void contrast_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_lowpass_params_t *p = (dt_iop_lowpass_params_t *)self->params;
  p->contrast = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void brightness_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_lowpass_params_t *p = (dt_iop_lowpass_params_t *)self->params;
  p->brightness = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_lowpass_params_t *p = (dt_iop_lowpass_params_t *)self->params;
  p->saturation = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

#if 0 // gaussian order not user selectable
static void
order_changed (GtkComboBox *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_lowpass_params_t *p = (dt_iop_lowpass_params_t *)self->params;
  p->order = gtk_combo_box_get_active(combo);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lowpass_params_t *p = (dt_iop_lowpass_params_t *)p1;
  dt_iop_lowpass_data_t *d = (dt_iop_lowpass_data_t *)piece->data;
  d->order = p->order;
  d->radius = p->radius;
  d->contrast = p->contrast;
  d->brightness = p->brightness;
  d->saturation = p->saturation;
  d->lowpass_algo = p->lowpass_algo;
  d->unbound = p->unbound;

#ifdef HAVE_OPENCL
  if(d->lowpass_algo == LOWPASS_ALGO_BILATERAL)
    piece->process_cl_ready = (piece->process_cl_ready && !(darktable.opencl->avoid_atomics));
#endif


  // generate precomputed contrast curve
  if(fabs(d->contrast) <= 1.0f)
  {
    // linear curve for contrast up to +/- 1
    for(int k = 0; k < 0x10000; k++) d->ctable[k] = d->contrast * (100.0f * k / 0x10000 - 50.0f) + 50.0f;
  }
  else
  {
    // sigmoidal curve for contrast above +/-1 1
    // going from (0,0) to (1,100) or (0,100) to (1,0), respectively
    const float boost = 5.0f;
    const float contrastm1sq = boost * (fabs(d->contrast) - 1.0f) * (fabs(d->contrast) - 1.0f);
    const float contrastscale = copysign(sqrt(1.0f + contrastm1sq), d->contrast);
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d) schedule(static)
#endif
    for(int k = 0; k < 0x10000; k++)
    {
      float kx2m1 = 2.0f * (float)k / 0x10000 - 1.0f;
      d->ctable[k] = 50.0f * (contrastscale * kx2m1 / sqrtf(1.0f + contrastm1sq * kx2m1 * kx2m1) + 1.0f);
    }
  }

  // now the extrapolation stuff for the contrast curve:
  const float xc[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
  const float yc[4] = { d->ctable[CLAMP((int)(xc[0] * 0x10000ul), 0, 0xffff)],
                        d->ctable[CLAMP((int)(xc[1] * 0x10000ul), 0, 0xffff)],
                        d->ctable[CLAMP((int)(xc[2] * 0x10000ul), 0, 0xffff)],
                        d->ctable[CLAMP((int)(xc[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(xc, yc, 4, d->cunbounded_coeffs);


  // generate precomputed brightness curve
  const float gamma = (d->brightness >= 0.0f) ? 1.0f / (1.0f + d->brightness) : (1.0f - d->brightness);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d) schedule(static)
#endif
  for(int k = 0; k < 0x10000; k++)
  {
    d->ltable[k] = 100.0f * powf((float)k / 0x10000, gamma);
  }

  // now the extrapolation stuff for the brightness curve:
  const float xl[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
  const float yl[4] = { d->ltable[CLAMP((int)(xl[0] * 0x10000ul), 0, 0xffff)],
                        d->ltable[CLAMP((int)(xl[1] * 0x10000ul), 0, 0xffff)],
                        d->ltable[CLAMP((int)(xl[2] * 0x10000ul), 0, 0xffff)],
                        d->ltable[CLAMP((int)(xl[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(xl, yl, 4, d->lunbounded_coeffs);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lowpass_data_t *d = (dt_iop_lowpass_data_t *)calloc(1, sizeof(dt_iop_lowpass_data_t));
  piece->data = (void *)d;
  self->commit_params(self, self->default_params, pipe, piece);
  for(int k = 0; k < 0x10000; k++) d->ctable[k] = d->ltable[k] = 100.0f * k / 0x10000; // identity
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_lowpass_gui_data_t *g = (dt_iop_lowpass_gui_data_t *)self->gui_data;
  dt_iop_lowpass_params_t *p = (dt_iop_lowpass_params_t *)module->params;
  dt_bauhaus_slider_set(g->radius, p->radius);
  dt_bauhaus_combobox_set(g->lowpass_algo, p->lowpass_algo);
  dt_bauhaus_slider_set(g->contrast, p->contrast);
  dt_bauhaus_slider_set(g->brightness, p->brightness);
  dt_bauhaus_slider_set(g->saturation, p->saturation);
  // gtk_combo_box_set_active(g->order, p->order);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_lowpass_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_lowpass_params_t));
  module->default_enabled = 0;
  module->priority = 753; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_lowpass_params_t);
  module->gui_data = NULL;
  dt_iop_lowpass_params_t tmp = (dt_iop_lowpass_params_t){ 0, 10.0f, 1.0f, 0.0f, 1.0f, LOWPASS_ALGO_GAUSSIAN, 1 };
  memcpy(module->params, &tmp, sizeof(dt_iop_lowpass_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_lowpass_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 6; // gaussian.cl, from programs.conf
  dt_iop_lowpass_global_data_t *gd
      = (dt_iop_lowpass_global_data_t *)malloc(sizeof(dt_iop_lowpass_global_data_t));
  module->data = gd;
  gd->kernel_lowpass_mix = dt_opencl_create_kernel(program, "lowpass_mix");
}

void init_presets(dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "begin", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("local contrast mask"), self->op, self->version(),
                             &(dt_iop_lowpass_params_t){ 0, 50.0f, -1.0f, 0.0f, 0.0f, LOWPASS_ALGO_GAUSSIAN, 1 },
                             sizeof(dt_iop_lowpass_params_t), 1);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "commit", NULL, NULL, NULL);
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_lowpass_global_data_t *gd = (dt_iop_lowpass_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_lowpass_mix);
  free(module->data);
  module->data = NULL;
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_lowpass_gui_data_t));
  dt_iop_lowpass_gui_data_t *g = (dt_iop_lowpass_gui_data_t *)self->gui_data;
  dt_iop_lowpass_params_t *p = (dt_iop_lowpass_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

#if 0 // gaussian is order not user selectable here, as it does not make much sense for a lowpass filter
  GtkBox *hbox  = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), FALSE, FALSE, 0);
  GtkWidget *label = dtgtk_reset_label_new(_("filter order"), self, &p->order, sizeof(float));
  gtk_box_pack_start(hbox, label, FALSE, FALSE, 0);
  g->order = GTK_COMBO_BOX(gtk_combo_box_text_new());
  gtk_combo_box_text_append_text(g->order, _("0th order"));
  gtk_combo_box_text_append_text(g->order, _("1st order"));
  gtk_combo_box_text_append_text(g->order, _("2nd order"));
  gtk_widget_set_tooltip_text(g->order, _("filter order of gaussian blur"));
  gtk_box_pack_start(hbox, GTK_WIDGET(g->order), TRUE, TRUE, 0);
#endif

  g->radius = dt_bauhaus_slider_new_with_range(self, 0.1, 200.0, 0.1, p->radius, 2);
  g->contrast = dt_bauhaus_slider_new_with_range(self, -3.0, 3.0, 0.01, p->contrast, 2);
  g->brightness = dt_bauhaus_slider_new_with_range(self, -3.0, 3.0, 0.01, p->brightness, 2);
  g->saturation = dt_bauhaus_slider_new_with_range(self, -3.0, 3.0, 0.01, p->saturation, 2);

  dt_bauhaus_widget_set_label(g->radius, NULL, _("radius"));
  dt_bauhaus_widget_set_label(g->contrast, NULL, _("contrast"));
  dt_bauhaus_widget_set_label(g->brightness, NULL, C_("lowpass", "brightness"));
  dt_bauhaus_widget_set_label(g->saturation, NULL, _("saturation"));

  g->lowpass_algo = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->lowpass_algo, NULL, _("soften with"));
  dt_bauhaus_combobox_add(g->lowpass_algo, _("gaussian"));
  dt_bauhaus_combobox_add(g->lowpass_algo, _("bilateral filter"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->radius, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->lowpass_algo, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->contrast, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->brightness, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->saturation, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->radius, _("radius of gaussian/bilateral blur"));
  gtk_widget_set_tooltip_text(g->contrast, _("contrast of lowpass filter"));
  gtk_widget_set_tooltip_text(g->brightness, _("brightness adjustment of lowpass filter"));
  gtk_widget_set_tooltip_text(g->saturation, _("color saturation of lowpass filter"));
  gtk_widget_set_tooltip_text(g->lowpass_algo, _("which filter to use for blurring"));

  g_signal_connect(G_OBJECT(g->radius), "value-changed", G_CALLBACK(radius_callback), self);
  g_signal_connect(G_OBJECT(g->lowpass_algo), "value-changed", G_CALLBACK(lowpass_algo_callback), self);
  g_signal_connect(G_OBJECT(g->contrast), "value-changed", G_CALLBACK(contrast_callback), self);
  g_signal_connect(G_OBJECT(g->brightness), "value-changed", G_CALLBACK(brightness_callback), self);
  g_signal_connect(G_OBJECT(g->saturation), "value-changed", G_CALLBACK(saturation_callback), self);
#if 0 // gaussian order not user selectable
  g_signal_connect (G_OBJECT (g->order), "changed",
                    G_CALLBACK (order_changed), self);
#endif
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
