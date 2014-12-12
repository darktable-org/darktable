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
#include "common/camera_control.h"

typedef struct dt_camera_import_dialog_param_t
{
  dt_camera_t *camera;
  gchar *jobcode;
  time_t time_override;
  /** Filenames of selected images to import*/
  GList *result;
} dt_camera_import_dialog_param_t;

/** Fires up the camera import dialog, result is a list of images paths on camera to be imported. */
void dt_camera_import_dialog_new(dt_camera_import_dialog_param_t *param);

#endif


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
