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

#include <math.h>

#include "common/darktable.h"
#include "common/iop_profile.h"
#include "develop/develop.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "scopes.h"

#define BLACK_POINT_REGION (2.0/9.0)

typedef enum dt_wave_orient_t
{
  DT_WAVE_ORIENT_HORI = 0,
  DT_WAVE_ORIENT_VERT,
  DT_WAVE_ORIENT_N // needs to be the last one
} dt_wave_orient_t;

static const gchar *dt_wave_orient_names[DT_WAVE_ORIENT_N] =
  { "horizontal",
    "vertical"
  };

typedef struct dt_scopes_wave_t
{
  uint8_t *waveform_img[3];           // image per RGB channel
  int waveform_bins, waveform_tones, waveform_max_bins;
  GtkWidget *orient_button;           // GtkButton -- horizontal or vertical
  // state set by buttons
  dt_wave_orient_t orient;
} dt_scopes_wave_t;


const char* _wave_name(const dt_scopes_mode_t *const self)
{
  return N_("waveform");
}

static void _wave_process(dt_scopes_mode_t *const self,
                          const float *const input,
                          dt_histogram_roi_t *const roi,
                          const dt_iop_order_iccprofile_info_t *vs_prof)
{
  dt_scopes_wave_t *const d = self->data;
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
  const dt_wave_orient_t orient = d->orient;
  const int to_bin = orient == DT_WAVE_ORIENT_HORI ? sample_width : sample_height;
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
      const size_t bin = (orient == DT_WAVE_ORIENT_HORI ? x : y) / samples_per_bin;
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
     orient == DT_WAVE_ORIENT_HORI ? num_bins : num_tones);

  // Every bin_width x height portion of the image is being described
  // in a 1 pixel x waveform_tones portion of the histogram.
  // NOTE: if constant is decreased, will brighten output

  // FIXME: instead of using an area-beased scale, figure out max bin
  // count and scale to that?

  const float brightness = num_tones / 40.0f;
  const float scale = brightness / ((orient == DT_WAVE_ORIENT_HORI
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
        if(orient == DT_WAVE_ORIENT_HORI)
          wf_img[tone * wf_img_stride + bin] = display;
        else
          wf_img[bin * wf_img_stride + tone] = display;
      }

  dt_free_align(partial_binned);

  // waveform and rgb parade share underlying data, so updates to one update both
  self->scopes->modes[DT_SCOPES_MODE_WAVEFORM].update_counter =
    self->scopes->modes[DT_SCOPES_MODE_PARADE].update_counter =
      self->scopes->update_counter;
}

static void _wave_draw_grid(const dt_scopes_mode_t *const self,
                            cairo_t *cr,
                            const int width,
                            const int height)
{
  const dt_scopes_wave_t *const d = self->data;
  dt_draw_waveform_lines(cr, 0, 0, width, height,
                         d->orient == DT_WAVE_ORIENT_HORI);
}

static void _wave_draw_highlight(const dt_scopes_mode_t *const self,
                                 cairo_t *cr,
                                 dt_scopes_highlight_t highlight,
                                 const int width,
                                 const int height)
{
  dt_scopes_wave_t *const d = self->data;

  if(highlight == DT_SCOPES_HIGHLIGHT_BLACK_POINT)
  {
    if(d->orient == DT_WAVE_ORIENT_HORI)
      cairo_rectangle(cr, 0., (1.0 - BLACK_POINT_REGION) * height, width, height);
    else if(d->orient == DT_WAVE_ORIENT_VERT)
      cairo_rectangle(cr, 0., 0., BLACK_POINT_REGION * width, height);
    else
      dt_unreachable_codepath();
    cairo_fill(cr);
  }
  else if(highlight == DT_SCOPES_HIGHLIGHT_EXPOSURE)
  {
    if(d->orient == DT_WAVE_ORIENT_HORI)
      cairo_rectangle(cr, 0., 0., width, (1.0 - BLACK_POINT_REGION) * height);
    else if(d->orient == DT_WAVE_ORIENT_VERT)
      cairo_rectangle(cr, BLACK_POINT_REGION * width, 0., width, height);
    else
      dt_unreachable_codepath();
    cairo_fill(cr);
  }
}

static void _wave_draw(const dt_scopes_mode_t *const self,
                       cairo_t *cr,
                       const int width,
                       const int height,
                       const scopes_channels_t channels)
{
  const dt_scopes_wave_t *const d = self->data;

  // FIXME: need this if we have a working update counter?
  if(!d->waveform_bins) return;

  // composite before scaling to screen dimensions, as scaling each
  // layer on draw causes a >2x slowdown
  const double alpha_chroma = 0.75, desat_over = 0.75, alpha_over = 0.35;
  const int img_width = d->orient == DT_WAVE_ORIENT_HORI
    ? d->waveform_bins : d->waveform_tones;
  const int img_height = d->orient == DT_WAVE_ORIENT_HORI
    ? d->waveform_tones : d->waveform_bins;
  const size_t img_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, img_width);
  cairo_surface_t *cs[3] = { NULL, NULL, NULL };
  cairo_surface_t *cst = cairo_image_surface_create
    (CAIRO_FORMAT_ARGB32, img_width, img_height);

  cairo_t *crt = cairo_create(cst);
  cairo_set_operator(crt, CAIRO_OPERATOR_ADD);
  for(int ch = 0; ch < 3; ch++)
    if(channels[ch])
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
  if(d->orient == DT_WAVE_ORIENT_HORI)
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

static double _wave_get_exposure_pos(const dt_scopes_mode_t *const self,
                                     const double x,
                                     const double y)
{
  const dt_scopes_wave_t *const d = self->data;
  return d->orient == DT_WAVE_ORIENT_HORI ? -y : x;
}

static dt_scopes_highlight_t _wave_get_highlight(const dt_scopes_mode_t *const self,
                                                 const double posx,
                                                 const double posy)
{
  const dt_scopes_wave_t *const d = self->data;
  if((posy > (1.0f - BLACK_POINT_REGION) && d->orient == DT_WAVE_ORIENT_HORI)
     ||(posx < BLACK_POINT_REGION && d->orient == DT_WAVE_ORIENT_VERT))
    return DT_SCOPES_HIGHLIGHT_BLACK_POINT;
  else
    return DT_SCOPES_HIGHLIGHT_EXPOSURE;
}

static void _wave_clear(dt_scopes_mode_t *const self)
{
  // FIXME: make sure this is actually called appropriately in histogram.c
  dt_scopes_wave_t *const d = self->data;
  d->waveform_bins = 0;
}

static void _wave_update_buttons(const dt_scopes_mode_t *const self)
{
  dt_scopes_wave_t *d = self->data;
  switch(d->orient)
  {
    case DT_WAVE_ORIENT_HORI:
      gtk_widget_set_tooltip_text(d->orient_button, _("set scope to vertical"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->orient_button),
                             dtgtk_cairo_paint_arrow, CPF_DIRECTION_UP, NULL);
      break;
    case DT_WAVE_ORIENT_VERT:
      gtk_widget_set_tooltip_text(d->orient_button, _("set scope to horizontal"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->orient_button),
                             dtgtk_cairo_paint_arrow, CPF_DIRECTION_RIGHT, NULL);
      break;
    case DT_WAVE_ORIENT_N:
      dt_unreachable_codepath();
  }
}

static void _wave_mode_enter(dt_scopes_mode_t *const self)
{
  const dt_scopes_wave_t *const d = self->data;
  gtk_widget_show(d->orient_button);
}

static void _wave_mode_leave(const dt_scopes_mode_t *const self)
{
  const dt_scopes_wave_t *const d = self->data;
  gtk_widget_hide(d->orient_button);
}

static void _wave_gui_init(dt_scopes_mode_t *const self,
                           dt_scopes_t *const scopes)
{
  dt_scopes_wave_t *d = dt_calloc1_align_type(dt_scopes_wave_t);
  self->data = (void *)d;

  const char *str = dt_conf_get_string_const("plugins/darkroom/histogram/orient");
  for(dt_wave_orient_t i=0; i<DT_WAVE_ORIENT_N; i++)
    if(g_strcmp0(str, dt_wave_orient_names[i]) == 0)
      d->orient = i;

  // Waveform buffer doesn't need to be coupled with the scopes
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
  // FIXME: combine waveform_8bit and vectorscope_graph, as only ever
  // use one or the other
  //
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
}

static void _wave_orient_clicked(GtkWidget *button, dt_scopes_mode_t *const self)
{
  dt_scopes_wave_t *d = self->data;
  d->orient = (d->orient + 1) % DT_WAVE_ORIENT_N;
  dt_conf_set_string("plugins/darkroom/histogram/orient",
                     dt_wave_orient_names[d->orient]);
  d->waveform_bins = 0;
  _wave_update_buttons(self);
  dt_scopes_reprocess();
}

static void _wave_add_to_options_box(dt_scopes_mode_t *const self,
                                     dt_action_t *dark,
                                     GtkWidget *box)
{
  dt_scopes_wave_t *const d = self->data;
  d->orient_button = dtgtk_button_new(dtgtk_cairo_paint_empty, CPF_NONE, NULL);
  dt_action_define(dark, NULL, N_("switch scope orientation"),
                   d->orient_button, &dt_action_def_button);
  dt_gui_box_add(box, d->orient_button);
  g_signal_connect(G_OBJECT(d->orient_button), "clicked",
                   G_CALLBACK(_wave_orient_clicked), self);
}

static void _wave_gui_cleanup(dt_scopes_mode_t *const self)
{
  dt_scopes_wave_t *d = self->data;
  for(int ch=0; ch<3; ch++)
    dt_free_align(d->waveform_img[ch]);

  dt_free_align(self->data);
  self->data = NULL;
}

// The function table for waveform mode. This must be public, i.e. no "static" keyword.
const dt_scopes_functions_t dt_scopes_functions_waveform = {
  .name = _wave_name,
  .process = _wave_process,
  .clear = _wave_clear,
  .draw_bkgd = lib_histogram_draw_bkgd,
  .draw_grid = _wave_draw_grid,
  .draw_highlight = _wave_draw_highlight,
  .draw_scope = NULL,
  .draw_scope_channels = _wave_draw,
  .get_highlight = _wave_get_highlight,
  .get_exposure_pos = _wave_get_exposure_pos,
  .append_to_tooltip = NULL,
  .eventbox_scroll = NULL,
  .eventbox_motion = NULL,
  .update_buttons = _wave_update_buttons,
  .mode_enter = _wave_mode_enter,
  .mode_leave = _wave_mode_leave,
  .gui_init = _wave_gui_init,
  .add_to_main_box = NULL,
  .add_to_options_box = _wave_add_to_options_box,
  .gui_cleanup = _wave_gui_cleanup
};


const char* _parade_name(const dt_scopes_mode_t *const self)
{
  return N_("RGB parade");
}

static void _parade_draw(const dt_scopes_mode_t *const self,
                         cairo_t *cr,
                         const int width,
                         const int height)
{
  const dt_scopes_wave_t *const d = self->data;

  // FIXME: need this if we have a working update counter?
  if(!d->waveform_bins) return;

  // same composite-to-temp optimization as in waveform code above
  const double alpha_chroma = 0.85, desat_over = 0.85, alpha_over = 0.65;
  const int img_width = d->orient == DT_WAVE_ORIENT_HORI
    ? d->waveform_bins : d->waveform_tones;
  const int img_height = d->orient == DT_WAVE_ORIENT_HORI
    ? d->waveform_tones : d->waveform_bins;
  const size_t img_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, img_width);
  cairo_surface_t *cst =
    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, img_width, img_height);

  cairo_t *crt = cairo_create(cst);
  // Though this scales and throws away data on each composite, it
  // appears to be fastest and least memory wasteful. The bin-wise
  // resolution will be visually equivalent to waveform.
  if(d->orient == DT_WAVE_ORIENT_HORI)
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
    if(d->orient == DT_WAVE_ORIENT_HORI)
      cairo_translate(crt, img_width, 0);
    else
      cairo_translate(crt, 0, img_height);
  }
  cairo_destroy(crt);

  cairo_save(cr);
  if(d->orient == DT_WAVE_ORIENT_HORI)
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

static void _parade_gui_init(dt_scopes_mode_t *const self,
                           dt_scopes_t *const scopes)
{
  dt_scopes_mode_t *const wave_mode = &scopes->modes[DT_SCOPES_MODE_WAVEFORM];
  if(wave_mode->data)
    self->data = wave_mode->data;
  else
    dt_print(DT_DEBUG_ALWAYS, "[rgb parade] waveform must be initialized before rgb parade");
}

static void _parade_gui_cleanup(dt_scopes_mode_t *const self)
{
  self->data = NULL;
}

// The function table for rgb parade, which is a minor display variation off waveform
// FIXME: should rgb parade just be an option button for waveform?
const dt_scopes_functions_t dt_scopes_functions_parade = {
  .name = _parade_name,
  .process = _wave_process,
  .clear = _wave_clear,
  .draw_bkgd = lib_histogram_draw_bkgd,
  .draw_grid = _wave_draw_grid,
  .draw_highlight = _wave_draw_highlight,
  .draw_scope = _parade_draw,
  .draw_scope_channels = NULL,
  .get_highlight = _wave_get_highlight,
  .get_exposure_pos = _wave_get_exposure_pos,
  .append_to_tooltip = NULL,
  .eventbox_scroll = NULL,
  .eventbox_motion = NULL,
  .update_buttons = _wave_update_buttons,
  .mode_enter = _wave_mode_enter,
  .mode_leave = _wave_mode_leave,
  .gui_init = _parade_gui_init,
  .add_to_main_box = NULL,
  .add_to_options_box = NULL,
  .gui_cleanup = _parade_gui_cleanup
};

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
