/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include <string.h>
#include "gui/histogram.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "develop/develop.h"
#include "control/control.h"

#define DT_HIST_INSET 5


void dt_gui_histogram_init(dt_gui_histogram_t *n, GtkWidget *widget)
{
  n->highlight = 0;
  n->dragging = 0;
  n->exposure = NULL;

  g_object_set(G_OBJECT(widget), "tooltip-text",
               _("drag to change exposure,\ndoubleclick resets"), (char *)NULL);
  g_signal_connect (G_OBJECT (widget), "expose-event",
                    G_CALLBACK (dt_gui_histogram_expose), n);
  g_signal_connect (G_OBJECT (widget), "button-press-event",
                    G_CALLBACK (dt_gui_histogram_button_press), n);
  g_signal_connect (G_OBJECT (widget), "button-release-event",
                    G_CALLBACK (dt_gui_histogram_button_release), n);
  g_signal_connect (G_OBJECT (widget), "motion-notify-event",
                    G_CALLBACK (dt_gui_histogram_motion_notify), n);
  g_signal_connect (G_OBJECT (widget), "leave-notify-event",
                    G_CALLBACK (dt_gui_histogram_leave_notify), n);
  g_signal_connect (G_OBJECT (widget), "enter-notify-event",
                    G_CALLBACK (dt_gui_histogram_enter_notify), n);
  g_signal_connect (G_OBJECT (widget), "scroll-event",
                    G_CALLBACK (dt_gui_histogram_scroll), n);

  gtk_widget_add_events(widget, GDK_LEAVE_NOTIFY_MASK | GDK_ENTER_NOTIFY_MASK | GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_SCROLL);
}

void dt_gui_histogram_cleanup(dt_gui_histogram_t *n) {}

gboolean dt_gui_histogram_expose(GtkWidget *widget, GdkEventExpose *event, dt_gui_histogram_t *n)
{
  dt_develop_t *dev = darktable.develop;
  float *hist = dev->histogram;
  float hist_max = dev->histogram_max;
  const int inset = DT_HIST_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  GtkStyle *style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkWidget", GTK_TYPE_WIDGET);
  cairo_set_source_rgb(cr, style->bg[0].red/65535.0, style->bg[0].green/65535.0, style->bg[0].blue/65535.0);
  cairo_paint(cr);

  cairo_translate(cr, 4*inset, inset);
  width -= 2*4*inset;
  height -= 2*inset;

#if 1
  // draw shadow around
  float alpha = 1.0f;
  cairo_set_line_width(cr, 0.2);
  for(int k=0; k<inset; k++)
  {
    cairo_rectangle(cr, -k, -k, width + 2*k, height + 2*k);
    cairo_set_source_rgba(cr, 0, 0, 0, alpha);
    alpha *= 0.5f;
    cairo_fill(cr);
  }
  cairo_set_line_width(cr, 1.0);
#else
  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgb (cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);
#endif

  cairo_rectangle(cr, 0, 0, width, height);
  cairo_clip(cr);

  cairo_set_source_rgb (cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);
  if(n->highlight == 1)
  {
    cairo_set_source_rgb (cr, .5, .5, .5);
    cairo_rectangle(cr, 0, 0, .2*width, height);
    cairo_fill(cr);
  }
  else if(n->highlight == 2)
  {
    cairo_set_source_rgb (cr, .5, .5, .5);
    cairo_rectangle(cr, 0.2*width, 0, width, height);
    cairo_fill(cr);
  }

  // draw grid
  cairo_set_line_width(cr, .4);
  cairo_set_source_rgb (cr, .1, .1, .1);
  dt_draw_grid(cr, 4, 0, 0, width, height);

  if(hist_max > 0)
  {
    cairo_save(cr);
    // cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    cairo_translate(cr, 0, height);
    cairo_scale(cr, width/63.0, -(height-10)/hist_max);
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
    // cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_line_width(cr, 1.);
    cairo_set_source_rgba(cr, 1., 0., 0., 0.2);
    dt_gui_histogram_draw_8(cr, hist, 0);
    cairo_set_source_rgba(cr, 0., 1., 0., 0.2);
    dt_gui_histogram_draw_8(cr, hist, 1);
    cairo_set_source_rgba(cr, 0., 0., 1., 0.2);
    dt_gui_histogram_draw_8(cr, hist, 2);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    // cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
    cairo_restore(cr);
  }

  if(dev->image)
  {
    cairo_set_source_rgb(cr, .25, .25, .25);
    cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size (cr, .1*height);

    char exifline[50];
    cairo_move_to (cr, .02*width, .98*height);
    dt_image_print_exif(dev->image, exifline, 50);
    cairo_show_text(cr, exifline);
    /*cairo_text_path(cr, exifline);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_stroke(cr);*/
  }

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

gboolean dt_gui_histogram_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_gui_histogram_t *n = (dt_gui_histogram_t *)user_data;
  if(n->dragging && n->highlight == 2 && n->exposure && n->set_white)
  {
    float white = n->white - (event->x - n->button_down_x)*
                  1.0f/(float)widget->allocation.width;
    n->set_white(n->exposure, white);
  }
  else if(n->dragging && n->highlight == 1 && n->exposure && n->set_black)
  {
    float black = n->black - (event->x - n->button_down_x)*
                  .1f/(float)widget->allocation.width;
    n->set_black(n->exposure, black);
  }
  else
  {
    const float offs = 4*DT_HIST_INSET;
    const float pos = (event->x-offs)/(float)(widget->allocation.width - 2*offs);
    if(pos < 0 || pos > 1.0);
    else if(pos < 0.2)
    {
      n->highlight = 1;
      g_object_set(G_OBJECT(widget), "tooltip-text", _("drag to change black point,\ndoubleclick resets"), (char *)NULL);
    }
    else
    {
      n->highlight = 2;
      g_object_set(G_OBJECT(widget), "tooltip-text", _("drag to change exposure,\ndoubleclick resets"), (char *)NULL);
    }
    gtk_widget_queue_draw(widget);
  }
  gint x, y; // notify gtk for motion_hint.
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

gboolean dt_gui_histogram_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_gui_histogram_t *n = (dt_gui_histogram_t *)user_data;
  if(event->type == GDK_2BUTTON_PRESS && n->exposure)
  {
    memcpy(n->exposure->params, n->exposure->default_params, n->exposure->params_size);
    n->exposure->gui_update(n->exposure);
    dt_dev_add_history_item(n->exposure->dev, n->exposure, TRUE);
  }
  else
  {
    n->dragging = 1;
    if(n->exposure && n->highlight == 2 && n->get_white) n->white = n->get_white(n->exposure);
    if(n->exposure && n->highlight == 1 && n->get_black) n->black = n->get_black(n->exposure);
    n->button_down_x = event->x;
    n->button_down_y = event->y;
  }
  return TRUE;
}

gboolean dt_gui_histogram_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_gui_histogram_t *n = (dt_gui_histogram_t *)user_data;
  if(n->exposure && event->direction == GDK_SCROLL_UP && n->highlight ==2) n->set_white(n->exposure,n->get_white(n->exposure)-0.1);
  if(n->exposure && event->direction == GDK_SCROLL_DOWN && n->highlight ==2) n->set_white(n->exposure,n->get_white(n->exposure)+0.1);
  if(n->exposure && event->direction == GDK_SCROLL_UP && n->highlight ==1) n->set_black(n->exposure,n->get_black(n->exposure)-0.005);
  if(n->exposure && event->direction == GDK_SCROLL_DOWN && n->highlight ==1) n->set_black(n->exposure,n->get_black(n->exposure)+0.005);
  return TRUE;
}

gboolean dt_gui_histogram_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_gui_histogram_t *n = (dt_gui_histogram_t *)user_data;
  n->dragging = 0;
  return TRUE;
}

gboolean dt_gui_histogram_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_change_cursor(GDK_HAND1);
  return TRUE;
}

gboolean dt_gui_histogram_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_gui_histogram_t *n = (dt_gui_histogram_t *)user_data;
  n->dragging = 0;
  n->highlight = 0;
  dt_control_change_cursor(GDK_LEFT_PTR);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

void dt_gui_histogram_draw_8(cairo_t *cr, float *hist, int32_t channel)
{
  cairo_move_to(cr, 0, 0);
  for(int k=0; k<64; k++)
    cairo_line_to(cr, k, hist[4*k+channel]);
  cairo_line_to(cr, 63, 0);
  cairo_close_path(cr);
  cairo_fill(cr);
}
