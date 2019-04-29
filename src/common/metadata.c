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

#include "common/metadata.h"
#include "common/debug.h"
#include "common/undo.h"
#include "control/signal.h"

#include <stdlib.h>

typedef struct dt_undo_metadata_t
{
  int imgid;
  GList *before;      // list of key/value before
  gint keyid;         // new value (key, value)
  gchar *value;
} dt_undo_metadata_t;

static void _metadata_set_xmp(int id, const gint keyid, const char *value, gboolean undo_actif);

static void _pop_undo(gpointer user_data, const dt_undo_type_t type, dt_undo_data_t data, const dt_undo_action_t action)
{
  if(type == DT_UNDO_METADATA)
  {
    GList *list = (GList *)data;

    while(list)
    {
      dt_undo_metadata_t *metadata = (dt_undo_metadata_t *)list->data;

      GList *tag_list = metadata->before;

      // remove from meta_data
      dt_metadata_clear(metadata->imgid);

      // iterate over tag_list and attach tagid to imgid

      while(tag_list)
      {
        const gchar *key = (gchar *)tag_list->data;
        const gint keyid = atoi(key);
        tag_list = g_list_next(tag_list);
        const gchar *value = (gchar *)tag_list->data;
        tag_list = g_list_next(tag_list);
        _metadata_set_xmp(metadata->imgid, keyid, value, FALSE);
      }

      if(action == DT_ACTION_REDO)
      {
        _metadata_set_xmp(metadata->imgid, metadata->keyid, metadata->value, FALSE);
      }

      list = g_list_next(list);
    }

    dt_control_signal_raise(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  }
}

static dt_undo_metadata_t *_get_metadata(const int imgid, const gint keyid, const gchar *value)
{
  dt_undo_metadata_t *result = (dt_undo_metadata_t *)malloc(sizeof(dt_undo_metadata_t));
  result->imgid  = imgid;
  result->before = NULL;
  result->keyid  = keyid;
  result->value  = g_strdup(value);

  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT key, value FROM main.meta_data WHERE id=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const gchar *ckey = dt_util_dstrcat(NULL, "%d", sqlite3_column_int(stmt, 0));
    const gchar *cvalue = g_strdup((const char *)sqlite3_column_text(stmt, 1));
    result->before = g_list_append(result->before, (gpointer)ckey);
    result->before = g_list_append(result->before, (gpointer)cvalue);
  }
  sqlite3_finalize(stmt);

  return result;
}

GList *_get_metadata_selection(const gint keyid, const gchar *value)
{
  GList *result = NULL;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int imgid = sqlite3_column_int(stmt, 0);
    result = g_list_append(result, _get_metadata(imgid, keyid, value));
  }

  sqlite3_finalize(stmt);

  return result;
}

static void _undo_metadata_free(gpointer data)
{
  dt_undo_metadata_t *metadata = (dt_undo_metadata_t *)data;
  g_list_free_full(metadata->before, g_free);
  g_free(metadata->value);
}

static void _metadata_undo_data_free(gpointer data)
{
  GList *l = (GList *)data;
  g_list_free_full(l, _undo_metadata_free);
}

static void _metadata_set_xmp(const int id, const gint keyid, const char *value, gboolean undo_actif)
{
  sqlite3_stmt *stmt;
  GList *undo = NULL;

  if(undo_actif) dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

  if(id == -1)
  {
    if(undo_actif) undo = _get_metadata_selection(keyid, value);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM main.meta_data WHERE id IN (SELECT imgid FROM main.selected_images) "
                                "AND key = ?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, keyid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if(value != NULL && value[0] != '\0')
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "INSERT INTO main.meta_data (id, key, value) SELECT imgid, ?1, ?2 FROM "
                                  "main.selected_images",
                                  -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, keyid);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, value, -1, SQLITE_TRANSIENT);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }
  else
  {
    if(undo_actif) undo = g_list_append(undo, _get_metadata(id, keyid, value));

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM main.meta_data WHERE id = ?1 AND key = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, keyid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if(value != NULL && value[0] != '\0')
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "INSERT INTO main.meta_data (id, key, value) VALUES (?1, ?2, ?3)", -1, &stmt,
                                  NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, keyid);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, value, -1, SQLITE_TRANSIENT);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }

  if(undo_actif)
  {
    dt_undo_record(darktable.undo, NULL, DT_UNDO_METADATA, (dt_undo_data_t)undo, _pop_undo, _metadata_undo_data_free);
    dt_undo_end_group(darktable.undo);
  }
}

static void dt_metadata_set_xmp(int id, const char *key, const char *value)
{
  int keyid = dt_metadata_get_keyid(key);
  if(keyid == -1) // unknown key
    return;

  _metadata_set_xmp(id, keyid, value, TRUE);
}

static void dt_metadata_set_exif(int id, const char *key, const char *value)
{
} // TODO Is this useful at all?

static GList *dt_metadata_get_xmp(int id, const char *key, uint32_t *count)
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
static GList *dt_metadata_get_exif(int id, const char *key, uint32_t *count)
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
static GList *dt_metadata_get_dt(int id, const char *key, uint32_t *count)
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

void dt_metadata_set(int id, const char *key, const char *value)
{
  if(!key) return;

  char *v = NULL;
  char *c = NULL;

  // strip whitespace from start & end
  if(value)
  {
    v = g_strdup(value);
    c = v + strlen(v) - 1;
    while(c >= v && *c == ' ') *c-- = '\0';
    c = v;
    while(*c == ' ') c++;
  }

  if(strncmp(key, "Xmp.", 4) == 0)
    dt_metadata_set_xmp(id, key, c);
  else if(strncmp(key, "Exif.", 5) == 0)
    dt_metadata_set_exif(id, key, c);

  g_free(v);
}

void dt_metadata_set_list(int id, GList *key_value)
{
  dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

  GList *kv = key_value;

  while(kv)
  {
    const gchar *key = (const gchar *)kv->data;
    kv = g_list_next(kv);
    const gchar *value = (const gchar *)kv->data;
    kv = g_list_next(kv);
    dt_metadata_set(id, key, value);
  }

  dt_undo_end_group(darktable.undo);
}

GList *dt_metadata_get(int id, const char *key, uint32_t *count)
{
  if(strncmp(key, "Xmp.", 4) == 0) return dt_metadata_get_xmp(id, key, count);
  if(strncmp(key, "Exif.", 5) == 0) return dt_metadata_get_exif(id, key, count);
  if(strncmp(key, "darktable.", 10) == 0) return dt_metadata_get_dt(id, key, count);
  return NULL;
}

// TODO: Also clear exif data? I don't think it makes sense.
void dt_metadata_clear(int id)
{
  if(id == -1)
  {
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.meta_data WHERE id IN "
                                                         "(SELECT imgid FROM main.selected_images)",
                          NULL, NULL, NULL);
  }
  else
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.meta_data WHERE id = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
