/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson,
    copyright (c) 2011 johannes hanika

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
#include "develop/develop.h"
#include "control/control.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/history.h"
#include "common/imageio.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/tags.h"
#include "common/utility.h"
#include "lua/stmt.h"


void dt_history_delete_on_image(int32_t imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from history where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  dt_image_t tmp;
  dt_image_init (&tmp);

  /* if current image in develop reload history */
  if (dt_dev_is_current_image (darktable.develop, imgid))
    dt_dev_reload_history_items (darktable.develop);

  /* make sure mipmaps are recomputed */
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);

  /* remove darktable|style|* tags */
  dt_tag_detach_by_string("darktable|style%",imgid);

}

void
dt_history_delete_on_selection()
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int (stmt, 0);
    dt_history_delete_on_image (imgid);
  }
  sqlite3_finalize(stmt);
}

int
dt_history_load_and_apply_on_selection (gchar *filename)
{
  int res=0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, (int32_t)imgid);
    dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
    if(img)
    {
      if (dt_exif_xmp_read(img, filename, 1))
      {
        res=1;
        break;
      }

      /* if current image in develop reload history */
      if (dt_dev_is_current_image(darktable.develop, imgid))
        dt_dev_reload_history_items (darktable.develop);

      dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
      dt_image_cache_read_release(darktable.image_cache, img);
      dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
    }
  }
  sqlite3_finalize(stmt);
  return res;
}

int
dt_history_copy_and_paste_on_image (int32_t imgid, int32_t dest_imgid, gboolean merge)
{
  sqlite3_stmt *stmt;
  if(imgid==dest_imgid) return 1;

  if(imgid==-1)
  {
    dt_control_log(_("you need to copy history from an image before you paste it onto another"));
    return 1;
  }
    
  /* if merge onto history stack, lets find history offest in destination image */
  int32_t offs = 0;
  if (merge)
  {
    /* apply on top of history stack */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select count(num) from history where imgid = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    if (sqlite3_step (stmt) == SQLITE_ROW) offs = sqlite3_column_int (stmt, 0);
  }
  else
  {
    /* replace history stack */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from history where imgid = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    sqlite3_step (stmt);
  }
  sqlite3_finalize (stmt);

  /* add the history items to stack offest */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into history (imgid, num, module, operation, op_params, enabled, blendop_params, blendop_version) select ?1, num+?2, module, operation, op_params, enabled, blendop_params, blendop_version from history where imgid = ?3", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, offs);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, imgid);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* if current image in develop reload history */
  if (dt_dev_is_current_image(darktable.develop, dest_imgid))
    dt_dev_reload_history_items (darktable.develop);

  /* update xmp file */
  dt_image_synch_xmp(dest_imgid);

  dt_mipmap_cache_remove(darktable.mipmap_cache, dest_imgid);

  return 0;
}

GList *
dt_history_get_items(int32_t imgid)
{
  GList *result=NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select num, operation, enabled from history where imgid=?1 order by num desc", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    char name[512]= {0};
    dt_history_item_t *item=g_malloc (sizeof (dt_history_item_t));
    item->num = sqlite3_column_int (stmt, 0);
    g_snprintf(name,512,"%s (%s)",sqlite3_column_text (stmt, 1),(sqlite3_column_int (stmt, 2)!=0)?_("on"):_("off"));
    item->name = g_strdup (name);
    result = g_list_append (result,item);
  }
  return result;
}

char *
dt_history_get_items_as_string(int32_t imgid)
{
  GList *items = NULL;
  const char *onoff[2] = {_("off"), _("on")};
  unsigned int count = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select operation, enabled from history where imgid=?1 order by num desc", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  // collect all the entries in the history from the db
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    char name[512]= {0};
    g_snprintf(name,512,"%s (%s)", dt_iop_get_localized_name((char*)sqlite3_column_text(stmt, 0)), (sqlite3_column_int(stmt, 1)==0)?onoff[0]:onoff[1]);
    items = g_list_append(items, g_strdup(name));
    count++;
  }
  return dt_util_glist_to_str("\n", items, count);
}

int
dt_history_copy_and_paste_on_selection (int32_t imgid, gboolean merge)
{
  if (imgid < 0) return 1;

  int res=0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from selected_images where imgid != ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if (sqlite3_step(stmt) == SQLITE_ROW)
  {
    do
    {
      /* get imgid of selected image */
      int32_t dest_imgid = sqlite3_column_int (stmt, 0);

      /* paste history stack onto image id */
      dt_history_copy_and_paste_on_image(imgid,dest_imgid,merge);

    }
    while (sqlite3_step (stmt) == SQLITE_ROW);
  }
  else res = 1;

  sqlite3_finalize(stmt);
  return res;
}

/********************************************
  LUA STUFF
  *******************************************/
#define LUA_HISTORY "dt_lua_history"
typedef struct {
	int imgid;
} history_type;

int dt_history_lua_check(lua_State * L,int index){
	return ((history_type*)luaL_checkudata(L,index,LUA_HISTORY))->imgid;
}

void dt_history_lua_push(lua_State * L,int imgid) {
	// ckeck if history already is in the env
	// get the metatable and put it on top (side effect of newtable)
	luaL_newmetatable(L,LUA_HISTORY);
	lua_getfield(L,-1,"allocated");
	lua_pushinteger(L,imgid);
	lua_gettable(L,-2);
	// at this point our stack is :
	// -1 : the object or nil if it is not allocated
	// -2 : the allocation table
	// -3 : the metatable
	lua_remove(L,-3); // remove the metatable, we don't need it anymore
	if(!lua_isnil(L,-1)) {
		//printf("%s %d (reuse)\n",__FUNCTION__,imgid);
		dt_history_lua_check(L,-1);
		lua_remove(L,-2); // remove the table, but leave the history on top of the stac
		return;
	} else {
		//printf("%s %d (create)\n",__FUNCTION__,imgid);
		lua_pop(L,1); // remove nil at top
		lua_pushinteger(L,imgid);
		history_type * my_history = (history_type*)lua_newuserdata(L,sizeof(history_type));
		luaL_setmetatable(L,LUA_HISTORY);
		my_history->imgid =imgid;
		// add the value to the metatable, so it can be reused
		lua_settable(L,-3);
		// put the value back on top
		lua_pushinteger(L,imgid);
		lua_gettable(L,-2);
		lua_remove(L,-2); // remove the table, but leave the history on top of the stac
	}
}


static int history_index(lua_State *L){
	int imgid=dt_history_lua_check(L,-2);
	int value = luaL_checkinteger(L,-1);
	sqlite3_stmt *stmt;
	DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select operation, enabled from history where imgid=?1 and num=?2 ", -1, &stmt, NULL);
	DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
	DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, value);
	int result = sqlite3_step(stmt);
	if(result != SQLITE_ROW){
		return luaL_error(L,"incorrect history index %d",value);
	}
	lua_pushfstring(L,"%s (%s)",sqlite3_column_text (stmt, 0),(sqlite3_column_int (stmt, 1)!=0)?_("on"):_("off"));
	return 1;

}

static int history_next(lua_State *L) {
	//printf("%s\n",__FUNCTION__);
	//TBSL : check index and find the correct position in stmt if index was changed manually
	// 2 args, table, index, returns the next index,value or nil,nil return the first index,value if index is nil 

	sqlite3_stmt *stmt = dt_lua_stmt_check(L,-2);
	if(lua_isnil(L,-1)) {
		sqlite3_reset(stmt);
	} else if(luaL_checkinteger(L,-1) != sqlite3_column_int(stmt,0)) {
		return luaL_error(L,"TBSL : changing index of a loop on history images is not supported yet");
	}
	int result = sqlite3_step(stmt);
	if(result != SQLITE_ROW){
		return 0;
	}
	int historyidx = sqlite3_column_int(stmt,0);
	lua_pushinteger(L,historyidx);
	lua_pushfstring(L,"%s (%s)",sqlite3_column_text (stmt, 1),(sqlite3_column_int (stmt, 2)!=0)?_("on"):_("off"));
	return 2;
}
static int history_pairs(lua_State *L) {
	int imgid=dt_history_lua_check(L,-1);
	lua_pushcfunction(L,history_next);
	sqlite3_stmt *stmt;
	DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select num, operation, enabled from history where imgid=?1 order by num asc", -1, &stmt, NULL);
	DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
	dt_lua_stmt_push(L,stmt);
	lua_pushnil(L); // index set to null for reset
	return 3;
}
static int history_tostring(lua_State *L){
	int imgid=dt_history_lua_check(L,-1);
	const char * description =dt_history_get_items_as_string(imgid);
	if(!description) lua_pushstring(L,"");
	else lua_pushstring(L,description);
	return 1;
}
static const luaL_Reg dt_lua_history_meta[] = {
	{"__index", history_index },
	{"__tostring", history_tostring },
	{"__pairs", history_pairs },
	{0,0}
};
static int history_init(lua_State * L) {
	luaL_setfuncs(L,dt_lua_history_meta,0);
	dt_lua_init_singleton(L);
	return 0;
}


dt_lua_type dt_history_lua_type ={
	"history",
	history_init,
	NULL,
};

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
