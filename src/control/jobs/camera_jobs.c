/*
    This file is part of darktable,
    copyright (c) 2010 - 2012 Henrik Andersson.

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
#include "common/darktable.h"
#include "common/camera_control.h"
#include "common/utility.h"
#include "common/import_session.h"
#include "views/view.h"
#include "control/conf.h"
#include "control/jobs/camera_jobs.h"
#include "gui/gtk.h"

#include <stdio.h>
#include <glib.h>


int32_t dt_captured_image_import_job_run(dt_job_t *job)
{
  dt_captured_image_import_t *t = (dt_captured_image_import_t *)job->param;

  char message[512]= {0};
  snprintf(message, 512, _("importing image %s"), t->filename);
  const guint *jid = dt_control_backgroundjobs_create(darktable.control, 0, message );

  int id = dt_image_import(t->film_id, t->filename, TRUE);
  if(id)
  {
    //dt_film_open(1);
    dt_view_filmstrip_set_active_image(darktable.view_manager,id);
    dt_control_queue_redraw();
    //dt_ctl_switch_mode_to(DT_DEVELOP);
  }

  dt_control_backgroundjobs_progress(darktable.control, jid, 1.0);
  dt_control_backgroundjobs_destroy(darktable.control, jid);
  return 0;
}

void dt_captured_image_import_job_init(dt_job_t *job,uint32_t filmid, const char *filename)
{
  dt_control_job_init(job, "import tethered image");
  job->execute = &dt_captured_image_import_job_run;
  dt_captured_image_import_t *t = (dt_captured_image_import_t *)job->param;
  t->filename = g_strdup(filename);
  t->film_id = filmid;
}

int32_t dt_camera_capture_job_run(dt_job_t *job)
{
  dt_camera_capture_t *t=(dt_camera_capture_t*)job->param;
  int total = t->brackets ? t->count * t->brackets : t->count;
  char message[512]= {0};
  double fraction=0;
  snprintf(message, 512, ngettext ("capturing %d image", "capturing %d images", total), total );

  /* try to get exp program mode for nikon */
  char *expprogram = (char *)dt_camctl_camera_get_property(darktable.camctl, NULL, "expprogram");

  /* if fail, lets try fetching mode for cannon */
  if(!expprogram)
    expprogram = (char *)dt_camctl_camera_get_property(darktable.camctl, NULL, "autoexposuremode");

  /* Fetch all values for shutterspeed and initialize current value */
  GList *values=NULL;
  gconstpointer original_value=NULL;
  const char *cvalue = dt_camctl_camera_get_property(darktable.camctl, NULL, "shutterspeed");
  const char *value = dt_camctl_camera_property_get_first_choice(darktable.camctl, NULL, "shutterspeed");

  /* get values for bracketing */
  if (t->brackets && expprogram && expprogram[0]=='M' && value && cvalue)
  {
    do
    {
      // Add value to list
      values = g_list_append(values, g_strdup(value));
      // Check if current values is the same as original value, then lets store item ptr
      if (strcmp(value,cvalue) == 0)
        original_value = g_list_last(values)->data;
    }
    while ((value = dt_camctl_camera_property_get_next_choice(darktable.camctl, NULL, "shutterspeed")) != NULL);
  }
  else
  {
    /* if this was an intended bracket capture bail out */
    if(t->brackets)
    {
      dt_control_log(_("please set your camera to manual mode first!"));
      return 1;
    }
  }

  /* create the bgjob plate */
  const guint *jid  = dt_control_backgroundjobs_create(darktable.control, 0, message);

  GList *current_value = g_list_find(values,original_value);
  for(uint32_t i=0; i<t->count; i++)
  {
    // Delay if active
    if(t->delay)
      g_usleep(t->delay*G_USEC_PER_SEC);

    for(uint32_t b=0; b<(t->brackets*2)+1; b++)
    {
      // If bracket capture, lets set change shutterspeed
      if (t->brackets)
      {
        if (b == 0)
        {
          // First bracket, step down time with (steps*brackets), also check so we never set the longest shuttertime which would be bulb mode
          for(uint32_t s=0; s<(t->steps*t->brackets); s++)
            if (g_list_next(current_value) && g_list_next(g_list_next(current_value)))
              current_value = g_list_next(current_value);
        }
        else
        {
          // Step up with (steps)
          for(uint32_t s=0; s<t->steps; s++)
            if(g_list_previous(current_value))
              current_value = g_list_previous(current_value);
        }
      }

      // set the time property for bracket capture
      if (t->brackets && current_value)
        dt_camctl_camera_set_property_string(darktable.camctl, NULL, "shutterspeed", current_value->data);

      // Capture image
      dt_camctl_camera_capture(darktable.camctl,NULL);

      fraction += 1.0/total;
      dt_control_backgroundjobs_progress(darktable.control, jid, fraction);
    }

    // lets reset to original value before continue
    if (t->brackets)
    {
      current_value = g_list_find(values,original_value);
      dt_camctl_camera_set_property_string(darktable.camctl, NULL, "shutterspeed", current_value->data);
    }
  }

  dt_control_backgroundjobs_destroy(darktable.control, jid);


  // free values
  if(values)
  {
    g_list_free_full(values, g_free);
  }

  return 0;
}

void dt_camera_capture_job_init(dt_job_t *job,uint32_t filmid, uint32_t delay, uint32_t count, uint32_t brackets, uint32_t steps)
{
  dt_control_job_init(job, "remote capture of image(s)");
  job->execute = &dt_camera_capture_job_run;
  dt_camera_capture_t *t = (dt_camera_capture_t *)job->param;
  t->film_id=filmid;
  t->delay=delay;
  t->count=count;
  t->brackets=brackets;
  t->steps=steps;
}

void dt_camera_get_previews_job_init(dt_job_t *job,dt_camera_t *camera,dt_camctl_listener_t *listener,uint32_t flags)
{
  dt_control_job_init(job, "get camera previews job");
  job->execute = &dt_camera_get_previews_job_run;
  dt_camera_get_previews_t *t = (dt_camera_get_previews_t *)job->param;

  t->listener=g_malloc(sizeof(dt_camctl_listener_t));
  memcpy(t->listener,listener,sizeof(dt_camctl_listener_t));

  t->camera=camera;
  t->flags=flags;
}

int32_t dt_camera_get_previews_job_run(dt_job_t *job)
{
  dt_camera_get_previews_t *t=(dt_camera_get_previews_t*)job->param;

  dt_camctl_register_listener(darktable.camctl,t->listener);
  dt_camctl_get_previews(darktable.camctl,t->flags,t->camera);
  dt_camctl_unregister_listener(darktable.camctl,t->listener);
  g_free(t->listener);
  return 0;
}

void dt_camera_import_job_init(dt_job_t *job, const char *jobcode, GList *images, struct dt_camera_t *camera, time_t time_override)
{
  dt_control_job_init(job, "import selected images from camera");
  job->execute = &dt_camera_import_job_run;
  dt_camera_import_t *t = (dt_camera_import_t *)job->param;

  /* intitialize import session for camera import job */
  t->session = dt_import_session_new();
  dt_import_session_set_name(t->session, jobcode);
  if(time_override != 0)
    dt_import_session_set_time(t->session, time_override);

  t->fraction=0;
  t->images=g_list_copy(images);
  t->camera=camera;
  t->import_count=0;
}

/** Listener interface for import job */
void _camera_image_downloaded(const dt_camera_t *camera,const char *filename,void *data)
{
  // Import downloaded image to import filmroll
  dt_camera_import_t *t = (dt_camera_import_t *)data;
  dt_image_import(dt_import_session_film_id(t->session), filename, FALSE);
  dt_control_queue_redraw_center();
  dt_control_log(_("%d/%d imported to %s"), t->import_count+1,g_list_length(t->images), g_path_get_basename(filename));

  t->fraction+=1.0/g_list_length(t->images);

  dt_control_backgroundjobs_progress(darktable.control, t->bgj, t->fraction );

  t->import_count++;
}

const char *_camera_import_request_image_filename(const dt_camera_t *camera,const char *filename,void *data)
{
  const gchar *file;
  dt_camera_import_t *t = (dt_camera_import_t *)data;

  /* update import session with orginal filename so that $(FILE_EXTENSION)
     and alikes can be expanded. */
  dt_import_session_set_filename(t->session, filename);
  dt_import_session_path(t->session, FALSE);
  file = dt_import_session_filename(t->session, FALSE);

  if (file == NULL)
    return NULL;

  return g_strdup(file);
}

const char *_camera_import_request_image_path(const dt_camera_t *camera, void *data)
{
  dt_camera_import_t *t = (dt_camera_import_t *)data;
  return dt_import_session_path(t->session, FALSE);
}

int32_t dt_camera_import_job_run(dt_job_t *job)
{
  dt_camera_import_t *t = (dt_camera_import_t *)job->param;
  dt_control_log(_("starting to import images from camera"));

  if (!dt_import_session_ready(t->session))
  {
    dt_control_log("Failed to import images from camera.");
    return 1;
  }

  int total = g_list_length( t->images );
  char message[512]= {0};
  sprintf(message, ngettext ("importing %d image from camera", "importing %d images from camera", total), total );
  t->bgj = dt_control_backgroundjobs_create(darktable.control, 0, message);

  // Switch to new filmroll
  dt_film_open(dt_import_session_film_id(t->session));
  dt_ctl_switch_mode_to(DT_LIBRARY);

  // register listener
  dt_camctl_listener_t listener= {0};
  listener.data=t;
  listener.image_downloaded=_camera_image_downloaded;
  listener.request_image_path=_camera_import_request_image_path;
  listener.request_image_filename=_camera_import_request_image_filename;

  // start download of images
  dt_camctl_register_listener(darktable.camctl,&listener);
  dt_camctl_import(darktable.camctl,t->camera,t->images);
  dt_camctl_unregister_listener(darktable.camctl,&listener);
  dt_control_backgroundjobs_destroy(darktable.control, t->bgj);

  dt_import_session_destroy(t->session);

  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
