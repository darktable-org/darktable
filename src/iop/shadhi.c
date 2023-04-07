/*
  This file is part of darktable,
  Copyright (C) 2012-2020 darktable developers.

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
#include "develop/imageop_gui.h"
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

DT_MODULE_INTROSPECTION(5, dt_iop_shadhi_params_t)

typedef enum dt_iop_shadhi_algo_t
{
  SHADHI_ALGO_GAUSSIAN, // $DESCRIPTION: "gaussian"
  SHADHI_ALGO_BILATERAL // $DESCRIPTION: "bilateral filter"
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
  dt_gaussian_order_t order; // $DEFAULT: DT_IOP_GAUSSIAN_ZERO
  float radius;     // $MIN: 0.1 $MAX: 500.0 $DEFAULT: 100.0
  float shadows;    // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 50.0
  float whitepoint; // $MIN: -10.0 $MAX: 10.0 $DEFAULT: 0.0 $DESCRIPTION: "white point adjustment"
  float highlights; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: -50.0
  float reserved2;
  float compress;   // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0
  float shadows_ccorrect;    // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "shadows color adjustment"
  float highlights_ccorrect; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0 $DESCRIPTION: "highlights color adjustment"
  unsigned int flags;        // $DEFAULT: UNBOUND_DEFAULT
  float low_approximation;   // $DEFAULT: 0.000001
  dt_iop_shadhi_algo_t shadhi_algo; // $DEFAULT: SHADHI_ALGO_GAUSSIAN $DESCRIPTION: "soften with" $DEFAULT: 0
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

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("modify the tonal range of the shadows and highlights\n"
                                        "of an image by enhancing local contrast."),
                                      _("corrective and creative"),
                                      _("linear or non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
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

#ifdef _OPENMP
#pragma omp declare simd aligned(ivoid, ovoid : 64)
#endif
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_shadhi_data_t *const restrict data = (dt_iop_shadhi_data_t *)piece->data;
  const float *const restrict in = (float *)ivoid;
  float *const restrict out = (float *)ovoid;
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
    dt_aligned_pixel_t Labmax = { 100.0f, 128.0f, 128.0f, 1.0f };
    dt_aligned_pixel_t Labmin = { 0.0f, -128.0f, -128.0f, 0.0f };

    if(unbound_mask)
    {
      for(int k = 0; k < 4; k++) Labmax[k] = FLT_MAX;
      for(int k = 0; k < 4; k++) Labmin[k] = -FLT_MAX;
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

  const dt_aligned_pixel_t max = { 1.0f, 1.0f, 1.0f, 1.0f };
  const dt_aligned_pixel_t min = { 0.0f, -1.0f, -1.0f, 0.0f };
  const float lmin = 0.0f;
  const float lmax = max[0] + fabsf(min[0]);
  const float halfmax = lmax / 2.0;
  const float doublemax = lmax * 2.0;


#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, compress, doublemax, flags, halfmax, height, \
                      highlights, highlights_ccorrect, lmax, lmin, \
                      low_approximation, max, min,  shadows, \
                      shadows_ccorrect, unbound_mask, whitepoint, width) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t j = 0; j < (size_t)width * height * ch; j += ch)
  {
    dt_aligned_pixel_t ta, tb;
    _Lab_scale(&in[j], ta);
    // invert and desaturate the blurred output pixel
    out[j + 0] = 100.0f - out[j + 0];
    out[j + 1] = 0.0f;
    out[j + 2] = 0.0f;
    _Lab_scale(&out[j], tb);

    ta[0] = ta[0] > 0.0f ? ta[0] / whitepoint : ta[0];
    tb[0] = tb[0] > 0.0f ? tb[0] / whitepoint : tb[0];

    // overlay highlights
    float highlights2 = highlights * highlights;
    const float highlights_xform = CLAMP(1.0f - tb[0] / (1.0f - compress), 0.0f, 1.0f);

    while(highlights2 > 0.0f)
    {
      const float la = (flags & UNBOUND_HIGHLIGHTS_L) ? ta[0] : CLAMP(ta[0], lmin, lmax);
      float lb = (tb[0] - halfmax) * sign(-highlights) * sign(lmax - la) + halfmax;
      lb = unbound_mask ? lb : CLAMP(lb, lmin, lmax);
      const float lref = copysignf(fabsf(la) > low_approximation ? 1.0f / fabsf(la) : 1.0f / low_approximation, la);
      const float href = copysignf(
          fabsf(1.0f - la) > low_approximation ? 1.0f / fabsf(1.0f - la) : 1.0f / low_approximation, 1.0f - la);

      const float chunk = highlights2 > 1.0f ? 1.0f : highlights2;
      const float optrans = chunk * highlights_xform;
      highlights2 -= 1.0f;

      ta[0] = la * (1.0 - optrans)
              + (la > halfmax ? lmax - (lmax - doublemax * (la - halfmax)) * (lmax - lb) : doublemax * la
                                                                                           * lb) * optrans;

      ta[0] = (flags & UNBOUND_HIGHLIGHTS_L) ? ta[0] : CLAMP(ta[0], lmin, lmax);

      const float chroma_factor = (ta[0] * lref * (1.0f - highlights_ccorrect)
                                   + (1.0f - ta[0]) * href * highlights_ccorrect);
      ta[1] = ta[1] * (1.0f - optrans) + (ta[1] + tb[1]) * chroma_factor * optrans;
      ta[1] = (flags & UNBOUND_HIGHLIGHTS_A) ? ta[1] : CLAMP(ta[1], min[1], max[1]);

      ta[2] = ta[2] * (1.0f - optrans) + (ta[2] + tb[2]) * chroma_factor * optrans;
      ta[2] = (flags & UNBOUND_HIGHLIGHTS_B) ? ta[2] : CLAMP(ta[2], min[2], max[2]);
    }

    // overlay shadows
    float shadows2 = shadows * shadows;
    const float shadows_xform = CLAMP(tb[0] / (1.0f - compress) - compress / (1.0f - compress), 0.0f, 1.0f);

    while(shadows2 > 0.0f)
    {
      const float la = (flags & UNBOUND_HIGHLIGHTS_L) ? ta[0] : CLAMP(ta[0], lmin, lmax);
      float lb = (tb[0] - halfmax) * sign(shadows) * sign(lmax - la) + halfmax;
      lb = unbound_mask ? lb : CLAMP(lb, lmin, lmax);
      const float lref = copysignf(fabsf(la) > low_approximation ? 1.0f / fabsf(la) : 1.0f / low_approximation, la);
      const float href = copysignf(
          fabsf(1.0f - la) > low_approximation ? 1.0f / fabsf(1.0f - la) : 1.0f / low_approximation, 1.0f - la);


      const float chunk = shadows2 > 1.0f ? 1.0f : shadows2;
      const float optrans = chunk * shadows_xform;
      shadows2 -= 1.0f;

      ta[0] = la * (1.0 - optrans)
              + (la > halfmax ? lmax - (lmax - doublemax * (la - halfmax)) * (lmax - lb) : doublemax * la
                                                                                           * lb) * optrans;

      ta[0] = (flags & UNBOUND_SHADOWS_L) ? ta[0] : CLAMP(ta[0], lmin, lmax);

      const float chroma_factor = (ta[0] * lref * shadows_ccorrect
                                   + (1.0f - ta[0]) * href * (1.0f - shadows_ccorrect));
      ta[1] = ta[1] * (1.0f - optrans) + (ta[1] + tb[1]) * chroma_factor * optrans;
      ta[1] = (flags & UNBOUND_SHADOWS_A) ? ta[1] : CLAMP(ta[1], min[1], max[1]);

      ta[2] = ta[2] * (1.0f - optrans) + (ta[2] + tb[2]) * chroma_factor * optrans;
      ta[2] = (flags & UNBOUND_SHADOWS_B) ? ta[2] : CLAMP(ta[2], min[2], max[2]);
    }

    _Lab_rescale(ta, &out[j]);
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}



#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_shadhi_data_t *d = (dt_iop_shadhi_data_t *)piece->data;
  dt_iop_shadhi_global_data_t *gd = (dt_iop_shadhi_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
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
    dt_aligned_pixel_t Labmax = { 100.0f, 128.0f, 128.0f, 1.0f };
    dt_aligned_pixel_t Labmin = { 0.0f, -128.0f, -128.0f, 0.0f };

    if(unbound_mask)
    {
      for(int k = 0; k < 4; k++) Labmax[k] = FLT_MAX;
      for(int k = 0; k < 4; k++) Labmin[k] = -FLT_MAX;
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

  dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;

  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };
  err = dt_opencl_enqueue_copy_image(devid, dev_out, dev_tmp, origin, origin, region);
  if(err != CL_SUCCESS) goto error;

  // final mixing step
  sizes[0] = ROUNDUPDWD(width, devid);
  sizes[1] = ROUNDUPDHT(height, devid);
  sizes[2] = 1;
  dt_opencl_set_kernel_args(devid, gd->kernel_shadows_highlights_mix, 0, CLARG(dev_in), CLARG(dev_tmp),
    CLARG(dev_out), CLARG(width), CLARG(height), CLARG(shadows), CLARG(highlights), CLARG(compress),
    CLARG(shadows_ccorrect), CLARG(highlights_ccorrect), CLARG(flags), CLARG(unbound_mask), CLARG(low_approximation),
    CLARG(whitepoint));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_shadows_highlights_mix, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_tmp);
  return TRUE;

error:
  if(g) dt_gaussian_free_cl(g);
  if(b) dt_bilateral_free_cl(b);
  dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_shadows&highlights] couldn't enqueue kernel! %s\n", cl_errstr(err));
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

  const size_t basebuffer = sizeof(float) * channels * width * height;

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
#ifdef HAVE_OPENCL
    tiling->factor_cl = 2.0f + fmax(1.0f, (float)dt_gaussian_memory_use_cl(width, height, channels) / basebuffer);
#endif
    tiling->maxbuf = fmax(1.0f, (float)dt_gaussian_singlebuffer_size(width, height, channels) / basebuffer);
  }

  tiling->overhead = 0;
  tiling->overlap = ceilf(4 * sigma);
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
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
    piece->process_cl_ready = (piece->process_cl_ready && !dt_opencl_avoid_atomics(pipe->devid));
#endif
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_shadhi_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 6; // gaussian.cl, from programs.conf
  dt_iop_shadhi_global_data_t *gd
      = (dt_iop_shadhi_global_data_t *)malloc(sizeof(dt_iop_shadhi_global_data_t));
  module->data = gd;
  gd->kernel_shadows_highlights_mix = dt_opencl_create_kernel(program, "shadows_highlights_mix");
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
  dt_iop_shadhi_gui_data_t *g = IOP_GUI_ALLOC(shadhi);

  g->shadows = dt_bauhaus_slider_from_params(self, N_("shadows"));
  g->highlights = dt_bauhaus_slider_from_params(self, N_("highlights"));
  g->whitepoint = dt_bauhaus_slider_from_params(self, "whitepoint");
  g->shadhi_algo = dt_bauhaus_combobox_from_params(self, "shadhi_algo");
  g->radius = dt_bauhaus_slider_from_params(self, N_("radius"));
  g->compress = dt_bauhaus_slider_from_params(self, N_("compress"));
  dt_bauhaus_slider_set_format(g->compress, "%");
  g->shadows_ccorrect = dt_bauhaus_slider_from_params(self, "shadows_ccorrect");
  dt_bauhaus_slider_set_format(g->shadows_ccorrect, "%");
  g->highlights_ccorrect = dt_bauhaus_slider_from_params(self, "highlights_ccorrect");
  dt_bauhaus_slider_set_format(g->highlights_ccorrect, "%");

  gtk_widget_set_tooltip_text(g->shadows, _("correct shadows"));
  gtk_widget_set_tooltip_text(g->highlights, _("correct highlights"));
  gtk_widget_set_tooltip_text(g->whitepoint, _("shift white point"));
  gtk_widget_set_tooltip_text(g->radius, _("spatial extent"));
  gtk_widget_set_tooltip_text(g->shadhi_algo, _("filter to use for softening. bilateral avoids halos"));
  gtk_widget_set_tooltip_text(g->compress, _("compress the effect on shadows/highlights and\npreserve mid-tones"));
  gtk_widget_set_tooltip_text(g->shadows_ccorrect, _("adjust saturation of shadows"));
  gtk_widget_set_tooltip_text(g->highlights_ccorrect, _("adjust saturation of highlights"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

