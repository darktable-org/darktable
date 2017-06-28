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

#pragma once

#include "common/dtpthread.h"  // for dt_pthread_mutex_t
#include <glib.h>              // for gpointer, GList
#include <stdint.h>            // for uint32_t

//  types that are known by the undo module
typedef enum dt_undo_type_t
{
  DT_UNDO_GEOTAG = 1 << 0,
  DT_UNDO_HISTORY = 1 << 1,
  DT_UNDO_MASK = 1 << 2,
  DT_UNDO_DEVELOP = DT_UNDO_HISTORY | DT_UNDO_MASK,
  DT_UNDO_ALL = DT_UNDO_GEOTAG | DT_UNDO_HISTORY | DT_UNDO_MASK
} dt_undo_type_t;

typedef void *dt_undo_data_t;

typedef struct dt_undo_t
{
  GList *undo_list, *redo_list;
  dt_pthread_mutex_t mutex;
} dt_undo_t;

dt_undo_t *dt_undo_init(void);
void dt_undo_cleanup(dt_undo_t *self);

// record a change that will be insered into the undo list
void dt_undo_record(dt_undo_t *self, gpointer user_data, dt_undo_type_t type, dt_undo_data_t *data,
                    void (*undo)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t *item),
                    void (*free_data)(gpointer data));

//  undo an element which correspond to filter. filter here is expected to be
//  a set of dt_undo_type_t.
void dt_undo_do_undo(dt_undo_t *self, uint32_t filter);

//  redo a previously undone action, does nothing if the redo list is empty
void dt_undo_do_redo(dt_undo_t *self, uint32_t filter);

//  removes all items which correspond to filter in the undo/redo lists
void dt_undo_clear(dt_undo_t *self, uint32_t filter);

void dt_undo_iterate_internal(dt_undo_t *self, uint32_t filter, gpointer user_data,
                              void (*apply)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t *item));

void dt_undo_iterate(dt_undo_t *self, uint32_t filter, gpointer user_data,
                     void (*apply)(gpointer user_data, dt_undo_type_t type, dt_undo_data_t *item));

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
