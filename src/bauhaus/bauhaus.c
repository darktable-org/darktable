/*
    This file is part of darktable,
    Copyright (C) 2012-2024 darktable developers.

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
#include "common/math.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

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

static const dt_action_def_t _action_def_slider, _action_def_combo,
                             _action_def_focus_slider, _action_def_focus_combo,
                             _action_def_focus_button;

// INNER_PADDING is the horizontal space between slider and quad
// and vertical space between labels and slider baseline
static const double INNER_PADDING = 4.0;

// fwd declare
static void _popup_show(GtkWidget *w);
static void _popup_reject(void);
static void _popup_hide(void);
static gboolean _popup_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static gboolean _popup_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static gboolean _widget_draw(GtkWidget *widget, cairo_t *crf);
static gboolean _widget_scroll(GtkWidget *widget, GdkEventScroll *event);
static gboolean _widget_key_press(GtkWidget *widget, GdkEventKey *event);
static gboolean _widget_button_press(GtkWidget *widget, GdkEventButton *event);
static gboolean _widget_button_release(GtkWidget *widget, GdkEventButton *event);
static gboolean _widget_motion_notify(GtkWidget *widget, GdkEventMotion *event);
static void _widget_get_preferred_width(GtkWidget *widget,
                                        gint *minimum_size,
                                        gint *natural_size);
static void _widget_get_preferred_height(GtkWidget *widget,
                                         gint *minimum_height,
                                         gint *natural_height);
static void _combobox_set(dt_bauhaus_widget_t *w,
                          const int pos,
                          const gboolean mute);
static void _slider_set_normalized(dt_bauhaus_widget_t *w,
                                   const float pos);

static void _request_focus(dt_bauhaus_widget_t *w)
{
  if(w->module && w->module->type == DT_ACTION_TYPE_IOP_INSTANCE)
      dt_iop_request_focus((dt_iop_module_t *)w->module);
  else if(dt_action_lib(w->module))
    darktable.lib->gui_module = dt_action_lib(w->module);

  gtk_widget_set_state_flags(GTK_WIDGET(w), GTK_STATE_FLAG_FOCUSED, FALSE);
}

static float _widget_get_quad_width(dt_bauhaus_widget_t *w)
{
  if(w->show_quad)
    return darktable.bauhaus->quad_width + INNER_PADDING;
  else
    return .0f;
}

static dt_bauhaus_combobox_entry_t *_combobox_entry(const dt_bauhaus_combobox_data_t *d, int pos)
{
  return g_ptr_array_index(d->entries, pos);
}

static void _combobox_next_sensitive(dt_bauhaus_widget_t *w,
                                     int delta,
                                     guint state,
                                     const gboolean mute)
{
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  delta *= dt_accel_get_speed_multiplier(GTK_WIDGET(w), state);
  int new_pos = d->active;
  const int step = delta > 0 ? 1 : -1;
  int cur = new_pos + step;
  gchar *keys = g_utf8_casefold(darktable.bauhaus->keys, darktable.bauhaus->keys_cnt);

  while(delta && cur >= 0 && cur < d->entries->len)
  {
    dt_bauhaus_combobox_entry_t *entry = _combobox_entry(d, cur);
    gchar *text_cmp = g_utf8_casefold(entry->label, -1);
    if(entry->sensitive && strstr(text_cmp, keys))
    {
      new_pos = cur;
      delta -= step;
    }
    g_free(text_cmp);
    cur += step;
  }

  g_free(keys);
  _combobox_set(w, new_pos, mute);
}

static dt_bauhaus_combobox_entry_t *_new_combobox_entry
  (const char *label,
   const dt_bauhaus_combobox_alignment_t alignment,
   const gboolean sensitive,
   void *data,
   void (*free_func)(void *))
{
  dt_bauhaus_combobox_entry_t *entry = calloc(1, sizeof(dt_bauhaus_combobox_entry_t));
  if(entry)
  {
    entry->label = g_strdup(label);
    entry->alignment = alignment;
    entry->sensitive = sensitive;
    entry->data = data;
    entry->free_func = free_func;
  }
  return entry;
}

static void _free_combobox_entry(gpointer data)
{
  dt_bauhaus_combobox_entry_t *entry = (dt_bauhaus_combobox_entry_t *)data;
  if(entry)
  {
    g_free(entry->label);
    if(entry->free_func)
      entry->free_func(entry->data);
    free(entry);
  }
}

static GdkRGBA * _default_color_assign()
{
  // helper to initialize a color pointer with red color as a default
  GdkRGBA color = {.red = 1.0f, .green = 0.0f, .blue = 0.0f, .alpha = 1.0f};
  return gdk_rgba_copy(&color);
}

static void _margins_retrieve(dt_bauhaus_widget_t *w)
{
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(w));
  const GtkStateFlags state = gtk_widget_get_state_flags(GTK_WIDGET(w));
  gtk_style_context_get_margin(context, state, &w->margin);
  gtk_style_context_get_padding(context, state, &w->padding);
}

void dt_bauhaus_widget_set_section(GtkWidget *widget, const gboolean is_section)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->is_section = is_section;
}

static int _show_pango_text(dt_bauhaus_widget_t *w,
                            GtkStyleContext *context,
                            cairo_t *cr,
                            const char *text,
                            float x_pos,
                            float y_pos,
                            const float max_width,
                            const gboolean right_aligned,
                            const gboolean calc_only,
                            PangoEllipsizeMode ellipsize,
                            const gboolean is_markup,
                            const gboolean is_label,
                            float *width,
                            float *height)
{
  PangoLayout *layout = pango_cairo_create_layout(cr);

  if(max_width > 0)
  {
    pango_layout_set_ellipsize(layout, ellipsize);
    pango_layout_set_width(layout, (int)(PANGO_SCALE * max_width + 0.5f));
  }

  PangoFontDescription *font_desc = 0;
  gtk_style_context_get(context,
                        gtk_widget_get_state_flags(GTK_WIDGET(w)), "font",
                        &font_desc, NULL);

  pango_layout_set_font_description(layout, font_desc);

  PangoAttrList *attrlist = pango_attr_list_new();
  pango_attr_list_insert(attrlist, pango_attr_font_features_new("tnum"));
  pango_layout_set_attributes(layout, attrlist);
  pango_attr_list_unref(attrlist);

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
  dt_bauhaus_t *bh = darktable.bauhaus;

  if(bh->cursor_blink_counter > 0)
    bh->cursor_blink_counter--;

  bh->cursor_visible = !bh->cursor_visible;
  gtk_widget_queue_draw(bh->popup.area);

  // this can be >0 when we haven't reached the desired number or -1
  // when blinking forever
  if(bh->cursor_blink_counter != 0)
    return TRUE;

  bh->cursor_timeout = 0; // otherwise the cursor won't
                                         // come up when starting to
                                         // type
  return FALSE;
}

static void _start_cursor(const int max_blinks)
{
  dt_bauhaus_t *bh = darktable.bauhaus;
  bh->cursor_blink_counter = max_blinks;
  bh->cursor_visible = FALSE;
  if(bh->cursor_timeout == 0)
    bh->cursor_timeout = g_timeout_add(500, _cursor_timeout_callback, NULL);
}

static void _stop_cursor()
{
  dt_bauhaus_t *bh = darktable.bauhaus;
  if(bh->cursor_timeout > 0)
  {
    g_source_remove(bh->cursor_timeout);
    bh->cursor_timeout = 0;
    bh->cursor_visible = FALSE;
  }
}
// -------------------------------

static float _slider_right_pos(const float width,
                               dt_bauhaus_widget_t *w)
{
  // relative position (in widget) of the right bound of the slider
  // corrected with the inner padding
  return 1.0f - _widget_get_quad_width(w) / width;
}

static float _slider_coordinate(const float abs_position,
                                const float width,
                                dt_bauhaus_widget_t *w)
{
  // Translates an horizontal position relative to the slider in an
  // horizontal position relative to the widget
  const float left_bound = 0.0f;
  const float right_bound = _slider_right_pos(width, w); // exclude the quad area on
                                                         // the right
  return (left_bound + abs_position * (right_bound - left_bound)) * width;
}


static float _slider_get_line_offset(const float pos,
                                     const float scale,
                                     float x,
                                     float y,
                                     const float ht,
                                     const int width,
                                     dt_bauhaus_widget_t *w)
{
  // ht is in [0,1] scale here
  const float l = 0.0f;
  const float r = _slider_right_pos(width, w);

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
  if(pos + offset > 1.0f)
    offset = 1.0f - pos;
  if(pos + offset < 0.0f)
    offset = -pos;
  return offset;
}

// draw a loupe guideline for the quadratic zoom in in the slider interface:
static void _slider_draw_line(cairo_t *cr,
                              float pos,
                              float off,
                              float scale,
                              const int width,
                              const int height,
                              const int ht,
                              dt_bauhaus_widget_t *w)
{
  // pos is normalized position [0,1], offset is on that scale.
  // ht is in pixels here
  const float r = _slider_right_pos(width, w);

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

static void _slider_zoom_range(dt_bauhaus_widget_t *w,
                               const float zoom)
{
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  const float value = dt_bauhaus_slider_get(GTK_WIDGET(w));

  if(!zoom)
  {
    d->min = d->soft_min;
    d->max = d->soft_max;
    dt_bauhaus_slider_set(GTK_WIDGET(w), value); // restore value (and
                                                 // move min/max again
                                                 // if needed)
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
  if(darktable.bauhaus->current == w)
    gtk_widget_queue_draw(darktable.bauhaus->popup.window);
}

static void _slider_zoom_toast(dt_bauhaus_widget_t *w)
{
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  gchar *min_text = dt_bauhaus_slider_get_text(GTK_WIDGET(w),
                                               d->factor > 0 ? d->min : d->max);
  gchar *max_text = dt_bauhaus_slider_get_text(GTK_WIDGET(w),
                                               d->factor > 0 ? d->max : d->min);
  dt_action_widget_toast(w->module, GTK_WIDGET(w), "\n[%s , %s]", min_text, max_text);
  g_free(min_text);
  g_free(max_text);
}

static gboolean _popup_scroll(GtkWidget *widget,
                              GdkEventScroll *event,
                              gpointer user_data)
{
  dt_bauhaus_widget_t *w = darktable.bauhaus->current;
  int delta_y = 0;
  if(dt_gui_get_scroll_unit_delta(event, &delta_y))
  {
    if(w->type == DT_BAUHAUS_COMBOBOX)
      _combobox_next_sensitive(w, delta_y, 0, w->data.combobox.mute_scrolling);
    else
      _slider_zoom_range(w, delta_y);
  }
  return TRUE;
}

static void _window_moved_to_rect(GdkWindow *window,
                                  GdkRectangle *flipped_rect,
                                  GdkRectangle *final_rect,
                                  gboolean flipped_x,
                                  gboolean flipped_y,
                                  gpointer user_data)
{
  darktable.bauhaus->popup.offcut += final_rect->y - flipped_rect->y;
}

static void _window_position(const int offset)
{
  dt_bauhaus_popup_t *pop = &darktable.bauhaus->popup;

  if(pop->composited && gtk_widget_get_visible(pop->window))
  {
    pop->offcut += offset;
    return;
  }

  int height = pop->position.height;
  pop->offset += offset;

  pop->composited = FALSE;
  // On Xwayland gdk_screen_is_composited is TRUE but popups are opaque
  // So we need to explicitly test for pure wayland
#ifdef GDK_WINDOWING_WAYLAND
  if(GDK_IS_WAYLAND_DISPLAY(gtk_widget_get_display(pop->window)))
  {
    pop->composited = TRUE;
    gtk_widget_set_app_paintable(pop->window, TRUE);
    GdkScreen *screen = gtk_widget_get_screen(pop->window);
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    pop->offcut = -height;
    height *= 2;
    gtk_widget_set_visual(pop->window, visual);
  }
#endif

  if(pop->offcut > 0)
    pop->offcut = MAX(0, pop->offcut + offset);

  GdkWindow *window = gtk_widget_get_window(pop->window);
  gdk_window_resize(window, pop->position.width, height - pop->offcut);
  gdk_window_move_to_rect(window, &pop->position,
                          GDK_GRAVITY_NORTH_WEST,
                          GDK_GRAVITY_NORTH_WEST,
                          GDK_ANCHOR_SLIDE_X | GDK_ANCHOR_RESIZE_Y,
                          0, - pop->offset + pop->offcut);
}

static gboolean _window_motion_notify(GtkWidget *widget,
                                      GdkEventMotion *event,
                                      gpointer user_data)
{
  dt_bauhaus_t *bh = darktable.bauhaus;
  dt_bauhaus_popup_t *pop = &bh->popup;
  dt_bauhaus_widget_t *w = bh->current;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  const GtkBorder *padding = &pop->padding;

  // recalculate event coords so we get useful values outside window
  GdkWindow *window = gtk_widget_get_window(pop->area);
  gdk_window_get_origin(window, &allocation.x, &allocation.y);
  gint ex = event->x_root - allocation.x;
  gint ey = event->y_root - allocation.y;

  const float tol = 50;
  if(ex < - tol || ex > allocation.width + tol
     || ey + pop->offcut < - tol
     || ey + pop->offcut > pop->position.height + tol)
  {
    _popup_reject();
    return TRUE;
  }

  if(bh->keys_cnt == 0) _stop_cursor();

  bh->mouse_x = ex - padding->left;
  const float move = bh->mouse_y - ey - pop->offcut + padding->top;
  int dy = 0;
  if(move < 0 && pop->position.height > pop->offcut + allocation.height)
  {
    dy = pop->position.height - pop->offcut - allocation.height;
    if(ey < allocation.height)
      dy *= move / (move + ey - allocation.height);
  }
  else if(move > 0 && pop->offcut > 0)
  {
    dy = - pop->offcut;
    if(ey >= 0)
      dy *= move / (move + ey);
  }
  bh->mouse_y -= move - dy;
  if(dy)
    _window_position(dy);

  if(w->type == DT_BAUHAUS_SLIDER)
  {
    const dt_bauhaus_slider_data_t *d = &w->data.slider;
    const float width = allocation.width - padding->left - padding->right;
    const float ht = bh->line_height + INNER_PADDING * 2.0f;
    const float mouse_off = _slider_get_line_offset
      (d->oldpos, 5.0 * powf(10.0f, -d->digits) / (d->max - d->min) / d->factor,
       bh->mouse_x / width, bh->mouse_y / width, ht / width, allocation.width, w);
    if(!bh->change_active)
    {
      if((bh->mouse_line_distance < 0 && mouse_off >= 0)
          || (bh->mouse_line_distance > 0 && mouse_off <= 0)
          || event->state & GDK_BUTTON1_MASK)
        bh->change_active = TRUE;
      bh->mouse_line_distance = mouse_off;
    }
    if(bh->change_active)
      _slider_set_normalized(w, d->oldpos + mouse_off);
  }
  else if(w->type == DT_BAUHAUS_COMBOBOX)
  {
    dt_bauhaus_combobox_data_t *d = &w->data.combobox;
    const int active = (bh->mouse_y - w->top_gap) / bh->line_height;
    if(active >= 0 && active < d->entries->len)
    {
      if(_combobox_entry(d, active)->sensitive && event->state & GDK_BUTTON1_MASK)
      {
        if(active != d->active)
          _combobox_set(w, active, w->data.combobox.mute_scrolling);
      }
    }
  }

  gtk_widget_queue_draw(pop->area);
  return TRUE;
}

static gboolean _popup_leave_notify(GtkWidget *widget,
                                    GdkEventCrossing *event,
                                    gpointer user_data)
{
  gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_NORMAL, TRUE);
  return TRUE;
}

static gboolean _popup_button_release(GtkWidget *widget,
                                      GdkEventButton *event,
                                      gpointer user_data)
{
  if(darktable.bauhaus->change_active)
    _popup_hide();

  return TRUE;
}

static gboolean _popup_button_press(GtkWidget *widget,
                                    GdkEventButton *event,
                                    gpointer user_data)
{
  if(event->window != gtk_widget_get_window(widget))
  {
    _popup_reject();
    return TRUE;
  }

  dt_bauhaus_t *bh = darktable.bauhaus;
  dt_bauhaus_widget_t *w = bh->current;

  if(event->button == 1)
  {
    // only accept left mouse click
    gtk_widget_set_state_flags(GTK_WIDGET(w),
                               GTK_STATE_FLAG_FOCUSED, FALSE);

    if(w->type == DT_BAUHAUS_COMBOBOX
       && !dt_gui_long_click(event->time, bh->opentime))
    {
      // counts as double click, reset:
      if(!(dt_modifier_is(event->state, GDK_CONTROL_MASK) && w->field
          && dt_gui_presets_autoapply_for_module((dt_iop_module_t *)w->module,
                                                 GTK_WIDGET(w))))
        dt_bauhaus_widget_reset(GTK_WIDGET(w));
    }

    bh->change_active = TRUE;
    event->state |= GDK_BUTTON1_MASK;
    _window_motion_notify(widget, (GdkEventMotion*)event, user_data);
  }
  else if(event->button == 2 && w->type == DT_BAUHAUS_SLIDER)
    _slider_zoom_range(w, 0);
  else
    _popup_reject();

  return TRUE;
}

static void _window_show(GtkWidget *w, gpointer user_data)
{
  // make sure combo popup handles button release
  gtk_grab_add(GTK_WIDGET(user_data));
}

static void dt_bh_init(DtBauhausWidget *w)
{
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

static gboolean _widget_enter_leave(GtkWidget *widget, GdkEventCrossing *event)
{
  if(event->type == GDK_ENTER_NOTIFY)
    // gtk_widget_set_state_flags triggers resize&draw avalanche
    // instead add GTK_STATE_FLAG_PRELIGHT in _widget_draw
    darktable.bauhaus->hovered = widget;
  else
    darktable.bauhaus->hovered = NULL;

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
  g_free(w->tooltip);

  G_OBJECT_CLASS(dt_bh_parent_class)->finalize(widget);
}

static void dt_bh_class_init(DtBauhausWidgetClass *class)
{
  darktable.bauhaus->signals[DT_BAUHAUS_VALUE_CHANGED_SIGNAL]
      = g_signal_new("value-changed", G_TYPE_FROM_CLASS(class),
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                     g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
  darktable.bauhaus->signals[DT_BAUHAUS_QUAD_PRESSED_SIGNAL]
      = g_signal_new("quad-pressed", G_TYPE_FROM_CLASS(class),
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                     g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);
  widget_class->draw = _widget_draw;
  widget_class->scroll_event = _widget_scroll;
  widget_class->key_press_event = _widget_key_press;
  widget_class->button_press_event = _widget_button_press;
  widget_class->button_release_event = _widget_button_release;
  widget_class->motion_notify_event = _widget_motion_notify;
  widget_class->get_preferred_width = _widget_get_preferred_width;
  widget_class->get_preferred_height = _widget_get_preferred_height;
  widget_class->enter_notify_event = _widget_enter_leave;
  widget_class->leave_notify_event = _widget_enter_leave;
  G_OBJECT_CLASS(class)->finalize = _widget_finalize;
}

void dt_bauhaus_load_theme()
{
  GtkWidget *root_window = dt_ui_main_window(darktable.gui->ui);
  GtkStyleContext *ctx = gtk_style_context_new();
  GtkWidgetPath *path = gtk_widget_path_new();
  const int pos = gtk_widget_path_append_type(path, GTK_TYPE_WIDGET);
  gtk_widget_path_iter_add_class(path, pos, "dt_bauhaus");
  gtk_style_context_set_path(ctx, path);
  gtk_style_context_set_screen (ctx, gtk_widget_get_screen(root_window));

  dt_bauhaus_t *bh = darktable.bauhaus;
  gtk_style_context_lookup_color(ctx, "bauhaus_fg",
                                 &bh->color_fg);
  gtk_style_context_lookup_color(ctx, "bauhaus_fg_hover",
                                 &bh->color_fg_hover);
  gtk_style_context_lookup_color(ctx, "bauhaus_fg_insensitive",
                                 &bh->color_fg_insensitive);
  gtk_style_context_lookup_color(ctx, "bauhaus_bg",
                                 &bh->color_bg);
  gtk_style_context_lookup_color(ctx, "bauhaus_border",
                                 &bh->color_border);
  gtk_style_context_lookup_color(ctx, "bauhaus_fill",
                                 &bh->color_fill);
  gtk_style_context_lookup_color(ctx, "bauhaus_indicator_border",
                                 &bh->indicator_border);

  gtk_style_context_lookup_color(ctx, "graph_bg",
                                 &bh->graph_bg);
  gtk_style_context_lookup_color(ctx, "graph_exterior",
                                 &bh->graph_exterior);
  gtk_style_context_lookup_color(ctx, "graph_border",
                                 &bh->graph_border);
  gtk_style_context_lookup_color(ctx, "graph_grid",
                                 &bh->graph_grid);
  gtk_style_context_lookup_color(ctx, "graph_fg",
                                 &bh->graph_fg);
  gtk_style_context_lookup_color(ctx, "graph_fg_active",
                                 &bh->graph_fg_active);
  gtk_style_context_lookup_color(ctx, "graph_overlay",
                                 &bh->graph_overlay);
  gtk_style_context_lookup_color(ctx, "inset_histogram",
                                 &bh->inset_histogram);
  gtk_style_context_lookup_color(ctx, "graph_red",
                                 &bh->graph_colors[0]);
  gtk_style_context_lookup_color(ctx, "graph_green",
                                 &bh->graph_colors[1]);
  gtk_style_context_lookup_color(ctx, "graph_blue",
                                 &bh->graph_colors[2]);
  gtk_style_context_lookup_color(ctx, "colorlabel_red",
                                 &bh->colorlabels[DT_COLORLABELS_RED]);
  gtk_style_context_lookup_color(ctx, "colorlabel_yellow",
                                 &bh->colorlabels[DT_COLORLABELS_YELLOW]);
  gtk_style_context_lookup_color(ctx, "colorlabel_green",
                                 &bh->colorlabels[DT_COLORLABELS_GREEN]);
  gtk_style_context_lookup_color(ctx, "colorlabel_blue",
                                 &bh->colorlabels[DT_COLORLABELS_BLUE]);
  gtk_style_context_lookup_color(ctx, "colorlabel_purple",
                                 &bh->colorlabels[DT_COLORLABELS_PURPLE]);

  // make sure we release previously loaded font
  if(bh->pango_font_desc)
    pango_font_description_free(bh->pango_font_desc);
  bh->pango_font_desc = NULL;
  gtk_style_context_get(ctx, GTK_STATE_FLAG_NORMAL, "font",
                        &bh->pango_font_desc, NULL);

  if(bh->pango_sec_font_desc)
    pango_font_description_free(bh->pango_sec_font_desc);
  bh->pango_sec_font_desc = NULL;

  // now get the font for the section labels
  gtk_widget_path_iter_add_class(path, pos, "dt_section_label");
  gtk_style_context_set_path(ctx, path);
  gtk_style_context_get(ctx, GTK_STATE_FLAG_NORMAL, "font",
                        &bh->pango_sec_font_desc, NULL);
  gtk_widget_path_free(path);

  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 128, 128);
  cairo_t *cr = cairo_create(cst);
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_text(layout, "m", -1);
  pango_layout_set_font_description(layout, bh->pango_font_desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
  int pango_width, pango_height;
  pango_layout_get_size(layout, &pango_width, &pango_height);
  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_surface_destroy(cst);

  bh->line_height = pango_height / PANGO_SCALE;
  bh->quad_width = bh->line_height;

  // absolute size in Cairo unit:
  bh->baseline_size = bh->line_height / 2.5f;
  bh->border_width = 2.0f; // absolute size in Cairo unit
  bh->marker_size = (bh->baseline_size + bh->border_width) * 0.9f;
}

void dt_bauhaus_init()
{
  darktable.bauhaus = (dt_bauhaus_t *)calloc(1, sizeof(dt_bauhaus_t));
  dt_bauhaus_t *bh = darktable.bauhaus;
  dt_bauhaus_popup_t *pop = &bh->popup;

  bh->keys_cnt = 0;
  bh->current = NULL;
  bh->pango_font_desc = NULL;

  dt_bauhaus_load_theme();

  bh->skip_accel = 1;
  bh->combo_introspection = g_hash_table_new(NULL, NULL);
  bh->combo_list = g_hash_table_new(NULL, NULL);

  pop->window = gtk_window_new(GTK_WINDOW_POPUP);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(pop->window);
#endif
  gtk_widget_set_size_request(pop->window, 1, 1);
  gtk_window_set_keep_above(GTK_WINDOW(pop->window), TRUE);
  gtk_window_set_modal(GTK_WINDOW(pop->window), TRUE);
  gtk_window_set_type_hint(GTK_WINDOW(pop->window),
                           GDK_WINDOW_TYPE_HINT_POPUP_MENU);

  pop->area = gtk_drawing_area_new();
  g_object_set(pop->area, "expand", TRUE, NULL);
  gtk_container_add(GTK_CONTAINER(pop->window), pop->area);
  gtk_widget_set_can_focus(pop->area, TRUE);
  gtk_widget_add_events(pop->area,
                        GDK_POINTER_MOTION_MASK
                        | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                        | GDK_KEY_PRESS_MASK | GDK_LEAVE_NOTIFY_MASK
                        | darktable.gui->scroll_mask);

  GObject *window = G_OBJECT(pop->window);
  GObject *area = G_OBJECT(pop->area);

  gtk_widget_realize(pop->window);
  g_signal_connect(gtk_widget_get_window(pop->window),
                   "moved-to-rect", G_CALLBACK(_window_moved_to_rect), NULL);
  g_signal_connect(window, "show", G_CALLBACK(_window_show), area);
  g_signal_connect(window, "motion-notify-event", G_CALLBACK(_window_motion_notify), NULL);
  g_signal_connect(area, "draw", G_CALLBACK(_popup_draw), NULL);
  g_signal_connect(area, "leave-notify-event", G_CALLBACK(_popup_leave_notify), NULL);
  g_signal_connect(area, "button-press-event", G_CALLBACK(_popup_button_press), NULL);
  g_signal_connect(area, "button-release-event", G_CALLBACK (_popup_button_release), NULL);
  g_signal_connect(area, "key-press-event", G_CALLBACK(_popup_key_press), NULL);
  g_signal_connect(area, "scroll-event", G_CALLBACK(_popup_scroll), NULL);

  dt_action_define(&darktable.control->actions_focus, NULL, N_("sliders"),
                   NULL, &_action_def_focus_slider);
  dt_action_define(&darktable.control->actions_focus, NULL, N_("dropdowns"),
                   NULL, &_action_def_focus_combo);
  dt_action_define(&darktable.control->actions_focus, NULL, N_("buttons"),
                   NULL, &_action_def_focus_button);
}

void dt_bauhaus_cleanup()
{
}

// end static init/cleanup
// =================================================


void dt_bauhaus_combobox_set_default(GtkWidget *widget, const int def)
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

void dt_bauhaus_slider_set_hard_min(GtkWidget* widget, const float val)
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

void dt_bauhaus_slider_set_hard_max(GtkWidget* widget, const float val)
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

void dt_bauhaus_slider_set_soft_min(GtkWidget* widget, const float val)
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

void dt_bauhaus_slider_set_soft_max(GtkWidget* widget, const float val)
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

void dt_bauhaus_slider_set_default(GtkWidget *widget, const float def)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->defpos = def;
}

void dt_bauhaus_slider_set_soft_range(GtkWidget *widget,
                                      const float soft_min,
                                      const float soft_max)
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

dt_action_t *dt_bauhaus_widget_set_label(GtkWidget *widget,
                                         const char *section,
                                         const char *label)
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
                            w->type == DT_BAUHAUS_SLIDER
                            ? &_action_def_slider
                            : &_action_def_combo);
      if(w->module->type != DT_ACTION_TYPE_IOP_INSTANCE)
        w->module = ac;
    }

    // if new bauhaus widget added to front of widget_list; move it to the back
    dt_iop_module_t *m = (dt_iop_module_t *)w->module;
    if(w->module->type == DT_ACTION_TYPE_IOP_INSTANCE
       && w->field
       && m->widget_list
       && ((dt_action_target_t *)m->widget_list->data)->target == (gpointer)widget)
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

void dt_bauhaus_widget_hide_label(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->show_label = FALSE;
}

void dt_bauhaus_widget_set_quad_paint(GtkWidget *widget,
                                      dt_bauhaus_quad_paint_f f,
                                      const int paint_flags,
                                      void *paint_data)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->quad_paint = f;
  w->quad_paint_flags = paint_flags;
  w->quad_paint_data = paint_data;
}

void dt_bauhaus_widget_set_quad_tooltip(GtkWidget *widget,
                                        const gchar *text)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(!w) return;

  g_free(w->tooltip);
  w->tooltip = g_strdup(text);
}

gchar *dt_bauhaus_widget_get_tooltip_markup(GtkWidget *widget,
                                            dt_action_element_t element)
{
  gchar *tooltip = DT_IS_BAUHAUS_WIDGET(widget)
                   && element == DT_ACTION_ELEMENT_BUTTON
                 ? DT_BAUHAUS_WIDGET(widget)->tooltip : NULL;
  if(!(tooltip))
    return gtk_widget_get_tooltip_markup(widget);

  return g_markup_escape_text(tooltip, -1);
}

void dt_bauhaus_widget_set_field(GtkWidget *widget,
                                 gpointer field,
                                 const dt_introspection_type_t field_type)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(*w->label)
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_bauhaus_widget_set_field] bauhaus label '%s'"
             " set before field (needs to be after)",
             w->label);
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

  for(GList *c = gtk_container_get_children(GTK_CONTAINER(w));
      c;
      c = g_list_delete_link(c, c))
  {
    if(!is_changed && DT_IS_BAUHAUS_WIDGET(c->data) && gtk_widget_get_visible(c->data))
    {
      dt_bauhaus_widget_t *b = DT_BAUHAUS_WIDGET(c->data);
      if(!b->field) continue;
      if(b->type == DT_BAUHAUS_SLIDER)
      {
        dt_bauhaus_slider_data_t *d = &b->data.slider;
        is_changed = fabsf(d->pos - d->curve((d->defpos - d->min) / (d->max - d->min),
                                             DT_BAUHAUS_SET)) > 0.001f;
      }
      else
        is_changed = b->data.combobox.entries->len
          && b->data.combobox.active != b->data.combobox.defpos;
    }
  }

  GtkWidget *label = gtk_notebook_get_tab_label(GTK_NOTEBOOK(notebook), w);

  if(is_changed)
    dt_gui_add_class(label, "changed");
  else
    dt_gui_remove_class(label, "changed");
}

void dt_bauhaus_update_from_field(dt_iop_module_t *module,
                                  GtkWidget *widget,
                                  gpointer params,
                                  gpointer blend_params)
{
  GtkWidget *notebook = NULL;
  for(GSList *w = widget ? &(GSList){} : module->widget_list_bh; w; w = w->next)
  {
    dt_action_target_t *at = w->data;
    if(at) widget = at->target;
    dt_bauhaus_widget_t *bhw = DT_BAUHAUS_WIDGET(widget);
    if(!bhw) continue;

    gpointer field = bhw->field;

    if(params)
    {
      int offset = field - module->params;
      if(offset >= 0 && offset < module->params_size)
        field = params + offset;
      else
      {
        offset = field - (gpointer)module->blend_params;
        if(offset >= 0 && offset < sizeof(dt_develop_blend_params_t) && blend_params)
          field = blend_params + offset;
      }
    }

    switch(bhw->type)
    {
      case DT_BAUHAUS_SLIDER:
        switch(bhw->field_type)
        {
          case DT_INTROSPECTION_TYPE_FLOAT:
            dt_bauhaus_slider_set(widget, *(float *)field);
            break;
          case DT_INTROSPECTION_TYPE_INT:
            dt_bauhaus_slider_set(widget, *(int *)field);
            break;
          case DT_INTROSPECTION_TYPE_USHORT:
            dt_bauhaus_slider_set(widget, *(unsigned short *)field);
            break;
          default:
            dt_print(DT_DEBUG_ALWAYS,
                     "[dt_bauhaus_update_from_field] unsupported slider data type");
        }
        break;
      case DT_BAUHAUS_COMBOBOX:
        switch(bhw->field_type)
        {
          case DT_INTROSPECTION_TYPE_ENUM:
            dt_bauhaus_combobox_set_from_value(widget, *(int *)field);
            break;
          case DT_INTROSPECTION_TYPE_INT:
            dt_bauhaus_combobox_set(widget, *(int *)field);
            break;
          case DT_INTROSPECTION_TYPE_UINT:
            dt_bauhaus_combobox_set(widget, *(unsigned int *)field);
            break;
          case DT_INTROSPECTION_TYPE_BOOL:
            dt_bauhaus_combobox_set(widget, *(gboolean *)field);
            break;
          default:
            dt_print(DT_DEBUG_ALWAYS,
                     "[dt_bauhaus_update_from_field] unsupported combo data type");
        }
        break;
      default:
        dt_print
          (DT_DEBUG_ALWAYS,
           "[dt_bauhaus_update_from_field] invalid bauhaus widget type encountered");
    }

    // if gui->reset then notebook tab highlights were not yet changed
    if(!notebook && darktable.gui->reset
       && (notebook = gtk_widget_get_parent(widget))
       && (notebook = gtk_widget_get_parent(notebook))
       && !GTK_IS_NOTEBOOK(notebook))
      notebook = NULL;
  }

  if(notebook)
    gtk_container_foreach(GTK_CONTAINER(notebook),
                          _highlight_changed_notebook_tab, NULL);
}

// make this quad a toggle button:
void dt_bauhaus_widget_set_quad_toggle(GtkWidget *widget,
                                       const int toggle)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->quad_toggle = toggle;
}

void dt_bauhaus_widget_set_quad_active(GtkWidget *widget,
                                       const int active)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(active)
    w->quad_paint_flags |= CPF_ACTIVE;
  else
    w->quad_paint_flags &= ~CPF_ACTIVE;
  gtk_widget_queue_draw(GTK_WIDGET(w));
}

void dt_bauhaus_widget_set_quad_visibility(GtkWidget *widget,
                                           const gboolean visible)
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
    w->quad_paint_flags ^= CPF_ACTIVE;
  else
    w->quad_paint_flags |= CPF_ACTIVE;
  gtk_widget_queue_draw(GTK_WIDGET(w));

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

static float _default_linear_curve(const float value,
                                   const dt_bauhaus_curve_t dir)
{
  // regardless of dir: input <-> output
  return value;
}

static float _reverse_linear_curve(const float value,
                                   const dt_bauhaus_curve_t dir)
{
  // regardless of dir: input <-> output
  return 1.0 - value;
}

static float _curve_log10(const float inval,
                          const dt_bauhaus_curve_t dir)
{
  if(dir == DT_BAUHAUS_SET)
    return log10f(inval * 999.0f + 1.0f) / 3.0f;
  else
    return (expf(M_LN10 * inval * 3.0f) - 1.0f) / 999.0f;
}

GtkWidget *dt_bauhaus_slider_new(dt_iop_module_t *self)
{
  return dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.1, 0.5, 3);
}

GtkWidget *dt_bauhaus_slider_new_with_range(dt_iop_module_t *self,
                                            const float min,
                                            const float max,
                                            const float step,
                                            const float defval,
                                            const int digits)
{
  return dt_bauhaus_slider_new_with_range_and_feedback
    (self, min, max, step, defval, digits, 1);
}

GtkWidget *dt_bauhaus_slider_new_action(dt_action_t *self,
                                        const float min,
                                        const float max,
                                        const float step,
                                        const float defval,
                                        const int digits)
{
  return dt_bauhaus_slider_new_with_range((dt_iop_module_t *)self,
                                          min, max, step, defval, digits);
}

GtkWidget *dt_bauhaus_slider_new_with_range_and_feedback(dt_iop_module_t *self,
                                                         const float min,
                                                         const float max,
                                                         const float step,
                                                         const float defval,
                                                         const int digits,
                                                         const int feedback)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(g_object_new(DT_BAUHAUS_WIDGET_TYPE, NULL));
  return dt_bauhaus_slider_from_widget(w, self, min, max, step, defval, digits, feedback);
}


GtkWidget *dt_bauhaus_slider_from_widget(dt_bauhaus_widget_t* w,
                                         dt_iop_module_t *self,
                                         const float min,
                                         const float max,
                                         const float step,
                                         const float defval,
                                         const int digits,
                                         const int feedback)
{
  w->type = DT_BAUHAUS_SLIDER;
  w->module = DT_ACTION(self);
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

  return GTK_WIDGET(w);
}

GtkWidget *dt_bauhaus_combobox_new(dt_iop_module_t *self)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(g_object_new(DT_BAUHAUS_WIDGET_TYPE, NULL));
  return dt_bauhaus_combobox_from_widget(w, self);
}

GtkWidget *dt_bauhaus_combobox_new_action(dt_action_t *self)
{
  return dt_bauhaus_combobox_new((dt_iop_module_t *)self);
}

GtkWidget *dt_bauhaus_combobox_new_full(dt_action_t *action,
                                        const char *section,
                                        const char *label,
                                        const char *tip,
                                        const int pos,
                                        GtkCallback callback,
                                        gpointer data,
                                        const char **texts)
{
  GtkWidget *combo = dt_bauhaus_combobox_new_action(action);
  dt_action_t *ac = dt_bauhaus_widget_set_label(combo, section, label);
  dt_bauhaus_combobox_add_list(combo, ac, texts);
  dt_bauhaus_combobox_set(combo, pos);
  gtk_widget_set_tooltip_text(combo, tip ? tip : _(label));
  if(callback)
    g_signal_connect(G_OBJECT(combo), "value-changed", G_CALLBACK(callback), data);

  return combo;
}

GtkWidget *dt_bauhaus_combobox_from_widget(dt_bauhaus_widget_t* w,
                                           dt_iop_module_t *self)
{
  w->type = DT_BAUHAUS_COMBOBOX;
  w->module = DT_ACTION(self);
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->entries = g_ptr_array_new_full(4, _free_combobox_entry);
  d->defpos = -1;
  d->active = -1;
  d->editable = 0;
  d->text_align = DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT;
  d->entries_ellipsis = PANGO_ELLIPSIZE_END;
  d->mute_scrolling = FALSE;
  d->populate = NULL;
  d->text = NULL;

  gtk_widget_set_name(GTK_WIDGET(w), "bauhaus-combobox");

  return GTK_WIDGET(w);
}

static dt_bauhaus_combobox_data_t *_combobox_data(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return NULL;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  if(d->active >= d->entries->len) d->active = -1;

  return d;
}

void dt_bauhaus_combobox_add_populate_fct(GtkWidget *widget,
                                          void (*fct)(GtkWidget *w,
                                                      struct dt_iop_module_t **module))
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type == DT_BAUHAUS_COMBOBOX)
    w->data.combobox.populate = fct;
}

void dt_bauhaus_combobox_add_list(GtkWidget *widget,
                                  dt_action_t *action,
                                  const char **texts)
{
  if(action)
    g_hash_table_insert(darktable.bauhaus->combo_list, action, texts);

  while(texts && *texts)
    dt_bauhaus_combobox_add(widget, Q_(*(texts++)));
}

gboolean dt_bauhaus_combobox_add_introspection
  (GtkWidget *widget,
   dt_action_t *action,
   const dt_introspection_type_enum_tuple_t *list,
   const int start,
   const int end)
{
  dt_introspection_type_enum_tuple_t *item = (dt_introspection_type_enum_tuple_t *)list;

  if(action)
    g_hash_table_insert(darktable.bauhaus->combo_introspection, action, (gpointer)list);

  while(item->name && item->value != start) item++;
  for(; item->name; item++)
  {
    const char *text = item->description ? item->description : item->name;
    if(*text)
      dt_bauhaus_combobox_add_full(widget, Q_(text),
                                   DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                   GUINT_TO_POINTER(item->value), NULL, TRUE);
    if(item->value == end) return TRUE;
  }
  return FALSE;
}


void dt_bauhaus_combobox_add(GtkWidget *widget,
                             const char *text)
{
  dt_bauhaus_combobox_add_full(widget, text,
                               DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, NULL, NULL, TRUE);
}

void dt_bauhaus_combobox_add_section(GtkWidget *widget,
                                     const char *text)
{
  dt_bauhaus_combobox_add_full(widget, text,
                               DT_BAUHAUS_COMBOBOX_ALIGN_LEFT,
                               GINT_TO_POINTER(-1), NULL, FALSE);
}

void dt_bauhaus_combobox_add_aligned(GtkWidget *widget,
                                     const char *text,
                                     const dt_bauhaus_combobox_alignment_t align)
{
  dt_bauhaus_combobox_add_full(widget, text, align, NULL, NULL, TRUE);
}

void dt_bauhaus_combobox_add_full(GtkWidget *widget,
                                  const char *text,
                                  dt_bauhaus_combobox_alignment_t align,
                                  gpointer data, void (free_func)(void *data),
                                  const gboolean sensitive)
{
  if(darktable.control->accel_initialising) return;

  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  if(!data && d->entries->len > 0 && !_combobox_entry(d, 0)->data)
    data =_combobox_entry(d, d->entries->len - 1)->data + 1;
  dt_bauhaus_combobox_entry_t *entry = _new_combobox_entry(text, align,
                                                           sensitive, data, free_func);
  if(entry)
    g_ptr_array_add(d->entries, entry);
  if(d->active < 0)
    d->active = 0;
  if(d->defpos == -1 && sensitive)
    d->defpos = GPOINTER_TO_INT(data);
}

gboolean dt_bauhaus_combobox_set_entry_label(GtkWidget *widget,
                                             const int pos,
                                             const gchar *label)
{
  // change the text to show for the entry note that this doesn't
  // break shortcuts but their names in the shortcut panel will remain
  // the initial one
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return FALSE;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  if(!d || pos < 0 || pos >= d->entries->len) return FALSE;
  dt_bauhaus_combobox_entry_t *entry = _combobox_entry(d, pos);
  g_free(entry->label);
  entry->label = g_strdup(label);
  return TRUE;
}

void dt_bauhaus_combobox_set_entries_ellipsis(GtkWidget *widget,
                                              PangoEllipsizeMode ellipis)
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

void dt_bauhaus_combobox_set_editable(GtkWidget *widget,
                                      const int editable)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->editable = editable ? 1 : 0;
  if(d->editable && !d->text)
    d->text = calloc(1, DT_BAUHAUS_MAX_TEXT);
}

int dt_bauhaus_combobox_get_editable(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  return d ? d->editable : 0;
}

void dt_bauhaus_combobox_set_selected_text_align
  (GtkWidget *widget,
   const dt_bauhaus_combobox_alignment_t text_align)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->text_align = text_align;
}

void dt_bauhaus_combobox_remove_at(GtkWidget *widget,
                                   const int pos)
{
  dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  if(!d || pos < 0 || pos >= d->entries->len) return;

  // move active position up if removing anything before it or when
  // removing last position that is currently active.  this also sets
  // active to -1 when removing the last remaining entry in a
  // combobox.
  if(d->active > pos || d->active == d->entries->len-1)
    d->active--;

  g_ptr_array_remove_index(d->entries, pos);
}

void dt_bauhaus_combobox_insert(GtkWidget *widget,
                                const char *text,
                                const int pos)
{
  dt_bauhaus_combobox_insert_full(widget, text,
                                  DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, NULL, NULL, pos);
}

void dt_bauhaus_combobox_insert_full(GtkWidget *widget,
                                     const char *text,
                                     const dt_bauhaus_combobox_alignment_t align,
                                     gpointer data, void (*free_func)(void *),
                                     const int pos)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  dt_bauhaus_combobox_entry_t *entry = _new_combobox_entry(text, align, TRUE, data, free_func);
  if(entry)
    g_ptr_array_insert(d->entries, pos, entry);
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
    return d->editable ? d->text : NULL;
  else
    return _combobox_entry(d, d->active)->label;
}

gpointer dt_bauhaus_combobox_get_data(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d || d->active < 0) return NULL;

  return _combobox_entry(d, d->active)->data;
}

void dt_bauhaus_combobox_clear(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->active = -1;
  g_ptr_array_set_size(d->entries, 0);
}

const char *dt_bauhaus_combobox_get_entry(GtkWidget *widget,
                                          const int pos)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d || pos < 0 || pos >= d->entries->len) return NULL;

  return _combobox_entry(d, pos)->label;
}

void dt_bauhaus_combobox_set_text(GtkWidget *widget,
                                  const char *text)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d || !d->editable) return;

  g_strlcpy(d->text, text, DT_BAUHAUS_MAX_TEXT);
  gtk_widget_queue_draw(GTK_WIDGET(widget));
}

static void _combobox_set(dt_bauhaus_widget_t *w,
                          const int pos,
                          const gboolean mute)
{
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  d->active = CLAMP(pos, -1, (int)d->entries->len - 1);
  gtk_widget_queue_draw(GTK_WIDGET(w));

  dt_bauhaus_t *bh = darktable.bauhaus;
  if(bh->current == w)
  {
    bh->change_active = TRUE;
    float old_mouse_y = bh->mouse_y;
    bh->mouse_y = d->active * bh->line_height + w->top_gap
                  + fmodf(old_mouse_y - w->top_gap, bh->line_height);

    _window_position(bh->mouse_y - old_mouse_y);
    gtk_widget_queue_draw(bh->popup.window);
  }

  if(!darktable.gui->reset && !mute)
  {
    if(w->field)
    {
      switch(w->field_type)
      {
        case DT_INTROSPECTION_TYPE_ENUM:;
          if(d->active >= 0)
          {
            int *e = w->field, preve = *e;
            *e = GPOINTER_TO_INT(_combobox_entry(d, d->active)->data);
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
          dt_print(DT_DEBUG_ALWAYS, "[_combobox_set] unsupported combo data type");
      }
    }
    _highlight_changed_notebook_tab(GTK_WIDGET(w),
                                    GINT_TO_POINTER(d->active != d->defpos));
    g_signal_emit_by_name(G_OBJECT(w), "value-changed");
  }
}

void dt_bauhaus_combobox_set(GtkWidget *widget,
                             const int pos)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  _combobox_set(w, pos, FALSE);
}

gboolean dt_bauhaus_combobox_set_from_text(GtkWidget *widget,
                                           const char *text)
{
  if(!text) return FALSE;

  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  for(int i = 0; d && i < d->entries->len; i++)
  {
    if(!g_strcmp0(_combobox_entry(d, i)->label, text))
    {
      dt_bauhaus_combobox_set(widget, i);
      return TRUE;
    }
  }
  return FALSE;
}

int dt_bauhaus_combobox_get_from_value(GtkWidget *widget,
                                       const int value)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  for(int i = 0; d && i < d->entries->len; i++)
  {
    if(GPOINTER_TO_INT(_combobox_entry(d, i)->data) == value)
    {
      return i;
    }
  }

  return -1;
}

gboolean dt_bauhaus_combobox_set_from_value(GtkWidget *widget,
                                            const int value)
{
  const int pos = dt_bauhaus_combobox_get_from_value(widget, value);
  dt_bauhaus_combobox_set(widget, pos);

  if(pos != -1) return TRUE;

  // this might be a legacy option that was hidden; try to re-add from
  // introspection
  dt_introspection_type_enum_tuple_t *values
    = g_hash_table_lookup(darktable.bauhaus->combo_introspection, dt_action_widget(widget));
  if(values
     && dt_bauhaus_combobox_add_introspection(widget, NULL, values, value, value))
  {
    dt_bauhaus_combobox_set(widget, dt_bauhaus_combobox_length(widget) - 1);
    return TRUE;
  }

  return FALSE;
}

int dt_bauhaus_combobox_get(GtkWidget *widget)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);

  return d ? d->active : -1;
}

void dt_bauhaus_combobox_entry_set_sensitive(GtkWidget *widget,
                                             const int pos,
                                             const gboolean sensitive)
{
  const dt_bauhaus_combobox_data_t *d = _combobox_data(widget);
  if(!d || pos < 0 || pos >= d->entries->len) return;

  _combobox_entry(d, pos)->sensitive = sensitive;
}

void dt_bauhaus_slider_clear_stops(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->grad_cnt = 0;
}

void dt_bauhaus_slider_set_stop(GtkWidget *widget,
                                const float stop,
                                const float r,
                                const float g,
                                const float b)
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
    dt_print(DT_DEBUG_ALWAYS,
             "[bauhaus_slider_set_stop] only %d stops allowed",
             DT_BAUHAUS_SLIDER_MAX_STOPS);
  }
}

static void _draw_equilateral_triangle(cairo_t *cr, float radius)
{
  const float sin = 0.866025404 * radius;
  const float cos = 0.5f * radius;
  cairo_move_to(cr, 0.0, radius);
  cairo_line_to(cr, -sin, -cos);
  cairo_line_to(cr, sin, -cos);
  cairo_line_to(cr, 0.0, radius);
}

static void _draw_indicator(dt_bauhaus_widget_t *w,
                            const float pos,
                            cairo_t *cr,
                            const float wd,
                            const GdkRGBA fg_color,
                            const GdkRGBA border_color)
{
  // draw scale indicator (the tiny triangle)
  if(w->type != DT_BAUHAUS_SLIDER) return;

  const float border_width = darktable.bauhaus->border_width;
  const float size = darktable.bauhaus->marker_size;

  cairo_save(cr);
  cairo_translate(cr, _slider_coordinate(pos, wd, w),
                  darktable.bauhaus->line_height + INNER_PADDING
                      + (darktable.bauhaus->baseline_size - border_width) / 2.0f);
  cairo_scale(cr, 1.0f, -1.0f);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  // draw the outer triangle
  _draw_equilateral_triangle(cr, size);
  cairo_set_line_width(cr, border_width);
  set_color(cr, border_color);
  cairo_stroke(cr);

  _draw_equilateral_triangle(cr, size - border_width);
  cairo_clip(cr);

  // draw the inner triangle
  _draw_equilateral_triangle(cr, size - border_width);
  set_color(cr, fg_color);
  cairo_set_line_width(cr, border_width);

  const dt_bauhaus_slider_data_t *d = &w->data.slider;

  if(d->fill_feedback)
    cairo_fill(cr); // Plain indicator (regular sliders)
  else
    cairo_stroke(cr);  // Hollow indicator to see a color through it (gradient sliders)

  cairo_restore(cr);
}

static void _draw_quad(dt_bauhaus_widget_t *w,
                       cairo_t *cr,
                       const int width,
                       const int height)
{
  if(!w->show_quad) return;
  dt_bauhaus_t *bh = darktable.bauhaus;
  const gboolean sensitive = gtk_widget_is_sensitive(GTK_WIDGET(w));

  if(w->quad_paint)
  {
    cairo_save(cr);

    const gboolean hovering = bh->hovered == GTK_WIDGET(w)
      && darktable.control->element == DT_ACTION_ELEMENT_BUTTON;

    set_color(cr, sensitive && (w->quad_paint_flags & CPF_ACTIVE)
                ? hovering ? bh->color_fg_hover : bh->color_fg
                : hovering ? bh->color_fg       : bh->color_fg_insensitive);
    w->quad_paint(cr, width - bh->quad_width,  // x
                      0.0,                     // y
                      bh->quad_width,          // width
                      bh->quad_width,          // height
                      w->quad_paint_flags, w->quad_paint_data);

    cairo_restore(cr);
  }
  else
  {
    // draw active area square:
    cairo_save(cr);
    set_color(cr, sensitive ? bh->color_fg : bh->color_fg_insensitive);
    switch(w->type)
    {
      case DT_BAUHAUS_COMBOBOX:
        cairo_translate(cr, width - bh->quad_width * .5f, height * .5f);
        GdkRGBA *text_color = _default_color_assign();
        GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(w));
        const GtkStateFlags state = gtk_widget_get_state_flags(GTK_WIDGET(w));
        gtk_style_context_get_color(context, state, text_color);
        const float r = bh->quad_width * .2f;
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
        cairo_rectangle(cr, width - bh->quad_width, 0.0, bh->quad_width, bh->quad_width);
        cairo_fill(cr);
        break;
    }
    cairo_restore(cr);
  }
}

static void _draw_baseline(dt_bauhaus_widget_t *w,
                           cairo_t *cr,
                           const float width)
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
                                        d->grad_col[k][0],
                                        d->grad_col[k][1],
                                        d->grad_col[k][2],
                                        0.4f);
    cairo_set_source(cr, gradient);
  }
  else
  {
    // regular baseline
    set_color(cr, darktable.bauhaus->color_bg);
  }

  cairo_fill(cr);

  // get the reference of the slider aka the position of the 0 value
  const float origin =
    fmaxf(fminf((d->factor > 0 ? -d->min - d->offset/d->factor
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
      cairo_arc(cr, slider_width - graduation_height,
                graduation_top, graduation_height, 0, 2 * M_PI);
    else
      cairo_arc(cr, origin, graduation_top, graduation_height, 0, 2 * M_PI);
  }

  cairo_fill(cr);
  cairo_restore(cr);

  if(d->grad_cnt > 0) cairo_pattern_destroy(gradient);
}

static void _popup_reject(void)
{
  dt_bauhaus_widget_t *w = darktable.bauhaus->current;
  if(w->type == DT_BAUHAUS_SLIDER)
    _slider_set_normalized(w, w->data.slider.oldpos);
  _popup_hide();
}

static gchar *_build_label(const dt_bauhaus_widget_t *w)
{
  if(w->show_extended_label && w->section)
    return g_strdup_printf("%s - %s", w->section, w->label);
  else
    return g_strdup(w->label);
}

static gboolean _popup_draw(GtkWidget *widget,
                            cairo_t *cr,
                            gpointer user_data)
{
  dt_bauhaus_t *bh = darktable.bauhaus;
  dt_bauhaus_popup_t *pop = &bh->popup;
  dt_bauhaus_widget_t *w = bh->current;

  // dimensions of the popup
  const int width = gtk_widget_get_allocated_width(widget);
  const GtkBorder *padding = &pop->padding;
  const int w2 = width - padding->left - padding->right;
  const int h2 = pop->position.height - padding->top - padding->bottom;

  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  // look up some colors once
  GdkRGBA text_color, text_color_selected, text_color_hover, text_color_insensitive;
  gtk_style_context_get_color(context, GTK_STATE_FLAG_NORMAL, &text_color);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_SELECTED, &text_color_selected);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_PRELIGHT, &text_color_hover);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_INSENSITIVE, &text_color_insensitive);

  GdkRGBA *fg_color = _default_color_assign();
  GdkRGBA *bg_color;
  GtkStateFlags state = gtk_widget_get_state_flags(widget);

  gtk_style_context_get(context, state, "background-color", &bg_color, NULL);
  gtk_style_context_get_color(context, state, fg_color);

  // draw background
  gtk_render_background(context, cr, 0, - pop->offcut, width, pop->position.height);
  gtk_render_frame(context, cr, 0, - pop->offcut, width, pop->position.height);

  // translate to account for the widget spacing
  cairo_translate(cr, padding->left, padding->top - pop->offcut);

  gboolean none_found = TRUE;

  // switch on bauhaus widget type (so we only need one static window)
  switch(w->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      const dt_bauhaus_slider_data_t *d = &w->data.slider;

      _draw_baseline(w, cr, w2);

      cairo_save(cr);
      cairo_set_line_width(cr, 0.5);
      float scale = 5.0 * powf(10.0f, -d->digits)/(d->max - d->min) / d->factor;
      const int num_scales = 1.f / scale;
      const int ht = bh->line_height + INNER_PADDING * 2.0f;

      cairo_rectangle(cr, 0, ht, w2, h2 - ht);
      cairo_clip(cr);

      for(int k = 0; k < num_scales; k++)
      {
        const float off = k * scale - d->oldpos;
        GdkRGBA fg_copy = *fg_color;
        fg_copy.alpha = scale / fabsf(off);
        set_color(cr, fg_copy);
        _slider_draw_line(cr, d->oldpos, off, scale, w2, h2, ht, w);
        cairo_stroke(cr);
      }
      cairo_restore(cr);
      set_color(cr, *fg_color);

      // draw mouse over indicator line
      cairo_save(cr);
      cairo_set_line_width(cr, 2.);
      const float mouse_off
          = bh->change_active
                ? _slider_get_line_offset(d->oldpos, scale,
                                          bh->mouse_x / w2,
                                          bh->mouse_y / h2,
                                          ht / (float)h2, width, w)
                : 0.0f;
      _slider_draw_line(cr, d->oldpos, mouse_off, scale, w2, h2, ht, w);
      cairo_stroke(cr);
      cairo_restore(cr);

      // draw indicator
      _draw_indicator(w, d->oldpos + mouse_off, cr, w2, *fg_color, *bg_color);

      // draw numerical value:
      cairo_save(cr);

      char *text = dt_bauhaus_slider_get_text(GTK_WIDGET(w),
                                              dt_bauhaus_slider_get(GTK_WIDGET(w)));
      set_color(cr, *fg_color);
      const float value_width = _show_pango_text(w, context, cr, text, w2 -
                                                 _widget_get_quad_width(w), 0, 0,
                                                 TRUE, FALSE,
                                                 PANGO_ELLIPSIZE_END,
                                                 FALSE, FALSE, NULL, NULL);
      g_free(text);
      set_color(cr, text_color_insensitive);
      char *min = dt_bauhaus_slider_get_text(GTK_WIDGET(w),
                                             d->factor > 0 ? d->min : d->max);
      _show_pango_text(w, context, cr, min, 0, ht + INNER_PADDING, 0,
                       FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE, FALSE, NULL, NULL);
      g_free(min);
      char *max = dt_bauhaus_slider_get_text(GTK_WIDGET(w),
                                             d->factor > 0 ? d->max : d->min);
      _show_pango_text(w, context, cr, max, w2 -
                       _widget_get_quad_width(w), ht + INNER_PADDING, 0,
                       TRUE, FALSE, PANGO_ELLIPSIZE_END, FALSE, FALSE, NULL, NULL);
      g_free(max);

      const float label_width =
        w2 - _widget_get_quad_width(w) - INNER_PADDING - value_width;
      if(label_width > 0)
      {
        gchar *lb = _build_label(w);
        _show_pango_text(w, context, cr, lb, 0, 0, label_width,
                         FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE, FALSE, NULL, NULL);
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
      const int ht = bh->line_height;
      const int hovered = (bh->mouse_y - w->top_gap) / ht;
      gchar *keys = g_utf8_casefold(bh->keys, bh->keys_cnt);
      const PangoEllipsizeMode ellipsis = d->entries_ellipsis;

      bh->unique_match = -1;
      for(int j = 0; j < d->entries->len; j++)
      {
        const dt_bauhaus_combobox_entry_t *entry = _combobox_entry(d, j);
        gchar *text_cmp = g_utf8_casefold(entry->label, -1);
        gchar *search_found = strstr(text_cmp, keys);
        gchar *entry_found = entry->label + (search_found - text_cmp);

        gchar *label = NULL;
        float max_width = w2 - _widget_get_quad_width(w);
        float label_width = 0.0f;
        if(!entry->sensitive)
        {
          set_color(cr, text_color_insensitive);
          label = g_markup_printf_escaped("<b>%s</b>", entry->label);
        }
        else
        {
          if(j == hovered)
            set_color(cr, text_color_hover);
          else if(j == d->active)
            set_color(cr, text_color_selected);
          else
            set_color(cr, text_color);

          if(!search_found)
            label = g_markup_printf_escaped("<span alpha=\"50%%\">%s</span>",
                                            entry->label);
          else
          {
            if(!d->editable)
              bh->unique_match = none_found ? j : -1;
            else if(!strcmp(text_cmp, keys))
              bh->unique_match = j;
            none_found = FALSE;

            gchar *start = g_strndup(entry->label, entry_found - entry->label);
            gchar *match = g_strndup(entry_found, bh->keys_cnt);
            label = g_markup_printf_escaped("%s<b>%s</b>%s",
                                            start, match,
                                            entry_found + bh->keys_cnt);
            g_free(start);
            g_free(match);
          }
        }

        if(entry->alignment == DT_BAUHAUS_COMBOBOX_ALIGN_LEFT)
        {
          label_width = _show_pango_text(w, context, cr,
                                         label, 0, ht * j + w->top_gap, max_width,
                                         FALSE, FALSE, ellipsis, TRUE, FALSE, NULL, NULL);
        }
        else if(entry->alignment == DT_BAUHAUS_COMBOBOX_ALIGN_MIDDLE)
        {
          // first pass, we just get the text width
          label_width = _show_pango_text(w, context, cr,
                                         label, 0, ht * j + w->top_gap, max_width,
                                         FALSE, TRUE, ellipsis, TRUE, FALSE, NULL, NULL);
          // second pass, we draw it in the middle
          const int posx = MAX(0, (max_width - label_width) / 2);
          label_width = _show_pango_text(w, context, cr,
                                         label, posx, ht * j + w->top_gap, max_width,
                                         FALSE, FALSE, ellipsis, TRUE, FALSE, NULL, NULL);
        }
        else
        {
          if(first_label)
            max_width *= 0.8; // give the label at least some room
          label_width = _show_pango_text(w, context, cr,
                                         label, w2 - _widget_get_quad_width(w),
                                         ht * j + w->top_gap, max_width,
                                         TRUE, FALSE, ellipsis, TRUE, FALSE, NULL, NULL);
        }
        g_free(label);
        g_free(text_cmp);

        // prefer the entry over the label wrt. ellipsization when expanded
        if(first_label)
        {
          show_box_label = entry->alignment == DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT;
          first_label_width = label_width;
          first_label = FALSE;
        }
      }
      cairo_restore(cr);

      // left aligned box label. add it to the gui after the entries
      // so we can ellipsize it if needed
      if(show_box_label)
      {
        set_color(cr, text_color);
        gchar *lb = _build_label(w);
        _show_pango_text(w, context, cr,
                         lb, 0, w->top_gap,
                         w2 - _widget_get_quad_width(w) - first_label_width,
                         FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE, TRUE, NULL, NULL);
        g_free(lb);
      }
      g_free(keys);

      if(none_found && !d->editable && bh->keys_cnt > 0)
      {
        bh->keys_cnt =
          g_utf8_prev_char(bh->keys + bh->keys_cnt)
          - bh->keys;
        none_found = FALSE;
        gtk_widget_queue_draw(widget);
      }

    }
    break;
    default:
      // yell
      break;
  }

  // draw currently typed text. if a type doesn't want this, it should not
  // allow stuff to be written here in the key callback.
  const int size = MIN(3 * bh->line_height, .2 * h2);
  if(none_found && bh->keys_cnt)
  {
    cairo_save(cr);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoRectangle ink;
    pango_cairo_context_set_resolution(pango_layout_get_context(layout),
                                       darktable.gui->dpi);
    set_color(cr, text_color);

    // make extra large, but without dependency on popup window height
    // (that might differ for comboboxes for example). only fall back
    // to height dependency if the popup is really small.
    PangoFontDescription *desc =
      pango_font_description_copy_static(bh->pango_font_desc);
    pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    pango_layout_set_text(layout, bh->keys, bh->keys_cnt);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, w2 - _widget_get_quad_width(w) - ink.width, h2 * 0.5 - size);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
  if(bh->cursor_visible)
  {
    // show the blinking cursor
    cairo_save(cr);
    set_color(cr, text_color);
    cairo_move_to(cr, w2 - bh->quad_width + 3, h2 * 0.5 + size / 3);
    cairo_line_to(cr, w2 - bh->quad_width + 3, h2 * 0.5 - size);
    cairo_set_line_width(cr, 2.);
    cairo_stroke(cr);
    cairo_restore(cr);
  }

  gdk_rgba_free(bg_color);
  gdk_rgba_free(fg_color);

  return TRUE;
}

static gboolean _widget_draw(GtkWidget *widget,
                             cairo_t *crf)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  GdkRGBA *fg_color = _default_color_assign();
  GdkRGBA *bg_color;
  GdkRGBA *text_color = _default_color_assign();
  const GtkStateFlags hovering =
    widget == darktable.bauhaus->hovered ? GTK_STATE_FLAG_PRELIGHT : 0;
  const GtkStateFlags state = gtk_widget_get_state_flags(widget) | hovering;
  gtk_style_context_get_color(context, state, text_color);

  gtk_style_context_get_color(context, state, fg_color);
  gtk_style_context_get(context, state, "background-color", &bg_color, NULL);

  // translate to account for the widget spacing
  const int h2 = height - w->margin.top - w->margin.bottom;
  const int w2 = width - w->margin.left - w->margin.right;
  const int h3 = h2 - w->padding.top - w->padding.bottom;
  const int w3 = w2 - w->padding.left - w->padding.right;
  gtk_render_background(context, cr, w->margin.left, w->margin.top, w2, h2);
  cairo_translate(cr, w->margin.left + w->padding.left, w->margin.top + w->padding.top);

  // draw type specific content:
  cairo_save(cr);
  cairo_set_line_width(cr, 1.0);
  if(w->show_quad) _draw_quad(w, cr, w3, h3);
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
    {
      // draw label and quad area at right end
      set_color(cr, *text_color);

      dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      const PangoEllipsizeMode combo_ellipsis = d->entries_ellipsis;
      const gchar *text = d->text;
      if(d->active >= 0 && d->active < d->entries->len)
      {
        text = _combobox_entry(d, d->active)->label;
      }
      set_color(cr, *text_color);

      const float available_width = w3 - _widget_get_quad_width(w);

      //calculate total widths of label and combobox
      gchar *label_text = _build_label(w);
      float label_width = 0;
      float label_height = 0;
      // we only show the label if the text is aligned on the right
      if(label_text && w->show_label && d->text_align == DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT)
        _show_pango_text(w, context, cr, label_text, 0, 0, 0,
                         FALSE, TRUE, PANGO_ELLIPSIZE_END,
                         FALSE, TRUE, &label_width, &label_height);
      float combo_width = 0;
      float combo_height = 0;
      _show_pango_text(w, context, cr, text, available_width, 0, 0,
                       TRUE, TRUE, combo_ellipsis, FALSE, FALSE, &combo_width, &combo_height);
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
            _show_pango_text(w, context, cr,
                             label_text, 0,
                             w->top_gap, available_width * ratio - INNER_PADDING * 2,
                             FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE, TRUE, NULL, NULL);
          _show_pango_text(w, context, cr,
                           text, available_width, w->top_gap,
                           available_width * (1.0f - ratio),
                           TRUE, FALSE, combo_ellipsis, FALSE, FALSE, NULL, NULL);
        }
        else if(d->text_align == DT_BAUHAUS_COMBOBOX_ALIGN_MIDDLE)
        {
          const int posx = MAX(0, (available_width - combo_width) / 2);
          _show_pango_text(w, context, cr, text, posx, w->top_gap, available_width,
                           FALSE, FALSE, combo_ellipsis, FALSE, FALSE, NULL, NULL);
        }
        else
          _show_pango_text(w, context, cr, text, 0, w->top_gap, available_width,
                           FALSE, FALSE, combo_ellipsis, FALSE, FALSE, NULL, NULL);
      }
      else
      {
        if(d->text_align == DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT)
        {
          if(w->show_label)
            _show_pango_text(w, context, cr, label_text, 0, w->top_gap, 0,
                             FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE, TRUE, NULL, NULL);
          _show_pango_text(w, context, cr, text, available_width, w->top_gap, 0,
                           TRUE, FALSE, combo_ellipsis, FALSE, FALSE, NULL, NULL);
        }
        else if(d->text_align == DT_BAUHAUS_COMBOBOX_ALIGN_MIDDLE)
        {
          const int posx = MAX(0, (available_width - combo_width) / 2);
          _show_pango_text(w, context, cr, text, posx, w->top_gap, 0,
                           FALSE, FALSE, combo_ellipsis, FALSE, FALSE, NULL, NULL);
        }
        else
          _show_pango_text(w, context, cr, text, 0, w->top_gap, 0,
                           FALSE, FALSE, combo_ellipsis, FALSE, FALSE, NULL, NULL);
      }
      g_free(label_text);
      break;
    }
    case DT_BAUHAUS_SLIDER:
    {
      // line for orientation
      _draw_baseline(w, cr, w3);

      float value_width = 0;
      if(gtk_widget_is_sensitive(widget))
      {
        cairo_save(cr);
        cairo_rectangle(cr, 0, 0, w3 - _widget_get_quad_width(w), h3 + INNER_PADDING);
        cairo_clip(cr);
        _draw_indicator(w, w->data.slider.pos, cr, w3, *fg_color, *bg_color);
        cairo_restore(cr);

        // TODO: merge that text with combo

        char *text = dt_bauhaus_slider_get_text(widget, dt_bauhaus_slider_get(widget));
        set_color(cr, *text_color);
        value_width = _show_pango_text(w, context, cr,
                                       text, w3 - _widget_get_quad_width(w), 0, 0,
                                       TRUE, FALSE, PANGO_ELLIPSIZE_END,
                                       FALSE, FALSE, NULL, NULL);
        g_free(text);
      }
      // label on top of marker:
      gchar *label_text = _build_label(w);
      set_color(cr, *text_color);
      const float label_width = w3 - _widget_get_quad_width(w) - value_width;
      if(label_width > 0)
        _show_pango_text(w, context, cr, label_text, 0, 0, label_width,
                         FALSE, FALSE, PANGO_ELLIPSIZE_END, FALSE, TRUE, NULL, NULL);
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
  gtk_render_frame(context, crf, w->margin.left, w->margin.top, w2, h2);

  gdk_rgba_free(text_color);
  gdk_rgba_free(fg_color);
  gdk_rgba_free(bg_color);

  return TRUE;
}

static gint _natural_width(GtkWidget *widget,
                           const gboolean popup)
{
  gint natural_size = 0;

  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);

  PangoLayout *layout = gtk_widget_create_pango_layout(widget, NULL);
  PangoFontDescription *font_desc = 0;
  gtk_style_context_get(gtk_widget_get_style_context(widget),
                        gtk_widget_get_state_flags(GTK_WIDGET(w)),
                        "font", &font_desc, NULL);
  pango_layout_set_font_description(layout, font_desc);
  PangoAttrList *attrlist = pango_attr_list_new();
  pango_attr_list_insert(attrlist, pango_attr_font_features_new("tnum"));
  pango_attr_list_insert(attrlist, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
  pango_layout_set_attributes(layout, attrlist);
  pango_attr_list_unref(attrlist);

  if(w->show_label || popup)
  {
    pango_layout_set_text(layout, w->label, -1);
    pango_layout_get_size(layout, &natural_size, NULL);
    natural_size /= PANGO_SCALE;
  }

  if(w->type == DT_BAUHAUS_COMBOBOX)
  {
    dt_bauhaus_combobox_data_t *d = &w->data.combobox;

    gint label_width = 0, entry_width = 0;

    if(natural_size
       && d->text_align == DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT
       && (popup || w->show_label))
      label_width = natural_size + 2 * INNER_PADDING;

    for(int i = 0; i < d->entries->len; i++)
    {
      const dt_bauhaus_combobox_entry_t *entry = _combobox_entry(d, i);

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
    char *max = dt_bauhaus_slider_get_text(widget, w->data.slider.max);
    char *min = dt_bauhaus_slider_get_text(widget, w->data.slider.min);
    char *text = strlen(max) >= strlen(min) ? max : min;
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_size(layout, &number_width, NULL);
    natural_size += 2 * INNER_PADDING + number_width / PANGO_SCALE;
    g_free(max);
    g_free(min);
  }

  natural_size += (w->show_quad ? _widget_get_quad_width(w) : 0);
  g_object_unref(layout);

  return natural_size;
}

static void _widget_get_preferred_width(GtkWidget *widget,
                                        gint *minimum_width,
                                        gint *natural_width)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  _margins_retrieve(w);

  *natural_width = _natural_width(widget, FALSE)
                   + w->margin.left + w->margin.right + w->padding.left + w->padding.right;
}

static void _widget_get_preferred_height(GtkWidget *widget,
                                         gint *minimum_height,
                                         gint *natural_height)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  _margins_retrieve(w);

  *minimum_height = w->margin.top + w->margin.bottom + w->padding.top + w->padding.bottom
                    + darktable.bauhaus->line_height;
  if(w->type == DT_BAUHAUS_SLIDER)
  {
    // the lower thing to draw is indicator. See _draw_baseline for compute details
    *minimum_height += INNER_PADDING
      + darktable.bauhaus->baseline_size + 1.5f * darktable.bauhaus->border_width;
  }

  *natural_height = *minimum_height;
}

static void _popup_hide()
{
  dt_bauhaus_t *bh = darktable.bauhaus;
  dt_bauhaus_popup_t *pop = &bh->popup;
  dt_bauhaus_widget_t *w = bh->current;

  if(w)
  {
    if(w->type == DT_BAUHAUS_COMBOBOX
       && w->data.combobox.mute_scrolling
       && bh->change_active)
      g_signal_emit_by_name(G_OBJECT(w), "value-changed");

    gtk_grab_remove(pop->area);
    gtk_widget_hide(pop->window);
    gtk_window_set_attached_to(GTK_WINDOW(pop->window), NULL);
    g_signal_handlers_disconnect_by_func(pop->window, G_CALLBACK(dt_shortcut_dispatcher), NULL);
    bh->current = NULL;
  }
  _stop_cursor();
}

static void _popup_show(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_t *bh = darktable.bauhaus;
  dt_bauhaus_popup_t *pop = &bh->popup;

  if(bh->current) _popup_hide();
  bh->current = w;
  bh->keys_cnt = 0;
  bh->change_active = FALSE;
  bh->mouse_line_distance = 0.0f;
  _stop_cursor();

  _request_focus(w);

  // we update the popup padding defined in css
  GtkStyleContext *context = gtk_widget_get_style_context(pop->area);
  gtk_style_context_add_class(context, "dt_bauhaus_popup");
  // let's update the css class depending on the source widget type
  // this allow to set different padding for example
  if(w->show_quad)
    gtk_style_context_remove_class(context, "dt_bauhaus_popup_right");
  else
    gtk_style_context_add_class(context, "dt_bauhaus_popup_right");

  const GtkStateFlags state = gtk_widget_get_state_flags(pop->area);
  gtk_style_context_get_padding(context, state, &pop->padding);

  GdkRectangle *p = &pop->position;
  gtk_widget_get_allocation(widget, p);
  const int ht = p->height;
  gint px, py;
  // gtk_widget_get_toplevel doesn't work for popovers on wayland
  GdkWindow *main_window = gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui));
  GdkWindow *top = main_window;
  GdkWindow *widget_window = gtk_widget_get_window(widget);
  if(widget_window)
  {
    top = gdk_window_get_toplevel(widget_window);
    gdk_window_get_origin(top, &px, &py);
    gdk_window_get_origin(widget_window, &p->x, &p->y);
    p->x -= px;
    p->y -= py;
  }

  const int right_of_w = p->x + p->width - w->margin.right - w->padding.right;
  if(p->width == 1)
  {
    if(dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_RIGHT, widget))
      p->width = dt_ui_panel_get_size(darktable.gui->ui, DT_UI_PANEL_RIGHT);
    else if(dt_ui_panel_ancestor(darktable.gui->ui, DT_UI_PANEL_LEFT, widget))
      p->width = dt_ui_panel_get_size(darktable.gui->ui, DT_UI_PANEL_LEFT);
    else
      p->width = 300;
    p->width -= INNER_PADDING * 2;
  }
  else
  {
  // by default, we want the popup to be exactly the size of the widget content
    p->width =
      MAX(1,
          p->width - (w->margin.left + w->margin.right +
                      w->padding.left + w->padding.right));
  }

  const gint natural_w = _natural_width(widget, TRUE);
  if(p->width < natural_w)
    p->width = natural_w;

  GdkDevice *pointer =
    gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
  gdk_window_get_device_position(top, pointer, &px, &py, NULL);

  if(px > p->x + p->width  || px < p->x)
  {
    p->x = px - (p->width - _widget_get_quad_width(w)) / 2;
    p->y = py - bh->line_height / 2;
  }
  else
  {
    p->x = right_of_w - p->width;
    if(py < p->y || py > p->y + p->height)
    {
      p->y = py - bh->line_height / 2;
    }
  }

  switch(bh->current->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      d->oldpos = d->pos;
      p->height = p->width;
      _start_cursor(6);
      pop->offset = 0;
      bh->mouse_y = bh->line_height + ht / 2;
      break;
    }
    case DT_BAUHAUS_COMBOBOX:
    {
      // we launch the dynamic populate fct if any
      dt_iop_module_t *module = (dt_iop_module_t *)(w->module);
      const dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      if(d->populate) d->populate(widget, &module);

      if(!d->entries->len) return;
      p->height = bh->line_height * d->entries->len
                  + w->margin.top + w->margin.bottom + w->top_gap;

      pop->offset = d->active * bh->line_height;
      bh->mouse_x = 0;
      bh->mouse_y = d->active * bh->line_height + ht / 2;
      break;
    }
    default:
      break;
  }

  // by default, we want the popup to be exactly at the position of the widget content
  p->x += w->margin.left + w->padding.left;
  p->y += w->margin.top + w->padding.top;

  // and now we extent the popup to take account of its own padding
  p->x -= pop->padding.left;
  p->y -= pop->padding.top;
  p->width += pop->padding.left + pop->padding.right;
  p->height += pop->padding.top + pop->padding.bottom;
  pop->offcut = 0;

  gtk_tooltip_trigger_tooltip_query(gdk_display_get_default());
  if(top == main_window)
    g_signal_connect(pop->window, "event", G_CALLBACK(dt_shortcut_dispatcher), NULL);

  gtk_window_set_attached_to(GTK_WINDOW(pop->window), widget);
  gdk_window_set_transient_for(gtk_widget_get_window(pop->window), top);
  _window_position(0);
  gtk_widget_show_all(pop->window);
  gtk_widget_grab_focus(pop->area);
}

static void _slider_add_step(GtkWidget *widget,
                             float delta,
                             const guint state,
                             const gboolean force)
{
  if(delta == 0) return;

  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  const float value = dt_bauhaus_slider_get(widget);

  if(d->curve == _curve_log10)
    delta = value
      * (powf(0.97f, - delta * dt_accel_get_speed_multiplier(widget, state)) - 1.0f);
  else
    delta *= dt_bauhaus_slider_get_step(widget)
      * dt_accel_get_speed_multiplier(widget, state);

  const float min_visible = powf(10.0f, -d->digits) / fabsf(d->factor);
  if(delta && fabsf(delta) < min_visible)
    delta = copysignf(min_visible, delta);

  if(force || dt_modifier_is(state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
  {
    if(d->factor > 0 ? d->pos < 0.0001 : d->pos > 0.9999)
      d->min = d->min > d->soft_min ? d->max : d->soft_min;
    if(d->factor < 0 ? d->pos < 0.0001 : d->pos > 0.9999)
      d->max = d->max < d->soft_max ? d->min : d->soft_max;
    dt_bauhaus_slider_set(widget, value + delta);
  }
  else if(!strcmp(d->format,"°")
          && fabsf((d->max - d->min) * d->factor - 360.0f) < 1e-4
          && fabsf(value + delta)/(d->max - d->min) < 2)
    dt_bauhaus_slider_set(widget, value + delta);
  else
    dt_bauhaus_slider_set(widget, CLAMP(value + delta, d->min, d->max));
}

static gboolean _widget_scroll(GtkWidget *widget,
                               GdkEventScroll *event)
{
  if(dt_gui_ignore_scroll(event)) return FALSE;

  // handle speed adjustment in mapping mode in dispatcher
  if(darktable.control->mapping_widget)
    return dt_shortcut_dispatcher(widget, (GdkEvent*)event, NULL);

  gtk_widget_grab_focus(widget);

  int delta_y = 0;
  if(dt_gui_get_scroll_unit_delta(event, &delta_y))
  {
    if(delta_y == 0) return TRUE;

    dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
    _request_focus(w);

    if(w->type == DT_BAUHAUS_SLIDER)
    {
      gboolean force = darktable.control->element == DT_ACTION_ELEMENT_FORCE
                       && event->window == gtk_widget_get_window(widget);
      if(force && dt_modifier_is(event->state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
      {
        _slider_zoom_range(w, delta_y);
        _slider_zoom_toast(w);
      }
      else
        _slider_add_step(widget, - delta_y, event->state, force);
    }
    else
      _combobox_next_sensitive(w, delta_y, 0, FALSE);
  }
  return TRUE; // Ensure that scrolling the combobox cannot move side panel
}

static gboolean _widget_key_press(GtkWidget *widget, GdkEventKey *event)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;

  int delta = -1;
  switch(event->keyval)
  {
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
      if(w->type == DT_BAUHAUS_COMBOBOX) delta *= -1;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      delta *= -1;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
      if(w->type == DT_BAUHAUS_COMBOBOX) delta *= -1;
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
      _request_focus(w);

      if(w->type == DT_BAUHAUS_SLIDER)
        _slider_add_step(widget, delta, event->state, FALSE);
      else
        _combobox_next_sensitive(w, delta, 0, FALSE);

      return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      _popup_show(widget);
      return TRUE;
    default:
      return FALSE;
  }
}

float dt_bauhaus_slider_get(GtkWidget *widget)
{
  // first cast to bh widget, to check that type:
  const dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
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

char *dt_bauhaus_slider_get_text(GtkWidget *w,
                                 const float val)
{
  const dt_bauhaus_slider_data_t *d = &DT_BAUHAUS_WIDGET(w)->data.slider;
  if((d->hard_max * d->factor + d->offset)*(d->hard_min * d->factor + d->offset) < 0)
    return g_strdup_printf("%+.*f%s", d->digits, val * d->factor + d->offset, d->format);
  else
    return g_strdup_printf( "%.*f%s", d->digits, val * d->factor + d->offset, d->format);
}

void dt_bauhaus_slider_set(GtkWidget *widget,
                           const float pos)
{
  if(dt_isnan(pos)) return;

  // this is the public interface function, translate by bounds and call set_normalized
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER)
    return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;
  const float rpos = CLAMP(pos, d->hard_min, d->hard_max);
  // if this is an angle or gradient, wrap around
  // don't wrap yet if exactly at min or max
  const float gpos = rpos == pos || strcmp(d->format,"°") ? rpos :
                     d->hard_min + fmodf(pos + d->hard_max - 2*d->hard_min,
                                         d->hard_max - d->hard_min);
  // set new temp range to include new value
  // if angle has wrapped around, then set to full range
  d->min = (gpos == rpos) ? MIN(d->min, rpos) : d->hard_min;
  d->max = (gpos == rpos) ? MAX(d->max, rpos) : d->hard_max;;
  const float rawval = (gpos - d->min) / (d->max - d->min);
  _slider_set_normalized(w, d->curve(rawval, DT_BAUHAUS_SET));
}

void dt_bauhaus_slider_set_val(GtkWidget *widget,
                               const float val)
{
  const dt_bauhaus_slider_data_t *d = &DT_BAUHAUS_WIDGET(widget)->data.slider;
  dt_bauhaus_slider_set(widget, (val - d->offset) / d->factor);
}

void dt_bauhaus_slider_set_digits(GtkWidget *widget,
                                  const int val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->digits = val;
}

int dt_bauhaus_slider_get_digits(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return 0;

  const dt_bauhaus_slider_data_t *d = &w->data.slider;

  return d->digits;
}

void dt_bauhaus_slider_set_step(GtkWidget *widget,
                                const float val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->step = val;
}

float dt_bauhaus_slider_get_step(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);

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

void dt_bauhaus_slider_set_feedback(GtkWidget *widget,
                                    const int feedback)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->fill_feedback = feedback;

  gtk_widget_queue_draw(widget);
}

int dt_bauhaus_slider_get_feedback(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return 0;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  return d->fill_feedback;
}

void dt_bauhaus_widget_reset(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);

  if(w->type == DT_BAUHAUS_SLIDER)
  {
    dt_bauhaus_slider_data_t *d = &w->data.slider;
    d->is_dragging = 0;

    d->min = d->soft_min;
    d->max = d->soft_max;

    dt_bauhaus_slider_set(widget, d->defpos);
  }
  else
    dt_bauhaus_combobox_set_from_value(widget, w->data.combobox.defpos);

  return;
}

void dt_bauhaus_slider_set_format(GtkWidget *widget,
                                  const char *format)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->format = g_intern_string(format);

  if(strstr(format,"%") && fabsf(d->hard_max) <= 10)
  {
    if(d->factor == 1.0f) d->factor = 100;
    d->digits -= 2;
  }
}

void dt_bauhaus_slider_set_factor(GtkWidget *widget,
                                  const float factor)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->factor = factor;
  if(factor < 0) d->curve = _reverse_linear_curve;
}

void dt_bauhaus_slider_set_offset(GtkWidget *widget,
                                  const float offset)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->offset = offset;
}

void dt_bauhaus_slider_set_curve(GtkWidget *widget,
                                 float (*curve)(const float value,
                                                const dt_bauhaus_curve_t dir))
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  if(curve == NULL) curve = _default_linear_curve;

  d->pos = curve(d->curve(d->pos, DT_BAUHAUS_GET), DT_BAUHAUS_SET);

  d->curve = curve;
}

void dt_bauhaus_slider_set_log_curve(GtkWidget *widget)
{
  dt_bauhaus_slider_set_curve(widget, _curve_log10);
}

static gboolean _slider_value_change_dragging(gpointer data);

static void _slider_value_change(dt_bauhaus_widget_t *w)
{
  if(!GTK_IS_WIDGET(w)) return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;
  if(d->is_changed && !d->timeout_handle)
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
          dt_print(DT_DEBUG_ALWAYS,
                   "[_slider_value_change] unsupported slider data type");
      }
    }

    _highlight_changed_notebook_tab(GTK_WIDGET(w), NULL);
    g_signal_emit_by_name(G_OBJECT(w), "value-changed");
    d->is_changed = 0;

    if(d->is_dragging)
      d->timeout_handle = g_idle_add(_slider_value_change_dragging, w);
  }
}

static gboolean _slider_value_change_dragging(gpointer data)
{
  dt_bauhaus_widget_t *w = data;
  w->data.slider.timeout_handle = 0;
  _slider_value_change(w);
  return G_SOURCE_REMOVE;
}

static void _slider_set_normalized(dt_bauhaus_widget_t *w, float pos)
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
  if(darktable.bauhaus->current == w)
    gtk_widget_queue_draw(darktable.bauhaus->popup.area);
  if(!darktable.gui->reset)
  {
    d->is_changed = -1;
    _slider_value_change(w);
  }
}

static gboolean _popup_key_press(GtkWidget *widget,
                                 GdkEventKey *event,
                                 gpointer user_data)
{
  dt_bauhaus_t *bh = darktable.bauhaus;
  dt_bauhaus_widget_t *w = bh->current;
  gboolean is_combo = w->type == DT_BAUHAUS_COMBOBOX;
  int delta = -1;

  switch(event->keyval)
  {
    case GDK_KEY_BackSpace:
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
      if(bh->keys_cnt > 0)
        bh->keys_cnt = g_utf8_prev_char(bh->keys + bh->keys_cnt) - bh->keys;
      break;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      if(bh->keys_cnt > 0 && bh->keys_cnt + 1 < DT_BAUHAUS_MAX_TEXT)
      {
        bh->keys[bh->keys_cnt] = 0;
        if(is_combo)
        {
          if(!bh->change_active)
          {
            dt_bauhaus_combobox_data_t *d = &w->data.combobox;
            if(bh->unique_match == -1)
            {
              if(w->data.combobox.editable)
                g_strlcpy(d->text, bh->keys, DT_BAUHAUS_MAX_TEXT);
              else
                break; // don't hide popup
            }
            _combobox_set(w, bh->unique_match, FALSE);
          }
        }
        else
        {
          // unnormalized input, user was typing this:
          const float old_value = dt_bauhaus_slider_get_val(GTK_WIDGET(w));
          const float new_value = dt_calculator_solve(old_value, bh->keys);
          if(dt_isfinite(new_value))
            dt_bauhaus_slider_set_val(GTK_WIDGET(w), new_value);
        }
      }
      _popup_hide();
      break;
    case GDK_KEY_Escape:
      _popup_reject();
      break;
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
      delta *= -1;
    case GDK_KEY_End:
    case GDK_KEY_KP_End:
      delta *= 1e6;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
      if(is_combo) delta *= -1;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up:
      delta *= -1;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down:
      if(is_combo) delta *= -1;
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
      if(is_combo)
        _combobox_next_sensitive(w, delta, 0, w->data.combobox.mute_scrolling);
      else
        _slider_add_step(GTK_WIDGET(w), delta, event->state, FALSE);
      break;
    default:
      if(!event->string || !g_utf8_validate(event->string, -1, NULL)) return FALSE;
      const gunichar c = g_utf8_get_char(event->string);
      if(!g_unichar_isprint(c)) return FALSE;
      const long int char_width = g_utf8_next_char(event->string) - event->string;
      if(bh->keys_cnt + 1 + char_width < DT_BAUHAUS_MAX_TEXT
         && (is_combo || strchr("0123456789.,%%+-*Xx/:^~ ()", event->string[0])))
      {
        // only accept valid keys for slider; combo is checked in _popup_draw
        strncpy(bh->keys + bh->keys_cnt, event->string, char_width);
        bh->keys_cnt += char_width;

        if(!is_combo) _start_cursor(-1);
      }
  }
  gtk_widget_queue_draw(bh->popup.area);
  return TRUE;
}

static gboolean _widget_button_press(GtkWidget *widget,
                                     GdkEventButton *event)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  _request_focus(w);
  gtk_widget_grab_focus(widget);

  const int width = gtk_widget_get_allocated_width(widget);
  const int w3 = width - w->margin.left - w->padding.left
                       - w->margin.right - w->padding.right;
  const double ex = event->x - w->margin.left - w->padding.left;
  const double ey = event->y - w->margin.top - w->padding.top;

  if(w->quad_paint && event->window == gtk_widget_get_window(widget)
     && event->x > width - _widget_get_quad_width(w)
                         - w->margin.right - w->padding.right)
  {
    dt_bauhaus_widget_press_quad(widget);
  }
  else if(event->button == 1
          && event->type == GDK_2BUTTON_PRESS)
  {
    if(!(dt_modifier_is(event->state, GDK_CONTROL_MASK) && w->field
         && dt_gui_presets_autoapply_for_module((dt_iop_module_t *)w->module, widget)))
      dt_bauhaus_widget_reset(widget); // reset to default.
    // never called for combo, as we popup the other window under your cursor before.
    // (except in weird corner cases where the popup is under the -1st entry
    _popup_hide();
  }
  else if(event->button == 3 || w->type == DT_BAUHAUS_COMBOBOX)
  {
    darktable.bauhaus->opentime = event->time;
    darktable.bauhaus->mouse_x = event->x;
    darktable.bauhaus->mouse_y = event->y;
    _popup_show(widget);
  }
  else if(event->button == 2)
  {
    _slider_zoom_range(w, 0); // reset zoom range to soft min/max
    _slider_zoom_toast(w);
  }
  else
  {
    dt_bauhaus_slider_data_t *d = &w->data.slider;
    d->is_dragging = -1;
    if(!dt_modifier_is(event->state, 0) || event->window != gtk_widget_get_window(widget))
      darktable.bauhaus->mouse_x = ex;
    else if(ey > darktable.bauhaus->line_height / 2.0f)
    {
      const float r = _slider_right_pos((float)w3, w);
      _slider_set_normalized(w, (ex / w3) / r);

      darktable.bauhaus->mouse_x = NAN;
    }
    else
      return FALSE;
  }

  return TRUE;
}

static gboolean _widget_button_release(GtkWidget *widget,
                                       GdkEventButton *event)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_bauhaus_widget_release_quad(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return FALSE;

  dt_bauhaus_slider_data_t *d = &w->data.slider;
  if(event->button == 1 && d->is_dragging)
  {
    d->is_dragging = 0;
    if(d->timeout_handle) g_source_remove(d->timeout_handle);
    d->timeout_handle = 0;
    _slider_set_normalized(w, d->pos);

    return TRUE;
  }
  return FALSE;
}

static gboolean _widget_motion_notify(GtkWidget *widget,
                                      GdkEventMotion *event)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  const int width = gdk_window_get_width(event->window);
  const int w3 = width - w->margin.left - w->padding.left
                       - w->margin.right - w->padding.right;
  const double ex = event->x - w->margin.left - w->padding.left;

  if(w->type == DT_BAUHAUS_COMBOBOX)
  {
    darktable.control->element =
      event->x <= width - _widget_get_quad_width(w) || !w->quad_paint
      ? DT_ACTION_ELEMENT_SELECTION : DT_ACTION_ELEMENT_BUTTON;
  }
  else if(d->is_dragging && event->state & GDK_BUTTON1_MASK)
  {
    const float r = _slider_right_pos((float)w3, w);

    if(dt_isnan(darktable.bauhaus->mouse_x))
    {
      if(dt_modifier_is(event->state, 0))
        _slider_set_normalized(w, (ex / w3) / r);
      else
        darktable.bauhaus->mouse_x = ex;
    }
    else
    {
      const float scaled_step =
        w3 * r * dt_bauhaus_slider_get_step(widget) / (d->max - d->min);
      const float steps = floorf((ex - darktable.bauhaus->mouse_x) / scaled_step);
      _slider_add_step(widget, copysignf(1, d->factor) * steps, event->state, FALSE);

      darktable.bauhaus->mouse_x += steps * scaled_step;
    }
    darktable.control->element = DT_ACTION_ELEMENT_VALUE;
  }
  else if(ex <= w3 - _widget_get_quad_width(w))
  {
    darktable.control->element
        = ex > (0.1 * (w3 - _widget_get_quad_width(w)))
      && ex < (0.9 * (w3 - _widget_get_quad_width(w)))
      ? DT_ACTION_ELEMENT_VALUE : DT_ACTION_ELEMENT_FORCE;
  }
  else
    darktable.control->element =
      w->quad_paint ? DT_ACTION_ELEMENT_BUTTON : DT_ACTION_ELEMENT_VALUE;

  gtk_widget_queue_draw(widget);
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

  if(!ac
     || ac->type != DT_ACTION_TYPE_WIDGET
     || !ac->target
     || !DT_IS_BAUHAUS_WIDGET(ac->target))
    return;

  float old_value = .0f, new_value = .0f;

  GtkWidget *w = ac->target;

  switch(DT_BAUHAUS_WIDGET(w)->type)
  {
    case DT_BAUHAUS_SLIDER:
      old_value = dt_bauhaus_slider_get(w);
      new_value = dt_calculator_solve(old_value, input);
      dt_print(DT_DEBUG_ALWAYS, " = %f", new_value);
      if(dt_isfinite(new_value))
        dt_bauhaus_slider_set(w, new_value);
      break;
    case DT_BAUHAUS_COMBOBOX:
      // TODO: what about text as entry?
      old_value = dt_bauhaus_combobox_get(w);
      new_value = dt_calculator_solve(old_value, input);
      dt_print(DT_DEBUG_ALWAYS, " = %f", new_value);
      if(dt_isfinite(new_value))
        dt_bauhaus_combobox_set(w, new_value);
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
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->mute_scrolling = TRUE;
}

static void _action_process_button(GtkWidget *widget,
                                   const dt_action_effect_t effect)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(effect != (w->quad_paint_flags & CPF_ACTIVE
                ? DT_ACTION_EFFECT_ON : DT_ACTION_EFFECT_OFF))
  {
    dt_bauhaus_widget_press_quad(widget);
    dt_bauhaus_widget_release_quad(widget);
  }

  gchar *text = w->quad_toggle
              ? w->quad_paint_flags & CPF_ACTIVE ? _("button on") : _("button off")
              : _("button pressed")  ;
  dt_action_widget_toast(w->module, widget, text);

  gtk_widget_queue_draw(widget);
}

static float _action_process_slider(gpointer target,
                                    const dt_action_element_t element,
                                    const dt_action_effect_t effect,
                                    float move_size)
{
  GtkWidget *widget = GTK_WIDGET(target);
  dt_bauhaus_widget_t *bhw = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &bhw->data.slider;

  if(DT_PERFORM_ACTION(move_size))
  {
    switch(element)
    {
    case DT_ACTION_ELEMENT_VALUE:
    case DT_ACTION_ELEMENT_FORCE:
      switch(effect)
      {
      case DT_ACTION_EFFECT_POPUP:
        _popup_show(widget);
        break;
      case DT_ACTION_EFFECT_DOWN:
        move_size *= -1;
      case DT_ACTION_EFFECT_UP:
        ++d->is_dragging;
        _slider_add_step(widget, move_size,
                         GDK_MODIFIER_MASK, element == DT_ACTION_ELEMENT_FORCE);
        --d->is_dragging;
        break;
      case DT_ACTION_EFFECT_RESET:
        dt_bauhaus_widget_reset(widget);
        break;
      case DT_ACTION_EFFECT_TOP:
        dt_bauhaus_slider_set(widget,
                              element == DT_ACTION_ELEMENT_FORCE ? d->hard_max: d->max);
        break;
      case DT_ACTION_EFFECT_BOTTOM:
        dt_bauhaus_slider_set(widget,
                              element == DT_ACTION_ELEMENT_FORCE ? d->hard_min: d->min);
        break;
      case DT_ACTION_EFFECT_SET:
        dt_bauhaus_slider_set(widget, move_size);
        break;
      default:
        dt_print(DT_DEBUG_ALWAYS,
                 "[_action_process_slider] unknown shortcut effect (%d) for slider",
                 effect);
        break;
      }

      gchar *text = dt_bauhaus_slider_get_text(widget, dt_bauhaus_slider_get(widget));
      dt_action_widget_toast(bhw->module, widget, "%s", text);
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
        _popup_show(widget);
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
        dt_print(DT_DEBUG_ALWAYS,
                 "[_action_process_slider] unknown shortcut effect (%d) for slider",
                 effect);
        break;
      }

      _slider_zoom_toast(bhw);
      break;
    default:
      dt_print(DT_DEBUG_ALWAYS,
               "[_action_process_slider] unknown shortcut element (%d) for slider",
               element);
      break;
    }
  }

  if(element == DT_ACTION_ELEMENT_BUTTON)
    return dt_bauhaus_widget_get_quad_active(widget);

  if(effect == DT_ACTION_EFFECT_SET)
    return dt_bauhaus_slider_get(widget);

  if(effect == DT_ACTION_EFFECT_RESET)
    return fabsf(dt_bauhaus_slider_get(widget) - d->defpos) > 1e-5;

  return d->pos +
    ( d->min == -d->max                             ? DT_VALUE_PATTERN_PLUS_MINUS :
      ( d->min == 0 && (d->max == 1 || d->max == 100) ? DT_VALUE_PATTERN_PERCENTAGE : 0 ));
}

static gboolean _combobox_idle_value_changed(gpointer widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  _combobox_set(w, w->data.combobox.active, FALSE);

  while(g_idle_remove_by_data(widget));

  return G_SOURCE_REMOVE;
}

static float _action_process_combo(gpointer target,
                                   const dt_action_element_t element,
                                   const dt_action_effect_t effect,
                                   float move_size)
{
  GtkWidget *widget = GTK_WIDGET(target);
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;

  if(DT_PERFORM_ACTION(move_size))
  {
    if(element == DT_ACTION_ELEMENT_BUTTON || !w->data.combobox.entries->len)
    {
      _action_process_button(widget, effect);
      return dt_bauhaus_widget_get_quad_active(widget);
    }
    else switch(effect)
    {
    case DT_ACTION_EFFECT_POPUP:
      _popup_show(widget);
      break;
    case DT_ACTION_EFFECT_LAST:
      move_size *= - 1; // reversed in effect_previous
    case DT_ACTION_EFFECT_FIRST:
      move_size *= 1e3; // reversed in effect_previous
    case DT_ACTION_EFFECT_PREVIOUS:
      move_size *= - 1;
    case DT_ACTION_EFFECT_NEXT:
      ++darktable.gui->reset;
      _combobox_next_sensitive(w, move_size, GDK_MODIFIER_MASK, FALSE);
      --darktable.gui->reset;

      g_idle_add(_combobox_idle_value_changed, widget);
      break;
    case DT_ACTION_EFFECT_RESET:
      dt_bauhaus_widget_reset(widget);
      break;
    default:;
      int value = effect - DT_ACTION_EFFECT_COMBO_SEPARATOR - 1;
      dt_introspection_type_enum_tuple_t *values
        = g_hash_table_lookup(darktable.bauhaus->combo_introspection,
                              dt_action_widget(target));
      if(values)
        value = values[value].value;

      dt_bauhaus_combobox_set_from_value(widget, value);
      break;
    }

    dt_action_widget_toast(w->module, widget, "\n%s", dt_bauhaus_combobox_get_text(widget));
  }

  if(element == DT_ACTION_ELEMENT_BUTTON || !w->data.combobox.entries->len)
    return dt_bauhaus_widget_get_quad_active(widget);

  if(effect == DT_ACTION_EFFECT_RESET)
    return dt_bauhaus_combobox_get_data(widget)
      != GINT_TO_POINTER(dt_bauhaus_combobox_get_default(widget));

  int value = dt_bauhaus_combobox_get(widget);
  for(int i = value; i >= 0; i--)
  {
    if(!_combobox_entry(&w->data.combobox, i)->sensitive) value--; // don't count unselectable combo items in value
  }
  return - 1 - value
    + (value == effect - DT_ACTION_EFFECT_COMBO_SEPARATOR - 1
       ? DT_VALUE_PATTERN_ACTIVE : 0);
}

static gboolean _find_nth_bauhaus(GtkWidget **w,
                                  int *num,
                                  const dt_bauhaus_type_t type)
{
  if(!gtk_widget_get_visible(*w))
    return FALSE;
  if(DT_IS_BAUHAUS_WIDGET(*w))
  {
    dt_bauhaus_widget_t *bhw = DT_BAUHAUS_WIDGET(*w);
    return (bhw->type == type
            || (type == DT_BAUHAUS_BUTTON && bhw->quad_paint)) && !(*num)--;
  }
  if(GTK_IS_NOTEBOOK(*w) || GTK_IS_STACK(*w))
  {
    *w = GTK_IS_NOTEBOOK(*w)
       ? gtk_notebook_get_nth_page(GTK_NOTEBOOK(*w),
                                   gtk_notebook_get_current_page(GTK_NOTEBOOK(*w)))
       : gtk_stack_get_visible_child(GTK_STACK(*w));
    return _find_nth_bauhaus(w, num, type);
  }
  if(GTK_IS_CONTAINER(*w))
  {
    GList *l = gtk_container_get_children(GTK_CONTAINER(*w));
    for(GList *c = l; c && *num >= 0; c = c->next)
    {
      *w = c->data;
      _find_nth_bauhaus(w, num, type);
    }
    g_list_free(l);
  }
  return *num < 0;
}

static float _action_process_focus_slider(gpointer target,
                                          dt_action_element_t element,
                                          const dt_action_effect_t effect,
                                          const float move_size)
{
  GtkWidget *widget = ((dt_iop_module_t *)target)->widget;
  if(_find_nth_bauhaus(&widget, &element, DT_BAUHAUS_SLIDER))
    return _action_process_slider(widget, DT_ACTION_ELEMENT_VALUE, effect, move_size);

  if(DT_PERFORM_ACTION(move_size))
    dt_action_widget_toast(target, NULL, _("not that many sliders"));
  return DT_ACTION_NOT_VALID;
}

static float _action_process_focus_combo(gpointer target,
                                         dt_action_element_t element,
                                         const dt_action_effect_t effect,
                                         const float move_size)
{
  GtkWidget *widget = ((dt_iop_module_t *)target)->widget;
  if(_find_nth_bauhaus(&widget, &element, DT_BAUHAUS_COMBOBOX))
    return _action_process_combo(widget, DT_ACTION_ELEMENT_SELECTION, effect, move_size);

  if(DT_PERFORM_ACTION(move_size))
    dt_action_widget_toast(target, NULL, _("not that many dropdowns"));
  return DT_ACTION_NOT_VALID;
}

static float _action_process_focus_button(gpointer target,
                                          dt_action_element_t element,
                                          const dt_action_effect_t effect,
                                          const float move_size)
{
  GtkWidget *widget = ((dt_iop_module_t *)target)->widget;
  if(_find_nth_bauhaus(&widget, &element, DT_BAUHAUS_BUTTON))
  {
    if(DT_PERFORM_ACTION(move_size))
      _action_process_button(widget, effect);

    return dt_bauhaus_widget_get_quad_active(widget);
  }

  if(DT_PERFORM_ACTION(move_size))
    dt_action_widget_toast(target, NULL, _("not that many buttons"));
  return DT_ACTION_NOT_VALID;
}

static const dt_action_element_def_t _action_elements_slider[]
  = { { N_("value"), dt_action_effect_value },
      { N_("button"), dt_action_effect_toggle },
      { N_("force"), dt_action_effect_value },
      { N_("zoom"), dt_action_effect_value },
      { NULL } };
static const dt_action_element_def_t _action_elements_combo[]
  = { { N_("selection"), dt_action_effect_selection },
      { N_("button"), dt_action_effect_toggle },
      { NULL } };

static const dt_shortcut_fallback_t _action_fallbacks_slider[]
  = { { .element = DT_ACTION_ELEMENT_BUTTON,
        .button  = DT_SHORTCUT_LEFT },
      { .element = DT_ACTION_ELEMENT_BUTTON,
        .effect  = DT_ACTION_EFFECT_TOGGLE_CTRL,
        .button  = DT_SHORTCUT_LEFT,
        .mods    = GDK_CONTROL_MASK },
      { .element = DT_ACTION_ELEMENT_FORCE,
        .mods    = GDK_CONTROL_MASK | GDK_SHIFT_MASK,
        .speed   = 10.0 },
      { .element = DT_ACTION_ELEMENT_ZOOM,
        .effect  = DT_ACTION_EFFECT_DEFAULT_MOVE,
        .button  = DT_SHORTCUT_RIGHT,
        .move    = DT_SHORTCUT_MOVE_VERTICAL },
      { } };

static const dt_shortcut_fallback_t _action_fallbacks_combo[]
  = { { .element = DT_ACTION_ELEMENT_SELECTION,
        .effect  = DT_ACTION_EFFECT_RESET,
        .button  = DT_SHORTCUT_LEFT,
        .click   = DT_SHORTCUT_DOUBLE },
      { .element = DT_ACTION_ELEMENT_BUTTON,
        .button  = DT_SHORTCUT_LEFT },
      { .element = DT_ACTION_ELEMENT_BUTTON,
        .effect  = DT_ACTION_EFFECT_TOGGLE_CTRL,
        .button  = DT_SHORTCUT_LEFT,
        .mods    = GDK_CONTROL_MASK },
      { .move    = DT_SHORTCUT_MOVE_SCROLL,
        .effect  = DT_ACTION_EFFECT_DEFAULT_MOVE,
        .speed   = -1 },
      { .move    = DT_SHORTCUT_MOVE_VERTICAL,
        .effect  = DT_ACTION_EFFECT_DEFAULT_MOVE,
        .speed   = -1 },
      { } };

static const dt_action_def_t _action_def_slider
  = { N_("slider"),
      _action_process_slider,
      _action_elements_slider,
      _action_fallbacks_slider };
static const dt_action_def_t _action_def_combo
  = { N_("dropdown"),
      _action_process_combo,
      _action_elements_combo,
      _action_fallbacks_combo };

static const dt_action_def_t _action_def_focus_slider
  = { N_("sliders"),
      _action_process_focus_slider,
      DT_ACTION_ELEMENTS_NUM(value),
      NULL, TRUE };
static const dt_action_def_t _action_def_focus_combo
  = { N_("dropdowns"),
      _action_process_focus_combo,
      DT_ACTION_ELEMENTS_NUM(selection),
      NULL, TRUE };
static const dt_action_def_t _action_def_focus_button
  = { N_("buttons"),
      _action_process_focus_button,
      DT_ACTION_ELEMENTS_NUM(toggle),
      NULL, TRUE };

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
