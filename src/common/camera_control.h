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
  /** Locks the camera for an operation */
  pthread_mutex_t lock;
  const char *model;
  const char *port;
  CameraText summary;
 
  gboolean can_import;
  gboolean can_tether;
  gboolean can_config;
  
  /** Flag camera in tethering mode. \see dt_camera_tether_mode(gboolean *enable) */
  gboolean is_tethering;  
  
  Camera *gpcam;
} 
dt_camera_t;

typedef enum dt_camctl_status_t
{
	/** Camera control is busy, operations will block */
	CAMERA_CONTROL_BUSY,
	/** Camera control is available */
	CAMERA_CONTROL_AVAILABLE
}
dt_camctl_status_t;

typedef enum dt_camera_error_t 
{
  /** Locking camera failed */
  CAMERA_LOCK_FAILED,     
  /**  Camera conenction is broken and unuseable */
  CAMERA_CONNECTION_BROKEN        
} 
dt_camera_error_t;

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
  
  const dt_camera_t *active_camera;
} 
dt_camctl_t;


typedef struct dt_camctl_listener_t
{
  void *data;
 /** Invoked when a image is downloaded while in tethered mode or  by import */
  void (*control_status)(dt_camctl_status_t status,void *data);

  /** Invoked before images are fetched from camera and when tethered capture fetching an image. \note That only one listener should implement this... */
  const char * (*request_image_path)(const dt_camera_t *camera,void *data);

  /** Invoked before images are fetched from camera and when tethered capture fetching an image. \note That only one listener should implement this... */
  const char * (*request_image_filename)(const dt_camera_t *camera,const char *filename,void *data);
	
  /** Invoked when a image is downloaded while in tethered mode or  by import */
  void (*image_downloaded)(const dt_camera_t *camera,const char *filename,void *data);
  
  /** Invoked when a image is found on storage.. such as from dt_camctl_get_previews() */
  void (*camera_storage_image_filename)(const dt_camera_t *camera,const char *filename,CameraFile *preview,void *data);
  
  /** Invoked from dt_camctl_detect_cameras() when a new camera is connected */
  void (*camera_connected)(const dt_camera_t *camera,void *data);
  /** Invoked from dt_camctl_detect_cameras() when a new camera is disconnected, or when connection is broken and camera is unuseable */
  void (*camera_disconnected)(const dt_camera_t *camera,void *data);
  /** Invoked when a error occured \see dt_camera_error_t */
  void (*camera_error)(const dt_camera_t *camera,dt_camera_error_t error,void *data);
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
//void dt_camctl_set_active_camera(const dt_camctl_t *c, dt_camera_t *cam);
/** Enables/Disables the tether mode on camera. */
void dt_camctl_tether_mode(const dt_camctl_t *c,const dt_camera_t *cam,gboolean enable);

/** travers filesystem on camera an retreives previews of images */
void dt_camctl_get_previews(const dt_camctl_t *c,const dt_camera_t *cam);

/** Imports the images in list from specified camera */
void dt_camctl_import(const dt_camctl_t *c,const dt_camera_t *cam,GList *images);

#endif