/*
    This file is part of darktable,
    copyright (c) 2014 tobias ellinghaus.

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

#include "control/progress.h"
#include "control/control.h"

#ifdef HAVE_UNITY
#include <unity/unity/unity.h>
#endif
#ifdef MAC_INTEGRATION
#include <gtkosxapplication.h>
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


dt_progress_t *dt_control_progress_create(dt_control_t *control, gboolean has_progress_bar,
                                          const gchar *message)
{
  // create the object
  _dt_progress_t *progress = (_dt_progress_t *)calloc(1, sizeof(_dt_progress_t));
  dt_pthread_mutex_init(&(progress->mutex), NULL);

  // fill it with values
  progress->message = g_strdup(message);
  progress->has_progress_bar = has_progress_bar;

  dt_pthread_mutex_lock(&control->progress_system.mutex);

  // add it to the global list
  control->progress_system.list = g_list_append(control->progress_system.list, progress);
  control->progress_system.list_length++;

  // tell the gui
  if(control->progress_system.proxy.module != NULL)
    progress->gui_data = control->progress_system.proxy.added(control->progress_system.proxy.module,
                                                              has_progress_bar, message);

  dt_pthread_mutex_unlock(&control->progress_system.mutex);

#ifdef HAVE_UNITY
  if(has_progress_bar)
  {
    progress->darktable_launcher = unity_launcher_entry_get_for_desktop_id("darktable.desktop");
    unity_launcher_entry_set_progress(progress->darktable_launcher, 0.0);
    unity_launcher_entry_set_progress_visible(progress->darktable_launcher, TRUE);
  }
#endif

  // return the object
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

  dt_pthread_mutex_unlock(&control->progress_system.mutex);

#ifdef HAVE_UNITY
  if(progress->has_progress_bar)
  {
    unity_launcher_entry_set_progress(progress->darktable_launcher, 1.0);
    unity_launcher_entry_set_progress_visible(progress->darktable_launcher, FALSE);
  }
#endif

#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
  gtk_osxapplication_attention_request(g_object_new(GTK_TYPE_OSX_APPLICATION, NULL), INFO_REQUEST);
#else
  gtkosx_application_attention_request(g_object_new(GTKOSX_TYPE_APPLICATION, NULL), INFO_REQUEST);
#endif
#endif

  // free the object
  dt_pthread_mutex_destroy(&progress->mutex);
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
  dt_pthread_mutex_lock(&progress->mutex);
  progress->progress = value;
  dt_pthread_mutex_unlock(&progress->mutex);

  // tell the gui
  dt_pthread_mutex_lock(&control->progress_system.mutex);
  if(control->progress_system.proxy.module != NULL)
    control->progress_system.proxy.updated(control->progress_system.proxy.module, progress->gui_data, value);
  dt_pthread_mutex_unlock(&control->progress_system.mutex);

#ifdef HAVE_UNITY
  if(progress->has_progress_bar)
    unity_launcher_entry_set_progress(progress->darktable_launcher, CLAMP(value, 0, 1.0));
#endif
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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
