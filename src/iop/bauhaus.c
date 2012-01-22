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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "gui/presets.h"
#include <gtk/gtk.h>
#include <inttypes.h>

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
  GtkWidget *area, *popup_area;
  // associated image operation module (to handle focus and such)
  dt_iop_module_t *module;
  // TODO: callbacks for user signals?

  // are set by the motion notification, to be used during drawing.
  float mouse_x, mouse_y;
  // goes last, might extend past the end:
  dt_bauhaus_data_t data;
}
dt_bauhaus_widget_t;

// TODO: some functions for the above: new/clear etc.

dt_bauhaus_widget_t*
dt_bauhaus_slider_new()
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)malloc(sizeof(dt_bauhaus_widget_t) + sizeof(dt_bauhaus_slider_data_t));
  w->type = DT_BAUHAUS_SLIDER;
  return w;
}

dt_bauhaus_widget_t*
dt_bauhaus_combobox_new()
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)malloc(sizeof(dt_bauhaus_widget_t) + sizeof(dt_bauhaus_combobox_data_t));
  w->type = DT_BAUHAUS_COMBOBOX;
  return w;
}


typedef struct dt_iop_bauhaus_params_t
{
  int nothing;
}
dt_iop_bauhaus_params_t;

typedef struct dt_iop_bauhaus_gui_data_t
{
  // TODO: these two can be statically alloced once for the library
  GtkWidget *popup_area;
  GtkWidget *popup_window;
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

static gboolean
dt_iop_bauhaus_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  return TRUE;
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
draw_slider_line(cairo_t *cr, float pos, float off, float scale, const int width, const int height)
{
  const int steps = 20;
  cairo_move_to(cr, width*(pos+off), 0.0f);
  for(int j=1;j<steps;j++)
  {
    const float y = j/(steps-1.0f);
    const float x = y*y*.5f*(1.f+off/scale) + (1.0f-y*y)*(pos+off);
    cairo_line_to(cr, x*width, y*height);
  }
}

static float
get_slider_line_offset(float pos, float scale, float x, float y)
{
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
  cairo_show_text(cr, _("label"));
  cairo_restore(cr);
}

static gboolean
dt_bauhaus_popup_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)user_data;
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
        cairo_save(cr);
        cairo_set_line_width(cr, 1.);
        cairo_set_source_rgb(cr, .1, .1, .1);
        const float pos = 0.66f; // dt_bauhaus_slider_get()
        const float scale = 0.05f;
        const int num_scales = 3;
        for(float off=-num_scales*scale;off<num_scales*scale+0.001f;off+=scale)
          draw_slider_line(cr, pos, off, scale, width, height);
        cairo_stroke(cr);
        cairo_restore(cr);

        // draw indicator line
        cairo_save(cr);
        cairo_set_source_rgb(cr, .6, .6, .6);
        cairo_set_line_width(cr, 2.);
        cairo_translate(cr, 0.0f, ht*.5f);
        draw_slider_line(cr, pos, 0.0f, scale, width, height-ht*0.5f);
        cairo_stroke(cr);
        cairo_restore(cr);

        // draw mouse over indicator line
        cairo_save(cr);
        cairo_set_source_rgb(cr, .6, .6, .6);
        cairo_set_line_width(cr, 2.);
        const float mouse_off = get_slider_line_offset(pos, scale, w->mouse_x/width, w->mouse_y/height);
        cairo_translate(cr, 0.0f, ht*.5f);
        draw_slider_line(cr, pos, mouse_off, scale, width, height-ht*0.5f);
        cairo_stroke(cr);
        cairo_restore(cr);

        // DEBUG:
        // cairo_set_source_rgb(cr, .6, .6, .6);
        // cairo_set_line_width(cr, 1.);
        // cairo_arc(cr, w->mouse_x, w->mouse_y, 3.0f, 0.0f, M_PI);
        // cairo_stroke(cr);

        // draw indicator
        cairo_save(cr);
        cairo_set_source_rgb(cr, .6, .6, .6);
        cairo_set_line_width(cr, 1.);
        cairo_translate(cr, pos*wd, ht*.5f);
        draw_equilateral_triangle(cr, ht*0.38f);
        cairo_fill(cr);
        cairo_restore(cr);

        // draw numerical value:
        cairo_save(cr);
        cairo_text_extents_t ext;
        cairo_set_source_rgb(cr, 1., 1., 1.);
        cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, .8*ht);
        cairo_text_extents (cr, _("0.66"), &ext);
        cairo_move_to (cr, wd-4-ht-ext.width, ht*0.8);
        cairo_show_text(cr, _("0.66"));
        cairo_restore(cr);
      }
      break;
    case DT_BAUHAUS_COMBOBOX:
      // TODO: combo box: draw label top left + options below
      // TODO: need mouse over highlights
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
dt_iop_bauhaus_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_bauhaus_gui_data_t *g = (dt_iop_bauhaus_gui_data_t *)self->gui_data;
  const int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  dt_bauhaus_widget_t *w = g->slider;

  dt_bauhaus_clear(w, cr);
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
        cairo_text_extents (cr, _("complicated setting"), &ext);
        cairo_move_to (cr, width-4-height-ext.width, height*0.8);
        cairo_show_text(cr, _("complicated setting"));
      }
      break;
    case DT_BAUHAUS_SLIDER:
      {
        const float pos = 0.66f; // dt_bauhaus_slider_get()
        // draw scale indicator
        cairo_save(cr);
        cairo_set_source_rgb(cr, .6, .6, .6);
        cairo_translate(cr, pos*width, height*.5f);
        draw_equilateral_triangle(cr, height*0.38f);
        cairo_fill(cr);
        cairo_restore(cr);

        // TODO: merge that text with combo
        cairo_text_extents_t ext;
        cairo_set_source_rgb(cr, 1., 1., 1.);
        cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size (cr, .8*height);
        cairo_text_extents (cr, _("0.66"), &ext);
        cairo_move_to (cr, width-4-height-ext.width, height*0.8);
        cairo_show_text(cr, _("0.66"));
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

static gboolean
dt_iop_bauhaus_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_bauhaus_widget_t *w = (dt_bauhaus_widget_t *)user_data;
  // remember mouse position for motion effects in draw
  w->mouse_x = event->x;
  w->mouse_y = event->y;
  gtk_widget_queue_draw(w->popup_area);
  return TRUE;
}

static gboolean
dt_iop_bauhaus_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_bauhaus_gui_data_t *c = (dt_iop_bauhaus_gui_data_t *)self->gui_data;
  dt_iop_request_focus(self);
  gint wx, wy;
  gdk_window_get_origin (gtk_widget_get_window (self->widget), &wx, &wy);
  gtk_window_move(GTK_WINDOW(c->popup_window), wx, wy);
  GtkAllocation tmp;
  gtk_widget_get_allocation(self->widget, &tmp);
  gtk_widget_set_size_request(c->popup_area, tmp.width, tmp.width);
  gtk_widget_show_all(c->popup_window);
  return TRUE;
}

static gboolean
dt_iop_bauhaus_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_bauhaus_gui_data_t *c = (dt_iop_bauhaus_gui_data_t *)self->gui_data;
  gtk_widget_hide(c->popup_window);
  return TRUE;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_bauhaus_gui_data_t));
  dt_iop_bauhaus_gui_data_t *c = (dt_iop_bauhaus_gui_data_t *)self->gui_data;

  self->widget = gtk_drawing_area_new();
  GtkWidget *area = self->widget;
  gtk_widget_set_size_request(area, 260, 18);
  g_object_set (GTK_OBJECT(area), "tooltip-text", _("smart tooltip"), (char *)NULL);

  c->popup_area = gtk_drawing_area_new();
  gtk_widget_set_size_request(c->popup_area, 300, 300);
  c->popup_window = gtk_window_new(GTK_WINDOW_POPUP);
  gtk_window_set_resizable(GTK_WINDOW(c->popup_window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(c->popup_window), 260, 260);
  // gtk_window_set_modal(GTK_WINDOW(c->popup_window), TRUE);
  // gtk_window_set_decorated(GTK_WINDOW(c->popup_window), FALSE);

  // for pie menue:
  // gtk_window_set_position(GTK_WINDOW(c->popup_window), GTK_WIN_POS_MOUSE);// | GTK_WIN_POS_CENTER);
  // gtk_window_set_transient_for(GTK_WINDOW(c->popup_window), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_container_add(GTK_CONTAINER(c->popup_window), c->popup_area);
  gtk_window_set_title(GTK_WINDOW(c->popup_window), _("dtgtk control popup"));
  gtk_window_set_keep_above(GTK_WINDOW(c->popup_window), TRUE);
  gtk_window_set_gravity(GTK_WINDOW(c->popup_window), GDK_GRAVITY_STATIC);

  // TODO: move most of this into slider_new!
  c->slider = dt_bauhaus_slider_new();
  c->slider->area = area;
  c->slider->popup_area = c->popup_area;
  c->slider->module = self;

  gtk_widget_add_events(GTK_WIDGET(area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  gtk_widget_add_events(GTK_WIDGET(c->popup_area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect (G_OBJECT (area), "expose-event",
                    G_CALLBACK (dt_iop_bauhaus_expose), self);
  g_signal_connect (G_OBJECT (c->popup_area), "expose-event",
                    G_CALLBACK (dt_bauhaus_popup_expose), c->slider);
  g_signal_connect (G_OBJECT (area), "button-press-event",
                    G_CALLBACK (dt_iop_bauhaus_button_press), self);
  // g_signal_connect (G_OBJECT (area), "button-release-event",
                    // G_CALLBACK (dt_iop_bauhaus_button_release), self);
  g_signal_connect (G_OBJECT (c->popup_area), "motion-notify-event",
                    G_CALLBACK (dt_iop_bauhaus_motion_notify), c->slider);
  g_signal_connect (G_OBJECT (area), "leave-notify-event",
                    G_CALLBACK (dt_iop_bauhaus_leave_notify), self);


  g_signal_connect (G_OBJECT (c->popup_area), "button-release-event",
                    G_CALLBACK (dt_iop_bauhaus_button_release), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  // dt_iop_bauhaus_gui_data_t *c = (dt_iop_bauhaus_gui_data_t *)self->gui_data;
  free(self->gui_data);
  self->gui_data = NULL;
}

