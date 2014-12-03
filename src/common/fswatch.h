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
#ifndef DT_FSWATCH_H
#define DT_FSWATCH_H

#include "common/darktable.h"
#include "common/dtpthread.h"


/** fswatch context */
typedef struct dt_fswatch_t
{
  uint32_t inotify_fd;
  dt_pthread_mutex_t mutex;
  pthread_t thread;
  GList *items;
} dt_fswatch_t;

/** Types of filesystem watches. */
typedef enum dt_fswatch_type_t
{
  /** watch is an image file */
  DT_FSWATCH_IMAGE = 0,
  /** watch is on directory for curves files << Just an test  */
  DT_FSWATCH_CURVE_DIRECTORY,
} dt_fswatch_type_t;

/** initializes a new fswatch context. */
const dt_fswatch_t *dt_fswatch_new();
/** cleanup and destroy fswatch context. \remarks After this point pointer at fswatch is invalid.*/
void dt_fswatch_destroy(const dt_fswatch_t *fswatch);
/** adds an watch of type and assign data. */
void dt_fswatch_add(const dt_fswatch_t *fswatch, dt_fswatch_type_t type, void *data);
/** removes an watch of type and assigned data. */
void dt_fswatch_remove(const dt_fswatch_t *fswatch, dt_fswatch_type_t type, void *data);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
