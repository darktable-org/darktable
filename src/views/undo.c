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

static GList *_undo_list = NULL;
static GList *_redo_list = NULL;

typedef struct dt_undo_t
{
  dt_view_t *view;
  dt_undo_type_t type;
  dt_undo_data_t *data;
  void (*undo) (dt_view_t *view, dt_undo_type_t type, dt_undo_data_t *data);
} dt_undo_t;

void record_undo(dt_view_t *view, dt_undo_type_t type, dt_undo_data_t *data, void (*undo) (dt_view_t *view, dt_undo_type_t type, dt_undo_data_t *data))
{
  dt_undo_t *udata = g_malloc(sizeof (dt_undo_t));

  udata->view = view;
  udata->type = type;
  udata->data = data;
  udata->undo = undo;

  _undo_list = g_list_prepend(_undo_list, (gpointer)udata);

  // recording an undo data invalidate all the redo
  g_list_free_full(_redo_list,&g_free);
}

void do_redo(uint32_t filter)
{
  GList *l = g_list_first(_redo_list);

  // check for first item that is matching the given pattern

  while (l)
  {
    dt_undo_t *redo = (dt_undo_t*)l->data;
    if (redo->type & filter)
    {
      //  first remove element from _redo_list
      _redo_list = g_list_remove(_redo_list, redo);

      //  callback with redo data
      redo->undo(redo->view, redo->type, redo->data);

      //  add old position back into the undo list
      _undo_list = g_list_prepend(_undo_list, redo);
      break;
    }
    l=g_list_next(l);
  };
}

void do_undo(uint32_t filter)
{
  GList *l = g_list_first(_undo_list);

  // check for first item that is matching the given pattern

  while (l)
  {
    dt_undo_t *undo = (dt_undo_t*)l->data;
    if (undo->type & filter)
    {
      //  first remove element from _undo_list
      _undo_list = g_list_remove(_undo_list, undo);

      //  callback with undo data
      undo->undo(undo->view, undo->type, undo->data);

      //  add element into the redo list as filed with our previous position (before undo)

      _redo_list = g_list_prepend(_redo_list, undo);
      break;
    }
    l=g_list_next(l);
  };
}

void clear_undo(uint32_t filter)
{
  GList *l = g_list_first(_undo_list);

  // check for first item that is matching the given pattern

  while (l)
  {
    dt_undo_t *undo = (dt_undo_t*)l->data;
    if (undo->type & filter)
    {
      //  remove this element
      g_free(undo->data);
      _undo_list = g_list_remove(_undo_list, undo);
    }
    l=g_list_next(l);
  };
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
