/*
    This file is part of darktable,
    copyright (c) 2010--2011 henrik andersson.
    copyright (c) 2012 James C. McPherson

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
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"

gboolean dt_tag_new(const char *name, guint *tagid)
{
  int rt;
  sqlite3_stmt *stmt;

  if(!name || name[0] == '\0') return FALSE; // no tagid name.

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM tags WHERE name = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  rt = sqlite3_step(stmt);
  if(rt == SQLITE_ROW)
  {
    // tagid already exists.
    if(tagid != NULL) *tagid = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return TRUE;
  }
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "INSERT INTO tags (id, name) VALUES (null, ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if(tagid != NULL)
  {
    *tagid = 0;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM tags WHERE name = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(stmt) == SQLITE_ROW) *tagid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }

  return TRUE;
}

gboolean dt_tag_new_from_gui(const char *name, guint *tagid)
{
  gboolean ret = dt_tag_new(name, tagid);
  /* if everything went fine, raise signal of tags change to refresh keywords module in GUI */
  if(ret) dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  return ret;
}

guint dt_tag_remove(const guint tagid, gboolean final)
{
  int rv, count = -1;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT count() FROM tagged_images WHERE tagid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  rv = sqlite3_step(stmt);
  if(rv == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if(final == TRUE)
  {
    // let's actually remove the tag
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM tags WHERE id=?1", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* raise signal of tags change to refresh keywords module */
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  }

  return count;
}

gchar *dt_tag_get_name(const guint tagid)
{
  int rt;
  char *name = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT name FROM tags WHERE id= ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  rt = sqlite3_step(stmt);
  if(rt == SQLITE_ROW) name = g_strdup((const char *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);

  return name;
}

void dt_tag_reorganize(const gchar *source, const gchar *dest)
{
  sqlite3_stmt *stmt;

  if(!strcmp(source, dest)) return;

  gchar *tag = g_strrstr(source, "|");

  if(!tag) tag = g_strconcat("|", source, NULL);

  if(!strcmp(dest, " "))
  {
    tag++;
    dest++;
  }

  gchar *new_expr = g_strconcat(dest, tag, NULL);
  gchar *source_expr = g_strconcat(source, "%", NULL);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE tags SET name=REPLACE(name,?1,?2) WHERE name LIKE ?3", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, source, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, new_expr, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, source_expr, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  g_free(source_expr);
  g_free(new_expr);

  /* raise signal of tags change to refresh keywords module */
  // dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
}

gboolean dt_tag_exists(const char *name, guint *tagid)
{
  int rt;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM tags WHERE name = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  rt = sqlite3_step(stmt);

  if(rt == SQLITE_ROW)
  {
    if(tagid != NULL) *tagid = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return TRUE;
  }

  *tagid = -1;
  sqlite3_finalize(stmt);
  return FALSE;
}

// FIXME: shall we increment count in tagxtag if the image was already tagged?
void dt_tag_attach(guint tagid, gint imgid)
{
  sqlite3_stmt *stmt;
  if(imgid > 0)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT OR REPLACE INTO tagged_images (imgid, tagid) VALUES (?1, ?2)", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else
  {
    // insert into tagged_images if not there already.
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT OR REPLACE INTO tagged_images SELECT imgid, ?1 "
                                "FROM selected_images",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

void dt_tag_attach_list(GList *tags, gint imgid)
{
  GList *child = NULL;
  if((child = g_list_first(tags)) != NULL) do
    {
      dt_tag_attach(GPOINTER_TO_INT(child->data), imgid);
    } while((child = g_list_next(child)) != NULL);
}

void dt_tag_attach_string_list(const gchar *tags, gint imgid)
{
  gchar **tokens = g_strsplit(tags, ",", 0);
  if(tokens)
  {
    gchar **entry = tokens;
    while(*entry)
    {
      // remove leading and trailing spaces
      char *e = *entry + strlen(*entry) - 1;
      while(*e == ' ' && e > *entry) *e = '\0';
      e = *entry;
      while(*e == ' ' && *e != '\0') e++;
      if(*e)
      {
        // add the tag to the image
        guint tagid = 0;
        dt_tag_new(e, &tagid);
        dt_tag_attach(tagid, imgid);
      }
      entry++;
    }
  }
  g_strfreev(tokens);
}

void dt_tag_detach(guint tagid, gint imgid)
{
  sqlite3_stmt *stmt;
  if(imgid > 0)
  {
    // remove from tagged_images
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM tagged_images WHERE tagid = ?1 AND imgid = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else
  {
    // remove from tagged_images
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "delete from tagged_images where tagid = ?1 and imgid in "
                                "(select imgid from selected_images)",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

void dt_tag_detach_by_string(const char *name, gint imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM tagged_images WHERE tagid IN (SELECT id FROM "
                              "tags WHERE name LIKE ?1) AND imgid = ?2;",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}


uint32_t dt_tag_get_attached(gint imgid, GList **result, gboolean ignore_dt_tags)
{
  sqlite3_stmt *stmt;
  if(imgid > 0)
  {
    char query[1024] = { 0 };
    snprintf(query, sizeof(query), "SELECT DISTINCT T.id, T.name FROM tagged_images "
                                   "JOIN tags T on T.id = tagged_images.tagid "
                                   "WHERE tagged_images.imgid = %d %s ORDER BY T.name",
             imgid, ignore_dt_tags ? "AND NOT T.name LIKE \"darktable|%\"" : "");
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  }
  else
  {
    if(ignore_dt_tags)
      DT_DEBUG_SQLITE3_PREPARE_V2(
          dt_database_get(darktable.db),
          "SELECT DISTINCT T.id, T.name "
          "FROM tagged_images,tags as T "
          "WHERE tagged_images.imgid in (select imgid from selected_images)"
          "  AND T.id = tagged_images.tagid AND NOT T.name LIKE \"darktable|%\" ORDER BY T.name",
          -1, &stmt, NULL);
    else
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT DISTINCT T.id, T.name "
                                  "FROM tagged_images,tags as T "
                                  "WHERE tagged_images.imgid in (select imgid from selected_images)"
                                  "  AND T.id = tagged_images.tagid ORDER BY T.name",
                                  -1, &stmt, NULL);
  }

  // Create result
  uint32_t count = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_tag_t *t = g_malloc(sizeof(dt_tag_t));
    t->id = sqlite3_column_int(stmt, 0);
    t->tag = g_strdup((char *)sqlite3_column_text(stmt, 1));
    *result = g_list_append(*result, t);
    count++;
  }
  sqlite3_finalize(stmt);
  return count;
}

GList *dt_tag_get_list(gint imgid)
{
  GList *taglist = NULL;
  GList *tags = NULL;

  uint32_t count = dt_tag_get_attached(imgid, &taglist, TRUE);

  if(count < 1) return NULL;

  while(taglist)
  {
    dt_tag_t *t = (dt_tag_t *)taglist->data;
    gchar *value = t->tag;

    size_t j = 0;
    gchar **pch = g_strsplit(value, "|", -1);

    if(pch != NULL)
    {
      while(pch[j] != NULL)
      {
        tags = g_list_prepend(tags, g_strdup(pch[j]));
        j++;
      }
      g_strfreev(pch);
    }

    taglist = g_list_next(taglist);
  }

  g_list_free_full(taglist, g_free);

  return dt_util_glist_uniq(tags);
}

GList *dt_tag_get_hierarchical(gint imgid)
{
  GList *taglist = NULL;
  GList *tags = NULL;

  int count = dt_tag_get_attached(imgid, &taglist, TRUE);

  if(count < 1) return NULL;

  while(taglist)
  {
    dt_tag_t *t = (dt_tag_t *)taglist->data;

    tags = g_list_prepend(tags, t->tag);

    taglist = g_list_next(taglist);
  }

  g_list_free_full(taglist, g_free);

  tags = g_list_reverse(tags);
  return tags;
}

/*
 * dt_tag_get_suggestions() takes a string (keyword) and searches the
 * tagxtags table for possibly-related tags. The list we construct at
 * the end of the function is made up as follows:
 *
 * * Tags which appear as tagxtag.id2, where (keyword's name = tagxtag.id1)
 *   are listed first, ordered by count of times seen already.
 * * Tags which appear as tagxtag.id1, where (keyword's name = tagxtag.id2)
 *   are listed second, ordered as before.
 *
 * We do not suggest tags which have not yet been matched up in tagxtag,
 * because it is up to the user to add new tags to the list and thereby
 * make the association.
 *
 * Expressing these as separate queries avoids making the sqlite3 engine
 * do a large number of operations and thus makes the user experience
 * snappy.
 *
 * SELECT T.id FROM tags T WHERE T.name LIKE '?1';  --> into temp table
 * SELECT TXT.id2 FROM tagxtag TXT WHERE TXT.id1 IN (temp table)
 *   AND TXT.count > 0 ORDER BY TXT.count DESC;
 * SELECT TXT.id1 FROM tagxtag TXT WHERE TXT.id2 IN (temp table)
 *   AND TXT.count > 0 ORDER BY TXT.count DESC;
 *
 * SELECT DISTINCT(T.name) FROM tags T JOIN memoryquery MQ on MQ.id = T.id;
 *
 */
uint32_t dt_tag_get_suggestions(const gchar *keyword, GList **result)
{
  sqlite3_stmt *stmt;
  /*
   * Earlier versions of this function used a large collation of selects
   * and joins, resulting in multi-*second* timings for sqlite3_exec().
   *
   * Breaking the query into several smaller ones allows the sqlite3
   * execution engine to work more effectively, which is very important
   * for interactive response since we call this function several times
   * in quick succession (on every keystroke).
   */

  /* Quick sanity check - is keyword empty? If so .. return 0 */
  if(keyword == 0) return 0;

  gchar *keyword_expr = g_strdup_printf("%%%s%%", keyword);

  /* SELECT T.id FROM tags T WHERE T.name LIKE '%%%s%%';  --> into temp table */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO memory.tagq (id) SELECT id FROM tags T WHERE "
                              "T.name LIKE ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, keyword_expr, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  g_free(keyword_expr);

  /*
   * SELECT TXT.id2 FROM tagxtag TXT WHERE TXT.id1 IN (temp table)
   *   AND TXT.count > 0 ORDER BY TXT.count DESC;
   */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "INSERT INTO memory.taglist (id, count) "
                                                       "SELECT DISTINCT(TXT.id2), TXT.count FROM tagxtag TXT "
                                                       "WHERE TXT.count > 0 "
                                                       " AND TXT.id1 IN (SELECT id FROM memory.tagq) "
                                                       "ORDER BY TXT.count DESC",
                        NULL, NULL, NULL);

  /*
   * SELECT TXT.id1 FROM tagxtag TXT WHERE TXT.id2 IN (temp table)
   *   AND TXT.count > 0 ORDER BY TXT.count DESC;
   */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "INSERT OR REPLACE INTO memory.taglist (id, count) "
                                                       "SELECT DISTINCT(TXT.id1), TXT.count FROM tagxtag TXT "
                                                       "WHERE TXT.count > 0 "
                                                       " AND TXT.id2 IN (SELECT id FROM memory.tagq) "
                                                       "ORDER BY TXT.count DESC",
                        NULL, NULL, NULL);

  /* Now put all the bits together */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT T.name, T.id FROM tags T "
                              "JOIN memory.taglist MT ON MT.id = T.id "
                              "WHERE T.id IN (SELECT DISTINCT(MT.id) FROM memory.taglist MT) "
                              "  AND T.name NOT LIKE 'darktable|%%' "
                              "ORDER BY MT.count DESC",
                              -1, &stmt, NULL);

  /* ... and create the result list to send upwards */
  uint32_t count = 0;
  dt_tag_t *t;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    t = g_malloc(sizeof(dt_tag_t));
    t->tag = g_strdup((char *)sqlite3_column_text(stmt, 0));
    t->id = sqlite3_column_int(stmt, 1);
    *result = g_list_append((*result), t);
    count++;
  }

  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE from memory.taglist", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE from memory.tagq", NULL, NULL, NULL);

  return count;
}

static void _free_result_item(dt_tag_t *t, gpointer unused)
{
  g_free(t->tag);
  g_free(t);
}

void dt_tag_free_result(GList **result)
{
  if(result && *result)
  {
    g_list_free_full(*result, (GDestroyNotify)_free_result_item);
  }
}

uint32_t dt_tag_get_recent_used(GList **result)
{
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
