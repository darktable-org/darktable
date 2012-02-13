/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "gui/presets.h"

#include <sys/select.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef enum dt_bauhaus_type_t
{
  DT_BAUHAUS_SLIDER = 1,
  DT_BAUHAUS_COMBOBOX = 2,
  DT_BAUHAUS_CHECKBOX = 3,
}
dt_bauhaus_type_t;

typedef struct dt_bauhaus_data_t
{
  // this is the placeholder for the data portions
  // associated with the implementations such as
  // siders, combo boxes, ..
}
dt_bauhaus_data_t;

// data portion for a slider
typedef struct dt_bauhaus_slider_data_t
{
  float pos;
  float scale;
  char format[8];
}
dt_bauhaus_slider_data_t;

// data portion for a combobox
typedef struct dt_bauhaus_combobox_data_t
{
  // list of strings, probably
  // TODO:
}
dt_bauhaus_combobox_data_t;

typedef struct dt_bauhaus_widget_t
{
  // which type of control
  dt_bauhaus_type_t type;
  // associated drawing area in gtk
  GtkWidget *area;
  // associated image operation module (to handle focus and such)
  dt_iop_module_t *module;
  // TODO: callbacks for user signals?

  // goes last, might extend past the end:
  dt_bauhaus_data_t data;
}
dt_bauhaus_widget_t;



// =================================================
// begin static functions:
// TODO: some functions for the above: new/clear etc.

typedef struct dt_bauhaus_t
{
  dt_bauhaus_widget_t *current;
  GtkWidget *popup_window;
  GtkWidget *popup_area;
  // are set by the motion notification, to be used during drawing.
  float mouse_x, mouse_y;
  // pointer position when popup window is closed
  float end_mouse_x, end_mouse_y;
  // key input buffer
  char keys[64];
  int keys_cnt;
}
dt_bauhaus_t;

dt_bauhaus_t bauhaus;

static void
dt_bauhaus_widget_accept(dt_bauhaus_widget_t *w);

static gboolean
dt_bauhaus_popup_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  // remember mouse position for motion effects in draw
  bauhaus.mouse_x = event->x;
  bauhaus.mouse_y = event->y;
  gtk_widget_queue_draw(bauhaus.popup_area);
  return TRUE;
}

static gboolean
dt_bauhaus_popup_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  // TODO: make popup disappear, send accept signal or don't?
  // gtk_widget_hide(bauhaus.popup_window);
  return TRUE;
}

static gboolean
dt_bauhaus_popup_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    // only accept left mouse click
    bauhaus.end_mouse_x = event->x;
    bauhaus.end_mouse_y = event->y;
    dt_bauhaus_widget_accept(bauhaus.current);
  }
  // dt_control_key_accelerators_on(darktable.control);
  gtk_widget_hide(bauhaus.popup_window);
  return TRUE;
}

// fwd declare
static gboolean
dt_bauhaus_popup_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
static gboolean
dt_bauhaus_popup_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);

static void
window_show(GtkWidget *w, gpointer user_data)
{
  /* grabbing might not succeed immediately... */
  if (gdk_keyboard_grab(w->window, FALSE, GDK_CURRENT_TIME) != GDK_GRAB_SUCCESS) 
  {
    // never happened so far:
    /* ...wait a while and try again */
    fprintf(stderr, "failed to get keyboard focus for popup window!\n");
    // struct timeval s;
    // s.tv_sec = 0;
    // s.tv_usec = 5000;
    // select(0, NULL, NULL, NULL, &s);
    // sched_yield();
  }
}

void
dt_bauhaus_init()
{
  memset(bauhaus.keys, 0, 64);
  bauhaus.keys_cnt = 0;
  bauhaus.current = NULL;
  bauhaus.popup_area = gtk_drawing_area_new();
  // this easily gets keyboard input:
  // bauhaus.popup_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  // but this doesn't flicker, and the above hack with key input seems to work well.
  bauhaus.popup_window = gtk_window_new(GTK_WINDOW_POPUP);
  // this is needed for popup, not for toplevel.
  // since popup_area gets the focus if we show the window, this is all
  // we need.
  dt_gui_key_accel_block_on_focus(bauhaus.popup_area);

  gtk_widget_set_size_request(bauhaus.popup_area, 300, 300);
  gtk_window_set_resizable(GTK_WINDOW(bauhaus.popup_window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(bauhaus.popup_window), 260, 260);
  // gtk_window_set_modal(GTK_WINDOW(c->popup_window), TRUE);
  // gtk_window_set_decorated(GTK_WINDOW(c->popup_window), FALSE);

  // for pie menue:
  // gtk_window_set_position(GTK_WINDOW(c->popup_window), GTK_WIN_POS_MOUSE);// | GTK_WIN_POS_CENTER);
  // gtk_window_set_transient_for(GTK_WINDOW(c->popup_window), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_container_add(GTK_CONTAINER(bauhaus.popup_window), bauhaus.popup_area);
  // gtk_window_set_title(GTK_WINDOW(c->popup_window), _("dtgtk control popup"));
  gtk_window_set_keep_above(GTK_WINDOW(bauhaus.popup_window), TRUE);
  gtk_window_set_gravity(GTK_WINDOW(bauhaus.popup_window), GDK_GRAVITY_STATIC);

  gtk_widget_set_can_focus(bauhaus.popup_area, TRUE);
  gtk_widget_add_events(bauhaus.popup_area,
      GDK_POINTER_MOTION_MASK |
      GDK_POINTER_MOTION_HINT_MASK |
      GDK_BUTTON_PRESS_MASK |
      GDK_BUTTON_RELEASE_MASK |
      GDK_KEY_PRESS_MASK |
      GDK_LEAVE_NOTIFY_MASK);

  g_signal_connect (G_OBJECT (bauhaus.popup_window), "show", G_CALLBACK(window_show), (gpointer)NULL);
  g_signal_connect (G_OBJECT (bauhaus.popup_area), "expose-event",
                    G_CALLBACK (dt_bauhaus_popup_expose), (gpointer)NULL);
  g_signal_connect (G_OBJECT (bauhaus.popup_area), "motion-notify-event",
                    G_CALLBACK (dt_bauhaus_popup_motion_notify), (gpointer)NULL);
  g_signal_connect (G_OBJECT (bauhaus.popup_area), "leave-notify-event",
                    G_CALLBACK (dt_bauhaus_popup_leave_notify), (gpointer)NULL);
  g_signal_connect (G_OBJECT (bauhaus.popup_area), "button-press-event",
                    G_CALLBACK (dt_bauhaus_popup_button_press), (gpointer)NULL);
  g_signal_connect (G_OBJECT (bauhaus.popup_area), "key-press-event",
                    G_CALLBACK (dt_bauhaus_popup_key_press), (gpointer)NULL);
}

void
dt_bauhaus_cleanup()
{
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
  w->area = gtk_drawing_area_new();
  // dt_gui_key_accel_block_on_focus(w->area);
  w->module = self;

  gtk_widget_set_size_request(w->area, 260, 18);
  // TODO: encapsulate that for widgets, switch on widget type and add some descriptions (right click for slider + type etc)
  g_object_set (GTK_OBJECT(w->area), "tooltip-text", _("smart tooltip"), (char *)NULL);

  gtk_widget_add_events(GTK_WIDGET(w->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect (G_OBJECT (w->area), "expose-event",
                    G_CALLBACK (dt_bauhaus_expose), w);
}

// TODO: don't alloc, but init to avoid fragmentation (can't with dynamic struct sizes..)
dt_bauhaus_widget_t*
dt_bauhaus_slider_new(dt_iop_module_t *self)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)malloc(sizeof(dt_bauhaus_widget_t) + sizeof(dt_bauhaus_slider_data_t));
  w->type = DT_BAUHAUS_SLIDER;
  dt_bauhaus_widget_init(w, self);
  dt_bauhaus_slider_data_t *d = (dt_bauhaus_slider_data_t *)&w->data;
  d->pos = 0.5f;
  d->scale = 0.05f;
  snprintf(d->format, 8, "%%.03f");

  g_signal_connect (G_OBJECT (w->area), "button-press-event",
                    G_CALLBACK (dt_bauhaus_slider_button_press), w);
  g_signal_connect (G_OBJECT (w->area), "motion-notify-event",
                    G_CALLBACK (dt_bauhaus_slider_motion_notify), w);
  g_signal_connect (G_OBJECT (w->area), "leave-notify-event",
                    G_CALLBACK (dt_bauhaus_slider_leave_notify), w);
  return w;
}

dt_bauhaus_widget_t*
dt_bauhaus_combobox_new(dt_iop_module_t *self)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)malloc(sizeof(dt_bauhaus_widget_t) + sizeof(dt_bauhaus_combobox_data_t));
  w->type = DT_BAUHAUS_COMBOBOX;
  dt_bauhaus_widget_init(w, self);
  g_signal_connect (G_OBJECT (w->area), "button-press-event",
                    G_CALLBACK (dt_bauhaus_combobox_button_press), w);
  return w;
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

// draw a loupe guideline for the quadratic zoom in in the slider interface:
static void
draw_slider_line(cairo_t *cr, float pos, float off, float scale, const int width, const int height, const int ht)
{
  const int steps = 64;
  cairo_move_to(cr, width*(pos+off), ht*.5f);
  cairo_line_to(cr, width*(pos+off), ht);
  for(int j=1;j<steps;j++)
  {
    const float y = j/(steps-1.0f);
    const float x = y*y*.5f*(1.f+off/scale) + (1.0f-y*y)*(pos+off);
    cairo_line_to(cr, x*width, ht + y*(height-ht));
  }
}

static float
get_slider_line_offset(float pos, float scale, float x, float y, float ht)
{
  // handle linear startup and rescale y to fit the whole range again
  if(y < ht) return x - pos;
  y -= ht;
  y /= (1.0f-ht);
  // x = y^2 * .5(1+off/scale) + (1-y^2)*(pos+off)
  // now find off given pos, y, and x:
  // x - y^2*.5 - (1-y^2)*pos = y^2*.5f*off/scale + (1-y^2)off
  //                          = off ((.5f/scale-1)*y^2 + 1)
  return (x - y*y*.5f - (1.0f-y*y)*pos)/((.5f/scale-1.f)*y*y + 1.0f);
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
  int width  = w->area->allocation.width;
  int height = w->area->allocation.height;
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
  const int height = w->area->allocation.height;
  cairo_save(cr);
  cairo_set_source_rgb(cr, 1., 1., 1.);
  cairo_move_to(cr, 2, height*0.8);
  cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size (cr, .8*height);
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
      cairo_show_text(cr, _("combobox label"));
      break;
    case DT_BAUHAUS_SLIDER:
      cairo_show_text(cr, _("slider label"));
      break;
    default:
      cairo_show_text(cr, _("label"));
      break;
  }
  cairo_restore(cr);
}

static void
dt_bauhaus_widget_accept(dt_bauhaus_widget_t *w)
{
  int width = bauhaus.popup_window->allocation.width, height = bauhaus.popup_window->allocation.height;
  switch(w->type)
  {
    case DT_BAUHAUS_COMBOBOX:
      break;
    case DT_BAUHAUS_SLIDER:
      {
        dt_bauhaus_slider_data_t *d = (dt_bauhaus_slider_data_t *)&w->data;
        const float mouse_off = get_slider_line_offset(
            d->pos, d->scale, bauhaus.end_mouse_x/width, bauhaus.end_mouse_y/height, w->area->allocation.height/(float)height);
        d->pos = CLAMP(d->pos + mouse_off, 0.0f, 1.0f);
      }
      break;
    default:
      break;
  }
}

static gboolean
dt_bauhaus_popup_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = bauhaus.current;
  // dimensions of the popup
  int width = widget->allocation.width, height = widget->allocation.height;
  // dimensions of the original line
  int wd = w->area->allocation.width, ht = w->area->allocation.height;
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
        dt_bauhaus_slider_data_t *d = (dt_bauhaus_slider_data_t *)&w->data;
        cairo_save(cr);
        cairo_set_line_width(cr, 1.);
        cairo_set_source_rgb(cr, .1, .1, .1);
        const int num_scales = 1.f/d->scale;
        // const int rounded_pos = d->pos/d->scale;
        // for(int k=rounded_pos - num_scales;k<=rounded_pos + num_scales;k++)
        for(int k=0;k<num_scales;k++)
        {
          const float off = k*d->scale - d->pos;
          cairo_set_source_rgba(cr, .1, .1, .1, d->scale/fabsf(off));
          draw_slider_line(cr, d->pos, off, d->scale, width, height, ht);
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
        const float mouse_off = get_slider_line_offset(d->pos, d->scale, bauhaus.mouse_x/width, bauhaus.mouse_y/height, ht/(float)height);
        draw_slider_line(cr, d->pos, mouse_off, d->scale, width, height, ht);
        cairo_stroke(cr);
        cairo_restore(cr);

        // draw indicator
        cairo_save(cr);
        cairo_set_source_rgb(cr, .6, .6, .6);
        cairo_set_line_width(cr, 1.);
        cairo_translate(cr, (d->pos+mouse_off)*wd, ht*.5f);
        draw_equilateral_triangle(cr, ht*0.30f);
        cairo_fill(cr);
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
        snprintf(text, 256, d->format, d->pos+mouse_off);
        cairo_show_text(cr, text);
        cairo_restore(cr);

        // draw currently typed text
        if(bauhaus.keys_cnt)
        {
          cairo_save(cr);
          cairo_text_extents_t ext;
          cairo_set_source_rgb(cr, 1., 1., 1.);
          cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
          cairo_set_font_size (cr, .2*height);
          cairo_text_extents (cr, bauhaus.keys, &ext);
          cairo_move_to (cr, wd-4-ht-ext.width, height*0.5);
          cairo_show_text(cr, bauhaus.keys);
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
          snprintf(text, 256, "complicated setting %d", k);
          cairo_text_extents (cr, text, &ext);
          cairo_move_to (cr, wd-4-ht-ext.width, ht*0.8 + ht*k);
          cairo_show_text(cr, text);
        }
        cairo_restore(cr);
      }
      break;
    case DT_BAUHAUS_CHECKBOX:
      break;
      // TODO: color slider: could include chromaticity diagram
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
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)user_data;
  const int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  dt_bauhaus_clear(w, cr);

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
        cairo_text_extents (cr, _("complicated setting 0"), &ext);
        cairo_move_to (cr, width-4-height-ext.width, height*0.8);
        cairo_show_text(cr, _("complicated setting 0"));
      }
      break;
    case DT_BAUHAUS_SLIDER:
      {
        dt_bauhaus_slider_data_t *d = (dt_bauhaus_slider_data_t *)&w->data;
        // draw scale indicator
        cairo_save(cr);
        cairo_set_source_rgb(cr, .6, .6, .6);
        cairo_translate(cr, d->pos*width, height*.5f);
        draw_equilateral_triangle(cr, height*0.30f);
        cairo_fill(cr);
        cairo_restore(cr);

        // TODO: merge that text with combo
        char text[256];
        snprintf(text, 256, d->format, 0.0f);
        cairo_text_extents_t ext;
        cairo_set_source_rgb(cr, 1., 1., 1.);
        cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, .8*height);
        cairo_text_extents (cr, text, &ext);
        cairo_move_to (cr, width-4-height-ext.width, height*0.8);
        snprintf(text, 256, d->format, d->pos);
        cairo_show_text(cr, text);
      }
      break;
    default:
      break;
  }
  cairo_restore(cr);

  // draw label and quad area at right end
  dt_bauhaus_draw_label(w, cr);
  dt_bauhaus_draw_quad(w, cr);

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);

  return TRUE;
}

static void
dt_bauhaus_show_popup()
{
  bauhaus.keys_cnt = 0;
  memset(bauhaus.keys, 0, 64);

  dt_bauhaus_widget_t *w = bauhaus.current;
  dt_iop_request_focus(w->module);

  gint wx, wy;
  gdk_window_get_origin (gtk_widget_get_window (w->area), &wx, &wy);
  gtk_window_move(GTK_WINDOW(bauhaus.popup_window), wx, wy);
  GtkAllocation tmp;
  gtk_widget_get_allocation(w->area, &tmp);
  gtk_widget_set_size_request(bauhaus.popup_area, tmp.width, tmp.width);
  // dt_control_key_accelerators_off (darktable.control);
  gtk_widget_show_all(bauhaus.popup_window);
  gtk_widget_grab_focus(bauhaus.popup_area);
}

static gboolean
dt_bauhaus_combobox_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)user_data;
  bauhaus.current = w;
  bauhaus.mouse_x = event->x;
  bauhaus.mouse_y = event->y;
  dt_bauhaus_show_popup();
  return TRUE;
}

static void
dt_bauhaus_slider_set(dt_bauhaus_widget_t *w, float pos)
{
  dt_bauhaus_slider_data_t *d = (dt_bauhaus_slider_data_t *)&w->data;
  d->pos = CLAMP(pos, 0.0f, 1.0f);
  gtk_widget_queue_draw(w->area);
}

static gboolean
dt_bauhaus_popup_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  switch(bauhaus.current->type)
  {
    case DT_BAUHAUS_SLIDER:
    {
      if(bauhaus.keys_cnt + 2 < 64 &&
        (event->string[0] == 46 ||
        (event->string[0] >= 48 && event->string[0] <= 57)))
      {
        bauhaus.keys[bauhaus.keys_cnt++] = event->string[0];
        gtk_widget_queue_draw(bauhaus.popup_area);
      }
      else if(bauhaus.keys_cnt > 0 &&
             (event->keyval == GDK_KEY_BackSpace || event->keyval == GDK_KEY_Delete))
      {
        bauhaus.keys[--bauhaus.keys_cnt] = 0;
        gtk_widget_queue_draw(bauhaus.popup_area);
      }
      else if(bauhaus.keys_cnt > 0 && bauhaus.keys_cnt + 1 < 64 &&
             (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter))
      {
        bauhaus.keys[bauhaus.keys_cnt] = 0;
        dt_bauhaus_slider_set(bauhaus.current, atof(bauhaus.keys));
        bauhaus.keys_cnt = 0;
        memset(bauhaus.keys, 0, 64);
        // TODO: encapsulate that as well, if there's some more work to be done here.
        gtk_widget_hide(bauhaus.popup_window);
      }
      return TRUE;
    }
    default:
      return FALSE;
  }
}

static gboolean
dt_bauhaus_slider_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)user_data;
  dt_iop_request_focus(w->module);
  GtkAllocation tmp;
  gtk_widget_get_allocation(w->area, &tmp);
  if(event->button == 3)
  {
    bauhaus.current = w;
    bauhaus.mouse_x = event->x;
    bauhaus.mouse_y = event->y;
    dt_bauhaus_show_popup();
    return TRUE;
  }
  else if(event->button == 1)
  {
      // reset to default.
    if(event->type == GDK_2BUTTON_PRESS)
      // TODO: get default from slider data!
      dt_bauhaus_slider_set(w, 0.5f);
    else
      dt_bauhaus_slider_set(w, event->x/tmp.width);
    return TRUE;
  }
  return FALSE;
}

static gboolean
dt_bauhaus_slider_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  // remember mouse position for motion effects in draw
  if(event->state & GDK_BUTTON1_MASK)
  {
    dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)user_data;
    GtkAllocation tmp;
    gtk_widget_get_allocation(w->area, &tmp);
    dt_bauhaus_slider_set(w, event->x/tmp.width);
  }
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
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)user_data;
  gtk_widget_hide(bauhaus.popup_window);
  return TRUE;
}
#endif


// ====================================================
// iop stuff:

typedef struct dt_iop_bauhaus_params_t
{
  int nothing;
}
dt_iop_bauhaus_params_t;

typedef struct dt_iop_bauhaus_gui_data_t
{
  // TODO: glib ref counting + new/destroy!
  dt_bauhaus_widget_t *combobox;
  dt_bauhaus_widget_t *slider;
}
dt_iop_bauhaus_gui_data_t;

typedef struct dt_iop_bauhaus_data_t
{
}
dt_iop_bauhaus_data_t;

const char *name()
{
  return _("bauhaus controls test");
}

int
groups ()
{
  return IOP_GROUP_BASIC;
}


void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  memcpy(o, i, sizeof(float)*4*roi_in->width*roi_in->height);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void gui_update(struct dt_iop_module_t *self)
{
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_bauhaus_params_t));
  module->default_params = malloc(sizeof(dt_iop_bauhaus_params_t));
  module->default_enabled = 0;
  module->priority = 245; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_bauhaus_params_t);
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_bauhaus_gui_data_t));
  dt_iop_bauhaus_gui_data_t *c = (dt_iop_bauhaus_gui_data_t *)self->gui_data;

  self->widget = gtk_vbox_new(TRUE, 15);//DT_GUI_IOP_MODULE_CONTROL_SPACING);

  c->slider = dt_bauhaus_slider_new(self);
  gtk_box_pack_start(GTK_BOX(self->widget), c->slider->area, TRUE, TRUE, 0);

  c->combobox = dt_bauhaus_combobox_new(self);
  gtk_box_pack_start(GTK_BOX(self->widget), c->combobox->area, TRUE, TRUE, 0);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  // dt_iop_bauhaus_gui_data_t *c = (dt_iop_bauhaus_gui_data_t *)self->gui_data;
  // TODO: need to clean up bauhaus structs!
  free(self->gui_data);
  self->gui_data = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  dt_bauhaus_init();
}

void cleanup_global(dt_iop_module_so_t *module)
{
}

