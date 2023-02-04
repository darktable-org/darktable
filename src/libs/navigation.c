/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/drawingarea.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#define DT_NAVIGATION_INSET 5

typedef struct dt_lib_navigation_t
{
  int dragging;
  int zoom_w, zoom_h;
  GtkWidget *zoom;
} dt_lib_navigation_t;


/* expose function for navigation module */
static gboolean _lib_navigation_draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data);
/* motion notify callback handler*/
static gboolean _lib_navigation_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event,
                                                       gpointer user_data);
/* button press callback */
static gboolean _lib_navigation_button_press_callback(GtkWidget *widget, GdkEvent *event,
                                                      gpointer user_data);
/* button release callback */
static gboolean _lib_navigation_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                        gpointer user_data);
/* leave notify callback */
static gboolean _lib_navigation_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                      gpointer user_data);

/* helper function for position set */
static void _lib_navigation_set_position(struct dt_lib_module_t *self, double x, double y, int wd, int ht);

const char *name(dt_lib_module_t *self)
{
  return _("navigation");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_TOP;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 1001;
}


static void _lib_navigation_control_redraw_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_control_queue_redraw_widget(gtk_bin_get_child(GTK_BIN(self->widget)));
}


static void _lib_navigation_collapse_callback(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.lib->proxy.navigation.module;

  // Get the state
  const gboolean visible = dt_lib_is_visible(self);

  // Inverse the visibility
  dt_lib_set_visible(self, !visible);
}

static void _zoom_changed(GtkWidget *widget, gpointer user_data);

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)g_malloc0(sizeof(dt_lib_navigation_t));
  self->data = (void *)d;

  /* create drawingarea */
  GtkWidget *thumbnail = dt_ui_resize_wrap(NULL, 0, "plugins/darkroom/navigation/aspect_percent");
  gtk_widget_set_tooltip_text(thumbnail, _("navigation\nclick or drag to position zoomed area in center view"));

  /* connect callbacks */
  gtk_widget_set_app_paintable(thumbnail, TRUE);
  g_signal_connect(G_OBJECT(thumbnail), "draw", G_CALLBACK(_lib_navigation_draw_callback), self);
  g_signal_connect(G_OBJECT(thumbnail), "button-press-event",
                   G_CALLBACK(_lib_navigation_button_press_callback), self);
  g_signal_connect(G_OBJECT(thumbnail), "scroll-event",
                   G_CALLBACK(_lib_navigation_button_press_callback), self);
  g_signal_connect(G_OBJECT(thumbnail), "button-release-event",
                   G_CALLBACK(_lib_navigation_button_release_callback), self);
  g_signal_connect(G_OBJECT(thumbnail), "motion-notify-event",
                   G_CALLBACK(_lib_navigation_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(thumbnail), "leave-notify-event",
                   G_CALLBACK(_lib_navigation_leave_notify_callback), self);

  /* set size of navigation draw area */
  // gtk_widget_set_size_request(thumbnail, -1, DT_PIXEL_APPLY_DPI(175));
  gtk_widget_set_name(GTK_WIDGET(thumbnail), "navigation-module");
  dt_action_t *ac = dt_action_define(&darktable.view_manager->proxy.darkroom.view->actions, NULL,
                                     N_("hide navigation thumbnail"), thumbnail, NULL);
  dt_action_register(ac, NULL, _lib_navigation_collapse_callback, GDK_KEY_N, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  /* connect a redraw callback to control draw all and preview pipe finish signals */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(_lib_navigation_control_redraw_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_NAVIGATION_REDRAW,
                            G_CALLBACK(_lib_navigation_control_redraw_callback), self);

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->zoom, darktable.view_manager->proxy.darkroom.view,
                               NULL, N_("zoom"), _("image zoom level"),
                               -1, _zoom_changed, NULL,
                               N_("small"),
                               N_("fit"),
                               N_("fill"),
                               N_("50%"),
                               N_("100%"),
                               N_("200%"),
                               N_("400%"),
                               N_("800%"),
                               N_("1600%"));
  ac = dt_action_section(&darktable.view_manager->proxy.darkroom.view->actions, "zoom");
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_COMBO_SEPARATOR + 2, GDK_KEY_3, GDK_MOD1_MASK);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_COMBO_SEPARATOR + 3, GDK_KEY_2, GDK_MOD1_MASK);

  dt_bauhaus_combobox_set_editable(d->zoom, TRUE);
  DT_BAUHAUS_WIDGET(d->zoom)->show_label = FALSE;
  gtk_widget_set_halign(d->zoom, GTK_ALIGN_END);
  gtk_widget_set_valign(d->zoom, GTK_ALIGN_END);

  self->widget = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(self->widget), thumbnail);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->widget), d->zoom);
  dt_gui_add_class(self->widget, "dt_plugin_ui_main");

  darktable.lib->proxy.navigation.module = self;
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* disconnect from signal */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_navigation_control_redraw_callback), self);

  g_free(self->data);
  self->data = NULL;
}



static gboolean _lib_navigation_draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)self->data;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;

  dt_develop_t *dev = darktable.develop;

  /* get the current style */
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);

  /* draw navigation image if available */
  if(dev->preview_pipe->output_backbuf && dev->image_storage.id == dev->preview_pipe->output_imgid)
  {
    dt_pthread_mutex_t *mutex = &dev->preview_pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);

    cairo_save(cr);
    const int wd = dev->preview_pipe->output_backbuf_width;
    const int ht = dev->preview_pipe->output_backbuf_height;
    const float scale = fminf(width / (float)wd, height / (float)ht);

    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    cairo_surface_t *surface
        = cairo_image_surface_create_for_data(dev->preview_pipe->output_backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride);
    cairo_translate(cr, width / 2.0, height / 2.0f);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -.5f * wd, -.5f * ht);

    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_fill(cr);

    // draw box where we are
    dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
    int closeup = dt_control_get_dev_closeup();
    float zoom_x = dt_control_get_dev_zoom_x();
    float zoom_y = dt_control_get_dev_zoom_y();
    const float min_scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1<<closeup, 0);
    const float cur_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
    if(cur_scale > min_scale)
    {
      // Add a dark overlay on the picture to make it fade
      cairo_rectangle(cr, 0, 0, wd, ht);
      cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
      cairo_fill(cr);

      float boxw = 1, boxh = 1;
      dt_dev_check_zoom_bounds(darktable.develop, &zoom_x, &zoom_y, zoom, closeup, &boxw, &boxh);

      // Repaint the original image in the area of interest
      cairo_set_source_surface(cr, surface, 0, 0);
      cairo_translate(cr, wd * (.5f + zoom_x), ht * (.5f + zoom_y));
      boxw *= wd;
      boxh *= ht;
      cairo_rectangle(cr, -boxw / 2 - 1, -boxh / 2 - 1, boxw + 2, boxh + 2);
      cairo_clip_preserve(cr);
      cairo_fill_preserve(cr);

      // Paint the external border in black
      cairo_set_source_rgb(cr, 0., 0., 0.);
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
      cairo_stroke(cr);

      // Paint the internal border in white
      cairo_set_source_rgb(cr, 1., 1., 1.);
      cairo_rectangle(cr, -boxw / 2, -boxh / 2, boxw, boxh);
      cairo_stroke(cr);
    }
    cairo_restore(cr);

    gchar *zoomline = zoom == DT_ZOOM_FIT ? g_strdup(_("fit"))
                    : zoom == DT_ZOOM_FILL ? g_strdup(_("fill"))
                    : 0.5 * dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0)
                      == dt_dev_get_zoom_scale(dev, DT_ZOOM_FREE, 1.0, 0) ? g_strdup(_("small"))
                    : g_strdup_printf("%.0f%%", cur_scale * 100 * darktable.gui->ppd);
    ++darktable.gui->reset;
    dt_bauhaus_combobox_set(d->zoom, -1);
    if(!dt_bauhaus_combobox_set_from_text(d->zoom, zoomline))
      dt_bauhaus_combobox_set_text(d->zoom, zoomline);
    --darktable.gui->reset;
    g_free(zoomline);

    dt_pthread_mutex_unlock(mutex);
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

  if(d->dragging && zoom != DT_ZOOM_FIT)
  {
    const int inset = DT_NAVIGATION_INSET;
    const float width = wd - 2 * inset, height = ht - 2 * inset;
    const dt_develop_t *dev = darktable.develop;
    int iwd, iht;
    dt_dev_get_processed_size(dev, &iwd, &iht);
    float zoom_x = fmaxf(
        -.5,
        fminf(((x - inset) / width - .5f) / (iwd * fminf(wd / (float)iwd, ht / (float)iht) / (float)wd), .5));
    float zoom_y = fmaxf(
        -.5, fminf(((y - inset) / height - .5f) / (iht * fminf(wd / (float)iwd, ht / (float)iht) / (float)ht),
                   .5));
    dt_dev_check_zoom_bounds(darktable.develop, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    dt_control_set_dev_zoom_x(zoom_x);
    dt_control_set_dev_zoom_y(zoom_y);

    /* redraw myself */
    _lib_navigation_control_redraw_callback(NULL, self);

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
  return TRUE;
}

static void _zoom_changed(GtkWidget *widget, gpointer user_data)
{
  int val = dt_bauhaus_combobox_get(widget);
  if(val == -1 && 1 != sscanf(dt_bauhaus_combobox_get_text(widget), "%d", &val))
    return;

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
  const float ppd = darktable.gui->ppd;
  const gboolean low_ppd = (darktable.gui->ppd == 1);
  closeup = 0;
  if(val == 0u)
  {
    // small
    scale = 0.5 * dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);
    zoom = DT_ZOOM_FREE;
  }
  else if(val == 1u || val == -1u)
  {
    // fit to screen
    zoom = DT_ZOOM_FIT;
    scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, 1.0, 0);
  }
  else if(val == 2u)
  {
    // fit to screen
    zoom = DT_ZOOM_FILL;
    closeup = 0;
  }
  else if(val == 4u)
  {
    // 100%
    if(low_ppd == 1)
    {
      scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
      zoom = DT_ZOOM_1;
    }
    else
    {
      scale = 1.0f / ppd;
      zoom = DT_ZOOM_FREE;
    }
  }
  else if(val == 5u)
  {
    // 200%
    scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
    zoom = DT_ZOOM_1;
    if(low_ppd) closeup = 1;
  }
  else if(val == 3u)
  {
    // 50%
    scale = 0.5f / ppd;
    zoom = DT_ZOOM_FREE;
  }
  else if(val == 8u)
  {
    // 1600%
    scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
    zoom = DT_ZOOM_1;
    closeup = (low_ppd) ? 4 : 3;
  }
  else if(val == 6u)
  {
    // 400%
    scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
    zoom = DT_ZOOM_1;
    closeup = (low_ppd) ? 2 : 1;
  }
  else if(val == 7u)
  {
    // 800%
    scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_1, 1.0, 0);
    zoom = DT_ZOOM_1;
    closeup = (low_ppd) ? 3 : 2;
  }
  else
  {
    scale = val / 100.0f * ppd;
    zoom = DT_ZOOM_FREE;
  }

  // zoom_x = (1.0/(scale*(1<<closeup)))*(zoom_x - .5f*dev->width )/procw;
  // zoom_y = (1.0/(scale*(1<<closeup)))*(zoom_y - .5f*dev->height)/proch;

  dt_control_set_dev_zoom_scale(scale);
  dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
  dt_control_set_dev_zoom(zoom);
  dt_control_set_dev_closeup(closeup);
  dt_control_set_dev_zoom_x(zoom_x);
  dt_control_set_dev_zoom_y(zoom_y);
  dt_dev_invalidate(dev);
  dt_control_queue_redraw();
}

static gboolean _lib_navigation_button_press_callback(GtkWidget *widget, GdkEvent *event,
                                                      gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_navigation_t *d = (dt_lib_navigation_t *)self->data;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  if(event->type == GDK_BUTTON_PRESS && event->button.button == 1)
  {
    d->dragging = 1;
    _lib_navigation_set_position(self, event->button.x, event->button.y, allocation.width, allocation.height);

    return TRUE;
  }
  else
  {
    GtkWidget *center = dt_ui_center(darktable.gui->ui);
    GtkAllocation center_alloc;
    gtk_widget_get_allocation(center, &center_alloc);
    event->button.x *= (gdouble)center_alloc.width / allocation.width;
    event->button.y *= (gdouble)center_alloc.height / allocation.height;

    return gtk_widget_event(center, event);
  }
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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
