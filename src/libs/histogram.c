/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#include <stdint.h>

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/math.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef enum dt_lib_histogram_highlight_t
{
  DT_LIB_HISTOGRAM_HIGHLIGHT_NONE = 0,
  DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT,
  DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE,
  DT_LIB_HISTOGRAM_HIGHLIGHT_TYPE,
  DT_LIB_HISTOGRAM_HIGHLIGHT_MODE,
  DT_LIB_HISTOGRAM_HIGHLIGHT_RED,
  DT_LIB_HISTOGRAM_HIGHLIGHT_GREEN,
  DT_LIB_HISTOGRAM_HIGHLIGHT_BLUE,
} dt_lib_histogram_highlight_t;

typedef enum dt_lib_histogram_waveform_type_t
{
  DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID = 0,
  DT_LIB_HISTOGRAM_WAVEFORM_PARADE,
  DT_LIB_HISTOGRAM_WAVEFORM_N // needs to be the last one
} dt_lib_histogram_waveform_type_t;

const gchar *dt_lib_histogram_histogram_type_names[DT_DEV_HISTOGRAM_N] = { "logarithmic", "linear" };
const gchar *dt_lib_histogram_waveform_type_names[DT_LIB_HISTOGRAM_WAVEFORM_N] = { "overlaid", "parade" };

typedef struct dt_lib_histogram_t
{
  float exposure, black;
  int32_t dragging;
  int32_t button_down_x, button_down_y;
  dt_lib_histogram_highlight_t highlight;
  dt_lib_histogram_waveform_type_t waveform_type;
  gboolean red, green, blue;
  float type_x, mode_x, red_x, green_x, blue_x;
  float button_w, button_h, button_y, button_spacing;
} dt_lib_histogram_t;

const char *name(dt_lib_module_t *self)
{
  return _("histogram");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", /*"tethering",*/ NULL};
  /* histogram disabled on tethering mode for 3.2 release as not working,
     see #4298 for discussion and resolution of this issue. */
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}


static void _lib_histogram_change_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_control_queue_redraw_widget(self->widget);
}

static void _draw_color_toggle(cairo_t *cr, float x, float y, float width, float height, gboolean state)
{
  const float border = MIN(width * .05, height * .05);
  cairo_rectangle(cr, x + border, y + border, width - 2.0 * border, height - 2.0 * border);
  cairo_fill_preserve(cr);
  if(state)
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  else
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_set_line_width(cr, border);
  cairo_stroke(cr);
}

static void _draw_type_toggle(cairo_t *cr, float x, float y, float width, float height, int type)
{
  cairo_save(cr);
  cairo_translate(cr, x, y);

  // border
  const float border = MIN(width * .05, height * .05);
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_rectangle(cr, border, border, width - 2.0 * border, height - 2.0 * border);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_set_line_width(cr, border);
  cairo_stroke(cr);

  // icon
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  cairo_move_to(cr, 2.0 * border, height - 2.0 * border);
  switch(type)
  {
    case DT_DEV_SCOPE_HISTOGRAM:
      cairo_curve_to(cr, 0.3 * width, height - 2.0 * border, 0.3 * width, 2.0 * border,
                     0.5 * width, 2.0 * border);
      cairo_curve_to(cr, 0.7 * width, 2.0 * border, 0.7 * width, height - 2.0 * border,
                     width - 2.0 * border, height - 2.0 * border);
      cairo_fill(cr);
      break;
    case DT_DEV_SCOPE_WAVEFORM:
    {
      cairo_pattern_t *pattern;
      pattern = cairo_pattern_create_linear(0.0, 1.5 * border, 0.0, height - 3.0 * border);

      cairo_pattern_add_color_stop_rgba(pattern, 0.0, 0.0, 0.0, 0.0, 0.5);
      cairo_pattern_add_color_stop_rgba(pattern, 0.2, 0.2, 0.2, 0.2, 0.5);
      cairo_pattern_add_color_stop_rgba(pattern, 0.5, 1.0, 1.0, 1.0, 0.5);
      cairo_pattern_add_color_stop_rgba(pattern, 0.6, 1.0, 1.0, 1.0, 0.5);
      cairo_pattern_add_color_stop_rgba(pattern, 1.0, 0.2, 0.2, 0.2, 0.5);

      cairo_rectangle(cr, 1.5 * border, 1.5 * border, (width - 3.0 * border) * 0.3, height - 3.0 * border);
      cairo_set_source(cr, pattern);
      cairo_fill(cr);

      cairo_save(cr);
      cairo_scale(cr, 1, -1);
      cairo_translate(cr, 0, -height);
      cairo_rectangle(cr, 1.5 * border + (width - 3.0 * border) * 0.2, 1.5 * border,
                      (width - 3.0 * border) * 0.6, height - 3.0 * border);
      cairo_set_source(cr, pattern);
      cairo_fill(cr);
      cairo_restore(cr);

      cairo_rectangle(cr, 1.5 * border + (width - 3.0 * border) * 0.7, 1.5 * border,
                      (width - 3.0 * border) * 0.3, height - 3.0 * border);
      cairo_set_source(cr, pattern);
      cairo_fill(cr);

      cairo_pattern_destroy(pattern);
      break;
    }
  }
  cairo_restore(cr);
}

static void _draw_histogram_mode_toggle(cairo_t *cr, float x, float y, float width, float height, int mode)
{
  cairo_save(cr);
  cairo_translate(cr, x, y);

  // border
  const float border = MIN(width * .05, height * .05);
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_rectangle(cr, border, border, width - 2.0 * border, height - 2.0 * border);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_set_line_width(cr, border);
  cairo_stroke(cr);

  // icon
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  cairo_move_to(cr, 2.0 * border, height - 2.0 * border);
  switch(mode)
  {
    case DT_DEV_HISTOGRAM_LINEAR:
      cairo_line_to(cr, width - 2.0 * border, 2.0 * border);
      cairo_stroke(cr);
      break;
    case DT_DEV_HISTOGRAM_LOGARITHMIC:
      cairo_curve_to(cr, 2.0 * border, 0.33 * height, 0.66 * width, 2.0 * border, width - 2.0 * border,
                     2.0 * border);
      cairo_stroke(cr);
      break;
  }
  cairo_restore(cr);
}

static void _draw_waveform_mode_toggle(cairo_t *cr, float x, float y, float width, float height, int mode)
{
  cairo_save(cr);
  cairo_translate(cr, x, y);

  // border
  const float border = MIN(width * .05, height * .05);
  switch(mode)
  {
    case DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID:
    {
      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.33);
      cairo_rectangle(cr, border, border, width - 2.0 * border, height - 2.0 * border);
      cairo_fill_preserve(cr);
      break;
    }
    case DT_LIB_HISTOGRAM_WAVEFORM_PARADE:
    {
      cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.33);
      cairo_rectangle(cr, border, border, width / 3.0, height - 2.0 * border);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 0.33);
      cairo_rectangle(cr, width / 3.0, border, width / 3.0, height - 2.0 * border);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, 0.0, 0.0, 1.0, 0.33);
      cairo_rectangle(cr, width * 2.0 / 3.0, border, width / 3.0, height - 2.0 * border);
      cairo_fill(cr);
      cairo_rectangle(cr, border, border, width - 2.0 * border, height - 2.0 * border);
      break;
    }
  }

  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_set_line_width(cr, border);
  cairo_stroke(cr);

  cairo_restore(cr);
}

static gboolean _lib_histogram_configure_callback(GtkWidget *widget, GdkEventConfigure *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  const int width = event->width;
  // mode and color buttons position on first expose or widget size change
  // FIXME: should the button size depend on histogram width or just be set to something reasonable
  d->button_spacing = 0.02 * width;
  d->button_w = 0.06 * width;
  d->button_h = 0.06 * width;
  d->button_y = d->button_spacing;
  const float offset = d->button_w + d->button_spacing;
  d->blue_x = width - offset;
  d->green_x = d->blue_x - offset;
  d->red_x = d->green_x - offset;
  d->mode_x = d->red_x - offset;
  d->type_x = d->mode_x - offset;

  return TRUE;
}

static void _lib_histogram_draw_histogram(cairo_t *cr, int width, int height, const uint8_t mask[3])
{
  dt_develop_t *dev = darktable.develop;

  dt_pthread_mutex_lock(&dev->preview_pipe_mutex);
  const size_t histsize = 256 * 4 * sizeof(uint32_t); // histogram size is hardcoded :(
  // FIXME: does this need to be aligned?
  uint32_t *hist = malloc(histsize);
  if(hist) memcpy(hist, dev->histogram, histsize);
  dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);

  if(hist == NULL) return;
  if(!dev->histogram_max) return;

  const float hist_max = dev->histogram_type == DT_DEV_HISTOGRAM_LINEAR ? dev->histogram_max
                                                                        : logf(1.0 + dev->histogram_max);
  cairo_translate(cr, 0, height);
  cairo_scale(cr, width / 255.0, -(height - 10) / hist_max);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  for(int k = 0; k < 3; k++)
    if(mask[k])
    {
      cairo_set_source_rgba(cr, k == 0 ? 1. : 0., k == 1 ? 1. : 0., k == 2 ? 1. : 0., 0.5);
      dt_draw_histogram_8(cr, hist, 4, k, dev->histogram_type == DT_DEV_HISTOGRAM_LINEAR);
    }
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

  // FIXME: just free if it isn't aligned
  free(hist);
}

static void _lib_histogram_draw_waveform(cairo_t *cr, int width, int height, const uint8_t mask[3])
{
  dt_develop_t *dev = darktable.develop;

  dt_pthread_mutex_lock(&dev->preview_pipe_mutex);
  const int wf_width = dev->histogram_waveform_width;
  const int wf_height = dev->histogram_waveform_height;
  const gint wf_stride = dev->histogram_waveform_stride;
  uint8_t *wav = malloc(sizeof(uint8_t) * wf_height * wf_stride * 4);
  if(wav)
  {
    const uint8_t *const wf_buf = dev->histogram_waveform;
    for(int y = 0; y < wf_height; y++)
      for(int x = 0; x < wf_width; x++)
        for(int k = 0; k < 3; k++)
          wav[4 * (y * wf_stride + x) + k] = wf_buf[wf_stride * (y + k * wf_height) + x] * mask[2-k];
  }
  dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
  if(wav == NULL) return;

  // NOTE: The nice way to do this would be to draw each color channel
  // separately, overlaid, via cairo. Unfortunately, that is about
  // twice as slow as compositing the channels by hand, so we do the
  // latter, at the cost of some extra code (and comments) and of
  // making the color channel selector work by hand.

  cairo_surface_t *source
      = dt_cairo_image_surface_create_for_data(wav, CAIRO_FORMAT_RGB24,
                                               wf_width, wf_height, wf_stride * 4);
  cairo_scale(cr, darktable.gui->ppd*width/wf_width, darktable.gui->ppd*height/wf_height);
  cairo_set_source_surface(cr, source, 0.0, 0.0);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_paint(cr);
  cairo_surface_destroy(source);

  free(wav);
}

static void _lib_histogram_draw_rgb_parade(cairo_t *cr, int width, int height, const uint8_t mask[3])
{
  dt_develop_t *dev = darktable.develop;

  dt_pthread_mutex_lock(&dev->preview_pipe_mutex);
  const int wf_width = dev->histogram_waveform_width;
  const int wf_height = dev->histogram_waveform_height;
  const gint wf_stride = dev->histogram_waveform_stride;
  const size_t histsize = sizeof(uint8_t) * wf_height * wf_stride * 3;
  uint8_t *wav = malloc(histsize);
  if(wav) memcpy(wav, dev->histogram_waveform, histsize);
  dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
  if(wav == NULL) return;

  // don't multiply by ppd as the source isn't screen pixels (though the mask is pixels)
  cairo_scale(cr, (double)width/(wf_width*3), (double)height/wf_height);
  // this makes the blue come in more than CAIRO_OPERATOR_ADD, as it can go darker than the background
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  for(int k = 0; k < 3; k++)
  {
    if(mask[k])
    {
      cairo_save(cr);
      cairo_set_source_rgb(cr, k==0, k==1, k==2);
      cairo_surface_t *alpha
          = cairo_image_surface_create_for_data(wav + wf_stride * wf_height * (2-k),
                                                CAIRO_FORMAT_A8, wf_width, wf_height, wf_stride);
      cairo_mask_surface(cr, alpha, 0, 0);
      cairo_surface_destroy(alpha);
      cairo_restore(cr);
    }
    cairo_translate(cr, wf_width, 0);
  }

  free(wav);
}

static gboolean _lib_histogram_draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_develop_t *dev = darktable.develop;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width, height = allocation.height;

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0, width, height);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.5)); // borders width

  // Draw frame and background
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, width, height);
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_stroke_preserve(cr);
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_fill(cr);
  cairo_restore(cr);

  // exposure change regions
  if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
  {
    cairo_set_source_rgb(cr, .5, .5, .5);
    if(dev->scope_type == DT_DEV_SCOPE_WAVEFORM)
      cairo_rectangle(cr, 0, 7.0/9.0 * height, width, height);
    else
      cairo_rectangle(cr, 0, 0, 0.2 * width, height);
    cairo_fill(cr);
  }
  else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
  {
    cairo_set_source_rgb(cr, .5, .5, .5);
    if(dev->scope_type == DT_DEV_SCOPE_WAVEFORM)
      cairo_rectangle(cr, 0, 0, width, 7.0/9.0 * height);
    else
      cairo_rectangle(cr, 0.2 * width, 0, width, height);
    cairo_fill(cr);
  }

  // draw grid
  set_color(cr, darktable.bauhaus->graph_grid);

  if(dev->scope_type == DT_DEV_SCOPE_WAVEFORM)
    dt_draw_waveform_lines(cr, 0, 0, width, height);
  else
    dt_draw_grid(cr, 4, 0, 0, width, height);

  // draw scope
  if(dev->image_storage.id == dev->preview_pipe->output_imgid)
  {
    cairo_save(cr);
    uint8_t mask[3] = { d->red, d->green, d->blue };
    switch(dev->scope_type)
    {
      case DT_DEV_SCOPE_HISTOGRAM:
        _lib_histogram_draw_histogram(cr, width, height, mask);
        break;
      case DT_DEV_SCOPE_WAVEFORM:
        if(d->waveform_type == DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID)
          _lib_histogram_draw_waveform(cr, width, height, mask);
        else
          _lib_histogram_draw_rgb_parade(cr, width, height, mask);
        break;
      case DT_DEV_SCOPE_N:
        g_assert_not_reached();
    }
    cairo_restore(cr);
  }

  // buttons to control the display of the histogram: linear/log, r, g, b
  if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
  {
    _draw_type_toggle(cr, d->type_x, d->button_y, d->button_w, d->button_h, dev->scope_type);
    switch(dev->scope_type)
    {
      case DT_DEV_SCOPE_HISTOGRAM:
        _draw_histogram_mode_toggle(cr, d->mode_x, d->button_y, d->button_w, d->button_h, dev->histogram_type);
        break;
      case DT_DEV_SCOPE_WAVEFORM:
        _draw_waveform_mode_toggle(cr, d->mode_x, d->button_y, d->button_w, d->button_h, d->waveform_type);
        break;
      case DT_DEV_SCOPE_N:
        g_assert_not_reached();
    }
    cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.33);
    _draw_color_toggle(cr, d->red_x, d->button_y, d->button_w, d->button_h, d->red);
    cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 0.33);
    _draw_color_toggle(cr, d->green_x, d->button_y, d->button_w, d->button_h, d->green);
    cairo_set_source_rgba(cr, 0.0, 0.0, 1.0, 0.33);
    _draw_color_toggle(cr, d->blue_x, d->button_y, d->button_w, d->button_h, d->blue);
  }

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  return TRUE;
}

static gboolean _lib_histogram_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event,
                                                      gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_develop_t *dev = darktable.develop;

  /* check if exposure hooks are available */
  const gboolean hooks_available = dt_dev_exposure_hooks_available(dev);

  if(!hooks_available) return TRUE;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  if(d->dragging)
  {
    const float diff = dev->scope_type == DT_DEV_SCOPE_WAVEFORM ? d->button_down_y - event->y
                                                                : event->x - d->button_down_x;
    const int range = dev->scope_type == DT_DEV_SCOPE_WAVEFORM ? allocation.height
                                                               : allocation.width;
    if (d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
    {
      const float exposure = d->exposure + diff * 4.0f / (float)range;
      dt_dev_exposure_set_exposure(dev, exposure);
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
    {
      const float black = d->black - diff * .1f / (float)range;
      dt_dev_exposure_set_black(dev, black);
    }
  }
  else
  {
    const float x = event->x;
    const float y = event->y;
    const float posx = x / (float)(allocation.width);
    const float posy = y / (float)(allocation.height);
    const dt_lib_histogram_highlight_t prior_highlight = d->highlight;

    // FIXME: rather than roll button code from scratch, take advantage of bauhaus/gtk button code?
    if(posx < 0.0f || posx > 1.0f || posy < 0.0f || posy > 1.0f)
      ;
    // FIXME: simplify this, check for y position, and if it's in range, check for x, and set highlight, and depending on that draw tooltip
    // FIXME: or alternately use copy_path_flat(), append_path(p), in_fill() and keep around the rectangles for each button
    else if(x > d->type_x && x < d->type_x + d->button_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_TYPE;
      switch(dev->scope_type)
      {
        case DT_DEV_SCOPE_HISTOGRAM:
          gtk_widget_set_tooltip_text(widget, _("set mode to waveform"));
          break;
        case DT_DEV_SCOPE_WAVEFORM:
          gtk_widget_set_tooltip_text(widget, _("set mode to histogram"));
          break;
        case DT_DEV_SCOPE_N:
          g_assert_not_reached();
      }
    }
    else if(x > d->mode_x && x < d->mode_x + d->button_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_MODE;
      switch(dev->scope_type)
      {
        case DT_DEV_SCOPE_HISTOGRAM:
          switch(dev->histogram_type)
          {
            case DT_DEV_HISTOGRAM_LOGARITHMIC:
              gtk_widget_set_tooltip_text(widget, _("set mode to linear"));
              break;
            case DT_DEV_HISTOGRAM_LINEAR:
              gtk_widget_set_tooltip_text(widget, _("set mode to logarithmic"));
              break;
            default:
              g_assert_not_reached();
          }
          break;
        case DT_DEV_SCOPE_WAVEFORM:
          switch(d->waveform_type)
          {
            case DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID:
              gtk_widget_set_tooltip_text(widget, _("set mode to RGB parade"));
              break;
            case DT_LIB_HISTOGRAM_WAVEFORM_PARADE:
              gtk_widget_set_tooltip_text(widget, _("set mode to waveform"));
              break;
            default:
              g_assert_not_reached();
          }
          break;
        case DT_DEV_SCOPE_N:
          g_assert_not_reached();
      }
    }
    else if(x > d->red_x && x < d->red_x + d->button_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_RED;
      gtk_widget_set_tooltip_text(widget, d->red ? _("click to hide red channel") : _("click to show red channel"));
    }
    else if(x > d->green_x && x < d->green_x + d->button_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_GREEN;
      gtk_widget_set_tooltip_text(widget, d->green ? _("click to hide green channel")
                                                 : _("click to show green channel"));
    }
    else if(x > d->blue_x && x < d->blue_x + d->button_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_BLUE;
      gtk_widget_set_tooltip_text(widget, d->blue ? _("click to hide blue channel") : _("click to show blue channel"));
    }
    else if((posx < 0.2f && dev->scope_type == DT_DEV_SCOPE_HISTOGRAM) ||
            (posy > 7.0f/9.0f && dev->scope_type == DT_DEV_SCOPE_WAVEFORM))
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT;
      gtk_widget_set_tooltip_text(widget, _("drag to change black point,\ndoubleclick resets\nctrl+scroll to change display height"));
    }
    else
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE;
      gtk_widget_set_tooltip_text(widget, _("drag to change exposure,\ndoubleclick resets\nctrl+scroll to change display height"));
    }
    if(prior_highlight != d->highlight)
    {
      if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT ||
         d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
        dt_control_change_cursor(GDK_HAND1);
      else
        dt_control_change_cursor(GDK_LEFT_PTR);
      gtk_widget_queue_draw(widget);
    }
  }
  gint x, y; // notify gtk for motion_hint.
#if GTK_CHECK_VERSION(3, 20, 0)
  gdk_window_get_device_position(gtk_widget_get_window(widget),
      gdk_seat_get_pointer(gdk_display_get_default_seat(
          gdk_window_get_display(event->window))),
      &x, &y, 0);
#else
  gdk_window_get_device_position(event->window,
                                 gdk_device_manager_get_client_pointer(
                                     gdk_display_get_device_manager(gdk_window_get_display(event->window))),
                                 &x, &y, NULL);
#endif

  return TRUE;
}

static gboolean _lib_histogram_button_press_callback(GtkWidget *widget, GdkEventButton *event,
                                                     gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_develop_t *dev = darktable.develop;

  /* check if exposure hooks are available */
  const gboolean hooks_available = dt_dev_exposure_hooks_available(dev);

  if(!hooks_available) return TRUE;

  if(event->type == GDK_2BUTTON_PRESS &&
     (d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT || d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE))
  {
    dt_dev_exposure_reset_defaults(dev);
  }
  else
  {
    if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_TYPE)
    {
      dev->scope_type = (dev->scope_type + 1) % DT_DEV_SCOPE_N;
      dt_conf_set_string("plugins/darkroom/histogram/mode",
                         dt_dev_scope_type_names[dev->scope_type]);
      // we need to reprocess the preview pipe
      // FIXME: can we only make the regular histogram if we're drawing it? if so then reprocess the preview pipe when switch to that as well
      if(dev->scope_type == DT_DEV_SCOPE_WAVEFORM)
      {
        dt_dev_process_preview(dev);
      }
    }
    if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_MODE)
    {
      switch(dev->scope_type)
      {
        case DT_DEV_SCOPE_HISTOGRAM:
          dev->histogram_type = (dev->histogram_type + 1) % DT_DEV_HISTOGRAM_N;
          dt_conf_set_string("plugins/darkroom/histogram/histogram",
                             dt_lib_histogram_histogram_type_names[dev->histogram_type]);
          break;
        case DT_DEV_SCOPE_WAVEFORM:
          d->waveform_type = (d->waveform_type + 1) % DT_LIB_HISTOGRAM_WAVEFORM_N;
          dt_conf_set_string("plugins/darkroom/histogram/waveform",
                             dt_lib_histogram_waveform_type_names[d->waveform_type]);
          break;
        case DT_DEV_SCOPE_N:
          g_assert_not_reached();
      }
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_RED)
    {
      d->red = !d->red;
      dt_conf_set_bool("plugins/darkroom/histogram/show_red", d->red);
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_GREEN)
    {
      d->green = !d->green;
      dt_conf_set_bool("plugins/darkroom/histogram/show_green", d->green);
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLUE)
    {
      d->blue = !d->blue;
      dt_conf_set_bool("plugins/darkroom/histogram/show_blue", d->blue);
    }
    else
    {
      d->dragging = 1;

      if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
      {
        d->exposure = dt_dev_exposure_get_exposure(dev);
      }

      if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLUE)
      {
        d->black = dt_dev_exposure_get_black(dev);
      }

      d->button_down_x = event->x;
      d->button_down_y = event->y;
    }
  }
  // update for good measure
  dt_control_queue_redraw_widget(self->widget);

  return TRUE;
}

static gboolean _lib_histogram_scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_develop_t *dev = darktable.develop;

  const float ce = dt_dev_exposure_get_exposure(dev);
  const float cb = dt_dev_exposure_get_black(dev);

  int delta_y;
  // note are using unit rather than smooth scroll events, as
  // exposure changes can get laggy if handling a multitude of smooth
  // scroll events
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    if(event->state & GDK_CONTROL_MASK && !darktable.gui->reset)
    {
      /* set size of navigation draw area */
      const float histheight = clamp_range_f(dt_conf_get_int("plugins/darkroom/histogram/height") * 1.0f + 10 * delta_y, 100.0f, 200.0f);
      dt_conf_set_int("plugins/darkroom/histogram/height", histheight);
      gtk_widget_set_size_request(self->widget, -1, DT_PIXEL_APPLY_DPI(histheight));
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
    {
      dt_dev_exposure_set_exposure(dev, ce - 0.15f * delta_y);
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
    {
      dt_dev_exposure_set_black(dev, cb + 0.001f * delta_y);
    }
  }

  return TRUE;
}

static gboolean _lib_histogram_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                       gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  d->dragging = 0;
  return TRUE;
}

static gboolean _lib_histogram_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                     gpointer user_data)
{
  dt_control_change_cursor(GDK_HAND1);
  return TRUE;
}

static gboolean _lib_histogram_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                     gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  d->dragging = 0;
  d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
  dt_control_change_cursor(GDK_LEFT_PTR);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean _lib_histogram_collapse_callback(GtkAccelGroup *accel_group,
                                                GObject *acceleratable, guint keyval,
                                                GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;

  // Get the state
  const gint visible = dt_lib_is_visible(self);

  // Inverse the visibility
  dt_lib_set_visible(self, !visible);

  return TRUE;
}

static gboolean _lib_histogram_cycle_mode_callback(GtkAccelGroup *accel_group,
                                                GObject *acceleratable, guint keyval,
                                                GdkModifierType modifier, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_develop_t *dev = darktable.develop;

  // The cycle order is Hist log -> Lin -> Waveform -> parade (update logic on more scopes)

  switch(dev->scope_type)
  {
    case DT_DEV_SCOPE_HISTOGRAM:
      dev->histogram_type = (dev->histogram_type + 1);
      if(dev->histogram_type == DT_DEV_HISTOGRAM_N)
      {
        dev->histogram_type = DT_DEV_HISTOGRAM_LOGARITHMIC;
        d->waveform_type = DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID;
        dev->scope_type = DT_DEV_SCOPE_WAVEFORM;
      }
      break;
    case DT_DEV_SCOPE_WAVEFORM:
      d->waveform_type = (d->waveform_type + 1);
      if(d->waveform_type == DT_LIB_HISTOGRAM_WAVEFORM_N)
      {
        dev->histogram_type = DT_DEV_HISTOGRAM_LOGARITHMIC;
        d->waveform_type = DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID;
        dev->scope_type = DT_DEV_SCOPE_HISTOGRAM;
      }
      break;
    case DT_DEV_SCOPE_N:
      g_assert_not_reached();
  }
  dt_conf_set_string("plugins/darkroom/histogram/mode",
                     dt_dev_scope_type_names[dev->scope_type]);
  dt_conf_set_string("plugins/darkroom/histogram/histogram",
                     dt_lib_histogram_histogram_type_names[dev->histogram_type]);
  dt_conf_set_string("plugins/darkroom/histogram/waveform",
                     dt_lib_histogram_waveform_type_names[d->waveform_type]);

  if(dev->scope_type == DT_DEV_SCOPE_WAVEFORM)
  {
    dt_dev_process_preview(dev);
  }

  dt_control_queue_redraw_widget(self->widget);

  return TRUE;
}

static gboolean _lib_histogram_change_mode_callback(GtkAccelGroup *accel_group,
                                                GObject *acceleratable, guint keyval,
                                                GdkModifierType modifier, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_develop_t *dev = darktable.develop;
  dev->scope_type = (dev->scope_type + 1) % DT_DEV_SCOPE_N;
  dt_conf_set_string("plugins/darkroom/histogram/mode",
                     dt_dev_scope_type_names[dev->scope_type]);
  // we need to reprocess the preview pipe
  // FIXME: can we only make the regular histogram if we're drawing it? if so then reprocess the preview pipe when switch to that as well
  if(dev->scope_type == DT_DEV_SCOPE_WAVEFORM)
  {
    dt_dev_process_preview(dev);
  }
  dt_control_queue_redraw_widget(self->widget);
  return TRUE;
}

static gboolean _lib_histogram_change_type_callback(GtkAccelGroup *accel_group,
                                                GObject *acceleratable, guint keyval,
                                                GdkModifierType modifier, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_develop_t *dev = darktable.develop;

  switch(dev->scope_type)
  {
    case DT_DEV_SCOPE_HISTOGRAM:
      dev->histogram_type = (dev->histogram_type + 1) % DT_DEV_HISTOGRAM_N;
      dt_conf_set_string("plugins/darkroom/histogram/histogram",
                         dt_lib_histogram_histogram_type_names[dev->histogram_type]);
      break;
    case DT_DEV_SCOPE_WAVEFORM:
      d->waveform_type = (d->waveform_type + 1) % DT_LIB_HISTOGRAM_WAVEFORM_N;
      dt_conf_set_string("plugins/darkroom/histogram/waveform",
                         dt_lib_histogram_waveform_type_names[d->waveform_type]);
      break;
    case DT_DEV_SCOPE_N:
      g_assert_not_reached();
  }
  if(dev->scope_type == DT_DEV_SCOPE_WAVEFORM)
  {
    dt_dev_process_preview(dev);
  }
  dt_control_queue_redraw_widget(self->widget);
  return TRUE;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)g_malloc0(sizeof(dt_lib_histogram_t));
  self->data = (void *)d;

  d->red = dt_conf_get_bool("plugins/darkroom/histogram/show_red");
  d->green = dt_conf_get_bool("plugins/darkroom/histogram/show_green");
  d->blue = dt_conf_get_bool("plugins/darkroom/histogram/show_blue");

  gchar *waveform_type = dt_conf_get_string("plugins/darkroom/histogram/waveform");
  if(g_strcmp0(waveform_type, "overlaid") == 0)
    d->waveform_type = DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID;
  else if(g_strcmp0(waveform_type, "parade") == 0)
    d->waveform_type = DT_LIB_HISTOGRAM_WAVEFORM_PARADE;
  g_free(waveform_type);

  /* create drawingarea */
  self->widget = gtk_drawing_area_new();
  gtk_widget_set_name(self->widget, "main-histogram");
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  gtk_widget_add_events(self->widget, GDK_LEAVE_NOTIFY_MASK | GDK_ENTER_NOTIFY_MASK | GDK_POINTER_MOTION_MASK
                                      | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                      darktable.gui->scroll_mask);

  /* connect callbacks */
  gtk_widget_set_tooltip_text(self->widget, _("drag to change exposure,\ndoubleclick resets\nctrl+scroll to change display height"));
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(_lib_histogram_draw_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "button-press-event",
                   G_CALLBACK(_lib_histogram_button_press_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "button-release-event",
                   G_CALLBACK(_lib_histogram_button_release_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "motion-notify-event",
                   G_CALLBACK(_lib_histogram_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "leave-notify-event",
                   G_CALLBACK(_lib_histogram_leave_notify_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "enter-notify-event",
                   G_CALLBACK(_lib_histogram_enter_notify_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "scroll-event", G_CALLBACK(_lib_histogram_scroll_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "configure-event",
                   G_CALLBACK(_lib_histogram_configure_callback), self);

  /* set size of navigation draw area */
  const float histheight = dt_conf_get_int("plugins/darkroom/histogram/height") * 1.0f;
  gtk_widget_set_size_request(self->widget, -1, DT_PIXEL_APPLY_DPI(histheight));

  /* connect to preview pipe finished  signal */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(_lib_histogram_change_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* disconnect callback from  signal */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_histogram_change_callback), self);

  g_free(self->data);
  self->data = NULL;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib_as_view("darkroom", NC_("accel", "histogram/hide histogram"), GDK_KEY_H, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_lib_as_view("tethering", NC_("accel", "hide histogram"), GDK_KEY_H, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_lib_as_view("darkroom", NC_("accel", "histogram/cycle histogram modes"), 0, 0);
  dt_accel_register_lib_as_view("tethering", NC_("accel", "cycle histogram modes"), 0, 0);
  dt_accel_register_lib_as_view("darkroom", NC_("accel", "histogram/switch histogram mode"), 0, 0);
  dt_accel_register_lib_as_view("tethering", NC_("accel", "switch histogram mode"), 0, 0);
  dt_accel_register_lib_as_view("darkroom", NC_("accel", "histogram/switch histogram type"), 0, 0);
  dt_accel_register_lib_as_view("tethering", NC_("accel", "switch histogram type"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_accel_connect_lib_as_view(self, "darkroom", "histogram/hide histogram",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_collapse_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "tethering", "hide histogram",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_collapse_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "darkroom", "histogram/cycle histogram modes",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_cycle_mode_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "tethering", "cycle histogram modes",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_cycle_mode_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "darkroom", "histogram/switch histogram mode",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_change_mode_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "tethering", "switch histogram mode",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_change_mode_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "darkroom", "histogram/switch histogram type",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_change_type_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "tethering", "switch histogram type",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_change_type_callback), self, NULL));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
