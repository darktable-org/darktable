/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

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
#include "common/atomic.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/histogram.h"
#include "common/iop_profile.h"
#include "common/imagebuf.h"
#include "common/image_cache.h"
#include "common/math.h"
#include "common/color_picker.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/paint.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "libs/colorpicker.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#define HISTOGRAM_BINS 256
// # of gradations between each primary/secondary to draw the hue ring
// this is tuned to most degenerate cases: curve to blue primary in
// Luv in linear ProPhoto RGB and the widely spaced gradations of the
// PQ P3 RGB colorspace. This could be lowered to 32 with little
// visible consequence.

DT_MODULE(1)

typedef enum dt_lib_histogram_highlight_t
{
  DT_LIB_HISTOGRAM_HIGHLIGHT_NONE = 0,
  DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT,
  DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE
} dt_lib_histogram_highlight_t;

typedef enum dt_lib_histogram_scope_type_t
{
  DT_LIB_HISTOGRAM_SCOPE_WAVEFORM = 0,
  DT_LIB_HISTOGRAM_SCOPE_PARADE,
  DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM,
  DT_LIB_HISTOGRAM_SCOPE_N // needs to be the last one
} dt_lib_histogram_scope_type_t;

typedef enum dt_lib_histogram_scale_t
{
  DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC = 0,
  DT_LIB_HISTOGRAM_SCALE_LINEAR,
  DT_LIB_HISTOGRAM_SCALE_N // needs to be the last one
} dt_lib_histogram_scale_t;

typedef enum dt_lib_histogram_orient_t
{
  DT_LIB_HISTOGRAM_ORIENT_HORI = 0,
  DT_LIB_HISTOGRAM_ORIENT_VERT,
  DT_LIB_HISTOGRAM_ORIENT_N // needs to be the last one
} dt_lib_histogram_orient_t;

// FIXME: are these lists available from the enum/options in darktableconfig.xml?
const gchar *dt_lib_histogram_scope_type_names[DT_LIB_HISTOGRAM_SCOPE_N] =
{ N_("waveform"),
  N_("RGB parade"),
  N_("histogram")
};

const gchar *dt_lib_histogram_scale_names[DT_LIB_HISTOGRAM_SCALE_N] =
  { "logarithmic",
    "linear"
  };

const gchar *dt_lib_histogram_orient_names[DT_LIB_HISTOGRAM_ORIENT_N] =
  { "horizontal",
    "vertical"
  };

const void *dt_lib_histogram_scope_type_icons[DT_LIB_HISTOGRAM_SCOPE_N] =
  { dtgtk_cairo_paint_waveform_scope,
    dtgtk_cairo_paint_rgb_parade,
    dtgtk_cairo_paint_histogram_scope };

typedef struct dt_lib_histogram_t
{
  // histogram for display
  uint32_t *histogram;
  uint32_t histogram_max;
  // waveform buffers and dimensions
  uint8_t *waveform_img[3];           // image per RGB channel
  int waveform_bins, waveform_tones, waveform_max_bins;
  int selected_sample;                // position of the selected live sample in the list
  dt_pthread_mutex_t lock;
  GtkWidget *scope_draw;               // GtkDrawingArea -- scope, scale, and draggable overlays
  GtkWidget *button_box_main;          // GtkBox -- contains scope control buttons
  GtkWidget *button_box_opt;           // GtkBox -- contains options buttons
  GtkWidget *button_box_rgb;           // GtkBox -- contains RGB channels buttons
  GtkWidget *scope_type_button
    [DT_LIB_HISTOGRAM_SCOPE_N];        // Array of GtkToggleButton -- histogram control
  GtkWidget *scope_view_button;        // GtkButton -- how to render the current scope
  GtkWidget *red_channel_button;       // GtkToggleButton -- enable/disable processing R channel
  GtkWidget *green_channel_button;     // GtkToggleButton -- enable/disable processing G channel
  GtkWidget *blue_channel_button;      // GtkToggleButton -- enable/disable processing B channel
  // depends on mouse position
  dt_lib_histogram_highlight_t highlight;
  // state set by buttons
  dt_lib_histogram_scope_type_t scope_type;
  dt_lib_histogram_scale_t histogram_scale;
  dt_lib_histogram_orient_t scope_orient;
  gboolean red, green, blue;
} dt_lib_histogram_t;

const char *name(dt_lib_module_t *self)
{
  return _("scopes");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_DARKROOM | DT_VIEW_TETHERING;
}

uint32_t container(dt_lib_module_t *self)
{
  return g_strcmp0
    (dt_conf_get_string_const("plugins/darkroom/histogram/panel_position"), "right")
    ? DT_UI_CONTAINER_PANEL_LEFT_TOP
    : DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 1001;
}


static void _lib_histogram_process_histogram(dt_lib_histogram_t *const d,
                                             const float *const input,
                                             const dt_histogram_roi_t *const roi)
{
  dt_dev_histogram_collection_params_t histogram_params = { 0 };
  const dt_iop_colorspace_type_t cst = IOP_CS_RGB;
  dt_dev_histogram_stats_t histogram_stats =
    { .bins_count = HISTOGRAM_BINS,
      .ch = 4,
      .pixels = 0,
      .buf_size = sizeof(uint32_t) * 4 * HISTOGRAM_BINS };
  uint32_t histogram_max[4] = { 0 };

  d->histogram_max = 0;
  memset(d->histogram, 0, sizeof(uint32_t) * 4 * HISTOGRAM_BINS);

  histogram_params.roi = roi;
  histogram_params.bins_count = HISTOGRAM_BINS;

  // FIXME: for point sample, calculate whole graph and the point
  // sample values, draw these on top of the graph

  // FIXME: set up "custom" histogram worker which can do colorspace
  // conversion on fly -- in cases that we need to do that -- may need
  // to add from colorspace to dt_dev_histogram_collection_params_t
  dt_histogram_helper(&histogram_params, &histogram_stats, cst, IOP_CS_NONE,
                      input, &d->histogram, histogram_max, FALSE, NULL);
  d->histogram_max = MAX(MAX(histogram_max[0], histogram_max[1]), histogram_max[2]);
}

static void _lib_histogram_process_waveform(dt_lib_histogram_t *const d,
                                            const float *const input,
                                            const dt_histogram_roi_t *const roi)
{
  // FIXME: for point sample, calculate whole graph and the point
  // sample values, draw these on top of a dimmer graph
  const int sample_width = MAX(1, roi->width - roi->crop_right - roi->crop_x);
  const int sample_height = MAX(1, roi->height - roi->crop_bottom - roi->crop_y);

  // Use integral sized bins for columns, as otherwise they will be
  // unequal and have banding. Rely on draw to smoothly do horizontal
  // scaling. For a horizontal waveform of a 3:2 image, "landscape"
  // orientation, bin_width will generally be 4, for "portrait" it
  // will generally be 3. Note that waveform_bins varies, depending on
  // preview image width and # of bins.
  const dt_lib_histogram_orient_t orient = d->scope_orient;
  const int to_bin = orient == DT_LIB_HISTOGRAM_ORIENT_HORI ? sample_width : sample_height;
  const size_t samples_per_bin = ceilf(to_bin / (float)d->waveform_max_bins);
  const size_t num_bins = ceilf(to_bin / (float)samples_per_bin);
  d->waveform_bins = num_bins;
  const size_t num_tones = d->waveform_tones;

  // Note that, with current constants, the input buffer is from the
  // preview pixelpipe and should be <= 1440x900x4. The output buffer
  // will be <= 360x160x3. Hence process works with a relatively small
  // quantity of data.
  size_t bin_pad;
  uint32_t *const restrict partial_binned =
    dt_calloc_perthread(3U * num_bins * num_tones, sizeof(uint32_t), &bin_pad);

  DT_OMP_FOR()
  for(size_t y=0; y<sample_height; y++)
  {
    const float *const restrict px = DT_IS_ALIGNED((const float *const restrict)input +
                                                   4U * ((y + roi->crop_y) * roi->width));
    uint32_t *const restrict binned = dt_get_perthread(partial_binned, bin_pad);
    for(size_t x=0; x<sample_width; x++)
    {
      const size_t bin = (orient == DT_LIB_HISTOGRAM_ORIENT_HORI ? x : y) / samples_per_bin;
      size_t tone[4] DT_ALIGNED_PIXEL;
      for_each_channel(ch, aligned(px,tone:16))
      {
        // 1.0 is at 8/9 of the height!
        const float v = (8.0f / 9.0f) * px[4U * (x + roi->crop_x) + ch];
        // Using ceilf brings everything <= 0 to bottom tone,
        // everything > 1.0f/(num_tones-1) to top tone.
        tone[ch] = ceilf(CLAMPS(v, 0.0f, 1.0f) * (num_tones-1));
      }
      for(size_t ch = 0; ch < 3; ch++)
        binned[num_tones * (ch * num_bins + bin) + tone[ch]]++;
    }
  }

  // shortcut to change from linear to display gamma -- borrow hybrid log-gamma LUT
  const dt_iop_order_iccprofile_info_t *const profile =
    dt_ioppr_add_profile_info_to_list(darktable.develop, DT_COLORSPACE_HLG_REC2020,
                                      "", DT_INTENT_PERCEPTUAL);
  // lut for all three channels should be the same
  const float *const restrict lut =
    DT_IS_ALIGNED((const float *const restrict)profile->lut_out[0]);
  const float lutmax = profile->lutsize - 1;
  const size_t wf_img_stride = cairo_format_stride_for_width
    (CAIRO_FORMAT_A8,
     orient == DT_LIB_HISTOGRAM_ORIENT_HORI ? num_bins : num_tones);

  // Every bin_width x height portion of the image is being described
  // in a 1 pixel x waveform_tones portion of the histogram.
  // NOTE: if constant is decreased, will brighten output

  // FIXME: instead of using an area-beased scale, figure out max bin
  // count and scale to that?

  const float brightness = num_tones / 40.0f;
  const float scale = brightness / ((orient == DT_LIB_HISTOGRAM_ORIENT_HORI
                                     ? sample_height
                                     : sample_width) * samples_per_bin);
  const size_t nthreads = dt_get_num_threads();

  DT_OMP_FOR(collapse(3))
  for(size_t ch = 0; ch < 3; ch++)
    for(size_t bin = 0; bin < num_bins; bin++)
      for(size_t tone = 0; tone < num_tones; tone++)
      {
        uint8_t *const restrict wf_img =
          DT_IS_ALIGNED((uint8_t *const restrict)d->waveform_img[ch]);
        uint32_t acc = 0;
        for(size_t n = 0; n < nthreads; n++)
        {
          uint32_t *const restrict binned = dt_get_bythread(partial_binned, bin_pad, n);
          acc += binned[num_tones * (ch * num_bins + bin) + tone];
        }
        const float linear = MIN(1.f, scale * acc);
        const uint8_t display = lut[(int)(linear * lutmax)] * 255.f;
        if(orient == DT_LIB_HISTOGRAM_ORIENT_HORI)
          wf_img[tone * wf_img_stride + bin] = display;
        else
          wf_img[bin * wf_img_stride + tone] = display;
      }

  dt_free_align(partial_binned);
}

static void dt_lib_histogram_process
  (struct dt_lib_module_t *self,
   const float *const input,
   int width,
   int height,
   const dt_iop_order_iccprofile_info_t *const profile_info_from,
   const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  dt_times_t start;
  dt_get_perf_times(&start);

  dt_lib_histogram_t *d = self->data;

  // special case, clear the scopes
  if(!input)
  {
    dt_pthread_mutex_lock(&d->lock);
    memset(d->histogram, 0, sizeof(uint32_t) * 4 * HISTOGRAM_BINS);
    d->waveform_bins = 0;
    dt_pthread_mutex_unlock(&d->lock);
    return;
  }

  // FIXME: scope goes black when click histogram lib colorpicker on
  // -- is this meant to happen?
  //
  // FIXME: scope doesn't redraw when click histogram lib colorpicker
  // off -- is this meant to happen?
  dt_histogram_roi_t roi = { .width = width,
                             .height = height,
                             .crop_x = 0,
                             .crop_y = 0,
                             .crop_right = 0,
                             .crop_bottom = 0 };

  // Constraining the area if the colorpicker is active in area mode
  //
  // FIXME: only need to do colorspace conversion below on roi
  //
  // FIXME: if the only time we use roi in histogram to limit area is
  // here, and whenever we use tether there is no colorpicker (true?),
  // and if we're always doing a colorspace transform in darkroom and
  // clip to roi during conversion, then can get rid of all roi code
  // for common/histogram?  when darkroom colorpicker is active,
  // gui_module is set to colorout
  if(dt_view_get_current() == DT_VIEW_DARKROOM
     && darktable.lib->proxy.colorpicker.restrict_histogram)
  {
    const dt_colorpicker_sample_t *const sample =
      darktable.lib->proxy.colorpicker.primary_sample;
    const dt_iop_color_picker_t *proxy = darktable.lib->proxy.colorpicker.picker_proxy;
    if(proxy && !proxy->module)
    {
      // FIXME: for histogram process whole image, then pull point
      // sample #'s from primary_picker->scope_mean (point) or _mean,
      // _min, _max (as in rgb curve) and draw them as an overlay
      //
      // FIXME: for waveform point sample, could process whole image,
      // then do an overlay of the point sample from
      // primary_picker->scope_mean as red/green/blue dots (or short
      // lines) at appropriate position at the horizontal/vertical
      // position of sample
      dt_boundingbox_t pos;
      const gboolean isbox = sample->size == DT_LIB_COLORPICKER_SIZE_BOX;
      const gboolean ispoint = sample->size == DT_LIB_COLORPICKER_SIZE_POINT;
      if(ispoint || isbox)
      {
        dt_color_picker_transform_box(darktable.develop,
                                     isbox ? 2 : 1,
                                     isbox ? sample->box : sample->point,
                                     pos, TRUE);
        roi.crop_x = MIN(width, MAX(0, pos[0] * width));
        roi.crop_y = MIN(height, MAX(0, pos[1] * height));
        roi.crop_right = width -    MIN(width,  MAX(0, (isbox ? pos[2] : pos[0]) * width));
        roi.crop_bottom = height -  MIN(height, MAX(0, (isbox ? pos[3] : pos[1]) * height));
      }
    }
  }

  // Convert pixelpipe output in display RGB to histogram profile. If
  // in tether view, then the image is already converted by the
  // caller.

  float *img_display = dt_alloc_align_float((size_t)4 * width * height);
  if(!img_display) return;

  // FIXME: we might get called with profile_info_to == NULL due to caller errors
  if(!profile_info_to)
  {
    dt_print(DT_DEBUG_ALWAYS,
       "[histogram] no histogram profile, replaced with linear Rec2020");
    dt_control_log(_("unsupported profile selected for histogram,"
                     " it will be replaced with linear Rec2020"));
  }

  const dt_iop_order_iccprofile_info_t *fallback =
    dt_ioppr_add_profile_info_to_list(darktable.develop,
      DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_RELATIVE_COLORIMETRIC);

  const dt_iop_order_iccprofile_info_t *profile_info_out = !profile_info_to ? fallback : profile_info_to;

  dt_ioppr_transform_image_colorspace_rgb(input, img_display, width, height,
                                            profile_info_from, profile_info_out, "final histogram");
  dt_pthread_mutex_lock(&d->lock);
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      _lib_histogram_process_histogram(d, img_display, &roi);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
    case DT_LIB_HISTOGRAM_SCOPE_PARADE:
      _lib_histogram_process_waveform(d, img_display, &roi);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      dt_unreachable_codepath();
      break;
  }
  dt_pthread_mutex_unlock(&d->lock);
  dt_free_align(img_display);

  dt_show_times_f(&start, "[histogram]", "final %s",
                  dt_lib_histogram_scope_type_names[d->scope_type]);
}


static void _lib_histogram_draw_histogram(const dt_lib_histogram_t *d,
                                          cairo_t *cr,
                                          const int width,
                                          const int height,
                                          const uint8_t mask[3])
{
  if(!d->histogram_max) return;
  const float hist_max = d->histogram_scale == DT_LIB_HISTOGRAM_SCALE_LINEAR
    ? d->histogram_max
    : logf(1.0 + d->histogram_max);
  // The alpha of each histogram channel is 1, hence the primaries and
  // overlaid secondaries and neutral colors should be about the same
  // brightness. The combined group is then drawn with an alpha, which
  // dims things down.
  cairo_save(cr);
  cairo_push_group_with_content(cr, CAIRO_CONTENT_COLOR);
  cairo_translate(cr, 0, height);
  cairo_scale(cr, width / 255.0, -(height - 10) / hist_max);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  for(int k = 0; k < 3; k++)
    if(mask[k])
    {
      // FIXME: this is the last place in dt these are used -- if can
      // eliminate, then can directly set button colors in CSS and
      // simplify things
      set_color(cr, darktable.bauhaus->graph_colors[k]);
      dt_draw_histogram_8(cr, d->histogram, 4, k,
                          d->histogram_scale == DT_LIB_HISTOGRAM_SCALE_LINEAR);
    }
  cairo_pop_group_to_source(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_paint_with_alpha(cr, 0.5);
  cairo_restore(cr);
}

static void _lib_histogram_draw_waveform(const dt_lib_histogram_t *d,
                                         cairo_t *cr,
                                         const int width,
                                         const int height,
                                         const uint8_t mask[3])
{
  // composite before scaling to screen dimensions, as scaling each
  // layer on draw causes a >2x slowdown
  const double alpha_chroma = 0.75, desat_over = 0.75, alpha_over = 0.35;
  const int img_width = d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI
    ? d->waveform_bins : d->waveform_tones;
  const int img_height = d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI
    ? d->waveform_tones : d->waveform_bins;
  const size_t img_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, img_width);
  cairo_surface_t *cs[3] = { NULL, NULL, NULL };
  cairo_surface_t *cst = cairo_image_surface_create
    (CAIRO_FORMAT_ARGB32, img_width, img_height);

  cairo_t *crt = cairo_create(cst);
  cairo_set_operator(crt, CAIRO_OPERATOR_ADD);
  for(int ch = 0; ch < 3; ch++)
    if(mask[ch])
    {
      cs[ch] = cairo_image_surface_create_for_data(d->waveform_img[ch], CAIRO_FORMAT_A8,
                                                   img_width, img_height, img_stride);
      cairo_set_source_rgba(crt, ch==0 ? 1.:0., ch==1 ? 1.:0., ch==2 ? 1.:0., alpha_chroma);
      cairo_mask_surface(crt, cs[ch], 0., 0.);
    }
  cairo_set_operator(crt, CAIRO_OPERATOR_HARD_LIGHT);
  for(int ch = 0; ch < 3; ch++)
    if(cs[ch])
    {
      cairo_set_source_rgba(crt,
                            ch==0 ? 1.:desat_over,
                            ch==1 ? 1.:desat_over,
                            ch==2 ? 1.:desat_over, alpha_over);
      cairo_mask_surface(crt, cs[ch], 0., 0.);
      cairo_surface_destroy(cs[ch]);
    }
  cairo_destroy(crt);

  // scale and write to output buffer
  cairo_save(cr);
  if(d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI)
  {
    // y=0 is at bottom of widget
    cairo_translate(cr, 0., height);
    cairo_scale(cr, (float)width/img_width, (float)-height/img_height);
  }
  else
  {
    cairo_scale(cr, (float)width/img_width, (float)height/img_height);
  }
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_source_surface(cr, cst, 0., 0.);
  cairo_paint(cr);
  cairo_surface_destroy(cst);
  cairo_restore(cr);
}

static void _lib_histogram_draw_rgb_parade(const dt_lib_histogram_t *d,
                                           cairo_t *cr,
                                           const int width,
                                           const int height)
{
  // same composite-to-temp optimization as in waveform code above
  const double alpha_chroma = 0.85, desat_over = 0.85, alpha_over = 0.65;
  const int img_width = d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI
    ? d->waveform_bins : d->waveform_tones;
  const int img_height = d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI
    ? d->waveform_tones : d->waveform_bins;
  const size_t img_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, img_width);
  cairo_surface_t *cst =
    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, img_width, img_height);

  cairo_t *crt = cairo_create(cst);
  // Though this scales and throws away data on each composite, it
  // appears to be fastest and least memory wasteful. The bin-wise
  // resolution will be visually equivalent to waveform.
  if(d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI)
    cairo_scale(crt, 1./3., 1.);
  else
    cairo_scale(crt, 1., 1./3.);
  for(int ch = 0; ch < 3; ch++)
  {
    cairo_surface_t *cs
      = cairo_image_surface_create_for_data(d->waveform_img[ch], CAIRO_FORMAT_A8,
                                            img_width, img_height, img_stride);
    cairo_set_source_rgba(crt, ch==0 ? 1.:0., ch==1 ? 1.:0., ch==2 ? 1.:0., alpha_chroma);
    cairo_set_operator(crt, CAIRO_OPERATOR_ADD);
    cairo_mask_surface(crt, cs, 0., 0.);
    cairo_set_operator(crt, CAIRO_OPERATOR_HARD_LIGHT);
    cairo_set_source_rgba(crt,
                          ch==0 ? 1.:desat_over,
                          ch==1 ? 1.:desat_over,
                          ch==2 ? 1.:desat_over, alpha_over);
    cairo_mask_surface(crt, cs, 0., 0.);
    cairo_surface_destroy(cs);
    if(d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI)
      cairo_translate(crt, img_width, 0);
    else
      cairo_translate(crt, 0, img_height);
  }
  cairo_destroy(crt);

  cairo_save(cr);
  if(d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI)
  {
    // y=0 is at bottom of widget
    cairo_translate(cr, 0., height);
    cairo_scale(cr, (float)width/img_width, (float)-height/img_height);
  }
  else
  {
    cairo_scale(cr, (float)width/img_width, (float)height/img_height);
  }
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_source_surface(cr, cst, 0., 0.);
  cairo_paint(cr);
  cairo_surface_destroy(cst);
  cairo_restore(cr);
}

// FIXME: have different drawable for each scope in a stack --
// simplifies this function from being a swath of conditionals -- then
// essentially draw callbacks _lib_histogram_draw_waveform,
// _lib_histogram_draw_rgb_parade, etc.
//
// FIXME: if exposure change regions are separate widgets, then we
// could have a menu to swap in different overlay widgets (sort of
// like basic adjustments) to adjust other things about the image,
// e.g. tone equalizer, color balance, etc.
static gboolean _drawable_draw_callback(GtkWidget *widget,
                                        cairo_t *crf,
                                        const gpointer user_data)
{
  dt_times_t start;
  dt_get_perf_times(&start);

  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  const dt_develop_t *dev = darktable.develop;

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
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_fill(cr);
  cairo_restore(cr);

  // exposure change regions
  if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
  {
    set_color(cr, darktable.bauhaus->graph_overlay);
    if(d->scope_type == DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM)
      cairo_rectangle(cr, 0., 0., 0.2 * width, height);
    else if(d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI)
      cairo_rectangle(cr, 0., 7.0/9.0 * height, width, height);
    else if(d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_VERT)
      cairo_rectangle(cr, 0., 0., 2.0/9.0 * width, height);
    else
      dt_unreachable_codepath();
    cairo_fill(cr);
  }
  else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
  {
    set_color(cr, darktable.bauhaus->graph_overlay);
    if(d->scope_type == DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM)
      cairo_rectangle(cr, 0.2 * width, 0., width, height);
    else if(d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI)
      cairo_rectangle(cr, 0., 0., width, 7.0/9.0 * height);
    else if(d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_VERT)
      cairo_rectangle(cr, 2.0/9.0 * width, 0., width, height);
    else
      dt_unreachable_codepath();
    cairo_fill(cr);
  }

  // draw grid
  set_color(cr, darktable.bauhaus->graph_grid);
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      // FIXME: now that vectorscope grid represents log scale, should
      // histogram grid do the same?
      dt_draw_grid(cr, 4, 0, 0, width, height);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
    case DT_LIB_HISTOGRAM_SCOPE_PARADE:
      dt_draw_waveform_lines(cr, 0, 0, width, height,
                             d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      dt_unreachable_codepath();
  }

  // FIXME: should set histogram buffer to black if have just entered
  // tether view and nothing is displayed
  dt_pthread_mutex_lock(&d->lock);
  // darkroom view: draw scope so long as preview pipe is finished
  // tether view: draw whatever has come in from tether
  if(dt_view_get_current() == DT_VIEW_TETHERING
     || dev->image_storage.id == dev->preview_pipe->output_imgid)
  {
    const uint8_t mask[3] = { d->red, d->green, d->blue };
    switch(d->scope_type)
    {
      case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
        _lib_histogram_draw_histogram(d, cr, width, height, mask);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
        if(!d->waveform_bins) break;
        _lib_histogram_draw_waveform(d, cr, width, height, mask);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_PARADE:
        if(!d->waveform_bins) break;
        _lib_histogram_draw_rgb_parade(d, cr, width, height);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_N:
        dt_unreachable_codepath();
    }
  }
  dt_pthread_mutex_unlock(&d->lock);

  // finally a thin border
  cairo_rectangle(cr, 0, 0, width, height);
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_stroke(cr);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  dt_show_times_f(&start, "[histogram]", "scope draw");
  return FALSE;
}

static void _drawable_motion(GtkEventControllerMotion *controller,
                             double x,
                             double y,
                             dt_lib_histogram_t *d)
{
  if(dt_key_modifier_state() & GDK_BUTTON1_MASK)
  {
    if(d->scope_type != DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM
       && d->scope_orient != DT_LIB_HISTOGRAM_ORIENT_VERT)
      x = -y;

    dt_dev_exposure_handle_event(controller, 1, x, FALSE);
  }
  else
  {
    GtkAllocation allocation;
    gtk_widget_get_allocation(d->scope_draw, &allocation);
    const float posx = x / (float)(allocation.width);
    const float posy = y / (float)(allocation.height);
    const dt_lib_histogram_highlight_t prior_highlight = d->highlight;

    // FIXME: make just one tooltip for the widget depending on
    // whether it is draggable or not, and set it when enter the view
    gchar *tip = g_strdup_printf("%s\n(%s)\n%s\n%s",
                                 _(dt_lib_histogram_scope_type_names[d->scope_type]),
                                 _("use buttons at top of graph to change type"),
                                 _("click on ❓ and then graph for documentation"),
                                 _("use color picker module to restrict area"));
    if((posx < 0.2f && d->scope_type == DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM)
       || ((d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM || d->scope_type == DT_LIB_HISTOGRAM_SCOPE_PARADE)
           && ((posy > 7.0f/9.0f && d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI)
             ||(posx < 2.0f/9.0f && d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_VERT))))
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT;
      dt_util_str_cat(&tip, "\n%s\n%s",
                            _("drag to change black point"),
                            _("double-click resets"));
    }
    else
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE;
      dt_util_str_cat(&tip, "\n%s\n%s",
                            _("drag to change exposure"),
                            _("double-click resets"));
    }
    gtk_widget_set_tooltip_text(d->scope_draw, tip);
    g_free(tip);

    if(prior_highlight != d->highlight)
    {
      gtk_widget_queue_draw(d->scope_draw);
      if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
      {
        // FIXME: should really use named cursors, and differentiate
        // between "grab" and "grabbing"
        dt_control_change_cursor(GDK_HAND1);
      }
    }
  }
}

static void _drawable_button_press(GtkGestureSingle *gesture,
                                   int n_press,
                                   double x,
                                   double y,
                                   dt_lib_histogram_t *d)
{
  if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
  {
    if(d->scope_type != DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM
       && d->scope_orient != DT_LIB_HISTOGRAM_ORIENT_VERT)
      x = -y;

    dt_dev_exposure_handle_event(gesture, n_press, x, d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT);
  }
}

static gboolean _eventbox_scroll_callback(GtkWidget *widget,
                                          GdkEventScroll *event,
                                          dt_lib_histogram_t *d)
{
  if(dt_modifier_is(event->state, GDK_SHIFT_MASK | GDK_MOD1_MASK))
  {
    // bubble to adjusting the overall widget size
    gtk_widget_event(d->scope_draw, (GdkEvent*)event);
  }
  else if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
  {
    const gboolean black = d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT;
    if(black) { event->delta_x *= -1; event->delta_y *= -1; }
    dt_dev_exposure_handle_event(event, 0, 0, black);
  }
  return TRUE;
}

static void _drawable_button_release(GtkGestureSingle *gesture,
                                     int n_press,
                                     double x,
                                     double y,
                                     dt_lib_histogram_t *d)
{
  dt_dev_exposure_handle_event(gesture, -n_press, x, FALSE);
}

static void _drawable_leave(GtkEventControllerMotion *controller,
                            dt_lib_histogram_t *d)
{
  // if dragging, gtk keeps up motion notifications until mouse button
  // is released, at which point we'll get another leave event for
  // drawable if pointer is still outside of the widget
  if(!(dt_key_modifier_state() & GDK_BUTTON1_MASK)
     && d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
  {
    d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
    dt_control_change_cursor(GDK_LEFT_PTR);
    gtk_widget_queue_draw(d->scope_draw);
  }
}

static void _histogram_scale_update(const dt_lib_histogram_t *d)
{
  switch(d->histogram_scale)
  {
    case DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set scale to linear"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_logarithmic_scale, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_SCALE_LINEAR:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set scale to logarithmic"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_linear_scale, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_SCALE_N:
      dt_unreachable_codepath();
  }
  // FIXME: this should really redraw current iop if its background is
  // a histogram (check request_histogram)
  darktable.lib->proxy.histogram.is_linear =
    d->histogram_scale == DT_LIB_HISTOGRAM_SCALE_LINEAR;
}

static void _scope_orient_update(const dt_lib_histogram_t *d)
{
  switch(d->scope_orient)
  {
    case DT_LIB_HISTOGRAM_ORIENT_HORI:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set scope to vertical"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_arrow, CPF_DIRECTION_UP, NULL);
      break;
    case DT_LIB_HISTOGRAM_ORIENT_VERT:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set scope to horizontal"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_arrow, CPF_DIRECTION_RIGHT, NULL);
      break;
    case DT_LIB_HISTOGRAM_ORIENT_N:
      dt_unreachable_codepath();
  }
}

static void _scope_type_update(const dt_lib_histogram_t *d)
{
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      gtk_widget_show(d->button_box_rgb);
      _histogram_scale_update(d);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      gtk_widget_show(d->button_box_rgb);
      _scope_orient_update(d);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_PARADE:
      gtk_widget_hide(d->button_box_rgb);
      _scope_orient_update(d);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      dt_unreachable_codepath();
  }
}

static void _scope_type_changed(const dt_lib_histogram_t *d)
{
  dt_conf_set_string("plugins/darkroom/histogram/mode",
                     dt_lib_histogram_scope_type_names[d->scope_type]);
  _scope_type_update(d);

  if(d->waveform_bins)
  {
    // waveform and RGB parade both work on the same underlying data
    gtk_widget_queue_draw(d->scope_draw);
  }
  else
  {
    // generate data for changed scope and trigger widget redraw
    if(dt_view_get_current() == DT_VIEW_DARKROOM)
      dt_dev_process_preview(darktable.develop);
    else
      dt_control_queue_redraw_center();
  }
}

static gboolean _scope_histogram_mode_clicked(GtkWidget *button,
                                              GdkEventButton *event,
                                              dt_lib_histogram_t *d)
{
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)))
    return TRUE;
  int i;
  for(i = 0; i < DT_LIB_HISTOGRAM_SCOPE_N; i++) // find the position of the button
    if(d->scope_type_button[i] == button) break;
  gtk_toggle_button_set_active
    (GTK_TOGGLE_BUTTON(d->scope_type_button[d->scope_type]), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
  const dt_lib_histogram_scope_type_t scope_type_old = d->scope_type;
  d->scope_type = i;
  // waveform and RGB parade both work on the same underlying data
  if((d->scope_type != DT_LIB_HISTOGRAM_SCOPE_PARADE ||
      scope_type_old != DT_LIB_HISTOGRAM_SCOPE_WAVEFORM) &&
      (scope_type_old != DT_LIB_HISTOGRAM_SCOPE_PARADE ||
      d->scope_type != DT_LIB_HISTOGRAM_SCOPE_WAVEFORM))
    d->waveform_bins = 0;
  _scope_type_changed(d);
  return TRUE;
}

static void _scope_view_clicked(GtkWidget *button, dt_lib_histogram_t *d)
{
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      d->histogram_scale = (d->histogram_scale + 1) % DT_LIB_HISTOGRAM_SCALE_N;
      dt_conf_set_string("plugins/darkroom/histogram/histogram",
                         dt_lib_histogram_scale_names[d->histogram_scale]);
      _histogram_scale_update(d);
      // no need to reprocess data
      gtk_widget_queue_draw(d->scope_draw);
      return;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
    case DT_LIB_HISTOGRAM_SCOPE_PARADE:
      d->scope_orient = (d->scope_orient + 1) % DT_LIB_HISTOGRAM_ORIENT_N;
      dt_conf_set_string("plugins/darkroom/histogram/orient",
                         dt_lib_histogram_orient_names[d->scope_orient]);
      d->waveform_bins = 0;
      _scope_orient_update(d);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      dt_unreachable_codepath();
  }
  // trigger new process from scratch
  if(dt_view_get_current() == DT_VIEW_DARKROOM)
    dt_dev_process_preview(darktable.develop);
  else
    dt_control_queue_redraw_center();
}

// FIXME: these all could be the same function with different user_data
static void _red_channel_toggle(GtkWidget *button, dt_lib_histogram_t *d)
{
  d->red = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  dt_conf_set_bool("plugins/darkroom/histogram/show_red", d->red);
  gtk_widget_queue_draw(d->scope_draw);
}

static void _green_channel_toggle(GtkWidget *button, dt_lib_histogram_t *d)
{
  d->green = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  dt_conf_set_bool("plugins/darkroom/histogram/show_green", d->green);
  gtk_widget_queue_draw(d->scope_draw);
}

static void _blue_channel_toggle(GtkWidget *button, dt_lib_histogram_t *d)
{
  d->blue = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  dt_conf_set_bool("plugins/darkroom/histogram/show_blue", d->blue);
  gtk_widget_queue_draw(d->scope_draw);
}

static gboolean _eventbox_enter_notify_callback(GtkWidget *widget,
                                                GdkEventCrossing *event,
                                                const gpointer user_data)
{
  const dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  _scope_type_update(d);
  gtk_widget_show(d->button_box_main);
  gtk_widget_show(d->button_box_opt);
  return FALSE;
}

static gboolean _eventbox_motion_notify_callback(GtkWidget *widget,
                                                 const GdkEventMotion *event,
                                                 const gpointer user_data)
{
  //This is required in order to correctly display the button tooltips
  const dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  _scope_type_update(d);

  return FALSE;
}

static gboolean _eventbox_leave_notify_callback(GtkWidget *widget,
                                                const GdkEventCrossing *event,
                                                const gpointer user_data)
{
  // when click between buttons on the buttonbox a leave event is generated -- ignore it
  if(!(event->mode == GDK_CROSSING_UNGRAB && event->detail == GDK_NOTIFY_INFERIOR))
  {
    const dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
    gtk_widget_hide(d->button_box_main);
    gtk_widget_hide(d->button_box_opt);
  }
  return FALSE;
}

static void _lib_histogram_collapse_callback(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.lib->proxy.histogram.module;

  // Get the state
  const gint visible = dt_lib_is_visible(self);

  // Inverse the visibility
  dt_lib_set_visible(self, !visible);
}

static void _lib_histogram_cycle_mode_callback(dt_action_t *action)
{
  const dt_lib_module_t *self = darktable.lib->proxy.histogram.module;
  dt_lib_histogram_t *d = self->data;

  // FIXME: When switch modes, this hack turns off the highlight and
  // turn the cursor back to pointer, as we don't know what/if the new
  // highlight is going to be. Right solution would be to have a
  // highlight update function which takes cursor x,y and is called
  // either here or on pointer motion. Really right solution is
  // probably separate widgets for the drag areas which generate
  // enter/leave events.
  d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
  dt_control_change_cursor(GDK_LEFT_PTR);

  // The cycle order is Hist log -> lin -> waveform hori -> vert ->
  // parade hori -> vert (update logic on more scopes)
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      if(d->histogram_scale == DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC)
      {
        _scope_view_clicked(d->scope_view_button, d);
      }
      else
      {
        d->scope_orient = DT_LIB_HISTOGRAM_ORIENT_HORI;
        dt_conf_set_string("plugins/darkroom/histogram/orient",
                           dt_lib_histogram_orient_names[d->scope_orient]);
        _scope_histogram_mode_clicked
          (d->scope_type_button[DT_LIB_HISTOGRAM_SCOPE_WAVEFORM], NULL, d);
      }
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      if(d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI)
      {
        _scope_view_clicked(d->scope_view_button, d);
      }
      else
      {
        d->scope_orient = DT_LIB_HISTOGRAM_ORIENT_HORI;
        dt_conf_set_string("plugins/darkroom/histogram/orient",
                           dt_lib_histogram_orient_names[d->scope_orient]);
        // we can't reuse histogram data, as we are changing orientation
        // so this will force recalculation
        d->waveform_bins = 0;
        _scope_histogram_mode_clicked
          (d->scope_type_button[DT_LIB_HISTOGRAM_SCOPE_PARADE], NULL, d);
      }
      break;
    case DT_LIB_HISTOGRAM_SCOPE_PARADE:
      if(d->scope_orient == DT_LIB_HISTOGRAM_ORIENT_HORI)
      {
        _scope_view_clicked(d->scope_view_button, d);
      }
      else
      {
        d->histogram_scale = DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC;
        dt_conf_set_string("plugins/darkroom/histogram/histogram",
                           dt_lib_histogram_scale_names[d->histogram_scale]);
        _scope_histogram_mode_clicked
          (d->scope_type_button[DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM], NULL, d);
      }
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      dt_unreachable_codepath();
  }
}

static void _lib_histogram_change_type_callback(dt_action_t *action)
{
  const dt_lib_module_t *self = darktable.lib->proxy.histogram.module;
  dt_lib_histogram_t *d = self->data;
  _scope_view_clicked(d->scope_view_button, d);
}

// this is only called in darkroom view
static void _lib_histogram_preview_updated_callback(gpointer instance,
                                                    const dt_lib_module_t *self)
{
  // preview pipe has already given process() the high quality
  // pre-gamma image. Now that preview pipe is complete, draw it
  //
  // FIXME: it would be nice if process() just queued a redraw if not
  // in live view, but then our draw code would have to have some
  // other way to assure that the histogram image is current besides
  // checking the pixelpipe to see if it has processed the current
  // image
  const dt_lib_histogram_t *d = self->data;
  gtk_widget_queue_draw(d->scope_draw);
}

void view_enter(struct dt_lib_module_t *self,
                struct dt_view_t *old_view,
                struct dt_view_t *new_view)
{
  const dt_lib_histogram_t *d = self->data;
  if(new_view->view(new_view) == DT_VIEW_DARKROOM)
  {
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, _lib_histogram_preview_updated_callback);
  }
  // button box should be hidden when enter view, unless mouse is over
  // histogram, in which case gtk kindly generates enter events
  gtk_widget_hide(d->button_box_main);
  gtk_widget_hide(d->button_box_opt);

  // FIXME: set histogram data to blank if enter tether with no active image
}

void view_leave(struct dt_lib_module_t *self,
                struct dt_view_t *old_view,
                struct dt_view_t *new_view)
{
  DT_CONTROL_SIGNAL_DISCONNECT(_lib_histogram_preview_updated_callback, self);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_histogram_t *d = dt_calloc1_align_type(dt_lib_histogram_t);
  self->data = (void *)d;

  dt_pthread_mutex_init(&d->lock, NULL);

  d->red = dt_conf_get_bool("plugins/darkroom/histogram/show_red");
  d->green = dt_conf_get_bool("plugins/darkroom/histogram/show_green");
  d->blue = dt_conf_get_bool("plugins/darkroom/histogram/show_blue");

  const char *str = dt_conf_get_string_const("plugins/darkroom/histogram/mode");
  for(dt_lib_histogram_scope_type_t i=0; i<DT_LIB_HISTOGRAM_SCOPE_N; i++)
    if(g_strcmp0(str, dt_lib_histogram_scope_type_names[i]) == 0)
      d->scope_type = i;

  str = dt_conf_get_string_const("plugins/darkroom/histogram/histogram");
  for(dt_lib_histogram_scale_t i=0; i<DT_LIB_HISTOGRAM_SCALE_N; i++)
    if(g_strcmp0(str, dt_lib_histogram_scale_names[i]) == 0)
      d->histogram_scale = i;

  str = dt_conf_get_string_const("plugins/darkroom/histogram/orient");
  for(dt_lib_histogram_orient_t i=0; i<DT_LIB_HISTOGRAM_ORIENT_N; i++)
    if(g_strcmp0(str, dt_lib_histogram_orient_names[i]) == 0)
      d->scope_orient = i;

  d->histogram = dt_alloc_align_type(uint32_t, 4 * HISTOGRAM_BINS);
  memset(d->histogram, 0, 4 * HISTOGRAM_BINS * sizeof(uint32_t));
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
  //
  // FIXME: increasing waveform_max_bins increases processing speed
  // less than increasing waveform_tones -- tune these better?
  d->waveform_max_bins = darktable.mipmap_cache->max_width[DT_MIPMAP_F]/2;
  // initially no waveform to draw
  d->waveform_bins = 0;
  // Should be at least 100 (approx. # of tones the eye can see) and
  // multiple of 16 (for optimization), larger #'s will make a more
  // detailed scope display at cost of a bit of CPU/memory.
  // 175 rows is the default histogram widget height. It's OK if the
  // widget height changes from this, as the width will almost always
  // be scaled. 175 rows is reasonable CPU usage and represents plenty
  // of tonal gradation. 256 would match the # of bins in a regular
  // histogram.
  d->waveform_tones = 160;
  // FIXME: keep alignment instead via single alloc via
  // dt_alloc_perthread()?
  const size_t bytes_hori =
    d->waveform_tones
    * cairo_format_stride_for_width(CAIRO_FORMAT_A8, d->waveform_max_bins);
  const size_t bytes_vert =
    d->waveform_max_bins
    * cairo_format_stride_for_width(CAIRO_FORMAT_A8, d->waveform_tones);
  for(int ch=0; ch<3; ch++)
    d->waveform_img[ch] = dt_alloc_align_uint8(MAX(bytes_hori, bytes_vert));

  // initially no live samples
  d->selected_sample = -1;

  // proxy functions and data so that pixelpipe or tether can
  // provide data for a histogram
  // FIXME: do need to pass self, or can wrap a callback as a lambda
  darktable.lib->proxy.histogram.module = self;
  darktable.lib->proxy.histogram.process = dt_lib_histogram_process;
  darktable.lib->proxy.histogram.is_linear =
    d->histogram_scale == DT_LIB_HISTOGRAM_SCALE_LINEAR;

  // create widgets
  GtkWidget *overlay = gtk_overlay_new();
  dt_action_t *dark =
    dt_action_section(&darktable.view_manager->proxy.darkroom.view->actions,
                      N_("histogram"));
  dt_action_t *ac = NULL;

  dt_action_register(dark, N_("cycle histogram modes"),
                     _lib_histogram_cycle_mode_callback, 0, 0);

  // shows the scope, scale, and has draggable areas
  d->scope_draw = dt_ui_resize_wrap(NULL,
                                    0,
                                    "plugins/darkroom/histogram/graphheight");
  ac = dt_action_define(dark, NULL, N_("hide histogram"), d->scope_draw, NULL);
  dt_action_register(ac, NULL, _lib_histogram_collapse_callback,
                     GDK_KEY_H, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  gtk_widget_set_events(d->scope_draw, GDK_ENTER_NOTIFY_MASK);

  // a row of control buttons, split in two button boxes, on left and right side
  d->button_box_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_class(d->button_box_main, "button_box");
  gtk_widget_set_valign(d->button_box_main, GTK_ALIGN_START);
  gtk_widget_set_halign(d->button_box_main, GTK_ALIGN_START);

  GtkWidget *box_left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(box_left, GTK_ALIGN_START);
  gtk_widget_set_halign(box_left, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(d->button_box_main), box_left, FALSE, FALSE, 0);

  d->button_box_opt = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  dt_gui_add_class(d->button_box_opt, "button_box");
  gtk_widget_set_valign(d->button_box_opt, GTK_ALIGN_START);
  gtk_widget_set_halign(d->button_box_opt, GTK_ALIGN_END);

  // this intermediate box is needed to make the actions on buttons work
  GtkWidget *box_right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(box_right, GTK_ALIGN_START);
  gtk_widget_set_halign(box_right, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(d->button_box_opt), box_right, FALSE, FALSE, 0);

  d->button_box_rgb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(d->button_box_rgb, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(d->button_box_rgb, GTK_ALIGN_END);
  gtk_box_pack_end(GTK_BOX(box_right), d->button_box_rgb, FALSE, FALSE, 0);

  // FIXME: the button transitions when they appear on mouseover
  // (mouse enters scope widget) or change (mouse click) cause redraws
  // of the entire scope -- is there a way to avoid this?

  for(int i=0; i<DT_LIB_HISTOGRAM_SCOPE_N; i++)
  {
    d->scope_type_button[i] =
      dtgtk_togglebutton_new(dt_lib_histogram_scope_type_icons[i], CPF_NONE, NULL);
    gtk_widget_set_tooltip_text(d->scope_type_button[i],
                                _(dt_lib_histogram_scope_type_names[i]));
    dt_action_define(dark, N_("modes"), dt_lib_histogram_scope_type_names[i],
                     d->scope_type_button[i], &dt_action_def_toggle);
    gtk_box_pack_start(GTK_BOX(box_left), d->scope_type_button[i], FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(d->scope_type_button[i]), "button-press-event",
                     G_CALLBACK(_scope_histogram_mode_clicked), d);
  }
  gtk_toggle_button_set_active
    (GTK_TOGGLE_BUTTON(d->scope_type_button[d->scope_type]), TRUE);

  dt_action_t *teth = &darktable.view_manager->proxy.tethering.view->actions;
  if(teth)
  {
    dt_action_register(teth, N_("cycle histogram modes"),
                       _lib_histogram_cycle_mode_callback, 0, 0);
    dt_action_register(teth, N_("hide histogram"),
                       _lib_histogram_collapse_callback,
                       GDK_KEY_H, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    dt_action_register(teth, N_("switch histogram view"),
                       _lib_histogram_change_type_callback, 0, 0);
  }

  // red/green/blue channel on/off
  d->blue_channel_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_NONE, NULL);
  dt_gui_add_class(d->blue_channel_button, "rgb_toggle");
  gtk_widget_set_name(d->blue_channel_button, "blue-channel-button");
  gtk_widget_set_tooltip_text(d->blue_channel_button, _("toggle blue channel"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->blue_channel_button), d->blue);
  dt_action_define(dark, N_("toggle colors"), N_("blue"),
                   d->blue_channel_button, &dt_action_def_toggle);
  gtk_box_pack_end(GTK_BOX(d->button_box_rgb), d->blue_channel_button, FALSE, FALSE, 0);

  d->green_channel_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_NONE, NULL);
  dt_gui_add_class(d->green_channel_button, "rgb_toggle");
  gtk_widget_set_name(d->green_channel_button, "green-channel-button");
  gtk_widget_set_tooltip_text(d->green_channel_button, _("toggle green channel"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->green_channel_button), d->green);
  dt_action_define(dark, N_("toggle colors"), N_("green"),
                   d->green_channel_button, &dt_action_def_toggle);
  gtk_box_pack_end(GTK_BOX(d->button_box_rgb), d->green_channel_button, FALSE, FALSE, 0);

  d->red_channel_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_NONE, NULL);
  dt_gui_add_class(d->red_channel_button, "rgb_toggle");
  gtk_widget_set_name(d->red_channel_button, "red-channel-button");
  gtk_widget_set_tooltip_text(d->red_channel_button, _("toggle red channel"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->red_channel_button), d->red);
  dt_action_define(dark, N_("toggle colors"), N_("red"),
                   d->red_channel_button, &dt_action_def_toggle);
  gtk_box_pack_end(GTK_BOX(d->button_box_rgb), d->red_channel_button, FALSE, FALSE, 0);

  d->scope_view_button = dtgtk_button_new(dtgtk_cairo_paint_empty, CPF_NONE, NULL);
  ac = dt_action_define(dark, NULL, N_("switch histogram view"),
                        d->scope_view_button, &dt_action_def_button);
  gtk_box_pack_end(GTK_BOX(box_right), d->scope_view_button, FALSE, FALSE, 0);

  // will change visibility of buttons, hence must run after all buttons are declared
  _scope_type_update(d);

  // FIXME: add a brightness control (via GtkScaleButton?). Different per each mode?

  // assemble the widgets

  // The main widget is an overlay which has no window, and hence
  // can't catch events. We need something on top to catch events to
  // show/hide the buttons. The drawable is below the buttons, and
  // hence won't catch motion events for the buttons, and gets a leave
  // event when the cursor moves over the buttons.
  //
  // |----- EventBox -----|
  // |                    |
  // |  |-- Overlay  --|  |
  // |  |              |  |
  // |  |  ButtonBox   |  |
  // |  |              |  |
  // |  |--------------|  |
  // |  |              |  |
  // |  |  DrawingArea |  |
  // |  |              |  |
  // |  |--------------|  |
  // |                    |
  // |--------------------|

  GtkWidget *eventbox = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(overlay), d->scope_draw);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), d->button_box_main);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), d->button_box_opt);
  gtk_container_add(GTK_CONTAINER(eventbox), overlay);
  self->widget = eventbox;

  gtk_widget_set_name(self->widget, "main-histogram");

  /* connect callbacks */
  g_signal_connect(G_OBJECT(d->scope_view_button), "clicked",
                   G_CALLBACK(_scope_view_clicked), d);

  g_signal_connect(G_OBJECT(d->red_channel_button), "toggled",
                   G_CALLBACK(_red_channel_toggle), d);
  g_signal_connect(G_OBJECT(d->green_channel_button), "toggled",
                   G_CALLBACK(_green_channel_toggle), d);
  g_signal_connect(G_OBJECT(d->blue_channel_button), "toggled",
                   G_CALLBACK(_blue_channel_toggle), d);

  gtk_widget_add_events(d->scope_draw, GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK
                                     | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
  // FIXME: why does cursor motion over buttons trigger multiple draw callbacks?
  g_signal_connect(G_OBJECT(d->scope_draw), "draw", G_CALLBACK(_drawable_draw_callback), d);
  dt_gui_connect_click_all(d->scope_draw, _drawable_button_press,
                                          _drawable_button_release, d);
  dt_gui_connect_motion(d->scope_draw, _drawable_motion, NULL,
                                       _drawable_leave, d);

  gtk_widget_add_events
    (eventbox,
     GDK_LEAVE_NOTIFY_MASK | GDK_ENTER_NOTIFY_MASK
     | GDK_POINTER_MOTION_MASK | darktable.gui->scroll_mask);
  g_signal_connect(G_OBJECT(eventbox), "scroll-event",
                   G_CALLBACK(_eventbox_scroll_callback), d);
  g_signal_connect(G_OBJECT(eventbox), "enter-notify-event",
                   G_CALLBACK(_eventbox_enter_notify_callback), d);
  g_signal_connect(G_OBJECT(eventbox), "leave-notify-event",
                   G_CALLBACK(_eventbox_leave_notify_callback), d);
  g_signal_connect(G_OBJECT(eventbox), "motion-notify-event",
                   G_CALLBACK(_eventbox_motion_notify_callback), d);

  gtk_widget_show_all(self->widget);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = self->data;

  dt_free_align(d->histogram);
  for(int ch=0; ch<3; ch++)
    dt_free_align(d->waveform_img[ch]);
  d->selected_sample = -1;
  dt_pthread_mutex_destroy(&d->lock);
  dt_free_align(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
