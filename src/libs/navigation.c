/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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

#include "common/darktable.h"
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"

DT_MODULE(1)

#define DT_NAVIGATION_INSET 5

typedef struct dt_lib_navigation_t
{
  int dragging;
}
dt_lib_navigation_t;


/* expose function for navigation module */
static gboolean _lib_navigation_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
/* motion notify callback handler*/
static gboolean _lib_navigation_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
/* button press callback */
static gboolean _lib_navigation_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
/* button release callback */
static gboolean _lib_navigation_button_release_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
/* leave notify callback */
static gboolean _lib_navigation_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);

/* helper function for position set */
static void _lib_navigation_set_position(struct dt_lib_module_t *self, double x, double y, int wd, int ht);

const char* name()
{
  return _("navigation");
}

uint32_t views()
{
  return DT_VIEW_DARKROOM;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_TOP;
}

int expandable() 
{
  return 0;
}

int position()
{
  return 1001;
}


static void _lib_navigation_control_redraw_callback(gpointer instance, gpointer user_data) 
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_control_queue_redraw_widget(self->widget);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)g_malloc(sizeof(dt_lib_navigation_t));
  self->data = (void *)d;
  memset(d,0,sizeof(dt_lib_navigation_t));

  /* create drawingarea */
  self->widget = gtk_drawing_area_new();
  gtk_widget_set_events(self->widget,
                        GDK_EXPOSURE_MASK
                        | GDK_POINTER_MOTION_MASK
                        | GDK_POINTER_MOTION_HINT_MASK
                        | GDK_BUTTON_PRESS_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_STRUCTURE_MASK);
  
  /* connect callbacks */
  GTK_WIDGET_UNSET_FLAGS (self->widget, GTK_DOUBLE_BUFFERED);
  GTK_WIDGET_SET_FLAGS   (self->widget, GTK_APP_PAINTABLE);
  g_signal_connect (G_OBJECT (self->widget), "expose-event",
                    G_CALLBACK (_lib_navigation_expose_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "button-press-event",
                    G_CALLBACK (_lib_navigation_button_press_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "button-release-event",
                    G_CALLBACK (_lib_navigation_button_release_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "motion-notify-event",
                    G_CALLBACK (_lib_navigation_motion_notify_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "leave-notify-event",
                    G_CALLBACK (_lib_navigation_leave_notify_callback), self);

  /* set size of navigation draw area */
  int panel_width = dt_conf_get_int("panel_width");
  gtk_widget_set_size_request(self->widget, -1, panel_width*.5);

  /* connect a redraw callback to control draw all and preview pipe finish signals */
  dt_control_signal_connect(darktable.signals,DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED, G_CALLBACK(_lib_navigation_control_redraw_callback), self);
  dt_control_signal_connect(darktable.signals,DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, G_CALLBACK(_lib_navigation_control_redraw_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* disconnect from signal */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_navigation_control_redraw_callback), self);
   
  g_free(self->data);
  self->data = NULL;
}



static gboolean _lib_navigation_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{

  const int inset = DT_NAVIGATION_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;

  dt_develop_t *dev = darktable.develop;

  if (dev->preview_dirty) return FALSE;

  /* get the current style */
  GtkStyle *style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkWidget", GTK_TYPE_WIDGET);
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  
  /* fill background */ 
  cairo_set_source_rgb(cr, style->bg[0].red/65535.0, style->bg[0].green/65535.0, style->bg[0].blue/65535.0);
  cairo_paint(cr);

  width -= 2*inset;
  height -= 2*inset;
  cairo_translate(cr, inset, inset);

  /* draw navigation image if available */
  if(dev->preview_pipe->backbuf && !dev->preview_dirty)
  {
    dt_pthread_mutex_t *mutex = &dev->preview_pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);
    const int wd = dev->preview_pipe->backbuf_width;
    const int ht = dev->preview_pipe->backbuf_height;
    const float scale = fminf(width/(float)wd, height/(float)ht);

    const int stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, wd);
    cairo_surface_t *surface = cairo_image_surface_create_for_data (dev->preview_pipe->backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride);
    cairo_translate(cr, width/2.0, height/2.0f);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -.5f*wd, -.5f*ht);

    // draw shadow around
    float alpha = 1.0f;
    for(int k=0; k<4; k++)
    {
      cairo_rectangle(cr, -k/scale, -k/scale, wd + 2*k/scale, ht + 2*k/scale);
      cairo_set_source_rgba(cr, 0, 0, 0, alpha);
      alpha *= 0.6f;
      cairo_fill(cr);
    }

    cairo_rectangle(cr, 0, 0, wd-2, ht-1);
    cairo_set_source_surface (cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    cairo_fill(cr);
    cairo_surface_destroy (surface);

    dt_pthread_mutex_unlock(mutex);

    // draw box where we are
    dt_dev_zoom_t zoom;
    int closeup;
    float zoom_x, zoom_y;
    DT_CTL_GET_GLOBAL(zoom, dev_zoom);
    DT_CTL_GET_GLOBAL(closeup, dev_closeup);
    DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
    DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
    const float min_scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, closeup ? 2.0 : 1.0, 0);
    const float cur_scale = dt_dev_get_zoom_scale(dev, zoom,        closeup ? 2.0 : 1.0, 0);
    // avoid numerical instability for small resolutions:
    if(cur_scale > min_scale+0.001)
    {
      float boxw = 1, boxh = 1;
      dt_dev_check_zoom_bounds(darktable.develop, &zoom_x, &zoom_y, zoom, closeup, &boxw, &boxh);

      cairo_translate(cr, wd*(.5f+zoom_x), ht*(.5f+zoom_y));
      cairo_set_source_rgb(cr, 0., 0., 0.);
      cairo_set_line_width(cr, 1.f/scale);
      boxw *= wd;
      boxh *= ht;
      cairo_rectangle(cr, -boxw/2-1, -boxh/2-1, boxw+2, boxh+2);
      cairo_stroke(cr);
      cairo_set_source_rgb(cr, 1., 1., 1.);
      cairo_rectangle(cr, -boxw/2, -boxh/2, boxw, boxh);
      cairo_stroke(cr);
    }
  }

  /* blit memsurface into widget */
  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  
  return TRUE;
}

void _lib_navigation_set_position(dt_lib_module_t *self, double x, double y, int wd, int ht)
{
  dt_lib_navigation_t *d = ( dt_lib_navigation_t *)self->data;

  dt_dev_zoom_t zoom;
  int closeup;
  float zoom_x, zoom_y;
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);

  if(d->dragging && zoom != DT_ZOOM_FIT)
  {
    const int inset = DT_NAVIGATION_INSET;
    const float width = wd - 2*inset, height = ht - 2*inset;
    const dt_develop_t *dev = darktable.develop;
    int iwd, iht;
    dt_dev_get_processed_size(dev, &iwd, &iht);
    zoom_x = fmaxf(-.5, fminf(((x-inset)/width  - .5f)/(iwd*fminf(wd/(float)iwd, ht/(float)iht)/(float)wd), .5));
    zoom_y = fmaxf(-.5, fminf(((y-inset)/height - .5f)/(iht*fminf(wd/(float)iwd, ht/(float)iht)/(float)ht), .5));
    dt_dev_check_zoom_bounds(darktable.develop, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
    DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);

    /* redraw myself */
    gtk_widget_queue_draw(self->widget);

    /* redraw pipe */
    dt_dev_invalidate(darktable.develop);
    dt_control_queue_redraw_center();
  }
}

static gboolean _lib_navigation_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  _lib_navigation_set_position(self, event->x, event->y, widget->allocation.width, widget->allocation.height);
  gint x, y; // notify gtk for motion_hint.
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

static gboolean _lib_navigation_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)self->data;
  d->dragging = 1;
  _lib_navigation_set_position(self, event->x, event->y, widget->allocation.width, widget->allocation.height);
  return TRUE;
}

static gboolean _lib_navigation_button_release_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)self->data;
  d->dragging = 0;

  return TRUE;
}

static gboolean _lib_navigation_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  return TRUE;
}
