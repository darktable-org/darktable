#include "gui/histogram.h"
#include "develop/develop.h"
#include "develop/imageop.h"

#define DT_HIST_INSET 5

void dt_gui_histogram_init(dt_gui_histogram_t *n, GtkWidget *widget)
{
  n->dragging = 0;
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
}

void dt_gui_histogram_cleanup(dt_gui_histogram_t *n) {}

gboolean dt_gui_histogram_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  const int inset = DT_HIST_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset; height -= 2*inset;

#if 0
  // draw shadow around
  float alpha = 1.0f;
  for(int k=0;k<inset;k++)
  {
    cairo_rectangle(cr, -k, -k, width + 2*k, height + 2*k);
    cairo_set_source_rgba(cr, 0, 0, 0, alpha);
    alpha *= 0.6f;
    cairo_fill(cr);
  }
#else
  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgb (cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);
#endif

  cairo_set_source_rgb (cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw grid
  cairo_set_line_width(cr, .4);
  cairo_set_source_rgb (cr, .1, .1, .1);
  for(int k=1;k<4;k++)
  {
    cairo_move_to(cr, k/4.0*width, 0); cairo_line_to(cr, k/4.0*width, height);
    cairo_stroke(cr);
    cairo_move_to(cr, 0, k/4.0*height); cairo_line_to(cr, width, k/4.0*height);
    cairo_stroke(cr);
  }
  
  dt_develop_t *dev = darktable.develop;
  uint32_t *hist, hist_max;
  hist = dev->histogram;
  hist_max = dev->histogram_max;
  if(hist_max > 0)
  {
    // cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    cairo_translate(cr, 0, height);
    cairo_scale(cr, 256.0/width, -height/(float)hist_max);
    cairo_set_line_width(cr, 1.);
    cairo_set_source_rgba(cr, 1., 0., 0., 0.5);
    dt_gui_histogram_draw_8(cr, hist, 0);
    cairo_set_source_rgba(cr, 0., 1., 0., 0.5);
    dt_gui_histogram_draw_8(cr, hist, 1);
    cairo_set_source_rgba(cr, 0., 0., 1., 0.5);
    dt_gui_histogram_draw_8(cr, hist, 2);
    // cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
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
  return TRUE;
}
gboolean dt_gui_histogram_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  return TRUE;
}
gboolean dt_gui_histogram_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  return TRUE;
}
gboolean dt_gui_histogram_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  return TRUE;
}

void dt_gui_histogram_draw_8(cairo_t *cr, uint32_t *hist, int32_t channel)
{
  cairo_move_to(cr, 0, 0);
  for(int k=0;k<256;k++)
  {
    // cairo_move_to(cr, k, 0);
    cairo_line_to(cr, k, hist[4*k+channel]);
    //cairo_stroke(cr);
  }
  cairo_line_to(cr, 255, 0);
  cairo_close_path(cr);
  cairo_fill(cr);
}
