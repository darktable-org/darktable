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
#include "common/darktable.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "common/colorspaces.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/debug.h"
#include "bauhaus/bauhaus.h"
#include "views/view.h"
#include "gui/gtk.h"
#include "gui/draw.h"

#ifdef USE_COLORDGTK
#include "colord-gtk.h"
#endif

#ifdef GDK_WINDOWING_QUARTZ
#include <Carbon/Carbon.h>
#include <ApplicationServices/ApplicationServices.h>
#include <CoreServices/CoreServices.h>
#endif

#include <stdlib.h>
#include <strings.h>
#include <assert.h>
#include <math.h>
#include <string.h>
#include <glib/gstdio.h>
#include <gdk/gdkkeysyms.h>
#include <lcms2.h>


/* redraw mutex to synchronize redraws */
static dt_pthread_mutex_t _control_gdk_lock_threads_mutex;

int dt_control_load_config(dt_control_t *c)
{
  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);
  dt_conf_set_int("ui_last/view", DT_MODE_NONE);
  int width = dt_conf_get_int("ui_last/window_w");
  int height = dt_conf_get_int("ui_last/window_h");
#ifndef __WIN32__
  gint x = dt_conf_get_int("ui_last/window_x");
  gint y = dt_conf_get_int("ui_last/window_y");
  gtk_window_move(GTK_WINDOW(widget), x, y);
#endif
  gtk_window_resize(GTK_WINDOW(widget), width, height);
  int fullscreen = dt_conf_get_bool("ui_last/fullscreen");
  if(fullscreen)
    gtk_window_fullscreen(GTK_WINDOW(widget));
  else
  {
    gtk_window_unfullscreen(GTK_WINDOW(widget));
    int maximized = dt_conf_get_bool("ui_last/maximized");
    if(maximized)
      gtk_window_maximize(GTK_WINDOW(widget));
    else
      gtk_window_unmaximize(GTK_WINDOW(widget));
  }
  return 0;
}

int dt_control_write_config(dt_control_t *c)
{
  // TODO: move to gtk.c
  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  gint x, y;
  gtk_window_get_position(GTK_WINDOW(widget), &x, &y);
  dt_conf_set_int("ui_last/window_x", x);
  dt_conf_set_int("ui_last/window_y", y);
  dt_conf_set_int("ui_last/window_w", allocation.width);
  dt_conf_set_int("ui_last/window_h", allocation.height);
  dt_conf_set_bool("ui_last/maximized",
                   (gdk_window_get_state(gtk_widget_get_window(widget)) & GDK_WINDOW_STATE_MAXIMIZED));

  return 0;
}

#ifdef USE_COLORDGTK
static void dt_ctl_get_display_profile_colord_callback(GObject *source, GAsyncResult *res, gpointer user_data)
{
  pthread_rwlock_wrlock(&darktable.control->xprofile_lock);

  int profile_changed = 0;
  CdWindow *window = CD_WINDOW(source);
  GError *error = NULL;
  CdProfile *profile = cd_window_get_profile_finish(window, res, &error);
  if(error == NULL && profile != NULL)
  {
    const gchar *filename = cd_profile_get_filename(profile);
    if(filename)
    {
      if(g_strcmp0(filename, darktable.control->colord_profile_file))
      {
        /* the profile has changed (either because the user changed the colord settings or because we are on a
         * different screen now) */
        // update darktable.control->colord_profile_file
        g_free(darktable.control->colord_profile_file);
        darktable.control->colord_profile_file = g_strdup(filename);
        // read the file
        guchar *tmp_data = NULL;
        gsize size;
        g_file_get_contents(filename, (gchar **)&tmp_data, &size, NULL);
        profile_changed = size > 0 && (darktable.control->xprofile_size != size
                                       || memcmp(darktable.control->xprofile_data, tmp_data, size) != 0);
        if(profile_changed)
        {
          g_free(darktable.control->xprofile_data);
          darktable.control->xprofile_data = tmp_data;
          darktable.control->xprofile_size = size;
          dt_print(DT_DEBUG_CONTROL,
                   "[color profile] colord gave us a new screen profile: '%s' (size: %ld)\n", filename, size);
        }
      }
    }
  }
  if(profile) g_object_unref(profile);
  g_object_unref(window);

  pthread_rwlock_unlock(&darktable.control->xprofile_lock);
  if(profile_changed) dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_CHANGED);
}
#endif

// Get the display ICC profile of the monitor associated with the widget.
// For X display, uses the ICC profile specifications version 0.2 from
// http://burtonini.com/blog/computers/xicc
// Based on code from Gimp's modules/cdisplay_lcms.c
void dt_ctl_set_display_profile()
{
  if(!dt_control_running()) return;
  // make sure that no one gets a broken profile
  // FIXME: benchmark if the try is really needed when moving/resizing the window. Maybe we can just lock it
  // and block
  if(pthread_rwlock_trywrlock(&darktable.control->xprofile_lock))
    return; // we are already updating the profile. Or someone is reading right now. Too bad we can't
            // distinguish that. Whatever ...

  GtkWidget *widget = dt_ui_center(darktable.gui->ui);
  guint8 *buffer = NULL;
  gint buffer_size = 0;
  gchar *profile_source = NULL;

#if defined GDK_WINDOWING_X11

  // we will use the xatom no matter what configured when compiled without colord
  gboolean use_xatom = TRUE;
#if defined USE_COLORDGTK
  gboolean use_colord = TRUE;
  gchar *display_profile_source = dt_conf_get_string("ui_last/display_profile_source");
  if(display_profile_source)
  {
    if(!strcmp(display_profile_source, "xatom"))
      use_colord = FALSE;
    else if(!strcmp(display_profile_source, "colord"))
      use_xatom = FALSE;
    g_free(display_profile_source);
  }
#endif

  /* let's have a look at the xatom, just in case ... */
  if(use_xatom)
  {
    GdkScreen *screen = gtk_widget_get_screen(widget);
    if(screen == NULL) screen = gdk_screen_get_default();
    int monitor = gdk_screen_get_monitor_at_window(screen, gtk_widget_get_window(widget));
    char *atom_name;
    if(monitor > 0)
      atom_name = g_strdup_printf("_ICC_PROFILE_%d", monitor);
    else
      atom_name = g_strdup("_ICC_PROFILE");

    profile_source = g_strdup_printf("xatom %s", atom_name);

    GdkAtom type = GDK_NONE;
    gint format = 0;
    gdk_property_get(gdk_screen_get_root_window(screen), gdk_atom_intern(atom_name, FALSE), GDK_NONE, 0,
                     64 * 1024 * 1024, FALSE, &type, &format, &buffer_size, &buffer);
    g_free(atom_name);
  }

#ifdef USE_COLORDGTK
  /* also try to get the profile from colord. this will set the value asynchronously! */
  if(use_colord)
  {
    CdWindow *window = cd_window_new();
    GtkWidget *center_widget = dt_ui_center(darktable.gui->ui);
    cd_window_get_profile(window, center_widget, NULL, dt_ctl_get_display_profile_colord_callback, NULL);
  }
#endif

#elif defined GDK_WINDOWING_QUARTZ
  GdkScreen *screen = gtk_widget_get_screen(widget);
  if(screen == NULL) screen = gdk_screen_get_default();
  int monitor = gdk_screen_get_monitor_at_window(screen, gtk_widget_get_window(widget));

  CGDirectDisplayID ids[monitor + 1];
  uint32_t total_ids;
  CMProfileRef prof = NULL;
  if(CGGetOnlineDisplayList(monitor + 1, &ids[0], &total_ids) == kCGErrorSuccess && total_ids == monitor + 1)
    CMGetProfileByAVID(ids[monitor], &prof);
  if(prof != NULL)
  {
    CFDataRef data;
    data = CMProfileCopyICCData(NULL, prof);
    CMCloseProfile(prof);

    UInt8 *tmp_buffer = (UInt8 *)g_malloc(CFDataGetLength(data));
    CFDataGetBytes(data, CFRangeMake(0, CFDataGetLength(data)), tmp_buffer);

    buffer = (guint8 *)tmp_buffer;
    buffer_size = CFDataGetLength(data);

    CFRelease(data);
  }
  profile_source = g_strdup("osx color profile api");
#elif defined G_OS_WIN32
  (void)widget;
  HDC hdc = GetDC(NULL);
  if(hdc != NULL)
  {
    DWORD len = 0;
    GetICMProfile(hdc, &len, NULL);
    gchar *path = g_new(gchar, len);

    if(GetICMProfile(hdc, &len, path))
    {
      gsize size;
      g_file_get_contents(path, (gchar **)&buffer, &size, NULL);
      buffer_size = size;
    }
    g_free(path);
    ReleaseDC(NULL, hdc);
  }
  profile_source = g_strdup("windows color profile api");
#endif

  int profile_changed = buffer_size > 0
                        && (darktable.control->xprofile_size != buffer_size
                            || memcmp(darktable.control->xprofile_data, buffer, buffer_size) != 0);
  if(profile_changed)
  {
    cmsHPROFILE profile = NULL;
    char name[512] = { 0 };
    // thanks to ufraw for this!
    g_free(darktable.control->xprofile_data);
    darktable.control->xprofile_data = buffer;
    darktable.control->xprofile_size = buffer_size;
    profile = cmsOpenProfileFromMem(buffer, buffer_size);
    if(profile)
    {
      dt_colorspaces_get_profile_name(profile, "en", "US", name, sizeof(name));
      cmsCloseProfile(profile);
    }
    dt_print(DT_DEBUG_CONTROL, "[color profile] we got a new screen profile `%s' from the %s (size: %d)\n",
             *name ? name : "(unknown)", profile_source, buffer_size);
  }
  pthread_rwlock_unlock(&darktable.control->xprofile_lock);
  if(profile_changed) dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_CHANGED);
  g_free(profile_source);
}

void dt_control_init(dt_control_t *s)
{
  memset(s->vimkey, 0, sizeof(s->vimkey));
  s->vimkey_cnt = 0;

  // same thread as init
  s->gui_thread = pthread_self();

  // initialize static mutex
  dt_pthread_mutex_init(&_control_gdk_lock_threads_mutex, NULL);

  // s->last_expose_time = dt_get_wtime();
  s->key_accelerators_on = 1;
  s->log_pos = s->log_ack = 0;
  s->log_busy = 0;
  s->log_message_timeout_id = 0;
  dt_pthread_mutex_init(&(s->log_mutex), NULL);

  dt_conf_set_int("ui_last/view", DT_MODE_NONE);

  pthread_cond_init(&s->cond, NULL);
  dt_pthread_mutex_init(&s->cond_mutex, NULL);
  dt_pthread_mutex_init(&s->queue_mutex, NULL);
  dt_pthread_mutex_init(&s->run_mutex, NULL);
  pthread_rwlock_init(&s->xprofile_lock, NULL);
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

void dt_control_change_cursor(dt_cursor_t curs)
{
  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);
  GdkCursor *cursor = gdk_cursor_new(curs);
  gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
  g_object_unref(cursor);
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
#ifdef HAVE_MAP
  // since map mode doesn't like to quit we just switch to lighttable mode. hacky, but it works :(
  if(dt_conf_get_int("ui_last/view") == DT_MAP) // we are in map mode where no expose is running
    dt_ctl_switch_mode_to(DT_LIBRARY);
#endif

  dt_gui_gtk_quit();
  // thread safe quit, 1st pass:
  dt_pthread_mutex_lock(&darktable.control->cond_mutex);
  dt_pthread_mutex_lock(&darktable.control->run_mutex);
  darktable.control->running = 0;
  dt_pthread_mutex_unlock(&darktable.control->run_mutex);
  dt_pthread_mutex_unlock(&darktable.control->cond_mutex);
  // let gui pick up the running = 0 state and die
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
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

  gdk_threads_leave();
  int k;
  for(k = 0; k < s->num_threads; k++)
    // pthread_kill(s->thread[k], 9);
    pthread_join(s->thread[k], NULL);
  for(k = 0; k < DT_CTL_WORKER_RESERVED; k++)
    // pthread_kill(s->thread_res[k], 9);
    pthread_join(s->thread_res[k], NULL);


  gdk_threads_enter();
}

void dt_control_cleanup(dt_control_t *s)
{
  // vacuum TODO: optional?
  // DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "PRAGMA incremental_vacuum(0)", NULL, NULL, NULL);
  // DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "vacuum", NULL, NULL, NULL);
  dt_pthread_mutex_destroy(&s->queue_mutex);
  dt_pthread_mutex_destroy(&s->cond_mutex);
  dt_pthread_mutex_destroy(&s->log_mutex);
  dt_pthread_mutex_destroy(&s->run_mutex);
  dt_pthread_mutex_destroy(&s->progress_system.mutex);
  pthread_rwlock_destroy(&s->xprofile_lock);
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
  darktable.control->tabborder = 8;
  int tb = darktable.control->tabborder;
  // re-configure all components:
  dt_view_manager_configure(darktable.view_manager, event->width - 2 * tb, event->height - 2 * tb);
  return TRUE;
}

void *dt_control_expose(void *voidptr)
{
  int width, height, pointerx, pointery;
  if(!darktable.gui->surface) return NULL;
  width = dt_cairo_image_surface_get_width(darktable.gui->surface);
  height = dt_cairo_image_surface_get_height(darktable.gui->surface);
  GtkWidget *widget = dt_ui_center(darktable.gui->ui);
  GdkDevice *device
      = gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gtk_widget_get_display(widget)));
  gdk_window_get_device_position(gtk_widget_get_window(widget), device, &pointerx, &pointery, NULL);

  // create a gtk-independent surface to draw on
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // TODO: control_expose: only redraw the part not overlapped by temporary control panel show!
  //
  float tb = 8; // fmaxf(10, width/100.0);
  darktable.control->tabborder = tb;
  darktable.control->width = width;
  darktable.control->height = height;

  GdkRGBA color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gboolean color_found = gtk_style_context_lookup_color (context, "bg_color", &color);
  if(!color_found)
  {
    color.red = 1.0;
    color.green = 0.0;
    color.blue = 0.0;
    color.alpha = 1.0;
  }
  gdk_cairo_set_source_rgba(cr, &color);

  cairo_set_line_width(cr, tb);
  cairo_rectangle(cr, tb / 2., tb / 2., width - tb, height - tb);
  cairo_stroke(cr);
  cairo_set_line_width(cr, 1.5);
  color_found = gtk_style_context_lookup_color (context, "really_dark_bg_color", &color);
  if(!color_found)
  {
    color.red = 1.0;
    color.green = 0.0;
    color.blue = 0.0;
    color.alpha = 1.0;
  }
  gdk_cairo_set_source_rgba(cr, &color);
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
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    const float fontsize = 14;
    cairo_set_font_size(cr, fontsize);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, darktable.control->log_message[darktable.control->log_ack], &ext);
    const float pad = 20.0f, xc = width / 2.0;
    const float yc = height * 0.85 + 10, wd = pad + ext.width * .5f;
    float rad = 14;
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
        color_found = gtk_style_context_lookup_color (context, "selected_bg_color", &color);
        if(!color_found)
        {
          color.red = 1.0;
          color.green = 0.0;
          color.blue = 0.0;
          color.alpha = 1.0;
        }
        gdk_cairo_set_source_rgba(cr, &color);
        cairo_fill_preserve(cr);
      }
      cairo_set_source_rgba(cr, 0., 0., 0., 1.0 / (1 + k));
      cairo_stroke(cr);
      rad += .5f;
    }
    color_found = gtk_style_context_lookup_color (context, "fg_color", &color);
    if(!color_found)
    {
      color.red = 1.0;
      color.green = 0.0;
      color.blue = 0.0;
      color.alpha = 1.0;
    }
    gdk_cairo_set_source_rgba(cr, &color);
    cairo_move_to(cr, xc - wd + .5f * pad, yc + 1. / 3. * fontsize);
    cairo_show_text(cr, darktable.control->log_message[darktable.control->log_ack]);
  }
  // draw busy indicator
  if(darktable.control->log_busy > 0)
  {
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    const float fontsize = 14;
    cairo_set_font_size(cr, fontsize);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, _("working.."), &ext);
    const float xc = width / 2.0, yc = height * 0.85 - 30, wd = ext.width * .5f;
    cairo_move_to(cr, xc - wd, yc + 1. / 3. * fontsize);
    cairo_text_path(cr, _("working.."));
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 0.7);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_stroke(cr);
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
  float tb = darktable.control->tabborder;
  float wd = darktable.control->width;
  float ht = darktable.control->height;

  if(x > tb && x < wd - tb && y > tb && y < ht - tb)
    dt_view_manager_mouse_moved(darktable.view_manager, x - tb, y - tb, pressure, which);
}

void dt_control_button_released(double x, double y, int which, uint32_t state)
{
  darktable.control->button_down = 0;
  darktable.control->button_down_which = 0;
  float tb = darktable.control->tabborder;
  // float wd = darktable.control->width;
  // float ht = darktable.control->height;

  // always do this, to avoid missing some events.
  // if(x > tb && x < wd-tb && y > tb && y < ht-tb)
  dt_view_manager_button_released(darktable.view_manager, x - tb, y - tb, which, state);
}

void dt_ctl_switch_mode_to(dt_control_gui_mode_t mode)
{
  dt_control_gui_mode_t oldmode = dt_conf_get_int("ui_last/view");
  if(oldmode == mode) return;

  darktable.control->button_down = 0;
  darktable.control->button_down_which = 0;
  darktable.gui->center_tooltip = 0;
  GtkWidget *widget = dt_ui_center(darktable.gui->ui);
  g_object_set(G_OBJECT(widget), "tooltip-text", "", (char *)NULL);

  char buf[512];
  snprintf(buf, sizeof(buf) - 1, _("switch to %s mode"), dt_view_manager_name(darktable.view_manager));

  gboolean i_own_lock = dt_control_gdk_lock();

  int error = dt_view_manager_switch(darktable.view_manager, mode);

  if(i_own_lock) dt_control_gdk_unlock();

  if(error) return;

  dt_conf_set_int("ui_last/view", mode);
}

void dt_ctl_switch_mode()
{
  dt_control_gui_mode_t mode = dt_conf_get_int("ui_last/view");
  if(mode == DT_LIBRARY)
    mode = DT_DEVELOP;
  else
    mode = DT_LIBRARY;
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
  float tb = darktable.control->tabborder;
  darktable.control->button_down = 1;
  darktable.control->button_down_which = which;
  darktable.control->button_type = type;
  darktable.control->button_x = x - tb;
  darktable.control->button_y = y - tb;
  // adding pressure to this data structure is not needed right now. should the need ever arise: here is the
  // place to do it :)
  float wd = darktable.control->width;
  float ht = darktable.control->height;

  // ack log message:
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  const float /*xc = wd/4.0-20,*/ yc = ht * 0.85 + 10;
  if(darktable.control->log_ack != darktable.control->log_pos)
    if(which == 1 /*&& x > xc - 10 && x < xc + 10*/ && y > yc - 10 && y < yc + 10)
    {
      if(darktable.control->log_message_timeout_id)
        g_source_remove(darktable.control->log_message_timeout_id);
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

static __thread gboolean _control_gdk_lock_mine = FALSE;
gboolean dt_control_gdk_lock()
{
  /* if current thread equals gui thread do nothing */
  if(pthread_equal(darktable.control->gui_thread, pthread_self()) != 0) return FALSE;

  dt_pthread_mutex_lock(&_control_gdk_lock_threads_mutex);

  /* lets check if current thread has a managed lock */
  if(_control_gdk_lock_mine)
  {
    /* current thread has a lock just do nothing */
    dt_pthread_mutex_unlock(&_control_gdk_lock_threads_mutex);
    return FALSE;
  }

  /* lets lock */
  _control_gdk_lock_mine = TRUE;
  dt_pthread_mutex_unlock(&_control_gdk_lock_threads_mutex);

  /* enter gdk critical section */
  gdk_threads_enter();

  return TRUE;
}

void dt_control_gdk_unlock()
{
  /* check if current thread has a lock and remove if exists */
  dt_pthread_mutex_lock(&_control_gdk_lock_threads_mutex);
  if(_control_gdk_lock_mine)
  {
    /* remove lock */
    _control_gdk_lock_mine = FALSE;

    /* leave critical section */
    gdk_threads_leave();
  }
  dt_pthread_mutex_unlock(&_control_gdk_lock_threads_mutex);
}

gboolean dt_control_gdk_haslock()
{
  if(pthread_equal(darktable.control->gui_thread, pthread_self()) != 0) return TRUE;
  return _control_gdk_lock_mine;
}

void dt_control_queue_redraw()
{
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_ALL);
}

void dt_control_queue_redraw_center()
{
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_CENTER);
}

void dt_control_queue_redraw_widget(GtkWidget *widget)
{
  if(dt_control_running())
  {
    gboolean i_own_lock = dt_control_gdk_lock();

    gtk_widget_queue_draw(widget);

    if(i_own_lock) dt_control_gdk_unlock();
  }
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
        dt_control_log(darktable.control->vimkey);
      g_list_free(autocomplete);
      autocomplete = NULL;
    }
    else if(key == GDK_KEY_Tab)
    {
      // TODO: also support :preset and :get?
      // auto complete:
      if(darktable.control->vimkey_cnt < 5)
      {
        snprintf(darktable.control->vimkey, sizeof(darktable.control->vimkey), ":set ");
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
      dt_control_log(darktable.control->vimkey);
    }
    else if(g_unichar_isprint(unichar)) // printable unicode character
    {
      gchar utf8[6];
      gint char_width = g_unichar_to_utf8(unichar, utf8);
      if(darktable.control->vimkey_cnt + 1 + char_width < 256)
      {
        g_utf8_strncpy(darktable.control->vimkey + darktable.control->vimkey_cnt, utf8, 1);
        darktable.control->vimkey_cnt += char_width;
        darktable.control->vimkey[darktable.control->vimkey_cnt] = 0;
        dt_control_log(darktable.control->vimkey);
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
    dt_control_log(darktable.control->vimkey);
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
    char key[512];
    const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);

    /* do nothing if in collapse panel state
       TODO: reconsider adding this check to ui api */
    g_snprintf(key, sizeof(key), "%s/ui/panel_collaps_state", cv->module_name);
    if(dt_conf_get_int(key)) return 0;

    /* toggle the header visibility state */
    g_snprintf(key, sizeof(key), "%s/ui/show_header", cv->module_name);
    gboolean header = !dt_conf_get_bool(key);
    dt_conf_set_bool(key, header);

    /* show/hide the actual header panel */
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, header, TRUE);
    gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
    return 1;
  }
  return 0;
}

int dt_control_key_pressed(guint key, guint state)
{
  int handled = dt_view_manager_key_pressed(darktable.view_manager, key, state);
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
  int32_t result = darktable.control->mouse_over_id;
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
  float result = darktable.control->dev_zoom_x;
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
  float result = darktable.control->dev_zoom_y;
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
  float result = darktable.control->dev_zoom_scale;
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
  int result = darktable.control->dev_closeup;
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
  dt_dev_zoom_t result = darktable.control->dev_zoom;
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
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
