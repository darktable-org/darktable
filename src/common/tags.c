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

#include "common/darktable.h"
#include "common/tags.h"
#include "control/conf.h"

gboolean dt_tag_new(const char *name,guint *tagid)
{
  int rc, rt;
  guint id;
  sqlite3_stmt *stmt;
  
  if(!name || name[0] == '\0') return FALSE; // no tagid name.
  
  rc = sqlite3_prepare_v2(darktable.db, "select id from tags where name = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_text(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  rt = sqlite3_step(stmt);
  if(rt == SQLITE_ROW) 
  { // tagid already exists.
    if( tagid != NULL)
      *tagid=sqlite3_column_int64(stmt, 0);
    rc = sqlite3_finalize(stmt);
    return  TRUE; 
  }
  rc = sqlite3_finalize(stmt);
  
  
  
  rc = sqlite3_prepare_v2(darktable.db, "insert into tags (id, name) values (null, ?1)", -1, &stmt, NULL);
  rc = sqlite3_bind_text(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  pthread_mutex_lock(&(darktable.db_insert));
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  id = sqlite3_last_insert_rowid(darktable.db);
  pthread_mutex_unlock(&(darktable.db_insert));
  rc = sqlite3_prepare_v2(darktable.db, "insert into tagxtag select id, ?1, 0 from tags", -1, &stmt, NULL);
  rc = sqlite3_bind_int(stmt, 1, id);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  rc = sqlite3_prepare_v2(darktable.db, "update tagxtag set count = 1000000 where id1 = ?1 and id2 = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int(stmt, 1, id);
  rc = sqlite3_step(stmt);
  rc = sqlite3_finalize(stmt);
  
  if( tagid != NULL)
      *tagid=id;
    
  return TRUE;
}

guint dt_tag_remove(const guint tagid, gboolean final)
{
  int rc,rt,count=-1;
  sqlite3_stmt *stmt;
 
  rc = sqlite3_prepare_v2(darktable.db, "select count() from tagged_images where tagid=?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int(stmt, 1, tagid);
  rt = sqlite3_step(stmt);
  if( rt == SQLITE_ROW)
    count = sqlite3_column_int(stmt,0);
  rc = sqlite3_finalize(stmt);
  
  if( final == TRUE )
  { // let's actually remove the tag
    rc = sqlite3_prepare_v2(darktable.db, "delete from tags where id=?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tagid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "delete from tagxtag where id1=?1 or id2=?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tagid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "delete from tagged_images where tagid=?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tagid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
  }
  
  return count;
}

const gchar *dt_tag_get_name(const guint tagid) {
  int rc, rt;
  char *name=NULL;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "select name from tags where id= ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int(stmt, 1,tagid);
  rt = sqlite3_step(stmt);
  if( rt== SQLITE_ROW )
    name=g_strdup((const char *)sqlite3_column_text(stmt, 0));
  rc = sqlite3_finalize(stmt);
  
  return name;
}

gboolean dt_tag_exists(const char *name,guint *tagid)
{
  return FALSE;
}

void dt_tag_attach(guint tagid,gint imgid)
{
  int rc;
  sqlite3_stmt *stmt;
  if(imgid > 0)
  {
    rc = sqlite3_prepare_v2(darktable.db, "insert or replace into tagged_images (imgid, tagid) values (?1, ?2)", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, imgid);
    rc = sqlite3_bind_int(stmt, 2, tagid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(darktable.db, "update tagxtag set count = count + 1 where "
        "(id1 = ?1 and id2 in (select tagid from tagged_images where imgid = ?2)) or "
        "(id2 = ?1 and id1 in (select tagid from tagged_images where imgid = ?2))", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tagid);
    rc = sqlite3_bind_int(stmt, 2, imgid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
  }
  else
  { // insert into tagged_images if not there already.
    rc = sqlite3_prepare_v2(darktable.db, "insert or replace into tagged_images select imgid, ?1 from selected_images", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tagid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(darktable.db, "update tagxtag set count = count + 1 where "
        "(id1 = ?1 and id2 in (select tagid from selected_images join tagged_images)) or "
        "(id2 = ?1 and id1 in (select tagid from selected_images join tagged_images))", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tagid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
  }
}

void dt_tag_attach_list(GList *tags,gint imgid)
{
  GList *child=NULL;
  if( (child=g_list_first(tags))!=NULL )
    do 
    {
      dt_tag_attach((guint)child->data,imgid);
    } while( (child=g_list_next(child)) !=NULL);
}

void dt_tag_detach(guint tagid,gint imgid)
{
  int rc;
  sqlite3_stmt *stmt;
  if(imgid > 0)
  { // remove from specified image by id
    rc = sqlite3_prepare_v2(darktable.db, "update tagxtag set count = count - 1 where "
        "(id1 = ?1 and id2 in (select tagid from tagged_images where imgid = ?2)) or "
        "(id2 = ?1 and id1 in (select tagid from tagged_images where imgid = ?2))", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tagid);
    rc = sqlite3_bind_int(stmt, 2, imgid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);

    // remove from tagged_images
    rc = sqlite3_prepare_v2(darktable.db, "delete from tagged_images where tagid = ?1 and imgid = ?2", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tagid);
    rc = sqlite3_bind_int(stmt, 2, imgid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
  }
  else
  { // remove from all selected images
    rc = sqlite3_prepare_v2(darktable.db, "update tagxtag set count = count - 1 where "
        "(id1 = ?1 and id2 in (select tagid from selected_images join tagged_images)) or "
        "(id2 = ?1 and id1 in (select tagid from selected_images join tagged_images))", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tagid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);

    // remove from tagged_images
    rc = sqlite3_prepare_v2(darktable.db, "delete from tagged_images where tagid = ?1 and imgid in (select imgid from selected_images)", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, tagid);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
  }
}

uint32_t dt_tag_get_attached(guint imgid,GList **result)
{
  int rc;
  sqlite3_stmt *stmt; 
  if(imgid > 0)
  {
    char query[1024];
    snprintf(query, 1024, "select distinct tags.id, tags.name from tagged_images "
        "join tags on tags.id = tagged_images.tagid where tagged_images.imgid = %d", imgid);
    rc = sqlite3_prepare_v2(darktable.db, query, -1, &stmt, NULL);
  }
  else
  {
    rc = sqlite3_prepare_v2(darktable.db, "select distinct tags.id, tags.name from selected_images join tagged_images "
        "on selected_images.imgid = tagged_images.imgid join tags on tags.id = tagged_images.tagid", -1, &stmt, NULL);
  }
  
  // Create result
  uint32_t count=0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_tag_t *t=g_malloc(sizeof(dt_tag_t));
    t->id = sqlite3_column_int(stmt, 0);
    t->tag = g_strdup((char *)sqlite3_column_text(stmt, 1));
    *result=g_list_append(*result,t);
    count++;
  }
  rc = sqlite3_finalize(stmt);
  return count;
}

uint32_t dt_tag_get_suggestions(const gchar *keyword, GList **result)
{
  return 0;
}

uint32_t dt_tag_get_recent_used(GList **result)
{
  return 0;
}
