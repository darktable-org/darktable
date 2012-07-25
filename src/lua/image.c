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
#include "common/colorlabels.h"
#include "common/colorlabels.h"
#include "common/metadata.h"
#include "metadata_gen.h"

/***********************************************************************
  handling of dt_image_t
 **********************************************************************/
#define LUA_IMAGE "dt_lua_image"
typedef struct {
	int imgid;
	const dt_image_t * const_image; 
	dt_image_t * image; 
} lua_image;

lua_image *dt_lua_checkimage(lua_State * L,int index){
	lua_image* my_image= luaL_checkudata(L,index,LUA_IMAGE);
	return my_image;
}

const dt_image_t*dt_lua_checkreadimage(lua_State*L,int index) {
	lua_image* my_image=dt_lua_checkimage(L,index);
	if(my_image->const_image ==NULL) {
		// check that id is valid
		sqlite3_stmt *stmt;
		DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where id = ?1", -1, &stmt, NULL);
		DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->imgid);
		if(sqlite3_step(stmt) != SQLITE_ROW) {
			sqlite3_finalize(stmt);
			luaL_error(L,"invalid id for image : %d",my_image->imgid);
			return NULL; // lua_error never returns, this is here to remind us of the fact
		}
		sqlite3_finalize(stmt);
		my_image->const_image= dt_image_cache_read_get(darktable.image_cache,my_image->imgid);
	}
	return my_image->const_image;
}

dt_image_t*dt_lua_checkwriteimage(lua_State*L,int index) {
	lua_image* my_image=dt_lua_checkimage(L,index);
	if(my_image->image ==NULL) {
		const dt_image_t* my_readimage=dt_lua_checkreadimage(L,index);
		// id is valid or checkreadimage would have raised an error
		my_image->image= dt_image_cache_write_get(darktable.image_cache,my_readimage);
	}
	return my_image->image;
}

static int image_gc(lua_State *L) {
	lua_image * my_image=dt_lua_checkimage(L,-1);
	//printf("%s %d\n",__FUNCTION__,my_image->imgid);
	if(my_image->image) {
		dt_image_cache_write_release(darktable.image_cache,my_image->image,DT_IMAGE_CACHE_SAFE);
		my_image->image=NULL;
	}
	if(my_image->const_image) {
		dt_image_cache_read_release(darktable.image_cache,my_image->const_image);
		my_image->const_image=NULL;
	}
	return 0;
}

void dt_lua_push_image(lua_State * L,int imgid) {
	// ckeck if image already is in the env
	// get the metatable and put it on top (side effect of newtable)
	luaL_newmetatable(L,LUA_IMAGE);
	lua_pushinteger(L,imgid);
	lua_gettable(L,-2);
	if(!lua_isnil(L,-1)) {
		//printf("%s %d (reuse)\n",__FUNCTION__,imgid);
		dt_lua_checkimage(L,-1);
		lua_remove(L,-2); // remove the table, but leave the image on top of the stac
		return;
	} else {
		//printf("%s %d (create)\n",__FUNCTION__,imgid);
		lua_pop(L,1); // remove nil at top
		lua_pushinteger(L,imgid);
		lua_image * my_image = (lua_image*)lua_newuserdata(L,sizeof(lua_image));
		luaL_setmetatable(L,LUA_IMAGE);
		my_image->imgid=imgid;
		my_image->const_image= NULL;
		my_image->image= NULL;
		// add the value to the metatable, so it can be reused
		lua_settable(L,-3);
		// put the value back on top
		lua_pushinteger(L,imgid);
		lua_gettable(L,-2);
		lua_remove(L,-2); // remove the table, but leave the image on top of the stac
	}
}

typedef enum {
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
	FILENAME,
	PATH,
	DUP_INDEX,
	WIDTH,
	HEIGHT,
	IS_LDR,
	IS_HDR,
	IS_RAW,
	RATING,
	ID,
	RED,
	YELLOW,
	GREEN,
	BLUE,
	PURPLE,
	CREATOR,
	PUBLISHER,
	TITLE,
	DESCRIPTION,
	RIGHTS,
	LAST_IMAGE_FIELD
} image_fields;
const char *const image_fields_name[] = {
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
	"filename",
	"path",
	"duplicate_index",
	"width",
	"height",
	"is_ldr",
	"is_hdr",
	"is_raw",
	"rating",
	"id",
	"red",
	"yellow",
	"green",
	"blue",
	"purple",
	"creator",
	"publisher",
	"title",
	"description",
	"rights",
	NULL
};

static int image_index(lua_State *L){
	const dt_image_t * my_image=dt_lua_checkreadimage(L,-2);
	switch(luaL_checkoption(L,-1,NULL,image_fields_name)) {
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
		case FILENAME:
			lua_pushstring(L,my_image->filename);
			return 1;
		case PATH:
			{
				sqlite3_stmt *stmt;
				DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
						"select folder from images, film_rolls where "
						"images.film_id = film_rolls.id and images.id = ?1", -1, &stmt, NULL);
				DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
				if(sqlite3_step(stmt) == SQLITE_ROW)
				{
					lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
				} else {
					sqlite3_finalize(stmt);
					return luaL_error(L,"should never happen");
				}
				sqlite3_finalize(stmt);
				return 1;
			}
		case DUP_INDEX:
			{
				// get duplicate suffix
				int version = 0;
				sqlite3_stmt *stmt;
				DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
						"select count(id) from images where filename in "
						"(select filename from images where id = ?1) and film_id in "
						"(select film_id from images where id = ?1) and id < ?1",
						-1, &stmt, NULL);
				DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
				if(sqlite3_step(stmt) == SQLITE_ROW)
					version = sqlite3_column_int(stmt, 0);
				sqlite3_finalize(stmt);
				lua_pushinteger(L,version);
				return 1;
			}
		case WIDTH:
			lua_pushinteger(L,my_image->width);
			return 1;
		case HEIGHT:
			lua_pushinteger(L,my_image->height);
			return 1;
		case IS_LDR:
			lua_pushboolean(L,dt_image_is_ldr(my_image));
			return 1;
		case IS_HDR:
			lua_pushboolean(L,dt_image_is_hdr(my_image));
			return 1;
		case IS_RAW:
			lua_pushboolean(L,dt_image_is_raw(my_image));
			return 1;
		case RATING:
			{
				int score = my_image->flags & 0x7;
				if(score >6) score=5;
				if(score ==6) score=-1;

				lua_pushinteger(L,score);
				return 1;
			}
		case ID:
			lua_pushinteger(L,my_image->height);
			return 1;
		case RED:
			lua_pushboolean(L,dt_colorlabels_check_label(my_image->id,DT_COLORLABELS_RED));
			return 1;
		case YELLOW:
			lua_pushboolean(L,dt_colorlabels_check_label(my_image->id,DT_COLORLABELS_YELLOW));
			return 1;
		case GREEN:
			lua_pushboolean(L,dt_colorlabels_check_label(my_image->id,DT_COLORLABELS_GREEN));
			return 1;
		case BLUE:
			lua_pushboolean(L,dt_colorlabels_check_label(my_image->id,DT_COLORLABELS_BLUE));
			return 1;
		case PURPLE:
			lua_pushboolean(L,dt_colorlabels_check_label(my_image->id,DT_COLORLABELS_PURPLE));
			return 1;
		case CREATOR:
			{
				sqlite3_stmt *stmt;
				DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),"select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
				DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
				DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_CREATOR);
				if(sqlite3_step(stmt) != SQLITE_ROW) {
					lua_pushstring(L,"");
				} else {
					lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
				}
				sqlite3_finalize(stmt);
				return 1;

			}
		case PUBLISHER:
			{
				sqlite3_stmt *stmt;
				DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),"select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
				DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
				DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_PUBLISHER);
				if(sqlite3_step(stmt) != SQLITE_ROW) {
					lua_pushstring(L,"");
				} else {
					lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
				}
				sqlite3_finalize(stmt);
				return 1;

			}
		case TITLE:
			{
				sqlite3_stmt *stmt;
				DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),"select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
				DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
				DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_TITLE);
				if(sqlite3_step(stmt) != SQLITE_ROW) {
					lua_pushstring(L,"");
				} else {
					lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
				}
				sqlite3_finalize(stmt);
				return 1;

			}
		case DESCRIPTION:
			{
				sqlite3_stmt *stmt;
				DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),"select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
				DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
				DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_DESCRIPTION);
				if(sqlite3_step(stmt) != SQLITE_ROW) {
					lua_pushstring(L,"");
				} else {
					lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
				}
				sqlite3_finalize(stmt);
				return 1;

			}
		case RIGHTS:
			{
				sqlite3_stmt *stmt;
				DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),"select value from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
				DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, my_image->id);
				DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_METADATA_XMP_DC_RIGHTS);
				if(sqlite3_step(stmt) != SQLITE_ROW) {
					lua_pushstring(L,"");
				} else {
					lua_pushstring(L,(char *)sqlite3_column_text(stmt, 0));
				}
				sqlite3_finalize(stmt);
				return 1;

			}

		default:
			return luaL_error(L,"should never happen %s",lua_tostring(L,-1));

	}
}

static int image_newindex(lua_State *L){
	dt_image_t * my_image=dt_lua_checkwriteimage(L,-3);
	switch(luaL_checkoption(L,-2,NULL,image_fields_name)) {
		case EXIF_EXPOSURE:
			my_image->exif_exposure = luaL_checknumber(L,-1);
			return 0;
		case EXIF_APERTURE:
			my_image->exif_aperture = luaL_checknumber(L,-1);
			return 0;
		case EXIF_ISO:
			my_image->exif_iso = luaL_checknumber(L,-1);
			return 0;
		case EXIF_FOCAL_LENGTH:
			my_image->exif_focal_length = luaL_checknumber(L,-1);
			return 0;
		case EXIF_FOCUS_DISTANCE:
			my_image->exif_focus_distance = luaL_checknumber(L,-1);
			return 0;
		case EXIF_CROP:
			my_image->exif_crop = luaL_checknumber(L,-1);
			return 0;
		case EXIF_MAKER:
			{
				size_t tgt_size;
				const char * value = luaL_checklstring(L,-1,&tgt_size);
				if(tgt_size > 32) {
					return luaL_error(L,"string value too long");
				}
				strncpy(my_image->exif_maker,value,32);
				return 0;
			}
		case EXIF_MODEL:
			{
				size_t tgt_size;
				const char * value = luaL_checklstring(L,-1,&tgt_size);
				if(tgt_size > 32) {
					return luaL_error(L,"string value too long");
				}
				strncpy(my_image->exif_maker,value,32);
				return 0;
			}
		case EXIF_LENS:
			{
				size_t tgt_size;
				const char * value = luaL_checklstring(L,-1,&tgt_size);
				if(tgt_size > 32) {
					return luaL_error(L,"string value too long");
				}
				strncpy(my_image->exif_model,value,32);
				return 0;
			}
		case EXIF_DATETIME_TAKEN:
			{
				size_t tgt_size;
				const char * value = luaL_checklstring(L,-1,&tgt_size);
				if(tgt_size > 52) {
					return luaL_error(L,"string value too long");
				}
				strncpy(my_image->exif_lens,value,52);
				return 0;
			}
		case RATING:
			{
				int my_score = luaL_checkinteger(L,-1);
				if(my_score > 5) return luaL_error(L,"rating too high : %d",my_score);
				if(my_score == -1) my_score = 6;
				if(my_score < -1) return luaL_error(L,"rating too low : %d",my_score);
				my_image->flags &= ~0x7;
				my_image->flags |= my_score;
				return 0;
			}

		case RED:
			if(lua_toboolean(L,-1)) { // no testing of type so we can benefit from all types of values
				dt_colorlabels_set_label(my_image->id,DT_COLORLABELS_RED);
			} else {
				dt_colorlabels_remove_label(my_image->id,DT_COLORLABELS_RED);
			}
			return 0;
		case YELLOW:
			if(lua_toboolean(L,-1)) { // no testing of type so we can benefit from all types of values
				dt_colorlabels_set_label(my_image->id,DT_COLORLABELS_YELLOW);
			} else {
				dt_colorlabels_remove_label(my_image->id,DT_COLORLABELS_YELLOW);
			}
			return 0;
		case GREEN:
			if(lua_toboolean(L,-1)) { // no testing of type so we can benefit from all types of values
				dt_colorlabels_set_label(my_image->id,DT_COLORLABELS_GREEN);
			} else {
				dt_colorlabels_remove_label(my_image->id,DT_COLORLABELS_GREEN);
			}
			return 0;
		case BLUE:
			if(lua_toboolean(L,-1)) { // no testing of type so we can benefit from all types of values
				dt_colorlabels_set_label(my_image->id,DT_COLORLABELS_BLUE);
			} else {
				dt_colorlabels_remove_label(my_image->id,DT_COLORLABELS_BLUE);
			}
			return 0;
		case PURPLE:
			if(lua_toboolean(L,-1)) { // no testing of type so we can benefit from all types of values
				dt_colorlabels_set_label(my_image->id,DT_COLORLABELS_PURPLE);
			} else {
				dt_colorlabels_remove_label(my_image->id,DT_COLORLABELS_PURPLE);
			}
			return 0;
		case CREATOR:
			dt_metadata_set(my_image->id,"Xmp.dc.creator",luaL_checkstring(L,-1));
			dt_image_synch_xmp(my_image->id);
			return 0;
		case PUBLISHER:
			dt_metadata_set(my_image->id,"Xmp.dc.publisher",luaL_checkstring(L,-1));
			dt_image_synch_xmp(my_image->id);
			return 0;
		case TITLE:
			dt_metadata_set(my_image->id,"Xmp.dc.title",luaL_checkstring(L,-1));
			dt_image_synch_xmp(my_image->id);
			return 0;
		case DESCRIPTION:
			dt_metadata_set(my_image->id,"Xmp.dc.description",luaL_checkstring(L,-1));
			dt_image_synch_xmp(my_image->id);
			return 0;
		case RIGHTS:
			dt_metadata_set(my_image->id,"Xmp.dc.title",luaL_checkstring(L,-1));
			dt_image_synch_xmp(my_image->id);
			return 0;
		case FILENAME:
		case PATH:
		case DUP_INDEX:
		case WIDTH:
		case HEIGHT:
		case IS_LDR:
		case IS_HDR:
		case IS_RAW:
		case ID:
			return luaL_error(L,"read only field : ",lua_tostring(L,-2));
		default:
			return luaL_error(L,"unknown index for image : ",lua_tostring(L,-2));

	}
}

static int image_next(lua_State *L){
	//printf("%s\n",__FUNCTION__);
	int index;
	if(lua_isnil(L,-1)) {
		index = 0;
	} else {
		index = luaL_checkoption(L,-1,NULL,image_fields_name);
	}
	index++;
	if(!image_fields_name[index]) {
		lua_pushnil(L);
		lua_pushnil(L);
	} else {
		lua_pop(L,1);// remove the key, table is at top
		lua_pushstring(L,image_fields_name[index]); // push the index string
		image_index(L);
	}
	return 2;
}
static int image_pairs(lua_State *L){
	lua_pushcfunction(L,image_next);
	lua_pushvalue(L,-2);
	lua_pushnil(L); // index set to null for reset
	return 3;
}
static const luaL_Reg dt_lua_image_meta[] = {
	{"__index", image_index },
	{"__newindex", image_newindex },
	{"__pairs", image_pairs },
	{"__gc", image_gc },
	{0,0}
};
void dt_lua_image_init(lua_State * L) {
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


void dt_lua_image_gc(lua_State *L) {
	luaL_newmetatable(L,LUA_IMAGE);
	lua_pushnil(L);  /* first key */
	while (lua_next(L, -2) != 0) {
		/* image is at top, imgid is just under */
		if(lua_type(L,-2) == LUA_TNUMBER)
			image_gc(L); // release the locks
		/* remove the image, for the call to lua_next */
		lua_pop(L, 1);
	}
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
		return luaL_error(L,"TBSL : changing index of a loop on variable images is not supported yet");
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
