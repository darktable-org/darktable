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
#include "common/histogram.h"
#include "common/iop_profile.h"
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
#include "libs/colorpicker.h"

#define HISTOGRAM_BINS 256

DT_MODULE(1)

typedef enum dt_lib_histogram_highlight_t
{
  DT_LIB_HISTOGRAM_HIGHLIGHT_OUTSIDE_WIDGET = 0,
  DT_LIB_HISTOGRAM_HIGHLIGHT_IN_WIDGET,
  DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT,
  DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE,
  DT_LIB_HISTOGRAM_HIGHLIGHT_TYPE,
  DT_LIB_HISTOGRAM_HIGHLIGHT_MODE,
  DT_LIB_HISTOGRAM_HIGHLIGHT_RED,
  DT_LIB_HISTOGRAM_HIGHLIGHT_GREEN,
  DT_LIB_HISTOGRAM_HIGHLIGHT_BLUE,
} dt_lib_histogram_highlight_t;

typedef enum dt_lib_histogram_scope_type_t
{
  DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM = 0,
  DT_LIB_HISTOGRAM_SCOPE_WAVEFORM,
  DT_LIB_HISTOGRAM_SCOPE_N // needs to be the last one
} dt_lib_histogram_scope_type_t;

typedef enum dt_lib_histogram_scale_t
{
  DT_LIB_HISTOGRAM_LOGARITHMIC = 0,
  DT_LIB_HISTOGRAM_LINEAR,
  DT_LIB_HISTOGRAM_N // needs to be the last one
} dt_lib_histogram_scale_t;

typedef enum dt_lib_histogram_waveform_type_t
{
  DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID = 0,
  DT_LIB_HISTOGRAM_WAVEFORM_PARADE,
  DT_LIB_HISTOGRAM_WAVEFORM_N // needs to be the last one
} dt_lib_histogram_waveform_type_t;

const gchar *dt_lib_histogram_scope_type_names[DT_LIB_HISTOGRAM_SCOPE_N] = { "histogram", "waveform" };
const gchar *dt_lib_histogram_histogram_scale_names[DT_LIB_HISTOGRAM_N] = { "logarithmic", "linear" };
const gchar *dt_lib_histogram_waveform_type_names[DT_LIB_HISTOGRAM_WAVEFORM_N] = { "overlaid", "parade" };

typedef struct dt_lib_histogram_t
{
  // histogram for display
  uint32_t *histogram;
  uint32_t histogram_max;
  // waveform histogram buffer and dimensions
  uint8_t *waveform;
  uint32_t waveform_width, waveform_height, waveform_stride;
  dt_pthread_mutex_t lock;
  // exposure params on mouse down
  float exposure, black;
  // mouse state
  int32_t dragging;
  int32_t button_down_x, button_down_y;
  // depends on mouse positon
  dt_lib_histogram_highlight_t highlight;
  // state set by buttons
  dt_lib_histogram_scope_type_t scope_type;
  dt_lib_histogram_scale_t histogram_scale;
  dt_lib_histogram_waveform_type_t waveform_type;
  gboolean red, green, blue;
  // button locations
  float type_x, mode_x, red_x, green_x, blue_x;
  float button_w, button_h, button_y, button_spacing;
} dt_lib_histogram_t;

const char *name(dt_lib_module_t *self)
{
  return _("histogram");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", "tethering", NULL};
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


static void _lib_histogram_process_histogram(dt_lib_histogram_t *d, const float *const input, int width, int height)
{
  dt_dev_histogram_collection_params_t histogram_params = { 0 };
  const dt_iop_colorspace_type_t cst = iop_cs_rgb;
  dt_dev_histogram_stats_t histogram_stats = { .bins_count = HISTOGRAM_BINS, .ch = 4, .pixels = 0 };
  uint32_t histogram_max[4] = { 0 };
  dt_histogram_roi_t histogram_roi = { .width = width, .height = height,
                                      .crop_x = 0, .crop_y = 0, .crop_width = 0, .crop_height = 0 };

  // Constraining the area if the colorpicker is active in area mode
  dt_develop_t *dev = darktable.develop;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_DARKROOM &&
     dev->gui_module && !strcmp(dev->gui_module->op, "colorout")
     && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF
     && darktable.lib->proxy.colorpicker.restrict_histogram)
  {
    if(darktable.lib->proxy.colorpicker.size == DT_COLORPICKER_SIZE_BOX)
    {
      histogram_roi.crop_x = MIN(width, MAX(0, dev->gui_module->color_picker_box[0] * width));
      histogram_roi.crop_y = MIN(height, MAX(0, dev->gui_module->color_picker_box[1] * height));
      histogram_roi.crop_width = width - MIN(width, MAX(0, dev->gui_module->color_picker_box[2] * width));
      histogram_roi.crop_height = height - MIN(height, MAX(0, dev->gui_module->color_picker_box[3] * height));
    }
    else
    {
      histogram_roi.crop_x = MIN(width, MAX(0, dev->gui_module->color_picker_point[0] * width));
      histogram_roi.crop_y = MIN(height, MAX(0, dev->gui_module->color_picker_point[1] * height));
      histogram_roi.crop_width = width - MIN(width, MAX(0, dev->gui_module->color_picker_point[0] * width));
      histogram_roi.crop_height = height - MIN(height, MAX(0, dev->gui_module->color_picker_point[1] * height));
    }
  }

  dt_times_t start_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  d->histogram_max = 0;
  memset(d->histogram, 0, sizeof(uint32_t) * 4 * HISTOGRAM_BINS);

  histogram_params.roi = &histogram_roi;
  histogram_params.bins_count = HISTOGRAM_BINS;
  histogram_params.mul = histogram_params.bins_count - 1;

  dt_histogram_helper(&histogram_params, &histogram_stats, cst, iop_cs_NONE, input, &d->histogram, FALSE, NULL);
  dt_histogram_max_helper(&histogram_stats, cst, iop_cs_NONE, &d->histogram, histogram_max);
  d->histogram_max = MAX(MAX(histogram_max[0], histogram_max[1]), histogram_max[2]);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end_time = { 0 };
    dt_get_times(&end_time);
    fprintf(stderr, "final histogram took %.3f secs (%.3f CPU)\n", end_time.clock - start_time.clock, end_time.user - start_time.user);
  }
}

static void _lib_histogram_process_waveform(dt_lib_histogram_t *d, const float *const input, int width, int height)
{
  dt_times_t start_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  const int waveform_height = d->waveform_height;
  const int waveform_stride = d->waveform_stride;
  uint8_t *const waveform = d->waveform;
  // Use integral sized bins for columns, as otherwise they will be
  // unequal and have banding. Rely on draw to smoothly do horizontal
  // scaling.
  // Note that waveform_stride is pre-initialized/hardcoded,
  // but waveform_width varies, depending on preview image
  // width and # of bins.
  const int bin_width = ceilf(width / (float)waveform_stride);
  const int waveform_width = ceilf(width / (float)bin_width);

  // max input size should be 1440x900, and with a bin_width of 1,
  // that makes a maximum possible count of 900 in buf, while even if
  // waveform buffer is 128 (about smallest possible), bin_width is
  // 12, making max count of 10,800, still much smaller than uint16_t
  uint16_t *buf = calloc(waveform_width * waveform_height * 3, sizeof(uint16_t));

  // 1.0 is at 8/9 of the height!
  const float _height = (float)(waveform_height - 1);

  // count the colors into buf ...
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
  dt_omp_firstprivate(bin_width, _height, waveform_width, input, buf, width, height) \
  schedule(static)
#endif
  for(int in_y = 0; in_y < height; in_y++)
  {
    for(int in_x = 0; in_x < width; in_x++)
    {
      const float *const in = input + 4 * (in_y*width + in_x);
      const int out_x = in_x / bin_width;
      for(int k = 0; k < 3; k++)
      {
        const float v = 1.0f - (8.0f / 9.0f) * in[2 - k];
        // flipped from dt's CLAMPS so as to treat NaN's as 0 (NaN compares false)
        const int out_y = (v < 1.0f ? (v > 0.0f ? v : 0.0f) : 1.0f) * _height;
        __sync_add_and_fetch(buf + (out_x + waveform_width * out_y) * 3 + k, 1);
      }
    }
  }

  // TODO: Find a nicer function to map buf -> image than just clipping
  //         float factor[3];
  //         for(int k = 0; k < 3; k++)
  //           factor[k] = 255.0 / (float)(maxcol[k] - mincol[k]); // leave some clipping

  // ... and scale that into a nice image. putting the pixels into the image directly gets too
  // saturated/clips.

  // new scale factor to do about the same as the old one for 1MP views, but scale to hidpi
  const float scale = 0.5 * 1e6f/(height*width) *
    (waveform_width*waveform_height) / (350.0f*233.)
    / 255.0f; // normalization to 0..1 for gamma correction
  const float gamma = 1.0 / 1.5; // TODO make this settable from the gui?
  //uint32_t mincol[3] = {UINT32_MAX,UINT32_MAX,UINT32_MAX}, maxcol[3] = {0,0,0};
  // even bin_width 12 and height 900 image gives 10,800 byte cache, more normal will ~1K
  const int cache_size = (height * bin_width) + 1;
  uint8_t *cache = (uint8_t *)calloc(cache_size, sizeof(uint8_t));

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
  dt_omp_firstprivate(waveform_width, waveform_height, waveform_stride, buf, waveform, cache, scale, gamma) \
  schedule(static) collapse(2)
#endif
  for(int k = 0; k < 3; k++)
  {
    for(int out_y = 0; out_y < waveform_height; out_y++)
    {
      const uint16_t *const in = buf + (waveform_width * out_y) * 3 + k;
      uint8_t *const out = waveform + (waveform_stride * (waveform_height * k + out_y));
      for(int out_x = 0; out_x < waveform_width; out_x++)
      {
        //mincol[k] = MIN(mincol[k], in[k]);
        //maxcol[k] = MAX(maxcol[k], in[k]);
        const uint16_t v = in[out_x * 3];
        // cache XORd result so common casees cached and cache misses are quick to find
        if(!cache[v])
        {
          // multiple threads may be writing to cache[v], but as
          // they're writing the same value, don't declare omp atomic
          cache[v] = (uint8_t)(CLAMP(powf(v * scale, gamma) * 255.0, 0, 255)) ^ 1;
        }
        out[out_x] = cache[v] ^ 1;
        //               if(in[k] == 0)
        //                 out[k] = 0;
        //               else
        //                 out[k] = (float)(in[k] - mincol[k]) * factor[k];
      }
    }
  }
  //printf("mincol %d,%d,%d maxcol %d,%d,%d\n", mincol[0], mincol[1], mincol[2], maxcol[0], maxcol[1], maxcol[2]);
  d->waveform_width = waveform_width;

  free(cache);
  free(buf);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end_time = { 0 };
    dt_get_times(&end_time);
    fprintf(stderr, "final histogram waveform took %.3f secs (%.3f CPU)\n", end_time.clock - start_time.clock, end_time.user - start_time.user);
  }
}

static void dt_lib_histogram_process(struct dt_lib_module_t *self, const float *const input,
                                     int width, int height,
                                     dt_colorspaces_color_profile_type_t in_profile_type, const gchar *in_profile_filename)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_develop_t *dev = darktable.develop;
  float *img_display = NULL;

  // special case, clear the scopes
  if(!input)
  {
    dt_pthread_mutex_lock(&d->lock);
    memset(d->histogram, 0, sizeof(uint32_t) * 4 * HISTOGRAM_BINS);
    memset(d->waveform, 0, sizeof(uint8_t) * d->waveform_height * d->waveform_stride * 3);
    dt_pthread_mutex_unlock(&d->lock);
    return;
  }

  // Convert pixelpipe output to histogram profile. If in tether view,
  // then the image is already converted by the caller.
  if(in_profile_type != DT_COLORSPACE_NONE)
  {
    dt_colorspaces_color_profile_type_t out_profile_type;
    const char *out_profile_filename;
    const dt_iop_order_iccprofile_info_t *const profile_info_from
      = dt_ioppr_add_profile_info_to_list(dev, in_profile_type, in_profile_filename, INTENT_PERCEPTUAL);
    dt_ioppr_get_histogram_profile_type(&out_profile_type, &out_profile_filename);

    if(out_profile_type != DT_COLORSPACE_NONE)
    {
      const dt_iop_order_iccprofile_info_t *const profile_info_to =
        dt_ioppr_add_profile_info_to_list(dev, out_profile_type, out_profile_filename, DT_INTENT_PERCEPTUAL);
      img_display = dt_alloc_align(64, width * height * 4 * sizeof(float));
      if(!img_display) return;
      dt_ioppr_transform_image_colorspace_rgb(input, img_display, width, height, profile_info_from,
                                              profile_info_to, "final histogram");
    }
  }

  dt_pthread_mutex_lock(&d->lock);
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      _lib_histogram_process_histogram(d, img_display ? img_display : input, width, height);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      _lib_histogram_process_waveform(d, img_display ? img_display : input, width, height);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      g_assert_not_reached();
  }
  dt_pthread_mutex_unlock(&d->lock);

  if(img_display)
    dt_free_align(img_display);
}

static void _draw_color_toggle(cairo_t *cr, float x, float y, float width, float height, gboolean state)
{
  // FIXME: use dtgtk_cairo_paint_color()
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
  // FIXME: move to dtgtk/paint
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  cairo_move_to(cr, 2.0 * border, height - 2.0 * border);
  switch(type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      cairo_curve_to(cr, 0.3 * width, height - 2.0 * border, 0.3 * width, 2.0 * border,
                     0.5 * width, 2.0 * border);
      cairo_curve_to(cr, 0.7 * width, 2.0 * border, 0.7 * width, height - 2.0 * border,
                     width - 2.0 * border, height - 2.0 * border);
      cairo_fill(cr);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
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

static void _draw_histogram_scale_toggle(cairo_t *cr, float x, float y, float width, float height, int mode)
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
  // FIXME: move to dtgtk/paint
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  cairo_move_to(cr, 2.0 * border, height - 2.0 * border);
  switch(mode)
  {
    case DT_LIB_HISTOGRAM_LINEAR:
      cairo_line_to(cr, width - 2.0 * border, 2.0 * border);
      cairo_stroke(cr);
      break;
    case DT_LIB_HISTOGRAM_LOGARITHMIC:
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
  // FIXME: move to dtgtk/paint
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

  // FIXME: gtk-ize this, use buttons packed in a GtkBox
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

static void _lib_histogram_draw_histogram(dt_lib_histogram_t *d, cairo_t *cr, int width, int height, const uint8_t mask[3])
{
  if(!d->histogram_max) return;
  const float hist_max = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR ? d->histogram_max
                                                                       : logf(1.0 + d->histogram_max);
  cairo_translate(cr, 0, height);
  cairo_scale(cr, width / 255.0, -(height - 10) / hist_max);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  for(int k = 0; k < 3; k++)
    if(mask[k])
    {
      cairo_set_source_rgba(cr, k == 0 ? 1. : 0., k == 1 ? 1. : 0., k == 2 ? 1. : 0., 0.5);
      dt_draw_histogram_8(cr, d->histogram, 4, k, d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR);
    }
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
}

static void _lib_histogram_draw_waveform(dt_lib_histogram_t *d, cairo_t *cr, int width, int height, const uint8_t mask[3])
{
  const int wf_width = d->waveform_width;
  const int wf_height = d->waveform_height;
  const gint wf_stride = d->waveform_stride;
  // FIXME: skip this alloc/copy if there's a way not to re-arrange the data
  uint8_t *wav = malloc(sizeof(uint8_t) * wf_height * wf_stride * 4);
  if(wav)
  {
    const uint8_t *const wf_buf = d->waveform;
    for(int y = 0; y < wf_height; y++)
      for(int x = 0; x < wf_width; x++)
        for(int k = 0; k < 3; k++)
          wav[4 * (y * wf_stride + x) + k] = wf_buf[wf_stride * (y + k * wf_height) + x] * mask[2-k];
  }
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

static void _lib_histogram_draw_rgb_parade(dt_lib_histogram_t *d, cairo_t *cr, int width, int height, const uint8_t mask[3])
{
  const int wf_width = d->waveform_width;
  const int wf_height = d->waveform_height;
  const gint wf_stride = d->waveform_stride;

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
          = cairo_image_surface_create_for_data(d->waveform + wf_stride * wf_height * (2-k),
                                                CAIRO_FORMAT_A8, wf_width, wf_height, wf_stride);
      cairo_mask_surface(cr, alpha, 0, 0);
      cairo_surface_destroy(alpha);
      cairo_restore(cr);
    }
    cairo_translate(cr, wf_width, 0);
  }
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
    if(d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM)
      cairo_rectangle(cr, 0, 7.0/9.0 * height, width, height);
    else
      cairo_rectangle(cr, 0, 0, 0.2 * width, height);
    cairo_fill(cr);
  }
  else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
  {
    cairo_set_source_rgb(cr, .5, .5, .5);
    if(d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM)
      cairo_rectangle(cr, 0, 0, width, 7.0/9.0 * height);
    else
      cairo_rectangle(cr, 0.2 * width, 0, width, height);
    cairo_fill(cr);
  }

  // draw grid
  set_color(cr, darktable.bauhaus->graph_grid);

  if(d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM)
    dt_draw_waveform_lines(cr, 0, 0, width, height);
  else
    dt_draw_grid(cr, 4, 0, 0, width, height);

  // darkroom view: draw scope so long as preview pipe is finished
  // tether view: draw whatever has come in from tether
  // FIXME: should set histogram buffer to black if have just entered tether view and nothing is displayed, or if have left live view with no image selected
  dt_pthread_mutex_lock(&d->lock);
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_TETHERING || dev->image_storage.id == dev->preview_pipe->output_imgid)
  {
    cairo_save(cr);
    uint8_t mask[3] = { d->red, d->green, d->blue };
    switch(d->scope_type)
    {
      case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
        _lib_histogram_draw_histogram(d, cr, width, height, mask);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
        if(d->waveform_type == DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID)
          _lib_histogram_draw_waveform(d, cr, width, height, mask);
        else
          _lib_histogram_draw_rgb_parade(d, cr, width, height, mask);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_N:
        g_assert_not_reached();
    }
    cairo_restore(cr);
  }
  dt_pthread_mutex_unlock(&d->lock);

  // buttons to control the display of the histogram: linear/log, r, g, b
  if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_OUTSIDE_WIDGET)
  {
    _draw_type_toggle(cr, d->type_x, d->button_y, d->button_w, d->button_h, d->scope_type);
    switch(d->scope_type)
    {
      case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
        _draw_histogram_scale_toggle(cr, d->mode_x, d->button_y, d->button_w, d->button_h, d->histogram_scale);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
        _draw_waveform_mode_toggle(cr, d->mode_x, d->button_y, d->button_w, d->button_h, d->waveform_type);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_N:
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
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  const gboolean hooks_available = (cv->view(cv) == DT_VIEW_DARKROOM) && dt_dev_exposure_hooks_available(dev);
  // FIXME: as when dragging a bauhaus widget, delay processing the next event until the pixelpipe can update based on dev->preview_average_delay

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  if(d->dragging)
  {
    const float diff = d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM ? d->button_down_y - event->y
                                                                        : event->x - d->button_down_x;
    const int range = d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM ? allocation.height
                                                                       : allocation.width;
    if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
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
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_OUTSIDE_WIDGET;
    }
    // FIXME: simplify this, check for y position, and if it's in range, check for x, and set highlight, and depending on that draw tooltip
    // FIXME: or alternately use copy_path_flat(), append_path(p), in_fill() and keep around the rectangles for each button
    else if(x > d->type_x && x < d->type_x + d->button_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_TYPE;
      switch(d->scope_type)
      {
        case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
          gtk_widget_set_tooltip_text(widget, _("set mode to waveform"));
          break;
        case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
          gtk_widget_set_tooltip_text(widget, _("set mode to histogram"));
          break;
        case DT_LIB_HISTOGRAM_SCOPE_N:
          g_assert_not_reached();
      }
    }
    else if(x > d->mode_x && x < d->mode_x + d->button_w && y > d->button_y && y < d->button_y + d->button_h)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_MODE;
      switch(d->scope_type)
      {
        case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
          switch(d->histogram_scale)
          {
            case DT_LIB_HISTOGRAM_LOGARITHMIC:
              gtk_widget_set_tooltip_text(widget, _("set scale to linear"));
              break;
            case DT_LIB_HISTOGRAM_LINEAR:
              gtk_widget_set_tooltip_text(widget, _("set scale to logarithmic"));
              break;
            default:
              g_assert_not_reached();
          }
          break;
        case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
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
        case DT_LIB_HISTOGRAM_SCOPE_N:
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
    else if(hooks_available &&
            ((posx < 0.2f && d->scope_type == DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM) ||
             (posy > 7.0f/9.0f && d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM)))
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT;
      gtk_widget_set_tooltip_text(widget, _("drag to change black point,\ndoubleclick resets\nctrl+scroll to change display height"));
    }
    else if(hooks_available)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE;
      gtk_widget_set_tooltip_text(widget, _("drag to change exposure,\ndoubleclick resets\nctrl+scroll to change display height"));
    }
    else
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_IN_WIDGET;
      gtk_widget_set_tooltip_text(widget, _("ctrl+scroll to change display height"));
    }
    if(prior_highlight != d->highlight)
    {
      if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT ||
         d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
        dt_control_change_cursor(GDK_HAND1);
      else
        dt_control_change_cursor(GDK_LEFT_PTR);
      dt_control_queue_redraw_widget(widget);
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
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  const dt_view_type_flags_t view_type = cv->view(cv);
  const gboolean hooks_available = (view_type == DT_VIEW_DARKROOM) && dt_dev_exposure_hooks_available(dev);

  if(event->type == GDK_2BUTTON_PRESS && hooks_available &&
     (d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT || d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE))
  {
    dt_dev_exposure_reset_defaults(dev);
  }
  else
  {
    // FIXME: this handles repeated-click events in buttons weirdly, as it confuses them with doubleclicks
    if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_TYPE)
    {
      d->scope_type = (d->scope_type + 1) % DT_LIB_HISTOGRAM_SCOPE_N;
      dt_conf_set_string("plugins/darkroom/histogram/mode",
                         dt_lib_histogram_scope_type_names[d->scope_type]);
      // generate data for changed scope and trigger widget redraw
      if(view_type == DT_VIEW_DARKROOM)
        dt_dev_process_preview(dev);
      else
        dt_control_queue_redraw_center();
    }
    if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_MODE)
    {
      switch(d->scope_type)
      {
        case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
          d->histogram_scale = (d->histogram_scale + 1) % DT_LIB_HISTOGRAM_N;
          dt_conf_set_string("plugins/darkroom/histogram/histogram",
                             dt_lib_histogram_histogram_scale_names[d->histogram_scale]);
          // FIXME: this should really redraw current iop if its background is a histogram (check request_histogram)
          darktable.lib->proxy.histogram.is_linear = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR;
          break;
        case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
          d->waveform_type = (d->waveform_type + 1) % DT_LIB_HISTOGRAM_WAVEFORM_N;
          dt_conf_set_string("plugins/darkroom/histogram/waveform",
                             dt_lib_histogram_waveform_type_names[d->waveform_type]);
          break;
        case DT_LIB_HISTOGRAM_SCOPE_N:
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
    else if(hooks_available)
    {
      d->dragging = 1;
      if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
        d->exposure = dt_dev_exposure_get_exposure(dev);
      if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLUE)
        d->black = dt_dev_exposure_get_black(dev);
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
    else
    {
      dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
      dt_develop_t *dev = darktable.develop;
      const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
      const dt_view_type_flags_t view_type = cv->view(cv);
      if(view_type == DT_VIEW_DARKROOM && dt_dev_exposure_hooks_available(dev))
      {
        // FIXME: as with bauhaus widget, delay processing the next event until the pixelpipe can update based on dev->preview_average_delay
        if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
        {
          const float ce = dt_dev_exposure_get_exposure(dev);
          dt_dev_exposure_set_exposure(dev, ce - 0.15f * delta_y);
        }
        else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
        {
          const float cb = dt_dev_exposure_get_black(dev);
          dt_dev_exposure_set_black(dev, cb + 0.001f * delta_y);
        }
      }
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
  d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_OUTSIDE_WIDGET;
  dt_control_change_cursor(GDK_LEFT_PTR);
  dt_control_queue_redraw_widget(widget);
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

  // The cycle order is Hist log -> Lin -> Waveform -> parade (update logic on more scopes)

  dt_lib_histogram_scope_type_t old_scope = d->scope_type;
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      d->histogram_scale = (d->histogram_scale + 1);
      if(d->histogram_scale == DT_LIB_HISTOGRAM_N)
      {
        d->histogram_scale = DT_LIB_HISTOGRAM_LOGARITHMIC;
        d->waveform_type = DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID;
        d->scope_type = DT_LIB_HISTOGRAM_SCOPE_WAVEFORM;
      }
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      d->waveform_type = (d->waveform_type + 1);
      if(d->waveform_type == DT_LIB_HISTOGRAM_WAVEFORM_N)
      {
        d->histogram_scale = DT_LIB_HISTOGRAM_LOGARITHMIC;
        d->waveform_type = DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID;
        d->scope_type = DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM;
      }
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      g_assert_not_reached();
  }
  dt_conf_set_string("plugins/darkroom/histogram/mode",
                     dt_lib_histogram_scope_type_names[d->scope_type]);
  dt_conf_set_string("plugins/darkroom/histogram/histogram",
                     dt_lib_histogram_histogram_scale_names[d->histogram_scale]);
  dt_conf_set_string("plugins/darkroom/histogram/waveform",
                     dt_lib_histogram_waveform_type_names[d->waveform_type]);
  // FIXME: this should really redraw current iop if its background is a histogram (check request_histogram)
  darktable.lib->proxy.histogram.is_linear = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR;

  if(d->scope_type != old_scope)
  {
    const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
    if(cv->view(cv) == DT_VIEW_DARKROOM)
      dt_dev_process_preview(darktable.develop);
    else
      dt_control_queue_redraw_center();
  }
  else
  {
    // still update appearance
    dt_control_queue_redraw_widget(self->widget);
  }

  return TRUE;
}

static gboolean _lib_histogram_change_mode_callback(GtkAccelGroup *accel_group,
                                                GObject *acceleratable, guint keyval,
                                                GdkModifierType modifier, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  d->scope_type = (d->scope_type + 1) % DT_LIB_HISTOGRAM_SCOPE_N;
  dt_conf_set_string("plugins/darkroom/histogram/mode",
                     dt_lib_histogram_scope_type_names[d->scope_type]);
  if(cv->view(cv) == DT_VIEW_DARKROOM)
    dt_dev_process_preview(darktable.develop);
  else
    dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean _lib_histogram_change_type_callback(GtkAccelGroup *accel_group,
                                                GObject *acceleratable, guint keyval,
                                                GdkModifierType modifier, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      d->histogram_scale = (d->histogram_scale + 1) % DT_LIB_HISTOGRAM_N;
      dt_conf_set_string("plugins/darkroom/histogram/histogram",
                         dt_lib_histogram_histogram_scale_names[d->histogram_scale]);
      darktable.lib->proxy.histogram.is_linear = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR;
      // FIXME: this should really redraw current iop if its background is a histogram (check request_histogram)
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      d->waveform_type = (d->waveform_type + 1) % DT_LIB_HISTOGRAM_WAVEFORM_N;
      dt_conf_set_string("plugins/darkroom/histogram/waveform",
                         dt_lib_histogram_waveform_type_names[d->waveform_type]);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      g_assert_not_reached();
  }
  dt_control_queue_redraw_widget(self->widget);
  return TRUE;
}

// this is only called in darkroom view
static void _lib_histogram_preview_updated_callback(gpointer instance, dt_lib_module_t *self)
{
  // preview pipe has already given process() the high quality
  // pre-gamma image. Now that preview pipe is complete, draw it
  // FIXME: it would be nice if process() just queued a redraw if not in live view, but then our draw code would have to have some other way to assure that the histogram image is current besides checking the pixelpipe to see if it has processed the current image
  dt_control_queue_redraw_widget(self->widget);
}

void view_enter(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  if(new_view->view(new_view) == DT_VIEW_DARKROOM)
  {
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                              G_CALLBACK(_lib_histogram_preview_updated_callback), self);
  }
  // FIXME: set histogram data to blank if enter tether with no active image
}

void view_leave(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                               G_CALLBACK(_lib_histogram_preview_updated_callback),
                               self);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)g_malloc0(sizeof(dt_lib_histogram_t));
  self->data = (void *)d;

  dt_pthread_mutex_init(&d->lock, NULL);

  d->red = dt_conf_get_bool("plugins/darkroom/histogram/show_red");
  d->green = dt_conf_get_bool("plugins/darkroom/histogram/show_green");
  d->blue = dt_conf_get_bool("plugins/darkroom/histogram/show_blue");

  gchar *mode = dt_conf_get_string("plugins/darkroom/histogram/mode");
  if(g_strcmp0(mode, "histogram") == 0)
    d->scope_type = DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM;
  else if(g_strcmp0(mode, "waveform") == 0)
    d->scope_type = DT_LIB_HISTOGRAM_SCOPE_WAVEFORM;
  else if(g_strcmp0(mode, "linear") == 0)
  { // update legacy conf
    d->scope_type = DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM;
    dt_conf_set_string("plugins/darkroom/histogram/mode","histogram");
    dt_conf_set_string("plugins/darkroom/histogram/histogram","linear");
  }
  else if(g_strcmp0(mode, "logarithmic") == 0)
  { // update legacy conf
    d->scope_type = DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM;
    dt_conf_set_string("plugins/darkroom/histogram/mode","histogram");
    dt_conf_set_string("plugins/darkroom/histogram/histogram","logarithmic");
  }
  g_free(mode);

  gchar *histogram_scale = dt_conf_get_string("plugins/darkroom/histogram/histogram");
  if(g_strcmp0(histogram_scale, "linear") == 0)
    d->histogram_scale = DT_LIB_HISTOGRAM_LINEAR;
  else if(g_strcmp0(histogram_scale, "logarithmic") == 0)
    d->histogram_scale = DT_LIB_HISTOGRAM_LOGARITHMIC;
  g_free(histogram_scale);

  gchar *waveform_type = dt_conf_get_string("plugins/darkroom/histogram/waveform");
  if(g_strcmp0(waveform_type, "overlaid") == 0)
    d->waveform_type = DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID;
  else if(g_strcmp0(waveform_type, "parade") == 0)
    d->waveform_type = DT_LIB_HISTOGRAM_WAVEFORM_PARADE;
  g_free(waveform_type);

  d->histogram = (uint32_t *)calloc(4 * HISTOGRAM_BINS, sizeof(uint32_t));
  d->histogram_max = 0;

  // Waveform buffer doesn't need to be coupled with the histogram
  // widget size. The waveform is almost always scaled when
  // drawn. Choose buffer dimensions which produces workable detail,
  // don't use too much CPU/memory, and allow reasonable gradations
  // of tone.

  // Don't use absurd amounts of memory, exceed width of DT_MIPMAP_F
  // (which will be darktable.mipmap_cache->max_width[DT_MIPMAP_F]*2
  // for mosaiced images), nor make it too slow to calculate
  // (regardless of ppd). Try to get enough detail for a (default)
  // 350px panel, possibly 2x that on hidpi.  The actual buffer
  // width will vary with integral binning of image.
  d->waveform_width = darktable.mipmap_cache->max_width[DT_MIPMAP_F]/2;
  // 175 rows is the default histogram widget height. It's OK if the
  // widget height changes from this, as the width will almost always
  // be scaled. 175 rows is reasonable CPU usage and represents plenty
  // of tonal gradation. 256 would match the # of bins in a regular
  // histogram.
  d->waveform_height = 175;
  d->waveform_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, d->waveform_width);
  d->waveform = calloc(d->waveform_height * d->waveform_stride * 3, sizeof(uint8_t));

  // proxy functions and data so that pixelpipe or tether can
  // provide data for a histogram
  // FIXME: do need to pass self, or can wrap a callback as a lambda
  darktable.lib->proxy.histogram.module = self;
  darktable.lib->proxy.histogram.process = dt_lib_histogram_process;
  darktable.lib->proxy.histogram.is_linear = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR;

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
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  free(d->histogram);
  free(d->waveform);
  dt_pthread_mutex_destroy(&d->lock);

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
