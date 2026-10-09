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

// Mask blob version migration (dt_masks_legacy_params).
//
// This is the code that carries every already-saved edit forward when the
// group point struct gains a field. It is worth testing carefully for three
// reasons:
//
//   * it runs against data nobody can regenerate -- a user's existing library;
//   * it fails silently. A missed fixup does not crash or log, it just renders
//     a subtly (or catastrophically) different mask; and
//   * it is the one place where a zero-filled field is NOT automatically safe.
//     Fields appended to dt_masks_point_group_t are read at the historic
//     stride and zero-filled (see dt_masks_read_forms_ext), which is neutral
//     for most of them -- but group_opacity is multiplicative, so a zero-fill
//     would silently blank out every pre-v7 group's mask.
//
// These cover the migration that runs after the read, on a form whose points
// have already been zero-filled the way the reader leaves them, and the
// read-time stride (dt_masks_point_stride).

#include "flexi_fixture.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

static int _teardown(void **state)
{
  flexi_teardown();
  return 0;
}

// a group whose points look the way dt_masks_read_forms_ext leaves them for an
// edit saved at `version`: everything appended after that version zero-filled.
static dt_masks_form_t *_build_legacy(const char *layout, const int version)
{
  dt_masks_form_t *grp = flexi_build_classic(layout);
  grp->version = version;
  for(GList *l = grp->points; l; l = g_list_next(l))
  {
    dt_masks_point_group_t *pt = l->data;
    if(version < 7)
    {
      memset(&pt->refinement, 0, sizeof(pt->refinement));
      memset(pt->name, 0, sizeof(pt->name));
      pt->group_opacity = 0.0f;
    }
  }
  return grp;
}

// the groups an old list makes: the list is converted the way the classic
// migration converts it, then read like any other
#define assert_converted(grp, expect)                                          \
  do {                                                                         \
    dt_masks_group_mark_classic_runs(&flexi_dev.forms, (grp), NULL);                 \
    assert_tree((grp), expect);                                                \
  } while(0)

static void _migrate(dt_masks_form_t *grp)
{
  const int rc = dt_masks_legacy_params(&flexi_dev, grp, grp->version,
                                        dt_masks_version());
  assert_int_equal(rc, 0);
  assert_int_equal(grp->version, dt_masks_version());
}

// ---------------------------------------------------------------------------
// v6 -> v7
// ---------------------------------------------------------------------------

// The dangerous one. group_opacity multiplies the group's finished sub-mask,
// so the 0.0 left by the read-time zero-fill is not neutral -- it is "erase
// this group entirely". Every pre-v7 group point must come out at 1.0.
static void test_v7_gives_every_group_point_unit_opacity(void **state)
{
  dt_masks_form_t *grp = _build_legacy("1,2,i3,i4", 6);
  _migrate(grp);

  for(GList *l = grp->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(pt->group_opacity != 1.0f)
      fail_msg("form %d migrated from v6 with group_opacity %f -- a pre-v7 "
               "group would render blank", (int)pt->formid, pt->group_opacity);
  }
}

// an edit already at v7 keeps whatever the user actually set -- the fixup
// must not stomp a real value
static void test_v7_does_not_overwrite_an_explicit_opacity(void **state)
{
  dt_masks_form_t *grp = _build_legacy("1,2", 7);
  for(GList *l = grp->points; l; l = g_list_next(l))
    ((dt_masks_point_group_t *)l->data)->group_opacity = 0.25f;

  _migrate(grp);

  for(GList *l = grp->points; l; l = g_list_next(l))
    assert_float_equal(((dt_masks_point_group_t *)l->data)->group_opacity,
                       0.25f, 1e-6);
}

// members sharing an operator in an old list are one group
static void test_v6_same_operator_list_yields_one_group(void **state)
{
  dt_masks_form_t *grp = _build_legacy("1,2,3", 6);
  _migrate(grp);
  assert_converted(grp, "u{1,2,3}");
}

// an operator change in an old list nests what comes before it
static void test_v6_operator_change_nests_old_edits(void **state)
{
  dt_masks_form_t *grp = _build_legacy("1,2,i3", 6);
  _migrate(grp);
  assert_converted(grp, "i{u{1,2},3}");
}

// ---------------------------------------------------------------------------
// chain properties
// ---------------------------------------------------------------------------

// a pre-v7 group point stops where the refinement block starts
static void test_group_point_strides(void **state)
{
  const size_t size = sizeof(dt_masks_point_group_t);
  assert_int_equal(dt_masks_point_stride(DT_MASKS_GROUP, 6, size),
                   offsetof(dt_masks_point_group_t, refinement));
  assert_int_equal(dt_masks_point_stride(DT_MASKS_GROUP, 7, size), size);
}

// migrating is idempotent: a form already at the current version comes out
// unchanged, structurally and by hash
static void test_migration_is_idempotent(void **state)
{
  dt_masks_form_t *grp = _build_legacy("1,2,i3,i4", 6);
  _migrate(grp);
  const dt_hash_t once = dt_masks_group_hash(DT_INITHASH, grp);
  char *layout = flexi_layout();

  _migrate(grp);
  assert_int_equal(dt_masks_group_hash(DT_INITHASH, grp), once);
  assert_layout(layout);
  g_free(layout);
}

// nonsensical version pairs are refused rather than half-applied
static void test_migration_rejects_impossible_versions(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");
  // newer than current: nothing can be done with it
  assert_int_not_equal(dt_masks_legacy_params(&flexi_dev, grp,
                                              dt_masks_version() + 1,
                                              dt_masks_version()), 0);
  // version 0 is not a thing
  assert_int_not_equal(dt_masks_legacy_params(&flexi_dev, grp, 0,
                                              dt_masks_version()), 0);
}

// the migration must not renumber, reorder or drop members
static void test_migration_preserves_membership(void **state)
{
  dt_masks_form_t *grp = _build_legacy("1,2,i3,i4,d5", 6);
  const guint before = g_list_length(grp->points);
  _migrate(grp);
  assert_int_equal(g_list_length(grp->points), before);
  assert_converted(grp, "d{i{u{1,2},3,4},5}");
}

// refinement is zero-filled for pre-v7 edits, and zero means disabled -- an old
// mask must not come back with feathering or blur switched on
static void test_pre_v7_refinement_stays_disabled(void **state)
{
  dt_masks_form_t *grp = _build_legacy("1,2,i3", 6);
  _migrate(grp);
  for(GList *l = grp->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    assert_int_equal(pt->refinement.enabled, 0);
    assert_float_equal(pt->refinement.feathering_radius, 0.0f, 1e-6);
    assert_float_equal(pt->refinement.blur_radius, 0.0f, 1e-6);
  }
}

// ---------------------------------------------------------------------------
// clean up unused shapes
// ---------------------------------------------------------------------------

#define CLEANUP_GROUP 100

static dt_masks_form_t *_cleanup_form(const dt_mask_id_t id, const dt_masks_type_t type)
{
  dt_masks_form_t *f = calloc(1, sizeof(dt_masks_form_t));
  f->formid = id;
  f->type = type;
  return f;
}

// a snapshot as a history item carries it: group CLEANUP_GROUP holding shapes
// 1 and 2, and shape 3, detached from every group
static GList *_cleanup_snapshot(void)
{
  dt_masks_form_t *grp = _cleanup_form(CLEANUP_GROUP, DT_MASKS_GROUP);
  for(dt_mask_id_t id = 1; id <= 2; id++)
  {
    dt_masks_point_group_t *pt = calloc(1, sizeof(dt_masks_point_group_t));
    pt->formid = id;
    pt->parentid = CLEANUP_GROUP;
    grp->points = g_list_append(grp->points, pt);
  }
  GList *forms = g_list_append(NULL, grp);
  for(dt_mask_id_t id = 1; id <= 3; id++)
    forms = g_list_append(forms, _cleanup_form(id, DT_MASKS_CIRCLE));
  return forms;
}

static void _cleanup_free(GList *forms)
{
  for(GList *l = forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = l->data;
    g_list_free_full(f->points, free);
    free(f);
  }
  g_list_free(forms);
}

static gboolean _cleanup_has(GList *forms, const dt_mask_id_t id)
{
  return dt_masks_get_from_id_ext(forms, id) != NULL;
}

// a module's mask was removed, so only its earlier, replaced history item
// still names the group. The newer snapshot must lose it all, the older one
// keeps what that item used, for going back in history. Neither item is a
// mask_manager one, so the cleanup must not depend on that
static void test_cleanup_drops_shapes_only_replaced_steps_use(void **state)
{
  flexi_build("u{1}");
  dt_iop_module_t mod = { 0 };
  dt_develop_blend_params_t with_mask = { 0 }, no_mask = { 0 };
  with_mask.mask_id = CLEANUP_GROUP;
  no_mask.mask_id = NO_MASKID;
  dt_dev_history_item_t older = { 0 }, newer = { 0 };
  older.module = newer.module = &mod;
  older.blend_params = &with_mask;
  newer.blend_params = &no_mask;
  older.forms = _cleanup_snapshot();
  newer.forms = _cleanup_snapshot();
  GList *history = g_list_append(g_list_append(NULL, &older), &newer);

  dt_masks_cleanup_unused_from_list(history);

  assert_null(newer.forms);
  assert_true(_cleanup_has(older.forms, CLEANUP_GROUP));
  assert_true(_cleanup_has(older.forms, 1));
  assert_true(_cleanup_has(older.forms, 2));
  assert_false(_cleanup_has(older.forms, 3));

  _cleanup_free(older.forms);
  _cleanup_free(flexi_dev.allforms);
  flexi_dev.allforms = NULL;
  g_list_free(history);
}

// an older item of a *different* module is still in effect: its group stays
// in the newer snapshot too
static void test_cleanup_keeps_shapes_another_module_still_uses(void **state)
{
  flexi_build("u{1}");
  dt_iop_module_t mod_a = { 0 }, mod_b = { 0 };
  dt_develop_blend_params_t with_mask = { 0 }, no_mask = { 0 };
  with_mask.mask_id = CLEANUP_GROUP;
  no_mask.mask_id = NO_MASKID;
  dt_dev_history_item_t older = { 0 }, newer = { 0 };
  older.module = &mod_a;
  newer.module = &mod_b;
  older.blend_params = &with_mask;
  newer.blend_params = &no_mask;
  older.forms = _cleanup_snapshot();
  newer.forms = _cleanup_snapshot();
  GList *history = g_list_append(g_list_append(NULL, &older), &newer);

  dt_masks_cleanup_unused_from_list(history);

  for(int i = 0; i < 2; i++)
  {
    GList *forms = i ? newer.forms : older.forms;
    assert_true(_cleanup_has(forms, CLEANUP_GROUP));
    assert_true(_cleanup_has(forms, 1));
    assert_true(_cleanup_has(forms, 2));
    assert_false(_cleanup_has(forms, 3));
  }

  _cleanup_free(older.forms);
  _cleanup_free(newer.forms);
  _cleanup_free(flexi_dev.allforms);
  flexi_dev.allforms = NULL;
  g_list_free(history);
}

// a locked mask keeps everything but blend mode, blend parameter and opacity
// from the params it replaces (reset, preset, style or paste)
static void test_locked_mask_survives_replacement(void **state)
{
  dt_develop_blend_params_t locked = { 0 }, incoming = { 0 };
  locked.mask_lock = 1;
  locked.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI;
  locked.mask_id = 7;
  locked.feathering_radius = 12.0f;
  locked.raster_mask_id = 3;
  locked.opacity = 40.0f;
  incoming.mask_mode = DEVELOP_MASK_DISABLED;
  incoming.mask_id = NO_MASKID;
  incoming.opacity = 80.0f;
  incoming.blend_mode = DEVELOP_BLEND_MULTIPLY;
  incoming.blend_parameter = 2.0f;

  dt_develop_blend_keep_locked_mask(&incoming, &locked);

  assert_int_equal(incoming.mask_lock, 1);
  assert_int_equal(incoming.mask_mode, DEVELOP_MASK_ENABLED | DEVELOP_MASK_FLEXI);
  assert_int_equal(incoming.mask_id, 7);
  assert_int_equal(incoming.raster_mask_id, 3);
  assert_true(incoming.feathering_radius == 12.0f);
  assert_true(incoming.opacity == 80.0f);
  assert_int_equal(incoming.blend_mode, DEVELOP_BLEND_MULTIPLY);
  assert_true(incoming.blend_parameter == 2.0f);
}

// an unlocked mask is replaced whole, and a lock the incoming params carry
// (a style or preset saved from a locked module) does not come along
static void test_incoming_lock_is_dropped(void **state)
{
  dt_develop_blend_params_t replaced = { 0 }, incoming = { 0 };
  replaced.mask_id = 7;
  incoming.mask_lock = 1;
  incoming.mask_id = 9;

  dt_develop_blend_keep_locked_mask(&incoming, &replaced);

  assert_int_equal(incoming.mask_lock, 0);
  assert_int_equal(incoming.mask_id, 9);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_teardown(test_v7_gives_every_group_point_unit_opacity, _teardown),
    cmocka_unit_test_teardown(test_v7_does_not_overwrite_an_explicit_opacity, _teardown),
    cmocka_unit_test_teardown(test_v6_same_operator_list_yields_one_group, _teardown),
    cmocka_unit_test_teardown(test_v6_operator_change_nests_old_edits, _teardown),
    cmocka_unit_test_teardown(test_group_point_strides, _teardown),
    cmocka_unit_test_teardown(test_migration_is_idempotent, _teardown),
    cmocka_unit_test_teardown(test_migration_rejects_impossible_versions, _teardown),
    cmocka_unit_test_teardown(test_migration_preserves_membership, _teardown),
    cmocka_unit_test_teardown(test_pre_v7_refinement_stays_disabled, _teardown),
    cmocka_unit_test_teardown(test_cleanup_drops_shapes_only_replaced_steps_use, _teardown),
    cmocka_unit_test_teardown(test_cleanup_keeps_shapes_another_module_still_uses, _teardown),
    cmocka_unit_test(test_locked_mask_survives_replacement),
    cmocka_unit_test(test_incoming_lock_is_dropped),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
