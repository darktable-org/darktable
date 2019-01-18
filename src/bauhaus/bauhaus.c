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

#include "bauhaus/bauhaus.h"
#include "common/calculator.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <math.h>
#include <pango/pangocairo.h>

G_DEFINE_TYPE(DtBauhausWidget, dt_bh, GTK_TYPE_DRAWING_AREA)

// fwd declare
static gboolean dt_bauhaus_popup_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static gboolean dt_bauhaus_popup_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static void dt_bauhaus_widget_accept(dt_bauhaus_widget_t *w);
static void dt_bauhaus_widget_reject(dt_bauhaus_widget_t *w);


static int get_line_space()
{
  return darktable.bauhaus->scale * darktable.bauhaus->line_space;
}

static int get_line_height()
{
  return darktable.bauhaus->scale * darktable.bauhaus->line_height;
}

static float get_marker_size()
{
  // will be fraction of the height, so doesn't depend on scale itself.
  return darktable.bauhaus->marker_size;
}

// TODO: remove / make use of the pango font size / X height
static float get_label_font_size()
{
  return get_line_height() * darktable.bauhaus->label_font_size;
}

static inline void set_color(cairo_t *cr, GdkRGBA color)
{
  cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
}

static int show_pango_text(cairo_t *cr, char *text, float x_pos, float y_pos, float max_width, gboolean right_aligned)
{
  PangoLayout *layout;

  layout = pango_cairo_create_layout(cr);

  if(max_width > 0)
  {
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_MIDDLE);
    pango_layout_set_width(layout, (int)(PANGO_SCALE * max_width + 0.5f));
  }

  if(text) {
    pango_layout_set_text(layout, text, -1);
  } else {
    // length of -1 is not allowed with NULL string (wtf)
    pango_layout_set_text(layout, NULL, 0);
  }

  pango_layout_set_font_description(layout, darktable.bauhaus->pango_font_desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);

  int pango_width, pango_height;
  pango_layout_get_size(layout, &pango_width, &pango_height);
  float text_width = ((double)pango_width/PANGO_SCALE);

  if(right_aligned) x_pos -= text_width;

  cairo_move_to(cr, x_pos, y_pos);
  pango_cairo_show_layout(cr, layout);
  g_object_unref(layout);

  return text_width;
}

// -------------------------------
static gboolean _cursor_timeout_callback(gpointer user_data)
{
  if(darktable.bauhaus->cursor_blink_counter > 0) darktable.bauhaus->cursor_blink_counter--;

  darktable.bauhaus->cursor_visible = !darktable.bauhaus->cursor_visible;
  gtk_widget_queue_draw(darktable.bauhaus->popup_area);

  if(darktable.bauhaus->cursor_blink_counter
     != 0) // this can be >0 when we haven't reached the desired number or -1 when blinking forever
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

static float get_slider_line_offset(float pos, float scale, float x, float y, float ht, const int width)
{
  // ht is in [0,1] scale here
  const float l = 4.0f / width;
  const float r = 1.0f - 4.0f / width - ht;

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

    // x = y^2 * .5(1+off/scale) + (1-y^2)*(l + (pos+off)*(r-l))
    // now find off given pos, y, and x:
    // x - y^2*.5 - (1-y^2)*pos = y^2*.5f*off/scale + (1-y^2)off
    //                          = off ((.5f/scale-1)*y^2 + 1)
    // return (x - y*y*.5f - (1.0f-y*y)*pos)/((.5f/scale-1.f)*y*y + 1.0f);

    offset = (x - y * y * .5f - (1.0f - y * y) * (l + pos * (r - l)))
             / (.5f * y * y / scale + (1.0f - y * y) * (r - l));
  }
  // clamp to result in a [0,1] range:
  if(pos + offset > 1.0f) offset = 1.0f - pos;
  if(pos + offset < 0.0f) offset = -pos;
  return offset;
}

// draw a loupe guideline for the quadratic zoom in in the slider interface:
static void draw_slider_line(cairo_t *cr, float pos, float off, float scale, const int width,
                             const int height, const int ht)
{
  // pos is normalized position [0,1], offset is on that scale.
  // ht is in pixels here
  const float l = 4.0f / width;
  const float r = 1.0f - (ht + 4.0f) / width;

  const int steps = 64;
  cairo_move_to(cr, width * (l + (pos + off) * (r - l)), ht * .7f);
  cairo_line_to(cr, width * (l + (pos + off) * (r - l)), ht);
  for(int j = 1; j < steps; j++)
  {
    const float y = j / (steps - 1.0f);
    const float x = y * y * .5f * (1.f + off / scale) + (1.0f - y * y) * (l + (pos + off) * (r - l));
    cairo_line_to(cr, x * width, ht + y * (height - ht));
  }
}
// -------------------------------

// handlers on the popup window, to close popup:
static gboolean dt_bauhaus_window_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  const float tol = 50;
  gint wx, wy;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  gdk_window_get_origin(gtk_widget_get_window(widget), &wx, &wy);
  if(event->x_root > wx + allocation.width + tol || event->y_root > wy + allocation.height + tol
     || event->x_root < (int)wx - tol || event->y_root < (int)wy - tol)
  {
    dt_bauhaus_widget_reject(darktable.bauhaus->current);
    dt_bauhaus_hide_popup();
    return TRUE;
  }
  // make sure to propagate the event further
  return FALSE;
}

static gboolean dt_bauhaus_window_button_press(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  const float tol = 0;
  gint wx, wy;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  gdk_window_get_origin(gtk_widget_get_window(widget), &wx, &wy);
  if((event->x_root > wx + allocation.width + tol || event->y_root > wy + allocation.height + tol
      || event->x_root < (int)wx - tol || event->y_root < (int)wy - tol))
  {
    dt_bauhaus_widget_reject(darktable.bauhaus->current);
    dt_bauhaus_hide_popup();
    return TRUE;
  }
  // make sure to propagate the event further
  return FALSE;
}

static void combobox_popup_scroll(int amt)
{
  gint wx, wy;
  GtkWidget *w = GTK_WIDGET(darktable.bauhaus->current);
  GtkAllocation allocation_w;
  gtk_widget_get_allocation(w, &allocation_w);
  const int ht = allocation_w.height;
  const int skip = ht + get_line_space();
  gdk_window_get_origin(gtk_widget_get_window(w), &wx, &wy);
  dt_bauhaus_combobox_data_t *d = &darktable.bauhaus->current->data.combobox;
  const int new_value = CLAMP(d->active + amt, 0, d->num_labels - 1);

  // we move the popup up or down
  if(new_value == d->active)
  {
    gdk_window_move(gtk_widget_get_window(darktable.bauhaus->popup_window), wx, wy - d->active * skip);
  }
  else
  {
    gint px, py;
    gdk_window_get_origin(gtk_widget_get_window(darktable.bauhaus->popup_window), &px, &py);
    gdk_window_move(gtk_widget_get_window(darktable.bauhaus->popup_window), wx, py - skip * amt);
  }

  // make sure highlighted entry is updated:
  darktable.bauhaus->mouse_x = 0;
  darktable.bauhaus->mouse_y = new_value * skip + ht / 2;
  gtk_widget_queue_draw(darktable.bauhaus->popup_area);

  // and we change the value
  dt_bauhaus_combobox_set(w, new_value);
}


static gboolean dt_bauhaus_popup_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  int delta_y;
  switch(darktable.bauhaus->current->type)
  {
    case DT_BAUHAUS_COMBOBOX:
      if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
         combobox_popup_scroll(delta_y);
      break;
    case DT_BAUHAUS_SLIDER:
      break;
    default:
      break;
  }
  return TRUE;
}

static gboolean dt_bauhaus_popup_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  GtkAllocation allocation_popup_window;
  gtk_widget_get_allocation(darktable.bauhaus->popup_window, &allocation_popup_window);
  gtk_widget_queue_draw(darktable.bauhaus->popup_area);
  dt_bauhaus_widget_t *w = darktable.bauhaus->current;
  GtkAllocation allocation_w;
  gtk_widget_get_allocation(GTK_WIDGET(w), &allocation_w);
  int width = allocation_popup_window.width, height = allocation_popup_window.height;
  // coordinate transform is in vain because we're only ever called after a button release.
  // that means the system is always the one of the popup.
  // that also means that we can't have hovering combobox entries while still holding the button. :(
  const float ex = event->x;
  const float ey = event->y;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  if(darktable.bauhaus->keys_cnt == 0) _stop_cursor();

  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
      darktable.bauhaus->mouse_x = ex;
      darktable.bauhaus->mouse_y = ey;
      break;
    case DT_BAUHAUS_SLIDER:
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      const float mouse_off = get_slider_line_offset(d->oldpos, d->scale, ex / width, ey / height,
                                                     allocation_w.height / (float)height, allocation.width);
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
    }
    break;
    default:
      break;
  }
  // throttling using motion hint:
  // gdk_event_request_motions(event);
  return TRUE;
}

static gboolean dt_bauhaus_popup_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
// all that doesn't seem to work (more events are created than necessary, exits at random
// during popup already etc.
#if 0
  // if(event->x > allocation.width + 20 || event->y > allocation.height + 20 ||
  // event->x < -20 || event->y < -20)
  if(!event->state)
  {
    dt_bauhaus_widget_reject(darktable.bauhaus->current);
    dt_bauhaus_hide_popup();
  }
#endif
  return TRUE;
}

static gboolean dt_bauhaus_popup_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(darktable.bauhaus->current && (darktable.bauhaus->current->type == DT_BAUHAUS_COMBOBOX)
     && (event->button == 1) &&                                // only accept left mouse click
     (dt_get_wtime() - darktable.bauhaus->opentime >= 0.250f)) // default gtk timeout for double-clicks
  {
    // event might be in wrong system, transform ourselves:
    gint wx, wy, x, y;
    gdk_window_get_origin(gtk_widget_get_window(darktable.bauhaus->popup_window), &wx, &wy);

    gdk_device_get_position(
#if GTK_CHECK_VERSION(3, 20, 0)
        gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(widget))), 0, &x, &y);
#else
        gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gdk_display_get_default())), NULL, &x,
        &y);
#endif
    darktable.bauhaus->end_mouse_x = x - wx;
    darktable.bauhaus->end_mouse_y = y - wy;
    dt_bauhaus_widget_accept(darktable.bauhaus->current);
  }
  dt_bauhaus_hide_popup();
  return TRUE;
}

static gboolean dt_bauhaus_popup_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    if(darktable.bauhaus->current->type == DT_BAUHAUS_COMBOBOX
       && dt_get_wtime() - darktable.bauhaus->opentime < 0.250f) // default gtk timeout for double-clicks
    {
      // counts as double click, reset:
      dt_bauhaus_combobox_data_t *d = &darktable.bauhaus->current->data.combobox;
      dt_bauhaus_combobox_set(GTK_WIDGET(darktable.bauhaus->current), d->defpos);
      dt_bauhaus_widget_reject(darktable.bauhaus->current);
    }
    else
    {
      // only accept left mouse click
      darktable.bauhaus->end_mouse_x = event->x;
      darktable.bauhaus->end_mouse_y = event->y;
      dt_bauhaus_widget_accept(darktable.bauhaus->current);
    }
  }
  else
  {
    dt_bauhaus_widget_reject(darktable.bauhaus->current);
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
  gtk_grab_add(w);
}

static void dt_bh_init(DtBauhausWidget *class)
{
  // not sure if we want to use this instead of our code in *_new()
  // TODO: the common code from bauhaus_widget_init() could go here.
}

static void dt_bh_class_init(DtBauhausWidgetClass *class)
{
  darktable.bauhaus->signals[DT_BAUHAUS_VALUE_CHANGED_SIGNAL]
      = g_signal_new("value-changed", G_TYPE_FROM_CLASS(class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                     g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
  darktable.bauhaus->signals[DT_BAUHAUS_QUAD_PRESSED_SIGNAL]
      = g_signal_new("quad-pressed", G_TYPE_FROM_CLASS(class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                     g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  // TODO: could init callbacks once per class for more efficiency:
  // GtkWidgetClass *widget_class;
  // widget_class = GTK_WIDGET_CLASS (class);
  // widget_class->draw = dt_bauhaus_draw;
}

void dt_bauhaus_init()
{
  darktable.bauhaus = (dt_bauhaus_t *)calloc(1, sizeof(dt_bauhaus_t));
  darktable.bauhaus->keys_cnt = 0;
  darktable.bauhaus->current = NULL;
  darktable.bauhaus->popup_area = gtk_drawing_area_new();
  gtk_widget_set_name(darktable.bauhaus->popup_area, "bauhaus-popup");

  darktable.bauhaus->line_space = 2;
  darktable.bauhaus->line_height = 11;
  darktable.bauhaus->marker_size = 0.3f;
  darktable.bauhaus->label_font_size = 0.6f;
  darktable.bauhaus->value_font_size = 0.6f;
  g_strlcpy(darktable.bauhaus->label_font, "sans", sizeof(darktable.bauhaus->label_font));
  g_strlcpy(darktable.bauhaus->value_font, "sans", sizeof(darktable.bauhaus->value_font));

  GtkWidget *root_window = dt_ui_main_window(darktable.gui->ui);
  GtkStyleContext *ctx = gtk_style_context_new();
  GtkWidgetPath *path;
  path = gtk_widget_path_new ();
  int pos = gtk_widget_path_append_type(path, GTK_TYPE_WIDGET);
  gtk_widget_path_iter_set_name(path, pos, "iop-plugin-ui");
  gtk_style_context_set_path(ctx, path);
  gtk_style_context_set_screen (ctx, gtk_widget_get_screen(root_window));

  if(!gtk_style_context_lookup_color(ctx, "bauhaus_fg", &darktable.bauhaus->color_fg))
  {
    darktable.bauhaus->color_fg.red = 0x99 / 255.0;
    darktable.bauhaus->color_fg.green = 0x99 / 255.0;
    darktable.bauhaus->color_fg.blue = 0x99 / 255.0;
    darktable.bauhaus->color_fg.alpha = 1.0;
  }
  if(!gtk_style_context_lookup_color(ctx, "bauhaus_fg_insensitive", &darktable.bauhaus->color_fg_insensitive))
  {
    darktable.bauhaus->color_fg_insensitive.red = 0x19 / 255.0;
    darktable.bauhaus->color_fg_insensitive.green = 0x19 / 255.0;
    darktable.bauhaus->color_fg_insensitive.blue = 0x19 / 255.0;
    darktable.bauhaus->color_fg_insensitive.alpha = .5;
  }
  if(!gtk_style_context_lookup_color(ctx, "bauhaus_bg", &darktable.bauhaus->color_bg))
  {
    darktable.bauhaus->color_bg.red = 0x99 / 255.0;
    darktable.bauhaus->color_bg.green = 0x99 / 255.0;
    darktable.bauhaus->color_bg.blue = 0x99 / 255.0;
    darktable.bauhaus->color_bg.alpha = .2;
  }
  if(!gtk_style_context_lookup_color(ctx, "bauhaus_border", &darktable.bauhaus->color_border))
  {
    darktable.bauhaus->color_border.red = 0x19 / 255.0;
    darktable.bauhaus->color_border.green = 0x19 / 255.0;
    darktable.bauhaus->color_border.blue = 0x19 / 255.0;
    darktable.bauhaus->color_border.alpha = 1.0;
  }


  PangoFontDescription *pfont = 0;
  gtk_style_context_get(ctx, GTK_STATE_FLAG_NORMAL, "font", &pfont, NULL);
  gtk_widget_path_free(path);

  darktable.bauhaus->pango_font_desc = pfont;

  PangoLayout *layout;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 128, 128);
  cairo_t *cr = cairo_create(cst);
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_text(layout, "X", -1);
  pango_layout_set_font_description(layout, darktable.bauhaus->pango_font_desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
  int pango_width, pango_height;
  pango_layout_get_size(layout, &pango_width, &pango_height);
  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_surface_destroy(cst);

  darktable.bauhaus->scale = (pango_height + 0.0f) / PANGO_SCALE / 8.5f;
  darktable.bauhaus->widget_space = 2.5f * darktable.bauhaus->scale;

  // keys are freed with g_free, values are ptrs to the widgets, these don't need to be cleaned up.
  darktable.bauhaus->keymap = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  darktable.bauhaus->key_mod = NULL;
  darktable.bauhaus->key_val = NULL;
  memset(darktable.bauhaus->key_history, 0, sizeof(darktable.bauhaus->key_history));

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
  dt_gui_key_accel_block_on_focus_connect(darktable.bauhaus->popup_area);

  gtk_widget_set_size_request(darktable.bauhaus->popup_area, DT_PIXEL_APPLY_DPI(300), DT_PIXEL_APPLY_DPI(300));
  gtk_window_set_resizable(GTK_WINDOW(darktable.bauhaus->popup_window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(darktable.bauhaus->popup_window), 260, 260);
  // gtk_window_set_modal(GTK_WINDOW(c->popup_window), TRUE);
  // gtk_window_set_decorated(GTK_WINDOW(c->popup_window), FALSE);

  // for pie menu:
  // gtk_window_set_position(GTK_WINDOW(c->popup_window), GTK_WIN_POS_MOUSE);// | GTK_WIN_POS_CENTER);

  // gtk_window_set_keep_above isn't enough on OS X
  gtk_window_set_transient_for(GTK_WINDOW(darktable.bauhaus->popup_window),
                               GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_container_add(GTK_CONTAINER(darktable.bauhaus->popup_window), darktable.bauhaus->popup_area);
  // gtk_window_set_title(GTK_WINDOW(c->popup_window), _("dtgtk control popup"));
  gtk_window_set_keep_above(GTK_WINDOW(darktable.bauhaus->popup_window), TRUE);
  gtk_window_set_gravity(GTK_WINDOW(darktable.bauhaus->popup_window), GDK_GRAVITY_STATIC);

  gtk_widget_set_can_focus(darktable.bauhaus->popup_area, TRUE);
  gtk_widget_add_events(darktable.bauhaus->popup_area, GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                                       | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                       | GDK_KEY_PRESS_MASK | GDK_LEAVE_NOTIFY_MASK
                                                       | darktable.gui->scroll_mask);

  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_window), "show", G_CALLBACK(dt_bauhaus_window_show), (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_area), "draw", G_CALLBACK(dt_bauhaus_popup_draw),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_window), "motion-notify-event",
                   G_CALLBACK(dt_bauhaus_window_motion_notify), (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_window), "button-press-event", G_CALLBACK(dt_bauhaus_window_button_press),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_area), "motion-notify-event",
                   G_CALLBACK(dt_bauhaus_popup_motion_notify), (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_area), "leave-notify-event",
                   G_CALLBACK(dt_bauhaus_popup_leave_notify), (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_area), "button-press-event",
                   G_CALLBACK(dt_bauhaus_popup_button_press), (gpointer)NULL);
  // this is connected to the widget itself, not the popup. we're only interested
  // in mouse release events that are initiated by a press on the original widget.
  g_signal_connect (G_OBJECT (darktable.bauhaus->popup_area), "button-release-event",
                    G_CALLBACK (dt_bauhaus_popup_button_release), (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_area), "key-press-event",
                   G_CALLBACK(dt_bauhaus_popup_key_press), (gpointer)NULL);
  g_signal_connect(G_OBJECT(darktable.bauhaus->popup_area), "scroll-event",
                   G_CALLBACK(dt_bauhaus_popup_scroll), (gpointer)NULL);
}

void dt_bauhaus_cleanup()
{
  // TODO: destroy popup window and resources
  // TODO: destroy keymap hash table!
  g_list_free_full(darktable.bauhaus->key_mod, (GDestroyNotify)g_free);
  g_list_free_full(darktable.bauhaus->key_val, (GDestroyNotify)g_free);
}

// fwd declare a few callbacks
static gboolean dt_bauhaus_slider_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_bauhaus_slider_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_bauhaus_slider_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
static gboolean dt_bauhaus_slider_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static gboolean dt_bauhaus_slider_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);


static gboolean dt_bauhaus_combobox_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_bauhaus_combobox_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
static gboolean dt_bauhaus_combobox_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);

// static gboolean
// dt_bauhaus_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_bauhaus_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data);


// end static init/cleanup
// =================================================



// common initialization
static void dt_bauhaus_widget_init(dt_bauhaus_widget_t *w, dt_iop_module_t *self)
{
  w->module = self;

  // no quad icon and no toggle button:
  w->quad_paint = 0;
  w->quad_paint_data = NULL;
  w->quad_toggle = 0;
  w->combo_populate = NULL;
  gtk_widget_set_size_request(GTK_WIDGET(w), -1, get_line_height());

  gtk_widget_add_events(GTK_WIDGET(w), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                       | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                       | GDK_LEAVE_NOTIFY_MASK | darktable.gui->scroll_mask);

  g_signal_connect(G_OBJECT(w), "draw", G_CALLBACK(dt_bauhaus_draw), NULL);

  // for combobox, where mouse-release triggers a selection, we need to catch this
  // event where the mouse-press occurred, which will be this widget. we just pass
  // it on though:
  // g_signal_connect (G_OBJECT (w), "button-release-event",
  //                   G_CALLBACK (dt_bauhaus_popup_button_release), (gpointer)NULL);
}

void dt_bauhaus_combobox_set_default(GtkWidget *widget, int def)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->defpos = def;
}

void dt_bauhaus_slider_set_hard_min(GtkWidget* widget, float val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float pos = dt_bauhaus_slider_get(widget);
  float rawval = d->callback(widget, val, DT_BAUHAUS_SET);
  d->hard_min = rawval;
  d->min = MAX(d->min, d->hard_min);
  d->soft_min = MAX(d->soft_min, d->hard_min);
  if(rawval > d->hard_max) dt_bauhaus_slider_set_hard_max(widget,val);
  if(pos < val)
  {
    dt_bauhaus_slider_set_soft(widget,val);
  }
  else
  {
    dt_bauhaus_slider_set_soft(widget,pos);
  }
}

float dt_bauhaus_slider_get_hard_min(GtkWidget* widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->callback(widget, d->hard_min, DT_BAUHAUS_GET);
}

void dt_bauhaus_slider_set_hard_max(GtkWidget* widget, float val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float pos = dt_bauhaus_slider_get(widget);
  float rawval = d->callback(widget, val, DT_BAUHAUS_SET);
  d->hard_max = rawval;
  d->max = MIN(d->max, d->hard_max);
  d->soft_max = MIN(d->soft_max, d->hard_max);
  if(rawval < d->hard_min) dt_bauhaus_slider_set_hard_min(widget,val);
  if(pos > val) {

    dt_bauhaus_slider_set_soft(widget,val);
  }
  else
  {
    dt_bauhaus_slider_set_soft(widget,pos);
  }
}

float dt_bauhaus_slider_get_hard_max(GtkWidget* widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->callback(widget, d->hard_max, DT_BAUHAUS_GET);
}

void dt_bauhaus_slider_set_soft_min(GtkWidget* widget, float val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float pos = dt_bauhaus_slider_get(widget);
  float rawval = d->callback(widget, val, DT_BAUHAUS_SET);
  d->soft_min = rawval;
  d->hard_min = MIN(d->hard_min,d->soft_min);
  d->min =  d->soft_min;
  if(rawval > d->soft_max) dt_bauhaus_slider_set_soft_max(widget,val);
  if(rawval > d->hard_max) dt_bauhaus_slider_set_hard_max(widget,val);
  if(pos < val)
  {
    dt_bauhaus_slider_set_soft(widget,val);
  }
  else
  {
    dt_bauhaus_slider_set_soft(widget,pos);
  }
}

float dt_bauhaus_slider_get_soft_min(GtkWidget* widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->callback(widget, d->soft_min, DT_BAUHAUS_GET);
}

void dt_bauhaus_slider_set_soft_max(GtkWidget* widget, float val)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float pos = dt_bauhaus_slider_get(widget);
  float rawval = d->callback(widget, val, DT_BAUHAUS_SET);
  d->soft_max = rawval;
  d->hard_max = MAX(d->soft_max, d->hard_max);
  d->max =  d->soft_max;
  if(rawval < d->soft_min) dt_bauhaus_slider_set_soft_min(widget,val);
  if(rawval < d->hard_min) dt_bauhaus_slider_set_hard_min(widget,val);
  if(pos > val) {
    dt_bauhaus_slider_set_soft(widget,val);
  } else {
    dt_bauhaus_slider_set_soft(widget,pos);
  }
}

float dt_bauhaus_slider_get_soft_max(GtkWidget* widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  return d->callback(widget, d->soft_max, DT_BAUHAUS_GET);
}

void dt_bauhaus_slider_set_default(GtkWidget *widget, float def)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float val = d->callback(widget, def, DT_BAUHAUS_SET);
  d->defpos = (val - d->min) / (d->max - d->min);
}

void dt_bauhaus_slider_enable_soft_boundaries(GtkWidget *widget, float hard_min, float hard_max)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->hard_min = d->callback(widget, hard_min, DT_BAUHAUS_SET);
  d->hard_max = d->callback(widget, hard_max, DT_BAUHAUS_SET);
}

void dt_bauhaus_widget_set_label(GtkWidget *widget, const char *section, const char *label)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  memset(w->label, 0, sizeof(w->label)); // keep valgrind happy
  g_strlcpy(w->label, label, sizeof(w->label));

  if(w->module)
  {
    // construct control path name and insert into keymap:
    gchar *path;
    if(section && section[0] != '\0')
    {
      path = g_strdup_printf("%s.%s.%s", w->module->name(), section, w->label);
      gchar *section_path = g_strdup_printf("%s.%s", w->module->name(), section);
      if(!g_list_find_custom(darktable.bauhaus->key_val, section_path, (GCompareFunc)strcmp))
        darktable.bauhaus->key_val
            = g_list_insert_sorted(darktable.bauhaus->key_val, section_path, (GCompareFunc)strcmp);
      else
        g_free(section_path);
    }
    else
      path = g_strdup_printf("%s.%s", w->module->name(), w->label);
    if(!g_hash_table_lookup(darktable.bauhaus->keymap, path))
    {
      // also insert into sorted tab-complete list.
      // (but only if this is the first time we insert this path)
      gchar *mod = g_strdup(path);
      gchar *val = g_strstr_len(mod, strlen(mod), ".");
      if(val)
      {
        *val = 0;
        if(!g_list_find_custom(darktable.bauhaus->key_mod, mod, (GCompareFunc)strcmp))
          darktable.bauhaus->key_mod
              = g_list_insert_sorted(darktable.bauhaus->key_mod, mod, (GCompareFunc)strcmp);
        else
          g_free(mod);

        // unfortunately need our own string, as replace in the hashtable below might destroy this pointer.
        darktable.bauhaus->key_val
            = g_list_insert_sorted(darktable.bauhaus->key_val, g_strdup(path), (GCompareFunc)strcmp);
      }
    }
    // might free an old path
    g_hash_table_replace(darktable.bauhaus->keymap, path, w);
    gtk_widget_queue_draw(GTK_WIDGET(w));
  }
}

const char* dt_bauhaus_widget_get_label(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  return w->label;
}

void dt_bauhaus_widget_set_quad_paint(GtkWidget *widget, dt_bauhaus_quad_paint_f f, int paint_flags, void *paint_data)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  w->quad_paint = f;
  w->quad_paint_flags = paint_flags;
  w->quad_paint_data = paint_data;
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
  if (active)
    w->quad_paint_flags |= CPF_ACTIVE;
  else
    w->quad_paint_flags &= ~CPF_ACTIVE;
  gtk_widget_queue_draw(GTK_WIDGET(w));
}

int dt_bauhaus_widget_get_quad_active(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  return (w->quad_paint_flags & CPF_ACTIVE) == CPF_ACTIVE;
}

static float _default_linear_callback(GtkWidget *self, float value, dt_bauhaus_callback_t dir)
{
  // regardless of dir: input <-> output
  return value;
}

static void dt_bauhaus_slider_destroy(dt_bauhaus_widget_t *widget, gpointer user_data)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  if(d->timeout_handle) g_source_remove(d->timeout_handle);
  d->timeout_handle = 0;
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

GtkWidget *dt_bauhaus_slider_new_with_range_and_feedback(dt_iop_module_t *self, float min, float max,
                                                         float step, float defval, int digits, int feedback)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(g_object_new(DT_BAUHAUS_WIDGET_TYPE, NULL));
  return dt_bauhaus_slider_from_widget(w,self, min, max, step, defval, digits, feedback);
}
GtkWidget *dt_bauhaus_slider_from_widget(dt_bauhaus_widget_t* w,dt_iop_module_t *self, float min, float max,
                                                         float step, float defval, int digits, int feedback)
{
  w->type = DT_BAUHAUS_SLIDER;
  dt_bauhaus_widget_init(w, self);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->min = d->soft_min = d->hard_min = min;
  d->max = d->soft_max = d->hard_max = max;
  d->step = step;
  // normalize default:
  d->defpos = (defval - min) / (max - min);
  d->pos = d->defpos;
  d->oldpos = d->defpos;
  d->scale = 5.0f * step / (max - min);
  d->digits = digits;
  snprintf(d->format, sizeof(d->format), "%%.0%df", digits);

  d->grad_cnt = 0;

  d->fill_feedback = feedback;

  d->is_dragging = 0;
  d->is_changed = 0;
  d->timeout_handle = 0;
  d->callback = _default_linear_callback;

  gtk_widget_add_events(GTK_WIDGET(w), GDK_KEY_PRESS_MASK);
  gtk_widget_set_can_focus(GTK_WIDGET(w), TRUE);

  g_signal_connect(G_OBJECT(w), "button-press-event", G_CALLBACK(dt_bauhaus_slider_button_press),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "button-release-event", G_CALLBACK(dt_bauhaus_slider_button_release),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "scroll-event", G_CALLBACK(dt_bauhaus_slider_scroll), (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "key-press-event", G_CALLBACK(dt_bauhaus_slider_key_press), (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "motion-notify-event", G_CALLBACK(dt_bauhaus_slider_motion_notify),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "destroy", G_CALLBACK(dt_bauhaus_slider_destroy), (gpointer)NULL);
  return GTK_WIDGET(w);
}

static void dt_bauhaus_combobox_destroy(dt_bauhaus_widget_t *widget, gpointer user_data)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  g_list_free_full(d->labels, g_free);
  d->labels = NULL;
  g_list_free(d->alignments);
  d->alignments = NULL;
  d->num_labels = 0;
  if(d->free_func)
    g_list_free_full(d->data, d->free_func);
  else
    g_list_free(d->data);
  d->data = NULL;
}

GtkWidget *dt_bauhaus_combobox_new(dt_iop_module_t *self)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(g_object_new(DT_BAUHAUS_WIDGET_TYPE, NULL));
  dt_bauhaus_combobox_from_widget(w,self);
  return GTK_WIDGET(w);
}

void dt_bauhaus_combobox_from_widget(dt_bauhaus_widget_t* w,dt_iop_module_t *self)
{
  w->type = DT_BAUHAUS_COMBOBOX;
  dt_bauhaus_widget_init(w, self);
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->labels = NULL;
  d->alignments = NULL;
  d->num_labels = 0;
  d->defpos = 0;
  d->active = d->defpos;
  d->editable = 0;
  d->data = NULL;
  memset(d->text, 0, sizeof(d->text));

  gtk_widget_add_events(GTK_WIDGET(w), GDK_KEY_PRESS_MASK);
  gtk_widget_set_can_focus(GTK_WIDGET(w), TRUE);

  g_signal_connect(G_OBJECT(w), "button-press-event", G_CALLBACK(dt_bauhaus_combobox_button_press),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "button-release-event", G_CALLBACK(dt_bauhaus_popup_button_release),
                   (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "scroll-event", G_CALLBACK(dt_bauhaus_combobox_scroll), (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "key-press-event", G_CALLBACK(dt_bauhaus_combobox_key_press), (gpointer)NULL);
  g_signal_connect(G_OBJECT(w), "destroy", G_CALLBACK(dt_bauhaus_combobox_destroy), (gpointer)NULL);
}

void dt_bauhaus_combobox_add_populate_fct(GtkWidget *widget, void (*fct)(GtkWidget *w, struct dt_iop_module_t **module))
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  w->combo_populate = fct;
}

void dt_bauhaus_combobox_add(GtkWidget *widget, const char *text)
{
  dt_bauhaus_combobox_add_full(widget, text, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, NULL, NULL);
}

void dt_bauhaus_combobox_add_aligned(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align)
{
  dt_bauhaus_combobox_add_full(widget, text, align, NULL, NULL);
}

void dt_bauhaus_combobox_add_full(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align,
                                  gpointer data, void (free_func)(void *data))
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->num_labels++;
  d->labels = g_list_append(d->labels, g_strdup(text));
  d->alignments = g_list_append(d->alignments, GINT_TO_POINTER(align));
  d->data = g_list_append(d->data, data);
  d->free_func = free_func;
}

void dt_bauhaus_combobox_set_editable(GtkWidget *widget, int editable)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->editable = editable ? 1 : 0;
}

int dt_bauhaus_combobox_get_editable(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return 0;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  return d->editable;
}

void dt_bauhaus_combobox_remove_at(GtkWidget *widget, int pos)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  if(pos < 0 || pos >= d->num_labels) return;

  GList *rm = g_list_nth(d->labels, pos);
  g_free(rm->data);
  d->labels = g_list_delete_link(d->labels, rm);

  rm = g_list_nth(d->alignments, pos);
  d->alignments = g_list_delete_link(d->alignments, rm);

  rm = g_list_nth(d->data, pos);
  if(d->free_func) d->free_func(rm->data);
  d->data = g_list_delete_link(d->data, rm);

  d->num_labels--;
}

void dt_bauhaus_combobox_insert(GtkWidget *widget, const char *text,int pos)
{
  dt_bauhaus_combobox_insert_full(widget, text, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, NULL, pos);
}

void dt_bauhaus_combobox_insert_full(GtkWidget *widget, const char *text, dt_bauhaus_combobox_alignment_t align,
                                     gpointer data, int pos)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->num_labels++;
  d->labels = g_list_insert(d->labels, g_strdup(text), pos);
  d->alignments = g_list_insert(d->alignments, GINT_TO_POINTER(align), pos);
  d->data = g_list_insert(d->data, data, pos);
}

int dt_bauhaus_combobox_length(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return 0;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  return d->num_labels;
}

const char *dt_bauhaus_combobox_get_text(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return NULL;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;

  if(d->editable && d->active < 0)
  {
    return d->text;
  }
  else
  {
    if(d->active < 0 || d->active >= d->num_labels) return NULL;
    return (const char *)g_list_nth_data(d->labels, d->active);
  }
  return NULL;
}

gpointer dt_bauhaus_combobox_get_data(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return NULL;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  return g_list_nth_data(d->data, d->active);
}

void dt_bauhaus_combobox_clear(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->active = 0;
  g_list_free_full(d->labels, g_free);
  d->labels = NULL;
  g_list_free(d->alignments);
  d->alignments = NULL;
  if(d->free_func)
    g_list_free_full(d->data, d->free_func);
  else
    g_list_free(d->data);
  d->data = NULL;
  d->num_labels = 0;
}

const GList *dt_bauhaus_combobox_get_labels(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return 0;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  return d->labels;
}

void dt_bauhaus_combobox_set_text(GtkWidget *widget, const char *text)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  if(!d->editable) return;
  g_strlcpy(d->text, text, sizeof(d->text));
}

void dt_bauhaus_combobox_set(GtkWidget *widget, int pos)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  d->active = CLAMP(pos, -1, d->num_labels - 1);
  gtk_widget_queue_draw(GTK_WIDGET(w));
  if(!darktable.gui->reset) g_signal_emit_by_name(G_OBJECT(w), "value-changed");
}

int dt_bauhaus_combobox_get(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_COMBOBOX) return -1;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  return d->active;
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
  float rawstop = d->callback(widget, stop, DT_BAUHAUS_SET);
  // need to replace stop?
  for(int k = 0; k < d->grad_cnt; k++)
  {
    if(d->grad_pos[k] == rawstop)
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
    d->grad_pos[k] = rawstop;
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

static void dt_bauhaus_draw_indicator(dt_bauhaus_widget_t *w, float pos, cairo_t *cr)
{
  // draw scale indicator (the tiny triangle)
  GtkWidget *widget = GTK_WIDGET(w);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  const int wd = allocation.width;
  const int ht = allocation.height;
  cairo_save(cr);

  const float l = 4.0f / wd;
  const float r = 1.0f - (ht + 4.0f) / wd;
  set_color(cr, darktable.bauhaus->color_fg);
  cairo_translate(cr, (l + pos * (r - l)) * wd,
                  get_line_height() * (darktable.bauhaus->label_font_size + 0.25f));
  cairo_scale(cr, 1.0f, -1.0f);
  draw_equilateral_triangle(cr, ht * get_marker_size());
  cairo_fill_preserve(cr);
  cairo_set_line_width(cr, 1.);
  set_color(cr, darktable.bauhaus->color_border);
  cairo_stroke(cr);
  cairo_restore(cr);
}

static void dt_bauhaus_draw_quad(dt_bauhaus_widget_t *w, cairo_t *cr)
{
  GtkWidget *widget = GTK_WIDGET(w);
  const gboolean sensitive = gtk_widget_is_sensitive(GTK_WIDGET(w));
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width;
  const int height = allocation.height;

  if(w->quad_paint)
  {
    const float font_size = get_label_font_size();
    cairo_save(cr);

    if(sensitive && (w->quad_paint_flags & CPF_ACTIVE))
      set_color(cr, darktable.bauhaus->color_fg);
    else
      set_color(cr, darktable.bauhaus->color_fg_insensitive);
    w->quad_paint(cr, width - height, 0, height, font_size, w->quad_paint_flags, w->quad_paint_data);
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
        cairo_translate(cr, width - height * .5f, get_label_font_size() * .5f);
        draw_equilateral_triangle(cr, height * get_marker_size());
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.);
        set_color(cr, darktable.bauhaus->color_border);
        cairo_stroke(cr);
        break;
      case DT_BAUHAUS_SLIDER:
        break;
      default:
        cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);
        cairo_rectangle(cr, width - height, 0, height, height);
        cairo_fill(cr);
        break;
    }
    cairo_restore(cr);
  }
}

static void dt_bauhaus_draw_baseline(dt_bauhaus_widget_t *w, cairo_t *cr)
{
  // draw line for orientation in slider
  GtkWidget *widget = GTK_WIDGET(w);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  const int wd = allocation.width;
  const int ht = allocation.height;
  cairo_save(cr);
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  // pos of baseline
  const float htm = ht * (darktable.bauhaus->label_font_size + 0.15f);
  const float htM = ht * 0.2f; // thickness of baseline

  // the background of the line
  cairo_pattern_t *gradient = NULL;
  if(d->grad_cnt > 0)
  {
    // gradient line as used in some modules
    gradient = cairo_pattern_create_linear(0, 0, wd - 4 - ht - 2, ht);
    for(int k = 0; k < d->grad_cnt; k++)
      cairo_pattern_add_color_stop_rgba(gradient, d->grad_pos[k], d->grad_col[k][0], d->grad_col[k][1],
                                        d->grad_col[k][2], 0.4f);
    cairo_set_source(cr, gradient);
  }
  else
  {
    // regular baseline
    set_color(cr, darktable.bauhaus->color_bg);
  }

  cairo_rectangle(cr, 2, htm, wd - 4 - ht - 2, htM);
  cairo_fill(cr);

  // have a `fill ratio feel'
  // - but only if set (default, but might not be good for 'balance' sliders)
  if(d->fill_feedback)
  {
    // only brighten, useful for colored sliders to not get too faint:
    cairo_set_operator(cr, CAIRO_OPERATOR_SCREEN);
    cairo_set_source_rgb(cr, .3, .3, .3);
    cairo_rectangle(cr, 2, htm, d->pos * (wd - 4 - ht - 2), htM);
    cairo_fill(cr);
    // change back to default cairo operator:
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  }

  cairo_rectangle(cr, 2, htm, wd - 4 - ht - 2, htM);
  cairo_set_line_width(cr, 1.);
  set_color(cr, darktable.bauhaus->color_border);
  cairo_stroke(cr);

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
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  GtkAllocation allocation_popup_window;
  gtk_widget_get_allocation(darktable.bauhaus->popup_window, &allocation_popup_window);
  int width = allocation_popup_window.width, height = allocation_popup_window.height;
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
    {
      // only set to what's in the filtered list.
      dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      int active = darktable.bauhaus->end_mouse_y >= 0
                       ? (darktable.bauhaus->end_mouse_y / (allocation.height + get_line_space()))
                       : d->active;
      GList *it = d->labels;
      int k = 0, i = 0, kk = 0, match = 1;

      gchar *keys = g_utf8_casefold(darktable.bauhaus->keys, -1);
      while(it)
      {
        gchar *text = (gchar *)it->data;
        gchar *text_cmp = g_utf8_casefold(text, -1);
        if(!strncmp(text_cmp, keys, darktable.bauhaus->keys_cnt))
        {
          if(active == k)
          {
            dt_bauhaus_combobox_set(widget, i);
            g_free(keys);
            g_free(text_cmp);
            return;
          }
          kk = i; // remember for down there
          // editable should only snap to perfect matches, not prefixes:
          if(d->editable && strcmp(text, darktable.bauhaus->keys)) match = 0;
          k++;
        }
        i++;
        it = g_list_next(it);
        g_free(text_cmp);
      }
      // if list is short (2 entries could be: typed something similar, and one similar)
      if(k < 3)
      {
        // didn't find it, but had only one matching choice?
        if(k == 1 && match)
          dt_bauhaus_combobox_set(widget, kk);
        else if(d->editable)
        {
          // had no close match (k == 1 && !match) or no match at all (k == 0)
          memset(d->text, 0, sizeof(d->text));
          g_strlcpy(d->text, darktable.bauhaus->keys, sizeof(d->text));
          // select custom entry
          dt_bauhaus_combobox_set(widget, -1);
        }
      }
      g_free(keys);
      break;
    }
    case DT_BAUHAUS_SLIDER:
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      const float mouse_off = get_slider_line_offset(
          d->oldpos, d->scale, darktable.bauhaus->end_mouse_x / width,
          darktable.bauhaus->end_mouse_y / height, allocation.height / (float)height, allocation.width);
      dt_bauhaus_slider_set_normalized(w, d->oldpos + mouse_off);
      d->oldpos = d->pos;
      break;
    }
    default:
      break;
  }
}

static gboolean dt_bauhaus_popup_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_bauhaus_widget_t *w = darktable.bauhaus->current;
  // dimensions of the popup
  int width = allocation.width, height = allocation.height;
  // dimensions of the original line
  GtkWidget *current = GTK_WIDGET(w);
  GtkAllocation allocation_current;
  gtk_widget_get_allocation(current, &allocation_current);
  int wd = allocation_current.width, ht = allocation_current.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_render_background(context, cr, 0.0, 0.0, width, height);

  GdkRGBA text_color, text_color_selected, text_color_hover;
  gtk_style_context_get_color(context, GTK_STATE_FLAG_NORMAL, &text_color);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_SELECTED, &text_color_selected);
  gtk_style_context_get_color(context, GTK_STATE_FLAG_PRELIGHT, &text_color_hover);

  // draw line around popup
  cairo_set_line_width(cr, 1.0);
  set_color(cr, darktable.bauhaus->color_border);
  if(w->type == DT_BAUHAUS_COMBOBOX)
  {
    // separate more clearly (looks terrible in a way but might help separate text
    // from other widgets above and below)
    cairo_move_to(cr, 1.0, height - 1.0);
    cairo_line_to(cr, width - 1.0, height - 1.0);
    cairo_line_to(cr, width - 1.0, 1.0);
    cairo_stroke(cr);
    GdkRGBA color = darktable.bauhaus->color_border;
    color.alpha = 0.4;
    set_color(cr, color);
    cairo_move_to(cr, 1.0, height - 1.0);
    cairo_line_to(cr, 1.0, 1.0);
    cairo_line_to(cr, width - 1.0, 1.0);
    cairo_stroke(cr);
  }
  else
  {
    // blend in
    cairo_move_to(cr, 1.0, 3.0 * ht);
    cairo_line_to(cr, 1.0, height - 1);
    cairo_line_to(cr, width - 1, height - 1);
    cairo_line_to(cr, width - 1, 3.0 * ht);
    cairo_stroke(cr);
    // fade in line around popup:
    for(int k = 0; k < 4; k++)
    {
      cairo_set_line_width(cr, (k + 1) / 4.0f);
      cairo_move_to(cr, 1.0, ht * (2.f + k / 4.0f));
      cairo_line_to(cr, 1.0, ht * (2.f + (k + 1) / 4.0f));
      cairo_move_to(cr, width - 1.0, ht * (2.f + k / 4.0f));
      cairo_line_to(cr, width - 1.0, ht * (2.f + (k + 1) / 4.0f));
      cairo_stroke(cr);
    }
  }

  // switch on bauhaus widget type (so we only need one static window)
  switch(w->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;

      dt_bauhaus_draw_baseline(w, cr);

      cairo_save(cr);
      cairo_set_line_width(cr, 1.);
      const int num_scales = 1.f / d->scale;
      // don't draw over orientation line
      cairo_rectangle(cr, 0.0f, 0.9 * ht, width, height);
      cairo_clip(cr);
      GdkRGBA color = darktable.bauhaus->color_border;
      for(int k = 0; k < num_scales; k++)
      {
        const float off = k * d->scale - d->oldpos;
        color.alpha = d->scale / fabsf(off);
        set_color(cr, color);
        draw_slider_line(cr, d->oldpos, off, d->scale, width, height, ht);
        cairo_stroke(cr);
      }
      cairo_restore(cr);
      set_color(cr, text_color);
      show_pango_text(cr, w->label, 2, 0, 0, FALSE);

      // draw mouse over indicator line
      cairo_save(cr);
      set_color(cr, darktable.bauhaus->color_fg);
      cairo_set_line_width(cr, 2.);
      const float mouse_off
          = darktable.bauhaus->change_active
                ? get_slider_line_offset(d->oldpos, d->scale, darktable.bauhaus->mouse_x / width,
                                         darktable.bauhaus->mouse_y / height, ht / (float)height, width)
                : 0.0f;
      draw_slider_line(cr, d->oldpos, mouse_off, d->scale, width, height, ht);
      cairo_stroke(cr);
      cairo_restore(cr);

      // draw indicator
      dt_bauhaus_draw_indicator(w, d->oldpos + mouse_off, cr);

      // draw numerical value:
      cairo_save(cr);
      char text[256];
      const float f = d->min + (d->oldpos + mouse_off) * (d->max - d->min);
      const float fc = d->callback(widget, f, DT_BAUHAUS_GET);
      snprintf(text, sizeof(text), d->format, fc);
      set_color(cr, text_color);
      show_pango_text(cr, text, wd - 4 - ht, 0, 0, TRUE);

      cairo_restore(cr);
    }
    break;
    case DT_BAUHAUS_COMBOBOX:
    {
      dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      cairo_save(cr);
      float first_label_width = 0.0;
      gboolean first_label = TRUE;
      int k = 0, i = 0;
      int hovered = darktable.bauhaus->mouse_y / (ht + get_line_space());
      gchar *keys = g_utf8_casefold(darktable.bauhaus->keys, -1);
      for(GList *it_label = d->labels, *it_align = d->alignments; it_label && it_align;
          it_label = g_list_next(it_label), it_align = g_list_next(it_align))
      {
        dt_bauhaus_combobox_alignment_t align = GPOINTER_TO_INT(it_align->data);
        gchar *text = (gchar *)it_label->data;
        gchar *text_cmp = g_utf8_casefold(text, -1);
        if(!strncmp(text_cmp, keys, darktable.bauhaus->keys_cnt))
        {
          float max_width = wd - 4 - ht;
          if(first_label) max_width *= 0.8; // give the label at least some room

          float label_width;
          if(i == hovered)
            set_color(cr, text_color_hover);
          else if(i == d->active)
            set_color(cr, text_color_selected);
          else
            set_color(cr, text_color);

          if(align == DT_BAUHAUS_COMBOBOX_ALIGN_LEFT)
            label_width = show_pango_text(cr, text, 2, (get_line_space() + ht) * k, max_width, FALSE);
          else
            label_width = show_pango_text(cr, text, wd - 4 - ht, (get_line_space() + ht) * k, max_width, TRUE);

          // prefer the entry over the label wrt. ellipsization when expanded
          if(first_label)
          {
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
      set_color(cr, text_color);
      show_pango_text(cr, w->label, 2, 0, wd - 8 - ht - first_label_width, FALSE);

      g_free(keys);
    }
    break;
    default:
      // yell
      break;
  }

  // draw currently typed text. if a type doesn't want this, it should not
  // allow stuff to be written here in the key callback.
  if(darktable.bauhaus->keys_cnt)
  {
    cairo_save(cr);
    PangoLayout *layout;
    PangoRectangle ink;
    layout = pango_cairo_create_layout(cr);
    pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
    set_color(cr, text_color);

    // make extra large, but without dependency on popup window height
    // (that might differ for comboboxes for example). only fall back
    // to height dependency if the popup is really small.
    const int line_height = get_line_height();
    const int size = MIN(3 * line_height, .2 * height);
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    pango_layout_set_text(layout, darktable.bauhaus->keys, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, wd - 4 - ht - ink.width, height * 0.5 - size);
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
    const int line_height = get_line_height();
    cairo_move_to(cr, wd - ht + 3, height * 0.5 + line_height);
    cairo_line_to(cr, wd - ht + 3, height * 0.5 - 3 * line_height);
    cairo_set_line_width(cr, 2.);
    cairo_stroke(cr);
    cairo_restore(cr);
  }

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  return TRUE;
}

static gboolean dt_bauhaus_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  GdkRGBA text_color;
  gtk_style_context_get_color(context, gtk_style_context_get_state(context), &text_color);

  gtk_render_background(context, cr, 0.0, 0.0, width, height);

  // draw type specific content:
  cairo_save(cr);
  cairo_set_line_width(cr, 1.0);
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
    {
      // draw label and quad area at right end
      set_color(cr, text_color);
      float label_width
          = show_pango_text(cr, w->label, 2, 0, 0, FALSE);
      dt_bauhaus_draw_quad(w, cr);

      dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      gchar *text = d->text;
      if(d->active >= 0) text = (gchar *)g_list_nth_data(d->labels, d->active);
      set_color(cr, text_color);
      show_pango_text(cr, text, width - 4 - height, 0, width - 4 - height - label_width - height, TRUE/*,
                      gtk_widget_is_sensitive(widget), FALSE*/);
      break;
    }
    case DT_BAUHAUS_SLIDER:
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;

      // line for orientation
      dt_bauhaus_draw_baseline(w, cr);
      dt_bauhaus_draw_quad(w, cr);

      if(gtk_widget_is_sensitive(widget))
      {
        dt_bauhaus_draw_indicator(w, d->pos, cr);

        // TODO: merge that text with combo
        char text[256];
        const float f = d->min + d->pos * (d->max - d->min);
        const float fc = d->callback(widget, f, DT_BAUHAUS_GET);
        snprintf(text, sizeof(text), d->format, fc);
        set_color(cr, text_color);
        show_pango_text(cr, text, width - 4 - height, 0, 0, TRUE);
      }
      // label on top of marker:
      set_color(cr, text_color);
      show_pango_text(cr, w->label, 2, 0, 0, FALSE/*, gtk_widget_is_sensitive(widget), FALSE*/);
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

  return TRUE;
}

void dt_bauhaus_hide_popup()
{
  if(darktable.bauhaus->current)
  {
    gtk_grab_remove(darktable.bauhaus->popup_window);
    gtk_widget_hide(darktable.bauhaus->popup_window);
    darktable.bauhaus->current = NULL;
    // TODO: give focus to center view? do in accept() as well?
  }
  _stop_cursor();
}

void dt_bauhaus_show_popup(dt_bauhaus_widget_t *w)
{
  if(darktable.bauhaus->current) dt_bauhaus_hide_popup();
  darktable.bauhaus->current = w;
  darktable.bauhaus->keys_cnt = 0;
  memset(darktable.bauhaus->keys, 0, sizeof(darktable.bauhaus->keys));
  darktable.bauhaus->change_active = 0;
  darktable.bauhaus->mouse_line_distance = 0.0f;
  _stop_cursor();

  if(w->module) dt_iop_request_focus(w->module);

  int offset = 0;
  GtkAllocation tmp;
  gtk_widget_get_allocation(GTK_WIDGET(w), &tmp);

  gtk_widget_realize(darktable.bauhaus->popup_window);
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
      if(w->combo_populate) w->combo_populate(GTK_WIDGET(w), &w->module);
      // comboboxes change immediately
      darktable.bauhaus->change_active = 1;
      dt_bauhaus_combobox_data_t *d = &w->data.combobox;
      tmp.height = (tmp.height + get_line_space()) * d->num_labels;
      GtkAllocation allocation_w;
      gtk_widget_get_allocation(GTK_WIDGET(w), &allocation_w);
      const int ht = allocation_w.height;
      const int skip = ht + get_line_space();
      offset = -d->active * skip;
      darktable.bauhaus->mouse_x = 0;
      darktable.bauhaus->mouse_y = d->active * skip + ht / 2;
      break;
    }
    default:
      break;
  }

  gint wx, wy;
  gdk_window_get_origin(gtk_widget_get_window(GTK_WIDGET(w)), &wx, &wy);

  // move popup so mouse is over currently active item, to minimize confusion with scroll wheel:
  if(darktable.bauhaus->current->type == DT_BAUHAUS_COMBOBOX) wy += offset;

  // gtk_widget_get_window will return null if not shown yet.
  // it is needed for gdk_window_move, and gtk_window move will
  // sometimes be ignored. this is why we always call both...
  // we also don't want to show before move, as this results in noticeable flickering.
  GdkWindow *window = gtk_widget_get_window(darktable.bauhaus->popup_window);
  if(window) gdk_window_move(window, wx, wy);
  gtk_window_move(GTK_WINDOW(darktable.bauhaus->popup_window), wx, wy);
  gtk_widget_set_size_request(darktable.bauhaus->popup_area, tmp.width, tmp.height);
  gtk_widget_set_size_request(darktable.bauhaus->popup_window, tmp.width, tmp.height);
  gtk_widget_show_all(darktable.bauhaus->popup_window);
  gtk_widget_grab_focus(darktable.bauhaus->popup_area);
}

static gboolean dt_bauhaus_slider_add_delta_internal(GtkWidget *widget, float delta, guint state)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  float multiplier;

  GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
  if((state & modifiers) == GDK_SHIFT_MASK)
  {
    multiplier = dt_conf_get_float("darkroom/ui/scale_rough_step_multiplier");
  }
  else if((state & modifiers) == GDK_CONTROL_MASK)
  {
    multiplier = dt_conf_get_float("darkroom/ui/scale_precise_step_multiplier");
  }
  else
  {
    multiplier = dt_conf_get_float("darkroom/ui/scale_step_multiplier");
  }

  delta *= multiplier;

  if(w->module) dt_iop_request_focus(w->module);

  dt_bauhaus_slider_set_normalized(w, d->pos + delta);

  return TRUE;
}

static gboolean dt_bauhaus_slider_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  gdouble delta_y;
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  if(w->type != DT_BAUHAUS_SLIDER) return FALSE;
  gtk_widget_grab_focus(widget);

  if(dt_gui_get_scroll_deltas(event, NULL, &delta_y))
  {
    delta_y *= -w->data.slider.scale / 5.0;
    return dt_bauhaus_slider_add_delta_internal(widget, delta_y, event->state);
  }

  return FALSE;
}

static gboolean dt_bauhaus_slider_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  if(w->type != DT_BAUHAUS_SLIDER) return FALSE;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  int handled = 0;
  float delta = 0.0f;
  if(event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up || event->keyval == GDK_KEY_Right
     || event->keyval == GDK_KEY_KP_Right)
  {
    handled = 1;
    delta = d->scale / 5.0f;
  }
  else if(event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down || event->keyval == GDK_KEY_Left
          || event->keyval == GDK_KEY_KP_Left)
  {
    handled = 1;
    delta = -d->scale / 5.0f;
  }

  if(!handled) return FALSE;

  return dt_bauhaus_slider_add_delta_internal(widget, delta, event->state);
}


static gboolean dt_bauhaus_combobox_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  int delta_y;
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  if(w->type != DT_BAUHAUS_COMBOBOX) return FALSE;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  gtk_widget_grab_focus(widget);

  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    if(w->module) dt_iop_request_focus(w->module);
    dt_bauhaus_combobox_set(widget, CLAMP(d->active + delta_y, 0, d->num_labels - 1));
    return TRUE;
  }

  return FALSE;
}

static gboolean dt_bauhaus_combobox_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  if(w->type != DT_BAUHAUS_COMBOBOX) return FALSE;
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  if(event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up || event->keyval == GDK_KEY_Left
     || event->keyval == GDK_KEY_KP_Left)
  {
    if(w->module) dt_iop_request_focus(w->module);
    dt_bauhaus_combobox_set(widget, CLAMP(d->active - 1, 0, d->num_labels - 1));
    return TRUE;
  }
  else if(event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down || event->keyval == GDK_KEY_Right
          || event->keyval == GDK_KEY_KP_Right)
  {
    if(w->module) dt_iop_request_focus(w->module);
    dt_bauhaus_combobox_set(widget, CLAMP(d->active + 1, 0, d->num_labels - 1));
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_bauhaus_combobox_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  if(w->type != DT_BAUHAUS_COMBOBOX) return FALSE;
  if(w->module) dt_iop_request_focus(w->module);
  gtk_widget_grab_focus(GTK_WIDGET(w));
  GtkAllocation tmp;
  gtk_widget_get_allocation(GTK_WIDGET(w), &tmp);
  dt_bauhaus_combobox_data_t *d = &w->data.combobox;
  if(w->quad_paint && (event->x > allocation.width - allocation.height))
  {
    if (w->quad_toggle)
    {
      if (w->quad_paint_flags & CPF_ACTIVE)
        w->quad_paint_flags &= ~CPF_ACTIVE;
      else
        w->quad_paint_flags |= CPF_ACTIVE;
    }
    g_signal_emit_by_name(G_OBJECT(w), "quad-pressed");
    return TRUE;
  }
  else if(event->button == 3)
  {
    darktable.bauhaus->mouse_x = event->x;
    darktable.bauhaus->mouse_y = event->y;
    dt_bauhaus_show_popup(w);
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
      darktable.bauhaus->opentime = dt_get_wtime();
      darktable.bauhaus->mouse_x = event->x;
      darktable.bauhaus->mouse_y = event->y;
      dt_bauhaus_show_popup(w);
    }
    return TRUE;
  }
  return FALSE;
}

float dt_bauhaus_slider_get(GtkWidget *widget)
{
  // first cast to bh widget, to check that type:
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return -1.0f;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  if(d->max == d->min) {
    return d->max;
  }
  float rawval = d->min + d->pos * (d->max - d->min);
  return d->callback(widget, rawval, DT_BAUHAUS_GET);
}

void dt_bauhaus_slider_set(GtkWidget *widget, float pos)
{
  // this is the public interface function, translate by bounds and call set_normalized
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float rawval = d->callback(widget, pos, DT_BAUHAUS_SET);
  dt_bauhaus_slider_set_normalized(w, (rawval - d->min) / (d->max - d->min));
}

void dt_bauhaus_slider_set_digits(GtkWidget *widget, int val)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->digits = val;
  snprintf(d->format, sizeof(d->format), "%%.0%df", val);
}

int dt_bauhaus_slider_get_digits(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return 0;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  return d->digits;
}

void dt_bauhaus_slider_set_step(GtkWidget *widget, float val)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->step = val;
  d->scale = 5.0f * d->step / (d->max - d->min);
}

float dt_bauhaus_slider_get_step(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return 0;

  dt_bauhaus_slider_data_t *d = &w->data.slider;

  return d->step;
}

void dt_bauhaus_slider_reset(GtkWidget *widget)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);

  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  d->min = d->soft_min;
  d->max = d->soft_max;
  dt_bauhaus_slider_set_normalized(w, d->defpos);

  return;
}

void dt_bauhaus_slider_set_format(GtkWidget *widget, const char *format)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  g_strlcpy(d->format, format, sizeof(d->format));
}

void dt_bauhaus_slider_set_callback(GtkWidget *widget, float (*callback)(GtkWidget *self, float value, dt_bauhaus_callback_t dir))
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->callback = (callback == NULL ? _default_linear_callback : callback);
}

void dt_bauhaus_slider_set_soft(GtkWidget *widget, float pos)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float rawval = d->callback(widget, pos, DT_BAUHAUS_SET);
  float rpos = CLAMP(rawval, d->hard_min, d->hard_max);
  d->min = MIN(d->min, rpos);
  d->max = MAX(d->max, rpos);
  d->scale = 5.0f * d->step / (d->max - d->min);
  rpos = (rpos - d->min) / (d->max - d->min);
  dt_bauhaus_slider_set_normalized(w, rpos);
}

static void dt_bauhaus_slider_set_normalized(dt_bauhaus_widget_t *w, float pos)
{
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  float rpos = CLAMP(pos, 0.0f, 1.0f);
  rpos = d->min + (d->max - d->min) * rpos;
  const float base = powf(10.0f, d->digits);
  rpos = roundf(base * rpos) / base;
  d->pos = (rpos - d->min) / (d->max - d->min);
  gtk_widget_queue_draw(GTK_WIDGET(w));
  d->is_changed = 1;
  if(!darktable.gui->reset && !d->is_dragging)
  {
    g_signal_emit_by_name(G_OBJECT(w), "value-changed");
    d->is_changed = 0;
  }
}

static gboolean dt_bauhaus_slider_postponed_value_change(gpointer data)
{
  if(!GTK_IS_WIDGET(data)) return 0;

  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)data;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  if(d->is_changed)
  {
    g_signal_emit_by_name(G_OBJECT(w), "value-changed");
    d->is_changed = 0;
  }

  if(!d->is_dragging) d->timeout_handle = 0;

  return d->is_dragging;
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
         && (event->keyval == GDK_KEY_space || event->keyval == GDK_KEY_KP_Space || // SPACE
             event->keyval == GDK_KEY_percent ||                                    // %
             (event->string[0] >= 40 && event->string[0] <= 57) ||                  // ()+-*/.,0-9
             event->keyval == GDK_KEY_asciicircum ||                                // ^
             event->keyval == GDK_KEY_X || event->keyval == GDK_KEY_x))             // Xx
      {
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
        float old_value = dt_bauhaus_slider_get(GTK_WIDGET(darktable.bauhaus->current));
        float new_value = dt_calculator_solve(old_value, darktable.bauhaus->keys);
        if(isfinite(new_value)) dt_bauhaus_slider_set_soft(GTK_WIDGET(darktable.bauhaus->current), new_value);
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
      gunichar c = g_utf8_get_char(event->string);
      long int char_width = g_utf8_next_char(event->string) - event->string;
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
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  if(w->module) dt_iop_request_focus(w->module);
  gtk_widget_grab_focus(GTK_WIDGET(w));
  GtkAllocation tmp;
  gtk_widget_get_allocation(GTK_WIDGET(w), &tmp);
  if(w->quad_paint && (event->x > allocation.width - allocation.height))
  {
    if (w->quad_toggle)
    {
      if (w->quad_paint_flags & CPF_ACTIVE)
        w->quad_paint_flags &= ~CPF_ACTIVE;
      else
        w->quad_paint_flags |= CPF_ACTIVE;
    }
    g_signal_emit_by_name(G_OBJECT(w), "quad-pressed");
    return TRUE;
  }
  else if(event->button == 3)
  {
    dt_bauhaus_show_popup(w);
    return TRUE;
  }
  else if(event->button == 1)
  {
    // reset to default.
    if(event->type == GDK_2BUTTON_PRESS)
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      d->is_dragging = 0;
      dt_bauhaus_slider_reset(GTK_WIDGET(w));
    }
    else
    {
      const float l = 4.0f / tmp.width;
      const float r = 1.0f - (tmp.height + 4.0f) / tmp.width;
      dt_bauhaus_slider_set_normalized(w, (event->x / tmp.width - l) / (r - l));
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      d->is_dragging = 1;
      int delay = CLAMP(darktable.develop->average_delay * 3 / 2, DT_BAUHAUS_SLIDER_VALUE_CHANGED_DELAY_MIN,
                        DT_BAUHAUS_SLIDER_VALUE_CHANGED_DELAY_MAX);
      // timeout_handle should always be zero here, but check just in case
      if(!d->timeout_handle)
        d->timeout_handle = g_timeout_add(delay, dt_bauhaus_slider_postponed_value_change, widget);
    }
    return TRUE;
  }
  return FALSE;
}

static gboolean dt_bauhaus_slider_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_bauhaus_slider_data_t *d = &w->data.slider;

  if((event->button == 1) && (d->is_dragging))
  {
    if(w->module) dt_iop_request_focus(w->module);
    GtkAllocation tmp;
    gtk_widget_get_allocation(GTK_WIDGET(w), &tmp);
    d->is_dragging = 0;
    if(d->timeout_handle) g_source_remove(d->timeout_handle);
    d->timeout_handle = 0;
    const float l = 4.0f / tmp.width;
    const float r = 1.0f - (tmp.height + 4.0f) / tmp.width;
    dt_bauhaus_slider_set_normalized(w, (event->x / tmp.width - l) / (r - l));

    return TRUE;
  }
  return FALSE;
}

static gboolean dt_bauhaus_slider_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  // remember mouse position for motion effects in draw
  if(event->state & GDK_BUTTON1_MASK && event->type != GDK_2BUTTON_PRESS)
  {
    dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
    GtkAllocation tmp;
    gtk_widget_get_allocation(GTK_WIDGET(w), &tmp);
    const float l = 4.0f / tmp.width;
    const float r = 1.0f - (tmp.height + 4.0f) / tmp.width;
    dt_bauhaus_slider_set_normalized(w, (event->x / tmp.width - l) / (r - l));
  }
  // not sure if needed:
  // gdk_event_request_motions(event);
  return TRUE;
}

void dt_bauhaus_vimkey_exec(const char *input)
{
  char module[64], label[64], value[256], *key;
  float old_value, new_value;

  sscanf(input, ":set %[^.].%[^=]=%s", module, label, value);
  fprintf(stderr, "[vimkey] setting module `%s', slider `%s' to `%s'", module, label, value);
  key = g_strjoin(".", module, label, NULL);
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)g_hash_table_lookup(darktable.bauhaus->keymap, key);
  g_free(key);
  if(!w) return;
  switch(w->type)
  {
    case DT_BAUHAUS_SLIDER:
      old_value = dt_bauhaus_slider_get(GTK_WIDGET(w));
      new_value = dt_calculator_solve(old_value, value);
      fprintf(stderr, " = %f\n", new_value);
      if(isfinite(new_value)) dt_bauhaus_slider_set_soft(GTK_WIDGET(w), new_value);
      break;
    case DT_BAUHAUS_COMBOBOX:
      // TODO: what about text as entry?
      old_value = dt_bauhaus_combobox_get(GTK_WIDGET(w));
      new_value = dt_calculator_solve(old_value, value);
      fprintf(stderr, " = %f\n", new_value);
      if(isfinite(new_value)) dt_bauhaus_combobox_set(GTK_WIDGET(w), new_value);
      break;
    default:
      break;
  }
}

// give autocomplete suggestions
GList *dt_bauhaus_vimkey_complete(const char *input)
{
  GList *cmp = darktable.bauhaus->key_mod;
  char *point = strstr(input, ".");
  if(point) cmp = darktable.bauhaus->key_val;
  int prefix = strlen(input);
  GList *res = NULL;
  int after = 0;
  while(cmp)
  {
    char *path = (char *)cmp->data;
    if(strncasecmp(path, input, prefix))
    {
      if(after) break; // sorted, so we're done
                       // else loop till we find the start of it
    }
    else
    {
      // append:
      res = g_list_insert_sorted(res, path, (GCompareFunc)strcmp);
      after = 1;
    }
    cmp = g_list_next(cmp);
  }
  return res;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
