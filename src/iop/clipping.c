/*
    This file is part of darktable,
    Copyright (C) 2009-2021 darktable developers.

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
#include "develop/tiling.h"
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

DT_MODULE_INTROSPECTION(5, dt_iop_clipping_params_t)

/** flip H/V, rotate an image, then clip the buffer. */
typedef enum dt_iop_clipping_flags_t
{
  FLAG_FLIP_HORIZONTAL = 1 << 0,
  FLAG_FLIP_VERTICAL   = 1 << 1
} dt_iop_clipping_flags_t;

typedef struct dt_iop_clipping_aspect_t
{
  char *name;
  int d, n;
} dt_iop_clipping_aspect_t;

typedef struct dt_iop_clipping_params_t
{
  float angle; // $MIN: -180.0 $MAX: 180.0
  float cx;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "left"
  float cy;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "top"
  float cw;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "right"
  float ch;    // $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "bottom"
  float k_h, k_v;
  float kxa;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2
  float kya;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2
  float kxb;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.8
  float kyb;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2
  float kxc;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.8
  float kyc;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.8
  float kxd;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2
  float kyd;   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.8
  int k_type, k_sym;
  int k_apply;   // $DEFAULT: 0
  gboolean crop_auto; // $DEFAULT: TRUE $DESCRIPTION: "automatic cropping"
  int ratio_n;   // $DEFAULT: -1
  int ratio_d;   // $DEFAULT: -1
} dt_iop_clipping_params_t;

typedef enum _grab_region_t
{
  GRAB_CENTER       = 0,                                               // 0
  GRAB_LEFT         = 1 << 0,                                          // 1
  GRAB_TOP          = 1 << 1,                                          // 2
  GRAB_RIGHT        = 1 << 2,                                          // 4
  GRAB_BOTTOM       = 1 << 3,                                          // 8
  GRAB_TOP_LEFT     = GRAB_TOP | GRAB_LEFT,                            // 3
  GRAB_TOP_RIGHT    = GRAB_TOP | GRAB_RIGHT,                           // 6
  GRAB_BOTTOM_RIGHT = GRAB_BOTTOM | GRAB_RIGHT,                        // 12
  GRAB_BOTTOM_LEFT  = GRAB_BOTTOM | GRAB_LEFT,                         // 9
  GRAB_HORIZONTAL   = GRAB_LEFT | GRAB_RIGHT,                          // 5
  GRAB_VERTICAL     = GRAB_TOP | GRAB_BOTTOM,                          // 10
  GRAB_ALL          = GRAB_LEFT | GRAB_TOP | GRAB_RIGHT | GRAB_BOTTOM, // 15
  GRAB_NONE         = 1 << 4                                           // 16
} _grab_region_t;

/* calculate the aspect ratios for current image */
static void keystone_type_populate(struct dt_iop_module_t *self, gboolean with_applied, int select);

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(new_version <= old_version) return 1;
  if(new_version != 5) return 1;

  dt_iop_clipping_params_t *n = (dt_iop_clipping_params_t *)new_params;
  if(old_version == 2 && new_version == 5)
  {
    union {
        float f;
        uint32_t u;
    } k;
    // old structure def
    typedef struct old_params_t
    {
      float angle, cx, cy, cw, ch, k_h, k_v;
    } old_params_t;

    const old_params_t *o = (old_params_t *)old_params;

    k.f = o->k_h;
    int is_horizontal;
    if(k.u & 0x40000000u)
      is_horizontal = 1;
    else
      is_horizontal = 0;
    k.u &= ~0x40000000;
    if(is_horizontal)
    {
      n->k_h = k.f;
      n->k_v = 0.0f;
    }
    else
    {
      n->k_h = 0.0f;
      n->k_v = k.f;
    }

    n->angle = o->angle, n->cx = o->cx, n->cy = o->cy, n->cw = o->cw, n->ch = o->ch;
    n->kxa = n->kxd = 0.2f;
    n->kxc = n->kxb = 0.8f;
    n->kya = n->kyb = 0.2f;
    n->kyc = n->kyd = 0.8f;
    if(n->k_h == 0 && n->k_v == 0)
      n->k_type = 0;
    else
      n->k_type = 4;
    n->k_sym = 0;
    n->k_apply = 0;
    n->crop_auto = 1;

    // will be computed later, -2 here is used to detect uninitialized value, -1 is already used for no
    // clipping.
    n->ratio_d = n->ratio_n = -2;
  }
  if(old_version == 3 && new_version == 5)
  {
    // old structure def
    typedef struct old_params_t
    {
      float angle, cx, cy, cw, ch, k_h, k_v;
    } old_params_t;

    const old_params_t *o = (old_params_t *)old_params;

    n->angle = o->angle, n->cx = o->cx, n->cy = o->cy, n->cw = o->cw, n->ch = o->ch;
    n->k_h = o->k_h, n->k_v = o->k_v;
    n->kxa = n->kxd = 0.2f;
    n->kxc = n->kxb = 0.8f;
    n->kya = n->kyb = 0.2f;
    n->kyc = n->kyd = 0.8f;
    if(n->k_h == 0 && n->k_v == 0)
      n->k_type = 0;
    else
      n->k_type = 4;
    n->k_sym = 0;
    n->k_apply = 0;
    n->crop_auto = 1;

    // will be computed later, -2 here is used to detect uninitialized value, -1 is already used for no
    // clipping.
    n->ratio_d = n->ratio_n = -2;
  }
  if(old_version == 4 && new_version == 5)
  {
    typedef struct old_params_t
    {
      float angle, cx, cy, cw, ch, k_h, k_v;
      float kxa, kya, kxb, kyb, kxc, kyc, kxd, kyd;
      int k_type, k_sym;
      int k_apply, crop_auto;
    } old_params_t;

    const old_params_t *o = (old_params_t *)old_params;

    n->angle = o->angle, n->cx = o->cx, n->cy = o->cy, n->cw = o->cw, n->ch = o->ch;
    n->k_h = o->k_h, n->k_v = o->k_v;
    n->kxa = o->kxa, n->kxb = o->kxb, n->kxc = o->kxc, n->kxd = o->kxd;
    n->kya = o->kya, n->kyb = o->kyb, n->kyc = o->kyc, n->kyd = o->kyd;
    n->k_type = o->k_type;
    n->k_sym = o->k_sym;
    n->k_apply = o->k_apply;
    n->crop_auto = o->crop_auto;

    // will be computed later, -2 here is used to detect uninitialized value, -1 is already used for no
    // clipping.
    n->ratio_d = n->ratio_n = -2;
  }

  return 0;
}

typedef struct dt_iop_clipping_gui_data_t
{
  GtkNotebook *notebook;

  GtkWidget *angle;
  GtkWidget *cx,*cy,*cw,*ch;
  GtkWidget *hvflip;

  GList *aspect_list;
  GtkWidget *aspect_presets;

  GtkWidget *keystone_type;
  GtkWidget *crop_auto;

  float button_down_x, button_down_y;
  float button_down_zoom_x, button_down_zoom_y,
      button_down_angle; // position in image where the button has been pressed.
  /* current clip box */
  float clip_x, clip_y, clip_w, clip_h, handle_x, handle_y;
  /* last box before change */
  float prev_clip_x, prev_clip_y, prev_clip_w, prev_clip_h;
  /* maximum clip box */
  float clip_max_x, clip_max_y, clip_max_w, clip_max_h;
  uint64_t clip_max_pipe_hash;

 int k_selected, k_show, k_selected_segment;
  gboolean k_drag;

  int cropping, straightening, applied;
  gboolean shift_hold;
  gboolean ctrl_hold;
  gboolean preview_ready;
} dt_iop_clipping_gui_data_t;

typedef struct dt_iop_clipping_data_t
{
  float angle;              // rotation angle
  float aspect;             // forced aspect ratio
  float m[4];               // rot/mirror matrix
  float inv_m[4];           // inverse of m (m^-1)
  float ki_h, k_h;          // keystone correction, ki and corrected k
  float ki_v, k_v;          // keystone correction, ki and corrected k
  float tx, ty;             // rotation center
  float cx, cy, cw, ch;     // crop window
  float cix, ciy;           // crop window on roi_out 1.0 scale
  uint32_t all_off;         // 1: v and h off, else one of them is used
  uint32_t flags;           // flipping flags
  uint32_t flip;            // flipped output buffer so more area would fit.

  dt_boundingbox_t k_space; // space for the "destination" rectangle of the keystone quadrilateral
  float kxa, kya, kxb, kyb, kxc, kyc, kxd,
      kyd; // point of the "source" quadrilatere (modified if keystone is not "full")
  float a, b, d, e, g, h; // value of the transformation matrix (c=f=0 && i=1)
  int k_apply;
  int crop_auto;
  float enlarge_x, enlarge_y;
} dt_iop_clipping_data_t;

typedef struct dt_iop_clipping_global_data_t
{
  int kernel_clip_rotate_bilinear;
  int kernel_clip_rotate_bicubic;
  int kernel_clip_rotate_lanczos2;
  int kernel_clip_rotate_lanczos3;
} dt_iop_clipping_global_data_t;

static void commit_box(dt_iop_module_t *self, dt_iop_clipping_gui_data_t *g, dt_iop_clipping_params_t *p);


// helper to count corners in for loops:
static inline void get_corner(const float *aabb, const int i, float *p)
{
  for(int k = 0; k < 2; k++) p[k] = aabb[2 * ((i >> k) & 1) + k];
}

static inline void adjust_aabb(const float *p, float *aabb)
{
  aabb[0] = fminf(aabb[0], p[0]);
  aabb[1] = fminf(aabb[1], p[1]);
  aabb[2] = fmaxf(aabb[2], p[0]);
  aabb[3] = fmaxf(aabb[3], p[1]);
}

const char *deprecated_msg()
{
  return _("this module is deprecated. please use the crop, orientation and/or rotate and perspective modules instead.");
}

const char *name()
{
  return _("crop and rotate");
}

const char *aliases()
{
  return _("reframe|perspective|keystone|distortion");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("change the framing and correct the perspective"),
                                      _("corrective or creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("geometric, RGB"),
                                      _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_ALLOW_FAST_PIPE
         | IOP_FLAGS_GUIDES_SPECIAL_DRAW | IOP_FLAGS_GUIDES_WIDGET | IOP_FLAGS_DEPRECATED;
}

int operation_tags()
{
  return IOP_TAG_DISTORT | IOP_TAG_CLIPPING;
}

int operation_tags_filter()
{
  // switch off watermark, it gets confused.
  return IOP_TAG_DECORATION | IOP_TAG_CLIPPING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static int gui_has_focus(struct dt_iop_module_t *self)
{
  return (self->dev->gui_module == self
          && dt_dev_modulegroups_get_activated(darktable.develop) != DT_MODULEGROUP_BASICS);
}

static void keystone_get_matrix(const dt_boundingbox_t k_space, float kxa, float kxb, float kxc, float kxd, float kya,
                                float kyb, float kyc, float kyd, float *a, float *b, float *d, float *e,
                                float *g, float *h)
{
  *a = -((kxb * (kyd * kyd - kyc * kyd) - kxc * kyd * kyd + kyb * (kxc * kyd - kxd * kyd) + kxd * kyc * kyd)
         * k_space[2])
       / (kxb * (kxc * kyd * kyd - kxd * kyc * kyd) + kyb * (kxd * kxd * kyc - kxc * kxd * kyd));
  *b = ((kxb * (kxd * kyd - kxd * kyc) - kxc * kxd * kyd + kxd * kxd * kyc + (kxc * kxd - kxd * kxd) * kyb)
        * k_space[2])
       / (kxb * (kxc * kyd * kyd - kxd * kyc * kyd) + kyb * (kxd * kxd * kyc - kxc * kxd * kyd));
  *d = (kyb * (kxb * (kyd * k_space[3] - kyc * k_space[3]) - kxc * kyd * k_space[3] + kxd * kyc * k_space[3])
        + kyb * kyb * (kxc * k_space[3] - kxd * k_space[3]))
       / (kxb * kyb * (-kxc * kyd - kxd * kyc) + kxb * kxb * kyc * kyd + kxc * kxd * kyb * kyb);
  *e = -(kxb * (kxd * kyc * k_space[3] - kxc * kyd * k_space[3])
         + kxb * kxb * (kyd * k_space[3] - kyc * k_space[3])
         + kxb * kyb * (kxc * k_space[3] - kxd * k_space[3]))
       / (kxb * kyb * (-kxc * kyd - kxd * kyc) + kxb * kxb * kyc * kyd + kxc * kxd * kyb * kyb);
  *g = -(kyb * (kxb * (2.0f * kxc * kyd * kyd - 2.0f * kxc * kyc * kyd) - kxc * kxc * kyd * kyd
                + 2.0f * kxc * kxd * kyc * kyd - kxd * kxd * kyc * kyc)
         + kxb * kxb * (kyc * kyc * kyd - kyc * kyd * kyd)
         + kyb * kyb * (-2.0f * kxc * kxd * kyd + kxc * kxc * kyd + kxd * kxd * kyc))
       / (kxb * kxb * (kxd * kyc * kyc * kyd - kxc * kyc * kyd * kyd)
          + kxb * kyb * (kxc * kxc * kyd * kyd - kxd * kxd * kyc * kyc)
          + kyb * kyb * (kxc * kxd * kxd * kyc - kxc * kxc * kxd * kyd));
  *h = (kxb * (-kxc * kxc * kyd * kyd + 2.0f * kxc * kxd * kyc * kyd - kxd * kxd * kyc * kyc)
        + kxb * kxb * (kxc * kyd * kyd - 2.0f * kxd * kyc * kyd + kxd * kyc * kyc)
        + kxb * (2.0f * kxd * kxd - 2.0f * kxc * kxd) * kyb * kyc
        + (kxc * kxc * kxd - kxc * kxd * kxd) * kyb * kyb)
       / (kxb * kxb * (kxd * kyc * kyc * kyd - kxc * kyc * kyd * kyd)
          + kxb * kyb * (kxc * kxc * kyd * kyd - kxd * kxd * kyc * kyc)
          + kyb * kyb * (kxc * kxd * kxd * kyc - kxc * kxc * kxd * kyd));
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void keystone_backtransform(float *i, const dt_boundingbox_t k_space, float a, float b, float d,
                                          float e, float g, float h, float kxa, float kya)
{
  const float xx = i[0] - k_space[0];
  const float yy = i[1] - k_space[1];

  const float div = ((d * xx - a * yy) * h + (b * yy - e * xx) * g + a * e - b * d);

  i[0] = (e * xx - b * yy) / div + kxa;
  i[1] = -(d * xx - a * yy) / div + kya;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void keystone_transform(float *i, const dt_boundingbox_t k_space, float a, float b, float d,
                                      float e, float g, float h, float kxa, float kya)
{
  const float xx = i[0] - kxa;
  const float yy = i[1] - kya;

  const float div = g * xx + h * yy + 1;
  i[0] = (a * xx + b * yy) / div + k_space[0];
  i[1] = (d * xx + e * yy) / div + k_space[1];
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void backtransform(float *x, float *o, const float *m, const float t_h, const float t_v)
{
  x[1] /= (1.0f + x[0] * t_h);
  x[0] /= (1.0f + x[1] * t_v);
  mul_mat_vec_2(m, x, o);
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void inv_matrix(float *m, float *inv_m)
{
  const float det = (m[0] * m[3]) - (m[1] * m[2]);
  inv_m[0] =  m[3] / det;
  inv_m[1] = -m[1] / det;
  inv_m[2] = -m[2] / det;
  inv_m[3] =  m[0] / det;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void transform(float *x, float *o, const float *m, const float t_h, const float t_v)
{
  mul_mat_vec_2(m, x, o);
  o[1] *= (1.0f + o[0] * t_h);
  o[0] *= (1.0f + o[1] * t_v);
}

int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points, size_t points_count)
{
  // as dt_iop_roi_t contain int values and not floats, we can have some rounding errors
  // as a workaround, we use a factor for preview pipes
  float factor = 1.0f;
  if(piece->pipe->type & (DT_DEV_PIXELPIPE_PREVIEW | DT_DEV_PIXELPIPE_PREVIEW2)) factor = 100.0f;
  // we first need to be sure that all data values are computed
  // this is done in modify_roi_out fct, so we create tmp roi
  dt_iop_roi_t roi_out, roi_in;
  roi_in.width = piece->buf_in.width * factor;
  roi_in.height = piece->buf_in.height * factor;
  self->modify_roi_out(self, piece, &roi_out, &roi_in);

  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;

  const float rx = piece->buf_in.width;
  const float ry = piece->buf_in.height;

  const dt_boundingbox_t k_space = { d->k_space[0] * rx, d->k_space[1] * ry, d->k_space[2] * rx, d->k_space[3] * ry };
  const float kxa = d->kxa * rx, kxb = d->kxb * rx, kxc = d->kxc * rx, kxd = d->kxd * rx;
  const float kya = d->kya * ry, kyb = d->kyb * ry, kyc = d->kyc * ry, kyd = d->kyd * ry;
  float ma = 0, mb = 0, md = 0, me = 0, mg = 0, mh = 0;
  if(d->k_apply == 1)
    keystone_get_matrix(k_space, kxa, kxb, kxc, kxd, kya, kyb, kyc, kyd, &ma, &mb, &md, &me, &mg, &mh);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(points_count, points, d, factor, k_space, ma, mb, md, me, mg, mh, kxa, kya) \
    schedule(static) if(points_count > 100)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    float pi[2], po[2];
    pi[0] = points[i];
    pi[1] = points[i + 1];

    if(d->k_apply == 1) keystone_transform(pi, k_space, ma, mb, md, me, mg, mh, kxa, kya);

    pi[0] -= d->tx / factor;
    pi[1] -= d->ty / factor;
    // transform this point using matrix m
    transform(pi, po, d->inv_m, d->k_h, d->k_v);

    if(d->flip)
    {
      po[1] += d->tx / factor;
      po[0] += d->ty / factor;
    }
    else
    {
      po[0] += d->tx / factor;
      po[1] += d->ty / factor;
    }

    points[i] = po[0] - (d->cix - d->enlarge_x) / factor;
    points[i + 1] = po[1] - (d->ciy - d->enlarge_y) / factor;
  }

  // revert side-effects of the previous call to modify_roi_out
  // TODO: this is just a quick hack. we need a major revamp of this module!
  if(factor != 1.0f)
  {
    roi_in.width = piece->buf_in.width;
    roi_in.height = piece->buf_in.height;
    self->modify_roi_out(self, piece, &roi_out, &roi_in);
  }

  return 1;
}
int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points,
                          size_t points_count)
{
  // as dt_iop_roi_t contain int values and not floats, we can have some rounding errors
  // as a workaround, we use a factor for preview pipes
  float factor = 1.0f;
  if(piece->pipe->type & (DT_DEV_PIXELPIPE_PREVIEW | DT_DEV_PIXELPIPE_PREVIEW2)) factor = 100.0f;
  // we first need to be sure that all data values are computed
  // this is done in modify_roi_out fct, so we create tmp roi
  dt_iop_roi_t roi_out, roi_in;
  roi_in.width = piece->buf_in.width * factor;
  roi_in.height = piece->buf_in.height * factor;
  self->modify_roi_out(self, piece, &roi_out, &roi_in);

  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;

  const float rx = piece->buf_in.width;
  const float ry = piece->buf_in.height;

  const dt_boundingbox_t k_space = { d->k_space[0] * rx, d->k_space[1] * ry, d->k_space[2] * rx, d->k_space[3] * ry };
  const float kxa = d->kxa * rx, kxb = d->kxb * rx, kxc = d->kxc * rx, kxd = d->kxd * rx;
  const float kya = d->kya * ry, kyb = d->kyb * ry, kyc = d->kyc * ry, kyd = d->kyd * ry;
  float ma, mb, md, me, mg, mh;
  if(d->k_apply == 1)
    keystone_get_matrix(k_space, kxa, kxb, kxc, kxd, kya, kyb, kyc, kyd, &ma, &mb, &md, &me, &mg, &mh);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(points_count, points, d, factor, k_space, ma, mb, md, me, mg, mh, kxa, kya) \
    schedule(static) if(points_count > 100) aligned(points:64) aligned(k_space:16)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    float pi[2], po[2];
    pi[0] = -(d->enlarge_x - d->cix) / factor + points[i];
    pi[1] = -(d->enlarge_y - d->ciy) / factor + points[i + 1];

    // transform this point using matrix m
    if(d->flip)
    {
      pi[1] -= d->tx / factor;
      pi[0] -= d->ty / factor;
    }
    else
    {
      pi[0] -= d->tx / factor;
      pi[1] -= d->ty / factor;
    }

    backtransform(pi, po, d->m, d->k_h, d->k_v);

    po[0] += d->tx / factor;
    po[1] += d->ty / factor;
    if(d->k_apply == 1) keystone_backtransform(po, k_space, ma, mb, md, me, mg, mh, kxa, kya);

    points[i] = po[0];
    points[i + 1] = po[1];
  }

  // revert side-effects of the previous call to modify_roi_out
  // TODO: this is just a quick hack. we need a major revamp of this module!
  if(factor != 1.0f)
  {
    roi_in.width = piece->buf_in.width;
    roi_in.height = piece->buf_in.height;
    self->modify_roi_out(self, piece, &roi_out, &roi_in);
  }

  return 1;
}

void distort_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                  float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;

  // only crop, no rot fast and sharp path:
  if(!d->flags && d->angle == 0.0 && d->all_off && roi_in->width == roi_out->width && roi_in->height == roi_out->height)
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);
  }
  else
  {
    const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    const float rx = piece->buf_in.width * roi_in->scale;
    const float ry = piece->buf_in.height * roi_in->scale;
    const dt_boundingbox_t k_space = { d->k_space[0] * rx, d->k_space[1] * ry, d->k_space[2] * rx, d->k_space[3] * ry };
    const float kxa = d->kxa * rx, kxb = d->kxb * rx, kxc = d->kxc * rx, kxd = d->kxd * rx;
    const float kya = d->kya * ry, kyb = d->kyb * ry, kyc = d->kyc * ry, kyd = d->kyd * ry;
    float ma, mb, md, me, mg, mh;
    if(d->k_apply == 1)
      keystone_get_matrix(k_space, kxa, kxb, kxc, kxd, kya, kyb, kyc, kyd, &ma, &mb, &md, &me, &mg, &mh);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, kxa, kya, out, roi_in, roi_out) \
    dt_omp_sharedconst(k_space)                             \
    shared(d, interpolation, ma, mb, md, me, mg, mh) \
    schedule(static)
#endif
    // (slow) point-by-point transformation.
    // TODO: optimize with scanlines and linear steps between?
    for(int j = 0; j < roi_out->height; j++)
    {
      float *_out = out + (size_t)j * roi_out->width;
      for(int i = 0; i < roi_out->width; i++)
      {
        float pi[2] = { 0.0f }, po[2] = { 0.0f };

        pi[0] = roi_out->x - roi_out->scale * d->enlarge_x + roi_out->scale * d->cix + i + 0.5f;
        pi[1] = roi_out->y - roi_out->scale * d->enlarge_y + roi_out->scale * d->ciy + j + 0.5f;

        // transform this point using matrix m
        if(d->flip)
        {
          pi[1] -= d->tx * roi_out->scale;
          pi[0] -= d->ty * roi_out->scale;
        }
        else
        {
          pi[0] -= d->tx * roi_out->scale;
          pi[1] -= d->ty * roi_out->scale;
        }
        pi[0] /= roi_out->scale;
        pi[1] /= roi_out->scale;
        backtransform(pi, po, d->m, d->k_h, d->k_v);
        po[0] *= roi_in->scale;
        po[1] *= roi_in->scale;
        po[0] += d->tx * roi_in->scale;
        po[1] += d->ty * roi_in->scale;
        if(d->k_apply == 1) keystone_backtransform(po, k_space, ma, mb, md, me, mg, mh, kxa, kya);
        po[0] -= roi_in->x + 0.5f;
        po[1] -= roi_in->y + 0.5f;

        _out[i] = dt_interpolation_compute_sample(interpolation, in, po[0], po[1], roi_in->width, roi_in->height, 1,
                                                  roi_in->width);
      }
    }
  }
}

static int _iop_clipping_set_max_clip(struct dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

  if(g->clip_max_pipe_hash == self->dev->preview_pipe->backbuf_hash) return 1;

  // we want to know the size of the actual buffer
  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
  if(!piece) return 0;

  float wp = piece->buf_out.width, hp = piece->buf_out.height;
  const float cx = CLAMPF(p->cx, 0.0f, 0.9f);
  const float cy = CLAMPF(p->cy, 0.0f, 0.9f);
  const float cw = CLAMPF(fabsf(p->cw), 0.1f, 1.0f);
  const float ch = CLAMPF(fabsf(p->ch), 0.1f, 1.0f);

  float points[8] = { 0.0f, 0.0f, wp, hp, cx * wp, cy * hp, cw * wp, ch * hp };
  if(!dt_dev_distort_transform_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, points, 4))
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

// 1st pass: how large would the output be, given this input roi?
// this is always called with the full buffer before processing.
void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in_orig)
{
  dt_iop_roi_t roi_in_d = *roi_in_orig;
  dt_iop_roi_t *roi_in = &roi_in_d;

  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;

  // use whole-buffer roi information to create matrix and inverse.
  float rt[] = { cosf(d->angle), sinf(d->angle), -sinf(d->angle), cosf(d->angle) };
  if(d->angle == 0.0f)
  {
    rt[0] = rt[3] = 1.0f;
    rt[1] = rt[2] = 0.0f;
  }

  for(int k = 0; k < 4; k++) d->m[k] = rt[k];
  if(d->flags & FLAG_FLIP_HORIZONTAL)
  {
    d->m[0] = -rt[0];
    d->m[2] = -rt[2];
  }
  if(d->flags & FLAG_FLIP_VERTICAL)
  {
    d->m[1] = -rt[1];
    d->m[3] = -rt[3];
  }

  // now compute inverse of m
  inv_matrix(d->m, d->inv_m);

  if(d->k_apply == 0 && d->crop_auto == 1) // this is the old solution.
  {
    float inv_rt[4] = { 0.0f };
    inv_matrix(rt, inv_rt);

    *roi_out = *roi_in;
    // correct keystone correction factors by resolution of this buffer
    const float kc = 1.0f / fminf(roi_in->width, roi_in->height);
    d->k_h = d->ki_h * kc;
    d->k_v = d->ki_v * kc;

    float cropscale = -1.0f;
    // check portrait/landscape orientation, whichever fits more area:
    const float oaabb[4]
        = { -.5f * roi_in->width, -.5f * roi_in->height, .5f * roi_in->width, .5f * roi_in->height };
    for(int flip = 0; flip < 2; flip++)
    {
      const float roi_in_width = flip ? roi_in->height : roi_in->width;
      const float roi_in_height = flip ? roi_in->width : roi_in->height;
      float newcropscale = 1.0f;
      // fwd transform rotated points on corners and scale back inside roi_in bounds.
      float p[2], o[2],
          aabb[4] = { -.5f * roi_in_width, -.5f * roi_in_height, .5f * roi_in_width, .5f * roi_in_height };
      for(int c = 0; c < 4; c++)
      {
        get_corner(oaabb, c, p);
        transform(p, o, inv_rt, d->k_h, d->k_v);
        for(int k = 0; k < 2; k++)
          if(fabsf(o[k]) > 0.001f) newcropscale = fminf(newcropscale, aabb[(o[k] > 0 ? 2 : 0) + k] / o[k]);
      }
      if(newcropscale >= cropscale)
      {
        cropscale = newcropscale;
        // remember rotation center in whole-buffer coordinates:
        d->tx = roi_in->width * .5f;
        d->ty = roi_in->height * .5f;
        d->flip = flip;

        float ach = d->ch - d->cy, acw = d->cw - d->cx;
        // rotate and clip to max extent
        if(flip)
        {
          roi_out->y = d->tx - (.5f - d->cy) * cropscale * roi_in->width;
          roi_out->x = d->ty - (.5f - d->cx) * cropscale * roi_in->height;
          roi_out->height = ach * cropscale * roi_in->width;
          roi_out->width = acw * cropscale * roi_in->height;
        }
        else
        {
          roi_out->x = d->tx - (.5f - d->cx) * cropscale * roi_in->width;
          roi_out->y = d->ty - (.5f - d->cy) * cropscale * roi_in->height;
          roi_out->width = acw * cropscale * roi_in->width;
          roi_out->height = ach * cropscale * roi_in->height;
        }
      }
    }
  }
  else
  {
    *roi_out = *roi_in;
    // set roi_out values with rotation and keystone
    // initial corners pos
    dt_boundingbox_t corn_x = { 0.0f, roi_in->width, roi_in->width, 0.0f };
    dt_boundingbox_t corn_y = { 0.0f, 0.0f, roi_in->height, roi_in->height };
    // destination corner points
    dt_boundingbox_t corn_out_x = { 0.0f };
    dt_boundingbox_t corn_out_y = { 0.0f };

    // we don't test image flip as autocrop is not completely ok...
    d->flip = 0;

    // we apply rotation and keystone to all those points
    float p[2], o[2];
    for(int c = 0; c < 4; c++)
    {
      // keystone
      o[0] = corn_x[c];
      o[1] = corn_y[c];
      if(d->k_apply == 1)
      {
        o[0] /= (float)roi_in->width;
        o[1] /= (float)roi_in->height;
        keystone_transform(o, d->k_space, d->a, d->b, d->d, d->e, d->g, d->h, d->kxa, d->kya);
        o[0] *= roi_in->width;
        o[1] *= roi_in->height;
      }
      // rotation
      p[0] = o[0] - .5f * roi_in->width;
      p[1] = o[1] - .5f * roi_in->height;
      transform(p, o, d->inv_m, d->k_h, d->k_v);
      o[0] += .5f * roi_in->width;
      o[1] += .5f * roi_in->height;

      // and we set the values
      corn_out_x[c] = o[0];
      corn_out_y[c] = o[1];
    }

    float new_x = fminf(fminf(fminf(corn_out_x[0], corn_out_x[1]), corn_out_x[2]), corn_out_x[3]);
    if(new_x + roi_in->width < 0) new_x = -roi_in->width;
    float new_y = fminf(fminf(fminf(corn_out_y[0], corn_out_y[1]), corn_out_y[2]), corn_out_y[3]);
    if(new_y + roi_in->height < 0) new_y = -roi_in->height;

    float new_sc_x = fmaxf(fmaxf(fmaxf(corn_out_x[0], corn_out_x[1]), corn_out_x[2]), corn_out_x[3]);
    if(new_sc_x > 2.0f * roi_in->width) new_sc_x = 2.0f * roi_in->width;
    float new_sc_y = fmaxf(fmaxf(fmaxf(corn_out_y[0], corn_out_y[1]), corn_out_y[2]), corn_out_y[3]);
    if(new_sc_y > 2.0f * roi_in->height) new_sc_y = 2.0f * roi_in->height;

    // be careful, we don't want too small area here !
    if(new_sc_x - new_x < roi_in->width / 8.0f)
    {
      float f = (new_sc_x + new_x) / 2.0f;
      if(f < roi_in->width / 16.0f) f = roi_in->width / 16.0f;
      if(f >= roi_in->width * 15.0f / 16.0f) f = roi_in->width * 15.0f / 16.0f - 1.0f;
      new_x = f - roi_in->width / 16.0f, new_sc_x = f + roi_in->width / 16.0f;
    }
    if(new_sc_y - new_y < roi_in->height / 8.0f)
    {
      float f = (new_sc_y + new_y) / 2.0f;
      if(f < roi_in->height / 16.0f) f = roi_in->height / 16.0f;
      if(f >= roi_in->height * 15.0f / 16.0f) f = roi_in->height * 15.0f / 16.0f - 1.0f;
      new_y = f - roi_in->height / 16.0f, new_sc_y = f + roi_in->height / 16.0f;
    }

    new_sc_y = new_sc_y - new_y;
    new_sc_x = new_sc_x - new_x;

    // now we apply the clipping
    new_x += d->cx * new_sc_x;
    new_y += d->cy * new_sc_y;
    new_sc_x *= d->cw - d->cx;
    new_sc_y *= d->ch - d->cy;

    d->enlarge_x = fmaxf(-new_x, 0.0f);
    roi_out->x = fmaxf(new_x, 0.0f);
    d->enlarge_y = fmaxf(-new_y, 0.0f);
    roi_out->y = fmaxf(new_y, 0.0f);

    roi_out->width = new_sc_x;
    roi_out->height = new_sc_y;
    d->tx = roi_in->width * .5f;
    d->ty = roi_in->height * .5f;
  }

  // sanity check.
  if(roi_out->x < 0) roi_out->x = 0;
  if(roi_out->y < 0) roi_out->y = 0;
  if(roi_out->width < 1) roi_out->width = 1;
  if(roi_out->height < 1) roi_out->height = 1;

  // save rotation crop on output buffer in world scale:
  d->cix = roi_out->x;
  d->ciy = roi_out->y;
}

// 2nd pass: which roi would this operation need as input to fill the given output region?
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  *roi_in = *roi_out;
  // modify_roi_out took care of bounds checking for us. we hopefully do not get requests outside the clipping
  // area.
  // transform aabb back to roi_in

  // this aabb is set off by cx/cy
  const float so = roi_out->scale;
  const float kw = piece->buf_in.width * so, kh = piece->buf_in.height * so;
  const float roi_out_x = roi_out->x - d->enlarge_x * so, roi_out_y = roi_out->y - d->enlarge_y * so;
  float p[2], o[2];
  dt_boundingbox_t aabb = { roi_out_x + d->cix * so, roi_out_y + d->ciy * so, roi_out_x + d->cix * so + roi_out->width,
                  roi_out_y + d->ciy * so + roi_out->height };
  dt_boundingbox_t aabb_in = { INFINITY, INFINITY, -INFINITY, -INFINITY };
  for(int c = 0; c < 4; c++)
  {
    // get corner points of roi_out
    get_corner(aabb, c, p);

    // backtransform aabb using m
    if(d->flip)
    {
      p[1] -= d->tx * so;
      p[0] -= d->ty * so;
    }
    else
    {
      p[0] -= d->tx * so;
      p[1] -= d->ty * so;
    }
    p[0] *= 1.0 / so;
    p[1] *= 1.0 / so;
    backtransform(p, o, d->m, d->k_h, d->k_v);
    o[0] *= so;
    o[1] *= so;
    o[0] += d->tx * so;
    o[1] += d->ty * so;
    o[0] /= kw;
    o[1] /= kh;
    if(d->k_apply == 1)
      keystone_backtransform(o, d->k_space, d->a, d->b, d->d, d->e, d->g, d->h, d->kxa, d->kya);
    o[0] *= kw;
    o[1] *= kh;
    // transform to roi_in space, get aabb.
    adjust_aabb(o, aabb_in);
  }

  // adjust roi_in to minimally needed region
  roi_in->x = aabb_in[0] - 1;
  roi_in->y = aabb_in[1] - 1;
  roi_in->width = aabb_in[2] - aabb_in[0] + 2;
  roi_in->height = aabb_in[3] - aabb_in[1] + 2;

  if(d->angle == 0.0f && d->all_off)
  {
    // just crop: make sure everything is precise.
    roi_in->x = aabb_in[0];
    roi_in->y = aabb_in[1];
    roi_in->width = roi_out->width;
    roi_in->height = roi_out->height;
  }

  // sanity check.
  const float scwidth = piece->buf_in.width * so, scheight = piece->buf_in.height * so;
  roi_in->x = CLAMP(roi_in->x, 0, (int)floorf(scwidth));
  roi_in->y = CLAMP(roi_in->y, 0, (int)floorf(scheight));
  roi_in->width = CLAMP(roi_in->width, 1, (int)ceilf(scwidth) - roi_in->x);
  roi_in->height = CLAMP(roi_in->height, 1, (int)ceilf(scheight) - roi_in->y);
}

// 3rd (final) pass: you get this input region (may be different from what was requested above),
// do your best to fill the output region!
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4/*need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return; // unsupported format, image has been copied to output and module's trouble flag set

  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;

  const int ch = 4;
  const int ch_width = ch * roi_in->width;

  // only crop, no rot fast and sharp path:
  if(!d->flags && d->angle == 0.0 && d->all_off && roi_in->width == roi_out->width
     && roi_in->height == roi_out->height)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
  }
  else
  {
    const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    const float rx = piece->buf_in.width * roi_in->scale;
    const float ry = piece->buf_in.height * roi_in->scale;
    const dt_boundingbox_t k_space = { d->k_space[0] * rx, d->k_space[1] * ry, d->k_space[2] * rx, d->k_space[3] * ry };
    const float kxa = d->kxa * rx, kxb = d->kxb * rx, kxc = d->kxc * rx, kxd = d->kxd * rx;
    const float kya = d->kya * ry, kyb = d->kyb * ry, kyc = d->kyc * ry, kyd = d->kyd * ry;
    float ma, mb, md, me, mg, mh;
    if(d->k_apply == 1)
      keystone_get_matrix(k_space, kxa, kxb, kxc, kxd, kya, kyb, kyc, kyd, &ma, &mb, &md, &me, &mg, &mh);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ch, ch_width, ivoid, kxa, kya, ovoid, roi_in, roi_out) \
    dt_omp_sharedconst(k_space) \
    shared(d, interpolation, ma, mb, md, me, mg, mh) \
    schedule(static)
#endif
    // (slow) point-by-point transformation.
    // TODO: optimize with scanlines and linear steps between?
    for(int j = 0; j < roi_out->height; j++)
    {
      float *out = ((float *)ovoid) + (size_t)ch * j * roi_out->width;
      for(int i = 0; i < roi_out->width; i++)
      {
        float pi[2], po[2];

        pi[0] = roi_out->x - roi_out->scale * d->enlarge_x + roi_out->scale * d->cix + i + 0.5f;
        pi[1] = roi_out->y - roi_out->scale * d->enlarge_y + roi_out->scale * d->ciy + j + 0.5f;

        // transform this point using matrix m
        if(d->flip)
        {
          pi[1] -= d->tx * roi_out->scale;
          pi[0] -= d->ty * roi_out->scale;
        }
        else
        {
          pi[0] -= d->tx * roi_out->scale;
          pi[1] -= d->ty * roi_out->scale;
        }
        pi[0] /= roi_out->scale;
        pi[1] /= roi_out->scale;
        backtransform(pi, po, d->m, d->k_h, d->k_v);
        po[0] *= roi_in->scale;
        po[1] *= roi_in->scale;
        po[0] += d->tx * roi_in->scale;
        po[1] += d->ty * roi_in->scale;
        if(d->k_apply == 1) keystone_backtransform(po, k_space, ma, mb, md, me, mg, mh, kxa, kya);
        po[0] -= roi_in->x + 0.5f;
        po[1] -= roi_in->y + 0.5f;

        dt_interpolation_compute_pixel4c(interpolation, (float *)ivoid, out + ch*i, po[0], po[1], roi_in->width,
                                         roi_in->height, ch_width);
      }
    }
  }
}



#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  dt_iop_clipping_global_data_t *gd = (dt_iop_clipping_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;

  // only crop, no rot fast and sharp path:
  if(!d->flags && d->angle == 0.0 && d->all_off && roi_in->width == roi_out->width
     && roi_in->height == roi_out->height)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    int crkernel = -1;

    const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

    switch(interpolation->id)
    {
      case DT_INTERPOLATION_BILINEAR:
        crkernel = gd->kernel_clip_rotate_bilinear;
        break;
      case DT_INTERPOLATION_BICUBIC:
        crkernel = gd->kernel_clip_rotate_bicubic;
        break;
      case DT_INTERPOLATION_LANCZOS2:
        crkernel = gd->kernel_clip_rotate_lanczos2;
        break;
      case DT_INTERPOLATION_LANCZOS3:
        crkernel = gd->kernel_clip_rotate_lanczos3;
        break;
      default:
        return FALSE;
    }

    int roi[2] = { roi_in->x, roi_in->y };
    float roo[2] = { roi_out->x - roi_out->scale * d->enlarge_x + roi_out->scale * d->cix,
                     roi_out->y - roi_out->scale * d->enlarge_y + roi_out->scale * d->ciy };
    float t[2] = { d->tx, d->ty };
    float k[2] = { d->k_h, d->k_v };
    float m[4] = { d->m[0], d->m[1], d->m[2], d->m[3] };

    float k_sizes[2] = { piece->buf_in.width * roi_in->scale, piece->buf_in.height * roi_in->scale };
    const dt_boundingbox_t k_space = { d->k_space[0] * k_sizes[0], d->k_space[1] * k_sizes[1],
                                       d->k_apply ? d->k_space[2] * k_sizes[0] : 0.0f,
                                       d->k_space[3] * k_sizes[1] };
    float ma, mb, md, me, mg, mh;
    keystone_get_matrix(k_space, d->kxa * k_sizes[0], d->kxb * k_sizes[0], d->kxc * k_sizes[0],
                        d->kxd * k_sizes[0], d->kya * k_sizes[1], d->kyb * k_sizes[1], d->kyc * k_sizes[1],
                        d->kyd * k_sizes[1], &ma, &mb, &md, &me, &mg, &mh);
    const float ka[2] = { d->kxa * k_sizes[0], d->kya * k_sizes[1] };
    const float maa[4] = { ma, mb, md, me };
    const float mbb[2] = { mg, mh };

    size_t sizes[3];

    sizes[0] = ROUNDUPDWD(width, devid);
    sizes[1] = ROUNDUPDHT(height, devid);
    sizes[2] = 1;
    dt_opencl_set_kernel_args(devid, crkernel, 0, CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height),
      CLARG(roi_in->width), CLARG(roi_in->height), CLARG(roi), CLARG(roo), CLARG(roi_in->scale), CLARG(roi_out->scale),
      CLARG(d->flip), CLARG(t), CLARG(k), CLARG(m), CLARG(k_space), CLARG(ka), CLARG(maa), CLARG(mbb));
    err = dt_opencl_enqueue_kernel_2d(devid, crkernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_clipping] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl from programs.conf
  dt_iop_clipping_global_data_t *gd
      = (dt_iop_clipping_global_data_t *)malloc(sizeof(dt_iop_clipping_global_data_t));
  module->data = gd;
  gd->kernel_clip_rotate_bilinear = dt_opencl_create_kernel(program, "clip_rotate_bilinear");
  gd->kernel_clip_rotate_bicubic = dt_opencl_create_kernel(program, "clip_rotate_bicubic");
  gd->kernel_clip_rotate_lanczos2 = dt_opencl_create_kernel(program, "clip_rotate_lanczos2");
  gd->kernel_clip_rotate_lanczos3 = dt_opencl_create_kernel(program, "clip_rotate_lanczos3");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_clipping_global_data_t *gd = (dt_iop_clipping_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_clip_rotate_bilinear);
  dt_opencl_free_kernel(gd->kernel_clip_rotate_bicubic);
  dt_opencl_free_kernel(gd->kernel_clip_rotate_lanczos2);
  dt_opencl_free_kernel(gd->kernel_clip_rotate_lanczos3);
  free(module->data);
  module->data = NULL;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)p1;
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;

  // reset all values to be sure everything is initialized
  d->m[0] = d->m[3] = 1.0f;
  d->m[1] = d->m[2] = 0.0f;
  d->ki_h = d->ki_v = d->k_h = d->k_v = 0.0f;
  d->tx = d->ty = 0.0f;
  d->cix = d->ciy = 0.0f;
  d->kxa = d->kxd = d->kya = d->kyb = 0.0f;
  d->kxb = d->kxc = d->kyc = d->kyd = 0.6f;
  d->k_space[0] = d->k_space[1] = 0.2f;
  d->k_space[2] = d->k_space[3] = 0.6f;
  d->k_apply = 0;
  d->enlarge_x = d->enlarge_y = 0.0f;
  d->flip = 0;
  d->angle = M_PI / 180.0 * p->angle;

  // image flip
  d->flags = (p->ch < 0 ? FLAG_FLIP_VERTICAL : 0) | (p->cw < 0 ? FLAG_FLIP_HORIZONTAL : 0);
  d->crop_auto = p->crop_auto;

  // keystones values computation
  if(p->k_type == 4)
  {
    // this is for old keystoning
    d->k_apply = 0;
    d->all_off = 1;
    if(fabsf(p->k_h) >= .0001) d->all_off = 0;
    if(p->k_h >= -1.0 && p->k_h <= 1.0)
      d->ki_h = p->k_h;
    else
      d->ki_h = 0.0f;
    if(fabsf(p->k_v) >= .0001) d->all_off = 0;
    if(p->k_v >= -1.0 && p->k_v <= 1.0)
      d->ki_v = p->k_v;
    else
      d->ki_v = 0.0f;
  }
  else if(p->k_type >= 0 && p->k_apply == 1)
  {
    // we reset old keystoning values
    d->ki_h = d->ki_v = 0;
    d->kxa = p->kxa;
    d->kxb = p->kxb;
    d->kxc = p->kxc;
    d->kxd = p->kxd;
    d->kya = p->kya;
    d->kyb = p->kyb;
    d->kyc = p->kyc;
    d->kyd = p->kyd;
    // we adjust the points if the keystoning is not in "full" mode
    if(p->k_type == 1) // we want horizontal points to be aligned
    {
      // line equations parameters
      float a1 = (d->kxd - d->kxa) / (d->kyd - d->kya);
      float b1 = d->kxa - a1 * d->kya;
      float a2 = (d->kxc - d->kxb) / (d->kyc - d->kyb);
      float b2 = d->kxb - a2 * d->kyb;

      if(d->kya > d->kyb)
      {
        // we move kya to the level of kyb
        d->kya = d->kyb;
        d->kxa = a1 * d->kya + b1;
      }
      else
      {
        // we move kyb to the level of kya
        d->kyb = d->kya;
        d->kxb = a2 * d->kyb + b2;
      }

      if(d->kyc > d->kyd)
      {
        // we move kyd to the level of kyc
        d->kyd = d->kyc;
        d->kxd = a1 * d->kyd + b1;
      }
      else
      {
        // we move kyc to the level of kyd
        d->kyc = d->kyd;
        d->kxc = a2 * d->kyc + b2;
      }
    }
    else if(p->k_type == 2) // we want vertical points to be aligned
    {
      // line equations parameters
      const float a1 = (d->kyb - d->kya) / (d->kxb - d->kxa);
      const float b1 = d->kya - a1 * d->kxa;
      const float a2 = (d->kyc - d->kyd) / (d->kxc - d->kxd);
      const float b2 = d->kyd - a2 * d->kxd;

      if(d->kxa > d->kxd)
      {
        // we move kxa to the level of kxd
        d->kxa = d->kxd;
        d->kya = a1 * d->kxa + b1;
      }
      else
      {
        // we move kyb to the level of kya
        d->kxd = d->kxa;
        d->kyd = a2 * d->kxd + b2;
      }

      if(d->kxc > d->kxb)
      {
        // we move kyd to the level of kyc
        d->kxb = d->kxc;
        d->kyb = a1 * d->kxb + b1;
      }
      else
      {
        // we move kyc to the level of kyd
        d->kxc = d->kxb;
        d->kyc = a2 * d->kxc + b2;
      }
    }
    d->k_space[0] = fabsf((d->kxa + d->kxd) / 2.0f);
    d->k_space[1] = fabsf((d->kya + d->kyb) / 2.0f);
    d->k_space[2] = fabsf((d->kxb + d->kxc) / 2.0f) - d->k_space[0];
    d->k_space[3] = fabsf((d->kyc + d->kyd) / 2.0f) - d->k_space[1];
    d->kxb = d->kxb - d->kxa;
    d->kxc = d->kxc - d->kxa;
    d->kxd = d->kxd - d->kxa;
    d->kyb = d->kyb - d->kya;
    d->kyc = d->kyc - d->kya;
    d->kyd = d->kyd - d->kya;
    keystone_get_matrix(d->k_space, d->kxa, d->kxb, d->kxc, d->kxd, d->kya, d->kyb, d->kyc, d->kyd, &d->a,
                        &d->b, &d->d, &d->e, &d->g, &d->h);

    d->k_apply = 1;
    d->all_off = 0;
    d->crop_auto = 0;
  }
  else
  {
    d->all_off = 1;
    d->k_apply = 0;
  }

  if(gui_has_focus(self))
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
    d->cw = CLAMPF(fabsf(p->cw), 0.1f, 1.0f);
    d->ch = CLAMPF(fabsf(p->ch), 0.1f, 1.0f);
    // we show a error on stderr if we have clamped something
    if(d->cx != p->cx || d->cy != p->cy || d->cw != fabsf(p->cw) || d->ch != fabsf(p->ch))
    {
      fprintf(stderr, "[crop&rotate] invalid crop data for %d : x=%0.04f y=%0.04f w=%0.04f h=%0.04f\n",
              pipe->image.id, p->cx, p->cy, p->cw, p->ch);
    }
  }
}

static void _event_preview_updated_callback(gpointer instance, dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  g->preview_ready = TRUE;
  if(self->dev->gui_module != self)
  {
    dt_image_update_final_size(self->dev->preview_pipe->output_imgid);
  }

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_event_preview_updated_callback), self);
  // force max size to be recomputed
  g->clip_max_pipe_hash = 0;
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

  if(self->enabled)
  {
    if(in)
    {
      DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                                      G_CALLBACK(_event_preview_updated_callback), self);

      // got focus, grab stuff to gui:
      // need to get gui stuff for the first time for this image,
      g->clip_x = CLAMPF(p->cx, 0.0f, 0.9f);
      g->clip_y = CLAMPF(p->cy, 0.0f, 0.9f);
      g->clip_w = CLAMPF(fabsf(p->cw) - p->cx, 0.1f, 1.0f - g->clip_x);
      g->clip_h = CLAMPF(fabsf(p->ch) - p->cy, 0.1f, 1.0f - g->clip_y);
    }
    else
    {
      DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                                      G_CALLBACK(_event_preview_updated_callback), self);

      // lost focus, commit current params:
      // if the keystone setting is not finished, we discard it
      if(p->k_apply == 0 && p->k_type < 4 && p->k_type > 0)
      {
        keystone_type_populate(self, FALSE, 0);
      }
      // hack : commit_box use distort_transform routines with gui values to get params
      // but this values are accurate only if clipping is the gui_module...
      // so we temporary put back gui_module to clipping and revert once finished
      dt_iop_module_t *old_gui = self->dev->gui_module;
      self->dev->gui_module = self;
      commit_box(self, g, p);
      self->dev->gui_module = old_gui;
      g->clip_max_pipe_hash = 0;
    }
  }
  else if(in)
    g->preview_ready = TRUE;
}


void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_clipping_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

static float _ratio_get_aspect(dt_iop_module_t *self, GtkWidget *combo)
{
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

  //retrieve full image dimensions to calculate aspect ratio if "original image" specified
  const char *text = dt_bauhaus_combobox_get_text(combo);
  if(text && !g_strcmp0(text,_("original image")))
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
    if(fabsf(p->cw) == 1.0 && p->cx == 0.0 && fabsf(p->ch) == 1.0 && p->cy == 0.0)
    {
      p->ratio_d = -1;
      p->ratio_n = -1;
    }
    else
    {
      const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
      const float whratio = ((float)(iwd - 2 * interpolation->width) * (fabsf(p->cw) - p->cx))
                            / ((float)(iht - 2 * interpolation->width) * (fabsf(p->ch) - p->cy));
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

static void apply_box_aspect(dt_iop_module_t *self, _grab_region_t grab)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;

  int iwd, iht;
  dt_dev_get_processed_size(darktable.develop, &iwd, &iht);

  // enforce aspect ratio.
  float aspect = _ratio_get_aspect(self, g->aspect_presets);

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
      double prev_clip_h = clip_h;
      clip_h *= (clip_w + clip_x - g->clip_max_x) / clip_w;
      clip_w = clip_w + clip_x - g->clip_max_x;
      clip_x = g->clip_max_x;
      if(grab & GRAB_TOP) clip_y += prev_clip_h - clip_h;
    }
    if(clip_y < g->clip_max_y)
    {
      double prev_clip_w = clip_w;
      clip_w *= (clip_h + clip_y - g->clip_max_y) / clip_h;
      clip_h = clip_h + clip_y - g->clip_max_y;
      clip_y = g->clip_max_y;
      if(grab & GRAB_LEFT) clip_x += prev_clip_w - clip_w;
    }
    if(clip_x + clip_w > g->clip_max_x + g->clip_max_w)
    {
      double prev_clip_h = clip_h;
      clip_h *= (g->clip_max_x + g->clip_max_w - clip_x) / clip_w;
      clip_w = g->clip_max_x + g->clip_max_w - clip_x;
      if(grab & GRAB_TOP) clip_y += prev_clip_h - clip_h;
    }
    if(clip_y + clip_h > g->clip_max_y + g->clip_max_h)
    {
      double prev_clip_w = clip_w;
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

  dt_iop_clipping_params_t *d = (dt_iop_clipping_params_t *)self->default_params;

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

static void aspect_presets_changed(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
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
        int dd = atoi(text);
        int nn = atoi(c);
        // some sanity check
        if(nn == 0 || dd == 0)
        {
          dt_control_log(_("invalid ratio format. it should be \"number:number\""));
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
          dt_control_log(_("invalid ratio format. it should be a positive number"));
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
        if(nn % 2 == 0) nn /= 2;
        else if(dd % 2 == 0) dd /= 2;
        else if(nn > dd) nn = (nn - dd) / 2;
        else dd = (dd - nn) / 2;
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
      const dt_iop_clipping_aspect_t *aspect = iter->data;
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
    dt_conf_set_int("plugins/darkroom/clipping/ratio_d", abs(p->ratio_d));
    dt_conf_set_int("plugins/darkroom/clipping/ratio_n", abs(p->ratio_n));
    if(darktable.gui->reset) return;
    apply_box_aspect(self, GRAB_HORIZONTAL);
    dt_control_queue_redraw_center();
  }

  // Search if current aspect ratio matches something known
  int act = -1, i = 0;

  for(const GList *iter = g->aspect_list; iter; iter = g_list_next(iter))
  {
    const dt_iop_clipping_aspect_t *aspect = iter->data;
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
    snprintf(str, sizeof(str), "%d:%d %2.2f",
             abs(p->ratio_d), abs(p->ratio_n), (float)abs(p->ratio_d) / (float)abs(p->ratio_n));
    dt_bauhaus_combobox_set_text(g->aspect_presets, str);
  }
  else if(dt_bauhaus_combobox_get(g->aspect_presets) != act)
    // we got a default ratio
    dt_bauhaus_combobox_set(g->aspect_presets, act);

  --darktable.gui->reset;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

  ++darktable.gui->reset;

  if(w == g->cx)
  {
    dt_bauhaus_slider_set_soft_min(g->cw, p->cx + 0.10);
    g->clip_w = g->clip_x + g->clip_w - p->cx;
    g->clip_x = p->cx;
  }
  else if(w == g->cw)
  {
    dt_bauhaus_slider_set_soft_max(g->cx, p->cw - 0.10);
    g->clip_w = p->cw - g->clip_x;
  }
  else if(w == g->cy)
  {
    dt_bauhaus_slider_set_soft_min(g->ch, p->cy + 0.10);
    g->clip_h = g->clip_y + g->clip_h - p->cy;
    g->clip_y = p->cy;
  }
  else if(w == g->ch)
  {
    dt_bauhaus_slider_set_soft_max(g->cy, p->ch - 0.10);
    g->clip_h = p->ch - g->clip_y;
  }

  --darktable.gui->reset;

  commit_box(self, g, p);

  if(w == g->crop_auto) dt_control_queue_redraw_center();
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  /* reset aspect preset to default */
  dt_conf_set_int("plugins/darkroom/clipping/ratio_d", 0);
  dt_conf_set_int("plugins/darkroom/clipping/ratio_n", 0);
  g->k_show = -1;
}

static void keystone_type_changed(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  const int which = dt_bauhaus_combobox_get(combo);
  if((which == 5) || (which == 4 && p->k_h == 0 && p->k_v == 0))
  {
    // if the keystone is applied,autocrop must be disabled !
    gtk_widget_set_sensitive(g->crop_auto, FALSE);
    gtk_widget_set_sensitive(g->aspect_presets, TRUE);
    return;
  }
  // we recreate the list to be sure that the "already applied" entry is not display
  if(g->k_show == 2)
  {
    if(which == 0 || which == 4)
      g->k_show = 0;
    else
      g->k_show = 1;
    keystone_type_populate(self, FALSE, which);
  }

  // we set the params
  p->k_apply = 0;
  p->k_type = which;
  if(which == 0 || which == 4)
    g->k_show = 0;
  else
    g->k_show = 1;

  // we can enable autocrop
  gtk_widget_set_sensitive(g->crop_auto, (g->k_show == 0));
  gtk_widget_set_sensitive(g->aspect_presets, (g->k_show == 0));

  commit_box(self, g, p);
  dt_control_queue_redraw_center();
}

static void keystone_type_populate(struct dt_iop_module_t *self, gboolean with_applied, int select)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  dt_bauhaus_combobox_clear(g->keystone_type);
  dt_bauhaus_combobox_add(g->keystone_type, _("none"));
  dt_bauhaus_combobox_add(g->keystone_type, _("vertical"));
  dt_bauhaus_combobox_add(g->keystone_type, _("horizontal"));
  dt_bauhaus_combobox_add(g->keystone_type, _("full"));
  if(p->k_h != 0 || p->k_v != 0) dt_bauhaus_combobox_add(g->keystone_type, _("old system"));
  if(with_applied) dt_bauhaus_combobox_add(g->keystone_type, _("correction applied"));

  if(select < 0) return;
  int sel = 0;
  if(select > 10 && p->k_h == 0 && p->k_v == 0)
    sel = 4;
  else if(select > 10)
    sel = 5;
  else
    sel = select;

  dt_bauhaus_combobox_set(g->keystone_type, sel);
  // we have to be sure that the event is called...
  keystone_type_changed(g->keystone_type, self);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

  int hvflip = 0;
  if(p->cw < 0)
  {
    if(p->ch < 0)
      hvflip = 3; // BOTH
    else
      hvflip = 1; // HORIZONTAL
  }
  else
  {
    if(p->ch < 0)
      hvflip = 2; // VERTICAL
    else
      hvflip = 0; // NONE
  }
  dt_bauhaus_combobox_set(g->hvflip, hvflip);

  //  set aspect ratio based on the current image, if not found let's default
  //  to free aspect.

  if(p->ratio_d == -2 && p->ratio_n == -2) _ratio_get_aspect(self, g->aspect_presets);

  if(p->ratio_d == -1 && p->ratio_n == -1)
  {
    p->ratio_d = dt_conf_get_int("plugins/darkroom/clipping/ratio_d");
    p->ratio_n = dt_conf_get_int("plugins/darkroom/clipping/ratio_n");
  }

  const int d = abs(p->ratio_d), n = p->ratio_n;

  int act = -1;
  int i = 0;
  for(const GList *iter = g->aspect_list; iter; iter = g_list_next(iter))
  {
    const dt_iop_clipping_aspect_t *aspect = iter->data;
    if((aspect->d == d) && (aspect->n == n))
    {
      act = i;
      break;
    }
    i++;
  }

  // keystone :
  if(p->k_apply == 1) g->k_show = 2; // needed to initialise correctly the combobox
  else g->k_show = -1;

  if(g->k_show == 2)
  {
    keystone_type_populate(self, TRUE, 99);
  }
  else if(g->k_show == -1)
  {
    keystone_type_populate(self, FALSE, p->k_type);
  }

  /* special handling the combobox when current act is already selected
     callback is not called, let do it our self then..
   */
  if(act == -1)
  {
    char str[128];
    snprintf(str, sizeof(str), "%d:%d %2.2f",
             abs(p->ratio_d), abs(p->ratio_n), (float)abs(p->ratio_d) / (float)abs(p->ratio_n));
    dt_bauhaus_combobox_set_text(g->aspect_presets, str);
  }
  if(dt_bauhaus_combobox_get(g->aspect_presets) == act)
    aspect_presets_changed(g->aspect_presets, self);
  else
    dt_bauhaus_combobox_set(g->aspect_presets, act);

  // reset gui draw box to what we have in the parameters:
  g->applied = 1;
  g->clip_x = CLAMPF(p->cx, 0.0f, 0.9f);
  g->clip_y = CLAMPF(p->cy, 0.0f, 0.9f);
  g->clip_w = CLAMPF(fabsf(p->cw) - p->cx, 0.1f, 1.0f - g->clip_x);
  g->clip_h = CLAMPF(fabsf(p->ch) - p->cy, 0.1f, 1.0f - g->clip_y);
}

static void hvflip_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  const int flip = dt_bauhaus_combobox_get(widget);
  p->cw = copysignf(p->cw, (flip & 1) ? -1.0 : 1.0);
  p->ch = copysignf(p->ch, (flip & 2) ? -1.0 : 1.0);
  commit_box(self, g, p);
}

static void key_swap_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                              GdkModifierType modifier, gpointer d)
{
  (void)accel_group;
  (void)acceleratable;
  (void)keyval;
  (void)modifier;
  dt_iop_module_t *self = (dt_iop_module_t *)d;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->ratio_d = -p->ratio_d;
  apply_box_aspect(self, GRAB_HORIZONTAL);
  dt_control_queue_redraw_center();
}

static void aspect_flip(GtkWidget *button, dt_iop_module_t *self)
{
  key_swap_callback(NULL, NULL, 0, 0, self);
}

static gint _aspect_ratio_cmp(const dt_iop_clipping_aspect_t *a, const dt_iop_clipping_aspect_t *b)
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


static gchar *format_aspect(gchar *original, int adim, int bdim)
{
  // Special ratios:  freehand, original image
  if(bdim == 0) return g_strdup(original);

  return g_strdup_printf("%s  %4.2f", original, (float)adim / (float)bdim);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = IOP_GUI_ALLOC(clipping);

  g->aspect_list = NULL;
  g->clip_x = g->clip_y = g->handle_x = g->handle_y = 0.0;
  g->clip_w = g->clip_h = 1.0;
  g->clip_max_x = g->clip_max_y = 0.0;
  g->clip_max_w = g->clip_max_h = 1.0;
  g->clip_max_pipe_hash = 0;
  g->cropping = 0;
  g->straightening = 0;
  g->applied = 1;
  g->shift_hold = FALSE;
  g->ctrl_hold = FALSE;
  g->k_drag = FALSE;
  g->k_show = -1;
  g->k_selected = -1;
  g->preview_ready = FALSE;

  static struct dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);

  self->widget = dt_ui_notebook_page(g->notebook, N_("main"), NULL);

  g->hvflip = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->hvflip, NULL, N_("flip"));
  dt_bauhaus_combobox_add(g->hvflip, _("none"));
  dt_bauhaus_combobox_add(g->hvflip, _("horizontal"));
  dt_bauhaus_combobox_add(g->hvflip, _("vertical"));
  dt_bauhaus_combobox_add(g->hvflip, _("both"));
  g_signal_connect(G_OBJECT(g->hvflip), "value-changed", G_CALLBACK(hvflip_callback), self);
  gtk_widget_set_tooltip_text(g->hvflip, _("mirror image horizontally and/or vertically"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->hvflip, TRUE, TRUE, 0);

  g->angle = dt_bauhaus_slider_from_params(self, N_("angle"));
  dt_bauhaus_slider_set_factor(g->angle, -1.0);
  dt_bauhaus_slider_set_format(g->angle, "°");
  gtk_widget_set_tooltip_text(g->angle, _("right-click and drag a line on the image to drag a straight line"));

  g->keystone_type = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->keystone_type, NULL, N_("keystone"));
  dt_bauhaus_combobox_add(g->keystone_type, _("none"));
  dt_bauhaus_combobox_add(g->keystone_type, _("vertical"));
  dt_bauhaus_combobox_add(g->keystone_type, _("horizontal"));
  dt_bauhaus_combobox_add(g->keystone_type, _("full"));
  gtk_widget_set_tooltip_text(g->keystone_type, _("set perspective correction for your image"));
  g_signal_connect(G_OBJECT(g->keystone_type), "value-changed", G_CALLBACK(keystone_type_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->keystone_type, TRUE, TRUE, 0);

  g->crop_auto = dt_bauhaus_combobox_from_params(self, "crop_auto");
  gtk_widget_set_tooltip_text(g->crop_auto, _("automatically crop to avoid black edges"));

  dt_iop_clipping_aspect_t aspects[] = { { _("freehand"), 0, 0 },
                                         { _("original image"), 1, 0 },
                                         { _("square"), 1, 1 },
                                         { _("10:8 in print"), 2445, 2032 },
                                         { _("5:4, 4x5, 8x10"), 5, 4 },
                                         { _("11x14"), 14, 11 },
                                         { _("8.5x11, letter"), 110, 85 },
                                         { _("4:3, VGA, TV"), 4, 3 },
                                         { _("5x7"), 7, 5 },
                                         { _("ISO 216, DIN 476, A4"), 14142136, 10000000 },
                                         { _("3:2, 4x6, 35mm"), 3, 2 },
                                         { _("16:10, 8x5"), 16, 10 },
                                         { _("golden cut"), 16180340, 10000000 },
                                         { _("16:9, HDTV"), 16, 9 },
                                         { _("widescreen"), 185, 100 },
                                         { _("2:1, univisium"), 2, 1 },
                                         { _("cinemascope"), 235, 100 },
                                         { _("21:9"), 237, 100 },
                                         { _("anamorphic"), 239, 100 },
                                         { _("3:1, panorama"), 300, 100 },
  };

  const int aspects_count = sizeof(aspects) / sizeof(dt_iop_clipping_aspect_t);

  for(int i = 0; i < aspects_count; i++)
  {
    dt_iop_clipping_aspect_t *aspect = g_malloc(sizeof(dt_iop_clipping_aspect_t));
    aspect->name = format_aspect(aspects[i].name, aspects[i].d, aspects[i].n);
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
        dt_control_log(_("invalid ratio format for `%s'. it should be \"number:number\""), nv->key);
        continue;
      }
      dt_iop_clipping_aspect_t *aspect = g_malloc(sizeof(dt_iop_clipping_aspect_t));
      aspect->name = format_aspect(nv->key, d, n);
      aspect->d = d;
      aspect->n = n;
      g->aspect_list = g_list_append(g->aspect_list, aspect);
    }
    else
    {
      fprintf(stderr, "invalid ratio format for `%s'. it should be \"number:number\"\n", nv->key);
      dt_control_log(_("invalid ratio format for `%s'. it should be \"number:number\""), nv->key);
      continue;
    }

  }
  g_slist_free_full(custom_aspects, dt_conf_string_entry_free);


  g->aspect_list = g_list_sort(g->aspect_list, (GCompareFunc)_aspect_ratio_cmp);

  // remove duplicates from the aspect ratio list
  int d = ((dt_iop_clipping_aspect_t *)g->aspect_list->data)->d + 1,
      n = ((dt_iop_clipping_aspect_t *)g->aspect_list->data)->n + 1;
  for(GList *iter = g->aspect_list; iter; iter = g_list_next(iter))
  {
    dt_iop_clipping_aspect_t *aspect = (dt_iop_clipping_aspect_t *)iter->data;
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
  dt_bauhaus_widget_set_label(g->aspect_presets, NULL, N_("aspect"));

  for(GList *iter = g->aspect_list; iter; iter = g_list_next(iter))
  {
    const dt_iop_clipping_aspect_t *aspect = iter->data;
    dt_bauhaus_combobox_add(g->aspect_presets, aspect->name);
  }

  dt_bauhaus_combobox_set(g->aspect_presets, 0);

  g_signal_connect(G_OBJECT(g->aspect_presets), "value-changed", G_CALLBACK(aspect_presets_changed), self);
  gtk_widget_set_tooltip_text(g->aspect_presets, _("set the aspect ratio\n"
                                                   "the list is sorted: from most square to least square\n"
                                                   "to enter custom aspect ratio open the combobox and type ratio in x:y or decimal format"));
  dt_bauhaus_widget_set_quad_paint(g->aspect_presets, dtgtk_cairo_paint_aspectflip, 0, NULL);
  g_signal_connect(G_OBJECT(g->aspect_presets), "quad-pressed", G_CALLBACK(aspect_flip), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->aspect_presets, TRUE, TRUE, 0);

  self->widget = dt_ui_notebook_page(g->notebook, _("margins"), NULL);

  g->cx = dt_bauhaus_slider_from_params(self, "cx");
  dt_bauhaus_slider_set_digits(g->cx, 4);
  dt_bauhaus_slider_set_format(g->cx, "%");
  gtk_widget_set_tooltip_text(g->cx, _("the left margin cannot overlap with the right margin"));

  g->cw = dt_bauhaus_slider_from_params(self, "cw");
  dt_bauhaus_slider_set_digits(g->cw, 4);
  dt_bauhaus_slider_set_factor(g->cw, -100.0);
  dt_bauhaus_slider_set_offset(g->cw, 100.0);
  dt_bauhaus_slider_set_format(g->cw, "%");
  gtk_widget_set_tooltip_text(g->cw, _("the right margin cannot overlap with the left margin"));

  g->cy = dt_bauhaus_slider_from_params(self, "cy");
  dt_bauhaus_slider_set_digits(g->cy, 4);
  dt_bauhaus_slider_set_format(g->cy, "%");
  gtk_widget_set_tooltip_text(g->cy, _("the top margin cannot overlap with the bottom margin"));

  g->ch = dt_bauhaus_slider_from_params(self, "ch");
  dt_bauhaus_slider_set_digits(g->ch, 4);
  dt_bauhaus_slider_set_factor(g->ch, -100.0);
  dt_bauhaus_slider_set_offset(g->ch, 100.0);
  dt_bauhaus_slider_set_format(g->ch, "%");
  gtk_widget_set_tooltip_text(g->ch, _("the bottom margin cannot overlap with the top margin"));

  self->widget = GTK_WIDGET(g->notebook);
}

static void free_aspect(gpointer data)
{
  dt_iop_clipping_aspect_t *aspect = (dt_iop_clipping_aspect_t *)data;
  g_free(aspect->name);
  aspect->name = NULL;
  g_free(aspect);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  g_list_free_full(g->aspect_list, free_aspect);
  g->aspect_list = NULL;

  IOP_GUI_FREE;
}

static _grab_region_t get_grab(float pzx, float pzy, dt_iop_clipping_gui_data_t *g, const float border,
                               const float wd, const float ht)
{
  _grab_region_t grab = GRAB_NONE;
  if(!(pzx < g->clip_x || pzx > g->clip_x + g->clip_w || pzy < g->clip_y || pzy > g->clip_y + g->clip_h))
  {
    // we are inside the crop box
    grab = GRAB_CENTER;
    if(pzx >= g->clip_x && pzx * wd < g->clip_x * wd + border) grab |= GRAB_LEFT; // left border
    if(pzy >= g->clip_y && pzy * ht < g->clip_y * ht + border) grab |= GRAB_TOP;  // top border
    if(pzx <= g->clip_x + g->clip_w && pzx * wd > (g->clip_w + g->clip_x) * wd - border)
      grab |= GRAB_RIGHT; // right border
    if(pzy <= g->clip_y + g->clip_h && pzy * ht > (g->clip_h + g->clip_y) * ht - border)
      grab |= GRAB_BOTTOM; // bottom border
  }
  return grab;
}

// draw symmetry signs
static void gui_draw_sym(cairo_t *cr, float x, float y, float scale, gboolean active)
{
  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(16) * PANGO_SCALE * scale);
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  pango_layout_set_text(layout, "ꝏ", -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  dt_draw_set_color_overlay(cr, TRUE, 0.5);
  dt_gui_draw_rounded_rectangle(
      cr, ink.width + DT_PIXEL_APPLY_DPI(4) * scale, ink.height + DT_PIXEL_APPLY_DPI(8) * scale,
      x - ink.width / 2.0f - DT_PIXEL_APPLY_DPI(2) * scale, y - ink.height / 2.0f - DT_PIXEL_APPLY_DPI(4) * scale);
  cairo_move_to(cr, x - ink.width / 2.0f, y - 3.0 * ink.height / 4.0f - DT_PIXEL_APPLY_DPI(4) * scale);
  if(active)
    cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, .9);
  else
    cairo_set_source_rgba(cr, .2, .2, .2, .9);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(desc);
  g_object_unref(layout);
}

// draw guides and handles over the image
void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

  // we don't do anything if the image is not ready
  if(!g->preview_ready) return;

  // reapply box aspect to be sure that the ratio has not been modified by the keystone transform
  apply_box_aspect(self, GRAB_HORIZONTAL);

  const float wd = dev->preview_pipe->backbuf_width;
  const float ht = dev->preview_pipe->backbuf_height;
  const float zoom_y = dt_control_get_dev_zoom_y();
  const float zoom_x = dt_control_get_dev_zoom_x();
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float pr_d = dev->preview_downsampling;
  const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);

  cairo_translate(cr, width / 2.0, height / 2.0);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  double dashes = DT_PIXEL_APPLY_DPI(5.0) / zoom_scale;

  // draw cropping window
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  if(_iop_clipping_set_max_clip(self))
  {
    cairo_set_source_rgba(cr, .2, .2, .2, .8);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, g->clip_max_x * wd - pr_d, g->clip_max_y * ht - pr_d,
                    g->clip_max_w * wd + 2.0 * pr_d, g->clip_max_h * ht + 2.0 * pr_d);
    cairo_rectangle(cr, g->clip_x * wd, g->clip_y * ht, g->clip_w * wd, g->clip_h * ht);
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
  if(darktable.control->button_down && darktable.control->button_down_which == 1 && g->k_show != 1)
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
    const float text_h = DT_PIXEL_APPLY_DPI(16+2) / zoom_scale;
    const float margin = DT_PIXEL_APPLY_DPI(6) / zoom_scale;
    float xp = (g->clip_x + g->clip_w * .5f) * wd - text_w * .5f;
    float yp = (g->clip_y + g->clip_h * .5f) * ht - text_h * .5f;

    // ensure that the rendered string remains visible within the window bounds
    double x1, y1, x2, y2;
    cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
    xp = CLAMPF(xp, x1 + 2.0 * margin, x2 - text_w - 2.0 * margin);
    yp = CLAMPF(yp, y1 + 2.0 * margin, y2 - text_h - 2.0 * margin);

    cairo_set_source_rgba(cr, .5, .5, .5, .9);
    dt_gui_draw_rounded_rectangle(cr, text_w + 2 * margin, text_h + 2 * margin,
                                  xp - margin, yp - margin);
    cairo_set_source_rgb(cr, .7, .7, .7);
    cairo_move_to(cr, xp, yp);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }

  // draw crop area guides
  dt_guides_draw(cr, g->clip_x * wd, g->clip_y * ht, g->clip_w * wd, g->clip_h * ht, zoom_scale);

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0) / zoom_scale);
  dt_draw_set_color_overlay(cr, FALSE, 1.0);
  const int border = DT_PIXEL_APPLY_DPI(30.0) / zoom_scale;
  if(g->straightening)
  {
    PangoRectangle ink;
    PangoLayout *layout;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(16) * PANGO_SCALE / zoom_scale);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);
    const float bzx = g->button_down_zoom_x + .5f, bzy = g->button_down_zoom_y + .5f;
    cairo_arc(cr, bzx * wd, bzy * ht, DT_PIXEL_APPLY_DPI(3) * pr_d, 0, 2.0 * M_PI);
    cairo_stroke(cr);
    cairo_arc(cr, pzx * wd, pzy * ht, DT_PIXEL_APPLY_DPI(3) * pr_d, 0, 2.0 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, bzx * wd, bzy * ht);
    cairo_line_to(cr, pzx * wd, pzy * ht);
    cairo_stroke(cr);

    // show rotation angle
    float dx = pzx * wd - bzx * wd, dy = pzy * ht - bzy * ht;
    if(dx < 0)
    {
      dx = -dx;
      dy = -dy;
    }
    float angle = atan2f(dy, dx);
    angle = angle * 180 / M_PI;
    if(angle > 45.0) angle -= 90;
    if(angle < -45.0) angle += 90;

    char view_angle[16];
    view_angle[0] = '\0';
    snprintf(view_angle, sizeof(view_angle), "%.2f°", angle);
    pango_layout_set_text(layout, view_angle, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    const float text_w = ink.width;
    const float text_h = DT_PIXEL_APPLY_DPI(16+2) / zoom_scale;
    const float margin = DT_PIXEL_APPLY_DPI(6) / zoom_scale;
    cairo_set_source_rgba(cr, .5, .5, .5, .9);
    const float xp = pzx * wd + DT_PIXEL_APPLY_DPI(20) / zoom_scale;
    const float yp = pzy * ht - ink.height;
    dt_gui_draw_rounded_rectangle
      (cr, text_w + 2 * margin, text_h + 2 * margin, xp - margin, yp - margin);
    cairo_set_source_rgba(cr, .7, .7, .7, .7);
    cairo_move_to(cr, xp, yp);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
  else if(g->k_show != 1)
  {
    const _grab_region_t grab = g->cropping ? g->cropping : get_grab(pzx, pzy, g, border, wd, ht);
    if(grab == GRAB_LEFT) cairo_rectangle(cr, g->clip_x * wd, g->clip_y * ht, border, g->clip_h * ht);
    if(grab == GRAB_TOP) cairo_rectangle(cr, g->clip_x * wd, g->clip_y * ht, g->clip_w * wd, border);
    if(grab == GRAB_TOP_LEFT) cairo_rectangle(cr, g->clip_x * wd, g->clip_y * ht, border, border);
    if(grab == GRAB_RIGHT)
      cairo_rectangle(cr, (g->clip_x + g->clip_w) * wd - border, g->clip_y * ht, border, g->clip_h * ht);
    if(grab == GRAB_BOTTOM)
      cairo_rectangle(cr, g->clip_x * wd, (g->clip_y + g->clip_h) * ht - border, g->clip_w * wd, border);
    if(grab == GRAB_BOTTOM_RIGHT)
      cairo_rectangle(cr, (g->clip_x + g->clip_w) * wd - border, (g->clip_y + g->clip_h) * ht - border,
                      border, border);
    if(grab == GRAB_TOP_RIGHT)
      cairo_rectangle(cr, (g->clip_x + g->clip_w) * wd - border, g->clip_y * ht, border, border);
    if(grab == GRAB_BOTTOM_LEFT)
      cairo_rectangle(cr, g->clip_x * wd, (g->clip_y + g->clip_h) * ht - border, border, border);
    cairo_stroke(cr);
  }

  // draw keystone points and lines
  if(g->k_show == 1 && p->k_type > 0)
  {
    // points in screen space
    dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
    if(!piece) return;

    const float wp = piece->buf_out.width, hp = piece->buf_out.height;
    float pts[8] = { p->kxa * wp, p->kya * hp, p->kxb * wp, p->kyb * hp,
                     p->kxc * wp, p->kyc * hp, p->kxd * wp, p->kyd * hp };
    if(dt_dev_distort_transform_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 4))
    {
      if(p->k_type == 3)
      {
        // determine extremity of the lines
        const float v1t = pts[0] - (pts[6] - pts[0]) * pts[1] / (pts[7] - pts[1]);
        const float v1b = (pts[6] - pts[0]) * ht / (pts[7] - pts[1]) + v1t;
        const float v2t = pts[2] - (pts[4] - pts[2]) * pts[3] / (pts[5] - pts[3]);
        const float v2b = (pts[4] - pts[2]) * ht / (pts[5] - pts[3]) + v2t;
        const float h1l = pts[1] - (pts[3] - pts[1]) * pts[0] / (pts[2] - pts[0]);
        const float h1r = (pts[3] - pts[1]) * wd / (pts[2] - pts[0]) + h1l;
        const float h2l = pts[7] - (pts[5] - pts[7]) * pts[6] / (pts[4] - pts[6]);
        const float h2r = (pts[5] - pts[7]) * wd / (pts[4] - pts[6]) + h2l;

        // draw the lines
        cairo_move_to(cr, v1t, 0);
        cairo_line_to(cr, v1b, ht);
        cairo_stroke(cr);
        cairo_move_to(cr, v2t, 0);
        cairo_line_to(cr, v2b, ht);
        cairo_stroke(cr);
        cairo_move_to(cr, 0, h1l);
        cairo_line_to(cr, wd, h1r);
        cairo_stroke(cr);
        cairo_move_to(cr, 0, h2l);
        cairo_line_to(cr, wd, h2r);
        cairo_stroke(cr);
        // redraw selected one
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(4.0) / zoom_scale);
        if(g->k_selected_segment == 0)
        {
          cairo_move_to(cr, pts[0], pts[1]);
          cairo_line_to(cr, pts[2], pts[3]);
          cairo_stroke(cr);
        }
        else if(g->k_selected_segment == 1)
        {
          cairo_move_to(cr, pts[4], pts[5]);
          cairo_line_to(cr, pts[2], pts[3]);
          cairo_stroke(cr);
        }
        else if(g->k_selected_segment == 2)
        {
          cairo_move_to(cr, pts[4], pts[5]);
          cairo_line_to(cr, pts[6], pts[7]);
          cairo_stroke(cr);
        }
        else if(g->k_selected_segment == 3)
        {
          cairo_move_to(cr, pts[0], pts[1]);
          cairo_line_to(cr, pts[6], pts[7]);
          cairo_stroke(cr);
        }
      }
      else if(p->k_type == 2)
      {
        // determine extremity of the lines
        const float h1l = pts[1] - (pts[3] - pts[1]) * pts[0] / (pts[2] - pts[0]);
        const float h1r = (pts[3] - pts[1]) * wd / (pts[2] - pts[0]) + h1l;
        const float h2l = pts[7] - (pts[5] - pts[7]) * pts[6] / (pts[4] - pts[6]);
        const float h2r = (pts[5] - pts[7]) * wd / (pts[4] - pts[6]) + h2l;

        // draw the lines
        cairo_move_to(cr, 0, h1l);
        cairo_line_to(cr, wd, h1r);
        cairo_stroke(cr);
        cairo_move_to(cr, 0, h2l);
        cairo_line_to(cr, wd, h2r);
        cairo_stroke(cr);
        // redraw selected one
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(4.0) / zoom_scale);
        if(g->k_selected_segment == 1)
        {
          cairo_move_to(cr, pts[4], pts[5]);
          cairo_line_to(cr, pts[2], pts[3]);
          cairo_stroke(cr);
        }
        else if(g->k_selected_segment == 3)
        {
          cairo_move_to(cr, pts[0], pts[1]);
          cairo_line_to(cr, pts[6], pts[7]);
          cairo_stroke(cr);
        }
      }
      else if(p->k_type == 1)
      {
        // determine extremity of the lines
        const float v1t = pts[0] - (pts[6] - pts[0]) * pts[1] / (pts[7] - pts[1]);
        const float v1b = (pts[6] - pts[0]) * ht / (pts[7] - pts[1]) + v1t;
        const float v2t = pts[2] - (pts[4] - pts[2]) * pts[3] / (pts[5] - pts[3]);
        const float v2b = (pts[4] - pts[2]) * ht / (pts[5] - pts[3]) + v2t;

        // draw the lines
        cairo_move_to(cr, v1t, 0);
        cairo_line_to(cr, v1b, ht);
        cairo_stroke(cr);
        cairo_move_to(cr, v2t, 0);
        cairo_line_to(cr, v2b, ht);
        cairo_stroke(cr);
        // redraw selected one
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(4.0) / zoom_scale);
        if(g->k_selected_segment == 0)
        {
          cairo_move_to(cr, pts[0], pts[1]);
          cairo_line_to(cr, pts[2], pts[3]);
          cairo_stroke(cr);
        }
        else if(g->k_selected_segment == 2)
        {
          cairo_move_to(cr, pts[4], pts[5]);
          cairo_line_to(cr, pts[6], pts[7]);
          cairo_stroke(cr);
        }
      }

      // draw the points
      if(g->k_selected == 0) // point 1
      {
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(4.0) / zoom_scale);
        cairo_set_source_rgba(cr, 1.0, 0, 0, .8);
      }
      else
      {
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0) / zoom_scale);
        cairo_set_source_rgba(cr, 1.0, 0, 0, .5);
      }
      cairo_arc(cr, pts[0], pts[1], DT_PIXEL_APPLY_DPI(5.0) / zoom_scale, 0, 2.0 * M_PI);
      cairo_stroke(cr);
      if(g->k_selected == 1) // point 2
      {
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(4.0) / zoom_scale);
        cairo_set_source_rgba(cr, 1.0, 0, 0, .8);
      }
      else
      {
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0) / zoom_scale);
        cairo_set_source_rgba(cr, 1.0, 0, 0, .5);
      }
      cairo_arc(cr, pts[2], pts[3], DT_PIXEL_APPLY_DPI(5.0) / zoom_scale, 0, 2.0 * M_PI);
      cairo_stroke(cr);
      if(g->k_selected == 2) // point 3
      {
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(4.0) / zoom_scale);
        cairo_set_source_rgba(cr, 1.0, 0, 0, .8);
      }
      else
      {
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0) / zoom_scale);
        cairo_set_source_rgba(cr, 1.0, 0, 0, .5);
      }
      cairo_arc(cr, pts[4], pts[5], DT_PIXEL_APPLY_DPI(5.0) / zoom_scale, 0, 2.0 * M_PI);
      cairo_stroke(cr);
      if(g->k_selected == 3) // point 4
      {
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(4.0) / zoom_scale);
        cairo_set_source_rgba(cr, 1.0, 0, 0, .8);
      }
      else
      {
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0) / zoom_scale);
        cairo_set_source_rgba(cr, 1.0, 0, 0, .5);
      }
      cairo_arc(cr, pts[6], pts[7], DT_PIXEL_APPLY_DPI(5.0) / zoom_scale, 0, 2.0 * M_PI);
      cairo_stroke(cr);
      // draw the apply "button"
      PangoLayout *layout;
      PangoRectangle ink;
      PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
      pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
      pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(16) * PANGO_SCALE * pr_d);
      layout = pango_cairo_create_layout(cr);
      pango_layout_set_font_description(layout, desc);
      cairo_set_font_size(cr, DT_PIXEL_APPLY_DPI(16));
      pango_layout_set_text(layout, "ok", -1);
      pango_layout_get_pixel_extents(layout, &ink, NULL);
      int c[2] = { (MIN(pts[4], pts[2]) + MAX(pts[0], pts[6])) / 2.0f,
                   (MIN(pts[5], pts[7]) + MAX(pts[1], pts[3])) / 2.0f };
      cairo_set_source_rgba(cr, .5, .5, .5, .9);
      dt_gui_draw_rounded_rectangle(cr, ink.width + DT_PIXEL_APPLY_DPI(8) * pr_d,
                                    ink.height + DT_PIXEL_APPLY_DPI(12) * pr_d,
                                    c[0] - ink.width / 2.0f - DT_PIXEL_APPLY_DPI(4) * pr_d,
                                    c[1] - ink.height / 2.0f - DT_PIXEL_APPLY_DPI(6) * pr_d);
      cairo_move_to(cr, c[0] - ink.width / 2.0, c[1] - 3.0 * ink.height / 4.0);
      dt_draw_set_color_overlay(cr, FALSE, 0.9);
      pango_cairo_show_layout(cr, layout);
      pango_font_description_free(desc);
      g_object_unref(layout);

      // draw the symmetry buttons
      gboolean sym = FALSE;
      if(p->k_type == 1 || p->k_type == 3)
      {
        if(p->k_sym == 1 || p->k_sym == 3) sym = TRUE;
        gui_draw_sym(cr, (pts[0] + pts[6]) / 2.0f, (pts[1] + pts[7]) / 2.0f, pr_d, sym);
        gui_draw_sym(cr, (pts[2] + pts[4]) / 2.0f, (pts[3] + pts[5]) / 2.0f, pr_d, sym);
      }
      if(p->k_type == 2 || p->k_type == 3)
      {
        sym = (p->k_sym >= 2);
        gui_draw_sym(cr, (pts[0] + pts[2]) / 2.0f, (pts[1] + pts[3]) / 2.0f, pr_d, sym);
        gui_draw_sym(cr, (pts[6] + pts[4]) / 2.0f, (pts[7] + pts[5]) / 2.0f, pr_d, sym);
      }
    }
  }
}

// determine the distance between the segment [(xa,ya)(xb,yb)] and the point (xc,yc)
static float dist_seg(float xa, float ya, float xb, float yb, float xc, float yc)
{
  if(xa == xb && ya == yb) return (xc - xa) * (xc - xa) + (yc - ya) * (yc - ya);

  const float sx = xb - xa;
  const float sy = yb - ya;

  const float ux = xc - xa;
  const float uy = yc - ya;

  const float dp = sx * ux + sy * uy;
  if(dp < 0) return (xc - xa) * (xc - xa) + (yc - ya) * (yc - ya);

  const float sn2 = sx * sx + sy * sy;
  if(dp > sn2) return (xc - xb) * (xc - xb) + (yc - yb) * (yc - yb);

  const float ah2 = dp * dp / sn2;
  const float un2 = ux * ux + uy * uy;
  return un2 - ah2;
}

int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

  // we don't do anything if the image is not ready
  if(!g->preview_ready) return 0;

  const float wd = self->dev->preview_pipe->backbuf_width;
  const float ht = self->dev->preview_pipe->backbuf_height;
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(self->dev, zoom, 1<<closeup, 1);
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  _iop_clipping_set_max_clip(self);
  _grab_region_t grab = get_grab(pzx, pzy, g, DT_PIXEL_APPLY_DPI(30.0) / zoom_scale, wd, ht);

  if(darktable.control->button_down && darktable.control->button_down_which == 3 && g->k_show != 1)
  {
    // second mouse button, straighten activated:
    g->straightening = 1;
    dt_control_change_cursor(GDK_CROSSHAIR);
    dt_control_queue_redraw_center();
  }
  else if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    // case when we drag a point for keystone
    if(g->k_drag == TRUE && g->k_selected >= 0)
    {
      float pts[2] = { pzx * wd, pzy * ht };
      dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 1);
      dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
      const float xx = pts[0] / (float)piece->buf_out.width, yy = pts[1] / (float)piece->buf_out.height;
      if(g->k_selected == 0)
      {
        if(p->k_sym == 1 || p->k_sym == 3)
          p->kxa = fminf(xx, (p->kxc + p->kxd - 0.01f) / 2.0f), p->kxb = p->kxc - p->kxa + p->kxd;
        else
          p->kxa = fminf(xx, p->kxb - 0.01f);
        if(p->k_sym > 1)
          p->kya = fminf(yy, (p->kyc + p->kyb - 0.01f) / 2.0f), p->kyd = p->kyc - p->kya + p->kyb;
        else
          p->kya = fminf(yy, p->kyd - 0.01f);
      }
      else if(g->k_selected == 1)
      {
        if(p->k_sym == 1 || p->k_sym == 3)
          p->kxb = fmaxf(xx, (p->kxc + p->kxd + 0.01f) / 2.0f), p->kxa = p->kxc - p->kxb + p->kxd;
        else
          p->kxb = fmaxf(xx, p->kxa + 0.01f);
        if(p->k_sym > 1)
          p->kyb = fminf(yy, (p->kya + p->kyd - 0.01f) / 2.0f), p->kyc = p->kya - p->kyb + p->kyd;
        else
          p->kyb = fminf(yy, p->kyc - 0.01f);
      }
      else if(g->k_selected == 2)
      {
        if(p->k_sym == 1 || p->k_sym == 3)
          p->kxc = fmaxf(xx, (p->kxa + p->kxb + 0.01f) / 2.0f), p->kxd = p->kxa - p->kxc + p->kxb;
        else
          p->kxc = fmaxf(xx, p->kxd + 0.01f);
        if(p->k_sym > 1)
          p->kyc = fmaxf(yy, (p->kya + p->kyd + 0.01f) / 2.0f), p->kyb = p->kya - p->kyc + p->kyd;
        else
          p->kyc = fmaxf(yy, p->kyb + 0.01f);
      }
      else if(g->k_selected == 3)
      {
        if(p->k_sym == 1 || p->k_sym == 3)
          p->kxd = fminf(xx, (p->kxa + p->kxb - 0.01f) / 2.0f), p->kxc = p->kxa - p->kxd + p->kxb;
        else
          p->kxd = fminf(xx, p->kxc - 0.01f);
        if(p->k_sym > 1)
          p->kyd = fmaxf(yy, (p->kyc + p->kyb + 0.01f) / 2.0f), p->kya = p->kyc - p->kyd + p->kyb;
        else
          p->kyd = fmaxf(yy, p->kya + 0.01f);
      }
      dt_control_queue_redraw_center();
      return 1;
    }

    // case when we drag a segment for keystone
    if(g->k_drag == TRUE && g->k_selected_segment >= 0)
    {
      float decalx = pzx - g->button_down_zoom_x;
      float decaly = pzy - g->button_down_zoom_y;
      if(g->k_selected_segment == 0 && (p->k_type == 1 || p->k_type == 3))
      {
        decaly = fminf(decaly, p->kyd - p->kya);
        decaly = fminf(decaly, p->kyc - p->kyb);
        p->kxa += decalx;
        p->kya += decaly;
        p->kxb += decalx;
        p->kyb += decaly;
      }
      else if(g->k_selected_segment == 1 && (p->k_type == 2 || p->k_type == 3))
      {
        decalx = fmaxf(decalx, p->kxa - p->kxb);
        decalx = fmaxf(decalx, p->kxd - p->kxc);
        p->kxc += decalx;
        p->kyc += decaly;
        p->kxb += decalx;
        p->kyb += decaly;
      }
      else if(g->k_selected_segment == 2 && (p->k_type == 1 || p->k_type == 3))
      {
        decaly = fmaxf(decaly, p->kya - p->kyd);
        decaly = fmaxf(decaly, p->kyb - p->kyc);
        p->kxc += decalx;
        p->kyc += decaly;
        p->kxd += decalx;
        p->kyd += decaly;
      }
      else if(g->k_selected_segment == 3 && (p->k_type == 2 || p->k_type == 3))
      {
        decalx = fminf(decalx, p->kxb - p->kxa);
        decalx = fminf(decalx, p->kxc - p->kxd);
        p->kxa += decalx;
        p->kya += decaly;
        p->kxd += decalx;
        p->kyd += decaly;
      }
      g->button_down_zoom_x = pzx;
      g->button_down_zoom_y = pzy;
      dt_control_queue_redraw_center();
      return 1;
    }
    // draw a light gray frame, to show it's not stored yet:
    g->applied = 0;
    // first mouse button, adjust cropping frame, but what do we do?
    const float bzx = g->button_down_zoom_x + .5f, bzy = g->button_down_zoom_y + .5f;
    if(g->cropping == GRAB_CENTER && !g->straightening && g->k_show != 1)
    {
      g->cropping = grab;
      if(grab == GRAB_CENTER)
      {
        g->cropping = GRAB_ALL;
        g->handle_x = g->clip_x;
        g->handle_y = g->clip_y;
      }
      if(grab & GRAB_LEFT) g->handle_x = bzx - g->clip_x;
      if(grab & GRAB_TOP) g->handle_y = bzy - g->clip_y;
      if(grab & GRAB_RIGHT) g->handle_x = bzx - (g->clip_w + g->clip_x);
      if(grab & GRAB_BOTTOM) g->handle_y = bzy - (g->clip_h + g->clip_y);
      if(!grab && darktable.control->button_down_which == 3) g->straightening = 1;
    }
    if(!g->straightening && darktable.control->button_down_which == 1 && g->k_show != 1)
    {
      grab = g->cropping;

      if(grab == GRAB_ALL)
      {
        /* moving the crop window */
        if(!g->shift_hold)
          g->clip_x
              = fminf(g->clip_max_w + g->clip_max_x - g->clip_w, fmaxf(g->clip_max_x, g->handle_x + pzx - bzx));

        if(!g->ctrl_hold)
          g->clip_y
              = fminf(g->clip_max_h + g->clip_max_y - g->clip_h, fmaxf(g->clip_max_y, g->handle_y + pzy - bzy));
      }
      else
      {
        /* changing the crop window */
        if(g->shift_hold)
        {
          /* the center is locked, scale crop radial with locked ratio */
          float xx = 0.0f;
          float yy = 0.0f;
          if(grab & GRAB_LEFT || grab & GRAB_RIGHT) xx = (grab & GRAB_LEFT) ? (pzx - bzx) : (bzx - pzx);
          if(grab & GRAB_TOP || grab & GRAB_BOTTOM) yy = (grab & GRAB_TOP) ? (pzy - bzy) : (bzy - pzy);

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

          if(grab & GRAB_LEFT)
          {
            const float old_clip_x = g->clip_x;
            g->clip_x = fminf(fmaxf(g->clip_max_x, pzx - g->handle_x), g->clip_x + g->clip_w - 0.1f);
            g->clip_w = old_clip_x + g->clip_w - g->clip_x;
          }
          if(grab & GRAB_TOP)
          {
            const float old_clip_y = g->clip_y;
            g->clip_y = fminf(fmaxf(g->clip_max_y, pzy - g->handle_y), g->clip_y + g->clip_h - 0.1f);
            g->clip_h = old_clip_y + g->clip_h - g->clip_y;
          }
          if(grab & GRAB_RIGHT)
            g->clip_w = fmaxf(0.1f, fminf(g->clip_max_w + g->clip_max_x, pzx - g->clip_x - g->handle_x));
          if(grab & GRAB_BOTTOM)
            g->clip_h = fmaxf(0.1f, fminf(g->clip_max_h + g->clip_max_y, pzy - g->clip_y - g->handle_y));
        }

        if(g->clip_x + g->clip_w > g->clip_max_w + g->clip_max_x)
          g->clip_w = g->clip_max_w + g->clip_max_x - g->clip_x;
        if(g->clip_y + g->clip_h > g->clip_max_h + g->clip_max_y)
          g->clip_h = g->clip_max_h + g->clip_max_y - g->clip_y;
      }

      apply_box_aspect(self, grab);
      // we save crop params too
      float points[4]
          = { g->clip_x * wd, g->clip_y * ht, (g->clip_x + g->clip_w) * wd, (g->clip_y + g->clip_h) * ht };

      if(dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, points, 2))
      {
        dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
        if(piece)
        {
          // only update the sliders, not the dt_iop_clipping_params_t structure, so that the call to
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
        }
      }
    }
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(grab && g->k_show != 1)
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
      dt_control_hinter_message(darktable.control, _("<b>commit</b>: double-click, <b>straighten</b>: right-drag"));
      dt_control_change_cursor(GDK_LEFT_PTR);
    }
    if(grab != GRAB_NONE)
      dt_control_hinter_message(darktable.control, _("<b>resize</b>: drag, <b>keep aspect ratio</b>: shift+drag\n"
                                                     "<b>straighten</b>: right-drag"));
    dt_control_queue_redraw_center();
  }
  else
  {
    // somewhere besides borders. maybe rotate?
    dt_control_change_cursor(GDK_FLEUR);
    g->straightening = g->cropping = 0;
    // or maybe keystone
    const float pr_d = darktable.develop->preview_downsampling;
    // slightly adjust the size of keystone control area depending on the downsampling
    const float ext = DT_PIXEL_APPLY_DPI(0.005f + ((1.0f - pr_d) / 100.0f)) / zoom_scale;
    if(g->k_show == 1 && g->k_drag == FALSE)
    {
      float pts[2] = { pzx * wd, pzy * ht };
      dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 1);
      dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
      float xx = pts[0] / (float)piece->buf_out.width, yy = pts[1] / (float)piece->buf_out.height;
      // are we near a keystone point ?
      g->k_selected = -1;
      g->k_selected_segment = -1;
      if(xx < p->kxa + ext && xx > p->kxa - ext && yy < p->kya + ext && yy > p->kya - ext) g->k_selected = 0;
      if(xx < p->kxb + ext && xx > p->kxb - ext && yy < p->kyb + ext && yy > p->kyb - ext) g->k_selected = 1;
      if(xx < p->kxc + ext && xx > p->kxc - ext && yy < p->kyc + ext && yy > p->kyc - ext) g->k_selected = 2;
      if(xx < p->kxd + ext && xx > p->kxd - ext && yy < p->kyd + ext && yy > p->kyd - ext) g->k_selected = 3;
      // or near a keystone segment
      if(g->k_selected < 0)
      {
        if(p->k_type == 1 || p->k_type == 3)
        {
          if(dist_seg(p->kxa, p->kya, p->kxb, p->kyb, xx, yy) < ext * ext)
            g->k_selected_segment = 0;
          else if(dist_seg(p->kxd, p->kyd, p->kxc, p->kyc, xx, yy) < ext * ext)
            g->k_selected_segment = 2;
          if(dist_seg(p->kxb, p->kyb, p->kxc, p->kyc, xx, yy) < ext * ext)
            g->k_selected_segment = 1;
          else if(dist_seg(p->kxd, p->kyd, p->kxa, p->kya, xx, yy) < ext * ext)
            g->k_selected_segment = 3;
        }
      }
      if(g->k_selected >= 0)
      {
        dt_control_hinter_message(darktable.control, _("<b>move control point</b>: drag"));
        dt_control_change_cursor(GDK_CROSS);
      }
      else if(g->k_selected_segment >= 0)
      {
        dt_control_hinter_message(darktable.control, _("<b>move line</b>: drag, <b>toggle symmetry</b>: click <tt>ꝏ</tt>"));
        dt_control_change_cursor(GDK_CROSS);
      }
      else
      {
        dt_control_hinter_message(darktable.control, _("<b>apply</b>: click <tt>ok</tt>, <b>toggle symmetry</b>: click <tt>ꝏ</tt>\n"
                                                       "<b>move line/control point</b>: drag"));
        dt_control_change_cursor(GDK_FLEUR);
      }
    }
    else
    {
      dt_control_hinter_message(darktable.control, _("<b>move</b>: drag, <b>move vertically</b>: shift+drag, <b>move horizontally</b>: ctrl+drag\n"
                                                     "<b>straighten</b>: right-drag, <b>commit</b>: double-click"));
    }
    dt_control_queue_redraw_center();
  }
  return 0;
}

static void commit_box(dt_iop_module_t *self, dt_iop_clipping_gui_data_t *g, dt_iop_clipping_params_t *p)
{
  if(darktable.gui->reset) return;
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
  float points[4]
      = { g->clip_x * wd, g->clip_y * ht, (g->clip_x + g->clip_w) * wd, (g->clip_y + g->clip_h) * ht };
  if(dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, points, 2))
  {
    dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
    if(piece)
    {
      p->cx = CLAMPF(points[0] / (float)piece->buf_out.width, 0.0f, 0.9f);
      p->cy = CLAMPF(points[1] / (float)piece->buf_out.height, 0.0f, 0.9f);
      p->cw = copysignf(CLAMPF(points[2] / (float)piece->buf_out.width, 0.1f, 1.0f), p->cw);
      p->ch = copysignf(CLAMPF(points[3] / (float)piece->buf_out.height, 0.1f, 1.0f), p->ch);
    }
  }
  g->applied = 1;
  const gboolean changed = fabs(p->cx - old[0]) > eps || fabs(p->cy - old[1]) > eps || fabs(p->cw - old[2]) > eps || fabs(p->ch - old[3]) > eps;
  // fprintf(stderr, "[crop commit box] %i:  %e %e %e %e\n", changed, p->cx - old[0], p->cy - old[1], p->cw - old[2], p->ch - old[3]);
  if(changed) dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  // we don't do anything if the image is not ready
  if(!g->preview_ready) return 0;

  if(g->straightening)
  {
    // adjust the line with possible current angle and flip on this module
    dt_boundingbox_t pts = { x, y, g->button_down_x, g->button_down_y };
    dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_INCL, pts, 2);

    float dx = pts[0] - pts[2];
    float dy = pts[1] - pts[3];
    if(dx < 0)
    {
      dx = -dx;
      dy = -dy;
    }

    float angle = atan2f(dy, dx);
    if(!(angle >= -M_PI / 2.0 && angle <= M_PI / 2.0)) angle = 0.0f;
    float close = angle;
    if(close > M_PI / 4.0)
      close = M_PI / 2.0 - close;
    else if(close < -M_PI / 4.0)
      close = -M_PI / 2.0 - close;
    else
      close = -close;

    float a = 180.0 / M_PI * close;
    if(a < -180.0) a += 360.0;
    if(a > 180.0) a -= 360.0;

    dt_bauhaus_slider_set(g->angle, a);
    dt_control_change_cursor(GDK_LEFT_PTR);
  }
  if(g->k_drag) g->k_drag = FALSE;

  /* reset internal ui states*/
  g->straightening = g->cropping = 0;
  g->shift_hold = FALSE;
  g->ctrl_hold = FALSE;
  return 1;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{

  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  // we don't do anything if the image is not ready
  if(!g->preview_ready) return 0;

  // avoid unexpected back to lt mode:
  if(type == GDK_2BUTTON_PRESS && which == 1)
  {
    dt_iop_request_focus(NULL);
    commit_box(self, g, p);
    return 1;
  }
  if(which == 3 || which == 1)
  {
    // switch module on already, other code depends in this:
    dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);

    if(g->k_show == 1)
    {
      if(g->k_selected >= 0)
        g->k_drag = TRUE; // if a keystone point is selected then we start to drag it
      else // if we click to the apply button
      {
        const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
        const int closeup = dt_control_get_dev_closeup();
        const float zoom_scale = dt_dev_get_zoom_scale(self->dev, zoom, 1<<closeup, 1);
        float pzx, pzy;
        dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
        pzx += 0.5f;
        pzy += 0.5f;

        dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
        const float wp = piece->buf_out.width, hp = piece->buf_out.height;
        float pts[8] = { p->kxa * wp, p->kya * hp, p->kxb * wp, p->kyb * hp,
                         p->kxc * wp, p->kyc * hp, p->kxd * wp, p->kyd * hp };
        dt_dev_distort_transform_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 4);

        const float xx = pzx * self->dev->preview_pipe->backbuf_width,
                    yy = pzy * self->dev->preview_pipe->backbuf_height;
        float c[2] = { (MIN(pts[4], pts[2]) + MAX(pts[0], pts[6])) / 2.0f,
                       (MIN(pts[5], pts[7]) + MAX(pts[1], pts[3])) / 2.0f };
        const float ext = DT_PIXEL_APPLY_DPI(10.0) / (zoom_scale);
        // Apply button
        if(xx > c[0] - ext && xx < c[0] + ext && yy > c[1] - ext && yy < c[1] + ext)
        {
          // add an entry to the combo box and select it
          keystone_type_populate(self, TRUE, 99);
          // reset gui settings
          g->k_show = 2;
          g->k_selected = -1;
          g->k_drag = FALSE;
          // do the changes
          p->k_apply = 1;
          commit_box(self, g, p);
        }
        else
        {
          // Horizontal symmetry button (1)
          c[0] = (pts[0] + pts[6]) / 2.0f, c[1] = (pts[1] + pts[7]) / 2.0f;
          if(xx > c[0] - ext && xx < c[0] + ext && yy > c[1] - ext && yy < c[1] + ext
             && (p->k_type == 1 || p->k_type == 3))
          {
            if(p->k_sym == 0)
              p->k_sym = 1;
            else if(p->k_sym == 1)
              p->k_sym = 0;
            else if(p->k_sym == 2)
              p->k_sym = 3;
            else
              p->k_sym = 2;
          }
          else
          {
            // Horizontal symmetry button (2)
            c[0] = (pts[2] + pts[4]) / 2.0f, c[1] = (pts[3] + pts[5]) / 2.0f;
            if(xx > c[0] - ext && xx < c[0] + ext && yy > c[1] - ext && yy < c[1] + ext
               && (p->k_type == 1 || p->k_type == 3))
            {
              if(p->k_sym == 0)
                p->k_sym = 1;
              else if(p->k_sym == 1)
                p->k_sym = 0;
              else if(p->k_sym == 2)
                p->k_sym = 3;
              else
                p->k_sym = 2;
            }
            else
            {
              // vertical symmetry button (1)
              c[0] = (pts[2] + pts[0]) / 2.0f, c[1] = (pts[3] + pts[1]) / 2.0f;
              if(xx > c[0] - ext && xx < c[0] + ext && yy > c[1] - ext && yy < c[1] + ext
                 && (p->k_type == 2 || p->k_type == 3))
              {
                if(p->k_sym == 0)
                  p->k_sym = 2;
                else if(p->k_sym == 1)
                  p->k_sym = 3;
                else if(p->k_sym == 2)
                  p->k_sym = 0;
                else
                  p->k_sym = 1;
              }
              else
              {
                // vertical symmetry button (2)
                c[0] = (pts[4] + pts[6]) / 2.0f, c[1] = (pts[5] + pts[7]) / 2.0f;
                if(xx > c[0] - ext && xx < c[0] + ext && yy > c[1] - ext && yy < c[1] + ext
                   && (p->k_type == 2 || p->k_type == 3))
                {
                  if(p->k_sym == 0)
                    p->k_sym = 2;
                  else if(p->k_sym == 1)
                    p->k_sym = 3;
                  else if(p->k_sym == 2)
                    p->k_sym = 0;
                  else
                    p->k_sym = 1;
                }
                else
                {
                  // dragging a border ?
                  if(g->k_selected_segment >= 0)
                  {
                    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &g->button_down_zoom_x,
                                                &g->button_down_zoom_y);
                    g->button_down_zoom_x += 0.5;
                    g->button_down_zoom_y += 0.5;
                    g->k_drag = TRUE;
                  }
                }
              }
            }
          }
        }
      }
    }
    else
    {
      g->button_down_x = x;
      g->button_down_y = y;
      dt_dev_get_pointer_zoom_pos(self->dev, x, y, &g->button_down_zoom_x, &g->button_down_zoom_y);
      g->button_down_angle = p->angle;

      /* update prev clip box with current */
      g->prev_clip_x = g->clip_x;
      g->prev_clip_y = g->clip_y;
      g->prev_clip_w = g->clip_w;
      g->prev_clip_h = g->clip_h;

      /* if shift is pressed, then lock crop on center */
      if(dt_modifiers_include(state, GDK_SHIFT_MASK)) g->shift_hold = TRUE;
      if(dt_modifiers_include(state, GDK_CONTROL_MASK)) g->ctrl_hold = TRUE;
    }

    return 1;
  }
  else
    return 0;
}

GSList *mouse_actions(struct dt_iop_module_t *self)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_LEFT_DRAG, 0, _("[%s on borders] crop"), self->name());
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_LEFT_DRAG, GDK_SHIFT_MASK,
                                     _("[%s on borders] crop keeping ratio"), self->name());
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_RIGHT_DRAG, 0, _("[%s] define/rotate horizon"), self->name());
  return lm;
}

#undef PHI
#undef INVPHI

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

