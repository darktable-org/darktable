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

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include "common/database.h"
#include "common/color_harmony.h"
#include "common/history.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/presets.h"
#include "common/styles.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/masks/scratch_image.h"
#include "gui/presets.h"

#include <glib/gstdio.h>

#include "common/film.h"

static gchar *_tmpdir;
static dt_imgid_t _image;

static int _setup(void **state)
{
  _tmpdir = g_dir_make_tmp("dt-owned-presets-XXXXXX", NULL);
  char *argv[] = { "darktable-test", "--library", ":memory:",
    "--configdir", _tmpdir, "--cachedir", _tmpdir,
    "--datadir", DT_TEST_DATADIR, "--moduledir", DT_TEST_MODULEDIR,
    "--disable-opencl", "--conf", "write_sidecar_files=never", NULL };
  if(dt_init(sizeof(argv) / sizeof(argv[0]) - 1, argv, FALSE, FALSE, NULL)) return -1;
  // a generated PFM keeps this regression independent of the RAW submodule
  gchar *path = g_build_filename(_tmpdir, "fixture.pfm", NULL);
  FILE *f = g_fopen(path, "wb");
  if(!f) return -1;
  fprintf(f, "PF\n16 16\n%s1.0\n", G_BYTE_ORDER == G_LITTLE_ENDIAN ? "-" : "");
  float pixels[16 * 16 * 3];
  for(size_t i = 0; i < G_N_ELEMENTS(pixels); i++) pixels[i] = 0.25f;
  fwrite(pixels, sizeof(pixels), 1, f);
  fclose(f);
  dt_film_t film;
  dt_film_init(&film);
  dt_filmid_t filmid = dt_film_new(&film, _tmpdir);
  dt_film_cleanup(&film);
  _image = dt_image_import(filmid, path, TRUE, FALSE);
  g_free(path);
  return dt_is_valid_imgid(_image) ? 0 : -1;
}

static void _remove_tree(const char *path)
{
  GDir *d = g_dir_open(path, 0, NULL);
  if(d)
  {
    const char *name;
    while((name = g_dir_read_name(d)))
    {
      gchar *p = g_build_filename(path, name, NULL);
      if(g_file_test(p, G_FILE_TEST_IS_DIR) && !g_file_test(p, G_FILE_TEST_IS_SYMLINK))
        _remove_tree(p);
      else g_remove(p);
      g_free(p);
    }
    g_dir_close(d);
  }
  g_rmdir(path);
}

static int _teardown(void **state)
{
  // no local catalog operation may leave a statement blocking database shutdown
  assert_null(sqlite3_next_stmt(dt_database_get(darktable.db), NULL));
  dt_cleanup();
  _remove_tree(_tmpdir);
  g_free(_tmpdir);
  return 0;
}

static void _open(dt_develop_t *dev, const dt_imgid_t imgid)
{
  dt_dev_init(dev, FALSE);
  dt_dev_load_image(dev, imgid);
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
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, "retouch", -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, dt_iop_get_module_so("retouch")->version());
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, blob, size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, blendop_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, autoapply);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static dt_iop_module_t *_owned_module(dt_develop_t *dev, const char *op)
{
  for(GList *l = dev->iop; l; l = l->next)
    if(dt_iop_module_is(l->data, op)) return l->data;
  fail();
  return NULL;
}

// introspection accesses the real module layout, without duplicating retouch's
// private params struct in the fixture
static dt_mask_id_t *_owned_id(dt_iop_module_t *m, void *params)
{
  const gboolean retouch = dt_iop_module_is(m, "retouch");
  const dt_introspection_field_t *a = m->get_f(retouch ? "rt_forms" : "clone_id");
  size_t offset = a->header.offset;
  if(retouch)
  {
    const dt_introspection_field_t *e = a->Array.field;
    for(size_t i = 0; i < e->Struct.entries; i++)
      if(!strcmp(e->Struct.fields[i]->header.field_name, "formid"))
        offset += e->Struct.fields[i]->header.offset - e->header.offset;
  }
  return (dt_mask_id_t *)((char *)params + offset);
}

static void test_owned_presets(void **state)
{
  for(int spots = 0; spots < 2; spots++)
  {
    const char *op = spots ? "spots" : "retouch";
    const dt_imgid_t src = dt_image_duplicate(_image);
    dt_develop_t dev;
    _open(&dev, src);
    dt_iop_module_t *m = _owned_module(&dev, op);
    const size_t params_size = m->params_size;
    dt_masks_form_t *group = dt_masks_create(DT_MASKS_GROUP);
    dt_masks_form_t *circle = dt_masks_create(DT_MASKS_CIRCLE | DT_MASKS_CLONE);
    dt_masks_point_circle_t *pt = calloc(1, sizeof(*pt));
    pt->center[0] = pt->center[1] = 0.5f;
    pt->radius = 0.05f;
    pt->border = 0.01f;
    circle->points = g_list_append(NULL, pt);
    circle->source[0] = circle->source[1] = 0.3f;
    group->points = g_list_append(NULL, _member(circle->formid, group->formid));
    dev.forms = g_list_append(dev.forms, group);
    dev.forms = g_list_append(dev.forms, circle);
    // markers share the namespace with real forms, including on collision
    group->points = g_list_prepend(group->points, dt_masks_marker_new(dev.forms, group, 0));
    ((dt_masks_point_group_t *)group->points->data)->formid = 1;
    // a separate blending tree and an unrelated shape must not get confused
    // with the module's own content; only the two rooted trees are serialized
    dt_masks_form_t *blend = dt_masks_create(DT_MASKS_GROUP);
    dt_masks_form_t *nested = dt_masks_create(DT_MASKS_GROUP);
    nested->points = g_list_append(NULL, _member(circle->formid, nested->formid));
    group->points = g_list_append(group->points, _member(nested->formid, group->formid));
    blend->points = g_list_append(NULL, _member(circle->formid, blend->formid));
    dev.forms = g_list_append(dev.forms, nested);
    dev.forms = g_list_append(dev.forms, blend);
    dt_masks_form_t *unrelated = dt_masks_dup_masks_form(circle);
    unrelated->formid = circle->formid + 100;
    const dt_mask_id_t unrelated_id = unrelated->formid;
    dev.forms = g_list_append(dev.forms, unrelated);
    m->blend_params->blend_mask_id = blend->formid;
    m->blend_params->mask_id = group->formid;
    *_owned_id(m, m->params) = circle->formid;
    size_t size = 0;
    void *blob = dt_gui_presets_module_blend_blob(m, &size);
    assert_true(size > sizeof(*m->blend_params));
    dt_develop_blend_params_t decoded;
    GList *saved_forms = NULL;
    assert_true(dt_masks_blend_blob_decode(blob, size, &decoded, &saved_forms));
    assert_int_equal(g_list_length(saved_forms), 4);
    assert_null(dt_masks_get_from_id_ext(saved_forms, unrelated->formid));
    g_list_free_full(saved_forms, (GDestroyNotify)dt_masks_free_form);
    void *params = g_malloc(m->params_size);
    memcpy(params, m->params, m->params_size);
    GHashTable *source_ids = _tree_ids(dev.forms, group->formid);
    GHashTable *previous = NULL;
    // applying twice to the source deliberately collides with every source id
    for(int pass = 0; pass < 2; pass++)
    {
      memcpy(m->params, params, m->params_size);
      dt_gui_presets_apply_blend_params(m, blob, dt_develop_blend_version(), size);
      dt_mask_id_t id = *_owned_id(m, m->params);
      assert_non_null(dt_masks_get_from_id(&dev, id));
      assert_non_null(dt_masks_get_from_id(&dev, m->blend_params->blend_mask_id));
      assert_int_not_equal(m->blend_params->blend_mask_id, blend->formid);
      GHashTable *now = _tree_ids(dev.forms, m->blend_params->mask_id);
      assert_true(_disjoint(now, source_ids));
      if(previous) { assert_true(_disjoint(now, previous)); g_hash_table_destroy(previous); }
      previous = now;
    }
    g_hash_table_destroy(previous);
    const dt_mask_id_t locked_blend = m->blend_params->blend_mask_id;
    m->blend_params->mask_lock = 1;
    guint before_locked = g_list_length(dev.forms);
    memcpy(m->params, params, m->params_size);
    dt_gui_presets_apply_blend_params(m, blob, dt_develop_blend_version(), size);
    assert_int_equal(m->blend_params->blend_mask_id, locked_blend);
    assert_int_equal(g_list_length(dev.forms), before_locked + 3);
    m->blend_params->mask_lock = 0;
    // an unresolved op_params reference must not publish a partial tree
    const guint count = g_list_length(dev.forms);
    *_owned_id(m, m->params) = G_MAXINT;
    dt_develop_blend_params_t bp;
    assert_true(dt_develop_blend_params_read(m, blob, size, dt_develop_blend_version(),
                                             &bp, -1, FALSE, NULL));
    assert_int_equal(count, g_list_length(dev.forms));
    memcpy(m->params, params, m->params_size);
    assert_true(dt_masks_blend_blob_decode(blob, size, &decoded, &saved_forms));
    dt_masks_form_t *bad_group = dt_masks_get_from_id_ext(saved_forms, decoded.mask_id);
    dt_masks_point_group_t *bad_member = bad_group->points->next->data;
    // a cycle parses as a blob, but must never enter history or the live forms
    bad_member->formid = bad_group->formid;
    const dt_develop_blend_params_t original = decoded;
    decoded.mask_mode |= DEVELOP_MASK_FLEXI;
    size_t bad_size = 0;
    void *bad = dt_masks_blend_blob_encode(&decoded, saved_forms, &bad_size);
    memcpy(bad, &original, sizeof(original));
    assert_true(dt_develop_blend_params_read(m, bad, bad_size, dt_develop_blend_version(),
                                             &bp, 0, FALSE, NULL));
    assert_int_equal(count, g_list_length(dev.forms));
    assert_null(dev.pending_flexi_migrations);
    g_list_free_full(saved_forms, (GDestroyNotify)dt_masks_free_form);

    // malformed tails fail at every truncation boundary without publishing
    // forms or changing the incoming module parameters
    for(size_t n = sizeof(decoded) + 1; n < size; n++)
    {
      assert_true(dt_develop_blend_params_read(m, blob, n, dt_develop_blend_version(),
                                               &bp, -1, FALSE, NULL));
      assert_int_equal(count, g_list_length(dev.forms));
      assert_memory_equal(m->params, params, m->params_size);
    }

    // the GUI entry point must reject invalid data before labels, signals,
    // history or live parameters change; no GTK widgets are needed on failure
    _insert_preset("test owned bad manual", bad, bad_size, dt_develop_blend_version(), FALSE, "%");
    sqlite3_stmt *manual;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "UPDATE data.presets SET operation=?1, op_version=?2, op_params=?3"
      " WHERE name='test owned bad manual'", -1, &manual, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(manual, 1, op, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(manual, 2, m->version());
    DT_DEBUG_SQLITE3_BIND_BLOB(manual, 3, params, m->params_size, SQLITE_TRANSIENT);
    assert_int_equal(sqlite3_step(manual), SQLITE_DONE);
    sqlite3_finalize(manual);
    const guint history_count = g_list_length(dev.history);
    const gboolean was_enabled = m->enabled;
    const dt_develop_blend_params_t previous_bp = *m->blend_params;
    dt_gui_presets_apply_preset("test owned bad manual", m);
    assert_memory_equal(m->params, params, m->params_size);
    assert_memory_equal(m->blend_params, &previous_bp, sizeof(previous_bp));
    assert_int_equal(was_enabled, m->enabled);
    assert_int_equal(history_count, g_list_length(dev.history));
    assert_int_equal(count, g_list_length(dev.forms));

    // the manual import must survive the same masks/history write as an edit
    // from the dialog, including the existing colliding trees
    memcpy(m->params, params, m->params_size);
    dt_gui_presets_apply_blend_params(m, blob, dt_develop_blend_version(), size);
    const dt_mask_id_t manual_id = *_owned_id(m, m->params);
    dt_dev_add_masks_history_item_ext(&dev, m, TRUE, TRUE);
    dt_dev_write_history_ext(&dev, src);
    dt_dev_cleanup(&dev);
    _open(&dev, src);
    m = _owned_module(&dev, op);
    assert_non_null(dt_masks_get_from_id(&dev, manual_id));
    assert_non_null(dt_masks_get_from_id(&dev, unrelated_id));
    assert_non_null(dt_masks_get_from_id(&dev, *_owned_id(m, params)));

    // real autoapply: insert a user preset, load a previously untouched image,
    // then reopen it twice to check stable ids and persisted op_params
    _insert_preset("test owned auto", blob, size, dt_develop_blend_version(), TRUE, "%");
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "UPDATE data.presets SET operation=?1, op_version=?2, op_params=?3"
      " WHERE name='test owned auto'", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, op, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, m->version());
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, params, m->params_size, SQLITE_TRANSIENT);
    assert_int_equal(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    dt_dev_cleanup(&dev);
    // rearm only this disposable image to exercise the prepend/mask-snapshot
    // merge with existing edits; normal autoapply is once per history lifetime
    dt_image_t *image = dt_image_cache_get(src, 'w');
    image->flags &= ~DT_IMAGE_AUTO_PRESETS_APPLIED;
    dt_image_cache_write_release(image, DT_IMAGE_CACHE_RELAXED);
    for(int pass = 0; pass < 2; pass++)
    {
      _open(&dev, src);
      m = _owned_module(&dev, op);
      assert_non_null(dt_masks_get_from_id(&dev, manual_id));
      assert_non_null(dt_masks_get_from_id(&dev, unrelated_id));
      for(GList *l = dev.history; l; l = l->next)
      {
        const dt_dev_history_item_t *h = l->data;
        if(strcmp(h->op_name, op)) continue;
        assert_non_null(dt_masks_get_from_id(&dev, *_owned_id(m, h->params)));
        assert_non_null(dt_masks_get_from_id(&dev, h->blend_params->blend_mask_id));
      }
      dt_dev_cleanup(&dev);
    }
    const dt_imgid_t dst = dt_image_duplicate(_image);
    dt_mask_id_t stable = NO_MASKID;
    for(int pass = 0; pass < 3; pass++)
    {
      _open(&dev, dst);
      m = _owned_module(&dev, op);
      const dt_dev_history_item_t *h = NULL;
      for(GList *l = dev.history; l; l = l->next)
        if(!strcmp(((dt_dev_history_item_t *)l->data)->op_name, op)) h = l->data;
      assert_non_null(h);
      const dt_mask_id_t id = *_owned_id(m, h->params);
      assert_non_null(dt_masks_get_from_id(&dev, id));
      assert_non_null(dt_masks_get_from_id(&dev, h->blend_params->blend_mask_id));
      GHashTable *ids = _tree_ids(dev.forms, h->blend_params->mask_id);
      assert_true(g_hash_table_contains(ids, GINT_TO_POINTER(id)));
      assert_true(_disjoint(ids, source_ids));
      g_hash_table_destroy(ids);
      if(pass) assert_int_equal(id, stable);
      stable = id;
      dt_dev_cleanup(&dev);
    }
    // a style made from the auto-applied module carries the same owned tree
    // without depending on the earlier styles branch or on autoapply at target
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
      "UPDATE data.presets SET autoapply=0 WHERE name='test owned auto'", NULL, NULL, NULL);
    _open(&dev, dst);
    int style_num = -1;
    for(GList *l = dev.history; l; l = l->next)
    {
      const dt_dev_history_item_t *h = l->data;
      if(!strcmp(h->op_name, op)) style_num = h->num;
    }
    assert_true(style_num >= 0);
    dt_dev_cleanup(&dev);
    GList *items = g_list_append(NULL, GINT_TO_POINTER(style_num));
    assert_true(dt_styles_create_from_image("test owned style", "", dst, items, FALSE));
    g_list_free(items);
    const dt_imgid_t styled = dt_image_duplicate(_image);
    dt_styles_apply_to_image("test owned style", FALSE, FALSE, styled);
    for(int pass = 0; pass < 2; pass++)
    {
      _open(&dev, styled);
      m = _owned_module(&dev, op);
      gboolean found = FALSE;
      for(GList *l = dev.history; l; l = l->next)
      {
        const dt_dev_history_item_t *h = l->data;
        if(strcmp(h->op_name, op)) continue;
        found = TRUE;
        const dt_mask_id_t id = *_owned_id(m, h->params);
        assert_non_null(dt_masks_get_from_id(&dev, id));
        assert_int_not_equal(id, stable);
        assert_non_null(dt_masks_get_from_id(&dev, h->blend_params->blend_mask_id));
      }
      assert_true(found);
      dt_dev_cleanup(&dev);
    }
    dt_styles_delete_by_name("test owned style");
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
      "UPDATE data.presets SET autoapply=1 WHERE name='test owned auto'", NULL, NULL, NULL);

    // old presets with positive ids but no forms cannot be recovered, even
    // if the destination happens to contain a shape with the same id
    _open(&dev, dst);
    m = _owned_module(&dev, op);
    memcpy(m->params, params, m->params_size);
    assert_true(dt_develop_blend_params_read(m, blob, sizeof(decoded), dt_develop_blend_version(),
                                             &decoded, -1, FALSE, NULL));
    memset(m->params, 0, m->params_size);
    assert_true(dt_develop_blend_params_read(m, blob, sizeof(decoded), dt_develop_blend_version(),
                                             &decoded, -1, FALSE, NULL));
    dt_dev_cleanup(&dev);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "UPDATE data.presets SET blendop_params=?1 WHERE name='test owned auto'", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 1, blob, sizeof(decoded), SQLITE_TRANSIENT);
    assert_int_equal(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    const dt_imgid_t legacy = dt_image_duplicate(_image);
    for(int pass = 0; pass < 2; pass++)
    {
      _open(&dev, legacy);
      for(GList *l = dev.history; l; l = l->next)
        assert_string_not_equal(((dt_dev_history_item_t *)l->data)->op_name, op);
      assert_null(dev.forms);
      dt_dev_cleanup(&dev);
    }

    // refusal must precede a legacy conditional-mask migration: otherwise
    // its pending import would borrow the just-freed history buffer
    dt_develop_blend_params_t legacy_blend;
    memcpy(&legacy_blend, blob, sizeof(legacy_blend));
    legacy_blend.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "UPDATE data.presets SET blendop_params=?1, blendop_version=14 WHERE name='test owned auto'",
      -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 1, &legacy_blend, sizeof(legacy_blend), SQLITE_TRANSIENT);
    assert_int_equal(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    const dt_imgid_t legacy_conditional = dt_image_duplicate(_image);
    _open(&dev, legacy_conditional);
    assert_null(dev.pending_flexi_migrations);
    assert_null(dev.forms);
    for(GList *l = dev.history; l; l = l->next)
      assert_string_not_equal(((dt_dev_history_item_t *)l->data)->op_name, op);
    dt_dev_cleanup(&dev);

    // a corrupt automatically applied preset must leave neither a module row
    // nor forms, even after the normal history writer and another reopen
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "UPDATE data.presets SET blendop_params=?1, blendop_version=15 WHERE name='test owned auto'", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 1, bad, bad_size, SQLITE_TRANSIENT);
    assert_int_equal(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    _insert_preset("test owned auto second", bad, bad_size, dt_develop_blend_version(), TRUE, "%");
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "UPDATE data.presets SET operation=?1, op_version=?2, op_params=?3"
      " WHERE name='test owned auto second'", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, op, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, dt_iop_get_module_so(op)->version());
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, params, params_size,
                              SQLITE_TRANSIENT);
    assert_int_equal(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    const dt_imgid_t invalid = dt_image_duplicate(_image);
    for(int pass = 0; pass < 2; pass++)
    {
      _open(&dev, invalid);
      for(GList *l = dev.history; l; l = l->next)
        assert_string_not_equal(((dt_dev_history_item_t *)l->data)->op_name, op);
      assert_null(dev.forms);
      int instances = 0;
      for(GList *l = dev.iop; l; l = l->next)
      {
        dt_iop_module_t *instance = l->data;
        if(!dt_iop_module_is(instance, op)) continue;
        instances++;
        assert_string_equal(instance->multi_name, "");
      }
      assert_int_equal(instances, 1);
      dt_dev_cleanup(&dev);
    }
    g_free(bad);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
      "DELETE FROM data.presets WHERE name LIKE 'test owned %'", NULL, NULL, NULL);
    g_hash_table_destroy(source_ids);
    g_free(blob);
    g_free(params);
  }
}

// spots v1 predates mask ids and embeds coordinates; preserve its existing
// converter rather than treating it as a preset with missing external forms
static void test_legacy_spots_inline(void **state)
{
  struct { int num_spots; struct { float x, y, xc, yc, radius; } spot[32]; } old = { 0 };
  old.num_spots = 1;
  old.spot[0].x = old.spot[0].y = 0.5f;
  old.spot[0].xc = old.spot[0].yc = 0.3f;
  old.spot[0].radius = 0.05f;
  dt_develop_t dev;
  _open(&dev, dt_image_duplicate(_image));
  dt_iop_module_t *m = _owned_module(&dev, "spots");
  dt_develop_blend_params_t bp = *m->default_blendop_params;
  bp.mask_mode = DEVELOP_MASK_ENABLED;
  bp.mask_id = bp.blend_mask_id = NO_MASKID;
  const int order = m->iop_order;
  dt_dev_cleanup(&dev);
  _insert_preset("test owned inline", &bp, sizeof(bp), 14, TRUE, "%");
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "UPDATE data.presets SET operation='spots', op_version=1, op_params=?1"
    " WHERE name='test owned inline'", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 1, &old, sizeof(old), SQLITE_TRANSIENT);
  assert_int_equal(sqlite3_step(stmt), SQLITE_DONE);
  sqlite3_finalize(stmt);
  const dt_imgid_t dst = dt_image_duplicate(_image);
  for(int pass = 0; pass < 2; pass++)
  {
    _open(&dev, dst);
    m = _owned_module(&dev, "spots");
    gboolean found = FALSE;
    for(GList *l = dev.history; l; l = l->next)
    {
      const dt_dev_history_item_t *h = l->data;
      if(strcmp(h->op_name, "spots")) continue;
      found = TRUE;
      assert_non_null(dt_masks_get_from_id(&dev, *_owned_id(m, h->params)));
      assert_non_null(dt_masks_get_from_id(&dev, h->blend_params->mask_id));
    }
    assert_true(found);
    dt_dev_cleanup(&dev);
  }
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
    "DELETE FROM data.presets WHERE name='test owned inline'", NULL, NULL, NULL);
  const dt_imgid_t styled = dt_image_duplicate(_image);
  _open(&dev, styled);
  dt_style_item_t item = { .operation = "spots", .multi_name = "", .enabled = 1,
    .module_version = 1, .blendop_version = 14, .params = (dt_iop_params_t *)&old,
    .params_size = sizeof(old), .blendop_params = &bp, .blendop_params_size = sizeof(bp),
    .iop_order = order };
  GList *used = NULL;
  dt_styles_apply_style_item(&dev, &item, &used, TRUE);
  g_list_free(used);
  dt_dev_write_history_ext(&dev, styled);
  dt_dev_cleanup(&dev);
  _open(&dev, styled);
  m = _owned_module(&dev, "spots");
  gboolean found = FALSE;
  for(GList *l = dev.history; l; l = l->next)
  {
    const dt_dev_history_item_t *h = l->data;
    if(strcmp(h->op_name, "spots")) continue;
    found = TRUE;
    assert_non_null(dt_masks_get_from_id(&dev, *_owned_id(m, h->params)));
    assert_non_null(dt_masks_get_from_id(&dev, h->blend_params->mask_id));
  }
  assert_true(found);
  dt_dev_cleanup(&dev);
}

// optional real-image harness: the caller supplies only disposable paths;
// normal ctest still uses the generated PFM and never requires private RAWs
static int _manual_lab(char **args)
{
  char *argv[] = { "darktable-test", "--configdir", args[0], "--cachedir", args[0],
    "--library", args[1], "--datadir", DT_TEST_DATADIR, "--moduledir", DT_TEST_MODULEDIR,
    "--disable-opencl", "--conf", "write_sidecar_files=never", NULL };
  if(dt_init(G_N_ELEMENTS(argv) - 1, argv, FALSE, TRUE, NULL)) return 1;
  gchar *dir = g_path_get_dirname(args[2]);
  dt_film_t film;
  dt_film_init(&film);
  const dt_filmid_t filmid = dt_film_new(&film, dir);
  dt_film_cleanup(&film);
  g_free(dir);
  const dt_imgid_t imgid = dt_image_import(filmid, args[2], TRUE, FALSE);
  assert_true(dt_is_valid_imgid(imgid));
  dt_develop_t dev;
  _open(&dev, imgid);
  dt_iop_module_t *m = _owned_module(&dev, "retouch");
  for(int pass = 0; pass < 2; pass++)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "SELECT op_params, blendop_params, blendop_version FROM data.presets"
      " WHERE name='isolated preset regression' AND operation='retouch'", -1, &stmt, NULL);
    assert_int_equal(sqlite3_step(stmt), SQLITE_ROW);
    assert_int_equal(sqlite3_column_bytes(stmt, 0), m->params_size);
    memcpy(m->params, sqlite3_column_blob(stmt, 0), m->params_size);
    dt_gui_presets_apply_blend_params(m, sqlite3_column_blob(stmt, 1),
                                     sqlite3_column_int(stmt, 2), sqlite3_column_bytes(stmt, 1));
    sqlite3_finalize(stmt);
    assert_non_null(dt_masks_get_from_id(&dev, *_owned_id(m, m->params)));
    dt_dev_add_masks_history_item_ext(&dev, m, TRUE, TRUE);
    dt_dev_write_history_ext(&dev, imgid);
    dt_dev_cleanup(&dev);
    _open(&dev, imgid);
    m = _owned_module(&dev, "retouch");
  }
  dt_dev_cleanup(&dev);
  dt_cleanup();
  return 0;
}

// Run the same temporary-library setup and cleanup without executing any
// preset or style code. This distinguishes headless darktable initialization
// leaks from allocations caused by the owned-mask tests.
static void test_init_cleanup_baseline(void **state)
{
  (void)state;
  dt_color_harmony_guide_t guide;
  dt_color_harmony_init(&guide);
  assert_false(dt_color_harmony_get(_image, &guide));
  guide.type = DT_COLOR_HARMONY_COMPLEMENTARY;
  dt_color_harmony_set(_image, guide);
  dt_color_harmony_guide_t restored;
  dt_color_harmony_init(&restored);
  assert_true(dt_color_harmony_get(_image, &restored));
  assert_int_equal(restored.type, guide.type);
  assert_int_equal(restored.rotation, guide.rotation);
  assert_int_equal(restored.width, guide.width);
  guide.type = DT_COLOR_HARMONY_NONE;
  dt_color_harmony_set(_image, guide);
  assert_false(dt_color_harmony_get(_image, &restored));
  assert_null(sqlite3_next_stmt(dt_database_get(darktable.db), NULL));
}

int main(int argc, char **argv)
{
  if(argc == 2 && !strcmp(argv[1], "--lsan-baseline"))
  {
    const struct CMUnitTest baseline[] = { cmocka_unit_test(test_init_cleanup_baseline) };
    return cmocka_run_group_tests(baseline, _setup, _teardown);
  }
  if(argc == 2 && !strcmp(argv[1], "--lsan-owned-only"))
  {
    const struct CMUnitTest owned[] = { cmocka_unit_test(test_owned_presets) };
    return cmocka_run_group_tests(owned, _setup, _teardown);
  }
  if(argc == 2 && !strcmp(argv[1], "--lsan-spots-only"))
  {
    const struct CMUnitTest spots[] = { cmocka_unit_test(test_legacy_spots_inline) };
    return cmocka_run_group_tests(spots, _setup, _teardown);
  }
  if(argc == 5 && !strcmp(argv[1], "--manual-lab")) return _manual_lab(argv + 2);
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_init_cleanup_baseline),
    cmocka_unit_test(test_owned_presets),
    cmocka_unit_test(test_legacy_spots_inline)
  };
  return cmocka_run_group_tests(tests, _setup, _teardown);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
