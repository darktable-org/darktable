/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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
#include "common/utility.h"
#include "control/conf.h"
#include "views/view.h"
#include "control/signal.h"

#include <stdlib.h>

// this array should contain all dt metadata
// add the new metadata at the end when needed
// Dependencies
//    Must match with dt_metadata_t in metadata.h.
//    Exif.cc: add the new metadata into dt_xmp_keys[]
//    libs/metadata.c increment version and change legacy_param() accordingly
// CAUTION : key, subkey (last term of key) & name must be unique

static const struct
{
  char *key;
  char *name;
  int type;
  uint32_t display_order;
} dt_metadata_def[] = {
  // clang-format off
  {"Xmp.dc.creator", N_("creator"), DT_METADATA_TYPE_USER, 2},
  {"Xmp.dc.publisher", N_("publisher"), DT_METADATA_TYPE_USER, 3},
  {"Xmp.dc.title", N_("title"), DT_METADATA_TYPE_USER, 0},
  {"Xmp.dc.description", N_("description"), DT_METADATA_TYPE_USER, 1},
  {"Xmp.dc.rights", N_("rights"), DT_METADATA_TYPE_USER, 4},
  {"Xmp.acdsee.notes", N_("notes"), DT_METADATA_TYPE_USER, 5},
  {"Xmp.darktable.version_name", N_("version name"), DT_METADATA_TYPE_OPTIONAL, 6},
  {"Xmp.darktable.image_id", N_("image id"), DT_METADATA_TYPE_INTERNAL, 7}
  // clang-format on
};

unsigned int dt_metadata_get_nb_user_metadata()
{
  unsigned int nb = 0;
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(dt_metadata_def[i].type != DT_METADATA_TYPE_INTERNAL)
      nb++;
  }
  return nb;
}

const char *dt_metadata_get_name_by_display_order(const uint32_t order)
{
  if(order < DT_METADATA_NUMBER)
  {
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      if(order == dt_metadata_def[i].display_order)
        return dt_metadata_def[i].name;
    }
  }
  return NULL;
}

dt_metadata_t dt_metadata_get_keyid_by_display_order(const uint32_t order)
{
  if(order < DT_METADATA_NUMBER)
  {
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      if(order == dt_metadata_def[i].display_order)
        return i;
    }
  }
  return -1;
}

dt_metadata_t dt_metadata_get_keyid_by_name(const char* name)
{
  if(!name) return -1;
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(strncmp(name, dt_metadata_def[i].name, strlen(dt_metadata_def[i].name)) == 0)
      return i;
  }
  return -1;
}

int dt_metadata_get_type_by_display_order(const uint32_t order)
{
  if(order < DT_METADATA_NUMBER)
  {
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      if(order == dt_metadata_def[i].display_order)
        return dt_metadata_def[i].type;
    }
  }
  return 0;
}

const char *dt_metadata_get_name(const uint32_t keyid)
{
  if(keyid < DT_METADATA_NUMBER)
    return dt_metadata_def[keyid].name;
  else
    return NULL;
}

dt_metadata_t dt_metadata_get_keyid(const char* key)
{
  if(!key) return -1;
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(strncmp(key, dt_metadata_def[i].key, strlen(dt_metadata_def[i].key)) == 0)
      return i;
  }
  return -1;
}

const char *dt_metadata_get_key(const uint32_t keyid)
{
  if(keyid < DT_METADATA_NUMBER)
    return dt_metadata_def[keyid].key;
  else
    return NULL;
}

const char *dt_metadata_get_subkey(const uint32_t keyid)
{
  if(keyid < DT_METADATA_NUMBER)
  {
    char *t = g_strrstr(dt_metadata_def[keyid].key, ".");
    if(t) return t + 1;
  }
  return NULL;
}

const char *dt_metadata_get_key_by_subkey(const char *subkey)
{
  if(subkey)
  {
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      char *t = g_strrstr(dt_metadata_def[i].key, ".");
      if(t && !g_strcmp0(t + 1, subkey))
        return dt_metadata_def[i].key;
    }
  }
  return NULL;
}

int dt_metadata_get_type(const uint32_t keyid)
{
  if(keyid < DT_METADATA_NUMBER)
    return dt_metadata_def[keyid].type;
  else
    return 0;
}

void dt_metadata_init()
{
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const int type = dt_metadata_get_type(i);
    const char *name = (gchar *)dt_metadata_get_name(i);
    char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
    if(!dt_conf_key_exists(setting))
    {
      // per default should be imported - ignored if "write_sidecar_files" set
      uint32_t flag = DT_METADATA_FLAG_IMPORTED;
      if(type == DT_METADATA_TYPE_OPTIONAL)
      {
        // per default this one should be hidden
        flag |= DT_METADATA_FLAG_HIDDEN;
      }
      dt_conf_set_int(setting, flag);
    }
    g_free(setting);
  }
}

typedef struct dt_undo_metadata_t
{
  dt_imgid_t imgid;
  GList *before;      // list of key/value before
  GList *after;       // list of key/value after
} dt_undo_metadata_t;

static GList *_list_find_custom(GList *list, gpointer data)
{
  for(GList *i = list; i; i = g_list_next(i))
  {
    if(i->data && !g_strcmp0(i->data, data))
      return i;
    i = g_list_next(i);
  }
  return NULL;
}

static gchar *_get_tb_removed_metadata_string_values(GList *before, GList *after)
{
  GList *b = before;
  GList *a = after;
  gchar *metadata_list = NULL;

  while(b)
  {
    GList *same_key = _list_find_custom(a, b->data);
    GList *b2 = g_list_next(b);
    gboolean different_value = FALSE;
    const char *value = (char *)b2->data; // if empty we can remove it
    if(same_key)
    {
      GList *same2 = g_list_next(same_key);
      different_value = g_strcmp0(same2->data, b2->data);
    }
    if(!same_key || different_value || !value[0])
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
    GList *same_key = _list_find_custom(b, a->data);
    GList *a2 = g_list_next(a);
    gboolean different_value = FALSE;
    const char *value = (char *)a2->data; // if empty we don't add it to database
    if(same_key)
    {
      GList *same2 = g_list_next(same_key);
      different_value = g_strcmp0(same2->data, a2->data);
    }
    if((!same_key || different_value) && value[0])
    {
      char *escaped_text = sqlite3_mprintf("%q", value);
      metadata_list = dt_util_dstrcat(metadata_list, "(%d,%d,'%s'),", GPOINTER_TO_INT(img), atoi(a->data), escaped_text);
      sqlite3_free(escaped_text);
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
    sqlite3_stmt *stmt;
    gchar *query = g_strdup_printf("DELETE FROM main.meta_data WHERE id = %d AND key IN (%s)", img, metadata_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_free(query);
  }
}

static void _bulk_add_metadata(gchar *metadata_list)
{
  if(metadata_list)
  {
    sqlite3_stmt *stmt;
    gchar *query = g_strdup_printf("INSERT INTO main.meta_data (id, key, value) VALUES %s", metadata_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_free(query);
  }
}

static void _pop_undo_execute(const dt_imgid_t imgid, GList *before, GList *after)
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
    for(GList *list = (GList *)data; list; list = g_list_next(list))
    {
      dt_undo_metadata_t *undometadata = (dt_undo_metadata_t *)list->data;

      GList *before = (action == DT_ACTION_UNDO) ? undometadata->after : undometadata->before;
      GList *after = (action == DT_ACTION_UNDO) ? undometadata->before : undometadata->after;
      _pop_undo_execute(undometadata->imgid, before, after);
      *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(undometadata->imgid));
    }

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
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
    const gchar *value = (const char *)sqlite3_column_text(stmt, 1);
    gchar *ckey = g_strdup_printf("%d", sqlite3_column_int(stmt, 0));
    gchar *cvalue = g_strdup(value ? value : ""); // to avoid NULL value
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
  if(value && value[0])
  {
    v = g_strdup(value);
    c = v + strlen(v) - 1;
    while(c >= v && *c == ' ') *c-- = '\0';
    c = v;
    while(*c == ' ') c++;
  }
  c = g_strdup(c ? c : ""); // avoid NULL value
  g_free(v);
  return c;
}

GList *dt_metadata_get(const int id, const char *key, uint32_t *count)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;
  uint32_t local_count = 0;

  const int keyid = dt_metadata_get_keyid(key);
  // key not found in db. Maybe it's one of our "special" keys (rating, tags and colorlabels)?
  if(keyid == -1)
  {
    if(strncmp(key, "Xmp.xmp.Rating", 14) == 0)
    {
      if(id == -1)
      {
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT flags FROM main.images WHERE id IN "
                                                                   "(SELECT imgid FROM main.selected_images)",
                                    -1, &stmt, NULL);
        // clang-format on
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
        result = g_list_prepend(result, GINT_TO_POINTER(stars));
      }
      sqlite3_finalize(stmt);
    }
    else if(strncmp(key, "Xmp.dc.subject", 14) == 0)
    {
      if(id == -1)
      {
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT name FROM data.tags t JOIN main.tagged_images i ON "
                                    "i.tagid = t.id WHERE imgid IN "
                                    "(SELECT imgid FROM main.selected_images)",
                                    -1, &stmt, NULL);
        // clang-format on
      }
      else // single image under mouse cursor
      {
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT name FROM data.tags t JOIN main.tagged_images i ON "
                                    "i.tagid = t.id WHERE imgid = ?1",
                                    -1, &stmt, NULL);
        // clang-format on
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
      }
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        local_count++;
        result = g_list_prepend(result, g_strdup((char *)sqlite3_column_text(stmt, 0)));
      }
      sqlite3_finalize(stmt);
    }
    else if(strncmp(key, "Xmp.darktable.colorlabels", 25) == 0)
    {
      if(id == -1)
      {
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT color FROM main.color_labels WHERE imgid IN "
                                    "(SELECT imgid FROM main.selected_images)",
                                    -1, &stmt, NULL);
        // clang-format on
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
        result = g_list_prepend(result, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
      }
      sqlite3_finalize(stmt);
    }
    if(count != NULL) *count = local_count;
    return g_list_reverse(result);
  }

  // So we got this far -- it has to be a generic key-value entry from meta_data
  if(id == -1)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT value FROM main.meta_data WHERE id IN "
                                "(SELECT imgid FROM main.selected_images) AND key = ?1 ORDER BY value",
                                -1, &stmt, NULL);
    // clang-format on
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
    char *value = (char *)sqlite3_column_text(stmt, 0);
    result = g_list_prepend(result, g_strdup(value ? value : "")); // to avoid NULL value
  }
  sqlite3_finalize(stmt);
  if(count != NULL) *count = local_count;
  return g_list_reverse(result);  // list was built in reverse order, so un-reverse it
}

static void _metadata_add_metadata_to_list(GList **list, const GList *metadata)
{
  const GList *m = metadata;
  while(m)
  {
    GList *m2 = g_list_next(m);
    GList *same_key = _list_find_custom(*list, m->data);
    GList *same2 = g_list_next(same_key);
    gboolean different_value = FALSE;
    if(same_key) different_value = g_strcmp0(same2->data, m2->data);
    if(same_key && different_value)
    {
      // same key but different value - replace the old value by the new one
      g_free(same2->data);
      same2->data = g_strdup(m2->data);
    }
    else if(!same_key)
    {
      // new key for that image - append the new metadata item
      *list = g_list_append(*list, g_strdup(m->data));
      *list = g_list_append(*list, g_strdup(m2->data));
    }
    m = g_list_next(m);
    m = g_list_next(m);
  }
}

static void _metadata_remove_metadata_from_list(GList **list, const GList *metadata)
{
  // caution: metadata is a simple list here
  for(const GList *m = metadata; m; m = g_list_next(m))
  {
    GList *same_key = _list_find_custom(*list, m->data);
    if(same_key)
    {
      // same key for that image - remove metadata item
      GList *same2 = g_list_next(same_key);
      *list = g_list_remove_link(*list, same_key);
      g_free(same_key->data);
      g_list_free(same_key);
      *list = g_list_remove_link(*list, same2);
      g_free(same2->data);
      g_list_free(same2);
    }
  }
}

typedef enum dt_tag_actions_t
{
  DT_MA_SET = 0,
  DT_MA_ADD,
  DT_MA_REMOVE
} dt_tag_actions_t;

static void _metadata_execute(const GList *imgs, const GList *metadata, GList **undo,
                              const gboolean undo_on, const gint action)
{
  for(const GList *images = imgs; images; images = g_list_next(images))
  {
    const dt_imgid_t image_id = GPOINTER_TO_INT(images->data);

    dt_undo_metadata_t *undometadata = (dt_undo_metadata_t *)malloc(sizeof(dt_undo_metadata_t));
    undometadata->imgid = image_id;
    undometadata->before = dt_metadata_get_list_id(image_id);
    switch(action)
    {
      case DT_MA_SET:
        undometadata->after = metadata ? g_list_copy_deep((GList *)metadata, (GCopyFunc)g_strdup, NULL) : NULL;
        break;
      case DT_MA_ADD:
        undometadata->after = g_list_copy_deep(undometadata->before, (GCopyFunc)g_strdup, NULL);
        _metadata_add_metadata_to_list(&undometadata->after, metadata);
        break;
      case DT_MA_REMOVE:
        undometadata->after = g_list_copy_deep(undometadata->before, (GCopyFunc)g_strdup, NULL);
        _metadata_remove_metadata_from_list(&undometadata->after, metadata);
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
  }
}

void dt_metadata_set(const dt_imgid_t imgid, const char *key, const char *value, const gboolean undo_on)
{
  if(!key || !imgid) return;

  int keyid = dt_metadata_get_keyid(key);
  if(keyid != -1) // known key
  {
    GList *imgs = NULL;
    if(!dt_is_valid_imgid(imgid))
      imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
    else
      imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgid));
    if(imgs)
    {
      GList *undo = NULL;
      if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

      const gchar *ckey = g_strdup_printf("%d", keyid);
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
    }
  }
}

void dt_metadata_set_import(const dt_imgid_t imgid, const char *key, const char *value)
{
  if(!key || !dt_is_valid_imgid(imgid)) return;

  const int keyid = dt_metadata_get_keyid(key);

  if(keyid != -1) // known key
  {
    gboolean imported = (dt_image_get_xmp_mode() != DT_WRITE_XMP_NEVER);
    if(!imported && dt_metadata_get_type(keyid) != DT_METADATA_TYPE_INTERNAL)
    {
      const gchar *name = dt_metadata_get_name(keyid);
      char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
      imported = dt_conf_get_int(setting) & DT_METADATA_FLAG_IMPORTED;
      g_free(setting);
    }
    if(imported)
    {
      GList *imgs = NULL;
      imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgid));
      if(imgs)
      {
        GList *undo = NULL;

        const gchar *ckey = g_strdup_printf("%d", keyid);
        const gchar *cvalue = _cleanup_metadata_value(value);
        GList *metadata = NULL;
        metadata = g_list_append(metadata, (gpointer)ckey);
        metadata = g_list_append(metadata, (gpointer)cvalue);

        _metadata_execute(imgs, metadata, &undo, FALSE, DT_MA_ADD);

        g_list_free_full(metadata, g_free);
        g_list_free(imgs);
      }
    }
  }
}

void dt_metadata_set_list(const GList *imgs, GList *key_value, const gboolean undo_on)
{
  GList *metadata = NULL;
  GList *kv = key_value;
  while(kv)
  {
    const gchar *key = (const gchar *)kv->data;
    const int keyid = dt_metadata_get_keyid(key);
    if(keyid != -1) // known key
    {
      const gchar *ckey = g_strdup_printf("%d", keyid);
      kv = g_list_next(kv);
      const gchar *value = (const gchar *)kv->data;
      kv = g_list_next(kv);
      if(value)
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

  if(metadata && imgs)
  {
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

    _metadata_execute(imgs, metadata, &undo, undo_on, DT_MA_ADD);

    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_METADATA, undo, _pop_undo, _metadata_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }

    g_list_free_full(metadata, g_free);
  }
}

void dt_metadata_clear(const GList *imgs, const gboolean undo_on)
{
  // do not clear internal or hidden metadata
  GList *metadata = NULL;
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(dt_metadata_get_type(i) != DT_METADATA_TYPE_INTERNAL)
    {
      const gchar *name = dt_metadata_get_name(i);
      char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
      const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
      g_free(setting);
      if(!hidden)
      {
        // caution: metadata is a simple list here
        metadata = g_list_prepend(metadata, g_strdup_printf("%u", i));
      }
    }
  }

  if(metadata)
  {
    metadata = g_list_reverse(metadata);  // list was built in reverse order, so un-reverse it
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

    _metadata_execute(imgs, metadata, &undo, undo_on, DT_MA_REMOVE);

    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_METADATA, undo, _pop_undo, _metadata_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }

    g_list_free_full(metadata, g_free);
  }
}

void dt_metadata_set_list_id(const GList *img, const GList *metadata, const gboolean clear_on,
                             const gboolean undo_on)
{
  if(img)
  {
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);

    _metadata_execute(img, metadata, &undo, undo_on, clear_on ? DT_MA_SET : DT_MA_ADD);

    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_METADATA, undo, _pop_undo, _metadata_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }
  }
}

gboolean dt_metadata_already_imported(const char *filename, const char *datetime)
{
  if(!filename || !datetime)
    return FALSE;
  char *id = g_strconcat(filename, "-", datetime, NULL);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.meta_data WHERE value=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, id, -1, SQLITE_TRANSIENT);
  gboolean res = FALSE;
  if(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0)
    res = TRUE;
  sqlite3_finalize(stmt);
  g_free(id);
  return res;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
