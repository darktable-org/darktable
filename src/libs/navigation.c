/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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
static gboolean _lib_navigation_draw_callback(GtkWidget *widget,
                                              cairo_t *crf,
                                              gpointer user_data);
/* motion notify callback handler*/
static gboolean _lib_navigation_motion_notify_callback(GtkWidget *widget,
                                                       GdkEventMotion *event,
                                                       dt_lib_module_t *self);
/* button press callback */
static gboolean _lib_navigation_button_press_callback(GtkWidget *widget,
                                                      GdkEvent *event,
                                                      dt_lib_module_t *self);
/* button release callback */
static gboolean _lib_navigation_button_release_callback(GtkWidget *widget,
                                                        GdkEventButton *event,
                                                        dt_lib_module_t *self);
/* leave notify callback */
static gboolean _lib_navigation_leave_notify_callback(GtkWidget *widget,
                                                      GdkEventCrossing *event,
                                                      dt_lib_module_t *self);

/* helper function for position set */
static void _lib_navigation_set_position(struct dt_lib_module_t *self,
                                         const double x,
                                         const double y,
                                         const int wd,
                                         const int ht);

const char *name(dt_lib_module_t *self)
{
  return _("navigation");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_DARKROOM;
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


static void _lib_navigation_control_redraw_callback(gpointer instance,
                                                    dt_lib_module_t *self)
{
  dt_lib_navigation_t *d = self->data;

  dt_dev_viewport_t *port = &darktable.develop->full;

  dt_dev_zoom_t zoom;
  int closeup;
  dt_dev_get_viewport_params(port, &zoom, &closeup, NULL, NULL);
  const float cur_scale = dt_dev_get_zoom_scale(port, zoom, 1<<closeup, 0);

  gchar *zoomline = zoom == DT_ZOOM_FIT ? g_strdup(_("fit"))
                  : zoom == DT_ZOOM_FILL ? g_strdup(C_("navigationbox", "fill"))
                  : 0.5 * dt_dev_get_zoom_scale(port, DT_ZOOM_FIT, 1.0, 0)
                    == dt_dev_get_zoom_scale(port, DT_ZOOM_FREE, 1.0, 0)
                         ? g_strdup(_("small"))
                         : g_strdup_printf("%.0f%%", cur_scale * 100 * darktable.gui->ppd);
  ++darktable.gui->reset;
  if(!dt_bauhaus_combobox_set_from_text(d->zoom, zoomline))
  {
    dt_bauhaus_combobox_set_text(d->zoom, zoomline);
    dt_bauhaus_combobox_set(d->zoom, -1);
  }
  --darktable.gui->reset;
  g_free(zoomline);

  gtk_widget_queue_draw(gtk_bin_get_child(GTK_BIN(self->widget)));
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
  dt_lib_navigation_t *d = g_malloc0(sizeof(dt_lib_navigation_t));
  self->data = (void *)d;

  /* create drawingarea */
  GtkWidget *thumbnail = dt_ui_resize_wrap(NULL,
                                           0,
                                           "plugins/darkroom/navigation/graphheight");
  gtk_widget_set_tooltip_text
    (thumbnail,
     _("navigation\nclick or drag to position zoomed area in center view"));

  /* connect callbacks */
  gtk_widget_set_app_paintable(thumbnail, TRUE);
  g_signal_connect(G_OBJECT(thumbnail), "draw",
                   G_CALLBACK(_lib_navigation_draw_callback), self);
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
  dt_action_t *ac = dt_action_define(&darktable.view_manager->proxy.darkroom.view->actions,
                                     NULL,
                                     N_("hide navigation thumbnail"), thumbnail, NULL);
  dt_action_register(ac, NULL, _lib_navigation_collapse_callback,
                     GDK_KEY_N, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  /* connect a redraw callback to control draw all and preview pipe finish signals */
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            _lib_navigation_control_redraw_callback, self);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_NAVIGATION_REDRAW,
                            _lib_navigation_control_redraw_callback, self);

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->zoom, darktable.view_manager->proxy.darkroom.view,
                               NULL, N_("zoom"), _("image zoom level"),
                               -1, _zoom_changed, NULL,
                               N_("small"),
                               N_("fit"),
                               NC_("navigationbox", "fill"),
                               N_("50%"),
                               N_("100%"),
                               N_("200%"),
                               N_("400%"),
                               N_("800%"),
                               N_("1600%"));

  ac = dt_action_section(&darktable.view_manager->proxy.darkroom.view->actions, N_("zoom"));
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_COMBO_SEPARATOR + 2,
                       GDK_KEY_3, GDK_MOD1_MASK);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_COMBO_SEPARATOR + 3,
                       GDK_KEY_2, GDK_MOD1_MASK);

  dt_bauhaus_combobox_set_editable(d->zoom, TRUE);
  dt_bauhaus_widget_hide_label(d->zoom);
  gtk_widget_set_halign(d->zoom, GTK_ALIGN_END);
  gtk_widget_set_valign(d->zoom, GTK_ALIGN_END);
  gtk_widget_set_name(d->zoom, "nav-zoom");

  self->widget = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(self->widget), thumbnail);
  gtk_overlay_add_overlay(GTK_OVERLAY(self->widget), d->zoom);
  dt_gui_add_class(self->widget, "dt_plugin_ui_main");
  gtk_widget_show_all(self->widget);

  darktable.lib->proxy.navigation.module = self;
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* disconnect from signal */
  DT_CONTROL_SIGNAL_DISCONNECT(_lib_navigation_control_redraw_callback, self);

  g_free(self->data);
  self->data = NULL;
}

static gboolean _lib_navigation_draw_callback(GtkWidget *widget,
                                              cairo_t *crf,
                                              gpointer user_data)
{
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
  if(dev->preview_pipe->backbuf
     && dev->image_storage.id == dev->preview_pipe->output_imgid)
  {
    dt_pthread_mutex_t *mutex = &dev->preview_pipe->backbuf_mutex;
    dt_pthread_mutex_lock(mutex);

    cairo_save(cr);
    const int wd = dev->preview_pipe->backbuf_width;
    const int ht = dev->preview_pipe->backbuf_height;
    const float scale = fminf(width / (float)wd, height / (float)ht);

    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, wd);
    cairo_surface_t *surface
        = cairo_image_surface_create_for_data(dev->preview_pipe->backbuf,
                                              CAIRO_FORMAT_RGB24, wd, ht, stride);
    cairo_translate(cr, width / 2.0, height / 2.0f);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -.5f * wd, -.5f * ht);

    cairo_rectangle(cr, 0, 0, wd, ht);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_fill(cr);

    // draw box where we are
    float zoom_x, zoom_y, boxw, boxh;
    if(dt_dev_get_zoom_bounds(&dev->full, &zoom_x, &zoom_y, &boxw, &boxh))
    {
      // Add a dark overlay on the picture to make it fade
      cairo_rectangle(cr, 0, 0, wd, ht);
      cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
      cairo_fill(cr);

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

    dt_pthread_mutex_unlock(mutex);
  }

  /* blit memsurface into widget */
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  return FALSE;
}

void _lib_navigation_set_position(dt_lib_module_t *self,
                                  const double x,
                                  const double y,
                                  const int wd,
                                  const int ht)
{
  dt_lib_navigation_t *d = self->data;

  if(d->dragging)
  {
    const int inset = DT_NAVIGATION_INSET;
    const float width = wd - 2 * inset;
    const float height = ht - 2 * inset;
    dt_dev_viewport_t *port = &darktable.develop->full;
    int iwd, iht;
    dt_dev_get_processed_size(port, &iwd, &iht);

    float zoom_x = fmaxf(
      -.5,
      fminf(((x - inset) / width - .5f) / (iwd * fminf(wd / (float)iwd,
                                                       ht / (float)iht) / (float)wd),
            .5));
    float zoom_y = fmaxf(
      -.5,
      fminf(((y - inset) / height - .5f) / (iht * fminf(wd / (float)iwd,
                                                        ht / (float)iht) / (float)ht),
            .5));
    dt_dev_zoom_move(port, DT_ZOOM_POSITION, 0.0f, 0, zoom_x, zoom_y, TRUE);
  }
}

static gboolean _lib_navigation_motion_notify_callback(GtkWidget *widget,
                                                       GdkEventMotion *event,
                                                       dt_lib_module_t *self)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  _lib_navigation_set_position(self, event->x, event->y,
                               allocation.width, allocation.height);
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

  const float ppd = darktable.gui->ppd;

  dt_dev_viewport_t *port = &dev->full;
  float scale = 1.0f;
  int closeup = 0;
  dt_dev_zoom_t zoom = DT_ZOOM_FREE;

  if(val == 0u) // small
    scale = 0.5 * dt_dev_get_zoom_scale(port, DT_ZOOM_FIT, 1.0, 0);
  else if(val == 1u || val == -1u) // fit to screen
    zoom = DT_ZOOM_FIT;
  else if(val == 2u) // fill screen
    zoom = DT_ZOOM_FILL;
  else if(val == 3u) // 50%
    scale = 0.5f / ppd;
  else if(val == 4u && ppd != 1.0f) // 100%
    scale = 1.0f / ppd;
  else if(val >= 4u && val <= 8u) // 100%, 200%, 400%, 800%, 1600%
  {
    zoom = DT_ZOOM_1;
    closeup = val - 5 + (ppd == 1.0f);
  }
  else
    scale = val / 100.0f * ppd;

  dt_dev_zoom_move(port, zoom, scale, closeup, -1.0f, -1.0f, TRUE);
}

static gboolean _lib_navigation_button_press_callback(GtkWidget *widget,
                                                      GdkEvent *event,
                                                      dt_lib_module_t *self)
{
  dt_lib_navigation_t *d = self->data;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  if(event->type == GDK_BUTTON_PRESS && event->button.button != 2)
  {
    d->dragging = 1;
    _lib_navigation_set_position(self, event->button.x, event->button.y,
                                 allocation.width, allocation.height);

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

static gboolean _lib_navigation_button_release_callback(GtkWidget *widget,
                                                        GdkEventButton *event,
                                                        dt_lib_module_t *self)
{
  dt_lib_navigation_t *d = self->data;
  d->dragging = 0;

  return TRUE;
}

static gboolean _lib_navigation_leave_notify_callback(GtkWidget *widget,
                                                      GdkEventCrossing *event,
                                                      dt_lib_module_t *self)
{
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
