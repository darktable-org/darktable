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

// Behavioural regression tests for the flexi masks panel's model layer:
// grouping, drag-and-drop and selection, expressed as layout strings (see
// flexi_fixture.h). These run headless -- no GTK, no display -- because every
// behaviour asserted here lives in functions that take a mask group and plain
// values, not widgets.
//
// What this suite deliberately does NOT cover: anything that is a property of
// GTK itself rather than of the panel's logic: event propagation between
// nested widgets, CSS rendering, tooltip delivery, widget packing. Testing
// those needs real GTK event delivery on a real widget tree, with a display,
// and would be too brittle to trust. They stay a manual checklist; see
// README.md.

#include "flexi_fixture.h"
#include "develop/develop.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

static int _teardown(void **state)
{
  flexi_teardown();
  return 0;
}

// ---------------------------------------------------------------------------
// the layout DSL itself -- if these are wrong every other test lies
// ---------------------------------------------------------------------------

static void test_layout_roundtrip(void **state)
{
  const char *cases[] = {
    "u{1}",
    "u{1,2,3}",
    "u{1,2,i{3}}",
    "u{1,i{2},d{3},x{4},s{5},o{6},m{7}}",
    "u{u{},i{1},d{},x{}}",
    "u~@0.5{1~,i@0.25{2,d{3}}@0.5}",
    NULL,
  };
  for(int i = 0; cases[i]; i++)
  {
    flexi_build(cases[i]);
    assert_layout(cases[i]);
    flexi_teardown();
  }
}

// a nested group folding with its holder's operator is still a group of its
// own: only simplify replaces it by its members
static void test_a_same_op_nested_group_stays_separate(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,u{3}}");
  assert_layout("u{1,2,u{3}}");
  const dt_masks_form_t *sub = dt_masks_model_nested_group_of(grp, FLEXI_GID(1));
  assert_non_null(sub);
  assert_int_equal(sub->formid, FLEXI_SUB(1));
}

// ---------------------------------------------------------------------------
// group membership queries
// ---------------------------------------------------------------------------

// a group's id is its marker's, and a marker is in its own group. A nested
// group is a member of the group holding it
static void test_cid_of_form_is_the_groups_marker(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,4},d{}}");

  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, 1), FLEXI_GID(0));
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, 2), FLEXI_GID(0));
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, 3), FLEXI_GID(1));
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, 4), FLEXI_GID(1));
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, FLEXI_GID(2)), FLEXI_GID(2));
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, FLEXI_SUB(1)), FLEXI_GID(0));
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, 99), INVALID_MASKID);
}

static void test_selected_group_formids(void **state)
{
  flexi_build("u{1,2,i{3,4,5}}");
  GList *run = dt_masks_model_group_members(flexi_group(), 4);
  assert_int_equal(g_list_length(run), 3);
  g_list_free(run);

  // the nested group is a member of the mask's own
  run = dt_masks_model_group_members(flexi_group(), 1);
  assert_int_equal(g_list_length(run), 3);
  assert_int_equal(GPOINTER_TO_INT(run->data), FLEXI_SUB(1));
  g_list_free(run);

  // by the group's own id too, and the marker is no member
  run = dt_masks_model_group_members(flexi_group(), FLEXI_GID(1));
  assert_int_equal(g_list_length(run), 3);
  assert_int_equal(GPOINTER_TO_INT(run->data), 5);
  g_list_free(run);
}

static void test_empty_group_has_no_members(void **state)
{
  flexi_build("u{1,i{}}");
  assert_null(dt_masks_model_group_members(flexi_group(), FLEXI_GID(1)));
}

// ---------------------------------------------------------------------------
// drag and drop: element onto element
// ---------------------------------------------------------------------------

static void test_drop_element_into_other_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,4}}");

  // drop 1 onto 3, landing above it
  assert_true(dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 3, TRUE));
  assert_layout("u{2,i{3,1,4}}");
}

static void test_drop_element_below_target(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,4}}");

  assert_true(dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 3, FALSE));
  assert_layout("u{2,i{1,3,4}}");
}

// the dragged element is in its new group, whose operator is its marker's
static void test_drop_adopts_target_operator(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,d{3}}");

  assert_true(dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 3, TRUE));
  assert_layout("u{2,d{3,1}}");
}

// a move between two groups makes no group, whichever side of the target the
// element lands on
static void test_drop_between_groups_never_creates_a_third(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,4}}");
  dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 3, TRUE);
  assert_layout("u{2,i{3,1,4}}");
  flexi_teardown();

  grp = flexi_build("u{1,2,i{3,4}}");
  dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 3, FALSE);
  assert_layout("u{2,i{1,3,4}}");
}

// ...including when the two groups share an operator
static void test_drop_between_same_op_groups_keeps_both(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,u{3,4}}");

  dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 3, TRUE);
  assert_layout("u{2,u{3,1,4}}");
}

// out of a nested group, below the mask's bottom member
static void test_drop_onto_bottom_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,4}}");

  dt_masks_model_drop_element_onto_element(&flexi_module, grp, 3, 1, FALSE);
  assert_layout("u{3,1,2,i{4}}");
}

// Emptying a group leaves it where it was, so it does not silently vanish when
// its last member is dragged out.
static void test_drop_emptying_group_keeps_it(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{u{1},i{2,3}}");
  dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 2, TRUE);
  assert_layout("u{u{},i{2,1,3}}");
}

// ---------------------------------------------------------------------------
// members whose form is gone
// ---------------------------------------------------------------------------

// the form leaves dev->forms' lookup by moving to an id nothing references, so
// the fixture still owns and frees it
static void _lose_form(const dt_mask_id_t fid)
{
  dt_masks_get_from_id(&flexi_dev, fid)->formid = 999999;
}

// they render nothing and have no row; the group they were in stays
static void test_prune_drops_lost_members_and_keeps_their_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3}}");
  _lose_form(3);
  assert_int_equal(dt_masks_model_prune_dangling_members(grp), 1);
  assert_layout("u{1,2,i{}}");
}

// one lost form, referenced three times: every reference goes
static void test_prune_drops_every_reference_to_a_lost_form(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,i{2}}");
  for(int k = 0; k < 2; k++)
  {
    dt_masks_point_group_t *pt = calloc(1, sizeof(dt_masks_point_group_t));
    pt->formid = 2;
    pt->parentid = grp->formid;
    pt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    pt->opacity = 1.0f;
    grp->points = g_list_append(grp->points, pt);
  }
  _lose_form(2);
  assert_int_equal(dt_masks_model_prune_dangling_members(grp), 3);
  assert_layout("u{1,i{}}");
}

// a lost member leaves the rest of its group, and the groups around it, as
// they were
static void test_prune_keeps_the_rest_of_a_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,i{2,3},d{4}}");
  _lose_form(2);
  assert_int_equal(dt_masks_model_prune_dangling_members(grp), 1);
  assert_layout("u{1,i{3},d{4}}");
}

static void test_prune_without_lost_members_changes_nothing(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3}}");
  assert_int_equal(dt_masks_model_prune_dangling_members(grp), 0);
  assert_layout("u{1,2,i{3}}");
}

// ---------------------------------------------------------------------------
// the classic migration: a classic list becomes groups
// ---------------------------------------------------------------------------

// members lost from every group leave the groups, empty
static void test_lost_members_leave_their_groups(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{u{1},i{2}}");
  _lose_form(1);
  _lose_form(2);
  dt_masks_model_prune_dangling_members(grp);
  assert_layout("u{u{},i{}}");
}

// the classic migration folds a classic list into one group per run of
// members sharing an operator, what came before becoming the next group's
// first member. The first member is the base whatever operator it carries
static void test_classic_marking_folds_each_run_into_a_group(void **state)
{
  dt_masks_form_t *grp = flexi_build_classic("1,2,i3,i4");
  assert_true(dt_masks_group_mark_classic_runs(&flexi_dev.forms, grp, NULL));
  assert_tree(grp, "i{u{1,2},3,4}");
  assert_false(dt_masks_group_mark_classic_runs(&flexi_dev.forms, grp, NULL));
}

// one operator throughout is one group, the base's own operator ignored
static void test_classic_marking_keeps_one_operator_one_group(void **state)
{
  dt_masks_form_t *grp = flexi_build_classic("d1,d2,d3");
  assert_true(dt_masks_group_mark_classic_runs(&flexi_dev.forms, grp, NULL));
  assert_tree(grp, "d{1,2,3}");
}

// a lone base joins the run above it: `1 - 2 - 3 - 4` is one difference group
static void test_classic_marking_puts_the_base_in_the_first_run(void **state)
{
  dt_masks_form_t *grp = flexi_build_classic("1,d2,d3,d4");
  assert_true(dt_masks_group_mark_classic_runs(&flexi_dev.forms, grp, NULL));
  assert_tree(grp, "d{1,2,3,4}");
}

// every operator change nests what came before, so the order classic applied
// the members in is kept
static void test_classic_marking_nests_at_each_operator_change(void **state)
{
  dt_masks_form_t *grp = flexi_build_classic("1,2,d3,4");
  assert_true(dt_masks_group_mark_classic_runs(&flexi_dev.forms, grp, NULL));
  assert_tree(grp, "u{d{u{1,2},3},4}");
}

// a faded member keeps its fade: the group applies it member by member, as
// classic did
static void test_a_faded_member_keeps_its_opacity(void **state)
{
  dt_masks_form_t *grp = flexi_build_classic("1,d2,d3");
  dt_masks_gui_group_point(grp, 3)->opacity = 0.5f;
  assert_true(dt_masks_group_mark_classic_runs(&flexi_dev.forms, grp, NULL));
  assert_tree(grp, "d{1,2,3@0.5}");
}

// a member becomes a plain element: it keeps its own opacity and inversion,
// and nothing that belongs to a group
static void test_marking_leaves_members_plain(void **state)
{
  dt_masks_form_t *grp = flexi_build_classic("1,i2,i3");
  dt_masks_point_group_t *pt = dt_masks_gui_group_point(grp, 2);
  pt->state |= DT_MASKS_STATE_FLEXI_SCREEN | DT_MASKS_STATE_INVERSE;
  pt->opacity = 0.7f;
  pt->group_opacity = 0.5f;
  g_strlcpy(pt->name, "sky", sizeof(pt->name));
  pt->refinement = (dt_masks_refinement_t){ .enabled = DT_MASKS_REFINE_GROUP,
                                            .blur_radius = 2.0f };
  dt_masks_group_mark_classic_runs(&flexi_dev.forms, grp, NULL);
  assert_tree(grp, "i{1,2~@0.7,3}");

  const dt_masks_point_group_t *member = dt_masks_gui_group_point(grp, 2);
  assert_int_equal(member->state & DT_MASKS_STATE_OP, 0);
  assert_int_equal(member->state & DT_MASKS_STATE_FLEXI_OP, 0);
  assert_float_equal(member->group_opacity, 1.0f, 1e-6f);
  assert_string_equal(member->name, "");
  assert_int_equal(member->refinement.enabled, DT_MASKS_REFINE_OFF);
}

// the same run marked twice -- say in two history snapshots -- gets the same
// id, which the panel keys selection and numbering on
static void test_marking_the_same_run_twice_gives_the_same_id(void **state)
{
  dt_masks_form_t *grp = flexi_build_classic("1,i2");
  dt_masks_group_mark_classic_runs(&flexi_dev.forms, grp, NULL);
  const dt_mask_id_t first = dt_masks_gui_group_cid_of_form(grp, 2);
  flexi_teardown();

  grp = flexi_build_classic("1,i2");
  dt_masks_group_mark_classic_runs(&flexi_dev.forms, grp, NULL);
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, 2), first);
}

static void test_drop_onto_self_is_rejected(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3}}");

  assert_false(dt_masks_model_drop_element_onto_element(&flexi_module, grp, 2, 2, TRUE));
  assert_layout("u{1,2,i{3}}");
}

static void test_drop_of_unknown_element_is_rejected(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");

  assert_false(dt_masks_model_drop_element_onto_element(&flexi_module, grp, 77, 1, TRUE));
  assert_layout("u{1,2}");
}

// ---------------------------------------------------------------------------
// selection state after a gesture
// ---------------------------------------------------------------------------

// A moved element stays selected, and its recorded group follows it to its new
// group -- otherwise the panel highlights the group it came from.
static void test_drop_keeps_element_selected_in_new_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,4}}");

  dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 3, TRUE);
  assert_int_equal(flexi_bd.panel_selected_formid, 1);
  assert_int_equal(flexi_bd.panel_selected_group_cid, dt_masks_gui_group_cid_of_form(grp, 1));
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(1));
}

// ---------------------------------------------------------------------------
// the selection state machine
//
// The contract is that every reachable state is one click away. Each test
// below is one step of a click sequence a user can actually perform, applied
// through the same decision functions the click handlers use.
// ---------------------------------------------------------------------------

// apply a decision to the fixture's blend_data, the way the real callers do
static void _apply(const dt_masks_panel_sel_t s)
{
  flexi_bd.panel_selected_formid = s.formid;
  flexi_bd.panel_selected_group_cid = s.group_cid;
}

static void _click_element(const dt_mask_id_t id)
{
  _apply(dt_masks_model_click_element(&flexi_bd, flexi_group(), id));
}

static void _click_group(const dt_mask_id_t cid)
{
  _apply(dt_masks_model_click_group(&flexi_bd, cid));
}

static void test_click_group_selects_it(void **state)
{
  flexi_build("u{1,2,i{3}}");
  _click_group(FLEXI_GID(1));
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(1));
  assert_int_equal(flexi_bd.panel_selected_formid, INVALID_MASKID);
}

// deselecting a group selects the mask's own group: one group is always
// selected
static void test_click_selected_group_selects_the_whole_mask(void **state)
{
  flexi_build("u{1,2,i{3}}");
  _click_group(FLEXI_GID(1));
  _click_group(FLEXI_GID(1));
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(0));
  assert_int_equal(flexi_bd.panel_selected_formid, INVALID_MASKID);
}

// the mask's own group cannot be deselected: one group is always selected
static void test_click_mask_group_again_keeps_it_selected(void **state)
{
  flexi_build("u{1,2}");
  _click_group(FLEXI_GID(0));
  _click_group(FLEXI_GID(0));
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(0));
  assert_int_equal(flexi_bd.panel_selected_formid, INVALID_MASKID);
}

// selecting an element selects its group too -- the two levels are nested,
// not independent
static void test_click_element_selects_element_and_its_group(void **state)
{
  flexi_build("u{1,2,i{3,4}}");
  _click_element(4);
  assert_int_equal(flexi_bd.panel_selected_formid, 4);
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(1));
}

// deselecting an element leaves its GROUP selected, so getting back to the
// group is not a second click
static void test_click_selected_element_falls_back_to_its_group(void **state)
{
  flexi_build("u{1,2,i{3,4}}");
  _click_element(4);
  _click_element(4);
  assert_int_equal(flexi_bd.panel_selected_formid, INVALID_MASKID);
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(1));
}

// ...and one more click on that group then selects the whole mask
static void test_element_then_group_reaches_the_whole_mask(void **state)
{
  flexi_build("u{1,2,i{3,4}}");
  _click_element(4);
  _click_element(4);          // -> its group
  _click_group(FLEXI_GID(1)); // -> the whole mask
  assert_int_equal(flexi_bd.panel_selected_formid, INVALID_MASKID);
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(0));
}

// a group selected only because it holds the selected element is not
// deselected by a click on it: the click selects it in the element's place
static void test_click_group_of_selected_element_selects_the_group(void **state)
{
  flexi_build("u{1,2,i{3,4}}");
  _click_element(4);
  _click_group(FLEXI_GID(1));
  assert_int_equal(flexi_bd.panel_selected_formid, INVALID_MASKID);
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(1));
  // and now that it is selected by itself, a second click deselects it
  _click_group(FLEXI_GID(1));
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(0));
}

static void test_click_other_element_switches_directly(void **state)
{
  flexi_build("u{1,2,i{3,4}}");
  _click_element(4);
  _click_element(1); // a different group's element, in one click
  assert_int_equal(flexi_bd.panel_selected_formid, 1);
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(0));
}

static void test_click_other_group_switches_directly(void **state)
{
  flexi_build("u{1,2,i{3,4},d{5}}");
  _click_group(FLEXI_GID(1));
  _click_group(FLEXI_GID(2));
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(2));
}

// ---------------------------------------------------------------------------
// element chevrons under "auto-expand selected": the click decides what is
// open, not the selection it also makes
// ---------------------------------------------------------------------------

// a chevron click with the option on, applied the way _element_chevron_clicked
// applies it
static dt_masks_chevron_click_t _chevron(const dt_mask_id_t id, const gboolean expanded)
{
  const dt_masks_chevron_click_t c =
    dt_masks_model_element_chevron_click(&flexi_bd, id, expanded, TRUE);
  flexi_bd.masks_last_expanded_elem = c.last_expanded;
  return c;
}

static void test_chevron_expand_collapses_previous(void **state)
{
  flexi_build("u{1,2,3}");
  flexi_bd.masks_last_expanded_elem = INVALID_MASKID;
  assert_int_equal(_chevron(1, TRUE).collapse, INVALID_MASKID);
  assert_int_equal(flexi_bd.masks_last_expanded_elem, 1);
  assert_int_equal(_chevron(2, TRUE).collapse, 1);
  assert_int_equal(flexi_bd.masks_last_expanded_elem, 2);
}

// collapsing the open row must stick, not be re-opened
static void test_chevron_collapse_forgets_the_open_row(void **state)
{
  flexi_build("u{1,2,3}");
  flexi_bd.masks_last_expanded_elem = 2;
  assert_int_equal(_chevron(2, FALSE).collapse, INVALID_MASKID);
  assert_int_equal(flexi_bd.masks_last_expanded_elem, INVALID_MASKID);
}

static void test_chevron_collapse_of_other_row_keeps_the_open_one(void **state)
{
  flexi_build("u{1,2,3}");
  flexi_bd.masks_last_expanded_elem = 2;
  assert_int_equal(_chevron(1, FALSE).collapse, INVALID_MASKID);
  assert_int_equal(flexi_bd.masks_last_expanded_elem, 2);
}

static void test_chevron_reexpand_open_row_collapses_nothing(void **state)
{
  flexi_build("u{1,2,3}");
  flexi_bd.masks_last_expanded_elem = 1;
  assert_int_equal(_chevron(1, TRUE).collapse, INVALID_MASKID);
  assert_int_equal(flexi_bd.masks_last_expanded_elem, 1);
}

static void test_chevron_without_auto_expand_moves_nothing(void **state)
{
  flexi_build("u{1,2,3}");
  flexi_bd.masks_last_expanded_elem = 2;
  const dt_masks_chevron_click_t c =
    dt_masks_model_element_chevron_click(&flexi_bd, 1, TRUE, FALSE);
  assert_int_equal(c.collapse, INVALID_MASKID);
  assert_int_equal(c.last_expanded, 2);
}

// ---------------------------------------------------------------------------
// solo primitive (dt_masks_group_isolate_state)
// ---------------------------------------------------------------------------

static gboolean _hidden(const dt_mask_id_t fid)
{
  return (dt_masks_gui_group_point(flexi_group(), fid)->state & DT_MASKS_STATE_HIDDEN) != 0;
}

static void test_isolate_state_hides_everything_else(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,4}}");
  GList *keep = g_list_prepend(NULL, GINT_TO_POINTER(3));

  dt_masks_group_isolate_state(grp, keep, DT_MASKS_STATE_HIDDEN);
  g_list_free(keep);

  assert_true(_hidden(1));
  assert_true(_hidden(2));
  // the group holding what is kept stays
  assert_false(_hidden(FLEXI_SUB(1)));
  assert_false(_hidden(3));
  assert_true(_hidden(4));
}

// the inversion that matters: a NULL keep-list means "solo off" -- clear the
// bit everywhere -- NOT "keep nothing", which would hide every element
static void test_isolate_state_null_list_clears_everywhere(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3}}");
  const dt_mask_id_t ids[] = { 1, 2, FLEXI_SUB(1), 3 };
  for(int k = 0; k < 4; k++)
    dt_masks_gui_group_point(grp, ids[k])->state |= DT_MASKS_STATE_HIDDEN;

  dt_masks_group_isolate_state(grp, NULL, DT_MASKS_STATE_HIDDEN);

  for(int k = 0; k < 4; k++) assert_false(_hidden(ids[k]));
}

static void test_isolate_state_soloing_a_whole_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,4}}");
  GList *keep = g_list_prepend(NULL, GINT_TO_POINTER(4));
  keep = g_list_prepend(keep, GINT_TO_POINTER(3));

  dt_masks_group_isolate_state(grp, keep, DT_MASKS_STATE_HIDDEN);
  g_list_free(keep);

  assert_true(_hidden(1));
  assert_true(_hidden(2));
  assert_false(_hidden(3));
  assert_false(_hidden(4));
}

// ---------------------------------------------------------------------------
// linking and copying shapes between two modules' masks
// ---------------------------------------------------------------------------

#define OTHER_GROUP_ID 9000

// a second module, with a mask of its own over the fixture's forms
static dt_iop_module_t _other;
static dt_develop_blend_params_t _other_bp;
static dt_masks_form_t *_other_grp;

// a bare form in the fixture's list, freed by flexi_teardown()
static dt_masks_form_t *_new_form(const dt_mask_id_t fid, const dt_masks_type_t type)
{
  dt_masks_form_t *f = calloc(1, sizeof(dt_masks_form_t));
  f->formid = fid;
  f->type = type;
  snprintf(f->name, sizeof(f->name), "form #%d", (int)fid);
  flexi_dev.forms = g_list_append(flexi_dev.forms, f);
  return f;
}

// give the second module a mask: one maximum group, marker OTHER_GROUP_ID + 1,
// holding `fids` bottom-up, each a circle unless it already exists. Both
// modules make up the pipe
static void _other_module(const dt_mask_id_t *fids, const int n)
{
  memset(&_other, 0, sizeof(_other));
  memset(&_other_bp, 0, sizeof(_other_bp));
  _other_grp = _new_form(OTHER_GROUP_ID, DT_MASKS_GROUP);
  dt_masks_point_group_t *marker = calloc(1, sizeof(dt_masks_point_group_t));
  marker->formid = OTHER_GROUP_ID + 1;
  marker->parentid = OTHER_GROUP_ID;
  marker->state = DT_MASKS_STATE_GROUP_MARKER;
  marker->group_opacity = 1.0f;
  _other_grp->points = g_list_append(_other_grp->points, marker);
  for(int k = 0; k < n; k++)
  {
    if(!dt_masks_get_from_id(&flexi_dev, fids[k])) _new_form(fids[k], DT_MASKS_CIRCLE);
    dt_masks_point_group_t *pt = calloc(1, sizeof(dt_masks_point_group_t));
    pt->formid = fids[k];
    pt->parentid = OTHER_GROUP_ID;
    pt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    pt->opacity = 1.0f;
    _other_grp->points = g_list_append(_other_grp->points, pt);
  }
  _other_bp.mask_id = OTHER_GROUP_ID;
  _other.blend_params = &_other_bp;
  _other.dev = &flexi_dev;
  flexi_dev.iop = g_list_append(g_list_append(NULL, &flexi_module), &_other);
}

// what the panel does when a group is selected: new elements land on top of
// the group whose top member is `top`
static void _aim_at(const dt_mask_id_t top)
{
  flexi_bd.insert_active = TRUE;
  flexi_bd.insert_after_fid = top;
}

static int _users(const dt_mask_id_t fid)
{
  GList *users = dt_masks_model_form_users(fid);
  const int n = g_list_length(users);
  g_list_free(users);
  return n;
}

static int _teardown_linking(void **state)
{
  if(_other_grp)
  {
    g_list_free_full(_other_grp->points, free);
    _other_grp->points = NULL;
    _other_grp = NULL;
  }
  g_list_free(flexi_dev.iop);
  flexi_dev.iop = NULL;
  flexi_conf_cleanup();
  flexi_teardown();
  return 0;
}

static void test_link_lands_in_target_group_in_order(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2,i{3}}");
  const dt_mask_id_t other[] = { 11, 12 };
  _other_module(other, 2);
  _aim_at(3);

  GList *fids = dt_masks_model_module_shapes(&_other);
  GList *added = dt_masks_model_import_forms(&flexi_module, &_other, fids, FALSE);
  assert_int_equal(g_list_length(added), 2);
  assert_layout("u{1,2,i{3,11,12}}");
  // one form in two masks: that is the link
  assert_int_equal(_users(11), 2);
  assert_int_equal(_users(12), 2);
  g_list_free(added);
  g_list_free(fids);
}

static void test_link_skips_what_the_mask_already_uses(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2,i{3}}");
  const dt_mask_id_t other[] = { 11 };
  _other_module(other, 1);
  _aim_at(3);

  GList *fids = g_list_prepend(NULL, GINT_TO_POINTER(11));
  g_list_free(dt_masks_model_import_forms(&flexi_module, &_other, fids, FALSE));
  _aim_at(11);
  assert_null(dt_masks_model_import_forms(&flexi_module, &_other, fids, FALSE));
  assert_null(dt_masks_model_import_forms(&flexi_module, &_other, fids, TRUE));
  assert_layout("u{1,2,i{3,11}}");
  g_list_free(fids);
}

// the element looks as it does where it comes from; it is in the target group
static void test_link_keeps_the_source_look(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,i{3}}");
  const dt_mask_id_t other[] = { 11 };
  _other_module(other, 1);
  dt_masks_point_group_t *spt = dt_masks_gui_group_point(_other_grp, 11);
  spt->opacity = 0.4f;
  spt->state |= DT_MASKS_STATE_INVERSE;
  _aim_at(3);

  GList *fids = g_list_prepend(NULL, GINT_TO_POINTER(11));
  g_list_free(dt_masks_model_import_forms(&flexi_module, &_other, fids, FALSE));
  g_list_free(fids);

  const dt_masks_point_group_t *pt = dt_masks_gui_group_point(flexi_group(), 11);
  assert_non_null(pt);
  assert_float_equal(pt->opacity, 0.4f, 1e-6f);
  assert_true(pt->state & DT_MASKS_STATE_INVERSE);
  assert_int_equal(dt_masks_gui_group_cid_of_form(flexi_group(), 11), FLEXI_GID(1));
}

static void test_copy_is_a_new_independent_form(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,i{3}}");
  const dt_mask_id_t other[] = { 11 };
  _other_module(other, 1);
  _aim_at(3);

  GList *fids = g_list_prepend(NULL, GINT_TO_POINTER(11));
  GList *added = dt_masks_model_import_forms(&flexi_module, &_other, fids, TRUE);
  g_list_free(fids);
  assert_int_equal(g_list_length(added), 1);
  const dt_mask_id_t nid = GPOINTER_TO_INT(added->data);
  g_list_free(added);

  assert_int_not_equal(nid, 11);
  assert_null(dt_masks_gui_group_point(flexi_group(), 11));
  assert_int_equal(dt_masks_gui_group_cid_of_form(flexi_group(), nid), FLEXI_GID(1));
  assert_int_equal(_users(11), 1);
  assert_int_equal(_users(nid), 1);
  assert_string_equal(dt_masks_get_from_id(&flexi_dev, nid)->name, "form #11");
}

// an AI object's copy owns copies of its paths: sharing them would leave the
// two objects linked underneath
static void test_copy_of_object_copies_its_paths(void **state)
{
  flexi_conf_init();
  flexi_build("u{1}");
  const dt_mask_id_t other[] = { 11 };
  _other_module(other, 1);
  dt_masks_form_t *obj = dt_masks_get_from_id(&flexi_dev, 11);
  obj->type = DT_MASKS_OBJECT;
  for(dt_mask_id_t m = 21; m <= 22; m++)
  {
    _new_form(m, DT_MASKS_PATH);
    dt_masks_point_group_t *mpt = calloc(1, sizeof(dt_masks_point_group_t));
    mpt->formid = m;
    mpt->parentid = 11;
    obj->points = g_list_append(obj->points, mpt);
  }
  _aim_at(1);

  GList *fids = g_list_prepend(NULL, GINT_TO_POINTER(11));
  GList *added = dt_masks_model_import_forms(&flexi_module, &_other, fids, TRUE);
  g_list_free(fids);
  assert_int_equal(g_list_length(added), 1);
  const dt_masks_form_t *copy = dt_masks_get_from_id(&flexi_dev, GPOINTER_TO_INT(added->data));
  g_list_free(added);

  assert_non_null(copy);
  assert_true(copy->type & DT_MASKS_OBJECT);
  assert_int_equal(g_list_length(copy->points), 2);
  for(const GList *l = copy->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *mpt = l->data;
    assert_true(mpt->formid != 21 && mpt->formid != 22);
    assert_int_equal(mpt->parentid, copy->formid);
    assert_non_null(dt_masks_get_from_id(&flexi_dev, mpt->formid));
  }
  g_list_free_full(obj->points, free);
  obj->points = NULL;
}

static void test_module_shapes_follow_groups_and_skip_other_kinds(void **state)
{
  flexi_conf_init();
  flexi_build("u{1}");
  const dt_mask_id_t other[] = { 11, 12 };
  _other_module(other, 2);
  dt_masks_get_from_id(&flexi_dev, 12)->type = DT_MASKS_PARAMETRIC;
  // 13 is in a group nested in the other mask
  _new_form(13, DT_MASKS_CIRCLE);
  dt_masks_form_t *sub = _new_form(OTHER_GROUP_ID + 5, DT_MASKS_GROUP);
  const dt_mask_id_t pts[] = { OTHER_GROUP_ID + 6, 13 };
  for(int k = 0; k < 2; k++)
  {
    dt_masks_point_group_t *pt = calloc(1, sizeof(dt_masks_point_group_t));
    pt->formid = pts[k];
    pt->parentid = sub->formid;
    pt->state = k == 0 ? DT_MASKS_STATE_GROUP_MARKER : DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    pt->opacity = pt->group_opacity = 1.0f;
    sub->points = g_list_append(sub->points, pt);
  }
  dt_masks_point_group_t *ref = calloc(1, sizeof(dt_masks_point_group_t));
  ref->formid = sub->formid;
  ref->parentid = OTHER_GROUP_ID;
  ref->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
  ref->opacity = ref->group_opacity = 1.0f;
  _other_grp->points = g_list_append(_other_grp->points, ref);

  GList *all = dt_masks_model_module_shapes(&_other);
  assert_int_equal(g_list_length(all), 2);
  assert_int_equal(GPOINTER_TO_INT(all->data), 11);
  assert_int_equal(GPOINTER_TO_INT(all->next->data), 13);
  g_list_free(all);
}

static void test_unlink_gives_this_module_its_own_copy(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,i{3}}");
  const dt_mask_id_t other[] = { 11 };
  _other_module(other, 1);
  _aim_at(3);
  GList *fids = g_list_prepend(NULL, GINT_TO_POINTER(11));
  g_list_free(dt_masks_model_import_forms(&flexi_module, &_other, fids, FALSE));
  g_list_free(fids);
  flexi_bd.panel_selected_formid = 11;

  const dt_mask_id_t nid = dt_masks_model_unlink_form_point(&flexi_module, 11, NULL);
  assert_true(dt_is_valid_maskid(nid));
  assert_int_not_equal(nid, 11);
  assert_null(dt_masks_gui_group_point(flexi_group(), 11));
  assert_int_equal(dt_masks_gui_group_cid_of_form(flexi_group(), nid), FLEXI_GID(1));
  assert_non_null(dt_masks_gui_group_point(_other_grp, 11));
  assert_int_equal(_users(11), 1);
  assert_int_equal(flexi_bd.panel_selected_formid, nid);
}

// a group is known by its marker's id: unlinking its only element leaves the
// group, its number and its selection alone
static void test_unlink_leaves_the_group_alone(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,i{11}}");
  const dt_mask_id_t other[] = { 11 };
  _other_module(other, 1);
  flexi_set_ordinal(FLEXI_GID(1), 2);
  flexi_bd.panel_selected_group_cid = FLEXI_GID(1);

  const dt_mask_id_t nid = dt_masks_model_unlink_form_point(&flexi_module, 11, NULL);
  assert_int_equal(dt_masks_gui_group_cid_of_form(flexi_group(), nid), FLEXI_GID(1));
  assert_int_equal(flexi_get_ordinal(FLEXI_GID(1)), 2);
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(1));
}

// One mask can hold the same shape twice -- a group referencing a shape another
// group of the same mask defines -- and then each reference unlinks on its own:
// the row the user picked gets the copy, every other row keeps the original.
static void test_unlink_one_reference_leaves_the_others_linked(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,i{2}}");
  // a second reference to shape 1, on top of the mask
  dt_masks_point_group_t *second = calloc(1, sizeof(dt_masks_point_group_t));
  second->formid = 1;
  second->parentid = flexi_group()->formid;
  second->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
  second->opacity = 1.0f;
  second->group_opacity = 1.0f;
  flexi_group()->points = g_list_append(flexi_group()->points, second);
  flexi_bd.panel_selected_formid = 1;

  const dt_mask_id_t nid = dt_masks_model_unlink_form_point(&flexi_module, 1, second);
  assert_true(dt_is_valid_maskid(nid));
  assert_int_not_equal(nid, 1);
  // the picked reference now carries the copy, the other still the original
  assert_int_equal(second->formid, nid);
  assert_non_null(dt_masks_gui_group_point(flexi_group(), 1));
  // the selection describes the row that kept the shape, so it stays there
  assert_int_equal(flexi_bd.panel_selected_formid, 1);
}

// unlinking the last reference does carry the panel state over, exactly as
// unlinking a shape only one row shows always has
static void test_unlink_the_last_reference_carries_the_selection(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,i{2}}");
  const dt_mask_id_t other[] = { 1 };
  _other_module(other, 1);
  flexi_bd.panel_selected_formid = 1;

  const dt_mask_id_t nid = dt_masks_model_unlink_form_point(&flexi_module, 1, NULL);
  assert_true(dt_is_valid_maskid(nid));
  assert_null(dt_masks_gui_group_point(flexi_group(), 1));
  assert_int_equal(flexi_bd.panel_selected_formid, nid);
}

// the link indicator: the chain icon and the "unlink" entry show together
static void test_link_shows_only_for_shared_elements_but_raster(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2,3}");
  const dt_mask_id_t other[] = { 1, 3 };
  _other_module(other, 2);
  dt_masks_get_from_id(&flexi_dev, 3)->type = DT_MASKS_RASTER;

  assert_true(dt_masks_model_form_is_linked(dt_masks_get_from_id(&flexi_dev, 1)));
  assert_false(dt_masks_model_form_is_linked(dt_masks_get_from_id(&flexi_dev, 2)));
  // a raster element reads another module's mask; nothing of it is editable
  assert_false(dt_masks_model_form_is_linked(dt_masks_get_from_id(&flexi_dev, 3)));
  assert_false(dt_masks_model_form_is_linked(NULL));
}

// channels are only ever copied, so even one another module holds never
// shows the link nor offers "unlink"
static void test_parametric_channel_never_shows_the_link(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2}");
  const dt_mask_id_t other[] = { 2 };
  _other_module(other, 1);
  dt_masks_get_from_id(&flexi_dev, 2)->type = DT_MASKS_PARAMETRIC;
  assert_false(dt_masks_model_form_is_linked(dt_masks_get_from_id(&flexi_dev, 2)));
}

// another module linking or unlinking an element of this mask leaves the mask
// untouched, but its chain icon changes: the list must not skip the rebuild
static void test_list_signature_follows_links_made_elsewhere(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2}");
  const dt_mask_id_t other[] = { 11 };
  _other_module(other, 1);
  const dt_hash_t alone = dt_masks_gui_list_signature(&flexi_module);
  assert_true(alone == dt_masks_gui_list_signature(&flexi_module));

  dt_masks_point_group_t *pt = calloc(1, sizeof(dt_masks_point_group_t));
  pt->formid = 2;
  pt->parentid = OTHER_GROUP_ID;
  pt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
  pt->opacity = 1.0f;
  _other_grp->points = g_list_append(_other_grp->points, pt);
  assert_true(dt_masks_gui_list_signature(&flexi_module) != alone);

  _other_grp->points = g_list_remove(_other_grp->points, pt);
  free(pt);
  assert_true(dt_masks_gui_list_signature(&flexi_module) == alone);
}

static void test_list_signature_ignores_links_among_others(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2}");
  const dt_mask_id_t other[] = { 11 };
  _other_module(other, 1);
  const dt_hash_t before = dt_masks_gui_list_signature(&flexi_module);

  _new_form(12, DT_MASKS_CIRCLE);
  dt_masks_point_group_t *pt = calloc(1, sizeof(dt_masks_point_group_t));
  pt->formid = 12;
  pt->parentid = OTHER_GROUP_ID;
  _other_grp->points = g_list_append(_other_grp->points, pt);
  assert_true(dt_masks_gui_list_signature(&flexi_module) == before);
}

// a duplicated instance shares its shapes with the original but gets its own
// channels, looking as they did there
static void test_duplicate_shares_shapes_and_copies_channels(void **state)
{
  flexi_conf_init();
  flexi_build("u{1}");
  const dt_mask_id_t other[] = { 11, 12 };
  _other_module(other, 2);
  dt_masks_get_from_id(&flexi_dev, 12)->type = DT_MASKS_PARAMETRIC;
  dt_masks_point_group_t *spt = g_list_nth_data(_other_grp->points, 2);
  spt->opacity = 0.3f;
  spt->state = DT_MASKS_STATE_INVERSE | DT_MASKS_STATE_USE;

  dt_masks_form_t *dup = _new_form(8000, DT_MASKS_GROUP);
  dt_masks_group_add_members_of(dup, _other_grp);
  // the group's marker comes along, then the shape and the channel
  assert_int_equal(g_list_length(dup->points), 3);
  assert_true(dt_masks_point_is_marker(dup->points->data));
  const dt_masks_point_group_t *shape = dup->points->next->data;
  const dt_masks_point_group_t *channel = dup->points->next->next->data;
  assert_int_equal(shape->formid, 11);
  assert_int_not_equal(channel->formid, 12);
  assert_true(dt_masks_get_from_id(&flexi_dev, channel->formid)->type & DT_MASKS_PARAMETRIC);
  assert_float_equal(channel->opacity, 0.3f, 1e-6f);
  assert_int_equal(channel->state, spt->state);
  // the original keeps its own channel
  assert_int_equal(_users(12), 1);
  g_list_free_full(dup->points, free);
  dup->points = NULL;
}

// ---------------------------------------------------------------------------
// AI objects: stepping into one on the canvas, and what a delete takes away
// ---------------------------------------------------------------------------

static dt_masks_form_gui_t _gui;

static void _with_gui(const dt_mask_id_t entered)
{
  memset(&_gui, 0, sizeof(_gui));
  _gui.entered_object = entered;
  flexi_dev.form_gui = &_gui;
}

// make `oid` an AI object, its marker first as object.c makes one, and add
// `n` paths numbered from `first`
static dt_masks_form_t *_make_object(const dt_mask_id_t oid,
                                     const dt_mask_id_t first,
                                     const int n)
{
  dt_masks_form_t *obj = dt_masks_get_from_id(&flexi_dev, oid);
  if(!obj) obj = _new_form(oid, DT_MASKS_OBJECT);
  obj->type = DT_MASKS_OBJECT;
  if(!obj->points) obj->points = g_list_append(NULL, dt_masks_marker_new(flexi_dev.forms, obj, 0));
  for(int k = 0; k < n; k++)
  {
    _new_form(first + k, DT_MASKS_PATH);
    dt_masks_point_group_t *mpt = calloc(1, sizeof(dt_masks_point_group_t));
    mpt->formid = first + k;
    mpt->parentid = oid;
    mpt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    mpt->opacity = 1.0f;
    obj->points = g_list_append(obj->points, mpt);
  }
  return obj;
}

static int _teardown_objects(void **state)
{
  for(GList *l = flexi_dev.forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = l->data;
    if(!(f->type & DT_MASKS_OBJECT)) continue;
    g_list_free_full(f->points, free);
    f->points = NULL;
  }
  flexi_dev.form_gui = NULL;
  return _teardown_linking(state);
}

static void test_double_click_on_object_steps_in(void **state)
{
  flexi_conf_init();
  flexi_build("u{1}");
  _with_gui(INVALID_MASKID);
  assert_true(dt_masks_gui_step_object(NULL, &_gui, 5, TRUE, TRUE));
  assert_int_equal(_gui.entered_object, 5);
}

// inside, a double-click is an ordinary click on one of the paths
static void test_double_click_inside_object_is_ordinary(void **state)
{
  flexi_conf_init();
  flexi_build("u{1}");
  _with_gui(5);
  assert_false(dt_masks_gui_step_object(NULL, &_gui, 5, TRUE, TRUE));
  assert_int_equal(_gui.entered_object, 5);
}

static void test_click_inside_object_stays_in(void **state)
{
  flexi_conf_init();
  flexi_build("u{1}");
  _with_gui(5);
  assert_false(dt_masks_gui_step_object(NULL, &_gui, 5, TRUE, FALSE));
  assert_int_equal(_gui.entered_object, 5);
}

// the click that steps out still does what it would have done
static void test_click_outside_object_steps_out(void **state)
{
  flexi_conf_init();
  flexi_build("u{1}");
  _with_gui(5);
  assert_false(dt_masks_gui_step_object(NULL, &_gui, INVALID_MASKID, TRUE, FALSE));
  assert_int_equal(_gui.entered_object, INVALID_MASKID);
  _with_gui(5);
  assert_false(dt_masks_gui_step_object(NULL, &_gui, 6, TRUE, FALSE));
  assert_int_equal(_gui.entered_object, INVALID_MASKID);
}

static void test_double_click_on_other_object_moves_into_it(void **state)
{
  flexi_conf_init();
  flexi_build("u{1}");
  _with_gui(5);
  assert_true(dt_masks_gui_step_object(NULL, &_gui, 6, TRUE, TRUE));
  assert_int_equal(_gui.entered_object, 6);
}

// a right-click inside removes a path: it must neither step out nor in
static void test_secondary_button_never_steps(void **state)
{
  flexi_conf_init();
  flexi_build("u{1}");
  _with_gui(5);
  assert_false(dt_masks_gui_step_object(NULL, &_gui, INVALID_MASKID, FALSE, FALSE));
  assert_int_equal(_gui.entered_object, 5);
  _with_gui(INVALID_MASKID);
  assert_false(dt_masks_gui_step_object(NULL, &_gui, 6, FALSE, TRUE));
  assert_int_equal(_gui.entered_object, INVALID_MASKID);
}

static void test_double_click_on_plain_shape_does_nothing(void **state)
{
  flexi_conf_init();
  flexi_build("u{1}");
  _with_gui(INVALID_MASKID);
  assert_false(dt_masks_gui_step_object(NULL, &_gui, INVALID_MASKID, TRUE, TRUE));
  assert_int_equal(_gui.entered_object, INVALID_MASKID);
}

// hover, drag, scroll and rotate move an object's paths together, except
// inside the object the user stepped into
// "clean up unused shapes": an object's paths are only reached through the
// object, so they are used whenever the object is
static void test_cleanup_keeps_the_paths_of_a_used_object(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,5}");
  _make_object(5, 21, 2);

  dt_develop_blend_params_t bp = { 0 };
  bp.mask_id = grp->formid;
  dt_dev_history_item_t hist = { 0 };
  hist.blend_params = &bp;
  hist.forms = g_list_copy(flexi_dev.forms);
  GList *history = g_list_append(NULL, &hist);

  dt_masks_cleanup_unused_from_list(history);

  assert_non_null(dt_masks_get_from_id_ext(hist.forms, 5));
  assert_non_null(dt_masks_get_from_id_ext(hist.forms, 21));
  assert_non_null(dt_masks_get_from_id_ext(hist.forms, 22));

  g_list_free(history);
  g_list_free(hist.forms);
  // removed forms are only moved there; the fixture owns them
  g_list_free(flexi_dev.allforms);
  flexi_dev.allforms = NULL;
}

// an object none of whose paths exists is dropped from the group and the
// forms, and the rest of its group stays
static void test_empty_object_is_removed(void **state)
{
  flexi_build("u{1,u{5,6}}");
  dt_masks_form_t *obj = _make_object(5, 21, 0);

  assert_int_equal(dt_masks_prune_empty_objects(&flexi_dev.forms), 1);
  assert_layout("u{1,u{6}}");
  assert_null(dt_masks_get_from_id(&flexi_dev, 5));

  // back to the fixture, which frees it
  g_list_free(flexi_dev.allforms);
  flexi_dev.allforms = NULL;
  flexi_dev.forms = g_list_append(flexi_dev.forms, obj);
}

static void test_object_with_a_path_is_kept(void **state)
{
  flexi_build("u{1,u{5,6}}");
  _make_object(5, 21, 1);

  assert_int_equal(dt_masks_prune_empty_objects(&flexi_dev.forms), 0);
  assert_layout("u{1,u{5,6}}");
  assert_non_null(dt_masks_get_from_id(&flexi_dev, 5));
}

static void test_object_paths_act_as_one_until_stepped_in(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,5}");
  dt_masks_form_t *obj = _make_object(5, 21, 2);
  const dt_masks_point_group_t path = { .formid = 21, .parentid = 5 };
  const dt_masks_point_group_t plain = { .formid = 1, .parentid = flexi_bp.mask_id };

  _with_gui(INVALID_MASKID);
  assert_ptr_equal(dt_masks_bundle_of(&path), obj);
  assert_null(dt_masks_bundle_of(&plain));
  _gui.entered_object = 5;
  assert_null(dt_masks_bundle_of(&path));
  _gui.entered_object = 6;
  assert_ptr_equal(dt_masks_bundle_of(&path), obj);
}

static dt_masks_remove_target_t _removes(const dt_mask_id_t fid,
                                         const dt_mask_id_t parent,
                                         const gboolean whole,
                                         dt_mask_id_t *taken,
                                         dt_mask_id_t *from)
{
  dt_masks_form_t *form = dt_masks_get_from_id(&flexi_dev, fid);
  dt_mask_id_t parentid = parent;
  const dt_masks_remove_target_t t =
    dt_masks_remove_shape_target(&flexi_module, &form, &parentid, whole);
  *taken = form->formid;
  *from = parentid;
  return t;
}

// an object is one element: a right-click on any of its paths removes it
// from this module, the way the panel's delete does
static void test_right_click_on_object_path_removes_the_object(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,5}");
  _make_object(5, 21, 2);
  _with_gui(INVALID_MASKID);
  dt_mask_id_t taken, from;
  assert_int_equal(_removes(22, 5, TRUE, &taken, &from), DT_MASKS_REMOVE_ELEMENT);
  assert_int_equal(taken, 5);
  assert_int_equal(from, flexi_bp.mask_id);
}

static void test_right_click_inside_object_removes_one_path(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,5}");
  _make_object(5, 21, 2);
  _with_gui(5);
  dt_mask_id_t taken, from;
  assert_int_equal(_removes(22, 5, TRUE, &taken, &from), DT_MASKS_REMOVE_PATH);
  assert_int_equal(taken, 22);
  assert_int_equal(from, 5);
}

// deleting nodes down to a path's minimum removes it gradually, stepped in or not
static void test_node_deletion_removes_one_path(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,5}");
  _make_object(5, 21, 2);
  _with_gui(INVALID_MASKID);
  dt_mask_id_t taken, from;
  assert_int_equal(_removes(21, 5, FALSE, &taken, &from), DT_MASKS_REMOVE_PATH);
  assert_int_equal(taken, 21);
  assert_int_equal(from, 5);
}

// emptying the object would take it from every module that links it: its
// last path takes the object, from this module only
static void test_last_path_removes_the_object(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,5}");
  _make_object(5, 21, 1);
  _with_gui(5);
  dt_mask_id_t taken, from;
  assert_int_equal(_removes(21, 5, TRUE, &taken, &from), DT_MASKS_REMOVE_ELEMENT);
  assert_int_equal(taken, 5);
  assert_int_equal(_removes(21, 5, FALSE, &taken, &from), DT_MASKS_REMOVE_ELEMENT);
  assert_int_equal(taken, 5);
  assert_int_equal(from, flexi_bp.mask_id);
}

// no canvas edit session: a gesture on the object still takes it whole
static void test_object_removal_without_canvas_session(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,5}");
  _make_object(5, 21, 2);
  dt_mask_id_t taken, from;
  assert_int_equal(_removes(21, 5, TRUE, &taken, &from), DT_MASKS_REMOVE_ELEMENT);
  assert_int_equal(taken, 5);
}

// canvas and panel delete the same way: this module's instance only
static void test_canvas_delete_of_shape_is_the_panel_delete(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2}");
  const dt_mask_id_t other[] = { 2 };
  _other_module(other, 1);
  _with_gui(INVALID_MASKID);
  dt_mask_id_t taken, from;
  assert_int_equal(_removes(2, flexi_bp.mask_id, TRUE, &taken, &from),
                   DT_MASKS_REMOVE_ELEMENT);
  assert_int_equal(taken, 2);
  // outside this module's flexi mask a shape just leaves its parent
  assert_int_equal(_removes(2, OTHER_GROUP_ID, TRUE, &taken, &from),
                   DT_MASKS_REMOVE_FROM_PARENT);
  assert_int_equal(from, OTHER_GROUP_ID);
}

// the panel has a row for the object, none for its paths
static void test_canvas_path_selects_its_object_row(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,5}");
  _make_object(5, 21, 2);
  assert_int_equal(dt_masks_model_panel_formid_for(&flexi_module, 22), 5);
  assert_int_equal(dt_masks_model_panel_formid_for(&flexi_module, 5), 5);
  assert_int_equal(dt_masks_model_panel_formid_for(&flexi_module, 1), 1);
  assert_int_equal(dt_masks_model_panel_formid_for(&flexi_module, 77), 77);
  assert_int_equal(dt_masks_model_panel_formid_for(&flexi_module, INVALID_MASKID), INVALID_MASKID);
}

// stepped into, the object shows as its group and its paths as its rows
static void test_canvas_path_of_entered_object_selects_its_own_row(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,5}");
  _make_object(5, 21, 2);
  _with_gui(5);
  assert_int_equal(dt_masks_model_panel_formid_for(&flexi_module, 22), 22);
  assert_int_equal(dt_masks_model_panel_formid_for(&flexi_module, 5), 5);
  _with_gui(6);
  assert_int_equal(dt_masks_model_panel_formid_for(&flexi_module, 22), 5);
}

// those rows act on their points through the same lookup every row uses
static void test_points_of_entered_object_are_found(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,5}");
  _make_object(5, 21, 2);
  _with_gui(INVALID_MASKID);
  assert_null(dt_masks_gui_group_point(grp, 22));
  _with_gui(5);
  const dt_masks_point_group_t *pt = dt_masks_gui_group_point(grp, 22);
  assert_non_null(pt);
  assert_int_equal(pt->formid, 22);
  // an object the mask does not hold stays out of reach
  _with_gui(6);
  assert_null(dt_masks_gui_group_point(grp, 22));
}

// stepping in or out turns the object's row into its group and back, so the
// panel must not skip that rebuild as unchanged
static void test_stepping_in_moves_the_panel_signature(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,5}");
  _make_object(5, 21, 2);
  _with_gui(INVALID_MASKID);
  const dt_hash_t outside = dt_masks_gui_list_signature(&flexi_module);
  _with_gui(5);
  assert_true(dt_masks_gui_list_signature(&flexi_module) != outside);
  _with_gui(INVALID_MASKID);
  assert_true(dt_masks_gui_list_signature(&flexi_module) == outside);
}

// the properties subpanel follows the selection: a shape's geometry and its
// opacity
static void test_props_panel_shows_a_shapes_geometry(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,5}");
  _make_object(5, 21, 2);
  _with_gui(INVALID_MASKID);
  flexi_bd.panel_selected_formid = 1;
  dt_masks_props_target_t t = dt_masks_model_props_panel_target(&flexi_bd);
  assert_int_equal(t.id, 1);
  assert_false(t.is_group);
  assert_true(t.shape);
  assert_true(t.opacity);
  assert_false(t.boost);
  // an AI object is edited as one shape
  flexi_bd.panel_selected_formid = 5;
  t = dt_masks_model_props_panel_target(&flexi_bd);
  assert_int_equal(t.id, 5);
  assert_true(t.shape);
}

// stepped into, the object is a group, and its paths are the shapes: only its
// opacity is left to show
static void test_props_panel_entered_object_has_only_opacity(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,5}");
  _make_object(5, 21, 2);
  _with_gui(5);
  flexi_bd.panel_selected_formid = 5;
  const dt_masks_props_target_t t = dt_masks_model_props_panel_target(&flexi_bd);
  assert_int_equal(t.id, 5);
  assert_false(t.shape);
  assert_true(t.opacity);
  flexi_bd.panel_selected_formid = 22;
  assert_true(dt_masks_model_props_panel_target(&flexi_bd).shape);
}

// a raster mask has only its opacity, which is what it shows there
static void test_props_panel_shows_a_raster_masks_opacity(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2}");
  dt_masks_get_from_id(&flexi_dev, 2)->type = DT_MASKS_RASTER;
  flexi_bd.panel_selected_formid = 2;
  const dt_masks_props_target_t t = dt_masks_model_props_panel_target(&flexi_bd);
  assert_int_equal(t.id, 2);
  assert_false(t.shape);
  assert_true(t.opacity);
}

// the first channel of the scene-referred colorspace with a boost factor, or
// without one when `boost` is FALSE
static int _channel_with_boost(const gboolean boost)
{
  const dt_iop_gui_blendif_channel_t *ch =
    dt_develop_blendif_channels_for_csp(DEVELOP_BLEND_CS_RGB_SCENE);
  for(int k = 0; ch[k].label; k++)
    if(ch[k].boost_factor_enabled == boost) return k;
  return -1;
}

// a parametric channel's boost factor goes there with its opacity, but only
// for a channel that has one
static void test_props_panel_parametric_shows_its_boost_factor(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2}");
  dt_masks_form_t *f = dt_masks_get_from_id(&flexi_dev, 2);
  f->type = DT_MASKS_PARAMETRIC;
  dt_masks_point_parametric_t p = { 0 };
  p.colorspace = DEVELOP_BLEND_CS_RGB_SCENE;
  // checked as a signed index: p.channel is unsigned, where the helper's -1
  // for "none" would become a huge index instead
  const int with_boost = _channel_with_boost(TRUE);
  assert_true(with_boost >= 0);
  p.channel = with_boost;
  f->points = g_list_append(NULL, &p);
  flexi_bd.panel_selected_formid = 2;

  dt_masks_props_target_t t = dt_masks_model_props_panel_target(&flexi_bd);
  assert_int_equal(t.id, 2);
  assert_true(t.boost);
  assert_true(t.opacity);
  assert_false(t.shape);

  // no boost factor: only the opacity slider is left to show
  const int without_boost = _channel_with_boost(FALSE);
  assert_true(without_boost >= 0);
  p.channel = without_boost;
  t = dt_masks_model_props_panel_target(&flexi_bd);
  assert_int_equal(t.id, 2);
  assert_false(t.boost);
  assert_true(t.opacity);

  g_list_free(f->points);
  f->points = NULL;
}

// a group selection alone shows the group's opacity; an element selected in
// it wins, as for the refinements
static void test_props_panel_shows_a_groups_opacity(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,5}");
  const dt_masks_point_group_t *head = flexi_group()->points->data;
  assert_true(dt_masks_point_is_marker(head));
  flexi_bd.panel_selected_formid = INVALID_MASKID;
  flexi_bd.panel_selected_group_cid = head->formid;
  const dt_masks_props_target_t t = dt_masks_model_props_panel_target(&flexi_bd);
  assert_int_equal(t.id, head->formid);
  assert_true(t.is_group);
  assert_true(t.opacity);
  assert_false(t.shape);
  flexi_bd.panel_selected_formid = 1;
  assert_false(dt_masks_model_props_panel_target(&flexi_bd).is_group);
  assert_int_equal(dt_masks_model_props_panel_target(&flexi_bd).id, 1);
}

// nothing selected: nothing to show
static void test_props_panel_empty_without_a_selection(void **state)
{
  flexi_conf_init();
  flexi_build("u{1}");
  flexi_bd.panel_selected_formid = INVALID_MASKID;
  flexi_bd.panel_selected_group_cid = INVALID_MASKID;
  assert_int_equal(dt_masks_model_props_panel_target(&flexi_bd).id, INVALID_MASKID);
}

// solo edit inside the object keeps the object: narrowing to the pressed path
// rebuilt the canvas under the press and crashed its release
static void test_soloedit_inside_entered_object_isolates_the_object(void **state)
{
  flexi_conf_init();
  dt_conf_set_bool("plugins/darkroom/masks/solo_edit_mode", TRUE);
  flexi_build("u{1,5}");
  _make_object(5, 21, 2);
  flexi_bd.panel_selected_formid = 22;
  _with_gui(5);
  assert_int_equal(dt_masks_model_soloedit_target(&flexi_bd), 5);
  // outside it, a shape isolates itself as always
  flexi_bd.panel_selected_formid = 1;
  assert_int_equal(dt_masks_model_soloedit_target(&flexi_bd), 1);
  dt_conf_set_bool("plugins/darkroom/masks/solo_edit_mode", FALSE);
}

// a group selected on its own isolates its members. The mask's own group holds
// every shape, so selecting it leaves the whole mask on the canvas
static void test_soloedit_isolates_a_selected_group(void **state)
{
  flexi_conf_init();
  dt_conf_set_bool("plugins/darkroom/masks/solo_edit_mode", TRUE);
  flexi_build("u{1,2,i{3}}");
  flexi_bd.panel_selected_formid = INVALID_MASKID;
  flexi_bd.panel_selected_group_cid = FLEXI_GID(1);
  assert_int_equal(dt_masks_model_soloedit_target(&flexi_bd), FLEXI_GID(1));
  GList *members = dt_masks_model_group_members(flexi_group(), FLEXI_GID(1));
  assert_int_equal(g_list_length(members), 1);
  assert_int_equal(GPOINTER_TO_INT(members->data), 3);
  g_list_free(members);
  // an element of it selected isolates that element instead
  flexi_bd.panel_selected_formid = 3;
  assert_int_equal(dt_masks_model_soloedit_target(&flexi_bd), 3);
  dt_conf_set_bool("plugins/darkroom/masks/solo_edit_mode", FALSE);
}

static void test_soloedit_off_for_the_mask_group(void **state)
{
  flexi_conf_init();
  dt_conf_set_bool("plugins/darkroom/masks/solo_edit_mode", TRUE);
  flexi_build("u{1,2}");
  flexi_bd.panel_selected_formid = INVALID_MASKID;
  flexi_bd.panel_selected_group_cid = FLEXI_GID(0);
  assert_int_equal(dt_masks_model_soloedit_target(&flexi_bd), INVALID_MASKID);
  dt_conf_set_bool("plugins/darkroom/masks/solo_edit_mode", FALSE);
}

// its marker is no path: inside, the last path still takes the object away
static void test_marker_is_not_counted_as_a_path(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,5}");
  _make_object(5, 21, 1);
  _with_gui(5);
  dt_mask_id_t taken, from;
  assert_int_equal(_removes(21, 5, FALSE, &taken, &from), DT_MASKS_REMOVE_ELEMENT);
  assert_int_equal(taken, 5);
  _make_object(5, 22, 1);
  assert_int_equal(_removes(21, 5, FALSE, &taken, &from), DT_MASKS_REMOVE_PATH);
}

// ---------------------------------------------------------------------------
// the refinement panel's target when its element disappears
// ---------------------------------------------------------------------------

static void _scope(const int kind, const dt_mask_id_t fid)
{
  flexi_bd.masks_refine_scope_kind = kind;
  flexi_bd.masks_refine_scope_formid = fid;
}

// an element removed by a route that does not reselect (canvas, an AI
// object's last path, undo) must not stay the refinement target
static void test_refine_scope_of_removed_element_falls_back_to_its_group(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2,i{3}}");
  _scope(REFINE_SCOPE_ELEMENT, 9);
  flexi_bd.panel_selected_formid = 9;
  flexi_bd.panel_selected_group_cid = FLEXI_GID(1);

  assert_true(dt_masks_model_refine_scope_prune(&flexi_module));
  assert_int_equal(flexi_bd.panel_selected_formid, INVALID_MASKID);
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(1));
  dt_masks_model_refine_scope_from_selection(&flexi_module);
  assert_int_equal(flexi_bd.masks_refine_scope_kind, REFINE_SCOPE_GROUP);
  assert_int_equal(flexi_bd.masks_refine_scope_formid, FLEXI_GID(1));
}

static void test_refine_scope_with_nothing_left_is_the_whole_mask(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2}");
  _scope(REFINE_SCOPE_GROUP, 9);
  flexi_bd.panel_selected_group_cid = 9;

  assert_true(dt_masks_model_refine_scope_prune(&flexi_module));
  assert_int_equal(flexi_bd.panel_selected_group_cid, INVALID_MASKID);
  dt_masks_model_refine_scope_from_selection(&flexi_module);
  assert_int_equal(flexi_bd.masks_refine_scope_kind, REFINE_SCOPE_GLOBAL);
  assert_int_equal(flexi_bd.masks_refine_scope_formid, INVALID_MASKID);
}

// the mask's own group refines the whole mask, the module-wide refinement,
// not its marker's own group refinement
static void test_refine_scope_of_the_mask_group_is_the_whole_mask(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2}");
  flexi_bd.panel_selected_formid = INVALID_MASKID;
  flexi_bd.panel_selected_group_cid = FLEXI_GID(0);
  dt_masks_model_refine_scope_from_selection(&flexi_module);
  assert_int_equal(flexi_bd.masks_refine_scope_kind, REFINE_SCOPE_GLOBAL);
  assert_int_equal(flexi_bd.masks_refine_scope_formid, INVALID_MASKID);
}

static void test_refine_scope_of_present_element_is_kept(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2}");
  _scope(REFINE_SCOPE_ELEMENT, 2);
  flexi_bd.panel_selected_formid = 2;
  flexi_bd.panel_selected_group_cid = FLEXI_GID(0);

  assert_false(dt_masks_model_refine_scope_prune(&flexi_module));
  assert_int_equal(flexi_bd.panel_selected_formid, 2);
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(0));
  _scope(REFINE_SCOPE_GLOBAL, INVALID_MASKID);
  assert_false(dt_masks_model_refine_scope_prune(&flexi_module));
}

static void test_refine_scope_follows_the_selection(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,2,i{}}");
  flexi_bd.panel_selected_formid = 2;
  flexi_bd.panel_selected_group_cid = FLEXI_GID(0);
  dt_masks_model_refine_scope_from_selection(&flexi_module);
  assert_int_equal(flexi_bd.masks_refine_scope_kind, REFINE_SCOPE_ELEMENT);
  assert_int_equal(flexi_bd.masks_refine_scope_formid, 2);

  // an empty group is a group: its refinement is its marker's
  flexi_bd.panel_selected_formid = INVALID_MASKID;
  flexi_bd.panel_selected_group_cid = FLEXI_GID(1);
  dt_masks_model_refine_scope_from_selection(&flexi_module);
  assert_int_equal(flexi_bd.masks_refine_scope_kind, REFINE_SCOPE_GROUP);
  assert_int_equal(flexi_bd.masks_refine_scope_formid, FLEXI_GID(1));
}

// ---------------------------------------------------------------------------
// raster elements: named after their source until given a name of their own
// ---------------------------------------------------------------------------

static dt_iop_module_so_t _this_so, _other_so;
static const char *_exposure_name(void) { return "exposure"; }
static const char *_this_name(void) { return "contrast"; }

// the other module is an exposure instance, and form `fid` of this mask a
// raster element reading its mask, named by its type alone as when added
static dt_masks_form_t *_raster_of_other(const dt_mask_id_t fid)
{
  const dt_mask_id_t other[] = { 11 };
  _other_module(other, 1);
  g_strlcpy(_other_so.op, "exposure", sizeof(_other_so.op));
  _other.so = &_other_so;
  _other.name = _exposure_name;
  g_strlcpy(_this_so.op, "bilat", sizeof(_this_so.op));
  flexi_module.so = &_this_so;
  flexi_module.name = _this_name;

  dt_masks_form_t *f = dt_masks_get_from_id(&flexi_dev, fid);
  f->type = DT_MASKS_RASTER;
  dt_masks_point_raster_t *p = calloc(1, sizeof(dt_masks_point_raster_t));
  g_strlcpy(p->source, "exposure", sizeof(p->source));
  f->points = g_list_append(NULL, p);
  g_strlcpy(f->name, _("raster mask"), sizeof(f->name));
  return f;
}

static void _rename_other(const char *name)
{
  g_strlcpy(_other.multi_name, name, sizeof(_other.multi_name));
  _other.multi_name_hand_edited = TRUE;
}

static void _assert_shows(const dt_masks_form_t *f, const char *expect)
{
  gchar *shown = dt_masks_gui_form_display_name(f);
  assert_string_equal(shown, expect);
  g_free(shown);
}

static int _teardown_raster(void **state)
{
  for(GList *l = flexi_dev.forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = l->data;
    if(!(f->type & DT_MASKS_RASTER)) continue;
    g_list_free_full(f->points, free);
    f->points = NULL;
  }
  return _teardown_linking(state);
}

static void test_raster_element_follows_its_source_name(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,7}");
  const dt_masks_form_t *f = _raster_of_other(7);
  gchar *before = dt_history_item_get_name(&_other);
  _assert_shows(f, before);

  _rename_other("sky");
  gchar *after = dt_history_item_get_name(&_other);
  assert_string_not_equal(after, before);
  _assert_shows(f, after);
  g_free(before);
  g_free(after);
}

static void test_renamed_raster_element_keeps_its_name(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,7}");
  dt_masks_form_t *f = _raster_of_other(7);
  assert_true(dt_masks_model_rename_form(f, "mine"));
  _assert_shows(f, "mine");
  _rename_other("sky");
  _assert_shows(f, "mine");
}

// emptying the name hands it back to the source; other kinds need a name
static void test_emptied_raster_name_follows_the_source_again(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,7}");
  dt_masks_form_t *f = _raster_of_other(7);
  assert_true(dt_masks_model_rename_form(f, "mine"));
  assert_true(dt_masks_model_rename_form(f, ""));
  gchar *label = dt_history_item_get_name(&_other);
  _assert_shows(f, label);
  g_free(label);

  dt_masks_form_t *circle = dt_masks_get_from_id(&flexi_dev, 1);
  gchar *was = g_strdup(circle->name);
  assert_false(dt_masks_model_rename_form(circle, ""));
  assert_string_equal(circle->name, was);
  g_free(was);
}

static void test_raster_source_is_found_by_operation_and_instance(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,7}");
  dt_masks_form_t *f = _raster_of_other(7);
  assert_ptr_equal(dt_masks_raster_source(f), &_other);
  assert_null(dt_masks_raster_source(dt_masks_get_from_id(&flexi_dev, 1)));
  assert_null(dt_masks_raster_source(NULL));

  // a source that is gone leaves the row its operation to show
  ((dt_masks_point_raster_t *)f->points->data)->instance = 3;
  assert_null(dt_masks_raster_source(f));
  _assert_shows(f, "exposure");
}

// ---------------------------------------------------------------------------
// add target: the group the add buttons put what they add in
// ---------------------------------------------------------------------------

static void _no_selection(void)
{
  flexi_bd.panel_selected_group_cid = INVALID_MASKID;
}

// with no group selected, the whole mask
static void test_add_target_is_the_mask_group(void **state)
{
  flexi_build("u{1,i{2}}");
  _no_selection();
  assert_int_equal(dt_masks_gui_resolve_add_target(&flexi_module), FLEXI_GID(0));
}

static void test_add_target_ignores_a_stale_selection(void **state)
{
  flexi_build("u{1,2}");
  flexi_bd.panel_selected_group_cid = 9;
  assert_int_equal(dt_masks_gui_resolve_add_target(&flexi_module), FLEXI_GID(0));
}

// the selected group, nested or not, empty or not
static void test_add_target_is_the_selected_group(void **state)
{
  flexi_build("u{1,i{2},d{}}");
  flexi_bd.panel_selected_group_cid = FLEXI_GID(1);
  assert_int_equal(dt_masks_gui_resolve_add_target(&flexi_module), FLEXI_GID(1));
  flexi_bd.panel_selected_group_cid = FLEXI_GID(2);
  assert_int_equal(dt_masks_gui_resolve_add_target(&flexi_module), FLEXI_GID(2));
}

// a mask with no group form yet has no id for its group: its first element
// creates it
static void test_add_target_of_a_mask_with_no_group_form(void **state)
{
  flexi_build("u{1}");
  _no_selection();
  flexi_bp.mask_id = INVALID_MASKID;
  assert_int_equal(dt_masks_gui_resolve_add_target(&flexi_module), INVALID_MASKID);
}

static void test_list_signature_follows_source_rename(void **state)
{
  flexi_conf_init();
  flexi_build("u{1,7}");
  dt_masks_form_t *f = _raster_of_other(7);
  const dt_hash_t before = dt_masks_gui_list_signature(&flexi_module);
  _rename_other("sky");
  assert_true(dt_masks_gui_list_signature(&flexi_module) != before);

  assert_true(dt_masks_model_rename_form(f, "mine"));
  const dt_hash_t named = dt_masks_gui_list_signature(&flexi_module);
  _rename_other("trees");
  assert_true(dt_masks_gui_list_signature(&flexi_module) == named);
}

// "clean up unused shapes" records the ids it reaches in a table with one slot
// per form. A marker's id names no form, so storing it took a slot a member
// needed, and the members it crowded out were deleted as unused
static void test_cleanup_keeps_every_member_of_a_marked_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{u{1},u{2}}");

  dt_develop_blend_params_t bp = { 0 };
  bp.mask_id = grp->formid;
  dt_dev_history_item_t hist = { 0 };
  hist.blend_params = &bp;
  hist.forms = g_list_copy(flexi_dev.forms);
  GList *history = g_list_append(NULL, &hist);

  dt_masks_cleanup_unused_from_list(history);

  assert_non_null(dt_masks_get_from_id_ext(hist.forms, 1));
  assert_non_null(dt_masks_get_from_id_ext(hist.forms, 2));
  assert_non_null(dt_masks_get_from_id_ext(hist.forms, grp->formid));

  g_list_free(history);
  g_list_free(hist.forms);
  g_list_free(flexi_dev.allforms);
  flexi_dev.allforms = NULL;
}

// an earlier item can name a mask this snapshot no longer holds. Storing that
// id took a slot of the table too, crowding out a member of the live group
static void test_cleanup_keeps_every_member_past_a_stale_mask_id(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");

  dt_develop_blend_params_t stale_bp = { 0 };
  stale_bp.mask_id = 9999;
  dt_dev_history_item_t stale = { 0 };
  stale.blend_params = &stale_bp;

  dt_develop_blend_params_t bp = { 0 };
  bp.mask_id = grp->formid;
  dt_dev_history_item_t hist = { 0 };
  hist.blend_params = &bp;
  hist.forms = g_list_copy(flexi_dev.forms);
  GList *history = g_list_append(g_list_append(NULL, &stale), &hist);

  dt_masks_cleanup_unused_from_list(history);

  assert_non_null(dt_masks_get_from_id_ext(hist.forms, 1));
  assert_non_null(dt_masks_get_from_id_ext(hist.forms, 2));
  assert_non_null(dt_masks_get_from_id_ext(hist.forms, grp->formid));

  g_list_free(history);
  g_list_free(hist.forms);
  g_list_free(flexi_dev.allforms);
  flexi_dev.allforms = NULL;
}

// a copied group keeps its marker, settings and all, under an id of its own
static void test_copy_of_a_group_keeps_its_markers(void **state)
{
  // copying a shape reads its default size from the preferences
  flexi_conf_init();
  dt_masks_form_t *grp = flexi_build("i@0.5{1,2}");
  dt_masks_point_group_t *src = grp->points->data;
  g_strlcpy(src->name, "sky", sizeof(src->name));

  const dt_mask_id_t cid = dt_masks_form_copy(&flexi_dev, grp->formid);
  dt_masks_form_t *copy = dt_masks_get_from_id(&flexi_dev, cid);
  assert_non_null(copy);
  assert_int_equal(g_list_length(copy->points), 3);

  const dt_masks_point_group_t *m = copy->points->data;
  assert_true(dt_masks_point_is_marker(m));
  assert_int_not_equal(m->formid, FLEXI_GID(0));
  assert_null(dt_masks_get_from_id(&flexi_dev, m->formid));
  assert_int_equal(m->parentid, cid);
  assert_int_equal(m->state & DT_MASKS_STATE_FLEXI_OP, DT_MASKS_STATE_FLEXI_MINIMUM);
  assert_float_equal(m->group_opacity, 0.5f, 1e-6f);
  assert_string_equal(m->name, "sky");
}

// a marker's id resolves to no form by design, which is exactly what the prune
// looks for in a member whose form is gone
static void test_prune_spares_markers(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,i{}}");
  assert_int_equal(dt_masks_model_prune_dangling_members(grp), 0);
  assert_layout("u{1,i{}}");
}

// ---------------------------------------------------------------------------
// nested groups: the model reaches into them, and stops on a cycle
// ---------------------------------------------------------------------------

static void _free_nested_points(dt_masks_form_t *g)
{
  g_list_free_full(g->points, free);
  g->points = NULL;
}

// deselecting a group nested in the mask's own selects the mask's own
static void test_click_selected_nested_group_selects_the_mask_group(void **state)
{
  flexi_build("u{1,2,u{11}}");
  _click_group(FLEXI_GID(1));
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(1));
  _click_group(FLEXI_GID(1));
  assert_int_equal(flexi_bd.panel_selected_group_cid, FLEXI_GID(0));
}

// a raster element keeps its source publishing from inside a subgroup too
static void test_a_raster_element_in_a_subgroup_is_found(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,u{3000}}");
  dt_masks_form_t *r = dt_masks_get_from_id(&flexi_dev, 3000);
  r->type = DT_MASKS_RASTER;
  dt_masks_point_raster_t *rp = calloc(1, sizeof(dt_masks_point_raster_t));
  g_strlcpy(rp->source, "exposure", sizeof(rp->source));
  rp->instance = 1;
  rp->id = 5;
  r->points = g_list_append(NULL, rp);

  dt_iop_module_so_t so = { 0 };
  g_strlcpy(so.op, "exposure", sizeof(so.op));
  dt_iop_module_t source = { 0 };
  source.so = &so;
  source.multi_priority = 1;

  assert_ptr_equal(dt_masks_group_find_raster_of(flexi_dev.forms, grp, &source, 5, FALSE), rp);
  assert_ptr_equal(dt_masks_group_find_raster_of(flexi_dev.forms, grp, &source, NO_MASKID, TRUE),
                   rp);
  assert_null(dt_masks_group_find_raster_of(flexi_dev.forms, grp, &source, 6, FALSE));
  // another instance of the same module is another source
  source.multi_priority = 0;
  assert_null(dt_masks_group_find_raster_of(flexi_dev.forms, grp, &source, 5, FALSE));

  g_list_free_full(r->points, free);
  r->points = NULL;
}

// a group holding its own parent is malformed, and every walk has to end on it
static void test_a_cyclic_tree_ends_every_walk(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,u{1000}}");

  assert_null(dt_masks_group_find_node(flexi_dev.forms, grp, 9999, NULL));
  assert_null(dt_masks_group_find_raster_of(flexi_dev.forms, grp, NULL, NO_MASKID, TRUE));
  (void)dt_masks_group_hash_ext(DT_INITHASH, grp, flexi_dev.forms);

  dt_masks_form_t flat = { 0 };
  flat.type = DT_MASKS_GROUP;
  dt_masks_group_ungroup(&flat, grp);
  assert_true(g_list_length(flat.points) > 0);
  g_list_free_full(flat.points, free);
}

// ---------------------------------------------------------------------------
// nested groups in the panel: a gesture acts on the list that holds its target
// ---------------------------------------------------------------------------

static dt_masks_form_t *_sub = NULL;

static int _teardown_nested(void **state)
{
  if(_sub) _free_nested_points(_sub);
  _sub = NULL;
  flexi_conf_cleanup();
  flexi_teardown();
  return 0;
}

static void _circle(const dt_mask_id_t fid)
{
  dt_masks_form_t *c = calloc(1, sizeof(dt_masks_form_t));
  c->formid = fid;
  c->type = DT_MASKS_CIRCLE;
  snprintf(c->name, sizeof(c->name), "circle #%d", (int)fid);
  flexi_dev.forms = g_list_append(flexi_dev.forms, c);
}

/* the mask holding 1, 2 and a minimum group of 3 and a maximum group of 11,
   12 and a difference group of 13. _sub is the maximum group */
static dt_masks_form_t *_two_levels(void)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,u{11,12,d{13}}}}");
  _sub = dt_masks_get_from_id(&flexi_dev, FLEXI_SUB(2));
  return grp;
}

// the mask's own group is "whole mask" and takes no number, so the first
// group nested in it of the same mode is number 1
static void test_mask_group_takes_no_number(void **state)
{
  flexi_build("u{1,2,u{11}}");
  assert_int_equal(dt_masks_gui_group_ordinal_of_cid(&flexi_module, FLEXI_GID(0)), 0);
  assert_int_equal(dt_masks_gui_group_ordinal_of_cid(&flexi_module, FLEXI_GID(1)), 1);
}

static void test_nested_points_are_found_with_their_group(void **state)
{
  dt_masks_form_t *grp = _two_levels();
  assert_non_null(dt_masks_gui_group_point(grp, 13));
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, 13), FLEXI_GID(3));
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, 11), FLEXI_GID(2));
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, FLEXI_GID(3)), FLEXI_GID(3));
  // a nested group is a member of the group holding it
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, FLEXI_SUB(2)), FLEXI_GID(1));

  GList *run = dt_masks_model_group_members(grp, FLEXI_GID(2));
  assert_int_equal(g_list_length(run), 3);
  g_list_free(run);
}

static void test_nested_drop_stays_in_its_list(void **state)
{
  dt_masks_form_t *grp = _two_levels();
  assert_true(dt_masks_model_drop_element_onto_element(&flexi_module, grp, 11, 13, TRUE));
  assert_layout("u{1,2,i{3,u{12,d{13,11}}}}");

  assert_true(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 13, FLEXI_GID(2)));
  assert_layout("u{1,2,i{3,u{12,d{11},13}}}");
}

// an element moves between nesting levels; a nested group never into itself
static void test_nested_drop_moves_across_levels(void **state)
{
  dt_masks_form_t *grp = _two_levels();
  assert_true(dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 13, TRUE));
  assert_layout("u{2,i{3,u{11,12,d{13,1}}}}");
  assert_int_equal(dt_masks_gui_group_point(grp, 1)->parentid, FLEXI_SUB(3));

  assert_true(dt_masks_model_drop_element_onto_group(&flexi_module, grp, 12, FLEXI_GID(0)));
  assert_layout("u{2,i{3,u{11,d{13,1}}},12}");
  assert_int_equal(dt_masks_gui_group_point(grp, 12)->parentid, flexi_group()->formid);

  assert_false(dt_masks_model_drop_element_onto_element(&flexi_module, grp, FLEXI_SUB(2), 11, TRUE));
  assert_false(dt_masks_model_drop_element_onto_group(&flexi_module, grp, FLEXI_SUB(2), FLEXI_GID(3)));
  assert_layout("u{2,i{3,u{11,d{13,1}}},12}");
}

// a nested group goes inside another nested group
static void test_a_nested_group_goes_deeper(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3,u{11,12,d{13}}},u{14}}");
  assert_true(dt_masks_model_drop_element_onto_group(&flexi_module, grp, FLEXI_SUB(4), FLEXI_GID(2)));
  assert_layout("u{1,2,i{3,u{11,12,d{13},u{14}}}}");
  assert_true(dt_masks_model_drop_element_onto_element(&flexi_module, grp, FLEXI_SUB(4), 13, TRUE));
  assert_layout("u{1,2,i{3,u{11,12,d{13,u{14}}}}}");
  assert_true(dt_is_valid_maskid(dt_masks_model_nest_new_group(grp, 0, FLEXI_GID(2))));
}

// a cluster moves between nesting levels as its elements do, one by one
static void test_a_cluster_moves_across_levels(void **state)
{
  dt_masks_form_t *grp = _two_levels();
  GList *ids = g_list_append(g_list_append(NULL, GINT_TO_POINTER(11)), GINT_TO_POINTER(12));
  assert_true(dt_masks_gui_cluster_move(&flexi_module, ids, 3, FALSE, TRUE));
  assert_layout("u{1,2,i{3,11,12,u{d{13}}}}");
  assert_int_equal(dt_masks_gui_group_point(grp, 11)->parentid, FLEXI_SUB(1));
  g_list_free(ids);

  ids = g_list_append(g_list_append(NULL, GINT_TO_POINTER(1)), GINT_TO_POINTER(2));
  assert_true(dt_masks_gui_cluster_move(&flexi_module, ids, FLEXI_GID(3), TRUE, FALSE));
  assert_layout("u{i{3,11,12,u{d{13,1,2}}}}");
  assert_int_equal(dt_masks_gui_group_point(grp, 1)->parentid, FLEXI_SUB(3));

  // onto the group it is already in: nothing to do, as for one element
  assert_false(dt_masks_gui_cluster_move(&flexi_module, ids, FLEXI_GID(3), TRUE, FALSE));
  assert_layout("u{i{3,11,12,u{d{13,1,2}}}}");
  g_list_free(ids);
}

// a shape the mask holds twice, once in a nested group: a drop acts on the
// reference its row shows, not on the first one with its form id
static void test_a_drop_acts_on_the_row_reference(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3},u{1,11}}");
  _sub = dt_masks_get_from_id(&flexi_dev, FLEXI_SUB(2));
  const dt_masks_point_group_t *inner = g_list_nth_data(_sub->points, 1);
  assert_int_equal(inner->formid, 1);

  assert_true(dt_masks_model_drop_point_onto_point(&flexi_module, grp, dt_masks_gui_group_point(grp, 3), inner,
                                                   TRUE));
  assert_layout("u{1,2,i{},u{1,3,11}}");

  // the inner reference, not the top one that is already there
  assert_true(dt_masks_model_drop_point_onto_group(&flexi_module, grp, inner, FLEXI_GID(0)));
  assert_layout("u{1,2,i{},u{3,11},1}");
}

// the panel nests groups as deep as a walk of the mask follows, and no deeper
static void test_nesting_stops_where_walks_stop(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");
  dt_mask_id_t cid = FLEXI_GID(0);
  for(int depth = 1; depth <= DT_MASKS_NESTING_MAX; depth++)
  {
    cid = dt_masks_model_nest_new_group(grp, DT_MASKS_STATE_UNION, cid);
    assert_true(dt_is_valid_maskid(cid));
  }
  assert_false(dt_is_valid_maskid(dt_masks_model_nest_new_group(grp, DT_MASKS_STATE_UNION, cid)));
  for(GList *l = flexi_dev.forms; l; l = g_list_next(l))
    if(l->data != grp && (((dt_masks_form_t *)l->data)->type & DT_MASKS_GROUP))
      _free_nested_points(l->data);
}

// the nested group on top of the members of `grp`'s group whose top point is
// at index `at`
static dt_masks_form_t *_nested_at(dt_masks_form_t *grp, const int at)
{
  const dt_masks_point_group_t *pt = g_list_nth_data(grp->points, at);
  dt_masks_form_t *sub = pt ? dt_masks_get_from_id(&flexi_dev, pt->formid) : NULL;
  assert_non_null(sub);
  assert_true(sub->type & DT_MASKS_GROUP);
  return sub;
}

static void test_add_a_group_inside_a_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3}}");
  const dt_mask_id_t cid = dt_masks_model_nest_new_group(grp, 0, FLEXI_GID(0));
  assert_true(dt_is_valid_maskid(cid));
  // on top of the group's members, empty
  _sub = _nested_at(grp, 4);
  assert_layout("u{1,2,i{3},u{}}");
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, cid), cid);
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, _sub->formid), FLEXI_GID(0));
}

// a group dropped on another goes in on top of it, keeping its id; beside it,
// it joins the other's holder
static void test_drop_a_group_inside_another(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3},d{4}}");
  assert_true(dt_masks_model_move_group(&flexi_module, FLEXI_GID(2), FLEXI_GID(1), FALSE, TRUE));
  assert_layout("u{1,2,i{3,d{4}}}");
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, 4), FLEXI_GID(2));
  // a group is not put inside itself, nor inside what it holds, and the mask's
  // own group never moves
  assert_false(dt_masks_model_move_group(&flexi_module, FLEXI_GID(1), FLEXI_GID(1), FALSE, TRUE));
  assert_false(dt_masks_model_move_group(&flexi_module, FLEXI_GID(1), FLEXI_GID(2), FALSE, TRUE));
  assert_false(dt_masks_model_move_group(&flexi_module, FLEXI_GID(0), FLEXI_GID(1), FALSE, TRUE));
  assert_layout("u{1,2,i{3,d{4}}}");

  assert_true(dt_masks_model_move_group(&flexi_module, FLEXI_GID(2), FLEXI_GID(1), TRUE, FALSE));
  assert_layout("u{1,2,i{3},d{4}}");
}

// ---------------------------------------------------------------------------
// compose and simplify
// ---------------------------------------------------------------------------

// every nested group's points, _sub's included: compose and simplify make and
// empty groups the test never names
static int _teardown_composed(void **state)
{
  for(GList *l = flexi_dev.forms; l; l = g_list_next(l))
    if(l->data != flexi_group() && (((dt_masks_form_t *)l->data)->type & DT_MASKS_GROUP))
      _free_nested_points(l->data);
  _sub = NULL;
  flexi_teardown();
  return 0;
}

// an element goes into a new group where it was, keeping its own settings,
// under an empty group, which is what compose hands back to be selected
static void test_compose_an_element(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,3}");
  dt_masks_point_group_t *two = dt_masks_gui_group_point(grp, 2);
  two->opacity = 0.5f;
  two->state |= DT_MASKS_STATE_INVERSE;
  const dt_mask_id_t eid = dt_masks_model_compose(grp, two, DT_MASKS_STATE_FLEXI_DIFFERENCE);
  assert_true(dt_is_valid_maskid(eid));
  assert_tree(grp, "u{1,d{2~@0.5,u{}},3}");
  const dt_masks_point_group_t *empty = dt_masks_gui_group_point(grp, eid);
  assert_non_null(empty);
  assert_true(dt_masks_point_is_marker(empty));
  assert_int_equal(two->parentid, dt_masks_gui_group_point(grp, dt_masks_gui_group_cid_of_form(grp, 2))->parentid);
}

// a group is composed through its holder's reference, keeping its settings
static void test_compose_a_nested_group(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,u@0.5{11,12}}");
  assert_true(dt_is_valid_maskid(dt_masks_model_compose(grp, dt_masks_gui_group_point(grp, FLEXI_GID(1)),
                                                        DT_MASKS_STATE_FLEXI_MINIMUM)));
  assert_tree(grp, "u{1,i{u@0.5{11,12},u{}}}");
}

// the mask's own group stays the mask, with its id: its members and settings
// move into a group at its bottom, and it keeps only the new operator
static void test_compose_the_whole_mask(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");
  dt_masks_point_group_t *root = grp->points->data;
  root->state |= DT_MASKS_STATE_FLEXI_SCREEN | DT_MASKS_STATE_OP_INVERT;
  root->group_opacity = 0.7f;
  root->refinement.enabled = DT_MASKS_REFINE_GROUP;
  root->refinement.blur_radius = 3.0f;

  assert_true(dt_is_valid_maskid(dt_masks_model_compose(grp, root, DT_MASKS_STATE_FLEXI_DIFFERENCE)));
  assert_tree(grp, "d{o~@0.7{1,2},u{}}");
  assert_ptr_equal(grp->points->data, root);
  assert_int_equal(root->formid, FLEXI_GID(0));
  assert_int_equal(root->refinement.enabled, DT_MASKS_REFINE_OFF);
  const dt_masks_form_t *sub =
    dt_masks_get_from_id(&flexi_dev, ((dt_masks_point_group_t *)grp->points->next->data)->formid);
  const dt_masks_point_group_t *mk = sub->points->data;
  assert_int_equal(mk->refinement.enabled, DT_MASKS_REFINE_GROUP);
  assert_true(mk->refinement.blur_radius == 3.0f);
  assert_int_equal(((dt_masks_point_group_t *)sub->points->next->data)->parentid, sub->formid);
}

// compose adds a level, so it stops where nesting does
static void test_compose_stops_where_nesting_does(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1}");
  dt_mask_id_t cid = FLEXI_GID(0);
  for(int depth = 1; depth <= DT_MASKS_NESTING_MAX; depth++)
    cid = dt_masks_model_nest_new_group(grp, 0, cid);
  assert_false(dt_is_valid_maskid(dt_masks_model_compose(grp, dt_masks_gui_group_point(grp, cid), DT_MASKS_STATE_FLEXI_MINIMUM)));
  assert_false(dt_is_valid_maskid(dt_masks_model_compose(grp, grp->points->data, DT_MASKS_STATE_FLEXI_MINIMUM)));
  assert_true(dt_is_valid_maskid(dt_masks_model_compose(grp, dt_masks_gui_group_point(grp, 1), DT_MASKS_STATE_FLEXI_MINIMUM)));
}

// compose, then simplify with the empty group still empty: back where it was
static void test_simplify_undoes_a_compose(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,3}");
  dt_masks_model_compose(grp, dt_masks_gui_group_point(grp, 2), DT_MASKS_STATE_FLEXI_DIFFERENCE);
  assert_true(dt_masks_group_simplify(flexi_dev.forms, grp));
  assert_tree(grp, "u{1,2,3}");
  assert_int_equal(dt_masks_gui_group_point(grp, 2)->parentid, grp->formid);
  assert_false(dt_masks_group_simplify(flexi_dev.forms, grp));
}

// the whole mask takes back what compose moved out of it, its refinement
// going to the module's, and only where that renders the same
static void test_hoist_undoes_a_whole_mask_compose(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2}");
  dt_masks_point_group_t *root = grp->points->data;
  root->state |= DT_MASKS_STATE_FLEXI_SCREEN;
  root->group_opacity = 0.7f;
  dt_masks_model_compose(grp, root, DT_MASKS_STATE_FLEXI_DIFFERENCE);
  dt_masks_refinement_t whole = { 0 };
  assert_false(dt_masks_model_hoist_sole_group(grp, &whole)); // the empty group is still there
  assert_true(dt_masks_group_simplify(flexi_dev.forms, grp));
  assert_tree(grp, "d{o@0.7{1,2}}");
  assert_true(dt_masks_model_hoist_sole_group(grp, &whole));
  assert_tree(grp, "o@0.7{1,2}");
  assert_int_equal(whole.enabled, DT_MASKS_REFINE_OFF);
  assert_int_equal(dt_masks_gui_group_point(grp, 1)->parentid, grp->formid);
  assert_false(dt_masks_model_hoist_sole_group(grp, &whole));
  _teardown_composed(state);

  // a group's refinement becomes the whole mask's, unless that has one
  grp = flexi_build("u{1,2}");
  root = grp->points->data;
  root->refinement.enabled = DT_MASKS_REFINE_GROUP;
  root->refinement.details = 0.3f;
  dt_masks_model_compose(grp, root, DT_MASKS_STATE_FLEXI_MINIMUM);
  dt_masks_group_simplify(flexi_dev.forms, grp);
  whole.enabled = DT_MASKS_REFINE_GROUP;
  assert_false(dt_masks_model_hoist_sole_group(grp, &whole));
  whole.enabled = DT_MASKS_REFINE_OFF;
  assert_true(dt_masks_model_hoist_sole_group(grp, &whole));
  assert_int_equal(whole.enabled, DT_MASKS_REFINE_GROUP);
  assert_true(whole.details == 0.3f);
  _teardown_composed(state);

  // after the mask's invert and opacity, a refinement is not the same
  grp = flexi_build("u{1,2}");
  root = grp->points->data;
  root->group_opacity = 0.5f;
  root->refinement.enabled = DT_MASKS_REFINE_GROUP;
  dt_masks_model_compose(grp, root, DT_MASKS_STATE_FLEXI_MINIMUM);
  dt_masks_group_simplify(flexi_dev.forms, grp);
  whole.enabled = DT_MASKS_REFINE_OFF;
  assert_false(dt_masks_model_hoist_sole_group(grp, &whole));
}

// a group folding with its holder's operator gives the holder its members,
// except in the middle of an ordered fold: `1 - (11 - 12)` is no `1 - 11 - 12`
static void test_simplify_splices_same_operator_groups(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,u{11,12}}");
  assert_true(dt_masks_group_simplify(flexi_dev.forms, grp));
  assert_tree(grp, "u{1,11,12}");
  _teardown_composed(state);

  grp = flexi_build("d{1,d{11,12}}");
  assert_false(dt_masks_group_simplify(flexi_dev.forms, grp));
  assert_tree(grp, "d{1,d{11,12}}");
}

// a nested group another module renders as its mask is shared, even with no
// second reference: folding the holder's opacity into it would dim that
// module's mask too
static void test_simplify_leaves_a_group_another_module_renders(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,u{11,12}@0.5}");

  static dt_develop_blend_params_t other_bp = { .mask_id = FLEXI_SUB(1) };
  static dt_iop_module_t other = { .blend_params = &other_bp };
  flexi_dev.iop = g_list_append(NULL, &other);

  dt_masks_group_simplify(flexi_dev.forms, grp);
  assert_float_equal(dt_masks_gui_group_point(grp, FLEXI_GID(1))->group_opacity, 1.0f, 1e-6);

  g_list_free(flexi_dev.iop);
  flexi_dev.iop = NULL;
}

// what simplify leaves: a named group, even empty, a group whose settings
// change its result, and a group another reference shares
static void test_simplify_keeps_what_changes_something(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,u@0.5{11,12},u{}}");
  g_strlcpy(dt_masks_gui_group_point(grp, FLEXI_GID(2))->name, "later",
            sizeof(((dt_masks_point_group_t *)0)->name));
  assert_false(dt_masks_group_simplify(flexi_dev.forms, grp));
  assert_tree(grp, "u{1,u@0.5{11,12},u{}}");
  _teardown_composed(state);

  grp = flexi_build("u{1,u{11,12}}");
  // a second reference to the same group
  dt_masks_point_group_t *again = malloc(sizeof(dt_masks_point_group_t));
  memcpy(again, g_list_last(grp->points)->data, sizeof(dt_masks_point_group_t));
  grp->points = g_list_append(grp->points, again);
  assert_false(dt_masks_group_simplify(flexi_dev.forms, grp));
  assert_tree(grp, "u{1,u{11,12},u{11,12}}");
}

// nothing sits beside the mask's own group: a nested group dropped there
// stays where it is
static void test_a_nested_group_does_not_move_beside_the_mask(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,u{11}}");
  assert_int_equal(dt_masks_model_nested_group_of(grp, FLEXI_GID(1))->formid, FLEXI_SUB(1));

  assert_false(dt_masks_model_move_group(&flexi_module, FLEXI_GID(1), FLEXI_GID(0), TRUE, FALSE));
  assert_layout("u{1,2,u{11}}");
}

// a faded reference is shown as an element row, not as its group, and its
// fade applies on top of the group's own opacity: it stays nested
static void test_a_faded_nested_group_stays_nested(void **state)
{
  flexi_build("u{1,2,i{3,u{11}@0.5}}");
  assert_false(dt_masks_model_move_group(&flexi_module, FLEXI_GID(2), FLEXI_GID(0), TRUE, FALSE));
  assert_layout("u{1,2,i{3,u{11}@0.5}}");
}

// a group dropped beside a nested group joins that group's holder
static void test_a_group_beside_a_nested_group_joins_its_holder(void **state)
{
  dt_masks_form_t *grp = flexi_build("u{1,2,i{3},d{4,u{11}}}");
  assert_true(dt_masks_model_move_group(&flexi_module, FLEXI_GID(1), FLEXI_GID(3), TRUE, FALSE));
  assert_layout("u{1,2,d{4,u{11},i{3}}}");
  assert_int_equal(dt_masks_gui_group_cid_of_form(grp, FLEXI_SUB(1)), FLEXI_GID(2));
}

// nested groups shown as groups reorder among their holder's elements, and
// go inside another group as elements
static void test_a_nested_group_moves_as_an_element(void **state)
{
  flexi_build("u{1,2,i{3,u{11},u{12}}}");
  assert_true(dt_masks_model_move_group(&flexi_module, FLEXI_GID(2), FLEXI_GID(3), TRUE, FALSE));
  assert_layout("u{1,2,i{3,u{12},u{11}}}");
  assert_true(dt_masks_model_move_group(&flexi_module, FLEXI_GID(2), FLEXI_GID(0), FALSE, TRUE));
  assert_layout("u{1,2,i{3,u{12}},u{11}}");
  // never into itself
  assert_false(dt_masks_model_move_group(&flexi_module, FLEXI_GID(2), FLEXI_GID(2), FALSE, TRUE));
}

static void test_new_element_lands_in_a_nested_target(void **state)
{
  flexi_conf_init();
  _two_levels();
  _circle(14);
  _aim_at(12);
  dt_masks_point_group_t *pt = dt_masks_group_insert_point(
    &flexi_dev, &flexi_module, dt_masks_get_from_id(&flexi_dev, 14));
  assert_non_null(pt);
  assert_int_equal(pt->parentid, FLEXI_SUB(2));
  assert_layout("u{1,2,i{3,u{11,12,14,d{13}}}}");
}

static gboolean _is_hidden(const dt_mask_id_t fid)
{
  return (dt_masks_gui_group_point(flexi_group(), fid)->state & DT_MASKS_STATE_HIDDEN) != 0;
}

// soloing an element inside a nested group hides everything else at every
// level, but not the groups holding it, or nothing would show
static void test_solo_inside_a_nested_group_keeps_the_path(void **state)
{
  dt_masks_form_t *grp = _two_levels();
  GList *keep = g_list_prepend(NULL, GINT_TO_POINTER(13));
  dt_masks_group_isolate_state(grp, keep, DT_MASKS_STATE_HIDDEN);
  g_list_free(keep);

  assert_true(_is_hidden(1));
  assert_true(_is_hidden(3));
  assert_false(_is_hidden(FLEXI_SUB(1)));
  assert_false(_is_hidden(FLEXI_SUB(2)));
  assert_true(_is_hidden(11));
  assert_true(_is_hidden(12));
  assert_false(_is_hidden(13));

  dt_masks_group_isolate_state(grp, NULL, DT_MASKS_STATE_HIDDEN);
  assert_false(_is_hidden(1));
  assert_false(_is_hidden(11));
}

// soloing the nested group shows all of it, including what an earlier solo
// inside it hid
static void test_solo_of_a_nested_group_clears_inside_it(void **state)
{
  dt_masks_form_t *grp = _two_levels();
  dt_masks_gui_group_point(grp, 11)->state |= DT_MASKS_STATE_HIDDEN;
  GList *keep = g_list_prepend(NULL, GINT_TO_POINTER(FLEXI_SUB(2)));
  dt_masks_group_isolate_state(grp, keep, DT_MASKS_STATE_HIDDEN);
  g_list_free(keep);

  assert_false(_is_hidden(FLEXI_SUB(2)));
  assert_false(_is_hidden(11));
  assert_true(_is_hidden(3));
}

// one numbering for the whole mask, at every depth, by operator family
static void test_nested_groups_are_numbered(void **state)
{
  flexi_build("u{1,u{2},i{3,u{11,d{13}}}}");
  assert_int_equal(dt_masks_gui_group_ordinal_of_cid(&flexi_module, FLEXI_GID(1)), 1);
  assert_int_equal(dt_masks_gui_group_ordinal_of_cid(&flexi_module, FLEXI_GID(2)), 1);
  assert_int_equal(dt_masks_gui_group_ordinal_of_cid(&flexi_module, FLEXI_GID(3)), 2);
  assert_int_equal(dt_masks_gui_group_ordinal_of_cid(&flexi_module, FLEXI_GID(4)), 1);

  // a soloed nested group is still a group
  flexi_bd.solo_group_key = FLEXI_GID(4);
  dt_masks_gui_prune_stale_solo(&flexi_module);
  assert_int_equal(flexi_bd.solo_group_key, FLEXI_GID(4));
}

static void test_nested_shapes_are_listed_and_signed(void **state)
{
  dt_masks_form_t *grp = _two_levels();

  // what another mask can link from this one: its shapes, nested ones too
  GList *shapes = dt_masks_model_module_shapes(&flexi_module);
  assert_int_equal(g_list_length(shapes), 6);
  assert_int_equal(GPOINTER_TO_INT(g_list_nth_data(shapes, 0)), 1);
  assert_int_equal(GPOINTER_TO_INT(g_list_nth_data(shapes, 5)), 13);
  g_list_free(shapes);

  // a change the rows show, inside the nested group, rebuilds the panel
  const dt_hash_t before = dt_masks_gui_list_signature(&flexi_module);
  g_strlcpy(dt_masks_gui_group_point(grp, 13)->name, "sky", sizeof(((dt_masks_point_group_t *)0)->name));
  assert_true(dt_masks_gui_list_signature(&flexi_module) != before);
}

// a member of a nested group whose form is gone leaves that group's list
static void test_lost_member_leaves_a_nested_group(void **state)
{
  dt_masks_form_t *grp = _two_levels();
  for(GList *l = flexi_dev.forms; l; l = g_list_next(l))
    if(((dt_masks_form_t *)l->data)->formid == 12)
    {
      free(l->data);
      flexi_dev.forms = g_list_delete_link(flexi_dev.forms, l);
      break;
    }
  assert_int_equal(dt_masks_model_prune_dangling_members(grp), 1);
  assert_layout("u{1,2,i{3,u{11,d{13}}}}");
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_teardown(test_layout_roundtrip, _teardown),
    cmocka_unit_test_teardown(test_a_same_op_nested_group_stays_separate, _teardown),
    cmocka_unit_test_teardown(test_cid_of_form_is_the_groups_marker, _teardown),
    cmocka_unit_test_teardown(test_empty_group_has_no_members, _teardown),
    cmocka_unit_test_teardown(test_selected_group_formids, _teardown),
    cmocka_unit_test_teardown(test_drop_element_into_other_group, _teardown),
    cmocka_unit_test_teardown(test_drop_element_below_target, _teardown),
    cmocka_unit_test_teardown(test_drop_adopts_target_operator, _teardown),
    cmocka_unit_test_teardown(test_drop_between_groups_never_creates_a_third, _teardown),
    cmocka_unit_test_teardown(test_drop_between_same_op_groups_keeps_both, _teardown),
    cmocka_unit_test_teardown(test_drop_onto_bottom_group, _teardown),
    cmocka_unit_test_teardown(test_drop_emptying_group_keeps_it, _teardown),
    cmocka_unit_test_teardown(test_prune_drops_lost_members_and_keeps_their_group, _teardown),
    cmocka_unit_test_teardown(test_prune_drops_every_reference_to_a_lost_form, _teardown),
    cmocka_unit_test_teardown(test_prune_keeps_the_rest_of_a_group, _teardown),
    cmocka_unit_test_teardown(test_prune_without_lost_members_changes_nothing, _teardown),
    cmocka_unit_test_teardown(test_lost_members_leave_their_groups, _teardown),
    cmocka_unit_test_teardown(test_classic_marking_folds_each_run_into_a_group, _teardown),
    cmocka_unit_test_teardown(test_classic_marking_keeps_one_operator_one_group, _teardown),
    cmocka_unit_test_teardown(test_marking_leaves_members_plain, _teardown),
    cmocka_unit_test_teardown(test_marking_the_same_run_twice_gives_the_same_id, _teardown),
    cmocka_unit_test_teardown(test_drop_onto_self_is_rejected, _teardown),
    cmocka_unit_test_teardown(test_drop_of_unknown_element_is_rejected, _teardown),
    cmocka_unit_test_teardown(test_drop_keeps_element_selected_in_new_group, _teardown),
    cmocka_unit_test_teardown(test_click_group_selects_it, _teardown),
    cmocka_unit_test_teardown(test_click_selected_group_selects_the_whole_mask, _teardown),
    cmocka_unit_test_teardown(test_click_mask_group_again_keeps_it_selected, _teardown),
    cmocka_unit_test_teardown(test_click_element_selects_element_and_its_group, _teardown),
    cmocka_unit_test_teardown(test_click_selected_element_falls_back_to_its_group, _teardown),
    cmocka_unit_test_teardown(test_element_then_group_reaches_the_whole_mask, _teardown),
    cmocka_unit_test_teardown(test_click_group_of_selected_element_selects_the_group, _teardown),
    cmocka_unit_test_teardown(test_click_other_element_switches_directly, _teardown),
    cmocka_unit_test_teardown(test_click_other_group_switches_directly, _teardown),
    cmocka_unit_test_teardown(test_chevron_expand_collapses_previous, _teardown),
    cmocka_unit_test_teardown(test_chevron_collapse_forgets_the_open_row, _teardown),
    cmocka_unit_test_teardown(test_chevron_collapse_of_other_row_keeps_the_open_one, _teardown),
    cmocka_unit_test_teardown(test_chevron_reexpand_open_row_collapses_nothing, _teardown),
    cmocka_unit_test_teardown(test_chevron_without_auto_expand_moves_nothing, _teardown),
    cmocka_unit_test_teardown(test_isolate_state_hides_everything_else, _teardown),
    cmocka_unit_test_teardown(test_isolate_state_null_list_clears_everywhere, _teardown),
    cmocka_unit_test_teardown(test_isolate_state_soloing_a_whole_group, _teardown),
    cmocka_unit_test_teardown(test_link_lands_in_target_group_in_order, _teardown_linking),
    cmocka_unit_test_teardown(test_link_skips_what_the_mask_already_uses, _teardown_linking),
    cmocka_unit_test_teardown(test_link_keeps_the_source_look, _teardown_linking),
    cmocka_unit_test_teardown(test_copy_is_a_new_independent_form, _teardown_linking),
    cmocka_unit_test_teardown(test_copy_of_object_copies_its_paths, _teardown_linking),
    cmocka_unit_test_teardown(test_module_shapes_follow_groups_and_skip_other_kinds,
                              _teardown_linking),
    cmocka_unit_test_teardown(test_unlink_gives_this_module_its_own_copy, _teardown_linking),
    cmocka_unit_test_teardown(test_unlink_leaves_the_group_alone, _teardown_linking),
    cmocka_unit_test_teardown(test_link_shows_only_for_shared_elements_but_raster,
                              _teardown_linking),
    cmocka_unit_test_teardown(test_parametric_channel_never_shows_the_link, _teardown_linking),
    cmocka_unit_test_teardown(test_list_signature_follows_links_made_elsewhere,
                              _teardown_linking),
    cmocka_unit_test_teardown(test_list_signature_ignores_links_among_others, _teardown_linking),
    cmocka_unit_test_teardown(test_duplicate_shares_shapes_and_copies_channels,
                              _teardown_linking),
    cmocka_unit_test_teardown(test_double_click_on_object_steps_in, _teardown_objects),
    cmocka_unit_test_teardown(test_double_click_inside_object_is_ordinary, _teardown_objects),
    cmocka_unit_test_teardown(test_click_inside_object_stays_in, _teardown_objects),
    cmocka_unit_test_teardown(test_click_outside_object_steps_out, _teardown_objects),
    cmocka_unit_test_teardown(test_double_click_on_other_object_moves_into_it,
                              _teardown_objects),
    cmocka_unit_test_teardown(test_secondary_button_never_steps, _teardown_objects),
    cmocka_unit_test_teardown(test_double_click_on_plain_shape_does_nothing, _teardown_objects),
    cmocka_unit_test_teardown(test_object_paths_act_as_one_until_stepped_in, _teardown_objects),
    cmocka_unit_test_teardown(test_right_click_on_object_path_removes_the_object,
                              _teardown_objects),
    cmocka_unit_test_teardown(test_right_click_inside_object_removes_one_path,
                              _teardown_objects),
    cmocka_unit_test_teardown(test_node_deletion_removes_one_path, _teardown_objects),
    cmocka_unit_test_teardown(test_last_path_removes_the_object, _teardown_objects),
    cmocka_unit_test_teardown(test_object_removal_without_canvas_session, _teardown_objects),
    cmocka_unit_test_teardown(test_canvas_delete_of_shape_is_the_panel_delete,
                              _teardown_objects),
    cmocka_unit_test_teardown(test_canvas_path_selects_its_object_row, _teardown_objects),
    cmocka_unit_test_teardown(test_canvas_path_of_entered_object_selects_its_own_row,
                              _teardown_objects),
    cmocka_unit_test_teardown(test_points_of_entered_object_are_found, _teardown_objects),
    cmocka_unit_test_teardown(test_marker_is_not_counted_as_a_path, _teardown_objects),
    cmocka_unit_test_teardown(test_props_panel_shows_a_shapes_geometry, _teardown_objects),
    cmocka_unit_test_teardown(test_props_panel_entered_object_has_only_opacity,
                              _teardown_objects),
    cmocka_unit_test_teardown(test_props_panel_shows_a_raster_masks_opacity, _teardown_objects),
    cmocka_unit_test_teardown(test_props_panel_parametric_shows_its_boost_factor,
                              _teardown_objects),
    cmocka_unit_test_teardown(test_props_panel_shows_a_groups_opacity, _teardown_objects),
    cmocka_unit_test_teardown(test_props_panel_empty_without_a_selection, _teardown_objects),
    cmocka_unit_test_teardown(test_stepping_in_moves_the_panel_signature, _teardown_objects),
    cmocka_unit_test_teardown(test_soloedit_inside_entered_object_isolates_the_object,
                              _teardown_objects),
    cmocka_unit_test_teardown(test_soloedit_isolates_a_selected_group, _teardown),
    cmocka_unit_test_teardown(test_soloedit_off_for_the_mask_group, _teardown),
    cmocka_unit_test_teardown(test_refine_scope_of_removed_element_falls_back_to_its_group,
                              _teardown_objects),
    cmocka_unit_test_teardown(test_refine_scope_with_nothing_left_is_the_whole_mask,
                              _teardown_objects),
    cmocka_unit_test_teardown(test_refine_scope_of_present_element_is_kept, _teardown_objects),
    cmocka_unit_test_teardown(test_refine_scope_of_the_mask_group_is_the_whole_mask, _teardown_objects),
    cmocka_unit_test_teardown(test_refine_scope_follows_the_selection, _teardown_objects),
    cmocka_unit_test_teardown(test_raster_element_follows_its_source_name, _teardown_raster),
    cmocka_unit_test_teardown(test_renamed_raster_element_keeps_its_name, _teardown_raster),
    cmocka_unit_test_teardown(test_emptied_raster_name_follows_the_source_again,
                              _teardown_raster),
    cmocka_unit_test_teardown(test_raster_source_is_found_by_operation_and_instance,
                              _teardown_raster),
    cmocka_unit_test_teardown(test_list_signature_follows_source_rename, _teardown_raster),
    cmocka_unit_test_teardown(test_cleanup_keeps_the_paths_of_a_used_object,
                              _teardown_objects),
    cmocka_unit_test_teardown(test_empty_object_is_removed, _teardown_objects),
    cmocka_unit_test_teardown(test_object_with_a_path_is_kept, _teardown_objects),
    cmocka_unit_test_teardown(test_add_target_is_the_mask_group, _teardown),
    cmocka_unit_test_teardown(test_add_target_ignores_a_stale_selection, _teardown),
    cmocka_unit_test_teardown(test_add_target_is_the_selected_group, _teardown),
    cmocka_unit_test_teardown(test_add_target_of_a_mask_with_no_group_form, _teardown),
    cmocka_unit_test_teardown(test_cleanup_keeps_every_member_of_a_marked_group,
                              _teardown),
    cmocka_unit_test_teardown(test_cleanup_keeps_every_member_past_a_stale_mask_id,
                              _teardown),
    cmocka_unit_test_teardown(test_copy_of_a_group_keeps_its_markers, _teardown_linking),
    cmocka_unit_test_teardown(test_prune_spares_markers, _teardown),
    cmocka_unit_test_teardown(test_click_selected_nested_group_selects_the_mask_group, _teardown),
    cmocka_unit_test_teardown(test_a_raster_element_in_a_subgroup_is_found, _teardown),
    cmocka_unit_test_teardown(test_a_cyclic_tree_ends_every_walk, _teardown),
    cmocka_unit_test_teardown(test_nested_points_are_found_with_their_group, _teardown_nested),
    cmocka_unit_test_teardown(test_nested_drop_stays_in_its_list, _teardown_nested),
    cmocka_unit_test_teardown(test_nested_drop_moves_across_levels, _teardown_nested),
    cmocka_unit_test_teardown(test_a_nested_group_goes_deeper, _teardown_nested),
    cmocka_unit_test_teardown(test_a_cluster_moves_across_levels, _teardown_nested),
    cmocka_unit_test_teardown(test_a_drop_acts_on_the_row_reference, _teardown_nested),
    cmocka_unit_test_teardown(test_nesting_stops_where_walks_stop, _teardown_nested),
    cmocka_unit_test_teardown(test_add_a_group_inside_a_group, _teardown_nested),
    cmocka_unit_test_teardown(test_drop_a_group_inside_another, _teardown_nested),
    cmocka_unit_test_teardown(test_compose_an_element, _teardown_composed),
    cmocka_unit_test_teardown(test_compose_a_nested_group, _teardown_composed),
    cmocka_unit_test_teardown(test_compose_the_whole_mask, _teardown_composed),
    cmocka_unit_test_teardown(test_compose_stops_where_nesting_does, _teardown_composed),
    cmocka_unit_test_teardown(test_simplify_undoes_a_compose, _teardown_composed),
    cmocka_unit_test_teardown(test_hoist_undoes_a_whole_mask_compose, _teardown_composed),
    cmocka_unit_test_teardown(test_simplify_splices_same_operator_groups, _teardown_composed),
    cmocka_unit_test_teardown(test_simplify_leaves_a_group_another_module_renders,
                              _teardown_composed),
    cmocka_unit_test_teardown(test_simplify_keeps_what_changes_something, _teardown_composed),
    cmocka_unit_test_teardown(test_a_nested_group_does_not_move_beside_the_mask, _teardown_nested),
    cmocka_unit_test_teardown(test_a_faded_nested_group_stays_nested, _teardown_nested),
    cmocka_unit_test_teardown(test_a_group_beside_a_nested_group_joins_its_holder, _teardown_nested),
    cmocka_unit_test_teardown(test_a_nested_group_moves_as_an_element, _teardown_nested),
    cmocka_unit_test_teardown(test_new_element_lands_in_a_nested_target, _teardown_nested),
    cmocka_unit_test_teardown(test_solo_inside_a_nested_group_keeps_the_path,
                              _teardown_nested),
    cmocka_unit_test_teardown(test_solo_of_a_nested_group_clears_inside_it,
                              _teardown_nested),
    cmocka_unit_test_teardown(test_nested_groups_are_numbered, _teardown_nested),
    cmocka_unit_test_teardown(test_mask_group_takes_no_number, _teardown),
    cmocka_unit_test_teardown(test_nested_shapes_are_listed_and_signed, _teardown_nested),
    cmocka_unit_test_teardown(test_lost_member_leaves_a_nested_group, _teardown_nested),
    cmocka_unit_test_teardown(test_classic_marking_puts_the_base_in_the_first_run, _teardown),
    cmocka_unit_test_teardown(test_classic_marking_nests_at_each_operator_change,
                              _teardown),
    cmocka_unit_test_teardown(test_a_faded_member_keeps_its_opacity, _teardown),
    cmocka_unit_test_teardown(test_unlink_one_reference_leaves_the_others_linked,
                              _teardown_linking),
    cmocka_unit_test_teardown(test_unlink_the_last_reference_carries_the_selection,
                              _teardown_linking),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
