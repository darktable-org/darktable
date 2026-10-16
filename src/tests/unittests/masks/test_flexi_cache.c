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

// Cache-invalidation tests for the flexi mask model.
//
// dt_masks_group_hash() is what tells the pixelpipe whether a mask still
// renders the same. Every value the group renderer reads must therefore feed
// it, and anything that does not affect rendering must not -- the two failure
// modes are opposite and both bad:
//
//   * a rendering input missing from the hash    -> the edit does not appear
//     (stale cached mask; the classic "I moved the slider and nothing
//     happened until I forced a reprocess")
//   * a non-rendering value included in the hash -> everything recomputes on
//     cosmetic changes (renaming a group should not re-render the image)
//
// Both are invisible in ordinary use until someone notices the wrong thing
// happening, and neither shows up in a pixel-comparison suite, because the
// rendering itself is correct -- it is the decision to *re*-render that is
// wrong. That makes them a good fit for exactly this kind of test.
//
// The pattern throughout: hash, mutate one field, hash again, assert whether
// the two differ.

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

static dt_hash_t _hash(void)
{
  return dt_masks_group_hash(DT_INITHASH, flexi_group());
}

// mutate via the callback, and assert the hash did / did not move
#define assert_invalidates(what, mutation)                                     \
  do {                                                                         \
    const dt_hash_t before = _hash();                                          \
    mutation;                                                                  \
    const dt_hash_t after = _hash();                                           \
    if(before == after)                                                        \
      fail_msg("%s did not invalidate the mask cache "                         \
               "(hash unchanged) -- the edit will not appear until "           \
               "something else forces a reprocess", what);                     \
  } while(0)

#define assert_preserves(what, mutation)                                       \
  do {                                                                         \
    const dt_hash_t before = _hash();                                          \
    mutation;                                                                  \
    const dt_hash_t after = _hash();                                           \
    if(before != after)                                                        \
      fail_msg("%s invalidated the mask cache, forcing a needless "            \
               "recompute -- it is not a rendering input", what);              \
  } while(0)

// ---------------------------------------------------------------------------
// the hash itself
// ---------------------------------------------------------------------------

static void test_hash_is_stable(void **state)
{
  flexi_build("u{1,2,i{3}}");
  assert_int_equal(_hash(), _hash());
}

static void test_distinct_layouts_hash_differently(void **state)
{
  flexi_build("u{1,2,i{3}}");
  const dt_hash_t a = _hash();
  flexi_teardown();

  flexi_build("u{1,i{2,3}}");
  assert_int_not_equal(a, _hash());
}

// ---------------------------------------------------------------------------
// structural edits
// ---------------------------------------------------------------------------

static void test_adding_a_shape_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");
  assert_invalidates("adding a shape", ({
    dt_masks_point_group_t *pt = calloc(1, sizeof(dt_masks_point_group_t));
    pt->formid = 2; // reuse an existing form so the id resolves
    pt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    pt->opacity = 1.0f;
    grp->points = g_list_append(grp->points, pt);
  }));
}

static void test_removing_a_shape_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,3}");
  assert_invalidates("removing a shape", ({
    dt_masks_point_group_t *pt = dt_masks_gui_group_point(grp, 2);
    grp->points = g_list_remove(grp->points, pt);
    free(pt);
  }));
}

static void test_reordering_shapes_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,3}");
  // order is a rendering input: the operators fold in list order
  assert_invalidates("reordering shapes", ({
    dt_masks_point_group_t *pt = dt_masks_gui_group_point(grp, 1);
    grp->points = g_list_remove(grp->points, pt);
    grp->points = g_list_append(grp->points, pt);
  }));
}

static void test_moving_a_shape_between_groups_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,4}}");
  assert_invalidates("moving a shape to another group", ({
    dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 3, TRUE);
  }));
}

// ---------------------------------------------------------------------------
// per-element controls
// ---------------------------------------------------------------------------

static void test_shape_opacity_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");
  assert_invalidates("changing a shape's opacity",
                     dt_masks_gui_group_point(grp, 2)->opacity = 0.5f);
}

static void test_operator_change_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3}}");
  assert_invalidates("changing a group's operator", ({
    dt_masks_point_group_t *mk = dt_masks_gui_group_point(grp, FLEXI_GID(1));
    mk->state = (mk->state & ~DT_MASKS_STATE_FLEXI_OP) | DT_MASKS_STATE_FLEXI_DIFFERENCE;
  }));
}

static void test_invert_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");
  assert_invalidates("inverting an element",
                     dt_masks_gui_group_point(grp, 1)->state |= DT_MASKS_STATE_INVERSE);
}

// solo/mute hide other elements from the render, so they must invalidate
static void test_solo_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3}}");
  assert_invalidates("soloing an element (hiding its peers)", ({
    dt_masks_gui_group_point(grp, 2)->state |= DT_MASKS_STATE_HIDDEN;
    dt_masks_gui_group_point(grp, 3)->state |= DT_MASKS_STATE_HIDDEN;
  }));
}

static void test_unsolo_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");
  dt_masks_gui_group_point(grp, 2)->state |= DT_MASKS_STATE_HIDDEN;
  assert_invalidates("clearing solo",
                     dt_masks_gui_group_point(grp, 2)->state &= ~DT_MASKS_STATE_HIDDEN);
}

static void test_bypass_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3}}");
  assert_invalidates("bypassing a group",
                     dt_masks_gui_group_point(grp, FLEXI_GID(1))->state |= DT_MASKS_STATE_OP_BYPASS);
}

// ---------------------------------------------------------------------------
// refinement, at each level it can be set
// ---------------------------------------------------------------------------

static void test_shape_refinement_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");
  assert_invalidates("per-shape refinement (feathering radius)", ({
    dt_masks_point_group_t *pt = dt_masks_gui_group_point(grp, 1);
    pt->refinement.enabled = 1;
    pt->refinement.feathering_radius = 4.0f;
  }));
}

static void test_shape_refinement_blur_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");
  assert_invalidates("per-shape refinement (blur radius)", ({
    dt_masks_point_group_t *pt = dt_masks_gui_group_point(grp, 1);
    pt->refinement.enabled = 1;
    pt->refinement.blur_radius = 3.0f;
  }));
}

// a group's refinement is held by its marker, and setting it must move the
// hash exactly as a per-shape one does
static void test_group_refinement_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,4}}");
  assert_invalidates("group-level refinement", ({
    dt_masks_point_group_t *marker = dt_masks_gui_group_point(grp, FLEXI_GID(1));
    marker->refinement.enabled = DT_MASKS_REFINE_GROUP;
    marker->refinement.contrast = 0.4f;
  }));
}

static void test_refinement_disable_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");
  dt_masks_point_group_t *pt = dt_masks_gui_group_point(grp, 1);
  pt->refinement.enabled = 1;
  pt->refinement.blur_radius = 3.0f;
  assert_invalidates("turning a refinement back off", pt->refinement.enabled = 0);
}

// ---------------------------------------------------------------------------
// group-level opacity
// ---------------------------------------------------------------------------

// group_opacity multiplies the group's finished sub-mask at render time (see
// _group_get_mask_roi_flexi, masks/group.c) -- it is as much a rendering input
// as a per-shape opacity.
static void test_group_opacity_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3}}");
  assert_invalidates("changing a group's opacity",
                     dt_masks_gui_group_point(grp, FLEXI_GID(0))->group_opacity = 0.5f);
}

// ---------------------------------------------------------------------------
// group markers
// ---------------------------------------------------------------------------

// a group's settings are held by its marker (DT_MASKS_STATE_GROUP_MARKER),
// which refers to no form, so the hash cannot reach them through a form
// lookup the way it reaches a member's. A nested group's, here
static void test_marker_settings_invalidate(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3}}");
  dt_masks_point_group_t *m = dt_masks_gui_group_point(grp, FLEXI_GID(1));
  assert_invalidates("changing a group's operator on its marker",
                     m->state = DT_MASKS_STATE_GROUP_MARKER | DT_MASKS_STATE_FLEXI_DIFFERENCE);
  assert_invalidates("changing a group's opacity on its marker",
                     m->group_opacity = 0.5f);
  assert_invalidates("setting a group's refinement on its marker", ({
    m->refinement.enabled = DT_MASKS_REFINE_GROUP;
    m->refinement.blur_radius = 2.0f;
  }));
}

static void test_adding_a_group_invalidates(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,3}");
  assert_invalidates("adding a nested group",
                     dt_masks_model_nest_new_group(grp, DT_MASKS_STATE_FLEXI_MINIMUM, FLEXI_GID(0)));
}

// ---------------------------------------------------------------------------
// cosmetic changes must NOT invalidate
// ---------------------------------------------------------------------------

// These are the half of the contract that is easy to get wrong in the other
// direction. Panel state that describes how the mask is *presented* or
// *edited* -- not what it renders to -- must leave the hash alone, or every
// cosmetic interaction drags a full mask recompute (and, on a big image, a
// visible stall) behind it.
//
// Several of these look tautological today, because the state they poke lives
// in blend_data rather than in the group. That is exactly what makes them
// worth keeping: they are the tripwire for someone later storing presentation
// state inside dt_masks_point_group_t, where it would silently start
// invalidating the cache.

static void test_renaming_a_group_does_not_invalidate(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");
  dt_masks_point_group_t *marker = dt_masks_gui_group_point(grp, FLEXI_GID(0));
  assert_preserves("renaming a group",
                   g_strlcpy(marker->name, "sky", sizeof(marker->name)));
}

// Solo-EDIT narrows which shapes are editable on canvas. That is an editing
// scope, not a rendering one -- unlike solo/mute (DT_MASKS_STATE_HIDDEN), which
// changes what the mask renders to and must invalidate. The two are easy to
// conflate; this pins them apart.
static void test_solo_edit_does_not_invalidate(void **state)
{
  flexi_build("u{1,2,i{3}}");
  assert_preserves("entering solo-edit on one shape",
                   flexi_bd.soloedit_formid = 2);
  assert_preserves("leaving solo-edit",
                   flexi_bd.soloedit_formid = INVALID_MASKID);
}

static void test_selection_does_not_invalidate(void **state)
{
  flexi_build("u{1,2,i{3,4}}");
  assert_preserves("selecting an element", ({
    flexi_bd.panel_selected_formid = 4;
    flexi_bd.panel_selected_group_cid = FLEXI_GID(1);
  }));
  assert_preserves("clearing the selection", ({
    flexi_bd.panel_selected_formid = INVALID_MASKID;
    flexi_bd.panel_selected_group_cid = INVALID_MASKID;
  }));
}

// expanding or collapsing a same-kind cluster is a purely visual sub-grouping
// of a group's rows -- it does not change the group, let alone the mask
static void test_collapse_expand_does_not_invalidate(void **state)
{
  flexi_build("u{1,2,3,i{4}}");
  flexi_bd.masks_cluster_expanded = g_hash_table_new(g_direct_hash, g_direct_equal);

  assert_preserves("collapsing a cluster",
                   g_hash_table_insert(flexi_bd.masks_cluster_expanded,
                                       GINT_TO_POINTER(1),
                                       GINT_TO_POINTER(FALSE)));
  assert_preserves("expanding it again",
                   g_hash_table_insert(flexi_bd.masks_cluster_expanded,
                                       GINT_TO_POINTER(1),
                                       GINT_TO_POINTER(TRUE)));

  g_hash_table_destroy(flexi_bd.masks_cluster_expanded);
  flexi_bd.masks_cluster_expanded = NULL;
}

// solo bookkeeping in blend_data mirrors the HIDDEN bits it sets; the bits are
// the rendering input (and do invalidate, above), the bookkeeping is not
static void test_solo_bookkeeping_alone_does_not_invalidate(void **state)
{
  flexi_build("u{1,2,i{3}}");
  assert_preserves("recording which element is soloed", ({
    flexi_bd.solo_formid = 2;
    flexi_bd.solo_group_key = 16;
  }));
}

// canvas edit mode (showing shape outlines / handles) draws on top of the
// image; it does not change the mask
static void test_edit_mode_does_not_invalidate(void **state)
{
  flexi_build("u{1,2}");
  assert_preserves("turning canvas edit mode on",
                   flexi_bd.masks_shown = DT_MASKS_EDIT_FULL);
  assert_preserves("turning it off",
                   flexi_bd.masks_shown = DT_MASKS_EDIT_OFF);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_teardown(test_hash_is_stable, _teardown),
    cmocka_unit_test_teardown(test_distinct_layouts_hash_differently, _teardown),
    cmocka_unit_test_teardown(test_adding_a_shape_invalidates, _teardown),
    cmocka_unit_test_teardown(test_removing_a_shape_invalidates, _teardown),
    cmocka_unit_test_teardown(test_reordering_shapes_invalidates, _teardown),
    cmocka_unit_test_teardown(test_moving_a_shape_between_groups_invalidates, _teardown),
    cmocka_unit_test_teardown(test_shape_opacity_invalidates, _teardown),
    cmocka_unit_test_teardown(test_operator_change_invalidates, _teardown),
    cmocka_unit_test_teardown(test_invert_invalidates, _teardown),
    cmocka_unit_test_teardown(test_solo_invalidates, _teardown),
    cmocka_unit_test_teardown(test_unsolo_invalidates, _teardown),
    cmocka_unit_test_teardown(test_bypass_invalidates, _teardown),
    cmocka_unit_test_teardown(test_shape_refinement_invalidates, _teardown),
    cmocka_unit_test_teardown(test_shape_refinement_blur_invalidates, _teardown),
    cmocka_unit_test_teardown(test_group_refinement_invalidates, _teardown),
    cmocka_unit_test_teardown(test_refinement_disable_invalidates, _teardown),
    cmocka_unit_test_teardown(test_group_opacity_invalidates, _teardown),
    cmocka_unit_test_teardown(test_marker_settings_invalidate, _teardown),
    cmocka_unit_test_teardown(test_adding_a_group_invalidates, _teardown),
    cmocka_unit_test_teardown(test_renaming_a_group_does_not_invalidate, _teardown),
    cmocka_unit_test_teardown(test_solo_edit_does_not_invalidate, _teardown),
    cmocka_unit_test_teardown(test_selection_does_not_invalidate, _teardown),
    cmocka_unit_test_teardown(test_collapse_expand_does_not_invalidate, _teardown),
    cmocka_unit_test_teardown(test_solo_bookkeeping_alone_does_not_invalidate, _teardown),
    cmocka_unit_test_teardown(test_edit_mode_does_not_invalidate, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
