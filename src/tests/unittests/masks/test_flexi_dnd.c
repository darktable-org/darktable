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

// The drag-and-drop paths other than element-onto-element (which lives in
// test_flexi_model.c alongside the grouping primitives it exercises), and the
// group operations the panel's menus make: emptying a group, and finding the
// nested group a group's deletion takes.
//
// The panel offers four distinct drops, each with its own target type so they
// cannot interfere: an element onto another element, an element onto a group
// (empty or not), a whole same-kind cluster onto either of those, and a whole
// group moved against another group. A group's list holds its marker first,
// then its elements, so every one of them moves points from one group's list
// to another's: what they have to get right is where the dragged thing lands,
// and that no group's settings or place change on the way.

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

static void _name_group(dt_masks_form_t *grp, const dt_mask_id_t cid, const char *name)
{
  g_strlcpy(dt_masks_gui_group_point(grp, cid)->name, name, sizeof(dt_masks_gui_group_point(grp, cid)->name));
}

// ---------------------------------------------------------------------------
// element onto a group
// ---------------------------------------------------------------------------

// the element joins the group, landing on top of it
static void test_element_onto_group_header(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,4}}");
  assert_true(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 1, FLEXI_GID(1)));
  assert_layout("u{2,i{3,4,1}}");
}

// the group can also be named by any of its elements
static void test_element_onto_group_named_by_a_member(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,4}}");
  assert_true(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 1, 3));
  assert_layout("u{2,i{3,4,1}}");
}

// the group's operator is its marker's: an element that joins folds with it
static void test_element_onto_group_header_adopts_operator(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,d{3}}");
  assert_true(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 2, FLEXI_GID(1)));
  assert_layout("u{1,d{3,2}}");
}

// dropping an element on the header of the group it is already in is a no-op,
// not a reorder -- otherwise a stray drag silently shuffles the group
static void test_element_onto_its_own_group_header_is_a_noop(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,3,i{4}}");
  assert_false(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 2, FLEXI_GID(0)));
  assert_layout("u{1,2,3,i{4}}");
}

// the group an element leaves stays, empty, where it was: groups are only
// removed explicitly
static void test_element_onto_group_header_leaves_its_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{d{1},i{2}}");
  assert_true(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 1, FLEXI_GID(2)));
  assert_layout("u{d{},i{2,1}}");
}

static void test_element_onto_invalid_group_is_rejected(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");
  assert_false(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 1, INVALID_MASKID));
  assert_false(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 1, 12345));
  assert_layout("u{1,2}");
}

// a group's marker is not an element: it cannot be dropped into another group
static void test_marker_is_not_moved_as_an_element(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,i{2},d{3}}");
  assert_false(dt_masks_model_drop_element_onto_group(&flexi_module, grp, FLEXI_GID(0),
                                                      FLEXI_GID(1)));
  assert_false(dt_masks_model_drop_element_onto_group(&flexi_module, grp, FLEXI_GID(1),
                                                      FLEXI_GID(2)));
  assert_layout("u{1,i{2},d{3}}");
}

// ---------------------------------------------------------------------------
// element onto an empty group
// ---------------------------------------------------------------------------

// an empty group is a group like any other: the element lands in it
static void test_element_fills_an_empty_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,3,d{}}");
  assert_true(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 3, FLEXI_GID(1)));
  assert_layout("u{1,2,d{3}}");
}

static void test_element_fills_an_empty_bottom_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{i{},1,2,3}");
  assert_true(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 3, FLEXI_GID(1)));
  assert_layout("u{i{3},1,2}");
}

// the sole member of a group dropped onto the empty group below it: the
// groups keep their places, only the element moves
static void test_filling_the_group_below_keeps_their_order(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{d{},u{1},i{2}}");
  assert_true(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 1, FLEXI_GID(1)));
  assert_layout("u{d{1},u{},i{2}}");
}

// ...and the same move upwards
static void test_filling_the_group_above_keeps_their_order(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{u{1},d{},i{2}}");
  assert_true(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 1, FLEXI_GID(2)));
  assert_layout("u{u{},d{1},i{2}}");
}

// filling a group keeps the number it showed while empty: its id did not change
static void test_filling_a_group_keeps_its_number(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,3,d{}}");
  flexi_set_ordinal(FLEXI_GID(1), 4);
  dt_masks_model_drop_element_onto_group(&flexi_module, grp, 3, FLEXI_GID(1));
  assert_int_equal(flexi_get_ordinal(dt_masks_gui_group_cid_of_form(grp, 3)), 4);
}

// ---------------------------------------------------------------------------
// moves never change a group's settings
// ---------------------------------------------------------------------------

// a group's name, and every other setting, lives on its marker: moving
// elements between groups renames none of them
static void test_moving_elements_renames_no_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{u{1},d{},i{2}}");
  _name_group(grp, FLEXI_GID(1), "sky");
  _name_group(grp, FLEXI_GID(3), "trees");

  assert_true(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 1, FLEXI_GID(2)));
  assert_true(dt_masks_model_drop_element_onto_element(&flexi_module, grp, 2, 1, TRUE));
  assert_layout("u{u{},d{1,2},i{}}");
  assert_string_equal(dt_masks_gui_group_point(grp, FLEXI_GID(1))->name, "sky");
  assert_string_equal(dt_masks_gui_group_point(grp, FLEXI_GID(2))->name, "");
  assert_string_equal(dt_masks_gui_group_point(grp, FLEXI_GID(3))->name, "trees");
}

static void test_moving_elements_leaves_every_group_setting(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{u{1},i{2,3}}");
  dt_masks_point_group_t *from = dt_masks_gui_group_point(grp, FLEXI_GID(1));
  dt_masks_point_group_t *to = dt_masks_gui_group_point(grp, FLEXI_GID(2));
  from->group_opacity = 0.8f;
  to->state |= DT_MASKS_STATE_FLEXI_SCREEN;
  to->group_opacity = 0.5f;
  to->refinement = (dt_masks_refinement_t){ .enabled = DT_MASKS_REFINE_GROUP,
                                            .blur_radius = 3.0f };
  const dt_masks_point_group_t from_before = *from, to_before = *to;

  assert_true(dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 2, FALSE));
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, 1), FLEXI_GID(2));
  assert_memory_equal(from, &from_before, sizeof(from_before));
  assert_memory_equal(to, &to_before, sizeof(to_before));
}

// an element's own refinement is its own, whatever group it is in
static void test_moving_keeps_the_elements_own_refinement(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{u{1,4},i{2,3}}");
  dt_masks_gui_group_point(grp, 1)->refinement = (dt_masks_refinement_t){
    .enabled = DT_MASKS_REFINE_ELEMENT, .blur_radius = 7.0f };

  assert_true(dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 2, TRUE));
  assert_int_equal(dt_masks_gui_group_point(grp, 1)->refinement.enabled, DT_MASKS_REFINE_ELEMENT);
  assert_float_equal(dt_masks_gui_group_point(grp, 1)->refinement.blur_radius, 7.0f, 1e-6);
}

// a row drop moving a group's sole member under the first element of another
// group: both groups keep their places, the one emptied too
static void test_row_drop_from_a_sole_member_keeps_group_order(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{u{1},i{2,3}}");
  assert_true(dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 2, FALSE));
  assert_layout("u{u{},i{1,2,3}}");
}

// ---------------------------------------------------------------------------
// whole-cluster drops
// ---------------------------------------------------------------------------

static GList *_ids(const int a, const int b)
{
  GList *l = g_list_append(NULL, GINT_TO_POINTER(a));
  return g_list_append(l, GINT_TO_POINTER(b));
}

// every member moves together, as one contiguous block, keeping their relative
// order
static void test_cluster_onto_group_header(void **state)
{
  flexi_build("u{1,2,3,i{4}}");
  GList *ids = _ids(2, 3);

  assert_true(dt_masks_gui_cluster_move(&flexi_module, ids, FLEXI_GID(1), TRUE, FALSE));
  g_list_free(ids);
  assert_layout("u{1,i{4,2,3}}");
}

static void test_cluster_onto_element_row(void **state)
{
  flexi_build("u{1,2,3,i{4,5}}");
  GList *ids = _ids(2, 3);

  assert_true(dt_masks_gui_cluster_move(&flexi_module, ids, 4, FALSE, TRUE));
  g_list_free(ids);
  assert_layout("u{1,i{4,2,3,5}}");
}

static void test_cluster_onto_an_empty_group(void **state)
{
  flexi_build("u{1,2,3,d{}}");
  GList *ids = _ids(2, 3);

  assert_true(dt_masks_gui_cluster_move(&flexi_module, ids, FLEXI_GID(1), TRUE, FALSE));
  g_list_free(ids);
  assert_layout("u{1,d{2,3}}");
}

// moving out every member of a group leaves it, empty, where it was
static void test_cluster_emptying_group_leaves_it(void **state)
{
  flexi_build("u{1,2,i{3,4}}");
  GList *ids = _ids(3, 4);

  assert_true(dt_masks_gui_cluster_move(&flexi_module, ids, FLEXI_GID(0), TRUE, FALSE));
  g_list_free(ids);
  assert_layout("u{1,2,i{},3,4}");
}

// a cluster dropped on the header of the group it is in stays where it is
static void test_cluster_onto_its_own_group_is_a_noop(void **state)
{
  flexi_build("u{1,2,3,i{4}}");
  GList *ids = _ids(2, 3);

  assert_false(dt_masks_gui_cluster_move(&flexi_module, ids, FLEXI_GID(0), TRUE, FALSE));
  g_list_free(ids);
  assert_layout("u{1,2,3,i{4}}");
}

static void test_cluster_move_with_no_members_is_rejected(void **state)
{
  flexi_build("u{1,2}");
  assert_false(dt_masks_gui_cluster_move(&flexi_module, NULL, FLEXI_GID(0), TRUE, FALSE));
  assert_layout("u{1,2}");
}

// ---------------------------------------------------------------------------
// emptying and deleting groups
// ---------------------------------------------------------------------------

static void test_emptying_a_group_keeps_it(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,i{2,3},d{4}}");
  _name_group(grp, FLEXI_GID(1), "sky");
  GList *gone = dt_masks_model_empty_group(grp, FLEXI_GID(1));
  assert_int_equal(g_list_length(gone), 2);
  g_list_free(gone);
  assert_layout("u{1,i{},d{4}}");
  assert_string_equal(dt_masks_gui_group_point(grp, FLEXI_GID(1))->name, "sky");
}

// deleting a group deletes the nested group form whose marker it is, with
// what it holds. The mask's own group is the mask: it has none
static void test_a_group_deletes_as_its_nested_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,i{2,d{3}}}");
  const dt_masks_form_t *i = dt_masks_model_nested_group_of(grp, FLEXI_GID(1));
  const dt_masks_form_t *d = dt_masks_model_nested_group_of(grp, FLEXI_GID(2));
  assert_non_null(i);
  assert_non_null(d);
  assert_int_equal(i->formid, FLEXI_SUB(1));
  assert_int_equal(d->formid, FLEXI_SUB(2));
  assert_null(dt_masks_model_nested_group_of(grp, FLEXI_GID(0)));
  // an element is no group
  assert_null(dt_masks_model_nested_group_of(grp, 2));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_teardown(test_element_onto_group_header, _teardown),
    cmocka_unit_test_teardown(test_element_onto_group_named_by_a_member, _teardown),
    cmocka_unit_test_teardown(test_element_onto_group_header_adopts_operator, _teardown),
    cmocka_unit_test_teardown(test_element_onto_its_own_group_header_is_a_noop, _teardown),
    cmocka_unit_test_teardown(test_element_onto_group_header_leaves_its_group, _teardown),
    cmocka_unit_test_teardown(test_element_onto_invalid_group_is_rejected, _teardown),
    cmocka_unit_test_teardown(test_marker_is_not_moved_as_an_element, _teardown),
    cmocka_unit_test_teardown(test_element_fills_an_empty_group, _teardown),
    cmocka_unit_test_teardown(test_element_fills_an_empty_bottom_group, _teardown),
    cmocka_unit_test_teardown(test_filling_the_group_below_keeps_their_order, _teardown),
    cmocka_unit_test_teardown(test_filling_the_group_above_keeps_their_order, _teardown),
    cmocka_unit_test_teardown(test_filling_a_group_keeps_its_number, _teardown),
    cmocka_unit_test_teardown(test_moving_elements_renames_no_group, _teardown),
    cmocka_unit_test_teardown(test_moving_elements_leaves_every_group_setting, _teardown),
    cmocka_unit_test_teardown(test_moving_keeps_the_elements_own_refinement, _teardown),
    cmocka_unit_test_teardown(test_row_drop_from_a_sole_member_keeps_group_order, _teardown),
    cmocka_unit_test_teardown(test_cluster_onto_group_header, _teardown),
    cmocka_unit_test_teardown(test_cluster_onto_element_row, _teardown),
    cmocka_unit_test_teardown(test_cluster_onto_an_empty_group, _teardown),
    cmocka_unit_test_teardown(test_cluster_emptying_group_leaves_it, _teardown),
    cmocka_unit_test_teardown(test_cluster_onto_its_own_group_is_a_noop, _teardown),
    cmocka_unit_test_teardown(test_cluster_move_with_no_members_is_rejected, _teardown),
    cmocka_unit_test_teardown(test_emptying_a_group_keeps_it, _teardown),
    cmocka_unit_test_teardown(test_a_group_deletes_as_its_nested_group, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
