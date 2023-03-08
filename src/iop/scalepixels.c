/*
    This file is part of darktable,
    Copyright (C) 2014-2023 darktable developers.

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
#include "common/interpolation.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_scalepixels_params_t)

typedef struct dt_iop_scalepixels_params_t
{
  // Aspect ratio of the pixels, usually 1 but some cameras need scaling
  // <1 means the image needs to be stretched vertically, (0.5 means 2x)
  // >1 means the image needs to be stretched horizontally (2 mean 2x)
  float pixel_aspect_ratio; // $DEFAULT: 1.0f
} dt_iop_scalepixels_params_t;

typedef struct dt_iop_scalepixels_gui_data_t
{
} dt_iop_scalepixels_gui_data_t;

typedef struct dt_iop_scalepixels_data_t {
  float pixel_aspect_ratio;
  float x_scale;
  float y_scale;
} dt_iop_scalepixels_data_t;

const char *name()
{
  return C_("modulename", "scale pixels");
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE
    | IOP_FLAGS_UNSAFE_COPY;
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
}

int operation_tags()
{
  return IOP_TAG_DISTORT;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("internal module to setup technical specificities of raw sensor.\n\n"
                                  "you should not touch values here!"),
                                NULL, NULL, NULL, NULL);
}

static void transform(const dt_dev_pixelpipe_iop_t *const piece, float *p)
{
  dt_iop_scalepixels_data_t *d = piece->data;

  if(d->pixel_aspect_ratio < 1.0f)
  {
    p[1] /= d->pixel_aspect_ratio;
  }
  else
  {
    p[0] *= d->pixel_aspect_ratio;
  }
}

static void precalculate_scale(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  // Since the scaling is calculated by modify_roi_in use that to get them
  // This doesn't seem strictly needed but since clipping.c also does it we try
  // and avoid breaking any assumptions elsewhere in the code
  dt_iop_roi_t roi_out, roi_in;
  roi_out.width = piece->buf_in.width;
  roi_out.height = piece->buf_in.height;
  self->modify_roi_in(self, piece, &roi_out, &roi_in);
}

int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count)
{
  precalculate_scale(self, piece);
  dt_iop_scalepixels_data_t *d = piece->data;

  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] /= d->x_scale;
    points[i+1] /= d->y_scale;
  }

  return 1;
}

int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points,
                          size_t points_count)
{
  precalculate_scale(self, piece);
  dt_iop_scalepixels_data_t *d = piece->data;

  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] *= d->x_scale;
    points[i+1] *= d->y_scale;
  }

  return 1;
}

void distort_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                  float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // TODO
  memset(out, 0, sizeof(float) * roi_out->width * roi_out->height);
  dt_print(DT_DEBUG_ALWAYS, "TODO: implement %s() in %s\n", __FUNCTION__, __FILE__);
}

void modify_roi_out(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;

  float xy[2] = { roi_out->x, roi_out->y };
  float wh[2] = { roi_out->width, roi_out->height };

  transform(piece, xy);
  transform(piece, wh);

  roi_out->x = (int)floorf(xy[0]);
  roi_out->y = (int)floorf(xy[1]);
  roi_out->width = (int)ceilf(wh[0]);
  roi_out->height = (int)ceilf(wh[1]);

  // sanity check.
  if(roi_out->x < 0) roi_out->x = 0;
  if(roi_out->y < 0) roi_out->y = 0;
  if(roi_out->width < 1) roi_out->width = 1;
  if(roi_out->height < 1) roi_out->height = 1;
}

void modify_roi_in(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;

  // If possible try to get an image that's strictly larger than what we want to output
  float hw[2] = {roi_out->height, roi_out->width};
  transform(piece, hw); // transform() is used reversed here intentionally
  roi_in->height = hw[0];
  roi_in->width = hw[1];

  float reduction_ratio = MAX(hw[0] / (piece->buf_in.height * 1.0f), hw[1] / (piece->buf_in.width * 1.0f));
  if(reduction_ratio > 1.0f)
  {
    roi_in->height /= reduction_ratio;
    roi_in->width /= reduction_ratio;
  }

  dt_iop_scalepixels_data_t *d = piece->data;
  d->x_scale = (roi_in->width * 1.0f) / (roi_out->width * 1.0f);
  d->y_scale = (roi_in->height * 1.0f) / (roi_out->height * 1.0f);

  roi_in->scale = roi_out->scale * MAX(d->x_scale, d->y_scale);
  roi_in->x = roi_out->x * d->x_scale;
  roi_in->y = roi_out->y * d->y_scale;
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const int ch = piece->colors;
  const int ch_width = ch * roi_in->width;
  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  const dt_iop_scalepixels_data_t * const d = piece->data;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch_width, d, ivoid, ovoid, roi_in, roi_out) \
  shared(interpolation) \
  schedule(static)
#endif
  // (slow) point-by-point transformation.
  // TODO: optimize with scanlines and linear steps between?
  for(int j = 0; j < roi_out->height; j++)
  {
    float *out = ((float *)ovoid) + (size_t)4 * j * roi_out->width;
    for(int i = 0; i < roi_out->width; i++, out += 4)
    {
      float x = i*d->x_scale;
      float y = j*d->y_scale;

      dt_interpolation_compute_pixel4c(interpolation, (float *)ivoid, out, x, y, roi_in->width,
                                       roi_in->height, ch_width);
    }
  }
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_scalepixels_params_t *p = params;
  dt_iop_scalepixels_data_t *d = piece->data;

  d->pixel_aspect_ratio = p->pixel_aspect_ratio;
  d->x_scale = 1.0f;
  d->y_scale = 1.0f;

  if(isnan(p->pixel_aspect_ratio) || p->pixel_aspect_ratio <= 0.0f || p->pixel_aspect_ratio == 1.0f)
    piece->enabled = 0;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_scalepixels_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_scalepixels_params_t *d = self->default_params;

  const dt_image_t *const image = &(self->dev->image_storage);

  d->pixel_aspect_ratio = image->pixel_aspect_ratio;

  self->default_enabled = (!isnan(d->pixel_aspect_ratio) &&
                           d->pixel_aspect_ratio > 0.0f &&
                           d->pixel_aspect_ratio != 1.0f);

  // FIXME: does not work.
  self->hide_enable_button = !self->default_enabled;

  if(self->widget)
    gtk_label_set_text(GTK_LABEL(self->widget), self->default_enabled
                       ? _("automatic pixel scaling")
                       :_("automatic pixel scaling\nonly works for the sensors that need it."));
}

void gui_update(dt_iop_module_t *self)
{
}

void gui_init(dt_iop_module_t *self)
{
  IOP_GUI_ALLOC(scalepixels);

  self->widget = dt_ui_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(self->widget), TRUE);

}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

