/*
    This file is part of darktable,
    Copyright (C) 2017-2021 darktable developers.

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

#include "common/undo.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/image.h"
#include "control/control.h"
#include <glib.h>   // for GList, gpointer, g_list_prepend
#include <stdlib.h> // for NULL, malloc, free
#include <sys/time.h>

const double MAX_TIME_PERIOD = 0.5; // in second

typedef struct dt_undo_item_t
{
  gpointer user_data;
  dt_undo_type_t type;
  dt_undo_data_t data;
  double ts;
  gboolean is_group;
  void (*undo)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action, GList **imgs);
  void (*free_data)(gpointer data);
} dt_undo_item_t;

dt_undo_t *dt_undo_init(void)
{
  dt_undo_t *udata = malloc(sizeof(dt_undo_t));
  udata->undo_list = NULL;
  udata->redo_list = NULL;
  udata->disable_next = FALSE;
  udata->locked = FALSE;
  dt_pthread_mutex_init(&udata->mutex, NULL);
  udata->group = DT_UNDO_NONE;
  udata->group_indent = 0;
  dt_print(DT_DEBUG_UNDO, "[undo] init\n");
  return udata;
}

#define LOCK \
  dt_pthread_mutex_lock(&self->mutex); self->locked = TRUE

#define UNLOCK \
  self->locked = FALSE; dt_pthread_mutex_unlock(&self->mutex)

void dt_undo_disable_next(dt_undo_t *self)
{
  self->disable_next = TRUE;
  dt_print(DT_DEBUG_UNDO, "[undo] disable next\n");
}

void dt_undo_cleanup(dt_undo_t *self)
{
  dt_undo_clear(self, DT_UNDO_ALL);
  dt_pthread_mutex_destroy(&self->mutex);
}

static void _free_undo_data(void *p)
{
  dt_undo_item_t *item = (dt_undo_item_t *)p;
  if(item->free_data) item->free_data(item->data);
  free(item);
}

static void _undo_record(dt_undo_t *self, gpointer user_data, dt_undo_type_t type, dt_undo_data_t data,
                         gboolean is_group,
                         void (*undo)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item, dt_undo_action_t action, GList **imgs),
                         void (*free_data)(gpointer data))
{
  if(!self) return;

  if(self->disable_next)
  {
    if(free_data) free_data(data);
    self->disable_next = FALSE;
  }
  else
  {
    // do not block, if an undo record is asked and there is a lock it means that this call has been done in un
    // undo/redo callback. We just skip this event.

    if(!self->locked)
    {
      LOCK;

      dt_undo_item_t *item = malloc(sizeof(dt_undo_item_t));

      item->user_data = user_data;
      item->type      = type;
      item->data      = data;
      item->undo      = undo;
      item->free_data = free_data;
      item->ts        = dt_get_wtime();
      item->is_group  = is_group;

      self->undo_list = g_list_prepend(self->undo_list, (gpointer)item);

      // recording an undo data invalidate all the redo
      g_list_free_full(self->redo_list, _free_undo_data);
      self->redo_list = NULL;

      dt_print(DT_DEBUG_UNDO, "[undo] record for type %d (length %d)\n",
               type, g_list_length(self->undo_list));

      UNLOCK;
    }
  }
}

void dt_undo_start_group(dt_undo_t *self, dt_undo_type_t type)
{
  if(!self) return;

  if(self->group == DT_UNDO_NONE)
  {
    dt_print(DT_DEBUG_UNDO, "[undo] start group for type %d\n", type);
    self->group = type;
    self->group_indent = 1;
    _undo_record(self, NULL, type, NULL, TRUE, NULL, NULL);
  }
  else
    self->group_indent++;
}

void dt_undo_end_group(dt_undo_t *self)
{
  if(!self) return;

  assert(self->group_indent>0);
  self->group_indent--;
  if(self->group_indent == 0)
  {
    _undo_record(self, NULL, self->group, NULL, TRUE, NULL, NULL);
    dt_print(DT_DEBUG_UNDO, "[undo] end group for type %d\n", self->group);
    self->group = DT_UNDO_NONE;
  }
}

void dt_undo_record(dt_undo_t *self, gpointer user_data, dt_undo_type_t type, dt_undo_data_t data,
                    void (*undo)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item, dt_undo_action_t action, GList **imgs),
                    void (*free_data)(gpointer data))
{
  _undo_record(self, user_data, type, data, FALSE, undo, free_data);
}

gint _images_list_cmp(gconstpointer a, gconstpointer b)
{
  return GPOINTER_TO_INT(a) - GPOINTER_TO_INT(b);
}

static void _undo_do_undo_redo(dt_undo_t *self, uint32_t filter, dt_undo_action_t action)
{
  if(!self) return;

  LOCK;

  // we take/remove item from the FROM list and add them into the TO list:
  GList **from = action == DT_ACTION_UNDO ? &self->undo_list : &self->redo_list;
  GList **to   = action == DT_ACTION_UNDO ? &self->redo_list : &self->undo_list;

  GList *imgs = NULL;

  // check for first item that is matching the given pattern

  dt_print(DT_DEBUG_UNDO, "[undo] action %s for %d (from length %d -> to length %d)\n",
           action == DT_ACTION_UNDO?"UNDO":"DO", filter, g_list_length(*from), g_list_length(*to));

  for(GList *l = *from; l; l = g_list_next(l))
  {
    dt_undo_item_t *item = (dt_undo_item_t *)l->data;

    if(item->type & filter)
    {
      if(item->is_group)
      {
        gboolean is_group = FALSE;

        GList *next = g_list_next(l);

        //  first move the group item into the TO list
        *from = g_list_remove(*from, item);
        *to   = g_list_prepend(*to, item);

        while((l = next) && !is_group)
        {
          item = (dt_undo_item_t *)l->data;
          next = g_list_next(l);

          //  first remove element from FROM list
          *from = g_list_remove(*from, item);

          //  callback with undo or redo data
          if(item->is_group)
            is_group = TRUE;
          else
            item->undo(item->user_data, item->type, item->data, action, &imgs);

          //  add old position back into the TO list
          *to = g_list_prepend(*to, item);
        }
      }
      else
      {
        const double first_item_ts = item->ts;
        gboolean in_group = FALSE;

        //  when found, handle all items of the same type and in the same time period

        do
        {
          GList *next = g_list_next(l);

          //  first remove element from FROM list
          *from = g_list_remove(*from, item);

          if(item->is_group)
            in_group = !in_group;
          else
            //  callback with redo or redo data
            item->undo(item->user_data, item->type, item->data, action, &imgs);

          //  add old position back into the TO list
          *to = g_list_prepend(*to, item);

          l = next;
          if(l) item = (dt_undo_item_t *)l->data;
        } while(l && (item->type & filter) && (in_group || (fabs(item->ts - first_item_ts) < MAX_TIME_PERIOD)));
      }

      break;
    }
  }
  UNLOCK;

  if(imgs)
  {
    imgs = g_list_sort(imgs, _images_list_cmp);
    // remove duplicates
    for(const GList *img = imgs; img; img = g_list_next(img))
      while(img->next && img->data == img->next->data)
        imgs = g_list_delete_link(imgs, img->next);
    // udpate xmp for updated images

    dt_image_synch_xmps(imgs);
  }

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, imgs);
}

void dt_undo_do_redo(dt_undo_t *self, uint32_t filter)
{
  _undo_do_undo_redo(self, filter, DT_ACTION_REDO);
}

void dt_undo_do_undo(dt_undo_t *self, uint32_t filter)
{
  _undo_do_undo_redo(self, filter, DT_ACTION_UNDO);
}

static void _undo_clear_list(GList **list, uint32_t filter)
{
  // check for first item that is matching the given pattern

  GList *next;
  for(GList *l = *list; l; l = next)
  {
    dt_undo_item_t *item = (dt_undo_item_t *)l->data;
    next = g_list_next(l); // get next node now, because we may delete the current one
    if(item->type & filter)
    {
      //  remove this element
      *list = g_list_remove(*list, item);
      _free_undo_data((void *)item);
    }
  };

  dt_print(DT_DEBUG_UNDO, "[undo] clear list for %d (length %d)\n",
           filter, g_list_length(*list));
}

void dt_undo_clear(dt_undo_t *self, uint32_t filter)
{
  if(!self) return;

  LOCK;
  _undo_clear_list(&self->undo_list, filter);
  _undo_clear_list(&self->redo_list, filter);
  self->undo_list = NULL;
  self->redo_list = NULL;
  self->disable_next = FALSE;
  UNLOCK;
}

static void _undo_iterate(GList *list, uint32_t filter, gpointer user_data,
                          void (*apply)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item))
{
  // check for first item that is matching the given pattern
  for(GList *l = list; l; l = g_list_next(l))
  {
    dt_undo_item_t *item = (dt_undo_item_t *)l->data;
    if(!item->is_group && (item->type & filter))
    {
      apply(user_data, item->type, item->data);
    }
  };
}

void dt_undo_iterate_internal(dt_undo_t *self, uint32_t filter, gpointer user_data,
                              void (*apply)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item))
{
  if(!self) return;

  _undo_iterate(self->undo_list, filter, user_data, apply);
  _undo_iterate(self->redo_list, filter, user_data, apply);
}


void dt_undo_iterate(dt_undo_t *self, uint32_t filter, gpointer user_data,
                     void (*apply)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item))
{
  if(!self) return;

  LOCK;
  dt_undo_iterate_internal(self, filter, user_data, apply);
  UNLOCK;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
