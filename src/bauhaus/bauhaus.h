/*
    This file is part of darktable,
    Copyright (C) 2012-2025 darktable developers.

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

#include "common/debug.h"
#include "common/introspection.h"
#include "common/colorlabels.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "gui/draw.h"
#include "gui/gtk.h"

#include <assert.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

G_BEGIN_DECLS

#define DT_BAUHAUS_WIDGET_TYPE dt_bh_get_type()
G_DECLARE_FINAL_TYPE(DtBauhausWidget, dt_bh, DT, BAUHAUS_WIDGET, GtkDrawingArea)

#define DT_BAUHAUS_SLIDER_MAX_STOPS 20
#define DT_BAUHAUS_MAX_TEXT 180

typedef enum dt_bauhaus_type_t
{
  DT_BAUHAUS_INVALID = -1,
  DT_BAUHAUS_BUTTON = 0,
  DT_BAUHAUS_SLIDER = 1,
  DT_BAUHAUS_COMBOBOX = 2,
  // TODO: all the fancy color sliders..
} dt_bauhaus_type_t;

typedef enum dt_bauhaus_curve_t
{
  DT_BAUHAUS_SET = 1,
  DT_BAUHAUS_GET = 2
} dt_bauhaus_curve_t;

typedef enum dt_bauhaus_combobox_alignment_t
{
  DT_BAUHAUS_COMBOBOX_ALIGN_LEFT = 0,
  DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT = 1,
  DT_BAUHAUS_COMBOBOX_ALIGN_MIDDLE = 2
} dt_bauhaus_combobox_alignment_t;

// data portion for a combobox
typedef void (*dt_bauhaus_combobox_populate_fct)
  (GtkWidget *widget,
   struct dt_iop_module_t **module);

typedef int (*dt_bauhaus_combobox_entry_select_fct)
  (GtkWidget *widget,
   const char *entry,
   const int delta,
   struct dt_iop_module_t **module);

typedef DTGTKCairoPaintIconFunc dt_bauhaus_quad_paint_f;

// global static data:
enum
{
  DT_BAUHAUS_VALUE_CHANGED_SIGNAL,
  DT_BAUHAUS_QUAD_PRESSED_SIGNAL,
  DT_BAUHAUS_LAST_SIGNAL
};

typedef struct _DtBauhausWidget dt_bauhaus_widget_t;

typedef enum dt_bauhaus_marker_shape_t
{
  DT_BAUHAUS_MARKER_TRIANGLE,
  DT_BAUHAUS_MARKER_CIRCLE,
  DT_BAUHAUS_MARKER_DIAMOND,
  DT_BAUHAUS_MARKER_BAR,
} dt_bauhaus_marker_shape_t;

typedef struct dt_bauhaus_popup_t
{
  GtkWidget *window;
  GtkWidget *area;
  GtkBorder padding;     // padding of the popup. updated in show function
  GdkRectangle position;
  int offset;
  int offcut;
  float oldpos;   // slider value before entering finetune mode (normalized)
} dt_bauhaus_popup_t;

typedef struct dt_bauhaus_t
{
  dt_bauhaus_widget_t *current;
  dt_bauhaus_popup_t popup;

  // the widget that has the mouse over it
  GtkWidget *hovered;
  // are set by the motion notification, to be used during drawing.
  float mouse_x, mouse_y;
  // time when the popup window was opened. this is sortof a hack to
  // detect `double clicks between windows' to reset the combobox.
  guint32 opentime;
  // used to determine whether the user crossed the line or made a change already.
  gboolean change_active;
  float mouse_line_distance;
  // key input buffer
  char keys[DT_BAUHAUS_MAX_TEXT];
  int keys_cnt;
  int unique_match;
  // our custom signals
  guint signals[DT_BAUHAUS_LAST_SIGNAL];

  // initialise or connect accelerators in set_label
  int skip_accel;
  GHashTable *combo_introspection, *combo_list;

  // appearance relevant stuff:
  // sizes and fonts:
  float line_height;                     // height of a line of text
  float marker_size;                     // height of the slider indicator
  dt_bauhaus_marker_shape_t marker_shape;// shape of the slider indicator
  float baseline_size;                   // height of the slider bar
  float border_width;                    // width of the border of the slider marker
  float quad_width;                      // width of the quad area to paint icons
  PangoFontDescription *pango_font_desc; // no need to recreate this for every string we want to print

  // the slider popup has a blinking cursor
  guint cursor_timeout;
  gboolean cursor_visible;
  int cursor_blink_counter;

  // colors for sliders and comboboxes
  GdkRGBA color_fg, color_fg_hover, color_fg_insensitive, color_bg, color_border, indicator_border, color_fill;

  // colors for graphs
  GdkRGBA graph_bg, graph_exterior, graph_border, graph_fg, graph_grid, graph_fg_active, graph_overlay, inset_histogram;
  GdkRGBA graph_colors[3];               // primaries
  GdkRGBA colorlabels[DT_COLORLABELS_LAST];

  // for use by histogram -> exposure proxy
  void (*press)(GtkGestureSingle*, int, double, double, GtkWidget*);
  void (*release)(GtkGestureSingle*, int, double, double, GtkWidget*);
  void (*motion)(GtkEventControllerMotion*, double, double, GtkWidget*);
  gboolean (*scroll)(GtkWidget*, GdkEventScroll*);
} dt_bauhaus_t;

#define DT_BAUHAUS_SPACE 0

void dt_bauhaus_init();
void dt_bauhaus_cleanup();

// load theme colors, fonts, etc
void dt_bauhaus_load_theme();

// common functions:
dt_bauhaus_type_t dt_bauhaus_widget_get_type(GtkWidget *widget);
// set the label text:
dt_action_t *dt_bauhaus_widget_set_label(GtkWidget *widget,
                                         const char *section,
                                         const char *label);
const char* dt_bauhaus_widget_get_label(GtkWidget *widget);
void dt_bauhaus_widget_hide_label(GtkWidget *widget);
void dt_bauhaus_widget_set_show_extended_label(GtkWidget *widget,
                                               gboolean show);
void dt_bauhaus_widget_set_module(GtkWidget *widget,
                                  dt_action_t *module);
gpointer dt_bauhaus_widget_get_module(GtkWidget *widget);
// attach a custom painted quad to the space at the right side (overwriting the default icon if any):
void dt_bauhaus_widget_set_quad_paint(GtkWidget *widget,
                                      dt_bauhaus_quad_paint_f f,
                                      const int paint_flags,
                                      void *paint_data);
// make this quad a toggle button:
void dt_bauhaus_widget_set_quad_toggle(GtkWidget *widget,
                                       gboolean toggle);
// set active status for the quad toggle button:
void dt_bauhaus_widget_set_quad_active(GtkWidget *widget,
                                       gboolean active);
// get active status for the quad toggle button:
int dt_bauhaus_widget_get_quad_active(GtkWidget *widget);
// set quad visibility:
void dt_bauhaus_widget_set_quad_visibility(GtkWidget *widget,
                                           const gboolean visible);
// set a tooltip for the quad button:
void dt_bauhaus_widget_set_quad_tooltip(GtkWidget *widget,
                                        const gchar *text);
// helper macro to set all quad properties at once
static inline void dt_bauhaus_widget_set_quad(GtkWidget *widget,
                                              dt_iop_module_t *self,
                                              dt_bauhaus_quad_paint_f paint,
                                              gboolean toggle,
                                              void (*callback)(GtkWidget *a, dt_iop_module_t *b),
                                              const gchar *tooltip)
{
  dt_bauhaus_widget_set_quad_paint(widget, paint, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(widget, toggle);
  g_signal_connect(G_OBJECT(widget), "quad-pressed", G_CALLBACK(callback), self);
  if(tooltip) dt_bauhaus_widget_set_quad_tooltip(widget, tooltip);
}
// get the tooltip for widget or quad button:
gchar *dt_bauhaus_widget_get_tooltip_markup(GtkWidget *widget,
                                            const int x,
                                            const int y);
// set pointer to iop params field:
void dt_bauhaus_widget_set_field(GtkWidget *widget,
                                 gpointer field,
                                 dt_introspection_type_t field_type);
gpointer dt_bauhaus_widget_get_field(GtkWidget *widget);
// update one bauhaus widget or all widgets in a module from the provided (blend)params
void dt_bauhaus_update_from_field(dt_iop_module_t *module,
                                  GtkWidget *widget,
                                  gpointer params,
                                  gpointer blend_params);
// reset widget to default value
void dt_bauhaus_widget_reset(GtkWidget *widget);

// slider:
GtkWidget *dt_bauhaus_slider_new(dt_iop_module_t *self);
GtkWidget *dt_bauhaus_slider_new_with_range(dt_iop_module_t *self,
                                            float min,
                                            float max,
                                            float step,
                                            float defval,
                                            int digits);
GtkWidget *dt_bauhaus_slider_new_with_range_and_feedback(dt_iop_module_t *self,
                                                         float min,
                                                         float max,
                                                         float step,
                                                         float defval,
                                                         int digits,
                                                         int feedback);
GtkWidget *dt_bauhaus_slider_from_widget(dt_bauhaus_widget_t* widget,
                                         dt_iop_module_t *self,
                                         const float min,
                                         const float max,
                                         const float step,
                                         const float defval,
                                         const int digits,
                                         const int feedback);
GtkWidget *dt_bauhaus_slider_new_action(dt_action_t *self,
                                        float min,
                                        float max,
                                        float step,
                                        float defval,
                                        int digits);

// outside doesn't see the real type, we cast it internally.
void dt_bauhaus_slider_set(GtkWidget *widget,
                           float pos);
void dt_bauhaus_slider_set_val(GtkWidget *widget,
                               float val);
float dt_bauhaus_slider_get(GtkWidget *widget);
float dt_bauhaus_slider_get_val(GtkWidget *widget);
char *dt_bauhaus_slider_get_text(GtkWidget *widget,
                                 float val);

void dt_bauhaus_slider_set_soft_min(GtkWidget* widget,
                                    float val);
float dt_bauhaus_slider_get_soft_min(GtkWidget* widget);
void dt_bauhaus_slider_set_soft_max(GtkWidget* widget,
                                    float val);
float dt_bauhaus_slider_get_soft_max(GtkWidget* widget);
void dt_bauhaus_slider_set_soft_range(GtkWidget *widget,
                                      const float soft_min,
                                      const float soft_max);

void dt_bauhaus_slider_set_hard_min(GtkWidget* widget,
                                    const float val);
float dt_bauhaus_slider_get_hard_min(GtkWidget* widget);
void dt_bauhaus_slider_set_hard_max(GtkWidget* widget,
                                    const float val);
float dt_bauhaus_slider_get_hard_max(GtkWidget* widget);

void dt_bauhaus_slider_set_digits(GtkWidget *widget,
                                  int val);
int dt_bauhaus_slider_get_digits(GtkWidget *widget);
void dt_bauhaus_slider_set_step(GtkWidget *widget,
                                float val);
float dt_bauhaus_slider_get_step(GtkWidget *widget);

void dt_bauhaus_slider_set_feedback(GtkWidget *widget,
                                    const int feedback);
int dt_bauhaus_slider_get_feedback(GtkWidget *widget);

void dt_bauhaus_slider_set_format(GtkWidget *widget,
                                  const char *format);
void dt_bauhaus_slider_set_factor(GtkWidget *widget,
                                  float factor);
void dt_bauhaus_slider_set_offset(GtkWidget *widget,
                                  float offset);
void dt_bauhaus_slider_set_stop(GtkWidget *widget,
                                float stop,
                                float r,
                                float g,
                                float b);
void dt_bauhaus_slider_clear_stops(GtkWidget *widget);
void dt_bauhaus_slider_set_default(GtkWidget *widget,
                                   float def);
float dt_bauhaus_slider_get_default(GtkWidget *widget);
void dt_bauhaus_slider_set_curve(GtkWidget *widget,
                                 float (*curve)(float value, dt_bauhaus_curve_t dir));
void dt_bauhaus_slider_set_log_curve(GtkWidget *widget);

// combobox:
GtkWidget *dt_bauhaus_combobox_from_widget(dt_bauhaus_widget_t* widget,
                                           dt_iop_module_t *self);
GtkWidget *dt_bauhaus_combobox_new(dt_iop_module_t *self);
GtkWidget *dt_bauhaus_combobox_new_action(dt_action_t *self);
GtkWidget *dt_bauhaus_combobox_new_full(dt_action_t *action,
                                        const char *section,
                                        const char *label,
                                        const char *tip,
                                        int pos,
                                        GtkCallback callback,
                                        gpointer data,
                                        const char **texts);
#define DT_BAUHAUS_COMBOBOX_NEW_FULL(widget, action, section, label, tip, pos, callback, data, ...) \
{                                                                                                   \
  static const gchar *texts[] = { __VA_ARGS__, NULL };                                              \
  widget = dt_bauhaus_combobox_new_full(DT_ACTION(action), section, label, tip, pos,                \
                                        (GtkCallback)callback, data, texts);                        \
}

void dt_bauhaus_combobox_add(GtkWidget *widget,
                             const char *text);
void dt_bauhaus_combobox_add_section(GtkWidget *widget,
                                     const char *text);
void dt_bauhaus_combobox_add_aligned(GtkWidget *widget,
                                     const char *text,
                                     dt_bauhaus_combobox_alignment_t align);
void dt_bauhaus_combobox_add_full(GtkWidget *widget,
                                  const char *text,
                                  dt_bauhaus_combobox_alignment_t align,
                                  gpointer data,
                                  void (*free_func)(void *data),
                                  const gboolean sensitive);
gboolean dt_bauhaus_combobox_set_entry_label(GtkWidget *widget,
                                             const int pos,
                                             const gchar *label);
void dt_bauhaus_combobox_set(GtkWidget *widget,
                             int pos);
gboolean dt_bauhaus_combobox_set_from_text(GtkWidget *widget,
                                           const char *text);
gboolean dt_bauhaus_combobox_set_from_value(GtkWidget *widget,
                                            const int value);
int dt_bauhaus_combobox_get_from_value(GtkWidget *widget,
                                       const int value);
void dt_bauhaus_combobox_remove_at(GtkWidget *widget,
                                   const int pos);
void dt_bauhaus_combobox_insert(GtkWidget *widget,
                                const char *text,
                                const int pos);
void dt_bauhaus_combobox_insert_full(GtkWidget *widget,
                                     const char *text,
                                     dt_bauhaus_combobox_alignment_t align,
                                     gpointer data,
                                     void (*free_func)(void *data),
                                     const int pos);
int dt_bauhaus_combobox_length(GtkWidget *widget);
void dt_bauhaus_combobox_set_editable(GtkWidget *widget,
                                      const int editable);
void dt_bauhaus_combobox_set_selected_text_align
  (GtkWidget *widget,
   const dt_bauhaus_combobox_alignment_t text_align);
int dt_bauhaus_combobox_get_editable(GtkWidget *widget);
const char *dt_bauhaus_combobox_get_text(GtkWidget *widget);
void dt_bauhaus_combobox_set_text(GtkWidget *widget,
                                  const char *text);
int dt_bauhaus_combobox_get(GtkWidget *widget);
const char *dt_bauhaus_combobox_get_entry(GtkWidget *widget,
                                          int pos);
gpointer dt_bauhaus_combobox_get_data(GtkWidget *widget);
void dt_bauhaus_combobox_clear(GtkWidget *widget);
void dt_bauhaus_combobox_set_default(GtkWidget *widget,
                                     int def);
int dt_bauhaus_combobox_get_default(GtkWidget *widget);
void dt_bauhaus_combobox_add_populate_fct
  (GtkWidget *widget,
   dt_bauhaus_combobox_populate_fct fct);
void dt_bauhaus_combobox_add_entry_select_fct
  (GtkWidget *widget,
   dt_bauhaus_combobox_entry_select_fct fct);
void dt_bauhaus_combobox_add_list(GtkWidget *widget,
                                  dt_action_t *action,
                                  const char **texts);
gboolean dt_bauhaus_combobox_add_introspection
  (GtkWidget *widget,
   dt_action_t *action,
   const dt_introspection_type_enum_tuple_t *list,
   const int start,
   const int end);
void dt_bauhaus_combobox_entry_set_sensitive(GtkWidget *widget,
                                             int pos,
                                             const gboolean sensitive);
void dt_bauhaus_combobox_set_entries_ellipsis(GtkWidget *widget,
                                              PangoEllipsizeMode ellipis);
PangoEllipsizeMode dt_bauhaus_combobox_get_entries_ellipsis(GtkWidget *widget);
void dt_bauhaus_combobox_mute_scrolling(GtkWidget *widget);

static inline void set_color(cairo_t *cr, GdkRGBA color)
{
  cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
}

#define DT_IOP_SECTION_FOR_PARAMS_UNWIND(self) \
  while(self && self->actions==DT_ACTION_TYPE_IOP_SECTION) self = (dt_iop_module_t *)self->module

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
