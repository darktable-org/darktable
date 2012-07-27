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
#include "common/dt_lua.h"

/** copy history from imgid and pasts on dest_imgid, merge or overwrite... */
int dt_history_copy_and_paste_on_image(int32_t imgid, int32_t dest_imgid, gboolean merge);

void dt_history_delete_on_image(int32_t imgid);

/** copy history from imgid and pasts on selected images, merge or overwrite... */
int dt_history_copy_and_paste_on_selection(int32_t imgid, gboolean merge);

/** load a dt file and applies to selected images */
int dt_history_load_and_apply_on_selection(gchar *filename);

/** delete historystack of selected images */
void dt_history_delete_on_selection();

typedef struct dt_history_item_t
{
  guint num;
  gchar *name;
} dt_history_item_t;

/** get list of history items for image */
GList *dt_history_get_items(int32_t imgid);

/** get list of history items for image as a nice string */
char *dt_history_get_items_as_string(int32_t imgid);

/********************************************
  LUA STUFF
  *******************************************/

int dt_history_lua_check(lua_State * L,int index);
void dt_history_lua_push(lua_State * L,int imgid);

extern dt_lua_type dt_history_lua_type;


#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
