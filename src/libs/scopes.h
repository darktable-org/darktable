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

#pragma once

#include "bauhaus/bauhaus.h"
// FIXME: if we don't use histogram ROI this include can move to scopes/histogram.c
#include "common/histogram.h"
#include "common/iop_profile.h"
#include "gui/gtk.h"

G_BEGIN_DECLS

typedef enum dt_scopes_mode_type_t
{
  DT_SCOPES_MODE_VECTORSCOPE = 0,
  DT_SCOPES_MODE_WAVEFORM,
  DT_SCOPES_MODE_SPLIT,      // must come after waveform and vectorscope
  DT_SCOPES_MODE_PARADE,     // must come after waveform as uses waveform data
  DT_SCOPES_MODE_HISTOGRAM,
  DT_SCOPES_MODE_N           // needs to be the last one
} dt_scopes_mode_type_t;

typedef enum dt_scopes_highlight_t
{
  DT_SCOPES_HIGHLIGHT_NONE = 0,
  DT_SCOPES_HIGHLIGHT_BLACK_POINT,
  DT_SCOPES_HIGHLIGHT_EXPOSURE
} dt_scopes_highlight_t;

typedef enum dt_scopes_rgb_t
{
  DT_SCOPES_RGB_RED = 0,
  DT_SCOPES_RGB_GREEN,
  DT_SCOPES_RGB_BLUE,
  DT_SCOPES_RGB_N // needs to be the last one
} dt_scopes_rgb_t;

typedef gboolean scopes_channels_t[DT_SCOPES_RGB_N];

struct dt_scopes_t;
struct dt_scopes_mode_t;

/** structure used to store pointers to the functions implementing scope modes */
/** plus a few per-class descriptive data items */
typedef struct dt_scopes_functions_t
{
  // name (untranslated) of the current view
  const char* (*name)(const struct dt_scopes_mode_t *const self);
  void (*process)(struct dt_scopes_mode_t *const self,
                  const float *const input,
                  // FIXME: should ROI by dt_histogram_roi_t or another type?
                  dt_histogram_roi_t *const roi,
                  const dt_iop_order_iccprofile_info_t *vs_prof);
  // FIXME: do want a proper clear function or just tag as not up to date?
  void (*clear)(struct dt_scopes_mode_t *const self);
  void (*draw_bkgd)(const struct dt_scopes_mode_t *const self,
                    cairo_t *cr,
                    const int width,
                    const int height);
  void (*draw_grid)(const struct dt_scopes_mode_t *const self,
                    cairo_t *cr,
                    const int width,
                    const int height);
  void (*draw_highlight)(const struct dt_scopes_mode_t *const self,
                         cairo_t *cr,
                         dt_scopes_highlight_t highlight,
                         const int width,
                         const int height);
  void (*draw_scope)(const struct dt_scopes_mode_t *const self,
                     cairo_t *cr,
                     const int width,
                     const int height);
  void (*draw_scope_channels)(const struct dt_scopes_mode_t *const self,
                              cairo_t *cr,
                              const int width,
                              const int height,
                              const scopes_channels_t channels);
  // FIXME: rename to something more sensible
  dt_scopes_highlight_t (*get_highlight)(const struct dt_scopes_mode_t *const self,
                                         const double posx,
                                         const double posy);
  double (*get_exposure_pos)(const struct dt_scopes_mode_t *const self,
                             const double x,
                             const double y);
  void (*append_to_tooltip)(const struct dt_scopes_mode_t *const self,
                            gchar **tip);
  void (*eventbox_scroll)(struct dt_scopes_mode_t *const self,
                          gdouble x, gdouble y,
                          gdouble delta_x, gdouble delta_y,
                          GdkModifierType state);
  void (*eventbox_motion)(struct dt_scopes_mode_t *const self,
                          GtkEventControllerMotion *controller,
                          double x,
                          double y);
  // set option button icons to current state, updates tooltips
  // accordingly, and if necessary update any state which depends on
  // current option buttons
  void (*update_buttons)(const struct dt_scopes_mode_t *const self);
  void (*mode_enter)(struct dt_scopes_mode_t *const self);
  void (*mode_leave)(const struct dt_scopes_mode_t *const self);
  void (*gui_init)(struct dt_scopes_mode_t *const self, struct dt_scopes_t *const scopes);
  void (*add_to_main_box)(struct dt_scopes_mode_t *const self,
                          dt_action_t *dark,
                          GtkWidget *box);
  void (*add_to_options_box)(struct dt_scopes_mode_t *const mode,
                             dt_action_t *dark,
                             GtkWidget *box);
  void (*gui_cleanup)(struct dt_scopes_mode_t *const self);
} dt_scopes_functions_t;

typedef struct dt_scopes_mode_t
{
  const dt_scopes_functions_t *functions;
  void *data;
  GtkWidget *button_activate;                   // GtkDarktableToggleButton which activates mode
  gulong toggle_signal_handler;
  int update_counter;
  // point back to parent
  // FIXME: is this healthy?
  struct dt_scopes_t *scopes;
} dt_scopes_mode_t;

/** structure used to define internal storage for a scope */
typedef struct dt_scopes_t
{
  dt_scopes_mode_t *cur_mode;                   // scope currently displayed
  // FIXME: should this be a GList which is appended with scopes on module init?
  dt_scopes_mode_t modes[DT_SCOPES_MODE_N];     // all available modes
  int update_counter;                           // most recent pixelpipe vs mode data
  dt_scopes_highlight_t highlight;              // depends on mouse position
  scopes_channels_t channels;                   // display state chosen by RGB buttons
  // UI elements
  GtkWidget *button_box_main;                   // GtkBox -- contains scope control buttons
  GtkWidget *button_box_opt;                    // GtkBox -- contains options buttons
  GtkWidget *button_box_rgb;                    // GtkBox -- contains RGB channels buttons
  GtkWidget *channel_buttons[DT_SCOPES_RGB_N];  // Array of GtkToggleButton -- RGB channel display
  GtkWidget *scope_draw;                        // GtkDrawingArea -- scope, scale, draggable overlays
  // for access to data during process/draw
  dt_pthread_mutex_t lock;
} dt_scopes_t;

/** the scope-specific function tables */
// FIXME: does this even need to be declared if it is set up within each scopes/*.c?
extern const dt_scopes_functions_t dt_scopes_functions_histogram;
extern const dt_scopes_functions_t dt_scopes_functions_waveform;
extern const dt_scopes_functions_t dt_scopes_functions_parade;
extern const dt_scopes_functions_t dt_scopes_functions_vectorscope;
extern const dt_scopes_functions_t dt_scopes_functions_split;

// FIXME: instead of making these extern & relying on linker (obscure?) make them part of dt_scopes_t?
extern void lib_histogram_draw_bkgd(const dt_scopes_mode_t *const self,
                                    cairo_t *cr,
                                    const int width,
                                    const int height);
// FIXME: is there any reason this needs to be called with an argument?
extern void lib_histogram_update_tooltip(const dt_scopes_t *const scopes);

#define dt_scopes_func_exists(mode, func) ((mode)->functions->func != NULL)

// FIXME: print warning if func not defined in function table? -- but then how do we return a value?
#define dt_scopes_call(mode, func, ...) (mode)->functions->func(mode, ##__VA_ARGS__)

// FIXME: can make this return a value?
#define dt_scopes_call_if_exists(mode, func, ...) \
    {                                             \
      dt_scopes_mode_t *m = mode;                 \
      if(m->functions->func)                      \
        m->functions->func(m, ##__VA_ARGS__);     \
    }

static inline void dt_scopes_refresh(const dt_scopes_t *const scopes)
{
  gtk_widget_queue_draw(scopes->scope_draw);
}

static inline void dt_scopes_reprocess()
{
  if(dt_view_get_current() == DT_VIEW_DARKROOM)
    dt_dev_process_preview(darktable.develop);
  else
    dt_control_queue_redraw_center();
}

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
