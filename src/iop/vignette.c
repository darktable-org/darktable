/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#ifdef HAVE_GEGL
#include <gegl.h>
#endif
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/blend.h"
#include "control/control.h"
#include "common/opencl.h"
#include "bauhaus/bauhaus.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/presets.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(4, dt_iop_vignette_params_t)

#define CLIP(x) ((x < 0) ? 0.0 : (x > 1.0) ? 1.0 : x)
#define TEA_ROUNDS 8

typedef enum dt_iop_dither_t
{
  DITHER_OFF = 0,
  DITHER_8BIT = 1,
  DITHER_16BIT = 2
} dt_iop_dither_t;

typedef struct dt_iop_dvector_2d_t
{
  double x;
  double y;
} dt_iop_dvector_2d_t;

typedef struct dt_iop_fvector_2d_t
{
  float x;
  float y;
} dt_iop_vector_2d_t;

typedef struct dt_iop_vignette_params1_t
{
  double scale;         // 0 - 100 Radie
  double falloff_scale; // 0 - 100 Radie for falloff inner radie of falloff=scale and
                        // outer=scale+falloff_scale
  double strength;      // 0 - 1 strength of effect
  double uniformity;    // 0 - 1 uniformity of center
  double bsratio;       // -1 - +1 ratio of brightness/saturation effect
  gboolean invert_falloff;
  gboolean invert_saturation;
  dt_iop_dvector_2d_t center; // Center of vignette
} dt_iop_vignette_params1_t;

typedef struct dt_iop_vignette_params2_t
{
  float scale;               // 0 - 100 Inner radius, percent of largest image dimension
  float falloff_scale;       // 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;          // -1 - 1 Strength of brightness reduction
  float saturation;          // -1 - 1 Strength of saturation reduction
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;        //
  float whratio;             // 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;
} dt_iop_vignette_params2_t;

typedef struct dt_iop_vignette_params3_t
{
  float scale;               // 0 - 100 Inner radius, percent of largest image dimension
  float falloff_scale;       // 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;          // -1 - 1 Strength of brightness reduction
  float saturation;          // -1 - 1 Strength of saturation reduction
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;        //
  float whratio;             // 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;
  int dithering; // if and how to perform dithering
} dt_iop_vignette_params3_t;

typedef struct dt_iop_vignette_params_t
{
  float scale;               // 0 - 100 Inner radius, percent of largest image dimension
  float falloff_scale;       // 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;          // -1 - 1 Strength of brightness reduction
  float saturation;          // -1 - 1 Strength of saturation reduction
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;        //
  float whratio;             // 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;
  int dithering;    // if and how to perform dithering
  gboolean unbound; // whether the values should be clipped
} dt_iop_vignette_params_t;


typedef struct dt_iop_vignette_gui_data_t
{
  GtkWidget *scale;
  GtkWidget *falloff_scale;
  GtkWidget *brightness;
  GtkWidget *saturation;
  GtkWidget *center_x;
  GtkWidget *center_y;
  GtkToggleButton *autoratio;
  GtkWidget *whratio;
  GtkWidget *shape;
  GtkWidget *dithering;
} dt_iop_vignette_gui_data_t;

typedef struct dt_iop_vignette_data_t
{
  float scale;
  float falloff_scale;
  float brightness;
  float saturation;
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;
  float whratio;
  float shape;
  int dithering;
  gboolean unbound;
} dt_iop_vignette_data_t;

typedef struct dt_iop_vignette_global_data_t
{
  int kernel_vignette;
} dt_iop_vignette_global_data_t;

const char *name()
{
  return _("vignetting");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING
         | IOP_FLAGS_TILING_FULL_ROI;
}

int groups()
{
  return IOP_GROUP_EFFECT;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "scale"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "fall-off strength"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "brightness"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "saturation"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "horizontal center"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "vertical center"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "shape"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "width-height ratio"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "dithering"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "scale", GTK_WIDGET(g->scale));
  dt_accel_connect_slider_iop(self, "fall-off strength", GTK_WIDGET(g->falloff_scale));
  dt_accel_connect_slider_iop(self, "brightness", GTK_WIDGET(g->brightness));
  dt_accel_connect_slider_iop(self, "saturation", GTK_WIDGET(g->saturation));
  dt_accel_connect_slider_iop(self, "horizontal center", GTK_WIDGET(g->center_x));
  dt_accel_connect_slider_iop(self, "vertical center", GTK_WIDGET(g->center_y));
  dt_accel_connect_slider_iop(self, "shape", GTK_WIDGET(g->shape));
  dt_accel_connect_slider_iop(self, "width-height ratio", GTK_WIDGET(g->whratio));
  dt_accel_connect_slider_iop(self, "dithering", GTK_WIDGET(g->dithering));
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 4)
  {
    const dt_iop_vignette_params1_t *old = old_params;
    dt_iop_vignette_params_t *new = new_params;
    new->scale = old->scale;
    new->falloff_scale = old->falloff_scale;
    new->brightness = -(1.0 - MAX(old->bsratio, 0.0)) * old->strength / 100.0;
    new->saturation = -(1.0 + MIN(old->bsratio, 0.0)) * old->strength / 100.0;
    if(old->invert_saturation) new->saturation *= -2.0; // Double effect for increasing saturation
    if(old->invert_falloff) new->brightness = -new->brightness;
    new->center.x = old->center.x;
    new->center.y = old->center.y;
    new->autoratio = TRUE;
    new->whratio = 1.0;
    new->shape = 1.0;
    new->dithering = DITHER_OFF;
    new->unbound = FALSE;
    return 0;
  }
  if(old_version == 2 && new_version == 4)
  {
    const dt_iop_vignette_params2_t *old = old_params;
    dt_iop_vignette_params_t *new = new_params;
    new->scale = old->scale;
    new->falloff_scale = old->falloff_scale;
    new->brightness = old->brightness;
    new->saturation = old->saturation;
    new->center.x = old->center.x;
    new->center.y = old->center.y;
    new->autoratio = old->autoratio;
    new->whratio = old->whratio;
    new->shape = old->shape;
    new->dithering = DITHER_OFF;
    new->unbound = FALSE;
    return 0;
  }
  if(old_version == 3 && new_version == 4)
  {
    const dt_iop_vignette_params3_t *old = old_params;
    dt_iop_vignette_params_t *new = new_params;
    new->scale = old->scale;
    new->falloff_scale = old->falloff_scale;
    new->brightness = old->brightness;
    new->saturation = old->saturation;
    new->center.x = old->center.x;
    new->center.y = old->center.y;
    new->autoratio = old->autoratio;
    new->whratio = old->whratio;
    new->shape = old->shape;
    new->dithering = old->dithering;
    new->unbound = FALSE;
    return 0;
  }

  return 1;
}


static void encrypt_tea(unsigned int *arg)
{
  const unsigned int key[] = { 0xa341316c, 0xc8013ea4, 0xad90777d, 0x7e95761e };
  unsigned int v0 = arg[0], v1 = arg[1];
  unsigned int sum = 0;
  unsigned int delta = 0x9e3779b9;
  for(int i = 0; i < TEA_ROUNDS; i++)
  {
    sum += delta;
    v0 += ((v1 << 4) + key[0]) ^ (v1 + sum) ^ ((v1 >> 5) + key[1]);
    v1 += ((v0 << 4) + key[2]) ^ (v0 + sum) ^ ((v0 >> 5) + key[3]);
  }
  arg[0] = v0;
  arg[1] = v1;
}

static float tpdf(unsigned int urandom)
{
  float frandom = (float)urandom / 0xFFFFFFFFu;

  return (frandom < 0.5f ? (sqrtf(2.0f * frandom) - 1.0f) : (1.0f - sqrtf(2.0f * (1.0f - frandom))));
}

static int get_grab(float pointerx, float pointery, float startx, float starty, float endx, float endy,
                    float zoom_scale)
{
  const float radius = 5.0 / zoom_scale;

  if(powf(pointerx - startx, 2) + powf(pointery, 2) <= powf(radius, 2)) return 2; // x size
  if(powf(pointerx, 2) + powf(pointery - starty, 2) <= powf(radius, 2)) return 4; // y size
  if(powf(pointerx, 2) + powf(pointery, 2) <= powf(radius, 2)) return 1;          // center
  if(powf(pointerx - endx, 2) + powf(pointery, 2) <= powf(radius, 2)) return 8;   // x falloff
  if(powf(pointerx, 2) + powf(pointery - endy, 2) <= powf(radius, 2)) return 16;  // y falloff

  return 0;
}

static void draw_overlay(cairo_t *cr, float x, float y, float fx, float fy, int grab, float zoom_scale)
{
  // half width/height of the crosshair
  float crosshair_w = DT_PIXEL_APPLY_DPI(10.0) / zoom_scale;
  float crosshair_h = DT_PIXEL_APPLY_DPI(10.0) / zoom_scale;

  // center crosshair
  cairo_move_to(cr, -crosshair_w, 0.0);
  cairo_line_to(cr, crosshair_w, 0.0);
  cairo_move_to(cr, 0.0, -crosshair_h);
  cairo_line_to(cr, 0.0, crosshair_h);
  cairo_stroke(cr);

  // inner border of the vignette
  cairo_save(cr);
  if(x <= y)
  {
    cairo_scale(cr, x / y, 1.0);
    cairo_arc(cr, 0.0, 0.0, y, 0.0, M_PI * 2.0);
  }
  else
  {
    cairo_scale(cr, 1.0, y / x);
    cairo_arc(cr, 0.0, 0.0, x, 0.0, M_PI * 2.0);
  }
  cairo_restore(cr);
  cairo_stroke(cr);

  // outer border of the vignette
  cairo_save(cr);
  if(fx <= fy)
  {
    cairo_scale(cr, fx / fy, 1.0);
    cairo_arc(cr, 0.0, 0.0, fy, 0.0, M_PI * 2.0);
  }
  else
  {
    cairo_scale(cr, 1.0, fy / fx);
    cairo_arc(cr, 0.0, 0.0, fx, 0.0, M_PI * 2.0);
  }
  cairo_restore(cr);
  cairo_stroke(cr);

  // the handles
  const float radius_sel = DT_PIXEL_APPLY_DPI(6.0) / zoom_scale;
  const float radius_reg = DT_PIXEL_APPLY_DPI(4.0) / zoom_scale;
  if(grab == 1)
    cairo_arc(cr, 0.0, 0.0, radius_sel, 0.0, M_PI * 2.0);
  else
    cairo_arc(cr, 0.0, 0.0, radius_reg, 0.0, M_PI * 2.0);
  cairo_stroke(cr);
  if(grab == 2)
    cairo_arc(cr, x, 0.0, radius_sel, 0.0, M_PI * 2.0);
  else
    cairo_arc(cr, x, 0.0, radius_reg, 0.0, M_PI * 2.0);
  cairo_stroke(cr);
  if(grab == 4)
    cairo_arc(cr, 0.0, -y, radius_sel, 0.0, M_PI * 2.0);
  else
    cairo_arc(cr, 0.0, -y, radius_reg, 0.0, M_PI * 2.0);
  cairo_stroke(cr);
  if(grab == 8)
    cairo_arc(cr, fx, 0.0, radius_sel, 0.0, M_PI * 2.0);
  else
    cairo_arc(cr, fx, 0.0, radius_reg, 0.0, M_PI * 2.0);
  cairo_stroke(cr);
  if(grab == 16)
    cairo_arc(cr, 0.0, -fy, radius_sel, 0.0, M_PI * 2.0);
  else
    cairo_arc(cr, 0.0, -fy, radius_reg, 0.0, M_PI * 2.0);
  cairo_stroke(cr);
}

// FIXME: For portrait images the overlay is a bit off. The coordinates in mouse_moved seem to be ok though.
// WTF?
void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  //   dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;

  float wd = dev->preview_pipe->backbuf_width;
  float ht = dev->preview_pipe->backbuf_height;
  float bigger_side, smaller_side;
  if(wd >= ht)
  {
    bigger_side = wd;
    smaller_side = ht;
  }
  else
  {
    bigger_side = ht;
    smaller_side = wd;
  }
  float zoom_y = dt_control_get_dev_zoom_y();
  float zoom_x = dt_control_get_dev_zoom_x();
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  cairo_translate(cr, width / 2.0, height / 2.0);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  float vignette_x = (p->center.x + 1.0) * 0.5 * wd;
  float vignette_y = (p->center.y + 1.0) * 0.5 * ht;

  cairo_translate(cr, vignette_x, vignette_y);

  float vignette_w = p->scale * 0.01 * 0.5 * wd; // start of falloff
  float vignette_h = p->scale * 0.01 * 0.5 * ht;
  float vignette_fx = vignette_w + p->falloff_scale * 0.01 * 0.5 * wd; // end of falloff
  float vignette_fy = vignette_h + p->falloff_scale * 0.01 * 0.5 * ht;

  if(p->autoratio == FALSE)
  {
    float factor1 = bigger_side / smaller_side;
    if(wd >= ht)
    {
      float factor2 = (2.0 - p->whratio) * factor1;

      if(p->whratio <= 1)
      {
        vignette_h *= factor1;
        vignette_w *= p->whratio;
        vignette_fx *= p->whratio;
        vignette_fy *= factor1;
      }
      else
      {
        vignette_h *= factor2;
        vignette_fy *= factor2;
      }
    }
    else
    {
      float factor2 = (p->whratio) * factor1;

      if(p->whratio <= 1)
      {
        vignette_w *= factor2;
        vignette_fx *= factor2;
      }
      else
      {
        vignette_w *= factor1;
        vignette_h *= (2.0 - p->whratio);
        vignette_fx *= factor1;
        vignette_fy *= (2.0 - p->whratio);
      }
    }
  }

  int grab = get_grab(pzx * wd - vignette_x, pzy * ht - vignette_y, vignette_w, -vignette_h, vignette_fx,
                      -vignette_fy, zoom_scale);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3.0) / zoom_scale);
  cairo_set_source_rgba(cr, .3, .3, .3, .8);
  draw_overlay(cr, vignette_w, vignette_h, vignette_fx, vignette_fy, grab, zoom_scale);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0) / zoom_scale);
  cairo_set_source_rgba(cr, .8, .8, .8, .8);
  draw_overlay(cr, vignette_w, vignette_h, vignette_fx, vignette_fy, grab, zoom_scale);
}

// FIXME: Pumping of the opposite direction when changing width/height. See two FIXMEs further down.
int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  float wd = self->dev->preview_pipe->backbuf_width;
  float ht = self->dev->preview_pipe->backbuf_height;
  float bigger_side, smaller_side;
  if(wd >= ht)
  {
    bigger_side = wd;
    smaller_side = ht;
  }
  else
  {
    bigger_side = ht;
    smaller_side = wd;
  }
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_scale = dt_dev_get_zoom_scale(self->dev, zoom, closeup ? 2 : 1, 1);
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  static int old_grab = -1;
  int grab = old_grab;

  float vignette_x = (p->center.x + 1.0) * 0.5 * wd;
  float vignette_y = (p->center.y + 1.0) * 0.5 * ht;

  float vignette_w = p->scale * 0.01 * 0.5 * wd; // start of falloff
  float vignette_h = p->scale * 0.01 * 0.5 * ht;
  float vignette_fx = vignette_w + p->falloff_scale * 0.01 * 0.5 * wd; // end of falloff
  float vignette_fy = vignette_h + p->falloff_scale * 0.01 * 0.5 * ht;

  if(p->autoratio == FALSE)
  {
    float factor1 = bigger_side / smaller_side;
    if(wd >= ht)
    {
      float factor2 = (2.0 - p->whratio) * factor1;

      if(p->whratio <= 1)
      {
        vignette_h *= factor1;
        vignette_w *= p->whratio;
        vignette_fx *= p->whratio;
        vignette_fy *= factor1;
      }
      else
      {
        vignette_h *= factor2;
        vignette_fy *= factor2;
      }
    }
    else
    {
      float factor2 = (p->whratio) * factor1;

      if(p->whratio <= 1)
      {
        vignette_w *= factor2;
        vignette_fx *= factor2;
      }
      else
      {
        vignette_w *= factor1;
        vignette_h *= (2.0 - p->whratio);
        vignette_fx *= factor1;
        vignette_fy *= (2.0 - p->whratio);
      }
    }
  }

  if(grab == 0 || !(darktable.control->button_down && darktable.control->button_down_which == 1))
  {
    grab = get_grab(pzx * wd - vignette_x, pzy * ht - vignette_y, vignette_w, -vignette_h, vignette_fx,
                    -vignette_fy, zoom_scale);
  }

  if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    if(grab == 0) // pan the image
    {
      dt_control_change_cursor(GDK_HAND1);
      return 0;
    }
    else if(grab == 1) // move the center
    {
      dt_bauhaus_slider_set(g->center_x, pzx * 2.0 - 1.0);
      dt_bauhaus_slider_set(g->center_y, pzy * 2.0 - 1.0);
    }
    else if(grab == 2) // change the width
    {
      float max = 0.5 * ((p->whratio <= 1.0) ? bigger_side * p->whratio : bigger_side);
      float new_vignette_w = MIN(bigger_side * 0.5, MAX(0.1, pzx * wd - vignette_x));
      float ratio = new_vignette_w / vignette_h;
      float new_scale = 100.0 * new_vignette_w / max;
      // FIXME: When going over the 1.0 boundary from wide to narrow (>1.0 -> <=1.0) the height slightly
      // changes, depending on speed.
      //        I guess we have to split the computation.
      if(ratio <= 1.0)
      {
        if(which == GDK_CONTROL_MASK)
        {
          dt_bauhaus_slider_set(g->scale, new_scale);
        }
        else
        {
          dt_bauhaus_slider_set(g->whratio, ratio);
        }
      }
      else
      {
        dt_bauhaus_slider_set(g->scale, new_scale);

        if(which != GDK_CONTROL_MASK)
        {
          float new_whratio = 2.0 - 1.0 / ratio;
          dt_bauhaus_slider_set(g->whratio, new_whratio);
        }
      }
    }
    else if(grab == 4) // change the height
    {
      float new_vignette_h = MIN(bigger_side * 0.5, MAX(0.1, vignette_y - pzy * ht));
      float ratio = new_vignette_h / vignette_w;
      float max = 0.5 * ((ratio <= 1.0) ? bigger_side * (2.0 - p->whratio) : bigger_side);
      // FIXME: When going over the 1.0 boundary from narrow to wide (>1.0 -> <=1.0) the width slightly
      // changes, depending on speed.
      //        I guess we have to split the computation.
      if(ratio <= 1.0)
      {
        if(which == GDK_CONTROL_MASK)
        {
          float new_scale = 100.0 * new_vignette_h / max;
          dt_bauhaus_slider_set(g->scale, new_scale);
        }
        else
        {
          dt_bauhaus_slider_set(g->whratio, 2.0 - ratio);
        }
      }
      else
      {
        float new_scale = 100.0 * new_vignette_h / max;
        dt_bauhaus_slider_set(g->scale, new_scale);

        if(which != GDK_CONTROL_MASK)
        {
          float new_whratio = 1.0 / ratio;
          dt_bauhaus_slider_set(g->whratio, new_whratio);
        }
      }
    }
    else if(grab == 8) // change the falloff on the right
    {
      float new_vignette_fx = pzx * wd - vignette_x;
      float max = 0.5 * ((p->whratio <= 1.0) ? bigger_side * p->whratio : bigger_side);
      float delta_x = MIN(max, MAX(0.0, new_vignette_fx - vignette_w));
      float new_falloff = 100.0 * delta_x / max;
      dt_bauhaus_slider_set(g->falloff_scale, new_falloff);
    }
    else if(grab == 16) // change the falloff on the top
    {
      float new_vignette_fy = vignette_y - pzy * ht;
      float max = 0.5 * ((p->whratio > 1.0) ? bigger_side * (2.0 - p->whratio) : bigger_side);
      float delta_y = MIN(max, MAX(0.0, new_vignette_fy - vignette_h));
      float new_falloff = 100.0 * delta_y / max;
      dt_bauhaus_slider_set(g->falloff_scale, new_falloff);
    }
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(grab)
  {
    if(grab == 1)
      dt_control_change_cursor(GDK_FLEUR);
    else if(grab == 2)
      dt_control_change_cursor(GDK_SB_H_DOUBLE_ARROW);
    else if(grab == 4)
      dt_control_change_cursor(GDK_SB_V_DOUBLE_ARROW);
    else if(grab == 8)
      dt_control_change_cursor(GDK_SB_H_DOUBLE_ARROW);
    else if(grab == 16)
      dt_control_change_cursor(GDK_SB_V_DOUBLE_ARROW);
  }
  else
  {
    if(old_grab != grab) dt_control_change_cursor(GDK_LEFT_PTR);
  }
  old_grab = grab;
  dt_control_queue_redraw_center();
  return 0;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  if(which == 1) return 1;
  return 0;
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  if(which == 1) return 1;
  return 0;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const dt_iop_vignette_data_t *data = (dt_iop_vignette_data_t *)piece->data;
  const dt_iop_roi_t *buf_in = &piece->buf_in;
  const int ch = piece->colors;
  const gboolean unbound = data->unbound;

  /* Center coordinates of buf_in, these should not consider buf_in->{x,y}! */
  const dt_iop_vector_2d_t buf_center = { buf_in->width * .5f, buf_in->height * .5f };
  /* Center coordinates of vignette center */
  const dt_iop_vector_2d_t vignette_center = { buf_center.x + data->center.x * buf_in->width / 2.0,
                                               buf_center.y + data->center.y * buf_in->height / 2.0 };
  /* Coordinates of vignette_center in terms of roi_in */
  const dt_iop_vector_2d_t roi_center
      = { vignette_center.x * roi_in->scale - roi_in->x, vignette_center.y * roi_in->scale - roi_in->y };
  float xscale;
  float yscale;

  /* w/h ratio follows piece dimensions */
  if(data->autoratio)
  {
    xscale = 2.0 / (buf_in->width * roi_out->scale);
    yscale = 2.0 / (buf_in->height * roi_out->scale);
  }
  else /* specified w/h ratio, scale proportional to longest side */
  {
    const float basis = 2.0 / (MAX(buf_in->height, buf_in->width) * roi_out->scale);
    // w/h ratio from 0-1 use as-is
    if(data->whratio <= 1.0)
    {
      yscale = basis;
      xscale = yscale / data->whratio;
    }
    // w/h ratio from 1-2 interpret as 1-inf
    // that is, the h/w ratio + 1
    else
    {
      xscale = basis;
      yscale = xscale / (2.0 - data->whratio);
    }
  }
  const float dscale = data->scale / 100.0;
  // A minimum falloff is used, based on the image size, to smooth out aliasing artifacts
  const float min_falloff = 100.0 / MIN(buf_in->width, buf_in->height);
  const float fscale = MAX(data->falloff_scale, min_falloff) / 100.0;
  const float shape = MAX(data->shape, 0.001);
  const float exp1 = 2.0 / shape;
  const float exp2 = shape / 2.0;
  // Pre-scale the center offset
  const dt_iop_vector_2d_t roi_center_scaled = { roi_center.x * xscale, roi_center.y * yscale };

  float dither = 0.0f;

  switch(data->dithering)
  {
    case DITHER_8BIT:
      dither = 1.0f / 256;
      break;
    case DITHER_16BIT:
      dither = 1.0f / 65536;
      break;
    case DITHER_OFF:
    default:
      dither = 0.0f;
  }

  unsigned int tea_states[2 * dt_get_num_threads()];
  memset(tea_states, 0, 2 * dt_get_num_threads() * sizeof(unsigned int));

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, data, yscale, xscale, tea_states,       \
                                              dither) schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    const size_t k = (size_t)ch * roi_out->width * j;
    const float *in = (const float *)ivoid + k;
    float *out = (float *)ovoid + k;
    unsigned int *tea_state = tea_states + 2 * dt_get_thread_num();
    tea_state[0] = j * roi_out->height + dt_get_thread_num();
    for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
    {
      // current pixel coord translated to local coord
      const dt_iop_vector_2d_t pv
          = { fabsf(i * xscale - roi_center_scaled.x), fabsf(j * yscale - roi_center_scaled.y) };

      // Calculate the pixel weight in vignette
      const float cplen = powf(powf(pv.x, exp1) + powf(pv.y, exp1), exp2); // Length from center to pv
      float weight = 0.0;
      float dith = 0.0;

      if(cplen >= dscale) // pixel is outside the inner vignette circle, lets calculate weight of vignette
      {
        weight = ((cplen - dscale) / fscale);
        if(weight >= 1.0)
          weight = 1.0;
        else if(weight <= 0.0)
          weight = 0.0;
        else
        {
          weight = 0.5 - cosf(M_PI * weight) / 2.0;
          encrypt_tea(tea_state);
          dith = dither * tpdf(tea_state[0]);
        }
      }

      // Let's apply weighted effect on brightness and desaturation
      float col0 = in[0], col1 = in[1], col2 = in[2], col3 = in[3];
      if(weight > 0)
      {
        // Then apply falloff vignette
        float falloff = (data->brightness < 0) ? (1.0 + (weight * data->brightness))
                                               : (weight * data->brightness);
        col0 = data->brightness < 0 ? col0 * falloff + dith : col0 + falloff + dith;
        col1 = data->brightness < 0 ? col1 * falloff + dith : col1 + falloff + dith;
        col2 = data->brightness < 0 ? col2 * falloff + dith : col2 + falloff + dith;

        col0 = unbound ? col0 : CLIP(col0);
        col1 = unbound ? col1 : CLIP(col1);
        col2 = unbound ? col2 : CLIP(col2);

        // apply saturation
        float mv = (col0 + col1 + col2) / 3.0;
        float wss = weight * data->saturation;
        col0 = col0 - ((mv - col0) * wss);
        col1 = col1 - ((mv - col1) * wss);
        col2 = col2 - ((mv - col2) * wss);

        col0 = unbound ? col0 : CLIP(col0);
        col1 = unbound ? col1 : CLIP(col1);
        col2 = unbound ? col2 : CLIP(col2);
      }

      out[0] = col0;
      out[1] = col1;
      out[2] = col2;
      out[3] = col3;
    }
  }
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_vignette_data_t *data = (dt_iop_vignette_data_t *)piece->data;
  dt_iop_vignette_global_data_t *gd = (dt_iop_vignette_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_out->width;
  const int height = roi_out->height;

  const dt_iop_roi_t *buf_in = &piece->buf_in;

  /* Center coordinates of buf_in, these should not consider buf_in->{x,y}! */
  const dt_iop_vector_2d_t buf_center = { buf_in->width * .5f, buf_in->height * .5f };
  /* Center coordinates of vignette center */
  const dt_iop_vector_2d_t vignette_center = { buf_center.x + data->center.x * buf_in->width / 2.0,
                                               buf_center.y + data->center.y * buf_in->height / 2.0 };
  /* Coordinates of vignette_center in terms of roi_in */
  const dt_iop_vector_2d_t roi_center
      = { vignette_center.x * roi_in->scale - roi_in->x, vignette_center.y * roi_in->scale - roi_in->y };
  float xscale;
  float yscale;

  /* w/h ratio follows piece dimensions */
  if(data->autoratio)
  {
    xscale = 2.0 / (buf_in->width * roi_out->scale);
    yscale = 2.0 / (buf_in->height * roi_out->scale);
  }
  else /* specified w/h ratio, scale proportional to longest side */
  {
    const float basis = 2.0 / (MAX(buf_in->height, buf_in->width) * roi_out->scale);
    // w/h ratio from 0-1 use as-is
    if(data->whratio <= 1.0)
    {
      yscale = basis;
      xscale = yscale / data->whratio;
    }
    // w/h ratio from 1-2 interpret as 1-inf
    // that is, the h/w ratio + 1
    else
    {
      xscale = basis;
      yscale = xscale / (2.0 - data->whratio);
    }
  }
  const float dscale = data->scale / 100.0;
  // A minimum falloff is used, based on the image size, to smooth out aliasing artifacts
  const float min_falloff = 100.0 / MIN(buf_in->width, buf_in->height);
  const float fscale = MAX(data->falloff_scale, min_falloff) / 100.0;
  const float shape = MAX(data->shape, 0.001);
  const float exp1 = 2.0 / shape;
  const float exp2 = shape / 2.0;
  // Pre-scale the center offset
  const dt_iop_vector_2d_t roi_center_scaled = { roi_center.x * xscale, roi_center.y * yscale };

  float dither = 0.0f;

  switch(data->dithering)
  {
    case DITHER_8BIT:
      dither = 1.0f / 256;
      break;
    case DITHER_16BIT:
      dither = 1.0f / 65536;
      break;
    case DITHER_OFF:
    default:
      dither = 0.0f;
  }

  float scale[2] = { xscale, yscale };
  float roi_center_scaled_f[2] = { roi_center_scaled.x, roi_center_scaled.y };
  float expt[2] = { exp1, exp2 };
  float brightness = data->brightness;
  float saturation = data->saturation;
  int unbound = data->unbound;

  size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };

  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 4, 2 * sizeof(float), &scale);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 5, 2 * sizeof(float), &roi_center_scaled_f);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 6, 2 * sizeof(float), &expt);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 7, sizeof(float), &dscale);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 8, sizeof(float), &fscale);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 9, sizeof(float), &brightness);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 10, sizeof(float), &saturation);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 11, sizeof(float), &dither);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 12, sizeof(int), &unbound);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_vignette, sizes);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_vignette] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl from programs.conf
  dt_iop_vignette_global_data_t *gd
      = (dt_iop_vignette_global_data_t *)malloc(sizeof(dt_iop_vignette_global_data_t));
  module->data = gd;
  gd->kernel_vignette = dt_opencl_create_kernel(program, "vignette");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_vignette_global_data_t *gd = (dt_iop_vignette_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_vignette);
  free(module->data);
  module->data = NULL;
}


static void scale_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->scale = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void falloff_scale_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->falloff_scale = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void brightness_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->brightness = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->saturation = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void centerx_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->center.x = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void centery_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->center.y = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void shape_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->shape = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void autoratio_callback(GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->autoratio = gtk_toggle_button_get_active(button);
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void whratio_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->whratio = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dithering_callback(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->dithering = (dt_iop_dither_t)dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)p1;
  dt_iop_vignette_data_t *d = (dt_iop_vignette_data_t *)piece->data;
  d->scale = p->scale;
  d->falloff_scale = p->falloff_scale;
  d->brightness = p->brightness;
  d->saturation = p->saturation;
  d->center = p->center;
  d->autoratio = p->autoratio;
  d->whratio = p->whratio;
  d->shape = p->shape;
  d->dithering = p->dithering;
  d->unbound = p->unbound;
}

void init_presets(dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "begin", NULL, NULL, NULL);
  dt_iop_vignette_params_t p;
  p.scale = 40.0f;
  p.falloff_scale = 100.0f;
  p.brightness = -1.0f;
  p.saturation = 0.5f;
  p.center.x = 0.0f;
  p.center.y = 0.0f;
  p.autoratio = FALSE;
  p.whratio = 1.0f;
  p.shape = 1.0f;
  p.dithering = 0;
  p.unbound = TRUE;
  dt_gui_presets_add_generic(_("lomo"), self->op, self->version(), &p, sizeof(p), 1);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "commit", NULL, NULL, NULL);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_vignette_data_t));
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
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)module->params;
  dt_bauhaus_slider_set(g->scale, p->scale);
  dt_bauhaus_slider_set(g->falloff_scale, p->falloff_scale);
  dt_bauhaus_slider_set(g->brightness, p->brightness);
  dt_bauhaus_slider_set(g->saturation, p->saturation);
  dt_bauhaus_slider_set(g->center_x, p->center.x);
  dt_bauhaus_slider_set(g->center_y, p->center.y);
  gtk_toggle_button_set_active(g->autoratio, p->autoratio);
  dt_bauhaus_slider_set(g->whratio, p->whratio);
  dt_bauhaus_slider_set(g->shape, p->shape);
  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);
  dt_bauhaus_combobox_set(g->dithering, p->dithering);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_vignette_params_t));
  module->default_params = malloc(sizeof(dt_iop_vignette_params_t));
  module->default_enabled = 0;
  module->priority = 883; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_vignette_params_t);
  module->gui_data = NULL;
  dt_iop_vignette_params_t tmp
      = (dt_iop_vignette_params_t){ 80, 50, -0.5, -0.5, { 0, 0 }, FALSE, 1.0, 1.0, DITHER_OFF, TRUE };
  memcpy(module->params, &tmp, sizeof(dt_iop_vignette_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_vignette_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_vignette_gui_data_t));
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  GtkWidget *hbox, *label1;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  label1 = dtgtk_reset_label_new(_("automatic ratio"), self, &p->autoratio, sizeof p->autoratio);

  g->scale = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 0.5, p->scale, 2);
  g->falloff_scale = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1.0, p->falloff_scale, 2);
  g->brightness = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.01, p->brightness, 3);
  g->saturation = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.01, p->saturation, 3);
  g->center_x = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.01, p->center.x, 3);
  g->center_y = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.01, p->center.y, 3);
  g->shape = dt_bauhaus_slider_new_with_range(self, 0.0, 5.0, 0.1, p->shape, 2);
  g->whratio = dt_bauhaus_slider_new_with_range(self, 0.0, 2.0, 0.01, p->shape, 3);
  g->autoratio = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(_("automatic")));
  g->dithering = dt_bauhaus_combobox_new(self);

  dt_bauhaus_combobox_add(g->dithering, _("off"));
  dt_bauhaus_combobox_add(g->dithering, _("8-bit output"));
  dt_bauhaus_combobox_add(g->dithering, _("16-bit output"));

  dt_bauhaus_slider_set_format(g->scale, "%.02f%%");
  dt_bauhaus_slider_set_format(g->falloff_scale, "%.02f%%");
  dt_bauhaus_widget_set_label(g->scale, NULL, _("scale"));
  dt_bauhaus_widget_set_label(g->falloff_scale, NULL, _("fall-off strength"));
  dt_bauhaus_widget_set_label(g->brightness, NULL, _("brightness"));
  dt_bauhaus_widget_set_label(g->saturation, NULL, _("saturation"));
  dt_bauhaus_widget_set_label(g->center_x, NULL, _("horizontal center"));
  dt_bauhaus_widget_set_label(g->center_y, NULL, _("vertical center"));
  dt_bauhaus_widget_set_label(g->shape, NULL, _("shape"));
  dt_bauhaus_widget_set_label(g->whratio, NULL, _("width/height ratio"));
  dt_bauhaus_widget_set_label(g->dithering, NULL, _("dithering"));

  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);

  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->autoratio), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), g->scale, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->falloff_scale, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->brightness, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->saturation, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->center_x, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->center_y, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->shape, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->whratio, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->dithering, TRUE, TRUE, 0);

  g_object_set(G_OBJECT(g->scale), "tooltip-text", _("the radii scale of vignette for start of fall-off"),
               (char *)NULL);
  g_object_set(G_OBJECT(g->falloff_scale), "tooltip-text",
               _("the radii scale of vignette for end of fall-off"), (char *)NULL);
  g_object_set(G_OBJECT(g->brightness), "tooltip-text", _("strength of effect on brightness"), (char *)NULL);
  g_object_set(G_OBJECT(g->saturation), "tooltip-text", _("strength of effect on saturation"), (char *)NULL);
  g_object_set(G_OBJECT(g->center_x), "tooltip-text", _("horizontal offset of center of the effect"),
               (char *)NULL);
  g_object_set(G_OBJECT(g->center_y), "tooltip-text", _("vertical offset of center of the effect"),
               (char *)NULL);
  g_object_set(
      G_OBJECT(g->shape), "tooltip-text",
      _("shape factor\n0 produces a rectangle\n1 produces a circle or ellipse\n2 produces a diamond"),
      (char *)NULL);
  g_object_set(G_OBJECT(g->autoratio), "tooltip-text",
               _("enable to have the ratio automatically follow the image size"), (char *)NULL);
  g_object_set(G_OBJECT(g->whratio), "tooltip-text", _("width-to-height ratio"), (char *)NULL);
  g_object_set(G_OBJECT(g->dithering), "tooltip-text", _("add some level of random noise to prevent banding"),
               (char *)NULL);

  g_signal_connect(G_OBJECT(g->scale), "value-changed", G_CALLBACK(scale_callback), self);
  g_signal_connect(G_OBJECT(g->falloff_scale), "value-changed", G_CALLBACK(falloff_scale_callback), self);
  g_signal_connect(G_OBJECT(g->brightness), "value-changed", G_CALLBACK(brightness_callback), self);
  g_signal_connect(G_OBJECT(g->saturation), "value-changed", G_CALLBACK(saturation_callback), self);
  g_signal_connect(G_OBJECT(g->center_x), "value-changed", G_CALLBACK(centerx_callback), self);
  g_signal_connect(G_OBJECT(g->center_y), "value-changed", G_CALLBACK(centery_callback), self);
  g_signal_connect(G_OBJECT(g->shape), "value-changed", G_CALLBACK(shape_callback), self);
  g_signal_connect(G_OBJECT(g->autoratio), "toggled", G_CALLBACK(autoratio_callback), self);
  g_signal_connect(G_OBJECT(g->whratio), "value-changed", G_CALLBACK(whratio_callback), self);
  g_signal_connect(G_OBJECT(g->dithering), "value-changed", G_CALLBACK(dithering_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
