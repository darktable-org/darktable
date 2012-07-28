/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include "common/darktable.h"
#include "common/colorlabels.h"
#include "common/image_cache.h"
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include <gdk/gdkkeysyms.h>

const char *dt_colorlabels_name[] = {
    "red",
    "yellow",
    "green",
    "blue",
    "purple",
    NULL // termination
};

void dt_colorlabels_remove_labels_selection ()
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "delete from color_labels where imgid in (select imgid from selected_images)", NULL, NULL, NULL);
}

void dt_colorlabels_remove_labels (const int imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from color_labels where imgid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_colorlabels_set_label (const int imgid, const int color)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into color_labels (imgid, color) values (?1, ?2)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_colorlabels_remove_label (const int imgid, const int color)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from color_labels where imgid=?1 and color=?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}


void dt_colorlabels_toggle_label_selection (const int color)
{
  sqlite3_stmt *stmt;
  // store away all previously unlabeled images in selection:
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into memory.color_labels_temp select a.imgid from selected_images as a join color_labels as b on a.imgid = b.imgid where b.color = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // delete all currently colored image labels in selection
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from color_labels where imgid in (select imgid from selected_images) and color=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // label all previously unlabeled images:
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into color_labels select imgid, ?1 from selected_images where imgid not in (select imgid from memory.color_labels_temp)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, color);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // clean up
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "delete from memory.color_labels_temp", NULL, NULL, NULL);
}

void dt_colorlabels_toggle_label (const int imgid, const int color)
{
  if(imgid <= 0) return;
  sqlite3_stmt *stmt, *stmt2;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from color_labels where imgid=?1 and color=?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, color);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from color_labels where imgid=?1 and color=?2", -1, &stmt2, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 2, color);
    sqlite3_step(stmt2);
    sqlite3_finalize(stmt2);
  }
  else
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into color_labels (imgid, color) values (?1, ?2)", -1, &stmt2, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 2, color);
    sqlite3_step(stmt2);
    sqlite3_finalize(stmt2);
  }
  sqlite3_finalize(stmt);
}

int dt_colorlabels_check_label (const int imgid, const int color)
{
	if(imgid <= 0) return 0;
	sqlite3_stmt *stmt;
	DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from color_labels where imgid=?1 and color=?2", -1, &stmt, NULL);
	DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
	DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, color);
	if(sqlite3_step(stmt) == SQLITE_ROW)
	{
		sqlite3_finalize(stmt);
		return 1;
	}
	else
	{
		sqlite3_finalize(stmt);
		return 0;
	}
}

gboolean dt_colorlabels_key_accel_callback(GtkAccelGroup *accel_group,
                                       GObject *acceleratable, guint keyval,
                                       GdkModifierType modifier, gpointer data)
{
  const long int mode = (long int)data;
  int selected;
  DT_CTL_GET_GLOBAL(selected, lib_image_mouse_over_id);
  if(selected <= 0)
  {
    switch(mode)
    {
      case 0:
      case 1:
      case 2:
      case 3:
      case 4: // colors red, yellow, green, blue, purple
        dt_colorlabels_toggle_label_selection(mode);
        break;
      case 5:
      default: // remove all selected
        dt_colorlabels_remove_labels_selection();
        break;
    }
  }
  else
  {
    switch(mode)
    {
      case 0:
      case 1:
      case 2:
      case 3:
      case 4: // colors red, yellow, green, blue, purple
        dt_colorlabels_toggle_label(selected, mode);
        break;
      case 5:
      default: // remove all selected
        dt_colorlabels_remove_labels(selected);
        break;
    }
  }
  // synch to file:
  // TODO: move color labels to image_t cache and sync via write_get!
  dt_image_synch_xmp(selected);
  dt_control_queue_redraw_center();
  return TRUE;
}

//FIXME: XMP uses Red, Green, ... while we use red, green, ... What should this function return?
const char* dt_colorlabels_to_string(int label)
{
	if(label < 0 || label >= DT_COLORLABELS_LAST ) return ""; // shouldn't happen
	return dt_colorlabels_name[label];
}

/********************************************
  LUA STUFF
  *******************************************/
#define LUA_COLORLABEL "dt_lua_colorlabel"
typedef struct {
	int imgid;
} colorlabel_type;

static int colorlabels_check(lua_State * L,int index){
	return ((colorlabel_type*)luaL_checkudata(L,index,LUA_COLORLABEL))->imgid;
}

void dt_colorlabels_lua_push(lua_State * L,int imgid) {
	if(dt_lua_singleton_find(L,imgid,&dt_colorlabels_lua_type)) {
		return;
	}
	colorlabel_type * my_colorlabel = (colorlabel_type*)lua_newuserdata(L,sizeof(colorlabel_type));
	my_colorlabel->imgid =imgid;
	dt_lua_singleton_register(L,imgid,&dt_colorlabels_lua_type);
}


static int colorlabel_index(lua_State *L){
	int imgid=colorlabels_check(L,-2);
	const int value =luaL_checkoption(L,-1,NULL,dt_colorlabels_name);
	if(value < 0 || value >= DT_COLORLABELS_LAST) {
			return luaL_error(L,"should never happen %s",lua_tostring(L,-1));
	} else {
		lua_pushboolean(L,dt_colorlabels_check_label(imgid,value));
		return 1;
	}
}

static int colorlabel_newindex(lua_State *L){
	int imgid=colorlabels_check(L,-3);
	const int value =luaL_checkoption(L,-2,NULL,dt_colorlabels_name);
	if(value < 0 || value >= DT_COLORLABELS_LAST) {
			return luaL_error(L,"should never happen %s",lua_tostring(L,-1));
	} else {
			if(lua_toboolean(L,-1)) { // no testing of type so we can benefit from all types of values
				dt_colorlabels_set_label(imgid,value);
			} else {
				dt_colorlabels_remove_label(imgid,value);
			}
			return 0;

	}
}

static const luaL_Reg dt_lua_colorlabel_meta[] = {
	{"__index", colorlabel_index },
	{"__newindex", colorlabel_newindex },
	{0,0}
};
static int colorlabel_init(lua_State * L) {
	luaL_setfuncs(L,dt_lua_colorlabel_meta,0);
	dt_lua_init_name_list_pair(L, dt_colorlabels_name);
	dt_lua_init_singleton(L);
	//pop the metatable itself to be clean
	//loader convention, we declare a type but we don't create any function
	lua_pushnil(L);
	return 1;
}


dt_lua_type dt_colorlabels_lua_type ={
	"colorlabel",
	colorlabel_init,
	NULL
};

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
