/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.
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
#include "develop/imageop.h"
#include "gui/accelerators.h"
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

static float _action_process_accels_show(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  if(!isnan(move_size))
  {
    if(darktable.view_manager->accels_window.window == NULL)
    {
      if(effect != DT_ACTION_EFFECT_OFF)
        dt_view_accels_show(darktable.view_manager);
    }
    else
    {
      if(effect != DT_ACTION_EFFECT_ON)
        dt_view_accels_hide(darktable.view_manager);
    }
  }

  return darktable.view_manager->accels_window.window != NULL;
}

const dt_action_def_t dt_action_def_accels_show
  = { N_("hold"),
      _action_process_accels_show,
      dt_action_elements_hold,
      NULL, TRUE };


GdkModifierType dt_modifier_shortcuts;

static float _action_process_modifiers(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  GdkModifierType mask = 1;
  if(element) mask <<= element + 1; // ctrl = 4, alt = 8
  if(!isnan(move_size))
  {
    if(dt_modifier_shortcuts & mask)
    {
      if(effect != DT_ACTION_EFFECT_ON)
        dt_modifier_shortcuts &= ~mask;
    }
    else
    {
      if(effect != DT_ACTION_EFFECT_OFF)
        dt_modifier_shortcuts |= mask;
    }
  }

  return ((dt_modifier_shortcuts | dt_key_modifier_state()) & mask) != 0;
}

const dt_action_element_def_t _action_elements_modifiers[]
  = { { "shift", dt_action_effect_hold },
      { "ctrl", dt_action_effect_hold },
      { "alt", dt_action_effect_hold },
      { NULL } };

const dt_action_def_t dt_action_def_modifiers
  = { N_("modifiers"),
      _action_process_modifiers,
      _action_elements_modifiers,
      NULL, TRUE };

void dt_control_init(dt_control_t *s)
{
  s->actions_global = (dt_action_t){ DT_ACTION_TYPE_GLOBAL, "global", C_("accel", "global"), .next = &s->actions_views };
  s->actions_views = (dt_action_t){ DT_ACTION_TYPE_CATEGORY, "views", C_("accel", "views"), .next = &s->actions_libs, .target = &s->actions_thumb };
  s->actions_thumb = (dt_action_t){ DT_ACTION_TYPE_CATEGORY, "thumbtable", C_("accel", "thumbtable"), .owner = &s->actions_views };
  s->actions_libs = (dt_action_t){ DT_ACTION_TYPE_CATEGORY, "lib", C_("accel", "utility modules"), .next = &s->actions_iops };
  s->actions_format = (dt_action_t){ DT_ACTION_TYPE_SECTION, "format", C_("accel", "format") };   // will be placed under lib/export
  s->actions_storage = (dt_action_t){ DT_ACTION_TYPE_SECTION, "storage", C_("accel", "storage")}; // will be placed under lib/export
  s->actions_iops = (dt_action_t){ DT_ACTION_TYPE_CATEGORY, "iop", C_("accel", "processing modules"), .next = &s->actions_lua, .target = &s->actions_blend };
  s->actions_blend = (dt_action_t){ DT_ACTION_TYPE_BLEND, "blend", C_("accel", "<blending>"), .owner = &s->actions_iops };
  s->actions_lua = (dt_action_t){ DT_ACTION_TYPE_CATEGORY, "lua", C_("accel", "lua scripts"), .next = &s->actions_fallbacks };
  s->actions_fallbacks = (dt_action_t){ DT_ACTION_TYPE_CATEGORY, "fallbacks", C_("accel", "fallbacks") };
  s->actions = &s->actions_global;

  s->actions_focus = (dt_action_t){ DT_ACTION_TYPE_IOP, "focus", C_("accel", "<focused>") };
  dt_action_insert_sorted(&s->actions_iops, &s->actions_focus);

  s->widgets = g_hash_table_new(NULL, NULL);
  s->combo_introspection = g_hash_table_new(NULL, NULL);
  s->combo_list = g_hash_table_new(NULL, NULL);
  s->shortcuts = g_sequence_new(g_free);
  s->enable_fallbacks = dt_conf_get_bool("accel/enable_fallbacks");
  s->mapping_widget = NULL;
  s->confirm_mapping = TRUE;
  s->widget_definitions = g_ptr_array_new ();
  s->input_drivers = NULL;

  dt_action_define_fallback(DT_ACTION_TYPE_IOP, &dt_action_def_iop);
  dt_action_define_fallback(DT_ACTION_TYPE_LIB, &dt_action_def_lib);
  dt_action_define_fallback(DT_ACTION_TYPE_VALUE_FALLBACK, &dt_action_def_value);

  dt_action_t *ac = dt_action_define(&s->actions_global, NULL, N_("show accels window"), NULL, &dt_action_def_accels_show);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_HOLD, GDK_KEY_h, 0);

  s->actions_modifiers = dt_action_define(&s->actions_global, NULL, N_("modifiers"), NULL, &dt_action_def_modifiers);

  memset(s->vimkey, 0, sizeof(s->vimkey));
  s->vimkey_cnt = 0;

  // same thread as init
  s->gui_thread = pthread_self();

  // s->last_expose_time = dt_get_wtime();
  s->log_pos = s->log_ack = 0;
  s->log_busy = 0;
  s->log_message_timeout_id = 0;
  dt_pthread_mutex_init(&(s->log_mutex), NULL);

  s->toast_pos = s->toast_ack = 0;
  s->toast_busy = 0;
  s->toast_message_timeout_id = 0;
  dt_pthread_mutex_init(&(s->toast_mutex), NULL);

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
  if(!darktable.control->lock_cursor_shape)
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

  /* first wait for gphoto device updater */
#ifdef HAVE_GPHOTO2
  pthread_join(s->update_gphoto_thread, NULL);
#endif
  /* then wait for kick_on_workers_thread */
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
  dt_pthread_mutex_destroy(&s->toast_mutex);
  dt_pthread_mutex_destroy(&s->res_mutex);
  dt_pthread_mutex_destroy(&s->run_mutex);
  dt_pthread_mutex_destroy(&s->progress_system.mutex);
  if(s->widgets) g_hash_table_destroy(s->widgets);
  if(s->shortcuts) g_sequence_free(s->shortcuts);
  if(s->input_drivers) g_slist_free_full(s->input_drivers, g_free);
}


// ================================================================================
//  gui functions:
// ================================================================================

gboolean dt_control_configure(GtkWidget *da, GdkEventConfigure *event, gpointer user_data)
{
  // re-configure all components:
  dt_view_manager_configure(darktable.view_manager, event->width, event->height);
  return TRUE;
}

static GdkRGBA lookup_color(GtkStyleContext *context, const char *name)
{
  GdkRGBA color, fallback = {1.0, 0.0, 0.0, 1.0};
  if(!gtk_style_context_lookup_color (context, name, &color))
    color = fallback;
  return color;
}

void dt_control_draw_busy_msg(cairo_t *cr, int width, int height)
{
  PangoRectangle ink;
  PangoLayout *layout;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  const float fontsize = DT_PIXEL_APPLY_DPI(14);
  pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  pango_layout_set_text(layout, _("working..."), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  if(ink.width > width * 0.98)
  {
    pango_layout_set_text(layout, "...", -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
  }
  const float xc = width / 2.0, yc = height * 0.85 - DT_PIXEL_APPLY_DPI(30), wd = ink.width * .5f;
  cairo_move_to(cr, xc - wd, yc + 1. / 3. * fontsize - fontsize);
  pango_cairo_layout_path(cr, layout);
  cairo_set_line_width(cr, 2.0);
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LOG_BG);
  cairo_stroke_preserve(cr);
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LOG_FG);
  cairo_fill(cr);
  pango_font_description_free(desc);
  g_object_unref(layout);
}

void *dt_control_expose(void *voidptr)
{
  int pointerx, pointery;
  if(!darktable.gui->surface) return NULL;
  const int width = dt_cairo_image_surface_get_width(darktable.gui->surface);
  const int height = dt_cairo_image_surface_get_height(darktable.gui->surface);
  GtkWidget *widget = dt_ui_center(darktable.gui->ui);
  gdk_window_get_device_position(gtk_widget_get_window(widget),
      gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(widget))),
      &pointerx, &pointery, NULL);

  // create a gtk-independent surface to draw on
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // TODO: control_expose: only redraw the part not overlapped by temporary control panel show!
  //
  darktable.control->width = width;
  darktable.control->height = height;

  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  // look up some colors once
  GdkRGBA bg_color = lookup_color(context, "bg_color");

  gdk_cairo_set_source_rgba(cr, &bg_color);
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_clip(cr);
  cairo_new_path(cr);
  // draw view
  dt_view_manager_expose(darktable.view_manager, cr, width, height, pointerx, pointery);
  cairo_restore(cr);

  // draw busy indicator
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  if(darktable.control->log_busy > 0)
  {
    dt_control_draw_busy_msg(cr, width, height);
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
  dt_view_manager_mouse_moved(darktable.view_manager, x, y, pressure, which);
}

void dt_control_button_released(double x, double y, int which, uint32_t state)
{
  darktable.control->button_down = 0;
  darktable.control->button_down_which = 0;

  dt_view_manager_button_released(darktable.view_manager, x, y, which, state);
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
  if(current_view && !strcmp(mode, current_view->module_name))
  {
    // if we are not in lighttable, we switch back to that view
    if(strcmp(current_view->module_name, "lighttable")) dt_ctl_switch_mode_to("lighttable");
    return;
  }

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
  dt_control_log_redraw();
  return FALSE;
}

static gboolean _dt_ctl_toast_message_timeout_callback(gpointer data)
{
  dt_pthread_mutex_lock(&darktable.control->toast_mutex);
  if(darktable.control->toast_ack != darktable.control->toast_pos)
    darktable.control->toast_ack = (darktable.control->toast_ack + 1) % DT_CTL_TOAST_SIZE;
  darktable.control->toast_message_timeout_id = 0;
  dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
  dt_control_toast_redraw();
  return FALSE;
}

void dt_control_button_pressed(double x, double y, double pressure, int which, int type, uint32_t state)
{
  darktable.control->button_down = 1;
  darktable.control->button_down_which = which;
  darktable.control->button_type = type;
  darktable.control->button_x = x;
  darktable.control->button_y = y;
  // adding pressure to this data structure is not needed right now. should the need ever arise: here is the
  // place to do it :)
  //const float wd = darktable.control->width;
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

  // ack toast message:
  dt_pthread_mutex_lock(&darktable.control->toast_mutex);
  if(darktable.control->toast_ack != darktable.control->toast_pos)
    if(which == 1 /*&& x > xc - 10 && x < xc + 10*/ && y > yc - 10 && y < yc + 10)
    {
      if(darktable.control->toast_message_timeout_id)
      {
        g_source_remove(darktable.control->toast_message_timeout_id);
        darktable.control->toast_message_timeout_id = 0;
      }
      darktable.control->toast_ack = (darktable.control->toast_ack + 1) % DT_CTL_TOAST_SIZE;
      dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
      return;
    }
  dt_pthread_mutex_unlock(&darktable.control->toast_mutex);

  if(!dt_view_manager_button_pressed(darktable.view_manager, x, y, pressure, which, type, state))
    if(type == GDK_2BUTTON_PRESS && which == 1) dt_ctl_switch_mode();
}

static gboolean _redraw_center(gpointer user_data)
{
  dt_control_log_redraw();
  dt_control_toast_redraw();
  return FALSE; // don't call this again
}

void dt_control_log(const char *msg, ...)
{
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  va_list ap;
  va_start(ap, msg);
  char *escaped_msg = g_markup_vprintf_escaped(msg, ap);
  const int msglen = strlen(escaped_msg);
  g_strlcpy(darktable.control->log_message[darktable.control->log_pos], escaped_msg, DT_CTL_LOG_MSG_SIZE);
  g_free(escaped_msg);
  va_end(ap);
  if(darktable.control->log_message_timeout_id)
    g_source_remove(darktable.control->log_message_timeout_id);
  darktable.control->log_ack = darktable.control->log_pos;
  darktable.control->log_pos = (darktable.control->log_pos + 1) % DT_CTL_LOG_SIZE;

  darktable.control->log_message_timeout_id
    = g_timeout_add(DT_CTL_LOG_TIMEOUT + 1000 * (msglen / 40),
                    _dt_ctl_log_message_timeout_callback, NULL);
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
  // redraw center later in gui thread:
  g_idle_add(_redraw_center, 0);
}

static void _toast_log(const gboolean markup, const char *msg, va_list ap)
{
  dt_pthread_mutex_lock(&darktable.control->toast_mutex);

  // if we don't want markup, we escape <>&... so they are not interpreted later
  if(markup)
    vsnprintf(darktable.control->toast_message[darktable.control->toast_pos], DT_CTL_TOAST_MSG_SIZE, msg, ap);
  else
  {
    char *escaped_msg = g_markup_vprintf_escaped(msg, ap);
    g_strlcpy(darktable.control->toast_message[darktable.control->toast_pos], escaped_msg, DT_CTL_TOAST_MSG_SIZE);
    g_free(escaped_msg);
  }

  if(darktable.control->toast_message_timeout_id) g_source_remove(darktable.control->toast_message_timeout_id);
  darktable.control->toast_ack = darktable.control->toast_pos;
  darktable.control->toast_pos = (darktable.control->toast_pos + 1) % DT_CTL_TOAST_SIZE;
  darktable.control->toast_message_timeout_id
      = g_timeout_add(DT_CTL_TOAST_TIMEOUT, _dt_ctl_toast_message_timeout_callback, NULL);
  dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
  // redraw center later in gui thread:
  g_idle_add(_redraw_center, 0);
}

void dt_toast_log(const char *msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  _toast_log(FALSE, msg, ap);
  va_end(ap);
}

void dt_toast_markup_log(const char *msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  _toast_log(TRUE, msg, ap);
  va_end(ap);
}

static void _control_log_ack_all()
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

void dt_control_toast_busy_enter()
{
  dt_pthread_mutex_lock(&darktable.control->toast_mutex);
  darktable.control->toast_busy++;
  dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
}

void dt_control_log_busy_leave()
{
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  darktable.control->log_busy--;
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
  /* lets redraw */
  dt_control_queue_redraw_center();
}

void dt_control_toast_busy_leave()
{
  dt_pthread_mutex_lock(&darktable.control->toast_mutex);
  darktable.control->toast_busy--;
  dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
  /* lets redraw */
  dt_control_queue_redraw_center();
}

void dt_control_queue_redraw()
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_ALL);
}

void dt_control_queue_redraw_center()
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_CENTER);
}

void dt_control_navigation_redraw()
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_NAVIGATION_REDRAW);
}

void dt_control_log_redraw()
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_LOG_REDRAW);
}

void dt_control_toast_redraw()
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_TOAST_REDRAW);
}

static int _widget_queue_draw(void *widget)
{
  gtk_widget_queue_draw((GtkWidget*)widget);
  return FALSE;
}

void dt_control_queue_redraw_widget(GtkWidget *widget)
{
  if(dt_control_running())
  {
    g_idle_add(_widget_queue_draw, (void*)widget);
  }
}

int dt_control_key_pressed_override(guint key, guint state)
{
#ifdef HAVE_GAME
  // ↑ ↑ ↓ ↓ ← → ← → b a
  static int konami_state = 0;
  static guint konami_sequence[] = {
    GDK_KEY_Up,
    GDK_KEY_Up,
    GDK_KEY_Down,
    GDK_KEY_Down,
    GDK_KEY_Left,
    GDK_KEY_Right,
    GDK_KEY_Left,
    GDK_KEY_Right,
    GDK_KEY_b,
    GDK_KEY_a
  };
  if(key == konami_sequence[konami_state])
  {
    konami_state++;
    if(konami_state == G_N_ELEMENTS(konami_sequence))
    {
      dt_ctl_switch_mode_to("knight");
      konami_state = 0;
    }
  }
  else
    konami_state = 0;
#endif

  // TODO: if darkroom mode
  // did a : vim-style command start?
  static GList *autocomplete = NULL;
  if(darktable.control->vimkey_cnt)
  {
    gunichar unichar = gdk_keyval_to_unicode(key);
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
      _control_log_ack_all();
      g_list_free(autocomplete);
      autocomplete = NULL;
    }
    else if(key == GDK_KEY_Escape)
    {
      darktable.control->vimkey[0] = 0;
      darktable.control->vimkey_cnt = 0;
      _control_log_ack_all();
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
        _control_log_ack_all();
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
        if(darktable.control->vimkey_cnt < strlen(darktable.control->vimkey))
          darktable.control->vimkey[darktable.control->vimkey_cnt] = 0;
        else
          autocomplete = dt_bauhaus_vimkey_complete(darktable.control->vimkey + 5);
      }
      if(autocomplete)
      {
        // pop first.
        // the paths themselves are owned by bauhaus,
        // no free required.
        darktable.control->vimkey[darktable.control->vimkey_cnt] = 0;
        g_strlcat(darktable.control->vimkey, (char *)autocomplete->data, sizeof(darktable.control->vimkey));
        autocomplete = g_list_remove(autocomplete, autocomplete->data);
      }
      dt_control_log("%s", darktable.control->vimkey);
    }
    else if(g_unichar_isprint(unichar)) // printable unicode character
    {
      gchar utf8[6] = { 0 };
      g_unichar_to_utf8(unichar, utf8);
      g_strlcat(darktable.control->vimkey, utf8, sizeof(darktable.control->vimkey));
      darktable.control->vimkey_cnt = strlen(darktable.control->vimkey);
      dt_control_log("%s", darktable.control->vimkey);
      g_list_free(autocomplete);
      autocomplete = NULL;
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
  else if(key == ':')
  {
    darktable.control->vimkey[0] = ':';
    darktable.control->vimkey[1] = 0;
    darktable.control->vimkey_cnt = 1;
    dt_control_log("%s", darktable.control->vimkey);
    return 1;
  }

  return 0;
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
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
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


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

