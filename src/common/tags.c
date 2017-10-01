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
#include "common/tags.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include <glib.h>
#if defined (_WIN32)
#include "win/getdelim.h"
#endif // defined (_WIN32)

gboolean dt_tag_new(const char *name, guint *tagid)
{
  int rt;
  sqlite3_stmt *stmt;

  if(!name || name[0] == '\0') return FALSE; // no tagid name.

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM data.tags WHERE name = ?1", -1, &stmt,
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

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "INSERT INTO data.tags (id, name) VALUES (NULL, ?1)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if(tagid != NULL)
  {
    *tagid = 0;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM data.tags WHERE name = ?1", -1,
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
                              "SELECT COUNT(*) FROM main.tagged_images WHERE tagid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  rv = sqlite3_step(stmt);
  if(rv == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if(final == TRUE)
  {
    // let's actually remove the tag
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM data.tags WHERE id=?1", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.used_tags WHERE id=?1", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.tagged_images WHERE tagid=?1",
                                -1, &stmt, NULL);
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
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT name FROM data.tags WHERE id= ?1", -1, &stmt,
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
                              "UPDATE data.tags SET name=REPLACE(name,?1,?2) WHERE name LIKE ?3", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, source, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, new_expr, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, source_expr, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.used_tags SET name=REPLACE(name,?1,?2) WHERE name LIKE ?3",
                              -1, &stmt, NULL);
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
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM data.tags WHERE name = ?1", -1, &stmt,
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

// we keep this separate so that updating the gui only happens once (and it's the caller's responsibility)
static void _attach_tag(guint tagid, gint imgid)
{
  sqlite3_stmt *stmt;
  if(imgid > 0)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT OR REPLACE INTO main.tagged_images (imgid, tagid) VALUES (?1, ?2)", -1,
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
                                "INSERT OR REPLACE INTO main.tagged_images SELECT imgid, ?1 "
                                "FROM main.selected_images",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

void dt_tag_attach(guint tagid, gint imgid)
{
  _attach_tag(tagid, imgid);

  dt_tag_update_used_tags();

  dt_collection_update_query(darktable.collection);
}

void dt_tag_attach_list(GList *tags, gint imgid)
{
  GList *child = NULL;
  if((child = g_list_first(tags)) != NULL) do
    {
      _attach_tag(GPOINTER_TO_INT(child->data), imgid);
    } while((child = g_list_next(child)) != NULL);

  dt_tag_update_used_tags();

  dt_collection_update_query(darktable.collection);
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
        _attach_tag(tagid, imgid);
      }
      entry++;
    }

    dt_tag_update_used_tags();

    dt_collection_update_query(darktable.collection);
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
                                "DELETE FROM main.tagged_images WHERE tagid = ?1 AND imgid = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else
  {
    // remove from tagged_images
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM main.tagged_images WHERE tagid = ?1 AND imgid IN "
                                "(SELECT imgid FROM main.selected_images)",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  dt_tag_update_used_tags();

  dt_collection_update_query(darktable.collection);
}

void dt_tag_detach_by_string(const char *name, gint imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.tagged_images WHERE tagid IN (SELECT id FROM "
                              "data.tags WHERE name LIKE ?1) AND imgid = ?2;",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  dt_tag_update_used_tags();

  dt_collection_update_query(darktable.collection);
}


uint32_t dt_tag_get_attached(gint imgid, GList **result, gboolean ignore_dt_tags)
{
  sqlite3_stmt *stmt;
  if(imgid > 0)
  {
    char query[1024] = { 0 };
    snprintf(query, sizeof(query), "SELECT DISTINCT T.id, T.name FROM main.tagged_images AS I "
                                   "JOIN data.tags T on T.id = I.tagid "
                                   "WHERE I.imgid = %d %s ORDER BY T.name",
             imgid, ignore_dt_tags ? "AND NOT T.name LIKE \"darktable|%\"" : "");
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  }
  else
  {
    if(ignore_dt_tags)
      DT_DEBUG_SQLITE3_PREPARE_V2(
          dt_database_get(darktable.db),
          "SELECT DISTINCT T.id, T.name "
          "FROM main.tagged_images AS I, data.tags AS T "
          "WHERE I.imgid IN (SELECT imgid FROM main.selected_images) "
          "AND T.id = I.tagid AND NOT T.name LIKE \"darktable|%\" ORDER BY T.name",
          -1, &stmt, NULL);
    else
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT DISTINCT T.id, T.name "
                                  "FROM main.tagged_images AS I, data.tags AS T "
                                  "WHERE I.imgid IN (SELECT imgid FROM main.selected_images) "
                                  "AND T.id = I.tagid ORDER BY T.name",
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

  gboolean omit_tag_hierarchy = dt_conf_get_bool("omit_tag_hierarchy");

  uint32_t count = dt_tag_get_attached(imgid, &taglist, TRUE);

  if(count < 1) return NULL;

  for(; taglist; taglist = g_list_next(taglist))
  {
    dt_tag_t *t = (dt_tag_t *)taglist->data;
    gchar *value = t->tag;

    size_t j = 0;
    gchar **pch = g_strsplit(value, "|", -1);

    if(pch != NULL)
    {
      if(omit_tag_hierarchy)
      {
        char **iter = pch;
        for(; *iter && *(iter + 1); iter++);
        if(*iter) tags = g_list_prepend(tags, g_strdup(*iter));
      }
      else
      {
        while(pch[j] != NULL)
        {
          tags = g_list_prepend(tags, g_strdup(pch[j]));
          j++;
        }
      }
      g_strfreev(pch);
    }
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

GList *dt_tag_get_images_from_selection(gint imgid, gint tagid)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;

  if(imgid > 0)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.tagged_images WHERE "
                                "imgid = ?1 AND tagid = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, tagid);
  }
  else
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.tagged_images WHERE "
                                "tagid = ?1 AND imgid IN (SELECT imgid FROM main.selected_images)", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  }


  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
    result = g_list_append(result, GINT_TO_POINTER(id));
  }

  sqlite3_finalize(stmt);

  return result;
}

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
  if(!keyword) return 0;

  gchar *keyword_expr = g_strdup_printf("%%%s%%", keyword);

  /* Only select tags that are similar to the one we are looking for once. */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO memory.similar_tags (tagid) SELECT id FROM data.tags WHERE name LIKE ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, keyword_expr, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  g_free(keyword_expr);

  /* Select tags that are similar to the keyword and are actually used to tag images*/
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO memory.taglist (id, count) SELECT tagid, 1000000+COUNT(*) "
                              "FROM main.tagged_images "
                              "WHERE tagid IN memory.similar_tags GROUP BY tagid ",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  /* Select tags that are similar to the keyword but were not used to tag any image*/
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO memory.taglist (id, count) SELECT tagid,1000000 FROM memory.similar_tags",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  /* Select tags from tagged images when at least one tag is similar to the keyword and insert in temp table*/
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO memory.tagq (id) SELECT tagid FROM main.tagged_images WHERE imgid IN "
                              "(SELECT DISTINCT imgid FROM main.tagged_images JOIN memory.similar_tags USING (tagid)) ",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  /* Select tags from temp table that are not similar to the keyword */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "INSERT INTO memory.taglist (id, count) SELECT id, "
                                                       "COUNT(*) FROM memory.tagq WHERE id NOT IN (SELECT id FROM "
                                                       "memory.taglist) GROUP BY id", NULL, NULL, NULL);

  /* Now put all the bits together */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT T.name, T.id FROM data.tags T "
                              "JOIN memory.taglist MT ON MT.id = T.id "
                              "WHERE T.id IN (SELECT DISTINCT(MT.id) FROM memory.taglist MT) "
                              "AND T.name NOT LIKE 'darktable|%%' "
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
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.taglist", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.tagq", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.similar_tags", NULL, NULL, NULL);

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

/*
  TODO
  the file format allows to specify {synonyms} that are one hierarchy level deeper than the parent. those are not
  to be shown in the gui but can be searched. when the parent or a synonym is attached then ALSO the rest of the
  bunch is to be added. currently dt doesn't allow something like that but it would be really great if it could
  be added. currently we don't import synonyms.
  there is also a ~ prefix for tags that indicate that the tag order has to be kept instead of sorting them. that's
  also not possible at the moment.
*/
ssize_t dt_tag_import(const char *filename)
{
  FILE *fd = g_fopen(filename, "r");

  if(!fd) return -1;

  GList * hierarchy = NULL;
  char *line = NULL;
  size_t len = 0;
  ssize_t count = 0;

  while(getline(&line, &len, fd) != -1)
  {
    // remove newlines and set start past the initial tabs
    char *start = line;
    while(*start == '\t') start++;
    const int depth = start - line;

    char *end = line + strlen(line) - 1;
    while((*end == '\n' || *end == '\r') && end >= start)
    {
      *end = '\0';
      end--;
    }

    // remove control characters from the string
    // don't add the entry if it's a category
    // TODO also ignore synonyms for now as our db can't express that concept.
    gboolean skip = FALSE;
    if((*start == '[' && *end == ']') // categories
      || (*start == '{' && *end == '}')) // synonyms
    {
      skip = TRUE;
      start++;
      *end-- = '\0';
    }
    if(*start == '~') // fixed order. TODO not possible with our db
    {
      skip = TRUE;
      start++;
    }

    // remove everything past the current prefix from hierarchy
    GList *iter = g_list_nth(hierarchy, depth);
    while(iter)
    {
      GList *current = iter;
      iter = g_list_next(iter);
      hierarchy = g_list_delete_link(hierarchy, current);
    }

    // add the current level
    hierarchy = g_list_append(hierarchy, g_strdup(start));

    // add tag to db iff it's not something to be ignored
    if(!skip)
    {
      count++;
      char *tag = dt_util_glist_to_str("|", hierarchy);
      dt_tag_new(tag, NULL);
      g_free(tag);
    }
  }

  free(line);
  g_list_free_full(hierarchy, g_free);
  fclose(fd);

  dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);

  return count;
}

/*
  TODO: there is one corner case where i am not sure if we are doing the correct thing. some examples i found
  on the internet agreed with this version, some used an alternative:
  consider two tags like "foo|bar" and "foo|bar|baz". the "foo|bar" part is both a regular tag (from the 1st tag)
  and also a category (from the 2nd tag). the two way to output are

  [foo]
      bar
          baz

  and

  [foo]
      bar
      [bar]
          baz

  we are using the first (mostly because it was easier to implement ;)). if this poses problems with other programs
  supporting these files then we should fix that.
*/
ssize_t dt_tag_export(const char *filename)
{
  FILE *fd = g_fopen(filename, "w");

  if(!fd) return -1;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name FROM data.tags WHERE name NOT LIKE \"darktable|%\" "
                              "ORDER BY name COLLATE NOCASE ASC", -1, &stmt, NULL);


  ssize_t count = 0;
  gchar **hierarchy = NULL;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *tag = (char *)sqlite3_column_text(stmt, 0);

    gchar **tokens = g_strsplit(tag, "|", -1);

    // find how many common levels are shared with the last tag
    int common_start;
    for(common_start = 0; hierarchy && hierarchy[common_start] && tokens && tokens[common_start]; common_start++)
    {
      if(g_strcmp0(hierarchy[common_start], tokens[common_start])) break;
    }

    g_strfreev(hierarchy);
    hierarchy = tokens;

    int tabs = common_start;
    for(size_t i = common_start; tokens && tokens[i]; i++, tabs++)
    {
      for(int j = 0; j < tabs; j++) fputc('\t', fd);
      if(!tokens[i + 1])
      {
        count++;
        fprintf(fd, "%s\n", tokens[i]);
      }
      else
        fprintf(fd, "[%s]\n", tokens[i]);
    }
  }

  g_strfreev(hierarchy);

  sqlite3_finalize(stmt);

  fclose(fd);

  return count;
}

void dt_tag_update_used_tags()
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.used_tags WHERE id NOT IN "
                                                       "(SELECT tagid FROM main.tagged_images GROUP BY tagid)",
                        NULL, NULL, NULL);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "INSERT OR IGNORE INTO main.used_tags (id, name) "
                                                       "SELECT t.id, t.name "
                                                       "FROM data.tags AS t, main.tagged_images AS i "
                                                       "ON t.id = i.tagid GROUP BY t.id",
                        NULL, NULL, NULL);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
