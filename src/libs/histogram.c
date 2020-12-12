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
  float *waveform_linear, *waveform_display;
  uint8_t *waveform_8bit;
  uint32_t waveform_width, waveform_height, waveform_max_width;
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

  const int wf_height = d->waveform_height;
  float *const wf_linear = d->waveform_linear;
  // Use integral sized bins for columns, as otherwise they will be
  // unequal and have banding. Rely on draw to smoothly do horizontal
  // scaling.
  // Note that waveform_stride is pre-initialized/hardcoded,
  // but waveform_width varies, depending on preview image
  // width and # of bins.
  const int bin_width = ceilf(width / (float)d->waveform_max_width);
  const int wf_width = ceilf(width / (float)bin_width);
  d->waveform_width = wf_width;

  memset(wf_linear, 0, sizeof(float) * wf_width * wf_height * 4);

  // Every bin_width x height portion of the image is being described
  // in a 1 pixel x wf_height portion of the histogram.
  const float brightness = wf_height / 40.0f;
  const float scale = brightness / (height * bin_width);

  // 1.0 is at 8/9 of the height!
  const float _height = (float)(wf_height - 1);

  // count the colors
  // note that threads must handle >= bin_width columns to not overwrite each other
  // FIXME: instead outer loop could be by bin
  // FIXME: could flip x/y axes here and when reading to make row-wise iteration?
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(input, width, height, wf_width, bin_width, _height, scale) \
  dt_omp_sharedconst(wf_linear) \
  aligned(input, wf_linear:64) \
  schedule(simd:static, bin_width)
#endif
  for(int x = 0; x < width; x++)
  {
    float *const out = wf_linear + 4 * (x / bin_width);
    for(int y = 0; y < height; y++)
    {
      const float *const in = input + 4 * (y * width + x);
      for(int k = 0; k < 3; k++)
      {
        const float v = 1.0f - (8.0f / 9.0f) * in[2 - k];
        // flipped from dt's CLAMPS so as to treat NaN's as 0 (NaN compares false)
        const int out_y = (v < 1.0f ? (v > 0.0f ? v : 0.0f) : 1.0f) * _height;
        out[4 * wf_width * out_y + k] += scale;
      }
    }
  }

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
    d->waveform_width = 0;
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

static void _draw_color_toggle(cairo_t *cr, float x, float y, float width, float height, gboolean state,
                               const GdkRGBA color)
{
  // FIXME: use dtgtk_cairo_paint_color()
  // FIXME: add "cairo_set_source_rgb_with_alpha()" to make cases such as this less verbose
  cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha/3.0);
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
      const GdkRGBA *const primaries = darktable.bauhaus->graph_primaries;
      cairo_set_source_rgba(cr, primaries[0].red, primaries[0].green, primaries[0].blue, primaries[0].alpha/3.0);
      cairo_rectangle(cr, border, border, width / 3.0, height - 2.0 * border);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, primaries[1].red, primaries[1].green, primaries[1].blue, primaries[1].alpha/3.0);
      cairo_rectangle(cr, width / 3.0, border, width / 3.0, height - 2.0 * border);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, primaries[2].red, primaries[2].green, primaries[2].blue, primaries[2].alpha/3.0);
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

static void _lib_histogram_draw_histogram(dt_lib_histogram_t *d, cairo_t *cr,
                                          int width, int height, const uint8_t mask[3])
{
  if(!d->histogram_max) return;
  const float hist_max = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR ? d->histogram_max
                                                                       : logf(1.0 + d->histogram_max);
  // The alpha of each histogram channel is 1, hence the primaries and
  // overlaid secondaries and neutral colors should be about the same
  // brightness. The combined group is then drawn with an alpha, which
  // dims things down.
  cairo_push_group_with_content(cr, CAIRO_CONTENT_COLOR);
  cairo_translate(cr, 0, height);
  cairo_scale(cr, width / 255.0, -(height - 10) / hist_max);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  for(int k = 0; k < 3; k++)
    if(mask[k])
    {
      set_color(cr, darktable.bauhaus->graph_primaries[k]);
      dt_draw_histogram_8(cr, d->histogram, 4, k, d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR);
    }
  cairo_pop_group_to_source(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_paint_with_alpha(cr, 0.5);
}

static void _lib_histogram_draw_waveform_channel(dt_lib_histogram_t *d, cairo_t *cr, int ch)
{
  const int wf_width = d->waveform_width;
  const int wf_height = d->waveform_height;
  const int wf_stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, wf_width);
  // unused 4th element is for speed
  const float primaries_linear[3][4] = {
    {darktable.bauhaus->graph_primaries[2].blue, darktable.bauhaus->graph_primaries[2].green, darktable.bauhaus->graph_primaries[2].red, 0.0f},
    {darktable.bauhaus->graph_primaries[1].blue, darktable.bauhaus->graph_primaries[1].green, darktable.bauhaus->graph_primaries[1].red, 0.0f},
    {darktable.bauhaus->graph_primaries[0].blue, darktable.bauhaus->graph_primaries[0].green, darktable.bauhaus->graph_primaries[0].red, 0.0f},
  };
  const float *const wf_linear = d->waveform_linear;
  float *const wf_display = d->waveform_display;
  uint8_t *const wf_8bit = d->waveform_8bit;

  // map linear waveform data to a display colorspace
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(wf_width, wf_height, wf_linear, primaries_linear, ch) \
  dt_omp_sharedconst(wf_display) aligned(wf_linear, wf_display, primaries_linear:64) \
  schedule(simd:static)
#endif
  for(int p = 0; p < wf_height * wf_width * 4; p += 4)
  {
    const float src = MIN(1.0f, wf_linear[p + ch]);
    // primaries: colors used to represent primary colors!
    for(int k = 0; k < 3; k++)
      wf_display[p+k] = src * primaries_linear[ch][k];
    wf_display[p+3] = src;
  }
  // this is a shortcut to change the gamma
  const dt_iop_order_iccprofile_info_t *profile_linear =
    dt_ioppr_add_profile_info_to_list(darktable.develop, DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_PERCEPTUAL);
  const dt_iop_order_iccprofile_info_t *profile_work =
    dt_ioppr_add_profile_info_to_list(darktable.develop, DT_COLORSPACE_HLG_REC2020, "", DT_INTENT_PERCEPTUAL);
  // in place transform will preserve alpha
  // dt's transform is approx. 2x faster than LCMS here
  dt_ioppr_transform_image_colorspace_rgb(wf_display, wf_display, wf_width, wf_height,
                                          profile_linear, profile_work, "waveform gamma");

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(wf_display, wf_width, wf_height, wf_stride) \
  dt_omp_sharedconst(wf_8bit) aligned(wf_8bit, wf_display:64) \
  schedule(simd:static) collapse(2)
#endif
  // FIXME: we could do this in place in wf_display, but it'd require care w/OpenMP
  for(int y = 0; y < wf_height; y++)
    for(int x = 0; x < wf_width; x++)
      for(int k = 0; k < 4; k++)
        // linear -> display transform can return pixels > 1, hence limit these
        wf_8bit[(y * wf_stride + x * 4) + k] = MIN(255, (int)(wf_display[4 * (y * wf_width + x) + k] * 255.0f));

  cairo_surface_t *source
    = dt_cairo_image_surface_create_for_data(wf_8bit, CAIRO_FORMAT_ARGB32,
                                             wf_width, wf_height, wf_stride);
  cairo_set_source_surface(cr, source, 0.0, 0.0);
  cairo_paint_with_alpha(cr, 0.5);
  cairo_surface_destroy(source);
}

static void _lib_histogram_draw_waveform(dt_lib_histogram_t *d, cairo_t *cr,
                                         int width, int height,
                                         const uint8_t mask[3])
{
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_scale(cr, darktable.gui->ppd*width/d->waveform_width,
              darktable.gui->ppd*height/d->waveform_height);

  for(int ch = 0; ch < 3; ch++)
    if(mask[2-ch])
      _lib_histogram_draw_waveform_channel(d, cr, ch);
  cairo_restore(cr);
}

static void _lib_histogram_draw_rgb_parade(dt_lib_histogram_t *d, cairo_t *cr,
                                           int width, int height, const uint8_t mask[3])
{
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_scale(cr, darktable.gui->ppd*width/(d->waveform_width*3),
              darktable.gui->ppd*height/d->waveform_height);
  for(int ch = 2; ch >= 0; ch--)
  {
    if(mask[2-ch])
      _lib_histogram_draw_waveform_channel(d, cr, ch);
    cairo_translate(cr, d->waveform_width/darktable.gui->ppd, 0);
  }
  cairo_restore(cr);
}

static gboolean _lib_histogram_draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_times_t start_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

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
    set_color(cr, darktable.bauhaus->graph_overlay);
    if(d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM)
      cairo_rectangle(cr, 0, 7.0/9.0 * height, width, height);
    else
      cairo_rectangle(cr, 0, 0, 0.2 * width, height);
    cairo_fill(cr);
  }
  else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
  {
    set_color(cr, darktable.bauhaus->graph_overlay);
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

  // FIXME: should set histogram buffer to black if have just entered tether view and nothing is displayed
  dt_pthread_mutex_lock(&d->lock);
  // darkroom view: draw scope so long as preview pipe is finished
  // tether view: draw whatever has come in from tether
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_TETHERING || dev->image_storage.id == dev->preview_pipe->output_imgid)
  {
    const uint8_t mask[3] = { d->red, d->green, d->blue };
    switch(d->scope_type)
    {
      case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
        _lib_histogram_draw_histogram(d, cr, width, height, mask);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
        if(!d->waveform_width) break;
        if(d->waveform_type == DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID)
          _lib_histogram_draw_waveform(d, cr, width, height, mask);
        else
          _lib_histogram_draw_rgb_parade(d, cr, width, height, mask);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_N:
        g_assert_not_reached();
    }
  }
  dt_pthread_mutex_unlock(&d->lock);

  // buttons to control the display of the histogram: linear/log, r, g, b
  if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_OUTSIDE_WIDGET)
  {
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
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
    _draw_color_toggle(cr, d->red_x, d->button_y, d->button_w, d->button_h, d->red, darktable.bauhaus->graph_primaries[0]);
    _draw_color_toggle(cr, d->green_x, d->button_y, d->button_w, d->button_h, d->green, darktable.bauhaus->graph_primaries[1]);
    _draw_color_toggle(cr, d->blue_x, d->button_y, d->button_w, d->button_h, d->blue, darktable.bauhaus->graph_primaries[2]);
  }

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end_time = { 0 };
    dt_get_times(&end_time);
    fprintf(stderr, "scope draw took %.3f secs (%.3f CPU)\n", end_time.clock - start_time.clock, end_time.user - start_time.user);
  }

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
  // FIXME: increasing waveform_max_width increases processing speed less than increasing waveform_height -- tune these better?
  d->waveform_max_width = darktable.mipmap_cache->max_width[DT_MIPMAP_F]/2;
  // initially no waveform to draw
  d->waveform_width = 0;
  // 175 rows is the default histogram widget height. It's OK if the
  // widget height changes from this, as the width will almost always
  // be scaled. 175 rows is reasonable CPU usage and represents plenty
  // of tonal gradation. 256 would match the # of bins in a regular
  // histogram.
  d->waveform_height = 175;
  d->waveform_linear = dt_alloc_align(64, sizeof(float) * d->waveform_height * d->waveform_max_width * 4);
  d->waveform_display = dt_alloc_align(64, sizeof(float) * d->waveform_height * d->waveform_max_width * 4);
  d->waveform_8bit = dt_alloc_align(64, sizeof(uint8_t) * d->waveform_height * d->waveform_max_width * 4);

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
  dt_free_align(d->waveform_linear);
  dt_free_align(d->waveform_display);
  dt_free_align(d->waveform_8bit);
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
