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
#include "common/imagebuf.h"
#include "common/image_cache.h"
#include "common/math.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
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
  DT_LIB_HISTOGRAM_HIGHLIGHT_NONE = 0,
  DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT,
  DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE
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
  int waveform_width, waveform_height, waveform_max_width;
  dt_pthread_mutex_t lock;
  // drawable with scope image
  GtkWidget *scope_draw;
  // contains scope control buttons
  GtkWidget *button_box;
  // buttons which change between histogram and waveform scopes
  GtkWidget *mode_stack;
  // drag to change parameters
  gboolean dragging;
  int32_t button_down_x, button_down_y;
  float button_down_value;
  // depends on mouse positon
  dt_lib_histogram_highlight_t highlight;
  // state set by buttons
  dt_lib_histogram_scope_type_t scope_type;
  dt_lib_histogram_scale_t histogram_scale;
  dt_lib_histogram_waveform_type_t waveform_type;
  gboolean red, green, blue;
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

  // Note that, with current constants, the input buffer is from the
  // preview pixelpipe and should be <= 1440x900x4. The output buffer
  // will be <= 360x175x4. Hence process works with a relatively small
  // quantity of data.
  const int wf_height = d->waveform_height;
  const float *const restrict in = DT_IS_ALIGNED((const float *const restrict)input);
  float *const restrict wf_linear = DT_IS_ALIGNED((float *const restrict)d->waveform_linear);

  // Use integral sized bins for columns, as otherwise they will be
  // unequal and have banding. Rely on draw to smoothly do horizontal
  // scaling. For a 3:2 image, "landscape" orientation, bin_width will
  // generally be 4, for "portrait" it will generally be 3.
  // Note that waveform_stride is pre-initialized/hardcoded,
  // but waveform_width varies, depending on preview image
  // width and # of bins.
  const size_t bin_width = ceilf(width / (float)d->waveform_max_width);
  const size_t wf_width = ceilf(width / (float)bin_width);
  d->waveform_width = wf_width;

  dt_iop_image_fill(wf_linear, 0.0f, wf_width, wf_height, 4);

  // Every bin_width x height portion of the image is being described
  // in a 1 pixel x wf_height portion of the histogram.
  // NOTE: if constant is decreased, will brighten output
  const float brightness = wf_height / 40.0f;
  const float scale = brightness / (height * bin_width);

  // 1.0 is at 8/9 of the height!
  const size_t height_i = wf_height-1;
  const float height_f = height_i;

  const size_t in_stride = 4U * width;
  const size_t out_stride = 4U * wf_width;

  // count the colors
  // FIXME: could flip x/y axes here and when reading to make row-wise iteration?
#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, wf_linear, width, height, wf_width, bin_width, in_stride, out_stride, height_f, height_i, scale) \
  schedule(static)
#endif
  for(size_t out_x = 0; out_x < wf_width; out_x++)
  {
    const size_t x_from = out_x * bin_width;
    const size_t x_high = MIN(x_from + bin_width, width);
    for(size_t in_x = x_from; in_x < x_high; in_x++)
    {
      for(size_t in_y = 0; in_y < height; in_y++)
      {
        // While it would be nice to use for_each_channel(), making
        // the BGR/RGB flip doesn't allow for this. Regardless, the
        // fourth channel will be ignored when waveform is drawn.
        for(size_t k = 0; k < 3; k++)
        {
          const float v = 1.0f - (8.0f / 9.0f) * in[in_stride * in_y + 4U * in_x + (2U - k)];
          const size_t out_y = isnan(v) ? 0 : MIN((size_t)fmaxf(v*height_f, 0.0f), height_i);
          wf_linear[out_stride * out_y + 4U * out_x + k] += scale;
        }
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
    const dt_iop_order_iccprofile_info_t *const profile_info_from
      = dt_ioppr_add_profile_info_to_list(dev, in_profile_type, in_profile_filename, INTENT_PERCEPTUAL);

    dt_colorspaces_color_profile_type_t out_profile_type;
    const char *out_profile_filename;
    dt_ioppr_get_histogram_profile_type(&out_profile_type, &out_profile_filename);
    if(out_profile_type != DT_COLORSPACE_NONE)
    {
      const dt_iop_order_iccprofile_info_t *const profile_info_to =
        dt_ioppr_add_profile_info_to_list(dev, out_profile_type, out_profile_filename, DT_INTENT_RELATIVE_COLORIMETRIC);
      img_display = dt_alloc_align_float((size_t)4 * width * height);
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
  // map linear waveform data to a display colorspace
  const float *const restrict wf_linear = DT_IS_ALIGNED((const float *const restrict)d->waveform_linear);
  float *const restrict wf_display = DT_IS_ALIGNED((float *const restrict)d->waveform_display);
  const int wf_width = d->waveform_width;
  const int wf_height = d->waveform_height;
  // colors used to represent primary colors
  const GdkRGBA *const css_primaries = darktable.bauhaus->graph_primaries;
  const float DT_ALIGNED_ARRAY primaries_linear[3][4] = {
    {css_primaries[2].blue, css_primaries[2].green, css_primaries[2].red, 1.0f},
    {css_primaries[1].blue, css_primaries[1].green, css_primaries[1].red, 1.0f},
    {css_primaries[0].blue, css_primaries[0].green, css_primaries[0].red, 1.0f},
  };
  const size_t nfloats = 4U * wf_width * wf_height;
  // this should be <= 250K iterations, hence not worth the overhead to thread
  for(size_t p = 0; p < nfloats; p += 4)
  {
    const float src = MIN(1.0f, wf_linear[p + ch]);
    for_four_channels(k,aligned(wf_display,primaries_linear:64))
    {
      wf_display[p+k] = src * primaries_linear[ch][k];
    }
  }

  // shortcut for a fast gamma change
  const dt_iop_order_iccprofile_info_t *profile_linear =
    dt_ioppr_add_profile_info_to_list(darktable.develop, DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_PERCEPTUAL);
  const dt_iop_order_iccprofile_info_t *profile_work =
    dt_ioppr_add_profile_info_to_list(darktable.develop, DT_COLORSPACE_HLG_REC2020, "", DT_INTENT_PERCEPTUAL);
  // in place transform will preserve alpha
  // dt's transform is approx. 2x faster than LCMS here
  dt_ioppr_transform_image_colorspace_rgb(wf_display, wf_display, wf_width, wf_height,
                                          profile_linear, profile_work, "waveform gamma");

  const size_t wf_width_floats = 4U * wf_width;
  uint8_t *const restrict wf_8bit = DT_IS_ALIGNED((uint8_t *const restrict)d->waveform_8bit);
  const size_t wf_8bit_stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, wf_width);
  // not enough iterations to be worth threading
  for(size_t y = 0; y < wf_height; y++)
  {
#ifdef _OPENMP
#pragma simd aligned(wf_display, wf_8bit : 64)
#endif
    for(size_t k = 0; k < wf_width_floats; k++)
    {
      // linear -> display transform can return pixels > 1, hence limit these
      wf_8bit[y * wf_8bit_stride + k] = MIN(255, (int)(wf_display[y * wf_width_floats + k] * 255.0f));
    }
  }

  cairo_surface_t *source
    = dt_cairo_image_surface_create_for_data(wf_8bit, CAIRO_FORMAT_ARGB32,
                                             wf_width, wf_height, wf_8bit_stride);
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

static gboolean _drawable_draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_times_t start_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
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

static gboolean _drawable_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event,
                                                      gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
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
    // FIXME: should limit exposure iop changes as bauhaus sliders do, for smoother interaction
    if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
    {
      const float exposure = d->button_down_value + diff * 4.0f / (float)range;
      dt_dev_exposure_set_exposure(dev, exposure);
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
    {
      const float black = d->button_down_value - diff * .1f / (float)range;
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

    // FIXME: make just one tooltip for the widget depending on whether it is draggable or not, and set it when enter the view
    if(!hooks_available)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
      gtk_widget_set_tooltip_text(widget, _("ctrl+scroll to change display height"));
    }
    // FIXME: could a GtkRange be used to do this work?
    else if((posx < 0.2f && d->scope_type == DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM) ||
            (posy > 7.0f/9.0f && d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM))
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
      dt_control_queue_redraw_widget(widget);
    }
  }
  // FIXME: is this code obsolete?
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

static gboolean _drawable_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  dt_develop_t *dev = darktable.develop;

  if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
  {
    if(event->type == GDK_2BUTTON_PRESS)
    {
      dt_dev_exposure_reset_defaults(dev);
    }
    else
    {
      d->dragging = TRUE;
      if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
        d->button_down_value = dt_dev_exposure_get_exposure(dev);
      else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
        d->button_down_value = dt_dev_exposure_get_black(dev);
      d->button_down_x = event->x;
      d->button_down_y = event->y;
    }
  }

  return TRUE;
}

static void _scope_type_toggle(GtkToggleButton *button, dt_lib_histogram_t *d)
{
  // FIXME: make this a combobox -- simplifies this code and reflects what it really is doing, and adds room for other scopes (e.g. vectorscope)
  d->scope_type = gtk_toggle_button_get_active(button) ? DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM : DT_LIB_HISTOGRAM_SCOPE_WAVEFORM;
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      gtk_widget_set_tooltip_text(GTK_WIDGET(button), _("set mode to waveform"));
      dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(button),
                                   dtgtk_cairo_paint_histogram_scope, CPF_NONE, NULL);
      gtk_stack_set_visible_child_name(GTK_STACK(d->mode_stack), "histogram");
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      gtk_widget_set_tooltip_text(GTK_WIDGET(button), _("set mode to histogram"));
      dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(button),
                                   dtgtk_cairo_paint_waveform_scope, CPF_NONE, NULL);
      gtk_stack_set_visible_child_name(GTK_STACK(d->mode_stack), "waveform");
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      g_assert_not_reached();
  }
  dt_conf_set_string("plugins/darkroom/histogram/mode",
                     dt_lib_histogram_scope_type_names[d->scope_type]);

  // generate data for changed scope and trigger widget redraw
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv)  // this may be called on init, before in a view
  {
    const dt_view_type_flags_t view_type = cv->view(cv);
    if(view_type == DT_VIEW_DARKROOM)
      dt_dev_process_preview(darktable.develop);
    else
      dt_control_queue_redraw_center();
  }
}

static void _histogram_scale_toggle(GtkToggleButton *button, dt_lib_histogram_t *d)
{
  // FIXME: make this a combobox -- simplifies this code and reflects what it really is doing
  d->histogram_scale = gtk_toggle_button_get_active(button) ? DT_LIB_HISTOGRAM_LOGARITHMIC : DT_LIB_HISTOGRAM_LINEAR;
  switch(d->histogram_scale)
  {
    case DT_LIB_HISTOGRAM_LOGARITHMIC:
      gtk_widget_set_tooltip_text(GTK_WIDGET(button), _("set scale to linear"));
      dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(button),
                                   dtgtk_cairo_paint_logarithmic_scale, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_LINEAR:
      gtk_widget_set_tooltip_text(GTK_WIDGET(button), _("set scale to logarithmic"));
      dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(button),
                                   dtgtk_cairo_paint_linear_scale, CPF_NONE, NULL);
      break;
    default:
      g_assert_not_reached();
  }
  dt_conf_set_string("plugins/darkroom/histogram/histogram",
                     dt_lib_histogram_histogram_scale_names[d->histogram_scale]);
  // FIXME: this should really redraw current iop if its background is a histogram (check request_histogram)
  darktable.lib->proxy.histogram.is_linear = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR;
  dt_control_queue_redraw_widget(d->scope_draw);
}

static void _waveform_type_toggle(GtkToggleButton *button, dt_lib_histogram_t *d)
{
  // FIXME: make this a combobox -- simplifies this code and reflects what it really is doing
  // FIXME: add other waveform types -- RGB parade overlaid top-to-bottom rather than left to right, possibly waveform calculated sideways (another button?)
  d->waveform_type = gtk_toggle_button_get_active(button) ? DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID : DT_LIB_HISTOGRAM_WAVEFORM_PARADE;
  switch(d->waveform_type)
  {
    case DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID:
      gtk_widget_set_tooltip_text(GTK_WIDGET(button), _("set mode to RGB parade"));
      dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(button),
                                   dtgtk_cairo_paint_waveform_overlaid, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_WAVEFORM_PARADE:
      gtk_widget_set_tooltip_text(GTK_WIDGET(button), _("set mode to waveform"));
      dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(button),
                                   dtgtk_cairo_paint_rgb_parade, CPF_NONE, NULL);
      // FIXME: in RGB parade mode, hide the buttons to select channel, which are fairly senseless in this context
      break;
    default:
      g_assert_not_reached();
  }
  dt_conf_set_string("plugins/darkroom/histogram/waveform",
                     dt_lib_histogram_waveform_type_names[d->waveform_type]);
  dt_control_queue_redraw_widget(d->scope_draw);
}

static void _red_channel_toggle(GtkToggleButton *button, dt_lib_histogram_t *d)
{
  d->red = gtk_toggle_button_get_active(button);
  gtk_widget_set_tooltip_text(GTK_WIDGET(button), d->red ? _("click to hide red channel") : _("click to show red channel"));
  dt_conf_set_bool("plugins/darkroom/histogram/show_red", d->red);
  dt_control_queue_redraw_widget(d->scope_draw);
}

static void _green_channel_toggle(GtkToggleButton *button, dt_lib_histogram_t *d)
{
  d->green = gtk_toggle_button_get_active(button);
  gtk_widget_set_tooltip_text(GTK_WIDGET(button), d->green ? _("click to hide green channel") : _("click to show green channel"));
  dt_conf_set_bool("plugins/darkroom/histogram/show_green", d->green);
  dt_control_queue_redraw_widget(d->scope_draw);
}

static void _blue_channel_toggle(GtkToggleButton *button, dt_lib_histogram_t *d)
{
  d->blue = gtk_toggle_button_get_active(button);
  gtk_widget_set_tooltip_text(GTK_WIDGET(button), d->blue ? _("click to hide blue channel") : _("click to show blue channel"));
  dt_conf_set_bool("plugins/darkroom/histogram/show_blue", d->blue);
  dt_control_queue_redraw_widget(d->scope_draw);
}

static gboolean _lib_histogram_scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  int delta_y;
  // note are using unit rather than smooth scroll events, as
  // exposure changes can get laggy if handling a multitude of smooth
  // scroll events
  // FIXME: should limit exposure iop changes as bauhaus sliders do, for smoother interaction?
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
      if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
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

static gboolean _drawable_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                  gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  d->dragging = FALSE;
  return TRUE;
}

static gboolean _drawable_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                gpointer user_data)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if((cv->view(cv) == DT_VIEW_DARKROOM) && dt_dev_exposure_hooks_available(darktable.develop))
  {
    // FIXME: should really use named cursors, and differentiate between "grab" and "grabbing"
    dt_control_change_cursor(GDK_HAND1);
  }
  return TRUE;
}

static gboolean _drawable_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  // if dragging, gtk keeps up motion notifications until mouse button
  // is released, at which point we'll get another leave event for
  // drawable if pointer is still outside of the widget
  if(!d->dragging)
  {
    d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
    dt_control_change_cursor(GDK_LEFT_PTR);
    dt_control_queue_redraw_widget(widget);
  }
  return TRUE;
}

static gboolean _eventbox_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                gpointer user_data)
{
  GtkWidget *button_box = (GtkWidget *)user_data;
  gtk_widget_show(button_box);
  return TRUE;
}

static gboolean _eventbox_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                gpointer user_data)
{
  // note that if we are dragging the drawable and cursor goes outside
  // of eventbox then the button is released, the eventbox doesn't
  // receive a leave event until the button is released
  GtkWidget *button_box = (GtkWidget *)user_data;
  gtk_widget_hide(button_box);
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

  // FIXME: call _scope_type_toggle() and _waveform_type_toggle() and _histogram_scale_toggle() to handle all these
  dt_lib_histogram_scope_type_t old_scope = d->scope_type;
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      d->histogram_scale = (d->histogram_scale + 1);
      if(d->histogram_scale == DT_LIB_HISTOGRAM_N)
      {
        d->histogram_scale = DT_LIB_HISTOGRAM_LOGARITHMIC;
        d->waveform_type = DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID;
        // FIXME: if are dragging, cancel dragging, as the draggable areas will have changed
        d->scope_type = DT_LIB_HISTOGRAM_SCOPE_WAVEFORM;
      }
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      d->waveform_type = (d->waveform_type + 1);
      if(d->waveform_type == DT_LIB_HISTOGRAM_WAVEFORM_N)
      {
        d->histogram_scale = DT_LIB_HISTOGRAM_LOGARITHMIC;
        d->waveform_type = DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID;
        // FIXME: if are dragging, cancel dragging, as the draggable areas will have changed
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
    // FIXME: properly update drawable -- do we need this?
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
  // FIXME: call _scope_type_toggle() to handle all these
  // FIXME: if are dragging, cancel dragging, as the draggable areas will have changed
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

  // FIXME: call _waveform_type_toggle() and _histogram_scale_toggle() to handle all these
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
  // FIXME: update just the drawable
  dt_control_queue_redraw_widget(self->widget);
  return TRUE;
}

// this is only called in darkroom view
static void _lib_histogram_preview_updated_callback(gpointer instance, dt_lib_module_t *self)
{
  // preview pipe has already given process() the high quality
  // pre-gamma image. Now that preview pipe is complete, draw it
  // FIXME: it would be nice if process() just queued a redraw if not in live view, but then our draw code would have to have some other way to assure that the histogram image is current besides checking the pixelpipe to see if it has processed the current image
  // FIXME: can just pass in scope_draw?
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_control_queue_redraw_widget(d->scope_draw);
}

void view_enter(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  if(new_view->view(new_view) == DT_VIEW_DARKROOM)
  {
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                              G_CALLBACK(_lib_histogram_preview_updated_callback), self);
  }
  // hack: setting these on gui_init doesn't work
  gtk_stack_set_visible_child_name(GTK_STACK(d->mode_stack),
                                   d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM ? "waveform" : "histogram");
  // button box should be hidden when enter view, unless mouse is over
  // histogram, in which case gtk kindly generates enter events
  gtk_widget_hide(d->button_box);

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
  d->waveform_height  = 175;
  d->waveform_linear  = dt_iop_image_alloc(d->waveform_max_width, d->waveform_height, 4);
  d->waveform_display = dt_iop_image_alloc(d->waveform_max_width, d->waveform_height, 4);
  d->waveform_8bit    = dt_alloc_align(64, sizeof(uint8_t) * 4 * d->waveform_height * d->waveform_max_width);

  // proxy functions and data so that pixelpipe or tether can
  // provide data for a histogram
  // FIXME: do need to pass self, or can wrap a callback as a lambda
  darktable.lib->proxy.histogram.module = self;
  darktable.lib->proxy.histogram.process = dt_lib_histogram_process;
  darktable.lib->proxy.histogram.is_linear = d->histogram_scale == DT_LIB_HISTOGRAM_LINEAR;

  // create widgets
  GtkWidget *overlay = gtk_overlay_new();

  /* create drawingarea */
  d->scope_draw = gtk_drawing_area_new();
  // FIXME: be consistent about using hyphens or underscores
  gtk_widget_set_name(d->scope_draw, "scope-draw");
  gtk_widget_set_tooltip_text(d->scope_draw, _("drag to change exposure,\ndoubleclick resets\nctrl+scroll to change display height"));
  gtk_container_add(GTK_CONTAINER(overlay), d->scope_draw);

  // a row of buttons
  // FIXME: button box margins "obscure" events to drawable below -- place button box in another widget which provides these margins and doesn't catch events, or is it good that entire top-right of histogram is controls and there aren't thin ribbons of drawable receiving events?
  d->button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(d->button_box, "button_box");
  gtk_widget_set_valign(d->button_box, GTK_ALIGN_START);
  gtk_widget_set_halign(d->button_box, GTK_ALIGN_END);
  // GtkButtonBox spreads out the icons, so we use GtkBox --
  // homogeneous shouldn't be necessary as icons are equal size, but
  // set it just to show the desired look
  // FIXME: can set GtkButtonBox to GTK_ALIGN_END to get rid of that behavior, then skip gtk_box_set_homogeneous()?
  gtk_box_set_homogeneous(GTK_BOX(d->button_box), TRUE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), d->button_box);

  // FIXME: should histogram/waveform each be its own widget, and a GtkStack to switch between them? -- if so, then the each of the histogram/waveform widgets will have its own associated "mode" button, drawable, and sensitive areas for scrolling, but the channel buttons will be shared between them, or will modify the same underlying data

  // FIXME: make keyboard accelerators work for these
  // FIXME: put the scope widget in libs/tools/scope.c?

  // note that are calling toggle callback by hand for each button as
  // a hack to finish init -- if we don't flip the tooltip text (and
  // icon) on toggle, this will be unnecessary

  // FIXME: can style these buttons as flat -- at least the non-channel ones -- to be closer to darktable defaults?
  // scope type
  // FIXME: this should really be a combobox to allow for more types and not to have to swap the icon on button down
  GtkWidget *scope_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_histogram_scope, CPF_NONE, NULL);
  gtk_widget_set_name(scope_button, "scope_type_button");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(scope_button), d->scope_type == DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM);
  gtk_box_pack_start(GTK_BOX(d->button_box), scope_button, FALSE, FALSE, 0);

  // scope mode
  GtkWidget *button;
  d->mode_stack = gtk_stack_new();
  gtk_widget_set_name(d->mode_stack, "scope_mode_stack");

  // histogram scale
  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_logarithmic_scale, CPF_NONE, NULL);
  gtk_widget_set_name(button, "histogram_scale_button");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), d->histogram_scale == DT_LIB_HISTOGRAM_LOGARITHMIC);
  _histogram_scale_toggle(GTK_TOGGLE_BUTTON(button), d);
  g_signal_connect(G_OBJECT(button), "toggled", G_CALLBACK(_histogram_scale_toggle), d);
  gtk_stack_add_named(GTK_STACK(d->mode_stack), button, "histogram");

  // histogram scale
  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_waveform_overlaid, CPF_NONE, NULL);
  gtk_widget_set_name(button, "waveform_type_button");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), d->waveform_type == DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID);
  _waveform_type_toggle(GTK_TOGGLE_BUTTON(button), d);
  g_signal_connect(G_OBJECT(button), "toggled", G_CALLBACK(_waveform_type_toggle), d);
  gtk_stack_add_named(GTK_STACK(d->mode_stack), button, "waveform");

  _scope_type_toggle(GTK_TOGGLE_BUTTON(scope_button), d);
  g_signal_connect(G_OBJECT(scope_button), "toggled", G_CALLBACK(_scope_type_toggle), d);
  gtk_box_pack_start(GTK_BOX(d->button_box), d->mode_stack, FALSE, FALSE, 0);

  // red channel on/off
  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_NONE, NULL);
  // FIXME: better to have a general tooltip rather than flipping it when the button is pressed
  gtk_widget_set_name(button, "red_channel_button");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), d->red);
  _red_channel_toggle(GTK_TOGGLE_BUTTON(button), d);
  g_signal_connect(G_OBJECT(button), "toggled", G_CALLBACK(_red_channel_toggle), d);
  gtk_box_pack_start(GTK_BOX(d->button_box), button, FALSE, FALSE, 0);

  // green channel on/off
  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_NONE, NULL);
  gtk_widget_set_name(button, "green_channel_button");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), d->green);
  _green_channel_toggle(GTK_TOGGLE_BUTTON(button), d);
  g_signal_connect(G_OBJECT(button), "toggled", G_CALLBACK(_green_channel_toggle), d);
  gtk_box_pack_start(GTK_BOX(d->button_box), button, FALSE, FALSE, 0);

  // blue channel on/off
  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_NONE, NULL);
  gtk_widget_set_name(button, "blue_channel_button");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), d->blue);
  _blue_channel_toggle(GTK_TOGGLE_BUTTON(button), d);
  g_signal_connect(G_OBJECT(button), "toggled", G_CALLBACK(_blue_channel_toggle), d);
  gtk_box_pack_start(GTK_BOX(d->button_box), button, FALSE, FALSE, 0);

  // The main widget is an overlay which has no window, and hence
  // can't catch events. We need something on top to catch events to
  // show/hide the buttons. The drawable is below the buttons, and
  // hence won't catch motion events for the buttons, and gets a leave
  // event when the cursor moves over the buttons.
  // FIXME: solve this by making the button box the size of the drawable, but have it not catch any events except enter/leave?
  GtkWidget *eventbox = gtk_event_box_new();
  // FIXME: should eventbox only contain the buttonbox, if its only job is to show the buttonbox?
  // FIXME: can just make buttonbox the size of the widget with a CSS hover property to make it disappear (not opaque, as it would still be sensitive) when mouse is over it?
  gtk_container_add(GTK_CONTAINER(eventbox), overlay);

  gtk_widget_add_events(eventbox, GDK_LEAVE_NOTIFY_MASK | GDK_ENTER_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(eventbox), "enter-notify-event",
                   G_CALLBACK(_eventbox_enter_notify_callback), d->button_box);
  g_signal_connect(G_OBJECT(eventbox), "leave-notify-event",
                   G_CALLBACK(_eventbox_leave_notify_callback), d->button_box);

  // FIXME: these events become less important if are using widgets on top of this
  // FIXME: GDK_POINTER_MOTION_MASK is deprecated, see https://developer.gnome.org/gdk3/stable/gdk3-Events.html#GdkEventMask
  gtk_widget_add_events(d->scope_draw, GDK_LEAVE_NOTIFY_MASK | GDK_ENTER_NOTIFY_MASK |
                                       GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
                                       GDK_BUTTON_RELEASE_MASK | darktable.gui->scroll_mask);

  /* connect callbacks */
  g_signal_connect(G_OBJECT(d->scope_draw), "draw", G_CALLBACK(_drawable_draw_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "enter-notify-event",
                   G_CALLBACK(_drawable_enter_notify_callback), NULL);
  g_signal_connect(G_OBJECT(d->scope_draw), "leave-notify-event",
                   G_CALLBACK(_drawable_leave_notify_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "button-press-event",
                   G_CALLBACK(_drawable_button_press_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "button-release-event",
                   G_CALLBACK(_drawable_button_release_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "motion-notify-event",
                   G_CALLBACK(_drawable_motion_notify_callback), d);
  // FIXME: connect the scroll-to-resize behavior to the main widget?
  g_signal_connect(G_OBJECT(d->scope_draw), "scroll-event",
                   G_CALLBACK(_lib_histogram_scroll_callback), self);

  // FIXME: do we even need to save self->widget? most references are to the drawable...
  // FIXME: how does reference counting of widgets work? do we need to dealloc or garbage collect them?
  self->widget = eventbox;
  gtk_widget_set_name(self->widget, "main-histogram");
  // FIXME: is this the right widget to have the help link?
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

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
