/*
  This file is part of darktable,
  copyright (c) 2012--2015 Ulrich Pegelow.

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
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <inttypes.h>

#define UNBOUND_L 1
#define UNBOUND_A 2
#define UNBOUND_B 4
#define UNBOUND_SHADOWS_L UNBOUND_L
#define UNBOUND_SHADOWS_A UNBOUND_A
#define UNBOUND_SHADOWS_B UNBOUND_B
#define UNBOUND_HIGHLIGHTS_L (UNBOUND_L << 3) /* 8 */
#define UNBOUND_HIGHLIGHTS_A (UNBOUND_A << 3) /* 16 */
#define UNBOUND_HIGHLIGHTS_B (UNBOUND_B << 3) /* 32 */
#define UNBOUND_GAUSSIAN 64
#define UNBOUND_BILATERAL 128 /* not implemented yet */
#define UNBOUND_DEFAULT                                                                                      \
  (UNBOUND_SHADOWS_L | UNBOUND_SHADOWS_A | UNBOUND_SHADOWS_B | UNBOUND_HIGHLIGHTS_L | UNBOUND_HIGHLIGHTS_A   \
   | UNBOUND_HIGHLIGHTS_B | UNBOUND_GAUSSIAN)

#define CLAMPF(a, mn, mx) ((a) < (mn) ? (mn) : ((a) > (mx) ? (mx) : (a)))
#define CLAMP_RANGE(x, y, z) (CLAMP(x, y, z))

#define BLOCKSIZE 64 /* maximum blocksize. must be a power of 2 and will be automatically reduced if needed  \
                        */

DT_MODULE_INTROSPECTION(5, dt_iop_shadhi_params_t)

typedef enum dt_iop_shadhi_algo_t
{
  SHADHI_ALGO_GAUSSIAN,
  SHADHI_ALGO_BILATERAL
} dt_iop_shadhi_algo_t;

/* legacy version 1 params */
typedef struct dt_iop_shadhi_params1_t
{
  dt_gaussian_order_t order;
  float radius;
  float shadows;
  float reserved1;
  float highlights;
  float reserved2;
  float compress;
} dt_iop_shadhi_params1_t;

/* legacy version 2 params */
typedef struct dt_iop_shadhi_params2_t
{
  dt_gaussian_order_t order;
  float radius;
  float shadows;
  float reserved1;
  float highlights;
  float reserved2;
  float compress;
  float shadows_ccorrect;
  float highlights_ccorrect;
} dt_iop_shadhi_params2_t;

typedef struct dt_iop_shadhi_params3_t
{
  dt_gaussian_order_t order;
  float radius;
  float shadows;
  float reserved1;
  float highlights;
  float reserved2;
  float compress;
  float shadows_ccorrect;
  float highlights_ccorrect;
  unsigned int flags;
} dt_iop_shadhi_params3_t;

typedef struct dt_iop_shadhi_params4_t
{
  dt_gaussian_order_t order;
  float radius;
  float shadows;
  float whitepoint;
  float highlights;
  float reserved2;
  float compress;
  float shadows_ccorrect;
  float highlights_ccorrect;
  unsigned int flags;
  float low_approximation;
} dt_iop_shadhi_params4_t;

typedef struct dt_iop_shadhi_params_t
{
  dt_gaussian_order_t order;
  float radius;
  float shadows;
  float whitepoint;
  float highlights;
  float reserved2;
  float compress;
  float shadows_ccorrect;
  float highlights_ccorrect;
  unsigned int flags;
  float low_approximation;
  dt_iop_shadhi_algo_t shadhi_algo;
} dt_iop_shadhi_params_t;

typedef struct dt_iop_shadhi_gui_data_t
{
  GtkWidget *shadows;
  GtkWidget *highlights;
  GtkWidget *whitepoint;
  GtkWidget *radius;
  GtkWidget *compress;
  GtkWidget *shadows_ccorrect;
  GtkWidget *highlights_ccorrect;
  GtkWidget *shadhi_algo;
} dt_iop_shadhi_gui_data_t;

typedef struct dt_iop_shadhi_data_t
{
  dt_gaussian_order_t order;
  float radius;
  float shadows;
  float highlights;
  float whitepoint;
  float compress;
  float shadows_ccorrect;
  float highlights_ccorrect;
  unsigned int flags;
  float low_approximation;
  dt_iop_shadhi_algo_t shadhi_algo;
} dt_iop_shadhi_data_t;

typedef struct dt_iop_shadhi_global_data_t
{
  int kernel_shadows_highlights_mix;
} dt_iop_shadhi_global_data_t;


const char *name()
{
  return _("shadows and highlights");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int groups()
{
  return IOP_GROUP_BASIC;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 5)
  {
    const dt_iop_shadhi_params1_t *old = old_params;
    dt_iop_shadhi_params_t *new = new_params;
    new->order = old->order;
    new->radius = fabs(old->radius);
    new->shadows = 0.5f * old->shadows;
    new->whitepoint = old->reserved1;
    new->reserved2 = old->reserved2;
    new->highlights = -0.5f * old->highlights;
    new->flags = 0;
    new->compress = old->compress;
    new->shadows_ccorrect = 100.0f;
    new->highlights_ccorrect = 0.0f;
    new->low_approximation = 0.01f;
    new->shadhi_algo = old->radius < 0.0f ? SHADHI_ALGO_BILATERAL : SHADHI_ALGO_GAUSSIAN;
    return 0;
  }
  else if(old_version == 2 && new_version == 5)
  {
    const dt_iop_shadhi_params2_t *old = old_params;
    dt_iop_shadhi_params_t *new = new_params;
    new->order = old->order;
    new->radius = fabs(old->radius);
    new->shadows = old->shadows;
    new->whitepoint = old->reserved1;
    new->reserved2 = old->reserved2;
    new->highlights = old->highlights;
    new->compress = old->compress;
    new->shadows_ccorrect = old->shadows_ccorrect;
    new->highlights_ccorrect = old->highlights_ccorrect;
    new->flags = 0;
    new->low_approximation = 0.01f;
    new->shadhi_algo = old->radius < 0.0f ? SHADHI_ALGO_BILATERAL : SHADHI_ALGO_GAUSSIAN;
    return 0;
  }
  else if(old_version == 3 && new_version == 5)
  {
    const dt_iop_shadhi_params3_t *old = old_params;
    dt_iop_shadhi_params_t *new = new_params;
    new->order = old->order;
    new->radius = fabs(old->radius);
    new->shadows = old->shadows;
    new->whitepoint = old->reserved1;
    new->reserved2 = old->reserved2;
    new->highlights = old->highlights;
    new->compress = old->compress;
    new->shadows_ccorrect = old->shadows_ccorrect;
    new->highlights_ccorrect = old->highlights_ccorrect;
    new->flags = old->flags;
    new->low_approximation = 0.01f;
    new->shadhi_algo = old->radius < 0.0f ? SHADHI_ALGO_BILATERAL : SHADHI_ALGO_GAUSSIAN;
    return 0;
  }
  else if(old_version == 4 && new_version == 5)
  {
    const dt_iop_shadhi_params4_t *old = old_params;
    dt_iop_shadhi_params_t *new = new_params;
    new->order = old->order;
    new->radius = fabs(old->radius);
    new->shadows = old->shadows;
    new->whitepoint = old->whitepoint;
    new->reserved2 = old->reserved2;
    new->highlights = old->highlights;
    new->compress = old->compress;
    new->shadows_ccorrect = old->shadows_ccorrect;
    new->highlights_ccorrect = old->highlights_ccorrect;
    new->flags = old->flags;
    new->low_approximation = old->low_approximation;
    new->shadhi_algo = old->radius < 0.0f ? SHADHI_ALGO_BILATERAL : SHADHI_ALGO_GAUSSIAN;
    return 0;
  }
  return 1;
}


void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "shadows"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "highlights"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "white point adjustment"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "radius"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "compress"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "shadows color correction"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "highlights color correction"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_shadhi_gui_data_t *g = (dt_iop_shadhi_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "shadows", GTK_WIDGET(g->shadows));
  dt_accel_connect_slider_iop(self, "highlights", GTK_WIDGET(g->highlights));
  dt_accel_connect_slider_iop(self, "white point adjustment", GTK_WIDGET(g->whitepoint));
  dt_accel_connect_slider_iop(self, "radius", GTK_WIDGET(g->radius));
  dt_accel_connect_slider_iop(self, "compress", GTK_WIDGET(g->compress));
  dt_accel_connect_slider_iop(self, "shadows color correction", GTK_WIDGET(g->shadows_ccorrect));
  dt_accel_connect_slider_iop(self, "highlights color correction", GTK_WIDGET(g->highlights_ccorrect));
}



static inline void _Lab_scale(const float *i, float *o)
{
  o[0] = i[0] / 100.0f;
  o[1] = i[1] / 128.0f;
  o[2] = i[2] / 128.0f;
}


static inline void _Lab_rescale(const float *i, float *o)
{
  o[0] = i[0] * 100.0f;
  o[1] = i[1] * 128.0f;
  o[2] = i[2] * 128.0f;
}

static inline float sign(float x)
{
  return (x < 0 ? -1.0f : 1.0f);
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_shadhi_data_t *data = (dt_iop_shadhi_data_t *)piece->data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int width = roi_out->width;
  const int height = roi_out->height;
  const int ch = piece->colors;

  const int order = data->order;
  const float radius = fmaxf(0.1f, data->radius);
  const float sigma = radius * roi_in->scale / piece->iscale;
  const float shadows = 2.0f * fmin(fmax(-1.0, (data->shadows / 100.0f)), 1.0f);
  const float highlights = 2.0f * fmin(fmax(-1.0, (data->highlights / 100.0f)), 1.0f);
  const float whitepoint = fmax(1.0f - data->whitepoint / 100.0f, 0.01f);
  const float compress
      = fmin(fmax(0, (data->compress / 100.0f)), 0.99f); // upper limit 0.99f to avoid division by zero later
  const float shadows_ccorrect = (fmin(fmax(0.0f, (data->shadows_ccorrect / 100.0f)), 1.0f) - 0.5f)
                                 * sign(shadows) + 0.5f;
  const float highlights_ccorrect = (fmin(fmax(0.0f, (data->highlights_ccorrect / 100.0f)), 1.0f) - 0.5f)
                                    * sign(-highlights) + 0.5f;
  const unsigned int flags = data->flags;
  const int unbound_mask = ((data->shadhi_algo == SHADHI_ALGO_BILATERAL) && (flags & UNBOUND_BILATERAL))
                           || ((data->shadhi_algo == SHADHI_ALGO_GAUSSIAN) && (flags & UNBOUND_GAUSSIAN));
  const float low_approximation = data->low_approximation;

  if(data->shadhi_algo == SHADHI_ALGO_GAUSSIAN)
  {
    float Labmax[4] = { 100.0f, 128.0f, 128.0f, 1.0f };
    float Labmin[4] = { 0.0f, -128.0f, -128.0f, 0.0f };

    if(unbound_mask)
    {
      for(int k = 0; k < 4; k++) Labmax[k] = INFINITY;
      for(int k = 0; k < 4; k++) Labmin[k] = -INFINITY;
    }

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

// invert and desaturate
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out) schedule(static)
#endif
  for(size_t j = 0; j < (size_t)roi_out->width * roi_out->height * 4; j += 4)
  {
    out[j + 0] = 100.0f - out[j + 0];
    out[j + 1] = 0.0f;
    out[j + 2] = 0.0f;
  }

  const float max[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
  const float min[4] = { 0.0f, -1.0f, -1.0f, 0.0f };
  const float lmin = 0.0f;
  const float lmax = max[0] + fabs(min[0]);
  const float halfmax = lmax / 2.0;
  const float doublemax = lmax * 2.0;


#ifdef _OPENMP
#pragma omp parallel for default(none) shared(in, out) schedule(static)
#endif
  for(size_t j = 0; j < (size_t)width * height * ch; j += ch)
  {
    float ta[3], tb[3];
    _Lab_scale(&in[j], ta);
    _Lab_scale(&out[j], tb);

    ta[0] = ta[0] > 0.0f ? ta[0] / whitepoint : ta[0];
    tb[0] = tb[0] > 0.0f ? tb[0] / whitepoint : tb[0];

    // overlay highlights
    float highlights2 = highlights * highlights;
    float highlights_xform = CLAMP_RANGE(1.0f - tb[0] / (1.0f - compress), 0.0f, 1.0f);

    while(highlights2 > 0.0f)
    {
      float la = (flags & UNBOUND_HIGHLIGHTS_L) ? ta[0] : CLAMP_RANGE(ta[0], lmin, lmax);
      float lb = (tb[0] - halfmax) * sign(-highlights) * sign(lmax - la) + halfmax;
      lb = unbound_mask ? lb : CLAMP_RANGE(lb, lmin, lmax);
      float lref = copysignf(fabs(la) > low_approximation ? 1.0f / fabs(la) : 1.0f / low_approximation, la);
      float href = copysignf(
          fabs(1.0f - la) > low_approximation ? 1.0f / fabs(1.0f - la) : 1.0f / low_approximation, 1.0f - la);

      float chunk = highlights2 > 1.0f ? 1.0f : highlights2;
      float optrans = chunk * highlights_xform;
      highlights2 -= 1.0f;

      ta[0] = la * (1.0 - optrans)
              + (la > halfmax ? lmax - (lmax - doublemax * (la - halfmax)) * (lmax - lb) : doublemax * la
                                                                                           * lb) * optrans;

      ta[0] = (flags & UNBOUND_HIGHLIGHTS_L) ? ta[0] : CLAMP_RANGE(ta[0], lmin, lmax);

      ta[1] = ta[1] * (1.0f - optrans)
              + (ta[1] + tb[1]) * (ta[0] * lref * (1.0f - highlights_ccorrect)
                                   + (1.0f - ta[0]) * href * highlights_ccorrect) * optrans;

      ta[1] = (flags & UNBOUND_HIGHLIGHTS_A) ? ta[1] : CLAMP_RANGE(ta[1], min[1], max[1]);

      ta[2] = ta[2] * (1.0f - optrans)
              + (ta[2] + tb[2]) * (ta[0] * lref * (1.0f - highlights_ccorrect)
                                   + (1.0f - ta[0]) * href * highlights_ccorrect) * optrans;

      ta[2] = (flags & UNBOUND_HIGHLIGHTS_B) ? ta[2] : CLAMP_RANGE(ta[2], min[2], max[2]);
    }

    // overlay shadows
    float shadows2 = shadows * shadows;
    float shadows_xform = CLAMP_RANGE(tb[0] / (1.0f - compress) - compress / (1.0f - compress), 0.0f, 1.0f);

    while(shadows2 > 0.0f)
    {
      float la = (flags & UNBOUND_HIGHLIGHTS_L) ? ta[0] : CLAMP_RANGE(ta[0], lmin, lmax);
      float lb = (tb[0] - halfmax) * sign(shadows) * sign(lmax - la) + halfmax;
      lb = unbound_mask ? lb : CLAMP_RANGE(lb, lmin, lmax);
      float lref = copysignf(fabs(la) > low_approximation ? 1.0f / fabs(la) : 1.0f / low_approximation, la);
      float href = copysignf(
          fabs(1.0f - la) > low_approximation ? 1.0f / fabs(1.0f - la) : 1.0f / low_approximation, 1.0f - la);


      float chunk = shadows2 > 1.0f ? 1.0f : shadows2;
      float optrans = chunk * shadows_xform;
      shadows2 -= 1.0f;

      ta[0] = la * (1.0 - optrans)
              + (la > halfmax ? lmax - (lmax - doublemax * (la - halfmax)) * (lmax - lb) : doublemax * la
                                                                                           * lb) * optrans;

      ta[0] = (flags & UNBOUND_SHADOWS_L) ? ta[0] : CLAMP_RANGE(ta[0], lmin, lmax);

      ta[1] = ta[1] * (1.0f - optrans)
              + (ta[1] + tb[1]) * (ta[0] * lref * shadows_ccorrect
                                   + (1.0f - ta[0]) * href * (1.0f - shadows_ccorrect)) * optrans;

      ta[1] = (flags & UNBOUND_SHADOWS_A) ? ta[1] : CLAMP_RANGE(ta[1], min[1], max[1]);

      ta[2] = ta[2] * (1.0f - optrans)
              + (ta[2] + tb[2]) * (ta[0] * lref * shadows_ccorrect
                                   + (1.0f - ta[0]) * href * (1.0f - shadows_ccorrect)) * optrans;

      ta[2] = (flags & UNBOUND_SHADOWS_B) ? ta[2] : CLAMP_RANGE(ta[2], min[2], max[2]);
    }

    _Lab_rescale(ta, &out[j]);
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}



#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_shadhi_data_t *d = (dt_iop_shadhi_data_t *)piece->data;
  dt_iop_shadhi_global_data_t *gd = (dt_iop_shadhi_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const int order = d->order;
  const float radius = fmaxf(0.1f, d->radius);
  const float sigma = radius * roi_in->scale / piece->iscale;
  const float shadows = 2.0f * fmin(fmax(-1.0f, (d->shadows / 100.0f)), 1.0f);
  const float highlights = 2.0f * fmin(fmax(-1.0f, (d->highlights / 100.0f)), 1.0f);
  const float whitepoint = fmax(1.0f - d->whitepoint / 100.0f, 0.01f);
  const float compress
      = fmin(fmax(0.0f, (d->compress / 100.0f)), 0.99f); // upper limit 0.99f to avoid division by zero later
  const float shadows_ccorrect = (fmin(fmax(0.0f, (d->shadows_ccorrect / 100.0f)), 1.0f) - 0.5f) * sign(shadows)
                                 + 0.5f;
  const float highlights_ccorrect = (fmin(fmax(0.0f, (d->highlights_ccorrect / 100.0f)), 1.0f) - 0.5f)
                                    * sign(-highlights) + 0.5f;
  const float low_approximation = d->low_approximation;
  const unsigned int flags = d->flags;
  const int unbound_mask = ((d->shadhi_algo == SHADHI_ALGO_BILATERAL) && (flags & UNBOUND_BILATERAL))
                           || ((d->shadhi_algo == SHADHI_ALGO_GAUSSIAN) && (flags & UNBOUND_GAUSSIAN));

  size_t sizes[3];

  dt_gaussian_cl_t *g = NULL;
  dt_bilateral_cl_t *b = NULL;
  cl_mem dev_tmp = NULL;

  if(d->shadhi_algo == SHADHI_ALGO_GAUSSIAN)
  {
    float Labmax[4] = { 100.0f, 128.0f, 128.0f, 1.0f };
    float Labmin[4] = { 0.0f, -128.0f, -128.0f, 0.0f };

    if(unbound_mask)
    {
      for(int k = 0; k < 4; k++) Labmax[k] = INFINITY;
      for(int k = 0; k < 4; k++) Labmin[k] = -INFINITY;
    }

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

  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };
  err = dt_opencl_enqueue_copy_image(devid, dev_out, dev_tmp, origin, origin, region);
  if(err != CL_SUCCESS) goto error;

  // final mixing step
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPHT(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 1, sizeof(cl_mem), (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 2, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 3, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 4, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 5, sizeof(float), (void *)&shadows);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 6, sizeof(float), (void *)&highlights);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 7, sizeof(float), (void *)&compress);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 8, sizeof(float),
                           (void *)&shadows_ccorrect);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 9, sizeof(float),
                           (void *)&highlights_ccorrect);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 10, sizeof(unsigned int), (void *)&flags);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 11, sizeof(int), (void *)&unbound_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 12, sizeof(float),
                           (void *)&low_approximation);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 13, sizeof(float),
                           (void *)&whitepoint);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_shadows_highlights_mix, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_tmp);
  return TRUE;

error:
  if(g) dt_gaussian_free_cl(g);
  if(b) dt_bilateral_free_cl(b);
  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_shadows&highlights] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_shadhi_data_t *d = (dt_iop_shadhi_data_t *)piece->data;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const float radius = fmax(0.1f, d->radius);
  const float sigma = radius * roi_in->scale / piece->iscale;
  const float sigma_r = 100.0f; // does not depend on scale
  const float sigma_s = sigma;

  const size_t basebuffer = width * height * channels * sizeof(float);

  if(d->shadhi_algo == SHADHI_ALGO_BILATERAL)
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



static void radius_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->radius = copysignf(dt_bauhaus_slider_get(slider), p->radius);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void shadhi_algo_callback(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->shadhi_algo = dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void shadows_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->shadows = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void highlights_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->highlights = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void whitepoint_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->whitepoint = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void compress_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->compress = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void shadows_ccorrect_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->shadows_ccorrect = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void highlights_ccorrect_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->highlights_ccorrect = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)p1;
  dt_iop_shadhi_data_t *d = (dt_iop_shadhi_data_t *)piece->data;

  d->order = p->order;
  d->radius = p->radius;
  d->shadows = p->shadows;
  d->highlights = p->highlights;
  d->whitepoint = p->whitepoint;
  d->compress = p->compress;
  d->shadows_ccorrect = p->shadows_ccorrect;
  d->highlights_ccorrect = p->highlights_ccorrect;
  d->flags = p->flags;
  d->low_approximation = p->low_approximation;
  d->shadhi_algo = p->shadhi_algo;

#ifdef HAVE_OPENCL
  if(d->shadhi_algo == SHADHI_ALGO_BILATERAL)
    piece->process_cl_ready = (piece->process_cl_ready && !(darktable.opencl->avoid_atomics));
#endif
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_shadhi_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_shadhi_gui_data_t *g = (dt_iop_shadhi_gui_data_t *)self->gui_data;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)module->params;
  dt_bauhaus_slider_set(g->shadows, p->shadows);
  dt_bauhaus_slider_set(g->highlights, p->highlights);
  dt_bauhaus_slider_set(g->whitepoint, p->whitepoint);
  dt_bauhaus_slider_set(g->radius, p->radius);
  dt_bauhaus_combobox_set(g->shadhi_algo, p->shadhi_algo);
  dt_bauhaus_slider_set(g->compress, p->compress);
  dt_bauhaus_slider_set(g->shadows_ccorrect, p->shadows_ccorrect);
  dt_bauhaus_slider_set(g->highlights_ccorrect, p->highlights_ccorrect);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_shadhi_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_shadhi_params_t));
  module->default_enabled = 0;
  module->priority = 553; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_shadhi_params_t);
  module->gui_data = NULL;
  dt_iop_shadhi_params_t tmp
      = (dt_iop_shadhi_params_t){ DT_IOP_GAUSSIAN_ZERO, 100.0f, 50.0f, 0.0f, -50.0f, 0.0f, 50.0f, 100.0f,
                                  50.0f, UNBOUND_DEFAULT, 0.000001f, SHADHI_ALGO_GAUSSIAN };
  memcpy(module->params, &tmp, sizeof(dt_iop_shadhi_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_shadhi_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 6; // gaussian.cl, from programs.conf
  dt_iop_shadhi_global_data_t *gd
      = (dt_iop_shadhi_global_data_t *)malloc(sizeof(dt_iop_shadhi_global_data_t));
  module->data = gd;
  gd->kernel_shadows_highlights_mix = dt_opencl_create_kernel(program, "shadows_highlights_mix");
}


void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_shadhi_global_data_t *gd = (dt_iop_shadhi_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_shadows_highlights_mix);
  free(module->data);
  module->data = NULL;
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_shadhi_gui_data_t));
  dt_iop_shadhi_gui_data_t *g = (dt_iop_shadhi_gui_data_t *)self->gui_data;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->shadows = dt_bauhaus_slider_new_with_range(self, -100.0, 100.0, 2., p->shadows, 2);
  g->highlights = dt_bauhaus_slider_new_with_range(self, -100.0, 100.0, 2., p->highlights, 2);
  g->whitepoint = dt_bauhaus_slider_new_with_range(self, -10.0, 10.0, .2, p->whitepoint, 2);
  g->shadhi_algo = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->shadhi_algo, NULL, _("soften with"));
  dt_bauhaus_combobox_add(g->shadhi_algo, _("gaussian"));
  dt_bauhaus_combobox_add(g->shadhi_algo, _("bilateral filter"));
  g->radius = dt_bauhaus_slider_new_with_range(self, 0.1, 200.0, 2., p->radius, 2);
  g->compress = dt_bauhaus_slider_new_with_range(self, 0, 100.0, 2., p->compress, 2);
  g->shadows_ccorrect = dt_bauhaus_slider_new_with_range(self, 0, 100.0, 2., p->shadows_ccorrect, 2);
  g->highlights_ccorrect = dt_bauhaus_slider_new_with_range(self, 0, 100.0, 2., p->highlights_ccorrect, 2);
  dt_bauhaus_widget_set_label(g->shadows, NULL, _("shadows"));
  dt_bauhaus_widget_set_label(g->highlights, NULL, _("highlights"));
  dt_bauhaus_widget_set_label(g->whitepoint, NULL, _("white point adjustment"));
  dt_bauhaus_widget_set_label(g->radius, NULL, _("radius"));
  dt_bauhaus_widget_set_label(g->compress, NULL, _("compress"));
  dt_bauhaus_widget_set_label(g->shadows_ccorrect, NULL, _("shadows color adjustment"));
  dt_bauhaus_widget_set_label(g->highlights_ccorrect, NULL, _("highlights color adjustment"));
  dt_bauhaus_slider_set_format(g->shadows, "%.02f");
  dt_bauhaus_slider_set_format(g->highlights, "%.02f");
  dt_bauhaus_slider_set_format(g->whitepoint, "%.02f");
  dt_bauhaus_slider_set_format(g->radius, "%.02f");
  dt_bauhaus_slider_set_format(g->compress, "%.02f%%");
  dt_bauhaus_slider_set_format(g->shadows_ccorrect, "%.02f%%");
  dt_bauhaus_slider_set_format(g->highlights_ccorrect, "%.02f%%");

  gtk_box_pack_start(GTK_BOX(self->widget), g->shadows, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->highlights, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->whitepoint, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->shadhi_algo, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->radius, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->compress, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->shadows_ccorrect, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->highlights_ccorrect, TRUE, TRUE, 0);

  gtk_widget_set_tooltip_text(g->shadows, _("correct shadows"));
  gtk_widget_set_tooltip_text(g->highlights, _("correct highlights"));
  gtk_widget_set_tooltip_text(g->whitepoint, _("shift white point"));
  gtk_widget_set_tooltip_text(g->radius, _("spatial extent"));
  gtk_widget_set_tooltip_text(g->shadhi_algo, _("filter to use for softening. bilateral avoids halos"));
  gtk_widget_set_tooltip_text(g->compress, _("compress the effect on shadows/highlights and\npreserve midtones"));
  gtk_widget_set_tooltip_text(g->shadows_ccorrect, _("adjust saturation of shadows"));
  gtk_widget_set_tooltip_text(g->highlights_ccorrect, _("adjust saturation of highlights"));

  g_signal_connect(G_OBJECT(g->shadows), "value-changed", G_CALLBACK(shadows_callback), self);
  g_signal_connect(G_OBJECT(g->highlights), "value-changed", G_CALLBACK(highlights_callback), self);
  g_signal_connect(G_OBJECT(g->whitepoint), "value-changed", G_CALLBACK(whitepoint_callback), self);
  g_signal_connect(G_OBJECT(g->radius), "value-changed", G_CALLBACK(radius_callback), self);
  g_signal_connect(G_OBJECT(g->shadhi_algo), "value-changed", G_CALLBACK(shadhi_algo_callback), self);
  g_signal_connect(G_OBJECT(g->compress), "value-changed", G_CALLBACK(compress_callback), self);
  g_signal_connect(G_OBJECT(g->shadows_ccorrect), "value-changed", G_CALLBACK(shadows_ccorrect_callback), self);
  g_signal_connect(G_OBJECT(g->highlights_ccorrect), "value-changed", G_CALLBACK(highlights_ccorrect_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
