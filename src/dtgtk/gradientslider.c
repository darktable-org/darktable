/*
    This file is part of darktable,
    Copyright (C) 2010-2026 darktable developers.

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

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "common/darktable.h"
#include "common/math.h"
#include "develop/develop.h"
#include "gradientslider.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "bauhaus/bauhaus.h"

#define DTGTK_GRADIENT_SLIDER_VALUE_CHANGED_DELAY_MAX 50
#define DTGTK_GRADIENT_SLIDER_VALUE_CHANGED_DELAY_MIN 10
#define DTGTK_GRADIENT_SLIDER_DEFAULT_INCREMENT 0.01

// define GTypes
G_DEFINE_TYPE(GtkDarktableGradientSlider, _gradient_slider, GTK_TYPE_DRAWING_AREA);
#define parent_class _gradient_slider_parent_class

// Class overrides
static void _gradient_slider_get_preferred_height(GtkWidget *widget,
                                                  gint *min_height,
                                                  gint *nat_height);
static void _gradient_slider_get_preferred_width(GtkWidget *widget,
                                                 gint *min_width,
                                                 gint *nat_width);
static gboolean _gradient_slider_draw(GtkWidget *widget, cairo_t *cr);
static void _gradient_slider_dispose(GObject *object);

// Events
static void _gradient_slider_enter(GtkEventControllerMotion *controller,
                                  gdouble x,
                                  gdouble y,
                                  gpointer user_data);
static void _gradient_slider_leave(GtkEventControllerMotion *controller,
                                   gpointer user_data);
static void _gradient_slider_button_pressed(GtkGestureSingle *gesture,
                                            gint n_press,
                                            gdouble x,
                                            gdouble y,
                                            gpointer user_data);
static void _gradient_slider_button_released(GtkGestureSingle *gesture,
                                             gint n_press,
                                             gdouble x,
                                             gdouble y,
                                             gpointer user_data);
static void _gradient_slider_motion(GtkEventControllerMotion *controller,
                                    gdouble x,
                                    gdouble y,
                                    gpointer user_data);
static void _gradient_slider_scroll(GtkEventControllerScroll *controller,
                                    gdouble dx,
                                    gdouble dy,
                                    gpointer user_data);
static gboolean _gradient_slider_key_pressed(GtkEventControllerKey *controller,
                                             guint keyval,
                                             guint keycode,
                                             GdkModifierType state,
                                             gpointer user_data);

enum
{
  VALUE_CHANGED,
  VALUE_RESET,
  LAST_SIGNAL
};

static guint _signals[LAST_SIGNAL] = { 0 };

static gboolean _gradient_slider_postponed_value_change(gpointer data)
{
  if(!GTK_IS_WIDGET(data)) return 0;

  if(DTGTK_GRADIENT_SLIDER(data)->is_changed)
  {
    g_signal_emit_by_name(G_OBJECT(data), "value-changed");
    DTGTK_GRADIENT_SLIDER(data)->is_changed = FALSE;
  }

  if(!DTGTK_GRADIENT_SLIDER(data)->is_dragging) DTGTK_GRADIENT_SLIDER(data)->timeout_handle = 0;
  else
  {
    const int delay = CLAMP(darktable.develop->full.pipe->average_delay * 3 / 2000,
                            DTGTK_GRADIENT_SLIDER_VALUE_CHANGED_DELAY_MIN,
                            DTGTK_GRADIENT_SLIDER_VALUE_CHANGED_DELAY_MAX);
    DTGTK_GRADIENT_SLIDER(data)->timeout_handle = g_timeout_add(delay, _gradient_slider_postponed_value_change, data);
  }

  return FALSE; // This is called by the gtk mainloop and is threadsafe
}

static inline gboolean _test_if_marker_is_upper_or_down(const gint marker,
                                                        const gboolean up)
{
  if(up && (marker == GRADIENT_SLIDER_MARKER_LOWER_OPEN ||
                  marker == GRADIENT_SLIDER_MARKER_LOWER_FILLED ||
                  marker == GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG ||
                  marker == GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG))
    return FALSE;
  else if(!up && (marker == GRADIENT_SLIDER_MARKER_UPPER_OPEN ||
                  marker == GRADIENT_SLIDER_MARKER_UPPER_FILLED ||
                  marker == GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG ||
                  marker == GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG))
    return FALSE;
  else
    return TRUE; // must be a DOUBLE
}

// a gradient slider is sized by three independent css numbers, so that a
// theme can retune any one of them without the others moving:
//   1. min-height on the widget itself -- the whole thing
//   2. min-height on its gslider-bar node -- the colored bar, centered in it
//   3. min-height on its gslider-marker node -- the handles, which are
//      always drawn flush with the widget's top and bottom edge
// shrinking (2) while (1) stays put therefore parks the handles further off
// the bar, and growing it closes the gap. 0 (the default for both parts)
// means "unset": the bar fills the widget but for a pixel of breathing room,
// and the marker matches a bauhaus slider's indicator so the two families of
// slider read alike.
//
// the two parts are css child nodes of the widget, so a theme writes
// `<selector> gslider-bar { min-height: 5px; }`, and the widget's own
// selectors, which size the whole control, do not size a part
#define GRADIENT_SLIDER_NODE_BAR "gslider-bar"
#define GRADIENT_SLIDER_NODE_MARKER "gslider-marker"

// min-height of a named css child node of the widget
static int _css_part_height(GtkWidget *widget,
                            const char *node_name)
{
  GtkWidgetPath *path = gtk_widget_path_copy(gtk_widget_get_path(widget));
  const gint pos = gtk_widget_path_append_type(path, G_TYPE_NONE);
  gtk_widget_path_iter_set_object_name(path, pos, node_name);

  GtkStyleContext *ctx = gtk_style_context_new();
  gtk_style_context_set_screen(ctx, gtk_widget_get_screen(widget));
  gtk_style_context_set_path(ctx, path);

  gint height = 0;
  gtk_style_context_get(ctx, GTK_STATE_FLAG_NORMAL, "min-height", &height, NULL);

  g_object_unref(ctx);
  gtk_widget_path_unref(path);
  return MAX(height, 0);
}

static void _update_css_metrics(GtkWidget *widget)
{
  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);
  if(gslider->css_metrics_valid) return;

  gslider->css_bar_height = _css_part_height(widget, GRADIENT_SLIDER_NODE_BAR);
  gslider->css_marker_height = _css_part_height(widget, GRADIENT_SLIDER_NODE_MARKER);
  gslider->css_metrics_valid = TRUE;
}

static void _gradient_slider_style_updated(GtkWidget *widget,
                                           gpointer user_data)
{
  DTGTK_GRADIENT_SLIDER(widget)->css_metrics_valid = FALSE;
}

static inline double _marker_border_width(void)
{
  return DT_PIXEL_APPLY_DPI(1.0);
}

static inline dt_bauhaus_marker_shape_t _marker_shape(void)
{
  const dt_bauhaus_t *bh = darktable.bauhaus;
  return bh ? bh->marker_shape : DT_BAUHAUS_MARKER_TRIANGLE;
}

// vertical extent of a marker of radius r: a triangle reaches r one way and
// half that the other, every other shape r both ways
static inline double _marker_height_factor(const dt_bauhaus_marker_shape_t shape)
{
  return (shape == DT_BAUHAUS_MARKER_TRIANGLE) ? 1.5 : 2.0;
}

// one size for every handle, whatever its state: the selected one is picked
// out by color alone, the way a bauhaus slider's indicator is. a handle that
// grew under the pointer would shift its own outer edge off the widget
// border and nudge the neighboring positions' apparent spacing
static double _marker_radius(GtkWidget *widget,
                             const dt_bauhaus_marker_shape_t shape)
{
  _update_css_metrics(widget);
  const gint css = DTGTK_GRADIENT_SLIDER(widget)->css_marker_height;
  const dt_bauhaus_t *bh = darktable.bauhaus;
  return css > 0
    ? css / _marker_height_factor(shape)
    : ((bh && bh->marker_size > 0.0f) ? bh->marker_size : DT_PIXEL_APPLY_DPI(4.0));
}

static inline double _marker_half_width(const double r,
                                        const dt_bauhaus_marker_shape_t shape)
{
  return (shape == DT_BAUHAUS_MARKER_TRIANGLE) ? 0.866025404 * r
       : (shape == DT_BAUHAUS_MARKER_BAR)      ? 0.2 * r
                                               : r; // circle and diamond
}

// how far a marker of radius r reaches beyond the edge it is drawn against:
// the tip of a triangle points inwards, so only its base sits on the edge
static inline double _marker_outer_extent(const double r,
                                          const dt_bauhaus_marker_shape_t shape)
{
  return (shape == DT_BAUHAUS_MARKER_TRIANGLE) ? 0.5 * r : r;
}

static inline int _marker_h_padding(GtkWidget *widget,
                                    const dt_bauhaus_marker_shape_t shape)
{
  return ceil(_marker_half_width(_marker_radius(widget, shape), shape));
}

static inline gdouble _screen_to_scale(GtkWidget *widget,
                                       const gint screen)
{
  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  return ((gdouble)screen - gslider->margin_left) / ((gdouble)allocation.width - gslider->margin_left - gslider->margin_right);
}

static inline gint _scale_to_screen(GtkWidget *widget,
                                    const gdouble scale)
{
  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  return (gint)(scale * (allocation.width - gslider->margin_left - gslider->margin_right) + gslider->margin_left);
}

static inline gdouble _get_position_from_screen(GtkWidget *widget,
                                                const gdouble x)
{
  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);
  gdouble position = roundf(_screen_to_scale(widget, x) / gslider->increment) * gslider->increment;
  return CLAMP(position, 0., 1.);
}

static inline gint _get_active_marker(GtkDarktableGradientSlider *gslider)
{
  return (gslider->selected >= 0) ? gslider->selected : gslider->active;
}

static inline void _clamp_marker(GtkDarktableGradientSlider *gslider,
                                 const gint selected)
{
  g_return_if_fail(gslider != NULL);

  const gdouble min = (selected == 0) ? 0.0f : gslider->position[selected - 1];
  const gdouble max = (selected == gslider->positions - 1) ? 1.0f : gslider->position[selected + 1];
  gslider->position[selected] = CLAMP(gslider->position[selected], min, max);
}

static gint _get_active_marker_from_screen(GtkWidget *widget,
                                           const gdouble x,
                                           const gdouble y)
{
  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);
  assert(gslider->positions > 0);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const gboolean up = (y <= allocation.height / 2.f);

  // only the markers on the pointer's half (upper or lower) are eligible, so
  // that an upper and a lower marker at the same place (a zero-width feather)
  // can each be picked. A full-height marker counts on both halves; with none
  // on the pointer's half, the closest marker overall wins
  const gdouble newposition = _get_position_from_screen(widget, x);

  gint best = -1;
  gdouble bestdx = 0;
  gint best_any = 0;
  gdouble bestdx_any = fabs(newposition - gslider->position[0]);

  for(int k = 0; k < gslider->positions; k++)
  {
    const gdouble dx = fabs(newposition - gslider->position[k]);
    if(dx < bestdx_any - 1e-9)
    {
      bestdx_any = dx;
      best_any = k;
    }

    if(_test_if_marker_is_upper_or_down(gslider->marker[k], up)
       && (best == -1 || dx < bestdx - 1e-9))
    {
      best = k;
      bestdx = dx;
    }
  }

  return (best >= 0) ? best : best_any;
}

static gdouble _slider_move(GtkWidget *widget,
                            const gint k,
                            const gdouble value,
                            const gint direction)
{
  g_return_val_if_fail(DTGTK_IS_GRADIENT_SLIDER(widget), value);

  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);

  gdouble newvalue = value;
  gdouble leftnext, rightnext, ms;

  switch(gslider->markers_type)
  {
    case FREE_MARKERS:
    {
      leftnext = (k == 0) ? 0.0f : gslider->position[k - 1];
      rightnext = (k == gslider->positions - 1) ? 1.0f : gslider->position[k + 1];
      ms = gslider->min_spacing;
      switch(direction)
      {
        case MOVE_LEFT:
          if(value < leftnext + ms)
            newvalue = (k == 0) ? fmax(value, 0.0f) : _slider_move(widget, k - 1, value - ms, direction) + ms;
          break;
        case MOVE_RIGHT:
          if(value > rightnext - ms)
            newvalue = (k == gslider->positions - 1) ? fmin(value, 1.0f) : _slider_move(widget, k + 1, value +  ms, direction) - ms;
          break;
      }
      break;
    }
    case PROPORTIONAL_MARKERS:
    {
      ms = fmax(gslider->min_spacing, 1.0e-6);
      const double vmin = ((k == 0) ? 0.0f : gslider->position[0]);
      const double vmax = ((k == gslider->positions - 1) ? 1.0f : gslider->position[gslider->positions - 1]);

      newvalue = CLAMP(value, vmin + ms * k, vmax - ms * (gslider->positions - 1 - k));
      const double rl = (newvalue - gslider->position[0]) / (gslider->position[k] - gslider->position[0]);
      const double rh = (gslider->position[gslider->positions - 1] - newvalue) /
                        (gslider->position[gslider->positions - 1] - gslider->position[k]);

      for(int i = 1; i < k; i++)
        gslider->position[i] = rl * (gslider->position[i] - gslider->position[0]) + gslider->position[0];

      for(int i = k + 1; i < gslider->positions; i++)
        gslider->position[i] = gslider->position[gslider->positions - 1] -
                               rh * (gslider->position[gslider->positions - 1] - gslider->position[i]);
      break;
    }
  }
  gslider->position[k] = newvalue;
  return newvalue;
}

static gboolean _gradient_slider_add_delta_internal(GtkWidget *widget,
                                                    gdouble delta,
                                                    const guint state,
                                                    const gint selected)
{
  g_return_val_if_fail(DTGTK_IS_GRADIENT_SLIDER(widget), TRUE);

  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);

  if(selected == -1) return TRUE;

  delta *= dt_accel_get_speed_multiplier(widget, state);

  gslider->position[selected] = gslider->position[selected] + delta;
  _clamp_marker(gslider, selected);

  gtk_widget_queue_draw(widget);
  g_signal_emit_by_name(G_OBJECT(widget), "value-changed");

  return TRUE;
}

static float _default_linear_scale_callback(GtkWidget *self,
                                            const float value,
                                            const int dir)
{
  // regardless of dir: input <-> output
  return value;
}

static void _gradient_slider_enter(GtkEventControllerMotion *controller,
                                   gdouble x,
                                   gdouble y,
                                   gpointer user_data)
{
  GtkWidget *widget = dt_gui_get_widget(controller);
  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);
  gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_PRELIGHT, TRUE);
  gslider->is_entered = TRUE;
  gtk_widget_queue_draw(widget);
}

static void _gradient_slider_leave(GtkEventControllerMotion *controller,
                                   gpointer user_data)
{
  GtkWidget *widget = dt_gui_get_widget(controller);
  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);
  if(!(gslider->is_dragging))
  {
    gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_NORMAL, TRUE);
    gslider->is_entered = FALSE;
    gslider->active = -1;
    gtk_widget_queue_draw(widget);
  }
}

static void _gradient_slider_button_pressed(GtkGestureSingle *gesture,
                                            gint n_press,
                                            gdouble x,
                                            gdouble y,
                                            gpointer user_data)
{
  GtkWidget *widget = dt_gui_get_widget(gesture);
  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);
  const guint button = gtk_gesture_single_get_current_button(gesture);

  // reset slider on double click
  if(button == GDK_BUTTON_PRIMARY && n_press == 2 && gslider->is_resettable)
  {
    gslider->is_dragging = FALSE;
    gslider->do_reset = TRUE;
    gslider->selected = -1;
    for(int k = 0; k < gslider->positions; k++) gslider->position[k] = gslider->resetvalue[k];
    gtk_widget_queue_draw(widget);
    g_signal_emit_by_name(G_OBJECT(widget), "value-changed");
    g_signal_emit_by_name(G_OBJECT(widget), "value-reset");
  }
  else if((button == GDK_BUTTON_PRIMARY || button == GDK_BUTTON_SECONDARY) && n_press == 1)
  {
    const gint lselected = _get_active_marker_from_screen(widget, x, y);

    assert(lselected >= 0);
    assert(lselected <= gslider->positions - 1);

    if(button == GDK_BUTTON_PRIMARY) // left mouse button : select and start dragging
    {
      gslider->selected = lselected;
      gslider->do_reset = FALSE;

      const gdouble newposition = _get_position_from_screen(widget, x);
      const gint direction = gslider->position[gslider->selected] <= newposition ? MOVE_RIGHT : MOVE_LEFT;

      _slider_move(widget, gslider->selected, newposition, direction);

      gslider->is_changed = TRUE;
      gslider->is_dragging = TRUE;

      /** We modify the slider's timeout_handle setting it to 150% of average_delay of the full pipe.
          Note that g_timeout_add() delay is in ms and average_delay in microseconds!
      */
      const int delay = CLAMP(darktable.develop->full.pipe->average_delay * 3 / 2000,
                              DTGTK_GRADIENT_SLIDER_VALUE_CHANGED_DELAY_MIN,
                              DTGTK_GRADIENT_SLIDER_VALUE_CHANGED_DELAY_MAX);
      // timeout_handle should always be zero here, but check just in case
      if(!gslider->timeout_handle)
        gslider->timeout_handle = g_timeout_add(delay, _gradient_slider_postponed_value_change, widget);
    }
    else if(gslider->positions > 1) // right mouse button: switch on/off selection (only if we have more than one marker)
    {
      gslider->is_dragging = FALSE;
      gslider->do_reset = FALSE;

      if(gslider->selected != lselected)
        gslider->selected = lselected;
      else
        gslider->selected = -1;

      gtk_widget_queue_draw(widget);
    }
  }

  // claim the sequence, or an ancestor's gesture (the scrolled panel the
  // slider is in) claims it on the first move, and this gesture's cancel
  // (_gesture_cancel in gui/gtk.c) ends the drag there
  dt_gui_claim(gesture);
}

static void _gradient_slider_motion(GtkEventControllerMotion *controller,
                                    gdouble x,
                                    gdouble y,
                                    gpointer user_data)
{
  GtkWidget *widget = dt_gui_get_widget(controller);
  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);

  if(gslider->is_dragging && gslider->selected != -1 && !gslider->do_reset)
  {
    assert(gslider->timeout_handle > 0);

    const gdouble newposition = _get_position_from_screen(widget, x);
    const gint direction = gslider->position[gslider->selected] <= newposition ? MOVE_RIGHT : MOVE_LEFT;

    _slider_move(widget, gslider->selected, newposition, direction);

    gslider->is_changed = TRUE;

    gtk_widget_queue_draw(widget);
  }
  else
  {
    const gint active = _get_active_marker_from_screen(widget, x, y);
    if(active != gslider->active)
    {
      gslider->active = active;
      gtk_widget_queue_draw(widget);
    }
  }

  if(gslider->selected != -1) gtk_widget_grab_focus(widget);
}

static void _gradient_slider_button_released(GtkGestureSingle *gesture,
                                             gint n_press,
                                             gdouble x,
                                             gdouble y,
                                             gpointer user_data)
{
  GtkWidget *widget = dt_gui_get_widget(gesture);
  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);
  const gint selected = _get_active_marker(gslider);

  if(gtk_gesture_single_get_current_button(gesture) == GDK_BUTTON_PRIMARY
     && selected != -1 && gslider->do_reset == FALSE)
  {
    // First get some dimension info
    gslider->is_changed = TRUE;
    const gdouble newposition = _get_position_from_screen(widget, x);
    const gint direction = gslider->position[selected] <= newposition ? MOVE_RIGHT : MOVE_LEFT;

    _slider_move(widget, selected, newposition, direction);

    gtk_widget_queue_draw(widget);

    gslider->is_dragging = FALSE;
    if(gslider->timeout_handle) g_source_remove(gslider->timeout_handle);
    gslider->timeout_handle = 0;
    g_signal_emit_by_name(G_OBJECT(widget), "value-changed");
  }

  dt_gui_claim(gesture);
}

static void _gradient_slider_scroll(GtkEventControllerScroll *controller,
                                    gdouble dx,
                                    gdouble dy,
                                    gpointer user_data)
{
  GtkWidget *widget = dt_gui_get_widget(controller);
  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);
  const gint selected = _get_active_marker(gslider);
  if(selected == -1) return;

  gtk_widget_grab_focus(widget);

  // the DISCRETE proxy already accumulated smooth deltas into unit steps
  const int delta_y = (int)dy;
  if(delta_y == 0) return;

  _gradient_slider_add_delta_internal(widget, delta_y * -gslider->increment,
                                      dt_gui_get_current_event_state(GTK_EVENT_CONTROLLER(controller)),
                                      selected);
}

static gboolean _gradient_slider_key_pressed(GtkEventControllerKey *controller,
                                             guint keyval,
                                             guint keycode,
                                             GdkModifierType state,
                                             gpointer user_data)
{
  GtkWidget *widget = dt_gui_get_widget(controller);
  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);

  int handled = FALSE;
  float delta = -gslider->increment;
  switch(keyval)
  {
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
      delta = gslider->increment;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
      handled = TRUE;
  }

  if(!handled) return FALSE;

  const gint selected = _get_active_marker(gslider);
  if(selected == -1) return TRUE;

  return _gradient_slider_add_delta_internal(widget, delta, state, selected);
}

static void _gradient_slider_class_init(GtkDarktableGradientSliderClass *klass)
{
  GtkWidgetClass *widget_class = (GtkWidgetClass *)klass;

  widget_class->get_preferred_height = _gradient_slider_get_preferred_height;
  widget_class->get_preferred_width = _gradient_slider_get_preferred_width;
  widget_class->draw = _gradient_slider_draw;
  GObjectClass *object_class = (GObjectClass *)klass;
  object_class->dispose = _gradient_slider_dispose;

  _signals[VALUE_CHANGED] = g_signal_new("value-changed",
                                         G_TYPE_FROM_CLASS(klass),
                                         G_SIGNAL_RUN_LAST, 0,
                                         NULL, NULL,
                                         g_cclosure_marshal_VOID__VOID,
                                         G_TYPE_NONE, 0);
  _signals[VALUE_RESET] = g_signal_new("value-reset",
                                       G_TYPE_FROM_CLASS(klass),
                                       G_SIGNAL_RUN_LAST, 0,
                                       NULL, NULL,
                                       g_cclosure_marshal_VOID__VOID,
                                       G_TYPE_NONE, 0);
}

static void _gradient_slider_init(GtkDarktableGradientSlider *gslider)
{
  g_return_if_fail(gslider != NULL);

  GtkWidget *widget = GTK_WIDGET(gslider);
  gtk_widget_add_events(widget,
                        GDK_EXPOSURE_MASK |
                        GDK_BUTTON_PRESS_MASK |
                        GDK_BUTTON_RELEASE_MASK |
                        GDK_ENTER_NOTIFY_MASK |
                        GDK_LEAVE_NOTIFY_MASK |
                        GDK_KEY_PRESS_MASK |
                        GDK_KEY_RELEASE_MASK |
                        GDK_POINTER_MOTION_MASK |
                        darktable.gui->scroll_mask);

  gtk_widget_set_has_window(widget, TRUE);
  gtk_widget_set_can_focus(widget, TRUE);

  // GTK3 class handlers (button-press-event & friends) are gone in GTK4:
  // route the input through controllers, which exist in both.  n_press
  // replaces the GDK_2BUTTON_PRESS reset check, the DISCRETE scroll proxy
  // replaces the unit-delta accumulator.
  dt_gui_connect_click(widget, _gradient_slider_button_pressed,
                       _gradient_slider_button_released, NULL);
  dt_gui_connect_motion(widget, _gradient_slider_motion,
                        _gradient_slider_enter, _gradient_slider_leave, NULL);
  dt_gui_connect_scroll(widget,
                        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL
                          | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE,
                        _gradient_slider_scroll, NULL);
  dt_gui_connect_key(widget, _gradient_slider_key_pressed, NULL);

  gslider->css_metrics_valid = FALSE;
  g_signal_connect(G_OBJECT(widget), "style-updated",
                   G_CALLBACK(_gradient_slider_style_updated), NULL);
}

static void _gradient_slider_get_preferred_height(GtkWidget *widget,
                                                  gint *min_height,
                                                  gint *nat_height)
{
  g_return_if_fail(widget != NULL);

  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  GtkStateFlags state = gtk_widget_get_state_flags(widget);

  GtkBorder margin, border, padding;
  int css_min_height;
  gtk_style_context_get(context, state, "min-height", &css_min_height, NULL);
  gtk_style_context_get_margin(context, state, &margin);
  gtk_style_context_get_border(context, state, &border);
  gtk_style_context_get_padding(context, state, &padding);
  *min_height = *nat_height = css_min_height + padding.top + padding.bottom + border.top + border.bottom + margin.top + margin.bottom;
}

static void _gradient_slider_get_preferred_width(GtkWidget *widget,
                                                 gint *min_width,
                                                 gint *nat_width)
{
  g_return_if_fail(DTGTK_IS_GRADIENT_SLIDER(widget));

  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  GtkStateFlags state = gtk_widget_get_state_flags(widget);

  GtkBorder margin, border, padding;
  int css_min_width;
  gtk_style_context_get (context, state, "min-width", &css_min_width, NULL);
  gtk_style_context_get_margin(context, state, &margin);
  gtk_style_context_get_border(context, state, &border);
  gtk_style_context_get_padding(context, state, &padding);
  *min_width = *nat_width = css_min_width + padding.left + padding.right + border.left + border.right + margin.left + margin.right;

  const int hpad = _marker_h_padding(widget, _marker_shape());

  DTGTK_GRADIENT_SLIDER(widget)->margin_left = padding.left + border.left + margin.left + hpad;
  DTGTK_GRADIENT_SLIDER(widget)->margin_right = padding.right + border.right + margin.right + hpad;
}

static void _gradient_slider_dispose(GObject *object)
{
  g_return_if_fail(DTGTK_IS_GRADIENT_SLIDER(object));

  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(object);

  if(gslider->timeout_handle)
    g_source_remove(gslider->timeout_handle);

  gslider->timeout_handle = 0;

  if(gslider->colors)
    g_list_free_full(gslider->colors, g_free);

  gslider->colors = NULL;

  G_OBJECT_CLASS(parent_class)->dispose(object);
}

static void _draw_gradient_marker_shape(cairo_t *cr,
                                        const double vx,
                                        const double cy,
                                        const double r,
                                        const gboolean is_upper,
                                        const dt_bauhaus_marker_shape_t shape)
{
  if(shape == DT_BAUHAUS_MARKER_CIRCLE)
  {
    cairo_arc(cr, vx, cy, r, 0, 2.0 * M_PI);
  }
  else if(shape == DT_BAUHAUS_MARKER_DIAMOND)
  {
    cairo_move_to(cr, vx, cy - r);
    cairo_line_to(cr, vx - r, cy);
    cairo_line_to(cr, vx, cy + r);
    cairo_line_to(cr, vx + r, cy);
  }
  else if(shape == DT_BAUHAUS_MARKER_BAR)
  {
    cairo_rectangle(cr, vx - r * 0.2, cy - r, r * 0.4, 2.0 * r);
  }
  else
  {
    // DT_BAUHAUS_MARKER_TRIANGLE (default)
    const double sin_r = 0.866025404 * r;
    const double cos_r = 0.5 * r;
    if(is_upper)
    {
      cairo_move_to(cr, vx, cy + r);
      cairo_line_to(cr, vx - sin_r, cy - cos_r);
      cairo_line_to(cr, vx + sin_r, cy - cos_r);
    }
    else
    {
      cairo_move_to(cr, vx, cy - r);
      cairo_line_to(cr, vx - sin_r, cy + cos_r);
      cairo_line_to(cr, vx + sin_r, cy + cos_r);
    }
  }
  cairo_close_path(cr);
}

// the two marker styles are each other's negative: one is filled with the
// foreground color and outlined in the background one, the other the way
// round. both are the same silhouette, and the outline is drawn *inside* it
// (stroked at twice the width, clipped to the shape, so half the stroke
// lands within the edge), so neither style grows the marker beyond r -- an
// outline stroked around the path would make the handle read larger than
// every other slider's indicator.
static void _draw_gradient_marker(cairo_t *cr,
                                  const double vx,
                                  const double cy,
                                  const double r,
                                  const gboolean is_upper,
                                  const dt_bauhaus_marker_shape_t shape,
                                  const GdkRGBA *fill,
                                  const GdkRGBA *border,
                                  const double border_width)
{
  cairo_save(cr);
  _draw_gradient_marker_shape(cr, vx, cy, r, is_upper, shape);
  cairo_set_source_rgba(cr, fill->red, fill->green, fill->blue, fill->alpha);
  cairo_fill_preserve(cr);
  cairo_clip_preserve(cr);
  cairo_set_source_rgba(cr, border->red, border->green, border->blue, border->alpha);
  cairo_set_line_width(cr, 2.0 * border_width);
  cairo_stroke(cr);
  cairo_restore(cr);
}

static gboolean _gradient_slider_draw(GtkWidget *widget,
                                      cairo_t *cr)
{
  g_return_val_if_fail(DTGTK_IS_GRADIENT_SLIDER(widget), FALSE);
  GtkDarktableGradientSlider *gslider = DTGTK_GRADIENT_SLIDER(widget);

  assert(gslider->position > 0);

  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  GtkStateFlags state = gtk_widget_get_state_flags(widget);

  GdkRGBA color;
  gtk_style_context_get_color(context, state, &color);

  GtkAllocation allocation;
  GtkBorder margin, border, padding;
  gtk_widget_get_allocation(widget, &allocation);
  gtk_style_context_get_margin(context, state, &margin);
  gtk_style_context_get_border(context, state, &border);
  gtk_style_context_get_padding(context, state, &padding);

  // Begin cairo drawing
  // for frame and background, we remove css margin from allocation
  int startx = margin.left;
  int starty = margin.top;
  int cwidth = allocation.width - margin.left - margin.right;
  int cheight = allocation.height - margin.top - margin.bottom;
  gtk_render_background(context, cr, startx, starty, cwidth, cheight);
  gtk_render_frame(context, cr, startx, starty, cwidth, cheight);

  // then we draw the content
  startx += padding.left + border.left;
  starty += padding.top + border.top;
  cwidth -= padding.left + padding.right + border.left + border.right;
  cheight -= padding.top + padding.bottom + border.top + border.bottom;

  const dt_bauhaus_marker_shape_t shape = _marker_shape();
  const double marker_border_width = _marker_border_width();
  const int hpad = _marker_h_padding(widget, shape);

  // the two edges of a range slider are told apart by inverting one pair of
  // theme colors rather than by size: the range handles are filled with the
  // foreground and outlined in the background, the feather ones the other
  // way round. a theme that defines neither falls back to the widget's own
  // css color, dimmed while idle, over black.
  GdkRGBA c_fg, c_fg_hover, c_bg;
  if(!gtk_style_context_lookup_color(context, "gslider_marker_fg", &c_fg))
    c_fg = (GdkRGBA){ color.red * 0.85, color.green * 0.85, color.blue * 0.85, 1.0 };
  if(!gtk_style_context_lookup_color(context, "gslider_marker_fg_hover", &c_fg_hover))
    c_fg_hover = color;
  if(!gtk_style_context_lookup_color(context, "gslider_marker_bg", &c_bg))
    c_bg = (GdkRGBA){ 0.0, 0.0, 0.0, 1.0 };

  gslider->margin_left = padding.left + border.left + margin.left + hpad;
  gslider->margin_right = padding.right + border.right + margin.right + hpad;

  const int gx = startx + hpad;
  const int gw = cwidth - 2 * hpad;

  // the bar takes the height css gives it, centered in the widget, and the
  // markers below sit against the widget's edges, not the bar's: a theme sets
  // the gap between them with the two heights. Unset (0), the bar fills the
  // widget but for a pixel top and bottom
  _update_css_metrics(widget);
  const int y1 = DT_PIXEL_APPLY_DPI(1);
  const int gheight = CLAMP(gslider->css_bar_height > 0
                            ? gslider->css_bar_height
                            : cheight - 2 * y1, 1, cheight);
  const int gtop = starty + (cheight - gheight) / 2;

  // First build the cairo gradient and then fill the gradient
  if(gslider->colors)
  {
    cairo_pattern_t *gradient = cairo_pattern_create_linear(0, 0, gw, 0);
    for(GList *current = gslider->colors; current; current = g_list_next(current))
    {
      _gradient_slider_stop_t *stop = (_gradient_slider_stop_t *)current->data;
      cairo_pattern_add_color_stop_rgba(gradient, stop->position, stop->color.red, stop->color.green,
                                       stop->color.blue, stop->color.alpha);
    }
    if(gradient != NULL) // Do we got a gradient, lets draw it
    {
      cairo_set_line_width(cr, 0.1);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_translate(cr, gx, gtop);
      cairo_set_source(cr, gradient);
      cairo_rectangle(cr, 0, 0, gw, gheight);
      cairo_fill(cr);
      cairo_stroke(cr);
      cairo_pattern_destroy(gradient);
      cairo_translate(cr, -gx, -gtop);
    }
  }

  // Lets draw position arrows
  cairo_set_source_rgba(cr, color.red, color.green, color.blue, 1.0);

  // do we have a picker value to draw?
  if(!dt_isnan(gslider->picker[0]))
  {
    int vx_min = _scale_to_screen(widget, CLAMP(gslider->picker[1], 0.0, 1.0));
    int vx_max = _scale_to_screen(widget, CLAMP(gslider->picker[2], 0.0, 1.0));
    int vx_avg = _scale_to_screen(widget, CLAMP(gslider->picker[0], 0.0, 1.0));

    cairo_set_source_rgba(cr, color.red, color.green, color.blue, 0.33);

    cairo_rectangle(cr, vx_min, gtop, fmax((float)vx_max - vx_min, 0.0f), gheight);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, color.red, color.green, color.blue, 1.0);

    cairo_move_to(cr, vx_avg, gtop);
    cairo_rel_line_to(cr, 0, gheight);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
  }

  // A 4-point open/filled/filled/open marker set is the "range + feather"
  // convention (parametric blendif channels): paint the feather zones
  // directly on the gradient bar, white and fading from the open (feather)
  // point to full opacity at the neighboring filled (range) point, plus a
  // plain outline around the flat, fully-selected zone between the two
  // filled points. The open marker's own up/down bit decides which edge
  // (top or bottom) each wedge's point sits on, so this follows polarity
  // (the "invert" toggle swaps that bit) instead of assuming a fixed orientation.
  if(gslider->positions == 4
     && !(gslider->marker[0] & 0x01) && (gslider->marker[1] & 0x01)
     && (gslider->marker[2] & 0x01) && !(gslider->marker[3] & 0x01))
  {
    const int x0 = _scale_to_screen(widget, gslider->position[0]);
    const int x1 = _scale_to_screen(widget, gslider->position[1]);
    const int x2 = _scale_to_screen(widget, gslider->position[2]);
    const int x3 = _scale_to_screen(widget, gslider->position[3]);
    const int top = gtop;
    const int bottom = gtop + gheight;

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.55);

    const int apex0 = (gslider->marker[0] & 0x04) ? top : bottom;
    cairo_move_to(cr, x0, apex0);
    cairo_line_to(cr, x1, top);
    cairo_line_to(cr, x1, bottom);
    cairo_close_path(cr);
    cairo_fill(cr);

    const int apex3 = (gslider->marker[3] & 0x04) ? top : bottom;
    cairo_move_to(cr, x3, apex3);
    cairo_line_to(cr, x2, top);
    cairo_line_to(cr, x2, bottom);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
    // marker[0]'s up bit is the polarity, as for the wedges: inverted, the
    // selection is everything outside [x1, x2], so the two outer bands get
    // the outline instead of the inner one
    if(gslider->marker[0] & 0x04)
    {
      cairo_rectangle(cr, gx + 0.5, top + 0.5, fmax(x1 - gx - 1, 0), fmax(bottom - top - 1, 0));
      cairo_stroke(cr);
      cairo_rectangle(cr, x2 + 0.5, top + 0.5, fmax(gx + gw - x2 - 1, 0), fmax(bottom - top - 1, 0));
      cairo_stroke(cr);
    }
    else
    {
      cairo_rectangle(cr, x1 + 0.5, top + 0.5, fmax(x2 - x1 - 1, 0), fmax(bottom - top - 1, 0));
      cairo_stroke(cr);
    }
  }

  // highlight the marker closest to the pointer (gslider->active, from the
  // click's hit test on every motion), or the dragged one while dragging, so
  // that it stays lit when the pointer passes a neighbor, or the pinned one.
  // Not gslider->selected, which stays set after release for the keyboard
  // and the scroll wheel
  const gint hovered_marker = gslider->pinned >= 0 ? gslider->pinned
                             : gslider->is_dragging ? gslider->selected
                             : (gslider->is_entered ? gslider->active : -1);

  const double r = _marker_radius(widget, shape);
  const double outer = _marker_outer_extent(r, shape);

  for(int k = 0; k < gslider->positions; k++)
  {
    const int vx = _scale_to_screen(widget, gslider->position[k]);
    const int mk = gslider->marker[k];
    const gboolean hovered = (k == hovered_marker);
    const gboolean filled = mk & 0x01;
    const GdkRGBA accent = hovered ? c_fg_hover : c_fg;
    const GdkRGBA *m_fill = filled ? &accent : &c_bg;
    const GdkRGBA *m_border = filled ? &c_bg : &accent;

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);

    if(mk & 0x04) /* upper handle, flush with the widget's top edge */
    {
      _draw_gradient_marker(cr, vx, starty + outer, r, TRUE, shape,
                            m_fill, m_border, marker_border_width);
    }

    if(mk & 0x02) /* lower handle, flush with the widget's bottom edge */
    {
      _draw_gradient_marker(cr, vx, starty + cheight - outer, r, FALSE, shape,
                            m_fill, m_border, marker_border_width);
    }
  }

  return FALSE;
}

gint _list_find_by_position(gconstpointer a, gconstpointer b)
{
  _gradient_slider_stop_t *stop = (_gradient_slider_stop_t *)a;
  gfloat position = *((gfloat *)b);
  return (gint)((stop->position * 100.0) - (position * 100.0));
}

static void _gradient_slider_set_defaults(GtkDarktableGradientSlider *gslider)
{
  g_return_if_fail(gslider != NULL);

  gslider->is_dragging = gslider->is_changed =
    gslider->do_reset = gslider->is_entered = 0;
  gslider->timeout_handle = 0;
  gslider->selected = gslider->positions == 1 ? 0 : -1;
  gslider->active = -1;
  gslider->pinned = -1;
  gslider->scale_callback = _default_linear_scale_callback;
  gslider->is_resettable = FALSE;
  gslider->is_entered = FALSE;
  gslider->picker[0] = gslider->picker[1] = gslider->picker[2] = NAN;
  gslider->increment = DTGTK_GRADIENT_SLIDER_DEFAULT_INCREMENT;
  gslider->margin_left = gslider->margin_right =
    GRADIENT_SLIDER_MARGINS_DEFAULT;
  gslider->markers_type = FREE_MARKERS;
  gslider->colors = NULL;
  gslider->min_spacing = 0;
  for(int k = 0; k < gslider->positions; k++)
  {
    gslider->position[k] = 0.0;
    gslider->resetvalue[k] = 0.0;
    gslider->marker[k] = GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG;
  }
}


// Public functions for multivalue type
GtkWidget *dtgtk_gradient_slider_multivalue_new(const gint positions)
{
  assert(positions <= GRADIENT_SLIDER_MAX_POSITIONS);

  GtkDarktableGradientSlider *gslider;
  gslider = g_object_new(_gradient_slider_get_type(), NULL);
  gslider->positions = positions;
  _gradient_slider_set_defaults(gslider);
  dt_gui_add_class(GTK_WIDGET(gslider), "dt_gslider_multivalue");
  return (GtkWidget *)gslider;
}

GtkWidget *dtgtk_gradient_slider_multivalue_new_with_name
  (const gint positions,
   gchar *name)
{
  GtkWidget *widget = GTK_WIDGET(dtgtk_gradient_slider_multivalue_new(positions));
  if(name) gtk_widget_set_name(widget, name);

  return widget;
}

GtkWidget *dtgtk_gradient_slider_multivalue_new_with_color
  (const GdkRGBA start,
   const GdkRGBA end,
   const gint positions)
{
  assert(positions <= GRADIENT_SLIDER_MAX_POSITIONS);

  GtkDarktableGradientSlider *gslider;
  gslider = g_object_new(_gradient_slider_get_type(), NULL);
  gslider->positions = positions;
  _gradient_slider_set_defaults(gslider);

  // Construct gradient start color
  _gradient_slider_stop_t *gc = g_malloc(sizeof(_gradient_slider_stop_t));
  gc->position = 0.0;
  memcpy(&gc->color, &start, sizeof(GdkRGBA));
  gslider->colors = g_list_append(gslider->colors, gc);

  // Construct gradient stop color
  gc = g_malloc(sizeof(_gradient_slider_stop_t));
  gc->position = 1.0;
  memcpy(&gc->color, &end, sizeof(GdkRGBA));
  gslider->colors = g_list_append(gslider->colors, gc);
  dt_gui_add_class(GTK_WIDGET(gslider), "dt_gslider_multivalue");
  return (GtkWidget *)gslider;
}

GtkWidget *dtgtk_gradient_slider_multivalue_new_with_color_and_name
  (const GdkRGBA start,
   const GdkRGBA end,
   const gint positions,
   gchar *name)
{
  GtkWidget *widget = GTK_WIDGET(dtgtk_gradient_slider_multivalue_new_with_color(start, end, positions));
  if(name) gtk_widget_set_name(widget, name);

  return widget;
}

void dtgtk_gradient_slider_multivalue_set_stop
  (GtkDarktableGradientSlider *gslider,
   const gfloat position,
   const GdkRGBA color)
{
  g_return_if_fail(gslider != NULL);
  const gfloat rawposition = gslider->scale_callback((GtkWidget *)gslider,
                                                     position, GRADIENT_SLIDER_SET);
  // First find color at position, if exists update color, otherwise
  // create a new stop at position.
  GList *current = g_list_find_custom(gslider->colors, (gpointer)&rawposition,
                                      _list_find_by_position);
  if(current != NULL)
  {
    memcpy(&((_gradient_slider_stop_t *)current->data)->color,
           &color, sizeof(GdkRGBA));
  }
  else
  {
    // stop didn't exist lets add it
    _gradient_slider_stop_t *gc = g_malloc(sizeof(_gradient_slider_stop_t));
    gc->position = rawposition;
    memcpy(&gc->color, &color, sizeof(GdkRGBA));
    gslider->colors = g_list_append(gslider->colors, gc);
  }
}

void dtgtk_gradient_slider_multivalue_clear_stops
  (GtkDarktableGradientSlider *gslider)
{
  g_return_if_fail(gslider != NULL);
  g_list_free_full(gslider->colors, g_free);
  gslider->colors = NULL;
}

GType dtgtk_gradient_slider_multivalue_get_type()
{
  return _gradient_slider_get_type();
}

gdouble dtgtk_gradient_slider_multivalue_get_value
  (GtkDarktableGradientSlider *gslider,
   const gint pos)
{
  assert(pos <= gslider->positions);

  return gslider->scale_callback((GtkWidget *)gslider,
                                 gslider->position[pos],
                                 GRADIENT_SLIDER_GET);
}

void dtgtk_gradient_slider_multivalue_get_values
  (GtkDarktableGradientSlider *gslider,
   gdouble *values)
{
  g_return_if_fail(gslider != NULL);
  for(int k = 0; k < gslider->positions; k++)
    values[k] = gslider->scale_callback((GtkWidget *)gslider,
                                        gslider->position[k],
                                        GRADIENT_SLIDER_GET);
}

void dtgtk_gradient_slider_multivalue_set_value
  (GtkDarktableGradientSlider *gslider,
   const gdouble value,
   const gint pos)
{
  g_return_if_fail(gslider != NULL);
  assert(pos <= gslider->positions);

  gslider->position[pos] = CLAMP(gslider->scale_callback((GtkWidget *)gslider,
                                                         value,
                                                         GRADIENT_SLIDER_SET),
                                 0.0, 1.0);
  gslider->selected = gslider->positions == 1 ? 0 : -1;
  if(!DT_IN_GUI_UPDATE()) g_signal_emit_by_name(G_OBJECT(gslider),
                                                  "value-changed");
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}

void dtgtk_gradient_slider_multivalue_set_value_pushing
  (GtkDarktableGradientSlider *gslider,
   const gdouble value,
   const gint pos)
{
  g_return_if_fail(gslider != NULL);
  assert(pos <= gslider->positions);

  const gdouble newpos = CLAMP(gslider->scale_callback((GtkWidget *)gslider,
                                                        value,
                                                        GRADIENT_SLIDER_SET),
                               0.0, 1.0);
  // _slider_move (FREE_MARKERS branch) only ever pushes the neighbor that
  // lies on the side newpos is heading towards -- the same direction a mouse
  // drag would be moving in to reach it.
  const gint direction = (newpos < gslider->position[pos]) ? MOVE_LEFT : MOVE_RIGHT;
  _slider_move((GtkWidget *)gslider, pos, newpos, direction);

  gslider->selected = gslider->positions == 1 ? 0 : -1;
  if(!DT_IN_GUI_UPDATE()) g_signal_emit_by_name(G_OBJECT(gslider),
                                                  "value-changed");
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}

void dtgtk_gradient_slider_multivalue_set_values
  (GtkDarktableGradientSlider *gslider,
   gdouble *values)
{
  g_return_if_fail(gslider != NULL);
  g_return_if_fail(values != NULL);
  for(int k = 0; k < gslider->positions; k++)
    gslider->position[k] = CLAMP(gslider->scale_callback((GtkWidget *)gslider,
                                                         values[k],
                                                         GRADIENT_SLIDER_SET),
                                 0.0, 1.0);
  gslider->selected = gslider->positions == 1 ? 0 : -1;
  if(!DT_IN_GUI_UPDATE()) g_signal_emit_by_name(G_OBJECT(gslider),
                                                  "value-changed");
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}

void dtgtk_gradient_slider_multivalue_set_marker
  (GtkDarktableGradientSlider *gslider,
   const gint mark,
   const gint pos)
{
  g_return_if_fail(gslider != NULL);
  assert(pos <= gslider->positions);

  gslider->marker[pos] = mark;
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}

void dtgtk_gradient_slider_multivalue_set_markers
  (GtkDarktableGradientSlider *gslider,
   gint *markers)
{
  g_return_if_fail(gslider != NULL);
  for(int k = 0; k < gslider->positions; k++) gslider->marker[k] = markers[k];
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}

void dtgtk_gradient_slider_multivalue_set_resetvalue
  (GtkDarktableGradientSlider *gslider,
   const gdouble value,
   const gint pos)
{
  g_return_if_fail(gslider != NULL);
  assert(pos <= gslider->positions);

  gslider->resetvalue[pos] = gslider->scale_callback((GtkWidget *)gslider,
                                                     value, GRADIENT_SLIDER_SET);
  gslider->is_resettable = TRUE;
}

gdouble dtgtk_gradient_slider_multivalue_get_resetvalue
  (GtkDarktableGradientSlider *gslider,
   const gint pos)
{
  assert(pos <= gslider->positions);

  return gslider->scale_callback((GtkWidget *)gslider,
                                 gslider->resetvalue[pos], GRADIENT_SLIDER_GET);
}

void dtgtk_gradient_slider_multivalue_set_resetvalues
  (GtkDarktableGradientSlider *gslider,
   gdouble *values)
{
  g_return_if_fail(gslider != NULL);
  for(int k = 0; k < gslider->positions; k++)
    gslider->resetvalue[k] = gslider->scale_callback((GtkWidget *)gslider,
                                                     values[k],
                                                     GRADIENT_SLIDER_SET);
  gslider->is_resettable = TRUE;
}

void dtgtk_gradient_slider_multivalue_set_picker
  (GtkDarktableGradientSlider *gslider,
   const gdouble value)
{
  g_return_if_fail(gslider != NULL);
  gslider->picker[0] = gslider->picker[1] = gslider->picker[2]
    = gslider->scale_callback((GtkWidget *)gslider, value, GRADIENT_SLIDER_SET);
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}

void dtgtk_gradient_slider_multivalue_set_picker_meanminmax
  (GtkDarktableGradientSlider *gslider,
   const gdouble mean,
   const gdouble min,
   const gdouble max)
{
  g_return_if_fail(gslider != NULL);
  gslider->picker[0] = gslider->scale_callback((GtkWidget *)gslider,
                                               mean, GRADIENT_SLIDER_SET);
  gslider->picker[1] = gslider->scale_callback((GtkWidget *)gslider,
                                               min, GRADIENT_SLIDER_SET);
  gslider->picker[2] = gslider->scale_callback((GtkWidget *)gslider,
                                               max, GRADIENT_SLIDER_SET);
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}

gboolean dtgtk_gradient_slider_multivalue_is_dragging
  (GtkDarktableGradientSlider *gslider)
{
  g_return_val_if_fail(gslider != NULL, FALSE);
  return gslider->is_dragging;
}

void dtgtk_gradient_slider_multivalue_set_increment
  (GtkDarktableGradientSlider *gslider,
   const gdouble value)
{
  g_return_if_fail(gslider != NULL);
  gslider->increment = value;
}

void dtgtk_gradient_slider_multivalue_set_scale_callback
  (GtkDarktableGradientSlider *gslider,
   float (*callback)(GtkWidget *self, float value, int dir))
{
  float (*old_callback)(GtkWidget*, float, int) = gslider->scale_callback;
  float (*new_callback)(GtkWidget*, float, int) = (callback == NULL ? _default_linear_scale_callback : callback);
  GtkWidget *self = (GtkWidget *)gslider;

  if(old_callback == new_callback) return;

  for(int k = 0; k < gslider->positions; k++)
  {
    gslider->position[k] = new_callback(self, old_callback(self, gslider->position[k], GRADIENT_SLIDER_GET), GRADIENT_SLIDER_SET);
    gslider->resetvalue[k] = new_callback(self, old_callback(self, gslider->resetvalue[k], GRADIENT_SLIDER_GET), GRADIENT_SLIDER_SET);
  }

  for(int k = 0; k < 3; k++)
  {
    gslider->picker[k] = new_callback(self, old_callback(self, gslider->picker[k], GRADIENT_SLIDER_GET), GRADIENT_SLIDER_SET);
  }

  for(GList *current = gslider->colors; current; current = g_list_next(current))
  {
    _gradient_slider_stop_t *stop = (_gradient_slider_stop_t *)current->data;
    stop->position = new_callback(self, old_callback(self, stop->position, GRADIENT_SLIDER_GET), GRADIENT_SLIDER_SET);
  }

  gslider->scale_callback = new_callback;
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}


// Public functions for single value type
GtkWidget *dtgtk_gradient_slider_new()
{
  GtkWidget *gslider = dtgtk_gradient_slider_multivalue_new(1);
  dt_gui_add_class(gslider, "dt_gslider");
  return gslider;
}

GtkWidget *dtgtk_gradient_slider_new_with_name(gchar *name)
{
  GtkWidget *widget = GTK_WIDGET(dtgtk_gradient_slider_new());
  if(name) gtk_widget_set_name(widget, name);

  return widget;
}

GtkWidget *dtgtk_gradient_slider_new_with_color(const GdkRGBA start,
                                                const GdkRGBA end)
{
  GtkWidget *gslider = dtgtk_gradient_slider_multivalue_new_with_color
    (start, end, 1);
  dt_gui_add_class(gslider, "dt_gslider");
  return gslider;
}

GtkWidget *dtgtk_gradient_slider_new_with_color_and_name(const GdkRGBA start,
                                                         const GdkRGBA end,
                                                         gchar *name)
{
  GtkWidget *widget = GTK_WIDGET(dtgtk_gradient_slider_new_with_color(start, end));
  if(name) gtk_widget_set_name(widget, name);

  return widget;
}

void dtgtk_gradient_slider_set_stop(GtkDarktableGradientSlider *gslider,
                                    const gfloat position,
                                    const GdkRGBA color)
{
  dtgtk_gradient_slider_multivalue_set_stop(gslider, position, color);
}

GType dtgtk_gradient_slider_get_type()
{
  return _gradient_slider_get_type();
}

gdouble dtgtk_gradient_slider_get_value(GtkDarktableGradientSlider *gslider)
{
  return dtgtk_gradient_slider_multivalue_get_value(gslider, 0);
}

void dtgtk_gradient_slider_set_value(GtkDarktableGradientSlider *gslider,
                                     const gdouble value)
{
  dtgtk_gradient_slider_multivalue_set_value(gslider, value, 0);
}

void dtgtk_gradient_slider_set_marker(GtkDarktableGradientSlider *gslider,
                                      const gint mark)
{
  dtgtk_gradient_slider_multivalue_set_marker(gslider, mark, 0);
}

void dtgtk_gradient_slider_set_resetvalue(GtkDarktableGradientSlider *gslider,
                                          const gdouble value)
{
  dtgtk_gradient_slider_multivalue_set_resetvalue(gslider, value, 0);
}

gdouble dtgtk_gradient_slider_get_resetvalue(GtkDarktableGradientSlider *gslider)
{
  return dtgtk_gradient_slider_multivalue_get_resetvalue(gslider, 0);
}

void dtgtk_gradient_slider_set_picker(GtkDarktableGradientSlider *gslider,
                                      const gdouble value)
{
  g_return_if_fail(gslider != NULL);
  gslider->picker[0] = gslider->picker[1] = gslider->picker[2]
    = gslider->scale_callback((GtkWidget *)gslider, value, GRADIENT_SLIDER_SET);
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}

void dtgtk_gradient_slider_set_picker_meanminmax
  (GtkDarktableGradientSlider *gslider,
   const gdouble mean,
   const gdouble min,
   const gdouble max)
{
  g_return_if_fail(gslider != NULL);
  gslider->picker[0] = gslider->scale_callback((GtkWidget *)gslider,
                                               mean, GRADIENT_SLIDER_SET);
  gslider->picker[1] = gslider->scale_callback((GtkWidget *)gslider,
                                               min, GRADIENT_SLIDER_SET);
  gslider->picker[2] = gslider->scale_callback((GtkWidget *)gslider,
                                               max, GRADIENT_SLIDER_SET);
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}

gboolean dtgtk_gradient_slider_is_dragging(GtkDarktableGradientSlider *gslider)
{
  g_return_val_if_fail(gslider != NULL, FALSE);
  return gslider->is_dragging;
}

void dtgtk_gradient_slider_set_increment(GtkDarktableGradientSlider *gslider,
                                         const gdouble value)
{
  g_return_if_fail(gslider != NULL);
  gslider->increment = value;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
