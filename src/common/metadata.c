/*
    This file is part of darktable,
    copyright (c) 2010 tobias ellinghaus.

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

#include "metadata.h"

static void dt_metadata_set_dt(int id, const char* key, const char* value){
	sqlite3_stmt *stmt;

	int keyid = dt_metadata_get_keyid(key);

	if(id == -1){
		sqlite3_prepare_v2(darktable.db, "delete from meta_data where id in (select imgid from selected_images) and key = ?1", -1, &stmt, NULL);
		sqlite3_bind_int(stmt, 1, keyid);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);

		if(value != NULL && value[0] != '\0'){
			sqlite3_prepare_v2(darktable.db, "insert into meta_data (id, key, value) select imgid, ?1, ?2 from selected_images", -1, &stmt, NULL);
			sqlite3_bind_int(stmt, 1, keyid);
			sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}
	} else {
		sqlite3_prepare_v2(darktable.db, "delete from meta_data where id = ?1 and key = ?2", -1, &stmt, NULL);
		sqlite3_bind_int(stmt, 1, id);
		sqlite3_bind_int(stmt, 2, keyid);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);

		if(value != NULL && value[0] != '\0'){
			sqlite3_prepare_v2(darktable.db, "insert into meta_data (id, key, value) values (?1, ?2, ?3)", -1, &stmt, NULL);
			sqlite3_bind_int(stmt, 1, id);
			sqlite3_bind_int(stmt, 2, keyid);
			sqlite3_bind_text(stmt, 3, value, -1, SQLITE_TRANSIENT);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}	
	}
}

static void dt_metadata_set_exif(int id, const char* key, const char* value){} //TODO
static void dt_metadata_set_xmp(int id, const char* key, const char* value){} //TODO? is this needed at all, or is it the same as _dt?

static GList* dt_metadata_get_dt(int id, const char* key, uint32_t* count){
	GList *result = NULL;
	sqlite3_stmt *stmt;
	uint32_t local_count = 0;

	int keyid = dt_metadata_get_keyid(key);

	if(id == -1){
		sqlite3_prepare_v2(darktable.db, "select value from meta_data where id in (select imgid from selected_images) and key = ?1 order by value", -1, &stmt, NULL);
		sqlite3_bind_int(stmt, 1, keyid);
	} else { // single image under mouse cursor
		sqlite3_prepare_v2(darktable.db, "select value from meta_data where id = ?1 and key = ?2 order by value", -1, &stmt, NULL);
		sqlite3_bind_int(stmt, 1, id);
		sqlite3_bind_int(stmt, 2, keyid);
	}
	while(sqlite3_step(stmt) == SQLITE_ROW){
		local_count++;
		result = g_list_append(result, g_strdup((char *)sqlite3_column_text(stmt, 0)));
	}
	sqlite3_finalize(stmt);
	if(count != NULL)
		*count = local_count;
	return result;
}

static GList* dt_metadata_get_exif(int id, const char* key, uint32_t* count){ return NULL; } // TODO
static GList* dt_metadata_get_xmp(int id, const char* key, uint32_t* count){ return NULL; } // TODO? is this needed at all, or is it the same as _dt?

void dt_metadata_set(int id, const char* key, const char* value){
	if(strncmp(key, "darktable.", 10) == 0)
		dt_metadata_set_dt(id, key, value);
	else if(strncmp(key, "Exif.", 5) == 0)
		dt_metadata_set_exif(id, key, value);
	else if(strncmp(key, "Xmp.", 4) == 0)
		dt_metadata_set_xmp(id, key, value);
}

GList* dt_metadata_get(int id, const char* key, uint32_t* count){
	if(strncmp(key, "darktable.", 10) == 0)
		return dt_metadata_get_dt(id, key, count);
	if(strncmp(key, "Exif.", 5) == 0)
		return dt_metadata_get_exif(id, key, count);
	if(strncmp(key, "Xmp.", 4) == 0)
		return dt_metadata_get_xmp(id, key, count);
	return NULL;
}

// TODO: Also clear exif data? I don't think it makes sense.
void dt_metadata_clear(int id){
	if(id == -1)
		sqlite3_exec(darktable.db, "delete from meta_data where id in (select imgid from selected_images)", NULL, NULL, NULL);
	else{
		sqlite3_stmt *stmt;
		sqlite3_prepare_v2(darktable.db, "delete from meta_data where id = ?1", -1, &stmt, NULL);
		sqlite3_bind_int(stmt, 1, id);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
}