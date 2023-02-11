/*
    This file is part of darktable,
    Copyright (C) 2014-2022 darktable developers.

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
#include "common/math.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_rotatepixels_params_t)

typedef struct dt_iop_rotatepixels_gui_data_t
{
} dt_iop_rotatepixels_gui_data_t;

typedef struct dt_iop_rotatepixels_params_t
{
  uint32_t rx, ry;
  float angle;
} dt_iop_rotatepixels_params_t;

typedef struct dt_iop_rotatepixels_data_t
{
  uint32_t rx, ry; // rotation center
  float m[4];      // rotation matrix
} dt_iop_rotatepixels_data_t;

dt_iop_rotatepixels_gui_data_t dummy;

// helper to count corners in for loops:
static void get_corner(const float *aabb, const int i, float *p)
{
  for(int k = 0; k < 2; k++) p[k] = aabb[2 * ((i >> k) & 1) + k];
}

static void adjust_aabb(const float *p, float *aabb)
{
  aabb[0] = fminf(aabb[0], p[0]);
  aabb[1] = fminf(aabb[1], p[1]);
  aabb[2] = fmaxf(aabb[2], p[0]);
  aabb[3] = fmaxf(aabb[3], p[1]);
}


const char *name()
{
  return C_("modulename", "Rotate pixels");
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
                                _("Internal module to setup technical specificities of raw sensor.\n\n"
                                  "You should not touch values here!"),
                                NULL, NULL, NULL, NULL);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static void transform(const dt_dev_pixelpipe_iop_t *const piece, const float scale, const float *const x,
                      float *o)
{
  dt_iop_rotatepixels_data_t *d = (dt_iop_rotatepixels_data_t *)piece->data;

  float pi[2] = { x[0] - d->rx * scale, x[1] - d->ry * scale };

  mul_mat_vec_2(d->m, pi, o);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static void backtransform(const dt_dev_pixelpipe_iop_t *const piece, const float scale, const float *const x,
                          float *o)
{
  dt_iop_rotatepixels_data_t *d = (dt_iop_rotatepixels_data_t *)piece->data;

  float rt[] = { d->m[0], -d->m[1], -d->m[2], d->m[3] };
  mul_mat_vec_2(rt, x, o);

  o[0] += d->rx * scale;
  o[1] += d->ry * scale;
}

int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points, size_t points_count)
{
  const float scale = piece->buf_in.scale / piece->iscale;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(points_count, points, scale, piece) \
    schedule(static) if(points_count > 100) aligned(points:64)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    float pi[2], po[2];

    pi[0] = points[i];
    pi[1] = points[i + 1];

    transform(piece, scale, pi, po);

    points[i] = po[0];
    points[i + 1] = po[1];
  }

  return 1;
}

int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points,
                          size_t points_count)
{
  const float scale = piece->buf_in.scale / piece->iscale;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(points_count, points, scale, piece) \
    schedule(static) if(points_count > 100) aligned(points:64)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    float pi[2], po[2];

    pi[0] = points[i];
    pi[1] = points[i + 1];

    backtransform(piece, scale, pi, po);

    points[i] = po[0];
    points[i + 1] = po[1];
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

// 1st pass: how large would the output be, given this input roi?
// this is always called with the full buffer before processing.
void modify_roi_out(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  dt_iop_rotatepixels_data_t *d = (dt_iop_rotatepixels_data_t *)piece->data;

  *roi_out = *roi_in;

  /*
   * Think of input image as:
   * 1. Square, containing:
   * 3. 4x Right triangles (two pairs), located in the Edges of square
   * 2. Rectangle (rotated 45 degrees), located in between triangles.
   *
   * Therefore, output image dimensions, that are sizes of inner Rectangle
   * can be found using Pythagorean theorem.
   *
   * Not precise pseudographics: (width == height + 1)
   *         height
   *     _  -------
   *     |  |1 /\2|
   * ty  |  | y  \|
   *     _  |/   /| width
   *        |\x / |
   *        |2\/ 1|
   *        -------
   */

  const float scale = roi_in->scale / piece->iscale, T = (float)d->ry * scale;

  const float y = sqrtf(2.0f * T * T),
              x = sqrtf(2.0f * ((float)roi_in->width - T) * ((float)roi_in->width - T));

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  const float IW = (float)interpolation->width * scale;

  roi_out->width = y - IW;
  roi_out->height = x - IW;

  roi_out->width = MAX(0, roi_out->width & ~1);
  roi_out->height = MAX(0, roi_out->height & ~1);
}

// 2nd pass: which roi would this operation need as input to fill the given output region?
void modify_roi_in(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;

  const float scale = roi_in->scale / piece->iscale;

  dt_boundingbox_t aabb = { roi_out->x, roi_out->y, roi_out->x + roi_out->width, roi_out->y + roi_out->height };

  dt_boundingbox_t aabb_in = { INFINITY, INFINITY, -INFINITY, -INFINITY };

  for(int c = 0; c < 4; c++)
  {
    float p[2], o[2];

    // get corner points of roi_out
    get_corner(aabb, c, p);

    backtransform(piece, scale, p, o);

    // transform to roi_in space, get aabb.
    adjust_aabb(o, aabb_in);
  }

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  const float IW = (float)interpolation->width * scale;

  const float orig_w = roi_in->scale * piece->buf_in.width, orig_h = roi_in->scale * piece->buf_in.height;

  // adjust roi_in to minimally needed region
  roi_in->x = fmaxf(0.0f, aabb_in[0] - IW);
  roi_in->y = fmaxf(0.0f, aabb_in[1] - IW);
  roi_in->width = fminf(orig_w - roi_in->x, aabb_in[2] - roi_in->x + IW);
  roi_in->height = fminf(orig_h - roi_in->y, aabb_in[3] - roi_in->y + IW);

  // sanity check.
  roi_in->x = CLAMP(roi_in->x, 0, (int)floorf(orig_w));
  roi_in->y = CLAMP(roi_in->y, 0, (int)floorf(orig_h));
  roi_in->width = CLAMP(roi_in->width, 1, (int)ceilf(orig_w) - roi_in->x);
  roi_in->height = CLAMP(roi_in->height, 1, (int)ceilf(orig_h) - roi_in->y);
}

// 3rd (final) pass: you get this input region (may be different from what was requested above),
// do your best to fill the output region!
void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const int ch = piece->colors;
  const int ch_width = ch * roi_in->width;

  const float scale = roi_in->scale / piece->iscale;

  assert(ch == 4);

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, ch_width, ivoid, ovoid, roi_in, roi_out, scale) \
  shared(piece, interpolation) \
  schedule(static)
#endif
  // (slow) point-by-point transformation.
  // TODO: optimize with scanlines and linear steps between?
  for(int j = 0; j < roi_out->height; j++)
  {
    float *out = ((float *)ovoid) + (size_t)ch * j * roi_out->width;
    for(int i = 0; i < roi_out->width; i++, out += ch)
    {
      float pi[2], po[2];

      pi[0] = roi_out->x + i;
      pi[1] = roi_out->y + j;

      backtransform(piece, scale, pi, po);

      po[0] -= roi_in->x;
      po[1] -= roi_in->y;

      dt_interpolation_compute_pixel4c(interpolation, (float *)ivoid, out, po[0], po[1], roi_in->width,
                                       roi_in->height, ch_width);
    }
  }
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rotatepixels_params_t *p = (dt_iop_rotatepixels_params_t *)p1;
  dt_iop_rotatepixels_data_t *d = (dt_iop_rotatepixels_data_t *)piece->data;

  d->rx = p->rx;
  d->ry = p->ry;

  const float angle = p->angle * M_PI / 180.0f;

  float rt[] = { cosf(angle), sinf(angle), -sinf(angle), cosf(angle) };
  for(int k = 0; k < 4; k++) d->m[k] = rt[k];

  // this should not be used for normal images
  // (i.e. for those, when this iop is off by default)
  if((d->rx == 0u) && (d->ry == 0u)) piece->enabled = 0;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_rotatepixels_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_rotatepixels_params_t *d = self->default_params;

  const dt_image_t *const image = &(self->dev->image_storage);

  *d = (dt_iop_rotatepixels_params_t){ .rx = 0u, .ry = image->fuji_rotation_pos, .angle = -45.0f };

  self->default_enabled = ((d->rx != 0u) || (d->ry != 0u));

  // FIXME: does not work.
  self->hide_enable_button = !self->default_enabled;

  if(self->widget)
    gtk_label_set_text(GTK_LABEL(self->widget), self->default_enabled
                       ? _("Automatic pixel rotation")
                       : _("Automatic pixel rotation\nonly works for the sensors that need it."));
}

void gui_update(dt_iop_module_t *self)
{
}
void gui_init(dt_iop_module_t *self)
{
  IOP_GUI_ALLOC(rotatepixels);

  self->widget = dt_ui_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(self->widget), TRUE);

}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
