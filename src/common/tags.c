/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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
#include "common/undo.h"
#include "common/grouping.h"
#include "control/conf.h"
#include "control/control.h"
#include <glib.h>
#if defined (_WIN32)
#include "win/getdelim.h"
#endif // defined (_WIN32)

typedef struct dt_undo_tags_t
{
  int imgid;
  GList *before; // list of tagid before
  GList *after; // list of tagid after
} dt_undo_tags_t;

static void dt_set_darktable_tags();

static gchar *_get_tb_removed_tag_string_values(GList *before, GList *after)
{
  GList *b = before;
  GList *a = after;
  gchar *tag_list = NULL;
  while(b)
  {
    if(!g_list_find(a, b->data))
    {
      tag_list = dt_util_dstrcat(tag_list, "%d,", GPOINTER_TO_INT(b->data));
    }
    b = g_list_next(b);
  }
  if(tag_list) tag_list[strlen(tag_list) - 1] = '\0';
  return tag_list;
}

static gchar *_get_tb_added_tag_string_values(const int img, GList *before, GList *after)
{
  GList *b = before;
  GList *a = after;
  gchar *tag_list = NULL;
  while(a)
  {
    if(!g_list_find(b, a->data))
    {
      tag_list = dt_util_dstrcat(tag_list,
                                 "(%d,%d,"
                                 "  (SELECT (IFNULL(MAX(position),0) & 0xFFFFFFFF00000000) + (1 << 32)"
                                 "    FROM main.tagged_images)"
                                 "),",
                                 GPOINTER_TO_INT(img),
                                 GPOINTER_TO_INT(a->data));
    }
    a = g_list_next(a);
  }
  if(tag_list) tag_list[strlen(tag_list) - 1] = '\0';
  return tag_list;
}

static void _bulk_remove_tags(const int img, const gchar *tag_list)
{
  if(img > 0 && tag_list)
  {
    char *query = NULL;
    sqlite3_stmt *stmt;
    query = dt_util_dstrcat(query, "DELETE FROM main.tagged_images WHERE imgid = %d AND tagid IN (%s)", img, tag_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_free(query);
  }
}

static void _bulk_add_tags(const gchar *tag_list)
{
  if(tag_list)
  {
    char *query = NULL;
    sqlite3_stmt *stmt;
    query = dt_util_dstrcat(query, "INSERT INTO main.tagged_images (imgid, tagid, position) VALUES %s", tag_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_free(query);
  }
}

static void _pop_undo_execute(const int imgid, GList *before, GList *after)
{
  gchar *tobe_removed_list = _get_tb_removed_tag_string_values(before, after);
  gchar *tobe_added_list = _get_tb_added_tag_string_values(imgid, before, after);

  _bulk_remove_tags(imgid, tobe_removed_list);
  _bulk_add_tags(tobe_added_list);

  g_free(tobe_removed_list);
  g_free(tobe_added_list);
}

static void _pop_undo(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action, GList **imgs)
{
  if(type == DT_UNDO_TAGS)
  {
    GList *list = (GList *)data;

    while(list)
    {
      dt_undo_tags_t *undotags = (dt_undo_tags_t *)list->data;

      GList *before = (action == DT_ACTION_UNDO) ? undotags->after : undotags->before;
      GList *after = (action == DT_ACTION_UNDO) ? undotags->before : undotags->after;
      _pop_undo_execute(undotags->imgid, before, after);
      *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(undotags->imgid));
      list = g_list_next(list);
    }

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  }
}

static void _undo_tags_free(gpointer data)
{
  dt_undo_tags_t *undotags = (dt_undo_tags_t *)data;
  g_list_free(undotags->before);
  g_list_free(undotags->after);
  g_free(undotags);
}

static void _tags_undo_data_free(gpointer data)
{
  GList *l = (GList *)data;
  g_list_free_full(l, _undo_tags_free);
}

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

  if(g_strstr_len(name, -1, "darktable|") == name)
  {
    // clear darktable tags list to make sure the new tag will be added by next call to dt_set_darktable_tags()
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.darktable_tags", NULL, NULL, NULL);
  }

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
  const gboolean ret = dt_tag_new(name, tagid);
  /* if everything went fine, raise signal of tags change to refresh keywords module in GUI */
  if(ret) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
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

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.tagged_images WHERE tagid=?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  return count;
}

void dt_tag_delete_tag_batch(const char *flatlist)
{
  sqlite3_stmt *stmt;

  char *query = NULL;
  query = dt_util_dstrcat(query, "DELETE FROM data.tags WHERE id IN (%s)", flatlist);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  g_free(query);

  query = NULL;
  query = dt_util_dstrcat(query, "DELETE FROM main.tagged_images WHERE tagid IN (%s)", flatlist);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  g_free(query);
}

guint dt_tag_remove_list(GList *tag_list)
{
  if (!tag_list) return 0;

  char *flatlist = NULL;
  guint count = 0;
  guint tcount = 0;
  for (GList *taglist = tag_list; taglist ; taglist = g_list_next(taglist))
  {
    const guint tagid = ((dt_tag_t *)taglist->data)->id;
    flatlist = dt_util_dstrcat(flatlist, "%u,", tagid);
    count++;
    if(flatlist && count > 1000)
    {
      flatlist[strlen(flatlist)-1] = '\0';
      dt_tag_delete_tag_batch(flatlist);
      g_free(flatlist);
      flatlist = NULL;
      tcount = tcount + count;
      count = 0;
    }
  }
  if(flatlist)
  {
    flatlist[strlen(flatlist)-1] = '\0';
    dt_tag_delete_tag_batch(flatlist);
    g_free(flatlist);
    tcount = tcount + count;
  }
  return tcount;
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

void dt_tag_rename(const guint tagid, const gchar *new_tagname)
{
  sqlite3_stmt *stmt;

  if(!new_tagname || !new_tagname[0]) return;
  if(dt_tag_exists(new_tagname, NULL)) return;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.tags SET name = ?2 WHERE id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, new_tagname, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

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

  if(tagid != NULL) *tagid = -1;
  sqlite3_finalize(stmt);
  return FALSE;
}

static gboolean _tag_add_tags_to_list(GList **list, const GList *tags)
{
  const GList *t = tags;
  gboolean res = FALSE;
  while(t)
  {
    if(!g_list_find(*list, t->data))
    {
      *list = g_list_prepend(*list, t->data);
      res = TRUE;
    }
    t = g_list_next(t);
  }
  return res;
}

static gboolean _tag_remove_tags_from_list(GList **list, const GList *tags)
{
  const GList *t = tags;
  const int nb_ini = g_list_length(*list);
  while(t)
  {
    *list = g_list_remove(*list, t->data);
    t = g_list_next(t);
  }
  return (g_list_length(*list) != nb_ini);
}

typedef enum dt_tag_type_t
{
  DT_TAG_TYPE_DT,
  DT_TAG_TYPE_USER,
  DT_TAG_TYPE_ALL,
} dt_tag_type_t;

typedef enum dt_tag_actions_t
{
  DT_TA_ATTACH = 0,
  DT_TA_DETACH,
  DT_TA_SET,
  DT_TA_SET_ALL,
} dt_tag_actions_t;

static GList *_tag_get_tags(const gint imgid, const dt_tag_type_t type);

static gboolean _tag_execute(const GList *tags, const GList *imgs, GList **undo, const gboolean undo_on,
                             const gint action)
{
  const GList *images = imgs;
  gboolean res = FALSE;
  while(images)
  {
    const int image_id = GPOINTER_TO_INT(images->data);
    dt_undo_tags_t *undotags = (dt_undo_tags_t *)malloc(sizeof(dt_undo_tags_t));
    undotags->imgid = image_id;
    undotags->before = _tag_get_tags(image_id, DT_TAG_TYPE_ALL);
    switch(action)
    {
      case DT_TA_ATTACH:
        undotags->after = g_list_copy(undotags->before);
        if(_tag_add_tags_to_list(&undotags->after, tags)) res = TRUE;
        break;
      case DT_TA_DETACH:
        undotags->after = g_list_copy(undotags->before);
        if(_tag_remove_tags_from_list(&undotags->after, tags)) res = TRUE;
        break;
      case DT_TA_SET:
        undotags->after = g_list_copy((GList *)tags);
        // preserve dt tags
        GList *dttags = _tag_get_tags(image_id, DT_TAG_TYPE_DT);
        if(dttags) undotags->after = g_list_concat(undotags->after, dttags);
        res = TRUE;
        break;
      case DT_TA_SET_ALL:
        undotags->after = g_list_copy((GList *)tags);
        res = TRUE;
        break;
      default:
        undotags->after = g_list_copy(undotags->before);
        res = FALSE;
        break;
    }
    _pop_undo_execute(image_id, undotags->before, undotags->after);
    if(undo_on)
      *undo = g_list_append(*undo, undotags);
    else
      _undo_tags_free(undotags);
    images = g_list_next(images);
  }
  return res;
}

gboolean dt_tag_attach_images(const guint tagid, const GList *img, const gboolean undo_on)
{
  if(!img) return FALSE;
  GList *undo = NULL;
  GList *tags = NULL;
  GList *imgs = g_list_copy((GList *)img);

  if(imgs)
  {
    tags = g_list_prepend(tags, GINT_TO_POINTER(tagid));
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_TAGS);

    const gboolean res = _tag_execute(tags, imgs, &undo, undo_on, DT_TA_ATTACH);

    g_list_free(tags);
    g_list_free(imgs);
    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_TAGS, undo, _pop_undo, _tags_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }

    return res;
  }

  return FALSE;
}

gboolean dt_tag_attach(const guint tagid, const gint imgid, const gboolean undo_on, const gboolean group_on)
{
  GList *imgs = NULL;
  gboolean res = FALSE;
  if(imgid == -1)
  {
    imgs = (GList *)dt_view_get_images_to_act_on(!group_on, TRUE);
    res = dt_tag_attach_images(tagid, imgs, undo_on);
  }
  else
  {
    if(dt_is_tag_attached(tagid, imgid)) return FALSE;
    imgs = g_list_append(imgs, GINT_TO_POINTER(imgid));
    res = dt_tag_attach_images(tagid, imgs, undo_on);
    g_list_free(imgs);
  }
  return res;
}

gboolean dt_tag_set_tags(const GList *tags, const GList *img, const gboolean ignore_dt_tags,
                         const gboolean clear_on, const gboolean undo_on)
{
  GList *imgs = g_list_copy((GList *)img);
  if(imgs)
  {
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_TAGS);

    const gboolean res = _tag_execute(tags, imgs, &undo, undo_on,
                                      clear_on ? ignore_dt_tags ? DT_TA_SET : DT_TA_SET_ALL : DT_TA_ATTACH);
    g_list_free(imgs);
    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_TAGS, undo, _pop_undo, _tags_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }
    return res;
  }
  return FALSE;
}

gboolean dt_tag_attach_string_list(const gchar *tags, const GList *img, const gboolean undo_on)
{
  // tags may not exist yet
  // undo only undoes the tags attachments. it doesn't remove created tags.
  gchar **tokens = g_strsplit(tags, ",", 0);
  gboolean res = FALSE;
  if(tokens)
  {
    // tag(s) creation
    GList *tagl = NULL;
    gchar **entry = tokens;
    while(*entry)
    {
      char *e = g_strstrip(*entry);
      if(*e)
      {
        guint tagid = 0;
        dt_tag_new(e, &tagid);
        tagl = g_list_prepend(tagl, GINT_TO_POINTER(tagid));
      }
      entry++;
    }

    // attach newly created tags
    GList *imgs = g_list_copy((GList *)img);
    if(imgs)
    {
      GList *undo = NULL;
      if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_TAGS);

      res = _tag_execute(tagl, imgs, &undo, undo_on, DT_TA_ATTACH);

      g_list_free(imgs);
      if(undo_on)
      {
        dt_undo_record(darktable.undo, NULL, DT_UNDO_TAGS, undo, _pop_undo, _tags_undo_data_free);
        dt_undo_end_group(darktable.undo);
      }
    }
    g_list_free(tagl);
  }
  g_strfreev(tokens);
  return res;
}

gboolean dt_tag_detach_images(const guint tagid, const GList *img, const gboolean undo_on)
{
  GList *imgs = g_list_copy((GList *)img);
  if(imgs)
  {
    GList *tags = NULL;
    tags = g_list_prepend(tags, GINT_TO_POINTER(tagid));
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_TAGS);

    const gboolean res = _tag_execute(tags, imgs, &undo, undo_on, DT_TA_DETACH);

    g_list_free(tags);
    g_list_free(imgs);
    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_TAGS, undo, _pop_undo, _tags_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }
    return res;
  }
  return FALSE;
}

gboolean dt_tag_detach(const guint tagid, const gint imgid, const gboolean undo_on, const gboolean group_on)
{
  GList *imgs = NULL;
  if(imgid == -1)
    imgs = g_list_copy((GList *)dt_view_get_images_to_act_on(TRUE, TRUE));
  else
    imgs = g_list_append(imgs, GINT_TO_POINTER(imgid));
  if(group_on) dt_grouping_add_grouped_images(&imgs);

  const gboolean res = dt_tag_detach_images(tagid, imgs, undo_on);
  g_list_free(imgs);
  return res;
}

gboolean dt_tag_detach_by_string(const char *name, const gint imgid, const gboolean undo_on,
                                 const gboolean group_on)
{
  if(!name || !name[0]) return FALSE;
  guint tagid = 0;
  if(!dt_tag_exists(name, &tagid)) return FALSE;

  return dt_tag_detach(tagid, imgid, undo_on, group_on);
}

// to be called before issuing any query based on memory.darktable_tags
static void dt_set_darktable_tags()
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM memory.darktable_tags",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  const guint count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if (!count)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO memory.darktable_tags (tagid)"
                                " SELECT DISTINCT id"
                                " FROM data.tags"
                                " WHERE name LIKE 'darktable|%%'",
                                -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

uint32_t dt_tag_get_attached(const gint imgid, GList **result, const gboolean ignore_dt_tags)
{
  sqlite3_stmt *stmt;
  dt_set_darktable_tags();
  char *images = NULL;
  uint32_t nb_selected = 0;
  uint32_t count = 0;
  if(imgid > 0)
  {
    images = dt_util_dstrcat(images, "%d",imgid);
    nb_selected = 1;
  }
  else
  {
    const GList *imgs = dt_view_get_images_to_act_on(TRUE, FALSE);
    while(imgs)
    {
      images = dt_util_dstrcat(images, "%d,",GPOINTER_TO_INT(imgs->data));
      nb_selected++;
      imgs = g_list_next((GList *)imgs);
    }
    if(images) images[strlen(images) - 1] = '\0';
  }
  if(images)
  {
    char *query = NULL;
    query = dt_util_dstrcat(query,
                            "SELECT DISTINCT I.tagid, T.name, T.flags, T.synonyms,"
                            " COUNT(DISTINCT I.imgid) AS inb"
                            " FROM main.tagged_images AS I"
                            " JOIN data.tags AS T ON T.id = I.tagid"
                            " WHERE I.imgid IN (%s)%s"
                            " GROUP BY I.tagid "
                            " ORDER by T.name",
                            images, ignore_dt_tags ? " AND T.id NOT IN memory.darktable_tags" : "");
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    g_free(images);

    // Create result
    *result = NULL;
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      dt_tag_t *t = g_malloc0(sizeof(dt_tag_t));
      t->id = sqlite3_column_int(stmt, 0);
      t->tag = g_strdup((char *)sqlite3_column_text(stmt, 1));
      t->leave = g_strrstr(t->tag, "|");
      t->leave = t->leave ? t->leave + 1 : t->tag;
      t->flags = sqlite3_column_int(stmt, 2);
      t->synonym = g_strdup((char *)sqlite3_column_text(stmt, 3));
      uint32_t imgnb = sqlite3_column_int(stmt, 4);
      t->count = imgnb;
      t->select = (nb_selected == 0) ? DT_TS_NO_IMAGE :
                  (imgnb == nb_selected) ? DT_TS_ALL_IMAGES :
                  (imgnb == 0) ? DT_TS_NO_IMAGE : DT_TS_SOME_IMAGES;
      *result = g_list_append(*result, t);
      count++;
    }
    sqlite3_finalize(stmt);
    g_free(query);
  }
  return count;
}

static uint32_t _tag_get_attached_export(const gint imgid, GList **result)
{
  if(!(imgid > 0)) return 0;

  sqlite3_stmt *stmt;
  dt_set_darktable_tags();
  char *query = NULL;
  query = dt_util_dstrcat(query,
                          "SELECT DISTINCT T.id, T.name, T.flags, T.synonyms"
                          // all tags
                          " FROM data.tags AS T"
                          // tags attached to image(s), not dt tag, ordered by name
                          " JOIN (SELECT DISTINCT I.tagid, T.name"
                          "       FROM main.tagged_images AS I"
                          "       JOIN data.tags AS T ON T.id = I.tagid"
                          "       WHERE I.imgid = %d AND T.id NOT IN memory.darktable_tags"
                          "       ORDER by T.name) AS T1"
                          // keep all tags in the path
                          "   ON T.name = SUBSTR(T1.name, 1, LENGTH(T.name))",
                          imgid);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

  // Create result
  uint32_t count = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_tag_t *t = g_malloc0(sizeof(dt_tag_t));
    t->id = sqlite3_column_int(stmt, 0);
    t->tag = g_strdup((char *)sqlite3_column_text(stmt, 1));
    t->leave = g_strrstr(t->tag, "|");
    t->leave = t->leave ? t->leave + 1 : t->tag;
    t->flags = sqlite3_column_int(stmt, 2);
    t->synonym = g_strdup((char *)sqlite3_column_text(stmt, 3));
    *result = g_list_append(*result, t);
    count++;
  }
  sqlite3_finalize(stmt);
  g_free(query);

  return count;
}

static gint sort_tag_by_path(gconstpointer a, gconstpointer b)
{
  const dt_tag_t *tuple_a = (const dt_tag_t *)a;
  const dt_tag_t *tuple_b = (const dt_tag_t *)b;

  return g_ascii_strcasecmp(tuple_a->tag, tuple_b->tag);
}

static gint sort_tag_by_leave(gconstpointer a, gconstpointer b)
{
  const dt_tag_t *tuple_a = (const dt_tag_t *)a;
  const dt_tag_t *tuple_b = (const dt_tag_t *)b;

  return g_ascii_strcasecmp(tuple_a->leave, tuple_b->leave);
}

static gint sort_tag_by_count(gconstpointer a, gconstpointer b)
{
  const dt_tag_t *tuple_a = (const dt_tag_t *)a;
  const dt_tag_t *tuple_b = (const dt_tag_t *)b;

  return (tuple_b->count - tuple_a->count);
}
// sort_type 0 = path, 1 = leave other = count
GList *dt_sort_tag(GList *tags, gint sort_type)
{
  GList *sorted_tags;
  if (sort_type <= 1)
  {
    for(GList *taglist = tags; taglist; taglist = g_list_next(taglist))
    {
      // order such that sub tags are coming directly behind their parent
      gchar *tag = ((dt_tag_t *)taglist->data)->tag;
      for(char *letter = tag; *letter; letter++)
        if(*letter == '|') *letter = '\1';
    }
    sorted_tags = g_list_sort(tags, !sort_type ? sort_tag_by_path : sort_tag_by_leave);
    for(GList *taglist = sorted_tags; taglist; taglist = g_list_next(taglist))
    {
      gchar *tag = ((dt_tag_t *)taglist->data)->tag;
      for(char *letter = tag; *letter; letter++)
        if(*letter == '\1') *letter = '|';
    }
  }
  else
  {
    sorted_tags = g_list_sort(tags, sort_tag_by_count);
  }
  return sorted_tags;
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
        size_t j = 0;
        while(pch[j] != NULL)
        {
          tags = g_list_prepend(tags, g_strdup(pch[j]));
          j++;
        }
      }
      g_strfreev(pch);
    }
  }

  dt_tag_free_result(&taglist);

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

  dt_tag_free_result(&taglist);

  tags = g_list_reverse(tags);
  return tags;
}

static GList *_tag_get_tags(const gint imgid, const dt_tag_type_t type)
{
  GList *tags = NULL;
  if(imgid < 0) return tags;

  sqlite3_stmt *stmt;
  dt_set_darktable_tags();
  char query[256] = { 0 };
  snprintf(query, sizeof(query), "SELECT DISTINCT T.id"
                                 "  FROM main.tagged_images AS I"
                                 "  JOIN data.tags T on T.id = I.tagid"
                                 "  WHERE I.imgid = %d %s",
           imgid, type == DT_TAG_TYPE_ALL ? "" :
                  type == DT_TAG_TYPE_DT ? "AND T.id IN memory.darktable_tags" :
                                           "AND NOT T.id IN memory.darktable_tags");
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    tags = g_list_prepend(tags, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  }

  sqlite3_finalize(stmt);

  return tags;
}

GList *dt_tag_get_tags(const gint imgid, const gboolean ignore_dt_tags)
{
  return _tag_get_tags(imgid, ignore_dt_tags ? DT_TAG_TYPE_USER : DT_TAG_TYPE_ALL);
}

static gint _is_not_exportable_tag(gconstpointer a, gconstpointer b)
{
  dt_tag_t *ta = (dt_tag_t *)a;
  dt_tag_t *tb = (dt_tag_t *)b;
  return ((g_strcmp0(ta->tag, tb->tag) == 0) &&
          ((ta->flags) & (DT_TF_CATEGORY | DT_TF_PRIVATE))) ? 0 : -1;
}

GList *dt_tag_get_list_export(gint imgid, int32_t flags)
{
  GList *taglist = NULL;
  GList *tags = NULL;

  gboolean omit_tag_hierarchy = flags & DT_META_OMIT_HIERARCHY;
  gboolean export_private_tags = flags & DT_META_PRIVATE_TAG;
  gboolean export_tag_synomyms = flags & DT_META_SYNONYMS_TAG;

  uint32_t count = _tag_get_attached_export(imgid, &taglist);

  if(count < 1) return NULL;
  GList *sorted_tags = dt_sort_tag(taglist, 0);
  sorted_tags = g_list_reverse(sorted_tags);

  // reset private if export private
  if(export_private_tags)
  {
    for(GList *tagt = sorted_tags; tagt; tagt = g_list_next(tagt))
    {
      dt_tag_t *t = (dt_tag_t *)sorted_tags->data;
      t->flags &= ~DT_TF_PRIVATE;
    }
  }
  for(; sorted_tags; sorted_tags = g_list_next(sorted_tags))
  {
    dt_tag_t *t = (dt_tag_t *)sorted_tags->data;
    if ((export_private_tags || !(t->flags & DT_TF_PRIVATE))
        && !(t->flags & DT_TF_CATEGORY))
    {
      gchar *tagname = t->leave;
      tags = g_list_prepend(tags, g_strdup(tagname));

      // if not "omit tag hierarchy" the path elements are added
      // unless otherwise stated (defined as category or private)
      if(!omit_tag_hierarchy)
      {
        GList *next = g_list_next(sorted_tags);
        gchar *end = g_strrstr(t->tag, "|");
        while (end)
        {
          end[0] = '\0';
          end = g_strrstr(t->tag, "|");
          if (!next ||
              !g_list_find_custom(next, t, (GCompareFunc)_is_not_exportable_tag))
          {
            const gchar *tag = end ? end + 1 : t->tag;
            tags = g_list_prepend(tags, g_strdup(tag));
          }
        }
      }

      // add synonyms as necessary
      if (export_tag_synomyms)
      {
        gchar *synonyms = t->synonym;
        if (synonyms && synonyms[0])
          {
          gchar **tokens = g_strsplit(synonyms, ",", 0);
          if(tokens)
          {
            gchar **entry = tokens;
            while(*entry)
            {
              char *e = *entry;
              if (*e == ' ') e++;
              tags = g_list_append(tags, g_strdup(e));
              entry++;
            }
          }
          g_strfreev(tokens);
        }
      }
    }
  }
  dt_tag_free_result(&taglist);

  return dt_util_glist_uniq(tags);
}

GList *dt_tag_get_hierarchical_export(gint imgid, int32_t flags)
{
  GList *taglist = NULL;
  GList *tags = NULL;

  const int count = dt_tag_get_attached(imgid, &taglist, TRUE);

  if(count < 1) return NULL;
  const gboolean export_private_tags = flags & DT_META_PRIVATE_TAG;

  while(taglist)
  {
    dt_tag_t *t = (dt_tag_t *)taglist->data;
    if (export_private_tags || !(t->flags & DT_TF_PRIVATE))
    {
      tags = g_list_append(tags, t->tag);
    }
    taglist = g_list_next(taglist);
  }

  dt_tag_free_result(&taglist);

  return tags;
}

gboolean dt_is_tag_attached(const guint tagid, const gint imgid)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid"
                              " FROM main.tagged_images"
                              " WHERE imgid = ?1 AND tagid = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, tagid);

  const gboolean ret = (sqlite3_step(stmt) == SQLITE_ROW);
  sqlite3_finalize(stmt);
  return ret;
}

GList *dt_tag_get_images(const gint tagid)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM main.tagged_images"
                              " WHERE tagid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
    result = g_list_append(result, GINT_TO_POINTER(id));
  }
  sqlite3_finalize(stmt);

  return result;
}

GList *dt_tag_get_images_from_list(const GList *img, const gint tagid)
{
  GList *result = NULL;
  char *images = NULL;
  GList *imgs = (GList *)img;
  while(imgs)
  {
    images = dt_util_dstrcat(images, "%d,",GPOINTER_TO_INT(imgs->data));
    imgs = g_list_next(imgs);
  }
  if(images)
  {
    images[strlen(images) - 1] = '\0';

    sqlite3_stmt *stmt;

    char *query = NULL;
    query = dt_util_dstrcat(query,
                            "SELECT imgid FROM main.tagged_images"
                            " WHERE tagid = %d AND imgid IN (%s)",
                            tagid, images);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int id = sqlite3_column_int(stmt, 0);
      result = g_list_append(result, GINT_TO_POINTER(id));
    }

    sqlite3_finalize(stmt);
    g_free(query);
    g_free(images);
  }
  return result;
}

uint32_t dt_tag_get_suggestions(GList **result)
{
  sqlite3_stmt *stmt;

  dt_set_darktable_tags();

  /* list tags and count */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO memory.taglist (id, count)"
                              " SELECT S.tagid, COUNT(*)"
                              "  FROM main.tagged_images AS S"
                              "  WHERE S.tagid NOT IN memory.darktable_tags"
                              "  GROUP BY S.tagid",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  const uint32_t nb_selected = dt_selected_images_count();
  /* Now put all the bits together */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT T.name, T.id, MT.count, CT.imgnb, T.flags, T.synonyms"
                              "  FROM memory.taglist MT"
                              "  JOIN data.tags T ON MT.id = T.id"
                              "  LEFT JOIN (SELECT tagid, COUNT(DISTINCT imgid) AS imgnb"
                              "             FROM main.tagged_images"
                              "             WHERE imgid IN (SELECT imgid FROM main.selected_images)"
                              "             GROUP BY tagid) AS CT"
                              "    ON CT.tagid = MT.id"
                              "  WHERE T.id NOT IN (SELECT DISTINCT tagid"
                              "                     FROM (SELECT TI.tagid, COUNT(DISTINCT SI.imgid) AS imgnb"
                              "                           FROM main.selected_images AS SI"
                              "                           JOIN main.tagged_images AS TI ON TI.imgid = SI.imgid"
                              "                           GROUP BY TI.tagid)"
                              "                           WHERE imgnb = ?1)"
                              "  AND (T.flags IS NULL OR (T.flags & 1) = 0)"
                              "  ORDER BY MT.count DESC"
                              "  LIMIT 500",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, nb_selected);

  /* ... and create the result list to send upwards */
  uint32_t count = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_tag_t *t = g_malloc0(sizeof(dt_tag_t));
    t->tag = g_strdup((char *)sqlite3_column_text(stmt, 0));
    t->leave = g_strrstr(t->tag, "|");
    t->leave = t->leave ? t->leave + 1 : t->tag;
    t->id = sqlite3_column_int(stmt, 1);
    t->count = sqlite3_column_int(stmt, 2);
    uint32_t imgnb = sqlite3_column_int(stmt, 3);
    t->select = (nb_selected == 0) ? DT_TS_NO_IMAGE :
                (imgnb == nb_selected) ? DT_TS_ALL_IMAGES :
                (imgnb == 0) ? DT_TS_NO_IMAGE : DT_TS_SOME_IMAGES;
    t->flags = sqlite3_column_int(stmt, 4);
    t->synonym = g_strdup((char *)sqlite3_column_text(stmt, 5));
    *result = g_list_append(*result, t);
    count++;
  }

  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.taglist", NULL, NULL, NULL);

  return count;
}

void dt_tag_count_tags_images(const gchar *keyword, int *tag_count, int *img_count)
{
  sqlite3_stmt *stmt;
  *tag_count = 0;
  *img_count = 0;

  if(!keyword) return;
  gchar *keyword_expr = g_strdup_printf("%s|", keyword);

  /* Only select tags that are equal or child to the one we are looking for once. */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO memory.similar_tags (tagid)"
                              "  SELECT id"
                              "    FROM data.tags"
                              "    WHERE name = ?1 OR SUBSTR(name, 1, LENGTH(?2)) = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, keyword, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, keyword_expr, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  g_free(keyword_expr);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(DISTINCT tagid) FROM memory.similar_tags",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  *tag_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(DISTINCT ti.imgid)"
                              "  FROM main.tagged_images AS ti "
                              "  JOIN memory.similar_tags AS st"
                              "    ON st.tagid = ti.tagid",
                              -1, &stmt, NULL);

  sqlite3_step(stmt);
  *img_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.similar_tags", NULL, NULL, NULL);
  }

void dt_tag_get_tags_images(const gchar *keyword, GList **tag_list, GList **img_list)
{
  sqlite3_stmt *stmt;

  if(!keyword) return;
  gchar *keyword_expr = g_strdup_printf("%s|", keyword);

/* Only select tags that are equal or child to the one we are looking for once. */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO memory.similar_tags (tagid)"
                              "  SELECT id"
                              "  FROM data.tags"
                              "  WHERE name = ?1 OR SUBSTR(name, 1, LENGTH(?2)) = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, keyword, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, keyword_expr, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  g_free(keyword_expr);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT ST.tagid, T.name"
                              " FROM memory.similar_tags ST"
                              " JOIN data.tags T"
                              "   ON T.id = ST.tagid ",
                              -1, &stmt, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_tag_t *t = g_malloc0(sizeof(dt_tag_t));
    t->id = sqlite3_column_int(stmt, 0);
    t->tag = g_strdup((char *)sqlite3_column_text(stmt, 1));
    *tag_list = g_list_append((*tag_list), t);
  }
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT DISTINCT ti.imgid"
                              " FROM main.tagged_images AS ti"
                              " JOIN memory.similar_tags AS st"
                              "   ON st.tagid = ti.tagid",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    *img_list = g_list_append((*img_list), GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  }
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.similar_tags", NULL, NULL, NULL);
}

uint32_t dt_selected_images_count()
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT count(*) FROM main.selected_images",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  const uint32_t nb_selected = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return nb_selected;
}

uint32_t dt_tag_images_count(gint tagid)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(DISTINCT imgid) AS imgnb"
                              " FROM main.tagged_images"
                              " WHERE tagid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  sqlite3_step(stmt);
  const uint32_t nb_images = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return nb_images;
}

uint32_t dt_tag_get_with_usage(GList **result)
{
  sqlite3_stmt *stmt;

  dt_set_darktable_tags();

  /* Select tags that are similar to the keyword and are actually used to tag images*/
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO memory.taglist (id, count)"
                              "  SELECT tagid, COUNT(*)"
                              "  FROM main.tagged_images"
                              "  GROUP BY tagid",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  const uint32_t nb_selected = dt_selected_images_count();

  /* Now put all the bits together */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT T.name, T.id, MT.count, CT.imgnb, T.flags, T.synonyms"
                              "  FROM data.tags T "
                              "  LEFT JOIN memory.taglist MT ON MT.id = T.id "
                              "  LEFT JOIN (SELECT tagid, COUNT(DISTINCT imgid) AS imgnb"
                              "             FROM main.tagged_images "
                              "             WHERE imgid IN (SELECT imgid FROM main.selected_images) GROUP BY tagid) AS CT "
                              "    ON CT.tagid = T.id"
                              "  WHERE T.id NOT IN memory.darktable_tags "
                              "  ORDER BY T.name ",
                              -1, &stmt, NULL);

  /* ... and create the result list to send upwards */
  uint32_t count = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_tag_t *t = g_malloc0(sizeof(dt_tag_t));
    t->tag = g_strdup((char *)sqlite3_column_text(stmt, 0));
    t->leave = g_strrstr(t->tag, "|");
    t->leave = t->leave ? t->leave + 1 : t->tag;
    t->id = sqlite3_column_int(stmt, 1);
    t->count = sqlite3_column_int(stmt, 2);
    uint32_t imgnb = sqlite3_column_int(stmt, 3);
    t->select = (nb_selected == 0) ? DT_TS_NO_IMAGE :
                (imgnb == nb_selected) ? DT_TS_ALL_IMAGES :
                (imgnb == 0) ? DT_TS_NO_IMAGE : DT_TS_SOME_IMAGES;
    t->flags = sqlite3_column_int(stmt, 4);
    t->synonym = g_strdup((char *)sqlite3_column_text(stmt, 5));
    *result = g_list_append(*result, t);
    count++;
  }

  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.taglist", NULL, NULL, NULL);

  return count;
}

static gchar *dt_cleanup_synonyms(gchar *synonyms_entry)
{
  gchar *synonyms = NULL;
  for(char *letter = synonyms_entry; *letter; letter++)
  {
    if(*letter == ';' || *letter == '\n') *letter = ',';
    if(*letter == '\r') *letter = ' ';
  }
  gchar **tokens = g_strsplit(synonyms_entry, ",", 0);
  if(tokens)
  {
    gchar **entry = tokens;
    while (*entry)
    {
      // remove leading and trailing spaces
      char *e = *entry + strlen(*entry) - 1;
      while(*e == ' ' && e > *entry)
      {
        *e = '\0';
        e--;
      }
      e = *entry;
      while(*e == ' ') e++;
      if(*e)
      {
        synonyms = dt_util_dstrcat(synonyms, "%s, ", e);
      }
      entry++;
    }
    if (synonyms)
      synonyms[strlen(synonyms) - 2] = '\0';
  }
  g_strfreev(tokens);
  return synonyms;
}

gchar *dt_tag_get_synonyms(gint tagid)
{
  sqlite3_stmt *stmt;
  gchar *synonyms = NULL;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT synonyms FROM data.tags WHERE id = ?1 ",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);

  if (sqlite3_step(stmt) == SQLITE_ROW)
  {
    synonyms = g_strdup((char *)sqlite3_column_text(stmt, 0));
  }
  sqlite3_finalize(stmt);
  return synonyms;
}

void dt_tag_set_synonyms(gint tagid, gchar *synonyms_entry)
{
  if (!synonyms_entry) return;
  char *synonyms = dt_cleanup_synonyms(synonyms_entry);

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.tags SET synonyms = ?2 WHERE id = ?1 ",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, synonyms, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  g_free(synonyms);
}

gint dt_tag_get_flags(gint tagid)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT flags FROM data.tags WHERE id = ?1 ",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);

  gint flags = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW)
  {
    flags = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return flags;
}

void dt_tag_set_flags(gint tagid, gint flags)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.tags SET flags = ?2 WHERE id = ?1 ",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, flags);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_tag_add_synonym(gint tagid, gchar *synonym)
{
  char *synonyms = dt_tag_get_synonyms(tagid);
  if (synonyms)
  {
    synonyms = dt_util_dstrcat(synonyms, ", %s", synonym);
  }
  else
  {
    synonyms = g_strdup(synonym);
  }
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.tags SET synonyms = ?2 WHERE id = ?1 ",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, synonyms, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  g_free(synonyms);
}

static void _free_result_item(dt_tag_t *t, gpointer unused)
{
  g_free(t->tag);
  g_free(t->synonym);
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
  bunch is to be added.
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
  guint tagid = 0;
  guint previous_category_depth = 0;
  gboolean previous_category = FALSE;
  gboolean previous_synonym = FALSE;

  while(getline(&line, &len, fd) != -1)
  {
    // remove newlines and set start past the initial tabs
    char *start = line;
    while(*start == '\t' || *start == ' ' || *start == ',' || *start == ';') start++;
    const int depth = start - line;

    char *end = line + strlen(line) - 1;
    while((*end == '\n' || *end == '\r' || *end == ',' || *end == ';') && end >= start)
    {
      *end = '\0';
      end--;
    }

    // remove control characters from the string
    // if no associated synonym the previous category node can be reused
    gboolean skip = FALSE;
    gboolean category = FALSE;
    gboolean synonym = FALSE;
    if (*start == '[' && *end == ']') // categories
    {
      category = TRUE;
      start++;
      *end-- = '\0';
    }
    else if (*start == '{' && *end == '}')  // synonyms
    {
      synonym = TRUE;
      start++;
      *end-- = '\0';
    }
    if(*start == '~') // fixed order. TODO not possible with our db
    {
      skip = TRUE;
      start++;
    }

    if (synonym)
    {
      // associate the synonym to last tag
      if (tagid)
      {
        char *tagname = g_strdup(start);
        // clear synonyms before importing the new ones => allows export, modification and back import
        if (!previous_synonym) dt_tag_set_synonyms(tagid, "");
        dt_tag_add_synonym(tagid, tagname);
        g_free(tagname);
      }
    }
    else
    {
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
        char *tag = dt_util_glist_to_str("|", hierarchy);
        if (previous_category && (depth > previous_category_depth + 1))
        {
          // reuse previous tag
          dt_tag_rename(tagid, tag);
          if (!category)
            dt_tag_set_flags(tagid, 0);
        }
        else
        {
          // create a new tag
          count++;
          tagid = 1;  // if 0, dt_tag_new creates a new one even if  the tag already exists
          dt_tag_new(tag, &tagid);
          if (category)
            dt_tag_set_flags(tagid, DT_TF_CATEGORY);
        }
        g_free(tag);
      }
    }
    previous_category_depth = category ? depth : 0;
    previous_category = category;
    previous_synonym = synonym;
  }

  free(line);
  g_list_free_full(hierarchy, g_free);
  fclose(fd);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);

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

  GList *tags = NULL;
  gint count = 0;
  dt_tag_get_with_usage(&tags);
  GList *sorted_tags = dt_sort_tag(tags, 0);

  gchar **hierarchy = NULL;
  for(GList *tag_elt = sorted_tags; tag_elt; tag_elt = g_list_next(tag_elt))
  {
    const gchar *tag = ((dt_tag_t *)tag_elt->data)->tag;
    const char *synonyms = ((dt_tag_t *)tag_elt->data)->synonym;
    const guint flags = ((dt_tag_t *)tag_elt->data)->flags;
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
        if (flags & DT_TF_CATEGORY)
          fprintf(fd, "[%s]\n", tokens[i]);
        else
          fprintf(fd, "%s\n", tokens[i]);
        if (synonyms && synonyms[0])
        {
          gchar **tokens2 = g_strsplit(synonyms, ",", 0);
          if(tokens2)
          {
            gchar **entry = tokens2;
            while(*entry)
            {
              char *e = *entry;
              if (*e == ' ') e++;
              for(int j = 0; j < tabs+1; j++) fputc('\t', fd);
              fprintf(fd, "{%s}\n", e);
              entry++;
            }
          }
          g_strfreev(tokens2);
        }
      }
      else
        fprintf(fd, "%s\n", tokens[i]);
    }
  }

  g_strfreev(hierarchy);

  dt_tag_free_result(&tags);

  fclose(fd);

  return count;
}

char *dt_tag_get_subtags(const gint imgid, const char *category, const int level)
{
  if (!category) return NULL;
  const guint rootnb = dt_util_string_count_char(category, '|');
  char *tags = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
          "SELECT DISTINCT T.name FROM main.tagged_images AS I "
          "INNER JOIN data.tags AS T "
          "ON T.id = I.tagid AND SUBSTR(T.name, 1, LENGTH(?2)) = ?2 "
          "WHERE I.imgid = ?1",
          -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, category, -1, SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *tag = (char *)sqlite3_column_text(stmt, 0);
    const guint tagnb = dt_util_string_count_char(tag, '|');
    if (tagnb >= rootnb + level)
    {
      gchar **pch = g_strsplit(tag, "|", -1);
      char *subtag = pch[rootnb + level];
      tags = dt_util_dstrcat(tags, "%s,", subtag);
      g_strfreev(pch);
    }
  }
  if(tags) tags[strlen(tags) - 1] = '\0'; // remove the last comma
  sqlite3_finalize(stmt);
  return tags;
}

const gboolean dt_tag_get_tag_order_by_id(const uint32_t tagid, uint32_t *sort,
                                          gboolean *descending)
{
  gboolean res = FALSE;
  if(!sort  || !descending) return res;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
          "SELECT T.flags FROM data.tags AS T "
          "WHERE T.id = ?1",
          -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const uint32_t flags = sqlite3_column_int(stmt, 0);
    if((flags & (DT_TF_ORDER_SET)) == (DT_TF_ORDER_SET))
    {
      // the 16 upper bits of flags hold the order
      *sort = (flags & ~DT_TF_DESCENDING) >> 16;
      *descending = flags & DT_TF_DESCENDING;
      res = TRUE;
    }
  }
  sqlite3_finalize(stmt);
  return res;
}

const uint32_t dt_tag_get_tag_id_by_name(const char * const name)
{
  uint32_t tagid = 0;
  if(!name) return tagid;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
          "SELECT T.id, T.flags FROM data.tags AS T "
          "WHERE T.name = ?1",
          -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    tagid = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return tagid;
}

void dt_tag_set_tag_order_by_id(const uint32_t tagid, const uint32_t sort,
                                const gboolean descending)
{
  // use the upper 16 bits of flags to store the order
  const uint32_t flags = sort << 16 | (descending ? DT_TF_DESCENDING : 0)
                                    | DT_TF_ORDER_SET;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.tags"
                              " SET flags = (IFNULL(flags, 0) & ?3) | ?2 "
                              "WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, flags);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, DT_TF_ALL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
