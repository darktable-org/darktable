/*
    This file is part of darktable,
    copyright (c) 2012 johannes hanika.
    copyright (c) 2012--2014 tobias ellinghaus.

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
#include "control/control.h"
#include "develop/develop.h"
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

#define DT_BAUHAUS_WIDGET_TYPE dt_bh_get_type()
#define DT_BAUHAUS_WIDGET(obj) G_TYPE_CHECK_INSTANCE_CAST((obj), DT_BAUHAUS_WIDGET_TYPE, DtBauhausWidget)
#define DT_BAUHAUS_WIDGET_CLASS(obj) G_TYPE_CHECK_CLASS_CAST((obj), DT_BAUHAUS_WIDGET, DtBauhausWidgetClass)
#define DT_IS_BAUHAUS_WIDGET(obj) G_TYPE_CHECK_INSTANCE_TYPE((obj), DT_BAUHAUS_WIDGET_TYPE)
#define DT_IS_BAUHAUS_WIDGET_CLASS(obj) G_TYPE_CHECK_CLASS_TYPE((obj), DT_BAUHAUS_WIDGET_TYPE)
#define DT_BAUHAUS_WIDGET_GET_CLASS                                                                          \
  G_TYPE_INSTANCE_GET_CLASS((obj), DT_BAUHAUS_WIDGET_TYPE, DtBauhausWidgetClass)

extern GType DT_BAUHAUS_WIDGET_TYPE;

#define DT_BAUHAUS_SLIDER_VALUE_CHANGED_DELAY_MAX 500
#define DT_BAUHAUS_SLIDER_VALUE_CHANGED_DELAY_MIN 25
#define DT_BAUHAUS_SLIDER_MAX_STOPS 10

typedef enum dt_bauhaus_type_t
{
  DT_BAUHAUS_SLIDER = 1,
  DT_BAUHAUS_COMBOBOX = 2,
  // TODO: all the fancy color sliders..
} dt_bauhaus_type_t;

typedef enum dt_bauhaus_callback_t
{
  DT_BAUHAUS_SET = 1,
  DT_BAUHAUS_GET = 2
} dt_bauhaus_callback_t;

// data portion for a slider
typedef struct dt_bauhaus_slider_data_t
{
  float pos;      // normalized slider value
  float oldpos;   // slider value before entering finetune mode (normalized)
  float step;     // step width (not normalized)
  float defpos;   // default value (normalized)
  float min, max; // min and max range
  float soft_min, soft_max;
  float hard_min, hard_max;
  float scale; // step width for loupe mode
  int digits;  // how many decimals to round to

  float grad_col[DT_BAUHAUS_SLIDER_MAX_STOPS][3]; // colors for gradient slider
  int grad_cnt;                                   // how many stops
  float grad_pos[DT_BAUHAUS_SLIDER_MAX_STOPS];    // and position of these.

  int fill_feedback; // fill the slider with brighter part up to the handle?

  char format[24]; // numeric value is printed with this string

  int is_dragging;      // indicates is mouse is dragging slider
  int is_changed;       // indicates new data
  guint timeout_handle; // used to store id of timeout routine
  float (*callback)(GtkWidget*, float, dt_bauhaus_callback_t); // callback function
} dt_bauhaus_slider_data_t;

typedef enum dt_bauhaus_combobox_alignment_t
{
  DT_BAUHAUS_COMBOBOX_ALIGN_LEFT = 0,
  DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT = 1
} dt_bauhaus_combobox_alignment_t;

// data portion for a combobox
typedef struct dt_bauhaus_combobox_data_t
{
  int num_labels;    // number of elements
  int active;        // currently active element
  int defpos;        // default position
  int editable;      // 1 if arbitrary text may be typed
  char text[180];    // roughly as much as a slider
  GList *labels;     // list of elements
  GList *alignments; // alignments of the labels. we keep this extra to make it easy to pass the labels around
  GList *data;       // every entry in the combobox can have a gpointer attached
  void (*free_func)(void *); // callback to free data elements
} dt_bauhaus_combobox_data_t;

typedef union dt_bauhaus_data_t
{
  // this is the placeholder for the data portions
  // associated with the implementations such as
  // sliders, combo boxes, ..
  dt_bauhaus_slider_data_t slider;
  dt_bauhaus_combobox_data_t combobox;
} dt_bauhaus_data_t;

// gah, caps.
typedef struct dt_bauhaus_widget_t DtBauhausWidget;
typedef struct dt_bauhaus_widget_class_t DtBauhausWidgetClass;

typedef void (*dt_bauhaus_quad_paint_f)(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);

// our new widget and its private members, inheriting from drawing area:
typedef struct dt_bauhaus_widget_t
{
  // gtk base widget
  GtkDrawingArea parent;
  // which type of control
  dt_bauhaus_type_t type;
  // associated image operation module (to handle focus and such)
  dt_iop_module_t *module;
  // label text, short
  char label[256];
  // callback function to draw the quad icon
  dt_bauhaus_quad_paint_f quad_paint;
  // minimal modifiers for paint function.
  int quad_paint_flags;
  // data for the paint callback
  void *quad_paint_data;
  // quad is a toggle button?
  int quad_toggle;

  // function to populate the combo list on the fly
  void (*combo_populate)(GtkWidget *w, struct dt_iop_module_t **module);

  // goes last, might extend past the end:
  dt_bauhaus_data_t data;
} dt_bauhaus_widget_t;

// class of our new widget, inheriting from drawing area
typedef struct dt_bauhaus_widget_class_t
{
  GtkDrawingAreaClass parent_class;
} dt_bauhaus_widget_class_t;

// global static data:
enum
{
  DT_BAUHAUS_VALUE_CHANGED_SIGNAL,
  DT_BAUHAUS_QUAD_PRESSED_SIGNAL,
  DT_BAUHAUS_LAST_SIGNAL
};

typedef struct dt_bauhaus_t
{
  dt_bauhaus_widget_t *current;
  GtkWidget *popup_window;
  GtkWidget *popup_area;
  // are set by the motion notification, to be used during drawing.
  float mouse_x, mouse_y;
  // time when the popup window was opened. this is sortof a hack to
  // detect `double clicks between windows' to reset the combobox.
  double opentime;
  // pointer position when popup window is closed
  float end_mouse_x, end_mouse_y;
  // used to determine whether the user crossed the line already.
  int change_active;
  float mouse_line_distance;
  // key input buffer
  char keys[64];
  int keys_cnt;
  // our custom signals
  guint signals[DT_BAUHAUS_LAST_SIGNAL];

  // vim-style keyboard interfacing/scripting stuff:
  GHashTable *keymap; // hashtable translating control name -> bauhaus widget ptr
  GList *key_mod;     // for autocomplete, before the point: module.
  GList *key_val;     // for autocomplete, after the point: .value
  char key_history[64][256];

  // appearance relevant stuff:
  // sizes and fonts:
  float scale;                           // gui scale multiplier
  int widget_space;                      // space between widgets in a module
  int line_space;                        // space between lines of text in e.g. the combo box
  int line_height;                       // height of a line of text
  float marker_size;                     // height of the slider indicator
  float label_font_size;                 // percent of line height to fill with font for labels
  float value_font_size;                 // percent of line height to fill with font for values
  char label_font[256];                  // font to draw the label with
  char value_font[256];                  // font to draw the value with
  PangoFontDescription *pango_font_desc; // no need to recreate this for every string we want to print

  // the slider popup has a blinking cursor
  guint cursor_timeout;
  gboolean cursor_visible;
  int cursor_blink_counter;

  // colors:
  GdkRGBA color_fg, color_fg_insensitive, color_bg, color_border;
} dt_bauhaus_t;

static inline int dt_bauhaus_get_widget_space()
{
  return darktable.bauhaus->widget_space;
}
#define DT_BAUHAUS_SPACE dt_bauhaus_get_widget_space()


void dt_bauhaus_init();
void dt_bauhaus_cleanup();

// common functions:
// set the label text:
void dt_bauhaus_widget_set_label(GtkWidget *w, const char *section, const char *label);
const char* dt_bauhaus_widget_get_label(GtkWidget *w);
// attach a custom painted quad to the space at the right side (overwriting the default icon if any):
void dt_bauhaus_widget_set_quad_paint(GtkWidget *w, dt_bauhaus_quad_paint_f f, int paint_flags, void *paint_data);
// make this quad a toggle button:
void dt_bauhaus_widget_set_quad_toggle(GtkWidget *w, int toggle);
// set active status for the quad toggle button:
void dt_bauhaus_widget_set_quad_active(GtkWidget *w, int active);
// get active status for the quad toggle button:
int dt_bauhaus_widget_get_quad_active(GtkWidget *w);

void dt_bauhaus_hide_popup();
void dt_bauhaus_show_popup(dt_bauhaus_widget_t *w);

// slider:
GtkWidget *dt_bauhaus_slider_new(dt_iop_module_t *self);
GtkWidget *dt_bauhaus_slider_new_with_range(dt_iop_module_t *self, float min, float max, float step,
                                            float defval, int digits);
GtkWidget *dt_bauhaus_slider_new_with_range_and_feedback(dt_iop_module_t *self, float min, float max,
                                                         float step, float defval, int digits, int feedback);

GtkWidget *dt_bauhaus_slider_from_widget(dt_bauhaus_widget_t* widget, dt_iop_module_t *self, float min, float max,
                                                         float step, float defval, int digits, int feedback);
// outside doesn't see the real type, we cast it internally.
void dt_bauhaus_slider_set(GtkWidget *w, float pos);
void dt_bauhaus_slider_set_soft(GtkWidget *w, float pos);
float dt_bauhaus_slider_get(GtkWidget *w);

void dt_bauhaus_slider_set_soft_min(GtkWidget* w, float val);
float dt_bauhaus_slider_get_soft_min(GtkWidget* w);
void dt_bauhaus_slider_set_soft_max(GtkWidget* w, float val);
float dt_bauhaus_slider_get_soft_max(GtkWidget* w);

void dt_bauhaus_slider_set_hard_min(GtkWidget* w, float val);
float dt_bauhaus_slider_get_hard_min(GtkWidget* w);
void dt_bauhaus_slider_set_hard_max(GtkWidget* w, float val);
float dt_bauhaus_slider_get_hard_max(GtkWidget* w);

void dt_bauhaus_slider_set_digits(GtkWidget *w, int val);
int dt_bauhaus_slider_get_digits(GtkWidget *w);
void dt_bauhaus_slider_set_step(GtkWidget *w, float val);
float dt_bauhaus_slider_get_step(GtkWidget *w);

void dt_bauhaus_slider_reset(GtkWidget *widget);
void dt_bauhaus_slider_set_format(GtkWidget *w, const char *format);
void dt_bauhaus_slider_set_stop(GtkWidget *widget, float stop, float r, float g, float b);
void dt_bauhaus_slider_clear_stops(GtkWidget *widget);
void dt_bauhaus_slider_set_default(GtkWidget *widget, float def);
void dt_bauhaus_slider_enable_soft_boundaries(GtkWidget *widget, float hard_min, float hard_max);
void dt_bauhaus_slider_set_callback(GtkWidget *widget, float (*callback)(GtkWidget *self, float value, dt_bauhaus_callback_t dir));

// combobox:
void dt_bauhaus_combobox_from_widget(dt_bauhaus_widget_t* widget,dt_iop_module_t *self);
GtkWidget *dt_bauhaus_combobox_new(dt_iop_module_t *self);

void dt_bauhaus_combobox_add(GtkWidget *widget, const char *text);
void dt_bauhaus_combobox_add_aligned(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align);
void dt_bauhaus_combobox_add_full(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align,
                                  gpointer data, void (*free_func)(void *data));
void dt_bauhaus_combobox_set(GtkWidget *w, int pos);
void dt_bauhaus_combobox_remove_at(GtkWidget *widget, int pos);
void dt_bauhaus_combobox_insert(GtkWidget *widget, const char *text,int pos);
void dt_bauhaus_combobox_insert_full(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align,
                                     gpointer data, int pos);
int dt_bauhaus_combobox_length(GtkWidget *widget);
void dt_bauhaus_combobox_set_editable(GtkWidget *w, int editable);
int dt_bauhaus_combobox_get_editable(GtkWidget *w);
const char *dt_bauhaus_combobox_get_text(GtkWidget *w);
void dt_bauhaus_combobox_set_text(GtkWidget *w, const char *text);
int dt_bauhaus_combobox_get(GtkWidget *w);
const GList *dt_bauhaus_combobox_get_labels(GtkWidget *w);
gpointer dt_bauhaus_combobox_get_data(GtkWidget *widget);
void dt_bauhaus_combobox_clear(GtkWidget *w);
void dt_bauhaus_combobox_set_default(GtkWidget *widget, int def);
void dt_bauhaus_combobox_add_populate_fct(GtkWidget *widget, void (*fct)(GtkWidget *w, struct dt_iop_module_t **module));

// key accel parsing:
// execute a line of input
void dt_bauhaus_vimkey_exec(const char *input);
// give autocomplete suggestions
GList *dt_bauhaus_vimkey_complete(const char *input);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
