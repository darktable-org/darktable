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

#include "views/view.h"

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

typedef void* dt_undo_data_t;

void record_undo(dt_view_t *view, dt_undo_type_t type, dt_undo_data_t *data, void (*undo) (dt_view_t *view, dt_undo_type_t type, dt_undo_data_t *data));

//  undo an element which correspond to filter. filter here is expected to be
//  a set of dt_undo_type_t.
void do_undo(uint32_t filter);

void do_redo(uint32_t filter);

//  removes all items which correspond to filter
void clear_undo(uint32_t filter);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
