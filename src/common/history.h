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

#ifndef DT_HISTORY_H
#define DT_HISTORY_H
#include <sqlite3.h>
#include <glib.h>
#include <inttypes.h>

/** copy history from imgid and pasts on dest_imgid, merge or overwrite... */
int dt_history_copy_and_paste_on_image(int32_t imgid, int32_t dest_imgid, gboolean merge);

void dt_history_delete_on_image(int32_t imgid);

/** copy history from imgid and pasts on selected images, merge or overwrite... */
int dt_history_copy_and_paste_on_selection(int32_t imgid, gboolean merge);

/** load a dt file and applies to selected images */
int dt_history_load_and_apply_on_selection(gchar *filename);

/** delete historystack of selected images */
void dt_history_delete_on_selection();

#endif
