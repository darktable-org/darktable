
#include "gui/gtk.h"
#include "gui/navigation.h"
#include "library/library.h"
#include "develop/develop.h"
#include "control/control.h"

#include <math.h>
#define DT_NAVIGATION_INSET 5


void dt_gui_navigation_init(dt_gui_navigation_t *n, GtkWidget *widget)
{
  n->dragging = 0;
  g_signal_connect (G_OBJECT (widget), "expose-event",
                    G_CALLBACK (dt_gui_navigation_expose), n);
  g_signal_connect (G_OBJECT (widget), "button-press-event",
                    G_CALLBACK (dt_gui_navigation_button_press), n);
  g_signal_connect (G_OBJECT (widget), "button-release-event",
                    G_CALLBACK (dt_gui_navigation_button_release), n);
  g_signal_connect (G_OBJECT (widget), "motion-notify-event",
                    G_CALLBACK (dt_gui_navigation_motion_notify), n);
  g_signal_connect (G_OBJECT (widget), "leave-notify-event",
                    G_CALLBACK (dt_gui_navigation_leave_notify), n);
}

void dt_gui_navigation_cleanup(dt_gui_navigation_t *n) {}

gboolean dt_gui_navigation_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  const int inset = DT_NAVIGATION_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
  cairo_paint(cr);

  width -= 2*inset; height -= 2*inset;
  cairo_translate(cr, inset, inset);

  dt_image_t *image = darktable.develop->image;
  dt_image_buffer_t mip = DT_IMAGE_NONE;
  int32_t iiwd, iiht;
  float iwd, iht;
  if(image)
  {
    mip = dt_image_get_matching_mip_size(image, width, height, &iiwd, &iiht);
    mip = dt_image_get(image, mip, 'r');
    dt_image_get_exact_mip_size(image, mip, &iwd, &iht);
    dt_image_get_mip_size(image, mip, &iiwd, &iiht);
  }
  if(mip != DT_IMAGE_NONE)
  {
    const float scale = fminf(width/iwd, height/iht);
    int32_t stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, iiwd);
    // cairo_save(cr);
    cairo_surface_t *surface = cairo_image_surface_create_for_data (image->mip[mip], CAIRO_FORMAT_RGB24, iiwd, iiht, stride); 
    cairo_translate(cr, width/2.0, height/2.0f);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -.5f*iwd, -.5f*iht);

    // draw shadow around
    float alpha = 1.0f;
    for(int k=0;k<4;k++)
    {
      cairo_rectangle(cr, -k/scale, -k/scale, iwd + 2*k/scale, iht + 2*k/scale);
      cairo_set_source_rgba(cr, 0, 0, 0, alpha);
      alpha *= 0.6f;
      cairo_fill(cr);
    }

    cairo_rectangle(cr, 0, 0, iwd, iht);
    // cairo_stroke_preserve(cr);
    cairo_set_source_surface (cr, surface, 0, 0);
    cairo_fill(cr);
    cairo_surface_destroy (surface);
    // cairo_restore(cr);
    // draw box where we are
    dt_dev_zoom_t zoom;
    int closeup;
    float zoom_x, zoom_y;
    DT_CTL_GET_GLOBAL(zoom, dev_zoom);
    DT_CTL_GET_GLOBAL(closeup, dev_closeup);
    DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
    DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
    if(zoom != DT_ZOOM_FIT)
    {
      float boxw = 1, boxh = 1;
      dt_dev_check_zoom_bounds(darktable.develop, &zoom_x, &zoom_y, zoom, closeup, &boxw, &boxh);
     
      DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
      DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);

      cairo_translate(cr, iwd*(.5f+zoom_x), iht*(.5f+zoom_y));
      cairo_set_source_rgb(cr, 0., 0., 0.);
      cairo_set_line_width(cr, 1.f);
      boxw *= iwd;
      boxh *= iht;
      cairo_rectangle(cr, -boxw/2-1, -boxh/2-1, boxw+2, boxh+2);
      cairo_stroke(cr);
      cairo_set_source_rgb(cr, 1., 1., 1.);
      cairo_rectangle(cr, -boxw/2, -boxh/2, boxw, boxh);
      cairo_stroke(cr);
    }
    dt_image_release(image, mip, 'r');
  }

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

void dt_gui_navigation_set_position(dt_gui_navigation_t *n, double x, double y, int wd, int ht)
{
  dt_dev_zoom_t zoom;
  int closeup;
  float zoom_x, zoom_y;
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);

  if(n->dragging && zoom != DT_ZOOM_FIT)
  {
    const int inset = DT_NAVIGATION_INSET;
    const float width = wd - 2*inset, height = ht - 2*inset;
    const float iwd = darktable.develop->image->width, iht = darktable.develop->image->height;
    zoom_x = fmaxf(-.5, fminf(((x-inset)/width  - .5f), .5))/(iwd*fminf(wd/iwd, ht/iht)/wd);
    zoom_y = fmaxf(-.5, fminf(((y-inset)/height - .5f), .5))/(iht*fminf(wd/iwd, ht/iht)/ht);
    dt_dev_check_zoom_bounds(darktable.develop, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
    DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);

    dt_dev_invalidate(darktable.develop);
    dt_control_gui_queue_draw();
  }
}

gboolean dt_gui_navigation_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_gui_navigation_t *n = (dt_gui_navigation_t *)user_data;
  dt_gui_navigation_set_position(n, event->x, event->y, widget->allocation.width, widget->allocation.height);
  gint x, y; // notify gtk for motion_hint.
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}
gboolean dt_gui_navigation_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_gui_navigation_t *n = (dt_gui_navigation_t *)user_data;
  n->dragging = 1;
  dt_gui_navigation_set_position(n, event->x, event->y, widget->allocation.width, widget->allocation.height);
  return TRUE;
}
gboolean dt_gui_navigation_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_gui_navigation_t *n = (dt_gui_navigation_t *)user_data;
  n->dragging = 0;
  return TRUE;
}
gboolean dt_gui_navigation_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  return TRUE;
}
void dt_gui_navigation_get_pos(dt_gui_navigation_t *n, float *x, float *y)
{
}
