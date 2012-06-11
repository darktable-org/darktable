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
#include "gui/draw.h"

DT_MODULE(1)

#define DT_HIST_INSET 5

typedef struct dt_lib_histogram_t
{
  float white, black;
  int32_t dragging;
  int32_t button_down_x, button_down_y;
  int32_t highlight;
}
dt_lib_histogram_t;

static gboolean _lib_histogram_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
static gboolean _lib_histogram_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
static gboolean _lib_histogram_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean _lib_histogram_button_release_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean _lib_histogram_scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
static gboolean _lib_histogram_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean _lib_histogram_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);

const char* name()
{
  return _("histogram");
}

uint32_t views()
{
  return DT_VIEW_DARKROOM | DT_VIEW_TETHERING;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}

int expandable() 
{
  return 0;
}

int position()
{
  return 1001;
}


static void _lib_histogram_change_callback(gpointer instance, gpointer user_data) 
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_control_queue_redraw_widget(self->widget);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)g_malloc(sizeof(dt_lib_histogram_t));
  memset(d,0,sizeof(dt_lib_histogram_t));
  self->data = (void *)d;

  /* create drawingarea */
  self->widget = gtk_drawing_area_new();
  
  gtk_widget_add_events(self->widget, 
			GDK_LEAVE_NOTIFY_MASK | 
			GDK_ENTER_NOTIFY_MASK | 
			GDK_POINTER_MOTION_MASK | 
			GDK_BUTTON_PRESS_MASK | 
			GDK_BUTTON_RELEASE_MASK | 
			GDK_SCROLL);
  
  /* connect callbacks */
  g_object_set(G_OBJECT(self->widget), "tooltip-text",
               _("drag to change exposure,\ndoubleclick resets"), (char *)NULL);
  g_signal_connect (G_OBJECT (self->widget), "expose-event",
                    G_CALLBACK (_lib_histogram_expose_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "button-press-event",
                    G_CALLBACK (_lib_histogram_button_press_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "button-release-event",
                    G_CALLBACK (_lib_histogram_button_release_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "motion-notify-event",
                    G_CALLBACK (_lib_histogram_motion_notify_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "leave-notify-event",
                    G_CALLBACK (_lib_histogram_leave_notify_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "enter-notify-event",
                    G_CALLBACK (_lib_histogram_enter_notify_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "scroll-event",
                    G_CALLBACK (_lib_histogram_scroll_callback), self);

  /* set size of navigation draw area */
  int panel_width = dt_conf_get_int("panel_width");
  gtk_widget_set_size_request(self->widget, -1, panel_width*.5);

  /* connect to preview pipe finished  signal */
  dt_control_signal_connect(darktable.signals,DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, G_CALLBACK(_lib_histogram_change_callback), self);
 
 
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* disconnect callback from  signal */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_histogram_change_callback), self);

  g_free(self->data);
  self->data = NULL;
}

static gboolean _lib_histogram_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  dt_develop_t *dev = darktable.develop;
  float *hist = dev->histogram;
  float hist_max = dev->histogram_max;
  const int inset = DT_HIST_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  GtkStyle *style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkWidget", GTK_TYPE_WIDGET);
  if(!style) style = gtk_rc_get_style(widget);
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
  if(d->highlight == 1)
  {
    cairo_set_source_rgb (cr, .5, .5, .5);
    cairo_rectangle(cr, 0, 0, .2*width, height);
    cairo_fill(cr);
  }
  else if(d->highlight == 2)
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
    dt_draw_histogram_8(cr, hist, 0);
    cairo_set_source_rgba(cr, 0., 1., 0., 0.2);
    dt_draw_histogram_8(cr, hist, 1);
    cairo_set_source_rgba(cr, 0., 0., 1., 0.2);
    dt_draw_histogram_8(cr, hist, 2);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    // cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
    cairo_restore(cr);
  }

  cairo_set_source_rgb(cr, .25, .25, .25);
  cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size (cr, .1*height);

  char exifline[50];
  cairo_move_to (cr, .02*width, .98*height);
  dt_image_print_exif(&dev->image_storage, exifline, 50);
  cairo_show_text(cr, exifline);
  /*cairo_text_path(cr, exifline);
  cairo_fill_preserve(cr);
  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
  cairo_stroke(cr);*/

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean _lib_histogram_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  
  /* check if exposure hooks are available */
  gboolean hooks_available = dt_dev_exposure_hooks_available(darktable.develop);

  if(!hooks_available)
    return TRUE;

  if (d->dragging && d->highlight == 2)
  {
    float white = d->white - (event->x - d->button_down_x)*
                  1.0f/(float)widget->allocation.width;
    dt_dev_exposure_set_white(darktable.develop, white);
  }
  else if(d->dragging && d->highlight == 1)
  {
    float black = d->black - (event->x - d->button_down_x)*
                  .1f/(float)widget->allocation.width;
    dt_dev_exposure_set_black(darktable.develop, black);
  }
  else
  {
    const float offs = 4*DT_HIST_INSET;
    const float pos = (event->x-offs)/(float)(widget->allocation.width - 2*offs);
    if(pos < 0 || pos > 1.0);
    else if(pos < 0.2)
    {
      d->highlight = 1;
      g_object_set(G_OBJECT(widget), "tooltip-text", _("drag to change black point,\ndoubleclick resets"), (char *)NULL);
    }
    else
    {
      d->highlight = 2;
      g_object_set(G_OBJECT(widget), "tooltip-text", _("drag to change exposure,\ndoubleclick resets"), (char *)NULL);
    }
    gtk_widget_queue_draw(widget);
  }
  gint x, y; // notify gtk for motion_hint.
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

static gboolean _lib_histogram_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  /* check if exposure hooks are available */
  gboolean hooks_available = dt_dev_exposure_hooks_available(darktable.develop);

  if(!hooks_available)
    return TRUE;

  if(event->type == GDK_2BUTTON_PRESS)
  {
    dt_dev_exposure_reset_defaults(darktable.develop);
  }
  else
  {
    d->dragging = 1;

    if(d->highlight == 2)
      d->white = dt_dev_exposure_get_white(darktable.develop);

    if(d->highlight == 1)
      d->black = dt_dev_exposure_get_black(darktable.develop);

    d->button_down_x = event->x;
    d->button_down_y = event->y;
  }
  return TRUE;
}

static gboolean _lib_histogram_scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  float cw = dt_dev_exposure_get_white(darktable.develop);
  float cb = dt_dev_exposure_get_black(darktable.develop);

  if(event->direction == GDK_SCROLL_UP && d->highlight == 2)
    dt_dev_exposure_set_white(darktable.develop, cw-0.1);

  if(event->direction == GDK_SCROLL_DOWN && d->highlight == 2)
    dt_dev_exposure_set_white(darktable.develop, cw+0.1);

  if(event->direction == GDK_SCROLL_UP && d->highlight == 1) 
    dt_dev_exposure_set_black(darktable.develop, cb-0.005);

  if(event->direction == GDK_SCROLL_DOWN && d->highlight == 1)
    dt_dev_exposure_set_black(darktable.develop, cb+0.005); 

  return TRUE;
}

static gboolean _lib_histogram_button_release_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  d->dragging = 0;
  return TRUE;
}

static gboolean _lib_histogram_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_change_cursor(GDK_HAND1);
  return TRUE;
}

static gboolean _lib_histogram_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  d->dragging = 0;
  d->highlight = 0;
  dt_control_change_cursor(GDK_LEFT_PTR);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
