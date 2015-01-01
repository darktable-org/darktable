/*
   This file is part of darktable,
   copyright (c) 2010-2012 Henrik Andersson.
   copyright (c) 2012 Tobias Ellinghaus.

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
#include "common/camera_control.h"
#include "common/exif.h"
#include "control/control.h"
#include <gphoto2/gphoto2-file.h>

#include <unistd.h>
#include <stdlib.h>
#if defined(__SUNOS__)
#include <fcntl.h>
#endif
#include <sys/fcntl.h>

/***/
typedef enum _camctl_camera_job_type_t
{
  /** Start a scan of devices and announce new and removed. */
  _JOB_TYPE_DETECT_DEVICES,
  /** Remotly executes a capture. */
  _JOB_TYPE_EXECUTE_CAPTURE,
  /** Fetch a preview for live view. */
  _JOB_TYPE_EXECUTE_LIVE_VIEW,
  /** Read a copy of remote camera config into cache. */
  _JOB_TYPE_READ_CONFIG,
  /** Writes changed properties in cache to camera */
  _JOB_TYPE_WRITE_CONFIG,
  /** Set's a property in config cache. \todo This shouldn't be a job in jobqueue !? */
  _JOB_TYPE_SET_PROPERTY_STRING,
  /** For some reason stopping live view needs to pass an int, not a string. */
  _JOB_TYPE_SET_PROPERTY_INT,
  /** get's a property from config cache. \todo This shouldn't be a job in jobqueue !?  */
  _JOB_TYPE_GET_PROPERTY
} _camctl_camera_job_type_t;

typedef struct _camctl_camera_job_t
{
  _camctl_camera_job_type_t type;
} _camctl_camera_job_t;

typedef struct _camctl_camera_set_property_string_job_t
{
  _camctl_camera_job_type_t type;
  char *name;
  char *value;
} _camctl_camera_set_property_string_job_t;

typedef struct _camctl_camera_set_property_int_job_t
{
  _camctl_camera_job_type_t type;
  char *name;
  int value;
} _camctl_camera_set_property_int_job_t;

/** Initializes camera */
gboolean _camera_initialize(const dt_camctl_t *c, dt_camera_t *cam);

/** Poll camera events, this one is called from the thread handling the camera. */
void _camera_poll_events(const dt_camctl_t *c, const dt_camera_t *cam);

/** Lock camera control and notify listener. \note Locks mutex and signals CAMERA_CONTROL_BUSY. \remarks all
 * interface functions available to host application should lock/unlock its operation. */
void _camctl_lock(const dt_camctl_t *c, const dt_camera_t *cam);
/** Lock camera control and notify listener. \note Locks mutex and signals CAMERA_CONTROL_AVAILABLE. \see
 * _camctl_lock() */
void _camctl_unlock(const dt_camctl_t *c);

/** Updates the cached configuration with a copy of camera configuration */
void _camera_configuration_update(const dt_camctl_t *c, const dt_camera_t *camera);
/** Commit the changes in cached configuration to the camera configuration */
void _camera_configuration_commit(const dt_camctl_t *c, const dt_camera_t *camera);
/** Merges source with destination and notifies listeners of the changes. \param notify_all If true every
 * widget is notified as change */
void _camera_configuration_merge(const dt_camctl_t *c, const dt_camera_t *camera, CameraWidget *source,
                                 CameraWidget *destination, gboolean notify_all);
/** Put a job on the queue */
void _camera_add_job(const dt_camctl_t *c, const dt_camera_t *camera, gpointer job);
/** Get a job from the queue */
gpointer _camera_get_job(const dt_camctl_t *c, const dt_camera_t *camera);
static void _camera_process_job(const dt_camctl_t *c, const dt_camera_t *camera, gpointer job);

/** Dispatch functions for listener interfaces */
const char *_dispatch_request_image_path(const dt_camctl_t *c, const dt_camera_t *camera);
const char *_dispatch_request_image_filename(const dt_camctl_t *c, const char *filename,
                                             const dt_camera_t *camera);
void _dispatch_camera_image_downloaded(const dt_camctl_t *c, const dt_camera_t *camera, const char *filename);
void _dispatch_camera_connected(const dt_camctl_t *c, const dt_camera_t *camera);
void _dispatch_camera_disconnected(const dt_camctl_t *c, const dt_camera_t *camera);
void _dispatch_control_status(const dt_camctl_t *c, dt_camctl_status_t status);
void _dispatch_camera_error(const dt_camctl_t *c, const dt_camera_t *camera, dt_camera_error_t error);
int _dispatch_camera_storage_image_filename(const dt_camctl_t *c, const dt_camera_t *camera,
                                            const char *filename, CameraFile *preview, CameraFile *exif);
void _dispatch_camera_property_value_changed(const dt_camctl_t *c, const dt_camera_t *camera,
                                             const char *name, const char *value);
void _dispatch_camera_property_accessibility_changed(const dt_camctl_t *c, const dt_camera_t *camera,
                                                     const char *name, gboolean read_only);

/** Helper function to destroy a dt_camera_t object */
static void dt_camctl_camera_destroy(dt_camera_t *cam);

/** Wrapper to asynchronously look for cameras */
static int _detect_cameras_callback(dt_job_t *job);


static int logid = 0;

void _gphoto_log25(GPLogLevel level, const char *domain, const char *log, void *data)
{
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] %s %s\n", domain, log);
}

#ifndef HAVE_GPHOTO_25_OR_NEWER
void _gphoto_log(GPLogLevel level, const char *domain, const char *format, va_list args, void *data)
{
  char log[4096] = { 0 };
  vsnprintf(log, sizeof(log), format, args);
  _gphoto_log25(level, domain, log, data);
}
#endif

void _enable_debug()
{
#ifdef HAVE_GPHOTO_25_OR_NEWER
  logid = gp_log_add_func(GP_LOG_DATA, (GPLogFunc)_gphoto_log25, NULL);
#else
  logid = gp_log_add_func(GP_LOG_DATA, (GPLogFunc)_gphoto_log, NULL);
#endif
}
void _disable_debug()
{
  gp_log_remove_func(logid);
}


static void _idle_func_dispatch(GPContext *context, void *data)
{
  // Let's do gtk main event iteration for not locking the ui
  gdk_threads_enter();
  if(gtk_events_pending()) gtk_main_iteration();
  gdk_threads_leave();
}

static void _error_func_dispatch25(GPContext *context, const char *text, void *data)
{
  dt_camctl_t *camctl = (dt_camctl_t *)data;

  dt_print(DT_DEBUG_CAMCTL, "[camera_control] gphoto2 error: %s\n", text);

  if(strstr(text, "PTP"))
  {

    /* remove camera for camctl camera list */
    GList *ci = g_list_find(camctl->cameras, camctl->active_camera);
    if(ci) camctl->cameras = g_list_remove(camctl->cameras, ci);

    /* notify client of camera connection broken */
    _dispatch_camera_error(camctl, camctl->active_camera, CAMERA_CONNECTION_BROKEN);

    /* notify client of camera disconnection */
    _dispatch_camera_disconnected(camctl, camctl->active_camera);
  }
}

static void _status_func_dispatch25(GPContext *context, const char *text, void *data)
{
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] gphoto2 status: %s\n", text);
}

static void _message_func_dispatch25(GPContext *context, const char *text, void *data)
{
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] gphoto2 message: %s\n", text);
}

#ifndef HAVE_GPHOTO_25_OR_NEWER
static void _status_func_dispatch(GPContext *context, const char *format, va_list args, void *data)
{
  char buffer[4096];
  vsnprintf(buffer, sizeof(buffer), format, args);

  _status_func_dispatch25(context, buffer, data);
}

static void _error_func_dispatch(GPContext *context, const char *format, va_list args, void *data)
{
  char buffer[4096];
  vsnprintf(buffer, sizeof(buffer), format, args);

  _error_func_dispatch25(context, buffer, data);
}

static void _message_func_dispatch(GPContext *context, const char *format, va_list args, void *data)
{
  char buffer[4096];
  vsnprintf(buffer, sizeof(buffer), format, args);
  _message_func_dispatch25(context, buffer, data);
}
#endif

static gboolean _camera_timeout_job(gpointer data)
{
  dt_camera_t *cam = (dt_camera_t *)data;
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] Calling timeout func for camera 0x%x.\n", cam);
  cam->timeout(cam->gpcam, cam->gpcontext);
  return TRUE;
}

static int _camera_start_timeout_func(Camera *c, unsigned int timeout, CameraTimeoutFunc func, void *data)
{
  dt_print(DT_DEBUG_CAMCTL,
           "[camera_control] start timeout %d seconds for camera 0x%x requested by driver.\n", timeout, data);
  dt_camera_t *cam = (dt_camera_t *)data;
  cam->timeout = func;
  return g_timeout_add_seconds(timeout, _camera_timeout_job, cam);
}

static void _camera_stop_timeout_func(Camera *c, int id, void *data)
{
  dt_camera_t *cam = (dt_camera_t *)data;
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] Removing timeout %d for camera 0x%x.\n", id, cam);
  g_source_remove(id);
  cam->timeout = NULL;
}


void _camera_add_job(const dt_camctl_t *c, const dt_camera_t *camera, gpointer job)
{
  dt_camera_t *cam = (dt_camera_t *)camera;
  dt_pthread_mutex_lock(&cam->jobqueue_lock);
  cam->jobqueue = g_list_append(cam->jobqueue, job);
  dt_pthread_mutex_unlock(&cam->jobqueue_lock);
}

gpointer _camera_get_job(const dt_camctl_t *c, const dt_camera_t *camera)
{
  dt_camera_t *cam = (dt_camera_t *)camera;
  dt_pthread_mutex_lock(&cam->jobqueue_lock);
  gpointer job = NULL;
  if(g_list_length(cam->jobqueue) > 0)
  {
    job = g_list_nth_data(cam->jobqueue, 0);
    cam->jobqueue = g_list_remove(cam->jobqueue, job);
  }
  dt_pthread_mutex_unlock(&cam->jobqueue_lock);
  return job;
}


static void _camera_process_job(const dt_camctl_t *c, const dt_camera_t *camera, gpointer job)
{
  dt_camera_t *cam = (dt_camera_t *)camera;
  _camctl_camera_job_t *j = (_camctl_camera_job_t *)job;
  switch(j->type)
  {

    case _JOB_TYPE_EXECUTE_CAPTURE:
    {
      dt_print(DT_DEBUG_CAMCTL, "[camera_control] executing remote camera capture job\n");
      CameraFilePath fp;
      int res = GP_OK;
      if((res = gp_camera_capture(camera->gpcam, GP_CAPTURE_IMAGE, &fp, c->gpcontext)) == GP_OK)
      {
        CameraFile *destination;
        const char *output_path = _dispatch_request_image_path(c, camera);
        if(!output_path) output_path = "/tmp";

        const char *fname = _dispatch_request_image_filename(c, fp.name, cam);
        if(!fname) break;

        char *output = g_build_filename(output_path, fname, (char *)NULL);

        int handle = open(output, O_CREAT | O_WRONLY, 0666);
        if(handle != -1)
        {
          gp_file_new_from_fd(&destination, handle);
          if(gp_camera_file_get(camera->gpcam, fp.folder, fp.name, GP_FILE_TYPE_NORMAL, destination,
                                c->gpcontext) == GP_OK)
          {
            // Notify listeners of captured image
            _dispatch_camera_image_downloaded(c, camera, output);
          }
          else
            dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to download file %s\n", output);
          close(handle);
        }
        else
          dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to download file %s\n", output);
        g_free(output);
      }
      else
        dt_print(DT_DEBUG_CAMCTL, "[camera_control] capture job failed to capture image: %s\n",
                 gp_result_as_string(res));
    }
    break;

    case _JOB_TYPE_EXECUTE_LIVE_VIEW:
    {
      CameraFile *fp = NULL;
      int res = GP_OK;
      const gchar *data = NULL;
      unsigned long int data_size = 0;

      gp_file_new(&fp);

      if((res = gp_camera_capture_preview(cam->gpcam, fp, c->gpcontext)) != GP_OK)
      {
        dt_print(DT_DEBUG_CAMCTL, "[camera_control] live view failed to capture preview: %s\n",
                 gp_result_as_string(res));
      }
      else if((res = gp_file_get_data_and_size(fp, &data, &data_size)) != GP_OK)
      {
        dt_print(DT_DEBUG_CAMCTL, "[camera_control] live view failed to get preview data: %s\n",
                 gp_result_as_string(res));
      }
      else
      {
        // everything worked
        GError *error = NULL;
        GdkPixbufLoader *loader = gdk_pixbuf_loader_new_with_mime_type(
            "image/jpeg", &error); // there were cases where GDKPixbufLoader failed to recognize the JPEG
        if(error)
        {
          dt_print(DT_DEBUG_CAMCTL, "[camera_control] live view failed to create jpeg image loader: %s\n",
                   error->message);
          g_error_free(error);
        }
        else
        {
          if(gdk_pixbuf_loader_write(loader, (guchar *)data, data_size, NULL) == TRUE)
          {
            dt_pthread_mutex_lock(&cam->live_view_pixbuf_mutex);
            if(cam->live_view_pixbuf != NULL) g_object_unref(cam->live_view_pixbuf);
            cam->live_view_pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
            dt_pthread_mutex_unlock(&cam->live_view_pixbuf_mutex);
          }
        }
        gdk_pixbuf_loader_close(loader, NULL);
      }
      if(fp) gp_file_free(fp);
      dt_pthread_mutex_unlock(&cam->live_view_synch);
      dt_control_queue_redraw_center();
    }
    break;

    case _JOB_TYPE_SET_PROPERTY_STRING:
    {
      _camctl_camera_set_property_string_job_t *spj = (_camctl_camera_set_property_string_job_t *)job;
      dt_print(DT_DEBUG_CAMCTL, "[camera_control] executing set camera config job %s=%s\n", spj->name,
               spj->value);

      CameraWidget *config; // Copy of camera configuration
      CameraWidget *widget;
      gp_camera_get_config(cam->gpcam, &config, c->gpcontext);
      if(gp_widget_get_child_by_name(config, spj->name, &widget) == GP_OK)
      {
        gp_widget_set_value(widget, spj->value);
        gp_camera_set_config(cam->gpcam, config, c->gpcontext);
      }
      /* dt_pthread_mutex_lock( &cam->config_lock );
       CameraWidget *widget;
       if(  gp_widget_get_child_by_name ( camera->configuration, spj->name, &widget) == GP_OK) {
         gp_widget_set_value ( widget , spj->value);
         //gp_widget_set_changed( widget, 1 );
         cam->config_changed=TRUE;
       }

       dt_pthread_mutex_unlock( &cam->config_lock);*/
      g_free(spj->name);
      g_free(spj->value);
    }
    break;

    case _JOB_TYPE_SET_PROPERTY_INT:
    {
      _camctl_camera_set_property_int_job_t *spj = (_camctl_camera_set_property_int_job_t *)job;
      dt_print(DT_DEBUG_CAMCTL, "[camera_control] executing set camera config job %s=%d\n", spj->name,
               spj->value);

      CameraWidget *config; // Copy of camera configuration
      CameraWidget *widget;
      gp_camera_get_config(cam->gpcam, &config, c->gpcontext);
      if(gp_widget_get_child_by_name(config, spj->name, &widget) == GP_OK)
      {
        int value = spj->value;
        gp_widget_set_value(widget, &value);
        gp_camera_set_config(cam->gpcam, config, c->gpcontext);
      }
      /* dt_pthread_mutex_lock( &cam->config_lock );
       CameraWidget *widget;
       if(  gp_widget_get_child_by_name ( camera->configuration, spj->name, &widget) == GP_OK) {
         gp_widget_set_value ( widget , spj->value);
         //gp_widget_set_changed( widget, 1 );
         cam->config_changed=TRUE;
       }

       dt_pthread_mutex_unlock( &cam->config_lock);*/
      g_free(spj->name);
    }
    break;

    default:
      dt_print(DT_DEBUG_CAMCTL, "[camera_control] process of unknown job type %p\n", j->type);
      break;
  }

  g_free(j);
}

/*************/
/* LIVE VIEW */
/*************/
static void *dt_camctl_camera_get_live_view(void *data)
{
  dt_camctl_t *camctl = (dt_camctl_t *)data;
  dt_camera_t *cam = (dt_camera_t *)camctl->active_camera;
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] live view thread started\n");

  int frames = 0;
  double capture_time = dt_get_wtime();

  while(cam->is_live_viewing == TRUE)
  {
    dt_pthread_mutex_lock(&cam->live_view_synch);

    // calculate FPS
    double current_time = dt_get_wtime();
    if(current_time - capture_time >= 1.0)
    {
      // a second has passed
      dt_print(DT_DEBUG_CAMCTL, "%d fps\n", frames + 1);
      frames = 0;
      capture_time = current_time;
    }
    else
    {
      // just increase the frame counter
      frames++;
    }

    _camctl_camera_job_t *job = g_malloc(sizeof(_camctl_camera_job_t));
    job->type = _JOB_TYPE_EXECUTE_LIVE_VIEW;
    _camera_add_job(camctl, cam, job);

    g_usleep((1.0 / 15) * G_USEC_PER_SEC); // never update faster than 15 FPS. going too fast will result in
                                           // too many redraws without a real benefit
  }
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] live view thread stopped\n");
  return NULL;
}

gboolean dt_camctl_camera_start_live_view(const dt_camctl_t *c)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  dt_camera_t *cam = (dt_camera_t *)camctl->active_camera;
  if(cam == NULL)
  {
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] Failed to start live view, camera==NULL\n");
    return FALSE;
  }
  else
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] Starting live view\n");

  if(cam->can_live_view == FALSE)
  {
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] Camera does not support live view\n");
    return FALSE;
  }
  cam->is_live_viewing = TRUE;
  dt_camctl_camera_set_property_int(camctl, NULL, "eosviewfinder", 1);
  pthread_create(&cam->live_view_thread, NULL, &dt_camctl_camera_get_live_view, (void *)camctl);
  return TRUE;
}

void dt_camctl_camera_stop_live_view(const dt_camctl_t *c)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  dt_camera_t *cam = (dt_camera_t *)camctl->active_camera;
  if(cam->is_live_viewing == FALSE)
  {
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] Not in live view mode, nothing to stop\n");
    return;
  }
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] Stopping live view\n");
  cam->is_live_viewing = FALSE;
  pthread_join(cam->live_view_thread, NULL);
  // tell camera to get back to normal state (close mirror)
  dt_camctl_camera_set_property_int(camctl, NULL, "eosviewfinder", 0);
}

void _camctl_lock(const dt_camctl_t *c, const dt_camera_t *cam)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  dt_pthread_mutex_lock(&camctl->lock);
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] camera control locked for camera %p\n", cam);
  camctl->active_camera = cam;
  _dispatch_control_status(c, CAMERA_CONTROL_BUSY);
}

void _camctl_unlock(const dt_camctl_t *c)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  const dt_camera_t *cam = camctl->active_camera;
  camctl->active_camera = NULL;
  dt_pthread_mutex_unlock(&camctl->lock);
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] camera control un-locked for camera %p\n", cam);
  _dispatch_control_status(c, CAMERA_CONTROL_AVAILABLE);
}


dt_camctl_t *dt_camctl_new()
{
  dt_camctl_t *camctl = g_malloc0(sizeof(dt_camctl_t));
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] creating new context %p\n", camctl);

  // Initialize gphoto2 context and setup dispatch callbacks
  camctl->gpcontext = gp_context_new();
  gp_context_set_idle_func(camctl->gpcontext, (GPContextIdleFunc)_idle_func_dispatch, camctl);

#ifdef HAVE_GPHOTO_25_OR_NEWER
  gp_context_set_status_func(camctl->gpcontext, (GPContextStatusFunc)_status_func_dispatch25, camctl);
  gp_context_set_error_func(camctl->gpcontext, (GPContextErrorFunc)_error_func_dispatch25, camctl);
  gp_context_set_message_func(camctl->gpcontext, (GPContextMessageFunc)_message_func_dispatch25, camctl);
#else
  gp_context_set_status_func(camctl->gpcontext, (GPContextStatusFunc)_status_func_dispatch, camctl);
  gp_context_set_error_func(camctl->gpcontext, (GPContextErrorFunc)_error_func_dispatch, camctl);
  gp_context_set_message_func(camctl->gpcontext, (GPContextMessageFunc)_message_func_dispatch, camctl);
#endif

  // Load all camera drivers we know...
  gp_abilities_list_new(&camctl->gpcams);
  gp_abilities_list_load(camctl->gpcams, camctl->gpcontext);
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] loaded %d camera drivers.\n",
           gp_abilities_list_count(camctl->gpcams));

  dt_pthread_mutex_init(&camctl->lock, NULL);
  dt_pthread_mutex_init(&camctl->listeners_lock, NULL);

  // asynchronously call dt_camctl_detect_cameras(camctl); to fill in camctl
  dt_job_t *job = dt_control_job_create(&_detect_cameras_callback, "detect connected cameras");
  if(job)
  {
    dt_control_job_set_params(job, camctl);
    dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_BG, job);
  }

  return camctl;
}

static void dt_camctl_camera_destroy(dt_camera_t *cam)
{
  gp_camera_exit(cam->gpcam, cam->gpcontext);
  gp_camera_unref(cam->gpcam);
  gp_widget_unref(cam->configuration);
  if(cam->live_view_pixbuf != NULL)
  {
    g_object_unref(cam->live_view_pixbuf);
    cam->live_view_pixbuf = NULL; // just in case someone else is using this
  }
  g_free(cam->model);
  g_free(cam->port);
  // TODO: cam->jobqueue
  g_free(cam);
}

void dt_camctl_destroy(const dt_camctl_t *camctl)
{
  // Go thru all c->cameras and release them..
  for(GList *it = g_list_first(camctl->cameras); it != NULL; it = g_list_delete_link(it, it))
  {
    dt_camctl_camera_destroy((dt_camera_t *)it->data);
  }
  gp_context_unref(camctl->gpcontext);
  gp_abilities_list_free(camctl->gpcams);
  gp_port_info_list_free(camctl->gpports);
}


int dt_camctl_have_cameras(const dt_camctl_t *c)
{
  return (g_list_length(c->cameras) > 0) ? 1 : 0;
}

void dt_camctl_register_listener(const dt_camctl_t *c, dt_camctl_listener_t *listener)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  // Just locking mutex and prevent signalling CAMERA_CONTROL_BUSY
  dt_pthread_mutex_lock(&camctl->listeners_lock);
  if(g_list_find(camctl->listeners, listener) == NULL)
  {
    camctl->listeners = g_list_append(camctl->listeners, listener);
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] registering listener %p\n", listener);
  }
  else
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] registering already registered listener %p\n", listener);
  dt_pthread_mutex_unlock(&camctl->listeners_lock);
}

void dt_camctl_unregister_listener(const dt_camctl_t *c, dt_camctl_listener_t *listener)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  // Just locking mutex and prevent signalling CAMERA_CONTROL_BUSY
  dt_pthread_mutex_lock(&camctl->listeners_lock);
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] unregistering listener %p\n", listener);
  camctl->listeners = g_list_remove(camctl->listeners, listener);
  dt_pthread_mutex_unlock(&camctl->listeners_lock);
}

gint _compare_camera_by_port(gconstpointer a, gconstpointer b)
{
  dt_camera_t *ca = (dt_camera_t *)a;
  dt_camera_t *cb = (dt_camera_t *)b;
  return strcmp(ca->port, cb->port);
}

void dt_camctl_detect_cameras(const dt_camctl_t *c)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  dt_pthread_mutex_lock(&camctl->lock);

  /* reload portdrivers */
  if(camctl->gpports) gp_port_info_list_free(camctl->gpports);

  gp_port_info_list_new(&camctl->gpports);
  gp_port_info_list_load(camctl->gpports);
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] loaded %d port drivers.\n",
           gp_port_info_list_count(camctl->gpports));



  CameraList *available_cameras = NULL;
  gp_list_new(&available_cameras);
  gp_abilities_list_detect(c->gpcams, c->gpports, available_cameras, c->gpcontext);
  dt_print(DT_DEBUG_CAMCTL, "[camera_control] %d cameras connected\n",
           gp_list_count(available_cameras) > 0 ? gp_list_count(available_cameras) : 0);


  for(int i = 0; i < gp_list_count(available_cameras); i++)
  {
    dt_camera_t *camera = g_malloc0(sizeof(dt_camera_t));
    const gchar *s;
    gp_list_get_name(available_cameras, i, &s);
    camera->model = g_strdup(s);
    gp_list_get_value(available_cameras, i, &s);
    camera->port = g_strdup(s);
    dt_pthread_mutex_init(&camera->config_lock, NULL);
    dt_pthread_mutex_init(&camera->live_view_pixbuf_mutex, NULL);
    dt_pthread_mutex_init(&camera->live_view_synch, NULL);

    // if(strcmp(camera->port,"usb:")==0) { g_free(camera); continue; }
    GList *citem;
    if((citem = g_list_find_custom(c->cameras, camera, _compare_camera_by_port)) == NULL
       || strcmp(((dt_camera_t *)citem->data)->model, camera->model) != 0)
    {
      if(citem == NULL)
      {
        // Newly connected camera
        if(_camera_initialize(c, camera) == FALSE)
        {
          dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to initialize device %s on port %s, probably "
                                    "causes are: locked by another application, no access to udev etc.\n",
                   camera->model, camera->port);
          dt_camctl_camera_destroy(camera);
          continue;
        }

        // Check if camera has capabilities for being presented to darktable
        if(camera->can_import == FALSE && camera->can_tether == FALSE)
        {
          dt_print(
              DT_DEBUG_CAMCTL,
              "[camera_control] device %s on port %s doesn't support import or tether, skipping device.\n",
              camera->model, camera->port);
          dt_camctl_camera_destroy(camera);
          continue;
        }

        // Fetch some summary of camera
        if(gp_camera_get_summary(camera->gpcam, &camera->summary, c->gpcontext) == GP_OK)
        {
          // Remove device property summary:
          char *eos = strstr(camera->summary.text, "Device Property Summary:\n");
          if(eos) eos[0] = '\0';
        }

        // Add to camera list
        camctl->cameras = g_list_append(camctl->cameras, camera);

        // Notify listeners of connected camera
        _dispatch_camera_connected(camctl, camera);
      }
    }
    else
      dt_camctl_camera_destroy(camera);
  }

  /* check c->cameras in available_cameras */
  if(c->cameras && g_list_length(c->cameras) > 0)
  {
    GList *citem = c->cameras;
    do
    {
      int index = 0;
      dt_camera_t *cam = (dt_camera_t *)citem->data;
      if(gp_list_find_by_name(available_cameras, &index, cam->model) != GP_OK)
      {
        /* remove camera from cached list.. */
        dt_camctl_t *camctl = (dt_camctl_t *)c;
        dt_camera_t *oldcam = (dt_camera_t *)citem->data;
        camctl->cameras = citem = g_list_delete_link(c->cameras, citem);
        dt_camctl_camera_destroy(oldcam);
      }
    } while(citem && (citem = g_list_next(citem)) != NULL);
  }

  gp_list_unref(available_cameras);

  dt_pthread_mutex_unlock(&camctl->lock);

  // tell the world that we are done. this assumes that there is just one global camctl.
  // if there would ever be more it would be easy to pass c with the signal.
  // TODO: only raise it when the connected cameras changed?
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_CAMERA_DETECTED);
}

static int _detect_cameras_callback(dt_job_t *job)
{
  dt_camctl_t *c = dt_control_job_get_params(job);
  dt_camctl_detect_cameras(c);
  return 0;
}

static void *_camera_event_thread(void *data)
{
  dt_camctl_t *camctl = (dt_camctl_t *)data;

  const dt_camera_t *camera = camctl->active_camera;

  dt_print(DT_DEBUG_CAMCTL, "[camera_control] starting camera event thread %p of context %p\n",
           camctl->camera_event_thread, data);

  while(camera->is_tethering == TRUE)
  {
    // Poll event from camera
    _camera_poll_events(camctl, camera);

    // Let's check if there are jobs in queue to process
    gpointer job;
    while((job = _camera_get_job(camctl, camera)) != NULL) _camera_process_job(camctl, camera, job);

    // Check it jobs did change the configuration
    if(camera->config_changed == TRUE) _camera_configuration_commit(camctl, camera);
  }

  dt_print(DT_DEBUG_CAMCTL, "[camera_control] exiting camera thread %p.\n", camctl->camera_event_thread);

  return NULL;
}

gboolean _camera_initialize(const dt_camctl_t *c, dt_camera_t *cam)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  CameraAbilities a;
  GPPortInfo pi;
  if(cam->gpcam == NULL)
  {
    gp_camera_new(&cam->gpcam);
    int m = gp_abilities_list_lookup_model(c->gpcams, cam->model);
    gp_abilities_list_get_abilities(c->gpcams, m, &a);
    gp_camera_set_abilities(cam->gpcam, a);

    int p = gp_port_info_list_lookup_path(c->gpports, cam->port);
    gp_port_info_list_get_info(c->gpports, p, &pi);
    gp_camera_set_port_info(cam->gpcam, pi);

    // Check for abilities
    if((a.operations & GP_OPERATION_CAPTURE_IMAGE)) cam->can_tether = TRUE;
    if((a.operations & GP_OPERATION_CAPTURE_PREVIEW)) cam->can_live_view = TRUE;
    if(cam->can_tether && (a.operations & GP_OPERATION_CONFIG)) cam->can_config = TRUE;
    if(!(a.file_operations & GP_FILE_OPERATION_NONE)) cam->can_import = TRUE;

    if(gp_camera_init(cam->gpcam, camctl->gpcontext) != GP_OK)
    {
      dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to initialize camera %s on port %s\n", cam->model,
               cam->port);
      return FALSE;
    }

    // read a full copy of config to configuration cache
    gp_camera_get_config(cam->gpcam, &cam->configuration, c->gpcontext);

    // TODO: find a more robust way for this, once we find out how to do it with non-EOS cameras
    if(cam->can_live_view && dt_camctl_camera_property_exists(camctl, cam, "eoszoomposition"))
      cam->can_live_view_advanced = TRUE;

    // initialize timeout callbacks eg. keep alive, some cameras needs it.
    cam->gpcontext = camctl->gpcontext;
    gp_camera_set_timeout_funcs(cam->gpcam, (CameraTimeoutStartFunc)_camera_start_timeout_func,
                                (CameraTimeoutStopFunc)_camera_stop_timeout_func, cam);


    dt_pthread_mutex_init(&cam->jobqueue_lock, NULL);

    dt_print(DT_DEBUG_CAMCTL, "[camera_control] device %s on port %s initialized\n", cam->model, cam->port);
  }
  else
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] device %s on port %s already initialized\n", cam->model,
             cam->port);

  return TRUE;
}



void dt_camctl_import(const dt_camctl_t *c, const dt_camera_t *cam, GList *images)
{
  // dt_camctl_t *camctl=(dt_camctl_t *)c;
  _camctl_lock(c, cam);

  GList *ifile = g_list_first(images);

  const char *output_path = _dispatch_request_image_path(c, cam);
  if(ifile) do
    {
      // Split file into folder and filename
      char *eos;
      char folder[PATH_MAX] = { 0 };
      char filename[PATH_MAX] = { 0 };
      char *file = (char *)ifile->data;
      eos = file + strlen(file);
      while(--eos > file && *eos != '/')
        ;
      char *_file = g_strndup(file, eos - file);
      g_strlcat(folder, _file, sizeof(folder));
      g_strlcat(filename, eos + 1, sizeof(filename));
      g_free(_file);

      const char *fname = _dispatch_request_image_filename(c, filename, cam);
      if(!fname) continue;

      char *output = g_build_filename(output_path, fname, (char *)NULL);

      // Now we have filenames lets download file and notify listener of image download
      CameraFile *destination;
      int handle = open(output, O_CREAT | O_WRONLY, 0666);
      if(handle != -1)
      {
        gp_file_new_from_fd(&destination, handle);
        if(gp_camera_file_get(cam->gpcam, folder, filename, GP_FILE_TYPE_NORMAL, destination, c->gpcontext)
           == GP_OK)
        {
          _dispatch_camera_image_downloaded(c, cam, output);
        }
        else
          dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to download file %s\n", output);
        close(handle);
      }
      else
        dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to download file %s\n", output);
      g_free(output);
    } while((ifile = g_list_next(ifile)));

  _dispatch_control_status(c, CAMERA_CONTROL_AVAILABLE);
  _camctl_unlock(c);
}


int _camctl_recursive_get_previews(const dt_camctl_t *c, dt_camera_preview_flags_t flags, char *path)
{
  CameraList *files;
  CameraList *folders;
  const char *filename;
  const char *foldername;

  gp_list_new(&files);
  gp_list_new(&folders);

  // Process files in current folder...
  if(gp_camera_folder_list_files(c->active_camera->gpcam, path, files, c->gpcontext) == GP_OK)
  {
    for(int i = 0; i < gp_list_count(files); i++)
    {
      gp_list_get_name(files, i, &filename);
      char *file = g_strconcat(path, "/", filename, NULL);

      // Lets check the type of file...
      CameraFileInfo cfi;
      if(gp_camera_file_get_info(c->active_camera->gpcam, path, filename, &cfi, c->gpcontext) == GP_OK)
      {
        CameraFile *preview = NULL;
        CameraFile *exif = NULL;

        /*
         * Fetch image preview if flagged...
         */
        if(flags & CAMCTL_IMAGE_PREVIEW_DATA)
        {
          gp_file_new(&preview);
          if(gp_camera_file_get(c->active_camera->gpcam, path, filename, GP_FILE_TYPE_PREVIEW, preview,
                                c->gpcontext) < GP_OK)
          {
            // No preview for file lets check image size to see if we should download full image for
            // preview...
            if(cfi.file.size > 0 && cfi.file.size < 512000)
            {
              if(gp_camera_file_get(c->active_camera->gpcam, path, filename, GP_FILE_TYPE_NORMAL, preview,
                                    c->gpcontext) < GP_OK)
              {
                preview = NULL;
                dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to retrieve preview of file %s\n",
                         filename);
              }
            }
            else if(!strncmp(c->active_camera->port, "disk:", 5))
            {
              char fullpath[PATH_MAX] = { 0 };
              snprintf(fullpath, sizeof(fullpath), "%s/%s/%s", c->active_camera->port + 5, path, filename);
              uint8_t *buf = NULL; // gphoto takes care of freeing it eventually
              size_t bufsize;
              char *mime_type = NULL;

              if(!dt_exif_get_thumbnail(fullpath, &buf, &bufsize, &mime_type))
                gp_file_set_data_and_size(preview, (char *)buf, bufsize);

              free(mime_type);
            }
          }
        }

        if(flags & CAMCTL_IMAGE_EXIF_DATA)
        {
          gp_file_new(&exif);
          if(gp_camera_file_get(c->active_camera->gpcam, path, filename, GP_FILE_TYPE_EXIF, exif,
                                c->gpcontext) < GP_OK)
          {
            exif = NULL;
            dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to retrieve exif of file %s\n", filename);
          }
        }

        // let's dispatch to host app.. return if we should stop...
        int res = _dispatch_camera_storage_image_filename(c, c->active_camera, file, preview, exif);
        gp_file_free(preview);
        if(!res)
        {
          g_free(file);
          return 0;
        }
      }
      else
        dt_print(DT_DEBUG_CAMCTL,
                 "[camera_control] failed to get file information of %s in folder %s on device\n", filename,
                 path);
      g_free(file);
    }
  }

  // Recurse into folders in current folder...
  if(gp_camera_folder_list_folders(c->active_camera->gpcam, path, folders, c->gpcontext) == GP_OK)
  {
    for(int i = 0; i < gp_list_count(folders); i++)
    {
      char buffer[PATH_MAX] = { 0 };
      g_strlcat(buffer, path, sizeof(buffer));
      if(path[1] != '\0') g_strlcat(buffer, "/", sizeof(buffer));
      gp_list_get_name(folders, i, &foldername);
      g_strlcat(buffer, foldername, sizeof(buffer));
      if(!_camctl_recursive_get_previews(c, flags, buffer)) return 0;
    }
  }
  gp_list_free(files);
  gp_list_free(folders);
  return 1;
}

void dt_camctl_select_camera(const dt_camctl_t *c, const dt_camera_t *cam)
{
  _camctl_lock(c, cam);
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  camctl->wanted_camera = cam;
  _camctl_unlock(c);
}


void dt_camctl_get_previews(const dt_camctl_t *c, dt_camera_preview_flags_t flags, const dt_camera_t *cam)
{
  _camctl_lock(c, cam);
  _camctl_recursive_get_previews(c, flags, "/");
  _camctl_unlock(c);
}

int dt_camctl_can_enter_tether_mode(const dt_camctl_t *c, const dt_camera_t *cam)
{
  /* first check if camera is provided else use wanted cam */
  if(cam == NULL) cam = c->wanted_camera;

  /* check if wanted cam is available else use active camera */
  if(cam == NULL) cam = c->active_camera;

  /* check if active cam is available else use first detected one */
  if(cam == NULL && c->cameras) cam = g_list_nth_data(c->cameras, 0);

  if(cam && cam->can_tether)
  {
    dt_camctl_t *camctl = (dt_camctl_t *)c;
    camctl->wanted_camera = cam;
    return 1;
  }

  return 0;
}

void dt_camctl_tether_mode(const dt_camctl_t *c, const dt_camera_t *cam, gboolean enable)
{
  /* first check if camera is provided else use wanted cam */
  if(cam == NULL) cam = c->wanted_camera;

  /* check if wanted cam is available else use active camera */
  if(cam == NULL) cam = c->active_camera;

  /* check if active cam is available else use first detected one */
  if(cam == NULL && c->cameras) cam = g_list_nth_data(c->cameras, 0);

  if(cam && cam->can_tether)
  {
    dt_camctl_t *camctl = (dt_camctl_t *)c;
    dt_camera_t *camera = (dt_camera_t *)cam;

    if(enable == TRUE && camera->is_tethering != TRUE)
    {
      _camctl_lock(c, cam);
      // Start up camera event polling thread
      dt_print(DT_DEBUG_CAMCTL, "[camera_control] enabling tether mode\n");
      camctl->active_camera = camera;
      camera->is_tethering = TRUE;
      pthread_create(&camctl->camera_event_thread, NULL, &_camera_event_thread, (void *)c);
    }
    else
    {
      camera->is_live_viewing = FALSE;
      camera->is_tethering = FALSE;
      dt_print(DT_DEBUG_CAMCTL, "[camera_control] disabling tether mode\n");
      _camctl_unlock(c);
      // Wait for tether thread with join??
    }
  }
  else
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to set tether mode with reason: %s\n",
             cam ? "device does not support tethered capture" : "no active camera");
}

const char *dt_camctl_camera_get_model(const dt_camctl_t *c, const dt_camera_t *cam)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  if(!cam && (cam = camctl->active_camera) == NULL && (cam = camctl->wanted_camera) == NULL)
  {
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to get model of camera, camera==NULL\n");
    return NULL;
  }
  return cam->model;
}


void _camera_build_property_menu(CameraWidget *widget, GtkMenu *menu, GCallback item_activate,
                                 gpointer user_data)
{
  int childs = 0;
  const char *sk;
  CameraWidgetType type;

  /* if widget has children lets add menutitem and recurse into container */
  if((childs = gp_widget_count_children(widget)) > 0)
  {
    CameraWidget *child = NULL;
    for(int i = 0; i < childs; i++)
    {
      gp_widget_get_child(widget, i, &child);
      gp_widget_get_name(child, &sk);

      /* Check if widget is submenu */
      if(gp_widget_count_children(child) > 0)
      {
        /* create submenu item */
        GtkMenuItem *item = GTK_MENU_ITEM(gtk_menu_item_new_with_label(sk));
        gtk_menu_item_set_submenu(item, gtk_menu_new());

        /* recurse into submenu */
        _camera_build_property_menu(child, GTK_MENU(gtk_menu_item_get_submenu(item)), item_activate,
                                    user_data);

        /* add submenu item to menu if not empty*/
        if(gtk_container_get_children(GTK_CONTAINER(gtk_menu_item_get_submenu(item))) != NULL)
          gtk_menu_shell_append(GTK_MENU_SHELL(menu), GTK_WIDGET(item));
      }
      else
      {
        /* check widget type */
        gp_widget_get_type(child, &type);
        if(type == GP_WIDGET_MENU || type == GP_WIDGET_TEXT || type == GP_WIDGET_RADIO)
        {
          /* construct menu item for property */
          gp_widget_get_name(child, &sk);
          GtkMenuItem *item = GTK_MENU_ITEM(gtk_menu_item_new_with_label(sk));
          g_signal_connect(G_OBJECT(item), "activate", item_activate, user_data);
          /* add submenu item to menu */
          gtk_menu_shell_append(GTK_MENU_SHELL(menu), GTK_WIDGET(item));
        }
      }
    }
  }
}

void dt_camctl_camera_build_property_menu(const dt_camctl_t *c, const dt_camera_t *cam, GtkMenu **menu,
                                          GCallback item_activate, gpointer user_data)
{
  /* get active camera */
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  if(!cam && (cam = camctl->active_camera) == NULL && (cam = camctl->wanted_camera) == NULL)
  {
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to build property menu from camera, camera==NULL\n");
    return;
  }

  dt_print(DT_DEBUG_CAMCTL, "[camera_control] building property menu from camera configuration\n");

  /* lock camera config mutex while recursive building property menu */
  dt_camera_t *camera = (dt_camera_t *)cam;
  dt_pthread_mutex_lock(&camera->config_lock);
  *menu = GTK_MENU(gtk_menu_new());
  _camera_build_property_menu(camera->configuration, *menu, item_activate, user_data);
  gtk_widget_show_all(GTK_WIDGET(*menu));
  dt_pthread_mutex_unlock(&camera->config_lock);
}



void dt_camctl_camera_set_property_string(const dt_camctl_t *c, const dt_camera_t *cam,
                                          const char *property_name, const char *value)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  if(!cam && (cam = camctl->active_camera) == NULL && (cam = camctl->wanted_camera) == NULL)
  {
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to set property from camera, camera==NULL\n");
    return;
  }
  dt_camera_t *camera = (dt_camera_t *)cam;

  _camctl_camera_set_property_string_job_t *job = g_malloc(sizeof(_camctl_camera_set_property_string_job_t));
  job->type = _JOB_TYPE_SET_PROPERTY_STRING;
  job->name = g_strdup(property_name);
  job->value = g_strdup(value);

  // Push the job on the jobqueue
  _camera_add_job(camctl, camera, job);
}

void dt_camctl_camera_set_property_int(const dt_camctl_t *c, const dt_camera_t *cam,
                                       const char *property_name, const int value)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  if(!cam && (cam = camctl->active_camera) == NULL && (cam = camctl->wanted_camera) == NULL)
  {
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to set property from camera, camera==NULL\n");
    return;
  }
  dt_camera_t *camera = (dt_camera_t *)cam;

  _camctl_camera_set_property_int_job_t *job = g_malloc(sizeof(_camctl_camera_set_property_int_job_t));
  job->type = _JOB_TYPE_SET_PROPERTY_INT;
  job->name = g_strdup(property_name);
  job->value = value;

  // Push the job on the jobqueue
  _camera_add_job(camctl, camera, job);
}

const char *dt_camctl_camera_get_property(const dt_camctl_t *c, const dt_camera_t *cam,
                                          const char *property_name)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  if(!cam && (cam = camctl->active_camera) == NULL && (cam = camctl->wanted_camera) == NULL)
  {
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to get property from camera, camera==NULL\n");
    return NULL;
  }
  dt_camera_t *camera = (dt_camera_t *)cam;
  dt_pthread_mutex_lock(&camera->config_lock);
  const char *value = NULL;
  CameraWidget *widget;
  if(gp_widget_get_child_by_name(camera->configuration, property_name, &widget) == GP_OK)
  {
    gp_widget_get_value(widget, &value);
  }
  dt_pthread_mutex_unlock(&camera->config_lock);
  return value;
}

int dt_camctl_camera_property_exists(const dt_camctl_t *c, const dt_camera_t *cam, const char *property_name)
{
  int exists = 0;
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  if(!cam && ((cam = camctl->active_camera) == NULL && (cam = camctl->wanted_camera) == NULL))
  {
    dt_print(DT_DEBUG_CAMCTL,
             "[camera_control] failed to check if property exists in camera configuration, camera==NULL\n");
    return 0;
  }

  dt_camera_t *camera = (dt_camera_t *)cam;
  dt_pthread_mutex_lock(&camera->config_lock);

  CameraWidget *widget;
  if(gp_widget_get_child_by_name(camera->configuration, property_name, &widget) == GP_OK) exists = 1;

  dt_pthread_mutex_unlock(&camera->config_lock);

  return exists;
}

const char *dt_camctl_camera_property_get_first_choice(const dt_camctl_t *c, const dt_camera_t *cam,
                                                       const char *property_name)
{
  const char *value = NULL;
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  if(!cam && ((cam = camctl->active_camera) == NULL && (cam = camctl->wanted_camera) == NULL))
  {
    dt_print(DT_DEBUG_CAMCTL,
             "[camera_control] failed to get first choice of property from camera, camera==NULL\n");
    return NULL;
  }
  dt_camera_t *camera = (dt_camera_t *)cam;
  dt_pthread_mutex_lock(&camera->config_lock);
  if(gp_widget_get_child_by_name(camera->configuration, property_name, &camera->current_choice.widget)
     == GP_OK)
  {
    camera->current_choice.index = 0;
    gp_widget_get_choice(camera->current_choice.widget, camera->current_choice.index, &value);
  }
  else
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] property name '%s' not found in camera configuration.\n",
             property_name);

  dt_pthread_mutex_unlock(&camera->config_lock);

  return value;
}

const char *dt_camctl_camera_property_get_next_choice(const dt_camctl_t *c, const dt_camera_t *cam,
                                                      const char *property_name)
{
  const char *value = NULL;
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  if(!cam && ((cam = camctl->active_camera) == NULL && (cam = camctl->wanted_camera) == NULL))
  {
    dt_print(DT_DEBUG_CAMCTL,
             "[camera_control] Failed to get next choice of property from camera, camera==NULL\n");
    return NULL;
  }
  dt_camera_t *camera = (dt_camera_t *)cam;
  dt_pthread_mutex_lock(&camera->config_lock);
  if(camera->current_choice.widget != NULL)
  {

    if(++camera->current_choice.index < gp_widget_count_choices(camera->current_choice.widget))
    {
      // get the choice value...
      gp_widget_get_choice(camera->current_choice.widget, camera->current_choice.index, &value);
    }
    else
    {
      // No more choices, reset current_choices for further use
      camera->current_choice.index = 0;
      camera->current_choice.widget = NULL;
    }
  }

  dt_pthread_mutex_unlock(&camera->config_lock);
  return value;
}

void dt_camctl_camera_capture(const dt_camctl_t *c, const dt_camera_t *cam)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  if(!cam && (cam = camctl->active_camera) == NULL)
  {
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] Failed to capture from camera, camera==NULL\n");
    return;
  }
  dt_camera_t *camera = (dt_camera_t *)cam;

  _camctl_camera_job_t *job = g_malloc(sizeof(_camctl_camera_job_t));
  job->type = _JOB_TYPE_EXECUTE_CAPTURE;
  _camera_add_job(camctl, camera, job);
}

void _camera_poll_events(const dt_camctl_t *c, const dt_camera_t *cam)
{
  CameraEventType event;
  gpointer data;
  if(gp_camera_wait_for_event(cam->gpcam, 30, &event, &data, c->gpcontext) == GP_OK)
  {
    if(event == GP_EVENT_UNKNOWN)
    {
      /* this is really some undefined behavior, seems like its
      camera driver dependent... very ugly! */
      if(strstr((char *)data, "4006") || // Nikon PTP driver
         (strstr((char *)data, "PTP Property")
          && strstr((char *)data, "changed")) // Some Canon driver maybe all ??
         )
      {
        // Property change event occurred on camera
        // let's update cache and signalling
        dt_print(DT_DEBUG_CAMCTL, "[camera_control] Camera configuration change event, lets update internal "
                                  "configuration cache.\n");
        _camera_configuration_update(c, cam);
      }
    }
    else if(event == GP_EVENT_FILE_ADDED)
    {
      if(cam->is_tethering)
      {
        dt_print(DT_DEBUG_CAMCTL, "[camera_control] Camera file added event\n");
        CameraFilePath *fp = (CameraFilePath *)data;
        CameraFile *destination;
        const char *output_path = _dispatch_request_image_path(c, cam);
        if(!output_path) output_path = "/tmp";
        const char *fname = _dispatch_request_image_filename(c, fp->name, cam);
        if(!fname) fname = fp->name;

        char *output = g_build_filename(output_path, fname, (char *)NULL);

        int handle = open(output, O_CREAT | O_WRONLY, 0666);
        if(handle != -1)
        {
          gp_file_new_from_fd(&destination, handle);
          if(gp_camera_file_get(cam->gpcam, fp->folder, fp->name, GP_FILE_TYPE_NORMAL, destination,
                                c->gpcontext) == GP_OK)
          {
            // Notify listeners of captured image
            _dispatch_camera_image_downloaded(c, cam, output);
          }
          else
            dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to download file %s\n", output);
          close(handle);
        }
        else
          dt_print(DT_DEBUG_CAMCTL, "[camera_control] failed to download file %s\n", output);
        g_free(output);
      }
    }
  }
}


void _camera_configuration_merge(const dt_camctl_t *c, const dt_camera_t *camera, CameraWidget *source,
                                 CameraWidget *destination, gboolean notify_all)
{
  int childs = 0;
  const char *sk;
  const char *stv;
  CameraWidget *dw;
  const char *dtv;
  CameraWidgetType type;
  // If source widget has childs let's recurse into each children
  if((childs = gp_widget_count_children(source)) > 0)
  {
    CameraWidget *child = NULL;
    for(int i = 0; i < childs; i++)
    {
      gp_widget_get_child(source, i, &child);
      // gp_widget_get_name( source, &sk );
      _camera_configuration_merge(c, camera, child, destination, notify_all);
    }
  }
  else
  {
    gboolean changed = TRUE;
    gp_widget_get_type(source, &type);

    // Get the two keys to compare
    gp_widget_get_name(source, &sk);
    gp_widget_get_child_by_name(destination, sk, &dw);

    //
    // First of all check if widget has change accessibility
    //
    /// TODO: Resolve this 2.4.8 libgphoto2 dependency
    /*
    int sa,da;
    gp_widget_get_readonly( source, &sa );
    gp_widget_get_readonly( dw, &da );

    if(  notify_all || ( sa != da ) ) {
      // update destination widget to new accessibility if differ then notify of the change
      if( ( sa != da )  )
        gp_widget_set_readonly( dw, sa );

      _dispatch_camera_property_accessibility_changed(c, camera,sk, ( sa == 1 ) ? TRUE: FALSE) ;
    }
    */

    //
    // Lets compare values and notify on change or by notifyAll flag
    //
    if(type == GP_WIDGET_MENU || type == GP_WIDGET_TEXT || type == GP_WIDGET_RADIO)
    {

      // Get source and destination value to be compared
      gp_widget_get_value(source, &stv);
      gp_widget_get_value(dw, &dtv);

      if(((stv && dtv) && strcmp(stv, dtv) != 0) && (changed = TRUE))
      {
        gp_widget_set_value(dw, stv);
        // Don't flag this change as changed, otherwise a read-only widget might get tried
        // to update the camera configuration...
        gp_widget_set_changed(dw, 0);
      }

      if((stv && dtv) && (notify_all || changed)) _dispatch_camera_property_value_changed(c, camera, sk, stv);
    }
  }
}

void _camera_configuration_commit(const dt_camctl_t *c, const dt_camera_t *camera)
{
  g_assert(camera != NULL);

  dt_camera_t *cam = (dt_camera_t *)camera;

  dt_pthread_mutex_lock(&cam->config_lock);
  if(gp_camera_set_config(camera->gpcam, camera->configuration, c->gpcontext) != GP_OK)
    dt_print(DT_DEBUG_CAMCTL, "[camera_control] Failed to commit configuration changes to camera\n");

  cam->config_changed = FALSE;
  dt_pthread_mutex_unlock(&cam->config_lock);
}

void _camera_configuration_update(const dt_camctl_t *c, const dt_camera_t *camera)
{
  // dt_camctl_t *camctl=(dt_camctl_t *)c;
  dt_camera_t *cam = (dt_camera_t *)camera;
  dt_pthread_mutex_lock(&cam->config_lock);
  CameraWidget *remote; // Copy of remote configuration
  gp_camera_get_config(camera->gpcam, &remote, c->gpcontext);
  // merge remote copy with cache and notify on changed properties to host application
  _camera_configuration_merge(c, camera, remote, camera->configuration, FALSE);
  dt_pthread_mutex_unlock(&cam->config_lock);
}

const char *_dispatch_request_image_filename(const dt_camctl_t *c, const char *filename,
                                             const dt_camera_t *camera)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  GList *listener;
  const char *path = NULL;
  dt_pthread_mutex_lock(&camctl->listeners_lock);
  if((listener = g_list_first(camctl->listeners)) != NULL) do
    {
      if(((dt_camctl_listener_t *)listener->data)->request_image_filename != NULL)
        path = ((dt_camctl_listener_t *)listener->data)
                   ->request_image_filename(camera, filename, ((dt_camctl_listener_t *)listener->data)->data);
    } while((listener = g_list_next(listener)) != NULL);
  dt_pthread_mutex_unlock(&camctl->listeners_lock);
  return path;
}


const char *_dispatch_request_image_path(const dt_camctl_t *c, const dt_camera_t *camera)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  GList *listener;
  const char *path = NULL;
  dt_pthread_mutex_lock(&camctl->listeners_lock);
  if((listener = g_list_first(camctl->listeners)) != NULL) do
    {
      if(((dt_camctl_listener_t *)listener->data)->request_image_path != NULL)
        path = ((dt_camctl_listener_t *)listener->data)
                   ->request_image_path(camera, ((dt_camctl_listener_t *)listener->data)->data);
    } while((listener = g_list_next(listener)) != NULL);
  dt_pthread_mutex_unlock(&camctl->listeners_lock);
  return path;
}

void _dispatch_camera_connected(const dt_camctl_t *c, const dt_camera_t *camera)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  GList *listener;
  dt_pthread_mutex_lock(&camctl->listeners_lock);
  if((listener = g_list_first(camctl->listeners)) != NULL) do
    {
      if(((dt_camctl_listener_t *)listener->data)->camera_connected != NULL)
        ((dt_camctl_listener_t *)listener->data)
            ->camera_connected(camera, ((dt_camctl_listener_t *)listener->data)->data);
    } while((listener = g_list_next(listener)) != NULL);
  dt_pthread_mutex_unlock(&camctl->listeners_lock);
}

void _dispatch_camera_disconnected(const dt_camctl_t *c, const dt_camera_t *camera)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  GList *listener;
  dt_pthread_mutex_lock(&camctl->listeners_lock);
  if((listener = g_list_first(camctl->listeners)) != NULL) do
    {
      if(((dt_camctl_listener_t *)listener->data)->camera_disconnected != NULL)
        ((dt_camctl_listener_t *)listener->data)
            ->camera_disconnected(camera, ((dt_camctl_listener_t *)listener->data)->data);
    } while((listener = g_list_next(listener)) != NULL);
  dt_pthread_mutex_unlock(&camctl->listeners_lock);
}


void _dispatch_camera_image_downloaded(const dt_camctl_t *c, const dt_camera_t *camera, const char *filename)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  GList *listener;
  dt_pthread_mutex_lock(&camctl->listeners_lock);
  if((listener = g_list_first(camctl->listeners)) != NULL) do
    {
      if(((dt_camctl_listener_t *)listener->data)->image_downloaded != NULL)
        ((dt_camctl_listener_t *)listener->data)
            ->image_downloaded(camera, filename, ((dt_camctl_listener_t *)listener->data)->data);
    } while((listener = g_list_next(listener)) != NULL);
  dt_pthread_mutex_unlock(&camctl->listeners_lock);
}

int _dispatch_camera_storage_image_filename(const dt_camctl_t *c, const dt_camera_t *camera,
                                            const char *filename, CameraFile *preview, CameraFile *exif)
{
  int res = 0;
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  GList *listener;
  dt_pthread_mutex_lock(&camctl->listeners_lock);
  if((listener = g_list_first(camctl->listeners)) != NULL) do
    {
      if(((dt_camctl_listener_t *)listener->data)->camera_storage_image_filename != NULL)
        res = ((dt_camctl_listener_t *)listener->data)
                  ->camera_storage_image_filename(camera, filename, preview, exif,
                                                  ((dt_camctl_listener_t *)listener->data)->data);
    } while((listener = g_list_next(listener)) != NULL);
  dt_pthread_mutex_unlock(&camctl->listeners_lock);
  return res;
}

void _dispatch_control_status(const dt_camctl_t *c, dt_camctl_status_t status)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  GList *listener;
  dt_pthread_mutex_lock(&camctl->listeners_lock);
  if((listener = g_list_first(camctl->listeners)) != NULL) do
    {
      if(((dt_camctl_listener_t *)listener->data)->control_status != NULL)
        ((dt_camctl_listener_t *)listener->data)
            ->control_status(status, ((dt_camctl_listener_t *)listener->data)->data);
    } while((listener = g_list_next(listener)) != NULL);
  dt_pthread_mutex_unlock(&camctl->listeners_lock);
}

void _dispatch_camera_property_value_changed(const dt_camctl_t *c, const dt_camera_t *camera,
                                             const char *name, const char *value)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  GList *listener;
  dt_pthread_mutex_lock(&camctl->listeners_lock);
  if((listener = g_list_first(camctl->listeners)) != NULL) do
    {
      if(((dt_camctl_listener_t *)listener->data)->camera_property_value_changed != NULL)
        ((dt_camctl_listener_t *)listener->data)
            ->camera_property_value_changed(camera, name, value,
                                            ((dt_camctl_listener_t *)listener->data)->data);
    } while((listener = g_list_next(listener)) != NULL);
  dt_pthread_mutex_unlock(&camctl->listeners_lock);
}

void _dispatch_camera_property_accessibility_changed(const dt_camctl_t *c, const dt_camera_t *camera,
                                                     const char *name, gboolean read_only)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  GList *listener;
  dt_pthread_mutex_lock(&camctl->listeners_lock);
  if((listener = g_list_first(camctl->listeners)) != NULL) do
    {
      if(((dt_camctl_listener_t *)listener->data)->camera_property_accessibility_changed != NULL)
        ((dt_camctl_listener_t *)listener->data)
            ->camera_property_accessibility_changed(camera, name, read_only,
                                                    ((dt_camctl_listener_t *)listener->data)->data);
    } while((listener = g_list_next(listener)) != NULL);
  dt_pthread_mutex_unlock(&camctl->listeners_lock);
}

void _dispatch_camera_error(const dt_camctl_t *c, const dt_camera_t *camera, dt_camera_error_t error)
{
  dt_camctl_t *camctl = (dt_camctl_t *)c;
  GList *listener;
  dt_pthread_mutex_lock(&camctl->listeners_lock);
  if((listener = g_list_first(camctl->listeners)) != NULL) do
    {
      if(((dt_camctl_listener_t *)listener->data)->camera_error != NULL)
        ((dt_camctl_listener_t *)listener->data)
            ->camera_error(camera, error, ((dt_camctl_listener_t *)listener->data)->data);
    } while((listener = g_list_next(listener)) != NULL);
  dt_pthread_mutex_unlock(&camctl->listeners_lock);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
