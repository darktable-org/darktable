/*
   This file is part of darktable,
   copyright (c) 2012 Jeremy Rosen

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

#include "lua/image.h"
#include "lua/stmt.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"

/***********************************************************************
  handling of dt_image_t
 **********************************************************************/
#define LUA_IMAGE "dt_lua_image"
typedef struct {
	int imgid;
	const dt_image_t * const_image; 
} lua_image;

lua_image *dt_lua_checkimage(lua_State * L,int index){
	lua_image* my_image= luaL_checkudata(L,index,LUA_IMAGE);
	return my_image;
}

static int image_tostring(lua_State *L) {
	//printf("%s %d\n",__FUNCTION__,dt_lua_checkimage(L,-1));
	lua_pushinteger(L,dt_lua_checkimage(L,-1)->imgid);
	return 1;
}

static int image_gc(lua_State *L) {
	lua_image * my_image=dt_lua_checkimage(L,-1);
	//printf("%s %d\n",__FUNCTION__,my_image->imgid);
	dt_image_cache_read_release(darktable.image_cache,my_image->const_image);
	return 0;
}

void dt_lua_push_image(lua_State * L,int imgid) {
	// ckeck if image already is in the env
	// get the metatable and put it on top (side effect of newtable)
	luaL_newmetatable(L,LUA_IMAGE);
	lua_pushinteger(L,imgid);
	lua_gettable(L,-2);
	if(!lua_isnil(L,-1)) {
		dt_lua_checkimage(L,-1);
		lua_remove(L,-2); // remove the table, but leave the image on top of the stac
		return;
	} else {
		lua_pop(L,1); // remove nil at top
		// check that id is valid
		sqlite3_stmt *stmt;
		DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where id = ?1", -1, &stmt, NULL);
		DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
		if(sqlite3_step(stmt) != SQLITE_ROW) {
			sqlite3_finalize(stmt);
			luaL_error(L,"invalid id for image : %d",imgid);
		}
		sqlite3_finalize(stmt);
		lua_pushinteger(L,imgid);
		lua_image * my_image = (lua_image*)lua_newuserdata(L,sizeof(lua_image));
		luaL_setmetatable(L,LUA_IMAGE);
		my_image->imgid=imgid;
		my_image->const_image= dt_image_cache_read_get(darktable.image_cache,imgid);
		// add the value to the metatable, so it can be reused
		lua_settable(L,-3);
		// put the value back on top
		lua_pushinteger(L,imgid);
		lua_gettable(L,-2);
		lua_remove(L,-2); // remove the table, but leave the image on top of the stac
	}
}

typedef enum {
	FILENAME,
	EXIF_EXPOSURE,
	EXIF_APERTURE,
	EXIF_ISO,
	EXIF_FOCAL_LENGTH,
	EXIF_FOCUS_DISTANCE,
	EXIF_CROP,
	EXIF_MAKER,
	EXIF_MODEL,
	EXIF_LENS,
	EXIF_DATETIME_TAKEN,
	LAST_IMAGE_FIELD
} image_fields;
const char *const image_fields_name[] = {
	"filename",
	"exif_exposure",
	"exif_aperture",
	"exif_iso",
	"exif_focal_length",
	"exif_focus_distance",
	"exif_crop",
	"exif_maker",
	"exif_model",
	"exif_lens",
	"exif_datetime_taken",
	NULL
};

static int image_index(lua_State *L){
	const dt_image_t * my_image=dt_lua_checkimage(L,-2)->const_image;
	switch(luaL_checkoption(L,-1,NULL,image_fields_name)) {
		case FILENAME:
			lua_pushstring(L,my_image->filename);
			return 1;
		case EXIF_EXPOSURE:
			lua_pushnumber(L,my_image->exif_exposure);
			return 1;
		case EXIF_APERTURE:
			lua_pushnumber(L,my_image->exif_aperture);
			return 1;
		case EXIF_ISO:
			lua_pushnumber(L,my_image->exif_iso);
			return 1;
		case EXIF_FOCAL_LENGTH:
			lua_pushnumber(L,my_image->exif_focal_length);
			return 1;
		case EXIF_FOCUS_DISTANCE:
			lua_pushnumber(L,my_image->exif_focus_distance);
			return 1;
		case EXIF_CROP:
			lua_pushnumber(L,my_image->exif_crop);
			return 1;
		case EXIF_MAKER:
			lua_pushstring(L,my_image->exif_maker);
			return 1;
		case EXIF_MODEL:
			lua_pushstring(L,my_image->exif_model);
			return 1;
		case EXIF_LENS:
			lua_pushstring(L,my_image->exif_lens);
			return 1;
		case EXIF_DATETIME_TAKEN:
			lua_pushstring(L,my_image->exif_datetime_taken);
			return 1;
		default:
			dt_image_cache_read_release(darktable.image_cache,my_image);
			luaL_error(L,"should never happen");
			return 0;

	}
}

static const luaL_Reg dt_lua_image_meta[] = {
	{"__tostring", image_tostring },
	{"__index", image_index },
	{"__gc", image_gc },
	{0,0}
};
void dt_lua_init_image(lua_State * L) {
	luaL_newmetatable(L,LUA_IMAGE);
	luaL_setfuncs(L,dt_lua_image_meta,0);
	// add a metatable to the metatable, just for the __mode field
	lua_newtable(L);
	lua_pushstring(L,"v");
	lua_setfield(L,-2,"__mode");
	lua_setmetatable(L,-2);
	//pop the metatable itself to be clean
	lua_pop(L,1);
}
/***********************************************************************
  Creating the images global variable
 **********************************************************************/
static int images_next(lua_State *L) {
	//printf("%s\n",__FUNCTION__);
	//TBSL : check index and find the correct position in stmt if index was changed manually
	// 2 args, table, index, returns the next index,value or nil,nil return the first index,value if index is nil 

	sqlite3_stmt *stmt = dt_lua_checkstmt(L,-2);
	if(lua_isnil(L,-1)) {
		sqlite3_reset(stmt);
	} else if(luaL_checkinteger(L,-1) != sqlite3_column_int(stmt,0)) {
		luaL_error(L,"TBSL : changing index of a loop on variable images is not supported yet");
	}
	int result = sqlite3_step(stmt);
	if(result != SQLITE_ROW){
		lua_pushnil(L);
		lua_pushnil(L);
		return 2;
	}
	int imgid = sqlite3_column_int(stmt,0);
	lua_pushinteger(L,imgid);
	dt_lua_push_image(L,imgid);
	return 2;
}

static int images_index(lua_State *L) {
	int imgid = luaL_checkinteger(L,-1);
	dt_lua_push_image(L,imgid);
	return 1;
}

static int images_pairs(lua_State *L) {
	lua_pushcfunction(L,images_next);
	sqlite3_stmt *stmt;
	DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images", -1, &stmt, NULL);
	dt_lua_push_stmt(L,stmt);
	lua_pushnil(L); // index set to null for reset
	return 3;
}
static const luaL_Reg stmt_meta[] = {
	{"__pairs", images_pairs },
	{"__index", images_index },
	{0,0}
};
void dt_lua_images_init(lua_State * L) {
	lua_newuserdata(L,1); // placeholder we can't use a table because we can't prevent assignment
	luaL_newlib(L,stmt_meta);
	lua_setmetatable(L,-2);
	lua_setfield(L,-2,"images");
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
