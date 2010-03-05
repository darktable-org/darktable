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

#include <stdio.h>
#include <unistd.h>
#include <glib.h>
#ifdef  HAVE_INOTIFY
#include <sys/inotify.h>
#endif

#include "common/image.h"
#include "common/fswatch.h"

typedef struct _watch_t {
  uint32_t descriptor;		// Handle
  dt_fswatch_type_t type;		// DT_FSWATCH_* type
  void *data;				// Assigned data
} _watch_t;

typedef struct inotify_event_t {
  long int wd;       /* Watch descriptor */
  uint32_t mask;     /* Mask of events */
  uint32_t cookie;   /* Unique cookie associating related events (for rename(2)) */
  uint32_t len;      /* Size of 'name' field */
  char     name[];   /* Optional null-terminated name */
} inotify_event_t;

// Compare func for GList
static gint _fswatch_items_by_data(const void* a,const void *b) {
  return (((_watch_t*)a)->data<b)?-1:((((_watch_t*)a)->data==b)?0:1);
}

// Compare func for GList
static gint _fswatch_items_by_descriptor(const void* a,const void *b) {
  gint result=(((_watch_t*)a)->descriptor < (long int)b)?-1:((((_watch_t*)a)->descriptor==(long int)b)?0:1);
  return result;
}


static void *_fswatch_thread(void *data) {
  dt_fswatch_t *fswatch=(dt_fswatch_t *)data;
  uint32_t res=0;
  uint32_t event_hdr_size=sizeof(inotify_event_t);
  inotify_event_t *event_hdr=g_malloc(sizeof(inotify_event_t));
  char *name=g_malloc(2048);
  dt_print(DT_DEBUG_FSWATCH,"[fswatch_thread] Starting thread of context %lx\n",(unsigned long int)data);
  while(1) {
    // Blocking read loop of event fd into event
    if((res=read(fswatch->inotify_fd,event_hdr,event_hdr_size))!=event_hdr_size) {
      break;
    }

    // Read name into buffer if any...
    if( event_hdr->len > 0 ) {
      res=read(fswatch->inotify_fd,name,event_hdr_size);
      name[res]='\0';
    }

    dt_print(DT_DEBUG_FSWATCH,"[fswatch_thread] Got event for %ld mask %x with name: %s\n", event_hdr->wd, event_hdr->mask, ((event_hdr->len>0)?name:""));

    // when event is read pass it to an handler..
    // _fswatch_handler(fswatch,event);
    pthread_mutex_lock(&fswatch->mutex);
    GList *gitem=g_list_find_custom(fswatch->items,(void *)event_hdr->wd,&_fswatch_items_by_descriptor);
    if( gitem ) {
      _watch_t *item = gitem->data;
      switch( item->type ) {
        case DT_FSWATCH_IMAGE:
          {
            if((event_hdr->mask&(IN_CLOSE_WRITE|IN_CREATE|IN_MOVE_SELF|IN_MOVED_TO)))
            {	//  Something wrote on image externally, lets reimport...
              dt_image_t *img=(dt_image_t *)item->data;
              dt_image_reimport(img,img->filename);
            }
          } break;

        default:
          dt_print(DT_DEBUG_FSWATCH,"[fswatch_thread] Unhandled object type %d for event descriptor %ld\n", item->type, event_hdr->wd );
          break;
      }
    } else
      dt_print(DT_DEBUG_FSWATCH,"[fswatch_thread] Failed to found watch item for descriptor %ld\n", event_hdr->wd );

    pthread_mutex_unlock(&fswatch->mutex);
  }
  return NULL;
}





const dt_fswatch_t* dt_fswatch_new()
{
  dt_fswatch_t *fswatch=g_malloc(sizeof(dt_fswatch_t));
  if((fswatch->inotify_fd=inotify_init())==-1)
    return NULL;
  fswatch->items=NULL;
  pthread_mutex_init(&fswatch->mutex, NULL);
  pthread_create(&fswatch->thread, NULL, &_fswatch_thread, fswatch);
  dt_print(DT_DEBUG_FSWATCH,"[fswatch_new] Creating new context %lx\n",(unsigned long int)fswatch);

  return fswatch;
}

void dt_fswatch_destroy(const dt_fswatch_t *fswatch)
{
  dt_print(DT_DEBUG_FSWATCH,"[fswatch_destroy] Destroying context %lx\n",(unsigned long int)fswatch);
  dt_fswatch_t *ctx=(dt_fswatch_t *)fswatch;
  pthread_mutex_destroy(&ctx->mutex);
  g_list_free(fswatch->items);
  g_free(ctx);
}

void dt_fswatch_add(const dt_fswatch_t * fswatch,dt_fswatch_type_t type, void *data)
{
  char *filename=NULL;
  uint32_t mask=0;
  dt_fswatch_t *ctx=(dt_fswatch_t *)fswatch;

  switch(type) 
  {
    case DT_FSWATCH_IMAGE:
      mask=IN_CLOSE_WRITE|IN_CREATE|IN_MOVE_SELF|IN_MOVED_TO;
      filename=((dt_image_t*)data)->filename;
      break;
    case DT_FSWATCH_CURVE_DIRECTORY:
      break;
    default:
      dt_print(DT_DEBUG_FSWATCH,"[fswatch_add] Unhandled object type %d\n",type);
      break;
  }

  if(filename)
  {
    pthread_mutex_lock(&ctx->mutex);
    _watch_t *item = g_malloc(sizeof(_watch_t));
    item->type=type;
    item->data=data;
    ctx->items=g_list_append(fswatch->items, item);
    item->descriptor=inotify_add_watch(fswatch->inotify_fd,filename,mask);
    pthread_mutex_unlock(&ctx->mutex);
    dt_print(DT_DEBUG_FSWATCH,"[fswatch_add] Watch on object %lx added on file %s\n",(unsigned long int)data,filename);
  } else 
    dt_print(DT_DEBUG_FSWATCH,"[fswatch_add] No watch added, failed to get related filename of object type %d\n",type);

}

void dt_fswatch_remove(const dt_fswatch_t * fswatch,dt_fswatch_type_t type, void *data)
{
  dt_fswatch_t *ctx=(dt_fswatch_t *)fswatch;
  pthread_mutex_lock(&ctx->mutex);
  dt_print(DT_DEBUG_FSWATCH,"[fswatch_remove] removing watch on object %lx\n",(unsigned long int)data);
  GList *gitem=g_list_find_custom(fswatch->items,data,&_fswatch_items_by_data);
  if( gitem ) {
    _watch_t *item=gitem->data;
    ctx->items=g_list_remove(ctx->items,item);
    inotify_rm_watch(fswatch->inotify_fd,item->descriptor); 
    g_free(item);
  } else
    dt_print(DT_DEBUG_FSWATCH,"[fswatch_remove] Didn't find watch on object %lx type %d\n",(unsigned long int)data,type);

  pthread_mutex_unlock(&ctx->mutex);
}

