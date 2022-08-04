/*
  This file is part of darktable,
  Copyright (C) 2015-2021 darktable developers.

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
#include "common/colorspaces_inline_conversions.h"
#include "common/debug.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S 500
#define DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_R 100
#define DT_COLORRECONSTRUCT_SPATIAL_APPROX 100.0f

DT_MODULE_INTROSPECTION(3, dt_iop_colorreconstruct_params_t)

typedef enum dt_iop_colorreconstruct_precedence_t
{
  COLORRECONSTRUCT_PRECEDENCE_NONE,   // $DESCRIPTION: "None" same weighting factor for all pixels
  COLORRECONSTRUCT_PRECEDENCE_CHROMA, // $DESCRIPTION: "Saturated colors" use chromaticy as weighting factor -> prefers saturated colors
  COLORRECONSTRUCT_PRECEDENCE_HUE     // $DESCRIPTION: "Hue" use a specific hue as weighting factor
} dt_iop_colorreconstruct_precedence_t;

typedef struct dt_iop_colorreconstruct_params1_t
{
  float threshold;
  float spatial;
  float range;
} dt_iop_colorreconstruct_params1_t;

typedef struct dt_iop_colorreconstruct_params2_t
{
  float threshold;
  float spatial;
  float range;
  dt_iop_colorreconstruct_precedence_t precedence;
} dt_iop_colorreconstruct_params2_t;

typedef struct dt_iop_colorreconstruct_params_t
{
  float threshold; // $MIN: 50.0 $MAX: 150.0 $DEFAULT: 100.0
  float spatial;   // $MIN: 0.0 $MAX: 1000.0 $DEFAULT: 400.0 $DESCRIPTION: "Spatial extent"
  float range;     // $MIN: 0.0 $MAX: 50.0 $DEFAULT: 10.0 $DESCRIPTION: "Range extent"
  float hue;       // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.66
  dt_iop_colorreconstruct_precedence_t precedence; // $DEFAULT: 0 COLORRECONSTRUCT_PRECEDENCE_NONE
} dt_iop_colorreconstruct_params_t;

typedef struct dt_iop_colorreconstruct_Lab_t
{
  float L;
  float a;
  float b;
  float weight;
} dt_iop_colorreconstruct_Lab_t;

typedef struct dt_iop_colorreconstruct_bilateral_frozen_t
{
  size_t size_x, size_y, size_z;
  int width, height, x, y;
  float scale;
  float sigma_s, sigma_r;
  dt_iop_colorreconstruct_Lab_t *buf;
} dt_iop_colorreconstruct_bilateral_frozen_t;

typedef struct dt_iop_colorreconstruct_gui_data_t
{
  GtkWidget *threshold;
  GtkWidget *spatial;
  GtkWidget *range;
  GtkWidget *precedence;
  GtkWidget *hue;
  dt_iop_colorreconstruct_bilateral_frozen_t *can;
  uint64_t hash;
} dt_iop_colorreconstruct_gui_data_t;

typedef struct dt_iop_colorreconstruct_data_t
{
  float threshold;
  float spatial;
  float range;
  float hue;
  dt_iop_colorreconstruct_precedence_t precedence;
} dt_iop_colorreconstruct_data_t;

typedef struct dt_iop_colorreconstruct_global_data_t
{
  int kernel_colorreconstruct_zero;
  int kernel_colorreconstruct_splat;
  int kernel_colorreconstruct_blur_line;
  int kernel_colorreconstruct_slice;
} dt_iop_colorreconstruct_global_data_t;


const char *name()
{
  return _("Color reconstruction");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("Recover clipped highlights by propagating surrounding colors"),
                                      _("Corrective"),
                                      _("Linear or non-linear, Lab, display-referred"),
                                      _("Non-linear, Lab"),
                                      _("Non-linear, Lab, display-referred"));
}

int flags()
{
  // we do not allow tiling. reason: this module needs to see the full surrounding of highlights.
  // if we would split into tiles, each tile would result in different color corrections
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 3)
  {
    const dt_iop_colorreconstruct_params1_t *old = old_params;
    dt_iop_colorreconstruct_params_t *new = new_params;
    new->threshold = old->threshold;
    new->spatial = old->spatial;
    new->range = old->range;
    new->precedence = COLORRECONSTRUCT_PRECEDENCE_NONE;
    new->hue = 0.66f;
    return 0;
  }
  else if(old_version == 2 && new_version == 3)
  {
    const dt_iop_colorreconstruct_params2_t *old = old_params;
    dt_iop_colorreconstruct_params_t *new = new_params;
    new->threshold = old->threshold;
    new->spatial = old->spatial;
    new->range = old->range;
    new->precedence = old->precedence;
    new->hue = 0.66f;
    return 0;
  }
  return 1;
}

typedef struct dt_iop_colorreconstruct_bilateral_t
{
  size_t size_x, size_y, size_z;
  int width, height, x, y;
  float scale;
  float sigma_s, sigma_r;
  dt_iop_colorreconstruct_Lab_t *buf;
} dt_iop_colorreconstruct_bilateral_t;


static inline float hue_conversion(const float HSL_Hue)
{
  dt_aligned_pixel_t rgb = { 0 };
  dt_aligned_pixel_t XYZ = { 0 };
  dt_aligned_pixel_t Lab = { 0 };

  hsl2rgb(rgb, HSL_Hue, 1.0f, 0.5f);

  XYZ[0] = (rgb[0] * 0.4360747f) + (rgb[1] * 0.3850649f) + (rgb[2] * 0.1430804f);
  XYZ[1] = (rgb[0] * 0.2225045f) + (rgb[1] * 0.7168786f) + (rgb[2] * 0.0606169f);
  XYZ[2] = (rgb[0] * 0.0139322f) + (rgb[1] * 0.0971045f) + (rgb[2] * 0.7141733f);

  dt_XYZ_to_Lab(XYZ, Lab);

  // Hue from LCH color space in [-pi, +pi] interval
  float LCH_hue = atan2f(Lab[2], Lab[1]);

  return LCH_hue;
}


static inline void image_to_grid(const dt_iop_colorreconstruct_bilateral_t *const b, const float i, const float j, const float L, float *x,
                          float *y, float *z)
{
  *x = CLAMPS(i / b->sigma_s, 0, b->size_x - 1);
  *y = CLAMPS(j / b->sigma_s, 0, b->size_y - 1);
  *z = CLAMPS(L / b->sigma_r, 0, b->size_z - 1);
}

static inline void grid_rescale(const dt_iop_colorreconstruct_bilateral_t *const b, const int i, const int j, const dt_iop_roi_t *roi,
                         const float scale, float *px, float *py)
{
  *px = (roi->x + i) * scale - b->x;
  *py = (roi->y + j) * scale - b->y;
}

static void dt_iop_colorreconstruct_bilateral_dump(dt_iop_colorreconstruct_bilateral_frozen_t *bf)
{
  if(!bf) return;
  dt_free_align(bf->buf);
  free(bf);
}

static void dt_iop_colorreconstruct_bilateral_free(dt_iop_colorreconstruct_bilateral_t *b)
{
  if(!b) return;
  dt_free_align(b->buf);
  free(b);
}

static dt_iop_colorreconstruct_bilateral_t *dt_iop_colorreconstruct_bilateral_init(const dt_iop_roi_t *roi, // dimensions of input image
                                                                                   const float iscale,      // overall scale of input image
                                                                                   const float sigma_s,     // spatial sigma (blur pixel coords)
                                                                                   const float sigma_r)     // range sigma (blur luma values)
{
  dt_iop_colorreconstruct_bilateral_t *b = (dt_iop_colorreconstruct_bilateral_t *)malloc(sizeof(dt_iop_colorreconstruct_bilateral_t));
  if(!b)
  {
    fprintf(stderr, "[color reconstruction] not able to allocate buffer (a)\n");
    return NULL;
  }
  float _x = roundf(roi->width / sigma_s);
  float _y = roundf(roi->height / sigma_s);
  float _z = roundf(100.0f / sigma_r);
  b->size_x = CLAMPS((int)_x, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  b->size_y = CLAMPS((int)_y, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  b->size_z = CLAMPS((int)_z, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_R) + 1;
  b->width = roi->width;
  b->height = roi->height;
  b->x = roi->x;
  b->y = roi->y;
  b->scale = iscale / roi->scale;
  b->sigma_s = MAX(roi->height / (b->size_y - 1.0f), roi->width / (b->size_x - 1.0f));
  b->sigma_r = 100.0f / (b->size_z - 1.0f);
  b->buf = dt_alloc_align(64, sizeof(dt_iop_colorreconstruct_Lab_t) * b->size_x * b->size_y * b->size_z);
  if(!b->buf)
  {
    fprintf(stderr, "[color reconstruction] not able to allocate buffer (b)\n");
    dt_iop_colorreconstruct_bilateral_free(b);
    return NULL;
  }

  memset(b->buf, 0, sizeof(dt_iop_colorreconstruct_Lab_t) * b->size_x * b->size_y * b->size_z);
#if 0
  fprintf(stderr, "[bilateral] created grid [%d %d %d]"
          " with sigma (%f %f) (%f %f)\n", b->size_x, b->size_y, b->size_z,
          b->sigma_s, sigma_s, b->sigma_r, sigma_r);
#endif
  return b;
}

static dt_iop_colorreconstruct_bilateral_frozen_t *dt_iop_colorreconstruct_bilateral_freeze(dt_iop_colorreconstruct_bilateral_t *b)
{
  if(!b) return NULL;

  dt_iop_colorreconstruct_bilateral_frozen_t *bf = (dt_iop_colorreconstruct_bilateral_frozen_t *)malloc(sizeof(dt_iop_colorreconstruct_bilateral_frozen_t));
  if(!bf)
  {
    fprintf(stderr, "[color reconstruction] not able to allocate buffer (c)\n");
    return NULL;
  }

  bf->size_x = b->size_x;
  bf->size_y = b->size_y;
  bf->size_z = b->size_z;
  bf->width = b->width;
  bf->height = b->height;
  bf->x = b->x;
  bf->y = b->y;
  bf->scale = b->scale;
  bf->sigma_s = b->sigma_s;
  bf->sigma_r = b->sigma_r;
  bf->buf = dt_alloc_align(64, sizeof(dt_iop_colorreconstruct_Lab_t) * b->size_x * b->size_y * b->size_z);
  if(bf->buf && b->buf)
  {
    memcpy(bf->buf, b->buf, sizeof(dt_iop_colorreconstruct_Lab_t) * b->size_x * b->size_y * b->size_z);
  }
  else
  {
    fprintf(stderr, "[color reconstruction] not able to allocate buffer (d)\n");
    dt_iop_colorreconstruct_bilateral_dump(bf);
    return NULL;
  }

  return bf;
}

static dt_iop_colorreconstruct_bilateral_t *dt_iop_colorreconstruct_bilateral_thaw(dt_iop_colorreconstruct_bilateral_frozen_t *bf)
{
  if(!bf) return NULL;

  dt_iop_colorreconstruct_bilateral_t *b = (dt_iop_colorreconstruct_bilateral_t *)malloc(sizeof(dt_iop_colorreconstruct_bilateral_t));
  if(!b)
  {
    fprintf(stderr, "[color reconstruction] not able to allocate buffer (e)\n");
    return NULL;
  }

  b->size_x = bf->size_x;
  b->size_y = bf->size_y;
  b->size_z = bf->size_z;
  b->width = bf->width;
  b->height = bf->height;
  b->x = bf->x;
  b->y = bf->y;
  b->scale = bf->scale;
  b->sigma_s = bf->sigma_s;
  b->sigma_r = bf->sigma_r;
  b->buf = dt_alloc_align(64, sizeof(dt_iop_colorreconstruct_Lab_t) * b->size_x * b->size_y * b->size_z);
  if(b->buf && bf->buf)
  {
    memcpy(b->buf, bf->buf, sizeof(dt_iop_colorreconstruct_Lab_t) * b->size_x * b->size_y * b->size_z);
  }
  else
  {
    fprintf(stderr, "[color reconstruction] not able to allocate buffer (f)\n");
    dt_iop_colorreconstruct_bilateral_free(b);
    return NULL;
  }

  return b;
}


static void dt_iop_colorreconstruct_bilateral_splat(dt_iop_colorreconstruct_bilateral_t *b, const float *const in, const float threshold,
                                                    dt_iop_colorreconstruct_precedence_t precedence, const float *params)
{
  if(!b) return;

  // splat into downsampled grid
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, threshold) \
  shared(b, precedence, params)
#endif
  for(int j = 0; j < b->height; j++)
  {
    size_t index = (size_t)4 * j * b->width;
    for(int i = 0; i < b->width; i++, index += 4)
    {
      float x, y, z, weight, m;
      const float Lin = in[index];
      const float ain = in[index + 1];
      const float bin = in[index + 2];
      // we deliberately ignore pixels above threshold
      if (Lin > threshold) continue;

      switch(precedence)
      {
        case COLORRECONSTRUCT_PRECEDENCE_CHROMA:
          weight = sqrtf(ain * ain + bin * bin);
          break;

        case COLORRECONSTRUCT_PRECEDENCE_HUE:
          m = atan2f(bin, ain) - params[0];
          // readjust m into [-pi, +pi] interval
          m = m > M_PI ? m - 2*M_PI : (m < -M_PI ? m + 2*M_PI : m);
          weight = expf(-m*m/params[1]);
          break;

        case COLORRECONSTRUCT_PRECEDENCE_NONE:
        default:
          weight = 1.0f;
          break;
      }

      image_to_grid(b, i, j, Lin, &x, &y, &z);

      // closest integer splatting:
      const int xi = CLAMPS((int)round(x), 0, b->size_x - 1);
      const int yi = CLAMPS((int)round(y), 0, b->size_y - 1);
      const int zi = CLAMPS((int)round(z), 0, b->size_z - 1);
      const size_t grid_index = xi + b->size_x * (yi + b->size_y * zi);

#ifdef _OPENMP
#pragma omp atomic
#endif
      b->buf[grid_index].L += Lin * weight;

#ifdef _OPENMP
#pragma omp atomic
#endif
      b->buf[grid_index].a += ain * weight;

#ifdef _OPENMP
#pragma omp atomic
#endif
      b->buf[grid_index].b += bin * weight;

#ifdef _OPENMP
#pragma omp atomic
#endif
      b->buf[grid_index].weight += weight;
    }
  }
}


static void blur_line(dt_iop_colorreconstruct_Lab_t *buf, const int offset1, const int offset2, const int offset3, const int size1,
                      const int size2, const int size3)
{
  if(!buf) return;

  const float w0 = 6.f / 16.f;
  const float w1 = 4.f / 16.f;
  const float w2 = 1.f / 16.f;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(offset1, offset2, offset3, size1, size2, size3, w0, w1, w2) \
  shared(buf)
#endif
  for(int k = 0; k < size1; k++)
  {
    size_t index = (size_t)k * offset1;
    for(int j = 0; j < size2; j++)
    {
      dt_iop_colorreconstruct_Lab_t tmp1 = buf[index];
      buf[index].L      = buf[index].L      * w0 + w1 * buf[index + offset3].L      + w2 * buf[index + 2 * offset3].L;
      buf[index].a      = buf[index].a      * w0 + w1 * buf[index + offset3].a      + w2 * buf[index + 2 * offset3].a;
      buf[index].b      = buf[index].b      * w0 + w1 * buf[index + offset3].b      + w2 * buf[index + 2 * offset3].b;
      buf[index].weight = buf[index].weight * w0 + w1 * buf[index + offset3].weight + w2 * buf[index + 2 * offset3].weight;
      index += offset3;
      dt_iop_colorreconstruct_Lab_t tmp2 = buf[index];
      buf[index].L      = buf[index].L      * w0 + w1 * (buf[index + offset3].L      + tmp1.L)      + w2 * buf[index + 2 * offset3].L;
      buf[index].a      = buf[index].a      * w0 + w1 * (buf[index + offset3].a      + tmp1.a)      + w2 * buf[index + 2 * offset3].a;
      buf[index].b      = buf[index].b      * w0 + w1 * (buf[index + offset3].b      + tmp1.b)      + w2 * buf[index + 2 * offset3].b;
      buf[index].weight = buf[index].weight * w0 + w1 * (buf[index + offset3].weight + tmp1.weight) + w2 * buf[index + 2 * offset3].weight;
      index += offset3;
      for(int i = 2; i < size3 - 2; i++)
      {
        const dt_iop_colorreconstruct_Lab_t tmp3 = buf[index];
        buf[index].L      = buf[index].L      * w0 + w1 * (buf[index + offset3].L      + tmp2.L)
                     + w2 * (buf[index + 2 * offset3].L      + tmp1.L);
        buf[index].a      = buf[index].a      * w0 + w1 * (buf[index + offset3].a      + tmp2.a)
                     + w2 * (buf[index + 2 * offset3].a      + tmp1.a);
        buf[index].b      = buf[index].b      * w0 + w1 * (buf[index + offset3].b      + tmp2.b)
                     + w2 * (buf[index + 2 * offset3].b      + tmp1.b);
        buf[index].weight = buf[index].weight * w0 + w1 * (buf[index + offset3].weight + tmp2.weight)
                     + w2 * (buf[index + 2 * offset3].weight + tmp1.weight);

        index += offset3;
        tmp1 = tmp2;
        tmp2 = tmp3;
      }
      const dt_iop_colorreconstruct_Lab_t tmp3 = buf[index];
      buf[index].L      = buf[index].L      * w0 + w1 * (buf[index + offset3].L      + tmp2.L)      + w2 * tmp1.L;
      buf[index].a      = buf[index].a      * w0 + w1 * (buf[index + offset3].a      + tmp2.a)      + w2 * tmp1.a;
      buf[index].b      = buf[index].b      * w0 + w1 * (buf[index + offset3].b      + tmp2.b)      + w2 * tmp1.b;
      buf[index].weight = buf[index].weight * w0 + w1 * (buf[index + offset3].weight + tmp2.weight) + w2 * tmp1.weight;
      index += offset3;
      buf[index].L      = buf[index].L      * w0 + w1 * tmp3.L      + w2 * tmp2.L;
      buf[index].a      = buf[index].a      * w0 + w1 * tmp3.a      + w2 * tmp2.a;
      buf[index].b      = buf[index].b      * w0 + w1 * tmp3.b      + w2 * tmp2.b;
      buf[index].weight = buf[index].weight * w0 + w1 * tmp3.weight + w2 * tmp2.weight;
      index += offset3;
      index += offset2 - offset3 * size3;
    }
  }
}


static void dt_iop_colorreconstruct_bilateral_blur(dt_iop_colorreconstruct_bilateral_t *b)
{
  if(!b) return;

  // gaussian up to 3 sigma
  blur_line(b->buf, b->size_x * b->size_y, b->size_x, 1, b->size_z, b->size_y, b->size_x);
  // gaussian up to 3 sigma
  blur_line(b->buf, b->size_x * b->size_y, 1, b->size_x, b->size_z, b->size_x, b->size_y);
  // gaussian up to 3 sigma
  blur_line(b->buf, 1, b->size_x, b->size_x * b->size_y, b->size_x, b->size_y, b->size_z);
}

static void dt_iop_colorreconstruct_bilateral_slice(const dt_iop_colorreconstruct_bilateral_t *const b,
                                                    const float *const in, float *const out,
                                                    const float threshold, const dt_iop_roi_t *const roi,
                                                    const float iscale)
{
  if(!b) return;

  const float rescale = iscale / (roi->scale * b->scale);
  const int ox = 1;
  const int oy = b->size_x;
  const int oz = b->size_y * b->size_x;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(b, in, out, oy, oz, rescale, roi, threshold, ox)
#endif
  for(int j = 0; j < roi->height; j++)
  {
    size_t index = (size_t)4 * j * roi->width;
    for(int i = 0; i < roi->width; i++, index += 4)
    {
      float x, y, z;
      float px, py;
      const float Lin = out[index + 0] = in[index + 0];
      const float ain = out[index + 1] = in[index + 1];
      const float bin = out[index + 2] = in[index + 2];
      out[index + 3] = in[index + 3];
      const float blend = CLAMPS(20.0f / threshold * Lin - 19.0f, 0.0f, 1.0f);
      if (blend == 0.0f) continue;
      grid_rescale(b, i, j, roi, rescale, &px, &py);
      image_to_grid(b, px, py, Lin, &x, &y, &z);
      // trilinear lookup:
      const int xi = MIN((int)x, b->size_x - 2);
      const int yi = MIN((int)y, b->size_y - 2);
      const int zi = MIN((int)z, b->size_z - 2);
      const float xf = x - xi;
      const float yf = y - yi;
      const float zf = z - zi;
      const size_t gi = xi + b->size_x * (yi + b->size_y * zi);

      const float Lout =   b->buf[gi].L * (1.0f - xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + ox].L * (xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + oy].L * (1.0f - xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + ox + oy].L * (xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + oz].L * (1.0f - xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + ox + oz].L * (xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + oy + oz].L * (1.0f - xf) * (yf) * (zf)
                         + b->buf[gi + ox + oy + oz].L * (xf) * (yf) * (zf);

      const float aout =   b->buf[gi].a * (1.0f - xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + ox].a * (xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + oy].a * (1.0f - xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + ox + oy].a * (xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + oz].a * (1.0f - xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + ox + oz].a * (xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + oy + oz].a * (1.0f - xf) * (yf) * (zf)
                         + b->buf[gi + ox + oy + oz].a * (xf) * (yf) * (zf);


      const float bout =   b->buf[gi].b * (1.0f - xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + ox].b * (xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + oy].b * (1.0f - xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + ox + oy].b * (xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + oz].b * (1.0f - xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + ox + oz].b * (xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + oy + oz].b * (1.0f - xf) * (yf) * (zf)
                         + b->buf[gi + ox + oy + oz].b * (xf) * (yf) * (zf);

      const float weight = b->buf[gi].weight * (1.0f - xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + ox].weight * (xf) * (1.0f - yf) * (1.0f - zf)
                         + b->buf[gi + oy].weight * (1.0f - xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + ox + oy].weight * (xf) * (yf) * (1.0f - zf)
                         + b->buf[gi + oz].weight * (1.0f - xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + ox + oz].weight * (xf) * (1.0f - yf) * (zf)
                         + b->buf[gi + oy + oz].weight * (1.0f - xf) * (yf) * (zf)
                         + b->buf[gi + ox + oy + oz].weight * (xf) * (yf) * (zf);

      const float lout = fmax(Lout, 0.01f);
      out[index + 1] = (weight > 0.0f) ? ain * (1.0f - blend) + aout * Lin/lout * blend : ain;
      out[index + 2] = (weight > 0.0f) ? bin * (1.0f - blend) + bout * Lin/lout * blend : bin;
    }
  }
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorreconstruct_data_t *data = (dt_iop_colorreconstruct_data_t *)piece->data;
  dt_iop_colorreconstruct_gui_data_t *g = (dt_iop_colorreconstruct_gui_data_t *)self->gui_data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;

  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float sigma_r = fmax(data->range, 0.1f);
  const float sigma_s = fmax(data->spatial, 1.0f) / scale;
  const float hue = hue_conversion(data->hue); // convert to LCH hue which better fits to Lab colorspace

  const dt_aligned_pixel_t params = { hue, M_PI*M_PI/8, 0.0f, 0.0f };

  dt_iop_colorreconstruct_bilateral_t *b;
  dt_iop_colorreconstruct_bilateral_frozen_t *can = NULL;

  // color reconstruction often involves a massive spatial blur of the bilateral grid. this typically requires
  // more or less the whole image to contribute to the grid. In pixelpipe FULL we can not rely on this
  // as the pixelpipe might only see part of the image (region of interest). Therefore we "steal" the bilateral grid
  // of the preview pipe if needed. However, the grid of the preview pipeline is coarser and may lead
  // to other artifacts so we only want to use it when necessary. The threshold for data->spatial has been selected
  // arbitrarily.
  if(sigma_s > DT_COLORRECONSTRUCT_SPATIAL_APPROX
     && self->dev->gui_attached
     && g
     && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL) == DT_DEV_PIXELPIPE_FULL)
  {
    // check how far we are zoomed-in
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    int closeup = dt_control_get_dev_closeup();
    const float min_scale = dt_dev_get_zoom_scale(self->dev, DT_ZOOM_FIT, 1<<closeup, 0);
    const float cur_scale = dt_dev_get_zoom_scale(self->dev, zoom, 1<<closeup, 0);

    // if we are zoomed in more than just a little bit, we try to use the canned grid of the preview pipeline
    if(cur_scale > 1.05f * min_scale)
    {
      if(!dt_dev_sync_pixelpipe_hash(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, &self->gui_lock, &g->hash))
        dt_control_log(_("Inconsistent output"));

      dt_iop_gui_enter_critical_section(self);
      can = g->can;
      dt_iop_gui_leave_critical_section(self);
    }
  }

  if(can)
  {
    b = dt_iop_colorreconstruct_bilateral_thaw(can);
  }
  else
  {
    b = dt_iop_colorreconstruct_bilateral_init(roi_in, piece->iscale, sigma_s, sigma_r);
    dt_iop_colorreconstruct_bilateral_splat(b, in, data->threshold, data->precedence, params);
    dt_iop_colorreconstruct_bilateral_blur(b);
  }

  if(!b) goto error;

  dt_iop_colorreconstruct_bilateral_slice(b, in, out, data->threshold, roi_in, piece->iscale);

  // here is where we generate the canned bilateral grid of the preview pipe for later use
  if(self->dev->gui_attached && g && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW)
  {
    uint64_t hash = dt_dev_hash_plus(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL);
    dt_iop_gui_enter_critical_section(self);
    dt_iop_colorreconstruct_bilateral_dump(g->can);
    g->can = dt_iop_colorreconstruct_bilateral_freeze(b);
    g->hash = hash;
    dt_iop_gui_leave_critical_section(self);
  }

  dt_iop_colorreconstruct_bilateral_free(b);
  return;

error:
  dt_control_log(_("Module `color reconstruction' failed"));
  dt_iop_colorreconstruct_bilateral_free(b);
  dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, piece->colors);
}

#ifdef HAVE_OPENCL
typedef struct dt_iop_colorreconstruct_bilateral_cl_t
{
  dt_iop_colorreconstruct_global_data_t *global;
  int devid;
  size_t size_x, size_y, size_z;
  int width, height, x, y;
  float scale;
  size_t blocksizex, blocksizey;
  float sigma_s, sigma_r;
  cl_mem dev_grid;
  cl_mem dev_grid_tmp;
} dt_iop_colorreconstruct_bilateral_cl_t;

static void dt_iop_colorreconstruct_bilateral_free_cl(dt_iop_colorreconstruct_bilateral_cl_t *b)
{
  if(!b) return;
  // be sure we're done with the memory:
  dt_opencl_finish(b->devid);
  // free device mem
  dt_opencl_release_mem_object(b->dev_grid);
  dt_opencl_release_mem_object(b->dev_grid_tmp);
  free(b);
}

static dt_iop_colorreconstruct_bilateral_cl_t *dt_iop_colorreconstruct_bilateral_init_cl(
                                        const int devid,
                                        dt_iop_colorreconstruct_global_data_t *global,
                                        const dt_iop_roi_t *roi, // dimensions of input image
                                        const float iscale,      // overall scale of input image
                                        const float sigma_s,     // spatial sigma (blur pixel coords)
                                        const float sigma_r)     // range sigma (blur luma values)
{
  int blocksizex, blocksizey;

  dt_opencl_local_buffer_t locopt
    = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                  .cellsize = 4 * sizeof(float) + sizeof(int), .overhead = 0,
                                  .sizex = 1 << 6, .sizey = 1 << 6 };

  if(dt_opencl_local_buffer_opt(devid, global->kernel_colorreconstruct_splat, &locopt))
  {
    blocksizex = locopt.sizex;
    blocksizey = locopt.sizey;
  }
  else
    blocksizex = blocksizey = 1;

  if(blocksizex * blocksizey < 16 * 16)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_colorreconstruction] device %d does not offer sufficient resources to run bilateral grid\n",
             devid);
    return NULL;
  }

  dt_iop_colorreconstruct_bilateral_cl_t *b = (dt_iop_colorreconstruct_bilateral_cl_t *)malloc(sizeof(dt_iop_colorreconstruct_bilateral_cl_t));
  if(!b)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_colorreconstruction] not able to allocate host buffer (a)\n");
    return NULL;
  }

  float _x = roundf(roi->width / sigma_s);
  float _y = roundf(roi->height / sigma_s);
  float _z = roundf(100.0f / sigma_r);
  b->size_x = CLAMPS((int)_x, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  b->size_y = CLAMPS((int)_y, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  b->size_z = CLAMPS((int)_z, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_R) + 1;
  b->width = roi->width;
  b->height = roi->height;
  b->x = roi->x;
  b->y = roi->y;
  b->scale = iscale / roi->scale;
  b->blocksizex = blocksizex;
  b->blocksizey = blocksizey;
  b->sigma_s = MAX(roi->height / (b->size_y - 1.0f), roi->width / (b->size_x - 1.0f));
  b->sigma_r = 100.0f / (b->size_z - 1.0f);
  b->devid = devid;
  b->global = global;
  b->dev_grid = NULL;
  b->dev_grid_tmp = NULL;

  // alloc grid buffer:
  b->dev_grid
      = dt_opencl_alloc_device_buffer(b->devid, sizeof(float) * 4 * b->size_x * b->size_y * b->size_z);
  if(!b->dev_grid)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_colorreconstruction] not able to allocate device buffer (b)\n");
    dt_iop_colorreconstruct_bilateral_free_cl(b);
    return NULL;
  }

  // alloc temporary grid buffer
  b->dev_grid_tmp
      = dt_opencl_alloc_device_buffer(b->devid, sizeof(float) * 4 * b->size_x * b->size_y * b->size_z);
  if(!b->dev_grid_tmp)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_colorreconstruction] not able to allocate device buffer (c)\n");
    dt_iop_colorreconstruct_bilateral_free_cl(b);
    return NULL;
  }

  // zero out grid
  int wd = 4 * b->size_x, ht = b->size_y * b->size_z;
  size_t sizes[] = { ROUNDUPDWD(wd, b->devid), ROUNDUPDHT(ht, b->devid), 1 };
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_zero, 0, sizeof(cl_mem), (void *)&b->dev_grid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_zero, 1, sizeof(int), (void *)&wd);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_zero, 2, sizeof(int), (void *)&ht);
  cl_int err = -666;
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_colorreconstruct_zero, sizes);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_colorreconstruction] error running kernel colorreconstruct_zero: %d\n", err);
    dt_iop_colorreconstruct_bilateral_free_cl(b);
    return NULL;
  }

#if 0
  fprintf(stderr, "[bilateral] created grid [%d %d %d]"
          " with sigma (%f %f) (%f %f)\n", b->size_x, b->size_y, b->size_z,
          b->sigma_s, sigma_s, b->sigma_r, sigma_r);
#endif
  return b;
}

static dt_iop_colorreconstruct_bilateral_frozen_t *dt_iop_colorreconstruct_bilateral_freeze_cl(dt_iop_colorreconstruct_bilateral_cl_t *b)
{
  if(!b) return NULL;

  dt_iop_colorreconstruct_bilateral_frozen_t *bf = (dt_iop_colorreconstruct_bilateral_frozen_t *)malloc(sizeof(dt_iop_colorreconstruct_bilateral_frozen_t));
  if(!bf)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_colorreconstruction] not able to allocate host buffer (d)\n");
    return NULL;
  }

  bf->size_x = b->size_x;
  bf->size_y = b->size_y;
  bf->size_z = b->size_z;
  bf->width = b->width;
  bf->height = b->height;
  bf->x = b->x;
  bf->y = b->y;
  bf->scale = b->scale;
  bf->sigma_s = b->sigma_s;
  bf->sigma_r = b->sigma_r;
  bf->buf = dt_alloc_align(64, sizeof(dt_iop_colorreconstruct_Lab_t) * b->size_x * b->size_y * b->size_z);
  if(bf->buf && b->dev_grid)
  {
    // read bilateral grid from device memory to host buffer (blocking)
    cl_int err = dt_opencl_read_buffer_from_device(b->devid, bf->buf, b->dev_grid, 0,
                                    sizeof(dt_iop_colorreconstruct_Lab_t) * b->size_x * b->size_y * b->size_z, CL_TRUE);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL,
           "[opencl_colorreconstruction] can not read bilateral grid from device %d\n", b->devid);
      dt_iop_colorreconstruct_bilateral_dump(bf);
      return NULL;
    }
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_colorreconstruction] not able to allocate host buffer (e)\n");
    dt_iop_colorreconstruct_bilateral_dump(bf);
    return NULL;
  }

  return bf;
}

static dt_iop_colorreconstruct_bilateral_cl_t *dt_iop_colorreconstruct_bilateral_thaw_cl(dt_iop_colorreconstruct_bilateral_frozen_t *bf,
                                                                                         const int devid,
                                                                                         dt_iop_colorreconstruct_global_data_t *global)
{
  if(!bf || !bf->buf) return NULL;

  int blocksizex, blocksizey;

  dt_opencl_local_buffer_t locopt
    = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                  .cellsize = 4 * sizeof(float) + sizeof(int), .overhead = 0,
                                  .sizex = 1 << 6, .sizey = 1 << 6 };

  if(dt_opencl_local_buffer_opt(devid, global->kernel_colorreconstruct_splat, &locopt))
  {
    blocksizex = locopt.sizex;
    blocksizey = locopt.sizey;
  }
  else
    blocksizex = blocksizey = 1;

  if(blocksizex * blocksizey < 16 * 16)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_colorreconstruction] device %d does not offer sufficient resources to run bilateral grid\n",
             devid);
    return NULL;
  }

  dt_iop_colorreconstruct_bilateral_cl_t *b = (dt_iop_colorreconstruct_bilateral_cl_t *)malloc(sizeof(dt_iop_colorreconstruct_bilateral_cl_t));
  if(!b)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_colorreconstruction] not able to allocate host buffer (f)\n");
    return NULL;
  }

  b->devid = devid;
  b->blocksizex = blocksizex;
  b->blocksizey = blocksizey;
  b->global = global;
  b->size_x = bf->size_x;
  b->size_y = bf->size_y;
  b->size_z = bf->size_z;
  b->width = bf->width;
  b->height = bf->height;
  b->x = bf->x;
  b->y = bf->y;
  b->scale = bf->scale;
  b->sigma_s = bf->sigma_s;
  b->sigma_r = bf->sigma_r;
  b->dev_grid = NULL;
  b->dev_grid_tmp = NULL;

  // alloc grid buffer:
  b->dev_grid
      = dt_opencl_alloc_device_buffer(b->devid, sizeof(float) * 4 * b->size_x * b->size_y * b->size_z);
  if(!b->dev_grid)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_colorreconstruction] not able to allocate device buffer (g)\n");
    dt_iop_colorreconstruct_bilateral_free_cl(b);
    return NULL;
  }

  // alloc temporary grid buffer
  b->dev_grid_tmp
      = dt_opencl_alloc_device_buffer(b->devid, sizeof(float) * 4 * b->size_x * b->size_y * b->size_z);
  if(!b->dev_grid_tmp)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_colorreconstruction] not able to allocate device buffer (h)\n");
    dt_iop_colorreconstruct_bilateral_free_cl(b);
    return NULL;
  }

  if(bf->buf)
  {
    // write bilateral grid from host buffer to device memory (blocking)
    cl_int err = dt_opencl_write_buffer_to_device(b->devid, bf->buf, b->dev_grid, 0,
                                    bf->size_x * bf->size_y * bf->size_z * sizeof(dt_iop_colorreconstruct_Lab_t), CL_TRUE);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL,
           "[opencl_colorreconstruction] can not write bilateral grid to device %d\n", b->devid);
      dt_iop_colorreconstruct_bilateral_free_cl(b);
      return NULL;
    }
  }

  return b;
}

static cl_int dt_iop_colorreconstruct_bilateral_splat_cl(dt_iop_colorreconstruct_bilateral_cl_t *b, cl_mem in, const float threshold,
                                                         dt_iop_colorreconstruct_precedence_t precedence, const float *params)
{
  cl_int err = -666;
  if(!b) return err;
  int pref = precedence;
  size_t sizes[] = { ROUNDUP(b->width, b->blocksizex), ROUNDUP(b->height, b->blocksizey), 1 };
  size_t local[] = { b->blocksizex, b->blocksizey, 1 };
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 0, sizeof(cl_mem), (void *)&in);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 1, sizeof(cl_mem), (void *)&b->dev_grid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 2, sizeof(int), (void *)&b->width);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 3, sizeof(int), (void *)&b->height);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 4, sizeof(int), (void *)&b->size_x);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 5, sizeof(int), (void *)&b->size_y);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 6, sizeof(int), (void *)&b->size_z);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 7, sizeof(float), (void *)&b->sigma_s);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 8, sizeof(float), (void *)&b->sigma_r);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 9, sizeof(float), (void *)&threshold);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 10, sizeof(int), (void *)&pref);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 11, 4*sizeof(float), (void *)params);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 12, b->blocksizex * b->blocksizey * sizeof(int),
                           NULL);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_splat, 13,
                           b->blocksizex * b->blocksizey * 4 * sizeof(float), NULL);
  err = dt_opencl_enqueue_kernel_2d_with_local(b->devid, b->global->kernel_colorreconstruct_splat, sizes, local);
  return err;
}

static cl_int dt_iop_colorreconstruct_bilateral_blur_cl(dt_iop_colorreconstruct_bilateral_cl_t *b)
{
  cl_int err = -666;
  if(!b) return err;
  size_t sizes[3] = { 0, 0, 1 };

  err = dt_opencl_enqueue_copy_buffer_to_buffer(b->devid, b->dev_grid, b->dev_grid_tmp, 0, 0,
                                                b->size_x * b->size_y * b->size_z * 4 * sizeof(float));
  if(err != CL_SUCCESS) return err;

  sizes[0] = ROUNDUPDWD(b->size_z, b->devid);
  sizes[1] = ROUNDUPDHT(b->size_y, b->devid);
  int stride1, stride2, stride3;
  stride1 = b->size_x * b->size_y;
  stride2 = b->size_x;
  stride3 = 1;
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 0, sizeof(cl_mem), (void *)&b->dev_grid_tmp);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 1, sizeof(cl_mem), (void *)&b->dev_grid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 2, sizeof(int), (void *)&stride1);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 3, sizeof(int), (void *)&stride2);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 4, sizeof(int), (void *)&stride3);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 5, sizeof(int), (void *)&b->size_z);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 6, sizeof(int), (void *)&b->size_y);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 7, sizeof(int), (void *)&b->size_x);
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_colorreconstruct_blur_line, sizes);
  if(err != CL_SUCCESS) return err;

  stride1 = b->size_x * b->size_y;
  stride2 = 1;
  stride3 = b->size_x;
  sizes[0] = ROUNDUPDWD(b->size_z, b->devid);
  sizes[1] = ROUNDUPDHT(b->size_x, b->devid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 0, sizeof(cl_mem), (void *)&b->dev_grid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 1, sizeof(cl_mem), (void *)&b->dev_grid_tmp);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 2, sizeof(int), (void *)&stride1);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 3, sizeof(int), (void *)&stride2);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 4, sizeof(int), (void *)&stride3);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 5, sizeof(int), (void *)&b->size_z);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 6, sizeof(int), (void *)&b->size_x);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 7, sizeof(int), (void *)&b->size_y);
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_colorreconstruct_blur_line, sizes);
  if(err != CL_SUCCESS) return err;

  stride1 = 1;
  stride2 = b->size_x;
  stride3 = b->size_x * b->size_y;
  sizes[0] = ROUNDUPDWD(b->size_x, b->devid);
  sizes[1] = ROUNDUPDHT(b->size_y, b->devid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 0, sizeof(cl_mem),
                           (void *)&b->dev_grid_tmp);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 1, sizeof(cl_mem), (void *)&b->dev_grid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 2, sizeof(int), (void *)&stride1);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 3, sizeof(int), (void *)&stride2);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 4, sizeof(int), (void *)&stride3);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 5, sizeof(int), (void *)&b->size_x);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 6, sizeof(int), (void *)&b->size_y);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_blur_line, 7, sizeof(int), (void *)&b->size_z);
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_colorreconstruct_blur_line, sizes);
  return err;
}

static cl_int dt_iop_colorreconstruct_bilateral_slice_cl(dt_iop_colorreconstruct_bilateral_cl_t *b, cl_mem in, cl_mem out,
                                                         const float threshold, const dt_iop_roi_t *roi, const float iscale)
{
  cl_int err = -666;
  if(!b) return err;
  const int bxy[2] = { b->x, b->y };
  const int roixy[2] = { roi->x, roi->y };
  const float rescale = iscale / (roi->scale * b->scale);

  size_t sizes[] = { ROUNDUPDWD(roi->width, b->devid), ROUNDUPDHT(roi->height, b->devid), 1 };
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 0, sizeof(cl_mem), (void *)&in);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 1, sizeof(cl_mem), (void *)&out);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 2, sizeof(cl_mem), (void *)&b->dev_grid);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 3, sizeof(int), (void *)&roi->width);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 4, sizeof(int), (void *)&roi->height);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 5, sizeof(int), (void *)&b->size_x);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 6, sizeof(int), (void *)&b->size_y);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 7, sizeof(int), (void *)&b->size_z);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 8, sizeof(float), (void *)&b->sigma_s);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 9, sizeof(float), (void *)&b->sigma_r);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 10, sizeof(float), (void *)&threshold);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 11, 2*sizeof(int), (void *)&bxy);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 12, 2*sizeof(int), (void *)&roixy);
  dt_opencl_set_kernel_arg(b->devid, b->global->kernel_colorreconstruct_slice, 13, sizeof(float), (void *)&rescale);
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_colorreconstruct_slice, sizes);
  return err;
}

int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorreconstruct_data_t *d = (dt_iop_colorreconstruct_data_t *)piece->data;
  dt_iop_colorreconstruct_global_data_t *gd = (dt_iop_colorreconstruct_global_data_t *)self->global_data;
  dt_iop_colorreconstruct_gui_data_t *g = (dt_iop_colorreconstruct_gui_data_t *)self->gui_data;

  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = fmax(d->range, 0.1f); // does not depend on scale
  const float sigma_s = fmax(d->spatial, 1.0f) / scale;
  const float hue = hue_conversion(d->hue); // convert to LCH hue which better fits to Lab colorspace

  const float params[4] = { hue, M_PI*M_PI/8, 0.0f, 0.0f };

  cl_int err = -666;

  dt_iop_colorreconstruct_bilateral_cl_t *b;
  dt_iop_colorreconstruct_bilateral_frozen_t *can = NULL;

  // see process() for more details on how we transfer a bilateral grid from the preview to the full pipeline
  if(sigma_s > DT_COLORRECONSTRUCT_SPATIAL_APPROX
     && self->dev->gui_attached
     && g
     && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL) == DT_DEV_PIXELPIPE_FULL)
  {
    // check how far we are zoomed-in
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    int closeup = dt_control_get_dev_closeup();
    const float min_scale = dt_dev_get_zoom_scale(self->dev, DT_ZOOM_FIT, 1<<closeup, 0);
    const float cur_scale = dt_dev_get_zoom_scale(self->dev, zoom, 1<<closeup, 0);

    // if we are zoomed in more than just a little bit, we try to use the canned grid of the preview pipeline
    if(cur_scale > 1.05f * min_scale)
    {
      if(!dt_dev_sync_pixelpipe_hash(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, &self->gui_lock, &g->hash))
        dt_control_log(_("Inconsistent output"));

      dt_iop_gui_enter_critical_section(self);
      can = g->can;
      dt_iop_gui_leave_critical_section(self);
    }
  }

  if(can)
  {
    b = dt_iop_colorreconstruct_bilateral_thaw_cl(can, piece->pipe->devid, gd);
    if(!b) goto error;
  }
  else
  {
    b = dt_iop_colorreconstruct_bilateral_init_cl(piece->pipe->devid, gd, roi_in, piece->iscale, sigma_s, sigma_r);
    if(!b) goto error;
    err = dt_iop_colorreconstruct_bilateral_splat_cl(b, dev_in, d->threshold, d->precedence, params);
    if(err != CL_SUCCESS) goto error;
    err = dt_iop_colorreconstruct_bilateral_blur_cl(b);
    if(err != CL_SUCCESS) goto error;
  }

  err = dt_iop_colorreconstruct_bilateral_slice_cl(b, dev_in, dev_out, d->threshold, roi_in, piece->iscale);
  if(err != CL_SUCCESS) goto error;

  if(self->dev->gui_attached && g && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW)
  {
    uint64_t hash = dt_dev_hash_plus(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL);
    dt_iop_gui_enter_critical_section(self);
    dt_iop_colorreconstruct_bilateral_dump(g->can);
    g->can = dt_iop_colorreconstruct_bilateral_freeze_cl(b);
    g->hash = hash;
    dt_iop_gui_leave_critical_section(self);
  }

  dt_iop_colorreconstruct_bilateral_free_cl(b);
  return TRUE;

error:
  dt_iop_colorreconstruct_bilateral_free_cl(b);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorreconstruction] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


static size_t dt_iop_colorreconstruct_bilateral_memory_use(const int width,     // width of input image
                                                           const int height,    // height of input image
                                                           const float sigma_s, // spatial sigma (blur pixel coords)
                                                           const float sigma_r) // range sigma (blur luma values)
{
  float _x = roundf(width / sigma_s);
  float _y = roundf(height / sigma_s);
  float _z = roundf(100.0f / sigma_r);
  size_t size_x = CLAMPS((int)_x, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  size_t size_y = CLAMPS((int)_y, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  size_t size_z = CLAMPS((int)_z, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_R) + 1;

  return size_x * size_y * size_z * 4 * sizeof(float) * 2;   // in fact only the OpenCL path needs a second tmp buffer
}


static size_t dt_iop_colorreconstruct_bilateral_singlebuffer_size(const int width,     // width of input image
                                                                  const int height,    // height of input image
                                                                  const float sigma_s, // spatial sigma (blur pixel coords)
                                                                  const float sigma_r) // range sigma (blur luma values)
{
  float _x = roundf(width / sigma_s);
  float _y = roundf(height / sigma_s);
  float _z = roundf(100.0f / sigma_r);
  size_t size_x = CLAMPS((int)_x, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  size_t size_y = CLAMPS((int)_y, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_S) + 1;
  size_t size_z = CLAMPS((int)_z, 4, DT_COLORRECONSTRUCT_BILATERAL_MAX_RES_R) + 1;

  return size_x * size_y * size_z * 4 * sizeof(float);
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_colorreconstruct_data_t *d = (dt_iop_colorreconstruct_data_t *)piece->data;
  // the total scale is composed of scale before input to the pipeline (iscale),
  // and the scale of the roi.
  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = fmax(d->range, 0.1f);
  const float sigma_s = fmax(d->spatial, 1.0f) / scale;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const size_t basebuffer = sizeof(float) * channels * width * height;

  tiling->factor = 2.0f + (float)dt_iop_colorreconstruct_bilateral_memory_use(width, height, sigma_s, sigma_r) / basebuffer;
  tiling->maxbuf
      = fmax(1.0f, (float)dt_iop_colorreconstruct_bilateral_singlebuffer_size(width, height, sigma_s, sigma_r) / basebuffer);
  tiling->overhead = 0;
  tiling->overlap = ceilf(4 * sigma_s);
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}


void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_colorreconstruct_params_t *p = (dt_iop_colorreconstruct_params_t *)self->params;
  dt_iop_colorreconstruct_gui_data_t *g = (dt_iop_colorreconstruct_gui_data_t *)self->gui_data;
  if(w == g->precedence)
  {
    gtk_widget_set_visible(g->hue, p->precedence == COLORRECONSTRUCT_PRECEDENCE_HUE);
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorreconstruct_params_t *p = (dt_iop_colorreconstruct_params_t *)p1;
  dt_iop_colorreconstruct_data_t *d = (dt_iop_colorreconstruct_data_t *)piece->data;

  d->threshold = p->threshold;
  d->spatial = p->spatial;
  d->range = p->range;
  d->precedence = p->precedence;
  d->hue = p->hue;

#ifdef HAVE_OPENCL
  piece->process_cl_ready = (piece->process_cl_ready && !dt_opencl_avoid_atomics(pipe->devid));
#endif
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorreconstruct_data_t *d = (dt_iop_colorreconstruct_data_t *)calloc(1, sizeof(dt_iop_colorreconstruct_data_t));
  piece->data = (void *)d;
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  const gboolean monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  dt_iop_colorreconstruct_gui_data_t *g = (dt_iop_colorreconstruct_gui_data_t *)self->gui_data;
  dt_iop_colorreconstruct_params_t *p = (dt_iop_colorreconstruct_params_t *)self->params;

  self->hide_enable_button = monochrome;
  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), !monochrome ? "default" : "monochrome");

  gtk_widget_set_visible(g->hue, p->precedence == COLORRECONSTRUCT_PRECEDENCE_HUE);

  dt_iop_gui_enter_critical_section(self);
  dt_iop_colorreconstruct_bilateral_dump(g->can);
  g->can = NULL;
  g->hash = 0;
  dt_iop_gui_leave_critical_section(self);
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_colorreconstruct_global_data_t *gd
      = (dt_iop_colorreconstruct_global_data_t *)malloc(sizeof(dt_iop_colorreconstruct_global_data_t));
  module->data = gd;
  const int program = 13; // colorcorrection.cl, from programs.conf
  gd->kernel_colorreconstruct_zero = dt_opencl_create_kernel(program, "colorreconstruction_zero");
  gd->kernel_colorreconstruct_splat = dt_opencl_create_kernel(program, "colorreconstruction_splat");
  gd->kernel_colorreconstruct_blur_line = dt_opencl_create_kernel(program, "colorreconstruction_blur_line");
  gd->kernel_colorreconstruct_slice = dt_opencl_create_kernel(program, "colorreconstruction_slice");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorreconstruct_global_data_t *gd = (dt_iop_colorreconstruct_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorreconstruct_zero);
  dt_opencl_free_kernel(gd->kernel_colorreconstruct_splat);
  dt_opencl_free_kernel(gd->kernel_colorreconstruct_blur_line);
  dt_opencl_free_kernel(gd->kernel_colorreconstruct_slice);
  free(module->data);
  module->data = NULL;
}


void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_colorreconstruct_gui_data_t *g = IOP_GUI_ALLOC(colorreconstruct);

  g->can = NULL;
  g->hash = 0;

  GtkWidget *box_enabled = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->threshold = dt_bauhaus_slider_from_params(self, N_("Threshold"));
  g->spatial = dt_bauhaus_slider_from_params(self, N_("Spatial"));
  g->range = dt_bauhaus_slider_from_params(self, N_("Range"));
  g->precedence = dt_bauhaus_combobox_from_params(self, N_("Precedence"));
  g->hue = dt_bauhaus_slider_from_params(self, N_("Hue"));
  dt_bauhaus_slider_set_factor(g->hue, 360.0f);
  dt_bauhaus_slider_set_format(g->hue, "°");
  dt_bauhaus_slider_set_feedback(g->hue, 0);
  dt_bauhaus_slider_set_stop(g->hue, 0.0f,   1.0f, 0.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.166f, 1.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.322f, 0.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.498f, 0.0f, 1.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.664f, 0.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.830f, 1.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->hue, 1.0f,   1.0f, 0.0f, 0.0f);

  gtk_widget_show_all(g->hue);
  gtk_widget_set_no_show_all(g->hue, TRUE);

  gtk_widget_set_tooltip_text(g->threshold, _("Pixels with lightness values above this threshold are corrected"));
  gtk_widget_set_tooltip_text(g->spatial, _("How far to look for replacement colors in spatial dimensions"));
  gtk_widget_set_tooltip_text(g->range, _("How far to look for replacement colors in the luminance dimension"));
  gtk_widget_set_tooltip_text(g->precedence, _("If and how to give precedence to specific replacement colors"));
  gtk_widget_set_tooltip_text(g->hue, _("The hue tone which should be given precedence over other hue tones"));

  GtkWidget *monochromes = dt_ui_label_new(_("Not applicable"));
  gtk_widget_set_tooltip_text(monochromes, _("No highlights reconstruction for monochrome images"));

  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);
  gtk_stack_add_named(GTK_STACK(self->widget), monochromes, "monochrome");
  gtk_stack_add_named(GTK_STACK(self->widget), box_enabled, "default");
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorreconstruct_gui_data_t *g = (dt_iop_colorreconstruct_gui_data_t *)self->gui_data;
  dt_iop_colorreconstruct_bilateral_dump(g->can);

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

