/*
    This file is part of darktable,
    copyright (c) 2012 johannes hanika.

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

// new type dt_bauhaus_widget_t, gtk functions start with dt_bh (so they don't collide with ours), we inherit from drawing area
G_DEFINE_TYPE (DtBauhausWidget, dt_bh, GTK_TYPE_DRAWING_AREA);


// fwd declare
static void dt_bauhaus_hide_popup();
static void dt_bauhaus_show_popup(dt_bauhaus_widget_t *w);
static gboolean
dt_bauhaus_popup_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
static gboolean
dt_bauhaus_popup_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static void
dt_bauhaus_widget_accept(dt_bauhaus_widget_t *w);
static void
dt_bauhaus_widget_reject(dt_bauhaus_widget_t *w);


// -------------------------------
static void
dt_bauhaus_slider_set_normalized(dt_bauhaus_widget_t *w, float pos);

static float
get_slider_line_offset(float pos, float scale, float x, float y, float ht, const int width)
{
  // ht is in [0,1] scale here
  const float l = 4.0f/width;
  const float r = 1.0f-4.0f/width - ht;

  float offset = 0.0f;
  // handle linear startup and rescale y to fit the whole range again
  if(y < ht)
  {
    offset = (x-l)/(r-l) - pos;
  }
  else
  {
    y -= ht;
    y /= (1.0f-ht);

    // x = y^2 * .5(1+off/scale) + (1-y^2)*(l + (pos+off)*(r-l))
    // now find off given pos, y, and x:
    // x - y^2*.5 - (1-y^2)*pos = y^2*.5f*off/scale + (1-y^2)off
    //                          = off ((.5f/scale-1)*y^2 + 1)
    // return (x - y*y*.5f - (1.0f-y*y)*pos)/((.5f/scale-1.f)*y*y + 1.0f);

    offset = (x - y*y*.5f - (1.0f-y*y)*(l + pos*(r-l)))/(.5f*y*y/scale + (1.0f-y*y)*(r-l));
  }
  // clamp to result in a [0,1] range:
  if(pos + offset > 1.0f) offset = 1.0f-pos;
  if(pos + offset < 0.0f) offset = -pos;
  return offset;
}

// draw a loupe guideline for the quadratic zoom in in the slider interface:
static void
draw_slider_line(cairo_t *cr, float pos, float off, float scale, const int width, const int height, const int ht)
{
  // TODO: pos is normalized position [0,1], offset is on that scale.
  // TODO: respect inset_left and inset_right!
  // ht is in pixels here
  const float l = 4.0f/width;
  const float r = 1.0f-(ht+4.0f)/width;
  
  const int steps = 64;
  cairo_move_to(cr, width*(l + (pos+off)*(r-l)), ht*.5f);
  cairo_line_to(cr, width*(l + (pos+off)*(r-l)), ht);
  for(int j=1;j<steps;j++)
  {
    const float y = j/(steps-1.0f);
    const float x = y*y*.5f*(1.f+off/scale) + (1.0f-y*y)*(l + (pos+off)*(r-l));
    cairo_line_to(cr, x*width, ht + y*(height-ht));
  }
}
// -------------------------------

static gboolean
dt_bauhaus_popup_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  // remember mouse position for motion effects in draw
  darktable.bauhaus->mouse_x = event->x;
  darktable.bauhaus->mouse_y = event->y;
  gtk_widget_queue_draw(darktable.bauhaus->popup_area);
  dt_bauhaus_widget_t *w = darktable.bauhaus->current;
  int width = darktable.bauhaus->popup_window->allocation.width, height = darktable.bauhaus->popup_window->allocation.height;
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
      break;
    case DT_BAUHAUS_SLIDER:
      {
        dt_bauhaus_slider_data_t *d = &w->data.slider;
        const float mouse_off = get_slider_line_offset(
            d->oldpos, d->scale, darktable.bauhaus->mouse_x/width,
            darktable.bauhaus->mouse_y/height, GTK_WIDGET(w)->allocation.height/(float)height,
            widget->allocation.width);
        dt_bauhaus_slider_set_normalized(w, d->oldpos + mouse_off);
      }
      break;
    default:
      break;
  }
  // throttling using motion hint:
  // gdk_event_request_motions(event);
  return TRUE;
}

static gboolean
dt_bauhaus_popup_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  // TODO: make popup disappear, send accept signal or don't?
  // dt_bauhaus_hide_popup();
  return TRUE;
}

static gboolean
dt_bauhaus_popup_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    // only accept left mouse click
    darktable.bauhaus->end_mouse_x = event->x;
    darktable.bauhaus->end_mouse_y = event->y;
    dt_bauhaus_widget_accept(darktable.bauhaus->current);
  }
  else
  {
    dt_bauhaus_widget_reject(darktable.bauhaus->current);
  }
  dt_bauhaus_hide_popup();
  return TRUE;
}

static void
window_show(GtkWidget *w, gpointer user_data)
{
  /* grabbing might not succeed immediately... */
  if (gdk_keyboard_grab(w->window, FALSE, GDK_CURRENT_TIME) != GDK_GRAB_SUCCESS) 
  {
    // never happened so far:
    /* ...wait a while and try again */
    fprintf(stderr, "[bauhaus] failed to get keyboard focus for popup window!\n");
    // struct timeval s;
    // s.tv_sec = 0;
    // s.tv_usec = 5000;
    // select(0, NULL, NULL, NULL, &s);
    // sched_yield();
  }
}

static void
dt_bh_init(DtBauhausWidget *class)
{
  // not sure if we want to use this instead of our code in *_new()
  // TODO: the common code from bauhaus_widget_init() could go here.
}

static void
dt_bh_class_init(DtBauhausWidgetClass *class)
{
  darktable.bauhaus->signals[DT_BAUHAUS_VALUE_CHANGED_SIGNAL] =
    g_signal_new ("value-changed",
        G_TYPE_FROM_CLASS (class),
        G_SIGNAL_RUN_LAST,
        0,
        NULL, NULL,
        g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  // TODO: could init callbacks once per class for more efficiency:
  // GtkWidgetClass *widget_class;
  // widget_class = GTK_WIDGET_CLASS (class);
  // widget_class->expose_event = dt_bauhaus_expose;
}

void
dt_bauhaus_init()
{
  darktable.bauhaus = (dt_bauhaus_t *)malloc(sizeof(dt_bauhaus_t));
  memset(darktable.bauhaus, 0, sizeof(dt_bauhaus_t));
  darktable.bauhaus->keys_cnt = 0;
  darktable.bauhaus->current = NULL;
  darktable.bauhaus->popup_area = gtk_drawing_area_new();
  // this easily gets keyboard input:
  // darktable.bauhaus->popup_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  // but this doesn't flicker, and the above hack with key input seems to work well.
  darktable.bauhaus->popup_window = gtk_window_new(GTK_WINDOW_POPUP);
  // this is needed for popup, not for toplevel.
  // since popup_area gets the focus if we show the window, this is all
  // we need.
  dt_gui_key_accel_block_on_focus(darktable.bauhaus->popup_area);

  gtk_widget_set_size_request(darktable.bauhaus->popup_area, 300, 300);
  gtk_window_set_resizable(GTK_WINDOW(darktable.bauhaus->popup_window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(darktable.bauhaus->popup_window), 260, 260);
  // gtk_window_set_modal(GTK_WINDOW(c->popup_window), TRUE);
  // gtk_window_set_decorated(GTK_WINDOW(c->popup_window), FALSE);

  // for pie menue:
  // gtk_window_set_position(GTK_WINDOW(c->popup_window), GTK_WIN_POS_MOUSE);// | GTK_WIN_POS_CENTER);
  // gtk_window_set_transient_for(GTK_WINDOW(c->popup_window), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_container_add(GTK_CONTAINER(darktable.bauhaus->popup_window), darktable.bauhaus->popup_area);
  // gtk_window_set_title(GTK_WINDOW(c->popup_window), _("dtgtk control popup"));
  gtk_window_set_keep_above(GTK_WINDOW(darktable.bauhaus->popup_window), TRUE);
  gtk_window_set_gravity(GTK_WINDOW(darktable.bauhaus->popup_window), GDK_GRAVITY_STATIC);

  gtk_widget_set_can_focus(darktable.bauhaus->popup_area, TRUE);
  gtk_widget_add_events(darktable.bauhaus->popup_area,
      GDK_POINTER_MOTION_MASK |
      GDK_POINTER_MOTION_HINT_MASK |
      GDK_BUTTON_PRESS_MASK |
      GDK_BUTTON_RELEASE_MASK |
      GDK_KEY_PRESS_MASK |
      GDK_LEAVE_NOTIFY_MASK);

  g_signal_connect (G_OBJECT (darktable.bauhaus->popup_window), "show", G_CALLBACK(window_show), (gpointer)NULL);
  g_signal_connect (G_OBJECT (darktable.bauhaus->popup_area), "expose-event",
                    G_CALLBACK (dt_bauhaus_popup_expose), (gpointer)NULL);
  g_signal_connect (G_OBJECT (darktable.bauhaus->popup_area), "motion-notify-event",
                    G_CALLBACK (dt_bauhaus_popup_motion_notify), (gpointer)NULL);
  g_signal_connect (G_OBJECT (darktable.bauhaus->popup_area), "leave-notify-event",
                    G_CALLBACK (dt_bauhaus_popup_leave_notify), (gpointer)NULL);
  g_signal_connect (G_OBJECT (darktable.bauhaus->popup_area), "button-press-event",
                    G_CALLBACK (dt_bauhaus_popup_button_press), (gpointer)NULL);
  g_signal_connect (G_OBJECT (darktable.bauhaus->popup_area), "key-press-event",
                    G_CALLBACK (dt_bauhaus_popup_key_press), (gpointer)NULL);
}

void
dt_bauhaus_cleanup()
{
  // TODO: destroy popup window and resources
}

// fwd declare a few callbacks
static gboolean
dt_bauhaus_slider_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean
dt_bauhaus_slider_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
static gboolean
dt_bauhaus_slider_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);


static gboolean
dt_bauhaus_combobox_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
// static gboolean
// dt_bauhaus_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean
dt_bauhaus_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);

// end static init/cleanup
// =================================================





// common initialization
static void
dt_bauhaus_widget_init(dt_bauhaus_widget_t* w, dt_iop_module_t *self)
{
  // dt_gui_key_accel_block_on_focus(w->area);
  w->module = self;

  // TODO: check this size, and all other sizes/borders (and colors)
  gtk_widget_set_size_request(GTK_WIDGET(w), 260, 18);
  // TODO: encapsulate that for widgets, switch on widget type and add some descriptions (right click for slider + type etc)
  g_object_set (GTK_OBJECT(w), "tooltip-text", _("smart tooltip"), (char *)NULL);

  gtk_widget_add_events(GTK_WIDGET(w),
      GDK_POINTER_MOTION_MASK |
      GDK_POINTER_MOTION_HINT_MASK |
      GDK_BUTTON_PRESS_MASK |
      GDK_BUTTON_RELEASE_MASK |
      GDK_LEAVE_NOTIFY_MASK);

  g_signal_connect (G_OBJECT (w), "expose-event",
                    G_CALLBACK (dt_bauhaus_expose), NULL);
}

void dt_bauhaus_widget_set_label(GtkWidget *widget, const char *text)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  strncpy(w->label, text, 256);
}

GtkWidget*
dt_bauhaus_slider_new(dt_iop_module_t *self)
{
  return dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.1, 0.5, 3);
}

GtkWidget*
dt_bauhaus_slider_new_with_range(dt_iop_module_t *self, float min, float max, float step, float defval, int digits)
{
  dt_bauhaus_widget_t* w = DT_BAUHAUS_WIDGET(g_object_new(DT_BAUHAUS_WIDGET_TYPE, NULL));
  dt_bauhaus_widget_init(w, self);
  w->type = DT_BAUHAUS_SLIDER;
  dt_bauhaus_widget_init(w, self);
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->min = min;
  d->max = max;
  // normalize default:
  d->defpos = (defval-min)/(max-min);
  d->pos = d->defpos;
  d->oldpos = d->defpos;
  // TODO: normalize, too?
  d->scale = step;
  snprintf(d->format, 8, "%%.0%df", digits);

  g_signal_connect (G_OBJECT (w), "button-press-event",
                    G_CALLBACK (dt_bauhaus_slider_button_press), (gpointer)NULL);
  g_signal_connect (G_OBJECT (w), "motion-notify-event",
                    G_CALLBACK (dt_bauhaus_slider_motion_notify), (gpointer)NULL);
  g_signal_connect (G_OBJECT (w), "leave-notify-event",
                    G_CALLBACK (dt_bauhaus_slider_leave_notify), (gpointer)NULL);
  // TODO: implement destroy signal to get rid of dynamically alloc'ed strings!
  // (might be the label..?)
  return GTK_WIDGET(w);
}

GtkWidget*
dt_bauhaus_combobox_new(dt_iop_module_t *self)
{
  dt_bauhaus_widget_t* w = DT_BAUHAUS_WIDGET(g_object_new(DT_BAUHAUS_WIDGET_TYPE, NULL));
  w->type = DT_BAUHAUS_COMBOBOX;
  dt_bauhaus_widget_init(w, self);
  g_signal_connect (G_OBJECT (w), "button-press-event",
                    G_CALLBACK (dt_bauhaus_combobox_button_press), (gpointer)NULL);
  // TODO: implement destroy signal to get rid of dynamically alloc'ed strings!
  return GTK_WIDGET(w);
}


// TODO: into draw.h
static void
draw_equilateral_triangle(cairo_t *cr, float radius)
{
  const float sin = 0.866025404 * radius;
  const float cos = 0.5f * radius;
  cairo_move_to(cr, 0.0, radius);
  cairo_line_to(cr, -sin, -cos);
  cairo_line_to(cr,  sin, -cos);
  cairo_line_to(cr, 0.0, radius);
}

static void
dt_bauhaus_clear(dt_bauhaus_widget_t *w, cairo_t *cr)
{
  // clear bg with background color
  cairo_save(cr);
  GtkWidget *topwidget = dt_iop_gui_get_pluginui(w->module);
  GtkStyle *style = gtk_widget_get_style(topwidget);
  cairo_set_source_rgb (cr,
      style->bg[gtk_widget_get_state(topwidget)].red/65535.0f,
      style->bg[gtk_widget_get_state(topwidget)].green/65535.0f,
      style->bg[gtk_widget_get_state(topwidget)].blue/65535.0f);
  cairo_paint(cr);
  cairo_restore(cr);
}

static void
dt_bauhaus_draw_quad(dt_bauhaus_widget_t *w, cairo_t *cr)
{
  // TODO: decide if we need this at all.
  //       there is a chance it only introduces clutter.
#if 0
  GtkWidget *widget = GTK_WIDGET(w);
  int width  = widget->allocation.width;
  int height = widget->allocation.height;
  // draw active area square:
  // TODO: combo: V  slider: <>  checkbox: X
  cairo_save(cr);
  cairo_set_source_rgb(cr, .6, .6, .6);
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
      cairo_translate(cr, width -height*.5f, height*.5f);
      cairo_set_line_width(cr, 1.0);
      draw_equilateral_triangle(cr, height*0.38f);
      cairo_fill(cr);
      break;
    case DT_BAUHAUS_SLIDER:
      // TODO: two arrows to step with the mouse buttons?
      break;
    default:
      cairo_rectangle(cr, width - height, 0, height, height);
      cairo_fill(cr);
      break;
  }
  cairo_restore(cr);
#endif
}

static void
dt_bauhaus_draw_label(dt_bauhaus_widget_t *w, cairo_t *cr)
{
  // draw label:
  GtkWidget *widget = GTK_WIDGET(w);
  const int height = widget->allocation.height;
  cairo_save(cr);
  cairo_set_source_rgb(cr, 1., 1., 1.);
  cairo_move_to(cr, 2, height*0.8);
  cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, .8*height);
  cairo_show_text(cr, w->label);
  cairo_restore(cr);
}

static void
dt_bauhaus_widget_reject(dt_bauhaus_widget_t *w)
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
static void
dt_bauhaus_widget_accept(dt_bauhaus_widget_t *w)
{
  GtkWidget *widget = GTK_WIDGET(w);
  int width = darktable.bauhaus->popup_window->allocation.width, height = darktable.bauhaus->popup_window->allocation.height;
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
      break;
    case DT_BAUHAUS_SLIDER:
      {
        dt_bauhaus_slider_data_t *d = &w->data.slider;
        const float mouse_off = get_slider_line_offset(
            d->oldpos, d->scale, darktable.bauhaus->end_mouse_x/width,
            darktable.bauhaus->end_mouse_y/height, widget->allocation.height/(float)height,
            widget->allocation.width);
        dt_bauhaus_slider_set_normalized(w, d->oldpos + mouse_off);
        d->oldpos = d->pos;
      }
      break;
    default:
      break;
  }
}

static gboolean
dt_bauhaus_popup_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = darktable.bauhaus->current;
  // dimensions of the popup
  int width = widget->allocation.width, height = widget->allocation.height;
  // dimensions of the original line
  GtkWidget *current = GTK_WIDGET(w);
  int wd = current->allocation.width, ht = current->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // draw same things as original widget, for visual consistency:
  dt_bauhaus_clear(w, cr);
  dt_bauhaus_draw_label(w, cr);
  dt_bauhaus_draw_quad(w, cr);

  // draw line around popup
  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
  cairo_move_to(cr, 0.0, 0.0);
  cairo_line_to(cr, 0.0, height);
  cairo_line_to(cr, width, height);
  cairo_line_to(cr, width, 0.0);
  cairo_stroke(cr);

  // switch on bauhaus widget type (so we only need one static window)
  switch(w->type)
  {
    case DT_BAUHAUS_SLIDER:
      {
        dt_bauhaus_slider_data_t *d = &w->data.slider;
        cairo_save(cr);
        cairo_set_line_width(cr, 1.);
        cairo_set_source_rgb(cr, .1, .1, .1);
        const int num_scales = 1.f/d->scale;
        // const int rounded_pos = d->pos/d->scale;
        // for(int k=rounded_pos - num_scales;k<=rounded_pos + num_scales;k++)
        for(int k=0;k<num_scales;k++)
        {
          const float off = k*d->scale - d->oldpos;
          cairo_set_source_rgba(cr, .1, .1, .1, d->scale/fabsf(off));
          draw_slider_line(cr, d->oldpos, off, d->scale, width, height, ht);
          cairo_stroke(cr);
        }
        cairo_restore(cr);

#if 0
        // draw indicator line
        cairo_save(cr);
        cairo_set_source_rgb(cr, .6, .6, .6);
        cairo_set_line_width(cr, 2.);
        draw_slider_line(cr, pos, 0.0f, scale, width, height, ht);
        cairo_stroke(cr);
        cairo_restore(cr);
#endif

        // draw mouse over indicator line
        cairo_save(cr);
        cairo_set_source_rgb(cr, .6, .6, .6);
        cairo_set_line_width(cr, 2.);
        const float mouse_off = get_slider_line_offset(d->oldpos, d->scale,
            darktable.bauhaus->mouse_x/width, darktable.bauhaus->mouse_y/height, ht/(float)height,
            width);
        draw_slider_line(cr, d->oldpos, mouse_off, d->scale, width, height, ht);
        cairo_stroke(cr);
        cairo_restore(cr);

        const float l = 4.0f/width;
        const float r = 1.0f-(ht+4.0f)/width;
        // draw indicator
        cairo_save(cr);
        cairo_set_source_rgb(cr, .6, .6, .6);
        cairo_set_line_width(cr, 1.);
        cairo_translate(cr, (l + (d->oldpos+mouse_off)*(r-l))*wd, ht*.8f);
        cairo_scale(cr, 1.0f, -1.0f);
        draw_equilateral_triangle(cr, ht*0.35f);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.0f);
        cairo_set_source_rgb(cr, .1f, .1f, .1f);
        cairo_stroke(cr);
        cairo_restore(cr);

        // draw numerical value:
        cairo_save(cr);
        cairo_text_extents_t ext;
        cairo_set_source_rgb(cr, 1., 1., 1.);
        cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, .8*ht);
        char text[256];
        snprintf(text, 256, d->format, 0.0f);
        cairo_text_extents (cr, text, &ext);
        cairo_move_to (cr, wd-4-ht-ext.width, ht*0.8);
        snprintf(text, 256, d->format, d->min + (d->oldpos+mouse_off)*(d->max-d->min));
        cairo_show_text(cr, text);
        cairo_restore(cr);

        // draw currently typed text
        if(darktable.bauhaus->keys_cnt)
        {
          cairo_save(cr);
          cairo_text_extents_t ext;
          cairo_set_source_rgb(cr, 1., 1., 1.);
          cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
          cairo_set_font_size (cr, .2*height);
          cairo_text_extents (cr, darktable.bauhaus->keys, &ext);
          cairo_move_to (cr, wd-4-ht-ext.width, height*0.5);
          cairo_show_text(cr, darktable.bauhaus->keys);
          cairo_restore(cr);
        }
      }
      break;
    case DT_BAUHAUS_COMBOBOX:
      {
        cairo_save(cr);
        // TODO: combo box: draw label top left + options below
        // TODO: need mouse over highlights
        cairo_set_source_rgb(cr, 1., 1., 1.);
        cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, .8*ht);
        cairo_text_extents_t ext;
        char text[256];
        for(int k=0;k<5;k++)
        {
          snprintf(text, 256, "%d-th complicated setting", k);
          cairo_text_extents (cr, text, &ext);
          cairo_move_to (cr, wd-4-ht-ext.width, ht*0.8 + (10+ht)*k);
          cairo_show_text(cr, text);
        }
        cairo_restore(cr);
      }
      break;
    default:
      // yell
      break;
  }

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);

  return TRUE;
}

static gboolean
dt_bauhaus_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(widget);
  const int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  dt_bauhaus_clear(w, cr);

  // draw label and quad area at right end
  dt_bauhaus_draw_label(w, cr);
  dt_bauhaus_draw_quad(w, cr);

  // draw type specific content:
  cairo_save(cr);
  cairo_set_line_width(cr, 1.0);
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
      {
        cairo_text_extents_t ext;
        cairo_set_source_rgb(cr, 1., 1., 1.);
        cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, .8*height);
        cairo_text_extents (cr, _("0-th complicated setting"), &ext);
        cairo_move_to (cr, width-4-height-ext.width, height*0.8);
        cairo_show_text(cr, _("0-th complicated setting"));
      }
      break;
    case DT_BAUHAUS_SLIDER:
      {
        dt_bauhaus_slider_data_t *d = &w->data.slider;

#if 0
        // line for orientation? looks crappy:
        cairo_save(cr);
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_set_line_width(cr, 1.);
        cairo_move_to(cr, 2, 0.8*height);
        cairo_line_to(cr, width-4-height, 0.9*height);
        cairo_stroke(cr);
        cairo_restore(cr);
#endif

        // draw scale indicator
        cairo_save(cr);
        const float l = 4.0f/width;
        const float r = 1.0f-(height+4.0f)/width;
        cairo_set_source_rgb(cr, .6, .6, .6);
        cairo_translate(cr, (l + d->pos*(r-l))*width, height*.8f);
        cairo_scale(cr, 1.0f, -1.0f);
        draw_equilateral_triangle(cr, height*0.35f); // 0.3
        // cairo_fill(cr);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.);
        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_stroke(cr);
        cairo_restore(cr);

        // TODO: merge that text with combo
        char text[256];
        snprintf(text, 256, d->format, 0.0f);
        cairo_text_extents_t ext;
        cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, .8*height);
        cairo_text_extents (cr, text, &ext);
        cairo_set_source_rgb(cr, 1., 1., 1.);
        cairo_move_to (cr, width-4-height-ext.width, height*0.8);
        snprintf(text, 256, d->format, d->min + d->pos*(d->max-d->min));
        cairo_show_text(cr, text);
      }
      break;
    default:
      break;
  }
  cairo_restore(cr);

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);

  return TRUE;
}

static void
dt_bauhaus_hide_popup()
{
  gtk_widget_hide(darktable.bauhaus->popup_window);
  darktable.bauhaus->current = NULL;
  // TODO: give focus to center view? do in accept() as well?
}

static void
dt_bauhaus_show_popup(dt_bauhaus_widget_t *w)
{
  if(darktable.bauhaus->current)
    dt_bauhaus_hide_popup();
  darktable.bauhaus->current = w;
  darktable.bauhaus->keys_cnt = 0;
  memset(darktable.bauhaus->keys, 0, 64);

  dt_iop_request_focus(w->module);

  switch(darktable.bauhaus->current->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      d->oldpos = d->pos;
      break;
    }
    default:
      break;
  }

  gint wx, wy;
  gdk_window_get_origin (gtk_widget_get_window (GTK_WIDGET(w)), &wx, &wy);
  gtk_window_move(GTK_WINDOW(darktable.bauhaus->popup_window), wx, wy);
  GtkAllocation tmp;
  gtk_widget_get_allocation(GTK_WIDGET(w), &tmp);
  gtk_widget_set_size_request(darktable.bauhaus->popup_area, tmp.width, tmp.width);
  // dt_control_key_accelerators_off (darktable.control);
  gtk_widget_show_all(darktable.bauhaus->popup_window);
  gtk_widget_grab_focus(darktable.bauhaus->popup_area);
}

static gboolean
dt_bauhaus_combobox_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  darktable.bauhaus->mouse_x = event->x;
  darktable.bauhaus->mouse_y = event->y;
  dt_bauhaus_show_popup(w);
  return TRUE;
}

float
dt_bauhaus_slider_get(GtkWidget *widget)
{
  // first cast to bh widget, to check that type:
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(!w->type == DT_BAUHAUS_SLIDER) return -1.0f;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  return (d->pos+d->min)*(d->max-d->min);
}

void
dt_bauhaus_slider_set(GtkWidget *widget, float pos)
{
  // this is the public interface function, translate by bounds and call set_normalized
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)DT_BAUHAUS_WIDGET(widget);
  if(w->type != DT_BAUHAUS_SLIDER) return;
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  dt_bauhaus_slider_set_normalized(w, (pos-d->min)/(d->max-d->min));
}

static void
dt_bauhaus_slider_set_normalized(dt_bauhaus_widget_t *w, float pos)
{
  dt_bauhaus_slider_data_t *d = &w->data.slider;
  d->pos = CLAMP(pos, 0.0f, 1.0f);
  gtk_widget_queue_draw(GTK_WIDGET(w));
  g_signal_emit_by_name(G_OBJECT(w), "value-changed");
}

static gboolean
dt_bauhaus_popup_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  switch(darktable.bauhaus->current->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      if(darktable.bauhaus->keys_cnt + 2 < 64 &&
        (event->string[0] == 46 ||
        (event->string[0] >= 48 && event->string[0] <= 57)))
      {
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt++] = event->string[0];
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0 &&
             (event->keyval == GDK_KEY_BackSpace || event->keyval == GDK_KEY_Delete))
      {
        darktable.bauhaus->keys[--darktable.bauhaus->keys_cnt] = 0;
        gtk_widget_queue_draw(darktable.bauhaus->popup_area);
      }
      else if(darktable.bauhaus->keys_cnt > 0 && darktable.bauhaus->keys_cnt + 1 < 64 &&
             (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter))
      {
        // accept input
        darktable.bauhaus->keys[darktable.bauhaus->keys_cnt] = 0;
        // unnormalized input, user was typing this:
        dt_bauhaus_slider_set(GTK_WIDGET(darktable.bauhaus->current), atof(darktable.bauhaus->keys));
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, 64);
        dt_bauhaus_hide_popup();
      }
      else if(event->keyval == GDK_KEY_Escape)
      {
        // discard input and close popup
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, 64);
        dt_bauhaus_hide_popup();
      }
      return TRUE;
    }
    case DT_BAUHAUS_COMBOBOX:
      if(event->keyval == GDK_KEY_Escape)
      {
        // discard input and close popup
        darktable.bauhaus->keys_cnt = 0;
        memset(darktable.bauhaus->keys, 0, 64);
        dt_bauhaus_hide_popup();
      }
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean
dt_bauhaus_slider_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_iop_request_focus(w->module);
  GtkAllocation tmp;
  gtk_widget_get_allocation(GTK_WIDGET(w), &tmp);
  if(event->button == 3)
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
      dt_bauhaus_slider_data_t *d = &w->data.slider;
      dt_bauhaus_slider_set_normalized(w, d->defpos);
    }
    else
    {
      const float l = 4.0f/tmp.width;
      const float r = 1.0f-(tmp.height+4.0f)/tmp.width;
      dt_bauhaus_slider_set_normalized(w, (event->x/tmp.width - l)/(r-l));
    }
    return TRUE;
  }
  return FALSE;
}

static gboolean
dt_bauhaus_slider_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  // remember mouse position for motion effects in draw
  if(event->state & GDK_BUTTON1_MASK && event->type != GDK_2BUTTON_PRESS)
  {
    dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
    GtkAllocation tmp;
    gtk_widget_get_allocation(GTK_WIDGET(w), &tmp);
    const float l = 4.0f/tmp.width;
    const float r = 1.0f-(tmp.height+4.0f)/tmp.width;
    dt_bauhaus_slider_set_normalized(w, (event->x/tmp.width - l)/(r-l));
  }
  // not sure if needed:
  // gdk_event_request_motions(event);
  return TRUE;
}

static gboolean
dt_bauhaus_slider_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  // TODO: highlight?
  return TRUE;
}


#if 0
static gboolean
dt_bauhaus_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)widget;
  dt_bauhaus_hide_popup();
  return TRUE;
}
#endif

