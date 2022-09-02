/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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
#include <assert.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/debug.h"
#include "common/imageio.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(2, dt_iop_flip_params_t)

typedef struct dt_iop_flip_params_t
{
  dt_image_orientation_t orientation;
} dt_iop_flip_params_t;

typedef struct dt_iop_flip_params_t dt_iop_flip_data_t;

typedef struct dt_iop_flip_global_data_t
{
  int kernel_flip;
} dt_iop_flip_global_data_t;

// helper to count corners in for loops:
static void get_corner(const int32_t *aabb, const int i, int32_t *p)
{
  for(int k = 0; k < 2; k++) p[k] = aabb[2 * ((i >> k) & 1) + k];
}

static void adjust_aabb(const int32_t *p, int32_t *aabb)
{
  aabb[0] = MIN(aabb[0], p[0]);
  aabb[1] = MIN(aabb[1], p[1]);
  aabb[2] = MAX(aabb[2], p[0]);
  aabb[3] = MAX(aabb[3], p[1]);
}

const char *name()
{
  return _("orientation");
}

const char *aliases()
{
  return _("rotation|flip");
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int operation_tags()
{
  return IOP_TAG_DISTORT;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_UNSAFE_COPY
         | IOP_FLAGS_GUIDES_WIDGET;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("flip or rotate image by step of 90 degrees"), _("corrective"),
                                _("linear, RGB, scene-referred"), _("geometric, RGB"),
                                _("linear, RGB, scene-referred"));
}

static dt_image_orientation_t merge_two_orientations(dt_image_orientation_t raw_orientation,
                                                     dt_image_orientation_t user_orientation)
{
  dt_image_orientation_t raw_orientation_corrected = raw_orientation;
  /*
   * if user-specified orientation has ORIENTATION_SWAP_XY set, then we need
   * to swap ORIENTATION_FLIP_Y and ORIENTATION_FLIP_X bits
   * in raw orientation
   */
  if((user_orientation & ORIENTATION_SWAP_XY) == ORIENTATION_SWAP_XY)
  {
    if((raw_orientation & ORIENTATION_FLIP_Y) == ORIENTATION_FLIP_Y)
      raw_orientation_corrected |= ORIENTATION_FLIP_X;
    else
      raw_orientation_corrected &= ~ORIENTATION_FLIP_X;

    if((raw_orientation & ORIENTATION_FLIP_X) == ORIENTATION_FLIP_X)
      raw_orientation_corrected |= ORIENTATION_FLIP_Y;
    else
      raw_orientation_corrected &= ~ORIENTATION_FLIP_Y;

    if((raw_orientation & ORIENTATION_SWAP_XY) == ORIENTATION_SWAP_XY)
      raw_orientation_corrected |= ORIENTATION_SWAP_XY;
  }

  // and now we can automagically compute new flip
  return raw_orientation_corrected ^ user_orientation;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_iop_flip_params_v1_t
    {
      int32_t orientation;
    } dt_iop_flip_params_v1_t;

    const dt_iop_flip_params_v1_t *old = (dt_iop_flip_params_v1_t *)old_params;
    dt_iop_flip_params_t *n = (dt_iop_flip_params_t *)new_params;
    const dt_iop_flip_params_t *d = (dt_iop_flip_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    // we might be called from presets update infrastructure => there is no image
    dt_image_orientation_t image_orientation = ORIENTATION_NONE;

    if(self->dev)
      image_orientation = dt_image_orientation(&self->dev->image_storage);

    n->orientation = merge_two_orientations(image_orientation,
                                            (dt_image_orientation_t)(old->orientation));

    return 0;
  }
  return 1;
}

static void backtransform(const int32_t *x, int32_t *o, const dt_image_orientation_t orientation, int32_t iw,
                          int32_t ih)
{
  if(orientation & ORIENTATION_SWAP_XY)
  {
    o[1] = x[0];
    o[0] = x[1];
    const int32_t tmp = iw;
    iw = ih;
    ih = tmp;
  }
  else
  {
    o[0] = x[0];
    o[1] = x[1];
  }
  if(orientation & ORIENTATION_FLIP_X)
  {
    o[0] = iw - o[0] - 1;
  }
  if(orientation & ORIENTATION_FLIP_Y)
  {
    o[1] = ih - o[1] - 1;
  }
}

int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points, size_t points_count)
{
  // if(!self->enabled) return 2;
  const dt_iop_flip_data_t *d = (dt_iop_flip_data_t *)piece->data;

  // nothing to be done if parameters are set to neutral values (no flip or swap)
  if(d->orientation == 0) return 1;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(points_count, points, d, piece) \
    schedule(static) if(points_count > 500)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    float x = points[i];
    float y = points[i + 1];
    if(d->orientation & ORIENTATION_FLIP_X) x = piece->buf_in.width - points[i];
    if(d->orientation & ORIENTATION_FLIP_Y) y = piece->buf_in.height - points[i + 1];
    if(d->orientation & ORIENTATION_SWAP_XY)
    {
      const float yy = y;
      y = x;
      x = yy;
    }
    points[i] = x;
    points[i + 1] = y;
  }

  return 1;
}
int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points,
                          size_t points_count)
{
  // if(!self->enabled) return 2;
  const dt_iop_flip_data_t *d = (dt_iop_flip_data_t *)piece->data;

  // nothing to be done if parameters are set to neutral values (no flip or swap)
  if(d->orientation == 0) return 1;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(points_count, points, d, piece) \
    schedule(static) if(points_count > 500)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    float x, y;
    if(d->orientation & ORIENTATION_SWAP_XY)
    {
      y = points[i];
      x = points[i + 1];
    }
    else
    {
      x = points[i];
      y = points[i + 1];
    }
    if(d->orientation & ORIENTATION_FLIP_X) x = piece->buf_in.width - x;
    if(d->orientation & ORIENTATION_FLIP_Y) y = piece->buf_in.height - y;

    points[i] = x;
    points[i + 1] = y;
  }

  return 1;
}

void distort_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                  float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_flip_data_t *d = (dt_iop_flip_data_t *)piece->data;

  const int bpp = sizeof(float);
  const int stride = bpp * roi_in->width;

  dt_imageio_flip_buffers((char *)out, (const char *)in, bpp, roi_in->width, roi_in->height,
                          roi_in->width, roi_in->height, stride, d->orientation);
}

// 1st pass: how large would the output be, given this input roi?
// this is always called with the full buffer before processing.
void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)
{
  const dt_iop_flip_data_t *d = (dt_iop_flip_data_t *)piece->data;
  *roi_out = *roi_in;

  // transform whole buffer roi
  if(d->orientation & ORIENTATION_SWAP_XY)
  {
    roi_out->width = roi_in->height;
    roi_out->height = roi_in->width;
  }
}

// 2nd pass: which roi would this operation need as input to fill the given output region?
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  const dt_iop_flip_data_t *d = (dt_iop_flip_data_t *)piece->data;
  *roi_in = *roi_out;
  // transform aabb back to roi_in

  // this aabb contains all valid points (thus the -1)
  int32_t p[2], o[2],
      aabb[4] = { roi_out->x, roi_out->y, roi_out->x + roi_out->width - 1, roi_out->y + roi_out->height - 1 };
  int32_t aabb_in[4] = { INT_MAX, INT_MAX, INT_MIN, INT_MIN };
  for(int c = 0; c < 4; c++)
  {
    // get corner points of roi_out
    get_corner(aabb, c, p);
    // backtransform aabb
    backtransform(p, o, d->orientation, piece->buf_out.width * roi_out->scale,
                  piece->buf_out.height * roi_out->scale);
    // transform to roi_in space, get aabb.
    adjust_aabb(o, aabb_in);
  }

  // adjust roi_in to minimally needed region
  roi_in->x = aabb_in[0];
  roi_in->y = aabb_in[1];
  // to convert valid points to widths, we need to add one
  roi_in->width = aabb_in[2] - aabb_in[0] + 1;
  roi_in->height = aabb_in[3] - aabb_in[1] + 1;

  // sanity check.
  float w = piece->buf_in.width * roi_out->scale, h = piece->buf_in.height * roi_out->scale;
  roi_in->x = CLAMP(roi_in->x, 0, (int)floorf(w));
  roi_in->y = CLAMP(roi_in->y, 0, (int)floorf(h));
  roi_in->width = CLAMP(roi_in->width, 1, (int)ceilf(w) - roi_in->x);
  roi_in->height = CLAMP(roi_in->height, 1, (int)ceilf(h) - roi_in->y);
}

// 3rd (final) pass: you get this input region (may be different from what was requested above),
// do your best to fill the output region!
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_flip_data_t *d = (dt_iop_flip_data_t *)piece->data;

  const int bpp = sizeof(float) * piece->colors;
  const int stride = bpp * roi_in->width;

  dt_imageio_flip_buffers((char *)ovoid, (const char *)ivoid, bpp, roi_in->width, roi_in->height,
                          roi_in->width, roi_in->height, stride, d->orientation);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_flip_data_t *data = (dt_iop_flip_data_t *)piece->data;
  const dt_iop_flip_global_data_t *gd = (dt_iop_flip_global_data_t *)self->global_data;
  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int orientation = data->orientation;

  const size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  dt_opencl_set_kernel_arg(devid, gd->kernel_flip, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_flip, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_flip, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_flip, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_flip, 4, sizeof(int), (void *)&orientation);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_flip, sizes);

  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_flip] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_flip_global_data_t *gd = (dt_iop_flip_global_data_t *)malloc(sizeof(dt_iop_flip_global_data_t));
  self->data = gd;
  gd->kernel_flip = dt_opencl_create_kernel(program, "flip");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  const dt_iop_flip_global_data_t *gd = (dt_iop_flip_global_data_t *)self->data;
  dt_opencl_free_kernel(gd->kernel_flip);
  free(self->data);
  self->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_flip_params_t *p = (dt_iop_flip_params_t *)p1;
  dt_iop_flip_data_t *d = (dt_iop_flip_data_t *)piece->data;

  if(p->orientation == ORIENTATION_NULL)
    d->orientation = dt_image_orientation(&self->dev->image_storage);
  else
    d->orientation = p->orientation;

  if(d->orientation == ORIENTATION_NONE) piece->enabled = 0;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_flip_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_flip_params_t p = (dt_iop_flip_params_t){ ORIENTATION_NONE };
  dt_database_start_transaction(darktable.db);

  p.orientation = ORIENTATION_NULL;
  dt_gui_presets_add_generic(_("autodetect"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_NONE);
  dt_gui_presets_update_autoapply(_("autodetect"), self->op, self->version(), 1);

  p.orientation = ORIENTATION_NONE;
  dt_gui_presets_add_generic(_("no rotation"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_NONE);

  p.orientation = ORIENTATION_FLIP_HORIZONTALLY;
  dt_gui_presets_add_generic(_("flip horizontally"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_NONE);

  p.orientation = ORIENTATION_FLIP_VERTICALLY;
  dt_gui_presets_add_generic(_("flip vertically"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_NONE);

  p.orientation = ORIENTATION_ROTATE_CW_90_DEG;
  dt_gui_presets_add_generic(_("rotate by -90 degrees"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_NONE);

  p.orientation = ORIENTATION_ROTATE_CCW_90_DEG;
  dt_gui_presets_add_generic(_("rotate by  90 degrees"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_NONE);

  p.orientation = ORIENTATION_ROTATE_180_DEG;
  dt_gui_presets_add_generic(_("rotate by 180 degrees"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_NONE);

  dt_database_release_transaction(darktable.db);
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_flip_params_t *d = self->default_params;

  d->orientation = ORIENTATION_NULL;

  self->default_enabled = 1;

  if(self->dev->image_storage.legacy_flip.user_flip != 0
     && self->dev->image_storage.legacy_flip.user_flip != 0xff)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT * FROM main.history WHERE imgid = ?1 AND operation = 'flip'", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, self->dev->image_storage.id);
    if(sqlite3_step(stmt) != SQLITE_ROW)
    {
      // convert the old legacy flip bits to a proper parameter set:
      d->orientation
          = merge_two_orientations(dt_image_orientation(&self->dev->image_storage),
                                   (dt_image_orientation_t)(self->dev->image_storage.legacy_flip.user_flip));
    }
    sqlite3_finalize(stmt);
  }
}

static void do_rotate(dt_iop_module_t *self, uint32_t cw)
{
  dt_iop_flip_params_t *p = (dt_iop_flip_params_t *)self->params;
  dt_image_orientation_t orientation = p->orientation;

  if(orientation == ORIENTATION_NULL) orientation = dt_image_orientation(&self->dev->image_storage);

  if(cw == 0)
  {
    if(orientation & ORIENTATION_SWAP_XY)
      orientation ^= ORIENTATION_FLIP_Y;
    else
      orientation ^= ORIENTATION_FLIP_X;
  }
  else
  {
    if(orientation & ORIENTATION_SWAP_XY)
      orientation ^= ORIENTATION_FLIP_X;
    else
      orientation ^= ORIENTATION_FLIP_Y;
  }
  orientation ^= ORIENTATION_SWAP_XY;

  p->orientation = orientation;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void rotate_cw(GtkWidget *widget, dt_iop_module_t *self)
{
  do_rotate(self, 1);
}
static void rotate_ccw(GtkWidget *widget, dt_iop_module_t *self)
{
  do_rotate(self, 0);
}
static void _flip_h(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_flip_params_t *p = (dt_iop_flip_params_t *)self->params;
  dt_image_orientation_t orientation = p->orientation;

  if(orientation == ORIENTATION_NULL) orientation = dt_image_orientation(&self->dev->image_storage);

  if(orientation & ORIENTATION_SWAP_XY)
    p->orientation = orientation ^ ORIENTATION_FLIP_VERTICALLY;
  else
    p->orientation = orientation ^ ORIENTATION_FLIP_HORIZONTALLY;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void _flip_v(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_flip_params_t *p = (dt_iop_flip_params_t *)self->params;
  dt_image_orientation_t orientation = p->orientation;

  if(orientation == ORIENTATION_NULL) orientation = dt_image_orientation(&self->dev->image_storage);

  if(orientation & ORIENTATION_SWAP_XY)
    p->orientation = orientation ^ ORIENTATION_FLIP_HORIZONTALLY;
  else
    p->orientation = orientation ^ ORIENTATION_FLIP_VERTICALLY;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = NULL;
  dt_iop_flip_params_t *p = (dt_iop_flip_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  GtkWidget *label = dtgtk_reset_label_new(_("transform"), self, &p->orientation, sizeof(int32_t));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);

  dt_iop_button_new(self, N_("rotate 90 degrees CCW"),
                    G_CALLBACK(rotate_ccw), FALSE, GDK_KEY_bracketleft, 0,
                    dtgtk_cairo_paint_refresh, 0, self->widget);

  dt_iop_button_new(self, N_("rotate 90 degrees CW"),
                    G_CALLBACK(rotate_cw), FALSE, GDK_KEY_bracketright, 0,
                    dtgtk_cairo_paint_refresh, 1, self->widget);

  dt_iop_button_new(self, N_("flip horizontally"), G_CALLBACK(_flip_h), FALSE, 0, 0, dtgtk_cairo_paint_flip, 1,
                    self->widget);

  dt_iop_button_new(self, N_("flip vertically"), G_CALLBACK(_flip_v), FALSE, 0, 0, dtgtk_cairo_paint_flip, 0,
                    self->widget);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  self->gui_data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

