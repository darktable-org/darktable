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

// Presets and styles carrying a flexi mask: the forms of the mask tree after
// the blend params in blendop_params (masks/blend_blob.c), and what reads them
// back: preset apply, auto-apply, style apply, .dtpreset and .dtstyle files.
//
// And the presets and styles made before flexi masks, which carry a classic
// mask_mode and no forms: whichever way they are applied, they must give the
// mask the same edit stored as a history row migrates to.
//
// Trees are compared through a canonical form: the tree's forms with their ids
// renumbered in walk order, encoded. Equal canonical forms are the same mask,
// as ids only link the forms to each other.

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include "catalog_fixture.h"
#include "common/database.h"
#include "common/history.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/presets.h"
#include "common/styles.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/masks/scratch_image.h"
#include "gui/presets.h"

#include <glib/gstdio.h>

#define OP "colorbalancergb"

static void _open(dt_develop_t *dev, const dt_imgid_t imgid)
{
  dt_dev_init(dev, FALSE);
  dt_dev_load_image(dev, imgid);
}

static dt_iop_module_t *_module(const dt_develop_t *dev)
{
  for(GList *l = dev->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    if(dt_iop_module_is(m, OP) && m->multi_priority == 0) return m;
  }
  return NULL;
}

// the last history item of OP, NULL when there is none
static const dt_dev_history_item_t *_last_item(const dt_develop_t *dev)
{
  const dt_dev_history_item_t *item = NULL;
  for(GList *l = dev->history; l; l = g_list_next(l))
  {
    const dt_dev_history_item_t *h = l->data;
    if(!strcmp(h->op_name, OP)) item = h;
  }
  return item;
}

// stores the module's state as a history item with the forms, as preset and
// style apply record it, and writes the history
static void _commit(dt_develop_t *dev, dt_iop_module_t *module)
{
  dt_dev_add_masks_history_item_ext(dev, module, FALSE, TRUE);
  dt_dev_write_history_ext(dev, dev->image_storage.id);
}

static void _classic_green(dt_develop_blend_params_t *bp)
{
  dt_develop_blend_init_blend_parameters(bp, DEVELOP_BLEND_CS_RGB_SCENE);
  bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  bp->blendif = 1u << DEVELOP_BLENDIF_hz_in;
  float *const p = bp->blendif_parameters + 4 * DEVELOP_BLENDIF_hz_in;
  p[0] = 0.30f;
  p[1] = 0.35f;
  p[2] = 0.45f;
  p[3] = 0.50f;
}

static dt_masks_point_group_t *_member(const dt_mask_id_t formid, const dt_mask_id_t parentid)
{
  dt_masks_point_group_t *pt = calloc(1, sizeof(dt_masks_point_group_t));
  pt->formid = formid;
  pt->parentid = parentid;
  pt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION;
  pt->opacity = 1.0f;
  pt->group_opacity = 1.0f;
  return pt;
}

// a flexi mask with something of every kind of id in it: the parametric
// group a classic "select green" migrates to, plus a nested group, with its
// marker, holding a circle
static void _build_mask(dt_develop_t *dev, dt_iop_module_t *module)
{
  dt_develop_blend_params_t bp;
  _classic_green(&bp);
  dt_gui_presets_apply_blend_params(module, &bp, 14, sizeof(bp));
  dt_masks_form_t *grp = dt_masks_get_from_id(dev, module->blend_params->mask_id);
  assert_non_null(grp);

  dt_masks_form_t *circle = dt_masks_create(DT_MASKS_CIRCLE);
  dt_masks_point_circle_t *c = calloc(1, sizeof(dt_masks_point_circle_t));
  c->center[0] = 0.4f;
  c->center[1] = 0.6f;
  c->radius = 0.1f;
  c->border = 0.05f;
  circle->points = g_list_append(NULL, c);
  g_strlcpy(circle->name, "test circle", sizeof(circle->name));
  dev->forms = g_list_append(dev->forms, circle);

  dt_masks_form_t *nested = dt_masks_create(DT_MASKS_GROUP);
  g_strlcpy(nested->name, "test nested", sizeof(nested->name));
  dev->forms = g_list_append(dev->forms, nested);
  nested->points = g_list_append(NULL, dt_masks_marker_new(dev->forms, nested, 0));
  nested->points = g_list_append(nested->points, _member(circle->formid, nested->formid));

  grp->points = g_list_append(grp->points, _member(nested->formid, grp->formid));
}

// every id the tree of `mask_id` holds: forms, markers and members
static GHashTable *_tree_ids(GList *forms, const dt_mask_id_t mask_id)
{
  GHashTable *ids = g_hash_table_new(NULL, NULL);
  GList *tree = dt_masks_tree_forms(forms, mask_id);
  for(GList *l = tree; l; l = g_list_next(l))
  {
    const dt_masks_form_t *f = l->data;
    g_hash_table_add(ids, GINT_TO_POINTER(f->formid));
    if(f->type & DT_MASKS_GROUP)
      for(GList *p = f->points; p; p = g_list_next(p))
        g_hash_table_add(ids, GINT_TO_POINTER(((dt_masks_point_group_t *)p->data)->formid));
  }
  g_list_free(tree);
  return ids;
}

static gboolean _disjoint(GHashTable *a, GHashTable *b)
{
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init(&it, a);
  while(g_hash_table_iter_next(&it, &key, NULL))
    if(g_hash_table_contains(b, key)) return FALSE;
  return TRUE;
}

static dt_mask_id_t _canon_id(GHashTable *map, const dt_mask_id_t id)
{
  if(!dt_is_valid_maskid(id)) return id;
  gpointer found = g_hash_table_lookup(map, GINT_TO_POINTER(id));
  if(found) return GPOINTER_TO_INT(found);
  const dt_mask_id_t canon = 1000 + g_hash_table_size(map);
  g_hash_table_insert(map, GINT_TO_POINTER(id), GINT_TO_POINTER(canon));
  return canon;
}

// the mask `bp` renders with `forms`, ids renumbered in walk order
static GBytes *_canon(GList *forms, const dt_develop_blend_params_t *bp_in)
{
  dt_develop_blend_params_t bp = *bp_in;
  GList *tree = dt_masks_tree_forms(forms, bp.mask_id);
  GList *copies = NULL;
  GHashTable *map = g_hash_table_new(NULL, NULL);
  bp.mask_id = _canon_id(map, bp.mask_id);
  for(GList *l = tree; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = dt_masks_dup_masks_form(l->data);
    f->formid = _canon_id(map, f->formid);
    if(f->type & DT_MASKS_GROUP)
      for(GList *p = f->points; p; p = g_list_next(p))
      {
        dt_masks_point_group_t *pt = p->data;
        pt->formid = _canon_id(map, pt->formid);
        pt->parentid = _canon_id(map, pt->parentid);
      }
    copies = g_list_append(copies, f);
  }
  g_list_free(tree);
  g_hash_table_destroy(map);

  size_t size = 0;
  void *blob = dt_masks_blend_blob_encode(&bp, copies, &size);
  g_list_free_full(copies, (GDestroyNotify)dt_masks_free_form);
  return g_bytes_new_take(blob, size);
}

// the mask the last item of OP renders with at the end of the history: with
// the forms of the snapshot current there, which need not be the item's own.
// An image's first load prepends the auto-applied modules to a history a
// style was applied to, and moves the history items but not the masks
// history rows (develop.c, _dev_merge_history)
static GBytes *_canon_item(const dt_develop_t *dev)
{
  return _canon(dev->forms, _last_item(dev)->blend_params);
}

static void _insert_preset(const char *name,
                           const void *blob,
                           const size_t size,
                           const int blendop_version,
                           const gboolean autoapply,
                           const char *maker)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT OR REPLACE INTO data.presets"
                              " (name, description, operation, op_version, op_params, enabled,"
                              "  blendop_params, blendop_version, multi_priority, multi_name,"
                              "  multi_name_hand_edited, model, maker, lens,"
                              "  iso_min, iso_max, exposure_min, exposure_max,"
                              "  aperture_min, aperture_max, focal_length_min, focal_length_max,"
                              "  writeprotect, autoapply, filter, def, format)"
                              " VALUES (?1, '', ?2, ?3, NULL, 1, ?4, ?5, 0, '', 0, '%', ?6, '%',"
                              "         0, 340000000, 0, 10000000, 0, 10000000, 0, 1000000,"
                              "         0, ?7, 0, 0, 0)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, OP, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, dt_iop_get_module_so(OP)->version());
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, blob, size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, blendop_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, autoapply);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static GBytes *_select_blob(const char *query, const char *name)
{
  GBytes *out = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    out = g_bytes_new(sqlite3_column_blob(stmt, 0), sqlite3_column_bytes(stmt, 0));
  sqlite3_finalize(stmt);
  return out;
}

#define PRESET_BLOB_QUERY \
  "SELECT blendop_params FROM data.presets WHERE operation = '" OP "' AND name = ?1"
// the style's last item of OP
#define STYLE_BLOB_QUERY \
  "SELECT i.blendop_params FROM data.style_items i JOIN data.styles s ON s.id = i.styleid" \
  " WHERE s.name = ?1 AND i.operation = '" OP "' ORDER BY i.num DESC LIMIT 1"

// the source image's mask, built once: its canonical form, the ids it uses, and
// the blob a preset saved from it stores
static dt_imgid_t _src = NO_IMGID;
static GBytes *_src_canon = NULL;
static GHashTable *_src_ids = NULL;
static GBytes *_src_blob = NULL;

static void _ensure_source(void)
{
  if(_src_canon) return;
  _src = dt_image_duplicate(dt_test_catalog_image("mire1.cr2"));
  dt_develop_t dev;
  _open(&dev, _src);
  dt_iop_module_t *module = _module(&dev);
  _build_mask(&dev, module);
  _commit(&dev, module);
  _src_canon = _canon(dev.forms, module->blend_params);
  _src_ids = _tree_ids(dev.forms, module->blend_params->mask_id);
  size_t size = 0;
  void *blob = dt_gui_presets_module_blend_blob(module, &size);
  _src_blob = g_bytes_new_take(blob, size);
  dt_dev_cleanup(&dev);
}

// ---------------------------------------------------------------------------
// the format
// ---------------------------------------------------------------------------

static void test_blob_roundtrip(void **state)
{
  _ensure_source();
  gsize size = 0;
  const void *blob = g_bytes_get_data(_src_blob, &size);
  assert_true(size > sizeof(dt_develop_blend_params_t));

  dt_develop_blend_params_t bp;
  GList *forms = NULL;
  assert_true(dt_masks_blend_blob_decode(blob, size, &bp, &forms));
  // the root group, the parametric element, the nested group and its circle
  assert_int_equal(g_list_length(forms), 4);
  GBytes *canon = _canon(forms, &bp);
  assert_true(g_bytes_equal(canon, _src_canon));
  g_bytes_unref(canon);

  // re-encoding the decoded forms gives the same bytes
  size_t again_size = 0;
  void *again = dt_masks_blend_blob_encode(&bp, forms, &again_size);
  assert_int_equal(again_size, size);
  assert_memory_equal(again, blob, size);
  g_free(again);
  g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);

  // no tail when there is no flexi mask group to carry
  dt_develop_blend_params_t plain;
  _classic_green(&plain);
  size_t plain_size = 0;
  void *p = dt_masks_blend_blob_encode(&plain, NULL, &plain_size);
  assert_int_equal(plain_size, sizeof(plain));
  assert_true(dt_masks_blend_blob_decode(p, plain_size, &bp, &forms));
  assert_null(forms);
  assert_memory_equal(&bp, &plain, sizeof(bp));
  g_free(p);
}

// an AI object stored as pixels renders only on its own image, so saving one
// in a preset or a style asks; one traced into paths is ordinary shapes
static void test_pixel_forms_found(void **state)
{
  _ensure_source();
  gsize size = 0;
  const void *blob = g_bytes_get_data(_src_blob, &size);
  dt_develop_blend_params_t bp;
  GList *forms = NULL;
  assert_true(dt_masks_blend_blob_decode(blob, size, &bp, &forms));
  assert_false(dt_masks_tree_has_pixel_forms(forms, bp.mask_id));

  // the nested group gets an object beside its circle
  dt_masks_form_t *nested = NULL;
  for(GList *l = forms->next; l && !nested; l = g_list_next(l))
    if(((dt_masks_form_t *)l->data)->type & DT_MASKS_GROUP) nested = l->data;
  assert_non_null(nested);
  dt_masks_form_t *object = dt_masks_create(DT_MASKS_OBJECT);
  forms = g_list_append(forms, object);
  nested->points = g_list_append(nested->points, _member(object->formid, nested->formid));
  assert_true(dt_masks_tree_has_pixel_forms(forms, bp.mask_id));
  // outside the tree it does not count
  assert_false(dt_masks_tree_has_pixel_forms(forms, NO_MASKID));
  g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);
}

// a fresh image whose mask, as _build_mask() makes it, also holds an AI object
// stored as pixels, recorded in its history
static dt_imgid_t _image_with_pixel_object(void)
{
  const dt_imgid_t imgid = dt_image_duplicate(dt_test_catalog_image("bayer4.crw"));
  dt_develop_t dev;
  _open(&dev, imgid);
  dt_iop_module_t *module = _module(&dev);
  _build_mask(&dev, module);
  _commit(&dev, module);
  // a later history item of the same module, as an edit after a focus change
  dev.focus_hash++;
  dt_masks_form_t *grp = dt_masks_get_from_id(&dev, module->blend_params->mask_id);
  dt_masks_form_t *object = dt_masks_create(DT_MASKS_OBJECT);
  object->points = g_list_append(NULL, calloc(1, object->functions->point_struct_size));
  dev.forms = g_list_append(dev.forms, object);
  grp->points = g_list_append(grp->points, _member(object->formid, grp->formid));
  _commit(&dev, module);
  dt_dev_cleanup(&dev);
  return imgid;
}

// a style made from an image whose mask holds an AI object stored as pixels:
// the styles dialog asks first, naming the module whose mask holds it
// (dt_styles_history_pixel_mask_modules()) among the history items it lists,
// and the style then leaves the object out, with the
// group member referring to it, and keeps the rest of the mask
static void test_style_from_pixel_object(void **state)
{
  _ensure_source();
  const dt_imgid_t imgid = _image_with_pixel_object();

  // the items the styles dialog lists and passes on
  GList *items = dt_history_get_items(imgid, FALSE, TRUE, TRUE);
  GList *all = NULL, *others = NULL;
  for(GList *l = items; l; l = g_list_next(l))
  {
    const dt_history_item_t *it = l->data;
    all = g_list_append(all, GINT_TO_POINTER(it->num));
    if(strcmp(it->op, OP)) others = g_list_append(others, GINT_TO_POINTER(it->num));
  }
  g_list_free_full(items, dt_history_item_free);

  // as the history stack names it
  gchar *modules = dt_styles_history_pixel_mask_modules(imgid, all);
  assert_string_equal(modules, dt_iop_get_localized_name(OP));
  g_free(modules);
  modules = dt_styles_history_pixel_mask_modules(imgid, NULL);
  assert_string_equal(modules, dt_iop_get_localized_name(OP));
  g_free(modules);
  // without the module, or from an image with no object, nothing to ask
  assert_null(dt_styles_history_pixel_mask_modules(imgid, others));
  assert_null(dt_styles_history_pixel_mask_modules(_src, NULL));

  assert_true(dt_styles_create_from_image("test pixel style", "", imgid, all, FALSE));
  g_list_free(all);
  g_list_free(others);

  GBytes *item = _select_blob(STYLE_BLOB_QUERY, "test pixel style");
  assert_non_null(item);
  gsize size = 0;
  const void *blob = g_bytes_get_data(item, &size);
  dt_develop_blend_params_t bp;
  GList *forms = NULL;
  assert_true(dt_masks_blend_blob_decode(blob, size, &bp, &forms));
  assert_false(dt_masks_tree_has_pixel_forms(forms, bp.mask_id));
  // the root group, the parametric element, the nested group and its circle
  assert_int_equal(g_list_length(forms), 4);
  for(const GList *l = forms; l; l = g_list_next(l))
  {
    const dt_masks_form_t *f = l->data;
    if(!(f->type & DT_MASKS_GROUP)) continue;
    for(const GList *p = f->points; p; p = g_list_next(p))
    {
      const dt_masks_point_group_t *pt = p->data;
      if(!dt_masks_point_is_marker(pt))
        assert_non_null(dt_masks_get_from_id_ext(forms, pt->formid));
    }
  }
  g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);
  g_bytes_unref(item);
}

// every interactive save asks through dt_gui_presets_confirm_pixel_masks();
// without a GUI to ask in, as for import, Lua or the cli, it never refuses
static void test_pixel_object_save_headless(void **state)
{
  const dt_imgid_t imgid = _image_with_pixel_object();
  dt_develop_t dev;
  _open(&dev, imgid);
  dt_iop_module_t *module = _module(&dev);
  // the module as the darkroom holds it: a headless load leaves it at defaults
  memcpy(module->blend_params, _last_item(&dev)->blend_params,
         sizeof(dt_develop_blend_params_t));
  assert_true(dt_masks_tree_has_pixel_forms(dev.forms, module->blend_params->mask_id));
  assert_true(dt_gui_presets_confirm_module_mask(module));
  assert_true(dt_gui_presets_confirm_pixel_masks("exposure"));

  // and the preset leaves the object out, as a style does
  size_t size = 0;
  void *blob = dt_gui_presets_module_blend_blob(module, &size);
  dt_develop_blend_params_t bp;
  GList *forms = NULL;
  assert_true(dt_masks_blend_blob_decode(blob, size, &bp, &forms));
  assert_false(dt_masks_tree_has_pixel_forms(forms, bp.mask_id));
  assert_int_equal(g_list_length(forms), 4);
  g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);
  g_free(blob);
  dt_dev_cleanup(&dev);
}

static gboolean _decodes(const void *blob, const size_t size)
{
  dt_develop_blend_params_t bp;
  GList *forms = NULL;
  const gboolean ok = dt_masks_blend_blob_decode(blob, size, &bp, &forms);
  if(!ok) assert_null(forms);
  g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);
  return ok;
}

// offsets in the tail, see the format in masks/blend_blob.c
#define TAIL sizeof(dt_develop_blend_params_t)
#define REC0 (TAIL + 12)

// the offset of form record `i`
static size_t _record(const guint8 *blob, const int i)
{
  size_t off = REC0;
  for(int k = 0; k < i; k++)
  {
    uint32_t points_bytes, name_bytes;
    memcpy(&points_bytes, blob + off + 16, sizeof(points_bytes));
    memcpy(&name_bytes, blob + off + 20, sizeof(name_bytes));
    off += 36 + name_bytes + points_bytes;
  }
  return off;
}

static void test_blob_malformed(void **state)
{
  _ensure_source();
  gsize size = 0;
  const guint8 *good = g_bytes_get_data(_src_blob, &size);
  guint8 *b = g_malloc(size + 16);

  // every truncation but the plain blend params, and anything appended
  for(size_t n = 0; n < size; n++)
    if(n != TAIL) assert_false(_decodes(good, n));
  memcpy(b, good, size);
  memset(b + size, 0, 16);
  assert_false(_decodes(b, size + 1));
  assert_false(_decodes(b, size + 16));

#define MUTATE(offset, type, value)                    \
  do                                                   \
  {                                                    \
    memcpy(b, good, size);                             \
    const type v = (value);                            \
    memcpy(b + (offset), &v, sizeof(v));               \
    assert_false(_decodes(b, size));                   \
  } while(0)

  MUTATE(TAIL, char, 'x');                              // magic
  MUTATE(TAIL + 4, uint32_t, 2);                        // format
  MUTATE(TAIL + 8, uint32_t, 0);                        // count
  MUTATE(TAIL + 8, uint32_t, 3);                        // count short of the forms
  MUTATE(TAIL + 8, uint32_t, 5);                        // count beyond them
  MUTATE(TAIL + 8, uint32_t, 0xffffffffu);              // implausible count
  MUTATE(REC0 + 0, int32_t, 0);                         // invalid form id
  MUTATE(REC0 + 4, uint32_t, DT_MASKS_GROUP | DT_MASKS_CIRCLE); // two kinds
  MUTATE(REC0 + 4, uint32_t, DT_MASKS_CLONE);           // no kind
  MUTATE(REC0 + 4, uint32_t, DT_MASKS_CIRCLE);          // a group's points as a circle's
  MUTATE(REC0 + 8, int32_t, 99);                        // masks version from the future
  MUTATE(REC0 + 8, int32_t, 1);                         // one that needs the image
  MUTATE(REC0 + 12, int32_t, -1);                       // points count
  MUTATE(REC0 + 12, int32_t, 1000);                     // points count past the bytes
  MUTATE(REC0 + 16, uint32_t, 0xfffffff0u);             // points bytes
  MUTATE(REC0 + 20, uint32_t, 0xfffffff0u);             // name bytes
  MUTATE(REC0 + 20, uint32_t, 200);                     // name longer than a form's
  // the blend params name another group than the first form
  MUTATE(offsetof(dt_develop_blend_params_t, mask_id), int32_t, 77);
  // a classic mask with forms after it: nothing makes that
  MUTATE(offsetof(dt_develop_blend_params_t, mask_mode), uint32_t,
         DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL);

  // the same form id twice
  memcpy(b, good, size);
  memcpy(b + _record(good, 2), b + _record(good, 1), sizeof(int32_t));
  assert_false(_decodes(b, size));

#undef MUTATE
  g_free(b);
}

// ---------------------------------------------------------------------------
// presets and styles with a flexi mask
// ---------------------------------------------------------------------------

// saved on one image, applied on another: the same tree, under ids of its own;
// and a second apply shares nothing with the first
static void test_preset_apply_other_image(void **state)
{
  _ensure_source();
  gsize size = 0;
  const void *blob = g_bytes_get_data(_src_blob, &size);

  const dt_imgid_t dst = dt_image_duplicate(dt_test_catalog_image("bayer4.crw"));
  dt_develop_t dev;
  _open(&dev, dst);
  dt_iop_module_t *module = _module(&dev);

  dt_gui_presets_apply_blend_params(module, blob, dt_develop_blend_version(), size);
  GBytes *canon = _canon(dev.forms, module->blend_params);
  assert_true(g_bytes_equal(canon, _src_canon));
  g_bytes_unref(canon);
  GHashTable *first = _tree_ids(dev.forms, module->blend_params->mask_id);
  assert_true(_disjoint(first, _src_ids));
  assert_true(dt_masks_module_mask_in_forms(module, dev.forms));

  dt_gui_presets_apply_blend_params(module, blob, dt_develop_blend_version(), size);
  GHashTable *second = _tree_ids(dev.forms, module->blend_params->mask_id);
  assert_true(_disjoint(first, second));
  assert_true(_disjoint(second, _src_ids));
  canon = _canon(dev.forms, module->blend_params);
  assert_true(g_bytes_equal(canon, _src_canon));
  g_bytes_unref(canon);
  g_hash_table_destroy(first);
  g_hash_table_destroy(second);

  // recorded as preset apply records it, it is there on the next load
  _commit(&dev, module);
  dt_dev_cleanup(&dev);
  _open(&dev, dst);
  canon = _canon_item(&dev);
  assert_true(g_bytes_equal(canon, _src_canon));
  g_bytes_unref(canon);
  dt_dev_cleanup(&dev);
}

// auto-apply copies the preset's blob into history, tail and all
static void test_auto_apply_with_mask(void **state)
{
  _ensure_source();
  gsize size = 0;
  const void *blob = g_bytes_get_data(_src_blob, &size);
  _insert_preset("test flexi auto", blob, size, dt_develop_blend_version(), TRUE, "%sony%");

  const dt_imgid_t imgid = dt_test_catalog_image("hlrecovery.arw");
  for(int pass = 0; pass < 2; pass++)
  {
    dt_develop_t dev;
    _open(&dev, imgid);
    const dt_dev_history_item_t *item = _last_item(&dev);
    assert_non_null(item);
    GBytes *canon = _canon_item(&dev);
    assert_true(g_bytes_equal(canon, _src_canon));
    g_bytes_unref(canon);
    GHashTable *ids = _tree_ids(dev.forms, item->blend_params->mask_id);
    assert_true(_disjoint(ids, _src_ids));
    g_hash_table_destroy(ids);
    dt_dev_cleanup(&dev);
  }
}

static void test_style_with_mask(void **state)
{
  _ensure_source();
  assert_true(dt_styles_create_from_image("test flexi style", "", _src, NULL, FALSE));
  GBytes *item = _select_blob(STYLE_BLOB_QUERY, "test flexi style");
  assert_non_null(item);
  assert_true(g_bytes_get_size(item) > sizeof(dt_develop_blend_params_t));
  g_bytes_unref(item);

  const dt_imgid_t dst = dt_image_duplicate(dt_test_catalog_image("bayer4.crw"));
  dt_styles_apply_to_image("test flexi style", FALSE, FALSE, dst);

  for(int pass = 0; pass < 2; pass++)
  {
    dt_develop_t dev;
    _open(&dev, dst);
    const dt_dev_history_item_t *h = _last_item(&dev);
    GBytes *canon = _canon_item(&dev);
    assert_true(g_bytes_equal(canon, _src_canon));
    g_bytes_unref(canon);
    GHashTable *ids = _tree_ids(dev.forms, h->blend_params->mask_id);
    assert_true(_disjoint(ids, _src_ids));
    g_hash_table_destroy(ids);
    dt_dev_cleanup(&dev);
  }
}

static void test_files_roundtrip(void **state)
{
  _ensure_source();
  gsize size = 0;
  const void *blob = g_bytes_get_data(_src_blob, &size);
  _insert_preset("test flexi file", blob, size, dt_develop_blend_version(), FALSE, "%");

  gchar *dir = g_dir_make_tmp("dt-flexi-files-XXXXXX", NULL);
  assert_non_null(dir);

  // .dtpreset
  int rowid = -1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT rowid FROM data.presets WHERE name = 'test flexi file'",
                              -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) rowid = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  dt_presets_save_to_file(rowid, "test flexi file", dir);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE FROM data.presets WHERE name = 'test flexi file'", NULL, NULL, NULL);
  gchar *path = g_build_filename(dir, "test flexi file.dtpreset", NULL);
  assert_true(dt_presets_import_from_file(path));
  g_remove(path);
  g_free(path);
  GBytes *back = _select_blob(PRESET_BLOB_QUERY, "test flexi file");
  assert_non_null(back);
  assert_true(g_bytes_equal(back, _src_blob));
  g_bytes_unref(back);

  // .dtstyle
  if(!dt_styles_exists("test flexi style"))
    assert_true(dt_styles_create_from_image("test flexi style", "", _src, NULL, FALSE));
  GBytes *before = _select_blob(STYLE_BLOB_QUERY, "test flexi style");
  dt_styles_save_to_file("test flexi style", dir, TRUE);
  dt_styles_delete_by_name("test flexi style");
  path = g_build_filename(dir, "test flexi style.dtstyle", NULL);
  dt_styles_import_from_file(path);
  g_remove(path);
  g_free(path);
  GBytes *after = _select_blob(STYLE_BLOB_QUERY, "test flexi style");
  assert_non_null(after);
  assert_true(g_bytes_equal(before, after));
  assert_true(g_bytes_get_size(after) > sizeof(dt_develop_blend_params_t));
  g_bytes_unref(before);
  g_bytes_unref(after);

  g_rmdir(dir);
  g_free(dir);
}

// ---------------------------------------------------------------------------
// the masks history of a history the first load prepends to
// ---------------------------------------------------------------------------

// a style applied in the lighttable to an image never opened writes history
// rows and their forms snapshot; its first load then prepends the auto-applied
// modules. Every history position must render the forms it rendered before:
// none before the item that brought the mask, the mask from it on
static void test_masks_follow_prepended_history(void **state)
{
  _ensure_source();
  if(!dt_styles_exists("test flexi style"))
    assert_true(dt_styles_create_from_image("test flexi style", "", _src, NULL, FALSE));
  const dt_imgid_t imgid = dt_image_duplicate(dt_test_catalog_image("bayer4.crw"));
  dt_styles_apply_to_image("test flexi style", FALSE, FALSE, imgid);

  // the first load: auto-apply prepends, and the history is written back
  dt_develop_t dev;
  _open(&dev, imgid);
  const int history_end = dev.history_end;
  dt_dev_cleanup(&dev);

  int item_num = -1;
  dt_develop_blend_params_t bp;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT num, blendop_params FROM main.history"
                              " WHERE imgid = ?1 AND operation = '" OP "'"
                              " ORDER BY num DESC LIMIT 1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_bytes(stmt, 1) == sizeof(bp))
  {
    item_num = sqlite3_column_int(stmt, 0);
    memcpy(&bp, sqlite3_column_blob(stmt, 1), sizeof(bp));
  }
  sqlite3_finalize(stmt);
  assert_true(item_num > 0);
  assert_true(bp.mask_mode & DEVELOP_MASK_FLEXI);

  // position p renders the snapshot of the last item below it
  for(int p = 1; p <= history_end; p++)
  {
    GList *forms = dt_masks_read_forms_at(imgid, p - 1);
    const gboolean has_mask = dt_masks_get_from_id_ext(forms, bp.mask_id) != NULL;
    g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);
    if(has_mask != (p > item_num))
      fail_msg("history position %d of %d: mask %s, its item is num %d",
               p, history_end, has_mask ? "present" : "missing", item_num);
  }
}

// ---------------------------------------------------------------------------
// onto a locked mask
// ---------------------------------------------------------------------------

#define LOCK_STYLE "test flexi lock style"

// a fresh image whose module holds a locked "select green" mask
static dt_imgid_t _locked_image(dt_develop_t *dev, GBytes **canon, int *n_forms)
{
  const dt_imgid_t imgid = dt_image_duplicate(dt_test_catalog_image("bayer4.crw"));
  _open(dev, imgid);
  dt_iop_module_t *module = _module(dev);
  dt_develop_blend_params_t bp;
  _classic_green(&bp);
  dt_gui_presets_apply_blend_params(module, &bp, 14, sizeof(bp));
  module->blend_params->mask_lock = 1;
  *canon = _canon(dev->forms, module->blend_params);
  *n_forms = g_list_length(dev->forms);
  return imgid;
}

// the locked mask stays, and the incoming one leaves no form behind
static void _assert_kept(const dt_develop_t *dev,
                         const dt_develop_blend_params_t *bp,
                         GBytes *canon,
                         const int n_forms)
{
  assert_true(dt_develop_blend_mask_locked(bp));
  GBytes *now = _canon(dev->forms, bp);
  assert_true(g_bytes_equal(now, canon));
  g_bytes_unref(now);
  assert_int_equal(g_list_length(dev->forms), n_forms);
}

static void test_locked_mask_kept_on_preset_apply(void **state)
{
  _ensure_source();
  dt_develop_t dev;
  GBytes *canon = NULL;
  int n_forms = 0;
  _locked_image(&dev, &canon, &n_forms);
  dt_iop_module_t *module = _module(&dev);

  // a preset carrying forms
  gsize size = 0;
  const void *blob = g_bytes_get_data(_src_blob, &size);
  dt_gui_presets_apply_blend_params(module, blob, dt_develop_blend_version(), size);
  _assert_kept(&dev, module->blend_params, canon, n_forms);

  // a 5.6 preset, and the same as the startup upgrade of development builds
  // of 5.7 stored it: their migration must create no form either
  dt_develop_blend_params_t classic, stamped;
  _classic_green(&classic);
  classic.blendif |= 1u << DEVELOP_BLENDIF_Cz_in;
  dt_gui_presets_apply_blend_params(module, &classic, 14, sizeof(classic));
  _assert_kept(&dev, module->blend_params, canon, n_forms);
  assert_false(dt_develop_blend_legacy_params_from_so(dt_iop_get_module_so(OP), &classic, 14,
                                                      &stamped, dt_develop_blend_version(),
                                                      sizeof(classic)));
  dt_gui_presets_apply_blend_params(module, &stamped, dt_develop_blend_version(),
                                    sizeof(stamped));
  _assert_kept(&dev, module->blend_params, canon, n_forms);

  g_bytes_unref(canon);
  dt_dev_cleanup(&dev);
}

static void test_locked_mask_kept_on_style_apply(void **state)
{
  _ensure_source();
  if(!dt_styles_exists(LOCK_STYLE))
    assert_true(dt_styles_create_from_image(LOCK_STYLE, "", _src, NULL, FALSE));

  dt_develop_t dev;
  GBytes *canon = NULL;
  int n_forms = 0;
  const dt_imgid_t imgid = _locked_image(&dev, &canon, &n_forms);
  _commit(&dev, _module(&dev));
  dt_dev_cleanup(&dev);

  dt_styles_apply_to_image(LOCK_STYLE, FALSE, FALSE, imgid);

  _open(&dev, imgid);
  _assert_kept(&dev, _last_item(&dev)->blend_params, canon, n_forms);
  dt_dev_cleanup(&dev);

  // nor anywhere in the stored masks history
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(DISTINCT formid) FROM main.masks_history"
                              " WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  assert_int_equal(sqlite3_step(stmt), SQLITE_ROW);
  assert_int_equal(sqlite3_column_int(stmt, 0), n_forms);
  sqlite3_finalize(stmt);
  g_bytes_unref(canon);
}

// ---------------------------------------------------------------------------
// presets and styles from before flexi masks
// ---------------------------------------------------------------------------

typedef struct _classic_case_t
{
  const char *name;
  void (*make)(dt_develop_blend_params_t *bp);
} _classic_case_t;

// a mask_id that names no form: a preset never carried its shapes
#define DANGLING 123456

static void _case_parametric(dt_develop_blend_params_t *bp) { _classic_green(bp); }

static void _case_parametric_inverted(dt_develop_blend_params_t *bp)
{
  _classic_green(bp);
  bp->mask_combine |= DEVELOP_COMBINE_INV;
}

static void _case_raster(dt_develop_blend_params_t *bp)
{
  dt_develop_blend_init_blend_parameters(bp, DEVELOP_BLEND_CS_RGB_SCENE);
  bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER;
  g_strlcpy(bp->raster_mask_source, "exposure", sizeof(bp->raster_mask_source));
  bp->raster_mask_instance = 0;
  bp->raster_mask_id = 0;
  bp->raster_mask_invert = TRUE;
}

static void _case_uniform(dt_develop_blend_params_t *bp)
{
  dt_develop_blend_init_blend_parameters(bp, DEVELOP_BLEND_CS_RGB_SCENE);
  bp->mask_mode = DEVELOP_MASK_ENABLED;
  bp->opacity = 60.0f;
}

static void _case_disabled(dt_develop_blend_params_t *bp)
{
  dt_develop_blend_init_blend_parameters(bp, DEVELOP_BLEND_CS_RGB_SCENE);
  bp->mask_mode = DEVELOP_MASK_DISABLED;
}

static void _case_drawn_parametric(dt_develop_blend_params_t *bp)
{
  _classic_green(bp);
  bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL;
  bp->mask_id = DANGLING;
}

static void _case_drawn_parametric_inverted(dt_develop_blend_params_t *bp)
{
  _case_drawn_parametric(bp);
  bp->mask_combine |= DEVELOP_COMBINE_MASKS_POS;
}

static void _case_drawn(dt_develop_blend_params_t *bp)
{
  dt_develop_blend_init_blend_parameters(bp, DEVELOP_BLEND_CS_RGB_SCENE);
  bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK;
  bp->mask_id = DANGLING;
}

static const _classic_case_t _cases[] = {
  { "parametric", _case_parametric },
  { "parametric inverted", _case_parametric_inverted },
  { "raster", _case_raster },
  { "uniform", _case_uniform },
  { "disabled", _case_disabled },
  { "drawn and parametric", _case_drawn_parametric },
  { "drawn and parametric, drawn inverted", _case_drawn_parametric_inverted },
  { "drawn", _case_drawn },
  { NULL, NULL }
};

// the reference: the params as a history row of a fresh image, migrated by
// the history read
static GBytes *_as_history_row(const dt_develop_blend_params_t *bp, const int version)
{
  const dt_imgid_t imgid = dt_image_duplicate(dt_test_catalog_image("bayer4.crw"));
  dt_develop_t dev;
  _open(&dev, imgid);
  const int num = dev.history_end;
  dt_dev_cleanup(&dev);

  assert_true(dt_masks_scratch_seed_history(imgid, num, OP, 0, version, bp, NULL));
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET history_end = ?2 WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num + 1);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  _open(&dev, imgid);
  GBytes *canon = _canon_item(&dev);
  dt_dev_cleanup(&dev);
  return canon;
}

// preset apply on a fresh image, recorded, and read back
static GBytes *_as_preset(const dt_develop_blend_params_t *bp, const int version)
{
  const dt_imgid_t imgid = dt_image_duplicate(dt_test_catalog_image("bayer4.crw"));
  dt_develop_t dev;
  _open(&dev, imgid);
  dt_iop_module_t *module = _module(&dev);
  dt_gui_presets_apply_blend_params(module, bp, version, sizeof(*bp));
  _commit(&dev, module);
  dt_dev_cleanup(&dev);

  _open(&dev, imgid);
  GBytes *canon = _canon_item(&dev);
  dt_dev_cleanup(&dev);
  return canon;
}

// a style holding the params as its one item, applied to a fresh image
static GBytes *_as_style(const dt_develop_blend_params_t *bp, const int version)
{
  static int n = 0;
  gchar *name = g_strdup_printf("test classic style %d", n++);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO data.styles (name, description, id)"
                              " VALUES (?1, '', (SELECT COALESCE(MAX(id), 0) + 1 FROM data.styles))",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  dt_iop_module_so_t *so = dt_iop_get_module_so(OP);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO data.style_items"
                              " (styleid, num, module, operation, op_params, enabled,"
                              "  blendop_params, blendop_version, multi_priority, multi_name,"
                              "  multi_name_hand_edited)"
                              " VALUES ((SELECT id FROM data.styles WHERE name = ?1), 0, ?2, ?3,"
                              "         NULL, 1, ?4, ?5, 0, '', 0)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, so->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, OP, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, bp, sizeof(*bp), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  const dt_imgid_t imgid = dt_image_duplicate(dt_test_catalog_image("bayer4.crw"));
  dt_styles_apply_to_image(name, FALSE, FALSE, imgid);
  g_free(name);

  dt_develop_t dev;
  _open(&dev, imgid);
  GBytes *canon = _canon_item(&dev);
  dt_dev_cleanup(&dev);
  return canon;
}

static void _print_canon(const char *what, GBytes *b)
{
  gsize size = 0;
  const dt_develop_blend_params_t *bp = g_bytes_get_data(b, &size);
  fprintf(stderr, "  %s: %zu bytes, mask_mode 0x%x mask_id %d combine 0x%x opacity %g\n",
          what, size, bp->mask_mode, bp->mask_id, bp->mask_combine, bp->opacity);
}

// 5.6 presets and styles (blend version 14), and the same as the startup
// upgrade of development builds of 5.7 stored them: converted with no image,
// at the current version, a mask that needs forms still classic. The mask a
// preset or a style gives is the one the 5.6 history row migrates to
static void test_classic_lossless(void **state)
{
  for(const _classic_case_t *c = _cases; c->name; c++)
    for(int stamped = 0; stamped < 2; stamped++)
    {
      dt_develop_blend_params_t classic;
      memset(&classic, 0, sizeof(classic));
      c->make(&classic);
      dt_develop_blend_params_t bp = classic;
      int version = 14;
      if(stamped)
      {
        assert_false(dt_develop_blend_legacy_params_from_so(dt_iop_get_module_so(OP), &classic,
                                                            14, &bp,
                                                            dt_develop_blend_version(),
                                                            sizeof(classic)));
        version = dt_develop_blend_version();
      }
      GBytes *reference = _as_history_row(&classic, 14);
      GBytes *preset = _as_preset(&bp, version);
      GBytes *style = _as_style(&bp, version);
      const gboolean ok = g_bytes_equal(reference, preset) && g_bytes_equal(reference, style);
      if(!ok)
      {
        fprintf(stderr, "case '%s', blend version %d:\n", c->name, version);
        _print_canon("history row", reference);
        _print_canon("preset", preset);
        _print_canon("style", style);
      }
      g_bytes_unref(reference);
      g_bytes_unref(preset);
      g_bytes_unref(style);
      assert_true(ok);
    }
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_blob_roundtrip),
    cmocka_unit_test(test_blob_malformed),
    cmocka_unit_test(test_pixel_forms_found),
    cmocka_unit_test(test_style_from_pixel_object),
    cmocka_unit_test(test_pixel_object_save_headless),
    cmocka_unit_test(test_preset_apply_other_image),
    cmocka_unit_test(test_auto_apply_with_mask),
    cmocka_unit_test(test_style_with_mask),
    cmocka_unit_test(test_files_roundtrip),
    cmocka_unit_test(test_masks_follow_prepended_history),
    cmocka_unit_test(test_locked_mask_kept_on_preset_apply),
    cmocka_unit_test(test_locked_mask_kept_on_style_apply),
    cmocka_unit_test(test_classic_lossless),
  };
  return cmocka_run_group_tests(tests, dt_test_catalog_setup, dt_test_catalog_teardown);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
