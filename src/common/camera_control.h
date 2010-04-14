/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson.

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

#ifndef DT_CAMERA_CONTROL_H
#define DT_CAMERA_CONTROL_H

#include <glib.h>
#include <gtk/gtk.h>

#include <gphoto2/gphoto2.h>
#include "common/darktable.h"

/** A camera object used for camera actions and callbacks */
typedef struct dt_camera_t {
  const char *model;
  const char *port;
  gboolean can_import;
  gboolean can_tether;
  
  Camera *gpcam;
} 
dt_camera_t;

/** Context of camera control */
typedef struct dt_camctl_t 
{
  pthread_mutex_t mutex;
  pthread_t thread;
  GList *listeners;
  GList *cameras;
  
  GPContext *gpcontext;
  GPPortInfoList *gpports;
  CameraAbilitiesList *gpcams;
  
  dt_camera_t *active_camera;
} 
dt_camctl_t;


typedef struct dt_camctl_listener_t
{
  void *data;
  void (*captured_image)(const dt_camera_t *camera,const char *filename,void *data);
  void (*camera_connected)(const dt_camera_t *camera,void *data);
  void (*camera_disconnected)(const dt_camera_t *camera,void *data);
} dt_camctl_listener_t;

/** Initializes the gphoto and cam control, returns NULL if failed */
dt_camctl_t *dt_camctl_new();
/** Destroys the came control */
void dt_camctl_destroy(const dt_camctl_t *c);
/** Registers a listener of camera control */
void dt_camctl_register_listener(const dt_camctl_t *c, dt_camctl_listener_t *listener);
/** Unregisters a listener of camera control */
void dt_camctl_unregister_listener(const dt_camctl_t *c, dt_camctl_listener_t *listener);
/** Detect cameras and update list of available cameras */
void dt_camctl_detect_cameras(const dt_camctl_t *c);
/** Activates an camera to be used by cam control, this deactivates previous activated camera.. */
void dt_camctl_set_active_camera(const dt_camctl_t *c, dt_camera_t *cam);

/** poll for event on camera */
void dt_camera_poll_events(const dt_camctl_t *c,const dt_camera_t *cam);

#endif