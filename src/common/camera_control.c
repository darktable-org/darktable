/*
   This file is part of darktable,
   copyright (c) 2010 Henrik Andersson.

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
#include "../config.h"
#endif
#include <unistd.h>
#include <stdlib.h>
#include <sys/fcntl.h>
#include "common/camera_control.h"


static void _idle_func_dispatch(GPContext *context, void *data) {
  /*dt_camctl_t *camctl=(dt_camctl_t *)data;
  GList *listener;
  if((listener=g_list_first(camctl->listeners))!=NULL)
    do
    {
      ((dt_listener_t*)listener->data)->idle(camctl)
    } while((listener=g_list_next(listener))!=NULL);*/
}

static void _error_func_dispatch(GPContext *context, const char *format, va_list args, void *data) {
//  dt_camctl_t *camctl=(dt_camctl_t *)data;
  char buffer[4096];
	vsprintf( buffer, format, args );
  dt_print(DT_DEBUG_CAMCTL,"[camera_control] gphoto2 error: %s .\n",buffer);
}

static void _status_func_dispatch(GPContext *context, const char *format, va_list args, void *data) {
  //dt_camctl_t *camctl=(dt_camctl_t *)data;
  char buffer[4096];
	vsprintf( buffer, format, args );
  dt_print(DT_DEBUG_CAMCTL,"[camera_control] gphoto2 status: %s .\n",buffer);
}

static void _message_func_dispatch(GPContext *context, const char *format, va_list args, void *data) {
  //dt_camctl_t *camctl=(dt_camctl_t *)data;
  char buffer[4096];
	vsprintf( buffer, format, args );
  dt_print(DT_DEBUG_CAMCTL,"[camera_control] gphoto2 message: %s .\n",buffer);
}

/*
static void *_camera_control_thread(void *data) {
  dt_camctl_t *camctl=(dt_camctl_t *)data;
  dt_print(DT_DEBUG_CAMCTL,"[camera_control] Starting thread %lx of context %lx\n",(unsigned long int)camctl->thread,(unsigned long int)data);
  while(1) {
    pthread_mutex_lock(&camctl->mutex);
    
    pthread_mutex_unlock(&camctl->mutex);
  //  usleep(100000);
  }
  dt_print(DT_DEBUG_CAMCTL,"[camera_control] terminating thread %lx.\n",(unsigned long int)camctl->thread);
  return NULL;
}*/

dt_camctl_t *dt_camctl_new()
{
  dt_camctl_t *camctl=g_malloc(sizeof(dt_camctl_t));
  dt_print(DT_DEBUG_CAMCTL,"[camera_control] Creating new context %lx\n",(unsigned long int)camctl);

  // Initialize gphoto2 context and setup dispatch callbacks
  camctl->gpcontext = gp_context_new();
	gp_context_set_idle_func( camctl->gpcontext , _idle_func_dispatch, camctl );
	gp_context_set_status_func( camctl->gpcontext , _status_func_dispatch, camctl );
	gp_context_set_error_func( camctl->gpcontext , _error_func_dispatch, camctl );
	gp_context_set_message_func( camctl->gpcontext , _message_func_dispatch, camctl );
  
  gp_port_info_list_new( &camctl->gpports );
	gp_abilities_list_new( &camctl->gpcams );

  // Load drivers
  gp_port_info_list_load( camctl->gpports );
  dt_print(DT_DEBUG_CAMCTL,"[camera_control] Loaded %d port drivers.\n", gp_port_info_list_count( camctl->gpports ) );	
	
	// Load all camera drivers we know...
	gp_abilities_list_load( camctl->gpcams, camctl->gpcontext );
	dt_print(DT_DEBUG_CAMCTL,"[camera_control] Loaded %d camera drivers.\n", gp_abilities_list_count( camctl->gpcams ) );	
	
  pthread_mutex_init(&camctl->mutex, NULL);
  //pthread_create(&camctl->thread, NULL, &_camera_control_thread, camctl);
  
  // Let's detect cameras connexted
  dt_camctl_detect_cameras(camctl);
  
  return camctl;
}

void dt_camctl_destroy(const dt_camctl_t *c)
{
}

void dt_camctl_register_listener( const dt_camctl_t *c, dt_camctl_listener_t *listener)
{
  dt_camctl_t *camctl=(dt_camctl_t *)c;
  pthread_mutex_lock(&camctl->mutex);
  if( g_list_find(camctl->listeners,listener) == NULL )
    camctl->listeners=g_list_append(camctl->listeners,listener);
  else
     dt_print(DT_DEBUG_CAMCTL,"[camera_control] Registering already registered listener %lx\n",(unsigned long int)listener);
  pthread_mutex_unlock(&camctl->mutex);
}

void dt_camctl_unregister_listener( const dt_camctl_t *c, dt_camctl_listener_t *listener)
{
  dt_camctl_t *camctl=(dt_camctl_t *)c;
  pthread_mutex_lock(&camctl->mutex);
  camctl->listeners = g_list_remove( camctl->listeners, listener );
  pthread_mutex_unlock(&camctl->mutex);  
}

gint _compare_camera_by_port(gconstpointer a,gconstpointer b)
{
  dt_camera_t *ca=(dt_camera_t *)a;
  dt_camera_t *cb=(dt_camera_t *)b;
  return strcmp(ca->port,cb->port);
}

void dt_camctl_detect_cameras(const dt_camctl_t *c)
{
  dt_camctl_t *camctl=(dt_camctl_t *)c;
  CameraList *temporary=NULL;
  gp_list_new( &temporary );
  gp_abilities_list_detect (c->gpcams,c->gpports, temporary, c->gpcontext );
  dt_print(DT_DEBUG_CAMCTL,"[camera_control] %d cameras connected\n",gp_list_count( temporary ));

  pthread_mutex_lock(&camctl->mutex);
    
  // Adding 2 dummy cameras if list = NULL
  if(camctl->cameras==NULL)
  { 
    dt_camera_t *dummy=g_malloc(sizeof(dt_camera_t));
    dummy->model="Nikon D90";
    dummy->port="usb:1.1";
    dummy->can_import=dummy->can_tether=TRUE;
    camctl->cameras=g_list_append(camctl->cameras,dummy);  
    
    dummy=g_malloc(sizeof(dt_camera_t));
    dummy->model="Canon IXSUS2";
    dummy->port="usb:2.3";
    dummy->can_import=TRUE;
    camctl->cameras=g_list_append(camctl->cameras,dummy);  
  }    
  
  for(int i=0;i<gp_list_count( temporary );i++)
  {
    dt_camera_t *camera=g_malloc(sizeof(dt_camera_t));
    gp_list_get_name (temporary, i, &camera->model);
		gp_list_get_value (temporary, i, &camera->port);
    GList *citem;
  
    if( (citem=g_list_find_custom(c->cameras,camera,_compare_camera_by_port)) == NULL || strcmp(((dt_camera_t *)citem->data)->model,camera->model)!=0 ) 
    {
      if(citem==NULL)
      { // New camera
        camctl->cameras = g_list_append(camctl->cameras,camera);
      } 
      else
      { // Update camera on port
        ((dt_camera_t*)citem->data)->port=camera->port;
      }
      
      // Notify listerners of conencted camera
      GList *listener;
      if((listener=g_list_first(camctl->listeners))!=NULL)
        do
        {
          if(((dt_camctl_listener_t*)listener->data)->camera_connected)
            ((dt_camctl_listener_t*)listener->data)->camera_connected(camera,((dt_camctl_listener_t*)listener->data)->data);
        } while((listener=g_list_next(listener))!=NULL);
        
    } else g_free(camera);
    
  }
  pthread_mutex_unlock(&camctl->mutex);
}

void dt_camctl_set_active_camera(const dt_camctl_t *c, dt_camera_t *cam)
{
  dt_camctl_t *camctl=(dt_camctl_t *)c;
  
  CameraAbilities a;
  GPPortInfo pi;
  if( cam->gpcam )
  {
    gp_camera_new(&cam->gpcam);
    int m = gp_abilities_list_lookup_model( c->gpcams, cam->model );
    gp_abilities_list_get_abilities (c->gpcams, m, &a);
    gp_camera_set_abilities (cam->gpcam, a);
  
    int p = gp_port_info_list_lookup_path (c->gpports, cam->port);
    gp_port_info_list_get_info (c->gpports, p, &pi);
    gp_camera_set_port_info (cam->gpcam , pi);
  }
  
  if( gp_camera_init( cam->gpcam ,  camctl->gpcontext) == GP_OK )
  {
    dt_print(DT_DEBUG_CAMCTL,"[camera_control] Camera %s on port %s initialized and activated\n", cam->model,cam->port);
    camctl->active_camera = cam;
    
    return;
  }    

  dt_print(DT_DEBUG_CAMCTL,"[camera_control] Failed to initialize camera %s on port %s\n", cam->model,cam->port);
}

void dt_camera_poll_events(const dt_camctl_t *c,const dt_camera_t *cam)
{
  dt_camctl_t *camctl=(dt_camctl_t *)c;
  CameraEventType event;
  gpointer data;
  pthread_mutex_lock(&camctl->mutex);
  if( gp_camera_wait_for_event( cam->gpcam, 1, &event, &data, c->gpcontext ) >= GP_OK ) {
		switch( event ) {
      case GP_EVENT_UNKNOWN: 
			{
				if( strstr( (char *)data, "4006" ) ); // Property change event occured on camera
				
			} break;

      case GP_EVENT_FILE_ADDED:
			{
				CameraFilePath *fp = (CameraFilePath *)data;
				CameraFile *destination;
        char *filename="/tmp/capture";
				int handle = open( filename, O_CREAT | O_WRONLY,0666);
				gp_file_new_from_fd( &destination , handle );
				gp_camera_file_get( cam->gpcam, fp->folder , fp->name, GP_FILE_TYPE_NORMAL, destination,  c->gpcontext);
				close( handle );
			
				 // Notify listerners of captured image
        GList *listener;
        if((listener=g_list_first(camctl->listeners))!=NULL)
          do
          {
            if(((dt_camctl_listener_t*)listener->data)->captured_image)
              ((dt_camctl_listener_t*)listener->data)->captured_image(cam,filename,((dt_camctl_listener_t*)listener->data)->data);
          } while((listener=g_list_next(listener))!=NULL);
        
			} break;

      case GP_EVENT_TIMEOUT:
      case GP_EVENT_FOLDER_ADDED:
      case GP_EVENT_CAPTURE_COMPLETE:
      break;
      
    }
  }
  pthread_mutex_unlock(&camctl->mutex);
}
