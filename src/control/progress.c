/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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

#include "common/dbus.h"
#include "control/progress.h"
#include "control/control.h"

#ifdef HAVE_UNITY
#include <unity/unity/unity.h>
#endif
#ifdef MAC_INTEGRATION
#include <gtkosxapplication.h>
#endif

#ifdef _WIN32
#include <gdk/gdkwin32.h>
#ifndef ITaskbarList3_SetProgressValue
  #define ITaskbarList3_SetProgressValue(This,hwnd,ullCompleted,ullTotal) (This)->lpVtbl->SetProgressValue(This,hwnd,ullCompleted,ullTotal)
#endif
#ifndef ITaskbarList3_SetProgressState
  #define ITaskbarList3_SetProgressState(This,hwnd,tbpFlags) (This)->lpVtbl->SetProgressState(This,hwnd,tbpFlags)
#endif
#ifndef ITaskbarList3_HrInit
  #define ITaskbarList3_HrInit(This) (This)->lpVtbl->HrInit(This)
#endif
#endif


typedef struct _dt_progress_t
{
  double progress;
  gchar *message;
  gboolean has_progress_bar;
  dt_pthread_mutex_t mutex;
  void *gui_data;

  // cancel callback and its data
  dt_progress_cancel_callback_t cancel;
  void *cancel_data;

#ifdef HAVE_UNITY
  UnityLauncherEntry *darktable_launcher;
#endif

} _dt_progress_t;

static void global_progress_start(dt_control_t *control, dt_progress_t *progress)
{
  control->progress_system.n_progress_bar++;

#ifndef _WIN32

#ifdef HAVE_UNITY

  progress->darktable_launcher = unity_launcher_entry_get_for_desktop_id("org.darktable.darktable.desktop");
  unity_launcher_entry_set_progress(progress->darktable_launcher, 0.0);
  unity_launcher_entry_set_progress_visible(progress->darktable_launcher, TRUE);

#else

  // this should work for unity as well as kde
  // https://wiki.ubuntu.com/Unity/LauncherAPI#Low_level_DBus_API:_com.canonical.Unity.LauncherEntry
  if(darktable.dbus && darktable.dbus->dbus_connection)
  {
    GError *error = NULL;
    g_object_ref(G_OBJECT(darktable.dbus->dbus_connection));

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", "progress", g_variant_new_double(control->progress_system.global_progress));
    g_variant_builder_add(&builder, "{sv}", "progress-visible", g_variant_new_boolean(TRUE));
    GVariant *params = g_variant_new("(sa{sv})", "application://org.darktable.darktable.desktop", &builder);

    g_dbus_connection_emit_signal(darktable.dbus->dbus_connection,
                                  "com.canonical.Unity",
                                  "/darktable",
                                  "com.canonical.Unity.LauncherEntry",
                                  "Update",
                                  params,
                                  &error);
    if(error)
    {
      fprintf(stderr, "[progress_create] dbus error: %s\n", error->message);
      g_error_free(error);
    }
  }

#endif // HAVE_UNITY

#else // _WIN32

  // we can't init this in dt_control_progress_init as it's run too early :/
  if(!control->progress_system.taskbarlist)
  {
    void *taskbarlist;
    if(CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void **)&taskbarlist) == S_OK)
      if(ITaskbarList3_HrInit((ITaskbarList3 *)taskbarlist) == S_OK)
        control->progress_system.taskbarlist = taskbarlist;
  }

  if(control->progress_system.taskbarlist)
  {
    HWND hwnd = GDK_WINDOW_HWND(gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui)));
    if(ITaskbarList3_SetProgressState(control->progress_system.taskbarlist, hwnd, TBPF_NORMAL) != S_OK)
      fprintf(stderr, "[progress_create] SetProgressState failed\n");
    if(ITaskbarList3_SetProgressValue(control->progress_system.taskbarlist, hwnd, control->progress_system.global_progress * 100, 100) != S_OK)
      fprintf(stderr, "[progress_create] SetProgressValue failed\n");
  }

#endif
}

static void global_progress_set(dt_control_t *control, dt_progress_t *progress, double value)
{
  control->progress_system.global_progress = MAX(control->progress_system.global_progress, value);

#ifndef _WIN32

#ifdef HAVE_UNITY

  unity_launcher_entry_set_progress(progress->darktable_launcher, value);

#else

  if(darktable.dbus && darktable.dbus->dbus_connection)
  {
    GError *error = NULL;

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", "progress", g_variant_new_double(control->progress_system.global_progress));
    GVariant *params = g_variant_new("(sa{sv})", "application://org.darktable.darktable.desktop", &builder);

    g_dbus_connection_emit_signal(darktable.dbus->dbus_connection,
                                  "com.canonical.Unity",
                                  "/darktable",
                                  "com.canonical.Unity.LauncherEntry",
                                  "Update",
                                  params,
                                  &error);
    if(error)
    {
      fprintf(stderr, "[progress_set] dbus error: %s\n", error->message);
      g_error_free(error);
    }
  }

#endif // HAVE_UNITY

#else // _WIN32

  if(control->progress_system.taskbarlist)
  {
    HWND hwnd = GDK_WINDOW_HWND(gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui)));
    if(ITaskbarList3_SetProgressValue(control->progress_system.taskbarlist, hwnd, control->progress_system.global_progress * 100, 100) != S_OK)
      fprintf(stderr, "[progress_create] SetProgressValue failed\n");
  }

#endif
}

static void global_progress_end(dt_control_t *control, dt_progress_t *progress)
{
  control->progress_system.n_progress_bar--;

  // find the biggest progress value among the remaining progress bars
  control->progress_system.global_progress = 0.0;
  for(GList *iter = control->progress_system.list; iter; iter = g_list_next(iter))
  {
    // this is called after the current progress got removed from the list!
    dt_progress_t *p = (dt_progress_t *)iter->data;
    const double value = dt_control_progress_get_progress(p);
    control->progress_system.global_progress = MAX(control->progress_system.global_progress, value);
  }

#ifndef _WIN32

#ifdef HAVE_UNITY

  unity_launcher_entry_set_progress(progress->darktable_launcher, 1.0);
  unity_launcher_entry_set_progress_visible(progress->darktable_launcher, FALSE);

#else

  if(darktable.dbus && darktable.dbus->dbus_connection)
  {
    GError *error = NULL;

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    if(control->progress_system.n_progress_bar == 0)
      g_variant_builder_add(&builder, "{sv}", "progress-visible", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&builder, "{sv}", "progress", g_variant_new_double(control->progress_system.global_progress));
    GVariant *params = g_variant_new("(sa{sv})", "application://org.darktable.darktable.desktop", &builder);

    g_dbus_connection_emit_signal(darktable.dbus->dbus_connection,
                                  "com.canonical.Unity",
                                  "/darktable",
                                  "com.canonical.Unity.LauncherEntry",
                                  "Update",
                                  params,
                                  &error);
    if(error)
    {
      fprintf(stderr, "[progress_destroy] dbus error: %s\n", error->message);
      g_error_free(error);
    }

    g_object_unref(G_OBJECT(darktable.dbus->dbus_connection));
    darktable.dbus->dbus_connection = NULL;
  }

#endif // HAVE_UNITY

#else // _WIN32

  if(control->progress_system.taskbarlist)
  {
    HWND hwnd = GDK_WINDOW_HWND(gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui)));
    if(control->progress_system.n_progress_bar == 0)
    {
      if(ITaskbarList3_SetProgressState(control->progress_system.taskbarlist, hwnd, TBPF_NOPROGRESS) != S_OK)
        fprintf(stderr, "[progress_create] SetProgressState failed\n");
    }
    else
    {
      if(ITaskbarList3_SetProgressValue(control->progress_system.taskbarlist, hwnd,
                                        control->progress_system.global_progress * 100, 100) != S_OK)
        fprintf(stderr, "[progress_create] SetProgressValue failed\n");
    }
  }

#endif
}

void dt_control_progress_init(struct dt_control_t *control)
{
#ifndef _WIN32

#ifdef HAVE_UNITY

  UnityLauncherEntry *darktable_launcher = unity_launcher_entry_get_for_desktop_id("org.darktable.darktable.desktop");
  unity_launcher_entry_set_progress_visible(darktable_launcher, FALSE);

#else

  if(darktable.dbus->dbus_connection)
  {
    GError *error = NULL;

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", "progress-visible", g_variant_new_boolean(FALSE));
    GVariant *params = g_variant_new("(sa{sv})", "application://org.darktable.darktable.desktop", &builder);

    g_dbus_connection_emit_signal(darktable.dbus->dbus_connection,
                                  "com.canonical.Unity",
                                  "/darktable",
                                  "com.canonical.Unity.LauncherEntry",
                                  "Update",
                                  params,
                                  &error);
    if(error)
    {
      fprintf(stderr, "[progress_init] dbus error: %s\n", error->message);
      g_error_free(error);
    }

    g_object_unref(G_OBJECT(darktable.dbus->dbus_connection));
    darktable.dbus->dbus_connection = NULL;
  }

#endif // HAVE_UNITY

#else // _WIN32

  // initializing control->progress_system.taskbarlist in here doesn't work,
  // it seems to only succeed after dt_gui_gtk_init

#endif // _WIN32
}

dt_progress_t *dt_control_progress_create(dt_control_t *control, gboolean has_progress_bar,
                                          const gchar *message)
{
  // create the object
  dt_progress_t *progress = (dt_progress_t *)calloc(1, sizeof(dt_progress_t));
  dt_pthread_mutex_init(&(progress->mutex), NULL);

  // fill it with values
  progress->message = g_strdup(message);
  progress->has_progress_bar = has_progress_bar;

  dt_pthread_mutex_lock(&control->progress_system.mutex);

  // add it to the global list
  control->progress_system.list = g_list_append(control->progress_system.list, progress);
  control->progress_system.list_length++;
  if(has_progress_bar) global_progress_start(control, progress);

  // tell the gui
  if(control->progress_system.proxy.module != NULL)
    progress->gui_data = control->progress_system.proxy.added(control->progress_system.proxy.module,
                                                              has_progress_bar, message);

  dt_pthread_mutex_unlock(&control->progress_system.mutex);

  return progress;
}

void dt_control_progress_destroy(dt_control_t *control, dt_progress_t *progress)
{
  dt_pthread_mutex_lock(&control->progress_system.mutex);

  // tell the gui
  if(control->progress_system.proxy.module != NULL)
    control->progress_system.proxy.destroyed(control->progress_system.proxy.module, progress->gui_data);

  // remove the object from the global list
  control->progress_system.list = g_list_remove(control->progress_system.list, progress);
  control->progress_system.list_length--;
  if(progress->has_progress_bar) global_progress_end(control, progress);

  dt_pthread_mutex_unlock(&control->progress_system.mutex);

  // free the object
  dt_pthread_mutex_destroy(&progress->mutex);
  g_free(progress->message);
  free(progress);
}

void dt_control_progress_make_cancellable(struct dt_control_t *control, dt_progress_t *progress,
                                          dt_progress_cancel_callback_t cancel, void *data)
{
  // set the value
  dt_pthread_mutex_lock(&progress->mutex);
  progress->cancel = cancel;
  progress->cancel_data = data;
  dt_pthread_mutex_unlock(&progress->mutex);

  // tell the gui
  dt_pthread_mutex_lock(&control->progress_system.mutex);
  if(control->progress_system.proxy.module != NULL)
    control->progress_system.proxy.cancellable(control->progress_system.proxy.module, progress->gui_data,
                                               progress);
  dt_pthread_mutex_unlock(&control->progress_system.mutex);
}

static void dt_control_progress_cancel_callback(dt_progress_t *progress, void *data)
{
  dt_control_job_cancel((dt_job_t *)data);
}

void dt_control_progress_attach_job(dt_control_t *control, dt_progress_t *progress, dt_job_t *job)
{
  dt_control_progress_make_cancellable(control, progress, &dt_control_progress_cancel_callback, job);
}

void dt_control_progress_cancel(dt_control_t *control, dt_progress_t *progress)
{
  dt_pthread_mutex_lock(&progress->mutex);
  if(progress->cancel == NULL)
  {
    dt_pthread_mutex_unlock(&progress->mutex);
    return;
  }

  // call the cancel callback
  progress->cancel(progress, progress->cancel_data);

  dt_pthread_mutex_unlock(&progress->mutex);

  // the gui doesn't need to know I guess, it wouldn't to anything with that bit of information
}

void dt_control_progress_set_progress(dt_control_t *control, dt_progress_t *progress, double value)
{
  // set the value
  value = CLAMP(value, 0.0, 1.0);
  dt_pthread_mutex_lock(&progress->mutex);
  progress->progress = value;
  dt_pthread_mutex_unlock(&progress->mutex);

  // tell the gui
  dt_pthread_mutex_lock(&control->progress_system.mutex);
  if(control->progress_system.proxy.module != NULL)
    control->progress_system.proxy.updated(control->progress_system.proxy.module, progress->gui_data, value);

  if(progress->has_progress_bar) global_progress_set(control, progress, value);

  dt_pthread_mutex_unlock(&control->progress_system.mutex);
}

double dt_control_progress_get_progress(dt_progress_t *progress)
{
  dt_pthread_mutex_lock(&progress->mutex);
  double res = progress->progress;
  dt_pthread_mutex_unlock(&progress->mutex);
  return res;
}

const gchar *dt_control_progress_get_message(dt_progress_t *progress)
{
  dt_pthread_mutex_lock(&progress->mutex);
  const gchar *res = progress->message;
  dt_pthread_mutex_unlock(&progress->mutex);
  return res;
}

void dt_control_progress_set_message(dt_control_t *control, dt_progress_t *progress, const char *message)
{
  dt_pthread_mutex_lock(&progress->mutex);
  g_free(progress->message);
  progress->message = g_strdup(message);
  dt_pthread_mutex_unlock(&progress->mutex);

  // tell the gui
  dt_pthread_mutex_lock(&control->progress_system.mutex);
  if(control->progress_system.proxy.module != NULL)
    control->progress_system.proxy.message_updated(control->progress_system.proxy.module, progress->gui_data,
                                                   message);
  dt_pthread_mutex_unlock(&control->progress_system.mutex);
}

void dt_control_progress_set_gui_data(dt_progress_t *progress, void *data)
{
  dt_pthread_mutex_lock(&progress->mutex);
  progress->gui_data = data;
  dt_pthread_mutex_unlock(&progress->mutex);
}

void *dt_control_progress_get_gui_data(dt_progress_t *progress)
{
  dt_pthread_mutex_lock(&progress->mutex);
  void *res = progress->gui_data;
  dt_pthread_mutex_unlock(&progress->mutex);
  return res;
}

gboolean dt_control_progress_has_progress_bar(dt_progress_t *progress)
{
  dt_pthread_mutex_lock(&progress->mutex);
  gboolean res = progress->has_progress_bar;
  dt_pthread_mutex_unlock(&progress->mutex);
  return res;
}

gboolean dt_control_progress_cancellable(dt_progress_t *progress)
{
  dt_pthread_mutex_lock(&progress->mutex);
  gboolean res = progress->cancel != NULL;
  dt_pthread_mutex_unlock(&progress->mutex);
  return res;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

