/*
    This file is part of darktable,
    Copyright (C) 2015-2020 darktable developers.

    (based on code by johannes hanika)

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
#include "common/imageio_rawspeed.h" // for dt_rawspeed_crop_dcraw_filters
#include "common/opencl.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "common/image_cache.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdint.h>
#include <stdlib.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

DT_MODULE_INTROSPECTION(1, dt_iop_rawprepare_params_t)

typedef struct dt_iop_rawprepare_params_t
{
  int32_t x; // $MIN: 0 $MAX: UINT16_MAX $DESCRIPTION: "crop x"
  int32_t y; // $MIN: 0 $MAX: UINT16_MAX $DESCRIPTION: "crop y"
  int32_t width; // $MIN: 0 $MAX: UINT16_MAX $DESCRIPTION: "crop width"
  int32_t height; // $MIN: 0 $MAX: UINT16_MAX $DESCRIPTION: "crop height"
  uint16_t raw_black_level_separate[4]; // $MIN: 0 $MAX: UINT16_MAX $DESCRIPTION: "black level"
  uint16_t raw_white_point; // $MIN: 0 $MAX: UINT16_MAX $DESCRIPTION: "white point"
} dt_iop_rawprepare_params_t;

typedef struct dt_iop_rawprepare_gui_data_t
{
  GtkWidget *black_level_separate[4];
  GtkWidget *white_point;
  GtkWidget *x, *y, *width, *height;
} dt_iop_rawprepare_gui_data_t;

typedef struct dt_iop_rawprepare_data_t
{
  int32_t x, y, width, height; // crop, now unused, for future expansion
  float sub[4];
  float div[4];

  // cached for dt_iop_buffer_dsc_t::rawprepare
  struct
  {
    uint16_t raw_black_level;
    uint16_t raw_white_point;
  } rawprepare;
} dt_iop_rawprepare_data_t;

typedef struct dt_iop_rawprepare_global_data_t
{
  int kernel_rawprepare_1f;
  int kernel_rawprepare_1f_unnormalized;
  int kernel_rawprepare_4f;
} dt_iop_rawprepare_global_data_t;


const char *name()
{
  return C_("modulename", "raw black/white point");
}

int operation_tags()
{
  return IOP_TAG_DISTORT;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE
    | IOP_FLAGS_UNSAFE_COPY;
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_RAW;
}

void init_presets(dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "BEGIN", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("passthrough"), self->op, self->version(),
                             &(dt_iop_rawprepare_params_t){.x = 0,
                                                           .y = 0,
                                                           .width = 0,
                                                           .height = 0,
                                                           .raw_black_level_separate[0] = 0,
                                                           .raw_black_level_separate[1] = 0,
                                                           .raw_black_level_separate[2] = 0,
                                                           .raw_black_level_separate[3] = 0,
                                                           .raw_white_point = UINT16_MAX },
                             sizeof(dt_iop_rawprepare_params_t), 1);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "COMMIT", NULL, NULL, NULL);
}

// value to round,   reference on how to round:
//  if ref was even, returned value will be even
//  if ref was odd,  returned value will be odd
static int round_smart(float val, int ref)
{
  // first, just round it
  int round = (int)roundf(val);

  if((ref & 1) ^ (round & 1)) round++;

  return round;
}

static int compute_proper_crop(dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *const roi_in, int value)
{
  const float scale = roi_in->scale / piece->iscale;

  return round_smart((float)value * scale, value);
}

int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count)
{
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  const float scale = piece->buf_in.scale / piece->iscale;

  const float x = (float)d->x * scale, y = (float)d->y * scale;

  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] -= x;
    points[i + 1] -= y;
  }

  return 1;
}

int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points,
                          size_t points_count)
{
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  const float scale = piece->buf_in.scale / piece->iscale;

  const float x = (float)d->x * scale, y = (float)d->y * scale;

  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] += x;
    points[i + 1] += y;
  }

  return 1;
}

void distort_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                  float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // TODO
  memset(out, 0, sizeof(float) * roi_out->width * roi_out->height);
  fprintf(stderr, "TODO: implement %s() in %s\n", __FUNCTION__, __FILE__);
}

// we're not scaling here (bayer input), so just crop borders
void modify_roi_out(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  roi_out->x = roi_out->y = 0;

  int32_t x = d->x + d->width, y = d->y + d->height;

  const float scale = roi_in->scale / piece->iscale;
  roi_out->width -= round_smart((float)x * scale, x);
  roi_out->height -= round_smart((float)y * scale, y);
}

void modify_roi_in(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  int32_t x = d->x + d->width, y = d->y + d->height;

  const float scale = roi_in->scale / piece->iscale;
  roi_in->width += round_smart((float)x * scale, x);
  roi_in->height += round_smart((float)y * scale, y);
}

void output_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                   dt_iop_buffer_dsc_t *dsc)
{
  default_output_format(self, pipe, piece, dsc);

  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  dsc->rawprepare.raw_black_level = d->rawprepare.raw_black_level;
  dsc->rawprepare.raw_white_point = d->rawprepare.raw_white_point;
}

static void adjust_xtrans_filters(dt_dev_pixelpipe_t *pipe,
                                  uint32_t crop_x, uint32_t crop_y)
{
  for(int i = 0; i < 6; ++i)
  {
    for(int j = 0; j < 6; ++j)
    {
      pipe->dsc.xtrans[j][i] = pipe->image.buf_dsc.xtrans[(j + crop_y) % 6][(i + crop_x) % 6];
    }
  }
}

static int BL(const dt_iop_roi_t *const roi_out, const dt_iop_rawprepare_data_t *const d, const int row,
              const int col)
{
  return ((((row + roi_out->y + d->y) & 1) << 1) + ((col + roi_out->x + d->x) & 1));
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_rawprepare_data_t *const d = (dt_iop_rawprepare_data_t *)piece->data;

  // fprintf(stderr, "roi in %d %d %d %d\n", roi_in->x, roi_in->y, roi_in->width, roi_in->height);
  // fprintf(stderr, "roi out %d %d %d %d\n", roi_out->x, roi_out->y, roi_out->width, roi_out->height);

  const int csx = compute_proper_crop(piece, roi_in, d->x), csy = compute_proper_crop(piece, roi_in, d->y);

  if(piece->pipe->dsc.filters && piece->dsc_in.channels == 1 && piece->dsc_in.datatype == TYPE_UINT16)
  { // raw mosaic

    const uint16_t *const in = (const uint16_t *const)ivoid;
    float *const out = (float *const)ovoid;

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(csx, csy, d, in, out, roi_in, roi_out) \
    schedule(static) \
    collapse(2)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      for(int i = 0; i < roi_out->width; i++)
      {
        const size_t pin = (size_t)(roi_in->width * (j + csy) + csx) + i;
        const size_t pout = (size_t)j * roi_out->width + i;

        const int id = BL(roi_out, d, j, i);
        out[pout] = (in[pin] - d->sub[id]) / d->div[id];
      }
    }

    piece->pipe->dsc.filters = dt_rawspeed_crop_dcraw_filters(self->dev->image_storage.buf_dsc.filters, csx, csy);
    adjust_xtrans_filters(piece->pipe, csx, csy);
  }
  else if(piece->pipe->dsc.filters && piece->dsc_in.channels == 1 && piece->dsc_in.datatype == TYPE_FLOAT)
  { // raw mosaic, fp, unnormalized

    const float *const in = (const float *const)ivoid;
    float *const out = (float *const)ovoid;

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(csx, csy, d, in, out, roi_in, roi_out) \
    schedule(static) \
    collapse(2)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      for(int i = 0; i < roi_out->width; i++)
      {
        const size_t pin = (size_t)(roi_in->width * (j + csy) + csx) + i;
        const size_t pout = (size_t)j * roi_out->width + i;

        const int id = BL(roi_out, d, j, i);
        out[pout] = (in[pin] - d->sub[id]) / d->div[id];
      }
    }

    piece->pipe->dsc.filters = dt_rawspeed_crop_dcraw_filters(self->dev->image_storage.buf_dsc.filters, csx, csy);
    adjust_xtrans_filters(piece->pipe, csx, csy);
  }
  else
  { // pre-downsampled buffer that needs black/white scaling

    const float *const in = (const float *const)ivoid;
    float *const out = (float *const)ovoid;

    const float sub = d->sub[0], div = d->div[0];

    const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
    dt_omp_firstprivate(ch, csx, csy, div, in, out, roi_in, roi_out, sub) \
    schedule(static) collapse(3)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      for(int i = 0; i < roi_out->width; i++)
      {
        for(int c = 0; c < ch; c++)
        {
          const size_t pin = (size_t)ch * (roi_in->width * (j + csy) + csx + i) + c;
          const size_t pout = (size_t)ch * (j * roi_out->width + i) + c;

          out[pout] = (in[pin] - sub) / div;
        }
      }
    }
  }

  for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = 1.0f;
}

#if defined(__SSE2__)
void process_sse2(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                  void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_rawprepare_data_t *const d = (dt_iop_rawprepare_data_t *)piece->data;

  // fprintf(stderr, "roi in %d %d %d %d\n", roi_in->x, roi_in->y, roi_in->width, roi_in->height);
  // fprintf(stderr, "roi out %d %d %d %d\n", roi_out->x, roi_out->y, roi_out->width, roi_out->height);

  const int csx = compute_proper_crop(piece, roi_in, d->x), csy = compute_proper_crop(piece, roi_in, d->y);

  if(piece->pipe->dsc.filters && piece->dsc_in.channels == 1 && piece->dsc_in.datatype == TYPE_UINT16)
  { // raw mosaic
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(csx, csy, d, ivoid, ovoid, roi_in, roi_out) \
    schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const uint16_t *in = ((uint16_t *)ivoid) + ((size_t)roi_in->width * (j + csy) + csx);
      float *out = ((float *)ovoid) + (size_t)roi_out->width * j;

      int i = 0;

      // FIXME: figure alignment!  !!! replace with for !!!
      while((!dt_is_aligned(in, 16) || !dt_is_aligned(out, 16)) && (i < roi_out->width))
      {
        const int id = BL(roi_out, d, j, i);
        *out = (((float)(*in)) - d->sub[id]) / d->div[id];
        i++;
        in++;
        out++;
      }

      const __m128 sub = _mm_set_ps(d->sub[BL(roi_out, d, j, i + 3)], d->sub[BL(roi_out, d, j, i + 2)],
                                    d->sub[BL(roi_out, d, j, i + 1)], d->sub[BL(roi_out, d, j, i)]);

      const __m128 div = _mm_set_ps(d->div[BL(roi_out, d, j, i + 3)], d->div[BL(roi_out, d, j, i + 2)],
                                    d->div[BL(roi_out, d, j, i + 1)], d->div[BL(roi_out, d, j, i)]);

      // process aligned pixels with SSE
      for(; i < roi_out->width - (8 - 1); i += 8, in += 8)
      {
        const __m128i input = _mm_load_si128((__m128i *)in);

        __m128i ilo = _mm_unpacklo_epi16(input, _mm_set1_epi16(0));
        __m128i ihi = _mm_unpackhi_epi16(input, _mm_set1_epi16(0));

        __m128 flo = _mm_cvtepi32_ps(ilo);
        __m128 fhi = _mm_cvtepi32_ps(ihi);

        flo = _mm_div_ps(_mm_sub_ps(flo, sub), div);
        fhi = _mm_div_ps(_mm_sub_ps(fhi, sub), div);

        _mm_stream_ps(out, flo);
        out += 4;
        _mm_stream_ps(out, fhi);
        out += 4;
      }

      // process the rest
      for(; i < roi_out->width; i++, in++, out++)
      {
        const int id = BL(roi_out, d, j, i);
        *out = (((float)(*in)) - d->sub[id]) / d->div[id];
      }
    }

    piece->pipe->dsc.filters = dt_rawspeed_crop_dcraw_filters(self->dev->image_storage.buf_dsc.filters, csx, csy);
    adjust_xtrans_filters(piece->pipe, csx, csy);
  }
  else if(piece->pipe->dsc.filters && piece->dsc_in.channels == 1 && piece->dsc_in.datatype == TYPE_FLOAT)
  { // raw mosaic, fp, unnormalized
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(d, csx, csy, ivoid, ovoid, roi_in, roi_out) \
    schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + ((size_t)roi_in->width * (j + csy) + csx);
      float *out = ((float *)ovoid) + (size_t)roi_out->width * j;

      int i = 0;

      // FIXME: figure alignment!  !!! replace with for !!!
      while((!dt_is_aligned(in, 16) || !dt_is_aligned(out, 16)) && (i < roi_out->width))
      {
        const int id = BL(roi_out, d, j, i);
        *out = (*in - d->sub[id]) / d->div[id];
        i++;
        in++;
        out++;
      }

      const __m128 sub = _mm_set_ps(d->sub[BL(roi_out, d, j, i + 3)], d->sub[BL(roi_out, d, j, i + 2)],
                                    d->sub[BL(roi_out, d, j, i + 1)], d->sub[BL(roi_out, d, j, i)]);

      const __m128 div = _mm_set_ps(d->div[BL(roi_out, d, j, i + 3)], d->div[BL(roi_out, d, j, i + 2)],
                                    d->div[BL(roi_out, d, j, i + 1)], d->div[BL(roi_out, d, j, i)]);

      // process aligned pixels with SSE
      for(; i < roi_out->width - (4 - 1); i += 4, in += 4, out += 4)
      {
        const __m128 input = _mm_load_ps(in);

        const __m128 scaled = _mm_div_ps(_mm_sub_ps(input, sub), div);

        _mm_stream_ps(out, scaled);
      }

      // process the rest
      for(; i < roi_out->width; i++, in++, out++)
      {
        const int id = BL(roi_out, d, j, i);
        *out = (*in - d->sub[id]) / d->div[id];
      }
    }

    piece->pipe->dsc.filters = dt_rawspeed_crop_dcraw_filters(self->dev->image_storage.buf_dsc.filters, csx, csy);
    adjust_xtrans_filters(piece->pipe, csx, csy);
  }
  else
  { // pre-downsampled buffer that needs black/white scaling

    const __m128 sub = _mm_load_ps(d->sub), div = _mm_load_ps(d->div);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(csx, csy, div, ivoid, ovoid, roi_in, roi_out, sub) \
    schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + (size_t)4 * (roi_in->width * (j + csy) + csx);
      float *out = ((float *)ovoid) + (size_t)4 * roi_out->width * j;

      // process aligned pixels with SSE
      for(int i = 0; i < roi_out->width; i++, in += 4, out += 4)
      {
        const __m128 input = _mm_load_ps(in);

        const __m128 scaled = _mm_div_ps(_mm_sub_ps(input, sub), div);

        _mm_stream_ps(out, scaled);
      }
    }
  }

  for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = 1.0f;

  _mm_sfence();
}
#endif

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;
  dt_iop_rawprepare_global_data_t *gd = (dt_iop_rawprepare_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  cl_mem dev_sub = NULL;
  cl_mem dev_div = NULL;
  cl_int err = -999;

  int kernel = -1;

  if(piece->pipe->dsc.filters && piece->dsc_in.channels == 1 && piece->dsc_in.datatype == TYPE_UINT16)
  {
    kernel = gd->kernel_rawprepare_1f;
  }
  else if(piece->pipe->dsc.filters && piece->dsc_in.channels == 1 && piece->dsc_in.datatype == TYPE_FLOAT)
  {
    kernel = gd->kernel_rawprepare_1f_unnormalized;
  }
  else
  {
    kernel = gd->kernel_rawprepare_4f;
  }

  const int csx = compute_proper_crop(piece, roi_in, d->x), csy = compute_proper_crop(piece, roi_in, d->y);

  dev_sub = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 4, d->sub);
  if(dev_sub == NULL) goto error;

  dev_div = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 4, d->div);
  if(dev_div == NULL) goto error;

  const int width = roi_out->width;
  const int height = roi_out->height;

  size_t sizes[] = { ROUNDUPWD(roi_in->width), ROUNDUPHT(roi_in->height), 1 };
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&(width));
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&(height));
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), (void *)&csx);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), (void *)&csy);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), (void *)&dev_sub);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), (void *)&dev_div);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(uint32_t), (void *)&roi_out->x);
  dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(uint32_t), (void *)&roi_out->y);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_sub);
  dt_opencl_release_mem_object(dev_div);

  if(piece->pipe->dsc.filters)
  {
    piece->pipe->dsc.filters = dt_rawspeed_crop_dcraw_filters(self->dev->image_storage.buf_dsc.filters, csx, csy);
    adjust_xtrans_filters(piece->pipe, csx, csy);
  }

  for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = 1.0f;

  return TRUE;

error:
  dt_opencl_release_mem_object(dev_sub);
  dt_opencl_release_mem_object(dev_div);
  dt_print(DT_DEBUG_OPENCL, "[opencl_rawprepare] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

static int image_is_normalized(const dt_image_t *const image)
{
  // if raw with floating-point data, if not special magic whitelevel, then it needs normalization
  if((image->flags & DT_IMAGE_HDR) == DT_IMAGE_HDR)
  {
    union {
        float f;
        uint32_t u;
    } normalized;
    normalized.f = 1.0f;

    // dng spec is just broken here.
    return image->raw_white_point == normalized.u;
  }

  // else, assume normalized
  return image->buf_dsc.channels == 1 && image->buf_dsc.datatype == TYPE_FLOAT;
}

static gboolean image_set_rawcrops(const uint32_t imgid, int dx, int dy)
{
  dt_image_t *img = NULL;
  img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  const gboolean test = (img->p_width == img->width - dx) && (img->p_height == img->height - dy);
  dt_image_cache_read_release(darktable.image_cache, img);
  if(test) return FALSE;

  img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  img->p_width = img->width - dx;
  img->p_height = img->height - dy;
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
  return TRUE;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_rawprepare_params_t *const p = (dt_iop_rawprepare_params_t *)params;
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  d->x = p->x;
  d->y = p->y;
  d->width = p->width;
  d->height = p->height;

  if(piece->pipe->dsc.filters)
  {
    const float white = (float)p->raw_white_point;

    for(int i = 0; i < 4; i++)
    {
      d->sub[i] = (float)p->raw_black_level_separate[i];
      d->div[i] = (white - d->sub[i]);
    }
  }
  else
  {
    const float normalizer
        = ((piece->pipe->image.flags & DT_IMAGE_HDR) == DT_IMAGE_HDR) ? 1.0f : (float)UINT16_MAX;
    const float white = (float)p->raw_white_point / normalizer;
    float black = 0;
    for(int i = 0; i < 4; i++)
    {
      black += p->raw_black_level_separate[i] / normalizer;
    }
    black /= 4.0f;

    for(int i = 0; i < 4; i++)
    {
      d->sub[i] = black;
      d->div[i] = (white - black);
    }
  }

  float black = 0.0f;
  for(uint8_t i = 0; i < 4; i++)
  {
    black += (float)p->raw_black_level_separate[i];
  }
  d->rawprepare.raw_black_level = (uint16_t)(black / 4.0f);
  d->rawprepare.raw_white_point = p->raw_white_point;

  if(image_set_rawcrops(pipe->image.id, d->x + d->width, d->y + d->height))
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_METADATA_UPDATE);

  if(!(dt_image_is_rawprepare_supported(&piece->pipe->image)) || image_is_normalized(&piece->pipe->image)) piece->enabled = 0;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_rawprepare_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_rawprepare_params_t *d = self->default_params;
  const dt_image_t *const image = &(self->dev->image_storage);

  *d = (dt_iop_rawprepare_params_t){.x = image->crop_x,
                                    .y = image->crop_y,
                                    .width = image->crop_width,
                                    .height = image->crop_height,
                                    .raw_black_level_separate[0] = image->raw_black_level_separate[0],
                                    .raw_black_level_separate[1] = image->raw_black_level_separate[1],
                                    .raw_black_level_separate[2] = image->raw_black_level_separate[2],
                                    .raw_black_level_separate[3] = image->raw_black_level_separate[3],
                                    .raw_white_point = image->raw_white_point };

  self->hide_enable_button = 1;
  self->default_enabled = dt_image_is_rawprepare_supported(image) && !image_is_normalized(image);

  if(self->widget)
    gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "raw" : "non_raw");
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl, from programs.conf
  self->data = malloc(sizeof(dt_iop_rawprepare_global_data_t));

  dt_iop_rawprepare_global_data_t *gd = self->data;
  gd->kernel_rawprepare_1f = dt_opencl_create_kernel(program, "rawprepare_1f");
  gd->kernel_rawprepare_1f_unnormalized = dt_opencl_create_kernel(program, "rawprepare_1f_unnormalized");
  gd->kernel_rawprepare_4f = dt_opencl_create_kernel(program, "rawprepare_4f");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_rawprepare_global_data_t *gd = (dt_iop_rawprepare_global_data_t *)self->data;
  dt_opencl_free_kernel(gd->kernel_rawprepare_4f);
  dt_opencl_free_kernel(gd->kernel_rawprepare_1f_unnormalized);
  dt_opencl_free_kernel(gd->kernel_rawprepare_1f);
  free(self->data);
  self->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_rawprepare_gui_data_t *g = (dt_iop_rawprepare_gui_data_t *)self->gui_data;
  dt_iop_rawprepare_params_t *p = (dt_iop_rawprepare_params_t *)self->params;

  for(int i = 0; i < 4; i++)
  {
    dt_bauhaus_slider_set_soft(g->black_level_separate[i], p->raw_black_level_separate[i]);
  }

  dt_bauhaus_slider_set_soft(g->white_point, p->raw_white_point);

  if(dt_conf_get_bool("plugins/darkroom/rawprepare/allow_editing_crop"))
  {
    dt_bauhaus_slider_set_soft(g->x, p->x);
    dt_bauhaus_slider_set_soft(g->y, p->y);
    dt_bauhaus_slider_set_soft(g->width, p->width);
    dt_bauhaus_slider_set_soft(g->height, p->height);
  }
}

const gchar *black_label[]
  =  { N_("black level 0"),
       N_("black level 1"),
       N_("black level 2"),
       N_("black level 3") };

void gui_init(dt_iop_module_t *self)
{
  dt_iop_rawprepare_gui_data_t *g = IOP_GUI_ALLOC(rawprepare);

  GtkWidget *box_raw = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  for(int i = 0; i < 4; i++)
  {
    gchar *par = g_strdup_printf("raw_black_level_separate[%i]", i);

    g->black_level_separate[i] = dt_bauhaus_slider_from_params(self, par);
    dt_bauhaus_widget_set_label(g->black_level_separate[i], NULL, black_label[i]);
    gtk_widget_set_tooltip_text(g->black_level_separate[i], _(black_label[i]));
    dt_bauhaus_slider_set_soft_max(g->black_level_separate[i], 16384);

    g_free(par);
  }

  g->white_point = dt_bauhaus_slider_from_params(self, "raw_white_point");
  gtk_widget_set_tooltip_text(g->white_point, _("white point"));
  dt_bauhaus_slider_set_soft_max(g->white_point, 16384);

  if(dt_conf_get_bool("plugins/darkroom/rawprepare/allow_editing_crop"))
  {
    g->x = dt_bauhaus_slider_from_params(self, "x");
    gtk_widget_set_tooltip_text(g->x, _("crop from left border"));
    dt_bauhaus_slider_set_soft_max(g->x, 256);

    g->y = dt_bauhaus_slider_from_params(self, "y");
    gtk_widget_set_tooltip_text(g->y, _("crop from top"));
    dt_bauhaus_slider_set_soft_max(g->y, 256);

    g->width = dt_bauhaus_slider_from_params(self, "width");
    gtk_widget_set_tooltip_text(g->width, _("crop from right border"));
    dt_bauhaus_slider_set_soft_max(g->width, 256);

    g->height = dt_bauhaus_slider_from_params(self, "height");
    gtk_widget_set_tooltip_text(g->height, _("crop from bottom"));
    dt_bauhaus_slider_set_soft_max(g->height, 256);
  }

  // start building top level widget
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);

  GtkWidget *label_non_raw = dt_ui_label_new(_("raw black/white point correction\nonly works for the sensors that need it."));

  gtk_stack_add_named(GTK_STACK(self->widget), label_non_raw, "non_raw");
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "raw");
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
