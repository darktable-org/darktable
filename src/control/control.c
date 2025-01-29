/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.
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
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "imageio/imageio_common.h"
#include "views/view.h"

#include <assert.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gstdio.h>
#include <lcms2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static float _action_process_accels_show(const gpointer target,
                                         const dt_action_element_t element,
                                         const dt_action_effect_t effect,
                                         const float move_size)
{
  if(DT_PERFORM_ACTION(move_size))
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

static float _action_process_modifiers(const gpointer target,
                                       const dt_action_element_t element,
                                       const dt_action_effect_t effect,
                                       const float move_size)
{
  GdkModifierType mask = 1;
  if(element) mask <<= element + 1; // ctrl = 4, alt = 8
  if(DT_PERFORM_ACTION(move_size))
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
  s->actions_global = (dt_action_t){ DT_ACTION_TYPE_GLOBAL,
    "global",
    C_("accel", "global"),
    .target = NULL,
    .owner = NULL,
    .next = &s->actions_views };

  s->actions_views = (dt_action_t){ DT_ACTION_TYPE_CATEGORY,
    "views",
    C_("accel", "views"),
    .target = &s->actions_thumb,
    .owner = NULL,
    .next = &s->actions_libs };

  s->actions_thumb = (dt_action_t){ DT_ACTION_TYPE_CATEGORY,
    "thumbtable",
    C_("accel", "thumbtable"),
    .target = NULL,
    .owner = &s->actions_views,
    .next = NULL };

  s->actions_libs = (dt_action_t){ DT_ACTION_TYPE_CATEGORY,
    "lib",
    C_("accel", "utility modules"),
    NULL,
    NULL,
    .next = &s->actions_iops };

  s->actions_format = (dt_action_t){ DT_ACTION_TYPE_SECTION,
    "format",
    C_("accel", "format"),
    NULL,
    NULL,
    NULL };   // will be placed under lib/export

  s->actions_storage = (dt_action_t){ DT_ACTION_TYPE_SECTION,
    "storage",
    C_("accel", "storage"),
    NULL,
    NULL,
    NULL }; // will be placed under lib/export

  s->actions_iops = (dt_action_t){ DT_ACTION_TYPE_CATEGORY,
    "iop",
    C_("accel", "processing modules"),
    .target = &s->actions_blend,
    .owner = NULL,
    .next = &s->actions_lua };

  s->actions_blend = (dt_action_t){ DT_ACTION_TYPE_BLEND,
    "blend",
    C_("accel", "<blending>"),
    .target = NULL,
    .owner = &s->actions_iops,
    .next = NULL };

  s->actions_lua = (dt_action_t){ DT_ACTION_TYPE_CATEGORY,
    "lua",
    C_("accel", "Lua scripts"),
    .target = NULL,
    .owner = NULL,
    .next = &s->actions_fallbacks };

  s->actions_fallbacks = (dt_action_t){ DT_ACTION_TYPE_CATEGORY,
    "fallbacks",
    C_("accel", "fallbacks"),
    NULL,
    NULL,
    NULL };

  s->actions = &s->actions_global;

  s->actions_focus = (dt_action_t){ DT_ACTION_TYPE_IOP,
    "focus",
    C_("accel", "<focused>"),
    NULL,
    NULL,
    NULL };

  dt_action_insert_sorted(&s->actions_iops, &s->actions_focus);

  s->widgets = g_hash_table_new(NULL, NULL);
  s->shortcuts = g_sequence_new(g_free);
  s->enable_fallbacks = dt_conf_get_bool("accel/enable_fallbacks");
  s->mapping_widget = NULL;
  s->confirm_mapping = TRUE;
  s->widget_definitions = g_ptr_array_new ();
  s->input_drivers = NULL;
  dt_atomic_set_int(&s->running, DT_CONTROL_STATE_DISABLED);
  s->cups_started = FALSE;

  dt_action_define_fallback(DT_ACTION_TYPE_IOP, &dt_action_def_iop);
  dt_action_define_fallback(DT_ACTION_TYPE_LIB, &dt_action_def_lib);
  dt_action_define_fallback(DT_ACTION_TYPE_VALUE_FALLBACK, &dt_action_def_value);

  dt_action_t *ac = dt_action_define(&s->actions_global, NULL,
                                     N_("show accels window"), NULL,
                                     &dt_action_def_accels_show);
  dt_shortcut_register(ac, 0, DT_ACTION_EFFECT_HOLD, GDK_KEY_h, 0);

  s->actions_modifiers = dt_action_define(&s->actions_global, NULL,
                                          N_("modifiers"), NULL, &dt_action_def_modifiers);

  memset(s->vimkey, 0, sizeof(s->vimkey));
  s->vimkey_cnt = 0;

  // same thread as init
  s->gui_thread = pthread_self();

  // s->last_expose_time = dt_get_wtime();
  s->log_pos = s->log_ack = 0;
  s->log_busy = 0;
  s->log_message_timeout_id = 0;
  dt_pthread_mutex_init(&s->log_mutex, NULL);

  s->toast_pos = s->toast_ack = 0;
  s->toast_busy = 0;
  s->toast_message_timeout_id = 0;
  dt_pthread_mutex_init(&s->toast_mutex, NULL);

  pthread_cond_init(&s->cond, NULL);
  dt_pthread_mutex_init(&s->cond_mutex, NULL);
  dt_pthread_mutex_init(&s->queue_mutex, NULL);
  dt_pthread_mutex_init(&s->res_mutex, NULL);
  dt_pthread_mutex_init(&s->global_mutex, NULL);
  dt_pthread_mutex_init(&s->progress_system.mutex, NULL);

  // start threads
  dt_control_jobs_init(s);

  s->button_down = 0;
  s->button_down_which = 0;
  s->mouse_over_id = NO_IMGID;
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
  GdkWindow *window = gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui));
  if(!darktable.control->lock_cursor_shape && window)
  {
    GdkCursor *cursor = gdk_cursor_new_for_display(gdk_window_get_display(window), curs);
    gdk_window_set_cursor(window, cursor);
    g_object_unref(cursor);
  }
}

/* Some implementation and how-to use notes about control->running
  It is declared as a global gint with states defined in dt_control_state_t and may only be
    written/read or modified via atomic intructions to avoid a protecting mutex.

  We have gboolean dt_control_running() checking for a running control system all over the code.

  dt_control_quit() is called whenever we want to close darktable.
    It is triggered either by callbacks from the gui frontend (ctrl-q or click on close button)
    or from lua.
    By setting control->running to DT_CONTROL_STATE_CLEANUP here we
      a) make sure the system recognizes this a not running any more and
      b) leave a note about pending cleanup

  dt_control_shutdown() is called when darktable closes, it checks for pending work (threads to be joined)
    via DT_CONTROL_STATE_CLEANUP before doing so.
*/

gboolean dt_control_running()
{
  dt_control_t *dc = darktable.control;
  const int status = dc ? dt_atomic_get_int(&dc->running) : DT_CONTROL_STATE_DISABLED;
  return status == DT_CONTROL_STATE_RUNNING;
}

void dt_control_quit()
{
  if(dt_control_running())
  {
    dt_control_t *dc = darktable.control;

#ifdef HAVE_PRINT
    dt_printers_abort_discovery();
    // Cups timeout could be pretty long, at least 30seconds
    // but don't rely on cups returning correctly so a timeout
    for(int i = 0; i < 40000 && !dc->cups_started; i++)
      g_usleep(1000);
#endif

    dt_pthread_mutex_lock(&dc->cond_mutex);
    // set the "pending cleanup work" flag to be handled in dt_control_shutdown()
    dt_atomic_set_int(&dc->running, DT_CONTROL_STATE_CLEANUP);
    dt_pthread_mutex_unlock(&dc->cond_mutex);
  }

  if(g_atomic_int_get(&darktable.gui_running))
  {
    dt_gui_gtk_quit();
    gtk_main_quit();
  }
}

void dt_control_shutdown(dt_control_t *s)
{
  if(!s)
    return;

  dt_pthread_mutex_lock(&s->cond_mutex);
  const gboolean cleanup = dt_atomic_exch_int(&s->running, DT_CONTROL_STATE_DISABLED) == DT_CONTROL_STATE_CLEANUP;
  pthread_cond_broadcast(&s->cond);
  dt_pthread_mutex_unlock(&s->cond_mutex);

  int err = 0; // collect all joining errors
  /* first wait for gphoto device updater */
#ifdef HAVE_GPHOTO2
   err = dt_pthread_join(s->update_gphoto_thread);
#endif

  if(!cleanup)
    return;   // if not running there are no threads to join

  dt_print(DT_DEBUG_CONTROL, "[dt_control_shutdown] closing control threads");

  /* then wait for kick_on_workers_thread */
  err = dt_pthread_join(s->kick_on_workers_thread);
  dt_print(DT_DEBUG_CONTROL, "[dt_control_shutdown] joined kicker%s", err ? ", error" : "");

  for(int k = 0; k < s->num_threads-1; k++)
  {
    err = dt_pthread_join(s->thread[k]);
    dt_print(DT_DEBUG_CONTROL, "[dt_control_shutdown] joined num_thread %i%s", k, err ? ", error" : "");
  }

  for(int k = 0; k < DT_CTL_WORKER_RESERVED; k++)
  {
    err = dt_pthread_join(s->thread_res[k]);
    dt_print(DT_DEBUG_CONTROL, "[dt_control_shutdown] joined worker %i%s", k, err ? ", error" : "");
  }
}

void dt_control_cleanup(dt_control_t *s)
{
  if(!s)
    return;
  // vacuum TODO: optional?
  // DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "PRAGMA incremental_vacuum(0)", NULL, NULL, NULL);
  // DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "vacuum", NULL, NULL, NULL);
  dt_control_jobs_cleanup(s);
  dt_pthread_mutex_destroy(&s->queue_mutex);
  dt_pthread_mutex_destroy(&s->cond_mutex);
  dt_pthread_mutex_destroy(&s->log_mutex);
  dt_pthread_mutex_destroy(&s->toast_mutex);
  dt_pthread_mutex_destroy(&s->res_mutex);
  dt_pthread_mutex_destroy(&s->progress_system.mutex);
  if(s->widgets) g_hash_table_destroy(s->widgets);
  if(s->shortcuts) g_sequence_free(s->shortcuts);
  if(s->input_drivers) g_slist_free_full(s->input_drivers, g_free);
}


// ================================================================================
//  gui functions:
// ================================================================================

gboolean dt_control_configure(GtkWidget *da,
                              GdkEventConfigure *event,
                              gpointer user_data)
{
  // re-configure all components:
  dt_view_manager_configure(darktable.view_manager, event->width, event->height);
  return TRUE;
}

void dt_control_draw_busy_msg(cairo_t *cr, int width, int height)
{
  PangoRectangle ink;
  PangoLayout *layout;
  PangoFontDescription *desc =
    pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
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
  const double xc = width / 2.0;
  const double yc = height * 0.85 - DT_PIXEL_APPLY_DPI(30);
  const double wd = ink.width * .5;
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

void dt_control_expose(GtkWidget *widget, cairo_t *cr)
{
  int pointerx, pointery;
  gdk_window_get_device_position(gtk_widget_get_window(widget),
      gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(widget))),
      &pointerx, &pointery, NULL);

  dt_control_t *dc = darktable.control;
  dc->width = gtk_widget_get_allocated_width(widget);
  dc->height = gtk_widget_get_allocated_height(widget);

  dt_view_manager_expose(darktable.view_manager, cr, dc->width, dc->height, pointerx, pointery);

  // draw busy indicator
  dt_pthread_mutex_lock(&dc->log_mutex);
  if(dc->log_busy > 0)
  {
    dt_control_draw_busy_msg(cr, dc->width, dc->height);
  }
  dt_pthread_mutex_unlock(&dc->log_mutex);
}

gboolean dt_control_draw_endmarker(GtkWidget *widget,
                                   cairo_t *crf,
                                   gpointer user_data)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width;
  const int height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  dt_draw_endmarker(cr, width, height, GPOINTER_TO_INT(user_data));
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0.0, 0.0);
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

void dt_control_mouse_moved(double x,
                            double y,
                            double pressure,
                            int which)
{
  dt_view_manager_mouse_moved(darktable.view_manager, x, y, pressure, which);
}

void dt_control_button_released(double x,
                                double y,
                                int which,
                                uint32_t state)
{
  darktable.control->button_down = 0;
  darktable.control->button_down_which = 0;

  dt_view_manager_button_released(darktable.view_manager, x, y, which, state);
}

static void _dt_ctl_switch_mode_prepare()
{
  darktable.control->button_down = 0;
  darktable.control->button_down_which = 0;
  GtkWidget *widget = dt_ui_center(darktable.gui->ui);
  gtk_widget_set_tooltip_text(widget, "");
  gtk_widget_grab_focus(widget);
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
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv && !g_ascii_strcasecmp(mode, cv->module_name))
  {
    // if we are not in lighttable, we switch back to that view
    if(g_ascii_strcasecmp(cv->module_name, "lighttable"))
      dt_ctl_switch_mode_to("lighttable");
    return;
  }

  g_main_context_invoke(NULL, _dt_ctl_switch_mode_to, (gpointer)mode);
}

void dt_ctl_switch_mode_to_by_view(const dt_view_t *view)
{
  if(view == dt_view_manager_get_current_view(darktable.view_manager))
    return;
  g_main_context_invoke(NULL, _dt_ctl_switch_mode_to_by_view, (gpointer)view);
}

void dt_ctl_switch_mode()
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  const char *mode = (cv && !strcmp(cv->module_name, "lighttable"))
                      ? "darkroom"
                      : "lighttable";
  dt_ctl_switch_mode_to(mode);
}

static gboolean _dt_ctl_log_message_timeout_callback(gpointer data)
{
  dt_control_t *dc = darktable.control;
  dt_pthread_mutex_lock(&dc->log_mutex);
  dc->log_ack = dc->log_pos;
  dc->log_message_timeout_id = 0;
  dt_pthread_mutex_unlock(&dc->log_mutex);
  dt_control_log_redraw();
  return FALSE;
}

static gboolean _dt_ctl_toast_message_timeout_callback(gpointer data)
{
  dt_control_t *dc = darktable.control;
  dt_pthread_mutex_lock(&dc->toast_mutex);
  dc->toast_ack = dc->toast_pos;
  dc->toast_message_timeout_id = 0;
  dt_pthread_mutex_unlock(&dc->toast_mutex);
  dt_control_toast_redraw();
  return FALSE;
}

void dt_control_button_pressed(double x,
                               double y,
                               double pressure,
                               int which,
                               int type,
                               uint32_t state)
{
  dt_control_t *dc = darktable.control;
  dc->button_down = 1;
  dc->button_down_which = which;
  dc->button_type = type;
  dc->button_x = x;
  dc->button_y = y;
  // adding pressure to this data structure is not needed right
  // now. should the need ever arise: here is the place to do it :)
  // const float wd = dc->width;
  const double ht = dc->height;

  // ack log message:
  dt_pthread_mutex_lock(&dc->log_mutex);
  const double /*xc = wd/4.0-20,*/ yc = ht * 0.85 + 10.0;
  if(dc->log_ack != dc->log_pos)
  {
    if(which == 1 && y > yc - 10.0 && y < yc + 10.0)
    {
      if(dc->log_message_timeout_id)
      {
        g_source_remove(dc->log_message_timeout_id);
        dc->log_message_timeout_id = 0;
      }
      dc->log_ack = dc->log_pos;
      dt_pthread_mutex_unlock(&dc->log_mutex);
      return;
    }
  }
  dt_pthread_mutex_unlock(&dc->log_mutex);

  // ack toast message:
  dt_pthread_mutex_lock(&dc->toast_mutex);
  if(dc->toast_ack != dc->toast_pos)
  {
    if(which == 1 && y > yc - 10.0 && y < yc + 10.0)
    {
      if(dc->toast_message_timeout_id)
      {
        g_source_remove(dc->toast_message_timeout_id);
        dc->toast_message_timeout_id = 0;
      }
      dc->toast_ack = dc->toast_pos;
      dt_pthread_mutex_unlock(&dc->toast_mutex);
      return;
    }
  }
  dt_pthread_mutex_unlock(&dc->toast_mutex);

  if(!dt_view_manager_button_pressed(darktable.view_manager, x, y,
                                     pressure, which, type, state))
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
  dt_control_t *dc = darktable.control;
  dt_pthread_mutex_lock(&dc->log_mutex);
  va_list ap;
  va_start(ap, msg);
  char *escaped_msg = g_markup_vprintf_escaped(msg, ap);
  const int msglen = strlen(escaped_msg);

  const int old_idx = (dc->log_pos - 1) & (DT_CTL_LOG_SIZE-1);
  const gboolean timeout = dc->log_message_timeout_id;
  if(!timeout || g_strcmp0(escaped_msg, dc->log_message[old_idx]))
  {
    g_strlcpy(dc->log_message[dc->log_pos & (DT_CTL_LOG_SIZE-1)], escaped_msg, DT_CTL_LOG_MSG_SIZE);
    dc->log_pos++;
  }

  g_free(escaped_msg);
  va_end(ap);

  if(timeout)
    g_source_remove(dc->log_message_timeout_id);

  dc->log_message_timeout_id
    = g_timeout_add(DT_CTL_LOG_TIMEOUT + 1000 * (msglen / 40),
                    _dt_ctl_log_message_timeout_callback, NULL);
  dt_pthread_mutex_unlock(&dc->log_mutex);
  // redraw center later in gui thread:
  g_idle_add(_redraw_center, 0);
}

static void _toast_log(const gboolean markup, const char *msg, va_list ap)
{
  dt_control_t *dc = darktable.control;
  dt_pthread_mutex_lock(&dc->toast_mutex);

  // if we don't want markup, we escape <>&... so they are not interpreted later
  if(markup)
    vsnprintf(dc->toast_message[dc->toast_pos & (DT_CTL_TOAST_SIZE-1)], DT_CTL_TOAST_MSG_SIZE, msg, ap);
  else
  {
    char *escaped_msg = g_markup_vprintf_escaped(msg, ap);
    g_strlcpy(dc->toast_message[dc->toast_pos & (DT_CTL_TOAST_SIZE-1)], escaped_msg, DT_CTL_TOAST_MSG_SIZE);
    g_free(escaped_msg);
  }
  dc->toast_pos++;

  if(dc->toast_message_timeout_id)
    g_source_remove(dc->toast_message_timeout_id);

  dc->toast_message_timeout_id
      = g_timeout_add(DT_CTL_TOAST_TIMEOUT, _dt_ctl_toast_message_timeout_callback, NULL);
  dt_pthread_mutex_unlock(&dc->toast_mutex);
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
  dt_control_t *dc = darktable.control;
  dt_pthread_mutex_lock(&dc->log_mutex);
  dc->log_ack = dc->log_pos;
  dt_pthread_mutex_unlock(&dc->log_mutex);
  dt_control_queue_redraw_center();
}

void dt_control_log_busy_enter()
{
  dt_control_t *dc = darktable.control;
  dt_pthread_mutex_lock(&dc->log_mutex);
  dc->log_busy++;
  dt_pthread_mutex_unlock(&dc->log_mutex);
}

void dt_control_toast_busy_enter()
{
  dt_control_t *dc = darktable.control;
  dt_pthread_mutex_lock(&dc->toast_mutex);
  dc->toast_busy++;
  dt_pthread_mutex_unlock(&dc->toast_mutex);
}

void dt_control_log_busy_leave()
{
  dt_control_t *dc = darktable.control;
  dt_pthread_mutex_lock(&dc->log_mutex);
  dc->log_busy--;
  dt_pthread_mutex_unlock(&dc->log_mutex);
  /* lets redraw */
  dt_control_queue_redraw_center();
}

void dt_control_toast_busy_leave()
{
  dt_control_t *dc = darktable.control;
  dt_pthread_mutex_lock(&dc->toast_mutex);
  dc->toast_busy--;
  dt_pthread_mutex_unlock(&dc->toast_mutex);
  /* lets redraw */
  dt_control_queue_redraw_center();
}

void dt_control_queue_redraw()
{
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_REDRAW_ALL);
}

void dt_control_queue_redraw_center()
{
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_REDRAW_CENTER);
}

void dt_control_navigation_redraw()
{
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_NAVIGATION_REDRAW);
}

void dt_control_log_redraw()
{
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_LOG_REDRAW);
}

void dt_control_toast_redraw()
{
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_TOAST_REDRAW);
}

static int _widget_queue_draw(void *widget)
{
  gtk_widget_queue_draw((GtkWidget*)widget);
  g_object_unref(widget);
  return 0;
}

void dt_control_queue_redraw_widget(GtkWidget *widget)
{
  if(dt_control_running())
  {
    g_object_ref(widget);
    g_idle_add(_widget_queue_draw, (void*)widget);
  }
}

int dt_control_key_pressed_override(guint key, guint state)
{
  // TODO: if darkroom mode
  // did a : vim-style command start?
  static GList *autocomplete = NULL;
  dt_control_t *dc = darktable.control;
  if(dc->vimkey_cnt)
  {
    gunichar unichar = gdk_keyval_to_unicode(key);
    if(key == GDK_KEY_Return)
    {
      if(!strcmp(dc->vimkey, ":q"))
      {
        dt_control_quit();
      }
      else
      {
        dt_bauhaus_vimkey_exec(dc->vimkey);
      }
      dc->vimkey[0] = 0;
      dc->vimkey_cnt = 0;
      _control_log_ack_all();
      g_list_free(autocomplete);
      autocomplete = NULL;
    }
    else if(key == GDK_KEY_Escape)
    {
      dc->vimkey[0] = 0;
      dc->vimkey_cnt = 0;
      _control_log_ack_all();
      g_list_free(autocomplete);
      autocomplete = NULL;
    }
    else if(key == GDK_KEY_BackSpace)
    {
      dc->vimkey_cnt -= (dc->vimkey + dc->vimkey_cnt)
                        - g_utf8_prev_char(dc->vimkey + dc->vimkey_cnt);
      dc->vimkey[dc->vimkey_cnt] = 0;
      if(dc->vimkey_cnt == 0)
        _control_log_ack_all();
      else
        dt_control_log("%s", dc->vimkey);
      g_list_free(autocomplete);
      autocomplete = NULL;
    }
    else if(key == GDK_KEY_Tab)
    {
      // TODO: also support :preset and :get?
      // auto complete:
      if(dc->vimkey_cnt < 5)
      {
        g_strlcpy(dc->vimkey, ":set ", sizeof(dc->vimkey));
        dc->vimkey_cnt = 5;
      }
      else if(!autocomplete)
      {
        // TODO: handle '.'-separated things separately
        // this is a static list, and tab cycles through the list
        if(dc->vimkey_cnt < strlen(dc->vimkey))
          dc->vimkey[dc->vimkey_cnt] = 0;
        else
          autocomplete = dt_bauhaus_vimkey_complete(dc->vimkey + 5);
      }
      if(autocomplete)
      {
        // pop first.
        // the paths themselves are owned by bauhaus,
        // no free required.
        dc->vimkey[dc->vimkey_cnt] = 0;
        g_strlcat(dc->vimkey, (char *)autocomplete->data, sizeof(dc->vimkey));
        autocomplete = g_list_remove(autocomplete, autocomplete->data);
      }
      dt_control_log("%s", dc->vimkey);
    }
    else if(g_unichar_isprint(unichar)) // printable unicode character
    {
      gchar utf8[6] = { 0 };
      g_unichar_to_utf8(unichar, utf8);
      g_strlcat(dc->vimkey, utf8, sizeof(dc->vimkey));
      dc->vimkey_cnt = strlen(dc->vimkey);
      dt_control_log("%s", dc->vimkey);
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
    dc->vimkey[0] = ':';
    dc->vimkey[1] = 0;
    dc->vimkey_cnt = 1;
    dt_control_log("%s", dc->vimkey);
    return 1;
  }

  return 0;
}

void dt_control_hinter_message(const struct dt_control_t *s, const char *message)
{
  if(s->proxy.hinter.module)
    return s->proxy.hinter.set_message(s->proxy.hinter.module, message);
}

dt_imgid_t dt_control_get_mouse_over_id()
{
  dt_control_t *dc = darktable.control;
  dt_pthread_mutex_lock(&dc->global_mutex);
  const dt_imgid_t result = dc->mouse_over_id;
  dt_pthread_mutex_unlock(&dc->global_mutex);
  return result;
}

void dt_control_set_mouse_over_id(const dt_imgid_t imgid)
{
  dt_control_t *dc = darktable.control;
  dt_pthread_mutex_lock(&dc->global_mutex);
  if(dc->mouse_over_id != imgid)
  {
    dc->mouse_over_id = imgid;
    dt_pthread_mutex_unlock(&dc->global_mutex);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  }
  else
    dt_pthread_mutex_unlock(&dc->global_mutex);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
