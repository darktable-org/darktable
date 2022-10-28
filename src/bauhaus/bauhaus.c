/*
    This file is part of darktable,
    Copyright (C) 2012-2022 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/calculator.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <math.h>
#include <strings.h>

#include <pango/pangocairo.h>

G_DEFINE_TYPE(DtBauhausWidget, dt_bh, GTK_TYPE_DRAWING_AREA)

enum
{
  // Sliders
  DT_ACTION_ELEMENT_VALUE = 0,
  DT_ACTION_ELEMENT_BUTTON = 1,
  DT_ACTION_ELEMENT_FORCE = 2,
  DT_ACTION_ELEMENT_ZOOM = 3,

  // Combos
  DT_ACTION_ELEMENT_SELECTION = 0,
//DT_ACTION_ELEMENT_BUTTON = 1,
};

// INNER_PADDING is the horizontal space between slider and quad
// and vertical space between labels and slider baseline
static const double INNER_PADDING = 4.0;

// fwd declare
static gboolean dt_bauhaus_popup_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static gboolean dt_bauhaus_popup_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static gboolean _widget_draw(GtkWidget *widget, cairo_t *crf);
static gboolean _widget_scroll(GtkWidget *widget, GdkEventScroll *event);
static gboolean _widget_key_press(GtkWidget *widget, GdkEventKey *event);
static void _get_preferred_width(GtkWidget *widget, gint *minimum_size, gint *natural_size);
static void _style_updated(GtkWidget *widget);
static void dt_bauhaus_widget_accept(dt_bauhaus_widget_t *w);
static void dt_bauhaus_widget_reject(dt_bauhaus_widget_t *w);
static void _bauhaus_combobox_set(dt_bauhaus_widget_t *w, const int pos, const gboolean mute);

static void bauhaus_request_focus(dt_bauhaus_widget_t *w)
{
  if(w->module && w->module->type == DT_ACTION_TYPE_IOP_INSTANCE)
      dt_iop_request_focus((dt_iop_module_t *)w->module);
  gtk_widget_set_state_flags(GTK_WIDGET(w), GTK_STATE_FLAG_FOCUSED, FALSE);
}

static float _widget_get_quad_width(dt_bauhaus_widget_t *w)
{
  if(w->show_quad)
    return darktable.bauhaus->quad_width + INNER_PADDING;
  else
    return .0f;
}

static void _combobox_next_sensitive(dt_bauhaus_widget_t *w, int delta, const gboolean mute)
{
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  int new_pos = d->active;
  int step = delta > 0 ? 1 : -1;
  int cur = new_pos + step;
  while(delta && cur >= 0 && cur < d->entries->len)
  {
    dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, cur);
    if(entry->sensitive)
    {
      new_pos = cur;
      delta -= step;
    }
    cur += step;
  }

  _bauhaus_combobox_set(w, new_pos, mute);
}

static dt_bauhaus_combobox_entry_t *new_combobox_entry(const char *label, dt_bauhaus_combobox_alignment_t alignment,
                                                       gboolean sensitive, void *data, void (*free_func)(void *))
{
  dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)calloc(1, sizeof(dt_bauhaus_combobox_entry_t));
  entry->label = g_strdup(label);
  entry->alignment = alignment;
  entry->sensitive = sensitive;
  entry->data = data;
  entry->free_func = free_func;
  return entry;
}

static void free_combobox_entry(gpointer data)
{
  dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)data;
  g_free(entry->label);
  if(entry->free_func)
    entry->free_func(entry->data);
  free(entry);
}

static GdkRGBA * default_color_assign()
{
  // helper to initialize a color pointer with red color as a default
  GdkRGBA color = {.red = 1.0f, .green = 0.0f, .blue = 0.0f, .alpha = 1.0f};
  return gdk_rgba_copy(&color);
}

static void _margins_retrieve(dt_bauhaus_widget_t *w)
{
  if(!w->margin) w->margin = gtk_border_new();
  if(!w->padding) w->padding = gtk_border_new();
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(w));
  const GtkStateFlags state = gtk_widget_get_state_flags(GTK_WIDGET(w));
  gtk_style_context_get_margin(context, state, w->margin);
  gtk_style_context_get_padding(context, state, w->padding);
}

void dt_bauhaus_widget_set_section(GtkWidget *widget, const gboolean is_section)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->is_section = is_section;
}

static int show_pango_text(dt_bauhaus_widget_t *w, GtkStyleContext *context, cairo_t *cr, const char *text,
                           float x_pos, float y_pos, float max_width, gboolean right_aligned, gboolean calc_only,
                           PangoEllipsizeMode ellipsize, gboolean is_markup, gboolean is_label, float *width,
                           float *height)
{
  PangoLayout *layout = pango_cairo_create_layout(cr);

  if(max_width > 0)
  {
    pango_layout_set_ellipsize(layout, ellipsize);
    pango_layout_set_width(layout, (int)(PANGO_SCALE * max_width + 0.5f));
  }

  if(text)
  {
    if(is_markup)
      pango_layout_set_markup(layout, text, -1);
    else
      pango_layout_set_text(layout, text, -1);
  }
  else
  {
    // length of -1 is not allowed with NULL string (wtf)
    pango_layout_set_text(layout, NULL, 0);
  }

  PangoFontDescription *font_desc = 0;
  gtk_style_context_get(context, gtk_widget_get_state_flags(GTK_WIDGET(w)), "font", &font_desc, NULL);

  pango_layout_set_font_description(layout, font_desc);

  PangoAttrList *attrlist = pango_attr_list_new();
  PangoAttribute *attr = pango_attr_font_features_new("tnum");
  pango_attr_list_insert(attrlist, attr);
  pango_layout_set_attributes(layout, attrlist);
  pango_attr_list_unref(attrlist);

  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);

  int pango_width, pango_height;
  pango_layout_get_size(layout, &pango_width, &pango_height);
  const float text_width = ((double)pango_width/PANGO_SCALE);
  if(calc_only && width && height)
  {
    *width = text_width;
    *height = ((double)pango_height / PANGO_SCALE);
  }

  if(right_aligned) x_pos -= text_width;

  if(!calc_only)
  {
    cairo_move_to(cr, x_pos, y_pos);
    pango_cairo_show_layout(cr, layout);
  }
  pango_font_description_free(font_desc);
  g_object_unref(layout);

  return text_width;
}

// -------------------------------
static gboolean _cursor_timeout_callback(gpointer user_data)
{
  if(darktable.bauhaus->cursor_blink_counter > 0) darktable.bauhaus->cursor_blink_counter--;

  darktable.bauhaus->cursor_visible = !darktable.bauhaus->cursor_visible;
  gtk_widget_queue_draw(darktable.bauhaus->popup_area);

 // this can be >0 when we haven't reached the desired number or -1 when blinking forever
  if(darktable.bauhaus->cursor_blink_counter != 0)
    return TRUE;

  darktable.bauhaus->cursor_timeout = 0; // otherwise the cursor won't come up when starting to type
  return FALSE;
}

static void _start_cursor(int max_blinks)
{
  darktable.bauhaus->cursor_blink_counter = max_blinks;
  darktable.bauhaus->cursor_visible = FALSE;
  if(darktable.bauhaus->cursor_timeout == 0)
    darktable.bauhaus->cursor_timeout = g_timeout_add(500, _cursor_timeout_callback, NULL);
}

static void _stop_cursor()
{
  if(darktable.bauhaus->cursor_timeout > 0)
  {
    g_source_remove(darktable.bauhaus->cursor_timeout);
    darktable.bauhaus->cursor_timeout = 0;
    darktable.bauhaus->cursor_visible = FALSE;
  }
}
// -------------------------------


static void dt_bauhaus_slider_set_normalized(dt_bauhaus_widget_t *w, float pos);

static float slider_right_pos(float width, dt_bauhaus_widget_t *w)
{
  // relative position (in widget) of the right bound of the slider corrected with the inner padding
  return 1.0f - _widget_get_quad_width(w) / width;
}

static float slider_coordinate(const float abs_position, const float width, dt_bauhaus_widget_t *w)
{
  // Translates an horizontal position relative to the slider
  // in an horizontal position relative to the widget
  const float left_bound = 0.0f;
  const float right_bound = slider_right_pos(width, w); // exclude the quad area on the right
  return (left_bound + abs_position * (right_bound - left_bound)) * width;
}


static float get_slider_line_offset(float pos, float scale, float x, float y, float ht, const int width,
                                    dt_bauhaus_widget_t *w)
{
  // ht is in [0,1] scale here
  const float l = 0.0f;
  const float r = slider_right_pos(width, w);

  float offset = 0.0f;
  // handle linear startup and rescale y to fit the whole range again
  if(y < ht)
  {
    offset = (x - l) / (r - l) - pos;
  }
  else
  {
    y -= ht;
    y /= (1.0f - ht);

    offset = (x - y * y * .5f - (1.0f - y * y) * (l + pos * (r - l)))
             / (.5f * y * y / scale + (1.0f - y * y) * (r - l));
  }
  // clamp to result in a [0,1] range:
  if(pos + offset > 1.0f) offset = 1.0f - pos;
  if(pos + offset < 0.0f) offset = -pos;
  return offset;
}

// draw a loupe guideline for the quadratic zoom in in the slider interface:
static void draw_slider_line(cairo_t *cr, float pos, float off, float scale, const int width, const int height,
                             const int ht, dt_bauhaus_widget_t *w)
{
  // pos is normalized position [0,1], offset is on that scale.
  // ht is in pixels here
  const float r = slider_right_pos(width, w);

  const int steps = 64;
  cairo_move_to(cr, width * (pos + off) * r, ht * .7f);
  cairo_line_to(cr, width * (pos + off) * r, ht);
  for(int j = 1; j < steps; j++)
  {
    const float y = j / (steps - 1.0f);
    const float x = y * y * .5f * (1.f + off / scale) + (1.0f - y * y) * (pos + off) * r;
    cairo_line_to(cr, x * width, ht + y * (height - ht));
  }
}
// -------------------------------

static void combobox_popup_scroll(int amt)
{
  const dt_bauhaus_combobox_data_t *d = &darktable.bauhaus->current->data.combobox;
  int old_value = d->active;

  _combobox_next_sensitive(darktable.bauhaus->current, amt, d->mute_scrolling);

  gint wx = 0, wy = 0;
  const int skip = darktable.bauhaus->line_height;
  GdkWindow *w = gtk_widget_get_window(darktable.bauhaus->popup_window);
  gdk_window_get_origin(w, &wx, &wy);
  gdk_window_move(w, wx, wy - skip * (d->active - old_value));

  // make sure highlighted entry is updated:
  darktable.bauhaus->mouse_x = 0;
  darktable.bauhaus->mouse_y = d->active * skip + skip / 2;
  gtk_widget_queue_draw(darktable.bauhaus->popup_area);
}

static void _slider_zoom_range(dt_bauhaus_widget_t *w, float zoom)
{
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  const float value = dt_bauhaus_slider_get(GTK_WIDGET(w));

  if(!zoom)
  {
    d->min = d->soft_min;
    d->max = d->soft_max;
    dt_bauhaus_slider_set(GTK_WIDGET(w), value); // restore value (and move min/max again if needed)
    return;
  }

  // make sure current value still in zoomed range
  const float min_visible = powf(10.0f, -d->digits) / d->factor;
  const float multiplier = powf(2.0f, zoom/2);
  const float new_min = value - multiplier * (value - d->min);
  const float new_max = value + multiplier * (d->max - value);
  if(new_min >= d->hard_min
      && new_max <= d->hard_max
      && new_max - new_min >= min_visible * 10)
  {
    d->min = new_min;
    d->max = new_max;
  }

  gtk_widget_queue_draw(GTK_WIDGET(w));
}

static void _slider_zoom_toast(dt_bauhaus_widget_t *w)
{
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  gchar *min_text = dt_bauhaus_slider_get_text(GTK_WIDGET(w), d->factor > 0 ? d->min : d->max);
  gchar *max_text = dt_bauhaus_slider_get_text(GTK_WIDGET(w), d->factor > 0 ? d->max : d->min);
  dt_action_widget_toast(w->module, GTK_WIDGET(w), "\n[%s , %s]", min_text, max_text);
  g_free(min_text);
  g_free(max_text);
}

static gboolean dt_bauhaus_popup_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  int delta_y = 0;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    if(darktable.bauhaus->current->type == DT_BAUHAUS_COMBOBOX)
      combobox_popup_scroll(delta_y);
    else
    {
      _slider_zoom_range(darktable.bauhaus->current, delta_y);
      gtk_widget_queue_draw(widget);
    }
  }
  return TRUE;
}

static gboolean dt_bauhaus_popup_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  gtk_widget_queue_draw(darktable.bauhaus->popup_area);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  const GtkBorder *padding = darktable.bauhaus->popup_padding;
  const int width = allocation.width - padding->left - padding->right;
  const int height = allocation.height - padding->top - padding->bottom;
  const int ht = darktable.bauhaus->line_height + INNER_PADDING * 2.0f;

  gint wx, wy;
  GdkWindow *window = gtk_widget_get_window(darktable.bauhaus->popup_window);
  gdk_window_get_origin(window, &wx, &wy);

  const float tol = 50;
  if(event->x_root > wx + allocation.width + tol || event->y_root > wy + allocation.height + tol
     || event->x_root < (int)wx - tol || event->y_root < (int)wy - tol)
  {
    dt_bauhaus_widget_reject(darktable.bauhaus->current);
    dt_bauhaus_hide_popup();
    return TRUE;
  }

  const float ex = event->x_root - wx - padding->left;
  const float ey = event->y_root - wy - padding->top;

  if(darktable.bauhaus->keys_cnt == 0) _stop_cursor();

  dt_bauhaus_widget_t *w = darktable.bauhaus->current;
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
    {
      GdkRectangle workarea;
      gdk_monitor_get_workarea(gdk_display_get_monitor_at_window(gdk_window_get_display(window), window), &workarea);
      const gint workarea_bottom = workarea.y + workarea.height;

      float dy = 0;
      const float move = darktable.bauhaus->mouse_y - ey;
      if(move > 0 && wy < workarea.y)
      {
        dy = (workarea.y - wy);
        if(event->y_root >= workarea.y)
          dy *= move / (darktable.bauhaus->mouse_y + wy + padding->top - workarea.y);
      }
      if(move < 0 && wy + allocation.height > workarea_bottom)
      {
        dy = (workarea_bottom - wy - allocation.height);
        if(event->y_root <= workarea_bottom)
          dy *= move / (darktable.bauhaus->mouse_y + wy + padding->top - workarea_bottom);
      }

      darktable.bauhaus->mouse_x = ex;
      darktable.bauhaus->mouse_y = ey - dy;
      gdk_window_move(window, wx, wy + dy);

      break;
    }
    case DT_BAUHAUS_SLIDER:
    {
      const dt_bauhaus_slider_data_t *d = &w->data.slider;
      const float mouse_off
          = get_slider_line_offset(d->oldpos, 5.0 * powf(10.0f, -d->digits) / (d->max - d->min) / d->factor,
                                   ex / width, ey / height, ht / (float)height, allocation.width, w);
      if(!darktable.bauhaus->change_active)
      {
        if((darktable.bauhaus->mouse_line_distance < 0 && mouse_off >= 0)
           || (darktable.bauhaus->mouse_line_distance > 0 && mouse_off <= 0))
          darktable.bauhaus->change_active = 1;
        darktable.bauhaus->mouse_line_distance = mouse_off;
      }
      if(darktable.bauhaus->change_active)
      {
        // remember mouse position for motion effects in draw
        darktable.bauhaus->mouse_x = ex;
        darktable.bauhaus->mouse_y = ey;
        dt_bauhaus_slider_set_normalized(w, d->oldpos + mouse_off);
      }
      break;
    }
    default:
      break;
  }
  return TRUE;
}

static gboolean dt_bauhaus_popup_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_NORMAL, TRUE);
  return TRUE;
}

static gboolean dt_bauhaus_popup_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  int delay = 0;
  g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &delay, NULL);

  if(darktable.bauhaus->current && (darktable.bauhaus->current->type == DT_BAUHAUS_COMBOBOX)
     && (event->button == 1) && (event->time >= darktable.bauhaus->opentime + delay) && !darktable.bauhaus->hiding)
  {
    gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_ACTIVE, TRUE);

    // event might be in wrong system, transform ourselves:
    gint wx, wy, x, y;
    gdk_window_get_origin(gtk_widget_get_window(darktable.bauhaus->popup_window), &wx, &wy);

    gdk_device_get_position(
        gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(widget))), 0, &x, &y);
    darktable.bauhaus->end_mouse_x = x - wx;
    darktable.bauhaus->end_mouse_y = y - wy;
    const dt_bauhaus_combobox_data_t *d = &darktable.bauhaus->current->data.combobox;
    if(!d->mute_scrolling)
      dt_bauhaus_widget_accept(darktable.bauhaus->current);
    dt_bauhaus_hide_popup();
  }
  else if(darktable.bauhaus->hiding)
  {
    dt_bauhaus_hide_popup();
  }
  return TRUE;
}

static gboolean dt_bauhaus_popup_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->window != gtk_widget_get_window(widget))
  {
    dt_bauhaus_widget_reject(darktable.bauhaus->current);
    dt_bauhaus_hide_popup();
    return TRUE;
  }

  int delay = 0;
  g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &delay, NULL);
  if(event->button == 1)
  {
    if(darktable.bauhaus->current->type == DT_BAUHAUS_COMBOBOX
       && event->time < darktable.bauhaus->opentime + delay)
    {
      // counts as double click, reset:
      const dt_bauhaus_combobox_data_t *d = &darktable.bauhaus->current->data.combobox;
      dt_bauhaus_combobox_set(GTK_WIDGET(darktable.bauhaus->current), d->defpos);
      dt_bauhaus_widget_reject(darktable.bauhaus->current);
      gtk_widget_set_state_flags(GTK_WIDGET(darktable.bauhaus->current),
                                 GTK_STATE_FLAG_FOCUSED, FALSE);
    }
    else
    {
      // only accept left mouse click
      const GtkBorder *padding = darktable.bauhaus->popup_padding;
      darktable.bauhaus->end_mouse_x = event->x - padding->left;
      darktable.bauhaus->end_mouse_y = event->y - padding->top;
      dt_bauhaus_widget_accept(darktable.bauhaus->current);
      gtk_widget_set_state_flags(GTK_WIDGET(darktable.bauhaus->current),
                                 GTK_STATE_FLAG_FOCUSED, FALSE);
    }
    darktable.bauhaus->hiding = TRUE;
  }
  else if(event->button == 2 && darktable.bauhaus->current->type == DT_BAUHAUS_SLIDER)
  {
    _slider_zoom_range(darktable.bauhaus->current, 0);
    gtk_widget_queue_draw(widget);
  }
  else
  {
    dt_bauhaus_widget_reject(darktable.bauhaus->current);
    darktable.bauhaus->hiding = TRUE;
  }
  return TRUE;
}

static void dt_bauhaus_window_show(GtkWidget *w, gpointer user_data)
{
  // Could grab the popup_area rather than popup_window, but if so
  // then popup_area would get all motion events including those
  // outside of the popup. This way the popup_area gets motion events
  // related to updating the popup, and popup_window gets all others
  // which would be the ones telling it to close the popup.
  gtk_grab_add(GTK_WIDGET(user_data));
}

static void dt_bh_init(DtBauhausWidget *class)
{
  // not sure if we want to use this instead of our code in *_new()
  // TODO: the common code from bauhaus_widget_init() could go here.
}

static gboolean _enter_leave(GtkWidget *widget, GdkEventCrossing *event)
{
  if(event->type == GDK_ENTER_NOTIFY)
    gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_PRELIGHT, FALSE);
  else
    gtk_widget_unset_state_flags(widget, GTK_STATE_FLAG_PRELIGHT);

  gtk_widget_queue_draw(widget);

  return FALSE;
}

static void _widget_finalize(GObject *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type == DT_BAUHAUS_SLIDER)
  {
    dt_bauhaus_slider_data_t *d = &w->data.slider;
    if(d->timeout_handle) g_source_remove(d->timeout_handle);
    free(d->grad_col);
    free(d->grad_pos);
  }
  else
  {
    dt_bauhaus_combobox_data_t *d = &w->data.combobox;
    g_ptr_array_free(d->entries, TRUE);
    free(d->text);
  }
  g_free(w->section);
  gtk_border_free(w->margin);
  gtk_border_free(w->padding);

  G_OBJECT_CLASS(dt_bh_parent_class)->finalize(widget);
}

static void dt_bh_class_init(DtBauhausWidgetClass *class)
{
  darktable.bauhaus->signals[DT_BAUHAUS_VALUE_CHANGED_SIGNAL]
      = g_signal_new("value-changed", G_TYPE_FROM_CLASS(class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                     g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
  darktable.bauhaus->signals[DT_BAUHAUS_QUAD_PRESSED_SIGNAL]
      = g_signal_new("quad-pressed", G_TYPE_FROM_CLASS(class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                     g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);
  widget_class->draw = _widget_draw;
  widget_class->scroll_event = _widget_scroll;
  widget_class->key_press_event = _widget_key_press;
  widget_class->get_preferred_width = _get_preferred_width;
  widget_class->enter_notify_event = _enter_leave;
  widget_class->leave_notify_event = _enter_leave;
  widget_class->style_updated = _style_updated;
  G_OBJECT_CLASS(class)->finalize = _widget_finalize;
}

void dt_bauhaus_load_theme()
{
  darktable.bauhaus->line_height = 9;
  darktable.bauhaus->marker_size = 0.25f;

  GtkWidget *root_window = dt_ui_main_window(darktable.gui->ui);
  GtkStyleContext *ctx = gtk_style_context_new();
  GtkWidgetPath *path = gtk_widget_path_new();
  const int pos = gtk_widget_path_append_type(path, GTK_TYPE_WIDGET);
  gtk_widget_path_iter_add_class(path, pos, "dt_bauhaus");
  gtk_style_context_set_path(ctx, path);
  gtk_style_context_set_screen (ctx, gtk_widget_get_screen(root_window));

  gtk_style_context_lookup_color(ctx, "bauhaus_fg", &darktable.bauhaus->color_fg);
  gtk_style_context_lookup_color(ctx, "bauhaus_fg_insensitive", &darktable.bauhaus->color_fg_insensitive);
  gtk_style_context_lookup_color(ctx, "bauhaus_bg", &darktable.bauhaus->color_bg);
  gtk_style_context_lookup_color(ctx, "bauhaus_border", &darktable.bauhaus->color_border);
  gtk_style_context_lookup_color(ctx, "bauhaus_fill", &darktable.bauhaus->color_fill);
  gtk_style_context_lookup_color(ctx, "bauhaus_indicator_border", &darktable.bauhaus->indicator_border);

  gtk_style_context_lookup_color(ctx, "graph_bg", &darktable.bauhaus->graph_bg);
  gtk_style_context_lookup_color(ctx, "graph_exterior", &darktable.bauhaus->graph_exterior);
  gtk_style_context_lookup_color(ctx, "graph_border", &darktable.bauhaus->graph_border);
  gtk_style_context_lookup_color(ctx, "graph_grid", &darktable.bauhaus->graph_grid);
  gtk_style_context_lookup_color(ctx, "graph_fg", &darktable.bauhaus->graph_fg);
  gtk_style_context_lookup_color(ctx, "graph_fg_active", &darktable.bauhaus->graph_fg_active);
  gtk_style_context_lookup_color(ctx, "graph_overlay", &darktable.bauhaus->graph_overlay);
  gtk_style_context_lookup_color(ctx, "inset_histogram", &darktable.bauhaus->inset_histogram);
  gtk_style_context_lookup_color(ctx, "graph_red", &darktable.bauhaus->graph_colors[0]);
  gtk_style_context_lookup_color(ctx, "graph_green", &darktable.bauhaus->graph_colors[1]);
  gtk_style_context_lookup_color(ctx, "graph_blue", &darktable.bauhaus->graph_colors[2]);
  gtk_style_context_lookup_color(ctx, "colorlabel_red",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_RED]);
  gtk_style_context_lookup_color(ctx, "colorlabel_yellow",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_YELLOW]);
  gtk_style_context_lookup_color(ctx, "colorlabel_green",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_GREEN]);
  gtk_style_context_lookup_color(ctx, "colorlabel_blue",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_BLUE]);
  gtk_style_context_lookup_color(ctx, "colorlabel_purple",
                                 &darktable.bauhaus->colorlabels[DT_COLORLABELS_PURPLE]);

  PangoFontDescription *pfont = 0;
  gtk_style_context_get(ctx, GTK_STATE_FLAG_NORMAL, "font", &pfont, NULL);

  // make sure we release previously loaded font
  if(darktable.bauhaus->pango_font_desc)
    pango_font_description_free(darktable.bauhaus->pango_font_desc);

  darktable.bauhaus->pango_font_desc = pfont;

  if(darktable.bauhaus->pango_sec_font_desc)
    pango_font_description_free(darktable.bauhaus->pango_sec_font_desc);

  // now get the font for the section labels
  gtk_widget_path_iter_add_class(path, pos, "dt_section_label");
  gtk_style_context_set_path(ctx, path);
  gtk_style_context_get(ctx, GTK_STATE_FLAG_NORMAL, "font", &pfont, NULL);
  darktable.bauhaus->pango_sec_font_desc = pfont;

  gtk_widget_path_free(path);

  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 128, 128);
  cairo_t *cr = cairo_create(cst);
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_text(layout, "m", -1);
  pango_layout_set_font_description(layout, darktable.bauhaus->pango_font_desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
  int pango_width, pango_height;
  pango_layout_get_size(layout, &pango_width, &pango_height);
  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_surface_destroy(cst);

  darktable.bauhaus->line_height = pango_height / PANGO_SCALE;
  darktable.bauhaus->quad_width = darktable.bauhaus->line_height;

  darktable.bauhaus->baseline_size = darktable.bauhaus->line_height / 2.5f; // absolute size in Cairo unit
  darktable.bauhaus->border_width = 2.0f; // absolute size in Cairo unit
  darktable.bauhaus->marker_size = (darktable.bauhaus->baseline_size + darktable.bauhaus->border_width) * 0.9f;
}

void dt_bauhaus_init()
{
  darktable.bauhaus = (dt_bauhaus_t *)calloc(1, sizeof(dt_bauhaus_t));
  darktable.bauhaus->keys_cnt = 0;
  darktable.bauhaus->current = NULL;
  darktable.bauhaus->popup_area = gtk_drawing_area_new();
  darktable.bauhaus->pango_font_desc = NULL;

  dt_bauhaus_load_theme();

  darktable.bauhaus->skip_accel = 1;

  // this easily gets keyboard input:
  // darktable.bauhaus->popup_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  // but this doesn't flicker, and the above hack with key input seems to work well.
  darktable.bauhaus->popup_window = gtk_window_new(GTK_WINDOW_POPUP);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(darktable.bauhaus->popup_window);
#endif
  // this is needed for popup, not for toplevel.
  // since popup_area gets the focus if we show the window, this is all
  // we need.

  gtk_window_set_resizable(GTK_WINDOW(darktable.bauhaus->popup_window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(darktable.bauhaus->popup_window), 260, 260);
  gtk_window_set_transient_for(GTK_WINDOW(darktable.bauhaus->popup_window),
                               GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  // gtk_window_set_modal(GTK_WINDOW(c->popup_window), TRUE);
  // gtk_window_set_decorated(GTK_WINDOW(c->popup_window), FALSE);

  // for pie menu:
  // gtk_window_set_position(GTK_WINDOW(c->popup_window), GTK_WIN_POS_MOUSE);// | GTK_WIN_POS_CENTER);

  // needed on macOS to avoid fullscreening the popup with newer GTK
  gtk_window_set_type_hint(GTK_WINDOW(darktable.bauhaus->popup_window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);

  gtk_container_add(GTK_CONTAINER(darktable.bauhaus->popup_window), darktable.bauhaus->popup_area);
  gtk_widget_set_hexpand(darktable.bauhaus->popup_area, TRUE);
  gtk_widget_set_vexpand(darktable.bauhaus->popup_area, TRUE);
  // gtk_window_set_title(GTK_WINDOW(c->popup_window), _("dtgtk control popup"));
  gtk_window_set_keep_above(GTK_WINDOW(darktable.bauhaus->popup_window), TRUE);
  gtk_window_set_gravity(GTK_WINDOW(darktable.bauhaus->popup_window), GDK_GRAVITY_STATIC);

  gtk_widget_set_can_focus(darktable.bauhaus->popup_area, TRUE);
  gtk_widget_add_events(darktable.bauhaus->popup_area, GDK_POINTER_MOTION_MASK
                                                       | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                       | GDK_KEY_PRESS_MASK | GDK_LEAVE_NOTIFY_MASK
                                                       | darktable.gui->scroll_mask);

  GObject *window = G_OBJECT(darktable.bauhaus->popup_window), *area = G_OBJECT(darktable.bauhaus->popup_area);
  g_signal_connect(window, "show", G_CALLBACK(dt_bauhaus_window_show), area);
  g_signal_connect(area, "draw", G_CALLBACK(dt_bauhaus_popup_draw), NULL);
  g_signal_connect(area, "motion-notify-event", G_CALLBACK(dt_bauhaus_popup_motion_notify), NULL);
  g_signal_connect(area, "leave-notify-event", G_CALLBACK(dt_bauhaus_popup_leave_notify), NULL);
  g_signal_connect(area, "button-press-event", G_CALLBACK(dt_bauhaus_popup_button_press), NULL);
  g_signal_connect(area, "button-release-event", G_CALLBACK (dt_bauhaus_popup_button_release), NULL);
  g_signal_connect(area, "key-press-event", G_CALLBACK(dt_bauhaus_popup_key_press), NULL);
  g_signal_connect(area, "scroll-event", G_CALLBACK(dt_bauhaus_popup_scroll), NULL);
}

void dt_bauhaus_cleanup()
{
}

// fwd declare a few callbacks
static gboolean dt_bauhaus_slider_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_bauhaus_slider_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_bauhaus_slider_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);


static gboolean dt_bauhaus_combobox_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_bauhaus_combobox_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);

// static gboolean
// dt_bauhaus_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);

// end static init/cleanup
// =================================================



// common initialization
static void _bauhaus_widget_init(dt_bauhaus_widget_t *w, dt_iop_module_t *self)
{
  w->module = DT_ACTION(self);
  w->field = NULL;

  w->section = NULL;

  // no quad icon and no toggle button:
  w->quad_paint = 0;
  w->quad_paint_data = NULL;
  w->quad_toggle = 0;
  w->show_quad = TRUE;
  w->show_label = TRUE;

  gtk_widget_add_events(GTK_WIDGET(w), GDK_POINTER_MOTION_MASK
                                       | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                       | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                       | GDK_FOCUS_CHANGE_MASK
                                       | darktable.gui->scroll_mask);

  gtk_widget_set_can_focus(GTK_WIDGET(w), TRUE);
  dt_gui_add_class(GTK_WIDGET(w), "dt_bauhaus");
}

void dt_bauhaus_combobox_set_default(GtkWidget *widget, int def)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->defpos = def;
}

int dt_bauhaus_combobox_get_default(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  return d->defpos;
}

void dt_bauhaus_slider_set_hard_min(GtkWidget* widget, float val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float pos = dt_bauhaus_slider_get(widget);
  d->hard_min = val;
  d->min = MAX(d->min, d->hard_min);
  d->soft_min = MAX(d->soft_min, d->hard_min);

  if(val > d->hard_max) dt_bauhaus_slider_set_hard_max(widget,val);
  if(pos < val)
  {
    dt_bauhaus_slider_set(widget,val);
  }
  else
  {
    dt_bauhaus_slider_set(widget,pos);
  }
}

float dt_bauhaus_slider_get_hard_min(GtkWidget* widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->hard_min;
}

void dt_bauhaus_slider_set_hard_max(GtkWidget* widget, float val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float pos = dt_bauhaus_slider_get(widget);
  d->hard_max = val;
  d->max = MIN(d->max, d->hard_max);
  d->soft_max = MIN(d->soft_max, d->hard_max);

  if(val < d->hard_min) dt_bauhaus_slider_set_hard_min(widget,val);
  if(pos > val)
  {
    dt_bauhaus_slider_set(widget,val);
  }
  else
  {
    dt_bauhaus_slider_set(widget,pos);
  }
}

float dt_bauhaus_slider_get_hard_max(GtkWidget* widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->hard_max;
}

void dt_bauhaus_slider_set_soft_min(GtkWidget* widget, float val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float oldval = dt_bauhaus_slider_get(widget);
  d->min = d->soft_min = CLAMP(val,d->hard_min,d->hard_max);
  dt_bauhaus_slider_set(widget,oldval);
}

float dt_bauhaus_slider_get_soft_min(GtkWidget* widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->soft_min;
}

void dt_bauhaus_slider_set_soft_max(GtkWidget* widget, float val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float oldval = dt_bauhaus_slider_get(widget);
  d->max = d->soft_max = CLAMP(val,d->hard_min,d->hard_max);
  dt_bauhaus_slider_set(widget,oldval);
}

float dt_bauhaus_slider_get_soft_max(GtkWidget* widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->soft_max;
}

void dt_bauhaus_slider_set_default(GtkWidget *widget, float def)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->defpos = def;
}

void dt_bauhaus_slider_set_soft_range(GtkWidget *widget, float soft_min, float soft_max)
{
  dt_bauhaus_slider_set_soft_min(widget,soft_min);
  dt_bauhaus_slider_set_soft_max(widget,soft_max);
}

float dt_bauhaus_slider_get_default(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->defpos;
}

extern const dt_action_def_t dt_action_def_slider;
extern const dt_action_def_t dt_action_def_combo;

dt_action_t *dt_bauhaus_widget_set_label(GtkWidget *widget, const char *section, const char *label)
{
  dt_action_t *ac = NULL;
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  memset(w->label, 0, sizeof(w->label)); // keep valgrind happy
  if(label) g_strlcpy(w->label, Q_(label), sizeof(w->label));
  if(section) w->section = g_strdup(Q_(section));

  if(w->module)
  {
    if(!darktable.bauhaus->skip_accel || w->module->type != DT_ACTION_TYPE_IOP_INSTANCE)
    {
      ac = dt_action_define(w->module, section, label, widget,
                            w->type == DT_BAUHAUS_SLIDER ? &dt_action_def_slider : &dt_action_def_combo);
      if(w->module->type != DT_ACTION_TYPE_IOP_INSTANCE) w->module = ac;
    }

    // if new bauhaus widget added to front of widget_list; move it to the back
    dt_iop_module_t *m = (dt_iop_module_t *)w->module;
    if(w->module->type == DT_ACTION_TYPE_IOP_INSTANCE &&
       w->field && m->widget_list && ((dt_action_target_t *)m->widget_list->data)->target == (gpointer)widget)
    {
      if(!m->widget_list_bh)
      {
        m->widget_list_bh = m->widget_list;
        if(m->widget_list->next)
        {
          GSList *last = g_slist_last(m->widget_list);
          last->next = m->widget_list;
          m->widget_list = m->widget_list->next;
          last->next->next = NULL;
        }
      }
      else
      {
        GSList *first = m->widget_list->next;
        m->widget_list->next = m->widget_list_bh->next;
        m->widget_list_bh->next = m->widget_list;
        m->widget_list = first;
      }
    }

    gtk_widget_queue_draw(GTK_WIDGET(w));
  }
  return ac;
}

const char* dt_bauhaus_widget_get_label(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  return w->label;
}

void dt_bauhaus_widget_set_quad_paint(GtkWidget *widget, dt_bauhaus_quad_paint_f f, int paint_flags, void *paint_data)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->quad_paint = f;
  w->quad_paint_flags = paint_flags;
  w->quad_paint_data = paint_data;
}

void dt_bauhaus_widget_set_field(GtkWidget *widget, gpointer field, dt_introspection_type_t field_type)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(*w->label)
    fprintf(stderr, "[dt_bauhaus_widget_set_field] bauhaus label '%s' set before field (needs to be after)\n", w->label);
  w->field = field;
  w->field_type = field_type;
}

static void _highlight_changed_notebook_tab(GtkWidget *w, gpointer user_data)
{
  GtkWidget *notebook = gtk_widget_get_parent(w);
  if(!GTK_IS_NOTEBOOK(notebook))
  {
    w = notebook;
    if(!notebook || !(GTK_IS_NOTEBOOK(notebook = gtk_widget_get_parent(notebook))))
      return;
  }

  gboolean is_changed = GPOINTER_TO_INT(user_data);

  for(GList *c = gtk_container_get_children(GTK_CONTAINER(w)); c; c = g_list_delete_link(c, c))
  {
    if(!is_changed && DT_IS_BAUHAUS_WIDGET(c->data) && gtk_widget_get_visible(c->data))
    {
      dt_bauhaus_widget_t *b = DT_BAUHAUS_WIDGET(c->data);
      if(!b->field) continue;
      if(b->type == DT_BAUHAUS_SLIDER)
      {
        dt_bauhaus_slider_data_t *d = &b->data.slider;
        is_changed = fabsf(d->pos - d->curve((d->defpos - d->min) / (d->max - d->min), DT_BAUHAUS_SET)) > 0.001f;
      }
      else
        is_changed = b->data.combobox.entries->len && b->data.combobox.active != b->data.combobox.defpos;
    }
  }

  GtkWidget *label = gtk_notebook_get_tab_label(GTK_NOTEBOOK(notebook), w);

  if(is_changed)
    dt_gui_add_class(label, "changed");
  else
    dt_gui_remove_class(label, "changed");
}

void dt_bauhaus_update_module(dt_iop_module_t *self)
{
  GtkWidget *n = NULL;
  for(GSList *w = self->widget_list_bh; w; w = w->next)
  {
    dt_action_target_t *at = w->data;
    GtkWidget *widget = at->target;
    dt_bauhaus_widget_t *bhw = DT_BAUHAUS_WIDGET(widget);
    if(!bhw) continue;

    switch(bhw->type)
    {
      case DT_BAUHAUS_SLIDER:
        switch(bhw->field_type)
        {
          case DT_INTROSPECTION_TYPE_FLOAT:
            dt_bauhaus_slider_set(widget, *(float *)bhw->field);
            break;
          case DT_INTROSPECTION_TYPE_INT:
            dt_bauhaus_slider_set(widget, *(int *)bhw->field);
            break;
          case DT_INTROSPECTION_TYPE_USHORT:
            dt_bauhaus_slider_set(widget, *(unsigned short *)bhw->field);
            break;
          default:
            fprintf(stderr, "[dt_bauhaus_update_module] unsupported slider data type\n");
        }
        break;
      case DT_BAUHAUS_COMBOBOX:
        switch(bhw->field_type)
        {
          case DT_INTROSPECTION_TYPE_ENUM:
            dt_bauhaus_combobox_set_from_value(widget, *(int *)bhw->field);
            break;
          case DT_INTROSPECTION_TYPE_INT:
            dt_bauhaus_combobox_set(widget, *(int *)bhw->field);
            break;
          case DT_INTROSPECTION_TYPE_UINT:
            dt_bauhaus_combobox_set(widget, *(unsigned int *)bhw->field);
            break;
          case DT_INTROSPECTION_TYPE_BOOL:
            dt_bauhaus_combobox_set(widget, *(gboolean *)bhw->field);
            break;
          default:
            fprintf(stderr, "[dt_bauhaus_update_module] unsupported combo data type\n");
        }
        break;
      default:
        fprintf(stderr, "[dt_bauhaus_update_module] invalid bauhaus widget type encountered\n");
    }

    if(!n && (n = gtk_widget_get_parent(widget)) && (n = gtk_widget_get_parent(n)) && !GTK_IS_NOTEBOOK(n)) n = NULL;
  }

  if(n) gtk_container_foreach(GTK_CONTAINER(n), _highlight_changed_notebook_tab, NULL);
}

// make this quad a toggle button:
void dt_bauhaus_widget_set_quad_toggle(GtkWidget *widget, int toggle)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->quad_toggle = toggle;
}

void dt_bauhaus_widget_set_quad_active(GtkWidget *widget, int active)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(active)
    w->quad_paint_flags |= CPF_ACTIVE;
  else
    w->quad_paint_flags &= ~CPF_ACTIVE;
  gtk_widget_queue_draw(GTK_WIDGET(w));
}

void dt_bauhaus_widget_set_quad_visibility(GtkWidget *widget, const gboolean visible)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->show_quad = visible;
  gtk_widget_queue_draw(GTK_WIDGET(w));
}

int dt_bauhaus_widget_get_quad_active(GtkWidget *widget)
{
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  return (w->quad_paint_flags & CPF_ACTIVE) == CPF_ACTIVE;
}

void dt_bauhaus_widget_press_quad(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->quad_toggle)
  {
    w->quad_paint_flags ^= CPF_ACTIVE;
  }
  else
    w->quad_paint_flags |= CPF_ACTIVE;

  g_signal_emit_by_name(G_OBJECT(w), "quad-pressed");
}

void dt_bauhaus_widget_release_quad(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(!w->quad_toggle)
  {
    if(w->quad_paint_flags & CPF_ACTIVE)
      w->quad_paint_flags &= ~CPF_ACTIVE;
    gtk_widget_queue_draw(GTK_WIDGET(w));
  }
}

static float _default_linear_curve(float value, dt_bauhaus_curve_t dir)
{
  // regardless of dir: input <-> output
  return value;
}

static float _reverse_linear_curve(float value, dt_bauhaus_curve_t dir)
{
  // regardless of dir: input <-> output
  return 1.0 - value;
}

GtkWidget *dt_bauhaus_slider_new(dt_iop_module_t *self)
{
  return dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.1, 0.5, 3);
}

GtkWidget *dt_bauhaus_slider_new_with_range(dt_iop_module_t *self, float min, float max, float step,
                                            float defval, int digits)
{
  return dt_bauhaus_slider_new_with_range_and_feedback(self, min, max, step, defval, digits, 1);
}

GtkWidget *dt_bauhaus_slider_new_action(dt_action_t *self, float min, float max, float step,
                                        float defval, int digits)
{
  return dt_bauhaus_slider_new_with_range((dt_iop_module_t *)self, min, max, step, defval, digits);
}

GtkWidget *dt_bauhaus_slider_new_with_range_and_feedback(dt_iop_module_t *self, float min, float max,
                                                         float step, float defval, int digits, int feedback)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(g_object_new(DT_BAUHAUS_WIDGET_TYPE, NULL));
  return dt_bauhaus_slider_from_widget(w,self, min, max, step, defval, digits, feedback);
}

static void _style_updated(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  _margins_retrieve(w);

  if(w->type == DT_BAUHAUS_COMBOBOX)
  {
    gtk_widget_set_size_request(widget, -1,
                                w->margin->top + w->padding->top + w->margin->bottom + w->padding->bottom
                                    + darktable.bauhaus->line_height);
  }
  else if(w->type == DT_BAUHAUS_SLIDER)
  {
    // the lower thing to draw is indicator. See dt_bauhaus_draw_baseline for compute details
    gtk_widget_set_size_request(widget, -1,
                                w->margin->top + w->padding->top + w->margin->bottom + w->padding->bottom
                                    + INNER_PADDING + darktable.bauhaus->baseline_size
                                    + darktable.bauhaus->line_height + 1.5f * darktable.bauhaus->border_width);
  }
}

GtkWidget *dt_bauhaus_slider_from_widget(dt_bauhaus_widget_t* w,dt_iop_module_t *self, float min, float max,
                                                         float step, float defval, int digits, int feedback)
{
  w->type = DT_BAUHAUS_SLIDER;
  _bauhaus_widget_init(w, self);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->min = d->soft_min = d->hard_min = min;
  d->max = d->soft_max = d->hard_max = max;
  d->step = step;
  // normalize default:
  d->defpos = defval;
  d->pos = (defval - min) / (max - min);
  d->oldpos = d->pos;
  d->digits = digits;
  d->format = "";
  d->factor = 1.0f;
  d->offset = 0.0f;

  d->grad_cnt = 0;
  d->grad_col = NULL;
  d->grad_pos = NULL;

  d->fill_feedback = feedback;

  d->is_dragging = 0;
  d->is_changed = 0;
  d->timeout_handle = 0;
  d->curve = _default_linear_curve;

  gtk_widget_set_name(GTK_WIDGET(w), "bauhaus-slider");

  g_signal_connect(G_OBJECT(w), "button-press-event", G_CALLBACK(dt_bauhaus_slider_button_press), NULL);
  g_signal_connect(G_OBJECT(w), "button-release-event", G_CALLBACK(dt_bauhaus_slider_button_release), NULL);
  g_signal_connect(G_OBJECT(w), "motion-notify-event", G_CALLBACK(dt_bauhaus_slider_motion_notify), NULL);
  return GTK_WIDGET(w);
}

GtkWidget *dt_bauhaus_combobox_new(dt_iop_module_t *self)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(g_object_new(DT_BAUHAUS_WIDGET_TYPE, NULL));
  dt_bauhaus_combobox_from_widget(w,self);
  return GTK_WIDGET(w);
}

GtkWidget *dt_bauhaus_combobox_new_action(dt_action_t *self)
{
  return dt_bauhaus_combobox_new((dt_iop_module_t *)self);
}

GtkWidget *dt_bauhaus_combobox_new_full(dt_action_t *action, const char *section, const char *label, const char *tip,
                                        int pos, GtkCallback callback, gpointer data, const char **texts)
{
  GtkWidget *combo = dt_bauhaus_combobox_new_action(action);
  dt_action_t *ac = dt_bauhaus_widget_set_label(combo, section, label);
  dt_bauhaus_combobox_add_list(combo, ac, texts);
  dt_bauhaus_combobox_set(combo, pos);
  gtk_widget_set_tooltip_text(combo, tip ? tip : _(label));
  if(callback) g_signal_connect(G_OBJECT(combo), "value-changed", G_CALLBACK(callback), data);

  return combo;
}

void dt_bauhaus_combobox_from_widget(dt_bauhaus_widget_t* w,dt_iop_module_t *self)
{
  w->type = DT_BAUHAUS_COMBOBOX;
  _bauhaus_widget_init(w, self);
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->entries = g_ptr_array_new_full(4, free_combobox_entry);
  d->defpos = 0;
  d->active = -1;
  d->editable = 0;
  d->text_align = DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT;
  d->entries_ellipsis = PANGO_ELLIPSIZE_END;
  d->mute_scrolling = FALSE;
  d->populate = NULL;
  d->text = NULL;

  gtk_widget_set_name(GTK_WIDGET(w), "bauhaus-combobox");

  g_signal_connect(G_OBJECT(w), "button-press-event", G_CALLBACK(dt_bauhaus_combobox_button_press), NULL);
  g_signal_connect(G_OBJECT(w), "motion-notify-event", G_CALLBACK(dt_bauhaus_combobox_motion_notify), NULL);
}

static dt_bauhaus_combobox_data_t *_combobox_data(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return NULL;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  if(d->active >= d->entries->len) d->active = -1;

  return d;
}

void dt_bauhaus_combobox_add_populate_fct(GtkWidget *widget, void (*fct)(GtkWidget *w, struct dt_iop_module_t **module))
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type == DT_BAUHAUS_COMBOBOX)
    w->data.combobox.populate = fct;
}

void dt_bauhaus_combobox_add_list(GtkWidget *widget, dt_action_t *action, const char **texts)
{
  if(action)
    g_hash_table_insert(darktable.control->combo_list, action, texts);

  while(texts && *texts)
    dt_bauhaus_combobox_add_full(widget, Q_(*(texts++)), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, NULL, NULL, TRUE);
}

void dt_bauhaus_combobox_add(GtkWidget *widget, const char *text)
{
  dt_bauhaus_combobox_add_full(widget, text, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, NULL, NULL, TRUE);
}

void dt_bauhaus_combobox_add_section(GtkWidget *widget, const char *text)
{
  dt_bauhaus_combobox_add_full(widget, text, DT_BAUHAUS_COMBOBOX_ALIGN_LEFT, NULL, NULL, FALSE);
}

void dt_bauhaus_combobox_add_aligned(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align)
{
  dt_bauhaus_combobox_add_full(widget, text, align, NULL, NULL, TRUE);
}

void dt_bauhaus_combobox_add_full(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align,
                                  gpointer data, void (free_func)(void *data), gboolean sensitive)
{
  if(darktable.control->accel_initialising) return;

  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  dt_bauhaus_combobox_entry_t *entry = new_combobox_entry(text, align, sensitive, data, free_func);
  g_ptr_array_add(d->entries, entry);
  if(d->active < 0) d->active = 0;
}

gboolean dt_bauhaus_combobox_set_entry_label(GtkWidget *widget, const int pos, const gchar *label)
{
  // change the text to show for the entry
  // note that this doesn't break shortcuts but their names in the shortcut panel will remain the initial one
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return FALSE;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  if(!d || pos < 0 || pos >= d->entries->len) return FALSE;
  dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, pos);
  g_free(entry->label);
  entry->label = g_strdup(label);
  return TRUE;
}

void dt_bauhaus_combobox_set_entries_ellipsis(GtkWidget *widget, PangoEllipsizeMode ellipis)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->entries_ellipsis = ellipis;
}

PangoEllipsizeMode dt_bauhaus_combobox_get_entries_ellipsis(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  return d ? d->entries_ellipsis : PANGO_ELLIPSIZE_END;
}

void dt_bauhaus_combobox_set_editable(GtkWidget *widget, int editable)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->editable = editable ? 1 : 0;
  if(d->editable && !d->text)
    d->text = calloc(1, DT_BAUHAUS_COMBO_MAX_TEXT);
}

int dt_bauhaus_combobox_get_editable(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  return d ? d->editable : 0;
}

void dt_bauhaus_combobox_set_selected_text_align(GtkWidget *widget, const dt_bauhaus_combobox_alignment_t text_align)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->text_align = text_align;
}

void dt_bauhaus_combobox_remove_at(GtkWidget *widget, int pos)
{
  dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  if(!d || pos < 0 || pos >= d->entries->len) return;

  // move active position up if removing anything before it
  // or when removing last position that is currently active.
  // this also sets active to -1 when removing the last remaining entry in a combobox.
  if(d->active > pos || d->active == d->entries->len-1)
    d->active--;

  g_ptr_array_remove_index(d->entries, pos);
}

void dt_bauhaus_combobox_insert(GtkWidget *widget, const char *text,int pos)
{
  dt_bauhaus_combobox_insert_full(widget, text, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, NULL, NULL, pos);
}

void dt_bauhaus_combobox_insert_full(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align,
                                     gpointer data, void (*free_func)(void *), int pos)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  g_ptr_array_insert(d->entries, pos, new_combobox_entry(text, align, TRUE, data, free_func));
  if(d->active < 0) d->active = 0;
}

int dt_bauhaus_combobox_length(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  return d ? d->entries->len : 0;
}

const char *dt_bauhaus_combobox_get_text(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d) return NULL;

  if(d->active < 0)
  {
    return d->editable ? d->text : NULL;
  }
  else
  {
    const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, d->active);
    return entry->label;
  }
}

gpointer dt_bauhaus_combobox_get_data(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d || d->active < 0) return NULL;

  const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, d->active);
  return entry->data;
}

void dt_bauhaus_combobox_clear(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->active = -1;
  g_ptr_array_set_size(d->entries, 0);
}

const char *dt_bauhaus_combobox_get_entry(GtkWidget *widget, int pos)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d || pos < 0 || pos >= d->entries->len) return NULL;

  const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, pos);
  return entry->label;
}

void dt_bauhaus_combobox_set_text(GtkWidget *widget, const char *text)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d || !d->editable) return;

  g_strlcpy(d->text, text, DT_BAUHAUS_COMBO_MAX_TEXT);
}

static void _bauhaus_combobox_set(dt_bauhaus_widget_t *w, const int pos, const gboolean mute)
{
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->active = CLAMP(pos, -1, (int)d->entries->len - 1);
  gtk_widget_queue_draw(GTK_WIDGET(w));

  if(!darktable.gui->reset && !mute)
  {
    if(w->field)
    {
      switch(w->field_type)
      {
        case DT_INTROSPECTION_TYPE_ENUM:;
          if(d->active >= 0)
          {
            const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, d->active);
            int *e = w->field, preve = *e; *e = GPOINTER_TO_INT(entry->data);
            if(*e != preve) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &preve);
          }
          break;
        case DT_INTROSPECTION_TYPE_INT:;
          int *i = w->field, previ = *i; *i = d->active;
          if(*i != previ) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &previ);
          break;
        case DT_INTROSPECTION_TYPE_UINT:;
          unsigned int *u = w->field, prevu = *u; *u = d->active;
          if(*u != prevu) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &prevu);
          break;
        case DT_INTROSPECTION_TYPE_BOOL:;
          gboolean *b = w->field, prevb = *b; *b = d->active;
          if(*b != prevb) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &prevb);
          break;
        default:
          fprintf(stderr, "[_bauhaus_combobox_set] unsupported combo data type\n");
      }
    }
    _highlight_changed_notebook_tab(GTK_WIDGET(w), GINT_TO_POINTER(d->active != d->defpos));
    g_signal_emit_by_name(G_OBJECT(w), "value-changed");
  }
}

void dt_bauhaus_combobox_set(GtkWidget *widget, const int pos)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  _bauhaus_combobox_set(w, pos, FALSE);
}

gboolean dt_bauhaus_combobox_set_from_text(GtkWidget *widget, const char *text)
{
  if(!text) return FALSE;

  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  for(int i = 0; d && i < d->entries->len; i++)
  {
    const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, i);
    if(!g_strcmp0(entry->label, text))
    {
      dt_bauhaus_combobox_set(widget, i);
      return TRUE;
    }
  }
  return FALSE;
}

gboolean dt_bauhaus_combobox_set_from_value(GtkWidget *widget, int value)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  for(int i = 0; d && i < d->entries->len; i++)
  {
    const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, i);
    if(GPOINTER_TO_INT(entry->data) == value)
    {
      dt_bauhaus_combobox_set(widget, i);
      return TRUE;
    }
  }
  return FALSE;
}

int dt_bauhaus_combobox_get(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  return d ? d->active : -1;
}

void dt_bauhaus_combobox_entry_set_sensitive(GtkWidget *widget, int pos, gboolean sensitive)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d || pos < 0 || pos >= d->entries->len) return;

  dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)g_ptr_array_index(d->entries, pos);
  entry->sensitive = sensitive;
}

void dt_bauhaus_slider_clear_stops(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->grad_cnt = 0;
}

void dt_bauhaus_slider_set_stop(GtkWidget *widget, float stop, float r, float g, float b)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  if(!d->grad_col)
  {
    d->grad_col = malloc(DT_BAUHAUS_SLIDER_MAX_STOPS * sizeof(*d->grad_col));
    d->grad_pos = malloc(DT_BAUHAUS_SLIDER_MAX_STOPS * sizeof(*d->grad_pos));
  }
  // need to replace stop?
  for(int k = 0; k < d->grad_cnt; k++)
  {
    if(d->grad_pos[k] == stop)
    {
      d->grad_col[k][0] = r;
      d->grad_col[k][1] = g;
      d->grad_col[k][2] = b;
      return;
    }
  }
  // new stop:
  if(d->grad_cnt < DT_BAUHAUS_SLIDER_MAX_STOPS)
  {
    int k = d->grad_cnt++;
    d->grad_pos[k] = stop;
    d->grad_col[k][0] = r;
    d->grad_col[k][1] = g;
    d->grad_col[k][2] = b;
  }
  else
  {
    fprintf(stderr, "[bauhaus_slider_set_stop] only %d stops allowed.\n", DT_BAUHAUS_SLIDER_MAX_STOPS);
  }
}

static void draw_equilateral_triangle(cairo_t *cr, float radius)
{
  const float sin = 0.866025404 * radius;
  const float cos = 0.5f * radius;
  cairo_move_to(cr, 0.0, radius);
  cairo_line_to(cr, -sin, -cos);
  cairo_line_to(cr, sin, -cos);
  cairo_line_to(cr, 0.0, radius);
}

static void dt_bauhaus_draw_indicator(dt_bauhaus_widget_t *w, float pos, cairo_t *cr, float wd, const GdkRGBA fg_color, const GdkRGBA border_color)
{
  // draw scale indicator (the tiny triangle)
  if(w->type != DT_BAUHAUS_SLIDER) return;

  const float border_width = darktable.bauhaus->border_width;
  const float size = darktable.bauhaus->marker_size;

  cairo_save(cr);
  cairo_translate(cr, slider_coordinate(pos, wd, w),
                  darktable.bauhaus->line_height + INNER_PADDING
                      + (darktable.bauhaus->baseline_size - border_width) / 2.0f);
  cairo_scale(cr, 1.0f, -1.0f);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  // draw the outer triangle
  draw_equilateral_triangle(cr, size);
  cairo_set_line_width(cr, border_width);
  set_color(cr, border_color);
  cairo_stroke(cr);

  draw_equilateral_triangle(cr, size - border_width);
  cairo_clip(cr);

  // draw the inner triangle
  draw_equilateral_triangle(cr, size - border_width);
  set_color(cr, fg_color);
  cairo_set_line_width(cr, border_width);

  const dt_bauhaus_slider_data_t *d = &w->data.slider;

  if(d->fill_feedback)
    cairo_fill(cr); // Plain indicator (regular sliders)
  else
    cairo_stroke(cr);  // Hollow indicator to see a color through it (gradient sliders)

  cairo_restore(cr);
}

static void dt_bauhaus_draw_quad(dt_bauhaus_widget_t *w, cairo_t *cr, const int width, const int height)
{
  if(!w->show_quad) return;
  const gboolean sensitive = gtk_widget_is_sensitive(GTK_WIDGET(w));

  if(w->quad_paint)
  {
    cairo_save(cr);

    if(sensitive && (w->quad_paint_flags & CPF_ACTIVE))
      set_color(cr, darktable.bauhaus->color_fg);
    else
      set_color(cr, darktable.bauhaus->color_fg_insensitive);

    w->quad_paint(cr, width - darktable.bauhaus->quad_width,  // x
                      0.0,                                    // y
                      darktable.bauhaus->quad_width,          // width
                      darktable.bauhaus->quad_width,          // height
                      w->quad_paint_flags, w->quad_paint_data);

    cairo_restore(cr);
  }
  else
  {
    // draw active area square:
    cairo_save(cr);
    if(sensitive)
      set_color(cr, darktable.bauhaus->color_fg);
    else
      set_color(cr, darktable.bauhaus->color_fg_insensitive);
    switch(w->type)
    {
      case DT_BAUHAUS_COMBOBOX:
        cairo_translate(cr, width - darktable.bauhaus->quad_width * .5f, height * .5f);
        GdkRGBA *text_color = default_color_assign();
        GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(w));
        const GtkStateFlags state = gtk_widget_get_state_flags(GTK_WIDGET(w));
        gtk_style_context_get_color(context, state, text_color);
        const float r = darktable.bauhaus->quad_width * .2f;
        cairo_move_to(cr, -r, -r * .5f);
        cairo_line_to(cr, 0, r * .5f);
        cairo_line_to(cr, r, -r * .5f);
        set_color(cr, *text_color);
        cairo_stroke(cr);
        gdk_rgba_free(text_color);
        break;
      case DT_BAUHAUS_SLIDER:
        break;
      default:
        cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);
        cairo_rectangle(cr, width - darktable.bauhaus->quad_width, 0.0, darktable.bauhaus->quad_width, darktable.bauhaus->quad_width);
        cairo_fill(cr);
        break;
    }
    cairo_restore(cr);
  }
}

static void dt_bauhaus_draw_baseline(dt_bauhaus_widget_t *w, cairo_t *cr, float width)
{
  // draw line for orientation in slider
  if(w->type != DT_BAUHAUS_SLIDER) return;

  const float slider_width = width - _widget_get_quad_width(w);
  cairo_save(cr);
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  // pos of baseline
  const float htm = darktable.bauhaus->line_height + INNER_PADDING;

  // thickness of baseline
  const float htM = darktable.bauhaus->baseline_size - darktable.bauhaus->border_width;

  // the background of the line
  cairo_pattern_t *gradient = NULL;
  cairo_rectangle(cr, 0, htm, slider_width, htM);

  if(d->grad_cnt > 0)
  {
    // gradient line as used in some modules
    const double zoom = (d->max - d->min) / (d->hard_max - d->hard_min);
    const double offset = (d->min - d->hard_min) / (d->hard_max - d->hard_min);
    gradient = cairo_pattern_create_linear(0, 0, slider_width, htM);
    for(int k = 0; k < d->grad_cnt; k++)
      cairo_pattern_add_color_stop_rgba(gradient, (d->grad_pos[k] - offset) / zoom,
                                        d->grad_col[k][0], d->grad_col[k][1], d->grad_col[k][2], 0.4f);
    cairo_set_source(cr, gradient);
  }
  else
  {
    // regular baseline
    set_color(cr, darktable.bauhaus->color_bg);
  }

  cairo_fill(cr);

  // get the reference of the slider aka the position of the 0 value
  const float origin = fmaxf(fminf((d->factor > 0 ? -d->min - d->offset/d->factor
                                                  :  d->max + d->offset/d->factor)
                                                  / (d->max - d->min), 1.0f) * slider_width, 0.0f);
  const float position = d->pos * slider_width;
  const float delta = position - origin;

  // have a `fill ratio feel' from zero to current position
  // - but only if set
  if(d->fill_feedback)
  {
    // only brighten, useful for colored sliders to not get too faint:
    cairo_set_operator(cr, CAIRO_OPERATOR_SCREEN);
    set_color(cr, darktable.bauhaus->color_fill);
    cairo_rectangle(cr, origin, htm, delta, htM);
    cairo_fill(cr);

    // change back to default cairo operator:
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  }

  // draw the 0 reference graduation if it's different than the bounds of the slider
  const float graduation_top = htm + htM + 2.0f * darktable.bauhaus->border_width;
  const float graduation_height = darktable.bauhaus->border_width / 2.0f;
  set_color(cr, darktable.bauhaus->color_fg);

  // If the max of the slider is 180 or 360, it is likely a hue slider in degrees
  // a zero in periodic stuff has not much meaning so we skip it
  if(d->hard_max != 180.0f && d->hard_max != 360.0f)
  {
    // translate the dot if it overflows the widget frame
    if(origin < graduation_height)
      cairo_arc(cr, graduation_height, graduation_top, graduation_height, 0, 2 * M_PI);
    else if(origin > slider_width - graduation_height)
      cairo_arc(cr, slider_width - graduation_height, graduation_top, graduation_height, 0, 2 * M_PI);
    else
      cairo_arc(cr, origin, graduation_top, graduation_height, 0, 2 * M_PI);
  }

  cairo_fill(cr);
  cairo_restore(cr);

  if(d->grad_cnt > 0) cairo_pattern_destroy(gradient);
}

static void dt_bauhaus_widget_reject(dt_bauhaus_widget_t *w)
{
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
      break;
    case DT_BAUHAUS_SLIDER:
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      dt_bauhaus_slider_set_normalized(w, d->oldpos);
    }
    break;
    default:
      break;
  }
}

static void dt_bauhaus_widget_accept(dt_bauhaus_widget_t *w)
{
  GtkWidget *widget = GTK_WIDGET(w);

  GtkAllocation allocation_popup_window;
  gtk_widget_get_allocation(darktable.bauhaus->popup_window, &allocation_popup_window);
  const GtkBorder *padding = darktable.bauhaus->popup_padding;

  const int width = allocation_popup_window.width - padding->left - padding->right;
  const int height = allocation_popup_window.height - padding->top - padding->bottom;
  const int base_height = darktable.bauhaus->line_height + INNER_PADDING * 2.0f;

  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
    {
      // only set to what's in the filtered list.
      dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      const int top_gap = (w->detached_popup) ? w->top_gap + darktable.bauhaus->line_height : w->top_gap;
      const int active = darktable.bauhaus->end_mouse_y >= 0
                             ? ((darktable.bauhaus->end_mouse_y - top_gap) / darktable.bauhaus->line_height)
                             : d->active;
      int k = 0, i = 0, kk = 0, match = 1;

      gchar *keys = g_utf8_casefold(darktable.bauhaus->keys, -1);
      for(int j = 0; j < d->entries->len; j++)
      {
        const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, j);
        gchar *text_cmp = g_utf8_casefold(entry->label, -1);
        if(!strncmp(text_cmp, keys, darktable.bauhaus->keys_cnt))
        {
          if(active == k)
          {
            if(entry->sensitive)
              dt_bauhaus_combobox_set(widget, i);
            g_free(keys);
            g_free(text_cmp);
            return;
          }
          kk = i; // remember for down there
          // editable should only snap to perfect matches, not prefixes:
          if(d->editable && strcmp(entry->label, darktable.bauhaus->keys)) match = 0;
          k++;
        }
        i++;
        g_free(text_cmp);
      }
      // didn't find it, but had only one matching choice?
      if(k == 1 && match)
        dt_bauhaus_combobox_set(widget, kk);
      else if(d->editable)
      {
        // otherwise, if combobox is editable, assume it is a custom input
        memset(d->text, 0, DT_BAUHAUS_COMBO_MAX_TEXT);
        g_strlcpy(d->text, darktable.bauhaus->keys, DT_BAUHAUS_COMBO_MAX_TEXT);
        // select custom entry
        dt_bauhaus_combobox_set(widget, -1);
      }
      g_free(keys);
      break;
    }
    case DT_BAUHAUS_SLIDER:
    {
      if(darktable.bauhaus->end_mouse_y < darktable.bauhaus->line_height) break;

      dt_bauhaus_slider_data_t *d = &w->data.slider;
      const float mouse_off
          = get_slider_line_offset(d->oldpos, 5.0 * powf(10.0f, -d->digits) / (d->max - d->min) / d->factor,
                                   darktable.bauhaus->end_mouse_x / width, darktable.bauhaus->end_mouse_y / height,
                                   base_height / (float)height, allocation_popup_window.width, w);
      dt_bauhaus_slider_set_normalized(w, d->oldpos + mouse_off);
      d->oldpos = d->pos;
      break;
    }
    default:
      break;
  }
}

static gchar *_build_label(const dt_bauhaus_widget_t *w)
{
  if(w->show_extended_label && w->section)
    return g_strdup_printf("%s - %s", w->section, w->label);
  else
    return g_strdup(w->label);
}

static gboolean dt_bauhaus_popup_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_bauhaus_widget_t *w = darktable.bauhaus->current;

  // dimensions of the popup
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const GtkBorder *padding = darktable.bauhaus->popup_padding;
  const int w2 = allocation.width - padding->left - padding->right;
  const int h2 = allocation.height - padding->top - padding->bottom;

  // dimensions of the original line
  int ht = darktable.bauhaus->line_height + INNER_PADDING * 2.0f;

  // get area properties
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  cairo_t *cr = cairo_create(cst);

  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  // look up some colors once
  GdkRGBA text_color, text_color_selected, text_color_hover, text_color_insensitive;
  gtk_style_context_get_color(context, GTK_STATE_FLAG_NORMAL, &text_color);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_SELECTED, &text_color_selected);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_PRELIGHT, &text_color_hover);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_INSENSITIVE, &text_color_insensitive);

  GdkRGBA *fg_color = default_color_assign();
  GdkRGBA *bg_color;
  GtkStateFlags state = gtk_widget_get_state_flags(widget);

  gtk_style_context_get(context, state, "background-color", &bg_color, NULL);
  gtk_style_context_get_color(context, state, fg_color);

  // draw background
  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);
  gtk_render_frame(context, cr, 0, 0, allocation.width, allocation.height);

  // translate to account for the widget spacing
  cairo_translate(cr, padding->left, padding->top);

  // switch on bauhaus widget type (so we only need one static window)
  switch(w->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      const dt_bauhaus_slider_data_t *d = &w->data.slider;

      dt_bauhaus_draw_baseline(w, cr, w2);

      cairo_save(cr);
      cairo_set_line_width(cr, 0.5);
      float scale = 5.0 * powf(10.0f, -d->digits)/(d->max - d->min) / d->factor;
      const int num_scales = 1.f / scale;

      cairo_rectangle(cr, 0, ht, w2, h2 - ht);
      cairo_clip(cr);

      for(int k = 0; k < num_scales; k++)
      {
        const float off = k * scale - d->oldpos;
        GdkRGBA fg_copy = *fg_color;
        fg_copy.alpha = scale / fabsf(off);
        set_color(cr, fg_copy);
        draw_slider_line(cr, d->oldpos, off, scale, w2, h2, ht, w);
        cairo_stroke(cr);
      }
      cairo_restore(cr);
      set_color(cr, *fg_color);

      // draw mouse over indicator line
      cairo_save(cr);
      cairo_set_line_width(cr, 2.);
      const float mouse_off
          = darktable.bauhaus->change_active
                ? get_slider_line_offset(d->oldpos, scale, darktable.bauhaus->mouse_x / w2,
                                         darktable.bauhaus->mouse_y / h2, ht / (float)h2, allocation.width, w)
                : 0.0f;
      draw_slider_line(cr, d->oldpos, mouse_off, scale, w2, h2, ht, w);
      cairo_stroke(cr);
      cairo_restore(cr);

      // draw indicator
      dt_bauhaus_draw_indicator(w, d->oldpos + mouse_off, cr, w2, *fg_color, *bg_color);

      // draw numerical value:
      cairo_save(cr);

      char *text = dt_bauhaus_slider_get_text(GTK_WIDGET(w), dt_bauhaus_slider_get(GTK_WIDGET(w)));
      set_color(cr, *fg_color);
      const float value_width = show_pango_text(w, context, cr, text, w2 - _widget_get_quad_width(w), 0, 0, TRUE,
                                                FALSE, PANGO_ELLIPSIZE_END, FALSE, FALSE, NULL, NULL);
      g_free(text);
      set_color(cr, text_color_insensitive);
      char *min = dt_bauhaus_slider_get_text(GTK_WIDGET(w), d->factor > 0 ? d->min : d->max);
      show_pango_text(w, context, cr, min, 0, ht + INNER_PADDING, 0, FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE,
                      FALSE, NULL, NULL);
      g_free(min);
      char *max = dt_bauhaus_slider_get_text(GTK_WIDGET(w), d->factor > 0 ? d->max : d->min);
      show_pango_text(w, context, cr, max, w2 - _widget_get_quad_width(w), ht + INNER_PADDING, 0, TRUE, FALSE,
                      PANGO_ELLIPSIZE_END, FALSE, FALSE, NULL, NULL);
      g_free(max);

      const float label_width = w2 - _widget_get_quad_width(w) - INNER_PADDING - value_width;
      if(label_width > 0)
      {
        gchar *lb = _build_label(w);
        show_pango_text(w, context, cr, lb, 0, 0, label_width, FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE, FALSE,
                        NULL, NULL);
        g_free(lb);
      }
      cairo_restore(cr);
    }
    break;
    case DT_BAUHAUS_COMBOBOX:
    {
      const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      cairo_save(cr);
      float first_label_width = 0.0;
      gboolean first_label = *w->label;
      gboolean show_box_label = TRUE;
      int k = 0, i = 0;
      ht = darktable.bauhaus->line_height;
      // case where the popup is detcahed (label on its specific line)
      const int top_gap = (w->detached_popup) ? w->top_gap + ht : w->top_gap;
      if(w->detached_popup || !w->show_label) first_label = FALSE;
      if(!w->detached_popup && !w->show_label) show_box_label = FALSE;
      const int hovered = (darktable.bauhaus->mouse_y - top_gap) / darktable.bauhaus->line_height;
      gchar *keys = g_utf8_casefold(darktable.bauhaus->keys, -1);
      const PangoEllipsizeMode ellipsis = d->entries_ellipsis;

      for(int j = 0; j < d->entries->len; j++)
      {
        const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, j);
        gchar *text_cmp = g_utf8_casefold(entry->label, -1);
        if(!strncmp(text_cmp, keys, darktable.bauhaus->keys_cnt))
        {
          float max_width = w2 - _widget_get_quad_width(w);
          float label_width = 0.0f;
          if(!entry->sensitive)
            set_color(cr, text_color_insensitive);
          else if(i == hovered)
            set_color(cr, text_color_hover);
          else if(i == d->active)
            set_color(cr, text_color_selected);
          else
            set_color(cr, text_color);

          if(entry->alignment == DT_BAUHAUS_COMBOBOX_ALIGN_LEFT)
          {
            gchar *esc_label = g_markup_escape_text(entry->label, -1);
            gchar *label = g_strdup_printf("<b>%s</b>", esc_label);
            label_width = show_pango_text(w, context, cr, label, 0, ht * k + top_gap, max_width, FALSE, FALSE,
                                          ellipsis, TRUE, FALSE, NULL, NULL);
            g_free(label);
            g_free(esc_label);
          }
          else if(entry->alignment == DT_BAUHAUS_COMBOBOX_ALIGN_MIDDLE)
          {
            // first pass, we just get the text width
            label_width = show_pango_text(w, context, cr, entry->label, 0, ht * k + top_gap, max_width, FALSE,
                                          TRUE, ellipsis, TRUE, FALSE, NULL, NULL);
            // second pass, we draw it in the middle
            const int posx = MAX(0, (max_width - label_width) / 2);
            label_width = show_pango_text(w, context, cr, entry->label, posx, ht * k + top_gap, max_width, FALSE,
                                          FALSE, ellipsis, TRUE, FALSE, NULL, NULL);
          }
          else
          {
            if(first_label) max_width *= 0.8; // give the label at least some room
            label_width
                = show_pango_text(w, context, cr, entry->label, w2 - _widget_get_quad_width(w), ht * k + top_gap,
                                  max_width, TRUE, FALSE, ellipsis, FALSE, FALSE, NULL, NULL);
          }

          // prefer the entry over the label wrt. ellipsization when expanded
          if(first_label)
          {
            show_box_label = entry->alignment == DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT;
            first_label_width = label_width;
            first_label = FALSE;
          }

          k++;
        }
        i++;
        g_free(text_cmp);
      }
      cairo_restore(cr);

      // left aligned box label. add it to the gui after the entries so we can ellipsize it if needed
      if(show_box_label)
      {
        set_color(cr, text_color);
        gchar *lb = _build_label(w);
        show_pango_text(w, context, cr, lb, 0, w->top_gap, w2 - _widget_get_quad_width(w) - first_label_width,
                        FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE, TRUE, NULL, NULL);
        g_free(lb);
      }
      g_free(keys);
    }
    break;
    default:
      // yell
      break;
  }

  // draw currently typed text. if a type doesn't want this, it should not
  // allow stuff to be written here in the key callback.
  const int line_height = darktable.bauhaus->line_height;
  const int size = MIN(3 * line_height, .2 * h2);
  if(darktable.bauhaus->keys_cnt)
  {
    cairo_save(cr);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoRectangle ink;
    pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
    set_color(cr, text_color);

    // make extra large, but without dependency on popup window height
    // (that might differ for comboboxes for example). only fall back
    // to height dependency if the popup is really small.
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    pango_layout_set_text(layout, darktable.bauhaus->keys, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, w2 - _widget_get_quad_width(w) - ink.width, h2 * 0.5 - size);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
  if(darktable.bauhaus->cursor_visible)
  {
    // show the blinking cursor
    cairo_save(cr);
    set_color(cr, text_color);
    cairo_move_to(cr, w2 - darktable.bauhaus->quad_width + 3, h2 * 0.5 + size / 3);
    cairo_line_to(cr, w2 - darktable.bauhaus->quad_width + 3, h2 * 0.5 - size);
    cairo_set_line_width(cr, 2.);
    cairo_stroke(cr);
    cairo_restore(cr);
  }

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  gdk_rgba_free(bg_color);
  gdk_rgba_free(fg_color);

  return TRUE;
}

static gboolean _widget_draw(GtkWidget *widget, cairo_t *crf)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  GdkRGBA *fg_color = default_color_assign();
  GdkRGBA *bg_color;
  GdkRGBA *text_color = default_color_assign();
  const GtkStateFlags state = gtk_widget_get_state_flags(widget);
  gtk_style_context_get_color(context, state, text_color);

  gtk_style_context_get_color(context, state, fg_color);
  gtk_style_context_get(context, state, "background-color", &bg_color, NULL);
  _margins_retrieve(w);

  // translate to account for the widget spacing
  const int h2 = height - w->margin->top - w->margin->bottom;
  const int w2 = width - w->margin->left - w->margin->right;
  const int h3 = h2 - w->padding->top - w->padding->bottom;
  const int w3 = w2 - w->padding->left - w->padding->right;
  gtk_render_background(context, cr, w->margin->left, w->margin->top, w2, h2);
  cairo_translate(cr, w->margin->left + w->padding->left, w->margin->top + w->padding->top);

  // draw type specific content:
  cairo_save(cr);
  cairo_set_line_width(cr, 1.0);
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
    {
      // draw label and quad area at right end
      set_color(cr, *text_color);
      if(w->show_quad) dt_bauhaus_draw_quad(w, cr, w3, h3);

      dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      const PangoEllipsizeMode combo_ellipsis = d->entries_ellipsis;
      const gchar *text = d->text;
      if(d->active >= 0 && d->active < d->entries->len)
      {
        const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, d->active);
        text = entry->label;
      }
      set_color(cr, *text_color);

      const float available_width = w3 - _widget_get_quad_width(w);

      //calculate total widths of label and combobox
      gchar *label_text = _build_label(w);
      float label_width = 0;
      float label_height = 0;
      // we only show the label if the text is aligned on the right
      if(label_text && w->show_label && d->text_align == DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT)
        show_pango_text(w, context, cr, label_text, 0, 0, 0, FALSE, TRUE, PANGO_ELLIPSIZE_END, FALSE, TRUE,
                        &label_width, &label_height);
      float combo_width = 0;
      float combo_height = 0;
      show_pango_text(w, context, cr, text, available_width, 0, 0, TRUE, TRUE, combo_ellipsis, FALSE, FALSE,
                      &combo_width, &combo_height);
      // we want to center the text vertically
      w->top_gap = floor((h3 - fmaxf(label_height, combo_height)) / 2.0f);
      //check if they fit
      if((label_width + combo_width) > available_width)
      {
        if(d->text_align == DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT)
        {
          // they don't fit: evenly divide the available width between the two in proportion
          const float ratio = label_width / (label_width + combo_width);
          if(w->show_label)
            show_pango_text(w, context, cr, label_text, 0, w->top_gap, available_width * ratio - INNER_PADDING * 2,
                            FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE, TRUE, NULL, NULL);
          show_pango_text(w, context, cr, text, available_width, w->top_gap, available_width * (1.0f - ratio),
                          TRUE, FALSE, combo_ellipsis, FALSE, FALSE, NULL, NULL);
        }
        else if(d->text_align == DT_BAUHAUS_COMBOBOX_ALIGN_MIDDLE)
        {
          const int posx = MAX(0, (available_width - combo_width) / 2);
          show_pango_text(w, context, cr, text, posx, w->top_gap, available_width, FALSE, FALSE, combo_ellipsis,
                          FALSE, FALSE, NULL, NULL);
        }
        else
          show_pango_text(w, context, cr, text, 0, w->top_gap, available_width, FALSE, FALSE, combo_ellipsis,
                          FALSE, FALSE, NULL, NULL);
      }
      else
      {
        if(d->text_align == DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT)
        {
          if(w->show_label)
            show_pango_text(w, context, cr, label_text, 0, w->top_gap, 0, FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE,
                            TRUE, NULL, NULL);
          show_pango_text(w, context, cr, text, available_width, w->top_gap, 0, TRUE, FALSE, combo_ellipsis, FALSE,
                          FALSE, NULL, NULL);
        }
        else if(d->text_align == DT_BAUHAUS_COMBOBOX_ALIGN_MIDDLE)
        {
          const int posx = MAX(0, (available_width - combo_width) / 2);
          show_pango_text(w, context, cr, text, posx, w->top_gap, 0, FALSE, FALSE, combo_ellipsis, FALSE, FALSE,
                          NULL, NULL);
        }
        else
          show_pango_text(w, context, cr, text, 0, w->top_gap, 0, FALSE, FALSE, combo_ellipsis, FALSE, FALSE, NULL,
                          NULL);
      }
      g_free(label_text);
      break;
    }
    case DT_BAUHAUS_SLIDER:
    {
      // line for orientation
      dt_bauhaus_draw_baseline(w, cr, w3);
      if(w->show_quad) dt_bauhaus_draw_quad(w, cr, w3, h3);

      float value_width = 0;
      if(gtk_widget_is_sensitive(widget))
      {
        cairo_save(cr);
        cairo_rectangle(cr, 0, 0, w3 - _widget_get_quad_width(w), h3 + INNER_PADDING);
        cairo_clip(cr);
        dt_bauhaus_draw_indicator(w, w->data.slider.pos, cr, w3, *fg_color, *bg_color);
        cairo_restore(cr);

        // TODO: merge that text with combo

        char *text = dt_bauhaus_slider_get_text(widget, dt_bauhaus_slider_get(widget));
        set_color(cr, *text_color);
        value_width = show_pango_text(w, context, cr, text, w3 - _widget_get_quad_width(w), 0, 0, TRUE, FALSE,
                                      PANGO_ELLIPSIZE_END, FALSE, FALSE, NULL, NULL);
        g_free(text);
      }
      // label on top of marker:
      gchar *label_text = _build_label(w);
      set_color(cr, *text_color);
      const float label_width = w3 - _widget_get_quad_width(w) - value_width;
      if(label_width > 0)
        show_pango_text(w, context, cr, label_text, 0, 0, label_width, FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE,
                        TRUE, NULL, NULL);
      g_free(label_text);
    }
    break;
    default:
      break;
  }
  cairo_restore(cr);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  // render eventual css borders
  gtk_render_frame(context, crf, w->margin->left, w->margin->top, w2, h2);

  gdk_rgba_free(text_color);
  gdk_rgba_free(fg_color);
  gdk_rgba_free(bg_color);

  return TRUE;
}

static gint _bauhaus_natural_width(GtkWidget *widget, gboolean popup)
{
  gint natural_size = 0;

  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);

  PangoLayout *layout = gtk_widget_create_pango_layout(widget, NULL);
  PangoFontDescription *font_desc = 0;
  gtk_style_context_get(gtk_widget_get_style_context(widget), gtk_widget_get_state_flags(GTK_WIDGET(w)), "font", &font_desc, NULL);
  pango_layout_set_font_description(layout, font_desc);
  PangoAttrList *attrlist = pango_attr_list_new();
  PangoAttribute *attr = pango_attr_font_features_new("tnum");
  pango_attr_list_insert(attrlist, attr);
  pango_layout_set_attributes(layout, attrlist);
  pango_attr_list_unref(attrlist);

  pango_layout_set_text(layout, w->label, -1);
  pango_layout_get_size(layout, &natural_size, NULL);
  natural_size /= PANGO_SCALE;

  if(w->type == DT_BAUHAUS_COMBOBOX)
  {
    dt_bauhaus_combobox_data_t *d = &w->data.combobox;

    gint label_width = 0, entry_width = 0;

    if(d->text_align == DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT && w->show_label)
    {
      if(!w->detached_popup) label_width = natural_size;
      if(label_width) label_width += 2 * INNER_PADDING;
    }

    for(int i = 0; i < d->entries->len; i++)
    {
      const dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(d->entries, i);

      if(popup && (i || entry->alignment != DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT))
        label_width = 0;

      pango_layout_set_text(layout, entry->label, -1);
      pango_layout_get_size(layout, &entry_width, NULL);

      natural_size = MAX(natural_size, label_width + entry_width / PANGO_SCALE);
    }
  }
  else
  {
    gint number_width = 0;
    char *text = dt_bauhaus_slider_get_text(widget, dt_bauhaus_slider_get(widget));
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_size(layout, &number_width, NULL);
    natural_size += 2 * INNER_PADDING + number_width / PANGO_SCALE;
    g_free(text);
  }

  _margins_retrieve(w);
  natural_size += _widget_get_quad_width(w) + w->margin->left + w->margin->right
                  + w->padding->left + w->padding->right;

  g_object_unref(layout);

  return natural_size;
}

static void _get_preferred_width(GtkWidget *widget, gint *minimum_size, gint *natural_size)
{
  *natural_size = _bauhaus_natural_width(widget, FALSE);
}

void dt_bauhaus_hide_popup()
{
  if(darktable.bauhaus->current)
  {
    darktable.bauhaus->current->detached_popup = FALSE;
    gtk_grab_remove(darktable.bauhaus->popup_area);
    gtk_widget_hide(darktable.bauhaus->popup_window);
    gtk_window_set_attached_to(GTK_WINDOW(darktable.bauhaus->popup_window), NULL);
    darktable.bauhaus->current = NULL;
    // TODO: give focus to center view? do in accept() as well?
  }
  _stop_cursor();
}

void dt_bauhaus_show_popup(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(darktable.bauhaus->current) dt_bauhaus_hide_popup();
  darktable.bauhaus->current = w;
  darktable.bauhaus->keys_cnt = 0;
  memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
  darktable.bauhaus->change_active = 0;
  darktable.bauhaus->mouse_line_distance = 0.0f;
  darktable.bauhaus->hiding = FALSE;
  _stop_cursor();

  bauhaus_request_focus(w);

  gtk_widget_realize(darktable.bauhaus->popup_window);

  GdkWindow *widget_window = gtk_widget_get_window(widget);

  gint wx = 0, wy = 0;
  if(widget_window)
    gdk_window_get_origin(widget_window, &wx, &wy);

  GtkAllocation tmp;
  gtk_widget_get_allocation(widget, &tmp);
  gint natural_w = _bauhaus_natural_width(widget, TRUE);
  if(tmp.width < natural_w)
    tmp.width = natural_w;
  else if(tmp.width == 1 || !w->margin)
  {
    if(dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_RIGHT, widget))
      tmp.width = dt_ui_panel_get_size(darktable.gui->ui, DT_UI_PANEL_RIGHT);
    else if(dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_LEFT, widget))
      tmp.width = dt_ui_panel_get_size(darktable.gui->ui, DT_UI_PANEL_LEFT);
    else
      tmp.width = 300;
    tmp.width -= INNER_PADDING * 2;
  }
  else
  {
  // by default, we want the popup to be exactly the size of the widget content
    tmp.width = MAX(1, tmp.width - (w->margin->left + w->margin->right + w->padding->left + w->padding->right));
  }

  gint px, py;
  GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
  gdk_device_get_position(pointer, NULL, &px, &py);

  w->detached_popup = FALSE;
  if(px < wx || px > wx + tmp.width)
  {
    w->detached_popup = TRUE;
    wx = px - (tmp.width - _widget_get_quad_width(w)) / 2;
    wy = py - darktable.bauhaus->line_height / 2;
  }
  else if(py < wy || py > wy + tmp.height)
  {
    wy = py - darktable.bauhaus->line_height / 2;
  }

  switch(darktable.bauhaus->current->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      d->oldpos = d->pos;
      tmp.height = tmp.width;
      _start_cursor(6);
      break;
    }
    case DT_BAUHAUS_COMBOBOX:
    {
      // we launch the dynamic populate fct if any
      dt_iop_module_t *module = (dt_iop_module_t *)(w->module);
      const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      if(d->populate) d->populate(widget, &module);
      // comboboxes change immediately
      darktable.bauhaus->change_active = 1;
      if(!d->entries->len) return;
      tmp.height = darktable.bauhaus->line_height * d->entries->len;
      // if the popup is detached, we show the label in any cases, in a special line
      if(w->detached_popup)
      {
        tmp.height += darktable.bauhaus->line_height;
        wy -= darktable.bauhaus->line_height;
        darktable.bauhaus->mouse_y = darktable.bauhaus->line_height;
      }

      if(w->margin) tmp.height += w->margin->top + w->margin->bottom + w->top_gap;

      GtkAllocation allocation_w;
      gtk_widget_get_allocation(widget, &allocation_w);
      const int ht = allocation_w.height;
      wy -= d->active * darktable.bauhaus->line_height;
      darktable.bauhaus->mouse_x = 0;
      darktable.bauhaus->mouse_y += d->active * darktable.bauhaus->line_height + ht / 2;
      break;
    }
    default:
      break;
  }

  // by default, we want the popup to be exactly at the position of the widget content
  if(w->margin)
  {
    wx += w->margin->left + w->padding->left;
    wy += w->margin->top + w->padding->top;
  }

  // we update the popup padding defined in css
  if(!darktable.bauhaus->popup_padding) darktable.bauhaus->popup_padding = gtk_border_new();
  GtkStyleContext *context = gtk_widget_get_style_context(darktable.bauhaus->popup_area);
  gtk_style_context_add_class(context, "dt_bauhaus_popup");
  // let's update the css class depending on the source widget type
  // this allow to set different padding for example
  if(w->show_quad)
    gtk_style_context_remove_class(context, "dt_bauhaus_popup_right");
  else
    gtk_style_context_add_class(context, "dt_bauhaus_popup_right");

  const GtkStateFlags state = gtk_widget_get_state_flags(darktable.bauhaus->popup_area);
  gtk_style_context_get_padding(context, state, darktable.bauhaus->popup_padding);
  // and now we extent the popup to take account of its own padding
  wx -= darktable.bauhaus->popup_padding->left;
  wy -= darktable.bauhaus->popup_padding->top;
  tmp.width += darktable.bauhaus->popup_padding->left + darktable.bauhaus->popup_padding->right;
  tmp.height += darktable.bauhaus->popup_padding->top + darktable.bauhaus->popup_padding->bottom;

  if(widget_window)
  {
    GdkRectangle workarea;
    gdk_monitor_get_workarea(gdk_display_get_monitor_at_window(gdk_window_get_display(widget_window), widget_window), &workarea);
    wx = MAX(workarea.x, MIN(wx, workarea.x + workarea.width - tmp.width));
  }

  // gtk_widget_get_window will return null if not shown yet.
  // it is needed for gdk_window_move, and gtk_window move will
  // sometimes be ignored. this is why we always call both...
  // we also don't want to show before move, as this results in noticeable flickering.
  GdkWindow *window = gtk_widget_get_window(darktable.bauhaus->popup_window);
  if(window) gdk_window_move(window, wx, wy);
  gtk_window_move(GTK_WINDOW(darktable.bauhaus->popup_window), wx, wy);
  gtk_widget_set_size_request(darktable.bauhaus->popup_window, tmp.width, tmp.height);
  // gtk_window_set_keep_above isn't enough on macOS
  gtk_window_set_attached_to(GTK_WINDOW(darktable.bauhaus->popup_window), GTK_WIDGET(darktable.bauhaus->current));
  gtk_widget_show_all(darktable.bauhaus->popup_window);
  gtk_widget_grab_focus(darktable.bauhaus->popup_area);
}

static void _slider_add_step(GtkWidget *widget, float delta, guint state, gboolean force)
{
  if(delta == 0) return;

  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  delta *= dt_bauhaus_slider_get_step(widget) * dt_accel_get_speed_multiplier(widget, state);

  const float min_visible = powf(10.0f, -d->digits) / fabsf(d->factor);
  if(delta && fabsf(delta) < min_visible)
    delta = copysignf(min_visible, delta);

  const float value = dt_bauhaus_slider_get(widget);

  if(force || dt_modifier_is(state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
  {
    if(d->factor > 0 ? d->pos < 0.0001 : d->pos > 0.9999) d->min = d->min > d->soft_min ? d->max : d->soft_min;
    if(d->factor < 0 ? d->pos < 0.0001 : d->pos > 0.9999) d->max = d->max < d->soft_max ? d->min : d->soft_max;
    dt_bauhaus_slider_set(widget, value + delta);
  }
  else if(!strcmp(d->format,"°") && (d->max - d->min) * d->factor == 360.0f
          && fabsf(value + delta)/(d->max - d->min) < 2)
    dt_bauhaus_slider_set(widget, fmodf(value + delta + d->max - 2*d->min, d->max - d->min) + d->min);
  else
    dt_bauhaus_slider_set(widget, CLAMP(value + delta, d->min, d->max));
}

static gboolean _widget_scroll(GtkWidget *widget, GdkEventScroll *event)
{
  if(dt_gui_ignore_scroll(event)) return FALSE;

  // handle speed adjustment in mapping mode in dispatcher
  if(darktable.control->mapping_widget)
    return dt_shortcut_dispatcher(widget, (GdkEvent*)event, NULL);

  gtk_widget_grab_focus(widget);

  int delta_y = 0;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    if(delta_y == 0) return TRUE;

    dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
    bauhaus_request_focus(w);

    if(w->type == DT_BAUHAUS_SLIDER)
    {
      gboolean force = darktable.control->element == DT_ACTION_ELEMENT_FORCE;
      if(force && dt_modifier_is(event->state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
      {
        _slider_zoom_range(w, delta_y);
        _slider_zoom_toast(w);
      }
      else
        _slider_add_step(widget, - delta_y, event->state, force);
    }
    else
      _combobox_next_sensitive(w, delta_y, FALSE);
  }
  return TRUE; // Ensure that scrolling the combobox cannot move side panel
}

static gboolean _widget_key_press(GtkWidget *widget, GdkEventKey *event)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;

  int delta = -1;
  switch(event->keyval)
  {
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
      delta = 1;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
      bauhaus_request_focus(w);

      if(w->type == DT_BAUHAUS_SLIDER)
        _slider_add_step(widget, delta, event->state, FALSE);
      else
        _combobox_next_sensitive(w, -delta, FALSE);

      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean dt_bauhaus_combobox_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;

  if(w->type != DT_BAUHAUS_COMBOBOX) return FALSE;
  bauhaus_request_focus(w);
  gtk_widget_grab_focus(GTK_WIDGET(w));

  GtkAllocation tmp;
  gtk_widget_get_allocation(GTK_WIDGET(w), &tmp);
  const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  if(w->quad_paint && (event->x > allocation.width - _widget_get_quad_width(w)))
  {
    dt_bauhaus_widget_press_quad(widget);
    return TRUE;
  }
  else if(event->button == 3)
  {
    darktable.bauhaus->mouse_x = event->x;
    darktable.bauhaus->mouse_y = event->y;
    dt_bauhaus_show_popup(widget);
    return TRUE;
  }
  else if(event->button == 1)
  {
    // reset to default.
    if(event->type == GDK_2BUTTON_PRESS)
    {
      // never called, as we popup the other window under your cursor before.
      // (except in weird corner cases where the popup is under the -1st entry
      dt_bauhaus_combobox_set(widget, d->defpos);
      dt_bauhaus_hide_popup();
    }
    else
    {
      // single click, show options
      darktable.bauhaus->opentime = event->time;
      darktable.bauhaus->mouse_x = event->x;
      darktable.bauhaus->mouse_y = event->y;
      dt_bauhaus_show_popup(widget);
    }
    return TRUE;
  }
  return FALSE;
}

float dt_bauhaus_slider_get(GtkWidget *widget)
{
  // first cast to bh widget, to check that type:
  const dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return -1.0f;
  const dt_bauhaus_slider_data_t *d = &w->data.slider;
  if(d->max == d->min)
  {
    return d->max;
  }
  const float rawval = d->curve(d->pos, DT_BAUHAUS_GET);
  return d->min + rawval * (d->max - d->min);
}

float dt_bauhaus_slider_get_val(GtkWidget *widget)
{
  const dt_bauhaus_slider_data_t *d = &DT_BAUHAUS_WIDGET(widget)->data.slider;
  return dt_bauhaus_slider_get(widget) * d->factor + d->offset;
}

char *dt_bauhaus_slider_get_text(GtkWidget *w, float val)
{
  const dt_bauhaus_slider_data_t *d = &DT_BAUHAUS_WIDGET(w)->data.slider;
  if((d->hard_max * d->factor + d->offset)*(d->hard_min * d->factor + d->offset) < 0)
    return g_strdup_printf("%+.*f%s", d->digits, val * d->factor + d->offset, d->format);
  else
    return g_strdup_printf( "%.*f%s", d->digits, val * d->factor + d->offset, d->format);
}

void dt_bauhaus_slider_set(GtkWidget *widget, float pos)
{
  // this is the public interface function, translate by bounds and call set_normalized
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  const float rpos = CLAMP(pos, d->hard_min, d->hard_max);
  d->min = MIN(d->min, rpos);
  d->max = MAX(d->max, rpos);
  const float rawval = (rpos - d->min) / (d->max - d->min);
  dt_bauhaus_slider_set_normalized(w, d->curve(rawval, DT_BAUHAUS_SET));
}

void dt_bauhaus_slider_set_val(GtkWidget *widget, float val)
{
  const dt_bauhaus_slider_data_t *d = &DT_BAUHAUS_WIDGET(widget)->data.slider;
  dt_bauhaus_slider_set(widget, (val - d->offset) / d->factor);
}

void dt_bauhaus_slider_set_digits(GtkWidget *widget, int val)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->digits = val;
}

int dt_bauhaus_slider_get_digits(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return 0;

  const dt_bauhaus_slider_data_t *d = &w->data.slider;

  return d->digits;
}

void dt_bauhaus_slider_set_step(GtkWidget *widget, float val)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->step = val;
}

float dt_bauhaus_slider_get_step(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return 0;

  const dt_bauhaus_slider_data_t *d = &w->data.slider;

  float step = d->step;

  if(!step)
  {
    gboolean zoom = dt_conf_get_bool("bauhaus/zoom_step");
    const float min = zoom ? d->min : d->soft_min;
    const float max = zoom ? d->max : d->soft_max;

    const float top = fminf(max-min, fmaxf(fabsf(min), fabsf(max)));
    if(top >= 100)
    {
      step = 1.f;
    }
    else
    {
      step = top * fabsf(d->factor) / 100;
      const float log10step = log10f(step);
      const float fdigits = floorf(log10step+.1);
      step = powf(10.f,fdigits);
      if(log10step - fdigits > .5)
        step *= 5;
      step /= fabsf(d->factor);
    }
  }

  return copysignf(step, d->factor);
}

void dt_bauhaus_slider_set_feedback(GtkWidget *widget, int feedback)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->fill_feedback = feedback;

  gtk_widget_queue_draw(widget);
}

int dt_bauhaus_slider_get_feedback(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return 0;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  return d->fill_feedback;
}

void dt_bauhaus_widget_reset(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type == DT_BAUHAUS_SLIDER)
  {
    dt_bauhaus_slider_data_t *d = &w->data.slider;

    d->min = d->soft_min;
    d->max = d->soft_max;

    dt_bauhaus_slider_set(widget, d->defpos);
  }
  else
    dt_bauhaus_combobox_set(widget, w->data.combobox.defpos);

  return;
}

void dt_bauhaus_slider_set_format(GtkWidget *widget, const char *format)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->format = g_intern_string(format);

  if(strstr(format,"%") && fabsf(d->hard_max) <= 10)
  {
    if(d->factor == 1.0f) d->factor = 100;
    d->digits -= 2;
  }
}

void dt_bauhaus_slider_set_factor(GtkWidget *widget, float factor)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->factor = factor;
  if(factor < 0) d->curve = _reverse_linear_curve;
}

void dt_bauhaus_slider_set_offset(GtkWidget *widget, float offset)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->offset = offset;
}

void dt_bauhaus_slider_set_curve(GtkWidget *widget, float (*curve)(float value, dt_bauhaus_curve_t dir))
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  if(curve == NULL) curve = _default_linear_curve;

  d->pos = curve(d->curve(d->pos, DT_BAUHAUS_GET), DT_BAUHAUS_SET);

  d->curve = curve;
}

static gboolean _bauhaus_slider_value_change_dragging(gpointer data);

static void _bauhaus_slider_value_change(dt_bauhaus_widget_t *w)
{
  if(!GTK_IS_WIDGET(w)) return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;
  if(d->is_changed && !d->timeout_handle && !darktable.gui->reset)
  {
    if(w->field)
    {
      float val = dt_bauhaus_slider_get(GTK_WIDGET(w));
      switch(w->field_type)
      {
        case DT_INTROSPECTION_TYPE_FLOAT:;
          float *f = w->field, prevf = *f; *f = val;
          if(*f != prevf) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &prevf);
          break;
        case DT_INTROSPECTION_TYPE_INT:;
          int *i = w->field, previ = *i; *i = val;
          if(*i != previ) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &previ);
          break;
        case DT_INTROSPECTION_TYPE_USHORT:;
          unsigned short *s = w->field, prevs = *s; *s = val;
          if(*s != prevs) dt_iop_gui_changed(w->module, GTK_WIDGET(w), &prevs);
          break;
        default:
          fprintf(stderr, "[_bauhaus_slider_value_change] unsupported slider data type\n");
      }
    }

    _highlight_changed_notebook_tab(GTK_WIDGET(w), NULL);
    g_signal_emit_by_name(G_OBJECT(w), "value-changed");
    d->is_changed = 0;
  }

  if(d->is_changed && d->is_dragging && !d->timeout_handle)
    d->timeout_handle = g_idle_add(_bauhaus_slider_value_change_dragging, w);
}

static gboolean _bauhaus_slider_value_change_dragging(gpointer data)
{
  dt_bauhaus_widget_t *w = data;
  w->data.slider.timeout_handle = 0;
  _bauhaus_slider_value_change(data);
  return G_SOURCE_REMOVE;
}

static void dt_bauhaus_slider_set_normalized(dt_bauhaus_widget_t *w, float pos)
{
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float rpos = CLAMP(pos, 0.0f, 1.0f);
  rpos = d->curve(rpos, DT_BAUHAUS_GET);
  rpos = d->min + (d->max - d->min) * rpos;
  const float base = powf(10.0f, d->digits) * d->factor;
  rpos = roundf(base * rpos) / base;

  rpos = (rpos - d->min) / (d->max - d->min);
  d->pos = d->curve(rpos, DT_BAUHAUS_SET);
  gtk_widget_queue_draw(GTK_WIDGET(w));
  d->is_changed = -1;

  _bauhaus_slider_value_change(w);
}

static gboolean dt_bauhaus_popup_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  switch(darktable.bauhaus->current->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      // hack to do screenshots from popup:
      // if(event->string[0] == 'p') return system("scrot");
      // else
      if(darktable.bauhaus->keys_cnt + 2 < 64
         && (event->keyval == GDK_KEY_space || event->keyval == GDK_KEY_KP_Space ||              // SPACE
             event->keyval == GDK_KEY_percent ||                                                 // %
             (event->string[0] >= 40 && event->string[0] <= 57) ||                               // ()+-*/.,0-9
             event->keyval == GDK_KEY_asciicircum || event->keyval == GDK_KEY_dead_circumflex || // ^
             event->keyval == GDK_KEY_X || event->keyval == GDK_KEY_x))                          // Xx
      {
        if(event->keyval == GDK_KEY_dead_circumflex)
          darktable.bauhaus->keys[darktable.bauhaus->keys_cnt++] = '^';
        else
          darktable.bauhaus->keys[darktable.bauhaus->keys_cnt++] = event->string[0];
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0
              && (event->keyval == GDK_KEY_BackSpace || event->keyval == GDK_KEY_Delete))
      {
        darktable.bauhaus->keys[--darktable.bauhaus->keys_cnt] = 0;
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0 && darktable.bauhaus->keys_cnt + 1 < 64
              && (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter))
      {
        // accept input
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt] = 0;
        // unnormalized input, user was typing this:
        const float old_value = dt_bauhaus_slider_get_val(GTK_WIDGET(darktable.bauhaus->current));
        const float new_value = dt_calculator_solve(old_value, darktable.bauhaus->keys);
        if(isfinite(new_value)) dt_bauhaus_slider_set_val(GTK_WIDGET(darktable.bauhaus->current), new_value);
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_hide_popup();
      }
      else if(event->keyval == GDK_KEY_Escape)
      {
        // discard input and close popup
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_hide_popup();
      }
      else
        return FALSE;
      if(darktable.bauhaus->keys_cnt > 0) _start_cursor(-1);
      return TRUE;
    }
    case DT_BAUHAUS_COMBOBOX:
    {
      if(!g_utf8_validate(event->string, -1, NULL)) return FALSE;
      const gunichar c = g_utf8_get_char(event->string);
      const long int char_width = g_utf8_next_char(event->string) - event->string;
      // if(event->string[0] == 'p') return system("scrot");
      // else
      if(darktable.bauhaus->keys_cnt + 1 + char_width < 64 && g_unichar_isprint(c))
      {
        // only accept key input if still valid or editable?
        g_utf8_strncpy(darktable.bauhaus->keys + darktable.bauhaus->keys_cnt, event->string, 1);
        darktable.bauhaus->keys_cnt += char_width;
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0
              && (event->keyval == GDK_KEY_BackSpace || event->keyval == GDK_KEY_Delete))
      {
        darktable.bauhaus->keys_cnt
            -= (darktable.bauhaus->keys + darktable.bauhaus->keys_cnt)
               - g_utf8_prev_char(darktable.bauhaus->keys + darktable.bauhaus->keys_cnt);
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt] = 0;
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0 && darktable.bauhaus->keys_cnt + 1 < 64
              && (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter))
      {
        // accept unique matches only for editable:
        if(darktable.bauhaus->current->data.combobox.editable)
          darktable.bauhaus->end_mouse_y = FLT_MAX;
        else
          darktable.bauhaus->end_mouse_y = 0;
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt] = 0;
        dt_bauhaus_widget_accept(darktable.bauhaus->current);
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_hide_popup();
      }
      else if(event->keyval == GDK_KEY_Escape)
      {
        // discard input and close popup
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_hide_popup();
      }
      else if(event->keyval == GDK_KEY_Up)
      {
        combobox_popup_scroll(-1);
      }
      else if(event->keyval == GDK_KEY_Down)
      {
        combobox_popup_scroll(1);
      }
      else if(event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)
      {
        // return pressed, but didn't type anything
        darktable.bauhaus->end_mouse_y = -1; // negative will use currently highlighted instead.
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt] = 0;
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
        dt_bauhaus_widget_accept(darktable.bauhaus->current);
        dt_bauhaus_hide_popup();
      }
      else
        return FALSE;
      return TRUE;
    }
    default:
      return FALSE;
  }
}

static gboolean dt_bauhaus_slider_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  bauhaus_request_focus(w);
  gtk_widget_grab_focus(widget);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int w3 = allocation.width - w->margin->left - w->padding->left - w->margin->right - w->padding->right;
  const double ex = event->x - w->margin->left - w->padding->left;
  const double ey = event->y - w->margin->top - w->padding->top;
  if(event->x > allocation.width - _widget_get_quad_width(w) - w->margin->right - w->padding->right)
  {
    dt_bauhaus_widget_press_quad(widget);
    return TRUE;
  }
  else if(event->button == 3)
  {
    dt_bauhaus_show_popup(widget);
    return TRUE;
  }
  else if(event->button == 2)
  {
    _slider_zoom_range(w, 0); // reset zoom range to soft min/max
    _slider_zoom_toast(w);
  }
  else if(event->button == 1)
  {
    dt_bauhaus_slider_data_t *d = &w->data.slider;
    // reset to default.
    if(event->type == GDK_2BUTTON_PRESS)
    {
      d->is_dragging = 0;
      dt_bauhaus_widget_reset(widget);
    }
    else
    {
      d->is_dragging = -1;
      if(!dt_modifier_is(event->state, 0))
        darktable.bauhaus->mouse_x = ex;
      else if(ey > darktable.bauhaus->line_height / 2.0f)
      {
        const float r = slider_right_pos((float)w3, w);
        dt_bauhaus_slider_set_normalized(w, (ex / w3) / r);

        darktable.bauhaus->mouse_x = NAN;
      }
    }
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_bauhaus_slider_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  dt_bauhaus_widget_release_quad(widget);
  if(event->button == 1 && d->is_dragging)
  {
    d->is_dragging = 0;
    if(d->timeout_handle) g_source_remove(d->timeout_handle);
    d->timeout_handle = 0;
    dt_bauhaus_slider_set_normalized(w, d->pos);

    return TRUE;
  }
  return FALSE;
}

static gboolean dt_bauhaus_slider_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int w3 = allocation.width - w->margin->left - w->padding->left - w->margin->right - w->padding->right;
  const double ex = event->x - w->margin->left - w->padding->left;
  if(d->is_dragging && event->state & GDK_BUTTON1_MASK)
  {
    const float r = slider_right_pos((float)w3, w);

    if(isnan(darktable.bauhaus->mouse_x))
    {
      if(dt_modifier_is(event->state, 0))
        dt_bauhaus_slider_set_normalized(w, (ex / w3) / r);
      else
        darktable.bauhaus->mouse_x = ex;
    }
    else
    {
      const float scaled_step = w3 * r * dt_bauhaus_slider_get_step(widget) / (d->max - d->min);
      const float steps = floorf((ex - darktable.bauhaus->mouse_x) / scaled_step);
      _slider_add_step(widget, copysignf(1, d->factor) * steps, event->state, FALSE);

      darktable.bauhaus->mouse_x += steps * scaled_step;
    }
  }

  if(ex <= w3 - _widget_get_quad_width(w))
  {
    darktable.control->element
        = ex > (0.1 * (w3 - _widget_get_quad_width(w))) && ex < (0.9 * (w3 - _widget_get_quad_width(w)))
              ? DT_ACTION_ELEMENT_VALUE
              : DT_ACTION_ELEMENT_FORCE;
  }
  else
    darktable.control->element = DT_ACTION_ELEMENT_BUTTON;

  return TRUE;
}

static gboolean dt_bauhaus_combobox_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  darktable.control->element = event->x <= allocation.width - _widget_get_quad_width(w)
                                   ? DT_ACTION_ELEMENT_SELECTION
                                   : DT_ACTION_ELEMENT_BUTTON;

  return TRUE;
}


void dt_bauhaus_vimkey_exec(const char *input)
{
  dt_action_t *ac = darktable.control->actions_iops.target;
  input += 5; // skip ":set "

  while(ac)
  {
    const int prefix = strcspn(input, ".=");

    if(ac->type >= DT_ACTION_TYPE_WIDGET ||
       ac->type <= DT_ACTION_TYPE_SECTION)
    {
      if(!strncasecmp(ac->label, input, prefix))
      {
        if(!ac->label[prefix])
        {
          input += prefix;
          if(*input) input++; // skip . or =

          if(ac->type <= DT_ACTION_TYPE_SECTION)
          {
            ac = ac->target;
            continue;
          }
          else
            break;
        }
      }
    }

    ac = ac->next;
  }

  if(!ac || ac->type != DT_ACTION_TYPE_WIDGET || !ac->target || !DT_IS_BAUHAUS_WIDGET(ac->target))
    return;

  float old_value = .0f, new_value = .0f;

  GtkWidget *w = ac->target;

  switch(DT_BAUHAUS_WIDGET(w)->type)
  {
    case DT_BAUHAUS_SLIDER:
      old_value = dt_bauhaus_slider_get(w);
      new_value = dt_calculator_solve(old_value, input);
      fprintf(stderr, " = %f\n", new_value);
      if(isfinite(new_value)) dt_bauhaus_slider_set(w, new_value);
      break;
    case DT_BAUHAUS_COMBOBOX:
      // TODO: what about text as entry?
      old_value = dt_bauhaus_combobox_get(w);
      new_value = dt_calculator_solve(old_value, input);
      fprintf(stderr, " = %f\n", new_value);
      if(isfinite(new_value)) dt_bauhaus_combobox_set(w, new_value);
      break;
    default:
      break;
  }
}

// give autocomplete suggestions
GList *dt_bauhaus_vimkey_complete(const char *input)
{
  GList *res = NULL;

  dt_action_t *ac = darktable.control->actions_iops.target;

  while(ac)
  {
    const int prefix = strcspn(input, ".");

    if(ac->type >= DT_ACTION_TYPE_WIDGET ||
       ac->type <= DT_ACTION_TYPE_SECTION)
    {
      if(!prefix || !strncasecmp(ac->label, input, prefix))
      {
        if(!ac->label[prefix] && input[prefix] == '.')
        {
            input += prefix + 1;
          if(ac->type <= DT_ACTION_TYPE_SECTION) ac = ac->target;
          continue;
        }
        else
          res = g_list_append(res, (gchar *)ac->label + prefix);
      }
    }

    ac = ac->next;
  }
  return res;
}

void dt_bauhaus_combobox_mute_scrolling(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->mute_scrolling = TRUE;
}

static void _action_process_button(GtkWidget *widget, dt_action_effect_t effect)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(effect != (w->quad_paint_flags & CPF_ACTIVE ? DT_ACTION_EFFECT_ON : DT_ACTION_EFFECT_OFF))
    dt_bauhaus_widget_press_quad(widget);

  gchar *text = w->quad_paint_flags & CPF_ACTIVE ? _("button on") : _("button off");
  dt_action_widget_toast(w->module, widget, text);

  gtk_widget_queue_draw(widget);
}

static float _action_process_slider(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  GtkWidget *widget = GTK_WIDGET(target);
  dt_bauhaus_widget_t *bhw = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &bhw->data.slider;

  if(!isnan(move_size))
  {
    switch(element)
    {
    case DT_ACTION_ELEMENT_VALUE:
    case DT_ACTION_ELEMENT_FORCE:
      switch(effect)
      {
      case DT_ACTION_EFFECT_POPUP:
        dt_bauhaus_show_popup(widget);
        break;
      case DT_ACTION_EFFECT_DOWN:
        move_size *= -1;
      case DT_ACTION_EFFECT_UP:
        ++d->is_dragging;
        _slider_add_step(widget, move_size, GDK_MODIFIER_MASK, element == DT_ACTION_ELEMENT_FORCE);
        --d->is_dragging;
        break;
      case DT_ACTION_EFFECT_RESET:
        dt_bauhaus_widget_reset(widget);
        break;
      case DT_ACTION_EFFECT_TOP:
        dt_bauhaus_slider_set(widget, element == DT_ACTION_ELEMENT_FORCE ? d->hard_max: d->max);
        break;
      case DT_ACTION_EFFECT_BOTTOM:
        dt_bauhaus_slider_set(widget, element == DT_ACTION_ELEMENT_FORCE ? d->hard_min: d->min);
        break;
      case DT_ACTION_EFFECT_SET:
        dt_bauhaus_slider_set(widget, move_size);
        break;
      default:
        fprintf(stderr, "[_action_process_slider] unknown shortcut effect (%d) for slider\n", effect);
        break;
      }

      gchar *text = dt_bauhaus_slider_get_text(widget, dt_bauhaus_slider_get(widget));
      dt_action_widget_toast(bhw->module, widget, text);
      g_free(text);

      break;
    case DT_ACTION_ELEMENT_BUTTON:
      _action_process_button(widget, effect);
      break;
    case DT_ACTION_ELEMENT_ZOOM:
      ;
      switch(effect)
      {
      case DT_ACTION_EFFECT_POPUP:
        dt_bauhaus_show_popup(widget);
        break;
      case DT_ACTION_EFFECT_RESET:
        move_size = 0;
      case DT_ACTION_EFFECT_DOWN:
        move_size *= -1;
      case DT_ACTION_EFFECT_UP:
        _slider_zoom_range(bhw, move_size);
        break;
      case DT_ACTION_EFFECT_TOP:
      case DT_ACTION_EFFECT_BOTTOM:
        if((effect == DT_ACTION_EFFECT_TOP) ^ (d->factor < 0))
          d->max = d->hard_max;
        else
          d->min = d->hard_min;
        gtk_widget_queue_draw(widget);
        break;
      default:
        fprintf(stderr, "[_action_process_slider] unknown shortcut effect (%d) for slider\n", effect);
        break;
      }

      _slider_zoom_toast(bhw);
      break;
    default:
      fprintf(stderr, "[_action_process_slider] unknown shortcut element (%d) for slider\n", element);
      break;
    }
  }

  if(element == DT_ACTION_ELEMENT_BUTTON)
    return dt_bauhaus_widget_get_quad_active(widget);

  if(effect == DT_ACTION_EFFECT_SET)
    return dt_bauhaus_slider_get(widget);

  return d->pos +
         ( d->min == -d->max                             ? DT_VALUE_PATTERN_PLUS_MINUS :
         ( d->min == 0 && (d->max == 1 || d->max == 100) ? DT_VALUE_PATTERN_PERCENTAGE : 0 ));
}

gboolean combobox_idle_value_changed(gpointer widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  _bauhaus_combobox_set(w, w->data.combobox.active, FALSE);

  while(g_idle_remove_by_data(widget));

  return G_SOURCE_REMOVE;
}

static float _action_process_combo(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  GtkWidget *widget = GTK_WIDGET(target);
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  int value = dt_bauhaus_combobox_get(widget);

  if(!isnan(move_size))
  {
    if(element == DT_ACTION_ELEMENT_BUTTON || !w->data.combobox.entries->len)
    {
      _action_process_button(widget, effect);
      return dt_bauhaus_widget_get_quad_active(widget);
    }
    else switch(effect)
    {
    case DT_ACTION_EFFECT_POPUP:
      dt_bauhaus_show_popup(widget);
      break;
    case DT_ACTION_EFFECT_LAST:
      move_size *= - 1; // reversed in effect_previous
    case DT_ACTION_EFFECT_FIRST:
      move_size *= 1e3; // reversed in effect_previous
    case DT_ACTION_EFFECT_PREVIOUS:
      move_size *= - 1;
    case DT_ACTION_EFFECT_NEXT:
      ++darktable.gui->reset;
      _combobox_next_sensitive(w, move_size, FALSE);
      --darktable.gui->reset;

      g_idle_add(combobox_idle_value_changed, widget);
      break;
    case DT_ACTION_EFFECT_RESET:
      value = dt_bauhaus_combobox_get_default(widget);
      dt_bauhaus_combobox_set(widget, value);
      break;
    default:
      value = effect - DT_ACTION_EFFECT_COMBO_SEPARATOR - 1;
      dt_bauhaus_combobox_set(widget, value);
      break;
    }

    dt_action_widget_toast(w->module, widget, "\n%s", dt_bauhaus_combobox_get_text(widget));
  }

  if(element == DT_ACTION_ELEMENT_BUTTON || !w->data.combobox.entries->len)
    return dt_bauhaus_widget_get_quad_active(widget);

  for(int i = value; i >= 0; i--)
  {
    dt_bauhaus_combobox_entry_t *entry = g_ptr_array_index(w->data.combobox.entries, i);
    if(!entry->sensitive) value--; // don't count unselectable combo items in value
  }
  return - 1 - value + (value == effect - DT_ACTION_EFFECT_COMBO_SEPARATOR - 1 ? DT_VALUE_PATTERN_ACTIVE : 0);
}

const dt_action_element_def_t _action_elements_slider[]
  = { { N_("value"), dt_action_effect_value },
      { N_("button"), dt_action_effect_toggle },
      { N_("force"), dt_action_effect_value },
      { N_("zoom"), dt_action_effect_value },
      { NULL } };
const dt_action_element_def_t _action_elements_combo[]
  = { { N_("selection"), dt_action_effect_selection },
      { N_("button"), dt_action_effect_toggle },
      { NULL } };

static const dt_shortcut_fallback_t _action_fallbacks_slider[]
  = { { .element = DT_ACTION_ELEMENT_BUTTON, .button = DT_SHORTCUT_LEFT },
      { .element = DT_ACTION_ELEMENT_BUTTON, .effect = DT_ACTION_EFFECT_TOGGLE_CTRL, .button = DT_SHORTCUT_LEFT, .mods = GDK_CONTROL_MASK },
      { .element = DT_ACTION_ELEMENT_FORCE,  .mods   = GDK_CONTROL_MASK | GDK_SHIFT_MASK, .speed = 10.0 },
      { .element = DT_ACTION_ELEMENT_ZOOM,   .effect = DT_ACTION_EFFECT_DEFAULT_MOVE, .button = DT_SHORTCUT_RIGHT, .move = DT_SHORTCUT_MOVE_VERTICAL },
      { } };
static const dt_shortcut_fallback_t _action_fallbacks_combo[]
  = { { .element = DT_ACTION_ELEMENT_SELECTION, .effect = DT_ACTION_EFFECT_RESET, .button = DT_SHORTCUT_LEFT, .click = DT_SHORTCUT_DOUBLE },
      { .element = DT_ACTION_ELEMENT_BUTTON,    .button = DT_SHORTCUT_LEFT },
      { .element = DT_ACTION_ELEMENT_BUTTON,    .effect = DT_ACTION_EFFECT_TOGGLE_CTRL, .button = DT_SHORTCUT_LEFT, .mods = GDK_CONTROL_MASK },
      { .move    = DT_SHORTCUT_MOVE_SCROLL,     .effect = DT_ACTION_EFFECT_DEFAULT_MOVE, .speed = -1 },
      { .move    = DT_SHORTCUT_MOVE_VERTICAL,   .effect = DT_ACTION_EFFECT_DEFAULT_MOVE, .speed = -1 },
      { } };

const dt_action_def_t dt_action_def_slider
  = { N_("slider"),
      _action_process_slider,
      _action_elements_slider,
      _action_fallbacks_slider };
const dt_action_def_t dt_action_def_combo
  = { N_("dropdown"),
      _action_process_combo,
      _action_elements_combo,
      _action_fallbacks_combo };

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
