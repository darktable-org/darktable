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
#include "config.h"
#endif

#include "common/darktable.h"
#include "common/dtpthread.h"
#include "common/image.h"
#include "common/fswatch.h"
#include "develop/develop.h"

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <glib.h>
#include <strings.h>
#if 0 // def  HAVE_INOTIFY
#include <sys/inotify.h>
#endif


typedef struct _watch_t
{
  int descriptor;         // Handle
  dt_fswatch_type_t type; // DT_FSWATCH_* type
  void *data;             // Assigned data
  int events;             // events occurred..
} _watch_t;


#if 0 // def  HAVE_INOTIFY

typedef struct inotify_event_t
{
  uint32_t wd;       /* Watch descriptor */
  uint32_t mask;     /* Mask of events */
  uint32_t cookie;   /* Unique cookie associating related events (for rename(2)) */
  uint32_t len;      /* Size of 'name' field */
  char     name[];   /* Optional null-terminated name */
} inotify_event_t;

// Compare func for GList
static gint _fswatch_items_by_data(const void* a,const void *b)
{
  return (((_watch_t*)a)->data<b)?-1:((((_watch_t*)a)->data==b)?0:1);
}

// Compare func for GList
static gint _fswatch_items_by_descriptor(const void *a,const void *b)
{
  gint result=(((_watch_t*)a)->descriptor < *(const int *)b)?-1:((((_watch_t*)a)->descriptor==*(const int *)b)?0:1);
  return result;
}


static void *_fswatch_thread(void *data)
{
  dt_fswatch_t *fswatch=(dt_fswatch_t *)data;
  uint32_t res=0;
  uint32_t event_hdr_size=sizeof(inotify_event_t);
  inotify_event_t *event_hdr = g_malloc0(sizeof(inotify_event_t));
  char *name = g_malloc0(2048);
  dt_print(DT_DEBUG_FSWATCH,"[fswatch_thread] Starting thread of context %p\n", data);
  while(1)
  {
    // Blocking read loop of event fd into event
    if(read(fswatch->inotify_fd,event_hdr,event_hdr_size) != event_hdr_size)
    {
      if(errno == EINTR) continue;
      perror("[fswatch_thread] read inotify fd");
      break;
    }

    // Read name into buffer if any...
    if( event_hdr->len > 0 )
    {
      res=read(fswatch->inotify_fd,name,event_hdr_size);
      name[res]='\0';
    }

    dt_pthread_mutex_lock(&fswatch->mutex);
    GList *gitem=g_list_find_custom(fswatch->items,&event_hdr->wd,&_fswatch_items_by_descriptor);
    if( gitem )
    {
      _watch_t *item = gitem->data;
      item->events=item->events|event_hdr->mask;

      switch( item->type )
      {
        case DT_FSWATCH_IMAGE:
        {
          if( (event_hdr->mask&IN_CLOSE) && (item->events&IN_MODIFY) ) // Check if file modified and closed...
          {
            //  Something wrote on image externally and closed it, lets tag item as dirty...
            dt_image_t *img=(dt_image_t *)item->data;
            img->force_reimport = 1;
            if(darktable.develop->image==img)
              dt_dev_raw_reload(darktable.develop);
            item->events=0;
          }
          else if( (event_hdr->mask&IN_ATTRIB) && (item->events&IN_DELETE_SELF) && (item->events&IN_IGNORED))
          {
            // This pattern showed up when another file is replacing the original...
            dt_image_t *img=(dt_image_t *)item->data;
            img->force_reimport = 1;
            if(darktable.develop->image==img)
              dt_dev_raw_reload(darktable.develop);
            item->events=0;
          }
        }
        break;

        default:
          dt_print(DT_DEBUG_FSWATCH,"[fswatch_thread] Unhandled object type %d for event descriptor %ld\n", item->type, event_hdr->wd );
          break;
      }
    }
    else
      dt_print(DT_DEBUG_FSWATCH,"[fswatch_thread] Failed to found watch item for descriptor %ld\n", event_hdr->wd );

    dt_pthread_mutex_unlock(&fswatch->mutex);
  }
  dt_print(DT_DEBUG_FSWATCH,"[fswatch_thread] terminating.\n");
  g_free(event_hdr);
  g_free(name);
  return NULL;
}





const dt_fswatch_t* dt_fswatch_new()
{
  dt_fswatch_t *fswatch = g_malloc0(sizeof(dt_fswatch_t));
  if((fswatch->inotify_fd=inotify_init())==-1)
  {
    g_free(fswatch);
    return NULL;
  }
  fswatch->items=NULL;
  dt_pthread_mutex_init(&fswatch->mutex, NULL);
  pthread_create(&fswatch->thread, NULL, &_fswatch_thread, fswatch);
  dt_print(DT_DEBUG_FSWATCH,"[fswatch_new] Creating new context %p\n", fswatch);

  return fswatch;
}

void dt_fswatch_destroy(const dt_fswatch_t *fswatch)
{
  dt_print(DT_DEBUG_FSWATCH,"[fswatch_destroy] Destroying context %p\n", fswatch);
  dt_fswatch_t *ctx=(dt_fswatch_t *)fswatch;
  dt_pthread_mutex_destroy(&ctx->mutex);
//FIXME: g_list_free_full maybe?
  GList *item=g_list_first(fswatch->items);
  while(item)
  {
    g_free( item->data );
    item=g_list_next(item);
  }
  g_list_free(fswatch->items);
  g_free(ctx);
}

void dt_fswatch_add(const dt_fswatch_t * fswatch,dt_fswatch_type_t type, void *data)
{
  char filename[PATH_MAX] = { 0 };
  uint32_t mask=0;
  dt_fswatch_t *ctx=(dt_fswatch_t *)fswatch;
  filename[0] = '\0';

  switch(type)
  {
    case DT_FSWATCH_IMAGE:
      mask=IN_ALL_EVENTS;
      dt_image_full_path(((dt_image_t *)data)->id, filename, sizeof(filename));
      break;
    case DT_FSWATCH_CURVE_DIRECTORY:
      break;
    default:
      dt_print(DT_DEBUG_FSWATCH,"[fswatch_add] Unhandled object type %d\n",type);
      break;
  }

  if(filename[0] != '\0')
  {
    dt_pthread_mutex_lock(&ctx->mutex);
    _watch_t *item = g_malloc(sizeof(_watch_t));
    item->type=type;
    item->data=data;
    ctx->items=g_list_append(fswatch->items, item);
    item->descriptor=inotify_add_watch(fswatch->inotify_fd,filename,mask);
    dt_pthread_mutex_unlock(&ctx->mutex);
    dt_print(DT_DEBUG_FSWATCH,"[fswatch_add] Watch on object %p added on file %s\n", data,filename);
  }
  else
    dt_print(DT_DEBUG_FSWATCH,"[fswatch_add] No watch added, failed to get related filename of object type %d\n",type);

}

void dt_fswatch_remove(const dt_fswatch_t * fswatch,dt_fswatch_type_t type, void *data)
{
  dt_fswatch_t *ctx=(dt_fswatch_t *)fswatch;
  dt_pthread_mutex_lock(&ctx->mutex);
  dt_print(DT_DEBUG_FSWATCH,"[fswatch_remove] removing watch on object %p\n", data);
  GList *gitem=g_list_find_custom(fswatch->items,data,&_fswatch_items_by_data);
  if( gitem )
  {
    _watch_t *item=gitem->data;
    ctx->items=g_list_remove(ctx->items,item);
    inotify_rm_watch(fswatch->inotify_fd,item->descriptor);
    g_free(item);
  }
  else
    dt_print(DT_DEBUG_FSWATCH,"[fswatch_remove] Didn't find watch on object %p type %d\n", data,type);

  dt_pthread_mutex_unlock(&ctx->mutex);
}

#else // HAVE_INOTIFY
const dt_fswatch_t *dt_fswatch_new()
{
  dt_print(DT_DEBUG_FSWATCH, "[fswatch_new] fswatch not supported on your platform\n");
  return NULL;
}
void dt_fswatch_destroy(const dt_fswatch_t *fswatch)
{
}
void dt_fswatch_add(const dt_fswatch_t *fswatch, dt_fswatch_type_t type, void *data)
{
}
void dt_fswatch_remove(const dt_fswatch_t *fswatch, dt_fswatch_type_t type, void *data)
{
}
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
