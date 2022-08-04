/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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
#include "control/jobs/camera_jobs.h"
#include "common/darktable.h"
#include "common/collection.h"
#include "common/import_session.h"
#include "common/utility.h"
#include "common/datetime.h"
#include "control/conf.h"
#include "control/jobs/image_jobs.h"
#include "gui/gtk.h"
#include "views/view.h"

#include <glib.h>
#include <stdio.h>

typedef struct dt_camera_shared_t
{
  struct dt_import_session_t *session;
} dt_camera_shared_t;

typedef struct dt_camera_capture_t
{
  dt_camera_shared_t shared;

  /** delay between each capture, 0 no delay */
  uint32_t delay;
  /** count of images to capture, 0==1 */
  uint32_t count;
  /** bracket capture, 0=no bracket */
  uint32_t brackets;

  /** steps for each bracket, only used ig bracket capture*/
  uint32_t steps;

} dt_camera_capture_t;

typedef struct dt_camera_get_previews_t
{
  struct dt_camera_t *camera;
  uint32_t flags;
  struct dt_camctl_listener_t *listener;
  void *data;
} dt_camera_get_previews_t;

typedef struct dt_camera_import_t
{
  dt_camera_shared_t shared;

  GList *images;
  struct dt_camera_t *camera;
  dt_job_t *job;
  double fraction;
  uint32_t import_count;
} dt_camera_import_t;

static int32_t dt_camera_capture_job_run(dt_job_t *job)
{
  dt_camera_capture_t *params = dt_control_job_get_params(job);
  int total;
  char message[512] = { 0 };
  double fraction = 0;

  total = params->brackets ? params->count * params->brackets : params->count;
  snprintf(message, sizeof(message), ngettext("Capturing %d image", "Capturing %d images", total), total);

  dt_control_job_set_progress_message(job, message);

  /* try to get exp program mode for nikon */
  char *expprogram = (char *)dt_camctl_camera_get_property(darktable.camctl, NULL, "expprogram");

  /* if fail, lets try fetching mode for cannon */
  if(!expprogram)
    expprogram = (char *)dt_camctl_camera_get_property(darktable.camctl, NULL, "autoexposuremode");

  /* Fetch all values for shutterspeed and initialize current value */
  GList *values = NULL;
  gconstpointer original_value = NULL;
  const char *cvalue = dt_camctl_camera_get_property(darktable.camctl, NULL, "shutterspeed");
  const char *value = dt_camctl_camera_property_get_first_choice(darktable.camctl, NULL, "shutterspeed");

  /* get values for bracketing */
  if(params->brackets && expprogram && expprogram[0] == 'M' && value && cvalue)
  {
    do
    {
      // Add value to list
      values = g_list_prepend(values, g_strdup(value));
      // Check if current values is the same as original value, then lets store item ptr
      if(strcmp(value, cvalue) == 0) original_value = values->data;
    } while((value = dt_camctl_camera_property_get_next_choice(darktable.camctl, NULL, "shutterspeed"))
            != NULL);
  }
  else
  {
    /* if this was an intended bracket capture bail out */
    if(params->brackets)
    {
      dt_control_log(_("Please set your camera to manual mode first!"));
      return 1;
    }
  }

  GList *current_value = g_list_find(values, original_value);
  for(uint32_t i = 0; i < params->count; i++)
  {
    // Delay if active
    if(params->delay && !params->brackets) // delay between brackets
      g_usleep(params->delay * G_USEC_PER_SEC);

    for(uint32_t b = 0; b < (params->brackets * 2) + 1; b++)
    {
      // If bracket capture, lets set change shutterspeed
      if(params->brackets)
      {
        if(b == 0)
        {
          // First bracket, step down time with (steps*brackets), also check so we never set the longest
          // shuttertime which would be bulb mode
          for(uint32_t s = 0; s < (params->steps * params->brackets); s++)
            if(g_list_next(current_value) && g_list_next(g_list_next(current_value)))
              current_value = g_list_next(current_value);
        }
        else
        {
          if(params->delay) // delay after previous bracket (no delay for 1st bracket)
            g_usleep(params->delay * G_USEC_PER_SEC);

          // Step up with (steps)
          for(uint32_t s = 0; s < params->steps; s++)
            if(g_list_previous(current_value)) current_value = g_list_previous(current_value);
        }
      }

      // set the time property for bracket capture
      if(params->brackets && current_value)
        dt_camctl_camera_set_property_string(darktable.camctl, NULL, "shutterspeed", current_value->data);

      // Capture image
      dt_camctl_camera_capture(darktable.camctl, NULL);

      fraction += 1.0 / total;
      dt_control_job_set_progress(job, fraction);
    }

    // lets reset to original value before continue
    if(params->brackets)
    {
      if(params->delay) // delay after final bracket
        g_usleep(params->delay * G_USEC_PER_SEC);

      current_value = g_list_find(values, original_value);
      dt_camctl_camera_set_property_string(darktable.camctl, NULL, "shutterspeed", current_value->data);
    }
  }

  // free values
  if(values)
  {
    g_list_free_full(values, g_free);
  }
  return 0;
}

static dt_camera_capture_t *dt_camera_capture_alloc()
{
  dt_camera_capture_t *params = calloc(1, sizeof(dt_camera_capture_t));
  if(!params) return NULL;

  // FIXME: unused
  params->shared.session = dt_import_session_new();

  return params;
}

static void dt_camera_capture_cleanup(void *p)
{
  dt_camera_capture_t *params = p;

  dt_import_session_destroy(params->shared.session);

  free(params);
}

dt_job_t *dt_camera_capture_job_create(const char *jobcode, uint32_t delay, uint32_t count, uint32_t brackets,
                                       uint32_t steps)
{
  dt_job_t *job = dt_control_job_create(&dt_camera_capture_job_run, "remote capture of image(s)");
  if(!job) return NULL;
  dt_camera_capture_t *params = dt_camera_capture_alloc();
  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  dt_control_job_add_progress(job, _("Capture images"), FALSE);
  dt_control_job_set_params(job, params, dt_camera_capture_cleanup);

  dt_import_session_set_name(params->shared.session, jobcode);

  params->delay = delay;
  params->count = count;
  params->brackets = brackets;
  params->steps = steps;
  return job;
}

/** Listener interface for import job */
void _camera_import_image_downloaded(const dt_camera_t *camera, const char *in_path,
                                     const char *in_filename, const char *filename, void *data)
{
  // Import downloaded image to import filmroll
  dt_camera_import_t *t = (dt_camera_import_t *)data;
  const int32_t imgid = dt_image_import(dt_import_session_film_id(t->shared.session), filename, FALSE, TRUE);

  const time_t timestamp = (!in_path || !in_filename) ? 0 :
               dt_camctl_get_image_file_timestamp(darktable.camctl, in_path, in_filename);
  if(timestamp && imgid >= 0)
  {
    char dt_txt[DT_DATETIME_EXIF_LENGTH];
    dt_datetime_unix_to_exif(dt_txt, sizeof(dt_txt), &timestamp);
    gchar *id = g_strconcat(in_filename, "-", dt_txt, NULL);
    dt_metadata_set(imgid, "Xmp.darktable.image_id", id, FALSE);
    g_free(id);
  }

  dt_control_queue_redraw_center();
  gchar *basename = g_path_get_basename(filename);
  const int num_images = g_list_length(t->images);
  dt_control_log(ngettext("%d/%d imported to %s", "%d/%d imported to %s", t->import_count + 1),
                 t->import_count + 1, num_images, basename);
  g_free(basename);

  t->fraction += 1.0 / num_images;

  dt_control_job_set_progress(t->job, t->fraction);

  if((imgid & 3) == 3)
  {
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
  }

  if(t->import_count + 1 == num_images)
  {
    // only redraw at the end, to not spam the cpu with exposure events
    dt_control_queue_redraw_center();
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_FILMROLLS_IMPORTED,
                            dt_import_session_film_id(t->shared.session));
  }
  t->import_count++;
}

static const char *_camera_request_image_filename(const dt_camera_t *camera, const char *filename,
                                                  const char *exif_time, void *data)
{
  const gchar *file;
  struct dt_camera_shared_t *shared;
  shared = (dt_camera_shared_t *)data;
  const gboolean use_filename = dt_conf_get_bool("session/use_filename");

  dt_import_session_set_filename(shared->session, filename);
  if(exif_time && exif_time[0])
    dt_import_session_set_exif_time(shared->session, exif_time);
  file = dt_import_session_filename(shared->session, use_filename);

  if(file == NULL) return NULL;

  return g_strdup(file);
}

static const char *_camera_request_image_path(const dt_camera_t *camera, char *exif_time, void *data)
{
  struct dt_camera_shared_t *shared;
  shared = (struct dt_camera_shared_t *)data;
  if(exif_time && exif_time[0])
    dt_import_session_set_exif_time(shared->session, exif_time);
  return dt_import_session_path(shared->session, FALSE);
}

static int32_t dt_camera_import_job_run(dt_job_t *job)
{
  dt_camera_import_t *params = dt_control_job_get_params(job);
  dt_control_log(_("Starting to import images from camera"));

  if(!dt_import_session_ready(params->shared.session))
  {
    dt_control_log("Failed to import images from camera.");
    return 1;
  }

  guint total = g_list_length(params->images);
  char message[512] = { 0 };
  snprintf(message, sizeof(message),
           ngettext("Importing %d image from camera", "Importing %d images from camera", total), total);
  dt_control_job_set_progress_message(job, message);

  // Switch to new filmroll
  dt_film_open(dt_import_session_film_id(params->shared.session));
  dt_ctl_switch_mode_to("lighttable");

  // register listener
  dt_camctl_listener_t listener = { 0 };
  listener.data = params;
  listener.image_downloaded = _camera_import_image_downloaded;
  listener.request_image_path = _camera_request_image_path;
  listener.request_image_filename = _camera_request_image_filename;

  // start download of images
  dt_camctl_register_listener(darktable.camctl, &listener);
  dt_camctl_import(darktable.camctl, params->camera, params->images);
  dt_camctl_unregister_listener(darktable.camctl, &listener);

  // notify the user via the window manager
  dt_ui_notify_user();

  return 0;
}

static dt_camera_import_t *dt_camera_import_alloc()
{
  dt_camera_import_t *params = calloc(1, sizeof(dt_camera_import_t));
  if(!params) return NULL;

  params->shared.session = dt_import_session_new();

  return params;
}

static void dt_camera_import_cleanup(void *p)
{
  dt_camera_import_t *params = p;

  g_list_free(params->images);

  dt_import_session_destroy(params->shared.session);

  params->camera->is_importing = FALSE;
  free(params);
}

dt_job_t *dt_camera_import_job_create(GList *images, struct dt_camera_t *camera,
                                      const char *time_override)
{
  dt_job_t *job = dt_control_job_create(&dt_camera_import_job_run, "import selected images from camera");
  if(!job)
    return NULL;
  dt_camera_import_t *params = dt_camera_import_alloc();
  if(!params)
  {
    dt_control_job_dispose(job);
    return NULL;
  }
  camera->is_importing = TRUE;
  dt_control_job_add_progress(job, _("Import images from camera"), FALSE);
  dt_control_job_set_params(job, params, dt_camera_import_cleanup);

  /* initialize import session for camera import job */
  if(time_override && time_override[0]) dt_import_session_set_time(params->shared.session, time_override);
  const char *jobcode = dt_conf_get_string_const("ui_last/import_jobcode");
  dt_import_session_set_name(params->shared.session, jobcode);

  params->fraction = 0;
  params->images = images;
  params->camera = camera;
  params->import_count = 0;
  params->job = job;
  return job;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

