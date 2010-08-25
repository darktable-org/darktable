/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DT_CAPTURE_H
#define DT_CAPTURE_H

#include "views/view.h"

/** Set's the jobcode name for the capture view */
void dt_capture_view_set_jobcode(const dt_view_t *view, const char *name);

/** Retreives the jobcode name for the capture */
const char *dt_capture_view_get_jobcode(const dt_view_t *view);

/** Get the film id of capture session */
uint32_t dt_capture_view_get_film_id(const dt_view_t *view);

const gchar *dt_capture_view_get_session_path(const dt_view_t *view);
const gchar *dt_capture_view_get_session_filename(const dt_view_t *view,const char *filename);
#endif
