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
  int zoom_w, zoom_h;
  unsigned char* buffer;
  int wd;
  int ht;
  int timestamp;
} dt_lib_navigation_t;


/* expose function for navigation module */
static gboolean _lib_navigation_draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data);
/* motion notify callback handler*/
static gboolean _lib_navigation_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event,
                                                       gpointer user_data);
/* button press callback */
static gboolean _lib_navigation_button_press_callback(GtkWidget *widget, GdkEventButton *event,
                                                      gpointer user_data);
/* button release callback */
static gboolean _lib_navigation_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                        gpointer user_data);
/* leave notify callback */
static gboolean _lib_navigation_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                      gpointer user_data);

/* helper function for position set */
static void _lib_navigation_set_position(struct dt_lib_module_t *self, double x, double y, int wd, int ht);

const char *name()
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
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)g_malloc0(sizeof(dt_lib_navigation_t));
  self->data = (void *)d;

  d->buffer = NULL;
  d->wd = -1;
  d->ht = -1;
  d->timestamp = -1;

  /* create drawingarea */
  self->widget = gtk_drawing_area_new();
  gtk_widget_set_events(self->widget, GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK
                                      | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK
                                      | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK);

  /* connect callbacks */
  gtk_widget_set_app_paintable(self->widget, TRUE);
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(_lib_navigation_draw_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "button-press-event",
                   G_CALLBACK(_lib_navigation_button_press_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "button-release-event",
                   G_CALLBACK(_lib_navigation_button_release_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "motion-notify-event",
                   G_CALLBACK(_lib_navigation_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(self->widget), "leave-notify-event",
                   G_CALLBACK(_lib_navigation_leave_notify_callback), self);

  /* set size of navigation draw area */
  int panel_width = dt_conf_get_int("panel_width");
  gtk_widget_set_size_request(self->widget, -1, panel_width * .5);

  /* connect a redraw callback to control draw all and preview pipe finish signals */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                            G_CALLBACK(_lib_navigation_control_redraw_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(_lib_navigation_control_redraw_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* disconnect from signal */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_navigation_control_redraw_callback), self);

  dt_lib_navigation_t *d = self->data;
  g_free(d->buffer);

  g_free(self->data);
  self->data = NULL;
}



static gboolean _lib_navigation_draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)self->data;

  const int inset = DT_NAVIGATION_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;

  dt_develop_t *dev = darktable.develop;

  /* double buffering of image data: only take new data if valid */
  if(dev->preview_pipe->backbuf && dev->preview_status == DT_DEV_PIXELPIPE_VALID)
  {
    /* re-allocate in case of changed image dimensions */
    if(d->buffer == NULL || dev->preview_pipe->backbuf_width != d->wd || dev->preview_pipe->backbuf_height != d->ht)
    {
      g_free(d->buffer);
      d->wd = dev->preview_pipe->backbuf_width;
      d->ht = dev->preview_pipe->backbuf_height;
      d->buffer = g_malloc0((size_t)d->wd * d->ht * 4 * sizeof(unsigned char));
    }

    /* update buffer if new data is available */
    if(d->buffer && dev->preview_pipe->input_timestamp > d->timestamp)
    {
      dt_pthread_mutex_t *mutex = &dev->preview_pipe->backbuf_mutex;
      dt_pthread_mutex_lock(mutex);
      memcpy(d->buffer, dev->preview_pipe->backbuf, (size_t)d->wd * d->ht * 4 * sizeof(unsigned char));
      d->timestamp = dev->preview_pipe->input_timestamp;
      dt_pthread_mutex_unlock(mutex);
    }
  }

  /* get the current style */
  GdkRGBA color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  GtkStateFlags state = gtk_widget_get_state_flags(widget);
  gtk_style_context_get_background_color(context, state, &color);
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  /* fill background */
  gdk_cairo_set_source_rgba(cr, &color);
  cairo_paint(cr);

  width -= 2 * inset;
  height -= 2 * inset;
  cairo_translate(cr, inset, inset);

  /* draw navigation image if available */
  if(d->buffer)
  {
    cairo_save(cr);
    const int wd = d->wd;
    const int ht = d->ht;
    const float scale = fminf(width / (float)wd, height / (float)ht);

    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    cairo_surface_t *surface
        = cairo_image_surface_create_for_data(d->buffer, CAIRO_FORMAT_RGB24, wd, ht, stride);
    cairo_translate(cr, width / 2.0, height / 2.0f);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -.5f * wd, -.5f * ht);

    // draw shadow around
    float alpha = 1.0f;
    for(int k = 0; k < 4; k++)
    {
      cairo_rectangle(cr, -k / scale, -k / scale, wd + 2 * k / scale, ht + 2 * k / scale);
      cairo_set_source_rgba(cr, 0, 0, 0, alpha);
      alpha *= 0.6f;
      cairo_fill(cr);
    }

    cairo_rectangle(cr, 0, 0, wd - 2, ht - 1);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    cairo_fill(cr);
    cairo_surface_destroy(surface);

    // draw box where we are
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    int closeup = dt_control_get_dev_closeup();
    float zoom_x = dt_control_get_dev_zoom_x();
    float zoom_y = dt_control_get_dev_zoom_y();
    const float min_scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, closeup ? 2.0 : 1.0, 0);
    const float cur_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2.0 : 1.0, 0);
    // avoid numerical instability for small resolutions:
    double h, w;
    if(cur_scale > min_scale)
    {
      float boxw = 1, boxh = 1;
      dt_dev_check_zoom_bounds(darktable.develop, &zoom_x, &zoom_y, zoom, closeup, &boxw, &boxh);
      cairo_translate(cr, wd * (.5f + zoom_x), ht * (.5f + zoom_y));
      cairo_set_source_rgb(cr, 0., 0., 0.);
      cairo_set_line_width(cr, 1.f / scale);
      boxw *= wd;
      boxh *= ht;
      cairo_rectangle(cr, -boxw / 2 - 1, -boxh / 2 - 1, boxw + 2, boxh + 2);
      cairo_stroke(cr);
      cairo_set_source_rgb(cr, 1., 1., 1.);
      cairo_rectangle(cr, -boxw / 2, -boxh / 2, boxw, boxh);
      cairo_stroke(cr);
    }
    cairo_restore(cr);
    if(fabsf(cur_scale - min_scale) > 0.001f)
    {
      /* Zoom % */
      cairo_translate(cr, 0, height);
      cairo_set_source_rgba(cr, 1., 1., 1., 0.5);
      cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, DT_PIXEL_APPLY_DPI(11));

      char zoomline[5];
      snprintf(zoomline, sizeof(zoomline), "%.0f%%", cur_scale * 100);

      cairo_text_extents_t ext;
      cairo_text_extents(cr, zoomline, &ext);
      h = d->zoom_h = ext.height;
      w = d->zoom_w = ext.width;

      cairo_move_to(cr, width - w - h * 1.1, 0);

      cairo_save(cr);
      cairo_set_line_width(cr, 2.0);
      gdk_cairo_set_source_rgba(cr, &color);
      cairo_text_path(cr, zoomline);
      cairo_stroke_preserve(cr);
      cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
      cairo_fill(cr);
      cairo_restore(cr);
    }
    else
    {
      // draw the zoom-to-fit icon
      cairo_translate(cr, 0, height);
      cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
      cairo_text_extents_t ext;
      cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, DT_PIXEL_APPLY_DPI(11));
      cairo_text_extents(cr, "100%", &ext); // dummy text, just to get the height
      h = d->zoom_h = ext.height;
      w = h * 1.5;
      float sp = h * 0.6;
      d->zoom_w = w + sp;

      cairo_move_to(cr, width - w - h - sp, -1.0 * h);
      cairo_rectangle(cr, width - w - h - sp, -1.0 * h, w, h);
      cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
      cairo_fill(cr);

      cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
      cairo_move_to(cr, width - w * 0.8 - h - sp, -1.0 * h);
      cairo_line_to(cr, width - w - h - sp, -1.0 * h);
      cairo_line_to(cr, width - w - h - sp, -0.7 * h);
      cairo_stroke(cr);
      cairo_move_to(cr, width - w - h - sp, -0.3 * h);
      cairo_line_to(cr, width - w - h - sp, 0);
      cairo_line_to(cr, width - w * 0.8 - h - sp, 0);
      cairo_stroke(cr);
      cairo_move_to(cr, width - w * 0.2 - h - sp, 0);
      cairo_line_to(cr, width - h - sp, 0);
      cairo_line_to(cr, width - h - sp, -0.3 * h);
      cairo_stroke(cr);
      cairo_move_to(cr, width - h - sp, -0.7 * h);
      cairo_line_to(cr, width - h - sp, -1.0 * h);
      cairo_line_to(cr, width - w * 0.2 - h - sp, -1.0 * h);
      cairo_stroke(cr);
    }

    cairo_move_to(cr, width - 0.95 * h, -0.9 * h);
    cairo_line_to(cr, width - 0.05 * h, -0.9 * h);
    cairo_line_to(cr, width - 0.5 * h, -0.1 * h);
    cairo_fill(cr);
  }

  /* blit memsurface into widget */
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  return TRUE;
}

void _lib_navigation_set_position(dt_lib_module_t *self, double x, double y, int wd, int ht)
{
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)self->data;

  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_x = dt_control_get_dev_zoom_x();
  float zoom_y = dt_control_get_dev_zoom_y();

  if(d->dragging && zoom != DT_ZOOM_FIT)
  {
    const int inset = DT_NAVIGATION_INSET;
    const float width = wd - 2 * inset, height = ht - 2 * inset;
    const dt_develop_t *dev = darktable.develop;
    int iwd, iht;
    dt_dev_get_processed_size(dev, &iwd, &iht);
    zoom_x = fmaxf(
        -.5,
        fminf(((x - inset) / width - .5f) / (iwd * fminf(wd / (float)iwd, ht / (float)iht) / (float)wd), .5));
    zoom_y = fmaxf(
        -.5, fminf(((y - inset) / height - .5f) / (iht * fminf(wd / (float)iwd, ht / (float)iht) / (float)ht),
                   .5));
    dt_dev_check_zoom_bounds(darktable.develop, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    dt_control_set_dev_zoom_x(zoom_x);
    dt_control_set_dev_zoom_y(zoom_y);

    /* redraw myself */
    gtk_widget_queue_draw(self->widget);

    /* redraw pipe */
    dt_dev_invalidate(darktable.develop);
    dt_control_queue_redraw_center();
  }
}

static gboolean _lib_navigation_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event,
                                                       gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  _lib_navigation_set_position(self, event->x, event->y, allocation.width, allocation.height);
  gint x, y; // notify gtk for motion_hint.
  gdk_window_get_device_position(event->window,
                                 gdk_device_manager_get_client_pointer(
                                     gdk_display_get_device_manager(gdk_window_get_display(event->window))),
                                 &x, &y, NULL);
  return TRUE;
}

static void _zoom_preset_change(int val)
{
  // dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_develop_t *dev = darktable.develop;
  if(!dev) return;
  dt_dev_zoom_t zoom;
  int closeup, procw, proch;
  float zoom_x, zoom_y;
  zoom = dt_control_get_dev_zoom();
  closeup = dt_control_get_dev_closeup();
  zoom_x = dt_control_get_dev_zoom_x();
  zoom_y = dt_control_get_dev_zoom_y();
  dt_dev_get_processed_size(dev, &procw, &proch);
  float scale = 0;
  zoom_x = 0.0f; //+= (1.0/scale)*(x - .5f*dev->width )/procw;
  zoom_y = 0.0f; //+= (1.0/scale)*(y - .5f*dev->height)/proch;
  if(val == 0)
  {
    scale = 0.5 * dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);
    zoom = DT_ZOOM_FREE;
  }
  else if(val == 1)
  {
    zoom = DT_ZOOM_FIT;
    scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);
  }
  else if(val == 2)
  {
    scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
    zoom = DT_ZOOM_1;
  }
  else if(val == 3)
  {
    scale = 2.0f;
    zoom = DT_ZOOM_FREE;
  }

  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
  dt_control_set_dev_zoom_scale(scale);
  dt_control_set_dev_zoom(zoom);
  dt_control_set_dev_closeup(closeup);
  dt_control_set_dev_zoom_x(zoom_x);
  dt_control_set_dev_zoom_y(zoom_y);
  dt_dev_invalidate(dev);
  dt_control_queue_redraw();
}

static void _zoom_preset_mini(GtkButton *button, gpointer user_data)
{
  _zoom_preset_change(0);
}
static void _zoom_preset_fit(GtkButton *button, gpointer user_data)
{
  _zoom_preset_change(1);
}
static void _zoom_preset_1(GtkButton *button, gpointer user_data)
{
  _zoom_preset_change(2);
}
static void _zoom_preset_2(GtkButton *button, gpointer user_data)
{
  _zoom_preset_change(3);
}

static gboolean _lib_navigation_button_press_callback(GtkWidget *widget, GdkEventButton *event,
                                                      gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)self->data;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int w = allocation.width;
  int h = allocation.height;
  if(event->x >= w - DT_NAVIGATION_INSET - d->zoom_h - d->zoom_w
     && event->y >= h - DT_NAVIGATION_INSET - d->zoom_h)
  {
    // we show the zoom menu
    GtkMenuShell *menu = GTK_MENU_SHELL(gtk_menu_new());
    GtkWidget *item;

    item = gtk_menu_item_new_with_label(_("small"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_zoom_preset_mini), self);
    gtk_menu_shell_append(menu, item);

    item = gtk_menu_item_new_with_label(_("fit to screen"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_zoom_preset_fit), self);
    gtk_menu_shell_append(menu, item);

    item = gtk_menu_item_new_with_label(_("100%"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_zoom_preset_1), self);
    gtk_menu_shell_append(menu, item);

    item = gtk_menu_item_new_with_label(_("200%"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_zoom_preset_2), self);
    gtk_menu_shell_append(menu, item);

    gtk_widget_show_all(GTK_WIDGET(menu));
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 0, gtk_get_current_event_time());

    return TRUE;
  }
  d->dragging = 1;
  _lib_navigation_set_position(self, event->x, event->y, w, h);
  return TRUE;
}

static gboolean _lib_navigation_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                        gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)self->data;
  d->dragging = 0;

  return TRUE;
}

static gboolean _lib_navigation_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                      gpointer user_data)
{
  return TRUE;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
