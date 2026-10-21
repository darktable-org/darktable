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

// Presets carrying a classic mask, through every path that reads one: the
// startup upgrade of data.presets, preset apply, auto-apply, and the history
// read of an edit made from such a preset.
//
// The startup upgrade converts presets with a module that has no image, so it
// cannot create the flexi forms of a classic mask
// (dt_masks_migrate_classic_to_flexi()). Development builds of 5.7 stored such
// a preset at the current blend version anyway, still classic: a parametric
// mask that rendered but was not in the flexi panel, and that auto-apply copied
// into history.
//
// The startup upgrade only runs on presets already in data.db when darktable
// starts, so the setup starts darktable twice, on the same configdir: once in
// a child process (this binary with --seed) to create data.db and seed it, then
// here, where the upgrade runs on the seeded rows.

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include "common/darktable.h"
#include "common/database.h"
#include "common/debug.h"
#include "common/film.h"
#include "common/image.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/masks/scratch_image.h"
#include "gui/presets.h"

#include <glib/gstdio.h>

#define OP "colorbalancergb"
#define PRESET_CLASSIC "test classic green"
#define PRESET_UNIFORM "test uniform"
#define PRESET_AUTO "test classic green auto"

static const char *_images[] = { "mire1.cr2", "hlrecovery.arw", "bayer4.crw", NULL };
static gchar *_self = NULL;
static gchar *_tmpdir = NULL;

static int _init(const char *configdir, const gboolean load_data)
{
  char *argv[] = { "darktable-test",
                   "--library", ":memory:",
                   "--configdir", (char *)configdir,
                   "--cachedir", (char *)configdir,
                   "--datadir", DT_TEST_DATADIR,
                   "--moduledir", DT_TEST_MODULEDIR,
                   "--disable-opencl",
                   "--conf", "write_sidecar_files=never",
                   NULL };
  const int argc = sizeof(argv) / sizeof(argv[0]) - 1;
  return dt_init(argc, argv, FALSE, load_data, NULL);
}

// a "select green" parametric mask on the hue channel, as a 5.6 preset holds it
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

static int _op_version(void)
{
  for(GList *l = darktable.iop; l; l = g_list_next(l))
  {
    const dt_iop_module_so_t *so = l->data;
    if(!strcmp(so->op, OP)) return so->version();
  }
  return -1;
}

static void _insert_preset(const char *name,
                           const dt_develop_blend_params_t *bp,
                           const int blendop_version,
                           const gboolean autoapply,
                           const char *maker)
{
  sqlite3_stmt *stmt;
  // no op_params: apply and auto-apply use the module defaults then
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO data.presets"
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
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, _op_version());
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, bp, sizeof(*bp), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, blendop_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, autoapply);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// the child: creates data.db in `configdir` and adds 5.6 presets to it
static int _seed(const char *configdir)
{
  if(_init(configdir, TRUE)) return 1;

  dt_develop_blend_params_t bp;
  _classic_green(&bp);
  _insert_preset(PRESET_CLASSIC, &bp, 14, FALSE, "%");
  // auto-applied to the Sony image only, the one the auto-apply test opens
  _insert_preset(PRESET_AUTO, &bp, 14, TRUE, "%sony%");

  dt_develop_blend_init_blend_parameters(&bp, DEVELOP_BLEND_CS_RGB_SCENE);
  bp.mask_mode = DEVELOP_MASK_ENABLED;
  _insert_preset(PRESET_UNIFORM, &bp, 14, FALSE, "%");

  dt_cleanup();
  return 0;
}

static int _setup(void **state)
{
  _tmpdir = g_dir_make_tmp("dt-flexi-presets-test-XXXXXX", NULL);
  if(!_tmpdir) return -1;

  gchar *argv[] = { _self, "--seed", _tmpdir, NULL };
  gint status = 1;
  // a wait status of 0 is a child that exited with 0, on every platform
  if(!g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, &status, NULL)
     || status != 0)
    return -1;

  if(_init(_tmpdir, TRUE)) return -1;

  dt_film_t film;
  dt_film_init(&film);
  const dt_filmid_t filmid = dt_film_new(&film, DT_TEST_IMAGES);
  dt_film_cleanup(&film);
  if(!dt_is_valid_filmid(filmid)) return -1;

  for(int i = 0; _images[i]; i++)
  {
    gchar *path = g_build_filename(DT_TEST_IMAGES, _images[i], NULL);
    const dt_imgid_t imgid = dt_image_import(filmid, path, TRUE, FALSE);
    g_free(path);
    if(!dt_is_valid_imgid(imgid)) return -1;
  }
  return 0;
}

static void _rm_rf(const char *path)
{
  GDir *d = g_dir_open(path, 0, NULL);
  if(d)
  {
    const char *name;
    while((name = g_dir_read_name(d)))
    {
      gchar *full = g_build_filename(path, name, NULL);
      if(g_file_test(full, G_FILE_TEST_IS_DIR) && !g_file_test(full, G_FILE_TEST_IS_SYMLINK))
        _rm_rf(full);
      else
        g_remove(full);
      g_free(full);
    }
    g_dir_close(d);
  }
  g_rmdir(path);
}

static int _teardown(void **state)
{
  dt_cleanup();
  if(_tmpdir) _rm_rf(_tmpdir);
  g_free(_tmpdir);
  _tmpdir = NULL;
  return 0;
}

static dt_imgid_t _image(const char *filename)
{
  dt_imgid_t imgid = NO_IMGID;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM main.images WHERE filename = ?1 AND version = 0",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, filename, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW) imgid = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return imgid;
}

// opens `imgid` as an export does: the real history read, auto-apply on the
// first one included, which a read with no image skips
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

// the blend params of the last history item of OP, NULL when there is none
static const dt_develop_blend_params_t *_last_item(const dt_develop_t *dev)
{
  const dt_develop_blend_params_t *bp = NULL;
  for(GList *l = dev->history; l; l = g_list_next(l))
  {
    const dt_dev_history_item_t *h = l->data;
    if(!strcmp(h->op_name, OP)) bp = h->blend_params;
  }
  return bp;
}

// does the flexi mask of `bp` resolve in `forms` to a group holding a
// parametric element? That is what the flexi panel shows
static gboolean _shows_parametric(const dt_develop_blend_params_t *bp, GList *forms)
{
  if(!bp || !(bp->mask_mode & DEVELOP_MASK_FLEXI) || !dt_is_valid_maskid(bp->mask_id))
    return FALSE;
  const dt_masks_form_t *grp = dt_masks_get_from_id_ext(forms, bp->mask_id);
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return FALSE;
  for(const GList *l = grp->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    const dt_masks_form_t *member = dt_masks_get_from_id_ext(forms, pt->formid);
    if(member && (member->type & DT_MASKS_PARAMETRIC)) return TRUE;
  }
  return FALSE;
}

static int _preset_blend(const char *name, dt_develop_blend_params_t *bp)
{
  int version = -1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT blendop_version, blendop_params FROM data.presets"
                              " WHERE operation = ?1 AND name = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, OP, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, name, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW
     && sqlite3_column_bytes(stmt, 1) == sizeof(*bp))
  {
    version = sqlite3_column_int(stmt, 0);
    memcpy(bp, sqlite3_column_blob(stmt, 1), sizeof(*bp));
  }
  sqlite3_finalize(stmt);
  return version;
}

// what the stored history of `imgid` renders OP with, and how many parametric
// forms main.masks_history holds for it
static void _stored(const dt_imgid_t imgid, dt_develop_blend_params_t *bp, int *parametric)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT blendop_params FROM main.history"
                              " WHERE imgid = ?1 AND operation = ?2"
                              " ORDER BY num DESC LIMIT 1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, OP, -1, SQLITE_TRANSIENT);
  memset(bp, 0, sizeof(*bp));
  if(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_bytes(stmt, 0) == sizeof(*bp))
    memcpy(bp, sqlite3_column_blob(stmt, 0), sizeof(*bp));
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.masks_history"
                              " WHERE imgid = ?1 AND form = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, DT_MASKS_PARAMETRIC);
  *parametric = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
  sqlite3_finalize(stmt);
}

// the startup upgrade has no image to create the forms of a classic mask in:
// it must leave such a preset as stored, and still upgrade one with no mask
static void test_startup_upgrade_keeps_classic_preset(void **state)
{
  dt_develop_blend_params_t bp;
  assert_int_equal(_preset_blend(PRESET_CLASSIC, &bp), 14);
  assert_int_equal(bp.mask_mode, DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL);
  assert_int_equal(_preset_blend(PRESET_AUTO, &bp), 14);

  assert_int_equal(_preset_blend(PRESET_UNIFORM, &bp), dt_develop_blend_version());
  assert_int_equal(bp.mask_mode, DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI);
}

// preset apply, from the 5.6 preset and from the same preset as development
// builds of 5.7 stored it: current version, classic mask
static void test_preset_apply_migrates_classic_mask(void **state)
{
  dt_develop_t dev;
  _open(&dev, _image("bayer4.crw"));
  dt_iop_module_t *module = _module(&dev);
  assert_non_null(module);

  dt_develop_blend_params_t bp;
  _classic_green(&bp);
  for(int version = 14; version <= dt_develop_blend_version(); version++)
  {
    dt_gui_presets_apply_blend_params(module, &bp, version, sizeof(bp));
    assert_true(_shows_parametric(module->blend_params, dev.forms));
    // which preset apply records with the forms (dt_dev_add_masks_history_item)
    assert_true(dt_masks_module_mask_in_forms(module, dev.forms));
  }

  // a flexi mask is applied as it is: migrating it would drop its group
  const dt_develop_blend_params_t flexi = *module->blend_params;
  dt_gui_presets_apply_blend_params(module, &flexi, dt_develop_blend_version(), sizeof(flexi));
  assert_memory_equal(module->blend_params, &flexi, sizeof(flexi));

  dt_dev_cleanup(&dev);
}

// an edit holding a current-version classic mask, as auto-apply made from a
// preset development builds of 5.7 stored so: the history read migrates it
// and stores the forms
static void test_history_read_migrates_current_classic(void **state)
{
  const dt_imgid_t imgid = _image("mire1.cr2");

  // the first open auto-applies, then the edit gets OP with the stored mask
  dt_develop_t dev;
  _open(&dev, imgid);
  const int num = dev.history_end;
  dt_dev_cleanup(&dev);

  dt_develop_blend_params_t bp;
  _classic_green(&bp);
  assert_true(dt_masks_scratch_seed_history(imgid, num, OP, 0, dt_develop_blend_version(),
                                            &bp, NULL));
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET history_end = ?2 WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num + 1);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // twice: the second open reads what the first stored
  for(int pass = 0; pass < 2; pass++)
  {
    _open(&dev, imgid);
    assert_true(_shows_parametric(_last_item(&dev), dev.forms));
    dt_dev_cleanup(&dev);

    int parametric = 0;
    _stored(imgid, &bp, &parametric);
    assert_true(bp.mask_mode & DEVELOP_MASK_FLEXI);
    assert_false(dt_develop_blend_mask_is_classic(&bp));
    assert_true(parametric > 0);
  }
}

// a 5.6 auto-apply preset with a parametric mask, end to end: through the
// startup upgrade, then auto-applied by the first open of a matching image
static void test_auto_apply_migrates_classic_preset(void **state)
{
  const dt_imgid_t imgid = _image("hlrecovery.arw");

  for(int pass = 0; pass < 2; pass++)
  {
    dt_develop_t dev;
    _open(&dev, imgid);
    assert_true(_shows_parametric(_last_item(&dev), dev.forms));
    dt_dev_cleanup(&dev);

    dt_develop_blend_params_t bp;
    int parametric = 0;
    _stored(imgid, &bp, &parametric);
    assert_true(bp.mask_mode & DEVELOP_MASK_FLEXI);
    assert_true(parametric > 0);
  }
}

int main(int argc, char *argv[])
{
  if(argc == 3 && !strcmp(argv[1], "--seed")) return _seed(argv[2]);
  _self = argv[0];

  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_startup_upgrade_keeps_classic_preset),
    cmocka_unit_test(test_preset_apply_migrates_classic_mask),
    cmocka_unit_test(test_history_read_migrates_current_classic),
    cmocka_unit_test(test_auto_apply_migrates_classic_preset),
  };
  return cmocka_run_group_tests(tests, _setup, _teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
