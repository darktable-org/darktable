/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

/* A module's flexi mask as presets and styles store it, in their
 * blendop_params column: the fixed dt_develop_blend_params_t, followed by a
 * tail holding the forms of the mask group tree that mask_id names. A blob
 * without a tail is the plain struct, as stored before.
 *
 * The tail, in host byte order like the struct before it:
 *
 *   char     magic[4]          "dtmf"
 *   uint32_t format            DT_MASKS_BLOB_FORMAT
 *   uint32_t count             forms that follow, the mask group first
 *   count times:
 *     int32_t  formid
 *     uint32_t type            dt_masks_type_t
 *     int32_t  version         masks version the points are stored at
 *     int32_t  points_count
 *     uint32_t points_bytes
 *     uint32_t name_bytes      the name, without its terminating NUL
 *     float    source[3]
 *     char     name[name_bytes]
 *     uint8_t  points[points_bytes]
 *
 * A form record is a main.masks_history row, and is read by the same code
 * (dt_masks_form_from_stored()). The blob comes from .dtpreset and .dtstyle
 * files too, so the decoder takes nothing on trust: a tail that does not parse
 * exactly to the end of the blob rejects the whole blob.
 *
 * Form ids are the source image's. Whoever adds the forms to an image gives
 * them fresh ones (dt_masks_blend_blob_remap()), so that applying a preset
 * twice, or to two instances, never shares a form. */

#include "common/darktable.h"
#include "common/debug.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"

#define DT_MASKS_BLOB_MAGIC "dtmf"
#define DT_MASKS_BLOB_FORMAT 1
// a mask tree is nested at most DT_MASKS_NESTING_MAX deep and is edited by
// hand: anything near this is not a mask
#define DT_MASKS_BLOB_MAX_FORMS 4096

typedef struct _blob_header_t
{
  char magic[4];
  uint32_t format;
  uint32_t count;
} _blob_header_t;

typedef struct _blob_form_t
{
  int32_t formid;
  uint32_t type;
  int32_t version;
  int32_t points_count;
  uint32_t points_bytes;
  uint32_t name_bytes;
  float source[3];
} _blob_form_t;

// the form kinds; a stored type holds exactly one of them
static const uint32_t _kinds = DT_MASKS_CIRCLE | DT_MASKS_PATH | DT_MASKS_GROUP
                               | DT_MASKS_GRADIENT | DT_MASKS_ELLIPSE | DT_MASKS_BRUSH
                               | DT_MASKS_OBJECT | DT_MASKS_PARAMETRIC | DT_MASKS_RASTER;

static void _collect_tree(GList *forms,
                          const dt_mask_id_t id,
                          GHashTable *seen,
                          GList **out,
                          const int depth)
{
  if(depth > DT_MASKS_NESTING_MAX || !dt_is_valid_maskid(id)
     || g_hash_table_contains(seen, GINT_TO_POINTER(id)))
    return;
  dt_masks_form_t *form = dt_masks_get_from_id_ext(forms, id);
  if(!form) return;
  g_hash_table_add(seen, GINT_TO_POINTER(id));
  *out = g_list_append(*out, form);
  if(!(form->type & DT_MASKS_GROUP)) return;
  for(const GList *l = form->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(!dt_masks_point_is_marker(pt)) _collect_tree(forms, pt->formid, seen, out, depth + 1);
  }
}

GList *dt_masks_tree_forms(GList *forms, const dt_mask_id_t mask_id)
{
  GHashTable *seen = g_hash_table_new(NULL, NULL);
  GList *out = NULL;
  _collect_tree(forms, mask_id, seen, &out, 0);
  g_hash_table_destroy(seen);
  return out;
}

gboolean dt_masks_tree_has_pixel_forms(GList *forms, const dt_mask_id_t mask_id)
{
  GList *tree = dt_masks_tree_forms(forms, mask_id);
  gboolean found = FALSE;
  for(const GList *l = tree; l && !found; l = g_list_next(l))
    found = (((const dt_masks_form_t *)l->data)->type & DT_MASKS_PIXEL_FORMS) != 0;
  g_list_free(tree);
  return found;
}

static gboolean _carries_tree(const dt_develop_blend_params_t *bp, GList *forms)
{
  if(!(bp->mask_mode & DEVELOP_MASK_FLEXI) || !dt_is_valid_maskid(bp->mask_id)) return FALSE;
  const dt_masks_form_t *grp = dt_masks_get_from_id_ext(forms, bp->mask_id);
  return grp && (grp->type & DT_MASKS_GROUP);
}

void *dt_masks_blend_blob_encode(const dt_develop_blend_params_t *bp,
                                 GList *forms,
                                 size_t *size)
{
  GByteArray *out = g_byte_array_new();
  g_byte_array_append(out, (const guint8 *)bp, sizeof(*bp));

  GList *tree = _carries_tree(bp, forms) ? dt_masks_tree_forms(forms, bp->mask_id) : NULL;

  // a form stored as pixels refers to its own image's sidecar, so on any
  // other image it would only show as missing: it is left out, and so are
  // the group members referring to it
  GHashTable *dropped = g_hash_table_new(NULL, NULL);
  for(GList *l = tree; l;)
  {
    GList *next = g_list_next(l);
    const dt_masks_form_t *form = l->data;
    if(form->type & DT_MASKS_PIXEL_FORMS)
    {
      g_hash_table_add(dropped, GINT_TO_POINTER(form->formid));
      tree = g_list_delete_link(tree, l);
    }
    l = next;
  }

  if(tree)
  {
    _blob_header_t header = { .format = DT_MASKS_BLOB_FORMAT,
                              .count = g_list_length(tree) };
    memcpy(header.magic, DT_MASKS_BLOB_MAGIC, sizeof(header.magic));
    g_byte_array_append(out, (const guint8 *)&header, sizeof(header));

    for(const GList *l = tree; l; l = g_list_next(l))
    {
      const dt_masks_form_t *form = l->data;
      const size_t point_size = form->functions ? form->functions->point_struct_size : 0;
      GList *points = NULL;
      for(GList *p = point_size ? form->points : NULL; p; p = g_list_next(p))
      {
        const dt_masks_point_group_t *pt = p->data;
        if(!(form->type & DT_MASKS_GROUP) || dt_masks_point_is_marker(pt)
           || !g_hash_table_contains(dropped, GINT_TO_POINTER(pt->formid)))
          points = g_list_prepend(points, p->data);
      }
      points = g_list_reverse(points);
      const guint nb = g_list_length(points);
      _blob_form_t rec = { .formid = form->formid,
                           .type = form->type,
                           .version = form->version,
                           .points_count = nb,
                           .points_bytes = nb * point_size,
                           .name_bytes = strnlen(form->name, sizeof(form->name) - 1) };
      memcpy(rec.source, form->source, sizeof(rec.source));
      g_byte_array_append(out, (const guint8 *)&rec, sizeof(rec));
      g_byte_array_append(out, (const guint8 *)form->name, rec.name_bytes);
      for(const GList *p = points; p; p = g_list_next(p))
        g_byte_array_append(out, (const guint8 *)p->data, point_size);
      g_list_free(points);
    }
    g_list_free(tree);
  }
  g_hash_table_destroy(dropped);

  *size = out->len;
  return g_byte_array_free(out, FALSE);
}

gboolean dt_masks_blend_blob_has_tail(const size_t size)
{
  return size > sizeof(dt_develop_blend_params_t);
}

static gboolean _malformed(const char *why)
{
  dt_print(DT_DEBUG_ALWAYS, "[masks] stored blend params rejected: %s", why);
  return FALSE;
}

gboolean dt_masks_blend_blob_decode(const void *blob,
                                    const size_t size,
                                    dt_develop_blend_params_t *bp,
                                    GList **forms)
{
  *forms = NULL;
  if(!blob || size < sizeof(*bp)) return _malformed("shorter than the blend params");
  memcpy(bp, blob, sizeof(*bp));
  if(size == sizeof(*bp)) return TRUE;

  const uint8_t *p = (const uint8_t *)blob + sizeof(*bp);
  size_t left = size - sizeof(*bp);

  _blob_header_t header;
  if(left < sizeof(header)) return _malformed("truncated mask header");
  memcpy(&header, p, sizeof(header));
  p += sizeof(header);
  left -= sizeof(header);
  if(memcmp(header.magic, DT_MASKS_BLOB_MAGIC, sizeof(header.magic)))
    return _malformed("unknown data after the blend params");
  if(header.format != DT_MASKS_BLOB_FORMAT) return _malformed("unknown mask format");
  if(header.count == 0 || header.count > DT_MASKS_BLOB_MAX_FORMS)
    return _malformed("implausible number of mask forms");

  GHashTable *ids = g_hash_table_new(NULL, NULL);
  GList *out = NULL;
  gboolean ok = TRUE;
  for(uint32_t i = 0; ok && i < header.count; i++)
  {
    _blob_form_t rec;
    if(left < sizeof(rec)) { ok = _malformed("truncated mask form"); break; }
    memcpy(&rec, p, sizeof(rec));
    p += sizeof(rec);
    left -= sizeof(rec);

    if(rec.name_bytes >= sizeof(((dt_masks_form_t *)NULL)->name)
       || rec.name_bytes > left || rec.points_bytes > left - rec.name_bytes)
    {
      ok = _malformed("mask form longer than the data");
      break;
    }
    // one kind of form, read by its own point struct (dt_masks_create())
    const uint32_t kind = rec.type & _kinds;
    if(!kind || (kind & (kind - 1)))
    {
      ok = _malformed("mask form of no single kind");
      break;
    }
    if(!dt_is_valid_maskid(rec.formid) || g_hash_table_contains(ids, GINT_TO_POINTER(rec.formid)))
    {
      ok = _malformed("mask form id invalid or repeated");
      break;
    }

    char name[sizeof(((dt_masks_form_t *)NULL)->name)] = { 0 };
    memcpy(name, p, rec.name_bytes);
    p += rec.name_bytes;
    left -= rec.name_bytes;

    // no dev: forms stored before masks v3 need the image, and are refused
    dt_masks_form_t *form = dt_masks_form_from_stored(NULL, NO_IMGID, rec.formid, rec.type,
                                                      name, rec.version,
                                                      rec.points_bytes ? p : NULL,
                                                      rec.points_bytes, rec.points_count,
                                                      rec.source, sizeof(rec.source));
    p += rec.points_bytes;
    left -= rec.points_bytes;
    if(!form) { ok = _malformed("mask form unreadable"); break; }

    g_hash_table_add(ids, GINT_TO_POINTER(rec.formid));
    out = g_list_append(out, form);
  }
  g_hash_table_destroy(ids);

  if(ok && left) ok = _malformed("data after the mask forms");
  if(ok)
  {
    // the tree of the mask group: the group first, as encoded
    const dt_masks_form_t *grp = out->data;
    if(!(bp->mask_mode & DEVELOP_MASK_FLEXI) || grp->formid != bp->mask_id
       || !(grp->type & DT_MASKS_GROUP))
      ok = _malformed("mask forms that are not the module's mask group");
  }

  if(!ok)
  {
    g_list_free_full(out, (GDestroyNotify)dt_masks_free_form);
    return FALSE;
  }
  *forms = out;
  return TRUE;
}

static gboolean _id_in_use(GList *taken, GHashTable *taken_ids, GHashTable *map,
                           const dt_mask_id_t id)
{
  if(taken_ids && g_hash_table_contains(taken_ids, GINT_TO_POINTER(id))) return TRUE;
  // an id given in this remap is a value of the map
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init(&it, map);
  while(g_hash_table_iter_next(&it, &key, &value))
    if(GPOINTER_TO_INT(value) == id) return TRUE;
  return dt_masks_get_from_id_ext(taken, id) != NULL;
}

static dt_mask_id_t _map_id(GHashTable *map, GList *taken, GHashTable *taken_ids,
                            const dt_mask_id_t old)
{
  if(!dt_is_valid_maskid(old)) return old;
  gpointer found = NULL;
  if(g_hash_table_lookup_extended(map, GINT_TO_POINTER(old), NULL, &found))
    return GPOINTER_TO_INT(found);
  // dt_masks_new_marker_id() also avoids the marker ids in `taken`
  dt_mask_id_t id = dt_masks_new_marker_id(taken);
  while(_id_in_use(taken, taken_ids, map, id)) id = dt_masks_new_marker_id(taken);
  g_hash_table_insert(map, GINT_TO_POINTER(old), GINT_TO_POINTER(id));
  if(taken_ids) g_hash_table_add(taken_ids, GINT_TO_POINTER(id));
  return id;
}

void dt_masks_blend_blob_remap(GList *forms,
                               dt_develop_blend_params_t *bp,
                               GList *taken,
                               GHashTable *taken_ids)
{
  GHashTable *map = g_hash_table_new(NULL, NULL);
  for(GList *l = forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *form = l->data;
    form->formid = _map_id(map, taken, taken_ids, form->formid);
    if(!(form->type & DT_MASKS_GROUP)) continue;
    // members, markers and the parent: every id a group point holds
    for(GList *p = form->points; p; p = g_list_next(p))
    {
      dt_masks_point_group_t *pt = p->data;
      pt->formid = _map_id(map, taken, taken_ids, pt->formid);
      pt->parentid = _map_id(map, taken, taken_ids, pt->parentid);
    }
  }
  if(bp) bp->mask_id = _map_id(map, taken, taken_ids, bp->mask_id);
  g_hash_table_destroy(map);
}

GList *dt_masks_read_forms_at(const dt_imgid_t imgid, const int num)
{
  GList *forms = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "SELECT formid, form, name, version, points, points_count, source"
     " FROM main.masks_history"
     " WHERE imgid = ?1"
     "   AND num = (SELECT MAX(num) FROM main.masks_history"
     "              WHERE imgid = ?1 AND num <= ?2)",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const dt_mask_id_t formid = sqlite3_column_int(stmt, 0);
    const dt_masks_type_t type = sqlite3_column_int(stmt, 1);
    const char *name = (const char *)sqlite3_column_text(stmt, 2);
    const int version = sqlite3_column_int(stmt, 3);
    const void *points = sqlite3_column_blob(stmt, 4);
    const int points_bytes = sqlite3_column_bytes(stmt, 4);
    const int nb_points = sqlite3_column_int(stmt, 5);
    const void *source = sqlite3_column_blob(stmt, 6);
    const int source_bytes = sqlite3_column_bytes(stmt, 6);
    dt_masks_form_t *form = dt_masks_form_from_stored
      (NULL, imgid, formid, type, name, version,
       points, points_bytes, nb_points, source, source_bytes);
    if(form) forms = g_list_append(forms, form);
  }
  sqlite3_finalize(stmt);
  return forms;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
