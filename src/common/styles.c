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

#include "develop/develop.h"
#include "control/control.h"
#include "common/darktable.h"
#include "common/imageio.h"
#include "common/image_cache.h"
#include "common/styles.h"
#include "common/tags.h"

int32_t _styles_get_id_by_name (const char *name);

gboolean 
dt_styles_exists (const char *name)
{
  return (_styles_get_id_by_name(name))!=0?TRUE:FALSE;
}

void 
dt_styles_create_from_image (const char *name,const char *description,int32_t imgid,GList *filter)
{
  int rc=0,id=0;
  sqlite3_stmt *stmt;  
  
  /* check if name already exists */
  if (_styles_get_id_by_name(name)!=0)
  {
    dt_control_log(_("style with name '%s' already exists"),name);
    return;
  }
  
  /* verify that imgid has an history or bail out */
  
  /* first create the style header */
  sqlite3_prepare_v2 (darktable.db, "insert into styles (name,description) values (?1,?2)", -1, &stmt, NULL);
  rc = sqlite3_bind_text (stmt, 1, name,strlen (name),SQLITE_STATIC);
  rc = sqlite3_bind_text (stmt, 2, description,strlen (description),SQLITE_STATIC);
  rc = sqlite3_step (stmt);
  rc = sqlite3_finalize (stmt);
  
  if ((id=_styles_get_id_by_name(name)) != 0)
  {
    /* create the style_items from source image history stack */
    if (filter)
    {
      GList *list=filter;
      char tmp[64];
      char include[2048]={0};
      strcat(include,"num in (");
      do
      {
        if(list!=g_list_first(list))
          strcat(include,",");
        sprintf(tmp,"%ld",(long int)list->data);
        strcat(include,tmp);
      } while ((list=g_list_next(list)));
      strcat(include,")");
      char query[4096]={0};
      sprintf(query,"insert into style_items (styleid,num,module,operation,op_params,enabled) select ?1, num,module,operation,op_params,enabled from history where imgid=?2 and %s",include);
      sqlite3_prepare_v2 (darktable.db, query, -1, &stmt, NULL);
    } else
      sqlite3_prepare_v2 (darktable.db, "insert into style_items (styleid,num,module,operation,op_params,enabled) select ?1, num,module,operation,op_params,enabled from history where imgid=?2", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, id);
    rc = sqlite3_bind_int (stmt, 2, imgid);
    rc = sqlite3_step (stmt);
    rc = sqlite3_finalize (stmt);
    
    dt_control_log(_("style named '%s' successfully created"),name);
  }
}

void 
dt_styles_apply_to_selection(const char *name)
{
  /* for each selected image apply style */
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
     int imgid = sqlite3_column_int (stmt, 0);
    dt_styles_apply_to_image (name,imgid);
  }
  sqlite3_finalize(stmt);
}

void 
dt_styles_apply_to_image(const char *name,int32_t imgid)
{
  int rc=0,id=0;
  sqlite3_stmt *stmt;  
  
  if ((id=_styles_get_id_by_name(name)) != 0)
  {
     /* if merge onto history stack, lets find history offest in destination image */
    int32_t offs = 0; 
    if (FALSE)
    { 
      /* apply on top of history stack */
      rc = sqlite3_prepare_v2 (darktable.db, "select num from history where imgid = ?1", -1, &stmt, NULL);
      rc = sqlite3_bind_int (stmt, 1, imgid);
      while (sqlite3_step (stmt) == SQLITE_ROW) offs++;
    }
    else
    { 
      /* replace history stack */
      rc = sqlite3_prepare_v2 (darktable.db, "delete from history where imgid = ?1", -1, &stmt, NULL);
      rc = sqlite3_bind_int (stmt, 1, imgid);
      rc = sqlite3_step (stmt);
    }
    rc = sqlite3_finalize (stmt);
    
    /* copy history items from styles onto image */
    sqlite3_prepare_v2 (darktable.db, "insert into history (imgid,num,module,operation,op_params,enabled) select ?1, num+?2,module,operation,op_params,enabled from style_items where styleid=?3", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, imgid);
    rc = sqlite3_bind_int (stmt, 2, offs);
    rc = sqlite3_bind_int (stmt, 3, id);
    rc = sqlite3_step (stmt);
    rc = sqlite3_finalize (stmt);
    
    /* add tag */
    guint tagid=0;
    if (dt_tag_new(name,&tagid))
      dt_tag_attach(tagid,imgid);
    
    /* if current image in develop reload history */
    if (dt_dev_is_current_image(darktable.develop, imgid))
      dt_dev_reload_history_items (darktable.develop);
    
    /* reimport image */
    dt_image_t *img = dt_image_cache_get (imgid, 'r');
    img->force_reimport = 1;
   
    dt_image_cache_flush(img);
  }
}

void 
dt_styles_delete_by_name(const char *name)
{
  int id=0;
  if ((id=_styles_get_id_by_name(name)) != 0)
  {
    /* delete the style */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2 (darktable.db, "delete from styles where styleid = ?1", -1, &stmt, NULL);
    sqlite3_bind_int (stmt, 1, id);
    sqlite3_step (stmt);
    sqlite3_finalize (stmt);
    
    /* delete style_items belonging to style */
    sqlite3_prepare_v2 (darktable.db, "delete from style_items where styleid = ?1", -1, &stmt, NULL);
    sqlite3_bind_int (stmt, 1, id);
    sqlite3_step (stmt);
    sqlite3_finalize (stmt);
    
  }
}

GList *
dt_styles_get_item_list (const char *name)
{
  GList *result=NULL;
  sqlite3_stmt *stmt;
  int id=0,rc=0;
  if ((id=_styles_get_id_by_name(name)) != 0)
  {
    rc = sqlite3_prepare_v2 (darktable.db, "select num, operation, enabled from style_items where styleid=?1 order by num desc", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, id);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
      char name[512]={0};
      dt_style_item_t *item=g_malloc (sizeof (dt_style_item_t));
      item->num = sqlite3_column_int (stmt, 0);
      g_snprintf(name,512,"%s (%s)",sqlite3_column_text (stmt, 1),(sqlite3_column_int (stmt, 2)!=0)?_("on"):_("off"));
      item->name = g_strdup (name);
      result = g_list_append (result,item);
    }
  }
  return result;
}

GList *
dt_styles_get_list (const char *filter)
{
  char filterstring[512]={0};
  sqlite3_stmt *stmt;
  int rc=0;
  sprintf (filterstring,"%%%s%%",filter);
  rc = sqlite3_prepare_v2 (darktable.db, "select name, description from styles where name like ?1 or description like ?1 order by name", -1, &stmt, NULL);
  rc = sqlite3_bind_text (stmt, 1, filterstring, strlen(filterstring), SQLITE_TRANSIENT);
  GList *result = NULL;
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (const char *)sqlite3_column_text (stmt, 0);
    const char *description = (const char *)sqlite3_column_text (stmt, 1);
    dt_style_t *s=g_malloc (sizeof (dt_style_t));
    s->name = g_strdup (name);
    s->description= g_strdup (description);
    result = g_list_append(result,s);
  }
  sqlite3_finalize (stmt);
  return result;
}

gchar *dt_styles_get_description (const char *name)
{
  sqlite3_stmt *stmt;
  int id=0,rc=0;
  gchar *description = NULL;
  if ((id=_styles_get_id_by_name(name)) != 0)
  {
    rc = sqlite3_prepare_v2 (darktable.db, "select description from styles where styleid=?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, id);
    rc = sqlite3_step(stmt);
    description = (char *)sqlite3_column_text (stmt, 0);
    if (description)
      description = g_strdup (description);
    sqlite3_finalize (stmt);
  }
  return description;
}

int32_t 
_styles_get_id_by_name (const char *name)
{
  int id=0;
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "select styleid from styles where name=?1", -1, &stmt, NULL);
  sqlite3_bind_text (stmt, 1, name,strlen (name),SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int (stmt, 0);
  }
  sqlite3_finalize (stmt);
  return id;
}

