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

#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "scopes.h"

#define HISTOGRAM_BINS 256
#define BLACK_POINT_REGION 0.2f

// Tunable thresholds for histogram tooltip analysis
#define HIST_SHADOW_EDGE_BINS 16
#define HIST_HIGHLIGHT_EDGE_BINS 16
#define HIST_CLIP_THRESHOLD_PCT 0.5f
#define HIST_CROWD_THRESHOLD_PCT 5.0f
#define HIST_DARK_MEAN 85.0f
#define HIST_BRIGHT_MEAN 170.0f
#define HIST_LOW_CONTRAST_SPAN 128
#define HIST_COLOR_SKEW 15.0f

typedef enum _hist_issue_t
{
  HIST_ISSUE_HIGHLIGHT_CLIP = 0,
  HIST_ISSUE_SHADOW_CLIP,
  HIST_ISSUE_HIGHLIGHT_CROWD,
  HIST_ISSUE_SHADOW_CROWD,
  HIST_ISSUE_TOO_BRIGHT,
  HIST_ISSUE_TOO_DARK,
  HIST_ISSUE_LOW_CONTRAST,
  HIST_ISSUE_COLOR_CAST,
  HIST_ISSUE_N
} _hist_issue_t;

typedef enum dt_hist_scale_t
{
  DT_HIST_SCALE_LOGARITHMIC = 0,
  DT_HIST_SCALE_LINEAR,
  DT_HIST_SCALE_N // needs to be the last one
} dt_hist_scale_t;

static const gchar *dt_hist_scale_names[DT_HIST_SCALE_N] =
  { "logarithmic",
    "linear"
  };

typedef struct dt_scopes_hist_t
{
  uint32_t *histogram;
  uint32_t histogram_max;
  dt_hist_scale_t scale;
  GtkWidget *scale_button;        // GtkButton -- linear or logarithmic histogram
} dt_scopes_hist_t;


const char* _hist_name(const dt_scopes_mode_t *const self)
{
  return N_("histogram");
}

static void _hist_process(dt_scopes_mode_t *const self,
                          const float *const input,
                          dt_histogram_roi_t *const roi,
                          const dt_iop_order_iccprofile_info_t *vs_prof)
{
  dt_scopes_hist_t *const d = self->data;
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
  self->update_counter = self->scopes->update_counter;
}

static void _hist_draw_grid(const dt_scopes_mode_t *const self,
                            cairo_t *cr,
                            const int width,
                            const int height)
{
  // FIXME: now that vectorscope grid represents log scale, should
  // histogram grid do the same?
  dt_draw_grid(cr, 4, 0, 0, width, height);
}

static void _hist_draw_highlight(const dt_scopes_mode_t *const self,
                                 cairo_t *cr,
                                 dt_scopes_highlight_t highlight,
                                 const int width,
                                 const int height)
{
  if(highlight == DT_SCOPES_HIGHLIGHT_BLACK_POINT)
    cairo_rectangle(cr, 0., 0., BLACK_POINT_REGION * width, height);
  else if(highlight == DT_SCOPES_HIGHLIGHT_EXPOSURE)
    cairo_rectangle(cr, BLACK_POINT_REGION * width, 0., width, height);
  if(highlight != DT_SCOPES_HIGHLIGHT_NONE)
    cairo_fill(cr);
}

static dt_scopes_highlight_t _hist_get_highlight(const dt_scopes_mode_t *const self,
                                                 const double posx,
                                                 const double posy)
{
  return posx < BLACK_POINT_REGION
         ? DT_SCOPES_HIGHLIGHT_BLACK_POINT : DT_SCOPES_HIGHLIGHT_EXPOSURE;
}

static double _hist_get_exposure_pos(const dt_scopes_mode_t *const self,
                                     const double x,
                                     const double y)
{
  return x;
}

static void _hist_draw(const dt_scopes_mode_t *const self,
                       cairo_t *cr,
                       const int width,
                       const int height,
                       const scopes_channels_t channels)
{
  const dt_scopes_hist_t *const d = self->data;

  // FIXME: need this if we have a working update counter?
  if(!d->histogram_max) return;

  const float hist_max = d->scale == DT_HIST_SCALE_LINEAR
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
  for(int k = 0; k < DT_SCOPES_RGB_N; k++)
    if(channels[k])
    {
      // FIXME: this is the last place in dt these are used -- if can
      // eliminate, then can directly set button colors in CSS and
      // simplify things
      set_color(cr, darktable.bauhaus->graph_colors[k]);
      dt_draw_histogram_8(cr, d->histogram, 4, k,
                          d->scale == DT_HIST_SCALE_LINEAR);
    }
  cairo_pop_group_to_source(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_paint_with_alpha(cr, 0.5);
  cairo_restore(cr);
}

static void _hist_clear(dt_scopes_mode_t *const self)
{
  dt_scopes_hist_t *d = self->data;
  d->histogram_max = 0;
}

static void _hist_mode_enter(dt_scopes_mode_t *const self)
{
  dt_scopes_hist_t *d = self->data;
  gtk_widget_show(d->scale_button);
  // FIXME: can call _hist_scale_update() here instead of in gui_init_options?
}

static void _hist_mode_leave(const dt_scopes_mode_t *const self)
{
  dt_scopes_hist_t *d = self->data;
  gtk_widget_hide(d->scale_button);
}

static void _hist_gui_init(dt_scopes_mode_t *const self,
                           dt_scopes_t *const scopes)
{
  dt_scopes_hist_t *d = dt_calloc1_align_type(dt_scopes_hist_t);
  self->data = (void *)d;

  // FIXME: change to plugins/darkroom/scopes/histogram/mode
  const char *str = dt_conf_get_string_const("plugins/darkroom/histogram/histogram");
  for(dt_hist_scale_t i=0; i<DT_HIST_SCALE_N; i++)
    if(g_strcmp0(str, dt_hist_scale_names[i]) == 0)
      d->scale = i;
  darktable.lib->proxy.histogram.is_linear = d->scale == DT_HIST_SCALE_LINEAR;

  d->histogram = dt_calloc_align_type(uint32_t, 4 * HISTOGRAM_BINS);
  d->histogram_max = 0;
}

static void _hist_update_buttons(const dt_scopes_mode_t *const self)
{
  dt_scopes_hist_t *d = self->data;
  switch(d->scale)
  {
    case DT_HIST_SCALE_LOGARITHMIC:
      gtk_widget_set_tooltip_text(d->scale_button, _("set scale to linear"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scale_button),
                             dtgtk_cairo_paint_logarithmic_scale, CPF_NONE, NULL);
      break;
    case DT_HIST_SCALE_LINEAR:
      gtk_widget_set_tooltip_text(d->scale_button, _("set scale to logarithmic"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scale_button),
                             dtgtk_cairo_paint_linear_scale, CPF_NONE, NULL);
      break;
    case DT_HIST_SCALE_N:
      dt_unreachable_codepath();
  }
  // FIXME: this should really redraw current iop if its background is
  // a histogram (check request_histogram)
  darktable.lib->proxy.histogram.is_linear =
    d->scale == DT_HIST_SCALE_LINEAR;
}

static void _hist_scale_clicked(GtkWidget *button, dt_scopes_mode_t *self)
{
  dt_scopes_hist_t *d = self->data;

  d->scale = (d->scale + 1) % DT_HIST_SCALE_N;
  // FIXME: rename to plugins/darkroom/scope/histogram/scale
  dt_conf_set_string("plugins/darkroom/histogram/histogram",
                     dt_hist_scale_names[d->scale]);
  _hist_update_buttons(self);
  // no need to reprocess data
  dt_scopes_refresh(self->scopes);
}

static void _hist_add_to_options_box(dt_scopes_mode_t *const self,
                                     dt_action_t *dark,
                                     GtkWidget *box)
{
  dt_scopes_hist_t *d = self->data;
  d->scale_button = dtgtk_button_new(dtgtk_cairo_paint_empty, CPF_NONE, NULL);
  dt_action_define(dark, NULL, N_("switch histogram scale"),
                   d->scale_button, &dt_action_def_button);
  dt_gui_box_add(box, d->scale_button);
  g_signal_connect(G_OBJECT(d->scale_button), "clicked",
                   G_CALLBACK(_hist_scale_clicked), self);
}

static void _hist_append_to_tooltip(const dt_scopes_mode_t *const self,
                                    gchar **tip)
{
  const dt_scopes_hist_t *const d = self->data;
  if(!d->histogram_max) return;

  uint64_t total[3] = { 0 };
  uint64_t weighted_sum[3] = { 0 };
  uint64_t shadow_edge[3] = { 0 };
  uint64_t highlight_edge[3] = { 0 };
  uint64_t shadow_near[3] = { 0 };
  uint64_t highlight_near[3] = { 0 };
  int first_bin = HISTOGRAM_BINS, last_bin = -1;

  for(int i = 0; i < HISTOGRAM_BINS; i++)
  {
    gboolean any = FALSE;
    for(int ch = 0; ch < 3; ch++)
    {
      const uint32_t c = d->histogram[4 * i + ch];
      total[ch] += c;
      weighted_sum[ch] += (uint64_t)i * c;
      if(i == 0)
        shadow_edge[ch] = c;
      if(i == HISTOGRAM_BINS - 1)
        highlight_edge[ch] = c;
      if(i < HIST_SHADOW_EDGE_BINS)
        shadow_near[ch] += c;
      if(i >= HISTOGRAM_BINS - HIST_HIGHLIGHT_EDGE_BINS)
        highlight_near[ch] += c;
      if(c) any = TRUE;
    }
    if(any)
    {
      if(i < first_bin) first_bin = i;
      last_bin = i;
    }
  }

  const uint64_t max_total = MAX(MAX(total[0], total[1]), total[2]);
  if(max_total == 0) return;

  const float inv_total = 100.0f / (float)max_total;

  const float shadow_edge_pct
    = inv_total * (float)MAX(MAX(shadow_edge[0], shadow_edge[1]),
                             shadow_edge[2]);
  const float highlight_edge_pct
    = inv_total * (float)MAX(MAX(highlight_edge[0], highlight_edge[1]),
                             highlight_edge[2]);
  const float shadow_near_pct
    = inv_total * (float)MAX(MAX(shadow_near[0], shadow_near[1]),
                             shadow_near[2]);
  const float highlight_near_pct
    = inv_total * (float)MAX(MAX(highlight_near[0], highlight_near[1]),
                             highlight_near[2]);

  float ch_mean[3];
  for(int ch = 0; ch < 3; ch++)
    ch_mean[ch] = total[ch] > 0
      ? (float)weighted_sum[ch] / (float)total[ch] : 128.0f;
  const float lum_mean = (ch_mean[0] + ch_mean[1] + ch_mean[2]) / 3.0f;

  const int span = last_bin >= first_bin ? last_bin - first_bin : 0;
  const float max_ch_delta
    = MAX(MAX(fabsf(ch_mean[0] - ch_mean[1]),
              fabsf(ch_mean[1] - ch_mean[2])),
          fabsf(ch_mean[0] - ch_mean[2]));

  // Score each potential issue; zero means not triggered
  float sev[HIST_ISSUE_N];
  memset(sev, 0, sizeof(sev));

  if(highlight_edge_pct > HIST_CLIP_THRESHOLD_PCT)
    sev[HIST_ISSUE_HIGHLIGHT_CLIP] = highlight_edge_pct;

  if(shadow_edge_pct > HIST_CLIP_THRESHOLD_PCT)
    sev[HIST_ISSUE_SHADOW_CLIP] = shadow_edge_pct;

  // Crowding only fires when exact-edge clipping is below threshold
  if(highlight_near_pct > HIST_CROWD_THRESHOLD_PCT
     && sev[HIST_ISSUE_HIGHLIGHT_CLIP] == 0.0f)
    sev[HIST_ISSUE_HIGHLIGHT_CROWD] = highlight_near_pct;

  if(shadow_near_pct > HIST_CROWD_THRESHOLD_PCT
     && sev[HIST_ISSUE_SHADOW_CLIP] == 0.0f)
    sev[HIST_ISSUE_SHADOW_CROWD] = shadow_near_pct;

  // Brightness only fires when the matching edge issue did not
  if(lum_mean > HIST_BRIGHT_MEAN
     && sev[HIST_ISSUE_HIGHLIGHT_CLIP] == 0.0f
     && sev[HIST_ISSUE_HIGHLIGHT_CROWD] == 0.0f)
    sev[HIST_ISSUE_TOO_BRIGHT] = lum_mean - HIST_BRIGHT_MEAN;

  if(lum_mean < HIST_DARK_MEAN
     && sev[HIST_ISSUE_SHADOW_CLIP] == 0.0f
     && sev[HIST_ISSUE_SHADOW_CROWD] == 0.0f)
    sev[HIST_ISSUE_TOO_DARK] = HIST_DARK_MEAN - lum_mean;

  if(span > 0 && span < HIST_LOW_CONTRAST_SPAN)
    sev[HIST_ISSUE_LOW_CONTRAST] = (float)(HIST_LOW_CONTRAST_SPAN - span);

  if(max_ch_delta > HIST_COLOR_SKEW)
    sev[HIST_ISSUE_COLOR_CAST] = max_ch_delta - HIST_COLOR_SKEW;

  // Rank issues by severity (insertion sort, descending)
  int ranked[HIST_ISSUE_N];
  for(int i = 0; i < HIST_ISSUE_N; i++) ranked[i] = i;
  for(int i = 1; i < HIST_ISSUE_N; i++)
    for(int j = i; j > 0 && sev[ranked[j]] > sev[ranked[j - 1]]; j--)
    {
      const int tmp = ranked[j];
      ranked[j] = ranked[j - 1];
      ranked[j - 1] = tmp;
    }

  // Emit each triggered issue as a direct suggestion, highest severity first
  int n_suggestions = 0;
  for(int r = 0; r < HIST_ISSUE_N && n_suggestions < 3; r++)
  {
    if(sev[ranked[r]] <= 0.0f) break;
    const int issue = ranked[r];
    const char *text = NULL;
    switch(issue)
    {
      case HIST_ISSUE_HIGHLIGHT_CLIP:
        text = _("Highlights are clipping — lower exposure");
        break;
      case HIST_ISSUE_SHADOW_CLIP:
        text = _("Shadows are clipping — raise exposure "
                 "or lower the black point");
        break;
      case HIST_ISSUE_HIGHLIGHT_CROWD:
        text = _("Bright tones bunched at the right edge "
                 "— lower exposure slightly");
        break;
      case HIST_ISSUE_SHADOW_CROWD:
        text = _("Dark tones bunched at the left edge "
                 "— raise exposure slightly");
        break;
      case HIST_ISSUE_TOO_BRIGHT:
        text = _("Image looks overexposed — lower exposure");
        break;
      case HIST_ISSUE_TOO_DARK:
        text = _("Image looks underexposed — raise exposure");
        break;
      case HIST_ISSUE_LOW_CONTRAST:
        text = _("Low contrast — try tone curve or levels");
        break;
      case HIST_ISSUE_COLOR_CAST:
      {
        const float r_excess = ch_mean[0] - lum_mean;
        const float b_excess = ch_mean[2] - lum_mean;
        text = r_excess > b_excess
          ? _("Warm color cast — adjust white balance")
          : _("Cool color cast — adjust white balance");
        break;
      }
      default:
        break;
    }
    if(text)
    {
      dt_util_str_cat(tip, "\n%s", text);
      n_suggestions++;
    }
  }

  if(n_suggestions == 0)
    dt_util_str_cat(tip, "\n%s",
                    _("Exposure and contrast look balanced"));
}

static void _hist_gui_cleanup(dt_scopes_mode_t *const self)
{
  dt_scopes_hist_t *d = self->data;
  dt_free_align(d->histogram);

  dt_free_align(self->data);
  self->data = NULL;
}

// The function table for histogram mode. This must be public, i.e. no "static" keyword.
const dt_scopes_functions_t dt_scopes_functions_histogram = {
  .name = _hist_name,
  .process = _hist_process,
  .clear = _hist_clear,
  .draw_bkgd = lib_histogram_draw_bkgd,
  .draw_grid = _hist_draw_grid,
  .draw_highlight = _hist_draw_highlight,
  .draw_scope = NULL,
  .draw_scope_channels = _hist_draw,
  .get_highlight = _hist_get_highlight,
  .get_exposure_pos = _hist_get_exposure_pos,
  .append_to_tooltip = _hist_append_to_tooltip,
  .eventbox_scroll = NULL,
  .eventbox_motion = NULL,
  .update_buttons = _hist_update_buttons,
  .mode_enter = _hist_mode_enter,
  .mode_leave = _hist_mode_leave,
  .gui_init = _hist_gui_init,
  .add_to_main_box = NULL,
  .add_to_options_box = _hist_add_to_options_box,
  .gui_cleanup = _hist_gui_cleanup
};

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
