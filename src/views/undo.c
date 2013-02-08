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

GList *ulist = NULL;

typedef struct dt_undo_t
{
  dt_view_t *view;
  dt_undo_type_t type;
  dt_undo_data_t *data;
  void (*undo) (dt_view_t *view, dt_undo_type_t type, dt_undo_data_t *data);
} dt_undo_t;

void record_undo(dt_view_t *view, dt_undo_type_t type, dt_undo_data_t *data, void (*undo) (dt_view_t *view, dt_undo_type_t type, dt_undo_data_t *data))
{
  dt_undo_t *udata = malloc(sizeof (dt_undo_t));

  udata->view = view;
  udata->type = type;
  udata->data = data;
  udata->undo = undo;

  ulist = g_list_prepend(ulist, (gpointer)udata);
}

void do_undo(uint32_t filter)
{
  GList *l = g_list_first(ulist);

  // check for first item that is matching the given pattern

  while (l)
  {
    dt_undo_t *undo = (dt_undo_t*)l->data;
    if (undo->type & filter)
    {
      undo->undo(undo->view, undo->type, undo->data);
      //  remove this element
      free(undo->data);
      ulist = g_list_remove(ulist, undo);
      break;
    }
    l=g_list_next(l);
  };
}

void clear_undo(uint32_t filter)
{
  GList *l = g_list_first(ulist);

  // check for first item that is matching the given pattern

  while (l)
  {
    dt_undo_t *undo = (dt_undo_t*)l->data;
    if (undo->type & filter)
    {
      //  remove this element
      free(undo->data);
      ulist = g_list_remove(ulist, undo);
    }
    l=g_list_next(l);
  };
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
