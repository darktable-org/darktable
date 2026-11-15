/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "gui/presenter.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "gui/presenter_backend.h"

#define WIDGET_KEY "dt-presenter"
#define WINDOW_KEY "dt-presenter-window"

typedef struct _widget_t
{
  GtkWidget *widget;
  // its window once it drew in it, and the backend's data. NULL again
  // when the widget or the window goes away
  dt_presenter_window_t *window;
  void *data;
  // during a draw: GTK's context, the user space the draw started in, and
  // what becomes of the kept presentation
  cairo_t *widget_cr;
  cairo_matrix_t base;
  gboolean full, retain, kept_shown;
  // the images of the draw in progress, or of the last one
  GArray *images;
  // a reference on each frame they show
  GPtrArray *frames;
} _widget_t;

static cairo_user_data_key_t _cr_key;

static void _frame_unref(gpointer data)
{
  dt_frame_unref(data);
}

// the backend's data goes, and with it the records of the images, which
// are the backend's
static void _widget_release(_widget_t *w)
{
  w->window->backend->release(w->window->backend, &w->data);
  g_array_set_size(w->images, 0);
  g_ptr_array_set_size(w->frames, 0);
}

static void _widget_detach(_widget_t *w)
{
  // a widget draws images only in a window
  if(!w->window) return;
  _widget_release(w);
  w->window->widgets = g_list_remove(w->window->widgets, w);
  w->window = NULL;
}

static void _widget_free(gpointer data)
{
  _widget_t *w = data;
  _widget_detach(w);
  g_array_free(w->images, TRUE);
  g_ptr_array_free(w->frames, TRUE);
  g_free(w);
}

static void _widget_clear(_widget_t *w)
{
  if(w->window) w->window->backend->clear(w->window->backend, w->data);
  g_array_set_size(w->images, 0);
  g_ptr_array_set_size(w->frames, 0);
}

static void _widget_unmap(GtkWidget *widget,
                          _widget_t *w)
{
  _widget_clear(w);
}

static void _widget_destroy(GtkWidget *widget,
                            _widget_t *w)
{
  _widget_detach(w);
}

static _widget_t *_widget_get(GtkWidget *widget)
{
  _widget_t *w = g_object_get_data(G_OBJECT(widget), WIDGET_KEY);
  if(w) return w;

  w = g_malloc0(sizeof(_widget_t));
  w->widget = widget;
  w->images = g_array_new(FALSE, FALSE, sizeof(dt_presenter_image_t));
  w->frames = g_ptr_array_new_with_free_func(_frame_unref);
  g_signal_connect(widget, "unmap", G_CALLBACK(_widget_unmap), w);
  g_signal_connect(widget, "destroy", G_CALLBACK(_widget_destroy), w);
  g_object_set_data_full(G_OBJECT(widget), WIDGET_KEY, w, _widget_free);
  return w;
}

static void _settle_stop(dt_presenter_window_t *window)
{
  if(window->settle) g_source_remove(window->settle);
  window->settle = 0;
}

// whatever the platform owned goes back to the system display profile,
// which has headroom 1 (_describe_system_display() in colorspaces.c). the
// target stays while the platform does: it goes on reporting, and a native
// backend chosen again describes its image for it at setup
static void _give_back(dt_presenter_window_t *window)
{
  dt_colorspaces_set_window_output(window->kind, NULL);
}

// the target goes with the platform that reported it: the next platform's
// first report is taken at once (dt_presenter_target_report()), whatever
// generation it starts at, which may be its predecessor's
static void _platform_drop(dt_presenter_window_t *window)
{
  _settle_stop(window);
  window->platform->free(window->platform);
  window->platform = NULL;
  dt_presenter_target_init(&window->target);
}

static void _preferences_changed(gpointer instance,
                                 dt_presenter_window_t *window);

static void _window_free(gpointer data)
{
  dt_presenter_window_t *window = data;
  if(window->choice) g_source_remove(window->choice);
  DT_CONTROL_SIGNAL_DISCONNECT(_preferences_changed, window);
  _settle_stop(window);
  while(window->widgets) _widget_detach(window->widgets->data);
  // before the platform, which its transport may use until it is freed
  window->backend->free(window->backend);
  if(window->platform) window->platform->free(window->platform);
  _give_back(window);
  g_free(window);
}

static void _toplevel_unrealize(GtkWidget *toplevel,
                                gpointer data)
{
  g_object_set_data(G_OBJECT(toplevel), WINDOW_KEY, NULL);
}

// the one place that decides the window's backend and platform: at the
// window's first presenting draw, and from the idle every later request
// queues
static void _choose(dt_presenter_window_t *window)
{
  const char *name = window->kind == DT_COLORSPACE_DISPLAY ? "main" : "second";
  // an escape hatch for compositor or driver bugs in the native backends.
  // stored on every choice: the preferences handler retries a failed
  // window only when the key differs from the one the window follows
  window->forced = dt_conf_get_bool("ui_last/display_force_cairo");

  // why native setup failed in this choice
  gchar *reason = NULL;
  if(!window->forced && !window->failed && !window->platform)
  {
    window->platform = dt_presenter_platform_new(window, &reason);
    if(reason) window->failed = TRUE;
  }

  const gboolean wanted = !window->forced && !window->failed && window->platform
                          && window->platform->native(window->platform);
  // a window being made has no backend yet, which is not a cairo one
  const gboolean had = window->backend != NULL;
  const gboolean replace = !had || wanted != window->native;
  if(replace)
  {
    if(had)
    {
      // the kept presentations go too, being each backend's own
      for(GList *l = window->widgets; l; l = g_list_next(l)) _widget_release(l->data);
      dt_presenter_backend_t *old = window->backend;
      window->backend = NULL;
      old->free(old);
    }
    window->backend = wanted ? dt_presenter_layered_new(window, &reason) : NULL;
    window->native = window->backend != NULL;
    if(wanted && !window->native) window->failed = TRUE;
    if(!window->native)
    {
      window->backend = dt_presenter_cairo_new(window);
      _give_back(window);
    }
  }

  // not tied to the backend changing: a cairo window watching the
  // platform's verdict keeps its backend when forced, and loses the
  // platform, so that a forced window makes no color-management requests
  if((window->forced || window->failed) && window->platform) _platform_drop(window);

  // a failed promotion is logged although the backend stays cairo
  if(replace || reason)
  {
    if(window->native)
      dt_print(DT_DEBUG_CONTROL, "[presenter] %s window: native", name);
    else if(window->forced)
      dt_print(DT_DEBUG_CONTROL, "[presenter] %s window: cairo: forced by"
               " ui_last/display_force_cairo", name);
    else if(window->failed)
      dt_print(DT_DEBUG_CONTROL, "[presenter] %s window: cairo: native setup failed: %s",
               name, reason ? reason : "no reason given");
    else if(!window->platform)
      dt_print(DT_DEBUG_CONTROL, "[presenter] %s window: cairo: no native presentation"
               " on this platform", name);
    else
      dt_print(DT_DEBUG_CONTROL, "[presenter] %s window: cairo: the compositor does not"
               " color-manage the window now", name);
  }
  g_free(reason);

  // the window's producers render for its new serial; until then its
  // frames are shown converted. a window being made is in its first draw
  if(replace && had) dt_presenter_window_redraw(window);
}

static gboolean _choice_idle(gpointer data)
{
  dt_presenter_window_t *window = data;
  // first: a platform this choice makes may ask for another while it sets
  // up, which then finds nothing to change
  window->choice = 0;
  _choose(window);
  return G_SOURCE_REMOVE;
}

// the platform's verdict can change while the window exists: chosen
// again, as Chromium picks the output color space again for the display a
// window moves to. never at once: the request may come from a platform's
// event handler or its own setup, which a switch would free or nest a
// blocking setup in. the idle's priority is above GDK_PRIORITY_REDRAW, so
// it runs before the next paint dispatched on its own, but it cannot
// preempt a frame already running
void dt_presenter_window_choose(dt_presenter_window_t *window)
{
  if(!window->choice)
    window->choice = g_idle_add_full(G_PRIORITY_HIGH_IDLE, _choice_idle, window, NULL);
}

// the preferences dialog raises it when it closes, whatever changed. a
// change of the force key is also the user's retry of a failed native
// setup; any other close leaves the window alone
static void _preferences_changed(gpointer instance,
                                 dt_presenter_window_t *window)
{
  const gboolean forced = dt_conf_get_bool("ui_last/display_force_cairo");
  if(forced == window->forced) return;
  window->failed = FALSE;
  dt_presenter_window_choose(window);
}

static dt_presenter_window_t *_window_get(GtkWidget *widget)
{
  GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
  if(!gtk_widget_is_toplevel(toplevel) || !gtk_widget_get_realized(toplevel)) return NULL;

  dt_presenter_window_t *window = g_object_get_data(G_OBJECT(toplevel), WINDOW_KEY);
  if(window) return window;

  window = g_malloc0(sizeof(dt_presenter_window_t));
  window->toplevel = toplevel;
  window->kind = toplevel == dt_ui_main_window(darktable.gui->ui)
    ? DT_COLORSPACE_DISPLAY
    : DT_COLORSPACE_DISPLAY2;
  dt_presenter_target_init(&window->target);
  // the one choice made at once: the draw making the window presents on
  // its backend
  _choose(window);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_PREFERENCES_CHANGE, _preferences_changed, window);
  g_object_set_data_full(G_OBJECT(toplevel), WINDOW_KEY, window, _window_free);
  g_signal_connect(toplevel, "unrealize", G_CALLBACK(_toplevel_unrealize), NULL);
  return window;
}

void dt_presenter_window_output(const dt_presenter_window_t *window,
                                dt_display_output_t *output)
{
  dt_colorspaces_get_display_output(window->kind, output);
}

void dt_presenter_window_set_output(dt_presenter_window_t *window,
                                    const dt_display_output_t *output)
{
  dt_colorspaces_set_window_output(window->kind, output);
}

float dt_presenter_window_target(const dt_presenter_window_t *window)
{
  return dt_presenter_target_headroom(&window->target);
}

// not published here: the backend publishes the headroom its transport
// encodes for once it can (dt_presenter_window_set_output()), so the pipes
// never render for a range other than the one the transport clips to
static void _target_changed(dt_presenter_window_t *window)
{
  const float headroom = dt_presenter_target_headroom(&window->target);
  dt_print(DT_DEBUG_CONTROL, "[presenter] %s window headroom target %.4f",
           window->kind == DT_COLORSPACE_DISPLAY ? "main" : "second", headroom);
  // NULL while the backend is being set up: it asks for the target then
  dt_presenter_backend_t *backend = window->backend;
  if(backend && backend->target) backend->target(backend, headroom);
}

static void _settle_arm(dt_presenter_window_t *window);

static gboolean _settle_timeout(gpointer data)
{
  dt_presenter_window_t *window = data;
  window->settle = 0;
  if(dt_presenter_target_tick(&window->target, g_get_monotonic_time()))
    _target_changed(window);
  _settle_arm(window);
  return G_SOURCE_REMOVE;
}

static void _settle_arm(dt_presenter_window_t *window)
{
  const int64_t deadline = dt_presenter_target_deadline(&window->target);
  if(window->settle && deadline == window->settle_deadline) return;
  _settle_stop(window);
  window->settle_deadline = deadline;
  if(!deadline) return;
  // rounded up: the timer must not fire before the deadline
  const int64_t wait = deadline - g_get_monotonic_time();
  window->settle = g_timeout_add(wait > 0 ? (guint)((wait + 999) / 1000) : 0,
                                 _settle_timeout, window);
}

void dt_presenter_window_report_headroom(dt_presenter_window_t *window,
                                         const dt_presenter_headroom_t *report)
{
  dt_print(DT_DEBUG_CONTROL | DT_DEBUG_VERBOSE,
           "[presenter] %s window reports headroom %.4f (ceiling %.4f,"
           " generation %u)",
           window->kind == DT_COLORSPACE_DISPLAY ? "main" : "second",
           report->value, report->ceiling, report->generation);
  if(dt_presenter_target_report(&window->target, report, g_get_monotonic_time()))
    _target_changed(window);
  _settle_arm(window);
}

void dt_presenter_window_redraw(dt_presenter_window_t *window)
{
  gtk_widget_queue_draw(window->toplevel);
}

static gboolean _uses(const GArray *images,
                      const dt_frame_t *frame)
{
  for(guint k = 0; k < images->len; k++)
    if(g_array_index(images, dt_presenter_image_t, k).frame == frame) return TRUE;
  return FALSE;
}

static void _hold(_widget_t *w,
                  dt_frame_t *frame)
{
  if(!g_ptr_array_find(w->frames, frame, NULL))
    g_ptr_array_add(w->frames, dt_frame_ref(frame));
}

// a draw clipped to a damaged area records clipped images: it must not
// replace or drop what dt_presenter_keep() shows. GTK clips to a region of
// whole pixels, which cairo lists as its fewest rectangles: the draw is
// whole if that is one rectangle over the widget. the clip's extents are
// not enough, two damaged corners span the widget
static gboolean _full(cairo_t *cr,
                      GtkWidget *widget)
{
  cairo_rectangle_list_t *list = cairo_copy_clip_rectangle_list(cr);
  const cairo_rectangle_t *r = list->rectangles;
  const gboolean full = list->status == CAIRO_STATUS_SUCCESS && list->num_rectangles == 1
    && r->x <= 0.0 && r->y <= 0.0
    && r->x + r->width >= gtk_widget_get_allocated_width(widget)
    && r->y + r->height >= gtk_widget_get_allocated_height(widget);
  cairo_rectangle_list_destroy(list);
  return full;
}

cairo_t *dt_presenter_begin(GtkWidget *widget,
                            cairo_t *cr)
{
  _widget_t *w = _widget_get(widget);
  if(!w->window)
  {
    w->window = _window_get(widget);
    // not in a realized window: a plain draw, with no images
    if(!w->window) return cr;
    w->window->widgets = g_list_prepend(w->window->widgets, w);
  }

  dt_presenter_backend_t *backend = w->window->backend;
  g_array_set_size(w->images, 0);
  w->widget_cr = cr;
  w->retain = w->kept_shown = FALSE;
  cairo_t *draw_cr = backend->begin(backend, widget, &w->data, cr);

  w->full = _full(draw_cr, widget);
  cairo_get_matrix(draw_cr, &w->base);
  cairo_set_user_data(draw_cr, &_cr_key, w, NULL);
  return draw_cr;
}

void dt_presenter_end(GtkWidget *widget,
                      cairo_t *cr)
{
  _widget_t *w = cairo_get_user_data(cr, &_cr_key);
  if(!w) return;
  cairo_set_user_data(cr, &_cr_key, NULL, NULL);

  const dt_presenter_kept_t kept = !w->full ? DT_PRESENTER_KEPT_UNCHANGED
                                 : w->retain ? DT_PRESENTER_KEPT_THIS
                                 : w->kept_shown ? DT_PRESENTER_KEPT_UNCHANGED
                                 : DT_PRESENTER_KEPT_NONE;

  for(guint k = w->frames->len; k > 0; k--)
    if(!_uses(w->images, g_ptr_array_index(w->frames, k - 1)))
      g_ptr_array_remove_index_fast(w->frames, k - 1);

  dt_presenter_backend_t *backend = w->window->backend;
  backend->end(backend, w->data, w->widget_cr, &w->base, w->images, kept);
  w->widget_cr = NULL;
}

void dt_presenter_image(cairo_t *cr,
                        const double x,
                        const double y,
                        dt_frame_t *frame,
                        const cairo_filter_t filter)
{
  _widget_t *w = cairo_get_user_data(cr, &_cr_key);
  if(!w || !frame) return;

  dt_presenter_image_t image = { .frame = frame, .filter = filter };

  // frame pixels -> user space now -> device -> user space of the base
  cairo_matrix_t ctm, inverse_base = w->base;
  cairo_get_matrix(cr, &ctm);
  cairo_matrix_invert(&inverse_base);
  cairo_matrix_init_translate(&image.matrix, x, y);
  cairo_matrix_multiply(&image.matrix, &image.matrix, &ctm);
  cairo_matrix_multiply(&image.matrix, &image.matrix, &inverse_base);

  dt_presenter_backend_t *backend = w->window->backend;
  if(!backend->image(backend, w->data, cr, x, y, &image)) return;

  _hold(w, frame);
  g_array_append_val(w->images, image);
}

void dt_presenter_retain(cairo_t *cr)
{
  _widget_t *w = cairo_get_user_data(cr, &_cr_key);
  if(w) w->retain = TRUE;
}

gboolean dt_presenter_keep(cairo_t *cr)
{
  _widget_t *w = cairo_get_user_data(cr, &_cr_key);
  if(!w) return FALSE;

  const guint first = w->images->len;
  dt_presenter_backend_t *backend = w->window->backend;
  if(!backend->keep(backend, w->data, cr, &w->base, w->images)) return FALSE;

  for(guint k = first; k < w->images->len; k++)
    _hold(w, g_array_index(w->images, dt_presenter_image_t, k).frame);
  w->kept_shown = TRUE;
  return TRUE;
}

void dt_presenter_clear(GtkWidget *widget)
{
  _widget_t *w = g_object_get_data(G_OBJECT(widget), WIDGET_KEY);
  if(w) _widget_clear(w);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
