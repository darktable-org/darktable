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
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE(2)

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)

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
  double scale;              // 0 - 100 Radie
  double falloff_scale;   // 0 - 100 Radie for falloff inner radie of falloff=scale and outer=scale+falloff_scale
  double strength;         // 0 - 1 strength of effect
  double uniformity;       // 0 - 1 uniformity of center
  double bsratio;            // -1 - +1 ratio of brightness/saturation effect
  gboolean invert_falloff;
  gboolean invert_saturation;
  dt_iop_dvector_2d_t center;            // Center of vignette
}
dt_iop_vignette_params1_t;

typedef struct dt_iop_vignette_params_t
{
  float scale;			// 0 - 100 Inner radius, percent of largest image dimension
  float falloff_scale;		// 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;		// -1 - 1 Strength of brightness reduction
  float saturation;		// -1 - 1 Strength of saturation reduction
  dt_iop_vector_2d_t center;	// Center of vignette
  gboolean autoratio;		//
  float whratio;		// 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;
}
dt_iop_vignette_params_t;

typedef struct dt_iop_vignette_gui_data_t
{
  GtkDarktableSlider *scale;
  GtkDarktableSlider *falloff_scale;
  GtkDarktableSlider *brightness;
  GtkDarktableSlider *saturation;
  GtkDarktableSlider *center_x;
  GtkDarktableSlider *center_y;
  GtkToggleButton *autoratio;
  GtkDarktableSlider *whratio;
  GtkDarktableSlider *shape;
}
dt_iop_vignette_gui_data_t;

typedef struct dt_iop_vignette_data_t
{
  float scale;
  float falloff_scale;
  float brightness;
  float saturation;
  dt_iop_vector_2d_t center;	// Center of vignette
  gboolean autoratio;
  float whratio;
  float shape;
}
dt_iop_vignette_data_t;

const char *name()
{
  return _("vignetting");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int
groups ()
{
  return IOP_GROUP_EFFECT;
}

int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  if (old_version == 1 && new_version == 2)
  {
    const dt_iop_vignette_params1_t *old = old_params;
    dt_iop_vignette_params_t *new = new_params;
    new->scale = old->scale;
    new->falloff_scale = old->falloff_scale;
    new->brightness= -(1.0-MAX(old->bsratio,0.0))*old->strength/100.0;
    new->saturation= -(1.0+MIN(old->bsratio,0.0))*old->strength/100.0;
    if (old->invert_saturation)
      new->saturation *= -2.0;	// Double effect for increasing saturation
    if (old->invert_falloff)
      new->brightness = -new->brightness;
    new->center.x= old->center.x;
    new->center.y= old->center.y;
    new->autoratio= TRUE;
    new->whratio= 1.0;
    new->shape= 1.0;
    return 0;
  }
  return 1;
}

static int
get_grab(float pointerx, float pointery, float startx, float starty, float endx, float endy, float zoom_scale)
{
  const float radius = 5.0/zoom_scale;

  if(powf(pointerx-startx, 2)+powf(pointery, 2) <= powf(radius, 2)) return 2;    // x size
  if(powf(pointerx, 2)+powf(pointery-starty, 2) <= powf(radius, 2)) return 4;    // y size
  if(powf(pointerx, 2)+powf(pointery, 2) <= powf(radius, 2)) return 1;           // center
  if(powf(pointerx-endx, 2)+powf(pointery, 2) <= powf(radius, 2)) return 8;      // x falloff
  if(powf(pointerx, 2)+powf(pointery-endy, 2) <= powf(radius, 2)) return 16;     // y falloff

  return 0;
}

static void
draw_overlay(cairo_t *cr, float x, float y, float fx, float fy, int grab, float zoom_scale)
{
  // half width/height of the crosshair
  float crosshair_w = 10.0/zoom_scale;
  float crosshair_h = 10.0/zoom_scale;

  // center crosshair
  cairo_move_to(cr, -crosshair_w, 0.0);
  cairo_line_to(cr,  crosshair_w, 0.0);
  cairo_move_to(cr, 0.0, -crosshair_h);
  cairo_line_to(cr, 0.0,  crosshair_h);
  cairo_stroke(cr);

  // inner border of the vignette
  cairo_save(cr);
  if(x <= y)
  {
    cairo_scale(cr, x/y, 1.0);
    cairo_arc(cr, 0.0, 0.0, y, 0.0, M_PI*2.0);
  }
  else
  {
    cairo_scale(cr, 1.0, y/x);
    cairo_arc(cr, 0.0, 0.0, x, 0.0, M_PI*2.0);
  }
  cairo_restore(cr);
  cairo_stroke(cr);

  // outer border of the vignette
  cairo_save(cr);
  if(fx <= fy)
  {
    cairo_scale(cr, fx/fy, 1.0);
    cairo_arc(cr, 0.0, 0.0, fy, 0.0, M_PI*2.0);
  }
  else
  {
    cairo_scale(cr, 1.0, fy/fx);
    cairo_arc(cr, 0.0, 0.0, fx, 0.0, M_PI*2.0);
  }
  cairo_restore(cr);
  cairo_stroke(cr);

  // the handles
  const float radius_sel = 6.0/zoom_scale;
  const float radius_reg = 4.0/zoom_scale;
  if(grab ==  1) cairo_arc(cr, 0.0, 0.0, radius_sel, 0.0, M_PI*2.0);
  else           cairo_arc(cr, 0.0, 0.0, radius_reg, 0.0, M_PI*2.0);
  cairo_stroke(cr);
  if(grab ==  2) cairo_arc(cr, x, 0.0, radius_sel, 0.0, M_PI*2.0);
  else           cairo_arc(cr, x, 0.0, radius_reg, 0.0, M_PI*2.0);
  cairo_stroke(cr);
  if(grab ==  4) cairo_arc(cr, 0.0, -y, radius_sel, 0.0, M_PI*2.0);
  else           cairo_arc(cr, 0.0, -y, radius_reg, 0.0, M_PI*2.0);
  cairo_stroke(cr);
  if(grab ==  8) cairo_arc(cr, fx, 0.0, radius_sel, 0.0, M_PI*2.0);
  else           cairo_arc(cr, fx, 0.0, radius_reg, 0.0, M_PI*2.0);
  cairo_stroke(cr);
  if(grab == 16) cairo_arc(cr, 0.0, -fy, radius_sel, 0.0, M_PI*2.0);
  else           cairo_arc(cr, 0.0, -fy, radius_reg, 0.0, M_PI*2.0);
  cairo_stroke(cr);

}

//FIXME: For portrait images the overlay is a bit off. The coordinates in mouse_moved seem to be ok though. WTF?
void
gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev             = self->dev;
//   dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p   = (dt_iop_vignette_params_t *)self->params;

  int32_t zoom, closeup;
  float zoom_x, zoom_y;
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
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  cairo_translate(cr, width/2.0, height/2.0);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f*wd-zoom_x*wd, -.5f*ht-zoom_y*ht);

  float vignette_x = (p->center.x+1.0)*0.5*wd;
  float vignette_y = (p->center.y+1.0)*0.5*ht;

  cairo_translate(cr, vignette_x, vignette_y);

  float vignette_w = p->scale*0.01*0.5*wd; // start of falloff
  float vignette_h = p->scale*0.01*0.5*ht;
  float vignette_fx = vignette_w + p->falloff_scale*0.01*0.5*wd; // end of falloff
  float vignette_fy = vignette_h + p->falloff_scale*0.01*0.5*ht;

  if(p->autoratio == FALSE)
  {
    float factor1 = bigger_side/smaller_side;
    if(wd >= ht)
    {
      float factor2 = (2.0-p->whratio)*factor1;

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
      float factor2 = (p->whratio)*factor1;

      if(p->whratio <= 1)
      {
        vignette_w *= factor2;
        vignette_fx *= factor2;
      }
      else
      {
        vignette_w *= factor1;
        vignette_h *= (2.0-p->whratio);
        vignette_fx *= factor1;
        vignette_fy *= (2.0-p->whratio);
      }
    }
  }

  int grab = get_grab(pzx*wd-vignette_x, pzy*ht-vignette_y, vignette_w, -vignette_h, vignette_fx, -vignette_fy, zoom_scale);
  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
  cairo_set_line_width(cr, 3.0/zoom_scale);
  cairo_set_source_rgba(cr, .3, .3, .3, .8);
  draw_overlay(cr, vignette_w, vignette_h, vignette_fx, vignette_fy, grab, zoom_scale);
  cairo_set_line_width(cr, 1.0/zoom_scale);
  cairo_set_source_rgba(cr, .8, .8, .8, .8);
  draw_overlay(cr, vignette_w, vignette_h, vignette_fx, vignette_fy, grab, zoom_scale);

}

//FIXME: Pumping of the opposite direction when changing width/height. See two FIXMEs further down.
int
mouse_moved(struct dt_iop_module_t *self, double x, double y, int which)
{
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p   = (dt_iop_vignette_params_t *)self->params;
  int32_t zoom, closeup;
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
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  float zoom_scale = dt_dev_get_zoom_scale(self->dev, zoom, closeup ? 2 : 1, 1);
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  static int old_grab = -1;
  int grab = old_grab;

  float vignette_x = (p->center.x+1.0)*0.5*wd;
  float vignette_y = (p->center.y+1.0)*0.5*ht;

  float vignette_w = p->scale*0.01*0.5*wd; // start of falloff
  float vignette_h = p->scale*0.01*0.5*ht;
  float vignette_fx = vignette_w + p->falloff_scale*0.01*0.5*wd; // end of falloff
  float vignette_fy = vignette_h + p->falloff_scale*0.01*0.5*ht;

  if(p->autoratio == FALSE)
  {
    float factor1 = bigger_side/smaller_side;
    if(wd >= ht)
    {
      float factor2 = (2.0-p->whratio)*factor1;

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
      float factor2 = (p->whratio)*factor1;

      if(p->whratio <= 1)
      {
        vignette_w *= factor2;
        vignette_fx *= factor2;
      }
      else
      {
        vignette_w *= factor1;
        vignette_h *= (2.0-p->whratio);
        vignette_fx *= factor1;
        vignette_fy *= (2.0-p->whratio);
      }
    }
  }

  if(grab == 0 || !(darktable.control->button_down && darktable.control->button_down_which == 1))
  {
    grab = get_grab(pzx*wd-vignette_x, pzy*ht-vignette_y, vignette_w, -vignette_h, vignette_fx, -vignette_fy, zoom_scale);
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
      dtgtk_slider_set_value(g->center_x, pzx*2.0 - 1.0);
      dtgtk_slider_set_value(g->center_y, pzy*2.0 - 1.0);
    }
    else if(grab ==  2) // change the width
    {
      float max = 0.5*((p->whratio <= 1.0)?bigger_side*p->whratio:bigger_side);
      float new_vignette_w = MIN(bigger_side*0.5, MAX(0.1, pzx*wd - vignette_x));
      float ratio = new_vignette_w/vignette_h;
      float new_scale = 100.0 * new_vignette_w / max;
      // FIXME: When going over the 1.0 boundary from wide to narrow (>1.0 -> <=1.0) the height slightly changes, depending on speed.
      //        I guess we have to split the computation.
      if(ratio <= 1.0)
      {
        if(which == GDK_CONTROL_MASK)
        {
          dtgtk_slider_set_value(g->scale, new_scale);
        }
        else
        {
          dtgtk_slider_set_value(g->whratio, ratio);
        }
      }
      else
      {
        dtgtk_slider_set_value(g->scale, new_scale);

        if(which != GDK_CONTROL_MASK)
        {
          float new_whratio = 2.0 - 1.0 / ratio;
          dtgtk_slider_set_value(g->whratio, new_whratio);
        }
      }
    }
    else if(grab ==  4) // change the height
    {
      float new_vignette_h = MIN(bigger_side*0.5, MAX(0.1, vignette_y - pzy*ht));
      float ratio = new_vignette_h/vignette_w;
      float max = 0.5*((ratio <= 1.0)?bigger_side*(2.0-p->whratio):bigger_side);
      // FIXME: When going over the 1.0 boundary from narrow to wide (>1.0 -> <=1.0) the width slightly changes, depending on speed.
      //        I guess we have to split the computation.
      if(ratio <= 1.0)
      {
        if(which == GDK_CONTROL_MASK)
        {
          float new_scale = 100.0 * new_vignette_h / max;
          dtgtk_slider_set_value(g->scale, new_scale);
        }
        else
        {
          dtgtk_slider_set_value(g->whratio, 2.0-ratio);
        }
      }
      else
      {
        float new_scale = 100.0 * new_vignette_h / max;
        dtgtk_slider_set_value(g->scale, new_scale);

        if(which != GDK_CONTROL_MASK)
        {
          float new_whratio = 1.0 / ratio;
          dtgtk_slider_set_value(g->whratio, new_whratio);
        }
      }
    }
    else if(grab ==  8) // change the falloff on the right
    {
      float new_vignette_fx = pzx*wd - vignette_x;
      float max = 0.5*((p->whratio <= 1.0)?bigger_side*p->whratio:bigger_side);
      float delta_x = MIN(max, MAX(0.0, new_vignette_fx - vignette_w));
      float new_falloff = 100.0 * delta_x / max;
      dtgtk_slider_set_value(g->falloff_scale, new_falloff);
    }
    else if(grab == 16) // change the falloff on the top
    {
      float new_vignette_fy = vignette_y - pzy*ht;
      float max = 0.5*((p->whratio > 1.0)?bigger_side*(2.0-p->whratio):bigger_side);
      float delta_y = MIN(max, MAX(0.0, new_vignette_fy - vignette_h));
      float new_falloff = 100.0 * delta_y / max;
      dtgtk_slider_set_value(g->falloff_scale, new_falloff);
    }
    dt_control_gui_queue_draw();
    return 1;

  }
  else if(grab)
  {
    if     (grab ==  1) dt_control_change_cursor(GDK_FLEUR);
    else if(grab ==  2) dt_control_change_cursor(GDK_SB_H_DOUBLE_ARROW);
    else if(grab ==  4) dt_control_change_cursor(GDK_SB_V_DOUBLE_ARROW);
    else if(grab ==  8) dt_control_change_cursor(GDK_SB_H_DOUBLE_ARROW);
    else if(grab == 16) dt_control_change_cursor(GDK_SB_V_DOUBLE_ARROW);
  }
  else
  {
    if(old_grab != grab) dt_control_change_cursor(GDK_LEFT_PTR);
  }
  old_grab = grab;
  dt_control_gui_queue_draw();
  return 0;
}

int
button_pressed(struct dt_iop_module_t *self, double x, double y, int which, int type, uint32_t state)
{
  if(which == 1)
    return 1;
  return 0;
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  if(which == 1)
    return 1;
  return 0;
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const dt_iop_vignette_data_t *data = (dt_iop_vignette_data_t *)piece->data;
  const dt_iop_roi_t *buf_in = &piece->buf_in;
  const int ch = piece->colors;

  /* Center coordinates of buf_in, these should not consider buf_in->{x,y}! */
  const dt_iop_vector_2d_t buf_center =
  {
    buf_in->width * .5f,
    buf_in->height * .5f
  };
  /* Center coordinates of vignette center */
  const dt_iop_vector_2d_t vignette_center =
  {
    buf_center.x + data->center.x * buf_in->width / 2.0,
    buf_center.y + data->center.y * buf_in->height / 2.0
  };
  /* Coordinates of vignette_center in terms of roi_in */
  const dt_iop_vector_2d_t roi_center =
  {
    vignette_center.x * roi_in->scale - roi_in->x,
    vignette_center.y * roi_in->scale - roi_in->y
  };
  float xscale;
  float yscale;

  /* w/h ratio follows piece dimensions */
  if (data->autoratio)
  {
    xscale=2.0/(buf_in->width*roi_out->scale);
    yscale=2.0/(buf_in->height*roi_out->scale);
  }
  else				/* specified w/h ratio, scale proportional to longest side */
  {
    const float basis = 2.0 / (MAX(buf_in->height, buf_in->width) * roi_out->scale);
    // w/h ratio from 0-1 use as-is
    if (data->whratio <= 1.0)
    {
      yscale=basis;
      xscale=yscale/data->whratio;
    }
    // w/h ratio from 1-2 interpret as 1-inf
    // that is, the h/w ratio + 1
    else
    {
      xscale=basis;
      yscale=xscale/(2.0-data->whratio);
    }
  }
  const float dscale=data->scale/100.0;
  // A minimum falloff is used, based on the image size, to smooth out aliasing artifacts
  const float min_falloff=100.0/MIN(buf_in->width, buf_in->height);
  const float fscale=MAX(data->falloff_scale,min_falloff)/100.0;
  const float shape=MAX(data->shape,0.001);
  const float exp1=2.0/shape;
  const float exp2=shape/2.0;
  // Pre-scale the center offset
  const dt_iop_vector_2d_t roi_center_scaled =
  {
    roi_center.x * xscale,
    roi_center.y * yscale
  };

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, data, yscale, xscale) schedule(static)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    const int k = ch*roi_out->width*j;
    const float *in = (const float *)ivoid + k;
    float *out = (float *)ovoid + k;
    for(int i=0; i<roi_out->width; i++, in+=ch, out+=ch)
    {
      // current pixel coord translated to local coord
      const dt_iop_vector_2d_t pv =
      {
        fabsf(i*xscale-roi_center_scaled.x),
        fabsf(j*yscale-roi_center_scaled.y)
      };

      // Calculate the pixel weight in vignette
      const float cplen=powf(powf(pv.x,exp1)+powf(pv.y,exp1),exp2);  // Length from center to pv
      float weight=0.0;

      if( cplen>=dscale ) // pixel is outside the inner vingette circle, lets calculate weight of vignette
      {
        weight=((cplen-dscale)/fscale);
        if (weight >= 1.0)
          weight = 1.0;
        else if (weight <= 0.0)
          weight = 0.0;
        else
          weight=0.5 - cosf( M_PI*weight )/2.0;
      }

      // Let's apply weighted effect on brightness and desaturation
      float col0=in[0], col1=in[1], col2=in[2];
      if( weight > 0 )
      {
        // Then apply falloff vignette
        float falloff=(data->brightness<=0)?(1.0+(weight*data->brightness)):(weight*data->brightness);
        col0=CLIP( ((data->brightness<0)? col0*falloff: col0+falloff) );
        col1=CLIP( ((data->brightness<0)? col1*falloff: col1+falloff) );
        col2=CLIP( ((data->brightness<0)? col2*falloff: col2+falloff) );

        // apply saturation
        float mv=(col0+col1+col2)/3.0;
        float wss=weight*data->saturation;
        col0=CLIP( col0-((mv-col0)* wss) );
        col1=CLIP( col1-((mv-col1)* wss) );
        col2=CLIP( col2-((mv-col2)* wss) );
      }
      out[0]=col0;
      out[1]=col1;
      out[2]=col2;
    }
  }
}

static void
scale_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->scale= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
falloff_scale_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->falloff_scale= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
brightness_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->brightness= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
saturation_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->saturation = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
centerx_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->center.x = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
centery_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->center.y = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
shape_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->shape = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
autoratio_callback (GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->autoratio = gtk_toggle_button_get_active(button);
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
whratio_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->whratio = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[vignette] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_vignette_data_t *d = (dt_iop_vignette_data_t *)piece->data;
  d->scale = p->scale;
  d->falloff_scale = p->falloff_scale;
  d->brightness= p->brightness;
  d->saturation= p->saturation;
  d->center=p->center;
  d->autoratio=p->autoratio;
  d->whratio=p->whratio;
  d->shape=p->shape;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_vignette_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
  // no free necessary, no data is alloc'ed
#else
  free(piece->data);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)module->params;
  dtgtk_slider_set_value(g->scale, p->scale);
  dtgtk_slider_set_value(g->falloff_scale, p->falloff_scale);
  dtgtk_slider_set_value(g->brightness, p->brightness);
  dtgtk_slider_set_value(g->saturation, p->saturation);
  dtgtk_slider_set_value(g->center_x, p->center.x);
  dtgtk_slider_set_value(g->center_y, p->center.y);
  gtk_toggle_button_set_active(g->autoratio, p->autoratio);
  dtgtk_slider_set_value(g->whratio, p->whratio);
  dtgtk_slider_set_value(g->shape, p->shape);
  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_vignette_params_t));
  module->default_params = malloc(sizeof(dt_iop_vignette_params_t));
  module->default_enabled = 0;
  module->priority = 888; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_vignette_params_t);
  module->gui_data = NULL;
  dt_iop_vignette_params_t tmp = (dt_iop_vignette_params_t)
  {
    80,50,-0.5,-0.5, {0,0}, FALSE, 1.0, 1.0
  };
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
  GtkWidget *vbox, *hbox, *label1;

  self->widget = gtk_hbox_new(FALSE, 0);
  vbox = gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(vbox), TRUE, TRUE, 5);

  label1 = dtgtk_reset_label_new (_("automatic ratio"), self, &p->autoratio, sizeof p->autoratio);

  g->scale = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.5, p->scale, 2));
  g->falloff_scale = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 1.0, p->falloff_scale, 2));
  g->brightness = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-1.0, 1.0, 0.01, p->brightness, 3));
  g->saturation = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-1.0, 1.0, 0.01, p->saturation, 3));
  g->center_x = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-1.0, 1.0, 0.01, p->center.x, 3));
  g->center_y = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-1.0, 1.0, 0.01, p->center.y, 3));
  g->shape = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 5.0, 0.1, p->shape, 2));
  g->autoratio = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(_("automatic")));
  g->whratio = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 2.0, 0.01, p->shape, 3));

  dtgtk_slider_set_label(g->scale,_("scale"));
  dtgtk_slider_set_unit(g->scale,"%");
  dtgtk_slider_set_label(g->falloff_scale,_("fall-off strength"));
  dtgtk_slider_set_unit(g->falloff_scale,"%");
  dtgtk_slider_set_label(g->brightness,_("brightness"));
  dtgtk_slider_set_label(g->saturation,_("saturation"));
  dtgtk_slider_set_label(g->center_x,_("horizontal center"));
  dtgtk_slider_set_label(g->center_y,_("vertical center"));
  dtgtk_slider_set_label(g->shape,_("shape"));
  dtgtk_slider_set_label(g->whratio,_("width/height ratio"));

  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);

  hbox= gtk_hbox_new(FALSE,0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->autoratio), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->scale), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->falloff_scale), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->brightness), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->saturation), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->center_x), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->center_y), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->shape), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->whratio), TRUE, TRUE, 0);

  g_object_set(G_OBJECT(g->scale), "tooltip-text", _("the radii scale of vignette for start of fall-off"), (char *)NULL);
  g_object_set(G_OBJECT(g->falloff_scale), "tooltip-text", _("the radii scale of vignette for end of fall-off"), (char *)NULL);
  g_object_set(G_OBJECT(g->brightness), "tooltip-text", _("strength of effect on brightness"), (char *)NULL);
  g_object_set(G_OBJECT(g->saturation), "tooltip-text", _("strength of effect on saturation"), (char *)NULL);
  g_object_set(G_OBJECT(g->center_x), "tooltip-text", _("horizontal offset of center of the effect"), (char *)NULL);
  g_object_set(G_OBJECT(g->center_y), "tooltip-text", _("vertical offset of center of the effect"), (char *)NULL);
  g_object_set(G_OBJECT(g->shape), "tooltip-text", _("shape factor\n0 produces a rectangle\n1 produces a circle or elipse\n2 produces a diamond"), (char *)NULL);
  g_object_set(G_OBJECT(g->autoratio), "tooltip-text", _("enable to have the ratio automatically follow the image size"), (char *)NULL);
  g_object_set(G_OBJECT(g->whratio), "tooltip-text", _("width-to-height ratio"), (char *)NULL);

  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->scale),DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->falloff_scale),DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->center_x),DARKTABLE_SLIDER_FORMAT_RATIO);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->center_y),DARKTABLE_SLIDER_FORMAT_RATIO);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->whratio),DARKTABLE_SLIDER_FORMAT_RATIO);

  g_signal_connect (G_OBJECT (g->scale), "value-changed",
                    G_CALLBACK (scale_callback), self);
  g_signal_connect (G_OBJECT (g->falloff_scale), "value-changed",
                    G_CALLBACK (falloff_scale_callback), self);
  g_signal_connect (G_OBJECT (g->brightness), "value-changed",
                    G_CALLBACK (brightness_callback), self);
  g_signal_connect (G_OBJECT (g->saturation), "value-changed",
                    G_CALLBACK (saturation_callback), self);
  g_signal_connect (G_OBJECT (g->center_x), "value-changed",
                    G_CALLBACK (centerx_callback), self);
  g_signal_connect (G_OBJECT (g->center_y), "value-changed",
                    G_CALLBACK (centery_callback), self);
  g_signal_connect (G_OBJECT (g->shape), "value-changed",
                    G_CALLBACK (shape_callback), self);
  g_signal_connect (G_OBJECT (g->autoratio), "toggled",
                    G_CALLBACK (autoratio_callback), self);
  g_signal_connect (G_OBJECT (g->whratio), "value-changed",
                    G_CALLBACK (whratio_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
