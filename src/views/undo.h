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
#ifndef DT_UNDO_H
#define DT_UNDO_H

#include "common/dtpthread.h"
#include "views/view.h"
#include <glib.h>

//  types that are known by the undo module
typedef enum dt_undo_type_t
{
  DT_UNDO_GEOTAG = 1,
} dt_undo_type_t;

#define DT_UNDO_ALL (DT_UNDO_GEOTAG)

//  types that are known by the undo module are declared here to be shared by
//  all the views supporting undo.
typedef struct dt_undo_geotag_t
{
  int imgid;
  float longitude, latitude;
} dt_undo_geotag_t;

typedef void *dt_undo_data_t;

typedef struct dt_undo_t
{
  GList *undo_list, *redo_list;
  dt_pthread_mutex_t mutex;
} dt_undo_t;

dt_undo_t *dt_undo_init(void);
void dt_undo_cleanup(dt_undo_t *self);

// record a change that will be insered into the undo list
void dt_undo_record(dt_undo_t *self, dt_view_t *view, dt_undo_type_t type, dt_undo_data_t *data,
                    void (*undo)(dt_view_t *view, dt_undo_type_t type, dt_undo_data_t *item));

//  undo an element which correspond to filter. filter here is expected to be
//  a set of dt_undo_type_t.
void dt_undo_do_undo(dt_undo_t *self, uint32_t filter);

//  redo a previously undone action, does nothing if the redo list is empty
void dt_undo_do_redo(dt_undo_t *self, uint32_t filter);

//  removes all items which correspond to filter in the undo/redo lists
void dt_undo_clear(dt_undo_t *self, uint32_t filter);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
