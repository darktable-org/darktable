/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson.

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

#ifndef DT_CAMERA_IMPORT_DIALOG_H
#define DT_CAMERA_IMPORT_DIALOG_H

#include <glib.h>
#include <gtk/gtk.h>

/** Fires up the camera import dialog, result is a list of images paths on camera to be imported. */
void dt_camera_import_dialog_new(GList **result);

#endif


