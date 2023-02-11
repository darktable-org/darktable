/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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
#include "common/debug.h"
#include "common/imagebuf.h"
#include "common/interpolation.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "dtgtk/expander.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/guides.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "libs/modulegroups.h"

#include <assert.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_crop_params_t)

/** flip guides H/V */
typedef enum dt_iop_crop_flip_t
{
  FLAG_FLIP_HORIZONTAL = 1 << 0,
  FLAG_FLIP_VERTICAL = 1 << 1
} dt_iop_crop_flip_t;

typedef struct dt_iop_crop_aspect_t
{
  char *name;
  int d, n;
} dt_iop_crop_aspect_t;

typedef struct dt_iop_crop_params_t
{
  float cx;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "Left"
  float cy;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "Top"
  float cw;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "Right"
  float ch;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "Bottom"
  int ratio_n; // $DEFAULT: -1
  int ratio_d; // $DEFAULT: -1
} dt_iop_crop_params_t;

typedef enum _grab_region_t
{
  GRAB_CENTER = 0,                                            // 0
  GRAB_LEFT = 1 << 0,                                         // 1
  GRAB_TOP = 1 << 1,                                          // 2
  GRAB_RIGHT = 1 << 2,                                        // 4
  GRAB_BOTTOM = 1 << 3,                                       // 8
  GRAB_TOP_LEFT = GRAB_TOP | GRAB_LEFT,                       // 3
  GRAB_TOP_RIGHT = GRAB_TOP | GRAB_RIGHT,                     // 6
  GRAB_BOTTOM_RIGHT = GRAB_BOTTOM | GRAB_RIGHT,               // 12
  GRAB_BOTTOM_LEFT = GRAB_BOTTOM | GRAB_LEFT,                 // 9
  GRAB_HORIZONTAL = GRAB_LEFT | GRAB_RIGHT,                   // 5
  GRAB_VERTICAL = GRAB_TOP | GRAB_BOTTOM,                     // 10
  GRAB_ALL = GRAB_LEFT | GRAB_TOP | GRAB_RIGHT | GRAB_BOTTOM, // 15
  GRAB_NONE = 1 << 4                                          // 16
} _grab_region_t;

typedef struct dt_iop_crop_gui_data_t
{
  GtkWidget *cx, *cy, *cw, *ch;
  GList *aspect_list;
  GtkWidget *aspect_presets;

  float button_down_x, button_down_y;
  float button_down_zoom_x, button_down_zoom_y;

  /* current clip box */
  float clip_x, clip_y, clip_w, clip_h, handle_x, handle_y;
  /* last box before change */
  float prev_clip_x, prev_clip_y, prev_clip_w, prev_clip_h;
  /* maximum clip box */
  float clip_max_x, clip_max_y, clip_max_w, clip_max_h;
  uint64_t clip_max_pipe_hash;

  int cropping;
  gboolean shift_hold;
  gboolean ctrl_hold;
  gboolean preview_ready;
  gint64 focus_time;
  dt_gui_collapsible_section_t cs;
} dt_iop_crop_gui_data_t;

typedef struct dt_iop_crop_data_t
{
  float aspect;         // forced aspect ratio
  float cx, cy, cw, ch; // crop window
} dt_iop_crop_data_t;

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  return 0;
}

const char *name()
{
  return _("Crop");
}

const char *aliases()
{
  return _("Reframe|distortion");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("Change the framing"), _("Corrective or creative"),
                                _("Linear, RGB, scene-referred"), _("Geometric, RGB"),
                                _("Linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_ALLOW_FAST_PIPE
         | IOP_FLAGS_GUIDES_SPECIAL_DRAW | IOP_FLAGS_GUIDES_WIDGET;
}

int operation_tags()
{
  return IOP_TAG_DISTORT | IOP_TAG_CLIPPING;
}

int operation_tags_filter()
{
  // switch off watermark, it gets confused.
  return IOP_TAG_DECORATION;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static int _gui_has_focus(struct dt_iop_module_t *self)
{
  return (self->dev->gui_module == self
          && dt_dev_modulegroups_get_activated(darktable.develop) != DT_MODULEGROUP_BASICS);
}

static void _commit_box(dt_iop_module_t *self, dt_iop_crop_gui_data_t *g, dt_iop_crop_params_t *p)
{
  if(darktable.gui->reset) return;
  if(self->dev->preview_status != DT_DEV_PIXELPIPE_VALID) return;

  g->cropping = 0;
  const dt_boundingbox_t old = { p->cx, p->cy, p->cw, p->ch };
  const float eps = 1e-6f; // threshold to avoid rounding errors
  if(!self->enabled)
  {
    // first time crop, if any data is stored in p, it's obsolete:
    p->cx = p->cy = 0.0f;
    p->cw = p->ch = 1.0f;
  }
  // we want value in iop space
  const float wd = self->dev->preview_pipe->backbuf_width;
  const float ht = self->dev->preview_pipe->backbuf_height;
  dt_boundingbox_t points = { g->clip_x * wd, g->clip_y * ht, (g->clip_x + g->clip_w) * wd, (g->clip_y + g->clip_h) * ht };
  if(dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                       DT_DEV_TRANSFORM_DIR_FORW_EXCL, points, 2))
  {
    dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
    if(piece)
    {
      if(piece->buf_out.width < 1 || piece->buf_out.height < 1) return;
      p->cx = points[0] / (float)piece->buf_out.width;
      p->cy = points[1] / (float)piece->buf_out.height;
      p->cw = points[2] / (float)piece->buf_out.width;
      p->ch = points[3] / (float)piece->buf_out.height;
      // verify that the crop area stay in the image area
      p->cx = CLAMPF(p->cx, 0.0f, 0.9f);
      p->cy = CLAMPF(p->cy, 0.0f, 0.9f);
      p->cw = CLAMPF(p->cw, 0.1f, 1.0f);
      p->ch = CLAMPF(p->ch, 0.1f, 1.0f);
    }
  }
  const gboolean changed = fabs(p->cx - old[0]) > eps || fabs(p->cy - old[1]) > eps || fabs(p->cw - old[2]) > eps || fabs(p->ch - old[3]) > eps;
  // fprintf(stderr, "[crop commit box] %i:  %e %e %e %e\n", changed, p->cx - old[0], p->cy - old[1], p->cw - old[2], p->ch - old[3]);
  if(changed) dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static int _set_max_clip(struct dt_iop_module_t *self)
{
  dt_iop_crop_gui_data_t *g = (dt_iop_crop_gui_data_t *)self->gui_data;
  dt_iop_crop_params_t *p = (dt_iop_crop_params_t *)self->params;

  if(g->clip_max_pipe_hash == self->dev->preview_pipe->backbuf_hash) return 1;
  if(self->dev->preview_status != DT_DEV_PIXELPIPE_VALID) return 1;

  // we want to know the size of the actual buffer
  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
  if(!piece) return 0;

  float wp = piece->buf_out.width, hp = piece->buf_out.height;
  float points[8] = { 0.0f, 0.0f, wp, hp, p->cx * wp, p->cy * hp, p->cw * wp, p->ch * hp };
  if(!dt_dev_distort_transform_plus(self->dev, self->dev->preview_pipe, self->iop_order,
                                    DT_DEV_TRANSFORM_DIR_FORW_EXCL, points, 4))
    return 0;

  g->clip_max_x = fmaxf(points[0] / self->dev->preview_pipe->backbuf_width, 0.0f);
  g->clip_max_y = fmaxf(points[1] / self->dev->preview_pipe->backbuf_height, 0.0f);
  g->clip_max_w = fminf((points[2] - points[0]) / self->dev->preview_pipe->backbuf_width, 1.0f);
  g->clip_max_h = fminf((points[3] - points[1]) / self->dev->preview_pipe->backbuf_height, 1.0f);

  // if clipping values are not null, this is undistorted values...
  g->clip_x = fmaxf(points[4] / self->dev->preview_pipe->backbuf_width, g->clip_max_x);
  g->clip_y = fmaxf(points[5] / self->dev->preview_pipe->backbuf_height, g->clip_max_y);
  g->clip_w = fminf((points[6] - points[4]) / self->dev->preview_pipe->backbuf_width, g->clip_max_w);
  g->clip_h = fminf((points[7] - points[5]) / self->dev->preview_pipe->backbuf_height, g->clip_max_h);

  g->clip_max_pipe_hash = self->dev->preview_pipe->backbuf_hash;
  return 1;
}

int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points,
                      size_t points_count)
{
  dt_iop_crop_data_t *d = (dt_iop_crop_data_t *)piece->data;

  const float crop_top = piece->buf_in.height * d->cy;
  const float crop_left = piece->buf_in.width * d->cx;

  // nothing to be done if parameters are set to neutral values (no top/left border)
  if(crop_top == 0 && crop_left == 0) return 1;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(points, points_count, crop_left, crop_top)        \
    schedule(static) if(points_count > 100) aligned(points : 64)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] -= crop_left;
    points[i + 1] -= crop_top;
  }

  return 1;
}

int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points,
                          size_t points_count)
{
  dt_iop_crop_data_t *d = (dt_iop_crop_data_t *)piece->data;

  const float crop_top = piece->buf_in.height * d->cy;
  const float crop_left = piece->buf_in.width * d->cx;

  // nothing to be done if parameters are set to neutral values (no top/left border)
  if(crop_top == 0 && crop_left == 0) return 1;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(points, points_count, crop_left, crop_top)        \
    schedule(static) if(points_count > 100) aligned(points : 64)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] += crop_left;
    points[i + 1] += crop_top;
  }

  return 1;
}

void distort_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                  float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_copy_image_roi(out, in, 1, roi_in, roi_out, TRUE);
}

// 1st pass: how large would the output be, given this input roi?
// this is always called with the full buffer before processing.
void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
  dt_iop_crop_data_t *d = (dt_iop_crop_data_t *)piece->data;

  roi_out->width = roi_in->width * (d->cw - d->cx);
  roi_out->height = roi_in->height * (d->ch - d->cy);
  roi_out->x = roi_in->width * d->cx;
  roi_out->y = roi_in->height * d->cy;

  // sanity check.
  if(roi_out->x < 0) roi_out->x = 0;
  if(roi_out->y < 0) roi_out->y = 0;
  if(roi_out->width < 5) roi_out->width = 5;
  if(roi_out->height < 5) roi_out->height = 5;
}

// 2nd pass: which roi would this operation need as input to fill the given output region?
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  dt_iop_crop_data_t *d = (dt_iop_crop_data_t *)piece->data;
  *roi_in = *roi_out;

  const float iw = piece->buf_in.width * roi_out->scale;
  const float ih = piece->buf_in.height * roi_out->scale;

  roi_in->x += iw * d->cx;
  roi_in->y += ih * d->cy;

  roi_in->x = CLAMP(roi_in->x, 0, (int)floorf(iw));
  roi_in->y = CLAMP(roi_in->y, 0, (int)floorf(ih));
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_copy_image_roi(ovoid, ivoid, 4, roi_in, roi_out, TRUE);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { roi_out->width, roi_out->height, 1 };
  cl_int err = dt_opencl_enqueue_copy_image(piece->pipe->devid, dev_in, dev_out, origin, origin, region);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_crop] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_crop_params_t *p = (dt_iop_crop_params_t *)p1;
  dt_iop_crop_data_t *d = (dt_iop_crop_data_t *)piece->data;

  if(_gui_has_focus(self))
  {
    d->cx = 0.0f;
    d->cy = 0.0f;
    d->cw = 1.0f;
    d->ch = 1.0f;
  }
  else
  {
    d->cx = CLAMPF(p->cx, 0.0f, 0.9f);
    d->cy = CLAMPF(p->cy, 0.0f, 0.9f);
    d->cw = CLAMPF(p->cw, 0.1f, 1.0f);
    d->ch = CLAMPF(p->ch, 0.1f, 1.0f);
  }
}


static void _event_preview_updated_callback(gpointer instance, dt_iop_module_t *self)
{
  dt_iop_crop_gui_data_t *g = (dt_iop_crop_gui_data_t *)self->gui_data;
  if(!g) return; // seems that sometimes, g can be undefined for some reason...
  g->preview_ready = TRUE;
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_event_preview_updated_callback), self);

  // force max size to be recomputed
  g->clip_max_pipe_hash = 0;
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  darktable.develop->history_postpone_invalidate = in && dt_dev_modulegroups_get_activated(darktable.develop) != DT_MODULEGROUP_BASICS;

  dt_iop_crop_gui_data_t *g = (dt_iop_crop_gui_data_t *)self->gui_data;
  dt_iop_crop_params_t *p = (dt_iop_crop_params_t *)self->params;
  if(self->enabled)
  {
    // once the pipe is recomputed, we want to update final sizes
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                                    G_CALLBACK(_event_preview_updated_callback), self);
    if(in)
    {
      // got focus, grab stuff to gui:
      // need to get gui stuff for the first time for this image,
      g->clip_x = CLAMPF(p->cx, 0.0f, 0.9f);
      g->clip_y = CLAMPF(p->cy, 0.0f, 0.9f);
      g->clip_w = CLAMPF(p->cw - p->cx, 0.1f, 1.0f - g->clip_x);
      g->clip_h = CLAMPF(p->ch - p->cy, 0.1f, 1.0f - g->clip_y);
      g->preview_ready = FALSE;
    }
    else if(g->preview_ready)
    {
      // hack : commit_box use distort_transform routines with gui values to get params
      // but this values are accurate only if crop is the gui_module...
      // so we temporary put back gui_module to crop and revert once finished
      dt_iop_module_t *old_gui = self->dev->gui_module;
      self->dev->gui_module = self;
      _commit_box(self, g, p);
      self->dev->gui_module = old_gui;
      g->clip_max_pipe_hash = 0;
    }
  }
  else if(in)
    g->preview_ready = TRUE;

  g->focus_time = g_get_monotonic_time();
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_crop_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

static float _aspect_ratio_get(dt_iop_module_t *self, GtkWidget *combo)
{
  dt_iop_crop_params_t *p = (dt_iop_crop_params_t *)self->params;

  // retrieve full image dimensions to calculate aspect ratio if "original image" specified
  const char *text = dt_bauhaus_combobox_get_text(combo);
  if(text && !g_strcmp0(text, _("Original image")))
  {
    int proc_iwd = 0, proc_iht = 0;
    dt_dev_get_processed_size(darktable.develop, &proc_iwd, &proc_iht);

    if(!(proc_iwd > 0 && proc_iht > 0)) return 0.0f;

    if((p->ratio_d > 0 && proc_iwd > proc_iht) || (p->ratio_d < 0 && proc_iwd < proc_iht))
      return (float)proc_iwd / (float)proc_iht;
    else
      return (float)proc_iht / (float)proc_iwd;
  }

  // we want to know the size of the actual buffer
  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
  if(!piece) return 0.0f;

  const int iwd = piece->buf_in.width, iht = piece->buf_in.height;

  // if we do not have yet computed the aspect ratio, let's do it now
  if(p->ratio_d == -2 && p->ratio_n == -2)
  {
    if(p->cw == 1.0 && p->cx == 0.0 && p->ch == 1.0 && p->cy == 0.0)
    {
      p->ratio_d = -1;
      p->ratio_n = -1;
    }
    else
    {
      const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
      const float whratio = ((float)(iwd - 2 * interpolation->width) * (p->cw - p->cx))
                            / ((float)(iht - 2 * interpolation->width) * (p->ch - p->cy));
      const float ri = (float)iwd / (float)iht;

      const float prec = 0.0003f;
      if(fabsf(whratio - 3.0f / 2.0f) < prec)
      {
        p->ratio_d = 3;
        p->ratio_n = 2;
      }
      else if(fabsf(whratio - 2.0f / 1.0f) < prec)
      {
        p->ratio_d = 2;
        p->ratio_n = 1;
      }
      else if(fabsf(whratio - 7.0f / 5.0f) < prec)
      {
        p->ratio_d = 7;
        p->ratio_n = 5;
      }
      else if(fabsf(whratio - 4.0f / 3.0f) < prec)
      {
        p->ratio_d = 4;
        p->ratio_n = 3;
      }
      else if(fabsf(whratio - 5.0f / 4.0f) < prec)
      {
        p->ratio_d = 5;
        p->ratio_n = 4;
      }
      else if(fabsf(whratio - 1.0f / 1.0f) < prec)
      {
        p->ratio_d = 1;
        p->ratio_n = 1;
      }
      else if(fabsf(whratio - 16.0f / 9.0f) < prec)
      {
        p->ratio_d = 16;
        p->ratio_n = 9;
      }
      else if(fabsf(whratio - 16.0f / 10.0f) < prec)
      {
        p->ratio_d = 16;
        p->ratio_n = 10;
      }
      else if(fabsf(whratio - 244.5f / 203.2f) < prec)
      {
        p->ratio_d = 2445;
        p->ratio_n = 2032;
      }
      else if(fabsf(whratio - sqrtf(2.0f)) < prec)
      {
        p->ratio_d = 14142136;
        p->ratio_n = 10000000;
      }
      else if(fabsf(whratio - PHI) < prec)
      {
        p->ratio_d = 16180340;
        p->ratio_n = 10000000;
      }
      else if(fabsf(whratio - ri) < prec)
      {
        p->ratio_d = 1;
        p->ratio_n = 0;
      }
      else
      {
        p->ratio_d = 0;
        p->ratio_n = 0;
      }
    }
  }

  if(p->ratio_d == 0 && p->ratio_n == 0) return -1.0f;
  float d = 1.0f, n = 1.0f;
  if(p->ratio_n == 0)
  {
    d = copysignf(iwd, p->ratio_d);
    n = iht;
  }
  else
  {
    d = p->ratio_d;
    n = p->ratio_n;
  }

  // make aspect ratios like 3:2 and 2:3 to be the same thing
  const float dn = copysignf(MAX(fabsf(d), fabsf(n)), d);
  const float nn = copysignf(MIN(fabsf(d), fabsf(n)), n);

  if(dn < 0)
    return -nn / dn;
  else
    return dn / nn;
}

static void _aspect_apply(dt_iop_module_t *self, _grab_region_t grab)
{
  dt_iop_crop_gui_data_t *g = (dt_iop_crop_gui_data_t *)self->gui_data;

  int iwd, iht;
  dt_dev_get_processed_size(darktable.develop, &iwd, &iht);

  // enforce aspect ratio.
  float aspect = _aspect_ratio_get(self, g->aspect_presets);

  // since one rarely changes between portrait and landscape by cropping,
  // long side of the crop box should match the long side of the image.
  if(iwd < iht) aspect = 1.0f / aspect;

  if(aspect > 0)
  {
    // if only one side changed, force aspect by two adjacent in equal parts
    // 1 2 4 8 : x y w h
    double clip_x = MAX(iwd * g->clip_x / (float)iwd, 0.0f);
    double clip_y = MAX(iht * g->clip_y / (float)iht, 0.0f);
    double clip_w = MIN(iwd * g->clip_w / (float)iwd, 1.0f);
    double clip_h = MIN(iht * g->clip_h / (float)iht, 1.0f);

    // if we only modified one dim, respectively, we wanted these values:
    const double target_h = (double)iwd * g->clip_w / ((double)iht * aspect);
    const double target_w = (double)iht * g->clip_h * aspect / (double)iwd;
    // i.e. target_w/h = w/target_h = aspect
    // first fix aspect ratio:

    // corners: move two adjacent
    if(grab == GRAB_TOP_LEFT)
    {
      // move x y
      clip_x = clip_x + clip_w - (target_w + clip_w) * .5;
      clip_y = clip_y + clip_h - (target_h + clip_h) * .5;
      clip_w = (target_w + clip_w) * .5;
      clip_h = (target_h + clip_h) * .5;
    }
    else if(grab == GRAB_TOP_RIGHT) // move y w
    {
      clip_y = clip_y + clip_h - (target_h + clip_h) * .5;
      clip_w = (target_w + clip_w) * .5;
      clip_h = (target_h + clip_h) * .5;
    }
    else if(grab == GRAB_BOTTOM_RIGHT) // move w h
    {
      clip_w = (target_w + clip_w) * .5;
      clip_h = (target_h + clip_h) * .5;
    }
    else if(grab == GRAB_BOTTOM_LEFT) // move h x
    {
      clip_h = (target_h + clip_h) * .5;
      clip_x = clip_x + clip_w - (target_w + clip_w) * .5;
      clip_w = (target_w + clip_w) * .5;
    }
    else if(grab & GRAB_HORIZONTAL) // dragged either x or w (1 4)
    {
      // change h and move y, h equally
      const double off = target_h - clip_h;
      clip_h = clip_h + off;
      clip_y = clip_y - .5 * off;
    }
    else if(grab & GRAB_VERTICAL) // dragged either y or h (2 8)
    {
      // change w and move x, w equally
      const double off = target_w - clip_w;
      clip_w = clip_w + off;
      clip_x = clip_x - .5 * off;
    }
    // now fix outside boxes:
    if(clip_x < g->clip_max_x)
    {
      const double prev_clip_h = clip_h;
      clip_h *= (clip_w + clip_x - g->clip_max_x) / clip_w;
      clip_w = clip_w + clip_x - g->clip_max_x;
      clip_x = g->clip_max_x;
      if(grab & GRAB_TOP) clip_y += prev_clip_h - clip_h;
    }
    if(clip_y < g->clip_max_y)
    {
      const double prev_clip_w = clip_w;
      clip_w *= (clip_h + clip_y - g->clip_max_y) / clip_h;
      clip_h = clip_h + clip_y - g->clip_max_y;
      clip_y = g->clip_max_y;
      if(grab & GRAB_LEFT) clip_x += prev_clip_w - clip_w;
    }
    if(clip_x + clip_w > g->clip_max_x + g->clip_max_w)
    {
      const double prev_clip_h = clip_h;
      clip_h *= (g->clip_max_x + g->clip_max_w - clip_x) / clip_w;
      clip_w = g->clip_max_x + g->clip_max_w - clip_x;
      if(grab & GRAB_TOP) clip_y += prev_clip_h - clip_h;
    }
    if(clip_y + clip_h > g->clip_max_y + g->clip_max_h)
    {
      const double prev_clip_w = clip_w;
      clip_w *= (g->clip_max_y + g->clip_max_h - clip_y) / clip_h;
      clip_h = g->clip_max_y + g->clip_max_h - clip_y;
      if(grab & GRAB_LEFT) clip_x += prev_clip_w - clip_w;
    }
    g->clip_x = fmaxf(clip_x, 0.0f);
    g->clip_y = fmaxf(clip_y, 0.0f);
    g->clip_w = fminf(clip_w, 1.0f);
    g->clip_h = fminf(clip_h, 1.0f);
  }
}

void reload_defaults(dt_iop_module_t *self)
{
  const dt_image_t *img = &self->dev->image_storage;

  dt_iop_crop_params_t *d = (dt_iop_crop_params_t *)self->default_params;

  d->cx = img->usercrop[1];
  d->cy = img->usercrop[0];
  d->cw = img->usercrop[3];
  d->ch = img->usercrop[2];
}

static void _float_to_fract(const char *num, int *n, int *d)
{
  char tnum[100];
  gboolean sep_found = FALSE;
  char *p = (char *)num;
  int k = 0;

  *d = 1;

  while(*p)
  {
    if(sep_found) *d *= 10;

    // look for decimal sep
    if(!sep_found && ((*p == ',') || (*p == '.')))
    {
      sep_found = TRUE;
    }
    else if(*p < '0' || *p > '9')
    {
      *n = *d = 0;
      return;
    }
    else
    {
      tnum[k++] = *p;
    }

    p++;
  }

  tnum[k] = '\0';

  *n = atoi(tnum);
}

static void _event_aspect_presets_changed(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_crop_gui_data_t *g = (dt_iop_crop_gui_data_t *)self->gui_data;
  dt_iop_crop_params_t *p = (dt_iop_crop_params_t *)self->params;
  const int which = dt_bauhaus_combobox_get(combo);
  int d = abs(p->ratio_d), n = p->ratio_n;
  const char *text = dt_bauhaus_combobox_get_text(combo);
  if(which < 0)
  {
    if(text)
    {
      const char *c = text;
      const char *end = text + strlen(text);
      while(*c != ':' && *c != '/' && c < end) c++;
      if(c < end - 1)
      {
        // input the exact fraction
        c++;
        const int dd = atoi(text);
        const int nn = atoi(c);
        // some sanity check
        if(nn == 0 || dd == 0)
        {
          dt_control_log(_("Invalid ratio format. It should be \"number:number\""));
          dt_bauhaus_combobox_set(combo, 0);
          return;
        }
        d = MAX(dd, nn);
        n = MIN(dd, nn);
      }
      else
      {
        // find the closest fraction from the input ratio
        int nn = 0, dd = 0;
        _float_to_fract(text, &nn, &dd);

        // some sanity check
        if(dd == 0 || nn == 0)
        {
          dt_control_log(_("Invalid ratio format. It should be a positive number"));
          dt_bauhaus_combobox_set(combo, 0);
          return;
        }

        d = MAX(dd, nn);
        n = MIN(dd, nn);
      }

      // simplify the fraction with binary GCD - https://en.wikipedia.org/wiki/Greatest_common_divisor
      // search g and d such that g is odd and gcd(nn, dd) = g × 2^d
      int e = 0;
      int nn = abs(n);
      int dd = abs(d);
      while((nn % 2 == 0) && (dd % 2 == 0))
      {
        nn /= 2;
        dd /= 2;
        e++;
      }
      while(nn != dd)
      {
        if(nn % 2 == 0)
          nn /= 2;
        else if(dd % 2 == 0)
          dd /= 2;
        else if(nn > dd)
          nn = (nn - dd) / 2;
        else
          dd = (dd - nn) / 2;
      }

      // reduce the fraction with the GCD
      n /= (nn * 1 << e);
      d /= (nn * 1 << e);
    }
  }
  else
  {
    d = n = 0;

    for(const GList *iter = g->aspect_list; iter; iter = g_list_next(iter))
    {
      const dt_iop_crop_aspect_t *aspect = iter->data;
      if(g_strcmp0(aspect->name, text) == 0)
      {
        d = aspect->d;
        n = aspect->n;
        break;
      }
    }
  }

  // now we save all that if it has changed
  if(d != abs(p->ratio_d) || n != p->ratio_n)
  {
    if(p->ratio_d >= 0)
      p->ratio_d = d;
    else
      p->ratio_d = -d;

    p->ratio_n = n;
    dt_conf_set_int("plugins/darkroom/crop/ratio_d", abs(p->ratio_d));
    dt_conf_set_int("plugins/darkroom/crop/ratio_n", abs(p->ratio_n));
    if(darktable.gui->reset) return;
    _aspect_apply(self, GRAB_HORIZONTAL);
    dt_control_queue_redraw_center();
  }

  // Search if current aspect ratio matches something known
  int act = -1, i = 0;

  for(const GList *iter = g->aspect_list; iter; iter = g_list_next(iter))
  {
    const dt_iop_crop_aspect_t *aspect = iter->data;
    if((aspect->d == d) && (aspect->n == n))
    {
      act = i;
      break;
    }
    i++;
  }

  // Update combobox label
  ++darktable.gui->reset;

  if(act == -1)
  {
    // we got a custom ratio
    char str[128];
    snprintf(str, sizeof(str), "%d:%d %2.2f", abs(p->ratio_d), abs(p->ratio_n),
             (float)abs(p->ratio_d) / (float)abs(p->ratio_n));
    dt_bauhaus_combobox_set_text(g->aspect_presets, str);
  }
  else if(dt_bauhaus_combobox_get(g->aspect_presets) != act)
    // we got a default ratio
    dt_bauhaus_combobox_set(g->aspect_presets, act);

  --darktable.gui->reset;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_crop_gui_data_t *g = (dt_iop_crop_gui_data_t *)self->gui_data;
  dt_iop_crop_params_t *p = (dt_iop_crop_params_t *)self->params;

  ++darktable.gui->reset;

  if(w == g->cx)
  {
    g->clip_w = g->clip_x + g->clip_w - p->cx;
    g->clip_x = p->cx;
    _aspect_apply(self, GRAB_LEFT);
  }
  else if(w == g->cw)
  {
    g->clip_w = p->cw - g->clip_x;
    _aspect_apply(self, GRAB_RIGHT);
  }
  else if(w == g->cy)
  {
    g->clip_h = g->clip_y + g->clip_h - p->cy;
    g->clip_y = p->cy;
    _aspect_apply(self, GRAB_TOP);
  }
  else if(w == g->ch)
  {
    g->clip_h = p->ch - g->clip_y;
    _aspect_apply(self, GRAB_BOTTOM);
  }

  // update all sliders, as their values may have change to keep aspect ratio
  dt_bauhaus_slider_set(g->cx, g->clip_x);
  dt_bauhaus_slider_set_soft_min(g->cw, g->clip_x + 0.10);
  dt_bauhaus_slider_set(g->cy, g->clip_y);
  dt_bauhaus_slider_set_soft_min(g->ch, g->clip_y + 0.10);
  dt_bauhaus_slider_set(g->cw, g->clip_x + g->clip_w);
  dt_bauhaus_slider_set_soft_max(g->cx, g->clip_x + g->clip_w - 0.10);
  dt_bauhaus_slider_set(g->ch, g->clip_y + g->clip_h);
  dt_bauhaus_slider_set_soft_max(g->cy, g->clip_y + g->clip_h - 0.10);

  --darktable.gui->reset;

  _commit_box(self, g, p);
}

void gui_reset(struct dt_iop_module_t *self)
{
  /* reset aspect preset to default */
  dt_conf_set_int("plugins/darkroom/crop/ratio_d", 0);
  dt_conf_set_int("plugins/darkroom/crop/ratio_n", 0);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_crop_gui_data_t *g = (dt_iop_crop_gui_data_t *)self->gui_data;
  dt_iop_crop_params_t *p = (dt_iop_crop_params_t *)self->params;

  //  set aspect ratio based on the current image, if not found let's default
  //  to free aspect.

  if(p->ratio_d == -2 && p->ratio_n == -2) _aspect_ratio_get(self, g->aspect_presets);

  if(p->ratio_d == -1 && p->ratio_n == -1)
  {
    p->ratio_d = dt_conf_get_int("plugins/darkroom/crop/ratio_d");
    p->ratio_n = dt_conf_get_int("plugins/darkroom/crop/ratio_n");
  }

  const int d = abs(p->ratio_d), n = p->ratio_n;

  int act = -1;
  int i = 0;
  for(const GList *iter = g->aspect_list; iter; iter = g_list_next(iter))
  {
    const dt_iop_crop_aspect_t *aspect = iter->data;
    if((aspect->d == d) && (aspect->n == n))
    {
      act = i;
      break;
    }
    i++;
  }

  /* special handling the combobox when current act is already selected
     callback is not called, let do it our self then..
   */
  if(act == -1)
  {
    char str[128];
    snprintf(str, sizeof(str), "%d:%d %2.2f", abs(p->ratio_d), abs(p->ratio_n),
             (float)abs(p->ratio_d) / (float)abs(p->ratio_n));
    dt_bauhaus_combobox_set_text(g->aspect_presets, str);
  }
  if(dt_bauhaus_combobox_get(g->aspect_presets) == act)
    _event_aspect_presets_changed(g->aspect_presets, self);
  else
    dt_bauhaus_combobox_set(g->aspect_presets, act);

  // reset gui draw box to what we have in the parameters:
  g->clip_x = p->cx;
  g->clip_w = p->cw - p->cx;
  g->clip_y = p->cy;
  g->clip_h = p->ch - p->cy;

  dt_gui_update_collapsible_section(&g->cs);
}

static void _event_key_swap(dt_iop_module_t *self)
{
  dt_iop_crop_params_t *p = (dt_iop_crop_params_t *)self->params;
  p->ratio_d = -p->ratio_d;
  _aspect_apply(self, GRAB_HORIZONTAL);
  dt_control_queue_redraw_center();
}

static void _event_commit_clicked(GtkButton *button, dt_iop_module_t *self)
{
  dt_iop_crop_gui_data_t *g = (dt_iop_crop_gui_data_t *)self->gui_data;
  dt_iop_crop_params_t *p = (dt_iop_crop_params_t *)self->params;
  _commit_box(self, g, p);
}

static void _event_aspect_flip(GtkWidget *button, dt_iop_module_t *self)
{
  _event_key_swap(self);
}

static gint _aspect_ratio_cmp(const dt_iop_crop_aspect_t *a, const dt_iop_crop_aspect_t *b)
{
  // want most square at the end, and the most non-square at the beginning

  if((a->d == 0 || a->d == 1) && a->n == 0) return -1;

  const float ad = MAX(a->d, a->n);
  const float an = MIN(a->d, a->n);
  const float bd = MAX(b->d, b->n);
  const float bn = MIN(b->d, b->n);
  const float aratio = ad / an;
  const float bratio = bd / bn;

  if(aratio < bratio) return -1;

  const float prec = 0.0003f;
  if(fabsf(aratio - bratio) < prec) return 0;

  return 1;
}

static gchar *_aspect_format(gchar *original, int adim, int bdim)
{
  // Special ratios:  freehand, original image
  if(bdim == 0) return g_strdup(original);

  return g_strdup_printf("%s  %4.2f", original, (float)adim / (float)bdim);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_crop_gui_data_t *g = IOP_GUI_ALLOC(crop);

  g->aspect_list = NULL;
  g->clip_x = g->clip_y = g->handle_x = g->handle_y = 0.0;
  g->clip_w = g->clip_h = 1.0;
  g->clip_max_x = g->clip_max_y = 0.0;
  g->clip_max_w = g->clip_max_h = 1.0;
  g->clip_max_pipe_hash = 0;
  g->cropping = GRAB_CENTER;
  g->shift_hold = FALSE;
  g->ctrl_hold = FALSE;
  g->preview_ready = FALSE;

  GtkWidget *box_enabled = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  dt_iop_crop_aspect_t aspects[] = {
    { _("Freehand"), 0, 0 },
    { _("Original image"), 1, 0 },
    { _("Square"), 1, 1 },
    { _("10:8 in print"), 2445, 2032 },
    { _("5:4, 4x5, 8x10"), 5, 4 },
    { _("11x14"), 14, 11 },
    { _("8.5x11, letter"), 110, 85 },
    { _("4:3, VGA, TV"), 4, 3 },
    { _("5x7"), 7, 5 },
    { _("ISO 216, DIN 476, A4"), 14142136, 10000000 },
    { _("3:2, 4x6, 35mm"), 3, 2 },
    { _("16:10, 8x5"), 16, 10 },
    { _("Golden cut"), 16180340, 10000000 },
    { _("16:9, HDTV"), 16, 9 },
    { _("Widescreen"), 185, 100 },
    { _("2:1, univisium"), 2, 1 },
    { _("Cinemascope"), 235, 100 },
    { _("21:9"), 237, 100 },
    { _("Anamorphic"), 239, 100 },
    { _("3:1, panorama"), 300, 100 },
  };

  const int aspects_count = sizeof(aspects) / sizeof(dt_iop_crop_aspect_t);

  for(int i = 0; i < aspects_count; i++)
  {
    dt_iop_crop_aspect_t *aspect = g_malloc(sizeof(dt_iop_crop_aspect_t));
    aspect->name = _aspect_format(aspects[i].name, aspects[i].d, aspects[i].n);
    aspect->d = aspects[i].d;
    aspect->n = aspects[i].n;
    g->aspect_list = g_list_append(g->aspect_list, aspect);
  }

  // add custom presets from config to the list
  GSList *custom_aspects = dt_conf_all_string_entries("plugins/darkroom/clipping/extra_aspect_ratios");
  for(GSList *iter = custom_aspects; iter; iter = g_slist_next(iter))
  {
    dt_conf_string_entry_t *nv = (dt_conf_string_entry_t *)iter->data;

    const char *c = nv->value;
    const char *end = nv->value + strlen(nv->value);
    while(*c != ':' && *c != '/' && c < end) c++;
    if(c < end - 1)
    {
      c++;
      int d = atoi(nv->value);
      int n = atoi(c);
      // some sanity check
      if(n == 0 || d == 0)
      {
        fprintf(stderr, "invalid ratio format for `%s'. it should be \"number:number\"\n", nv->key);
        dt_control_log(_("Invalid ratio format for `%s'. It should be \"number:number\""), nv->key);
        continue;
      }
      dt_iop_crop_aspect_t *aspect = g_malloc(sizeof(dt_iop_crop_aspect_t));
      aspect->name = _aspect_format(nv->key, d, n);
      aspect->d = d;
      aspect->n = n;
      g->aspect_list = g_list_append(g->aspect_list, aspect);
    }
    else
    {
      fprintf(stderr, "invalid ratio format for `%s'. it should be \"number:number\"\n", nv->key);
      dt_control_log(_("Invalid ratio format for `%s'. It should be \"number:number\""), nv->key);
      continue;
    }
  }
  g_slist_free_full(custom_aspects, dt_conf_string_entry_free);


  g->aspect_list = g_list_sort(g->aspect_list, (GCompareFunc)_aspect_ratio_cmp);

  // remove duplicates from the aspect ratio list
  int d = ((dt_iop_crop_aspect_t *)g->aspect_list->data)->d + 1,
      n = ((dt_iop_crop_aspect_t *)g->aspect_list->data)->n + 1;
  for(GList *iter = g->aspect_list; iter; iter = g_list_next(iter))
  {
    dt_iop_crop_aspect_t *aspect = (dt_iop_crop_aspect_t *)iter->data;
    int dd = MIN(aspect->d, aspect->n);
    int nn = MAX(aspect->d, aspect->n);
    if(dd == d && nn == n)
    {
      // same as the last one, remove this entry
      g_free(aspect->name);
      GList *prev = g_list_previous(iter);
      g->aspect_list = g_list_delete_link(g->aspect_list, iter);
      // it should never be NULL as the 1st element can't be a duplicate, but better safe than sorry
      iter = prev ? prev : g->aspect_list;
    }
    else
    {
      d = dd;
      n = nn;
    }
  }

  g->aspect_presets = dt_bauhaus_combobox_new(self);
  dt_bauhaus_combobox_set_editable(g->aspect_presets, 1);
  dt_bauhaus_widget_set_label(g->aspect_presets, NULL, N_("Aspect"));

  for(GList *iter = g->aspect_list; iter; iter = g_list_next(iter))
  {
    const dt_iop_crop_aspect_t *aspect = iter->data;
    dt_bauhaus_combobox_add(g->aspect_presets, aspect->name);
  }

  dt_bauhaus_combobox_set(g->aspect_presets, 0);

  g_signal_connect(G_OBJECT(g->aspect_presets), "value-changed", G_CALLBACK(_event_aspect_presets_changed), self);
  gtk_widget_set_tooltip_text(g->aspect_presets, _("Set the aspect ratio\n"
                                                   "the list is sorted: from most square to least square\n"
                                                   "to enter custom aspect ratio open the combobox and type ratio in x:y or decimal format"));
  dt_bauhaus_widget_set_quad_paint(g->aspect_presets, dtgtk_cairo_paint_aspectflip, 0, NULL);
  g_signal_connect(G_OBJECT(g->aspect_presets), "quad-pressed", G_CALLBACK(_event_aspect_flip), self);
  gtk_box_pack_start(GTK_BOX(box_enabled), g->aspect_presets, TRUE, TRUE, 0);

  GtkWidget *commit = dt_action_button_new((dt_lib_module_t *)self, N_("Commit"), _event_commit_clicked, self, _("Commit changes"), GDK_KEY_Return, 0);
  gtk_box_pack_end(GTK_BOX(box_enabled), commit, TRUE, TRUE, 0);

  // we put margins values under an expander
  dt_gui_new_collapsible_section
    (&g->cs,
     "plugins/darkroom/crop/expand_margins",
     _("Margins"),
     GTK_BOX(box_enabled));

  self->widget = GTK_WIDGET(g->cs.container);

  g->cx = dt_bauhaus_slider_from_params(self, "cx");
  dt_bauhaus_slider_set_digits(g->cx, 4);
  dt_bauhaus_slider_set_format(g->cx, "%");
  gtk_widget_set_tooltip_text(g->cx, _("The left margin cannot overlap with the right margin"));

  g->cw = dt_bauhaus_slider_from_params(self, "cw");
  dt_bauhaus_slider_set_digits(g->cw, 4);
  dt_bauhaus_slider_set_factor(g->cw, -100.0);
  dt_bauhaus_slider_set_offset(g->cw, 100.0);
  dt_bauhaus_slider_set_format(g->cw, "%");
  gtk_widget_set_tooltip_text(g->cw, _("The right margin cannot overlap with the left margin"));

  g->cy = dt_bauhaus_slider_from_params(self, "cy");
  dt_bauhaus_slider_set_digits(g->cy, 4);
  dt_bauhaus_slider_set_format(g->cy, "%");
  gtk_widget_set_tooltip_text(g->cy, _("The top margin cannot overlap with the bottom margin"));

  g->ch = dt_bauhaus_slider_from_params(self, "ch");
  dt_bauhaus_slider_set_digits(g->ch, 4);
  dt_bauhaus_slider_set_factor(g->ch, -100.0);
  dt_bauhaus_slider_set_offset(g->ch, 100.0);
  dt_bauhaus_slider_set_format(g->ch, "%");
  gtk_widget_set_tooltip_text(g->ch, _("The bottom margin cannot overlap with the top margin"));

  self->widget = box_enabled;
}

static void _aspect_free(gpointer data)
{
  dt_iop_crop_aspect_t *aspect = (dt_iop_crop_aspect_t *)data;
  g_free(aspect->name);
  aspect->name = NULL;
  g_free(aspect);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_crop_gui_data_t *g = (dt_iop_crop_gui_data_t *)self->gui_data;
  g_list_free_full(g->aspect_list, _aspect_free);
  g->aspect_list = NULL;

  IOP_GUI_FREE;
}

static _grab_region_t _gui_get_grab(float pzx, float pzy, dt_iop_crop_gui_data_t *g, const float border,
                                    const float wd, const float ht)
{
  _grab_region_t grab = GRAB_NONE;
  if(!(pzx < g->clip_x || pzx > g->clip_x + g->clip_w || pzy < g->clip_y || pzy > g->clip_y + g->clip_h))
  {
    // we are inside the crop box
    grab = GRAB_CENTER;

    if(pzx >= g->clip_x && pzx * wd < g->clip_x * wd + border)
      grab |= GRAB_LEFT; // left border

    if(pzy >= g->clip_y && pzy * ht < g->clip_y * ht + border)
      grab |= GRAB_TOP;  // top border

    if(pzx <= g->clip_x + g->clip_w && pzx * wd > (g->clip_w + g->clip_x) * wd - border)
      grab |= GRAB_RIGHT; // right border

    if(pzy <= g->clip_y + g->clip_h && pzy * ht > (g->clip_h + g->clip_y) * ht - border)
      grab |= GRAB_BOTTOM; // bottom border
  }
  return grab;
}

// draw guides and handles over the image
void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                     int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  dt_iop_crop_gui_data_t *g = (dt_iop_crop_gui_data_t *)self->gui_data;

  // we don't do anything if the image is not ready
  if(!g->preview_ready) return;

  _aspect_apply(self, GRAB_HORIZONTAL);

  const float wd = dev->preview_pipe->backbuf_width;
  const float ht = dev->preview_pipe->backbuf_height;
  const float zoom_y = dt_control_get_dev_zoom_y();
  const float zoom_x = dt_control_get_dev_zoom_x();
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1 << closeup, 1);

  cairo_translate(cr, width / 2.0, height / 2.0);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  double dashes = DT_PIXEL_APPLY_DPI(5.0) / zoom_scale;

  // draw cropping window
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  if(_set_max_clip(self))
  {
    cairo_set_source_rgba(cr, .2, .2, .2, .8);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, g->clip_max_x * wd, g->clip_max_y * ht,
                        g->clip_max_w * wd, g->clip_max_h * ht);
    cairo_rectangle(cr, g->clip_x * wd, g->clip_y * ht,
                        g->clip_w * wd, g->clip_h * ht);
    cairo_fill(cr);
  }
  if(g->clip_x > .0f || g->clip_y > .0f || g->clip_w < 1.0f || g->clip_h < 1.0f)
  {
    cairo_set_line_width(cr, dashes / 2.0);
    cairo_rectangle(cr, g->clip_x * wd, g->clip_y * ht, g->clip_w * wd, g->clip_h * ht);
    dt_draw_set_color_overlay(cr, TRUE, 1.0);
    cairo_stroke(cr);
  }

  // draw cropping window dimensions if first mouse button is pressed
  if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    char dimensions[16];
    dimensions[0] = '\0';
    PangoLayout *layout;
    PangoRectangle ext;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(16) * PANGO_SCALE / zoom_scale);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);

    int procw, proch;
    dt_dev_get_processed_size(dev, &procw, &proch);
    snprintf(dimensions, sizeof(dimensions), "%i x %i", (int)(procw * g->clip_w), (int)(proch * g->clip_h));

    pango_layout_set_text(layout, dimensions, -1);
    pango_layout_get_pixel_extents(layout, NULL, &ext);
    const float text_w = ext.width;
    const float text_h = DT_PIXEL_APPLY_DPI(16 + 2) / zoom_scale;
    const float margin = DT_PIXEL_APPLY_DPI(6) / zoom_scale;
    float xp = (g->clip_x + g->clip_w * .5f) * wd - text_w * .5f;
    float yp = (g->clip_y + g->clip_h * .5f) * ht - text_h * .5f;

    // ensure that the rendered string remains visible within the window bounds
    double x1, y1, x2, y2;
    cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
    xp = CLAMPF(xp, x1 + 2.0 * margin, x2 - text_w - 2.0 * margin);
    yp = CLAMPF(yp, y1 + 2.0 * margin, y2 - text_h - 2.0 * margin);

    cairo_set_source_rgba(cr, .5, .5, .5, .9);
    dt_gui_draw_rounded_rectangle
      (cr, text_w + 2 * margin, text_h + 2 * margin, xp - margin, yp - margin);
    cairo_set_source_rgb(cr, .7, .7, .7);
    cairo_move_to(cr, xp, yp);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0) / zoom_scale);
  double alpha = CLAMP(1.0 - (g_get_monotonic_time() - g->focus_time) / 2e6f, 0.0, 1.0);
  dt_draw_set_color_overlay(cr, TRUE, alpha);
  const int border = DT_PIXEL_APPLY_DPI(30.0) / zoom_scale;

  cairo_move_to(cr, g->clip_x * wd + border, g->clip_y * ht);
  cairo_line_to(cr, g->clip_x * wd + border, (g->clip_y + g->clip_h) * ht);
  cairo_move_to(cr, (g->clip_x + g->clip_w) * wd - border, g->clip_y * ht);
  cairo_line_to(cr, (g->clip_x + g->clip_w) * wd - border, (g->clip_y + g->clip_h) * ht);
  cairo_move_to(cr, g->clip_x * wd, g->clip_y * ht + border);
  cairo_line_to(cr, (g->clip_x + g->clip_w) * wd, g->clip_y * ht + border);
  cairo_move_to(cr, g->clip_x * wd, (g->clip_y + g->clip_h) * ht - border);
  cairo_line_to(cr, (g->clip_x + g->clip_w) * wd, (g->clip_y + g->clip_h) * ht - border);
  cairo_stroke(cr);

  // draw crop area guides
  dt_guides_draw(cr, g->clip_x * wd, g->clip_y * ht, g->clip_w * wd, g->clip_h * ht, zoom_scale);

  dt_draw_set_color_overlay(cr, TRUE, 1.0);

  const _grab_region_t grab = g->cropping ? g->cropping : _gui_get_grab(pzx, pzy, g, border, wd, ht);

  if(grab == GRAB_LEFT)
    cairo_rectangle(cr, g->clip_x * wd, g->clip_y * ht, border, g->clip_h * ht);
  if(grab == GRAB_TOP)
    cairo_rectangle(cr, g->clip_x * wd, g->clip_y * ht, g->clip_w * wd, border);
  if(grab == GRAB_TOP_LEFT)
    cairo_rectangle(cr, g->clip_x * wd, g->clip_y * ht, border, border);
  if(grab == GRAB_RIGHT)
    cairo_rectangle(cr, (g->clip_x + g->clip_w) * wd - border, g->clip_y * ht, border, g->clip_h * ht);
  if(grab == GRAB_BOTTOM)
    cairo_rectangle(cr, g->clip_x * wd, (g->clip_y + g->clip_h) * ht - border, g->clip_w * wd, border);
  if(grab == GRAB_BOTTOM_RIGHT)
    cairo_rectangle(cr, (g->clip_x + g->clip_w) * wd - border, (g->clip_y + g->clip_h) * ht - border, border,
                    border);
  if(grab == GRAB_TOP_RIGHT)
    cairo_rectangle(cr, (g->clip_x + g->clip_w) * wd - border, g->clip_y * ht, border, border);
  if(grab == GRAB_BOTTOM_LEFT)
    cairo_rectangle(cr, g->clip_x * wd, (g->clip_y + g->clip_h) * ht - border, border, border);
  cairo_stroke(cr);
}

int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  dt_iop_crop_gui_data_t *g = (dt_iop_crop_gui_data_t *)self->gui_data;

  // we don't do anything if the image is not ready
  if(!g->preview_ready || self->dev->preview_loading) return 0;

  const float wd = self->dev->preview_pipe->backbuf_width;
  const float ht = self->dev->preview_pipe->backbuf_height;
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(self->dev, zoom, 1 << closeup, 1);
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  const _grab_region_t grab = _gui_get_grab(pzx, pzy, g, DT_PIXEL_APPLY_DPI(30.0) / zoom_scale, wd, ht);

  _set_max_clip(self);

  if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    // draw a light gray frame, to show it's not stored yet:
    // first mouse button, adjust cropping frame, but what do we do?
    const float bzx = g->button_down_zoom_x + .5f;
    const float bzy = g->button_down_zoom_y + .5f;

    if(g->cropping == GRAB_ALL)
    {
      /* moving the crop window */
      if(!g->shift_hold)
        g->clip_x
          = fminf(g->clip_max_w + g->clip_max_x - g->clip_w,
                  fmaxf(g->clip_max_x, g->handle_x + pzx - bzx));

      if(!g->ctrl_hold)
        g->clip_y
          = fminf(g->clip_max_h + g->clip_max_y - g->clip_h,
                  fmaxf(g->clip_max_y, g->handle_y + pzy - bzy));
    }
    else
    {
      /* changing the crop window */
      if(g->shift_hold)
      {
        /* the center is locked, scale crop radial with locked ratio */
        float xx = 0.0f;
        float yy = 0.0f;

        if(g->cropping & GRAB_LEFT || g->cropping & GRAB_RIGHT)
          xx = (g->cropping & GRAB_LEFT) ? (pzx - bzx) : (bzx - pzx);
        if(g->cropping & GRAB_TOP || g->cropping & GRAB_BOTTOM)
          yy = (g->cropping & GRAB_TOP) ? (pzy - bzy) : (bzy - pzy);

        float ratio = fmaxf((g->prev_clip_w - 2.0f * xx) / g->prev_clip_w,
                            (g->prev_clip_h - 2.0f * yy) / g->prev_clip_h);

        // ensure we don't get too small crop size
        if(g->prev_clip_w * ratio < 0.1f) ratio = 0.1f / g->prev_clip_w;
        if(g->prev_clip_h * ratio < 0.1f) ratio = 0.1f / g->prev_clip_h;

        // ensure we don't have too big crop size
        if(g->prev_clip_w * ratio > g->clip_max_w) ratio = g->clip_max_w / g->prev_clip_w;
        if(g->prev_clip_h * ratio > g->clip_max_h) ratio = g->clip_max_h / g->prev_clip_h;

        // now that we are sure that the crop size is correct, we have to adjust top & left
        float nx = g->prev_clip_x - (g->prev_clip_w * ratio - g->prev_clip_w) / 2.0f;
        float ny = g->prev_clip_y - (g->prev_clip_h * ratio - g->prev_clip_h) / 2.0f;
        float nw = g->prev_clip_w * ratio;
        float nh = g->prev_clip_h * ratio;

        // move crop area to the right if needed
        nx = fmaxf(nx, g->clip_max_x);
        // move crop area to the left if needed
        nx = fminf(nx, g->clip_max_w + g->clip_max_x - nw);
        // move crop area to the bottom if needed
        ny = fmaxf(ny, g->clip_max_y);
        // move crop area to the top if needed
        ny = fminf(ny, g->clip_max_h + g->clip_max_y - nh);

        g->clip_x = nx;
        g->clip_y = ny;
        g->clip_w = nw;
        g->clip_h = nh;
      }
      else
      {
        if(g->cropping & GRAB_LEFT)
        {
          const float old_clip_x = g->clip_x;
          g->clip_x = fminf(fmaxf(g->clip_max_x, pzx - g->handle_x), g->clip_x + g->clip_w - 0.1f);
          g->clip_w = old_clip_x + g->clip_w - g->clip_x;
        }
        if(g->cropping & GRAB_TOP)
        {
          const float old_clip_y = g->clip_y;
          g->clip_y = fminf(fmaxf(g->clip_max_y, pzy - g->handle_y), g->clip_y + g->clip_h - 0.1f);
          g->clip_h = old_clip_y + g->clip_h - g->clip_y;
        }
        if(g->cropping & GRAB_RIGHT)
          g->clip_w = fmaxf(0.1f, fminf(g->clip_max_w + g->clip_max_x, pzx - g->clip_x - g->handle_x));
        if(g->cropping & GRAB_BOTTOM)
          g->clip_h = fmaxf(0.1f, fminf(g->clip_max_h + g->clip_max_y, pzy - g->clip_y - g->handle_y));
      }

      if(g->clip_x + g->clip_w > g->clip_max_w + g->clip_max_x)
        g->clip_w = g->clip_max_w + g->clip_max_x - g->clip_x;
      if(g->clip_y + g->clip_h > g->clip_max_h + g->clip_max_y)
        g->clip_h = g->clip_max_h + g->clip_max_y - g->clip_y;
    }

    _aspect_apply(self, g->cropping);

    // only update the sliders, not the dt_iop_cropping_params_t structure, so that the call to
    // dt_control_queue_redraw_center below doesn't go rerun the pixelpipe because it thinks that
    // the image has changed when it actually hasn't, yet.  The actual clipping parameters get set
    // from the sliders when the iop loses focus, at which time the final selected crop is applied.
    ++darktable.gui->reset;

    dt_bauhaus_slider_set(g->cx, g->clip_x);
    dt_bauhaus_slider_set_soft_min(g->cw, g->clip_x + 0.10);
    dt_bauhaus_slider_set(g->cy, g->clip_y);
    dt_bauhaus_slider_set_soft_min(g->ch, g->clip_y + 0.10);
    dt_bauhaus_slider_set(g->cw, g->clip_x + g->clip_w);
    dt_bauhaus_slider_set_soft_max(g->cx, g->clip_x + g->clip_w - 0.10);
    dt_bauhaus_slider_set(g->ch, g->clip_y + g->clip_h);
    dt_bauhaus_slider_set_soft_max(g->cy, g->clip_y + g->clip_h - 0.10);

    --darktable.gui->reset;

    dt_control_queue_redraw_center();
    return 1;
  }
  else if(grab)
  {
    // hover over active borders, no button pressed
    // change mouse pointer
    if(grab == GRAB_LEFT)
      dt_control_change_cursor(GDK_LEFT_SIDE);
    else if(grab == GRAB_TOP)
      dt_control_change_cursor(GDK_TOP_SIDE);
    else if(grab == GRAB_RIGHT)
      dt_control_change_cursor(GDK_RIGHT_SIDE);
    else if(grab == GRAB_BOTTOM)
      dt_control_change_cursor(GDK_BOTTOM_SIDE);
    else if(grab == GRAB_TOP_LEFT)
      dt_control_change_cursor(GDK_TOP_LEFT_CORNER);
    else if(grab == GRAB_TOP_RIGHT)
      dt_control_change_cursor(GDK_TOP_RIGHT_CORNER);
    else if(grab == GRAB_BOTTOM_RIGHT)
      dt_control_change_cursor(GDK_BOTTOM_RIGHT_CORNER);
    else if(grab == GRAB_BOTTOM_LEFT)
      dt_control_change_cursor(GDK_BOTTOM_LEFT_CORNER);
    else if(grab == GRAB_NONE)
    {
      dt_control_hinter_message(darktable.control, "");
      dt_control_change_cursor(GDK_LEFT_PTR);
    }
    if(grab != GRAB_NONE)
      dt_control_hinter_message(darktable.control, _("<b>resize</b>: drag, <b>keep aspect ratio</b>: shift+drag"));
    dt_control_queue_redraw_center();
  }
  else
  {
    dt_control_change_cursor(GDK_FLEUR);
    g->cropping = GRAB_CENTER;
    dt_control_hinter_message(darktable.control, _("<b>move</b>: drag, <b>move vertically</b>: shift+drag, <b>move horizontally</b>: ctrl+drag"));
    dt_control_queue_redraw_center();
  }
  return 0;
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_iop_crop_gui_data_t *g = (dt_iop_crop_gui_data_t *)self->gui_data;
  dt_iop_crop_params_t *p = (dt_iop_crop_params_t *)self->params;
  // we don't do anything if the image is not ready
  if(!g->preview_ready) return 0;

  /* reset internal ui states*/
  g->shift_hold = FALSE;
  g->ctrl_hold = FALSE;
  g->cropping = GRAB_CENTER;

  dt_control_change_cursor(GDK_LEFT_PTR);

  // we save the crop into the params now so params are kept in synch with gui settings
  _commit_box(self, g, p);
  return 1;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  dt_iop_crop_gui_data_t *g = (dt_iop_crop_gui_data_t *)self->gui_data;
  // we don't do anything if the image is not ready

  if(!g->preview_ready) return 0;

  // avoid unexpected back to lt mode:
  if(type == GDK_2BUTTON_PRESS && which == 1)
    return 1;

  if(which == 1)
  {
    const float wd = self->dev->preview_pipe->backbuf_width;
    const float ht = self->dev->preview_pipe->backbuf_height;
    const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    const int closeup = dt_control_get_dev_closeup();
    const float zoom_scale = dt_dev_get_zoom_scale(self->dev, zoom, 1 << closeup, 1);

    float pzx, pzy;
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);

    // switch module on already, other code depends in this:
    if(!self->enabled) dt_dev_add_history_item(darktable.develop, self, TRUE);

    g->button_down_x = x;
    g->button_down_y = y;

    g->button_down_zoom_x = pzx;
    g->button_down_zoom_y = pzy;

    /* update prev clip box with current */
    g->prev_clip_x = g->clip_x;
    g->prev_clip_y = g->clip_y;
    g->prev_clip_w = g->clip_w;
    g->prev_clip_h = g->clip_h;

    /* if shift is pressed, then lock crop on center */
    if(dt_modifiers_include(state, GDK_SHIFT_MASK)) g->shift_hold = TRUE;
    if(dt_modifiers_include(state, GDK_CONTROL_MASK)) g->ctrl_hold = TRUE;

    /* store grabbed area */

    const float bzx = pzx + .5f;
    const float bzy = pzy + .5f;

    g->cropping = _gui_get_grab(bzx, bzy, g, DT_PIXEL_APPLY_DPI(30.0) / zoom_scale, wd, ht);

    if(g->cropping == GRAB_CENTER)
    {
      g->cropping = GRAB_ALL;
      g->handle_x = g->clip_x;
      g->handle_y = g->clip_y;
    }
    else
    {
      if(g->cropping & GRAB_LEFT)   g->handle_x = bzx - g->clip_x;
      if(g->cropping & GRAB_TOP)    g->handle_y = bzy - g->clip_y;
      if(g->cropping & GRAB_RIGHT)  g->handle_x = bzx - (g->clip_w + g->clip_x);
      if(g->cropping & GRAB_BOTTOM) g->handle_y = bzy - (g->clip_h + g->clip_y);
    }

    return 1;
  }
  else if(which == 3)
  {
    // we reset cropping
    g->clip_x = 0.0f;
    g->clip_y = 0.0f;
    g->clip_w = 1.0f;
    g->clip_h = 1.0f;
    _aspect_apply(self, GRAB_BOTTOM_RIGHT);
    gui_changed(self, NULL, NULL);
    return 1;
  }
  else
    return 0;
}

GSList *mouse_actions(struct dt_iop_module_t *self)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_LEFT_DRAG, 0, _("[%s On borders] Crop"), self->name());
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_LEFT_DRAG, GDK_SHIFT_MASK,
                                     _("[%s On borders] Crop keeping ratio"), self->name());
  return lm;
}

// #undef PHI
// #undef INVPHI

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
