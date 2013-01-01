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
#include "lua/glist.h"
#include "lautoc.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/colorlabels.h"
#include "common/history.h"
#include "common/metadata.h"
#include "metadata_gen.h"

/********************************************
common : for all types that are internally linked to a numid
 *******************************************/
static int numid_compare(lua_State*L) {
  int imgid=*((int*)lua_touserdata(L,-1));
  int imgid2=*((int*)lua_touserdata(L,-1));
  lua_pushboolean(L,imgid==imgid2);
  return 1;
}
/********************************************
  image labels handling
 *******************************************/
typedef int dt_lua_colorlabel_t;

static int colorlabels_pushFunc(lua_State* L,luaA_Type type_id,const void* colorlabel_ptr){
  int * my_colorlabel = (int*)lua_newuserdata(L,sizeof(dt_lua_colorlabel_t));
  *my_colorlabel = *(int*)colorlabel_ptr;
	luaL_setmetatable(L,"dt_lua_colorlabel_t");
  return 1;
}

static void colorlabels_toFunc(lua_State*L,luaA_Type type_id, void* colorlabel_ptr, int index) {
  *(int*)colorlabel_ptr =*((int*)luaL_checkudata(L,index,"dt_lua_colorlabel_t"));
}


static int colorlabel_index(lua_State *L){
  int imgid;
  luaA_to(L,dt_lua_colorlabel_t,&imgid,-2);
  lua_pushboolean(L,dt_colorlabels_check_label(imgid,lua_tointeger(L,-1)));
  return 1;
}

static int colorlabel_newindex(lua_State *L){
  int imgid;
  luaA_to(L,dt_lua_colorlabel_t,&imgid,-3);
  if(lua_toboolean(L,-1)) { // no testing of type so we can benefit from all types of values
    dt_colorlabels_set_label(imgid,lua_tointeger(L,-2));
  } else {
    dt_colorlabels_remove_label(imgid,lua_tointeger(L,-2));
  }
  return 0;
}

/************************************
  image history handling
 ***********************************/

static int history_item_tostring(lua_State*L) {
	dt_history_item_t * item =luaL_checkudata(L,-1,"dt_history_item_t");
	lua_pushfstring(L,"%d : %s (%s)",item->num,item->name,item->op);
  return 1;
}

static int history_item_gc(lua_State*L) {
	dt_history_item_t * item =luaL_checkudata(L,-1,"dt_history_item_t");
	free(item->name);
	free(item->op);
	return 0;
}
/***********************************************************************
  handling of dt_image_t
 **********************************************************************/
static const char * image_typename = "dt_lua_image";

static const dt_image_t*dt_lua_checkreadimage(lua_State*L,int index) {
  int imgid=dt_lua_image_get(L,index);
  return dt_image_cache_read_get(darktable.image_cache,imgid);
}

static void dt_lua_releasereadimage(lua_State*L,const dt_image_t* image) {
  dt_image_cache_read_release(darktable.image_cache,image);
}

static dt_image_t*dt_lua_checkwriteimage(lua_State*L,int index) {
  const dt_image_t* my_readimage=dt_lua_checkreadimage(L,index);
  return dt_image_cache_write_get(darktable.image_cache,my_readimage);
}

static void dt_lua_releasewriteimage(lua_State*L,dt_image_t* image) {
  dt_image_cache_write_release(darktable.image_cache,image,DT_IMAGE_CACHE_SAFE);
  dt_lua_releasereadimage(L,image);
}

int dt_lua_image_get(lua_State *L,int index) {
 return *(int*)luaL_checkudata(L,index,image_typename);
}
void dt_lua_image_push(lua_State * L,int imgid) {
  // check that id is valid
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    luaL_error(L,"invalid id for image : %d",imgid);
    return;
  }
  sqlite3_finalize(stmt);
  int * my_image = (int*)lua_newuserdata(L,sizeof(int));
  *my_image=imgid;
	luaL_setmetatable(L,image_typename);
}

void dt_lua_image_glist_push(lua_State *L,GList * list) 
{
  GList * elt = list;
  lua_newtable(L);
  while(elt) {
    dt_lua_image_push(L,(long int)elt->data);
    luaL_ref(L,-2);
    elt = g_list_next(elt);
  }
}

GList * dt_lua_image_glist_get(lua_State *L,int index)
{
  GList * list = NULL;
  // recreate list of images
  lua_pushnil(L);  /* first key */
  while (lua_next(L, index -1) != 0) {
    /* uses 'key' (at index -2) and 'value' (at index -1) */
    long int imgid = dt_lua_image_get(L,-1);
    lua_pop(L,1);
    list = g_list_prepend(list,(gpointer)imgid);
  }
  list = g_list_reverse(list);
  return list;
}


static int image_get_history(lua_State*L) {
  int imgid = dt_lua_image_get(L,-1);
  GList * items = dt_history_get_items(imgid,true);
  dt_lua_push_glist(L,items,dt_history_item_t,true);
  while(items) {
    g_free(items->data);
    items = items->next;
  }
  return 1;
}


static int image_reset_history(lua_State*L) {
  int imgid = dt_lua_image_get(L,-1);
  dt_history_delete_on_image(imgid);
  return 0;
}


typedef enum {
  PATH,
  DUP_INDEX,
  IS_LDR,
  IS_HDR,
  IS_RAW,
  RATING,
  ID,
  COLORLABEL,
  CREATOR,
  PUBLISHER,
  TITLE,
  DESCRIPTION,
  RIGHTS,
  GET_HISTORY,
  RESET,
  LAST_IMAGE_FIELD
} image_fields;
const char *image_fields_name[] = {
  "path",
  "duplicate_index",
  "is_ldr",
  "is_hdr",
  "is_raw",
  "rating",
  "id",
  "colorlabels",
  "creator",
  "publisher",
  "title",
  "description",
  "rights",
  "get_history",
  "reset",
  NULL
};

static int image_index(lua_State *L){
  const char* membername = lua_tostring(L, -1);
  const dt_image_t * my_image=dt_lua_checkreadimage(L,-2);
  if(luaA_struct_has_member_name(L,dt_image_t,membername)) {
    const int result = luaA_struct_push_member_name(L, dt_image_t, my_image, membername);
    dt_lua_releasereadimage(L,my_image);
    return result;
  }
  switch(luaL_checkoption(L,-1,NULL,image_fields_name)) {
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
          dt_lua_releasereadimage(L,my_image);
          return luaL_error(L,"should never happen");
        }
        sqlite3_finalize(stmt);
        break;
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
        break;
      }
    case IS_LDR:
      lua_pushboolean(L,dt_image_is_ldr(my_image));
      break;
    case IS_HDR:
      lua_pushboolean(L,dt_image_is_hdr(my_image));
      break;
    case IS_RAW:
      lua_pushboolean(L,dt_image_is_raw(my_image));
      break;
    case RATING:
      {
        int score = my_image->flags & 0x7;
        if(score >6) score=5;
        if(score ==6) score=-1;

        lua_pushinteger(L,score);
        break;
      }
    case ID:
      lua_pushinteger(L,my_image->height);
      break;
    case COLORLABEL:
      {
        luaA_push(L,dt_lua_colorlabel_t,&my_image->id);
        break;
      }
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
        break;

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
        break;

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
        break;

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
        break;

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
        break;

      }
    case GET_HISTORY:
      {
        lua_pushcfunction(L,image_get_history);
        break;
      }
    case RESET:
      {
        lua_pushcfunction(L,image_reset_history);
        break;
      }
    default:
      dt_lua_releasereadimage(L,my_image);
      return luaL_error(L,"should never happen %s",lua_tostring(L,-1));

  }
  dt_lua_releasereadimage(L,my_image);
  return 1;
}

static int image_newindex(lua_State *L){
  const char* membername = lua_tostring(L, -2);
  dt_image_t * my_image=dt_lua_checkwriteimage(L,-3);
  if(luaA_struct_has_member_name(L,dt_image_t,membername)) {
    if(luaA_type_has_to_func(luaA_struct_typeof_member_name(L,dt_image_t,membername))) {
      luaA_struct_to_member_name(L, dt_image_t, my_image, membername,-1);
    } else {
      dt_lua_releasewriteimage(L,my_image);
      luaL_error(L,"%s is read only",membername);
    }
    dt_lua_releasewriteimage(L,my_image);
    return 0;
  }
  switch(luaL_checkoption(L,-2,NULL,image_fields_name)) {
    case RATING:
      {
        int my_score = luaL_checkinteger(L,-1);
        if(my_score > 5) {
          dt_lua_releasewriteimage(L,my_image);
          return luaL_error(L,"rating too high : %d",my_score);
        }
        if(my_score == -1) my_score = 6;
        if(my_score < -1) {
          dt_lua_releasewriteimage(L,my_image);
          return luaL_error(L,"rating too low : %d",my_score);
        }
        my_image->flags &= ~0x7;
        my_image->flags |= my_score;
        break;
      }

    case CREATOR:
      dt_metadata_set(my_image->id,"Xmp.dc.creator",luaL_checkstring(L,-1));
      dt_image_synch_xmp(my_image->id);
      break;
    case PUBLISHER:
      dt_metadata_set(my_image->id,"Xmp.dc.publisher",luaL_checkstring(L,-1));
      dt_image_synch_xmp(my_image->id);
      break;
    case TITLE:
      dt_metadata_set(my_image->id,"Xmp.dc.title",luaL_checkstring(L,-1));
      dt_image_synch_xmp(my_image->id);
      break;
    case DESCRIPTION:
      dt_metadata_set(my_image->id,"Xmp.dc.description",luaL_checkstring(L,-1));
      dt_image_synch_xmp(my_image->id);
      break;
    case RIGHTS:
      dt_metadata_set(my_image->id,"Xmp.dc.title",luaL_checkstring(L,-1));
      dt_image_synch_xmp(my_image->id);
      break;
    case GET_HISTORY:
    case RESET:
    case PATH:
    case DUP_INDEX:
    case IS_LDR:
    case IS_HDR:
    case IS_RAW:
    case ID:
    case COLORLABEL:
      dt_lua_releasewriteimage(L,my_image);
      return luaL_error(L,"read only field : ",lua_tostring(L,-2));
    default:
      dt_lua_releasewriteimage(L,my_image);
      return luaL_error(L,"unknown index for image : ",lua_tostring(L,-2));

  }
  dt_lua_releasewriteimage(L,my_image);
  return 0;
}

static int image_tostring(lua_State *L) {
  const dt_image_t * my_image=dt_lua_checkreadimage(L,-1);
  char image_name[PATH_MAX];
  dt_image_full_path(my_image->id,image_name,PATH_MAX);
  dt_image_path_append_version(my_image->id,image_name,PATH_MAX);
  lua_pushstring(L,image_name);
  dt_lua_releasereadimage(L,my_image);
  return 1;
}
static const luaL_Reg image_meta[] = {
  {"__index", image_index },
  {"__newindex", image_newindex },
  {"__tostring", image_tostring },
  {"__eq", numid_compare },
  {0,0}
};
///////////////
// toplevel and common
///////////////

static int image_table(lua_State*L) {
	lua_newtable(L);
	sqlite3_stmt *stmt;
	DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images", -1, &stmt, NULL);
	while(sqlite3_step(stmt) == SQLITE_ROW) {
		int imgid = sqlite3_column_int(stmt,0);
		dt_lua_image_push(L,imgid);
		luaL_ref(L,-2);
	}
	sqlite3_finalize(stmt);
	return 1;
}

int dt_lua_init_image(lua_State * L) {
  /* history */
  dt_lua_init_type(L,dt_history_item_t,NULL,NULL,NULL);
  luaA_struct(L,dt_history_item_t);
  luaA_struct_member(L,dt_history_item_t,num,const int);
  luaA_struct_member(L,dt_history_item_t,op,const char*);
  luaA_struct_member(L,dt_history_item_t,name,const char*);
  lua_pushcfunction(L,history_item_gc);
  lua_setfield(L,-2,"__gc");
  lua_pushcfunction(L,history_item_tostring);
  lua_setfield(L,-2,"__tostring");
  /* colorlabels */
  dt_lua_init_type(L,dt_lua_colorlabel_t,dt_colorlabels_name,colorlabel_index,colorlabel_newindex);
  luaA_conversion(dt_lua_colorlabel_t,colorlabels_pushFunc,colorlabels_toFunc);
  lua_pushcfunction(L,numid_compare);
	lua_setfield(L,-2,"__eq");
  /*  image */
  luaA_struct(L,dt_image_t);
  luaA_struct_member(L,dt_image_t,exif_exposure,float);
  luaA_struct_member(L,dt_image_t,exif_aperture,float);
  luaA_struct_member(L,dt_image_t,exif_iso,float);
  luaA_struct_member(L,dt_image_t,exif_focal_length,float);
  luaA_struct_member(L,dt_image_t,exif_focus_distance,float);
  luaA_struct_member(L,dt_image_t,exif_crop,float);
  luaA_struct_member(L,dt_image_t,exif_maker,char_32);
  luaA_struct_member(L,dt_image_t,exif_model,char_32);
  luaA_struct_member(L,dt_image_t,exif_lens,char_52);
  luaA_struct_member(L,dt_image_t,exif_datetime_taken,char_20);
  luaA_struct_member(L,dt_image_t,filename,const char_filename_length);
  luaA_struct_member(L,dt_image_t,width,const int32_t);
  luaA_struct_member(L,dt_image_t,height,const int32_t);
  luaA_struct_member(L,dt_image_t,longitude,double);
  luaA_struct_member(L,dt_image_t,latitude,double);
  luaL_newmetatable(L,image_typename);
  luaL_setfuncs(L,image_meta,0);
  luaA_Type my_type =  luaA_type_find(image_typename);
  lua_pushnumber(L,my_type);
	lua_setfield(L,-2,"__luaA_Type");
	lua_pushlightuserdata(L,image_fields_name);
	lua_pushcclosure(L,dt_lua_autotype_pairs,1);
	lua_setfield(L,-2,"__pairs");


  /* darktable.images() */
  dt_lua_push_darktable_lib(L);
  lua_pushcfunction(L,image_table);
  lua_setfield(L,-2,"images");
  return 0;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
