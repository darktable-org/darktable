/*
    This file is part of darktable,
    copyright (c) 2016 pascal obry

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
#include "common/undo.h"
#include <glib.h>    // for GList, gpointer, g_list_first, g_list_prepend
#include <stdlib.h>  // for NULL, malloc, free
#include <sys/time.h>

const double MAX_TIME_PERIOD = 0.5; // in second

typedef struct dt_undo_item_t
{
  gpointer user_data;
  dt_undo_type_t type;
  dt_undo_data_t *data;
  double ts;
  void (*undo)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t *data);
  void (*free_data)(gpointer data);
} dt_undo_item_t;

dt_undo_t *dt_undo_init(void)
{
  dt_undo_t *udata = malloc(sizeof(dt_undo_t));
  udata->undo_list = NULL;
  udata->redo_list = NULL;
  dt_pthread_mutex_init(&udata->mutex, NULL);
  return udata;
}

void dt_undo_cleanup(dt_undo_t *self)
{
  dt_undo_clear(self, DT_UNDO_ALL);
  dt_pthread_mutex_destroy(&self->mutex);
}

static void _free_undo_data(void *p)
{
  dt_undo_item_t *item = (dt_undo_item_t *)p;
  if (item->free_data) item->free_data(item->data);
  free(item);
}

void dt_undo_record(dt_undo_t *self, gpointer user_data, dt_undo_type_t type, dt_undo_data_t *data,
                    void (*undo)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t *item),
                    void (*free_data)(gpointer data))
{
  if(!self) return;

  dt_undo_item_t *item = malloc(sizeof(dt_undo_item_t));

  item->user_data = user_data;
  item->type      = type;
  item->data      = data;
  item->undo      = undo;
  item->free_data = free_data;
  item->ts        = dt_get_wtime();

  dt_pthread_mutex_lock(&self->mutex);
  self->undo_list = g_list_prepend(self->undo_list, (gpointer)item);

  // recording an undo data invalidate all the redo
  g_list_free_full(self->redo_list, _free_undo_data);
  self->redo_list = NULL;
  dt_pthread_mutex_unlock(&self->mutex);
}

void dt_undo_do_redo(dt_undo_t *self, uint32_t filter)
{
  if(!self) return;

  dt_pthread_mutex_lock(&self->mutex);

  GList *l = g_list_first(self->redo_list);

  // check for first item that is matching the given pattern

  while(l)
  {
    dt_undo_item_t *item = (dt_undo_item_t *)l->data;

    if(item->type & filter)
    {
      const double first_item_ts = item->ts;

      //  when found, redo all items of the same type and in the same time period

      do
      {
        GList *next = g_list_next(l);

        //  first remove element from _redo_list
        self->redo_list = g_list_remove(self->redo_list, item);

        //  callback with redo data
        item->undo(item->user_data, item->type, item->data);

        //  add old position back into the undo list
        self->undo_list = g_list_prepend(self->undo_list, item);

        l = next;
        if (l) item = (dt_undo_item_t *)l->data;
      } while (l && (item->type & filter) && (item->ts - first_item_ts < MAX_TIME_PERIOD));

      break;
    }
    l = g_list_next(l);
  };
  dt_pthread_mutex_unlock(&self->mutex);
}

void dt_undo_do_undo(dt_undo_t *self, uint32_t filter)
{
  if(!self) return;

  dt_pthread_mutex_lock(&self->mutex);

  GList *l = g_list_first(self->undo_list);

  // the first matching item (current state) is moved into the redo list

  while(l)
  {
    dt_undo_item_t *item = (dt_undo_item_t *)l->data;
    GList *next = g_list_next(l);

    if(item->type & filter)
    {
      const double first_item_ts = item->ts;

      self->undo_list = g_list_remove(self->undo_list, item);
      self->redo_list = g_list_prepend(self->redo_list, item);

      //  now record in the redo list also all items that are on the same time period

      l = next;
      while (l)
      {
        next = g_list_next(l);
        item = (dt_undo_item_t *)l->data;
        //  undo item
        item->undo(item->user_data, item->type, item->data);

        //  if we are on the same time frame, just continue
        if (item->type & filter && (first_item_ts - item->ts < MAX_TIME_PERIOD))
        {
          self->undo_list = g_list_remove(self->undo_list, item);
          self->redo_list = g_list_prepend(self->redo_list, item);
          l = next;
        }
        else
          break;
      };

      break;
    }
    l = next;
  }

  dt_pthread_mutex_unlock(&self->mutex);
}

static void _undo_clear_list(GList **list, uint32_t filter)
{
  GList *l = g_list_first(*list);

  // check for first item that is matching the given pattern

  while(l)
  {
    dt_undo_item_t *item = (dt_undo_item_t *)l->data;
    GList *next = l->next;
    if(item->type & filter)
    {
      //  remove this element
      *list = g_list_remove(*list, item);
      _free_undo_data((void *)item);
    }
    l = next;
  };
}

void dt_undo_clear(dt_undo_t *self, uint32_t filter)
{
  if(!self) return;

  dt_pthread_mutex_lock(&self->mutex);
  _undo_clear_list(&self->undo_list, filter);
  _undo_clear_list(&self->redo_list, filter);
  self->undo_list = NULL;
  self->redo_list = NULL;
  dt_pthread_mutex_unlock(&self->mutex);
}

static void _undo_iterate(GList *list, uint32_t filter, gpointer user_data,
                          void (*apply)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t *item))
{
  GList *l = g_list_first(list);

  // check for first item that is matching the given pattern

  while(l)
  {
    dt_undo_item_t *item = (dt_undo_item_t *)l->data;
    if(item->type & filter)
    {
      apply(user_data, item->type, item->data);
    }
    l = l->next;
  };
}

void dt_undo_iterate_internal(dt_undo_t *self, uint32_t filter, gpointer user_data,
                              void (*apply)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t *item))
{
  if(!self) return;

  _undo_iterate(self->undo_list, filter, user_data, apply);
  _undo_iterate(self->redo_list, filter, user_data, apply);
}


void dt_undo_iterate(dt_undo_t *self, uint32_t filter, gpointer user_data,
                     void (*apply)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t *item))
{
  if(!self) return;

  dt_pthread_mutex_lock(&self->mutex);
  dt_undo_iterate_internal(self, filter, user_data, apply);
  dt_pthread_mutex_unlock(&self->mutex);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
