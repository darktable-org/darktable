/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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

#include <glib.h>

/** this isn't a background job on purpose. it has to be really fast so it shouldn't
 *  require locking from image cache or anything like that.
 *  should we find out that we want to have a background job that crawls over all images
 *  we can maybe refactor this, but for now it's good the way it is.
 */

// this function iterates over ALL images from the database and checks whether
// - the XMP file on disk is newer than the timestamp from db
// - there is a .txt or .wav file associated with the image and mark so in the db
//   or if such a file no longer exists
// it returns the list of images with a (supposedly) updated xmp file to let the user decide
GList *dt_control_crawler_run();

// show a popup with the images, let the user decide what to do and free the list afterwards
void dt_control_crawler_show_image_list(GList *images);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

