/*
    This file is part of darktable,
    Copyright (C) 2015-2023 darktable developers.

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
#include "common/opencl.h"
#include "common/imagebuf.h"
#include "common/image_cache.h"
#include "common/dng_opcode.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "imageio/imageio_rawspeed.h" // for dt_rawspeed_crop_dcraw_filters
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdint.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(2, dt_iop_rawprepare_params_t)

typedef enum dt_iop_rawprepare_flat_field_t
{
  FLAT_FIELD_OFF = 0,     // $DESCRIPTION: "disabled"
  FLAT_FIELD_EMBEDDED = 1 // $DESCRIPTION: "embedded GainMap"
} dt_iop_rawprepare_flat_field_t;

typedef struct dt_iop_rawprepare_params_t
{
  int32_t left; // $MIN: 0 $MAX: UINT16_MAX $DESCRIPTION: "crop left"
  int32_t top; // $MIN: 0 $MAX: UINT16_MAX $DESCRIPTION: "crop top"
  int32_t right; // $MIN: 0 $MAX: UINT16_MAX $DESCRIPTION: "crop right"
  int32_t bottom; // $MIN: 0 $MAX: UINT16_MAX $DESCRIPTION: "crop bottom"
  uint16_t raw_black_level_separate[4]; // $MIN: 0 $MAX: UINT16_MAX $DESCRIPTION: "black level"
  uint16_t raw_white_point; // $MIN: 0 $MAX: UINT16_MAX $DESCRIPTION: "white point"
  dt_iop_rawprepare_flat_field_t flat_field; // $DEFAULT: FLAT_FIELD_OFF $DESCRIPTION: "flat field correction"
} dt_iop_rawprepare_params_t;

typedef struct dt_iop_rawprepare_gui_data_t
{
  GtkWidget *black_level_separate[4];
  GtkWidget *white_point;
  GtkWidget *left, *top, *right, *bottom;
  GtkWidget *flat_field;
} dt_iop_rawprepare_gui_data_t;

typedef struct dt_iop_rawprepare_data_t
{
  int32_t left, top, right, bottom;
  float sub[4];
  float div[4];

  // cached for dt_iop_buffer_dsc_t::rawprepare
  struct
  {
    uint16_t raw_black_level;
    uint16_t raw_white_point;
  } rawprepare;

  // image contains GainMaps that should be applied
  gboolean apply_gainmaps;
  // GainMap for each filter of RGGB Bayer pattern
  dt_dng_gain_map_t *gainmaps[4];
} dt_iop_rawprepare_data_t;

typedef struct dt_iop_rawprepare_global_data_t
{
  int kernel_rawprepare_1f;
  int kernel_rawprepare_1f_gainmap;
  int kernel_rawprepare_1f_unnormalized;
  int kernel_rawprepare_1f_unnormalized_gainmap;
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

int default_colorspace(dt_iop_module_t *self,
                       dt_dev_pixelpipe_t *pipe,
                       dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void *new_params,
                  const int new_version)
{
  typedef struct dt_iop_rawprepare_params_t dt_iop_rawprepare_params_v2_t;
  typedef struct dt_iop_rawprepare_params_v1_t
  {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
    uint16_t raw_black_level_separate[4];
    uint16_t raw_white_point;
  } dt_iop_rawprepare_params_v1_t;

  if(old_version == 1 && new_version == 2)
  {
    dt_iop_rawprepare_params_v1_t *o = (dt_iop_rawprepare_params_v1_t *)old_params;
    dt_iop_rawprepare_params_v2_t *n = (dt_iop_rawprepare_params_v2_t *)new_params;
    memcpy(n, o, sizeof *o);
    n->flat_field = FLAT_FIELD_OFF;
    return 0;
  }

  return 1;
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("sets technical specificities of the raw sensor.\n"
                                        "touch with great care!"),
                                      _("mandatory"),
                                      _("linear, raw, scene-referred"),
                                      _("linear, raw"),
                                      _("linear, raw, scene-referred"));
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_database_start_transaction(darktable.db);

  dt_gui_presets_add_generic(_("passthrough"), self->op, self->version(),
                             &(dt_iop_rawprepare_params_t){.left = 0,
                                                           .top = 0,
                                                           .right = 0,
                                                           .bottom = 0,
                                                           .raw_black_level_separate[0] = 0,
                                                           .raw_black_level_separate[1] = 0,
                                                           .raw_black_level_separate[2] = 0,
                                                           .raw_black_level_separate[3] = 0,
                                                           .raw_white_point = UINT16_MAX },
                             sizeof(dt_iop_rawprepare_params_t), 1, DEVELOP_BLEND_CS_NONE);

  dt_database_release_transaction(darktable.db);
}

static int _compute_proper_crop(dt_dev_pixelpipe_iop_t *piece,
                                const dt_iop_roi_t *const roi_in,
                                const int value)
{
  const float scale = roi_in->scale / piece->iscale;
  return (int)roundf((float)value * scale);
}

int distort_transform(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        float *const restrict points,
        size_t points_count)
{
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  // nothing to be done if parameters are set to neutral values (no top/left crop)
  if(d->left == 0 && d->top == 0) return 1;

  const float scale = piece->buf_in.scale / piece->iscale;

  const float x = (float)d->left * scale;
  const float y = (float)d->top * scale;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(points_count, points, y, x) \
    schedule(static) \
    aligned(points:64) if(points_count > 100)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] -= x;
    points[i + 1] -= y;
  }

  return 1;
}

int distort_backtransform(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        float *const restrict points,
        size_t points_count)
{
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  // nothing to be done if parameters are set to neutral values (no top/left crop)
  if(d->left == 0 && d->top == 0) return 1;

  const float scale = piece->buf_in.scale / piece->iscale;

  const float x = (float)d->left * scale;
  const float y = (float)d->top * scale;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(points_count, points, y, x) \
    schedule(static) \
    aligned(points:64) if(points_count > 100)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] += x;
    points[i + 1] += y;
  }

  return 1;
}

void distort_mask(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        const float *const in,
        float *const out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  dt_iop_copy_image_roi(out, in, 1, roi_in, roi_out, TRUE);
}

// we're not scaling here (bayer input), so just crop borders
void modify_roi_out(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        dt_iop_roi_t *roi_out,
        const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  roi_out->x = roi_out->y = 0;

  const int32_t x = d->left + d->right;
  const int32_t y = d->top + d->bottom;

  const float scale = roi_in->scale / piece->iscale;
  roi_out->width -= (int)roundf((float)x * scale);
  roi_out->height -= (int)roundf((float)y * scale);
}

void modify_roi_in(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        const dt_iop_roi_t *const roi_out,
        dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  const int32_t x = d->left + d->right;
  const int32_t y = d->top + d->bottom;

  const float scale = roi_in->scale / piece->iscale;
  roi_in->width += (int)roundf((float)x * scale);
  roi_in->height += (int)roundf((float)y * scale);
}

void output_format(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_t *pipe,
        dt_dev_pixelpipe_iop_t *piece,
        dt_iop_buffer_dsc_t *dsc)
{
  default_output_format(self, pipe, piece, dsc);

  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  dsc->rawprepare.raw_black_level = d->rawprepare.raw_black_level;
  dsc->rawprepare.raw_white_point = d->rawprepare.raw_white_point;
}

static void _adjust_xtrans_filters(
        dt_dev_pixelpipe_t *pipe,
        const uint32_t crop_x,
        const uint32_t crop_y)
{
  for(int i = 0; i < 6; ++i)
  {
    for(int j = 0; j < 6; ++j)
    {
      pipe->dsc.xtrans[j][i] = pipe->image.buf_dsc.xtrans[(j + crop_y) % 6][(i + crop_x) % 6];
    }
  }
}

static int _BL(const dt_iop_roi_t *const roi_out,
               const dt_iop_rawprepare_data_t *const d,
               const int row,
               const int col)
{
  return ((((row + roi_out->y + d->top) & 1) << 1) + ((col + roi_out->x + d->left) & 1));
}

void process(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        const void *const ivoid,
        void *const ovoid,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  const dt_iop_rawprepare_data_t *const d = (dt_iop_rawprepare_data_t *)piece->data;

  const int csx = _compute_proper_crop(piece, roi_in, d->left);
  const int csy = _compute_proper_crop(piece, roi_in, d->top);

  if(piece->pipe->dsc.filters && piece->dsc_in.channels == 1
     && piece->dsc_in.datatype == TYPE_UINT16)
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

        const int id = _BL(roi_out, d, j, i);
        out[pout] = (in[pin] - d->sub[id]) / d->div[id];
      }
    }

    piece->pipe->dsc.filters =
      dt_rawspeed_crop_dcraw_filters(self->dev->image_storage.buf_dsc.filters, csx, csy);
    _adjust_xtrans_filters(piece->pipe, csx, csy);
  }
  else if(piece->pipe->dsc.filters && piece->dsc_in.channels == 1
          && piece->dsc_in.datatype == TYPE_FLOAT)
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

        const int id = _BL(roi_out, d, j, i);
        out[pout] = (in[pin] - d->sub[id]) / d->div[id];
      }
    }

    piece->pipe->dsc.filters =
      dt_rawspeed_crop_dcraw_filters(self->dev->image_storage.buf_dsc.filters, csx, csy);
    _adjust_xtrans_filters(piece->pipe, csx, csy);
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

  if(piece->pipe->dsc.filters && piece->dsc_in.channels == 1 && d->apply_gainmaps)
  {
    const uint32_t map_w = d->gainmaps[0]->map_points_h;
    const uint32_t map_h = d->gainmaps[0]->map_points_v;
    const float im_to_rel_x = 1.0f / piece->buf_in.width;
    const float im_to_rel_y = 1.0f / piece->buf_in.height;
    const float rel_to_map_x = 1.0f / d->gainmaps[0]->map_spacing_h;
    const float rel_to_map_y = 1.0f / d->gainmaps[0]->map_spacing_v;
    const float map_origin_h = d->gainmaps[0]->map_origin_h;
    const float map_origin_v = d->gainmaps[0]->map_origin_v;
    float *const out = (float *const)ovoid;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(csx, csy, roi_out, out, im_to_rel_x, im_to_rel_y, rel_to_map_x, rel_to_map_y, \
                        map_w, map_h, map_origin_h, map_origin_v) \
    dt_omp_sharedconst(d) schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const float y_map = CLAMP(((roi_out->y + csy + j) * im_to_rel_y - map_origin_v) * rel_to_map_y, 0, map_h);
      const uint32_t y_i0 = MIN(y_map, map_h - 1);
      const uint32_t y_i1 = MIN(y_i0 + 1, map_h - 1);
      const float y_frac = y_map - y_i0;
      const float * restrict map_row0[4];
      const float * restrict map_row1[4];
      for(int f = 0; f < 4; f++)
      {
        map_row0[f] = &d->gainmaps[f]->map_gain[y_i0 * map_w];
        map_row1[f] = &d->gainmaps[f]->map_gain[y_i1 * map_w];
      }
      for(int i = 0; i < roi_out->width; i++)
      {
        const int id = _BL(roi_out, d, j, i);
        const float x_map = CLAMP(((roi_out->x + csx + i) * im_to_rel_x - map_origin_h) * rel_to_map_x, 0, map_w);
        const uint32_t x_i0 = MIN(x_map, map_w - 1);
        const uint32_t x_i1 = MIN(x_i0 + 1, map_w - 1);
        const float x_frac = x_map - x_i0;
        const float gain_top = (1.0f - x_frac) * map_row0[id][x_i0] + x_frac * map_row0[id][x_i1];
        const float gain_bottom = (1.0f - x_frac) * map_row1[id][x_i0] + x_frac * map_row1[id][x_i1];
        out[j * roi_out->width + i] *= (1.0f - y_frac) * gain_top + y_frac * gain_bottom;
      }
    }
  }

  dt_dev_write_rawdetail_mask(piece, (float *const)ovoid, roi_in, DT_DEV_DETAIL_MASK_RAWPREPARE);

  for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = 1.0f;
}

#ifdef HAVE_OPENCL
int process_cl(
        dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;
  dt_iop_rawprepare_global_data_t *gd = (dt_iop_rawprepare_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  cl_mem dev_sub = NULL;
  cl_mem dev_div = NULL;
  cl_mem dev_gainmap[4] = {0};
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  int kernel = -1;
  gboolean gainmap_args = FALSE;

  if(piece->pipe->dsc.filters
     && piece->dsc_in.channels == 1
     && piece->dsc_in.datatype == TYPE_UINT16)
  {
    if(d->apply_gainmaps)
    {
      kernel = gd->kernel_rawprepare_1f_gainmap;
      gainmap_args = TRUE;
    }
    else
    {
      kernel = gd->kernel_rawprepare_1f;
    }
  }
  else if(piece->pipe->dsc.filters
          && piece->dsc_in.channels == 1
          && piece->dsc_in.datatype == TYPE_FLOAT)
  {
    if(d->apply_gainmaps)
    {
      kernel = gd->kernel_rawprepare_1f_unnormalized_gainmap;
      gainmap_args = TRUE;
    }
    else
    {
      kernel = gd->kernel_rawprepare_1f_unnormalized;
    }
  }
  else
  {
    kernel = gd->kernel_rawprepare_4f;
  }

  const int csx = _compute_proper_crop(piece, roi_in, d->left);
  const int csy = _compute_proper_crop(piece, roi_in, d->top);

  dev_sub = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 4, d->sub);
  if(dev_sub == NULL) goto error;

  dev_div = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 4, d->div);
  if(dev_div == NULL) goto error;

  const int width = roi_out->width;
  const int height = roi_out->height;

  size_t sizes[] = { ROUNDUPDWD(roi_in->width, devid), ROUNDUPDHT(roi_in->height, devid), 1 };
  dt_opencl_set_kernel_args(devid, kernel, 0, CLARG(dev_in), CLARG(dev_out), CLARG((width)), CLARG((height)),
    CLARG(csx), CLARG(csy), CLARG(dev_sub), CLARG(dev_div), CLARG(roi_out->x), CLARG(roi_out->y));
  if(gainmap_args)
  {
    const int map_size[2] = { d->gainmaps[0]->map_points_h, d->gainmaps[0]->map_points_v };
    const float im_to_rel[2] = { 1.0f / piece->buf_in.width, 1.0f / piece->buf_in.height };
    const float rel_to_map[2] = { 1.0f / d->gainmaps[0]->map_spacing_h, 1.0f / d->gainmaps[0]->map_spacing_v };
    const float map_origin[2] = { d->gainmaps[0]->map_origin_h, d->gainmaps[0]->map_origin_v };

    for(int i = 0; i < 4; i++)
    {
      dev_gainmap[i] = dt_opencl_alloc_device(devid, map_size[0], map_size[1], sizeof(float));
      if(dev_gainmap[i] == NULL) goto error;
      err = dt_opencl_write_host_to_device(devid, d->gainmaps[i]->map_gain, dev_gainmap[i],
                                           map_size[0], map_size[1], sizeof(float));
      if(err != CL_SUCCESS) goto error;
    }

    dt_opencl_set_kernel_args
      (devid, kernel, 10,
       CLARG(dev_gainmap[0]), CLARG(dev_gainmap[1]), CLARG(dev_gainmap[2]), CLARG(dev_gainmap[3]),
       CLARG(map_size), CLARG(im_to_rel), CLARG(rel_to_map), CLARG(map_origin));
  }
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_sub);
  dt_opencl_release_mem_object(dev_div);
  for(int i = 0; i < 4; i++) dt_opencl_release_mem_object(dev_gainmap[i]);

  if(piece->pipe->dsc.filters)
  {
    piece->pipe->dsc.filters =
      dt_rawspeed_crop_dcraw_filters(self->dev->image_storage.buf_dsc.filters, csx, csy);
    _adjust_xtrans_filters(piece->pipe, csx, csy);
  }

  for(int k = 0; k < 4; k++) piece->pipe->dsc.processed_maximum[k] = 1.0f;

  err = dt_dev_write_rawdetail_mask_cl(piece, dev_out, roi_in, DT_DEV_DETAIL_MASK_RAWPREPARE);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_opencl_release_mem_object(dev_sub);
  dt_opencl_release_mem_object(dev_div);
  for(int i = 0; i < 4; i++) dt_opencl_release_mem_object(dev_gainmap[i]);
  dt_print(DT_DEBUG_OPENCL, "[opencl_rawprepare] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif

static int _image_is_normalized(const dt_image_t *const image)
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

static gboolean _image_set_rawcrops(
        dt_iop_module_t *self,
        const uint32_t imgid,
        const int left,
        const int right,
        const int top,
        const int bottom)
{
  dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');

  const gboolean cropvalid = (left >= 0) && (right >= 0) && (top >= 0) && (bottom >= 0)
    && (left+right < img->width / 2) && (top + bottom < img->height /2);

  const gboolean testdim =
      (img->p_width == img->width - left - right)
   && (img->p_height == img->height - top - bottom);

  dt_image_cache_read_release(darktable.image_cache, img);
  if(testdim && cropvalid) return FALSE;

  if(!cropvalid)
  {
    dt_print
      (DT_DEBUG_ALWAYS,
       "[rawprepare] got wrong crop parameters left=%i, right=%i, top=%i, bottom=%i for size=%ix%i\n",
       left, right, top, bottom, img->width, img->height);
    dt_iop_set_module_trouble_message(self,
     _("invalid crop parameters"),
     _("please reset to defaults, update your preset or set to something correct"),
       "invalid crop parameters");
  }
  else
    dt_iop_set_module_trouble_message(self, NULL, NULL, NULL);

  img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  img->p_width = img->width - (cropvalid ? left + right : 0);
  img->p_height = img->height - (cropvalid ? top + bottom : 0);
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);

  return TRUE;
}

// check if image contains GainMaps of the exact type that we can apply here
// we may reject some GainMaps that are valid according to Adobe DNG spec but we do not support
static gboolean _check_gain_maps(dt_iop_module_t *self, dt_dng_gain_map_t **gainmaps_out)
{
  const dt_image_t *const image = &(self->dev->image_storage);
  dt_dng_gain_map_t *gainmaps[4] = {0};

  if(g_list_length(image->dng_gain_maps) != 4)
    return FALSE;

  // FIXME checks for witdh / height might be wrong
  for(int i = 0; i < 4; i++)
  {
    // check that each GainMap applies to one filter of a Bayer image,
    // covers the entire image, and is not a 1x1 no-op
    dt_dng_gain_map_t *g = (dt_dng_gain_map_t *)g_list_nth_data(image->dng_gain_maps, i);
    if(g == NULL
       || g->plane != 0
       || g->planes != 1
       || g->map_planes != 1
       || g->row_pitch != 2
       || g->col_pitch != 2
       || g->map_points_v < 2
       || g->map_points_h < 2
       || g->top > 1
       || g->left > 1
       || g->bottom != image->height
       || g->right != image->width)
      return FALSE;

    const uint32_t filter = ((g->top & 1) << 1) + (g->left & 1);
    gainmaps[filter] = g;
  }

  // check that there is a GainMap for each filter of the Bayer pattern
  if(gainmaps[0] == NULL || gainmaps[1] == NULL || gainmaps[2] == NULL || gainmaps[3] == NULL)
    return FALSE;

  // check that each GainMap has the same shape
  for(int i = 1; i < 4; i++)
  {
    if(gainmaps[i]->map_points_h != gainmaps[0]->map_points_h
       || gainmaps[i]->map_points_v != gainmaps[0]->map_points_v
       || gainmaps[i]->map_spacing_h != gainmaps[0]->map_spacing_h
       || gainmaps[i]->map_spacing_v != gainmaps[0]->map_spacing_v
       || gainmaps[i]->map_origin_h != gainmaps[0]->map_origin_h
       || gainmaps[i]->map_origin_v != gainmaps[0]->map_origin_v)
      return FALSE;
  }

  if(gainmaps_out)
    memcpy(gainmaps_out, gainmaps, sizeof(gainmaps));

  return TRUE;
}

void commit_params(
        dt_iop_module_t *self,
        dt_iop_params_t *params,
        dt_dev_pixelpipe_t *pipe,
        dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_rawprepare_params_t *const p = (dt_iop_rawprepare_params_t *)params;
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  d->left = p->left;
  d->top = p->top;
  d->right = p->right;
  d->bottom = p->bottom;

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
    const float normalizer =
      ((piece->pipe->image.flags & DT_IMAGE_HDR) == DT_IMAGE_HDR)
      ? 1.0f
      : (float)UINT16_MAX;

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

  if(p->flat_field == FLAT_FIELD_EMBEDDED)
    d->apply_gainmaps = _check_gain_maps(self, d->gainmaps);
  else
    d->apply_gainmaps = FALSE;

   if(_image_set_rawcrops(self, pipe->image.id, d->left, d->right, d->top, d->bottom))
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_METADATA_UPDATE);

  if(!(dt_image_is_rawprepare_supported(&piece->pipe->image))
     || _image_is_normalized(&piece->pipe->image))
    piece->enabled = FALSE;

  if(piece->pipe->want_detail_mask == (DT_DEV_DETAIL_MASK_REQUIRED | DT_DEV_DETAIL_MASK_RAWPREPARE))
    piece->process_tiling_ready = FALSE;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_rawprepare_data_t));
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

  // if there are embedded GainMaps, they should be applied by default to avoid uneven color cast
  const gboolean has_gainmaps = _check_gain_maps(self, NULL);

  *d = (dt_iop_rawprepare_params_t){.left = image->crop_x,
                                    .top = image->crop_y,
                                    .right = image->crop_right,
                                    .bottom = image->crop_bottom,
                                    .raw_black_level_separate[0] = image->raw_black_level_separate[0],
                                    .raw_black_level_separate[1] = image->raw_black_level_separate[1],
                                    .raw_black_level_separate[2] = image->raw_black_level_separate[2],
                                    .raw_black_level_separate[3] = image->raw_black_level_separate[3],
                                    .raw_white_point = image->raw_white_point,
                                    .flat_field = has_gainmaps ? FLAT_FIELD_EMBEDDED : FLAT_FIELD_OFF };

  self->hide_enable_button = TRUE;
  self->default_enabled = dt_image_is_rawprepare_supported(image) && !_image_is_normalized(image);

  if(self->widget)
    gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "raw" : "non_raw");
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl, from programs.conf
  self->data = malloc(sizeof(dt_iop_rawprepare_global_data_t));

  dt_iop_rawprepare_global_data_t *gd = self->data;
  gd->kernel_rawprepare_1f = dt_opencl_create_kernel(program, "rawprepare_1f");
  gd->kernel_rawprepare_1f_gainmap = dt_opencl_create_kernel(program, "rawprepare_1f_gainmap");
  gd->kernel_rawprepare_1f_unnormalized = dt_opencl_create_kernel(program, "rawprepare_1f_unnormalized");
  gd->kernel_rawprepare_1f_unnormalized_gainmap = dt_opencl_create_kernel(program, "rawprepare_1f_unnormalized_gainmap");
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

  const gboolean is_monochrome =
    (self->dev->image_storage.flags & (DT_IMAGE_MONOCHROME | DT_IMAGE_MONOCHROME_BAYER)) != 0;

  if(is_monochrome)
  {
    // we might have to deal with old edits, so get average first
    int av = 2; // for rounding
    for(int i = 0; i < 4; i++)
      av += p->raw_black_level_separate[i];

    for(int i = 0; i < 4; i++)
      dt_bauhaus_slider_set(g->black_level_separate[i], av / 4);
  }

  // don't show upper three black levels for monochromes
  for(int i = 1; i < 4; i++)
    gtk_widget_set_visible(g->black_level_separate[i], !is_monochrome);

  gtk_widget_set_visible(g->flat_field, _check_gain_maps(self, NULL));
  dt_bauhaus_combobox_set(g->flat_field, p->flat_field);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_rawprepare_gui_data_t *g = (dt_iop_rawprepare_gui_data_t *)self->gui_data;
  dt_iop_rawprepare_params_t *p = (dt_iop_rawprepare_params_t *)self->params;

  const gboolean is_monochrome =
    (self->dev->image_storage.flags & (DT_IMAGE_MONOCHROME | DT_IMAGE_MONOCHROME_BAYER)) != 0;

  if(is_monochrome)
  {
    if(w == g->black_level_separate[0])
    {
      const int val = p->raw_black_level_separate[0];
      for(int i = 1; i < 4; i++)
        dt_bauhaus_slider_set(g->black_level_separate[i], val);
    }
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

  g->flat_field = dt_bauhaus_combobox_from_params(self, "flat_field");
  gtk_widget_set_tooltip_text
    (g->flat_field,
     _("raw flat field correction to compensate for lens shading"));

  if(dt_conf_get_bool("plugins/darkroom/rawprepare/allow_editing_crop"))
  {
    gtk_box_pack_start(GTK_BOX(self->widget),
                       dt_ui_section_label_new(C_("section", "crop")), FALSE, FALSE, 0);

    g->left = dt_bauhaus_slider_from_params(self, "left");
    gtk_widget_set_tooltip_text(g->left, _("crop left border"));
    dt_bauhaus_slider_set_soft_max(g->left, 256);

    g->top = dt_bauhaus_slider_from_params(self, "top");
    gtk_widget_set_tooltip_text(g->top, _("crop top border"));
    dt_bauhaus_slider_set_soft_max(g->top, 256);

    g->right = dt_bauhaus_slider_from_params(self, "right");
    gtk_widget_set_tooltip_text(g->right, _("crop right border"));
    dt_bauhaus_slider_set_soft_max(g->right, 256);

    g->bottom = dt_bauhaus_slider_from_params(self, "bottom");
    gtk_widget_set_tooltip_text(g->bottom, _("crop bottom border"));
    dt_bauhaus_slider_set_soft_max(g->bottom, 256);
  }

  // start building top level widget
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);

  GtkWidget *label_non_raw =
    dt_ui_label_new(_("raw black/white point correction\nonly works for the sensors that need it."));

  gtk_stack_add_named(GTK_STACK(self->widget), label_non_raw, "non_raw");
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "raw");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
