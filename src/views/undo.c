/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2012 tobias ellinghaus.

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

#include "views/undo.h"
#include <glib.h>

typedef struct dt_undo_item_t
{
  dt_view_t *view;
  dt_undo_type_t type;
  dt_undo_data_t *data;
  void (*undo)(dt_view_t *view, dt_undo_type_t type, dt_undo_data_t *data);
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

void dt_undo_record(dt_undo_t *self, dt_view_t *view, dt_undo_type_t type, dt_undo_data_t *data,
                    void (*undo)(dt_view_t *view, dt_undo_type_t type, dt_undo_data_t *item))
{
  dt_undo_item_t *item = g_malloc(sizeof(dt_undo_item_t));

  item->view = view;
  item->type = type;
  item->data = data;
  item->undo = undo;

  dt_pthread_mutex_lock(&self->mutex);
  self->undo_list = g_list_prepend(self->undo_list, (gpointer)item);

  // recording an undo data invalidate all the redo
  g_list_free_full(self->redo_list, &g_free);
  self->redo_list = NULL;
  dt_pthread_mutex_unlock(&self->mutex);
}

void dt_undo_do_redo(dt_undo_t *self, uint32_t filter)
{
  dt_pthread_mutex_lock(&self->mutex);
  GList *l = g_list_first(self->redo_list);

  // check for first item that is matching the given pattern

  while(l)
  {
    dt_undo_item_t *item = (dt_undo_item_t *)l->data;
    if(item->type & filter)
    {
      //  first remove element from _redo_list
      self->redo_list = g_list_remove(self->redo_list, item);

      //  callback with redo data
      item->undo(item->view, item->type, item->data);

      //  add old position back into the undo list
      self->undo_list = g_list_prepend(self->undo_list, item);
      break;
    }
    l = g_list_next(l);
  };
  dt_pthread_mutex_unlock(&self->mutex);
}

void dt_undo_do_undo(dt_undo_t *self, uint32_t filter)
{
  dt_pthread_mutex_lock(&self->mutex);
  GList *l = g_list_first(self->undo_list);

  // check for first item that is matching the given pattern

  while(l)
  {
    dt_undo_item_t *item = (dt_undo_item_t *)l->data;
    if(item->type & filter)
    {
      //  first remove element from _undo_list
      self->undo_list = g_list_remove(self->undo_list, item);

      //  callback with undo data
      item->undo(item->view, item->type, item->data);

      //  add element into the redo list as filed with our previous position (before undo)

      self->redo_list = g_list_prepend(self->redo_list, item);
      break;
    }
    l = g_list_next(l);
  };
  dt_pthread_mutex_unlock(&self->mutex);
}

static void dt_undo_clear_list(GList **list, uint32_t filter)
{
  GList *l = g_list_first(*list);

  // check for first item that is matching the given pattern

  while(l)
  {
    dt_undo_item_t *item = (dt_undo_item_t *)l->data;
    if(item->type & filter)
    {
      //  remove this element
      g_free(item->data);
      *list = g_list_remove(*list, item);
    }
    l = g_list_next(l);
  };
}

void dt_undo_clear(dt_undo_t *self, uint32_t filter)
{
  dt_pthread_mutex_lock(&self->mutex);
  dt_undo_clear_list(&self->undo_list, filter);
  dt_undo_clear_list(&self->redo_list, filter);
  self->undo_list = NULL;
  self->redo_list = NULL;
  dt_pthread_mutex_unlock(&self->mutex);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
