/*
    This file is part of darktable,
    copyright (c) 2010 tobias ellinghaus.
    copyright (c) 2019 philippe weyland

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

#include "common/metadata.h"
#include "common/debug.h"
#include "common/collection.h"
#include "common/undo.h"
#include "common/grouping.h"
#include "views/view.h"
#include "control/signal.h"

#include <stdlib.h>

typedef struct dt_undo_metadata_t
{
  int imgid;
  GList *before;      // list of key/value before
  GList *after;       // list of key/value after
} dt_undo_metadata_t;

static gboolean _compare_metadata(gconstpointer a, gconstpointer b)
{
  return g_strcmp0(a, b);
}

static gchar *_get_tb_removed_metadata_string_values(GList *before, GList *after)
{
  GList *b = before;
  GList *a = after;
  gchar *metadata_list = NULL;

  while(b)
  {
    GList *same = g_list_find_custom(a, b->data, _compare_metadata);
    GList *b2 = g_list_next(b);
    gboolean different = FALSE;
    if(same)
    {
      GList *same2 = g_list_next(same);
      different = g_strcmp0(same2->data, b2->data);
    }
    if(!same || different)
    {
      metadata_list = dt_util_dstrcat(metadata_list, "%d,", atoi(b->data));
    }
    b = g_list_next(b);
    b = g_list_next(b);
  }
  if(metadata_list) metadata_list[strlen(metadata_list) - 1] = '\0';
  return metadata_list;
}

static gchar *_get_tb_added_metadata_string_values(const int img, GList *before, GList *after)
{
  GList *b = before;
  GList *a = after;
  gchar *metadata_list = NULL;

  while(a)
  {
    GList *same = g_list_find_custom(b, a->data, _compare_metadata);
    GList *a2 = g_list_next(a);
    gboolean different = FALSE;
    if(same)
    {
      GList *same2 = g_list_next(same);
      different = g_strcmp0(same2->data, a2->data);
    }
    if(!same || different)
    {
      metadata_list = dt_util_dstrcat(metadata_list, "(%d,%d,\'%s\'),", GPOINTER_TO_INT(img), atoi(a->data), (char *)a2->data);
    }
    a = g_list_next(a);
    a = g_list_next(a);
  }
  if(metadata_list) metadata_list[strlen(metadata_list) - 1] = '\0';
  return metadata_list;
}

static void _bulk_remove_metadata(const int img, const gchar *metadata_list)
{
  if(img > 0 && metadata_list)
  {
    char *query = NULL;
    sqlite3_stmt *stmt;
    query = dt_util_dstrcat(query, "DELETE FROM main.meta_data WHERE id = %d AND key IN (%s)", img, metadata_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_free(query);
  }
}

static void _bulk_add_metadata(const gchar *metadata_list)
{
  if(metadata_list)
  {
    char *query = NULL;
    sqlite3_stmt *stmt;
    query = dt_util_dstrcat(query, "INSERT INTO main.meta_data (id, key, value) VALUES %s", metadata_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_free(query);
  }
}

static void _pop_undo_execute(const int imgid, GList *before, GList *after)
{
  gchar *tobe_removed_list = _get_tb_removed_metadata_string_values(before, after);
  gchar *tobe_added_list = _get_tb_added_metadata_string_values(imgid, before, after);

  _bulk_remove_metadata(imgid, tobe_removed_list);
  _bulk_add_metadata(tobe_added_list);

  g_free(tobe_removed_list);
  g_free(tobe_added_list);
}

static void _pop_undo(gpointer user_data, const dt_undo_type_t type, dt_undo_data_t data, const dt_undo_action_t action, GList **imgs)
{
  if(type == DT_UNDO_METADATA)
  {
    GList *list = (GList *)data;

    while(list)
    {
      dt_undo_metadata_t *undometadata = (dt_undo_metadata_t *)list->data;

      GList *before = (action == DT_ACTION_UNDO) ? undometadata->after : undometadata->before;
      GList *after = (action == DT_ACTION_UNDO) ? undometadata->before : undometadata->after;
      _pop_undo_execute(undometadata->imgid, before, after);
      *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(undometadata->imgid));
      list = g_list_next(list);
    }

    dt_control_signal_raise(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  }
}

GList *dt_metadata_get_list_id(const int id)
{
  GList *metadata = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT key, value FROM main.meta_data WHERE id=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const gchar *ckey = dt_util_dstrcat(NULL, "%d", sqlite3_column_int(stmt, 0));
    const gchar *cvalue = g_strdup((const char *)sqlite3_column_text(stmt, 1));
    metadata = g_list_append(metadata, (gpointer)ckey);
    metadata = g_list_append(metadata, (gpointer)cvalue);
  }
  sqlite3_finalize(stmt);
  return metadata;
}

static void _undo_metadata_free(gpointer data)
{
  dt_undo_metadata_t *metadata = (dt_undo_metadata_t *)data;
  g_list_free_full(metadata->before, g_free);
  g_list_free_full(metadata->after, g_free);
  g_free(metadata);
}

static void _metadata_undo_data_free(gpointer data)
{
  GList *l = (GList *)data;
  g_list_free_full(l, _undo_metadata_free);
}

gchar *_cleanup_metadata_value(const gchar *value)
{
  char *v = NULL;
  char *c = NULL;
  if (value && value[0])
  {
    v = g_strdup(value);
    c = v + strlen(v) - 1;
    while(c >= v && *c == ' ') *c-- = '\0';
    c = v;
    while(*c == ' ') c++;
  }
  c = g_strdup(c);
  g_free(v);
  return c;
}

static GList *dt_metadata_get_xmp(const int id, const char *key, uint32_t *count)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;
  uint32_t local_count = 0;

  int keyid = dt_metadata_get_keyid(key);
  // key not found in db. Maybe it's one of our "special" keys (rating, tags and colorlabels)?
  if(keyid == -1)
  {
    if(strncmp(key, "Xmp.xmp.Rating", 14) == 0)
    {
      if(id == -1)
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT flags FROM main.images WHERE id IN "
                                                                   "(SELECT imgid FROM main.selected_images)",
                                    -1, &stmt, NULL);
      }
      else // single image under mouse cursor
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT flags FROM main.images WHERE id = ?1",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
      }
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        local_count++;
        int stars = sqlite3_column_int(stmt, 0);
        stars = (stars & 0x7) - 1;
        result = g_list_append(result, GINT_TO_POINTER(stars));
      }
      sqlite3_finalize(stmt);
    }
    else if(strncmp(key, "Xmp.dc.subject", 14) == 0)
    {
      if(id == -1)
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT name FROM data.tags t JOIN main.tagged_images i ON "
                                    "i.tagid = t.id WHERE imgid IN "
                                    "(SELECT imgid FROM main.selected_images)",
                                    -1, &stmt, NULL);
      }
      else // single image under mouse cursor
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT name FROM data.tags t JOIN main.tagged_images i ON "
                                    "i.tagid = t.id WHERE imgid = ?1",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
      }
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        local_count++;
        result = g_list_append(result, g_strdup((char *)sqlite3_column_text(stmt, 0)));
      }
      sqlite3_finalize(stmt);
    }
    else if(strncmp(key, "Xmp.darktable.colorlabels", 25) == 0)
    {
      if(id == -1)
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT color FROM main.color_labels WHERE imgid IN "
                                    "(SELECT imgid FROM main.selected_images)",
                                    -1, &stmt, NULL);
      }
      else // single image under mouse cursor
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT color FROM main.color_labels WHERE imgid=?1 ORDER BY color",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
      }
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        local_count++;
        result = g_list_append(result, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
      }
      sqlite3_finalize(stmt);
    }
    if(count != NULL) *count = local_count;
    return result;
  }

  // So we got this far -- it has to be a generic key-value entry from meta_data
  if(id == -1)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT value FROM main.meta_data WHERE id IN "
                                "(SELECT imgid FROM main.selected_images) AND key = ?1 ORDER BY value",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, keyid);
  }
  else // single image under mouse cursor
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT value FROM main.meta_data WHERE id = ?1 AND key = ?2 ORDER BY value", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, keyid);
  }
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    local_count++;
    result = g_list_append(result, g_strdup((char *)sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt);
  if(count != NULL) *count = local_count;
  return result;
}

/*
  Dear Mister Dijkstra,
  I hereby make a formal apology for using goto statements in the following
  function. While I am fully aware that I will rot in the deepest hells for
  this ultimate sin and that I'm not worth to be called a "programmer" from
  now on, I have one excuse to bring up: I never did so before, and this way
  the code gets a lot smaller and less repetitive. And since you are dead
  while I am not (yet) I will stick with my gotos.
  See you in hell
  houz
*/
static GList *dt_metadata_get_exif(const int id, const char *key, uint32_t *count)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;
  uint32_t local_count = 0;

  // the doubles
  if(strncmp(key, "Exif.Photo.ExposureTime", 23) == 0)
  {
    if(id == -1)
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT exposure FROM main.images WHERE id IN "
                                                                 "(SELECT imgid FROM main.selected_images)",
                                  -1, &stmt, NULL);
    }
    else // single image under mouse cursor
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT exposure FROM main.images WHERE id = ?1",
                                  -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    }
  }
  else if(strncmp(key, "Exif.Photo.ApertureValue", 24) == 0)
  {
    if(id == -1)
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT aperture FROM main.images WHERE id IN "
                                                                 "(SELECT imgid FROM main.selected_images)",
                                  -1, &stmt, NULL);
    }
    else // single image under mouse cursor
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT aperture FROM main.images WHERE id = ?1",
                                  -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    }
  }
  else if(strncmp(key, "Exif.Photo.ISOSpeedRatings", 26) == 0)
  {
    if(id == -1)
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT iso FROM main.images WHERE id IN "
                                  "(SELECT imgid FROM main.selected_images)", -1, &stmt, NULL);
    }
    else // single image under mouse cursor
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT iso FROM main.images WHERE id = ?1", -1,
                                  &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    }
  }
  else if(strncmp(key, "Exif.Photo.FocalLength", 22) == 0)
  {
    if(id == -1)
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT focal_length FROM main.images WHERE id IN "
                                  "(SELECT imgid FROM main.selected_images)",
                                  -1, &stmt, NULL);
    }
    else // single image under mouse cursor
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT focal_length FROM main.images WHERE id = ?1", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    }
  }
  else
  {

    // the strings
    if(strncmp(key, "Exif.Photo.DateTimeOriginal", 27) == 0)
    {
      if(id == -1)
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT datetime_taken FROM main.images WHERE id IN "
                                    "(SELECT imgid FROM main.selected_images)",
                                    -1, &stmt, NULL);
      }
      else // single image under mouse cursor
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT datetime_taken FROM main.images WHERE id = ?1", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
      }
    }
    else if(strncmp(key, "Exif.Image.Make", 15) == 0)
    {
      if(id == -1)
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT maker FROM main.images WHERE id IN "
                                                                   "(SELECT imgid FROM main.selected_images)",
                                    -1, &stmt, NULL);
      }
      else // single image under mouse cursor
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT maker FROM main.images WHERE id = ?1",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
      }
    }
    else if(strncmp(key, "Exif.Image.Model", 16) == 0)
    {
      if(id == -1)
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT model FROM main.images WHERE id IN "
                                                                   "(SELECT imgid FROM main.selected_images)",
                                    -1, &stmt, NULL);
      }
      else // single image under mouse cursor
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT model FROM main.images WHERE id = ?1",
                                    -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
      }
    }
    else
    {
      goto END;
    }
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      local_count++;
      result = g_list_append(result, g_strdup((char *)sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    goto END;
  }

  // the double queries
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    local_count++;
    double *tmp = (double *)malloc(sizeof(double));
    *tmp = sqlite3_column_double(stmt, 0);
    result = g_list_append(result, tmp);
  }
  sqlite3_finalize(stmt);

END:
  if(count != NULL) *count = local_count;
  return result;
}

// for everything which doesn't fit anywhere else (our made up stuff)
static GList *dt_metadata_get_dt(const int id, const char *key, uint32_t *count)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;
  uint32_t local_count = 0;

  if(strncmp(key, "darktable.Lens", 14) == 0)
  {
    if(id == -1)
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT lens FROM main.images WHERE id IN "
                                                                 "(SELECT imgid FROM main.selected_images)",
                                  -1, &stmt, NULL);
    }
    else // single image under mouse cursor
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT lens FROM main.images WHERE id = ?1", -1,
                                  &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    }
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      local_count++;
      result = g_list_append(result, g_strdup((char *)sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
  }
  else if(strncmp(key, "darktable.Name", 14) == 0)
  {
    result = g_list_append(result, g_strdup(PACKAGE_NAME));
    local_count = 1;
  }
  else if(strncmp(key, "darktable.Version", 17) == 0)
  {
    result = g_list_append(result, g_strdup(darktable_package_version));
    local_count = 1;
  }
  if(count != NULL) *count = local_count;
  return result;
}

static void _metadata_add_metadata_to_list(GList **list, GList *metadata)
{
  GList *m = metadata;
  while(m)
  {
    GList *m2 = g_list_next(m);
    GList *same = g_list_find_custom(*list, m->data, _compare_metadata);
    GList *same2 = g_list_next(same);
    gboolean different = FALSE;
    if(same) different = g_strcmp0(same2->data, m2->data);
    if(same && different)
    {
      // same key but different value - replace the old value by the new one
      g_free(same2->data);
      same2->data = g_strdup(m2->data);
    }
    else if(!same)
    {
      // new key for that image - append the new metadata item
      *list = g_list_append(*list, g_strdup(m->data));
      *list = g_list_append(*list, g_strdup(m2->data));
    }
    m = g_list_next(m);
    m = g_list_next(m);
  }
}

typedef enum dt_tag_actions_t
{
  DT_MA_SET = 0,
  DT_MA_ADD
} dt_tag_actions_t;

static void _metadata_execute(GList *imgs, GList *metadata, GList **undo, const gboolean undo_on, const gint action)
{
  GList *images = imgs;
  while(images)
  {
    const int image_id = GPOINTER_TO_INT(images->data);

    dt_undo_metadata_t *undometadata = (dt_undo_metadata_t *)malloc(sizeof(dt_undo_metadata_t));
    undometadata->imgid = image_id;
    undometadata->before = dt_metadata_get_list_id(image_id);
    switch(action)
    {
      case DT_MA_SET:
        undometadata->after = metadata ? g_list_copy_deep(metadata, (GCopyFunc)g_strdup, NULL) : NULL;
        break;
      case DT_MA_ADD:
        undometadata->after = g_list_copy_deep(undometadata->before, (GCopyFunc)g_strdup, NULL);
        _metadata_add_metadata_to_list(&undometadata->after, metadata);
        break;
      default:
        undometadata->after = g_list_copy_deep(undometadata->before, (GCopyFunc)g_strdup, NULL);
        break;
    }

    _pop_undo_execute(image_id, undometadata->before, undometadata->after);

    if(undo_on)
      *undo = g_list_append(*undo, undometadata);
    else
      _undo_metadata_free(undometadata);
    images = g_list_next(images);
  }
}

void dt_metadata_set(const int imgid, const char *key, const char *value, const gboolean undo_on, const gboolean group_on)
{
  if(!key) return;

  int keyid = dt_metadata_get_keyid(key);
  if(keyid != -1) // known key
  {
    GList *imgs = NULL;
    if(imgid == -1)
      imgs = dt_collection_get_selected(darktable.collection, -1);
    else
      imgs = g_list_append(imgs, GINT_TO_POINTER(imgid));
    if(imgs)
    {
      GList *undo = NULL;
      if(group_on) dt_grouping_add_grouped_images(&imgs);
      if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

      const gchar *ckey = dt_util_dstrcat(NULL, "%d", keyid);
      const gchar *cvalue = _cleanup_metadata_value(value);
      GList *metadata = NULL;
      metadata = g_list_append(metadata, (gpointer)ckey);
      metadata = g_list_append(metadata, (gpointer)cvalue);

      _metadata_execute(imgs, metadata, &undo, undo_on, DT_MA_ADD);

      g_list_free_full(metadata, g_free);
      g_list_free(imgs);
      if(undo_on)
      {
        dt_undo_record(darktable.undo, NULL, DT_UNDO_METADATA, undo, _pop_undo, _metadata_undo_data_free);
        dt_undo_end_group(darktable.undo);
      }
      dt_control_signal_raise(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
    }
  }
}

void dt_metadata_set_list(const int imgid, GList *key_value, const gboolean undo_on, const gboolean group_on)
{
  GList *metadata = NULL;
  GList *kv = key_value;
  while(kv)
  {
    const gchar *key = (const gchar *)kv->data;
    int keyid = dt_metadata_get_keyid(key);
    if(keyid != -1) // known key
    {
      const gchar *ckey = dt_util_dstrcat(NULL, "%d", keyid);
      kv = g_list_next(kv);
      const gchar *value = (const gchar *)kv->data;
      kv = g_list_next(kv);
      if(value && value[0])
      {
        metadata = g_list_append(metadata, (gchar *)ckey);
        metadata = g_list_append(metadata, _cleanup_metadata_value(value));
      }
    }
    else
    {
      kv = g_list_next(kv);
      kv = g_list_next(kv);
    }
  }

  if(metadata)
  {
    GList *imgs = NULL;
    if(imgid == -1)
      imgs = dt_collection_get_selected(darktable.collection, -1);
    else
      imgs = g_list_append(imgs, GINT_TO_POINTER(imgid));
    if(imgs)
    {
      GList *undo = NULL;
      if(group_on) dt_grouping_add_grouped_images(&imgs);
      if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

      _metadata_execute(imgs, metadata, &undo, undo_on, DT_MA_SET);

      g_list_free(imgs);
      if(undo_on)
      {
        dt_undo_record(darktable.undo, NULL, DT_UNDO_METADATA, undo, _pop_undo, _metadata_undo_data_free);
        dt_undo_end_group(darktable.undo);
      }
    }
  }
}

GList *dt_metadata_get(const int id, const char *key, uint32_t *count)
{
  if(strncmp(key, "Xmp.", 4) == 0) return dt_metadata_get_xmp(id, key, count);
  if(strncmp(key, "Exif.", 5) == 0) return dt_metadata_get_exif(id, key, count);
  if(strncmp(key, "darktable.", 10) == 0) return dt_metadata_get_dt(id, key, count);
  return NULL;
}

void dt_metadata_clear(const int imgid, const gboolean undo_on, const gboolean group_on)
{
  GList *imgs = NULL;
  if(imgid == -1)
    imgs = dt_collection_get_selected(darktable.collection, -1);
  else
    imgs = g_list_append(imgs, GINT_TO_POINTER(imgid));
  if(imgs)
  {
    GList *undo = NULL;
    if(group_on) dt_grouping_add_grouped_images(&imgs);
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

    _metadata_execute(imgs, NULL, &undo, undo_on, DT_MA_SET);

    g_list_free(imgs);
    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_METADATA, undo, _pop_undo, _metadata_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  }
}

void dt_metadata_set_list_id(const int imgid, GList *metadata, const gboolean clear_on, const gboolean undo_on, const gboolean group_on)
{
  GList *imgs = NULL;
  if(imgid == -1)
    imgs = dt_collection_get_selected(darktable.collection, -1);
  else
    imgs = g_list_append(imgs, GINT_TO_POINTER(imgid));
  if(imgs)
  {
    GList *undo = NULL;
    if(group_on) dt_grouping_add_grouped_images(&imgs);
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

    _metadata_execute(imgs, metadata, &undo, undo_on, clear_on ? DT_MA_SET : DT_MA_ADD);

    g_list_free(imgs);
    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_METADATA, undo, _pop_undo, _metadata_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
