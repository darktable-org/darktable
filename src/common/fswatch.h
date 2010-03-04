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


/** fswatch context */
typedef struct dt_fswatch_t {
	uint32_t inotify_fd;
	pthread_mutex_t mutex;
	pthread_t thread;
	GList *items;
}
dt_fswatch_t;

/** Types of filesystem watches. */
typedef enum dt_fswatch_type_t
{
	DT_FSWATCH_IMAGE = 0,          		// Watch is an image file
	DT_FSWATCH_CURVE_DIRECTORY,  	// Watch is on directory for curves files << Just an test
}
dt_fswatch_type_t;

/** Initializes a new fswatch context. */
const dt_fswatch_t* dt_fswatch_new();
/** Cleanup and destroy fswatch context. \remarks After this point pointer at fswatch is invalid.*/
void dt_fswatch_destroy(const dt_fswatch_t *fswatch);
/** Adds an watch of type and assign data. */
void dt_fswatch_add(const dt_fswatch_t *fswatch, dt_fswatch_type_t type, void *data);
/** Removes an watch of type and assigned data. */
void dt_fswatch_remove(const dt_fswatch_t * fswatch, dt_fswatch_type_t type, void *data);

#endif