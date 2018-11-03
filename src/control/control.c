/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2010--2013 henrik andersson.
    Copyright (c) 2012 James C. McPherson

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
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "views/view.h"

#include <assert.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gstdio.h>
#include <lcms2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void dt_control_init(dt_control_t *s)
{
  memset(s->vimkey, 0, sizeof(s->vimkey));
  s->vimkey_cnt = 0;

  // same thread as init
  s->gui_thread = pthread_self();

  // s->last_expose_time = dt_get_wtime();
  s->key_accelerators_on = 1;
  s->log_pos = s->log_ack = 0;
  s->log_busy = 0;
  s->log_message_timeout_id = 0;
  dt_pthread_mutex_init(&(s->log_mutex), NULL);

  pthread_cond_init(&s->cond, NULL);
  dt_pthread_mutex_init(&s->cond_mutex, NULL);
  dt_pthread_mutex_init(&s->queue_mutex, NULL);
  dt_pthread_mutex_init(&s->res_mutex, NULL);
  dt_pthread_mutex_init(&s->run_mutex, NULL);
  dt_pthread_mutex_init(&(s->global_mutex), NULL);
  dt_pthread_mutex_init(&(s->progress_system.mutex), NULL);

  // start threads
  dt_control_jobs_init(s);

  s->button_down = 0;
  s->button_down_which = 0;
  s->mouse_over_id = -1;
  s->dev_closeup = 0;
  s->dev_zoom_x = 0;
  s->dev_zoom_y = 0;
  s->dev_zoom = DT_ZOOM_FIT;
  s->lock_cursor_shape = FALSE;
}

void dt_control_key_accelerators_on(struct dt_control_t *s)
{
  gtk_window_add_accel_group(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                             darktable.control->accelerators);
  if(!s->key_accelerators_on) s->key_accelerators_on = 1;
}

void dt_control_key_accelerators_off(struct dt_control_t *s)
{
  gtk_window_remove_accel_group(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                darktable.control->accelerators);
  s->key_accelerators_on = 0;
}


int dt_control_is_key_accelerators_on(struct dt_control_t *s)
{
  return s->key_accelerators_on;
}

void dt_control_forbid_change_cursor()
{
  darktable.control->lock_cursor_shape = TRUE;
}

void dt_control_allow_change_cursor()
{
  darktable.control->lock_cursor_shape = FALSE;
}

void dt_control_change_cursor(dt_cursor_t curs)
{
  if (!darktable.control->lock_cursor_shape)
  {
    GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);
    GdkCursor *cursor = gdk_cursor_new_for_display(gdk_display_get_default(), curs);
    gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
    g_object_unref(cursor);
  }
}

int dt_control_running()
{
  // FIXME: when shutdown, run_mutex is not inited anymore!
  dt_control_t *s = darktable.control;
  dt_pthread_mutex_lock(&s->run_mutex);
  int running = s->running;
  dt_pthread_mutex_unlock(&s->run_mutex);
  return running;
}

void dt_control_quit()
{
  dt_gui_gtk_quit();
  // thread safe quit, 1st pass:
  dt_pthread_mutex_lock(&darktable.control->cond_mutex);
  dt_pthread_mutex_lock(&darktable.control->run_mutex);
  darktable.control->running = 0;
  dt_pthread_mutex_unlock(&darktable.control->run_mutex);
  dt_pthread_mutex_unlock(&darktable.control->cond_mutex);

  gtk_main_quit();
}

void dt_control_shutdown(dt_control_t *s)
{
  dt_pthread_mutex_lock(&s->cond_mutex);
  dt_pthread_mutex_lock(&s->run_mutex);
  s->running = 0;
  dt_pthread_mutex_unlock(&s->run_mutex);
  dt_pthread_mutex_unlock(&s->cond_mutex);
  pthread_cond_broadcast(&s->cond);

  /* first wait for kick_on_workers_thread */
  pthread_join(s->kick_on_workers_thread, NULL);

  int k;
  for(k = 0; k < s->num_threads; k++)
    // pthread_kill(s->thread[k], 9);
    pthread_join(s->thread[k], NULL);
  for(k = 0; k < DT_CTL_WORKER_RESERVED; k++)
    // pthread_kill(s->thread_res[k], 9);
    pthread_join(s->thread_res[k], NULL);

}

void dt_control_cleanup(dt_control_t *s)
{
  // vacuum TODO: optional?
  // DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "PRAGMA incremental_vacuum(0)", NULL, NULL, NULL);
  // DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "vacuum", NULL, NULL, NULL);
  dt_control_jobs_cleanup(s);
  dt_pthread_mutex_destroy(&s->queue_mutex);
  dt_pthread_mutex_destroy(&s->cond_mutex);
  dt_pthread_mutex_destroy(&s->log_mutex);
  dt_pthread_mutex_destroy(&s->res_mutex);
  dt_pthread_mutex_destroy(&s->run_mutex);
  dt_pthread_mutex_destroy(&s->progress_system.mutex);
  if(s->accelerator_list)
  {
    g_slist_free_full(s->accelerator_list, g_free);
  }
}


// ================================================================================
//  gui functions:
// ================================================================================

gboolean dt_control_configure(GtkWidget *da, GdkEventConfigure *event, gpointer user_data)
{
  darktable.control->tabborder = 2;
  const int tb = darktable.control->tabborder;
  // re-configure all components:
  dt_view_manager_configure(darktable.view_manager, event->width - 2 * tb, event->height - 2 * tb);
  return TRUE;
}

static GdkRGBA lookup_color(GtkStyleContext *context, const char *name)
{
  GdkRGBA color, fallback = {1.0, 0.0, 0.0, 1.0};
  if(!gtk_style_context_lookup_color (context, name, &color))
    color = fallback;
  return color;
}

void *dt_control_expose(void *voidptr)
{
  int pointerx, pointery;
  if(!darktable.gui->surface) return NULL;
  const int width = dt_cairo_image_surface_get_width(darktable.gui->surface);
  const int height = dt_cairo_image_surface_get_height(darktable.gui->surface);
  GtkWidget *widget = dt_ui_center(darktable.gui->ui);
#if GTK_CHECK_VERSION(3, 20, 0)
  gdk_window_get_device_position(gtk_widget_get_window(widget),
      gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(widget))),
      &pointerx, &pointery, NULL);
#else
  GdkDevice *device
      = gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gtk_widget_get_display(widget)));
  gdk_window_get_device_position(gtk_widget_get_window(widget), device, &pointerx, &pointery, NULL);
#endif

  // create a gtk-independent surface to draw on
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // TODO: control_expose: only redraw the part not overlapped by temporary control panel show!
  //
  float tb = 2; // fmaxf(10, width/100.0);
  darktable.control->tabborder = tb;
  darktable.control->width = width;
  darktable.control->height = height;

  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  // look up some colors once
  GdkRGBA bg_color = lookup_color(context, "bg_color");
  GdkRGBA really_dark_bg_color = lookup_color(context, "really_dark_bg_color");
  GdkRGBA selected_bg_color = lookup_color(context, "selected_bg_color");
  GdkRGBA fg_color = lookup_color(context, "fg_color");

  gdk_cairo_set_source_rgba(cr, &bg_color);

  cairo_set_line_width(cr, tb);
  cairo_rectangle(cr, tb / 2., tb / 2., width - tb, height - tb);
  cairo_stroke(cr);
  cairo_set_line_width(cr, 1.5);
  gdk_cairo_set_source_rgba(cr, &really_dark_bg_color);
  cairo_rectangle(cr, tb, tb, width - 2 * tb, height - 2 * tb);
  cairo_stroke(cr);

  cairo_save(cr);
  cairo_translate(cr, tb, tb);
  cairo_rectangle(cr, 0, 0, width - 2 * tb, height - 2 * tb);
  cairo_clip(cr);
  cairo_new_path(cr);
  // draw view
  dt_view_manager_expose(darktable.view_manager, cr, width - 2 * tb, height - 2 * tb, pointerx - tb,
                         pointery - tb);
  cairo_restore(cr);

  // draw log message, if any
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  if(darktable.control->log_ack != darktable.control->log_pos)
  {
    PangoRectangle ink;
    PangoLayout *layout;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    const float fontsize = DT_PIXEL_APPLY_DPI(14);
    pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, darktable.control->log_message[darktable.control->log_ack], -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    const float pad = DT_PIXEL_APPLY_DPI(20.0f), xc = width / 2.0;
    const float yc = height * 0.85 + DT_PIXEL_APPLY_DPI(10), wd = MIN(pad + ink.width * .5f, width * .5f - pad);
    float rad = DT_PIXEL_APPLY_DPI(14);
    // ellipsze the text if it does not fit on the screen
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_MIDDLE);
    pango_layout_set_width(layout, (int)(PANGO_SCALE * wd * 2.0f));
    cairo_set_line_width(cr, 1.);
    cairo_move_to(cr, xc - wd, yc + rad);
    for(int k = 0; k < 5; k++)
    {
      cairo_arc(cr, xc - wd, yc, rad, M_PI / 2.0, 3.0 / 2.0 * M_PI);
      cairo_line_to(cr, xc + wd, yc - rad);
      cairo_arc(cr, xc + wd, yc, rad, 3.0 * M_PI / 2.0, M_PI / 2.0);
      cairo_line_to(cr, xc - wd, yc + rad);
      if(k == 0)
      {
        gdk_cairo_set_source_rgba(cr, &selected_bg_color);
        cairo_fill_preserve(cr);
      }
      cairo_set_source_rgba(cr, 0., 0., 0., 1.0 / (1 + k));
      cairo_stroke(cr);
      rad += .5f;
    }
    gdk_cairo_set_source_rgba(cr, &fg_color);
    cairo_move_to(cr, xc - wd + .5f * pad, (yc + 1. / 3. * fontsize) - fontsize);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
  // draw busy indicator
  if(darktable.control->log_busy > 0)
  {
    PangoRectangle ink;
    PangoLayout *layout;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    const float fontsize = DT_PIXEL_APPLY_DPI(14);
    pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, _("working.."), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    const float xc = width / 2.0, yc = height * 0.85 - DT_PIXEL_APPLY_DPI(30), wd = ink.width * .5f;
    cairo_move_to(cr, xc - wd, yc + 1. / 3. * fontsize - fontsize);
    pango_cairo_layout_path(cr, layout);
    cairo_set_line_width(cr, 2.0);
    gdk_cairo_set_source_rgba(cr, &selected_bg_color);
    cairo_stroke_preserve(cr);
    gdk_cairo_set_source_rgba(cr, &fg_color);
    cairo_fill(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);

  cairo_destroy(cr);

  cairo_t *cr_pixmap = cairo_create(darktable.gui->surface);
  cairo_set_source_surface(cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);

  cairo_surface_destroy(cst);
  return NULL;
}

gboolean dt_control_draw_endmarker(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width;
  const int height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  dt_draw_endmarker(cr, width, height, GPOINTER_TO_INT(user_data));
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

void dt_control_mouse_leave()
{
  dt_view_manager_mouse_leave(darktable.view_manager);
}

void dt_control_mouse_enter()
{
  dt_view_manager_mouse_enter(darktable.view_manager);
}

void dt_control_mouse_moved(double x, double y, double pressure, int which)
{
  const float tb = darktable.control->tabborder;
  const float wd = darktable.control->width;
  const float ht = darktable.control->height;

  if(x > tb && x < wd - tb && y > tb && y < ht - tb)
    dt_view_manager_mouse_moved(darktable.view_manager, x - tb, y - tb, pressure, which);
}

void dt_control_button_released(double x, double y, int which, uint32_t state)
{
  darktable.control->button_down = 0;
  darktable.control->button_down_which = 0;
  const float tb = darktable.control->tabborder;
  // float wd = darktable.control->width;
  // float ht = darktable.control->height;

  // always do this, to avoid missing some events.
  // if(x > tb && x < wd-tb && y > tb && y < ht-tb)
  dt_view_manager_button_released(darktable.view_manager, x - tb, y - tb, which, state);
}

static void _dt_ctl_switch_mode_prepare()
{
  darktable.control->button_down = 0;
  darktable.control->button_down_which = 0;
  darktable.gui->center_tooltip = 0;
  GtkWidget *widget = dt_ui_center(darktable.gui->ui);
  gtk_widget_set_tooltip_text(widget, "");
}

static gboolean _dt_ctl_switch_mode_to(gpointer user_data)
{
  const char *mode = (const char*)user_data;
  _dt_ctl_switch_mode_prepare();
  dt_view_manager_switch(darktable.view_manager, mode);
  return FALSE;
}

static gboolean _dt_ctl_switch_mode_to_by_view(gpointer user_data)
{
  const dt_view_t *view = (const dt_view_t*)user_data;
  _dt_ctl_switch_mode_prepare();
  dt_view_manager_switch_by_view(darktable.view_manager, view);
  return FALSE;
}

void dt_ctl_switch_mode_to(const char *mode)
{
  const dt_view_t *current_view = dt_view_manager_get_current_view(darktable.view_manager);
  if(current_view && !strcmp(mode, current_view->module_name)) return;

  g_main_context_invoke(NULL, _dt_ctl_switch_mode_to, (gpointer)mode);
}

void dt_ctl_switch_mode_to_by_view(const dt_view_t *view)
{
  if(view == dt_view_manager_get_current_view(darktable.view_manager)) return;
  g_main_context_invoke(NULL, _dt_ctl_switch_mode_to_by_view, (gpointer)view);
}

void dt_ctl_switch_mode()
{
  const dt_view_t *view = dt_view_manager_get_current_view(darktable.view_manager);
  const char *mode = (view && !strcmp(view->module_name, "lighttable")) ? "darkroom" : "lighttable";
  dt_ctl_switch_mode_to(mode);
}

static gboolean _dt_ctl_log_message_timeout_callback(gpointer data)
{
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  if(darktable.control->log_ack != darktable.control->log_pos)
    darktable.control->log_ack = (darktable.control->log_ack + 1) % DT_CTL_LOG_SIZE;
  darktable.control->log_message_timeout_id = 0;
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
  dt_control_queue_redraw_center();
  return FALSE;
}


void dt_control_button_pressed(double x, double y, double pressure, int which, int type, uint32_t state)
{
  const float tb = darktable.control->tabborder;
  darktable.control->button_down = 1;
  darktable.control->button_down_which = which;
  darktable.control->button_type = type;
  darktable.control->button_x = x - tb;
  darktable.control->button_y = y - tb;
  // adding pressure to this data structure is not needed right now. should the need ever arise: here is the
  // place to do it :)
  const float wd = darktable.control->width;
  const float ht = darktable.control->height;

  // ack log message:
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  const float /*xc = wd/4.0-20,*/ yc = ht * 0.85 + 10;
  if(darktable.control->log_ack != darktable.control->log_pos)
    if(which == 1 /*&& x > xc - 10 && x < xc + 10*/ && y > yc - 10 && y < yc + 10)
    {
      if(darktable.control->log_message_timeout_id)
      {
        g_source_remove(darktable.control->log_message_timeout_id);
        darktable.control->log_message_timeout_id = 0;
      }
      darktable.control->log_ack = (darktable.control->log_ack + 1) % DT_CTL_LOG_SIZE;
      dt_pthread_mutex_unlock(&darktable.control->log_mutex);
      return;
    }
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);

  if(x > tb && x < wd - tb && y > tb && y < ht - tb)
  {
    if(!dt_view_manager_button_pressed(darktable.view_manager, x - tb, y - tb, pressure, which, type, state))
      if(type == GDK_2BUTTON_PRESS && which == 1) dt_ctl_switch_mode();
  }
}

static gboolean _redraw_center(gpointer user_data)
{
  dt_control_queue_redraw_center();
  return FALSE; // don't call this again
}

void dt_control_log(const char *msg, ...)
{
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  va_list ap;
  va_start(ap, msg);
  vsnprintf(darktable.control->log_message[darktable.control->log_pos], DT_CTL_LOG_MSG_SIZE, msg, ap);
  va_end(ap);
  if(darktable.control->log_message_timeout_id) g_source_remove(darktable.control->log_message_timeout_id);
  darktable.control->log_ack = darktable.control->log_pos;
  darktable.control->log_pos = (darktable.control->log_pos + 1) % DT_CTL_LOG_SIZE;
  darktable.control->log_message_timeout_id
      = g_timeout_add(DT_CTL_LOG_TIMEOUT, _dt_ctl_log_message_timeout_callback, NULL);
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
  // redraw center later in gui thread:
  g_idle_add(_redraw_center, 0);
}

static void dt_control_log_ack_all()
{
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  darktable.control->log_pos = darktable.control->log_ack;
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
  dt_control_queue_redraw_center();
}

void dt_control_log_busy_enter()
{
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  darktable.control->log_busy++;
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
}

void dt_control_log_busy_leave()
{
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  darktable.control->log_busy--;
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
  /* lets redraw */
  dt_control_queue_redraw_center();
}

void dt_control_queue_redraw()
{
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_ALL);
}

void dt_control_queue_redraw_center()
{
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_CENTER);
}

static gboolean _gtk_widget_queue_draw(gpointer user_data)
{
  gtk_widget_queue_draw(GTK_WIDGET(user_data));
  return FALSE;
}

void dt_control_queue_redraw_widget(GtkWidget *widget)
{
  if(dt_control_running())
    g_main_context_invoke(NULL, _gtk_widget_queue_draw, widget);
}


int dt_control_key_pressed_override(guint key, guint state)
{
  dt_control_accels_t *accels = &darktable.control->accels;

  // TODO: if darkroom mode
  // did a : vim-style command start?
  static GList *autocomplete = NULL;
  static char vimkey_input[256];
  if(darktable.control->vimkey_cnt)
  {
    guchar unichar = gdk_keyval_to_unicode(key);
    if(key == GDK_KEY_Return)
    {
      if(!strcmp(darktable.control->vimkey, ":q"))
      {
        dt_control_quit();
      }
      else
      {
        dt_bauhaus_vimkey_exec(darktable.control->vimkey);
      }
      darktable.control->vimkey[0] = 0;
      darktable.control->vimkey_cnt = 0;
      dt_control_log_ack_all();
      g_list_free(autocomplete);
      autocomplete = NULL;
    }
    else if(key == GDK_KEY_Escape)
    {
      darktable.control->vimkey[0] = 0;
      darktable.control->vimkey_cnt = 0;
      dt_control_log_ack_all();
      g_list_free(autocomplete);
      autocomplete = NULL;
    }
    else if(key == GDK_KEY_BackSpace)
    {
      darktable.control->vimkey_cnt
          -= (darktable.control->vimkey + darktable.control->vimkey_cnt)
             - g_utf8_prev_char(darktable.control->vimkey + darktable.control->vimkey_cnt);
      darktable.control->vimkey[darktable.control->vimkey_cnt] = 0;
      if(darktable.control->vimkey_cnt == 0)
        dt_control_log_ack_all();
      else
        dt_control_log("%s", darktable.control->vimkey);
      g_list_free(autocomplete);
      autocomplete = NULL;
    }
    else if(key == GDK_KEY_Tab)
    {
      // TODO: also support :preset and :get?
      // auto complete:
      if(darktable.control->vimkey_cnt < 5)
      {
        g_strlcpy(darktable.control->vimkey, ":set ", sizeof(darktable.control->vimkey));
        darktable.control->vimkey_cnt = 5;
      }
      else if(!autocomplete)
      {
        // TODO: handle '.'-separated things separately
        // this is a static list, and tab cycles through the list
        g_strlcpy(vimkey_input, darktable.control->vimkey + 5, sizeof(vimkey_input));
        autocomplete = dt_bauhaus_vimkey_complete(darktable.control->vimkey + 5);
        autocomplete = g_list_append(autocomplete, vimkey_input); // remember input to cycle back
      }
      if(autocomplete)
      {
        // pop first.
        // the paths themselves are owned by bauhaus,
        // no free required.
        snprintf(darktable.control->vimkey, sizeof(darktable.control->vimkey), ":set %s",
                 (char *)autocomplete->data);
        autocomplete = g_list_remove(autocomplete, autocomplete->data);
        darktable.control->vimkey_cnt = strlen(darktable.control->vimkey);
      }
      dt_control_log("%s", darktable.control->vimkey);
    }
    else if(g_unichar_isprint(unichar)) // printable unicode character
    {
      gchar utf8[6];
      const gint char_width = g_unichar_to_utf8(unichar, utf8);
      if(darktable.control->vimkey_cnt + 1 + char_width < 256)
      {
        g_utf8_strncpy(darktable.control->vimkey + darktable.control->vimkey_cnt, utf8, 1);
        darktable.control->vimkey_cnt += char_width;
        darktable.control->vimkey[darktable.control->vimkey_cnt] = 0;
        dt_control_log("%s", darktable.control->vimkey);
        g_list_free(autocomplete);
        autocomplete = NULL;
      }
    }
    else if(key == GDK_KEY_Up)
    {
      // TODO: step history up and copy to vimkey
    }
    else if(key == GDK_KEY_Down)
    {
      // TODO: step history down and copy to vimkey
    }
    return 1;
  }
  else if(key == ':' && darktable.control->key_accelerators_on)
  {
    darktable.control->vimkey[0] = ':';
    darktable.control->vimkey[1] = 0;
    darktable.control->vimkey_cnt = 1;
    dt_control_log("%s", darktable.control->vimkey);
    return 1;
  }

  /* check if key accelerators are enabled*/
  if(darktable.control->key_accelerators_on != 1) return 0;

  if(key == accels->global_sideborders.accel_key && state == accels->global_sideborders.accel_mods)
  {
    /* toggle panel viewstate */
    dt_ui_toggle_panels_visibility(darktable.gui->ui);

    /* trigger invalidation of centerview to reprocess pipe */
    dt_dev_invalidate(darktable.develop);
    gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
    return 1;
  }
  else if(key == accels->global_header.accel_key && state == accels->global_header.accel_mods)
  {
    char param[512];
    const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);

    /* do nothing if in collapse panel state
       TODO: reconsider adding this check to ui api */
    g_snprintf(param, sizeof(param), "%s/ui/panel_collaps_state", cv->module_name);
    if(dt_conf_get_int(param)) return 0;

    /* toggle the header visibility state */
    g_snprintf(param, sizeof(param), "%s/ui/show_header", cv->module_name);
    const gboolean header = !dt_conf_get_bool(param);
    dt_conf_set_bool(param, header);

    /* show/hide the actual header panel */
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, header, TRUE);
    gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
    return 1;
  }
  // add an option to allow skip mouse events while editing masks
  else if(key == accels->darkroom_skip_mouse_events.accel_key && state == accels->darkroom_skip_mouse_events.accel_mods)
  {
    darktable.develop->darkroom_skip_mouse_events = TRUE;
    return 1;
  }
  return 0;
}

int dt_control_key_pressed(guint key, guint state)
{
  const int handled = dt_view_manager_key_pressed(darktable.view_manager, key, state);
  if(handled) gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
  return handled;
}

int dt_control_key_released(guint key, guint state)
{
  // this line is here to find the right key code on different platforms (mac).
  // printf("key code pressed: %d\n", which);

  int handled = 0;
  switch(key)
  {
    default:
      // propagate to view modules.
      handled = dt_view_manager_key_released(darktable.view_manager, key, state);
      break;
  }

  if(handled) gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
  return handled;
}

void dt_control_hinter_message(const struct dt_control_t *s, const char *message)
{
  if(s->proxy.hinter.module) return s->proxy.hinter.set_message(s->proxy.hinter.module, message);
}

int32_t dt_control_get_mouse_over_id()
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  const int32_t result = darktable.control->mouse_over_id;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
  return result;
}

void dt_control_set_mouse_over_id(int32_t value)
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  if(darktable.control->mouse_over_id != value)
  {
    darktable.control->mouse_over_id = value;
    dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  }
  else
    dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
}

float dt_control_get_dev_zoom_x()
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  const float result = darktable.control->dev_zoom_x;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
  return result;
}
void dt_control_set_dev_zoom_x(float value)
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  darktable.control->dev_zoom_x = value;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
}

float dt_control_get_dev_zoom_y()
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  const float result = darktable.control->dev_zoom_y;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
  return result;
}
void dt_control_set_dev_zoom_y(float value)
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  darktable.control->dev_zoom_y = value;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
}

float dt_control_get_dev_zoom_scale()
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  const float result = darktable.control->dev_zoom_scale;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
  return result;
}
void dt_control_set_dev_zoom_scale(float value)
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  darktable.control->dev_zoom_scale = value;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
}

int dt_control_get_dev_closeup()
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  const int result = darktable.control->dev_closeup;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
  return result;
}
void dt_control_set_dev_closeup(int value)
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  darktable.control->dev_closeup = value;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
}

dt_dev_zoom_t dt_control_get_dev_zoom()
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  const dt_dev_zoom_t result = darktable.control->dev_zoom;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
  return result;
}
void dt_control_set_dev_zoom(dt_dev_zoom_t value)
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  darktable.control->dev_zoom = value;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
